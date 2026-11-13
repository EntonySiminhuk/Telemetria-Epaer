#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Modulo GY-91: MPU9250 (acelerometro + giroscopio + magnetometro AK8963)
// e BMP280 (pressao + temperatura), ligados por I2C.
//
// Ligacao na Heltec WiFi LoRa 32 V3:
//   VIN -> 3V3   GND -> GND   SDA -> GPIO41   SCL -> GPIO42
//   (SAO/SDO, NCS e CSB desligados)
//
// Usa o controlador I2C_NUM_1. O I2C_NUM_0 fica com o OLED (GPIO17/18).
// Todas as funcoes devem ser chamadas pela MESMA task (o driver nao tem
// trava propria): no projeto, so a imu_task acessa o sensor.

// Fundo de escala: limite de cada sensor (acima disso a leitura satura)
typedef enum {
    GY91_ACEL_2G = 0,
    GY91_ACEL_4G,
    GY91_ACEL_8G,
    GY91_ACEL_16G,
} gy91_acel_fs_t;

typedef enum {
    GY91_GIRO_250DPS = 0,
    GY91_GIRO_500DPS,
    GY91_GIRO_1000DPS,
    GY91_GIRO_2000DPS,
} gy91_giro_fs_t;

#define GY91_ACEL_FS    GY91_ACEL_8G
#define GY91_GIRO_FS    GY91_GIRO_1000DPS

// Chip de movimento encontrado (registrador WHO_AM_I)
typedef enum {
    GY91_IMU_DESCONHECIDO = 0,
    GY91_IMU_MPU9250,   // 0x71: tem magnetometro AK8963
    GY91_IMU_MPU9255,   // 0x73: tem magnetometro AK8963
    GY91_IMU_MPU6500,   // 0x70: comum em clones "MPU9250", SEM magnetometro
} gy91_imu_t;

typedef struct {
    gy91_imu_t imu;
    uint8_t    who_am_i;
    uint8_t    end_imu;    // endereco I2C do MPU (0x68 ou 0x69)
    bool       tem_mag;    // AK8963 respondeu
    bool       tem_baro;   // BMP280 respondeu
    uint8_t    end_baro;   // 0x76 ou 0x77
} gy91_info_t;

typedef struct {
    float ax, ay, az;   // aceleracao (g)
    float gx, gy, gz;   // velocidade angular (graus/s), ja sem o offset da calibracao
    float temp_c;       // temperatura interna do MPU
} gy91_imu_dados_t;

typedef struct {
    float mx, my, mz;   // campo magnetico (uT), nos mesmos eixos do acelerometro
} gy91_mag_dados_t;

typedef struct {
    float pressao_pa;
    float temp_c;
} gy91_baro_dados_t;

// Inicia o barramento e os chips encontrados. Retorna erro se o MPU nao
// responder; o magnetometro e o BMP280 sao opcionais (veja `info`).
esp_err_t gy91_init(gy91_info_t *info);

// Mede o offset do giroscopio. O sensor deve estar PARADO.
// Leva `amostras` x 10 ms (1 amostra nova do MPU por tick).
esp_err_t gy91_calibrar_giro(int amostras);

esp_err_t gy91_ler_imu(gy91_imu_dados_t *d);

// ESP_ERR_NOT_FOUND: modulo sem magnetometro
// ESP_ERR_NOT_FINISHED: ainda nao ha medicao nova (o AK8963 mede a 100 Hz)
// ESP_ERR_INVALID_RESPONSE: campo forte demais (saturou)
esp_err_t gy91_ler_mag(gy91_mag_dados_t *m);

// ESP_ERR_NOT_FOUND: BMP280 nao encontrado
esp_err_t gy91_ler_baro(gy91_baro_dados_t *b);

// Altitude (m) pela formula barometrica padrao, em relacao a pressao de
// referencia p0 (101325 Pa = nivel do mar padrao)
float gy91_altitude(float pressao_pa, float p0_pa);
