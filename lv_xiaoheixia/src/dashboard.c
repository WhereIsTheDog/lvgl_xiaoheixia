#include "dashboard.h"
#include "wifi_selector.h"
#include "weather.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <linux/input.h>

/* ── GPIO3 direct register access for rotary encoder ──────────────────────
 * RV1108 GPIO3 base: 0x10330000, GPIO_EXT_PORTA (input) at offset 0x50
 * gimbal_A = bit 4, gimbal_B = bit 3 (ACTIVE_LOW so invert before decode) */
#define GPIO3_BASE          0x10330000UL
#define GPIO3_MAP_SIZE      0x100
#define GPIO_EXT_PORT_OFF   0x50
#define ROTARY_PIN_A        4   /* gimbal_A */
#define ROTARY_PIN_B        3   /* gimbal_B */

/* Backlight sysfs */
#define BACKLIGHT_PATH  "/sys/class/backlight/rk28_bl/brightness"
/* Power key startup grace period (ms) — ignore events during boot */
#define POWER_GRACE_MS  3000

/* ────────────────────── colours ────────────────────── */
#define C_BG         lv_color_hex(0x1a1a2e)
#define C_PANEL      lv_color_hex(0x16213e)
#define C_JOY_BG     lv_color_hex(0x0f1b30)
#define C_CROSS      lv_color_hex(0x2a3a5c)
#define C_DOT        lv_color_hex(0x00e676)
#define C_ACCENT     lv_color_hex(0x00b4d8)
#define C_TEXT       lv_color_hex(0xe0e0e0)
#define C_MUTED      lv_color_hex(0x778899)
#define C_GPIO_ON    lv_color_hex(0x00e676)
#define C_GPIO_OFF   lv_color_hex(0x333344)
#define C_BTN_ACTIVE lv_color_hex(0xff9800)
#define C_CHARGE     lv_color_hex(0xffd600)

/* ────────────────────── layout ─────────────────────── */
#define SCR_W    240
#define SCR_H    240
#define VISIBLE_H 220   /* bottom 20px covered by case */

#define STAT_H   20

#define JOY_Y    22
#define JOY_W    114
#define JOY_H    98
#define JOY_L_X  3
#define JOY_R_X  123
#define JOY_DOT  10
#define JOY_PAD  6

#define KEY_ROW_Y  124

#define GPIO_Y    150
#define GPIO_SZ   14

#define INFO_Y    186

/* ────────────── CH0 button thresholds ────────────── */
/* ADC centre values for the 5 resistor-ladder buttons */
static const char *ch0_names[DASH_CH0_BTN_COUNT] = {"K1", "K2", "K3", "K4", "K5"};
/* upper boundary for each button zone (midpoints between centres) */
static const int ch0_thresh[DASH_CH0_BTN_COUNT] = {55, 173, 245, 358, 700};

/* ────────────────────── GPIO pin table ─────────────── */
static const int gpio_pins[DASH_GPIO_COUNT] = {97, 96, 95, 98};
static const char *gpio_tags[DASH_GPIO_COUNT] = {"C1", "C2", "C3", "JB"};

/* ────────────────── joystick crosshair points ──────── */
static lv_point_t cross_h[] = {{0, JOY_H / 2}, {JOY_W - 1, JOY_H / 2}};
static lv_point_t cross_v[] = {{JOY_W / 2, 0}, {JOY_W / 2, JOY_H - 1}};

/* ────────────────────── WiFi selector ──────────────── */
static wifi_selector_t g_wifi_sel;
static int g_wifi_sel_inited = 0;
static dashboard_t *g_dash_ptr = NULL;

/* ────────────────────── Weather screen ─────────────── */
static weather_screen_t g_weather;
static int g_weather_inited = 0;

/* ────────────────────── Menu system ────────────────── */
#define MENU_ITEM_COUNT 3
#define MENU_HEIGHT 40
#define MENU_ITEM_W  70
#define MENU_ITEM_H  30

static lv_obj_t *g_menu_panel = NULL;
static lv_obj_t *g_menu_items[MENU_ITEM_COUNT];
static int g_menu_visible = 0;
static int g_menu_selected = 0;
static int g_last_k3 = 0;  /* K3 previous state for edge detection */

static const char *menu_labels[MENU_ITEM_COUNT] = {
    LV_SYMBOL_WIFI " WiFi",
    LV_SYMBOL_VIDEO " RTSP",
    LV_SYMBOL_IMAGE " Wthr"
};

/* RTSP流地址 — 持久保存在与可执行文件同目录 */
#define RTSP_DEFAULT_URL "rtsp://admin:QMXEVX@192.168.95.247:554/h264/ch1/sub/av_stream"
#define RTSP_URL_PATH    "/mnt/udisk/product_test/rtsp_url.txt"

/* 系统时间持久化路径 */
#define LAST_TIME_PATH   "/mnt/udisk/last_time.txt"
#define TIME_SAVE_INTERVAL 3000  /* 每 3000 个 tick（5 min）保存一次 */

/* 读取持久化 RTSP URL，若文件不存在则创建并写入默认值 */
static const char *get_rtsp_url(void)
{
    static char url_buf[512];

    FILE *f = fopen(RTSP_URL_PATH, "r");
    if (f) {
        if (fgets(url_buf, sizeof(url_buf), f)) {
            url_buf[strcspn(url_buf, "\r\n")] = 0;
            fclose(f);
            if (url_buf[0])
                return url_buf;
        }
        fclose(f);
    }

    /* 文件不存在或为空，写入默认值 */
    f = fopen(RTSP_URL_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", RTSP_DEFAULT_URL);
        fclose(f);
    }
    strncpy(url_buf, RTSP_DEFAULT_URL, sizeof(url_buf) - 1);
    return url_buf;
}

/* C2按键状态 */
static int g_last_c2 = 1;  /* C2 previous state (1=not pressed, GPIO active low) */

/* Forward declarations for menu functions */
static void update_menu_highlight(void);
static void create_menu(lv_obj_t *parent);
static void show_menu(void);
static void hide_menu(void);
static void menu_move_left(void);
static void menu_move_right(void);
static void menu_select(dashboard_t *d);
static void start_rtsp_player(dashboard_t *d);

/* ═══════════════════ sysfs helpers ═══════════════════ */

static int read_sysfs_int(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static int read_adc(int ch)
{
    char p[128];
    snprintf(p, sizeof(p),
             "/sys/devices/1038c000.adc/iio:device0/in_voltage%d_raw", ch);
    return read_sysfs_int(p);
}

static int read_bat_cap(void)
{
    return read_sysfs_int("/sys/class/power_supply/battery/capacity");
}

static int read_bat_mv(void)
{
    int uv = read_sysfs_int("/sys/class/power_supply/battery/voltage_now");
    return (uv > 0) ? uv / 1000 : -1;
}

static void read_bat_status(char *buf, int len)
{
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
    if (!f) {
        snprintf(buf, len, "N/A");
        return;
    }
    if (!fgets(buf, len, f))
        snprintf(buf, len, "N/A");
    fclose(f);
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
}

static int read_wifi_rssi(void)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f)
        return -999;
    char line[256];
    int rssi = -999;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "wlan0")) {
            char *colon = strchr(line, ':');
            if (colon) {
                int st;
                float lnk, lvl;
                if (sscanf(colon + 1, "%d %f %f", &st, &lnk, &lvl) >= 3)
                    rssi = (int)lvl;
            }
            break;
        }
    }
    fclose(f);
    return rssi;
}

/* 读取当前连接的WiFi SSID — 从文件读取，不调用 wpa_cli
 * /tmp/wifi_connected_ssid.txt 由后台脚本 wifi_connect.sh 和
 * wifi_scan_bg.sh 写入，主线程只用 fopen 读（零阻塞）。
 * ⚠️ 绝不在主线程 popen("wpa_cli") — 会阻塞导致 ADB 冻结 */
static void read_wifi_ssid(char *buf, int len)
{
    buf[0] = '\0';
    FILE *f = fopen("/tmp/wifi_connected_ssid.txt", "r");
    if (!f) return;
    if (fgets(buf, len, f)) {
        /* 去除换行符 */
        buf[strcspn(buf, "\n\r")] = '\0';
    }
    fclose(f);
}

static void read_all_gpio(int *out)
{
    for (int i = 0; i < DASH_GPIO_COUNT; i++)
        out[i] = -1;

    FILE *f = fopen("/sys/kernel/debug/gpio", "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        for (int i = 0; i < DASH_GPIO_COUNT; i++) {
            char tag[32];
            snprintf(tag, sizeof(tag), "gpio-%d ", gpio_pins[i]);
            if (!strstr(line, tag))
                continue;
            int len = (int)strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == ' '))
                line[--len] = '\0';
            if (len >= 2 && line[len - 2] == 'h' && line[len - 1] == 'i')
                out[i] = 1;
            else if (len >= 2 && line[len - 2] == 'l' && line[len - 1] == 'o')
                out[i] = 0;
        }
    }
    fclose(f);
}

/* ── rotary encoder: read via /dev/mem GPIO3 registers (gray code) ── */
/* Called from a 10ms timer. Emits ±1 only on arriving at detent (ab==0).  */
static void encoder_timer_cb(lv_timer_t *t)
{
    dashboard_t *d = (dashboard_t *)t->user_data;
    if (!d->gpio3_map)
        return;

    volatile uint32_t *ext = (volatile uint32_t *)
        ((uint8_t *)d->gpio3_map + GPIO_EXT_PORT_OFF);
    uint32_t raw = *ext;
    int a = !((raw >> ROTARY_PIN_A) & 1);
    int b = !((raw >> ROTARY_PIN_B) & 1);
    int ab = (a << 1) | b;

    static const int8_t table[4][4] = {
        /* curr: 00  01  10  11 */
        { 0,  1, -1,  0 }, /* prev 00 */
        {-1,  0,  0,  1 }, /* prev 01 */
        { 1,  0,  0, -1 }, /* prev 10 */
        { 0, -1,  1,  0 }, /* prev 11 */
    };
    d->rotary_sub += table[d->rotary_last_ab][ab];
    d->rotary_last_ab = ab;

    /* At detent (ab==0): reset accumulator to eliminate bounce noise */
    if (ab == 0) {
        d->rotary_sub = 0;
        return;
    }

    /* Emit after 2 same-direction sub-steps — responsive mid-rotation,
     * no false triggers since bounces at detent are cleared above */
    int delta = 0;
    if (d->rotary_sub >= 2) {
        delta = 1;
        d->rotary_sub -= 2;
    } else if (d->rotary_sub <= -2) {
        delta = -1;
        d->rotary_sub += 2;
    }
    if (delta == 0)
        return;
    d->rotary_count += delta;

    /* Update display label */
    char buf[32];
    snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP "%d", d->rotary_count);
    lv_label_set_text(d->lbl_rotary, buf);

    /* Menu navigation */
    if (g_menu_visible) {
        if (delta > 0) menu_move_right();
        else           menu_move_left();
    }
}

/* ── screen backlight control ── */
static void set_backlight(int on)
{
    /* Turn off: disable fb0 display + set brightness to 0
     * Turn on:  re-enable fb0 + restore full brightness */
    FILE *f = fopen("/sys/class/graphics/fb0/enable", "w");
    if (f) { fprintf(f, "%d\n", on ? 1 : 0); fclose(f); }

    f = fopen(BACKLIGHT_PATH, "w");
    if (f) { fprintf(f, "%d\n", on ? 255 : 0); fclose(f); }
}

static void toggle_screen(dashboard_t *d)
{
    d->screen_off = !d->screen_off;
    set_backlight(!d->screen_off);
    printf("[Dashboard] Screen %s\n", d->screen_off ? "OFF" : "ON");
}

/* ── key event helper: drain fd and return 1 if target key was pressed ── */
static int drain_event_key(int fd, int code)
{
    if (fd < 0)
        return 0;
    struct input_event ev;
    int got_press = 0;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY && (int)ev.code == code && ev.value == 1)
            got_press = 1;
    }
    return got_press;
}

static int identify_ch0_btn(int raw)
{
    if (raw < 0)
        return -1;
    for (int i = 0; i < DASH_CH0_BTN_COUNT; i++) {
        if (raw <= ch0_thresh[i])
            return i;
    }
    return -1;  /* idle / no button */
}

/* ═══════════════════ UI helpers ══════════════════════ */

static void style_no_border(lv_obj_t *obj)
{
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *par, const lv_font_t *font,
                            lv_color_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    return l;
}

/* ─── create one joystick panel ─── */
static void create_joy_panel(dashboard_t *d, lv_obj_t **area,
                             lv_obj_t **dot, lv_obj_t **val_lbl,
                             lv_coord_t x)
{
    *area = lv_obj_create(d->scr);
    lv_obj_set_pos(*area, x, JOY_Y);
    lv_obj_set_size(*area, JOY_W, JOY_H);
    style_no_border(*area);
    lv_obj_set_style_bg_color(*area, C_JOY_BG, 0);
    lv_obj_set_style_bg_opa(*area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*area, 6, 0);

    lv_obj_t *hl = lv_line_create(*area);
    lv_line_set_points(hl, cross_h, 2);
    lv_obj_set_style_line_width(hl, 1, 0);
    lv_obj_set_style_line_color(hl, C_CROSS, 0);

    lv_obj_t *vl = lv_line_create(*area);
    lv_line_set_points(vl, cross_v, 2);
    lv_obj_set_style_line_width(vl, 1, 0);
    lv_obj_set_style_line_color(vl, C_CROSS, 0);

    *dot = lv_obj_create(*area);
    lv_obj_set_size(*dot, JOY_DOT, JOY_DOT);
    lv_obj_set_style_radius(*dot, JOY_DOT / 2, 0);
    lv_obj_set_style_bg_color(*dot, C_DOT, 0);
    lv_obj_set_style_bg_opa(*dot, LV_OPA_COVER, 0);
    style_no_border(*dot);
    lv_obj_set_pos(*dot, JOY_W / 2 - JOY_DOT / 2, JOY_H / 2 - JOY_DOT / 2);

    *val_lbl = make_label(*area, &lv_font_montserrat_10, C_MUTED, "---,---");
    lv_obj_align(*val_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void update_joy(lv_obj_t *dot, lv_obj_t *lbl, int ax, int ay)
{
    if (ax < 0) ax = DASH_ADC_MAX / 2;
    if (ay < 0) ay = DASH_ADC_MAX / 2;

    int x_range = JOY_W - 2 * JOY_PAD - JOY_DOT;
    int y_range = JOY_H - 2 * JOY_PAD - JOY_DOT;
    int dx = JOY_PAD + ((DASH_ADC_MAX - ax) * x_range) / DASH_ADC_MAX;
    int dy = JOY_PAD + ((DASH_ADC_MAX - ay) * y_range) / DASH_ADC_MAX;
    lv_obj_set_pos(dot, dx, dy);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d,%d", ax, ay);
    lv_label_set_text(lbl, buf);
}

/* ═══════════════ system time persistence ═════════════= */

/* 保存当前系统时间到持久存储（JFFS2），供下次开机恢复 */
static void save_system_time(void)
{
    time_t now = time(NULL);
    /* 只在时间合理（>2020年）时才保存，避免保存默认的 2016 时间 */
    if (now < 1577836800) return;  /* 2020-01-01 */
    FILE *f = fopen(LAST_TIME_PATH, "w");
    if (f) {
        fprintf(f, "%ld\n", (long)now);
        fclose(f);
    }
}

/* ═══════════════════ timer callback ═════════════════= */

static void dash_timer_cb(lv_timer_t *t)
{
    dashboard_t *d = (dashboard_t *)t->user_data;
    char buf[64];

    /* ── battery ── */
    int cap = read_bat_cap();
    int mv  = read_bat_mv();
    char bat_st[24];
    read_bat_status(bat_st, sizeof(bat_st));

    const char *bat_icon = LV_SYMBOL_BATTERY_FULL;
    if (cap >= 0) {
        if (cap < 15)       bat_icon = LV_SYMBOL_BATTERY_EMPTY;
        else if (cap < 40)  bat_icon = LV_SYMBOL_BATTERY_1;
        else if (cap < 65)  bat_icon = LV_SYMBOL_BATTERY_2;
        else if (cap < 85)  bat_icon = LV_SYMBOL_BATTERY_3;
    }

    if (cap >= 0)
        snprintf(buf, sizeof(buf), "%s %d%%", bat_icon, cap);
    else
        snprintf(buf, sizeof(buf), "%s --", bat_icon);
    lv_label_set_text(d->lbl_bat, buf);

    if (strcmp(bat_st, "Charging") == 0)
        lv_obj_set_style_text_color(d->lbl_bat, C_CHARGE, 0);
    else if (cap >= 0 && cap < 20)
        lv_obj_set_style_text_color(d->lbl_bat, lv_color_hex(0xff5252), 0);
    else
        lv_obj_set_style_text_color(d->lbl_bat, C_TEXT, 0);

    /* ── time ── */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             lt->tm_hour, lt->tm_min, lt->tm_sec);
    lv_label_set_text(d->lbl_time, buf);

    /* ── 定期保存系统时间到持久存储（每 ~5 分钟） ── */
    static int time_save_ctr = 0;
    if (++time_save_ctr >= TIME_SAVE_INTERVAL) {
        time_save_ctr = 0;
        save_system_time();
    }

    /* ── WiFi SSID + RSSI (throttled to every ~5s to avoid USB driver contention) ── */
    static int wifi_tick_ctr = 0;
    static char cached_ssid[64] = {0};
    static int  cached_rssi = -999;
    if (++wifi_tick_ctr >= 50) {   /* 50 × 100ms = 5 seconds */
        wifi_tick_ctr = 0;
        /* 轮询开机自动连接结果（完成后自动停止轮询） */
        if (wifi_autoconnect_poll()) {
            /* 刚连接成功，清空缓存强制立即刷新 SSID 显示 */
            cached_ssid[0] = '\0';
            cached_rssi    = -999;
        }
        read_wifi_ssid(cached_ssid, sizeof(cached_ssid));
        cached_rssi = read_wifi_rssi();
    }
    
    if (cached_ssid[0] != '\0') {
        /* 已连接WiFi - 显示SSID和信号强度 */
        if (cached_rssi > -999) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s %ddBm", cached_ssid, cached_rssi);
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", cached_ssid);
        }
        lv_obj_set_style_text_color(d->lbl_rssi, C_ACCENT, 0);
    } else {
        /* 未连接WiFi */
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " --");
        lv_obj_set_style_text_color(d->lbl_rssi, C_MUTED, 0);
    }
    lv_label_set_text(d->lbl_rssi, buf);

    /* ── joysticks ── */
    int lx = read_adc(DASH_JOY_L_X_CH);
    int ly = read_adc(DASH_JOY_L_Y_CH);
    int rx = read_adc(DASH_JOY_R_X_CH);
    int ry = read_adc(DASH_JOY_R_Y_CH);
    update_joy(d->joy_l_dot, d->joy_l_val, lx, ly);
    update_joy(d->joy_r_dot, d->joy_r_val, rx, ry);

    /* ── CH0 buttons ── */
    int ch0_raw = read_adc(0);
    int active_btn = identify_ch0_btn(ch0_raw);
    for (int i = 0; i < DASH_CH0_BTN_COUNT; i++) {
        if (i == active_btn) {
            lv_led_on(d->ch0_led[i]);
            lv_led_set_color(d->ch0_led[i], C_BTN_ACTIVE);
        } else {
            lv_led_off(d->ch0_led[i]);
            lv_led_set_color(d->ch0_led[i], C_GPIO_OFF);
        }
    }
    if (ch0_raw >= 0)
        snprintf(buf, sizeof(buf), "%d", ch0_raw);
    else
        snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(d->lbl_ch0_raw, buf);

    /* ── GPIO buttons ── */
    int gv[DASH_GPIO_COUNT];
    read_all_gpio(gv);
    for (int i = 0; i < DASH_GPIO_COUNT; i++) {
        if (gv[i] == 0) {
            lv_led_on(d->gpio_led[i]);
            lv_led_set_color(d->gpio_led[i], C_GPIO_ON);
        } else {
            lv_led_off(d->gpio_led[i]);
            lv_led_set_color(d->gpio_led[i], C_GPIO_OFF);
        }
    }
    
    /* ── C2按键 (GPIO 96, index 1) 直接启动RTSP播放 ── */
    int c2_pressed = (gv[1] == 0);  /* Active low */
    if (c2_pressed && g_last_c2) {
        /* C2 falling edge - 启动RTSP播放 */
        start_rtsp_player(d);
    }
    g_last_c2 = c2_pressed ? 0 : 1;

    /* ── Menu control via K3 and encoder click ── */
    /* K3 detection: ch0_thresh[2] = 245, previous threshold = 173 */
    int k3_pressed = (ch0_raw > 173 && ch0_raw <= 245);
    /* Encoder click (JB): read from event2 as EV_KEY code=0x09 */
    int encoder_clicked = drain_event_key(d->keypad_fd, DASH_KEY_ENCODER_CLICK);

    /* K3 falling edge (press) toggles menu */
    if (k3_pressed && !g_last_k3) {
        if (g_menu_visible) {
            hide_menu();
        } else {
            show_menu();
        }
    }
    g_last_k3 = k3_pressed;

    /* Encoder click: open menu when closed, select item when open */
    if (encoder_clicked) {
        if (!g_menu_visible) {
            show_menu();
        } else {
            menu_select(d);
        }
    }

    /* When menu is visible, handle navigation and selection */
    if (g_menu_visible) {
        /* Rotary encoder navigation handled by encoder_timer_cb (10ms) */
        
        /* 左操纵杆左右导航菜单 (方向已反转匹配实际硬件) */
        static int last_lx_menu = 512;
        if (lx < 300 && last_lx_menu >= 300) {
            menu_move_right();  /* 反转 */
        } else if (lx > 700 && last_lx_menu <= 700) {
            menu_move_left();   /* 反转 */
        }
        last_lx_menu = lx;
        
        /* K4 detection: ch0_thresh[3] = 358, previous threshold = 245 */
        int k4_pressed = (ch0_raw > 245 && ch0_raw <= 358);
        static int g_last_k4 = 0;
        
        /* K4 falling edge (press) selects current item */
        if (k4_pressed && !g_last_k4) {
            menu_select(d);
        }
        g_last_k4 = k4_pressed;
    }

    /* ── RTSP child monitoring ── */
    if (d->rtsp_pid > 0) {
        int status;
        pid_t ret = waitpid(d->rtsp_pid, &status, WNOHANG);
        if (ret == d->rtsp_pid || ret < 0) {
            /* child exited: resume LVGL rendering */
            d->rtsp_pid = 0;
            g_fb_paused = 0;
            /* Force full screen redraw */
            lv_obj_invalidate(lv_scr_act());
            printf("[RTSP] playback ended, LVGL resumed\n");
        } else {
            /* Still playing: power key or K5 stops it */
            int stop = 0;
            if (d->power_fd >= 0 && drain_event_key(d->power_fd, KEY_POWER))
                stop = 1;
            if (identify_ch0_btn(read_adc(0)) == 4)  /* K5 */
                stop = 1;
            if (stop) {
                system("killall rtsp_client 2>/dev/null");
                printf("[RTSP] stopping playback\n");
            }
            return;  /* skip normal dashboard updates while playing */
        }
    }

    /* ── power button: toggle screen (event1 = rk816_pwrkey) ── */
    if (d->power_fd >= 0 &&
        lv_tick_get() - d->power_startup_ms > POWER_GRACE_MS) {
        if (drain_event_key(d->power_fd, KEY_POWER))
            toggle_screen(d);
    } else if (d->power_fd >= 0) {
        /* Within grace period: drain without acting */
        drain_event_key(d->power_fd, KEY_POWER);
    }

    /* ── info line ── */
    if (mv > 0)
        snprintf(buf, sizeof(buf), "%d.%02dV  %s", mv / 1000, (mv % 1000) / 10, bat_st);
    else
        snprintf(buf, sizeof(buf), "--V  %s", bat_st);
    lv_label_set_text(d->lbl_info, buf);
}

/* ────────────────── WiFi selector callbacks ──────────── */

static void on_wifi_selected(const char *ssid, int security)
{
    printf("Selected WiFi: %s (security=%d)\n", ssid, security);
    
    if (security) {
        /* TODO: Show password input dialog */
        printf("WiFi requires password (not implemented yet)\n");
    } else {
        /* Connect to open network */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), 
                 "wpa_cli -i wlan0 add_network && "
                 "wpa_cli -i wlan0 set_network 0 ssid '\"%s\"' && "
                 "wpa_cli -i wlan0 set_network 0 key_mgmt NONE && "
                 "wpa_cli -i wlan0 enable_network 0 &", ssid);
        system(cmd);
        printf("Connecting to open WiFi: %s\n", ssid);
    }
}

static void on_wifi_exit(void)
{
    wifi_selector_hide(&g_wifi_sel);
    if (g_dash_ptr && g_dash_ptr->scr) {
        lv_scr_load(g_dash_ptr->scr);
    }
}

static void wifi_btn_event_cb(lv_event_t *e)
{
    dashboard_t *d = (dashboard_t *)lv_event_get_user_data(e);
    g_dash_ptr = d;
    
    if (!g_wifi_sel_inited) {
        wifi_selector_init(&g_wifi_sel);
        wifi_selector_set_callbacks(&g_wifi_sel, on_wifi_selected, on_wifi_exit);
        g_wifi_sel_inited = 1;
    }
    
    wifi_selector_show(&g_wifi_sel);
}

/* ────────────────────── Menu system functions ──────────── */

static void update_menu_highlight(void)
{
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        if (i == g_menu_selected) {
            lv_obj_set_style_bg_color(g_menu_items[i], C_ACCENT, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(g_menu_items[i], 0), 
                                        lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(g_menu_items[i], C_PANEL, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(g_menu_items[i], 0), 
                                        C_TEXT, 0);
        }
    }
}

static void create_menu(lv_obj_t *parent)
{
    if (g_menu_panel) return;  /* already created */
    
    /* Menu panel at bottom of visible screen area */
    g_menu_panel = lv_obj_create(parent);
    lv_obj_set_size(g_menu_panel, SCR_W, MENU_HEIGHT);
    lv_obj_set_pos(g_menu_panel, 0, VISIBLE_H - MENU_HEIGHT);  /* Use VISIBLE_H (220) instead of SCR_H */
    lv_obj_set_style_bg_color(g_menu_panel, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(g_menu_panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_menu_panel, 0, 0);
    lv_obj_set_style_border_width(g_menu_panel, 1, 0);
    lv_obj_set_style_border_color(g_menu_panel, C_ACCENT, 0);
    lv_obj_set_style_border_side(g_menu_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(g_menu_panel, 4, 0);
    lv_obj_set_flex_flow(g_menu_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_menu_panel, LV_FLEX_ALIGN_SPACE_EVENLY, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    /* Create menu items */
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        g_menu_items[i] = lv_obj_create(g_menu_panel);
        lv_obj_set_size(g_menu_items[i], MENU_ITEM_W, MENU_ITEM_H);
        lv_obj_set_style_bg_color(g_menu_items[i], C_PANEL, 0);
        lv_obj_set_style_radius(g_menu_items[i], 4, 0);
        lv_obj_set_style_border_width(g_menu_items[i], 1, 0);
        lv_obj_set_style_border_color(g_menu_items[i], C_MUTED, 0);
        lv_obj_set_style_pad_all(g_menu_items[i], 0, 0);
        lv_obj_clear_flag(g_menu_items[i], LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t *lbl = lv_label_create(g_menu_items[i]);
        lv_label_set_text(lbl, menu_labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, C_TEXT, 0);
        lv_obj_center(lbl);
    }
    
    g_menu_selected = 0;
    update_menu_highlight();
    lv_obj_add_flag(g_menu_panel, LV_OBJ_FLAG_HIDDEN);
    g_menu_visible = 0;
}

static void show_menu(void)
{
    if (!g_menu_panel) return;
    lv_obj_clear_flag(g_menu_panel, LV_OBJ_FLAG_HIDDEN);
    g_menu_visible = 1;
    g_menu_selected = 0;
    update_menu_highlight();
}

static void hide_menu(void)
{
    if (!g_menu_panel) return;
    lv_obj_add_flag(g_menu_panel, LV_OBJ_FLAG_HIDDEN);
    g_menu_visible = 0;
}

static void menu_move_left(void)
{
    if (g_menu_selected > 0) {
        g_menu_selected--;
        update_menu_highlight();
    }
}

static void menu_move_right(void)
{
    if (g_menu_selected < MENU_ITEM_COUNT - 1) {
        g_menu_selected++;
        update_menu_highlight();
    }
}

static void menu_select(dashboard_t *d)
{
    hide_menu();
    g_dash_ptr = d;  /* 保持 g_dash_ptr 始终最新 */

    switch (g_menu_selected) {
    case 0:  /* WiFi */
        if (!g_wifi_sel_inited) {
            wifi_selector_init(&g_wifi_sel);
            wifi_selector_set_callbacks(&g_wifi_sel, on_wifi_selected, on_wifi_exit);
            g_wifi_sel_inited = 1;
        }
        wifi_selector_show(&g_wifi_sel);
        break;
    case 1:  /* RTSP */
        start_rtsp_player(d);
        break;
    case 2:  /* Weather */
        if (!g_weather_inited) {
            weather_screen_init(&g_weather);
            g_weather_inited = 1;
        }
        weather_screen_show(&g_weather, d->scr);
        break;
    }
}

/* 启动RTSP视频流播放
 *
 * 新架构（不退出 LVGL）：
 *   1. 暂停 LVGL framebuffer flush（g_fb_paused=1）
 *   2. fork 子进程运行 mpp_player + rtsp_client
 *   3. LVGL 主循环继续（定时器仍运行），但不写 framebuffer
 *   4. LVGL 100ms 定时器检测到子进程退出后恢复渲染
 */
static void start_rtsp_player(dashboard_t *d)
{
    const char *rtsp_url = get_rtsp_url();
    printf("[RTSP] starting mpp_player pipeline, URL=%s\n", rtsp_url);

    /* Kill any stale processes */
    system("killall mpp_player 2>/dev/null; killall preview_stream 2>/dev/null;"
           " killall rtsp_client 2>/dev/null");
    usleep(100000);

    /* Create FIFO using C syscall (mkfifo shell cmd not available on device) */
    unlink("/var/run/preview_stream_fifo");
    if (mkfifo("/var/run/preview_stream_fifo", 0666) < 0) {
        perror("[RTSP] mkfifo");
    }

    /* Fork child: runs mpp_player + rtsp_client */
    pid_t pid = fork();
    if (pid < 0) {
        printf("[RTSP] fork failed\n");
        return;
    }
    if (pid == 0) {
        /* child: launch mpp_player (replaces preview_stream) + rtsp_client */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "/mnt/udisk/product_test/mpp_player"
            " > /dev/null 2>&1 &"
            " sleep 0.5;"
            " /mnt/udisk/product_test/rtsp_client"
            " '%s'"
            " > /dev/null 2>&1;"
            " killall mpp_player 2>/dev/null",
            rtsp_url);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    /* Parent: pause LVGL framebuffer, store child PID in dash */
    d->rtsp_pid = pid;
    g_fb_paused = 1;
    printf("[RTSP] child pid=%d, FB paused. Power key to stop.\n", pid);
}

/* ═══════════════════ public init ════════════════════= */

void dashboard_init(dashboard_t *d)
{
    memset(d, 0, sizeof(*d));
    d->rotary_fd  = -1;
    d->keypad_fd  = -1;
    d->power_fd   = -1;
    d->mem_fd     = -1;
    d->gpio3_map  = MAP_FAILED;
    d->power_startup_ms = lv_tick_get();

    /* ── screen ── */
    d->scr = lv_obj_create(NULL);
    lv_obj_set_size(d->scr, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(d->scr, C_BG, 0);
    lv_obj_set_style_bg_opa(d->scr, LV_OPA_COVER, 0);
    style_no_border(d->scr);

    /* ── status bar panel ── */
    lv_obj_t *stat = lv_obj_create(d->scr);
    lv_obj_set_pos(stat, 0, 0);
    lv_obj_set_size(stat, SCR_W, STAT_H);
    lv_obj_set_style_bg_color(stat, C_PANEL, 0);
    lv_obj_set_style_bg_opa(stat, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stat, 0, 0);
    style_no_border(stat);

    d->lbl_bat = make_label(stat, &lv_font_montserrat_12, C_TEXT,
                            LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_set_pos(d->lbl_bat, 4, 2);

    d->lbl_time = make_label(stat, &lv_font_montserrat_14, C_TEXT, "--:--:--");
    lv_obj_align(d->lbl_time, LV_ALIGN_TOP_MID, 0, 1);

    d->lbl_rssi = make_label(stat, &lv_font_montserrat_12, C_TEXT,
                             LV_SYMBOL_WIFI " --");
    lv_obj_align(d->lbl_rssi, LV_ALIGN_TOP_RIGHT, -4, 2);

    /* ── joystick panels ── */
    create_joy_panel(d, &d->joy_l_area, &d->joy_l_dot, &d->joy_l_val, JOY_L_X);
    create_joy_panel(d, &d->joy_r_area, &d->joy_r_dot, &d->joy_r_val, JOY_R_X);

    lv_obj_t *tl = make_label(d->joy_l_area, &lv_font_montserrat_10, C_MUTED, "L");
    lv_obj_set_pos(tl, 3, 2);
    lv_obj_t *tr = make_label(d->joy_r_area, &lv_font_montserrat_10, C_MUTED, "R");
    lv_obj_set_pos(tr, 3, 2);

    /* ── CH0 button indicators ── */
    int btn_start_x = 4;
    int btn_spacing = 28;
    for (int i = 0; i < DASH_CH0_BTN_COUNT; i++) {
        int x = btn_start_x + i * btn_spacing;

        d->ch0_led[i] = lv_led_create(d->scr);
        lv_obj_set_pos(d->ch0_led[i], x, KEY_ROW_Y + 2);
        lv_obj_set_size(d->ch0_led[i], 12, 12);
        lv_led_set_color(d->ch0_led[i], C_GPIO_OFF);
        lv_led_off(d->ch0_led[i]);

        lv_obj_t *bl = make_label(d->scr, &lv_font_montserrat_10, C_MUTED,
                                  ch0_names[i]);
        lv_obj_set_pos(bl, x + 13, KEY_ROW_Y + 3);
    }

    d->lbl_ch0_raw = make_label(d->scr, &lv_font_montserrat_10, C_MUTED, "--");
    lv_obj_set_pos(d->lbl_ch0_raw, 150, KEY_ROW_Y + 3);

    /* ── rotary encoder: /dev/mem GPIO3 mmap ── */
    d->mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (d->mem_fd >= 0) {
        d->gpio3_map = mmap(NULL, GPIO3_MAP_SIZE, PROT_READ,
                            MAP_SHARED, d->mem_fd, GPIO3_BASE);
        if (d->gpio3_map == MAP_FAILED) {
            printf("[Dashboard] GPIO3 mmap failed\n");
            d->gpio3_map = NULL;
        } else {
            /* Read initial A/B state */
            volatile uint32_t *ext = (volatile uint32_t *)
                ((uint8_t *)d->gpio3_map + GPIO_EXT_PORT_OFF);
            uint32_t raw = *ext;
            int a = !((raw >> ROTARY_PIN_A) & 1);
            int b = !((raw >> ROTARY_PIN_B) & 1);
            d->rotary_last_ab = (a << 1) | b;
            printf("[Dashboard] GPIO3 mmap OK, initial AB=%d\n", d->rotary_last_ab);
        }
    } else {
        printf("[Dashboard] /dev/mem open failed\n");
        d->gpio3_map = NULL;
    }

    /* Create rotary display panel */
    lv_obj_t *rotary_bg = lv_obj_create(d->scr);
    lv_obj_set_pos(rotary_bg, 175, KEY_ROW_Y - 3);
    lv_obj_set_size(rotary_bg, 60, 18);
    lv_obj_set_style_bg_color(rotary_bg, C_JOY_BG, 0);
    lv_obj_set_style_bg_opa(rotary_bg, LV_OPA_70, 0);
    lv_obj_set_style_radius(rotary_bg, 8, 0);
    lv_obj_set_style_border_width(rotary_bg, 1, 0);
    lv_obj_set_style_border_color(rotary_bg, C_ACCENT, 0);
    lv_obj_set_style_pad_all(rotary_bg, 0, 0);
    lv_obj_clear_flag(rotary_bg, LV_OBJ_FLAG_SCROLLABLE);
    
    d->lbl_rotary = make_label(rotary_bg, &lv_font_montserrat_10, C_ACCENT,
                               LV_SYMBOL_LOOP "0");
    lv_obj_align(d->lbl_rotary, LV_ALIGN_CENTER, 0, 0);
    
    /* ── keypad event device (/dev/input/event2) ── */
    d->keypad_fd = open(DASH_KEYPAD_DEV, O_RDONLY | O_NONBLOCK);
    printf("[Dashboard] Keypad device %s: fd=%d\n", DASH_KEYPAD_DEV, d->keypad_fd);

    /* ── power button (event1 = rk816_pwrkey) ── */
    d->power_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    printf("[Dashboard] Power key device: fd=%d\n", d->power_fd);

    /* ── GPIO indicators ── */
    int gpio_start_x = 12;
    int gpio_spacing = 50;
    for (int i = 0; i < DASH_GPIO_COUNT; i++) {
        int x = gpio_start_x + i * gpio_spacing;

        d->gpio_led[i] = lv_led_create(d->scr);
        lv_obj_set_pos(d->gpio_led[i], x, GPIO_Y);
        lv_obj_set_size(d->gpio_led[i], GPIO_SZ, GPIO_SZ);
        lv_led_set_color(d->gpio_led[i], C_GPIO_OFF);
        lv_led_off(d->gpio_led[i]);

        lv_obj_t *gl = make_label(d->scr, &lv_font_montserrat_10, C_MUTED,
                                  gpio_tags[i]);
        lv_obj_set_pos(gl, x - 1, GPIO_Y + GPIO_SZ + 2);
    }

    /* ── info bar ── */
    lv_obj_t *info_bg = lv_obj_create(d->scr);
    lv_obj_set_pos(info_bg, 0, INFO_Y);
    lv_obj_set_size(info_bg, SCR_W, VISIBLE_H - INFO_Y);
    lv_obj_set_style_bg_color(info_bg, C_PANEL, 0);
    lv_obj_set_style_bg_opa(info_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_bg, 0, 0);
    style_no_border(info_bg);

    d->lbl_info = make_label(info_bg, &lv_font_montserrat_14, C_TEXT, "---");
    lv_obj_align(d->lbl_info, LV_ALIGN_LEFT_MID, 8, 0);

    /* ── Create menu panel (hidden by default) ── */
    create_menu(d->scr);

    /* ── load screen & start timers ── */
    lv_scr_load(d->scr);
    d->tmr = lv_timer_create(dash_timer_cb, DASH_UPDATE_MS, d);
    /* Dedicated 10ms encoder timer for responsive rotary detection */
    lv_timer_create(encoder_timer_cb, 10, d);

    /* 开机自动连接已保存的 WiFi */
    wifi_autoconnect_start();
}
