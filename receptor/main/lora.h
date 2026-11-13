#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Radio SX1262 da Heltec WiFi LoRa 32 V3 (LoRa ponto a ponto, sem LoRaWAN).
// Este projeto e o RECEPTOR. Os parametros abaixo precisam ser IGUAIS aos
// do transmissor (pasta transmissor/), senao nenhum pacote e recebido.

#define LORA_FREQ_HZ    915000000  // Brasil (Anatel): 902-907,5 e 915-928 MHz
#define LORA_SF         7          // spreading factor 7..12 (maior = mais alcance, mais lento)
#define LORA_BW         0x04       // largura de banda: 0x04 = 125 kHz, 0x05 = 250 kHz, 0x06 = 500 kHz
#define LORA_CR         0x01       // coding rate: 0x01 = 4/5 ... 0x04 = 4/8
#define LORA_TX_DBM     17         // potencia de transmissao: -9..22 dBm (so usada no lora_send)
#define LORA_PREAMBULO  8          // simbolos de preambulo

esp_err_t lora_init(void);

// Transmite e espera o fim do envio (bloqueia por ~150 ms com SF7 e ~85 bytes).
// Interrompe a recepcao: chame lora_start_rx() de novo depois.
esp_err_t lora_send(const uint8_t *data, size_t len);

// Coloca o radio em recepcao continua (continua ouvindo depois de cada pacote)
esp_err_t lora_start_rx(void);

// Espera um pacote por ate timeout_ms. Retorna:
//   > 0  tamanho do pacote (copia no maximo max bytes em buf) e preenche rssi (dBm) e snr (dB)
//     0  nada chegou dentro do timeout
//    -1  pacote corrompido (erro de CRC ou de cabecalho)
//    -2  falha na comunicacao SPI com o radio
int lora_receive(uint8_t *buf, size_t max, int *rssi, float *snr, uint32_t timeout_ms);
