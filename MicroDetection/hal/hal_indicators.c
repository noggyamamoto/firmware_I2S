/*
 * ============================================================================
 * hal/hal_indicators – implementação sobre drivers/drv_gpio e drivers/drv_pwm
 * ============================================================================
 */
#include "hal_indicators.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "drv_gpio.h"
#include "drv_pwm.h"
#include "hal_time.h"

static const char *TAG = "leds";

#define PWM_TIMER_RGB       0                       // PWM do LED RGB (8 bits, 5 kHz)
#define PWM_TIMER_BUZZER    1                       // Frequência do buzzer (10 bits)
#define PWM_CH_R            0
#define PWM_CH_G            1
#define PWM_CH_B            2
#define PWM_CH_BUZZER       3
#define BUZZER_DUTY         512                     // 50% em 10 bits

static volatile StatusPattern s_status = STATUS_OFF;
static HalTimer s_rgb_off_timer;
static HalTimer s_beep_off_timer;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
#if RGB_LED_COMMON_ANODE
    r = 255 - r; g = 255 - g; b = 255 - b;          // Anodo comum: lógica invertida
#endif
    drv_pwm_set_duty(PWM_CH_R, r);
    drv_pwm_set_duty(PWM_CH_G, g);
    drv_pwm_set_duty(PWM_CH_B, b);
}

static void rgb_off_cb(void *arg) {
    (void)arg;
    set_rgb(0, 0, 0);
}

static void beep_off_cb(void *arg) {
    (void)arg;
    drv_pwm_set_duty(PWM_CH_BUZZER, 0);
}

/* Anima o LED de status conforme o padrão atual (passo de 100 ms). */
static void status_task(void *arg) {
    (void)arg;
    uint32_t step = 0;
    while (1) {
        bool level = false;
        switch (s_status) {
            case STATUS_OFF:        level = false; break;
            case STATUS_CONNECTING: level = (step % 2) == 0; break;             // 5 Hz
            case STATUS_AP_MODE:    level = (step % 10) == 0 || (step % 10) == 2; break;
            case STATUS_WIFI_OK:    level = (step % 10) < 5; break;             // 1 Hz
            case STATUS_PAIRED:     level = true; break;
            case STATUS_ERROR:      level = (step % 4) < 2; break;
        }
        drv_gpio_write(STATUS_LED_PIN, level);
        step++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool hal_indicators_init(void) {
    bool ok = drv_gpio_output_init(STATUS_LED_PIN);

    ok &= drv_pwm_timer_init(PWM_TIMER_RGB, 5000, 8);
    ok &= drv_pwm_channel_init(PWM_CH_R, PWM_TIMER_RGB, RGB_LED_R_PIN);
    ok &= drv_pwm_channel_init(PWM_CH_G, PWM_TIMER_RGB, RGB_LED_G_PIN);
    ok &= drv_pwm_channel_init(PWM_CH_B, PWM_TIMER_RGB, RGB_LED_B_PIN);
    set_rgb(0, 0, 0);

    ok &= drv_pwm_timer_init(PWM_TIMER_BUZZER, 1000, 10);
    ok &= drv_pwm_channel_init(PWM_CH_BUZZER, PWM_TIMER_BUZZER, BUZZER_PIN);

    s_rgb_off_timer = hal_timer_create(rgb_off_cb, NULL, "rgb_off");
    s_beep_off_timer = hal_timer_create(beep_off_cb, NULL, "beep_off");
    ok &= s_rgb_off_timer != NULL && s_beep_off_timer != NULL;

    xTaskCreatePinnedToCore(status_task, "status_led", 2048, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "Indicadores prontos (status=%d, RGB=%d/%d/%d, buzzer=%d)", STATUS_LED_PIN,
             RGB_LED_R_PIN, RGB_LED_G_PIN, RGB_LED_B_PIN, BUZZER_PIN);
    return ok;
}

void hal_status_led_set(StatusPattern pattern) {
    s_status = pattern;
}

StatusPattern hal_status_led_get(void) {
    return s_status;
}

void hal_rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    hal_timer_stop(s_rgb_off_timer);
    set_rgb(r, g, b);
    hal_timer_start_once(s_rgb_off_timer, (uint64_t)ms * 1000);
}

void hal_buzzer_beep(uint32_t freq_hz, uint32_t ms) {
    hal_timer_stop(s_beep_off_timer);
    drv_pwm_set_freq(PWM_TIMER_BUZZER, freq_hz);
    drv_pwm_set_duty(PWM_CH_BUZZER, BUZZER_DUTY);
    hal_timer_start_once(s_beep_off_timer, (uint64_t)ms * 1000);
}
