# Receptor LoRa com Heltec WiFi LoRa 32 V3

Firmware em **ESP-IDF** que recebe pelo rádio **LoRa (SX1262)** a telemetria enviada pelo
projeto transmissor ([transmissor](../transmissor)), com dados do GPS NEO-6M e do GY-91.
Ele confere cada linha recebida, conta os pacotes perdidos, mostra os dados no display OLED
e repassa tudo pela **serial** em um formato pronto para o dashboard.

```
TLM:42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45 - -48,12.5
TLM:43,12/11/2026 20:55:58,-26.229175,-52.672227,727.8,6 - 1.5,6.6,285.0,0.46 - -47,12.8
I (60292) RX: ===== RESUMO: recebidos 120 | perdidos 0 (0.0%) | corrompidos 0 | RSSI min -59 / med -53 / max -42 dBm | SNR med 12.5 dB =====
```

```
┌─────────────────────┐
│RX #42  Perd: 0      │
│─────────────────────│
│Lat: -26.229174      │
│Lon: -52.672226      │
│Alt:728 Bar:+0.5     │
│12/11 20:55:58 Sat:6 │
│R:+1 P:+7 Rumo:285   │
│RSSI:-48 SNR:12.5    │
└─────────────────────┘
```

## Formato da telemetria

### Linha enviada pelo transmissor (pacote LoRa)

Cada pacote LoRa é **uma linha de texto ASCII** (sem `\n`), enviada 2 vezes por segundo:

```
CONTADOR,DATA,LAT,LON,ALT,SATS - ROLL,PITCH,RUMO,ALT_BAR
└──────── bloco do GPS ──────┘   └──── bloco do GY-91 ───┘
42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45
```

### Linha que o receptor imprime na serial (para o dashboard)

O receptor repassa a linha **exatamente como chegou**, com o prefixo `TLM:` e um terceiro
bloco com a qualidade do sinal medida por ele:

```
TLM:CONTADOR,DATA,LAT,LON,ALT,SATS - ROLL,PITCH,RUMO,ALT_BAR - RSSI,SNR
TLM:42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45 - -48,12.5
    └──────────── bloco 1: GPS ──────────────────┘   └── bloco 2: GY-91 ─┘   └ bloco 3 ┘
```

### Campos

| Bloco | # | Campo | Exemplo | Tipo | Unidade / formato | Valor quando não há dado |
|---|---|---|---|---|---|---|
| 1 GPS | 0 | `CONTADOR` | `42` | inteiro | sequencial 0–65535, +1 por pacote; volta a 0 depois de 65535 ou quando o transmissor reinicia | — |
| 1 GPS | 1 | `DATA` | `12/11/2026 20:55:58` | texto | `DD/MM/AAAA HH:MM:SS`, **horário de Brasília (UTC−3)**, vem do GPS | `00/00/0000 00:00:00` (GPS sem hora ou sem dados) |
| 1 GPS | 2 | `LAT` | `-26.229174` | decimal | graus, 6 casas (sul negativo) | `0.000000` (sem posição) |
| 1 GPS | 3 | `LON` | `-52.672226` | decimal | graus, 6 casas (oeste negativo) | `0.000000` (sem posição) |
| 1 GPS | 4 | `ALT` | `727.9` | decimal | metros acima do nível do mar (GPS), 1 casa | `0.0` (sem posição) |
| 1 GPS | 5 | `SATS` | `6` | inteiro | satélites usados no cálculo | `0` (GPS sem dados) |
| 2 GY-91 | 0 | `ROLL` | `1.4` | decimal | graus, 1 casa; positivo = lado direito desce | `0.0` |
| 2 GY-91 | 1 | `PITCH` | `6.6` | decimal | graus, 1 casa; positivo = frente desce | `0.0` |
| 2 GY-91 | 2 | `RUMO` | `285.0` | decimal | graus **magnéticos**, 0–359.9 (0 = norte magnético) | `0.0` |
| 2 GY-91 | 3 | `ALT_BAR` | `0.45` | decimal | metros, 2 casas: altitude do barômetro **relativa ao ponto onde o transmissor ligou** | `0.00` |
| 3 sinal | 0 | `RSSI` | `-48` | inteiro | dBm, potência recebida (só na saída `TLM:`) | — |
| 3 sinal | 1 | `SNR` | `12.5` | decimal | dB, relação sinal/ruído (só na saída `TLM:`) | — |

Separador decimal é sempre o **ponto**. Não há aspas nem espaços extras, exceto o espaço
dentro de `DATA` e os espaços do separador `" - "`.

### Regras para interpretar

1. **Divida os blocos por `" - "` (espaço, hífen, espaço), nunca só por `-`.** Números
   negativos têm o `-` colado na vírgula (`,-26.229174` ou `6 - -1.4`). Um `split("-")`
   quebra a linha errado.
2. Divida cada bloco por `,`. O bloco 1 tem 6 campos, o bloco 2 tem 4 e o bloco 3 tem 2.
   Uma linha com outra quantidade deve ser descartada.
3. **GPS sem sinal** (posição reprovada pelo filtro do transmissor: sem fix, fix 2D, menos de 5
   satélites, HDOP alto ou posição com mais de 1 s): `LAT`, `LON` e `ALT` vêm **zerados**.
   `DATA` e `SATS` continuam reais enquanto o GPS manda dados, o que mostra que ele está vivo,
   mas sem posição boa. Para saber se há posição, teste `LAT != 0 || LON != 0`.
4. **GPS desligado ou desconectado**: além disso, `DATA` vem `00/00/0000 00:00:00` e `SATS`
   vem `0`.
5. **GY-91 sem dados**: o bloco 2 inteiro vem zerado (`0.0,0.0,0.0,0.00`). Sem magnetômetro
   (módulo com MPU6500), só o `RUMO` fica sempre `0.0`. Um zero isolado em `ROLL`/`PITCH` pode
   ser real (placa nivelada). Se o bloco inteiro for zero por vários pacotes seguidos, o sensor
   está sem dados.
6. **Pacotes perdidos**: compare o `CONTADOR` com o anterior. Um salto de 41 para 44 significa
   2 perdidos. Se o contador **diminuir** muito, o transmissor reiniciou. Nesse caso
   recomece a contagem e não conte como perda, e lembre que o zero do `ALT_BAR` também mudou.
7. **Altitude**: use `ALT_BAR` para a altura do voo, porque varia ~5 cm entre leituras. A
   `ALT` do GPS serve só como referência absoluta: com poucos satélites varia dezenas de
   metros com a placa parada. O `ALT_BAR` deriva lentamente com o clima (~0,5 m em alguns
   minutos), então ligue o transmissor pouco antes do voo.
8. **Hora**: `DATA` tem resolução de 1 s e chegam 2 pacotes por segundo, então dois pacotes
   seguidos podem ter a mesma `DATA`. Para ordenar, use o `CONTADOR`, ou a hora em que o
   dashboard recebeu a linha.

### Lendo pela serial (dashboard)

Conecte o receptor por USB e leia a porta COM em **115200 baud, 8N1**. Além das linhas `TLM:`,
a serial também tem o log de boot e linhas de log do ESP-IDF (`I (...)`, `W (...)`, `E (...)`)
com o resumo e os avisos. **Considere só as linhas que começam com `TLM:`** e ignore as outras.

Exemplo em Python (`pip install pyserial`):

```python
import serial

def ler_tlm(linha: str):
    """Converte uma linha 'TLM:...' em dicionário. Retorna None se o formato não bater."""
    if not linha.startswith("TLM:"):
        return None
    blocos = linha[4:].strip().split(" - ")          # separa por " - ", nunca só por "-"
    if len(blocos) != 3:
        return None
    gps, imu, sinal = (b.split(",") for b in blocos)
    if len(gps) != 6 or len(imu) != 4 or len(sinal) != 2:
        return None
    lat, lon = float(gps[2]), float(gps[3])
    return {
        "contador": int(gps[0]),
        "data": gps[1],                               # "DD/MM/AAAA HH:MM:SS" (UTC-3)
        "com_hora": gps[1] != "00/00/0000 00:00:00",
        "lat": lat, "lon": lon, "alt": float(gps[4]),
        "com_posicao": lat != 0.0 or lon != 0.0,
        "sats": int(gps[5]),
        "roll": float(imu[0]), "pitch": float(imu[1]),
        "rumo": float(imu[2]), "alt_bar": float(imu[3]),
        "rssi": int(sinal[0]), "snr": float(sinal[1]),
    }

with serial.Serial("COM11", 115200, timeout=1) as porta:   # troque pela porta do receptor
    for bruta in porta:
        dados = ler_tlm(bruta.decode("ascii", errors="ignore"))
        if dados:
            print(dados)
```

Para converter a `DATA`: `datetime.strptime(d, "%d/%m/%Y %H:%M:%S")`. O resultado é o horário
de Brasília (UTC−3).

### Se o formato mudar

O formato está definido em `lora_tx_task` no `main.c` do transmissor e em
`telemetria_parse` no [main/main.c](main/main.c) do receptor. Para acrescentar um campo,
coloque-o no **fim do bloco** do sensor correspondente, atualize os dois projetos, este
README e o parser do dashboard **juntos**. Um bloco com quantidade diferente de campos é
descartado pelo receptor (`Pacote descartado: formato invalido`).

## Hardware

| Item | Detalhe |
|---|---|
| Placa | Heltec WiFi LoRa 32 **V3** (ESP32-S3 + SX1262) |
| Rádio | SX1262 interno, 915 MHz, **com antena conectada** |
| Display | OLED SSD1306 128x64 da própria placa (I2C) |

Não há ligações externas: tudo usa os componentes da própria placa.

| Função | GPIO |
|---|---|
| Vext (alimentação do OLED) | 36 (LOW = ligado) |
| OLED SDA / SCL / RST | 17 / 18 / 21 |
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 |
| SX1262 RST / BUSY / DIO1 | 12 / 13 / 14 |

> **Nunca ligue a placa sem a antena conectada.** Mesmo o receptor pode transmitir
> (o driver tem `lora_send`), e a potência refletida sem antena pode danificar o SX1262.
> Na recepção, sem antena o alcance cai para poucos metros.

## Como funciona

```
 Transmissor (transmissor/)                                    Esta placa (receptor)
 GPS + GY-91 -> lora_tx_task ~~~ 915 MHz, a cada 500 ms ~~~> SX1262 -> lora_rx_task -> serial "TLM:" -> dashboard
                                                                                  \-> display OLED
```

1. **Inicialização (`app_main`)**
   - Liga o **Vext** (GPIO36 em LOW), que alimenta o OLED.
   - Inicializa o **display** (`oled_init`). Se falhar, registra o erro e segue só com a serial.
   - Inicializa o **rádio** (`lora_init`). Se falhar, mostra o erro no log e no display e para.
   - Cria a tarefa `lora_rx_task`.

2. **Recepção (`lora_rx_task`)**
   - Coloca o rádio em **recepção contínua** (`lora_start_rx`): depois de cada pacote o
     SX1262 continua ouvindo sozinho, sem perder o próximo.
   - `lora_receive` consulta as interrupções do rádio a cada 10 ms. Quando chega um pacote,
     lê o tamanho, os dados e a qualidade do sinal (RSSI e SNR).
   - Descarta, com aviso no log:
     - pacotes **corrompidos** (erro de CRC ou de cabeçalho), que são contados;
     - pacotes **binários** ou maiores que 128 bytes, que vêm de outros aparelhos LoRa na
       mesma frequência;
     - linhas fora do formato (`telemetria_parse`): sem exatamente dois blocos separados por
       `" - "`, com quantidade errada de campos, número inválido ou `DATA` fora do padrão.

3. **Pacotes perdidos (`conta_perdidos`)**
   - Um salto no `CONTADOR` de `#10` para `#13` significa 2 pacotes perdidos.
   - A diferença é calculada em 16 bits, então a volta de 65535 para 0 não conta como perda.
   - Se o contador "volta para trás" (por exemplo de `#250` para `#0`), o transmissor foi
     reiniciado: o receptor só mostra um aviso e recomeça a contagem dali.

4. **Saída**
   - **Serial**: imprime `TLM:<linha recebida> - RSSI,SNR` (veja
     [Formato da telemetria](#formato-da-telemetria)).
   - **Display** (`telemetria_print`): sem posição, mostra `TX sem posicao` e a altitude
     barométrica. Sem hora, mostra `Sem hora`.
   - Com `LOG_LEGIVEL 1`, também imprime cada pacote em formato de log legível.

5. **Resumo (`resumo_print`)**
   - A cada `RESUMO_A_CADA` pacotes válidos (1 min), imprime recebidos, perdidos (e a
     porcentagem), corrompidos, RSSI mínimo/médio/máximo e SNR médio **desse bloco de pacotes**.

### Display (21 colunas x 8 linhas)

| Linha | Conteúdo |
|---|---|
| 0 | `RX #N  Perd: X` (contador do pacote e total de perdidos) |
| 1 | divisória |
| 2–4 | Latitude, longitude e altitudes GPS + barométrica (`Alt:728 Bar:+0.5`). Sem posição: `TX sem posicao` e `Alt bar: +0.45 m` |
| 5 | Data/hora curta e satélites (`12/11 20:55:58 Sat:6`), ou `Sem hora` |
| 6 | Roll, pitch e rumo em graus inteiros (`R:+1 P:+7 Rumo:285`) |
| 7 | RSSI e SNR do último pacote |

### Lendo RSSI e SNR

| Medida | Bom | No limite |
|---|---|---|
| RSSI | acima de −100 dBm | perto de −123 dBm (sensibilidade do SF7/125 kHz) |
| SNR | positivo | perto de −7,5 dB (limite de demodulação do SF7) |

Com as placas na mesma mesa o RSSI fica entre −30 e −60 dBm. Muito perto (menos de 1 m)
o receptor pode saturar e começar a corromper pacotes: afaste as placas.

## Parâmetros que precisam bater com o transmissor

Em [main/lora.h](main/lora.h) e [main/lora.c](main/lora.c). Se **qualquer** um for diferente,
o receptor não recebe nada (nem pacotes corrompidos).

| Parâmetro | Valor |
|---|---|
| Frequência (`LORA_FREQ_HZ`) | 915 MHz |
| Modulação | LoRa |
| Spreading factor (`LORA_SF`) | **7** |
| Largura de banda (`LORA_BW`) | 0x04 = 125 kHz |
| Coding rate (`LORA_CR`) | 0x01 = 4/5 |
| Preâmbulo (`LORA_PREAMBULO`) | 8 símbolos |
| Cabeçalho | explícito |
| CRC | ligado |
| IQ | normal |
| Sync word | padrão do SX1262 (0x1424, não alterado) |

Configuração do SX1262 na Heltec V3: TCXO de 1,8 V pelo DIO3, chave de antena pelo DIO2 e
regulador DC-DC. Cada linha (~70–90 bytes) ocupa o ar por ~135–155 ms; o transmissor envia 2
por segundo.

## Estrutura do projeto

| Arquivo | Conteúdo |
|---|---|
| [main/main.c](main/main.c) | Formato e parser da telemetria, estatísticas, saída serial/display, tarefa de recepção e `app_main` |
| [main/lora.c](main/lora.c) / [main/lora.h](main/lora.h) | Driver do SX1262 por SPI (recepção contínua e transmissão) e parâmetros do rádio |
| [main/oled.c](main/oled.c) / [main/oled.h](main/oled.h) | Driver do SSD1306 por I2C com fonte 5x7 (21 colunas x 8 linhas) |
| [main/CMakeLists.txt](main/CMakeLists.txt) | Componentes usados (GPIO, I2C, SPI) e avisos tratados como erro (`-Wall -Wextra -Werror`) |
| [CMakeLists.txt](CMakeLists.txt) | Projeto `lora-receptor` com `MINIMAL_BUILD`: compila só os componentes necessários |
| [.clangd](.clangd) | Configuração do analisador do editor |

## Configuração

No topo de [main/main.c](main/main.c):

| Define | Padrão | Função |
|---|---|---|
| `RESUMO_A_CADA` | 120 | Pacotes válidos entre cada resumo (120 × 500 ms = 1 min) |
| `RX_TIMEOUT_MS` | 1000 | Tempo de cada espera do `lora_receive` |
| `LOG_LEGIVEL` | 0 | `1` imprime também cada pacote em formato legível (além da linha `TLM:`) |
| `SAIDA_PREFIXO` | `"TLM:"` | Prefixo das linhas de telemetria na serial |

Para mudar **o display**, edite `telemetria_print()`. Use `oled_printf(coluna, linha, ...)`
(coluna 0–20, linha 0–7) entre `oled_clear()` e `oled_update()`. A fonte só tem ASCII, sem acentos.

## Compilar e gravar

Requer **ESP-IDF v6.1**, com o alvo `esp32s3` já definido no `sdkconfig`.

Pela IDE da Espressif, use **Build, Flash and Monitor**. Pelo terminal do ESP-IDF
(troque `COMx` pela porta da placa receptora):

```bash
idf.py -p COMx flash monitor
```

Para sair do monitor, use `Ctrl + ]`.

Com duas placas ligadas no mesmo PC, cada uma aparece em uma porta COM diferente. Veja no
Gerenciador de Dispositivos qual é qual e abra um terminal para cada uma. A porta que o
dashboard lê não pode estar aberta ao mesmo tempo no PuTTY ou no `idf.py monitor`.

## Problemas comuns

| Sintoma | Causa / solução |
|---|---|
| `Receptor pronto` e nenhum pacote | Confira se o transmissor mostra `#N enviado`, se os parâmetros de `lora.h` são iguais nos dois projetos e se as duas placas têm antena. Teste primeiro com as placas a 2–5 m. |
| `Pacote corrompido (CRC/cabecalho)` frequente | Sinal fraco (RSSI perto de −120 dBm) ou placas encostadas uma na outra (saturação). Ajuste a distância ou a posição das antenas. |
| `Pacote descartado: N bytes que nao sao uma linha de telemetria` esporádico | Pacote de outro aparelho LoRa na mesma frequência. É normal e não atrapalha. |
| `Pacote descartado` em **todos** os pacotes | O transmissor está com um formato antigo (binário de 21/27/31 bytes). Grave a versão atual nas **duas** placas. |
| `Pacote descartado: formato invalido "..."` | A linha chegou, mas não bate com o formato (campo a mais ou a menos). Transmissor e receptor estão com versões diferentes do formato. |
| Nenhum pacote depois da atualização | Transmissor e receptor precisam estar ambos em **SF7**. Uma placa antiga em SF9 não conversa com uma nova em SF7. |
| Bloco do GY-91 sempre `0.0,0.0,0.0,0.00` | O GY-91 do transmissor falhou ou não está ligado. Veja o log do transmissor (`Falha ao iniciar o GY-91`). |
| `ALT_BAR` "andando" com o transmissor parado | Normal: a pressão atmosférica e a temperatura mudam (~0,5 m em alguns minutos). O zero é o momento em que o transmissor ligou; reinicie-o pouco antes do voo. |
| `Perdidos` aumentando | Pacotes que não chegaram (distância, obstáculos, interferência). Veja RSSI e SNR no resumo. |
| `Contador voltou ... transmissor reiniciado?` | O transmissor foi reiniciado e o contador recomeçou em 0. É só um aviso. |
| `TX sem posicao` / `LAT` e `LON` zerados | O rádio funciona, mas o GPS do transmissor ainda não tem posição aprovada. Leve-o para o céu aberto. |
| `Sem hora` / `DATA` = `00/00/0000 00:00:00` | O GPS do transmissor ainda não recebeu a hora dos satélites ou parou de mandar dados. |
| `Falha ao iniciar o radio: ESP_ERR_NOT_FOUND` | O SX1262 não respondeu pelo SPI. Confira se a placa é a V3. |
| `Falha ao iniciar o display` | O I2C não encontrou o OLED. Confira se a placa é a V3 (pinos 17/18/21). O receptor continua funcionando pela serial. |
| `Could not open COM.. Acesso negado` | A porta está aberta por outro programa (PuTTY, monitor serial, dashboard). Feche-o e grave de novo. |
| Aviso de Unicode do `idf.py` | Só afeta o terminal do Windows. Ative "UTF-8 Beta" nas configurações regionais ou defina `PYTHONUTF8=1`. |
| Editor acusa `stddef.h file not found` | O [.clangd](.clangd) aponta `-resource-dir` para o `esp-clang`; ajuste a versão no caminho se atualizar as ferramentas. |

## Próximos passos

- Dashboard lendo as linhas `TLM:` da serial (veja [Lendo pela serial](#lendo-pela-serial-dashboard)).
- Calcular a distância e a direção até o transmissor a partir de uma posição fixa.
- Gravar as linhas recebidas em CSV para análise depois do voo.

## Licença

Domínio público ou CC0, conforme o arquivo [LICENSE](LICENSE).
