/*
 * weather.h - 天气显示界面
 *
 * 从 wttr.in 获取天气数据（纯 HTTP，无需 SSL）
 * 通过 fork+exec wget 在后台抓取，LVGL 定时器轮询结果文件
 *
 * 控制:
 *   K3 - 返回主界面
 *   K4 - 刷新天气
 */

#ifndef WEATHER_H
#define WEATHER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

typedef struct {
    lv_obj_t  *scr;

    lv_obj_t  *lbl_city;
    lv_obj_t  *lbl_temp;
    lv_obj_t  *lbl_condition;
    lv_obj_t  *lbl_humidity;
    lv_obj_t  *lbl_wind;
    lv_obj_t  *lbl_status;

    lv_timer_t *tmr;

    int        last_k3;
    int        last_k4;
    int        last_k5;
    int        fetch_done;
    uint32_t   fetch_start_ms;
    uint32_t   last_fetch_ok_ms;   /* for auto-refresh every 10 min */

    lv_obj_t  *back_scr;   /* 返回时加载的屏幕（dashboard） */
} weather_screen_t;

/* 初始化（清零结构体） */
void weather_screen_init(weather_screen_t *ws);

/* 显示天气界面，back_scr 为返回时要加载的屏幕 */
void weather_screen_show(weather_screen_t *ws, lv_obj_t *back_scr);

/* 隐藏天气界面并返回 back_scr */
void weather_screen_hide(weather_screen_t *ws);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_H */
