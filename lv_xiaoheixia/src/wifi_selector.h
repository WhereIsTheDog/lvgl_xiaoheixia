/*
 * wifi_selector.h - WiFi选择界面
 * 
 * 界面布局 (240x220 可见区域):
 * - 顶部状态栏 (22px): 标题 "WiFi Settings"
 * - WiFi列表区: 可滚动的WiFi网络列表，每项30px高
 * - 底部提示栏 (32px): 操作提示
 * 
 * 控制方式:
 * - 波轮: 上下滚动列表 / 选择字符
 * - K1: 选择当前WiFi / 确认字符
 * - K3: 刷新WiFi列表 / 删除字符
 * - K5: 返回主界面 / 取消输入
 */

#ifndef WIFI_SELECTOR_H
#define WIFI_SELECTOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

#define WIFI_MAX_NETWORKS  20
#define WIFI_SSID_MAX_LEN  32
#define WIFI_PWD_MAX_LEN   64

/* WiFi网络信息结构 */
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    int  rssi;               /* 信号强度 (dBm) */
    int  security;           /* 0=开放, 1=加密 */
    int  connected;          /* 是否当前连接 */
} wifi_network_t;

/* 密码输入状态 */
typedef struct {
    lv_obj_t *panel;         /* 密码输入面板 */
    lv_obj_t *title_lbl;     /* 显示SSID */
    lv_obj_t *pwd_lbl;       /* 显示已输入密码 */
    lv_obj_t *keyboard;      /* 小键盘容器 */
    lv_obj_t *hint_lbl;      /* 操作提示 */
    
    char ssid[WIFI_SSID_MAX_LEN + 1];  /* 目标SSID */
    char password[WIFI_PWD_MAX_LEN + 1]; /* 输入的密码 */
    int  pwd_len;            /* 当前密码长度 */
    
    /* 小键盘导航 */
    int  kbd_row;            /* 当前行 (0-4) */
    int  kbd_col;            /* 当前列 (0-9) */
    int  visible;            /* 是否显示中 */
} wifi_pwd_input_t;

/* WiFi选择器状态 */
typedef struct {
    lv_obj_t *scr;
    lv_obj_t *title_bar;
    lv_obj_t *title_lbl;
    
    lv_obj_t *list;          /* 主列表容器 */
    lv_obj_t *list_items[WIFI_MAX_NETWORKS];
    
    lv_obj_t *hint_bar;      /* 底部提示栏 */
    lv_obj_t *hint_lbl;
    
    lv_obj_t *loading_spinner;
    
    wifi_network_t networks[WIFI_MAX_NETWORKS];
    int network_count;
    int selected_idx;        /* 当前选中的索引 */
    
    int rotary_fd;
    int last_rotary_count;

    /* key event device fd (/dev/input/event2) for encoder click (code=0x09) */
    int keypad_fd;
    
    lv_timer_t *update_tmr;
    
    int scan_pending;        /* 扫描进行中标志 */
    uint32_t scan_start_time; /* 扫描开始时间 */
    
    wifi_pwd_input_t pwd_input;  /* 密码输入状态 */
    
    void (*on_select_callback)(const char *ssid, int security);
    void (*on_exit_callback)(void);
    
} wifi_selector_t;

/* 初始化WiFi选择器 */
void wifi_selector_init(wifi_selector_t *ws);

/* 显示WiFi选择界面 */
void wifi_selector_show(wifi_selector_t *ws);

/* 隐藏并返回主界面 */
void wifi_selector_hide(wifi_selector_t *ws);

/* 刷新WiFi列表 */
void wifi_selector_scan(wifi_selector_t *ws);

/* 设置选择回调 */
void wifi_selector_set_callbacks(wifi_selector_t *ws,
                                 void (*on_select)(const char *, int),
                                 void (*on_exit)(void));

/* 显示密码输入界面 */
void wifi_selector_show_pwd_input(wifi_selector_t *ws, const char *ssid);

/* 开机自动连接：扫描已知 AP，按信号强度逐一尝试 */
void wifi_autoconnect_start(void);
/* 轮询自动连接结果，返回 1=成功连接。由 dash_timer_cb 每秒调用一次 */
int  wifi_autoconnect_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SELECTOR_H */
