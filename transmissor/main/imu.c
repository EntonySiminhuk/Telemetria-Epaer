#include <math.h>
#include <string.h>
#include "imu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "IMU";

#define RAD2DEG     57.29578f
#define BARO_DIV    4       // le o BMP280 a cada 4 ciclos (25 Hz; ele mede a ~23 Hz)
#define P0_PADRAO   101325.0f

static gy91_info_t info;
static float p0_local;      // pressao no ponto de partida (referencia da alt_rel_m)

static imu_estado_t estado;
static bool estado_ok;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static const char *nome_imu(gy91_imu_t t)
{
    switch (t) {
    case GY91_IMU_MPU9250: return "MPU9250";
    case GY91_IMU_MPU9255: return "MPU9255";
    case GY91_IMU_MPU6500: return "MPU6500 (sem magnetometro)";
    default:               return "desconhecido";
    }
}

// Leva o angulo para -180..180
static float wrap180(float a)
{
    while (a > 180.0f) {
        a -= 360.0f;
    }
    while (a < -180.0f) {
        a += 360.0f;
    }
    return a;
}

// Rumo compensado pela inclinacao: projeta o campo no plano horizontal
static float calc_rumo(const imu_estado_t *e)
{
    const float r = e->roll / RAD2DEG;
    const float p = e->pitch / RAD2DEG;
    const float xh = e->mx * cosf(p) + (e->my * sinf(r) + e->mz * cosf(r)) * sinf(p);
    const float yh = e->my * cosf(r) - e->mz * sinf(r);
    float rumo = atan2f(yh, xh) * RAD2DEG;
    return (rumo < 0) ? rumo + 360.0f : rumo;
}

// Media de algumas leituras do BMP280 para a referencia de altitude
static float baro_referencia(void)
{
    float soma = 0;
    int n = 0;
    for (int i = 0; i < 40 && n < 20; i++) {
        gy91_baro_dados_t b;
        if (gy91_ler_baro(&b) == ESP_OK) {
            soma += b.pressao_pa;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return n ? soma / n : 0;
}

static void imu_task(void *arg)
{
    imu_estado_t e = {0};
    e.tem_baro = info.tem_baro;

    // Atitude inicial pelo acelerometro, para o filtro nao comecar em 0
    gy91_imu_dados_t d;
    if (gy91_ler_imu(&d) == ESP_OK) {
        e.roll = atan2f(d.ay, d.az) * RAD2DEG;
        e.pitch = atan2f(-d.ax, sqrtf(d.ay * d.ay + d.az * d.az)) * RAD2DEG;
    }

    int64_t t_ant = esp_timer_get_time();
    TickType_t proximo = xTaskGetTickCount();
    TickType_t ultimo_log = proximo;
    uint32_t ciclo = 0, erros = 0, leituras = 0;
    bool tem_leitura = false;

    while (1) {
        xTaskDelayUntil(&proximo, pdMS_TO_TICKS(IMU_PERIODO_MS));
        ciclo++;

        if (gy91_ler_imu(&d) == ESP_OK) {
            const int64_t t = esp_timer_get_time();
            float dt = (t - t_ant) / 1e6f;
            t_ant = t;
            if (dt > 0.1f) {
                dt = 0.1f;     // a task ficou parada: nao integra um salto grande
            }

            e.ax = d.ax; e.ay = d.ay; e.az = d.az;
            e.gx = d.gx; e.gy = d.gy; e.gz = d.gz;
            if (!e.tem_baro) {
                e.temp_c = d.temp_c;
            }

            // Filtro complementar: o giroscopio segue movimentos rapidos e o
            // acelerometro (gravidade) corrige o desvio lento. Sob aceleracao
            // forte e prolongada (motor, curva) o acelerometro engana: ALPHA
            // alto reduz esse efeito.
            const float roll_acc = atan2f(d.ay, d.az) * RAD2DEG;
            const float pitch_acc = atan2f(-d.ax, sqrtf(d.ay * d.ay + d.az * d.az)) * RAD2DEG;
            e.roll = wrap180(e.roll + d.gx * dt);
            e.pitch = wrap180(e.pitch + d.gy * dt);
            e.roll = wrap180(e.roll + (1.0f - IMU_ALPHA) * wrap180(roll_acc - e.roll));
            e.pitch = wrap180(e.pitch + (1.0f - IMU_ALPHA) * wrap180(pitch_acc - e.pitch));

            e.tick = xTaskGetTickCount();
            tem_leitura = true;
            leituras++;
        } else {
            erros++;
        }

        gy91_mag_dados_t m;
        if (gy91_ler_mag(&m) == ESP_OK) {
            e.mx = m.mx - IMU_MAG_OFFSET_X;
            e.my = m.my - IMU_MAG_OFFSET_Y;
            e.mz = m.mz - IMU_MAG_OFFSET_Z;
            e.rumo = calc_rumo(&e);
            e.tem_rumo = true;
        }

        if (e.tem_baro && ciclo % BARO_DIV == 0) {
            gy91_baro_dados_t b;
            if (gy91_ler_baro(&b) == ESP_OK) {
                e.pressao_pa = b.pressao_pa;
                e.temp_c = b.temp_c;
                e.alt_baro_m = gy91_altitude(b.pressao_pa, P0_PADRAO);
                e.alt_rel_m = (p0_local > 0) ? gy91_altitude(b.pressao_pa, p0_local) : 0;
                e.tem_alt_rel = (p0_local > 0);
            }
        }

        if (tem_leitura) {
            taskENTER_CRITICAL(&lock);
            estado = e;
            estado_ok = true;
            taskEXIT_CRITICAL(&lock);
        }

        // Terminal: 1 linha por segundo
        const TickType_t agora = xTaskGetTickCount();
        if (agora - ultimo_log >= pdMS_TO_TICKS(1000)) {
            ultimo_log = agora;
            char rumo[12] = "--";
            if (e.tem_rumo) {
                snprintf(rumo, sizeof(rumo), "%.0f", e.rumo);
            }
            if (e.tem_baro) {
                ESP_LOGI(TAG, "Roll: %+.1f | Pitch: %+.1f | Rumo: %s | Alt rel: %+.2f m | "
                              "%.2f hPa | %.1f C | %lu Hz%s",
                         e.roll, e.pitch, rumo, e.alt_rel_m, e.pressao_pa / 100.0f, e.temp_c,
                         (unsigned long)leituras, erros ? " | erros I2C!" : "");
            } else {
                ESP_LOGI(TAG, "Roll: %+.1f | Pitch: %+.1f | Rumo: %s | %lu Hz%s",
                         e.roll, e.pitch, rumo, (unsigned long)leituras,
                         erros ? " | erros I2C!" : "");
            }
            if (erros) {
                ESP_LOGW(TAG, "%lu leituras do MPU falharam no ultimo segundo", (unsigned long)erros);
            }
            leituras = 0;
            erros = 0;
        }
    }
}

esp_err_t imu_iniciar(void)
{
    esp_err_t err = gy91_init(&info);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "GY-91: %s (WHO_AM_I 0x%02X em 0x%02X) | magnetometro: %s | BMP280: %s",
             nome_imu(info.imu), info.who_am_i, info.end_imu,
             info.tem_mag ? "sim" : "nao",
             info.tem_baro ? "sim" : "nao");

    ESP_LOGI(TAG, "Calibrando: mantenha o modulo PARADO...");
    err = gy91_calibrar_giro(IMU_AMOSTRAS_CALIB);
    if (err != ESP_OK) {
        return err;
    }

    if (info.tem_baro) {
        p0_local = baro_referencia();
        if (p0_local > 0) {
            ESP_LOGI(TAG, "Pressao de referencia: %.2f hPa (alt padrao %.1f m)",
                     p0_local / 100.0f, gy91_altitude(p0_local, P0_PADRAO));
        } else {
            ESP_LOGW(TAG, "BMP280 nao forneceu leituras: altitude relativa desativada");
        }
    }

    // Prioridade acima da gps_task (5) e da lora_tx_task (4) para manter os
    // 100 Hz certinhos: cada ciclo dura menos de 1 ms, entao nao atrasa as
    // outras. Fixa no nucleo 1; as tasks do sistema ficam no nucleo 0.
    // Pilha de 6 KB: o log com float usa bastante.
    if (xTaskCreatePinnedToCore(imu_task, "imu_task", 6144, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool imu_obter(imu_estado_t *e)
{
    taskENTER_CRITICAL(&lock);
    *e = estado;
    const bool ok = estado_ok;
    taskEXIT_CRITICAL(&lock);
    return ok && (xTaskGetTickCount() - e->tick) <= pdMS_TO_TICKS(IMU_MAX_IDADE_MS);
}

const gy91_info_t *imu_info(void)
{
    return &info;
}
