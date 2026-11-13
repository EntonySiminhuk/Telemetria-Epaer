# GPS NEO-6M + GY-91 com Heltec WiFi LoRa 32 V3

Firmware em **ESP-IDF** que lê um módulo GPS **u-blox NEO-6M** pela UART e um módulo
**GY-91** (MPU9250 + BMP280) por I2C. Ele mostra **data/hora, latitude, longitude, altitude,
número de satélites e atitude (roll/pitch)** no monitor serial e no display OLED da placa,
e transmite tudo por **LoRa** 2 vezes por segundo.

```
I (747270) GPS: 03/10/2026 16:10:53.4 | Lat: -26.229174 | Lon: -52.672226 | Alt: 727.9 m
```

O display alterna entre duas telas a cada 3 s:

```
 Tela 1: GPS                  Tela 2: GY-91
┌─────────────────────┐      ┌─────────────────────┐
│03/10/2026 16:10:53.4│      │GY-91        G: 1.00 │
│─────────────────────│      │─────────────────────│
│Lat: -26.229174      │      │Roll :    +1.7 graus │
│Lon: -52.672226      │      │Pitch:    +8.3 graus │
│Alt: 727.9 m         │      │Rumo :   306.0 graus │
│3D HDOP:1.2 e:3m     │      │Alt rel: -0.45 m     │
│Satelites: 5         │      │925.80 hPa  27.5 C   │
│LoRa: #42         1/2│      │LoRa: #48         2/2│
└─────────────────────┘      └─────────────────────┘
```

## Hardware

| Item | Detalhe |
|---|---|
| Placa | Heltec WiFi LoRa 32 **V3** (ESP32-S3 + SX1262) |
| GPS | u-blox NEO-6M (UART, 9600 baud, NMEA) |
| IMU / barômetro | GY-91: MPU9250 (acelerômetro, giroscópio, magnetômetro AK8963) + BMP280 (I2C) |
| Display | OLED SSD1306 128x64 da própria placa (I2C) |

### Ligações

| NEO-6M | Heltec V3 | Observação |
|---|---|---|
| TX | **GPIO4** | RX da UART1 do ESP32 |
| RX | **GPIO5** | TX da UART1 do ESP32 (necessário para configurar 38400 baud / 5 Hz) |
| VCC | 3V3 ou Vext | Vext é ligado pelo firmware (GPIO36 em LOW) |
| GND | GND | GND precisa ser comum |

| GY-91 | Heltec V3 | Observação |
|---|---|---|
| VIN | **3V3** | O módulo tem regulador próprio (aceita 3,3 a 5 V) |
| GND | GND | |
| SDA | **GPIO41** | I2C_NUM_1 (separado do OLED) |
| SCL | **GPIO42** | I2C_NUM_1 |
| 3V3, SAO/SDO, NCS, CSB | não ligar | Desligados: MPU em `0x68`, BMP280 em `0x76` (o driver também procura `0x69`/`0x77`) |

Recursos da placa usados pelo firmware (nenhum é compartilhado entre os sensores):

| Função | Periférico | GPIO |
|---|---|---|
| GPS | UART1 | 4 (RX), 5 (TX) |
| GY-91 | I2C_NUM_1 | 41 (SDA), 42 (SCL) |
| OLED | I2C_NUM_0 | 17 (SDA), 18 (SCL), 21 (RST) |
| Rádio SX1262 | SPI2 | 8, 9, 10, 11, 12, 13, 14 |
| Vext (alimentação do OLED / pino Vext) | GPIO | 36 (LOW = ligado) |

## Como funciona

```
NEO-6M --UART1 38400--> gps_task --> nmea_parse --> gps_leitura_t --> gps_validar --> gps_print --> monitor serial
                                                                                    \--> gps_ultimo --> display_task --> OLED
```

1. **Inicialização (`app_main`)**
   - Liga o **Vext** (GPIO36 em LOW), que alimenta o OLED e o pino Vext.
   - Inicializa o **display OLED** (`oled_init`). Se falhar, registra o erro e segue sem display.
   - Inicia o **GY-91** (`imu_iniciar`, veja a seção [GY-91](#gy-91-atitude-e-barômetro)).
     A placa precisa ficar **parada por ~2 s** ao ligar, para calibrar o giroscópio e
     medir a pressão de referência. Se o GY-91 falhar, o firmware segue só com o GPS.
   - Configura a **UART1** em 9600 baud, 8N1, nos pinos 4 (RX) e 5 (TX).
   - **Configura o GPS (`gps_config`)** com comandos binários **UBX** da u-blox:
     - `CFG-PRT`: muda o GPS para **38400 baud**. O comando é enviado em 9600 e em 38400,
       porque o GPS pode já estar configurado se só o ESP32 reiniciou.
     - `CFG-MSG`: liga **GSA** (tipo de fix e DOP) e **GST** (erro estimado em metros) e
       desliga GLL, GSV e VTG. RMC e GGA já vêm ligadas de fábrica.
     - `CFG-NAV5`: modelo dinâmico **aéreo < 1g** (`GPS_MODELO_DINAMICO`) e **fix somente 3D**.
       Com 3 satélites o NEO-6M faria um fix 2D, supondo a altitude, e a posição poderia
       errar centenas de metros.
     - `CFG-RATE`: define a taxa de atualização de **5 Hz** (uma medição a cada 200 ms).
     - Cada comando espera a confirmação (`ACK-ACK`). Se o GPS não responder, por exemplo
       com o fio GPIO5 → RX solto, o firmware volta para 9600 baud / 1 Hz e mostra um aviso.
     - A configuração fica na RAM do GPS e se perde quando ele é desligado, por isso é
       refeita a cada boot.
   - Cria a tarefa `gps_task`.

2. **Leitura (`gps_task`)**
   - Lê bytes da UART e monta uma linha por vez. Um `$` marca o início e `\r`/`\n` o fim.
   - Linhas maiores que o buffer de 128 bytes são descartadas.
   - Os buffers são `static` para não ocupar a pilha da tarefa.

3. **Interpretação NMEA (`nmea_parse`)**
   - Confere o **checksum** (`*XX`) e descarta sentenças corrompidas.
   - Aceita os prefixos `$GP` e `$GN`.
   - **`RMC`**: status (`A` = posição válida), latitude, longitude, velocidade, curso, hora e data.
   - **`GGA`**: qualidade do fix, número de satélites, HDOP e altitude.
   - **`GSA`**: tipo de fix (1 = sem fix, 2 = 2D, 3 = 3D), PDOP e VDOP.
   - **`GST`**: desvio-padrão estimado do erro de latitude, longitude e altitude, em metros.
   - As sentenças do mesmo instante formam uma **época** (`gps_leitura_t`). A época é
     publicada assim que chegam todas as esperadas, ou quando a próxima começa (mudança de
     hora ou tipo repetido). Assim uma sentença perdida não trava a saída. Campos não
     recebidos ficam negativos (desconhecidos).
   - Guarda também a fração do segundo (`ms`), exibida como décimos (`16:10:53.4`).
   - Converte as coordenadas de `ggmm.mmmm` para **graus decimais** (sul e oeste ficam negativos).
   - Converte hora e data para **Unix timestamp** (UTC).

4. **Validação da qualidade (`gps_validar`, em [main/gps_filtro.c](main/gps_filtro.c))**
   - Cada época passa pela camada 1 do filtro e só é **aceita** se todas as condições valerem:

     | Critério | Limite padrão | Motivo da rejeição |
     |---|---|---|
     | RMC `A` e GGA qualidade 1 ou 2 | — | `sem fix` |
     | Fix 3D (GSA) | — | `fix 2D` |
     | Satélites | ≥ `GPS_MIN_SATS` (5) | `poucos sats` |
     | HDOP | ≤ `GPS_MAX_HDOP` (2,5) | `HDOP alto` |
     | Erro horizontal da GST | ≤ `GPS_MAX_ERRO_M` (10 m) | `erro alto` |
     | Leituras boas seguidas | ≥ `GPS_LEITURAS_ESTAVEIS` (5) | `estabiliz.` |

   - Sem GSA ou sem GST (alguns clones não enviam), o critério correspondente é ignorado.
   - Os limites são pontos de partida experimentais: passar neles não garante precisão.
   - A época aceita vai para `gps_aceito`. A última época, aceita ou não, vai para `gps_ultimo`.

5. **Saída no terminal (`gps_print`)**
   - Converte o timestamp para o horário local usando `FUSO_HORARIO`.
   - Sem fix, mostra `Aguardando posicao...` e o número de satélites.
   - Com posição aceita, mostra data/hora, latitude, longitude, altitude, HDOP e erro estimado.
   - Com posição rejeitada, mostra o motivo (`Rejeitada (fix 2D)`, por exemplo).

6. **Display (`display_task`)**
   - É a única tarefa que desenha no OLED depois da inicialização. Ela redesenha a 5 Hz
     (`TELA_ATUALIZA_MS`) e troca de tela a cada `TELA_TROCA_MS` (3 s).
   - **Tela 1, GPS (`tela_gps`)**: data/hora local, lat/lon/alt (ou `Aguardando posicao...` /
     `Posicao rejeitada:` com o motivo), `3D HDOP:1.2 e:3m` (tipo de fix, HDOP, erro estimado)
     e satélites. Sem nenhuma época há mais de 2 s, mostra `GPS sem dados`.
   - **Tela 2, GY-91 (`tela_imu`)**: aceleração total em g no cabeçalho (1,00 parado), roll,
     pitch, rumo, altitude relativa, pressão e temperatura. Se o sensor falhar, mostra
     `GY-91: falha` ou `GY-91 sem dados`.
   - A última linha é igual nas duas telas: número do último pacote LoRa enviado e o número da tela (`1/2`, `2/2`).

Cada época é guardada em um `gps_leitura_t` ([main/gps_filtro.h](main/gps_filtro.h)):

| Campo | Tipo | Origem | Conteúdo |
|---|---|---|---|
| `rmc_ativo` | `bool` | RMC | `true` quando o status é `A` |
| `lat`, `lon` | `double` | RMC | graus decimais |
| `vel_ms`, `curso` | `float` | RMC | velocidade sobre o solo (m/s) e direção (graus) |
| `timestamp`, `ms` | `time_t`, `int` | RMC | Unix time em UTC e milissegundos (0, 200, 400...) |
| `qualidade` | `int` | GGA | 0 sem fix, 1 GPS, 2 DGPS, 6 estimado |
| `sats` | `int` | GGA | satélites usados no cálculo |
| `alt` | `float` | GGA | metros acima do nível do mar |
| `hdop` | `float` | GGA | diluição horizontal da precisão |
| `fix`, `pdop`, `vdop` | `int`, `float` | GSA | tipo de fix (2 = 2D, 3 = 3D) e DOPs |
| `erro_lat`, `erro_lon`, `erro_alt` | `float` | GST | desvio-padrão do erro estimado (m) |

### Saída CSV para análise

Com `SAIDA_CSV 1`, cada época (5 por segundo, aceita ou não) também é impressa como
uma linha `CSV,...`, com cabeçalho no início:

```
CSV,tick_ms,unix,ms,lat,lon,alt,vel_ms,curso,qualidade,fix,sats,hdop,pdop,vdop,erro_lat,erro_lon,erro_alt,motivo
```

Grave o monitor serial em arquivo e filtre as linhas que começam com `CSV,` para
ajustar os limites do filtro no PC.

## GY-91 (atitude e barômetro)

```
GY-91 --I2C_NUM_1 400 kHz--> imu_task (100 Hz) --> filtro complementar --> imu_estado_t --> lora_tx_task
                                                                                       \--> display_task (tela 2)
```

O código é dividido em duas camadas:

- **Driver** ([main/gy91.c](main/gy91.c)): fala com os chips e devolve valores em unidades
  físicas. Ele detecta o chip pelo `WHO_AM_I`: `0x71` é MPU9250, `0x73` é MPU9255 e `0x70`
  é MPU6500, que vem em muitos clones vendidos como MPU9250 e **não tem magnetômetro**.
  - MPU: acelerômetro ±8 g, giroscópio ±1000 °/s, filtro passa-baixa de ~41 Hz, 100 amostras/s.
  - AK8963: modo contínuo de 100 Hz e 16 bits, com o ajuste de sensibilidade de fábrica.
    Os eixos são convertidos para os do acelerômetro.
  - BMP280: pressão ×16, temperatura ×2, filtro IIR ×16 (~23 medições/s), com as fórmulas
    de compensação do datasheet.
- **IMU** ([main/imu.c](main/imu.c)): a `imu_task` lê o sensor a cada 10 ms e calcula:
  - **roll e pitch** com filtro complementar (`IMU_ALPHA` = 0,98: o giroscópio acompanha o
    movimento e a gravidade medida pelo acelerômetro corrige o desvio lento);
  - **rumo magnético** compensado pela inclinação (só com magnetômetro);
  - **altitude barométrica**: absoluta (padrão 1013,25 hPa) e relativa ao ponto onde a placa ligou.

Só a `imu_task` acessa o barramento do GY-91. As outras tarefas leem uma cópia do
resultado com `imu_obter()`, protegida por seção crítica. Essa cópia é considerada
inválida se tiver mais de `IMU_MAX_IDADE_MS` (200 ms).

Eixos do módulo: **X para frente, Y para a esquerda, Z para cima** (o lado dos componentes).
Roll positivo = lado direito desce. Pitch positivo = frente desce.

> O rumo só é confiável depois de calibrar o magnetômetro (`IMU_MAG_OFFSET_*` em
> [main/imu.h](main/imu.h)) e longe de ímãs, motores e fios com corrente. O rumo é
> magnético: para o norte verdadeiro, some a declinação do local.

## Transmissão LoRa

Este projeto é só o **transmissor**. O **receptor** é um projeto separado, gravado em outra
Heltec V3, que precisa usar os mesmos parâmetros de rádio e o mesmo formato de pacote.

```
 Esta placa (transmissor)                                          Outra placa (projeto receptor)
 GPS   -> gps_task -> gps_aceito/gps_ultimo -\
                                              > lora_tx_task ~~~ 915 MHz ~~~> recebe, decodifica e mostra
 GY-91 -> imu_task -> imu_estado_t ----------/   (a cada 500 ms)
```

A `lora_tx_task` acorda a cada `LORA_INTERVALO_MS` (500 ms) e copia as últimas épocas do
GPS (`gps_aceito` e `gps_ultimo`) e o último estado da IMU (`imu_obter`). As duas cópias
são protegidas por seção crítica do FreeRTOS. Depois ela monta uma linha de texto e envia.

O display mostra `LoRa: #N` (último pacote enviado) na última linha das duas telas, e o log
mostra cada linha enviada:

```
I (3679) LORA: #0 enviado (66 bytes, sem posicao: sem fix): 0,12/11/2026 20:55:58,0.000000,0.000000,0.0,4 - 1.4,6.6,285.0,0.00
```

### Formato do pacote (texto, uma linha)

```
CONTADOR,DATA,LAT,LON,ALT,SATS - ROLL,PITCH,RUMO,ALT_BAR
└──────── bloco do GPS ──────┘   └──── bloco do GY-91 ───┘
42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45
```

Os blocos de cada sensor são separados por **`" - "` (espaço, hífen, espaço)** e os campos
por vírgula. Números negativos têm o `-` colado (`,-26.229174`), por isso a separação dos
blocos deve usar `" - "` com os espaços, **nunca só o `-`**.

| Campo | Exemplo | Unidade / formato | Quando vai zerado |
|---|---|---|---|
| `CONTADOR` | `42` | sequencial 0–65535 (volta a 0); o receptor conta as perdas por ele | nunca |
| `DATA` | `12/11/2026 20:55:58` | `DD/MM/AAAA HH:MM:SS`, horário de Brasília (`FUSO_HORARIO`), vem do GPS | `00/00/0000 00:00:00` enquanto o GPS não tem hora ou parou de mandar dados |
| `LAT` | `-26.229174` | graus, 6 casas (~0,1 m) | `0.000000` sem posição aprovada |
| `LON` | `-52.672226` | graus, 6 casas | `0.000000` sem posição aprovada |
| `ALT` | `727.9` | m acima do nível do mar (GPS), 1 casa | `0.0` sem posição aprovada |
| `SATS` | `6` | satélites usados | `0` se o GPS parou de mandar dados |
| `ROLL` | `1.4` | graus, 1 casa (positivo: lado direito desce) | `0.0` se o GY-91 está sem dados |
| `PITCH` | `6.6` | graus, 1 casa (positivo: frente desce) | `0.0` se o GY-91 está sem dados |
| `RUMO` | `285.0` | graus magnéticos 0–359.9, 1 casa | `0.0` sem magnetômetro ou GY-91 sem dados |
| `ALT_BAR` | `0.45` | m, 2 casas: altitude do BMP280 relativa ao ponto onde o transmissor ligou | `0.00` sem BMP280 ou GY-91 sem dados |

**"Sem posição aprovada"** = o filtro do GPS rejeitou a época (sem fix, fix 2D, poucos
satélites, HDOP alto etc.) ou a última aprovada tem mais de `GPS_MAX_IDADE_MS` (1 s). Só LAT,
LON e ALT zeram; DATA e SATS continuam reais para mostrar que o GPS está vivo. O log do
transmissor mostra o motivo (`sem fix`, `fix 2D`, `GPS sem dados`...).

**`ALT` × `ALT_BAR`**: use a `ALT` (GPS) só como referência absoluta. Com poucos satélites ela
varia dezenas de metros com a placa parada. A `ALT_BAR` varia ~5 cm entre leituras, mas
**deriva lentamente** com o clima e o aquecimento (~0,5 m em alguns minutos), então ligue o
transmissor perto da hora do voo.

A linha tem de ~70 a ~90 caracteres (máximo de `PACOTE_MAX` = 128). O receptor
([receptor](../receptor)) explica como ler esses dados no dashboard.

### Rádio (SX1262)

O driver em [main/lora.c](main/lora.c) conversa com o SX1262 por SPI e configura:
TCXO de 1,8 V pelo DIO3, chave de antena pelo DIO2, regulador DC-DC, modo LoRa,
915 MHz, **SF7**, 125 kHz, CR 4/5, **17 dBm**, preâmbulo de 8 símbolos, CRC ligado,
cabeçalho explícito e sync word `0x1424` (rede privada, o padrão do chip).
São os mesmos parâmetros de rádio do projeto de teste do GY-91.
Com esses parâmetros, cada linha de ~70–90 bytes ocupa o ar por **~135–155 ms** (~30% do tempo a 2 pacotes/s).

Pinos internos usados: NSS 8, SCK 9, MOSI 10, MISO 11, RST 12, BUSY 13, DIO1 14.

> **Nunca transmita sem a antena conectada**: a potência refletida pode danificar o SX1262.

## Estrutura do projeto

| Arquivo | Conteúdo |
|---|---|
| [main/main.c](main/main.c) | UART, configuração UBX, parser NMEA, montagem das épocas, saída, tarefa de transmissão LoRa e `app_main` |
| [main/gy91.c](main/gy91.c) / [main/gy91.h](main/gy91.h) | Driver do GY-91 por I2C: MPU9250/6500, magnetômetro AK8963 e BMP280 |
| [main/imu.c](main/imu.c) / [main/imu.h](main/imu.h) | `imu_task` (100 Hz): filtro complementar, rumo, altitude barométrica e estado compartilhado |
| [main/gps_filtro.c](main/gps_filtro.c) / [main/gps_filtro.h](main/gps_filtro.h) | Filtro das leituras (camada 1: validação da qualidade) e seus limites. C puro, sem ESP-IDF |
| [main/oled.c](main/oled.c) / [main/oled.h](main/oled.h) | Driver do SSD1306 por I2C com fonte 5x7 (21 colunas x 8 linhas) |
| [main/lora.c](main/lora.c) / [main/lora.h](main/lora.h) | Driver do SX1262 por SPI (só transmissão) e parâmetros do rádio |
| [main/CMakeLists.txt](main/CMakeLists.txt) | Componentes usados (UART, GPIO, I2C, SPI, timer) e avisos tratados como erro (`-Wall -Wextra -Werror`) |
| [CMakeLists.txt](CMakeLists.txt) | Projeto com `MINIMAL_BUILD`: compila só os componentes necessários |
| [.clangd](.clangd) | Configuração do analisador do editor (veja abaixo) |

## Configuração

No topo de [main/main.c](main/main.c):

| Define | Padrão | Função |
|---|---|---|
| `GPS_RX_PIN` / `GPS_TX_PIN` | 4 / 5 | Pinos da UART do GPS |
| `GPS_BAUD_PADRAO` | 9600 | Velocidade de fábrica do NEO-6M |
| `GPS_BAUD` | 38400 | Velocidade usada depois da configuração |
| `GPS_TAXA_HZ` | 5 | Posições por segundo (1 a 5 no NEO-6M) |
| `FUSO_HORARIO` | -3 | Horas em relação ao UTC (Brasília) |
| `MOSTRAR_NMEA` | 0 | `1` imprime também as sentenças NMEA brutas |
| `SAIDA_CSV` | 0 | `1` imprime todas as épocas (5 Hz) como linhas `CSV,...` |
| `GPS_MODELO_DINAMICO` | 6 | Modelo do filtro interno do GPS: 3 pedestre, 4 automotivo, 6/7/8 aéreo < 1g/2g/4g |
| `LORA_INTERVALO_MS` | 500 | Intervalo entre transmissões (2 pacotes/s) |
| `TELA_TROCA_MS` | 3000 | Tempo que cada tela do display fica visível |
| `TELA_ATUALIZA_MS` | 200 | Intervalo de redesenho do display (5 Hz) |

Em [main/gps_filtro.h](main/gps_filtro.h) (critérios de qualidade):

| Define | Padrão | Função |
|---|---|---|
| `GPS_MIN_SATS` | 5 | Mínimo de satélites (o fix 3D exige 4, mas sem nenhuma redundância) |
| `GPS_MAX_HDOP` | 2.5 | HDOP máximo |
| `GPS_MAX_ERRO_M` | 10.0 | Erro horizontal máximo estimado pela GST (m); `0` desliga |
| `GPS_LEITURAS_ESTAVEIS` | 5 | Leituras boas seguidas antes de aceitar (1 s a 5 Hz) |
| `GPS_MAX_IDADE_MS` | 1000 | Idade máxima da posição para ela ser transmitida |

Em [main/lora.h](main/lora.h) (precisam ser **iguais** no projeto do receptor):

| Define | Padrão | Função |
|---|---|---|
| `LORA_FREQ_HZ` | 915000000 | Frequência (Brasil: 902–907,5 e 915–928 MHz) |
| `LORA_SF` | 7 | Spreading factor 7–12: maior = mais alcance e mais tempo no ar |
| `LORA_BW` | 0x04 (125 kHz) | Largura de banda |
| `LORA_CR` | 0x01 (4/5) | Coding rate |
| `LORA_TX_DBM` | 17 | Potência de transmissão (−9 a 22 dBm) |

Em [main/imu.h](main/imu.h) e [main/gy91.h](main/gy91.h) (GY-91):

| Define | Padrão | Função |
|---|---|---|
| `IMU_ALPHA` | 0.98 | Peso do giroscópio no filtro complementar (maior = menos sensível a acelerações) |
| `IMU_AMOSTRAS_CALIB` | 100 | Amostras da calibração do giroscópio (1 s, placa parada) |
| `IMU_MAG_OFFSET_X/Y/Z` | 0 | Offset do magnetômetro (hard iron, µT) |
| `GY91_ACEL_FS` | ±8 g | Fundo de escala do acelerômetro |
| `GY91_GIRO_FS` | ±1000 °/s | Fundo de escala do giroscópio |

Para mudar **o que aparece no terminal**, edite `gps_print()`. Para o **display**, edite
`tela_gps()` e `tela_imu()`; para adicionar uma tela, aumente `TELA_NUM` e trate o novo
número na `display_task`.
No display, use `oled_printf(coluna, linha, ...)` (coluna 0–20, linha 0–7) entre
`oled_clear()` e `oled_update()`. A fonte só tem ASCII, sem acentos.

## Compilar e gravar

Requer **ESP-IDF v6.1**, com o alvo `esp32s3` já definido no `sdkconfig`.

Pela IDE da Espressif, use **Build, Flash and Monitor**. Pelo terminal do ESP-IDF
(troque `COM10` pela porta da placa):

```bash
idf.py -p COM10 flash monitor
```

Para sair do monitor, use `Ctrl + ]`.

## Problemas comuns

| Sintoma | Causa / solução |
|---|---|
| Só aparece `Aguardando posicao... (satelites: 0)` | O GPS ainda não tem sinal. Use a antena com o céu aberto e espere de 1 a 5 min na primeira vez. O LED do NEO-6M pisca quando há posição. |
| Nenhuma linha do GPS | Confira se o TX do GPS está no GPIO4, o GND comum e a alimentação. Use `MOSTRAR_NMEA 1` para ver os dados brutos. |
| `GPS nao confirmou a configuracao` | O GPS não recebeu os comandos UBX. Confira o fio GPIO5 → RX do GPS. O firmware continua funcionando a 9600 baud / 1 Hz. |
| Posição "passeando" dezenas de metros parada | Sinal fraco ou poucos satélites (comum em ambiente fechado). Ao ar livre o erro típico é de 2 a 5 m. |
| `Posicao rejeitada` por muito tempo | Os critérios de [main/gps_filtro.h](main/gps_filtro.h) estão mais rígidos que o sinal disponível. Veja o motivo no display e no log; com céu aberto costuma passar em 1 a 3 min. |
| `GPS nao aceitou configurar a sentenca GST` | O módulo (alguns clones) não suporta GST. O filtro segue sem o critério de erro estimado. |
| `Could not open COM.. Acesso negado` | A porta está aberta por outro programa (monitor serial, Arduino IDE etc.). Feche-o e grave de novo. |
| `ClearCommError failed` no monitor | O USB desconectou (cabo, mau contato ou queda de alimentação). Reconecte e reabra o monitor. |
| `Falha ao iniciar o display` | O I2C não encontrou o OLED. Confira se a placa é a V3 (pinos 17/18/21). |
| `Falha ao iniciar o GY-91: ESP_ERR_NOT_FOUND` | O MPU não respondeu em `0x68`/`0x69`. Confira SDA → GPIO41, SCL → GPIO42, VIN → 3V3 e GND. |
| `GY-91: MPU6500 (sem magnetometro)` | O módulo é um clone com MPU6500. Roll e pitch funcionam, mas não há rumo (`RUMO` vai sempre `0.0`). |
| Roll/pitch derivando com a placa parada | A placa se mexeu durante a calibração. Reinicie com ela parada. |
| `erros I2C!` no log da IMU | Mau contato ou fios longos no GY-91. Use fios curtos; se persistir, reduza `GY91_I2C_HZ` em [main/gy91.c](main/gy91.c) para 100000. |
| `Falha ao iniciar o radio: ESP_ERR_NOT_FOUND` | O SX1262 não respondeu pelo SPI. Confira se a placa é a V3. |
| Receptor não recebe nada | Os parâmetros de `lora.h` e o formato do pacote precisam ser iguais no projeto do receptor, e as duas placas precisam de antena. Teste primeiro com as placas a poucos metros. |
| Receptor mostra `Pacote descartado` para todos os pacotes | Um dos dois ainda está com o formato antigo (binário de 21/27/31 bytes, ou o `pacote_t` de 12 bytes do projeto de teste do GY-91). Grave a versão atual (linha de texto) nas **duas** placas. |
| Aviso de Unicode do `idf.py` | Só afeta o terminal do Windows. Ative "UTF-8 Beta" nas configurações regionais ou defina `PYTHONUTF8=1`. |
| Editor acusa `stddef.h file not found` | O pacote `esp-clangd` vem sem os headers internos. O [.clangd](.clangd) aponta `-resource-dir` para o `esp-clang`; ajuste a versão no caminho se atualizar as ferramentas. |

## Próximos passos

- Gravar os pontos em CSV/GPX para visualizar o trajeto em um mapa.
- Filtro, camada 2: rejeitar saltos incompatíveis com a velocidade máxima do aeromodelo.
- Filtro, camada 3: suavização (mediana e depois Kalman com a velocidade da RMC).
- No projeto do receptor, repassar os pacotes por Wi-Fi para um mapa em tempo real (Traccar, Node-RED etc.).

## Licença

Domínio público ou CC0, conforme o arquivo [LICENSE](LICENSE).
