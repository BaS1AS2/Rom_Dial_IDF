/*
 * 彩色环绕旋转灯环（8x WS2812）
 * 依赖：components/led_strip（ESP-IDF 官方），RMT 后端
 */

#include "led_ring.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_check.h"
#include "led_strip.h"
#include <math.h>

static const char *LED_RING_TAG = "LED_RING";

/* ---------- 句柄 ---------- */
static led_strip_handle_t s_led_strip = NULL;
static TimerHandle_t      s_timer     = NULL;

/* ---------- 控制参数 ---------- */
typedef struct {
    float  base_hue_deg;       // 当前基准色相
    float  speed_deg_per_sec;  // 旋转速度（度/秒）
    float  brightness;         // [0,1]
    float  saturation;         // [0,1]
    float  hue_span_deg;       // 相邻 LED 的色相间隔
    float  update_hz;          // 刷新频率
} led_ring_ctrl_t;

static led_ring_ctrl_t s_ctrl = {
    .base_hue_deg      = 0.0f,
    .speed_deg_per_sec = 120.0f,
    .brightness        = 0.2f,
    .saturation        = 1.0f,
    .hue_span_deg      = 360.0f / LED_RING_COUNT,
    .update_hz         = 50.0f,
};

/* 参数临界区保护（软件定时器回调在 Timer Task 上下文运行） */
static portMUX_TYPE s_param_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ---------- HSV→RGB ---------- */
static inline void hsv_to_rgb(float H, float S, float V, uint8_t *r, uint8_t *g, uint8_t *b)
{
    while (H < 0)   H += 360.0f;
    while (H >= 360.0f) H -= 360.0f;

    float C = V * S;
    float X = C * (1 - fabsf(fmodf(H / 60.0f, 2.0f) - 1));
    float m = V - C;

    float r1=0, g1=0, b1=0;
    if (H < 60)       { r1 = C; g1 = X; b1 = 0; }
    else if (H < 120) { r1 = X; g1 = C; b1 = 0; }
    else if (H < 180) { r1 = 0; g1 = C; b1 = X; }
    else if (H < 240) { r1 = 0; g1 = X; b1 = C; }
    else if (H < 300) { r1 = X; g1 = 0; b1 = C; }
    else              { r1 = C; g1 = 0; b1 = X; }

    uint8_t R = (uint8_t)lrintf((r1 + m) * 255.0f);
    uint8_t G = (uint8_t)lrintf((g1 + m) * 255.0f);
    uint8_t B = (uint8_t)lrintf((b1 + m) * 255.0f);
    *r = R; *g = G; *b = B;
}

/* ---------- RMT/led_strip 初始化 ---------- */
static esp_err_t led_ring_init_strip(void)
{
    if (s_led_strip) return ESP_OK;

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_RING_GPIO,
        .max_leds       = LED_RING_COUNT,
        .led_model      = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false }
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = LED_RING_RES_HZ,
        .mem_block_symbols = LED_RING_MEM_WORDS,
        .flags = { .with_dma = LED_RING_USE_DMA }
    };
    ESP_RETURN_ON_ERROR(
        led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip),
        LED_RING_TAG, "create rmt led_strip failed");

    ESP_LOGI(LED_RING_TAG, "LED ring created on GPIO %d, count=%d", LED_RING_GPIO, LED_RING_COUNT);
    return ESP_OK;
}

/* ---------- 软件定时器回调：刷新旋转 ---------- */
static void led_ring_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    float base_hue_deg, hue_span_deg, brightness, saturation, upd_hz;
    taskENTER_CRITICAL(&s_param_spinlock);
    base_hue_deg = s_ctrl.base_hue_deg;
    hue_span_deg = s_ctrl.hue_span_deg;
    brightness   = s_ctrl.brightness;
    saturation   = s_ctrl.saturation;
    upd_hz       = s_ctrl.update_hz;
    taskEXIT_CRITICAL(&s_param_spinlock);

    for (int i = 0; i < LED_RING_COUNT; ++i) {
        float hue = base_hue_deg + i * hue_span_deg;
        uint8_t r,g,b;
        hsv_to_rgb(hue, saturation, brightness, &r, &g, &b);
        (void)led_strip_set_pixel(s_led_strip, i, r, g, b);
    }
    (void)led_strip_refresh(s_led_strip);

    taskENTER_CRITICAL(&s_param_spinlock);
    float delta_deg = s_ctrl.speed_deg_per_sec / (upd_hz > 0 ? upd_hz : 50.0f);
    s_ctrl.base_hue_deg += delta_deg;
    if (s_ctrl.base_hue_deg >= 360.0f) s_ctrl.base_hue_deg -= 360.0f;
    if (s_ctrl.base_hue_deg < 0.0f)    s_ctrl.base_hue_deg += 360.0f;
    taskEXIT_CRITICAL(&s_param_spinlock);
}

/* ---------- 对外 API ---------- */
esp_err_t led_ring_start(float brightness_0_1, float speed_deg_per_sec, float update_hz)
{
    ESP_RETURN_ON_ERROR(led_ring_init_strip(), LED_RING_TAG, "strip init failed");

    taskENTER_CRITICAL(&s_param_spinlock);
    s_ctrl.brightness        = fminf(fmaxf(brightness_0_1, 0.0f), 1.0f);
    s_ctrl.speed_deg_per_sec = speed_deg_per_sec;
    s_ctrl.update_hz         = (update_hz > 2.0f ? update_hz : 50.0f);
    s_ctrl.hue_span_deg      = 360.0f / (float)LED_RING_COUNT;
    s_ctrl.base_hue_deg      = 0.0f;
    taskEXIT_CRITICAL(&s_param_spinlock);

    if (s_timer) {
        xTimerStop(s_timer, 0);
        xTimerDelete(s_timer, 0);
        s_timer = NULL;
    }
    TickType_t period_ticks = pdMS_TO_TICKS((TickType_t)lrintf(1000.0f / s_ctrl.update_hz));
    if (period_ticks == 0) period_ticks = 1;

    s_timer = xTimerCreate("led_ring_tmr", period_ticks, pdTRUE, NULL, led_ring_timer_cb);
    if (!s_timer) {
        ESP_LOGE(LED_RING_TAG, "create timer failed");
        return ESP_FAIL;
    }
    if (xTimerStart(s_timer, 0) != pdPASS) {
        ESP_LOGE(LED_RING_TAG, "start timer failed");
        return ESP_FAIL;
    }
    ESP_LOGI(LED_RING_TAG, "LED ring started: brightness=%.2f, speed=%.1f deg/s, %.1f Hz",
             s_ctrl.brightness, s_ctrl.speed_deg_per_sec, s_ctrl.update_hz);
    return ESP_OK;
}

void led_ring_stop(void)
{
    if (s_timer) {
        xTimerStop(s_timer, 0);
        xTimerDelete(s_timer, 0);
        s_timer = NULL;
    }
    if (s_led_strip) {
        led_strip_clear(s_led_strip);
        led_strip_refresh(s_led_strip);
    }
}

void led_ring_set_brightness(float brightness_0_1)
{
    taskENTER_CRITICAL(&s_param_spinlock);
    s_ctrl.brightness = fminf(fmaxf(brightness_0_1, 0.0f), 1.0f);
    taskEXIT_CRITICAL(&s_param_spinlock);
}

void led_ring_set_speed_deg(float speed_deg_per_sec)
{
    taskENTER_CRITICAL(&s_param_spinlock);
    s_ctrl.speed_deg_per_sec = speed_deg_per_sec;
    taskEXIT_CRITICAL(&s_param_spinlock);
}

void led_ring_set_speed_pixels(float pixels_per_sec)
{
    float deg_per_sec = pixels_per_sec * (360.0f / (float)LED_RING_COUNT);
    led_ring_set_speed_deg(deg_per_sec);
}

void led_ring_set_update_hz(float update_hz)
{
    if (update_hz < 2.0f) update_hz = 2.0f;

    taskENTER_CRITICAL(&s_param_spinlock);
    s_ctrl.update_hz = update_hz;
    taskEXIT_CRITICAL(&s_param_spinlock);

    if (s_timer) {
        TickType_t period_ticks = pdMS_TO_TICKS((TickType_t)lrintf(1000.0f / update_hz));
        if (period_ticks == 0) period_ticks = 1;
        xTimerChangePeriod(s_timer, period_ticks, 0);
    }
}

void led_ring_set_saturation(float saturation_0_1)
{
    taskENTER_CRITICAL(&s_param_spinlock);
    s_ctrl.saturation = fminf(fmaxf(saturation_0_1, 0.0f), 1.0f);
    taskEXIT_CRITICAL(&s_param_spinlock);
}

void led_ring_set_hue_span_deg(float hue_span_deg)
{
    taskENTER_CRITICAL(&s_param_spinlock);
    s_ctrl.hue_span_deg = hue_span_deg;
    taskEXIT_CRITICAL(&s_param_spinlock);
}
