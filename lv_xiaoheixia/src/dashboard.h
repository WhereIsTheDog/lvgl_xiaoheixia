#ifndef DASHBOARD_H
#define DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/*
 * ADC channel → joystick axis mapping.
 * ch4/ch5 = left stick, ch2/ch3 = right stick.
 */
#define DASH_JOY_L_X_CH  4
#define DASH_JOY_L_Y_CH  5
#define DASH_JOY_R_X_CH  2
#define DASH_JOY_R_Y_CH  3

/* CH0 is a resistor-ladder keypad: 5 button states */
#define DASH_CH0_BTN_COUNT 5

#define DASH_ADC_MAX       1023

/* GPIO pins for button indicators (active-low: lo = pressed) */
#define DASH_GPIO_COUNT    4

/* Rotary encoder input device (REL_X events)
 * Primary: /dev/input/event1, fallback to event0/event2 if unavailable */
#define DASH_ROTARY_DEV    "/dev/input/event1"

/* Key event device: rockchip,key driver - handles ADC keys (K1-K5) and
 * GPIO keys (C1-C3, JB/encoder click) as EV_KEY events.
 * Codes: stop=0x02(K1), return=0x03(K2), home=0x01(K3), snapshot=0x04(K4),
 *        recording=0x05(K5), cus1=0x06(C1), cus2=0x07(C2), cus3=0x08(C3),
 *        joybtn=0x09(JB/encoder-click) */
#define DASH_KEYPAD_DEV    "/dev/input/event2"

/* Key code for rotary encoder click button (joybtn-key in DTS) */
#define DASH_KEY_ENCODER_CLICK  0x09

/* Timer interval for sensor polling (ms) */
#define DASH_UPDATE_MS     100

typedef struct {
    lv_obj_t *scr;

    /* status bar */
    lv_obj_t *lbl_bat;
    lv_obj_t *lbl_time;
    lv_obj_t *lbl_rssi;

    /* joystick panels */
    lv_obj_t *joy_l_area;
    lv_obj_t *joy_l_dot;
    lv_obj_t *joy_l_val;

    lv_obj_t *joy_r_area;
    lv_obj_t *joy_r_dot;
    lv_obj_t *joy_r_val;

    /* CH0 button indicators */
    lv_obj_t *ch0_led[DASH_CH0_BTN_COUNT];
    lv_obj_t *lbl_ch0_raw;

    /* rotary encoder */
    lv_obj_t *lbl_rotary;
    int       rotary_fd;
    int       rotary_count;
    int       rotary_last_ab;   /* last gray-code state (bits 1:0 = A:B) */
    int       rotary_sub;       /* sub-step accumulator (4 sub-steps = 1 tick) */
    void     *gpio3_map;        /* mmap of GPIO3 registers via /dev/mem */
    int       mem_fd;

    /* key event device fd (/dev/input/event2: all ADC+GPIO keys) */
    int       keypad_fd;

    /* power button */
    int       power_fd;
    uint32_t  power_startup_ms; /* lv_tick at app start, for grace period */
    int       screen_off;       /* 1 = backlight off */

    /* RTSP in-process player */
    pid_t     rtsp_pid;         /* child PID while playing, 0 = idle */

    /* GPIO LED indicators */
    lv_obj_t *gpio_led[DASH_GPIO_COUNT];

    /* bottom info */
    lv_obj_t *lbl_info;

    lv_timer_t *tmr;
} dashboard_t;

void dashboard_init(dashboard_t *d);

/* Set to 1 to pause LVGL framebuffer writes (preview_stream owns FB) */
extern volatile int g_fb_paused;

#ifdef __cplusplus
}
#endif

#endif /* DASHBOARD_H */
