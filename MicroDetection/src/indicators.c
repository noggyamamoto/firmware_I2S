/*
 * ============================================================================
 * Implementação dos indicadores (GPIO + LEDC + esp_timer)
 * ============================================================================
 */
#include "indicators.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "leds";

#define LEDC_MODE           LEDC_LOW_SPEED_MODE     // Único modo disponível no ESP32-S3
#define LEDC_RGB_TIMER      LEDC_TIMER_0            // PWM do LED RGB
#define LEDC_BUZZER_TIMER   LEDC_TIMER_1            // Frequência do buzzer
#define LEDC_CH_R           LEDC_CHANNEL_0
#define LEDC_CH_G           LEDC_CHANNEL_1
#define LEDC_CH_B           LEDC_CHANNEL_2
#define LEDC_CH_BUZZER      LEDC_CHANNEL_3
#define BUZZER_DUTY         512                     // 50% em 10 bits

static volatile StatusPattern s_status = STATUS_OFF;
static esp_timer_handle_t     s_rgb_off_timer;
static esp_timer_handle_t     s_beep_off_timer;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
#if RGB_LED_COMMON_ANODE
    r = 255 - r; g = 255 - g; b = 255 - b;          // Anodo comum: lógica invertida
#endif
    ledc_set_duty(LEDC_MODE, LEDC_CH_R, r);
    ledc_update_duty(LEDC_MODE, LEDC_CH_R);
    ledc_set_duty(LEDC_MODE, LEDC_CH_G, g);
    ledc_update_duty(LEDC_MODE, LEDC_CH_G);
    ledc_set_duty(LEDC_MODE, LEDC_CH_B, b);
    ledc_update_duty(LEDC_MODE, LEDC_CH_B);
}

static void rgb_off_cb(void *arg) {
    (void)arg;
    set_rgb(0, 0, 0);
}

static void beep_off_cb(void *arg) {
    (void)arg;
    ledc_set_duty(LEDC_MODE, LEDC_CH_BUZZER, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CH_BUZZER);
}

/* Anima o LED de status conforme o padrão atual (passo de 100 ms). */
static void status_task(void *arg) {
    (void)arg;
    uint32_t step = 0;
    while (1) {
        int level = 0;
        switch (s_status) {
            case STATUS_OFF:        level = 0; break;
            case STATUS_CONNECTING: level = (step % 2) == 0; break;             // 5 Hz
            case STATUS_AP_MODE:    level = (step % 10) == 0 || (step % 10) == 2; break;
            case STATUS_WIFI_OK:    level = (step % 10) < 5; break;             // 1 Hz
            case STATUS_PAIRED:     level = 1; break;
            case STATUS_ERROR:      level = (step % 4) < 2; break;
        }
        gpio_set_level(STATUS_LED_PIN, level);
        step++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void indicators_init(void) {
    // --- LED de status ---
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << STATUS_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(STATUS_LED_PIN, 0);

    // --- LED RGB (PWM 8 bits, 5 kHz) ---
    ledc_timer_config_t rgb_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_RGB_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&rgb_timer));
    const int pins[3] = {RGB_LED_R_PIN, RGB_LED_G_PIN, RGB_LED_B_PIN};
    const ledc_channel_t chans[3] = {LEDC_CH_R, LEDC_CH_G, LEDC_CH_B};
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t ch = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_MODE,
            .channel = chans[i],
            .timer_sel = LEDC_RGB_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
    set_rgb(0, 0, 0);

    // --- Buzzer passivo (PWM 10 bits, frequência variável) ---
    ledc_timer_config_t bz_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_BUZZER_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bz_timer));
    ledc_channel_config_t bz = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CH_BUZZER,
        .timer_sel = LEDC_BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bz));

    // --- Timers de desligamento ---
    const esp_timer_create_args_t rgb_args = {.callback = rgb_off_cb, .name = "rgb_off"};
    ESP_ERROR_CHECK(esp_timer_create(&rgb_args, &s_rgb_off_timer));
    const esp_timer_create_args_t beep_args = {.callback = beep_off_cb, .name = "beep_off"};
    ESP_ERROR_CHECK(esp_timer_create(&beep_args, &s_beep_off_timer));

    xTaskCreatePinnedToCore(status_task, "status_led", 2048, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "Indicadores prontos (status=%d, RGB=%d/%d/%d, buzzer=%d)", STATUS_LED_PIN,
             RGB_LED_R_PIN, RGB_LED_G_PIN, RGB_LED_B_PIN, BUZZER_PIN);
}

void indicators_set_status(StatusPattern pattern) {
    s_status = pattern;
}

StatusPattern indicators_status(void) {
    return s_status;
}

void indicators_flash_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    esp_timer_stop(s_rgb_off_timer);                    // Ignora erro se não estava ativo
    set_rgb(r, g, b);
    esp_timer_start_once(s_rgb_off_timer, (uint64_t)ms * 1000);
}

void indicators_beep(uint32_t freq_hz, uint32_t ms) {
    esp_timer_stop(s_beep_off_timer);
    ledc_set_freq(LEDC_MODE, LEDC_BUZZER_TIMER, freq_hz);
    ledc_set_duty(LEDC_MODE, LEDC_CH_BUZZER, BUZZER_DUTY);
    ledc_update_duty(LEDC_MODE, LEDC_CH_BUZZER);
    esp_timer_start_once(s_beep_off_timer, (uint64_t)ms * 1000);
}
