#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

/* ====== 可根据你的硬件在 Kconfig 或这里改默认值 ====== */
/* 若需要临时覆盖，可在编译命令里 -DXXX=... 或在此处修改 */
#ifndef LED_RING_GPIO
#define LED_RING_GPIO            (38)     // 灯环数据脚
#endif
#ifndef LED_RING_COUNT
#define LED_RING_COUNT           (8)      // 8 颗 RGB
#endif
#ifndef LED_RING_RES_HZ
#define LED_RING_RES_HZ          (10 * 1000 * 1000) // RMT 10MHz
#endif
#ifndef LED_RING_USE_DMA
#define LED_RING_USE_DMA         (0)      // ESP32-S3 可设 1
#endif
#ifndef LED_RING_MEM_WORDS
#define LED_RING_MEM_WORDS       (LED_RING_USE_DMA ? 1024 : 0)
#endif

/* ====== 对外 API ====== */

/**
 * @brief 启动彩色环绕（创建并启动 FreeRTOS 软件定时器）
 * @param brightness_0_1   亮度 [0.0, 1.0]
 * @param speed_deg_per_sec 旋转速度（度/秒），正值顺时针、负值逆时针
 * @param update_hz        刷新频率（建议 50~120）
 */
esp_err_t led_ring_start(float brightness_0_1, float speed_deg_per_sec, float update_hz);

/** 停止彩灯（清灯并删除定时器，不释放 driver 句柄） */
void led_ring_stop(void);

/** 动态调亮度 [0.0, 1.0] */
void led_ring_set_brightness(float brightness_0_1);

/** 动态调速度（度/秒），正=顺时针，负=逆时针 */
void led_ring_set_speed_deg(float speed_deg_per_sec);

/** 以“像素/秒”设置速度（每秒移动几颗灯） */
void led_ring_set_speed_pixels(float pixels_per_sec);

/** 动态调刷新率（Hz） */
void led_ring_set_update_hz(float update_hz);

/** 动态调饱和度 [0.0, 1.0] */
void led_ring_set_saturation(float saturation_0_1);

/** 调整相邻 LED 的色相间隔（度），默认 360/LED_RING_COUNT */
void led_ring_set_hue_span_deg(float hue_span_deg);

#ifdef __cplusplus
}
#endif
