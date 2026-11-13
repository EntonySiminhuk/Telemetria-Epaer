#include <math.h>
#include <string.h>
#include "gy91.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GY91";

// I2C proprio do GY-91 (o OLED usa o I2C_NUM_0 nos GPIO17/18)
#define GY91_I2C_PORT   I2C_NUM_1
#define GY91_SDA        41
#define GY91_SCL        42
#define GY91_I2C_HZ     400000
#define I2C_TIMEOUT_MS  20

// ---------------------------------------------------------------------------
// Registradores
// ---------------------------------------------------------------------------

// MPU9250 / MPU6500
#define MPU_SMPLRT_DIV      0x19
#define MPU_CONFIG          0x1A
#define MPU_GYRO_CONFIG     0x1B
#define MPU_ACCEL_CONFIG    0x1C
#define MPU_ACCEL_CONFIG2   0x1D
#define MPU_INT_PIN_CFG     0x37
#define MPU_ACCEL_XOUT_H    0x3B   // 14 bytes: acel XYZ, temperatura, giro XYZ
#define MPU_USER_CTRL       0x6A
#define MPU_PWR_MGMT_1      0x6B
#define MPU_PWR_MGMT_2      0x6C
#define MPU_WHO_AM_I        0x75

// AK8963 (magnetometro dentro do MPU9250, visivel no I2C com o bypass ligado)
#define AK_ADDR             0x0C
#define AK_WIA              0x00   // deve ler 0x48
#define AK_ST1              0x02   // bit 0: dado novo
#define AK_CNTL1            0x0A
#define AK_ASAX             0x10   // ajuste de sensibilidade de fabrica (3 bytes)
#define AK_MODO_DESLIGADO   0x00
#define AK_MODO_FUSE_ROM    0x0F
#define AK_MODO_CONT_100HZ  0x16   // 16 bits, continuo 2 (100 Hz)

// BMP280
#define BMP_CALIB           0x88   // 24 bytes de calibracao
#define BMP_ID              0xD0   // 0x58 (BMP280) ou 0x60 (BME280)
#define BMP_RESET           0xE0
#define BMP_STATUS          0xF3
#define BMP_CTRL_MEAS       0xF4
#define BMP_CONFIG          0xF5
#define BMP_PRESS_MSB       0xF7   // 6 bytes: pressao (20 bits) e temperatura (20 bits)

// ---------------------------------------------------------------------------

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev_imu, dev_mag, dev_baro;
static gy91_info_t info_atual;

static float acel_lsb_por_g;
static float giro_lsb_por_dps;
static float giro_off[3];
static float mag_ajuste[3];   // sensibilidade de fabrica do AK8963 (ASA)

// Calibracao do BMP280 (datasheet, secao 3.11.2)
static struct {
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
} bmp_cal;

#define CHECK(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) return e_; } while (0)

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *dst, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, dst, len, I2C_TIMEOUT_MS);
}

static esp_err_t add_device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = GY91_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, dev);
}

// Procura o chip em um dos dois enderecos possiveis
static bool probe(uint8_t a, uint8_t b, uint8_t *achado)
{
    if (i2c_master_probe(bus, a, I2C_TIMEOUT_MS) == ESP_OK) {
        *achado = a;
        return true;
    }
    if (b && i2c_master_probe(bus, b, I2C_TIMEOUT_MS) == ESP_OK) {
        *achado = b;
        return true;
    }
    return false;
}

static inline int16_t be16(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}

static inline int16_t le16(const uint8_t *p)
{
    return (int16_t)((p[1] << 8) | p[0]);
}

// ---------------------------------------------------------------------------
// MPU9250 / MPU6500
// ---------------------------------------------------------------------------

static esp_err_t mpu_init(void)
{
    uint8_t id;
    CHECK(reg_read(dev_imu, MPU_WHO_AM_I, &id, 1));
    info_atual.who_am_i = id;
    switch (id) {
    case 0x71: info_atual.imu = GY91_IMU_MPU9250; break;
    case 0x73: info_atual.imu = GY91_IMU_MPU9255; break;
    case 0x70: info_atual.imu = GY91_IMU_MPU6500; break;
    default:
        info_atual.imu = GY91_IMU_DESCONHECIDO;
        ESP_LOGW(TAG, "WHO_AM_I inesperado: 0x%02X (tentando mesmo assim)", id);
        break;
    }

    CHECK(reg_write(dev_imu, MPU_PWR_MGMT_1, 0x80));   // reset
    vTaskDelay(pdMS_TO_TICKS(100));
    CHECK(reg_write(dev_imu, MPU_PWR_MGMT_1, 0x01));   // relogio do PLL do giroscopio
    CHECK(reg_write(dev_imu, MPU_PWR_MGMT_2, 0x00));   // todos os eixos ligados
    vTaskDelay(pdMS_TO_TICKS(50));

    // Filtro passa-baixa de 41 Hz (giro) e 44,8 Hz (acel), amostragem de 100 Hz
    CHECK(reg_write(dev_imu, MPU_CONFIG, 0x03));
    CHECK(reg_write(dev_imu, MPU_SMPLRT_DIV, 9));      // 1 kHz / (1 + 9)
    CHECK(reg_write(dev_imu, MPU_ACCEL_CONFIG2, 0x03));

    CHECK(reg_write(dev_imu, MPU_GYRO_CONFIG, GY91_GIRO_FS << 3));
    CHECK(reg_write(dev_imu, MPU_ACCEL_CONFIG, GY91_ACEL_FS << 3));
    acel_lsb_por_g = 16384.0f / (float)(1 << GY91_ACEL_FS);
    giro_lsb_por_dps = 131.0f / (float)(1 << GY91_GIRO_FS);

    // Bypass: liga o I2C interno do MPU direto no barramento externo, para o
    // ESP32 falar com o AK8963 no endereco 0x0C
    CHECK(reg_write(dev_imu, MPU_USER_CTRL, 0x00));
    return reg_write(dev_imu, MPU_INT_PIN_CFG, 0x02);
}

static esp_err_t mpu_ler_bruto(int16_t v[7])
{
    uint8_t d[14];
    CHECK(reg_read(dev_imu, MPU_ACCEL_XOUT_H, d, sizeof(d)));
    for (int i = 0; i < 7; i++) {
        v[i] = be16(&d[2 * i]);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// AK8963
// ---------------------------------------------------------------------------

static esp_err_t mag_init(void)
{
    uint8_t wia;
    CHECK(reg_read(dev_mag, AK_WIA, &wia, 1));
    if (wia != 0x48) {
        ESP_LOGW(TAG, "AK8963 com WIA inesperado: 0x%02X", wia);
        return ESP_ERR_NOT_FOUND;
    }

    // Le os ajustes de sensibilidade gravados de fabrica
    CHECK(reg_write(dev_mag, AK_CNTL1, AK_MODO_DESLIGADO));
    vTaskDelay(pdMS_TO_TICKS(10));
    CHECK(reg_write(dev_mag, AK_CNTL1, AK_MODO_FUSE_ROM));
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t asa[3];
    CHECK(reg_read(dev_mag, AK_ASAX, asa, sizeof(asa)));
    for (int i = 0; i < 3; i++) {
        mag_ajuste[i] = ((float)asa[i] - 128.0f) / 256.0f + 1.0f;
    }
    CHECK(reg_write(dev_mag, AK_CNTL1, AK_MODO_DESLIGADO));
    vTaskDelay(pdMS_TO_TICKS(10));
    return reg_write(dev_mag, AK_CNTL1, AK_MODO_CONT_100HZ);
}

// ---------------------------------------------------------------------------
// BMP280
// ---------------------------------------------------------------------------

static esp_err_t baro_init(void)
{
    uint8_t id;
    CHECK(reg_read(dev_baro, BMP_ID, &id, 1));
    if (id != 0x58 && id != 0x60) {
        ESP_LOGW(TAG, "BMP280 com ID inesperado: 0x%02X", id);
        return ESP_ERR_NOT_FOUND;
    }

    CHECK(reg_write(dev_baro, BMP_RESET, 0xB6));
    vTaskDelay(pdMS_TO_TICKS(20));   // 2 ticks: garante pelo menos 10 ms (precisa de 2 ms)
    for (int i = 0; i < 10; i++) {                     // espera copiar a calibracao
        uint8_t st;
        CHECK(reg_read(dev_baro, BMP_STATUS, &st, 1));
        if (!(st & 0x01)) {
            break;
        }
        vTaskDelay(1);
    }

    uint8_t c[24];
    CHECK(reg_read(dev_baro, BMP_CALIB, c, sizeof(c)));
    bmp_cal.T1 = (uint16_t)le16(&c[0]);
    bmp_cal.T2 = le16(&c[2]);
    bmp_cal.T3 = le16(&c[4]);
    bmp_cal.P1 = (uint16_t)le16(&c[6]);
    bmp_cal.P2 = le16(&c[8]);
    bmp_cal.P3 = le16(&c[10]);
    bmp_cal.P4 = le16(&c[12]);
    bmp_cal.P5 = le16(&c[14]);
    bmp_cal.P6 = le16(&c[16]);
    bmp_cal.P7 = le16(&c[18]);
    bmp_cal.P8 = le16(&c[20]);
    bmp_cal.P9 = le16(&c[22]);
    if (bmp_cal.P1 == 0) {
        return ESP_ERR_INVALID_RESPONSE;               // calibracao invalida (evita divisao por 0)
    }

    // A config so e aceita em modo sleep (estado apos o reset), por isso vem antes.
    // Standby 0,5 ms + filtro IIR x16: ~23 medicoes/s, ruido de ~0,1 m
    CHECK(reg_write(dev_baro, BMP_CONFIG, (0x0 << 5) | (0x4 << 2)));
    // Temperatura x2, pressao x16, modo normal (mede continuamente)
    return reg_write(dev_baro, BMP_CTRL_MEAS, (0x2 << 5) | (0x5 << 2) | 0x3);
}

// Formulas de compensacao do datasheet (inteiros). Retorna centesimos de grau.
static int32_t bmp_temp(int32_t adc_t, int32_t *t_fine)
{
    const int32_t t1 = bmp_cal.T1;
    int32_t var1 = ((((adc_t >> 3) - (t1 * 2))) * bmp_cal.T2) >> 11;
    int32_t var2 = (((((adc_t >> 4) - t1) * ((adc_t >> 4) - t1)) >> 12) * bmp_cal.T3) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}

// Retorna a pressao em Pa no formato Q24.8 (dividir por 256)
static uint32_t bmp_press(int32_t adc_p, int32_t t_fine)
{
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * bmp_cal.P6;
    var2 = var2 + var1 * bmp_cal.P5 * 131072;               // << 17
    var2 = var2 + (int64_t)bmp_cal.P4 * 34359738368LL;      // << 35
    var1 = ((var1 * var1 * bmp_cal.P3) >> 8) + var1 * bmp_cal.P2 * 4096;  // << 12
    var1 = ((140737488355328LL + var1) * bmp_cal.P1) >> 33; // (1 << 47)
    if (var1 == 0) {
        return 0;
    }
    int64_t p = 1048576 - adc_p;
    p = ((p * 2147483648LL - var2) * 3125) / var1;          // << 31
    var1 = ((int64_t)bmp_cal.P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)bmp_cal.P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (int64_t)bmp_cal.P7 * 16;  // << 4
    return (uint32_t)p;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

esp_err_t gy91_init(gy91_info_t *info)
{
    memset(&info_atual, 0, sizeof(info_atual));

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = GY91_I2C_PORT,
        .sda_io_num = GY91_SDA,
        .scl_io_num = GY91_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // o modulo ja tem pull-up; este so ajuda
    };
    CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // MPU: 0x68 (SAO em nivel baixo, padrao do GY-91) ou 0x69
    if (!probe(0x68, 0x69, &info_atual.end_imu)) {
        ESP_LOGE(TAG, "MPU nao encontrado em 0x68/0x69 (confira SDA=GPIO%d, SCL=GPIO%d e 3V3)",
                 GY91_SDA, GY91_SCL);
        return ESP_ERR_NOT_FOUND;
    }
    CHECK(add_device(info_atual.end_imu, &dev_imu));
    CHECK(mpu_init());

    // Magnetometro: so aparece depois do bypass ligado no mpu_init()
    if (info_atual.imu != GY91_IMU_MPU6500 &&
        i2c_master_probe(bus, AK_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
        CHECK(add_device(AK_ADDR, &dev_mag));
        info_atual.tem_mag = (mag_init() == ESP_OK);
    }

    // BMP280: 0x76 (SDO em nivel baixo) ou 0x77
    if (probe(0x76, 0x77, &info_atual.end_baro)) {
        CHECK(add_device(info_atual.end_baro, &dev_baro));
        info_atual.tem_baro = (baro_init() == ESP_OK);
    }

    if (info) {
        *info = info_atual;
    }
    return ESP_OK;
}

esp_err_t gy91_calibrar_giro(int amostras)
{
    if (amostras <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    float soma[3] = {0};
    int ok = 0;
    for (int i = 0; i < amostras; i++) {
        int16_t v[7];
        if (mpu_ler_bruto(v) == ESP_OK) {
            for (int k = 0; k < 3; k++) {
                soma[k] += v[4 + k] / giro_lsb_por_dps;
            }
            ok++;
        }
        vTaskDelay(1);   // 1 tick = 10 ms: o MPU gera uma amostra nova a cada 10 ms
    }
    if (ok < amostras / 2) {
        return ESP_FAIL;
    }
    for (int k = 0; k < 3; k++) {
        giro_off[k] = soma[k] / ok;
    }
    ESP_LOGI(TAG, "Offset do giroscopio: X=%.2f Y=%.2f Z=%.2f graus/s",
             giro_off[0], giro_off[1], giro_off[2]);
    return ESP_OK;
}

esp_err_t gy91_ler_imu(gy91_imu_dados_t *d)
{
    int16_t v[7];
    CHECK(mpu_ler_bruto(v));
    d->ax = v[0] / acel_lsb_por_g;
    d->ay = v[1] / acel_lsb_por_g;
    d->az = v[2] / acel_lsb_por_g;
    d->temp_c = v[3] / 333.87f + 21.0f;
    d->gx = v[4] / giro_lsb_por_dps - giro_off[0];
    d->gy = v[5] / giro_lsb_por_dps - giro_off[1];
    d->gz = v[6] / giro_lsb_por_dps - giro_off[2];
    return ESP_OK;
}

esp_err_t gy91_ler_mag(gy91_mag_dados_t *m)
{
    if (!info_atual.tem_mag) {
        return ESP_ERR_NOT_FOUND;
    }
    // ST1, HXL..HZH, ST2: ler ate o ST2 libera a proxima medicao
    uint8_t d[8];
    CHECK(reg_read(dev_mag, AK_ST1, d, sizeof(d)));
    if (!(d[0] & 0x01)) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (d[7] & 0x08) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const float uT_por_lsb = 0.15f;   // modo 16 bits
    const float hx = le16(&d[1]) * mag_ajuste[0] * uT_por_lsb;
    const float hy = le16(&d[3]) * mag_ajuste[1] * uT_por_lsb;
    const float hz = le16(&d[5]) * mag_ajuste[2] * uT_por_lsb;
    // Os eixos do AK8963 sao diferentes dos do acelerometro:
    // X_mag = Y_acel, Y_mag = X_acel, Z_mag = -Z_acel
    m->mx = hy;
    m->my = hx;
    m->mz = -hz;
    return ESP_OK;
}

esp_err_t gy91_ler_baro(gy91_baro_dados_t *b)
{
    if (!info_atual.tem_baro) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t d[6];
    CHECK(reg_read(dev_baro, BMP_PRESS_MSB, d, sizeof(d)));
    const int32_t adc_p = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
    const int32_t adc_t = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
    if (adc_p == 0x80000 || adc_t == 0x80000) {
        return ESP_ERR_NOT_FINISHED;                    // primeira medicao ainda nao saiu
    }
    int32_t t_fine;
    b->temp_c = bmp_temp(adc_t, &t_fine) / 100.0f;
    const uint32_t p = bmp_press(adc_p, t_fine);
    if (p == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    b->pressao_pa = p / 256.0f;
    return ESP_OK;
}

float gy91_altitude(float pressao_pa, float p0_pa)
{
    return 44330.0f * (1.0f - powf(pressao_pa / p0_pa, 0.190295f));
}
