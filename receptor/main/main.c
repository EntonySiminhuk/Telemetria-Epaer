#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled.h"
#include "lora.h"

static const char *TAG = "RX";

// Imprime um resumo da recepcao a cada N pacotes validos (120 x 500 ms = 1 min)
#define RESUMO_A_CADA  120

// Tempo maximo de cada espera do lora_receive (so define a frequencia do laco)
#define RX_TIMEOUT_MS  1000

// 1 = alem da linha TLM, imprime cada pacote em formato legivel (Lat: ... | Roll: ...)
#define LOG_LEGIVEL    0

// Heltec WiFi LoRa 32 V3: Vext (GPIO36) alimenta o pino 3V3 "Vext" e o OLED.
// Nivel LOW = ligado.
#define VEXT_PIN    GPIO_NUM_36

// ---------------------------------------------------------------------------
// FORMATO DO PACOTE (texto ASCII, uma linha, sem '\n'). TEM que ser igual ao
// do transmissor (pasta transmissor/). Os blocos de cada sensor sao separados
// por " - " (espaco, hifen, espaco):
//
//   CONTADOR,DATA,LAT,LON,ALT,SATS - ROLL,PITCH,RUMO,ALT_BAR
//   42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45
//
// Numeros negativos tem o '-' colado (",-26.2"): por isso a divisao em blocos
// usa " - " com os espacos, nunca so o '-'.
//
// SAIDA PARA O DASHBOARD (serial, 115200): uma linha por pacote valido,
// com a qualidade do sinal medida aqui como terceiro bloco:
//
//   TLM:<linha recebida> - RSSI,SNR
//   TLM:42,12/11/2026 20:55:58,-26.229174,-52.672226,727.9,6 - 1.4,6.6,285.0,0.45 - -48,12.5
// ---------------------------------------------------------------------------
#define PACOTE_SEP        " - "
#define PACOTE_DATA_ZERO  "00/00/0000 00:00:00"   // GPS ainda sem hora
#define PACOTE_MAX        128
#define SAIDA_PREFIXO     "TLM:"

typedef struct {
    unsigned contador;
    char     data[20];  // "DD/MM/AAAA HH:MM:SS", horario de Brasilia (zerada: GPS sem hora)
    double   lat, lon;  // graus (0 = GPS sem posicao)
    float    alt;       // m, GPS (0 = sem posicao)
    int      sats;
    float    roll;      // graus
    float    pitch;     // graus
    float    rumo;      // graus, magnetico (0 = sem magnetometro ou GY-91 sem dados)
    float    alt_bar;   // m, barometrica relativa ao ponto onde o transmissor ligou
} telemetria_t;

// Estatisticas de recepcao
typedef struct {
    unsigned recebidos;     // pacotes validos
    unsigned perdidos;      // saltos no contador
    unsigned corrompidos;   // erro de CRC ou de cabecalho
    int      rssi_min;
    int      rssi_max;
    long     rssi_soma;
    float    snr_soma;
} estat_t;

static estat_t total;   // desde o boot (perdidos aparece no display e no log)
static estat_t bloco;   // desde o ultimo resumo

static bool oled_ok;

// ---------------------------------------------------------------------------
// Interpretacao da linha recebida
// ---------------------------------------------------------------------------

// Le `n` numeros separados por ',' ocupando toda a string `s`
static bool le_numeros(const char *s, double *v, int n)
{
    for (int i = 0; i < n; i++) {
        char *fim;
        v[i] = strtod(s, &fim);
        if (fim == s) {
            return false;
        }
        const char esperado = (i < n - 1) ? ',' : '\0';
        if (*fim != esperado) {
            return false;
        }
        s = fim + 1;
    }
    return true;
}

// Confere "DD/MM/AAAA HH:MM:SS"
static bool data_valida(const char *d)
{
    static const char modelo[] = "99/99/9999 99:99:99";
    for (int i = 0; modelo[i]; i++) {
        const bool ok = (modelo[i] == '9') ? (d[i] >= '0' && d[i] <= '9') : (d[i] == modelo[i]);
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Interpreta a linha (modifica `linha`). Retorna false se o formato nao bate.
static bool telemetria_parse(char *linha, telemetria_t *t)
{
    // Exatamente dois blocos: GPS e GY-91
    char *sep = strstr(linha, PACOTE_SEP);
    if (!sep || strstr(sep + strlen(PACOTE_SEP), PACOTE_SEP)) {
        return false;
    }
    *sep = '\0';
    const char *gps = linha;
    const char *imu = sep + strlen(PACOTE_SEP);

    // GPS: CONTADOR,DATA,LAT,LON,ALT,SATS
    char *fim;
    const unsigned long cont = strtoul(gps, &fim, 10);
    if (fim == gps || *fim != ',' || cont > 65535) {
        return false;
    }
    const char *data = fim + 1;
    const char *virgula = strchr(data, ',');
    if (!virgula || virgula - data != (int)sizeof(t->data) - 1 || !data_valida(data)) {
        return false;
    }
    double g[4];
    if (!le_numeros(virgula + 1, g, 4)) {
        return false;
    }

    // GY-91: ROLL,PITCH,RUMO,ALT_BAR
    double m[4];
    if (!le_numeros(imu, m, 4)) {
        return false;
    }

    t->contador = (unsigned)cont;
    memcpy(t->data, data, sizeof(t->data) - 1);
    t->data[sizeof(t->data) - 1] = '\0';
    t->lat = g[0];
    t->lon = g[1];
    t->alt = (float)g[2];
    t->sats = (int)g[3];
    t->roll = (float)m[0];
    t->pitch = (float)m[1];
    t->rumo = (float)m[2];
    t->alt_bar = (float)m[3];
    return true;
}

// Aceita so texto ASCII imprimivel (descarta pacotes binarios de outros aparelhos)
static bool texto_imprimivel(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (b[i] < 0x20 || b[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Estatisticas
// ---------------------------------------------------------------------------

static void estat_add_pacote(estat_t *e, int rssi, float snr)
{
    if (e->recebidos == 0 || rssi < e->rssi_min) {
        e->rssi_min = rssi;
    }
    if (e->recebidos == 0 || rssi > e->rssi_max) {
        e->rssi_max = rssi;
    }
    e->rssi_soma += rssi;
    e->snr_soma += snr;
    e->recebidos++;
}

static void resumo_print(const estat_t *e)
{
    unsigned esperados = e->recebidos + e->perdidos;
    float perda = esperados ? 100.0f * e->perdidos / esperados : 0.0f;

    ESP_LOGI(TAG, "===== RESUMO: recebidos %u | perdidos %u (%.1f%%) | corrompidos %u | "
                  "RSSI min %d / med %.0f / max %d dBm | SNR med %.1f dB =====",
             e->recebidos, e->perdidos, perda, e->corrompidos,
             e->rssi_min, (float)e->rssi_soma / e->recebidos, e->rssi_max,
             e->snr_soma / e->recebidos);
}

// Compara o contador com o do pacote anterior e devolve quantos se perderam.
// O contador e uint16: a diferenca em aritmetica de 16 bits ja trata a volta
// de 65535 para 0. Um salto "para tras" (diferenca >= 32768) indica que o
// transmissor reiniciou: nao conta como perda.
static unsigned conta_perdidos(uint16_t contador)
{
    static bool primeiro = true;
    static uint16_t anterior;

    unsigned perdidos = 0;
    if (!primeiro) {
        uint16_t salto = (uint16_t)(contador - anterior - 1);
        if (salto < 0x8000) {
            perdidos = salto;
        } else {
            ESP_LOGW(TAG, "Contador voltou de #%u para #%u: transmissor reiniciado?",
                     anterior, contador);
        }
    }
    primeiro = false;
    anterior = contador;
    return perdidos;
}

// ---------------------------------------------------------------------------
// FORMATO DE SAIDA NO DISPLAY (21 colunas x 8 linhas) e log legivel
// ---------------------------------------------------------------------------
static void telemetria_print(const telemetria_t *t, int rssi, float snr)
{
    const bool com_posicao = (t->lat != 0.0 || t->lon != 0.0);
    const bool com_hora = strcmp(t->data, PACOTE_DATA_ZERO) != 0;

    if (oled_ok) {
        oled_clear();
        oled_printf(0, 0, "RX #%u  Perd: %u", t->contador, total.perdidos);
        oled_hline(1);
        if (com_posicao) {
            oled_printf(0, 2, "Lat: %.6f", t->lat);
            oled_printf(0, 3, "Lon: %.6f", t->lon);
            // 21 colunas no pior caso: "Alt:1234 Bar:+1234.5"
            oled_printf(0, 4, "Alt:%.0f Bar:%+.1f", t->alt, t->alt_bar);
        } else {
            oled_text(0, 3, "TX sem posicao");
            oled_printf(0, 4, "Alt bar: %+.2f m", t->alt_bar);
        }
        if (com_hora) {
            // "12/11/2026 20:55:58" -> "12/11 20:55:58"
            oled_printf(0, 5, "%.5s %.8s Sat:%d", t->data, t->data + 11, t->sats);
        } else {
            oled_printf(0, 5, "Sem hora       Sat:%d", t->sats);
        }
        // 21 colunas no pior caso: "R:-180 P:-90 Rumo:359"
        oled_printf(0, 6, "R:%+.0f P:%+.0f Rumo:%.0f", t->roll, t->pitch, t->rumo);
        oled_printf(0, 7, "RSSI:%d SNR:%.1f", rssi, snr);
        oled_update();
    }

#if LOG_LEGIVEL
    ESP_LOGI(TAG, "#%u | %s | %s | Sats: %d | Roll: %+.1f | Pitch: %+.1f | Rumo: %.1f | "
                  "Alt bar: %+.2f m | RSSI: %d dBm | SNR: %.1f dB | Perdidos: %u",
             t->contador, com_hora ? t->data : "sem hora",
             com_posicao ? "com posicao" : "TX sem posicao", t->sats,
             t->roll, t->pitch, t->rumo, t->alt_bar, rssi, snr, total.perdidos);
#endif
}

// ---------------------------------------------------------------------------

static void vext_on(void)
{
    gpio_reset_pin(VEXT_PIN);
    gpio_set_direction(VEXT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VEXT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

// RECEPTOR: fica em recepcao continua e processa cada pacote que chega
static void lora_rx_task(void *arg)
{
    // static: fora da pilha da task
    static uint8_t buf[PACOTE_MAX + 1];
    static char linha[PACOTE_MAX + 1];

    esp_err_t err = lora_start_rx();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar a recepcao: %s", esp_err_to_name(err));
    }

    while (1) {
        int rssi;
        float snr;
        int n = lora_receive(buf, PACOTE_MAX, &rssi, &snr, RX_TIMEOUT_MS);

        if (n == 0) {
            continue;  // nada chegou ainda
        }
        if (n == -1) {
            total.corrompidos++;
            bloco.corrompidos++;
            ESP_LOGW(TAG, "Pacote corrompido (CRC/cabecalho) | total: %u", total.corrompidos);
            continue;
        }
        if (n < 0) {
            // Falha de SPI: espera e reconfigura a recepcao
            ESP_LOGE(TAG, "Falha de comunicacao com o radio, reiniciando a recepcao");
            vTaskDelay(pdMS_TO_TICKS(1000));
            lora_start_rx();
            continue;
        }
        if (n > PACOTE_MAX || !texto_imprimivel(buf, n)) {
            ESP_LOGW(TAG, "Pacote descartado: %d bytes que nao sao uma linha de telemetria | "
                          "RSSI: %d dBm", n, rssi);
            continue;
        }
        buf[n] = '\0';

        // O parser modifica a string: trabalha numa copia e guarda a original
        telemetria_t t;
        memcpy(linha, buf, (size_t)n + 1);
        if (!telemetria_parse(linha, &t)) {
            ESP_LOGW(TAG, "Pacote descartado: formato invalido \"%s\" | RSSI: %d dBm",
                     (const char *)buf, rssi);
            continue;
        }

        unsigned perdidos = conta_perdidos((uint16_t)t.contador);
        total.perdidos += perdidos;
        bloco.perdidos += perdidos;
        estat_add_pacote(&total, rssi, snr);
        estat_add_pacote(&bloco, rssi, snr);

        // Linha para o dashboard: o pacote exatamente como chegou + qualidade do sinal
        printf(SAIDA_PREFIXO "%s" PACOTE_SEP "%d,%.1f\n", (const char *)buf, rssi, snr);

        telemetria_print(&t, rssi, snr);

        if (bloco.recebidos >= RESUMO_A_CADA) {
            resumo_print(&bloco);
            memset(&bloco, 0, sizeof(bloco));
        }
    }
}

void app_main(void)
{
    vext_on();

    esp_err_t err = oled_init();
    oled_ok = (err == ESP_OK);
    if (oled_ok) {
        oled_text(0, 0, "Receptor LoRa");
        oled_text(0, 3, "Iniciando...");
        oled_update();
    } else {
        ESP_LOGE(TAG, "Falha ao iniciar o display: %s", esp_err_to_name(err));
    }

    err = lora_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o radio: %s", esp_err_to_name(err));
        if (oled_ok) {
            oled_clear();
            oled_text(0, 0, "Receptor LoRa");
            oled_hline(1);
            oled_text(0, 3, "Falha no radio:");
            oled_text(0, 4, esp_err_to_name(err));
            oled_update();
        }
        return;
    }

    ESP_LOGI(TAG, "Receptor pronto: %.1f MHz, SF%d. Aguardando pacotes...",
             LORA_FREQ_HZ / 1e6, LORA_SF);
    if (oled_ok) {
        oled_clear();
        oled_text(0, 0, "Receptor LoRa");
        oled_hline(1);
        oled_printf(0, 3, "%.1f MHz  SF%d", LORA_FREQ_HZ / 1e6, LORA_SF);
        oled_text(0, 5, "Aguardando pacotes...");
        oled_update();
    }

    xTaskCreate(lora_rx_task, "lora_rx_task", 6144, NULL, 5, NULL);  // log com float
}
