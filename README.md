# Telemetria-Epaer

Sistema de telemetria para aeromodelo utilizando LoRa, desenvolvido para a **EPAER – Equipe Patobranquense de AeroDesign**.

**Alunos Oficina Integração:** Laura Copetti, Entony Siminhuk e Isaque Santana.
**Alunos IoT:** Laura Copetti, Entony Siminhuk e Lorenzo Bonetti.

## Contexto

A EPAER é um projeto de extensão no qual os alunos desenvolvem aeromodelos para a competição **SAE Brasil**.

- **Problema:** durante testes e competições, é difícil obter informações objetivas sobre o comportamento da aeronave em tempo real.
- **Por que importa:** sem esses dados, fica difícil analisar o voo e identificar melhorias no projeto.
- **Solução:** um sistema de telemetria que usa **LoRa (Long Range)**, um padrão de comunicação de longa distância e baixo consumo de energia.

## Visão geral

Um sistema embarcado completo que coleta dados do aeromodelo durante o voo e os transmite em tempo real para uma estação de solo. Um broker publica os dados via protocolo **MQTT**, a nuvem os armazena e um dashboard os exibe.

```
Coletar (sensores) → Transmitir (LoRa) → Publicar (broker MQTT) → Armazenar (nuvem) → Visualizar (dashboard)
```

### Dados monitorados

| Categoria | Dados |
|-----------|-------|
| Velocidade | Velocidade em relação ao solo (GPS) |
| Navegação | Posição geográfica, altitude, roll, pitch e yaw |
| Potência | Corrente e tensão da bateria e carga estimada |

## Arquitetura

1. **Camada embarcada (aeromodelo):** sensores GY-91, GPS, corrente e tensão; processamento local no ESP32; transmissão via LoRa.
2. **Camada de borda (estação de solo):** recebe os dados LoRa em um ESP32, que atua como gateway MQTT para a internet.
3. **Camada de nuvem:** assina o broker MQTT, armazena os dados dos voos e alimenta o dashboard de monitoramento.

### Comunicação

- **LoRa:** conexão de longo alcance (cerca de 5 km em áreas urbanas e 15 km em áreas rurais) para transmissão contínua entre o aeromodelo e a estação de solo.
- **MQTT:** mensagens leves (publish/subscribe) para enviar os dados até a nuvem de forma eficiente.

## Hardware

### A bordo do aeromodelo

| Componente | Função |
|------------|--------|
| **ESP32** | Microcontrolador principal. Processa os dados e controla e gerencia os módulos e sensores. |
| **GY-91** | IMU de 9 eixos (acelerômetro, giroscópio e magnetômetro) com barômetro, para medir movimento, orientação e altitude. |
| **GPS (GY-GPS6MV2)** | Fornece posição geográfica, altitude e velocidade em tempo real. |
| **Sensor de corrente e tensão** | Monitora o sistema elétrico para controle e proteção da bateria. |
| **Módulo LoRa** | Transmite os dados para a estação de solo, com longo alcance e baixo consumo. |

### Estação em solo

| Componente | Função |
|------------|--------|
| **Módulo LoRa** | Recebe os dados transmitidos pelo aeromodelo. |
| **ESP32** | Processa os dados e os encaminha para a nuvem. |
| **Computador** | Visualização dos dados. |

## MVP

Ao final do semestre, o sistema demonstrará o fluxo IoT completo, da aquisição à visualização:

1. Coletar dados dos sensores
2. Transmitir via LoRa
3. Publicar via MQTT na nuvem
4. Visualizar os dados no dashboard

## Cronograma

| Etapa | Atividade | Semanas |
|-------|-----------|---------|
| 1 | Integração ESP32 LoRa + GY-91 | 1–3 |
| 2 | Integração ESP32 LoRa + GY-GPS6MV2 | 1–3 |
| 3 | Integração ESP32 LoRa + sensores de corrente e tensão | 1–3 |
| 4 | Envio dos dados à estação base LoRa | 4–5 |
| 5 | Publicação dos dados no broker MQTT | 6–7 |
| 6 | API para consumo dos dados | 8–9 |
| 7 | Dashboard de visualização | 10–11 |
| 8 | Testes e validação do sistema | 11–12 |

## Código

### Estrutura do repositório

| Pasta | Placa | O que faz |
|-------|-------|-----------|
| [`transmissor/`](transmissor) | Heltec WiFi LoRa 32 V3 **a bordo do aeromodelo** | Lê o GPS (NEO-6M) e o GY-91 (MPU9250 + BMP280), mostra os dados no display e transmite a telemetria via LoRa 2 vezes por segundo |
| [`receptor/`](receptor) | Heltec WiFi LoRa 32 V3 **na estação de solo** | Recebe a telemetria via LoRa, confere o formato, conta pacotes perdidos e repassa as linhas pela serial (`TLM:...`) para o dashboard |

Os dois são projetos **ESP-IDF v6.1** independentes, cada um com o seu `CMakeLists.txt` e
`sdkconfig`. Cada pasta tem um README próprio com ligações, funcionamento e configuração.

### Formato da telemetria

O transmissor envia uma linha de texto por pacote, com os dados de cada sensor separados por
`" - "`:

```
CONTADOR,DATA,LAT,LON,ALT,SATS - ROLL,PITCH,RUMO,ALT_BAR
42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45
```

O receptor imprime na serial `TLM:<linha recebida> - RSSI,SNR`. A descrição completa dos
campos, as regras de interpretação (valores zerados, pacotes perdidos, altitude) e um exemplo
de leitura em Python estão em
[receptor/README.md, seção "Formato da telemetria"](receptor/README.md#formato-da-telemetria).

> Transmissor, receptor e dashboard dependem do mesmo formato. Qualquer mudança nele deve
> alterar os três **no mesmo commit**.

### Compilar e gravar

Com o ESP-IDF v6.1 instalado, abra a pasta do projeto desejado na IDE da Espressif (ou no
terminal do ESP-IDF) e grave cada placa com o seu firmware:

```bash
cd transmissor
idf.py -p COMx flash monitor
```

```bash
cd receptor
idf.py -p COMy flash monitor
```
