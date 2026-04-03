/*
 * weather.c - Weather display screen
 *
 * Uses raw TCP socket HTTP GET to wttr.in (port 80, no SSL needed)
 * Forks a child process for the request to avoid blocking LVGL main thread
 * LVGL 500ms timer polls result file and updates display
 *
 * Response format: City|Temp|Humidity|Wind|Condition
 * e.g.: Shanghai|+23°C|65%|↑ 10 km/h|Partly cloudy
 */

#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

/* ── colours (match dashboard.c) ── */
#define W_C_BG      lv_color_hex(0x1a1a2e)
#define W_C_ACCENT  lv_color_hex(0x00b4d8)
#define W_C_TEXT    lv_color_hex(0xe0e0e0)
#define W_C_MUTED   lv_color_hex(0x607080)
#define W_C_PANEL   lv_color_hex(0x16213e)
#define W_C_WARN    lv_color_hex(0xff9800)

#define W_ADC_FMT       "/sys/devices/1038c000.adc/iio:device0/in_voltage%d_raw"
#define WEATHER_FILE    "/tmp/weather.txt"
#define WEATHER_CITY_FILE "/mnt/udisk/config/weather_city.txt"
#define FETCH_TIMEOUT_MS  25000
#define AUTO_REFRESH_MS   600000   /* 10 minutes */

/* ── Read ADC channel ── */
static int weather_read_adc(int ch)
{
    char path[128];
    snprintf(path, sizeof(path), W_ADC_FMT, ch);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int val = -1;
    fscanf(f, "%d", &val);
    fclose(f);
    return val;
}

/* ── Child process: HTTP GET → write result to WEATHER_FILE ── */
static void fetch_child(void)
{
    /* Read city: first from geo-sync, then config file, then default */
    char city[64] = "London";
    FILE *cf = fopen("/tmp/ip_city.txt", "r");
    if (!cf) cf = fopen(WEATHER_CITY_FILE, "r");
    if (cf) {
        if (fgets(city, sizeof(city), cf)) {
            int cl = (int)strlen(city);
            while (cl > 0 && (city[cl-1] == '\n' || city[cl-1] == '\r'
                              || city[cl-1] == ' '))
                city[--cl] = '\0';
        }
        fclose(cf);
    }

    struct hostent *he = gethostbyname("wttr.in");
    if (!he) {
        FILE *f = fopen(WEATHER_FILE, "w");
        if (f) { fprintf(f, "ERR:DNS\n"); fclose(f); }
        _exit(1);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { _exit(1); }

    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(80);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        FILE *f = fopen(WEATHER_FILE, "w");
        if (f) { fprintf(f, "ERR:Connect\n"); fclose(f); }
        _exit(1);
    }

    /* Build request: GET /<city>?format=%l|%t|%h|%w|%C */
    char req[512];
    snprintf(req, sizeof(req),
        "GET /%s?format=%%25l%%7C%%25t%%7C%%25h%%7C%%25w%%7C%%25C HTTP/1.0\r\n"
        "Host: wttr.in\r\n"
        "User-Agent: rv1108-weather/1.0\r\n"
        "Connection: close\r\n"
        "\r\n", city);
    write(fd, req, strlen(req));

    char resp[4096];
    int total = 0, n;
    while (total < (int)sizeof(resp) - 1) {
        n = read(fd, resp + total, sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    resp[total] = '\0';

    /* Skip HTTP headers (find blank line) */
    char *body = strstr(resp, "\r\n\r\n");
    if (body) body += 4;
    else {
        body = strstr(resp, "\n\n");
        if (body) body += 2;
        else body = resp;
    }

    /* Trim whitespace */
    while (*body == ' ' || *body == '\n' || *body == '\r') body++;
    int blen = (int)strlen(body);
    while (blen > 0 && (body[blen-1] == '\n' || body[blen-1] == '\r' ||
                        body[blen-1] == ' '))
        body[--blen] = '\0';

    FILE *f = fopen(WEATHER_FILE, "w");
    if (f) { fprintf(f, "%s\n", body); fclose(f); }
    _exit(0);
}

/* ── Start background fetch (fork child) ── */
static void start_fetch(weather_screen_t *ws)
{
    unlink(WEATHER_FILE);
    ws->fetch_done    = 0;
    ws->fetch_start_ms = lv_tick_get();

    lv_label_set_text(ws->lbl_status,    LV_SYMBOL_REFRESH " Fetching...");
    lv_label_set_text(ws->lbl_city,      "--");
    lv_label_set_text(ws->lbl_temp,      "-- °C");
    lv_label_set_text(ws->lbl_condition, "--");
    lv_label_set_text(ws->lbl_humidity,  "Humidity: --%");
    lv_label_set_text(ws->lbl_wind,      "Wind: --");

    pid_t pid = fork();
    if (pid == 0) fetch_child();   /* never returns */
}

/* ── Parse result file and update labels ── */
static void parse_and_display(weather_screen_t *ws)
{
    FILE *f = fopen(WEATHER_FILE, "r");
    if (!f) return;
    char line[256] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);

    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' '))
        line[--len] = '\0';

    if (strncmp(line, "ERR:", 4) == 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING " %s. K4:Retry", line + 4);
        lv_label_set_text(ws->lbl_status, msg);
        ws->fetch_done = 1;
        return;
    }
    /* Detect server-side errors (e.g. "render failed: ...") */
    if (strstr(line, "render failed") || strstr(line, "Unknown location")
        || strstr(line, "ERROR")) {
        lv_label_set_text(ws->lbl_status,
                          LV_SYMBOL_WARNING " Server err. K4:Retry");
        ws->fetch_done = 1;
        return;
    }
    if (len == 0) {
        lv_label_set_text(ws->lbl_status,
                          LV_SYMBOL_WARNING " No data. K4:Retry");
        ws->fetch_done = 1;
        return;
    }

    /* Split by '|': City|Temp|Humidity|Wind|Condition */
    char *fields[5] = {NULL};
    int nf = 0;
    char *p = line;
    fields[nf++] = p;
    while (nf < 5 && (p = strchr(p, '|')) != NULL) {
        *p++ = '\0';
        fields[nf++] = p;
    }

    char buf[128];
    if (nf >= 1) lv_label_set_text(ws->lbl_city,      fields[0]);
    if (nf >= 2) lv_label_set_text(ws->lbl_temp,      fields[1]);
    if (nf >= 3) {
        snprintf(buf, sizeof(buf), "Humidity: %s", fields[2]);
        lv_label_set_text(ws->lbl_humidity, buf);
    }
    if (nf >= 4) {
        /* Strip non-ASCII (wind direction arrows are Unicode, not in font) */
        char wind_clean[64] = {0};
        int wi = 0;
        for (const char *cp = fields[3]; *cp && wi < 62; cp++) {
            if ((unsigned char)*cp < 128) wind_clean[wi++] = *cp;
        }
        wind_clean[wi] = '\0';
        snprintf(buf, sizeof(buf), "Wind: %s", wind_clean);
        lv_label_set_text(ws->lbl_wind, buf);
    }
    if (nf >= 5) lv_label_set_text(ws->lbl_condition, fields[4]);

    lv_label_set_text(ws->lbl_status, "");
    ws->fetch_done = 1;
    ws->last_fetch_ok_ms = lv_tick_get();
}

/* ── 500ms LVGL timer callback ── */
static void weather_poll_cb(lv_timer_t *tmr)
{
    weather_screen_t *ws = (weather_screen_t *)tmr->user_data;
    if (!ws || !ws->scr) return;

    int ch0 = weather_read_adc(0);
    int k3  = (ch0 > 173 && ch0 <= 245);
    int k4  = (ch0 > 245 && ch0 <= 358);
    int k5  = (ch0 > 358 && ch0 <= 700);

    if ((k3 && !ws->last_k3) || (k5 && !ws->last_k5)) {
        weather_screen_hide(ws);
        return;
    }
    ws->last_k3 = k3;
    ws->last_k5 = k5;

    if (k4 && !ws->last_k4) start_fetch(ws);
    ws->last_k4 = k4;

    /* Auto-refresh every 10 minutes */
    if (ws->fetch_done && ws->last_fetch_ok_ms > 0 &&
        (lv_tick_get() - ws->last_fetch_ok_ms) > AUTO_REFRESH_MS) {
        start_fetch(ws);
    }

    if (!ws->fetch_done) {
        struct stat st;
        if (stat(WEATHER_FILE, &st) == 0 && st.st_size > 0) {
            parse_and_display(ws);
        } else if (lv_tick_get() - ws->fetch_start_ms > FETCH_TIMEOUT_MS) {
            lv_label_set_text(ws->lbl_status,
                              LV_SYMBOL_WARNING " Timeout. K4:Retry");
            ws->fetch_done = 1;
        }
    }
}

/* ════════════════════ public API ════════════════════ */

void weather_screen_init(weather_screen_t *ws)
{
    memset(ws, 0, sizeof(*ws));
}

void weather_screen_show(weather_screen_t *ws, lv_obj_t *back_scr)
{
    if (!ws) return;
    ws->back_scr = back_scr;
    ws->last_k3  = 0;
    ws->last_k4  = 0;

    /* Screen */
    ws->scr = lv_obj_create(NULL);
    lv_obj_set_size(ws->scr, 240, 240);
    lv_obj_set_style_bg_color(ws->scr, W_C_BG, 0);
    lv_obj_set_style_bg_opa(ws->scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ws->scr, 0, 0);
    lv_obj_clear_flag(ws->scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Title bar */
    lv_obj_t *bar = lv_obj_create(ws->scr);
    lv_obj_set_size(bar, 240, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, W_C_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_IMAGE "  Weather");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, W_C_ACCENT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    /* City */
    ws->lbl_city = lv_label_create(ws->scr);
    lv_label_set_text(ws->lbl_city, "--");
    lv_obj_set_style_text_font(ws->lbl_city, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ws->lbl_city, W_C_TEXT, 0);
    lv_obj_set_pos(ws->lbl_city, 12, 30);

    /* Temperature */
    ws->lbl_temp = lv_label_create(ws->scr);
    lv_label_set_text(ws->lbl_temp, "-- °C");
    lv_obj_set_style_text_font(ws->lbl_temp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ws->lbl_temp, W_C_ACCENT, 0);
    lv_obj_set_pos(ws->lbl_temp, 12, 55);

    /* Condition */
    ws->lbl_condition = lv_label_create(ws->scr);
    lv_label_set_text(ws->lbl_condition, "--");
    lv_obj_set_style_text_font(ws->lbl_condition, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ws->lbl_condition, W_C_TEXT, 0);
    lv_obj_set_pos(ws->lbl_condition, 12, 88);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(ws->scr);
    lv_obj_set_size(sep, 216, 1);
    lv_obj_set_pos(sep, 12, 108);
    lv_obj_set_style_bg_color(sep, W_C_MUTED, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* Humidity */
    ws->lbl_humidity = lv_label_create(ws->scr);
    lv_label_set_text(ws->lbl_humidity, "Humidity: --%");
    lv_obj_set_style_text_font(ws->lbl_humidity, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ws->lbl_humidity, W_C_TEXT, 0);
    lv_obj_set_pos(ws->lbl_humidity, 12, 118);

    /* Wind */
    ws->lbl_wind = lv_label_create(ws->scr);
    lv_label_set_text(ws->lbl_wind, "Wind: --");
    lv_obj_set_style_text_font(ws->lbl_wind, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ws->lbl_wind, W_C_TEXT, 0);
    lv_obj_set_pos(ws->lbl_wind, 12, 140);

    /* Status */
    ws->lbl_status = lv_label_create(ws->scr);
    lv_label_set_text(ws->lbl_status, "");
    lv_obj_set_style_text_font(ws->lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ws->lbl_status, W_C_WARN, 0);
    lv_obj_set_pos(ws->lbl_status, 12, 165);

    /* Hint bar */
    lv_obj_t *hint_bar = lv_obj_create(ws->scr);
    lv_obj_set_size(hint_bar, 240, 22);
    lv_obj_set_pos(hint_bar, 0, 198);
    lv_obj_set_style_bg_color(hint_bar, W_C_PANEL, 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hint_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(hint_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(hint_bar, W_C_MUTED, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hint_bar, 0, 0);
    lv_obj_clear_flag(hint_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hint = lv_label_create(hint_bar);
    lv_label_set_text(hint, "K3/K5:Back   K4:Refresh");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, W_C_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    lv_scr_load(ws->scr);
    ws->tmr = lv_timer_create(weather_poll_cb, 200, ws);
    start_fetch(ws);
}

void weather_screen_hide(weather_screen_t *ws)
{
    if (!ws || !ws->scr) return;
    if (ws->tmr) { lv_timer_del(ws->tmr); ws->tmr = NULL; }
    if (ws->back_scr) lv_scr_load(ws->back_scr);
    lv_obj_del(ws->scr);
    ws->scr = NULL;
}
