/*
 * rtsp_client.c — Minimal RTSP/RTP → H264 FIFO player
 *
 * Usage: rtsp_client <rtsp_url>
 *
 * Connects to an RTSP server, negotiates RTP/AVP/TCP interleaved transport,
 * reassembles H264 NAL units from RTP packets (handles FU-A fragmentation),
 * writes Annex-B formatted NAL units to /var/run/preview_stream_fifo so
 * preview_stream can hardware-decode and display them.
 *
 * Press the power button (KEY_POWER) to stop playback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <linux/input.h>
#include <signal.h>

/* ── constants ── */
#define FIFO_PATH       "/var/run/preview_stream_fifo"
#define POWERKEY_DEV    "/dev/input/event1"
#define BUF_SIZE        (512 * 1024)
#define RESP_SIZE       (16 * 1024)
#define MAX_NAL_SIZE    (512 * 1024)

/* Annex-B start code */
static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

/* ── URL parser ── */
typedef struct {
    char host[256];
    int  port;
    char path[1024];
    char user[128];
    char pass[128];
    char full[1024];
} rtsp_url_t;

static int parse_url(const char *url, rtsp_url_t *u)
{
    memset(u, 0, sizeof(*u));
    strncpy(u->full, url, sizeof(u->full) - 1);

    /* strip "rtsp://" */
    const char *p = url;
    if (strncmp(p, "rtsp://", 7) != 0) return -1;
    p += 7;

    /* check for user:pass@ */
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) {
        const char *colon = memchr(p, ':', at - p);
        if (colon) {
            int ulen = colon - p;
            int plen = at - colon - 1;
            strncpy(u->user, p, ulen < 127 ? ulen : 127);
            strncpy(u->pass, colon + 1, plen < 127 ? plen : 127);
        } else {
            int ulen = at - p;
            strncpy(u->user, p, ulen < 127 ? ulen : 127);
        }
        p = at + 1;
    }

    /* host:port */
    const char *host_end = strpbrk(p, ":/");
    if (!host_end) {
        strncpy(u->host, p, sizeof(u->host) - 1);
        u->port = 554;
        strcpy(u->path, "/");
        return 0;
    }
    int hlen = host_end - p;
    strncpy(u->host, p, hlen < 255 ? hlen : 255);
    p = host_end;
    if (*p == ':') {
        u->port = atoi(p + 1);
        p = strchr(p, '/');
        if (!p) p = "/";
    } else {
        u->port = 554;
    }
    strncpy(u->path, p, sizeof(u->path) - 1);
    return 0;
}

/* ── TCP helpers ── */
static int tcp_connect(const char *host, int port)
{
    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "[rtsp] gethostbyname(%s) failed\n", host); return -1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[rtsp] socket"); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("[rtsp] connect"); close(fd); return -1;
    }
    return fd;
}

static int tcp_send(int fd, const char *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = write(fd, data + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Read until we see "\r\n\r\n" (end of RTSP response headers) */
static int read_response(int fd, char *buf, int maxlen)
{
    int total = 0;
    while (total < maxlen - 1) {
        int n = read(fd, buf + total, 1);
        if (n <= 0) return -1;
        total++;
        buf[total] = '\0';
        if (total >= 4 &&
            buf[total-4]=='\r' && buf[total-3]=='\n' &&
            buf[total-2]=='\r' && buf[total-1]=='\n')
            break;
    }
    return total;
}

/* Base64 encode for Basic auth */
static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, int inlen, char *out)
{
    int i = 0, j = 0;
    while (i < inlen) {
        uint32_t o = (uint8_t)in[i++] << 16;
        if (i < inlen) o |= (uint8_t)in[i++] << 8;
        if (i < inlen) o |= (uint8_t)in[i++];
        out[j++] = b64[(o >> 18) & 63];
        out[j++] = b64[(o >> 12) & 63];
        out[j++] = (i - 1 < inlen) ? b64[(o >> 6) & 63] : '=';
        out[j++] = (i     < inlen || (i % 3 == 0 && i == inlen))
                   ? b64[o & 63] : '=';
    }
    /* Correct padding */
    if (inlen % 3 == 1) { out[j-2] = '='; out[j-1] = '='; }
    else if (inlen % 3 == 2) { out[j-1] = '='; }
    out[j] = '\0';
}

/* ── RTSP state ── */
typedef struct {
    int  fd;
    int  cseq;
    char session[128];
    char base_url[1024];
    char auth[256];
} rtsp_t;

static int rtsp_request(rtsp_t *r, const char *method, const char *url,
                        const char *extra, char *resp, int rlen)
{
    char req[4096];
    snprintf(req, sizeof(req),
        "%s %s RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "%s"           /* auth header if set */
        "%s"           /* session header if set */
        "%s"           /* extra headers */
        "\r\n",
        method, url, ++r->cseq,
        r->auth[0]    ? r->auth    : "",
        r->session[0] ? r->session : "",
        extra ? extra : "");

    printf("[rtsp] >> %s %s\n", method, url);
    if (tcp_send(r->fd, req, strlen(req)) < 0) return -1;
    int n = read_response(r->fd, resp, rlen);
    if (n <= 0) return -1;
    printf("[rtsp] << %.*s\n", n > 1024 ? 1024 : n, resp);
    return n;
}

/* Extract header value (first occurrence, trimmed) */
static const char *get_header(const char *resp, const char *name, char *val, int vlen)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\r\n%s:", name);
    const char *p = strcasestr(resp, needle);
    if (!p) {
        /* Try start of response too */
        if (strncasecmp(resp, name, strlen(name)) == 0 && resp[strlen(name)] == ':')
            p = resp - 2; /* fake offset so +2+len works */
        else
            return NULL;
    }
    p += 2 + strlen(name) + 1; /* skip \r\n, name, : */
    while (*p == ' ') p++;
    const char *end = strstr(p, "\r\n");
    int len = end ? (int)(end - p) : (int)strlen(p);
    if (len >= vlen) len = vlen - 1;
    strncpy(val, p, len);
    val[len] = '\0';
    return val;
}

/* ── RTP / H264 FU-A reassembly ── */
#define RTP_HDR_SIZE 12

static uint8_t nal_buf[MAX_NAL_SIZE];
static int     nal_len = 0;
static int     nal_fua_type = 0;
static int     g_sps_logged = 0;  /* only log resolution once */

/* ── H264 SPS parser (extract width/height) ── */
typedef struct {
    const uint8_t *data;
    int            len;
    int            bit_pos;
} bitreader_t;

static int br_read_bit(bitreader_t *br)
{
    if (br->bit_pos / 8 >= br->len) return 0;
    int val = (br->data[br->bit_pos / 8] >> (7 - (br->bit_pos % 8))) & 1;
    br->bit_pos++;
    return val;
}

static unsigned br_read_bits(bitreader_t *br, int n)
{
    unsigned val = 0;
    int i;
    for (i = 0; i < n; i++)
        val = (val << 1) | br_read_bit(br);
    return val;
}

/* Exp-Golomb unsigned */
static unsigned br_read_ue(bitreader_t *br)
{
    int lz = 0;
    while (br_read_bit(br) == 0 && lz < 32)
        lz++;
    if (lz == 0) return 0;
    return (1u << lz) - 1 + br_read_bits(br, lz);
}

/* Exp-Golomb signed */
static int br_read_se(bitreader_t *br)
{
    unsigned v = br_read_ue(br);
    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

/* Parse SPS to extract resolution and log it */
static void parse_sps_resolution(const uint8_t *sps, int len)
{
    if (g_sps_logged || len < 4) return;
    g_sps_logged = 1;

    /* Skip NAL header byte (1 byte) */
    bitreader_t br = { sps + 1, len - 1, 0 };

    unsigned profile_idc = br_read_bits(&br, 8);
    br_read_bits(&br, 8); /* constraint flags + reserved */
    unsigned level_idc = br_read_bits(&br, 8);
    br_read_ue(&br); /* seq_parameter_set_id */

    /* High profile: parse chroma/scaling */
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128) {
        unsigned chroma = br_read_ue(&br);
        if (chroma == 3) br_read_bits(&br, 1);
        br_read_ue(&br); /* bit_depth_luma */
        br_read_ue(&br); /* bit_depth_chroma */
        br_read_bits(&br, 1); /* qpprime */
        if (br_read_bits(&br, 1)) { /* scaling_matrix_present */
            int i, cnt = (chroma != 3) ? 8 : 12;
            for (i = 0; i < cnt; i++) {
                if (br_read_bits(&br, 1)) { /* list present */
                    int j, sz = (i < 6) ? 16 : 64, last = 8, next;
                    for (j = 0; j < sz; j++) {
                        if (last) { next = (last + br_read_se(&br) + 256) % 256; }
                        last = next ? next : last;
                    }
                }
            }
        }
    }

    br_read_ue(&br); /* log2_max_frame_num */
    unsigned poc_type = br_read_ue(&br);
    if (poc_type == 0) {
        br_read_ue(&br);
    } else if (poc_type == 1) {
        br_read_bits(&br, 1);
        br_read_se(&br);
        br_read_se(&br);
        unsigned n = br_read_ue(&br);
        unsigned i;
        for (i = 0; i < n; i++) br_read_se(&br);
    }

    br_read_ue(&br); /* max_num_ref_frames */
    br_read_bits(&br, 1); /* gaps_in_frame_num */
    unsigned mb_w = br_read_ue(&br) + 1;
    unsigned mb_h = br_read_ue(&br) + 1;
    unsigned frame_mbs_only = br_read_bits(&br, 1);
    if (!frame_mbs_only) br_read_bits(&br, 1); /* mb_adaptive */
    br_read_bits(&br, 1); /* direct_8x8 */

    unsigned crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (br_read_bits(&br, 1)) { /* cropping */
        crop_left   = br_read_ue(&br);
        crop_right  = br_read_ue(&br);
        crop_top    = br_read_ue(&br);
        crop_bottom = br_read_ue(&br);
    }

    unsigned width  = mb_w * 16 - (crop_left + crop_right) * 2;
    unsigned height = mb_h * 16 * (2 - frame_mbs_only)
                    - (crop_top + crop_bottom) * 2 * (2 - frame_mbs_only);

    printf("[rtsp] *** SPS: profile=%u level=%u resolution=%ux%u ***\n",
           profile_idc, level_idc, width, height);
    printf("[rtsp] *** preview_stream limit: 240x240. %s ***\n",
           (width <= 240 && height <= 240) ? "OK" : "TOO LARGE - will not display!");
}

/* Write one complete NAL unit to FIFO (Annex-B) */
static void write_nal(int fifo_fd, const uint8_t *nal, int len)
{
    if (len <= 0) return;

    /* Detect SPS (NAL type 7) and parse resolution */
    if ((nal[0] & 0x1F) == 7)
        parse_sps_resolution(nal, len);

    write(fifo_fd, start_code, 4);
    write(fifo_fd, nal, len);
}

/* Process one RTP packet payload (H264) */
static void process_rtp_payload(int fifo_fd, const uint8_t *payload, int plen)
{
    if (plen < 1) return;
    uint8_t nal_type = payload[0] & 0x1F;

    if (nal_type >= 1 && nal_type <= 23) {
        /* Single NAL unit */
        write_nal(fifo_fd, payload, plen);
    } else if (nal_type == 24) {
        /* STAP-A: multiple NAL units */
        int i = 1;
        while (i + 2 < plen) {
            int nsize = ((int)payload[i] << 8) | payload[i+1];
            i += 2;
            if (i + nsize <= plen)
                write_nal(fifo_fd, payload + i, nsize);
            i += nsize;
        }
    } else if (nal_type == 28) {
        /* FU-A */
        if (plen < 2) return;
        uint8_t fu_hdr  = payload[1];
        int     start   = (fu_hdr >> 7) & 1;
        int     end     = (fu_hdr >> 6) & 1;
        uint8_t fu_type = fu_hdr & 0x1F;

        if (start) {
            nal_len = 0;
            nal_fua_type = fu_type;
            /* reconstruct NAL header: forbidden_zero(1)|nal_ref_idc(2)|nal_unit_type(5) */
            nal_buf[nal_len++] = (payload[0] & 0xE0) | fu_type;
        }
        /* Append fragment data (skip 2-byte FU indicator+header) */
        int frag_len = plen - 2;
        if (nal_len + frag_len < MAX_NAL_SIZE) {
            memcpy(nal_buf + nal_len, payload + 2, frag_len);
            nal_len += frag_len;
        }
        if (end && nal_len > 0) {
            write_nal(fifo_fd, nal_buf, nal_len);
            nal_len = 0;
        }
    }
    /* nal_type 25-27, 29-31: ignore */
}

/* ── interleaved RTP reader ── */
static volatile int g_stop = 0;

static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: rtsp_client <rtsp_url>\n");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    rtsp_url_t url;
    if (parse_url(argv[1], &url) < 0) {
        fprintf(stderr, "[rtsp] bad URL: %s\n", argv[1]);
        return 1;
    }
    printf("[rtsp] host=%s port=%d path=%s user=%s\n",
           url.host, url.port, url.path, url.user);

    /* ── open FIFO ── */
    mkfifo(FIFO_PATH, 0666);
    printf("[rtsp] opening FIFO %s ...\n", FIFO_PATH);
    /* Open non-blocking first so we don't block waiting for reader */
    int fifo_fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        /* Try waiting a moment for preview_stream to start */
        sleep(1);
        fifo_fd = open(FIFO_PATH, O_WRONLY);
        if (fifo_fd < 0) {
            perror("[rtsp] open FIFO");
            return 1;
        }
    }
    /* Switch back to blocking for smooth writes */
    int flags = fcntl(fifo_fd, F_GETFL);
    fcntl(fifo_fd, F_SETFL, flags & ~O_NONBLOCK);
    printf("[rtsp] FIFO open OK fd=%d\n", fifo_fd);

    /* ── open power key device and drain stale events ── */
    int key_fd = open(POWERKEY_DEV, O_RDONLY | O_NONBLOCK);
    if (key_fd >= 0) {
        struct input_event ev;
        /* Drain all queued events so a boot KEY_POWER won't immediately stop us */
        while (read(key_fd, &ev, sizeof(ev)) == sizeof(ev)) {}
    }

    /* ── TCP connect ── */
    int tcp_fd = tcp_connect(url.host, url.port);
    if (tcp_fd < 0) return 1;
    printf("[rtsp] TCP connected\n");

    rtsp_t rtsp;
    memset(&rtsp, 0, sizeof(rtsp));
    rtsp.fd = tcp_fd;
    snprintf(rtsp.base_url, sizeof(rtsp.base_url), "rtsp://%s:%d%s",
             url.host, url.port, url.path);

    /* Build Basic auth header if credentials present */
    if (url.user[0]) {
        char creds[256];
        snprintf(creds, sizeof(creds), "%s:%s", url.user, url.pass);
        char b64creds[512];
        base64_encode((uint8_t *)creds, strlen(creds), b64creds);
        snprintf(rtsp.auth, sizeof(rtsp.auth),
                 "Authorization: Basic %s\r\n", b64creds);
    }

    char resp[RESP_SIZE];
    char val[512];

    /* OPTIONS */
    rtsp_request(&rtsp, "OPTIONS", rtsp.base_url, NULL, resp, sizeof(resp));

    /* DESCRIBE */
    rtsp_request(&rtsp, "DESCRIBE", rtsp.base_url,
                 "Accept: application/sdp\r\n", resp, sizeof(resp));

    /* Parse Content-Base or use base_url as track base */
    char track_url[1024];
    if (get_header(resp, "Content-Base", val, sizeof(val)))
        snprintf(track_url, sizeof(track_url), "%strackID=1", val);
    else
        snprintf(track_url, sizeof(track_url), "%s/trackID=1", rtsp.base_url);

    /* SETUP — request interleaved (TCP) transport */
    char setup_extra[256];
    snprintf(setup_extra, sizeof(setup_extra),
             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    rtsp_request(&rtsp, "SETUP", track_url, setup_extra, resp, sizeof(resp));

    /* Extract session ID */
    if (get_header(resp, "Session", val, sizeof(val))) {
        /* session may have timeout: strip it */
        char *semi = strchr(val, ';');
        if (semi) *semi = '\0';
        snprintf(rtsp.session, sizeof(rtsp.session), "Session: %s\r\n", val);
        printf("[rtsp] session: %s\n", val);
    }

    /* PLAY */
    rtsp_request(&rtsp, "PLAY", rtsp.base_url, "Range: npt=0.000-\r\n",
                 resp, sizeof(resp));

    printf("[rtsp] streaming started. Press power button to stop.\n");

    /* ── main receive loop ── */
    uint8_t *pkt_buf = malloc(BUF_SIZE);
    if (!pkt_buf) { fprintf(stderr, "[rtsp] OOM\n"); return 1; }

    /* interleaved read state */
    uint8_t  ihdr[4];   /* '$' + channel(1) + length(2) */
    int      ihdr_got = 0;
    int      pkt_expected = 0;
    int      pkt_got = 0;

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tcp_fd, &rfds);
        if (key_fd >= 0) FD_SET(key_fd, &rfds);
        int maxfd = tcp_fd > key_fd ? tcp_fd : key_fd;

        struct timeval tv = {1, 0};
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue; /* timeout, send OPTIONS keepalive */

        /* ── key press? → stop ── */
        if (key_fd >= 0 && FD_ISSET(key_fd, &rfds)) {
            struct input_event ev;
            while (read(key_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1) {
                    printf("[rtsp] power key pressed, stopping\n");
                    g_stop = 1;
                }
            }
            if (g_stop) break;
        }

        /* ── RTSP/TCP data ── */
        if (!FD_ISSET(tcp_fd, &rfds)) continue;

        if (pkt_expected == 0) {
            /* Reading interleaved header: '$', channel, len_hi, len_lo */
            while (ihdr_got < 4) {
                int n = read(tcp_fd, ihdr + ihdr_got, 4 - ihdr_got);
                if (n <= 0) { g_stop = 1; break; }
                ihdr_got += n;
            }
            if (g_stop) break;
            if (ihdr[0] != '$') {
                /* RTSP response line — drain until blank line */
                char c;
                int crlf = 0;
                while (!g_stop && crlf < 4) {
                    if (read(tcp_fd, &c, 1) != 1) { g_stop = 1; break; }
                    if (c == '\r' || c == '\n') crlf++;
                    else crlf = 0;
                }
                ihdr_got = 0;
                continue;
            }
            pkt_expected = ((int)ihdr[2] << 8) | ihdr[3];
            pkt_got = 0;
            ihdr_got = 0;
            if (pkt_expected > BUF_SIZE) {
                /* oversize, skip */
                int skip = pkt_expected;
                while (skip > 0 && !g_stop) {
                    int r = read(tcp_fd, pkt_buf,
                                 skip < BUF_SIZE ? skip : BUF_SIZE);
                    if (r <= 0) { g_stop = 1; break; }
                    skip -= r;
                }
                pkt_expected = 0;
                continue;
            }
        }

        /* Reading packet body */
        while (pkt_got < pkt_expected && !g_stop) {
            int n = read(tcp_fd, pkt_buf + pkt_got, pkt_expected - pkt_got);
            if (n <= 0) { g_stop = 1; break; }
            pkt_got += n;
        }
        if (g_stop) break;

        /* channel 0 = RTP video, channel 1 = RTCP (ignore) */
        if (ihdr[1] == 0 && pkt_got >= RTP_HDR_SIZE) {
            uint8_t *payload = pkt_buf + RTP_HDR_SIZE;
            int      paylen  = pkt_got - RTP_HDR_SIZE;
            /* skip CSRC extensions */
            int cc = pkt_buf[0] & 0x0F;
            payload += cc * 4;
            paylen  -= cc * 4;
            /* skip RTP extension header if X bit set */
            if ((pkt_buf[0] & 0x10) && paylen >= 4) {
                int ext_len = (((int)payload[2] << 8) | payload[3]) * 4 + 4;
                payload += ext_len;
                paylen  -= ext_len;
            }
            if (paylen > 0)
                process_rtp_payload(fifo_fd, payload, paylen);
        }

        pkt_expected = 0;
    }

    /* TEARDOWN */
    rtsp_request(&rtsp, "TEARDOWN", rtsp.base_url, NULL, resp, sizeof(resp));

    free(pkt_buf);
    close(tcp_fd);
    if (key_fd >= 0) close(key_fd);
    close(fifo_fd);
    printf("[rtsp] done\n");
    return 0;
}
