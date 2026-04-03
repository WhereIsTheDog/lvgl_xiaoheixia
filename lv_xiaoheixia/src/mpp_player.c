/*
 * mpp_player.c – H264 hardware decoder for RV1108 framebuffer
 *
 * Replaces OEM preview_stream: reads H264 Annex-B from FIFO, decodes via
 * Rockchip MPP hardware, scales NV12→RGB565 to fit 240×240, writes /dev/fb0.
 *
 * Usage: mpp_player [fifo_path]
 *        Default FIFO: /var/run/preview_stream_fifo
 *
 * Build: dynamically linked (needs librockchip_mpp.so on device)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/* ═══════════════════════════════════════════════════════════
 *  MPP type definitions (from rockchip-linux/mpp headers)
 * ═══════════════════════════════════════════════════════════ */

typedef int32_t  RK_S32;
typedef uint32_t RK_U32;
typedef int64_t  RK_S64;
typedef void    *MppCtx;
typedef void    *MppFrame;
typedef void    *MppPacket;
typedef void    *MppBuffer;
typedef RK_S32   MPP_RET;

/* MppCtxType */
#define MPP_CTX_DEC  0

/* MppCodingType — H.264/AVC */
#define MPP_VIDEO_CodingAVC  7

/* MPP commands (enum values from rk_mpi_cmd.h)
 *   MPP_DEC_CMD_BASE = CMD_MODULE_CODEC | CMD_CTX_ID_DEC = 0x00310000
 *   then auto-increment in enum order                                 */
#define MPP_SET_OUTPUT_TIMEOUT          0x00200007
#define MPP_DEC_SET_INFO_CHANGE_READY   0x00310003
#define MPP_DEC_SET_PARSER_SPLIT_MODE   0x00310005

/* MppApi — function pointer table returned by mpp_create() */
typedef struct {
    RK_U32  size;
    RK_U32  version;
    /* simple data flow */
    MPP_RET (*decode)(MppCtx, MppPacket, MppFrame *);
    MPP_RET (*decode_put_packet)(MppCtx, MppPacket);
    MPP_RET (*decode_get_frame)(MppCtx, MppFrame *);
    MPP_RET (*encode)(MppCtx, MppFrame, MppPacket *);
    MPP_RET (*encode_put_frame)(MppCtx, MppFrame);
    MPP_RET (*encode_get_packet)(MppCtx, MppPacket *);
    MPP_RET (*isp)(MppCtx, MppFrame, MppFrame);
    MPP_RET (*isp_put_frame)(MppCtx, MppFrame);
    MPP_RET (*isp_get_frame)(MppCtx, MppFrame *);
    /* advanced task flow */
    MPP_RET (*poll)(MppCtx, int, int);
    MPP_RET (*dequeue)(MppCtx, int, void **);
    MPP_RET (*enqueue)(MppCtx, int, void *);
    /* control */
    MPP_RET (*reset)(MppCtx);
    MPP_RET (*control)(MppCtx, int, void *);
    RK_U32  reserv[16];
} MppApi;

/* ═══════════════════════════════════════════════════════════
 *  MPP function declarations (linked directly against .so)
 * ═══════════════════════════════════════════════════════════ */

extern MPP_RET mpp_create(MppCtx *, MppApi **);
extern MPP_RET mpp_init(MppCtx, int, int);
extern MPP_RET mpp_destroy(MppCtx);

extern MPP_RET mpp_packet_init(MppPacket *, void *, size_t);
extern MPP_RET mpp_packet_deinit(MppPacket *);
extern MPP_RET mpp_packet_set_eos(MppPacket);

extern RK_U32  mpp_frame_get_width(MppFrame);
extern RK_U32  mpp_frame_get_height(MppFrame);
extern RK_U32  mpp_frame_get_hor_stride(MppFrame);
extern RK_U32  mpp_frame_get_ver_stride(MppFrame);
extern RK_U32  mpp_frame_get_info_change(MppFrame);
extern RK_U32  mpp_frame_get_eos(MppFrame);
extern RK_U32  mpp_frame_get_errinfo(MppFrame);
extern RK_U32  mpp_frame_get_discard(MppFrame);
extern MppBuffer mpp_frame_get_buffer(MppFrame);
extern MPP_RET mpp_frame_deinit(MppFrame *);

extern void *mpp_buffer_get_ptr_with_caller(MppBuffer, const char *);

static void *buffer_get_ptr(MppBuffer buf)
{
    return mpp_buffer_get_ptr_with_caller(buf, "mpp_player");
}

/* ═══════════════════════════════════════════════════════════
 *  Globals
 * ═══════════════════════════════════════════════════════════ */

static volatile int g_running = 1;
static uint16_t *g_fb_mem  = NULL;
static int       g_fb_fd   = -1;
static uint32_t  g_fb_w    = 240;
static uint32_t  g_fb_h    = 240;
static uint32_t  g_fb_stride_px;   /* pixels per line (may == g_fb_w) */
static size_t    g_fb_size;

#define READ_BUF_SIZE  (128 * 1024)
#define LOG(fmt, ...)  fprintf(stderr, "[mpp] " fmt "\n", ##__VA_ARGS__)

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ═══════════════════════════════════════════════════════════
 *  Framebuffer init
 * ═══════════════════════════════════════════════════════════ */

static int fb_init(void)
{
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) { perror("[mpp] open fb0"); return -1; }

    struct fb_var_screeninfo vinfo;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("[mpp] FBIOGET_VSCREENINFO");
        close(g_fb_fd); return -1;
    }

    g_fb_w = vinfo.xres;
    g_fb_h = vinfo.yres;
    g_fb_stride_px = vinfo.xres;   /* assume no padding */
    g_fb_size = (size_t)vinfo.xres * vinfo.yres_virtual * (vinfo.bits_per_pixel / 8);

    g_fb_mem = mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        perror("[mpp] mmap fb0"); close(g_fb_fd); return -1;
    }
    LOG("fb0: %ux%u %ubpp virt=%ux%u",
        vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
        vinfo.xres, vinfo.yres_virtual);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  NV12 → RGB565 with nearest-neighbor scaling (letterboxed)
 * ═══════════════════════════════════════════════════════════ */

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void render_nv12_to_fb(
    const uint8_t *y_plane, const uint8_t *uv_plane,
    uint32_t src_w, uint32_t src_h, uint32_t src_stride,
    uint16_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    /* Calculate letterbox dimensions (preserve aspect ratio) */
    uint32_t out_w, out_h, off_x, off_y;
    if (src_w * dst_h > src_h * dst_w) {
        out_w = dst_w;
        out_h = (src_h * dst_w + src_w / 2) / src_w;
    } else {
        out_h = dst_h;
        out_w = (src_w * dst_h + src_h / 2) / src_h;
    }
    if (out_w > dst_w) out_w = dst_w;
    if (out_h > dst_h) out_h = dst_h;
    off_x = (dst_w - out_w) / 2;
    off_y = (dst_h - out_h) / 2;

    /* Clear to black: top/bottom bars */
    for (uint32_t y = 0; y < off_y; y++)
        memset(dst + y * dst_w, 0, dst_w * 2);
    for (uint32_t y = off_y + out_h; y < dst_h; y++)
        memset(dst + y * dst_w, 0, dst_w * 2);

    /* Render scaled video */
    for (uint32_t dy = 0; dy < out_h; dy++) {
        uint32_t sy = dy * src_h / out_h;
        const uint8_t *yrow  = y_plane  + sy * src_stride;
        const uint8_t *uvrow = uv_plane + (sy / 2) * src_stride;
        uint16_t *drow = dst + (off_y + dy) * dst_w;

        /* Left black bar */
        if (off_x > 0)
            memset(drow, 0, off_x * 2);
        /* Right black bar */
        if (off_x + out_w < dst_w)
            memset(drow + off_x + out_w, 0, (dst_w - off_x - out_w) * 2);

        /* Scaled pixels */
        for (uint32_t dx = 0; dx < out_w; dx++) {
            uint32_t sx = dx * src_w / out_w;
            int Y = yrow[sx];
            int uv_idx = (sx & ~1u);
            int U = uvrow[uv_idx]     - 128;
            int V = uvrow[uv_idx + 1] - 128;

            int R = clamp8(Y + ((V * 359) >> 8));
            int G = clamp8(Y - ((U * 88 + V * 183) >> 8));
            int B = clamp8(Y + ((U * 454) >> 8));

            drow[off_x + dx] = (uint16_t)(((R >> 3) << 11) |
                                           ((G >> 2) << 5)  |
                                            (B >> 3));
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Drain decoded frames from MPP
 * ═══════════════════════════════════════════════════════════ */

static uint32_t s_src_w, s_src_h, s_src_stride, s_src_vstride;
static uint32_t s_frame_count;

static void drain_frames(MppCtx ctx, MppApi *mpi)
{
    while (g_running) {
        MppFrame frame = NULL;
        MPP_RET ret = mpi->decode_get_frame(ctx, &frame);
        if (ret != 0 || !frame)
            break;

        if (mpp_frame_get_info_change(frame)) {
            s_src_w = mpp_frame_get_width(frame);
            s_src_h = mpp_frame_get_height(frame);
            s_src_stride = mpp_frame_get_hor_stride(frame);
            s_src_vstride = mpp_frame_get_ver_stride(frame);
            LOG("info_change: %ux%u stride=%u vstride=%u",
                s_src_w, s_src_h, s_src_stride, s_src_vstride);
            mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        } else {
            int err = mpp_frame_get_errinfo(frame);
            int disc = mpp_frame_get_discard(frame);

            if (!err && !disc && s_src_w && s_src_h) {
                MppBuffer buf = mpp_frame_get_buffer(frame);
                if (buf) {
                    uint8_t *data = buffer_get_ptr(buf);
                    if (data) {
                        const uint8_t *y  = data;
                        const uint8_t *uv = data + s_src_stride * s_src_vstride;
                        render_nv12_to_fb(y, uv,
                            s_src_w, s_src_h, s_src_stride,
                            g_fb_mem, g_fb_w, g_fb_h);
                        s_frame_count++;
                        if (s_frame_count <= 3 || (s_frame_count % 150) == 0)
                            LOG("frame #%u (%ux%u→%ux%u)",
                                s_frame_count, s_src_w, s_src_h, g_fb_w, g_fb_h);
                    }
                }
            }

            if (mpp_frame_get_eos(frame)) {
                LOG("EOS received");
                g_running = 0;
            }
        }
        mpp_frame_deinit(&frame);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    const char *fifo_path = "/var/run/preview_stream_fifo";
    if (argc > 1)
        fifo_path = argv[1];

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    LOG("mpp_player starting, fifo=%s pid=%d", fifo_path, getpid());

    if (fb_init() < 0) return 1;

    /* Clear screen to black */
    memset(g_fb_mem, 0, g_fb_w * g_fb_h * 2);

    /* Create MPP decoder context */
    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    if (mpp_create(&ctx, &mpi) != 0) {
        LOG("mpp_create failed"); return 1;
    }

    /* Parser split mode: let MPP find NAL boundaries in raw stream */
    RK_U32 split = 1;
    mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);

    /* Non-blocking output so decode_get_frame returns immediately */
    RK_S64 timeout = 0;
    mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);

    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != 0) {
        LOG("mpp_init failed");
        mpp_destroy(ctx);
        return 1;
    }
    LOG("MPP H264 decoder ready");

    /* Open FIFO (blocks until writer connects) */
    LOG("waiting for FIFO writer...");
    int fifo_fd = open(fifo_path, O_RDONLY);
    if (fifo_fd < 0) {
        LOG("open FIFO: %s", strerror(errno));
        mpp_destroy(ctx); return 1;
    }
    LOG("FIFO connected");

    uint8_t *read_buf = malloc(READ_BUF_SIZE);
    if (!read_buf) {
        LOG("malloc failed");
        close(fifo_fd); mpp_destroy(ctx); return 1;
    }

    /* ── Main decode loop ── */
    while (g_running) {
        ssize_t n = read(fifo_fd, read_buf, READ_BUF_SIZE);
        if (n <= 0) {
            if (n == 0) {
                /* FIFO writer closed — flush decoder and reopen */
                LOG("FIFO EOF, flushing decoder...");
                {
                    MppPacket eos_pkt = NULL;
                    if (mpp_packet_init(&eos_pkt, NULL, 0) == 0) {
                        mpp_packet_set_eos(eos_pkt);
                        mpi->decode_put_packet(ctx, eos_pkt);
                        mpp_packet_deinit(&eos_pkt);
                    }
                }
                drain_frames(ctx, mpi);

                /* Reset decoder for next stream */
                mpi->reset(ctx);
                s_src_w = s_src_h = 0;

                close(fifo_fd);
                usleep(200000);
                LOG("reopening FIFO...");
                fifo_fd = open(fifo_path, O_RDONLY);
                if (fifo_fd < 0) {
                    LOG("reopen FIFO: %s", strerror(errno));
                    break;
                }
                LOG("FIFO reconnected");
                continue;
            }
            if (errno == EINTR) continue;
            LOG("read error: %s", strerror(errno));
            break;
        }

        /* Feed data to decoder */
        MppPacket pkt = NULL;
        if (mpp_packet_init(&pkt, read_buf, (size_t)n) != 0) {
            LOG("packet_init failed");
            continue;
        }

        /* Retry if decoder queue is full */
        int retries = 0;
        MPP_RET ret;
        do {
            ret = mpi->decode_put_packet(ctx, pkt);
            if (ret != 0) {
                drain_frames(ctx, mpi);
                usleep(2000);
            }
        } while (ret != 0 && g_running && ++retries < 50);

        mpp_packet_deinit(&pkt);

        if (retries >= 50)
            LOG("WARN: put_packet failed after retries");

        /* Drain any ready frames */
        drain_frames(ctx, mpi);
    }

    LOG("shutting down, rendered %u frames total", s_frame_count);

    free(read_buf);
    if (fifo_fd >= 0) close(fifo_fd);
    mpp_destroy(ctx);
    if (g_fb_mem && g_fb_mem != MAP_FAILED)
        munmap(g_fb_mem, g_fb_size);
    if (g_fb_fd >= 0) close(g_fb_fd);
    return 0;
}
