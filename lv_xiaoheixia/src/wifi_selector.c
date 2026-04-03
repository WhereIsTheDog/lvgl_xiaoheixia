/*
 * wifi_selector.c - WiFi选择界面实现
 */

#include "wifi_selector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>

/* ────────────────────── 颜色主题 ────────────────────── */
#define C_BG          lv_color_hex(0x1a1a2e)
#define C_PANEL       lv_color_hex(0x16213e)
#define C_ITEM_BG     lv_color_hex(0x0f1b30)
#define C_ITEM_SEL    lv_color_hex(0x2a4a7c)
#define C_ITEM_CONN   lv_color_hex(0x1b4332)
#define C_TEXT        lv_color_hex(0xe0e0e0)
#define C_MUTED       lv_color_hex(0x778899)
#define C_ACCENT      lv_color_hex(0x00b4d8)
#define C_GOOD_RSSI   lv_color_hex(0x00e676)
#define C_MED_RSSI    lv_color_hex(0xffd600)
#define C_WEAK_RSSI   lv_color_hex(0xff5252)

/* ────────────────────── 布局常量 ────────────────────── */
#define SCR_W         240
#define SCR_H         240
#define VISIBLE_H     220   /* 实际可见高度，底部20px被遮住 */

#define TITLE_H       22
#define HINT_H        32    /* 减小提示栏高度 */
#define LIST_Y        TITLE_H
#define LIST_H        (VISIBLE_H - TITLE_H - HINT_H)  /* 使用可见高度 */

#define ITEM_H        30
#define ITEM_MARGIN   2

#define ROTARY_DEV    "/dev/input/event1"

/* 密码输入可用字符集 - 重新组织为小键盘布局 */
/* 10列布局，适配220px屏幕宽度 */
static const char* KEYBOARD_LAYOUT_LOWER[6][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", ";"},
    {"z", "x", "c", "v", "b", "n", "m", ",", ".", "?"},
    {"@", "#", "$", "%", "&", "*", "(", ")", "-", "_"},
    {"Aa", " ", " ", " ", " ", " ", " ", "Del", "Del", "OK"}
};

static const char* KEYBOARD_LAYOUT_UPPER[6][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L", ":"},
    {"Z", "X", "C", "V", "B", "N", "M", "<", ">", "!"},
    {"@", "#", "$", "%", "&", "*", "(", ")", "-", "_"},
    {"Aa", " ", " ", " ", " ", " ", " ", "Del", "Del", "OK"}
};

#define KBD_ROWS 6
#define KBD_COLS 10
#define KBD_BUTTON_W 21
#define KBD_BUTTON_H 17
#define KBD_MARGIN   1

/* 大小写状态 */
static int g_kbd_uppercase = 0;

/* ────────────────────── 辅助函数 ────────────────────── */

static void style_no_border(lv_obj_t *obj)
{
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

/* 读取旋转编码器按钮 (JB, joybtn-key, code=0x09 on /dev/input/event2) */
static int detect_jb_press(int fd)
{
    if (fd < 0)
        return 0;
    struct input_event ev;
    int got_press = 0;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY && ev.code == 0x09 && ev.value == 1)
            got_press = 1;
    }
    return got_press;
}

/* 读取ADC通道 */
static int read_adc_channel(int ch)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/1038c000.adc/iio:device0/in_voltage%d_raw", ch);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int val = -1;
    if (fscanf(f, "%d", &val) != 1)
        val = -1;
    fclose(f);
    return val;
}

/* 检测操纵杆移动 (使用左操纵杆: Ch4=X, Ch5=Y) */
/* 方向已反转以匹配实际硬件布局 */
static void detect_joystick_movement(int *up, int *down, int *left, int *right)
{
    static int last_x = 512, last_y = 512;  /* 中心位置 */
    const int threshold = 200;  /* 移动阈值 */
    const int center = 512;     /* ADC中心值 (1024/2) */
    
    int x = read_adc_channel(4);  /* 左操纵杆X轴 */
    int y = read_adc_channel(5);  /* 左操纵杆Y轴 */
    
    /* 默认无移动 */
    *up = *down = *left = *right = 0;
    
    if (x < 0 || y < 0) return;  /* ADC读取失败 */
    
    /* X轴移动检测 (反转: 原left变right, 原right变left) */
    if (x < center - threshold && last_x >= center - threshold) {
        *right = 1;  /* 反转 */
    } else if (x > center + threshold && last_x <= center + threshold) {
        *left = 1;   /* 反转 */
    }
    
    /* Y轴移动检测 (反转: 原up变down, 原down变up) */
    if (y < center - threshold && last_y >= center - threshold) {
        *down = 1;   /* 反转 */
    } else if (y > center + threshold && last_y <= center + threshold) {
        *up = 1;     /* 反转 */
    }
    
    last_x = x;
    last_y = y;
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

/* 从/dev/input/event1读取波轮增量 */
static int drain_rotary(int fd)
{
    if (fd < 0)
        return 0;
    int total = 0;
    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_REL && ev.code == REL_X)
            total += ev.value;
    }
    return total;
}

/* 读取ADC Channel 0 (K1-K5按键) */
static int read_adc_ch0(void)
{
    FILE *f = fopen("/sys/devices/1038c000.adc/iio:device0/in_voltage0_raw", "r");
    if (!f)
        return -1;
    int val = -1;
    if (fscanf(f, "%d", &val) != 1)
        val = -1;
    fclose(f);
    return val;
}

/* 检测K1 (ADC ≤ 55), K3 (≤ 245), K4 (≤ 358), K5 (≤ 700) */
static int detect_key_press(void)
{
    int adc = read_adc_ch0();
    if (adc < 0)
        return 0;
    if (adc <= 55)
        return 1;  /* K1 */
    if (adc > 173 && adc <= 245)
        return 3;  /* K3 - 扫描刷新 */
    if (adc > 245 && adc <= 358)
        return 4;  /* K4 - 选择WiFi */
    if (adc > 358 && adc <= 700)
        return 5;  /* K5 - 返回 */
    return 0;
}

/* ────────────── 密码持久化存储 ────────────── */

/* 格式: 每行一个条目 "SSID\tPSK\n"，保存在可写分区 */
#define WIFI_PWD_STORE_PATH "/mnt/udisk/product_test/wifi_passwords.txt"

/* 保存 WiFi 密码（同 SSID 覆盖旧条目） */
static void save_wifi_password(const char *ssid, const char *psk)
{
    char lines[20][WIFI_SSID_MAX_LEN + WIFI_PWD_MAX_LEN + 4];
    int nlines = 0, found = 0, i;

    FILE *f = fopen(WIFI_PWD_STORE_PATH, "r");
    if (f) {
        char line[WIFI_SSID_MAX_LEN + WIFI_PWD_MAX_LEN + 4];
        while (nlines < 20 && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            if (strcmp(line, ssid) == 0) {
                snprintf(lines[nlines], sizeof(lines[nlines]), "%s\t%s", ssid, psk);
                found = 1;
            } else {
                snprintf(lines[nlines], sizeof(lines[nlines]), "%s\t%s", line, tab + 1);
            }
            nlines++;
        }
        fclose(f);
    }

    if (!found && nlines < 20) {
        snprintf(lines[nlines], sizeof(lines[nlines]), "%s\t%s", ssid, psk);
        nlines++;
    }

    f = fopen(WIFI_PWD_STORE_PATH, "w");
    if (f) {
        for (i = 0; i < nlines; i++)
            fprintf(f, "%s\n", lines[i]);
        fclose(f);
        printf("WiFi password saved for SSID: [%s]\n", ssid);
    } else {
        printf("WARNING: failed to save WiFi password to %s\n", WIFI_PWD_STORE_PATH);
    }
}

/* 加载已保存的 WiFi 密码，返回 1=找到, 0=未找到 */
static int load_wifi_password(const char *ssid, char *psk_out, int maxlen)
{
    FILE *f = fopen(WIFI_PWD_STORE_PATH, "r");
    if (!f) return 0;

    char line[WIFI_SSID_MAX_LEN + WIFI_PWD_MAX_LEN + 4];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcmp(line, ssid) == 0) {
            strncpy(psk_out, tab + 1, maxlen - 1);
            psk_out[maxlen - 1] = 0;
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* 从持久文件中删除某个 SSID 的密码 */
static void forget_wifi_password(const char *ssid)
{
    char lines[20][WIFI_SSID_MAX_LEN + WIFI_PWD_MAX_LEN + 4];
    int nlines = 0, i;

    FILE *f = fopen(WIFI_PWD_STORE_PATH, "r");
    if (!f) return;
    char line[WIFI_SSID_MAX_LEN + WIFI_PWD_MAX_LEN + 4];
    while (nlines < 20 && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcmp(line, ssid) == 0) continue;  /* 跳过要删除的条目 */
        snprintf(lines[nlines], sizeof(lines[nlines]), "%s\t%s", line, tab + 1);
        nlines++;
    }
    fclose(f);

    f = fopen(WIFI_PWD_STORE_PATH, "w");
    if (f) {
        for (i = 0; i < nlines; i++)
            fprintf(f, "%s\n", lines[i]);
        fclose(f);
        printf("WiFi password forgotten for SSID: [%s]\n", ssid);
    }
}

/* ═══════════════ IP geolocation + time sync ═══════════════
 * After WiFi connects, fork a child to:
 *   1. HTTP GET ip-api.com → city + UTC offset
 *   2. Parse HTTP Date header → current UTC time
 *   3. Set system clock to local time (UTC + offset)
 *   4. Write city to /tmp/ip_city.txt for weather to use
 * Runs in child process — never blocks LVGL main thread. */

#define GEO_CITY_FILE "/tmp/ip_city.txt"

static int month_from_name(const char *m) {
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++)
        if (strncmp(m, months[i], 3) == 0) return i;
    return -1;
}

static time_t tm_to_epoch_utc(int year, int mon, int day,
                               int hour, int min, int sec)
{
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    time_t days = 0;
    for (int y = 1970; y < year; y++)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int m = 0; m < mon; m++) {
        days += mdays[m];
        if (m == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
            days++;
    }
    days += day - 1;
    return days * 86400 + hour * 3600 + min * 60 + sec;
}

static void geo_time_sync_child(void)
{
    struct hostent *he = gethostbyname("ip-api.com");
    if (!he) _exit(1);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) _exit(1);

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
        _exit(1);
    }

    const char *req =
        "GET /line/?fields=city,offset HTTP/1.0\r\n"
        "Host: ip-api.com\r\n"
        "User-Agent: rv1108/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";
    write(fd, req, strlen(req));

    char resp[2048];
    int total = 0, n;
    while (total < (int)sizeof(resp) - 1) {
        n = read(fd, resp + total, sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    resp[total] = '\0';

    /* Parse HTTP Date header: "Date: Mon, 31 Mar 2026 00:15:30 GMT" */
    time_t utc_epoch = 0;
    char *date_hdr = strstr(resp, "Date: ");
    if (date_hdr) {
        int day, year, hour, min, sec;
        char mon_str[4] = {0};
        if (sscanf(date_hdr, "Date: %*[^,], %d %3s %d %d:%d:%d",
                   &day, mon_str, &year, &hour, &min, &sec) == 6) {
            int mon = month_from_name(mon_str);
            if (mon >= 0)
                utc_epoch = tm_to_epoch_utc(year, mon, day, hour, min, sec);
        }
    }

    /* Parse body: line 1 = city, line 2 = offset (seconds from UTC) */
    char *body = strstr(resp, "\r\n\r\n");
    if (!body) { body = strstr(resp, "\n\n"); if (body) body += 2; }
    else body += 4;
    if (!body) _exit(1);

    char city[64] = {0};
    int offset = 0;
    char *nl = strchr(body, '\n');
    if (nl) {
        int clen = (int)(nl - body);
        if (clen > 0 && clen < (int)sizeof(city)) {
            memcpy(city, body, clen);
            city[clen] = '\0';
            /* Trim CR */
            while (clen > 0 && city[clen-1] == '\r') city[--clen] = '\0';
        }
        offset = atoi(nl + 1);
    }

    /* Write city for weather to use */
    if (city[0]) {
        FILE *f = fopen(GEO_CITY_FILE, "w");
        if (f) { fprintf(f, "%s\n", city); fclose(f); }
    }

    /* Set system clock to local time (UTC + offset).
     * localtime() without TZ set treats system time as UTC,
     * so setting clock to UTC+offset makes display show local time. */
    if (utc_epoch > 0) {
        struct timeval stv;
        stv.tv_sec  = utc_epoch + offset;
        stv.tv_usec = 0;
        settimeofday(&stv, NULL);

        /* 立即保存同步后的时间，供下次开机恢复 */
        FILE *tf = fopen("/mnt/udisk/last_time.txt", "w");
        if (tf) {
            fprintf(tf, "%ld\n", (long)stv.tv_sec);
            fclose(tf);
        }
    }

    _exit(0);
}

static void start_geo_time_sync(void)
{
    pid_t pid = fork();
    if (pid == 0) geo_time_sync_child();
    /* Parent: fire-and-forget, child sets clock & writes file */
}

/* ═══════════════ 开机自动 WiFi 连接 ═══════════════
 * 启动后台脚本：扫描可见 AP → 与已保存密码文件匹配 →
 * 按信号强度从强到弱逐一尝试 → 成功后触发 geo/time sync。
 * 密码失败时不删除密码文件（区别于手动连接），直接切换下一个。
 * 状态写入 /tmp/wifi_autoconn_status.txt:
 *   TRYING       脚本运行中
 *   CONNECTED:X  成功连接 SSID=X
 *   IDLE         无匹配网络或全部失败
 * ══════════════════════════════════════════════════ */

static int      s_autoconn_active   = 0;
static uint32_t s_autoconn_start_ms = 0;

void wifi_autoconnect_start(void)
{
    /* 如果没有保存的密码，跳过 */
    if (access(WIFI_PWD_STORE_PATH, F_OK) != 0) return;

    FILE *f = fopen("/tmp/wifi_autoconn.sh", "w");
    if (!f) return;

    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "PWDFILE=%s\n", WIFI_PWD_STORE_PATH);
    fprintf(f, "[ -f \"$PWDFILE\" ] || { echo IDLE > /tmp/wifi_autoconn_status.txt; exit 0; }\n");
    fprintf(f, "echo TRYING > /tmp/wifi_autoconn_status.txt\n");

    /* 等待 3 秒，让系统完全启动后再连接（仅开机时执行一次） */
    fprintf(f, "sleep 3\n");

    /* 配置 loopback，避免 ADB 冻结 */
    fprintf(f, "ifconfig lo 127.0.0.1 netmask 255.0.0.0 up 2>/dev/null\n");
    fprintf(f, "kill $(pidof wpa_cli) 2>/dev/null\n");
    fprintf(f, "kill $(pidof update_wifi_status) 2>/dev/null\n");
    fprintf(f, "sleep 1\n");

    /* 扫描 — 使用 bss 枚举（scan_results 在本设备返回空） */
    fprintf(f, "/usr/local/sbin/wpa_cli -i wlan0 scan > /dev/null 2>&1\n");
    fprintf(f, "sleep 4\n");

    /* 直接枚举 BSS → 匹配密码文件 → 写入 sorted 文件（避免 awk -F'\\t' 兼容问题） */
    fprintf(f, "> /tmp/ac_sorted.txt\n");
    fprintf(f, "for idx in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n");
    fprintf(f, "  BSS=$(/usr/local/sbin/wpa_cli -i wlan0 bss $idx 2>/dev/null)\n");
    fprintf(f, "  AP_SSID=$(echo \"$BSS\" | grep '^ssid=' | sed 's/ssid=//')\n");
    fprintf(f, "  [ -z \"$AP_SSID\" ] && break\n");
    fprintf(f, "  LVL=$(echo \"$BSS\" | grep '^level=' | sed 's/level=//')\n");
    /* 用 grep 在密码文件中查找匹配的 SSID 行，sed 提取密码 */
    fprintf(f, "  MATCH=$(grep -F \"$AP_SSID\" \"$PWDFILE\" | sed -n \"1p\")\n");
    fprintf(f, "  [ -z \"$MATCH\" ] && continue\n");
    /* 用 sed 删除 SSID 和第一个 Tab，剩下的就是密码 */
    fprintf(f, "  AP_PSK=$(echo \"$MATCH\" | sed \"s/^[^\\t]*\\t//\")\n");
    fprintf(f, "  [ -z \"$AP_PSK\" ] && continue\n");
    fprintf(f, "  printf \"%%s\\t%%s\\t%%s\\n\" \"$LVL\" \"$AP_SSID\" \"$AP_PSK\" >> /tmp/ac_sorted.txt\n");
    fprintf(f, "done\n");
    fprintf(f, "sort -rn /tmp/ac_sorted.txt -o /tmp/ac_sorted.txt 2>/dev/null\n");

    fprintf(f, "N=$(grep -c '' /tmp/ac_sorted.txt 2>/dev/null)\n");
    fprintf(f, "[ -z \"$N\" ] || [ \"$N\" = \"0\" ] && { echo IDLE > /tmp/wifi_autoconn_status.txt; exit 0; }\n");

    /* 逐一尝试，从信号最强的开始 */
    fprintf(f, "i=1\n");
    fprintf(f, "while [ \"$i\" -le \"$N\" ]; do\n");
    fprintf(f, "  LINE=$(sed -n \"${i}p\" /tmp/ac_sorted.txt)\n");
    /* 用 sed 提取第2和第3个 Tab 分隔字段 */
    fprintf(f, "  SSID=$(echo \"$LINE\" | sed 's/^[^\\t]*\\t//;s/\\t.*//')\n");
    fprintf(f, "  PSK=$(echo \"$LINE\" | sed 's/^[^\\t]*\\t[^\\t]*\\t//')\n");
    fprintf(f, "  [ -z \"$SSID\" ] && { i=$((i+1)); continue; }\n");

    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 remove_network 0 2>/dev/null\n");
    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 remove_network 1 2>/dev/null\n");
    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 remove_network 2 2>/dev/null\n");
    fprintf(f, "  NET=$(/usr/local/sbin/wpa_cli -i wlan0 add_network)\n");
    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 set_network \"$NET\" ssid \"\\\"$SSID\\\"\"\n");
    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 set_network \"$NET\" psk \"\\\"$PSK\\\"\"\n");
    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 select_network \"$NET\"\n");
    fprintf(f, "  /usr/local/sbin/wpa_cli -i wlan0 enable_network \"$NET\"\n");

    /* 等待连接，最多 15 秒 */
    fprintf(f, "  CONN=0\n");
    fprintf(f, "  for j in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n");
    fprintf(f, "    STATE=$(/usr/local/sbin/wpa_cli -i wlan0 status 2>/dev/null"
               " | grep wpa_state | sed 's/wpa_state=//')\n");
    fprintf(f, "    if [ \"$STATE\" = \"COMPLETED\" ]; then\n");
    fprintf(f, "      killall udhcpc 2>/dev/null\n");
    fprintf(f, "      udhcpc -i wlan0 -t 8 -T 3 -n -q > /dev/null 2>&1\n");
    fprintf(f, "      IP=$(ifconfig wlan0 2>/dev/null | grep 'inet addr'"
               " | sed 's/.*inet addr://;s/ .*//')\n");
    fprintf(f, "      if [ -n \"$IP\" ]; then\n");
    fprintf(f, "        echo \"CONNECTED:$SSID\" > /tmp/wifi_autoconn_status.txt\n");
    fprintf(f, "        echo \"$SSID\" > /tmp/wifi_connected_ssid.txt\n");
    fprintf(f, "        CONN=1\n");
    fprintf(f, "      fi\n");
    fprintf(f, "      break\n");
    fprintf(f, "    fi\n");
    fprintf(f, "    sleep 1\n");
    fprintf(f, "  done\n");
    fprintf(f, "  [ \"$CONN\" = \"1\" ] && break\n");
    fprintf(f, "  i=$((i+1))\n");
    fprintf(f, "done\n");

    fprintf(f, "grep -q '^CONNECTED' /tmp/wifi_autoconn_status.txt 2>/dev/null"
               " || echo IDLE > /tmp/wifi_autoconn_status.txt\n");

    fclose(f);
    system("chmod +x /tmp/wifi_autoconn.sh");
    system("/tmp/wifi_autoconn.sh &");

    s_autoconn_active   = 1;
    s_autoconn_start_ms = lv_tick_get();
    printf("[AutoWiFi] auto-connect started\n");
}

/* 返回 1 = 连接成功（调用方可刷新 UI），0 = 仍在进行或已放弃 */
int wifi_autoconnect_poll(void)
{
    if (!s_autoconn_active) return 0;

    /* 3 分钟超时（扫描 4s + 最多 5 个 AP × 每个 20s） */
    if (lv_tick_get() - s_autoconn_start_ms > 180000) {
        s_autoconn_active = 0;
        return 0;
    }

    FILE *f = fopen("/tmp/wifi_autoconn_status.txt", "r");
    if (!f) return 0;

    char line[128] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;

    if (strncmp(line, "CONNECTED:", 10) == 0) {
        s_autoconn_active = 0;
        printf("[AutoWiFi] connected to [%s]\n", line + 10);
        start_geo_time_sync();
        return 1;
    }
    if (strcmp(line, "IDLE") == 0) {
        s_autoconn_active = 0;
        printf("[AutoWiFi] no known networks found\n");
    }
    return 0;
}
static int       s_connecting_active   = 0;
static char      s_connecting_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
static char      s_connecting_psk[WIFI_PWD_MAX_LEN + 1]   = {0};
static uint32_t  s_connecting_start_ms = 0;
static int       s_connecting_poll_ctr = 0;

/* 通用连接函数：暂存密码 + 后台脚本连接，连接成功后才持久化密码
 * 注意：不调用 wpa_cli save_config，避免 wpa_supplicant 下次开机自动连接
 * 重要：绝对不要在脚本中 kill adbd 或操作 USB gadget，否则 ADB 会永久挂起
 * 重要：LVGL 主线程绝对不要 popen("wpa_cli")，会阻塞主线程导致 ADB 冻结 */
static void wifi_connect_with_password(wifi_selector_t *ws,
                                       const char *ssid, const char *psk)
{
    /* 暂存密码，连接成功后再持久化保存（避免错误密码被保存） */
    strncpy(s_connecting_psk, psk, WIFI_PWD_MAX_LEN);
    s_connecting_psk[WIFI_PWD_MAX_LEN] = 0;

    FILE *script = fopen("/tmp/wifi_connect.sh", "w");
    if (!script) {
        printf("Failed to create connection script\n");
        return;
    }

    fprintf(script, "#!/bin/sh\n");
    fprintf(script, "LOG=/dev/null\n");

    /* Ensure loopback is configured — the system ships with lo having NO IP.
     * adbd tries to connect to 127.0.0.1:1445 when WiFi gets IP; without lo
     * configured, the SYN goes unanswered → connect() blocks → ADB freezes. */
    fprintf(script, "ifconfig lo 127.0.0.1 netmask 255.0.0.0 up 2>/dev/null\n");

    /* Kill the wpa_cli action daemon AND any running update_wifi_status.
     * update_wifi_status runs for 10s on CONNECTED event (polls gui_fifo),
     * holding wpa_supplicant's control socket busy → other wpa_cli calls block. */
    fprintf(script, "kill $(pidof wpa_cli) 2>/dev/null\n");
    fprintf(script, "kill $(pidof update_wifi_status) 2>/dev/null\n");
    fprintf(script, "sleep 1\n");

    /* Configure and connect WiFi network */
    fprintf(script, "echo 'Configuring [%s]...' >> $LOG\n", ssid);
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 remove_network 0 2>/dev/null\n");
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 remove_network 1 2>/dev/null\n");
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 remove_network 2 2>/dev/null\n");
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 add_network > /tmp/netid.txt\n");
    fprintf(script, "NET=$(cat /tmp/netid.txt)\n");
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 set_network $NET ssid '\"%s\"'\n", ssid);
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 set_network $NET psk '\"%s\"'\n", psk);
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 select_network $NET\n");
    fprintf(script, "/usr/local/sbin/wpa_cli -i wlan0 enable_network $NET\n");

    /* Poll for COMPLETED and write status to file for LVGL to read (non-blocking).
     * LVGL main thread must NEVER call popen("wpa_cli") — it blocks and freezes ADB.
     * NOTE: BusyBox on this device has no 'cut' — use awk/sed instead. */
    fprintf(script, "echo 'CONNECTING' > /tmp/wifi_status.txt\n");
    fprintf(script, "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n");
    fprintf(script, "  STATE=$(/usr/local/sbin/wpa_cli -i wlan0 status 2>/dev/null"
                    " | grep wpa_state | sed 's/wpa_state=//')\n");
    fprintf(script, "  echo \"Poll $i: $STATE\" >> $LOG\n");
    fprintf(script, "  if [ \"$STATE\" = \"COMPLETED\" ]; then\n");
    fprintf(script, "    echo 'COMPLETED' > /tmp/wifi_status.txt\n");
    fprintf(script, "    break\n");
    fprintf(script, "  fi\n");
    fprintf(script, "  sleep 1\n");
    fprintf(script, "done\n\n");

    /* DHCP — use system default script for full IP/route/DNS setup */
    fprintf(script, "killall udhcpc 2>/dev/null\n");
    fprintf(script, "udhcpc -i wlan0 -t 8 -T 3 -n -q >> $LOG 2>&1\n");
    fprintf(script, "echo \"udhcpc exit=$?\" >> $LOG\n");

    /* Write final status — also write connected SSID for scan to read.
     * NOTE: No 'cut' on this BusyBox — use sed. */
    fprintf(script, "IP=$(ifconfig wlan0 2>/dev/null | grep 'inet addr' | sed 's/.*inet addr://;s/ .*//')\n");
    fprintf(script, "if [ -n \"$IP\" ]; then\n");
    fprintf(script, "  echo 'CONNECTED' > /tmp/wifi_status.txt\n");
    fprintf(script, "  echo '%s' > /tmp/wifi_connected_ssid.txt\n", ssid);
    fprintf(script, "else\n");
    fprintf(script, "  echo 'FAILED' > /tmp/wifi_status.txt\n");
    fprintf(script, "fi\n");

    fclose(script);

    system("chmod +x /tmp/wifi_connect.sh");
    system("/tmp/wifi_connect.sh &");

    printf("Connecting to [%s]...\n", ssid);

    /* 启动连接状态轮询 */
    s_connecting_active  = 1;
    s_connecting_poll_ctr = 0;
    s_connecting_start_ms = lv_tick_get();
    strncpy(s_connecting_ssid, ssid, WIFI_SSID_MAX_LEN);
    s_connecting_ssid[WIFI_SSID_MAX_LEN] = 0;

    lv_label_set_text(ws->hint_lbl, "Connecting...");
}

/* ────────────── WiFi扫描 (后台脚本 + 文件) ────────────── */

/* Launch background scan script — does wpa_cli scan + bss enumeration
 * and writes results to /tmp/wifi_scan.txt.  Also checks connected SSID
 * and writes to /tmp/wifi_connected_ssid.txt.
 * ALL wpa_cli calls happen in the background script — NEVER in main thread. */
static void launch_scan_script(void)
{
    FILE *f = fopen("/tmp/wifi_scan_bg.sh", "w");
    if (!f) return;

    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "/usr/local/sbin/wpa_cli -i wlan0 scan > /dev/null 2>&1\n");
    fprintf(f, "sleep 3\n");
    /* Dump all BSS entries */
    fprintf(f, "> /tmp/wifi_scan.txt\n");
    fprintf(f, "for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n");
    fprintf(f, "  OUT=$(/usr/local/sbin/wpa_cli -i wlan0 bss $i 2>/dev/null)\n");
    fprintf(f, "  SSID=$(echo \"$OUT\" | grep '^ssid=')\n");
    fprintf(f, "  [ -z \"$SSID\" ] && break\n");
    fprintf(f, "  echo \"$SSID\" >> /tmp/wifi_scan.txt\n");
    fprintf(f, "  echo \"$OUT\" | grep '^level=' >> /tmp/wifi_scan.txt\n");
    fprintf(f, "  echo \"$OUT\" | grep '^flags=' >> /tmp/wifi_scan.txt\n");
    fprintf(f, "  echo '' >> /tmp/wifi_scan.txt\n");
    fprintf(f, "done\n");
    /* Record connected SSID */
    fprintf(f, "STATE=$(/usr/local/sbin/wpa_cli -i wlan0 status 2>/dev/null)\n");
    fprintf(f, "WS=$(echo \"$STATE\" | grep '^wpa_state=' | sed 's/wpa_state=//')\n");
    fprintf(f, "if [ \"$WS\" = \"COMPLETED\" ]; then\n");
    fprintf(f, "  echo \"$STATE\" | grep '^ssid=' | sed 's/ssid=//' > /tmp/wifi_connected_ssid.txt\n");
    fprintf(f, "else\n");
    fprintf(f, "  > /tmp/wifi_connected_ssid.txt\n");
    fprintf(f, "fi\n");
    /* Signal completion */
    fprintf(f, "echo DONE > /tmp/wifi_scan_status.txt\n");
    fclose(f);

    system("chmod +x /tmp/wifi_scan_bg.sh");
    /* Remove old status to prevent reading stale results */
    unlink("/tmp/wifi_scan_status.txt");
    system("/tmp/wifi_scan_bg.sh &");
}

/* Read scan results from /tmp/wifi_scan.txt (written by background script).
 * File format: blocks separated by blank lines, each with ssid=/level=/flags= lines.
 * NO popen("wpa_cli") — everything is file I/O, zero blocking. */
static int parse_scan_results(wifi_selector_t *ws)
{
    ws->network_count = 0;

    /* Read connected SSID from file written by scan script or connect script */
    char cur_connected_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    int  cur_is_connected = 0;
    {
        FILE *sf = fopen("/tmp/wifi_connected_ssid.txt", "r");
        if (sf) {
            if (fgets(cur_connected_ssid, sizeof(cur_connected_ssid), sf)) {
                cur_connected_ssid[strcspn(cur_connected_ssid, "\n\r")] = 0;
                if (cur_connected_ssid[0])
                    cur_is_connected = 1;
            }
            fclose(sf);
        }
    }

    FILE *f = fopen("/tmp/wifi_scan.txt", "r");
    if (!f) return 0;

    char line[256];
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char flags[128] = {0};
    int level = -100;
    int found_ssid = 0;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n\r")] = 0;

        if (line[0] == '\0') {
            /* blank line = end of block */
            if (found_ssid && ssid[0] && ws->network_count < WIFI_MAX_NETWORKS) {
                wifi_network_t *net = &ws->networks[ws->network_count];
                strncpy(net->ssid, ssid, WIFI_SSID_MAX_LEN);
                net->ssid[WIFI_SSID_MAX_LEN] = '\0';
                net->rssi = level;
                net->security = (strstr(flags, "WPA") != NULL) || (strstr(flags, "WEP") != NULL);
                net->connected = (cur_is_connected && strcmp(cur_connected_ssid, ssid) == 0);
                ws->network_count++;
            }
            ssid[0] = flags[0] = 0;
            level = -100;
            found_ssid = 0;
            continue;
        }

        if (strncmp(line, "ssid=", 5) == 0) {
            strncpy(ssid, line + 5, WIFI_SSID_MAX_LEN);
            ssid[WIFI_SSID_MAX_LEN] = '\0';
            found_ssid = 1;
        } else if (strncmp(line, "level=", 6) == 0) {
            level = atoi(line + 6);
        } else if (strncmp(line, "flags=", 6) == 0) {
            strncpy(flags, line + 6, sizeof(flags) - 1);
        }
    }
    /* Handle last block if no trailing blank line */
    if (found_ssid && ssid[0] && ws->network_count < WIFI_MAX_NETWORKS) {
        wifi_network_t *net = &ws->networks[ws->network_count];
        strncpy(net->ssid, ssid, WIFI_SSID_MAX_LEN);
        net->ssid[WIFI_SSID_MAX_LEN] = '\0';
        net->rssi = level;
        net->security = (strstr(flags, "WPA") != NULL) || (strstr(flags, "WEP") != NULL);
        net->connected = (cur_is_connected && strcmp(cur_connected_ssid, ssid) == 0);
        ws->network_count++;
    }
    fclose(f);

    if (ws->network_count == 0) {
        strcpy(ws->networks[0].ssid, "(No WiFi found)");
        ws->networks[0].rssi = -100;
        ws->networks[0].security = 0;
        ws->networks[0].connected = 0;
        ws->network_count = 1;
    }

    return ws->network_count;
}

void wifi_selector_scan(wifi_selector_t *ws)
{
    /* 显示加载动画 */
    if (ws->loading_spinner) {
        lv_obj_clear_flag(ws->loading_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    
    /* Launch background scan script — no blocking wpa_cli in main thread */
    launch_scan_script();
    
    /* 标记扫描中 */
    ws->scan_pending = 1;
    ws->scan_start_time = lv_tick_get();
    
    lv_label_set_text(ws->hint_lbl, "Scanning...");
}

/* ────────────────── 列表项创建 ──────────────────── */

static void create_wifi_item(wifi_selector_t *ws, int idx)
{
    if (idx >= ws->network_count || idx >= WIFI_MAX_NETWORKS)
        return;
    
    wifi_network_t *net = &ws->networks[idx];
    
    /* 创建列表项容器 */
    lv_obj_t *item = lv_obj_create(ws->list);
    ws->list_items[idx] = item;
    
    lv_obj_set_size(item, SCR_W - 4, ITEM_H);
    lv_obj_set_pos(item, 2, idx * (ITEM_H + ITEM_MARGIN));
    lv_obj_set_style_radius(item, 4, 0);
    style_no_border(item);
    
    /* 背景色 */
    if (net->connected) {
        lv_obj_set_style_bg_color(item, C_ITEM_CONN, 0);
    } else if (idx == ws->selected_idx) {
        lv_obj_set_style_bg_color(item, C_ITEM_SEL, 0);
    } else {
        lv_obj_set_style_bg_color(item, C_ITEM_BG, 0);
    }
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    
    /* 信号强度图标 */
    const char *rssi_icon;
    lv_color_t rssi_color;
    if (net->rssi >= -50) {
        rssi_icon = LV_SYMBOL_WIFI;
        rssi_color = C_GOOD_RSSI;
    } else if (net->rssi >= -70) {
        rssi_icon = LV_SYMBOL_WIFI;
        rssi_color = C_MED_RSSI;
    } else {
        rssi_icon = LV_SYMBOL_WIFI;
        rssi_color = C_WEAK_RSSI;
    }
    
    lv_obj_t *ico = make_label(item, &lv_font_montserrat_14, rssi_color, rssi_icon);
    lv_obj_set_pos(ico, 4, 7);
    
    /* SSID */
    lv_obj_t *ssid_lbl = make_label(item, &lv_font_montserrat_12, C_TEXT, net->ssid);
    lv_obj_set_pos(ssid_lbl, 24, 2);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ssid_lbl, 160);
    
    /* RSSI值 + 锁图标 */
    char info[32];
    if (net->security) {
        snprintf(info, sizeof(info), "%ddB %s", net->rssi, LV_SYMBOL_SETTINGS);
    } else {
        snprintf(info, sizeof(info), "%ddB", net->rssi);
    }
    lv_obj_t *info_lbl = make_label(item, &lv_font_montserrat_10, C_MUTED, info);
    lv_obj_set_pos(info_lbl, 24, 16);
    
    /* 已连接标记 */
    if (net->connected) {
        lv_obj_t *conn_mark = make_label(item, &lv_font_montserrat_10, 
                                         C_GOOD_RSSI, LV_SYMBOL_OK);
        lv_obj_align(conn_mark, LV_ALIGN_RIGHT_MID, -4, 0);
    }
}

static void refresh_wifi_list(wifi_selector_t *ws)
{
    /* 清除旧列表项 */
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        if (ws->list_items[i]) {
            lv_obj_del(ws->list_items[i]);
            ws->list_items[i] = NULL;
        }
    }
    
    /* 重新创建 */
    for (int i = 0; i < ws->network_count; i++) {
        create_wifi_item(ws, i);
    }
    
    /* 更新提示 */
    lv_label_set_text(ws->hint_lbl, "🕹️Up/Down  K3:Scan  K4:Select  K5:Exit");
}

static void update_selection(wifi_selector_t *ws, int new_idx)
{
    if (new_idx < 0)
        new_idx = 0;
    if (new_idx >= ws->network_count)
        new_idx = ws->network_count - 1;
    
    int old_idx = ws->selected_idx;
    ws->selected_idx = new_idx;
    
    /* 更新背景色 */
    if (ws->list_items[old_idx]) {
        if (ws->networks[old_idx].connected)
            lv_obj_set_style_bg_color(ws->list_items[old_idx], C_ITEM_CONN, 0);
        else
            lv_obj_set_style_bg_color(ws->list_items[old_idx], C_ITEM_BG, 0);
    }
    
    if (ws->list_items[new_idx]) {
        lv_obj_set_style_bg_color(ws->list_items[new_idx], C_ITEM_SEL, 0);
    }
    
    /* 滚动列表使选中项可见 */
    if (ws->list_items[new_idx]) {
        lv_obj_scroll_to_view(ws->list_items[new_idx], LV_ANIM_ON);
    }
}

/* ────────────────── 密码输入界面 ──────────────────── */

static void pwd_input_update_display(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    
    /* 更新密码显示 (显示星号) */
    char pwd_display[WIFI_PWD_MAX_LEN + 1];
    int i;
    for (i = 0; i < pi->pwd_len && i < WIFI_PWD_MAX_LEN; i++) {
        pwd_display[i] = '*';
    }
    pwd_display[i] = '\0';
    
    /* 如果密码为空，显示提示 */
    if (pi->pwd_len == 0) {
        lv_label_set_text(pi->pwd_lbl, "(empty)");
    } else {
        lv_label_set_text(pi->pwd_lbl, pwd_display);
    }
    
    /* 更新小键盘高亮 */
    if (pi->keyboard) {
        lv_obj_t *child = lv_obj_get_child(pi->keyboard, 0);
        int index = 0;
        
        while (child != NULL) {
            int row = index / KBD_COLS;
            int col = index % KBD_COLS;
            
            if (row == pi->kbd_row && col == pi->kbd_col) {
                /* 当前选中的按钮 */
                lv_obj_set_style_bg_color(child, C_ITEM_SEL, 0);
                lv_obj_set_style_border_width(child, 1, 0);
                lv_obj_set_style_border_color(child, C_ACCENT, 0);
            } else {
                /* 非选中按钮 */
                lv_obj_set_style_bg_color(child, C_ITEM_BG, 0);
                lv_obj_set_style_border_width(child, 0, 0);
            }
            
            child = lv_obj_get_child(pi->keyboard, ++index);
            if (index >= KBD_ROWS * KBD_COLS) break;
        }
    }
}

static void pwd_input_create(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    
    if (pi->panel) return;  /* 已创建 */
    
    /* 创建覆盖整个屏幕的面板 */
    pi->panel = lv_obj_create(ws->scr);
    lv_obj_set_size(pi->panel, SCR_W, VISIBLE_H);
    lv_obj_set_pos(pi->panel, 0, 0);
    lv_obj_set_style_bg_color(pi->panel, C_BG, 0);
    lv_obj_set_style_bg_opa(pi->panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pi->panel, 0, 0);
    style_no_border(pi->panel);
    lv_obj_clear_flag(pi->panel, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 标题 - 显示SSID */
    pi->title_lbl = make_label(pi->panel, &lv_font_montserrat_14, C_ACCENT, "");
    lv_obj_set_pos(pi->title_lbl, 10, 5);
    lv_obj_set_width(pi->title_lbl, SCR_W - 20);
    
    /* 密码显示框 */
    lv_obj_t *pwd_bg = lv_obj_create(pi->panel);
    lv_obj_set_pos(pwd_bg, 10, 25);
    lv_obj_set_size(pwd_bg, SCR_W - 20, 24);
    lv_obj_set_style_bg_color(pwd_bg, C_PANEL, 0);
    lv_obj_set_style_radius(pwd_bg, 4, 0);
    style_no_border(pwd_bg);
    
    pi->pwd_lbl = make_label(pwd_bg, &lv_font_montserrat_12, C_TEXT, "(empty)");
    lv_obj_set_pos(pi->pwd_lbl, 5, 5);
    
    /* 创建小键盘布局容器 */
    pi->keyboard = lv_obj_create(pi->panel);
    lv_obj_set_pos(pi->keyboard, 5, 55);
    lv_obj_set_size(pi->keyboard, SCR_W - 10, KBD_ROWS * (KBD_BUTTON_H + KBD_MARGIN) + 5);
    lv_obj_set_style_bg_color(pi->keyboard, C_BG, 0);
    lv_obj_set_style_bg_opa(pi->keyboard, LV_OPA_TRANSP, 0);
    style_no_border(pi->keyboard);
    lv_obj_clear_flag(pi->keyboard, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 创建小键盘按钮矩阵 */
    for (int row = 0; row < KBD_ROWS; row++) {
        for (int col = 0; col < KBD_COLS; col++) {
            lv_obj_t *btn = lv_obj_create(pi->keyboard);
            
            int x = col * (KBD_BUTTON_W + KBD_MARGIN);
            int y = row * (KBD_BUTTON_H + KBD_MARGIN);
            
            lv_obj_set_pos(btn, x, y);
            lv_obj_set_size(btn, KBD_BUTTON_W, KBD_BUTTON_H);
            lv_obj_set_style_bg_color(btn, C_ITEM_BG, 0);
            lv_obj_set_style_radius(btn, 3, 0);
            style_no_border(btn);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            
            /* 添加按钮标签 (使用当前大小写状态) */
            const char *key_text = g_kbd_uppercase ? KEYBOARD_LAYOUT_UPPER[row][col] : KEYBOARD_LAYOUT_LOWER[row][col];
            lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_10, C_TEXT, key_text);
            lv_obj_center(lbl);
        }
    }
    
    /* 提示 */
    pi->hint_lbl = make_label(pi->panel, &lv_font_montserrat_10, C_MUTED, 
                              "Joy:Move K4:Select K3:Del K5:Cancel");
    lv_obj_set_pos(pi->hint_lbl, 5, VISIBLE_H - 15);
    lv_obj_set_width(pi->hint_lbl, SCR_W - 10);
    
    lv_obj_add_flag(pi->panel, LV_OBJ_FLAG_HIDDEN);
}

static void pwd_input_show(wifi_selector_t *ws, const char *ssid)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    
    /* 创建UI（如果未创建） */
    pwd_input_create(ws);
    
    /* 初始化状态 */
    strncpy(pi->ssid, ssid, WIFI_SSID_MAX_LEN);
    pi->ssid[WIFI_SSID_MAX_LEN] = '\0';
    pi->password[0] = '\0';
    pi->pwd_len = 0;
    pi->kbd_row = 0;  /* 从第一行开始 */
    pi->kbd_col = 0;  /* 从第一列开始 */
    pi->visible = 1;
    
    /* 更新标题 */
    char title[64];
    snprintf(title, sizeof(title), LV_SYMBOL_WIFI " %s", ssid);
    lv_label_set_text(pi->title_lbl, title);
    
    /* 更新显示 */
    pwd_input_update_display(ws);
    
    /* 显示面板 */
    lv_obj_clear_flag(pi->panel, LV_OBJ_FLAG_HIDDEN);
}

static void pwd_input_hide(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    
    if (pi->panel) {
        lv_obj_add_flag(pi->panel, LV_OBJ_FLAG_HIDDEN);
    }
    pi->visible = 0;
}

/* 小键盘导航函数 */
static void pwd_input_move_up(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    if (pi->kbd_row > 0) {
        pi->kbd_row--;
        pwd_input_update_display(ws);
    }
}

static void pwd_input_move_down(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    if (pi->kbd_row < KBD_ROWS - 1) {
        pi->kbd_row++;
        pwd_input_update_display(ws);
    }
}

static void pwd_input_move_left(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    if (pi->kbd_col > 0) {
        pi->kbd_col--;
        pwd_input_update_display(ws);
    }
}

static void pwd_input_move_right(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    if (pi->kbd_col < KBD_COLS - 1) {
        pi->kbd_col++;
        pwd_input_update_display(ws);
    }
}

/* 更新键盘布局显示(大小写切换后调用) */
static void pwd_input_refresh_keyboard(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    if (!pi->keyboard) return;
    
    int index = 0;
    lv_obj_t *btn = lv_obj_get_child(pi->keyboard, 0);
    
    while (btn != NULL && index < KBD_ROWS * KBD_COLS) {
        int row = index / KBD_COLS;
        int col = index % KBD_COLS;
        
        /* 更新按钮标签 */
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            const char *key_text = g_kbd_uppercase ? KEYBOARD_LAYOUT_UPPER[row][col] : KEYBOARD_LAYOUT_LOWER[row][col];
            lv_label_set_text(lbl, key_text);
        }
        
        btn = lv_obj_get_child(pi->keyboard, ++index);
    }
    
    pwd_input_update_display(ws);
}

static void pwd_input_select_key(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    
    const char *key_text = g_kbd_uppercase ? KEYBOARD_LAYOUT_UPPER[pi->kbd_row][pi->kbd_col] 
                                            : KEYBOARD_LAYOUT_LOWER[pi->kbd_row][pi->kbd_col];
    
    if (strcmp(key_text, "OK") == 0) {
        if (pi->pwd_len == 0) {
            printf("Password is empty, cannot connect\n");
            return;
        }

        printf("=== WiFi Connect: SSID=[%s] len=%d ===\n", pi->ssid, pi->pwd_len);

        /* 连接WiFi（密码在连接成功后才保存，不会保存错误密码） */
        wifi_connect_with_password(ws, pi->ssid, pi->password);

        /* 隐藏密码输入，返回列表 */
        pwd_input_hide(ws);
        return;
    }
    
    if (strcmp(key_text, "Del") == 0) {
        /* 删除最后一个字符 */
        if (pi->pwd_len > 0) {
            pi->pwd_len--;
            pi->password[pi->pwd_len] = '\0';
            pwd_input_update_display(ws);
        }
        return;
    }
    
    if (strcmp(key_text, "Aa") == 0) {
        /* 大小写切换 */
        g_kbd_uppercase = !g_kbd_uppercase;
        pwd_input_refresh_keyboard(ws);
        return;
    }
    
    /* 空格键 - 最后一行中间的空格 */
    if (strcmp(key_text, " ") == 0) {
        if (pi->pwd_len < WIFI_PWD_MAX_LEN) {
            pi->password[pi->pwd_len] = ' ';
            pi->pwd_len++;
            pi->password[pi->pwd_len] = '\0';
            pwd_input_update_display(ws);
        }
        return;
    }
    
    /* 添加字符到密码 */
    if (pi->pwd_len < WIFI_PWD_MAX_LEN) {
        pi->password[pi->pwd_len] = key_text[0];
        pi->pwd_len++;
        pi->password[pi->pwd_len] = '\0';
        pwd_input_update_display(ws);
    }
}

static void pwd_input_del_char(wifi_selector_t *ws)
{
    wifi_pwd_input_t *pi = &ws->pwd_input;
    
    if (pi->pwd_len > 0) {
        pi->pwd_len--;
        pi->password[pi->pwd_len] = '\0';
        pwd_input_update_display(ws);
    }
}

/* ────────────────── 连接状态轮询 & 操作菜单 ────────── */

/* 操作菜单（已连接/已保存网络弹出） */
#define ACTION_CONNECT     0
#define ACTION_DISCONNECT  1
#define ACTION_FORGET      2
#define ACTION_COUNT       3
#define ACTION_MENU_W      140
#define ACTION_MENU_H      110
#define ACTION_ITEM_H      22
#define ACTION_ITEM_GAP    2

static struct {
    lv_obj_t *panel;
    lv_obj_t *title_lbl;
    lv_obj_t *items[ACTION_COUNT];
    lv_obj_t *hint_lbl;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    int  selected;
    int  visible;
    int  is_connected;
} g_action_menu;

static void action_menu_update_highlight(void)
{
    for (int i = 0; i < ACTION_COUNT; i++) {
        if (!g_action_menu.items[i]) continue;
        lv_obj_set_style_bg_color(g_action_menu.items[i],
            (i == g_action_menu.selected) ? C_ITEM_SEL : C_ITEM_BG, 0);
    }
}

static void action_menu_create(wifi_selector_t *ws)
{
    if (g_action_menu.panel) return;

    g_action_menu.panel = lv_obj_create(ws->scr);
    lv_obj_set_size(g_action_menu.panel, ACTION_MENU_W, ACTION_MENU_H);
    lv_obj_set_pos(g_action_menu.panel,
                   (SCR_W - ACTION_MENU_W) / 2,
                   (VISIBLE_H - ACTION_MENU_H) / 2);
    lv_obj_set_style_bg_color(g_action_menu.panel, C_PANEL, 0);
    lv_obj_set_style_bg_opa(g_action_menu.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_action_menu.panel, 6, 0);
    lv_obj_set_style_border_width(g_action_menu.panel, 1, 0);
    lv_obj_set_style_border_color(g_action_menu.panel, C_ACCENT, 0);
    lv_obj_set_style_pad_all(g_action_menu.panel, 0, 0);
    lv_obj_set_style_shadow_width(g_action_menu.panel, 0, 0);
    lv_obj_clear_flag(g_action_menu.panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    g_action_menu.title_lbl = make_label(g_action_menu.panel,
                                          &lv_font_montserrat_12, C_ACCENT, "");
    lv_obj_set_pos(g_action_menu.title_lbl, 4, 4);
    lv_obj_set_width(g_action_menu.title_lbl, ACTION_MENU_W - 8);
    lv_label_set_long_mode(g_action_menu.title_lbl, LV_LABEL_LONG_DOT);

    /* 三个操作项 */
    static const char *labels[ACTION_COUNT] = {
        LV_SYMBOL_WIFI  " Connect",
        LV_SYMBOL_CLOSE " Disconnect",
        LV_SYMBOL_TRASH " Forget"
    };
    for (int i = 0; i < ACTION_COUNT; i++) {
        int y = 20 + i * (ACTION_ITEM_H + ACTION_ITEM_GAP);
        g_action_menu.items[i] = lv_obj_create(g_action_menu.panel);
        lv_obj_set_pos(g_action_menu.items[i], 4, y);
        lv_obj_set_size(g_action_menu.items[i], ACTION_MENU_W - 8, ACTION_ITEM_H);
        lv_obj_set_style_bg_color(g_action_menu.items[i], C_ITEM_BG, 0);
        lv_obj_set_style_bg_opa(g_action_menu.items[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g_action_menu.items[i], 3, 0);
        lv_obj_set_style_border_width(g_action_menu.items[i], 0, 0);
        lv_obj_set_style_pad_all(g_action_menu.items[i], 0, 0);
        lv_obj_set_style_shadow_width(g_action_menu.items[i], 0, 0);
        lv_obj_clear_flag(g_action_menu.items[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = make_label(g_action_menu.items[i],
                                    &lv_font_montserrat_12, C_TEXT, labels[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
    }

    /* 底部提示 */
    g_action_menu.hint_lbl = make_label(g_action_menu.panel,
                                         &lv_font_montserrat_10, C_MUTED,
                                         "Joy:Move  K4:OK  K5:Back");
    lv_obj_set_pos(g_action_menu.hint_lbl, 4, ACTION_MENU_H - 14);
    lv_obj_set_width(g_action_menu.hint_lbl, ACTION_MENU_W - 8);

    lv_obj_add_flag(g_action_menu.panel, LV_OBJ_FLAG_HIDDEN);
}

static void action_menu_show(wifi_selector_t *ws, const char *ssid, int is_connected)
{
    action_menu_create(ws);

    strncpy(g_action_menu.ssid, ssid, WIFI_SSID_MAX_LEN);
    g_action_menu.ssid[WIFI_SSID_MAX_LEN] = 0;
    g_action_menu.is_connected = is_connected;
    g_action_menu.selected     = 0;
    g_action_menu.visible      = 1;

    lv_label_set_text(g_action_menu.title_lbl, ssid);

    /* Disconnect 项: 未连接时变灰 */
    if (g_action_menu.items[ACTION_DISCONNECT]) {
        lv_obj_t *lbl = lv_obj_get_child(g_action_menu.items[ACTION_DISCONNECT], 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl, is_connected ? C_TEXT : C_MUTED, 0);
    }

    action_menu_update_highlight();
    lv_obj_clear_flag(g_action_menu.panel, LV_OBJ_FLAG_HIDDEN);
}

static void action_menu_hide(void)
{
    if (g_action_menu.panel)
        lv_obj_add_flag(g_action_menu.panel, LV_OBJ_FLAG_HIDDEN);
    g_action_menu.visible = 0;
}

/* ────────────────── 定时器回调 ──────────────────── */

static void wifi_selector_timer_cb(lv_timer_t *t)
{
    wifi_selector_t *ws = (wifi_selector_t *)t->user_data;
    
    /* 检查扫描是否完成 — 后台脚本写 /tmp/wifi_scan_status.txt 信号完成 */
    if (ws->scan_pending) {
        uint32_t elapsed = lv_tick_get() - ws->scan_start_time;
        /* Check for completion signal file, with 10s timeout */
        int scan_done = 0;
        if (elapsed >= 2000) {  /* 至少等 2 秒 */
            FILE *sf = fopen("/tmp/wifi_scan_status.txt", "r");
            if (sf) {
                char w[16] = {0};
                if (fgets(w, sizeof(w), sf))
                    scan_done = (strncmp(w, "DONE", 4) == 0);
                fclose(sf);
            }
        }
        if (scan_done || elapsed >= 10000) {
            ws->scan_pending = 0;
            
            /* 隐藏加载动画 */
            if (ws->loading_spinner) {
                lv_obj_add_flag(ws->loading_spinner, LV_OBJ_FLAG_HIDDEN);
            }
            
            /* 解析结果 */
            parse_scan_results(ws);
            
            /* 刷新列表 */
            refresh_wifi_list(ws);
            
            /* 更新提示 - 检查是否已连接 */
            char hint[128];
            int connected_found = 0;
            for (int i = 0; i < ws->network_count; i++) {
                if (ws->networks[i].connected) {
                    snprintf(hint, sizeof(hint), "Connected to %s", ws->networks[i].ssid);
                    connected_found = 1;
                    break;
                }
            }
            if (!connected_found) {
                snprintf(hint, sizeof(hint), "Found %d networks", ws->network_count);
            }
            lv_label_set_text(ws->hint_lbl, hint);
        }
        return;  /* 扫描期间跳过按键处理 */
    }

    /* 连接状态轮询: 每 ~1 秒读取 /tmp/wifi_status.txt（由后台脚本写入）
     * 绝不调用 popen("wpa_cli") — 会阻塞主线程导致 ADB 冻结 */
    if (s_connecting_active) {
        s_connecting_poll_ctr++;
        if (s_connecting_poll_ctr >= 100) {
            s_connecting_poll_ctr = 0;
            FILE *stf = fopen("/tmp/wifi_status.txt", "r");
            if (stf) {
                char status_word[32] = {0};
                if (fgets(status_word, sizeof(status_word), stf)) {
                    status_word[strcspn(status_word, "\n\r")] = 0;
                }
                fclose(stf);

                if (strcmp(status_word, "CONNECTED") == 0) {
                    s_connecting_active = 0;
                    /* 连接成功，现在才持久化保存密码 */
                    save_wifi_password(s_connecting_ssid, s_connecting_psk);
                    s_connecting_psk[0] = 0;
                    char hint[80];
                    snprintf(hint, sizeof(hint), "Connected: %s", s_connecting_ssid);
                    lv_label_set_text(ws->hint_lbl, hint);
                    start_geo_time_sync();
                } else if (strcmp(status_word, "FAILED") == 0) {
                    s_connecting_active = 0;
                    /* 连接失败，删除已保存的错误密码（若有），下次显示键盘重新输入 */
                    forget_wifi_password(s_connecting_ssid);
                    s_connecting_psk[0] = 0;
                    lv_label_set_text(ws->hint_lbl, "Wrong password");
                } else if (lv_tick_get() - s_connecting_start_ms > 30000) {
                    s_connecting_active = 0;
                    forget_wifi_password(s_connecting_ssid);
                    s_connecting_psk[0] = 0;
                    lv_label_set_text(ws->hint_lbl, "Connection timeout");
                }
            }
        }
    }
    
    /* 读取摇杆输入（旋转编码器） */
    int delta = drain_rotary(ws->rotary_fd);
    
    /* JB按键检测: 从event2读取EV_KEY code=0x09 (joybtn-key) */
    int jb_just_pressed = detect_jb_press(ws->keypad_fd);
    
    /* K键检测 (K3删除, K5退出) */
    static int last_key = 0;
    static int key_hold_count = 0;
    int key = detect_key_press();
    
    if (key == last_key && key != 0) {
        key_hold_count++;
        if (key_hold_count < 3)  /* 防抖: 需要30ms稳定 */
            return;
    } else {
        key_hold_count = 0;
        last_key = key;
    }
    
    /* 操作菜单模式 */
    if (g_action_menu.visible) {
        int joy_up2, joy_down2, joy_left2, joy_right2;
        detect_joystick_movement(&joy_up2, &joy_down2, &joy_left2, &joy_right2);

        if ((joy_up2 || delta < 0) && g_action_menu.selected > 0) {
            g_action_menu.selected--;
            action_menu_update_highlight();
        } else if ((joy_down2 || delta > 0) && g_action_menu.selected < ACTION_COUNT - 1) {
            g_action_menu.selected++;
            action_menu_update_highlight();
        }

        if (key == 4 && key_hold_count == 3) {
            int sel = g_action_menu.selected;
            char cur_ssid[WIFI_SSID_MAX_LEN + 1];
            int was_connected = g_action_menu.is_connected;
            strncpy(cur_ssid, g_action_menu.ssid, WIFI_SSID_MAX_LEN);
            cur_ssid[WIFI_SSID_MAX_LEN] = 0;
            action_menu_hide();

            if (sel == ACTION_CONNECT) {
                char saved_psk[WIFI_PWD_MAX_LEN + 1];
                if (load_wifi_password(cur_ssid, saved_psk, sizeof(saved_psk)))
                    wifi_connect_with_password(ws, cur_ssid, saved_psk);
                else
                    pwd_input_show(ws, cur_ssid);

            } else if (sel == ACTION_DISCONNECT) {
                if (was_connected) {
                    system("/usr/local/sbin/wpa_cli -i wlan0 disconnect > /dev/null 2>&1;"
                           "/usr/local/sbin/wpa_cli -i wlan0 remove_network 0 > /dev/null 2>&1;"
                           "/usr/local/sbin/wpa_cli -i wlan0 remove_network 1 > /dev/null 2>&1 &");
                    s_connecting_active = 0;
                    s_connecting_ssid[0] = 0;
                    unlink("/tmp/wifi_connected_ssid.txt");
                    lv_label_set_text(ws->hint_lbl, "Disconnected");
                } else {
                    lv_label_set_text(ws->hint_lbl, "Not connected");
                }

            } else if (sel == ACTION_FORGET) {
                forget_wifi_password(cur_ssid);
                if (was_connected) {
                    system("/usr/local/sbin/wpa_cli -i wlan0 disconnect > /dev/null 2>&1;"
                           "/usr/local/sbin/wpa_cli -i wlan0 remove_network 0 > /dev/null 2>&1;"
                           "/usr/local/sbin/wpa_cli -i wlan0 remove_network 1 > /dev/null 2>&1 &");
                }
                s_connecting_active = 0;
                unlink("/tmp/wifi_connected_ssid.txt");
                lv_label_set_text(ws->hint_lbl, "Password forgotten");
            }
        }

        if (key == 5 && key_hold_count == 3)
            action_menu_hide();

        return;
    }

    /* 密码输入模式 - 小键盘导航 */
    if (ws->pwd_input.visible) {
        /* 操纵杆控制小键盘四方向导航 */
        int joy_up, joy_down, joy_left, joy_right;
        detect_joystick_movement(&joy_up, &joy_down, &joy_left, &joy_right);
        
        if (joy_up) {
            pwd_input_move_up(ws);
        } else if (joy_down) {
            pwd_input_move_down(ws);
        } else if (joy_left) {
            pwd_input_move_left(ws);
        } else if (joy_right) {
            pwd_input_move_right(ws);
        }
        
        /* 旋转编码器作为备用导航（仅上下） */
        if (delta > 0) {
            pwd_input_move_down(ws);
        } else if (delta < 0) {
            pwd_input_move_up(ws);
        }
        
        /* K4按键选择当前按钮 (替代JB) */
        if (key == 4 && key_hold_count == 3) {
            pwd_input_select_key(ws);
        }
        
        /* K3删除字符, K5取消输入 */
        if (key == 3 && key_hold_count == 3) {
            pwd_input_del_char(ws);
        } else if (key == 5 && key_hold_count == 3) {
            pwd_input_hide(ws);
        }
        return;
    }
    
    /* WiFi列表模式 - 摇杆+波轮滚动列表 */
    /* 左操纵杆上下移动 */
    int joy_up, joy_down, joy_left, joy_right;
    detect_joystick_movement(&joy_up, &joy_down, &joy_left, &joy_right);
    
    if (joy_up) {
        update_selection(ws, ws->selected_idx - 1);
    } else if (joy_down) {
        update_selection(ws, ws->selected_idx + 1);
    }
    
    /* 波轮也可以滚动 */
    if (delta != 0) {
        update_selection(ws, ws->selected_idx - delta);
    }
    
    /* WiFi列表模式 - JB或K4按键选择WiFi */
    int select_wifi = 0;
    if (jb_just_pressed) {
        select_wifi = 1;
    } else if (key == 4 && key_hold_count == 3) {
        /* K4选择WiFi */
        select_wifi = 1;
    }
    
    if (select_wifi) {
        if (ws->selected_idx >= 0 && ws->selected_idx < ws->network_count) {
            wifi_network_t *net = &ws->networks[ws->selected_idx];
            if (net->security) {
                char saved_psk[WIFI_PWD_MAX_LEN + 1];
                int has_saved = load_wifi_password(net->ssid, saved_psk, sizeof(saved_psk));

                if (net->connected) {
                    /* 已连接 → 显示操作菜单 (Connect/Disconnect/Forget) */
                    action_menu_show(ws, net->ssid, 1);
                } else if (has_saved) {
                    /* 有保存密码，直接连接，无需重新输入 */
                    printf("Found saved password for [%s], connecting directly\n", net->ssid);
                    wifi_connect_with_password(ws, net->ssid, saved_psk);
                } else {
                    /* 无保存密码 → 弹键盘 */
                    pwd_input_show(ws, net->ssid);
                }
            } else {
                /* 开放网络 - 直接连接 */
                char cmd[512];
                snprintf(cmd, sizeof(cmd),
                         "/usr/local/sbin/wpa_cli -i wlan0 add_network && "
                         "/usr/local/sbin/wpa_cli -i wlan0 set_network 0 ssid '\"%s\"' && "
                         "/usr/local/sbin/wpa_cli -i wlan0 set_network 0 key_mgmt NONE && "
                         "/usr/local/sbin/wpa_cli -i wlan0 enable_network 0 &",
                         net->ssid);
                system(cmd);
                lv_label_set_text(ws->hint_lbl, "Connecting...");
            }
        }
    } else if (key == 3 && key_hold_count == 3) {
        /* K3 - 刷新扫描 */
        wifi_selector_scan(ws);
    } else if (key == 5 && key_hold_count == 3) {
        /* K5 - 退出 */
        if (ws->on_exit_callback) {
            ws->on_exit_callback();
        }
    }
}

/* ────────────────── 公共API ──────────────────── */

void wifi_selector_init(wifi_selector_t *ws)
{
    memset(ws, 0, sizeof(*ws));
    ws->rotary_fd = -1;
    ws->keypad_fd = -1;
    ws->selected_idx = 0;
    ws->network_count = 0;
    
    /* 创建屏幕 */
    ws->scr = lv_obj_create(NULL);
    lv_obj_set_size(ws->scr, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(ws->scr, C_BG, 0);
    lv_obj_set_style_bg_opa(ws->scr, LV_OPA_COVER, 0);
    style_no_border(ws->scr);
    
    /* 标题栏 */
    ws->title_bar = lv_obj_create(ws->scr);
    lv_obj_set_pos(ws->title_bar, 0, 0);
    lv_obj_set_size(ws->title_bar, SCR_W, TITLE_H);
    lv_obj_set_style_bg_color(ws->title_bar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(ws->title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ws->title_bar, 0, 0);
    style_no_border(ws->title_bar);
    
    ws->title_lbl = make_label(ws->title_bar, &lv_font_montserrat_14, 
                               C_TEXT, LV_SYMBOL_WIFI " WiFi Settings");
    lv_obj_align(ws->title_lbl, LV_ALIGN_CENTER, 0, 0);
    
    /* WiFi列表容器 */
    ws->list = lv_obj_create(ws->scr);
    lv_obj_set_pos(ws->list, 0, LIST_Y);
    lv_obj_set_size(ws->list, SCR_W, LIST_H);
    lv_obj_set_style_bg_color(ws->list, C_BG, 0);
    lv_obj_set_style_bg_opa(ws->list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ws->list, 0, 0);
    style_no_border(ws->list);
    lv_obj_set_flex_flow(ws->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ws->list, 0, 0);
    lv_obj_set_scrollbar_mode(ws->list, LV_SCROLLBAR_MODE_AUTO);
    
    /* 加载动画 (初始隐藏) */
    ws->loading_spinner = lv_spinner_create(ws->list, 1000, 60);
    lv_obj_set_size(ws->loading_spinner, 40, 40);
    lv_obj_center(ws->loading_spinner);
    lv_obj_add_flag(ws->loading_spinner, LV_OBJ_FLAG_HIDDEN);
    
    /* 底部提示栏 */
    ws->hint_bar = lv_obj_create(ws->scr);
    lv_obj_set_pos(ws->hint_bar, 0, LIST_Y + LIST_H);
    lv_obj_set_size(ws->hint_bar, SCR_W, HINT_H);
    lv_obj_set_style_bg_color(ws->hint_bar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(ws->hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ws->hint_bar, 0, 0);
    style_no_border(ws->hint_bar);
    
    ws->hint_lbl = make_label(ws->hint_bar, &lv_font_montserrat_10, 
                             C_MUTED, "🕹️Up/Down  K3:Scan  K4:Select  K5:Exit");
    lv_obj_align(ws->hint_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(ws->hint_lbl, SCR_W - 8);
    lv_label_set_long_mode(ws->hint_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    
    /* 打开波轮设备 */
    ws->rotary_fd = open(ROTARY_DEV, O_RDONLY | O_NONBLOCK);

    /* 打开按键事件设备 (event2: rockchip,key - 包含编码器按钮 code=0x09) */
    ws->keypad_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
}

void wifi_selector_show(wifi_selector_t *ws)
{
    /* 加载屏幕 */
    lv_scr_load(ws->scr);
    
    /* 先触发后台扫描 — 不阻塞主线程 */
    launch_scan_script();
    
    /* 标记为扫描中 */
    ws->scan_pending = 1;
    ws->scan_start_time = lv_tick_get();
    
    lv_label_set_text(ws->hint_lbl, "Scanning WiFi...");
    
    /* 显示加载动画 */
    if (ws->loading_spinner) {
        lv_obj_clear_flag(ws->loading_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    
    /* 启动定时器 */
    if (!ws->update_tmr) {
        ws->update_tmr = lv_timer_create(wifi_selector_timer_cb, 10, ws);
    }
}

void wifi_selector_hide(wifi_selector_t *ws)
{
    /* 停止定时器 */
    if (ws->update_tmr) {
        lv_timer_del(ws->update_tmr);
        ws->update_tmr = NULL;
    }
    
    /* 这里应该切换回主界面 - 由回调处理 */
}

void wifi_selector_set_callbacks(wifi_selector_t *ws,
                                 void (*on_select)(const char *, int),
                                 void (*on_exit)(void))
{
    ws->on_select_callback = on_select;
    ws->on_exit_callback = on_exit;
}

void wifi_selector_show_pwd_input(wifi_selector_t *ws, const char *ssid)
{
    pwd_input_show(ws, ssid);
}
