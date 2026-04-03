#include "lvgl/lvgl.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <linux/input.h>

#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include "dashboard.h"

#define DISP_BUF_SIZE (240 * 480)
#define BEEP_DEV      "/dev/input/event0"

/* keep the generated code happy – it references this global */
#include "generated/gui_guider.h"
lv_ui guider_ui;

static dashboard_t dash;

/* When 1, fbdev_flush is a no-op (preview_stream owns the framebuffer) */
volatile int g_fb_paused = 0;

static void beep_once(int freq_hz, int dur_ms)
{
    int fd = open(BEEP_DEV, O_WRONLY);
    if (fd < 0)
        return;

    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = EV_SND;
    ev.code  = SND_BELL;
    ev.value = freq_hz;
    write(fd, &ev, sizeof(ev));

    usleep(dur_ms * 1000);

    memset(&ev, 0, sizeof(ev));
    ev.type  = EV_SND;
    ev.code  = SND_BELL;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));

    close(fd);
}

static void my_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    if (!g_fb_paused)
        fbdev_flush(drv, area, color_p);
    else
        lv_disp_flush_ready(drv);
}

int main(void)
{
    lv_init();

    /* framebuffer display */
    fbdev_init();

    static lv_color_t buf[DISP_BUF_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, DISP_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = my_flush_cb;   /* wrapper that can pause */
    disp_drv.hor_res  = 240;
    disp_drv.ver_res  = 240;
    lv_disp_drv_register(&disp_drv);

    /* evdev input (keypad / rotary) */
    evdev_init();
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);

    /* ── launch dashboard ── */
    dashboard_init(&dash);

    /* startup beep */
    beep_once(1000, 120);

    /* main loop */
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}

uint32_t custom_tick_get(void)
{
    static uint64_t start_ms = 0;
    if (start_ms == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        start_ms = (tv.tv_sec * 1000000 + tv.tv_usec) / 1000;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = (tv.tv_sec * 1000000 + tv.tv_usec) / 1000;
    return (uint32_t)(now_ms - start_ms);
}
