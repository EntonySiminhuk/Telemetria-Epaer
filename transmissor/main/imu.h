#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "gy91.h"

// Atitude e altitude barometrica a partir do GY-91.
// Uma task le o sensor a 100 Hz e publica o resultado. As outras tasks
// (LoRa, display) so leem a copia publicada com imu_obter(), sem acessar o
// barramento I2C, entao nao ha disputa pelo sensor.

#define IMU_PERIODO_MS      10      // 100 Hz (CONFIG_FREERTOS_HZ = 100: 1 tick)
#define IMU_ALPHA           0.98f   // filtro complementar: peso do giroscopio
#define IMU_AMOSTRAS_CALIB  100     // 1 s de calibracao do giroscopio (sensor parado)
#define IMU_MAX_IDADE_MS    200     // leitura mais antiga que isso e considerada invalida

// Offset do magnetometro (hard iron, uT). Medir girando o modulo em todas as
// direcoes e usar o centro (max + min) / 2 de cada eixo. Sem isso o rumo
// pode errar dezenas de graus.
#define IMU_MAG_OFFSET_X    0.0f
#define IMU_MAG_OFFSET_Y    0.0f
#define IMU_MAG_OFFSET_Z    0.0f

typedef struct {
    // Atitude (graus). Eixos do modulo: X para frente, Y para a esquerda, Z para cima.
    float roll;          // rotacao em torno de X (positivo: lado direito desce)
    float pitch;         // rotacao em torno de Y (positivo: frente desce)
    float rumo;          // rumo magnetico 0..360 (0 = norte magnetico), se tem_rumo
    bool  tem_rumo;

    float ax, ay, az;    // aceleracao (g)
    float gx, gy, gz;    // velocidade angular (graus/s)
    float mx, my, mz;    // campo magnetico (uT), ja sem o offset

    bool  tem_baro;
    float pressao_pa;
    float temp_c;        // do BMP280 (ou do MPU, se nao houver BMP280)
    float alt_baro_m;    // altitude padrao (p0 = 1013,25 hPa): depende do clima
    float alt_rel_m;     // altitude em relacao ao ponto onde a placa ligou
    bool  tem_alt_rel;   // alt_rel_m valida (BMP280 ok e referencia medida no boot)

    TickType_t tick;     // instante da ultima leitura boa do MPU
} imu_estado_t;

// Inicia o GY-91, calibra (deixar PARADO por ~2 s) e cria a imu_task.
esp_err_t imu_iniciar(void);

// Copia o ultimo estado. Retorna false se o sensor nao esta funcionando
// ou a leitura e mais antiga que IMU_MAX_IDADE_MS.
bool imu_obter(imu_estado_t *e);

// Chips encontrados no modulo (valido depois do imu_iniciar)
const gy91_info_t *imu_info(void);
