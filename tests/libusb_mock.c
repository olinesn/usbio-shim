/* A stand-in for libusb that simulates USB devices, so usbio-helper can be tested end to end
 * (Wine + shim + helper) with no hardware. Link it instead of -lusb-1.0.
 *
 * Simulated: MOCK_DEVICES (env, default 1) high-speed devices 0547:1012, serials MOCK0001...,
 * one configuration whose interfaces come from MOCK_LAYOUT (env):
 *   test  (default) interface 0: alt 0 none, alt 1 = iso IN 0x82 (1024) + bulk OUT 0x02 + bulk IN 0x86
 *   1216h our guess at the UltraSoundGate 1216H, from what RECORDER USGH asks for: interface 0
 *         alts 0-2 (alt 2 = iso IN 0x82, 3 x 1024 bytes per microframe), interface 1 alt 0 with
 *         bulk OUT 0x01 / IN 0x81 (64). Unknown parts are guesses; RECORDER's requests will tell.
 *  - Isochronous IN streams in real time: one packet per 125 us microframe, MOCK_ISO_BYTES
 *    (env, default 750 = 12 channels x 2 bytes x 250 kHz / 8000) per packet, filled with a
 *    little-endian uint16 counter that continues across packets and transfers. Microframes with
 *    no transfer queued are lost, as on a real bus (counted in the log at exit).
 *    MOCK_ISO_MODE=frames instead sends whole sample frames like a multichannel recorder:
 *    MOCK_CHANNELS (12) x 16-bit samples at MOCK_RATE (250000) Hz, so packets of 31 or 32 frames
 *    (744/768 bytes) averaging the rate; channel c carries a sine of (c+1) kHz, LSB 0.
 *    MOCK_LOG_GAPS=1 logs each run of lost microframes.
 *  - Bulk IN returns whatever the last bulk OUT sent (echo).
 *  - Vendor IN requests return bytes (bRequest, wValue, wIndex, 0xA5...); vendor OUT requests are
 *    logged. Standard GET_DESCRIPTOR answers device, configuration and string descriptors.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <libusb.h>

struct libusb_context { int dummy; };
struct libusb_device { int index; int refs; };
struct libusb_device_handle {
    struct libusb_device *dev;
    int config, claimed[4], alt[4];
    uint8_t echo[4096]; int echo_len;
    uint64_t iso_counter;   /* next uint16 counter value of the stream */
    uint64_t microframe;    /* microframes streamed (frames mode) */
    double iso_next;        /* time of the next microframe */
    uint64_t lost_frames;
};

static struct libusb_device devs[8];
static int ndevs = -1;
static unsigned iso_bytes = 750;
static int frames_mode, channels = 12, log_gaps;
static unsigned rate = 250000;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
struct qitem { struct libusb_transfer *t; double due; struct qitem *next; int cancelled; };
static struct qitem *queue;      /* submitted, not completed (sorted by due time) */

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static void mlog(const char *f, ...) { va_list a; va_start(a, f); fprintf(stderr, "[mock] "); vfprintf(stderr, f, a); fputc('\n', stderr); va_end(a); }

static void setup(void)
{
    const char *n = getenv("MOCK_DEVICES"), *b = getenv("MOCK_ISO_BYTES");
    int i;
    if (ndevs >= 0) return;
    ndevs = n ? atoi(n) : 1;
    if (ndevs > 8) ndevs = 8;
    if (b) iso_bytes = (unsigned)atoi(b);
    if (getenv("MOCK_ISO_MODE") && !strcmp(getenv("MOCK_ISO_MODE"), "frames")) frames_mode = 1;
    if (getenv("MOCK_CHANNELS")) channels = atoi(getenv("MOCK_CHANNELS"));
    if (getenv("MOCK_RATE")) rate = (unsigned)atoi(getenv("MOCK_RATE"));
    log_gaps = getenv("MOCK_LOG_GAPS") != NULL;
    for (i = 0; i < ndevs; i++) devs[i].index = i;
}

static const uint8_t dev_desc[18] = {18, 1, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 0x47, 0x05, 0x12, 0x10, 0x00, 0x01, 1, 2, 3, 1};

struct ep_spec { uint8_t addr, attr; uint16_t maxpkt; uint8_t interval; };
struct alt_spec { uint8_t iface, alt, nep; struct ep_spec ep[4]; };
static const struct alt_spec layout_test[] = {
    {0, 0, 0, {{0}}},
    {0, 1, 3, {{0x82, 1, 0x0400, 1}, {0x02, 2, 0x0200, 0}, {0x86, 2, 0x0200, 0}}},
};
static const struct alt_spec layout_1216h[] = {
    {0, 0, 0, {{0}}},
    {0, 1, 1, {{0x82, 1, 0x0400, 1}}},
    {0, 2, 1, {{0x82, 1, 0x1400, 1}}},
    {1, 0, 2, {{0x01, 2, 0x0040, 0}, {0x81, 2, 0x0040, 0}}},
};
static const struct alt_spec *layout = layout_test;
static int nalts = 2, nifaces = 1;

/* ---- the configuration descriptor, raw and parsed (only what usbio-helper reads) -------------- */
static uint8_t cfg_desc[512];
static int cfg_len;
static struct libusb_endpoint_descriptor eps[16][4];
static struct libusb_interface_descriptor alts[16];
static struct libusb_interface itfs[4];
static struct libusb_config_descriptor cfg;
static void build_cfg(void)
{
    const char *l = getenv("MOCK_LAYOUT");
    int i, e, k = 9;
    if (l && !strcmp(l, "1216h")) { layout = layout_1216h; nalts = 4; nifaces = 2; }
    memset(&cfg, 0, sizeof(cfg));
    for (i = 0; i < nalts; i++) {
        const struct alt_spec *a = &layout[i];
        uint8_t *d = cfg_desc + k;
        d[0] = 9; d[1] = 4; d[2] = a->iface; d[3] = a->alt; d[4] = a->nep; d[5] = 0xff; d[6] = d[7] = d[8] = 0;
        k += 9;
        memset(&alts[i], 0, sizeof(alts[i]));
        alts[i].bLength = 9; alts[i].bDescriptorType = 4; alts[i].bInterfaceNumber = a->iface; alts[i].bAlternateSetting = a->alt;
        alts[i].bNumEndpoints = a->nep; alts[i].bInterfaceClass = 0xff; alts[i].endpoint = eps[i];
        for (e = 0; e < a->nep; e++) {
            const struct ep_spec *s = &a->ep[e];
            d = cfg_desc + k;
            d[0] = 7; d[1] = 5; d[2] = s->addr; d[3] = s->attr; d[4] = (uint8_t)s->maxpkt; d[5] = (uint8_t)(s->maxpkt >> 8); d[6] = s->interval;
            k += 7;
            eps[i][e].bLength = 7; eps[i][e].bDescriptorType = 5; eps[i][e].bEndpointAddress = s->addr;
            eps[i][e].bmAttributes = s->attr; eps[i][e].wMaxPacketSize = s->maxpkt; eps[i][e].bInterval = s->interval;
        }
    }
    cfg_len = k;
    cfg_desc[0] = 9; cfg_desc[1] = 2; cfg_desc[2] = (uint8_t)k; cfg_desc[3] = (uint8_t)(k >> 8);
    cfg_desc[4] = (uint8_t)nifaces; cfg_desc[5] = 1; cfg_desc[6] = 0; cfg_desc[7] = 0x80; cfg_desc[8] = 250;
    cfg.bLength = 9; cfg.bDescriptorType = 2; cfg.wTotalLength = (uint16_t)k; cfg.bNumInterfaces = (uint8_t)nifaces; cfg.bConfigurationValue = 1;
    for (i = 0; i < nifaces; i++) {
        int first = -1, n = 0, j;
        for (j = 0; j < nalts; j++) if (layout[j].iface == i) { if (first < 0) first = j; n++; }
        itfs[i].altsetting = &alts[first];
        itfs[i].num_altsetting = n;
    }
    cfg.interface = itfs;
}

/* which interface/alt an endpoint belongs to in the current settings */
static int ep_active(libusb_device_handle *h, uint8_t ep);

int LIBUSB_CALL libusb_init(libusb_context **ctx) { static libusb_context c; setup(); build_cfg(); if (ctx) *ctx = &c; return 0; }
const char * LIBUSB_CALL libusb_strerror(int code) { static char b[32]; snprintf(b, sizeof(b), "mock error %d", code); return b; }
const struct libusb_version * LIBUSB_CALL libusb_get_version(void) { static struct libusb_version v = {1, 0, 0, 0, "", "mock"}; return &v; }

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    int i;
    *list = calloc((size_t)ndevs + 1, sizeof(**list));
    for (i = 0; i < ndevs; i++) (*list)[i] = &devs[i];
    return ndevs;
}
void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref) { free(list); }
libusb_device * LIBUSB_CALL libusb_ref_device(libusb_device *d) { return d; }
void LIBUSB_CALL libusb_unref_device(libusb_device *d) {}
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *dd) { memcpy(dd, dev_desc, 18); return 0; }
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *d) { return 1; }
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *d, uint8_t *p, int n) { if (n > 0) p[0] = (uint8_t)(d->index + 1); return 1; }
int LIBUSB_CALL libusb_get_device_speed(libusb_device *d) { return LIBUSB_SPEED_HIGH; }

int LIBUSB_CALL libusb_open(libusb_device *d, libusb_device_handle **h)
{
    *h = calloc(1, sizeof(**h));
    (*h)->dev = d;
    (*h)->config = 1;
    return 0;
}
void LIBUSB_CALL libusb_close(libusb_device_handle *h)
{
    if (h->lost_frames) mlog("device %d: %llu microframes had no transfer queued (data lost)", h->dev->index, (unsigned long long)h->lost_frames);
    free(h);
}
int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { return 0; }
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *h, int i) { if (i < 0 || i >= nifaces) return LIBUSB_ERROR_NOT_FOUND; h->claimed[i] = 1; return 0; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *h, int i) { if (i >= 0 && i < 4) h->claimed[i] = 0; return 0; }
static int ep_active(libusb_device_handle *h, uint8_t ep)
{
    int i, e;
    for (i = 0; i < nalts; i++)
        for (e = 0; e < layout[i].nep; e++)
            if (layout[i].ep[e].addr == ep && h->claimed[layout[i].iface] && h->alt[layout[i].iface] == layout[i].alt) return 1;
    return 0;
}
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *h, int *c) { *c = h->config; return 0; }
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *h, int c) { mlog("SET_CONFIGURATION %d", c); h->config = c < 0 ? 0 : c; return 0; }
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a)
{
    int j;
    for (j = 0; j < nalts; j++) if (layout[j].iface == i && layout[j].alt == a) break;
    if (j == nalts || i >= 4) return LIBUSB_ERROR_NOT_FOUND;
    mlog("SET_INTERFACE %d alt %d", i, a);
    h->alt[i] = a;
    return 0;
}
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *d, struct libusb_config_descriptor **c) { *c = &cfg; return 0; }
int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *d, uint8_t i, struct libusb_config_descriptor **c) { if (i) return LIBUSB_ERROR_NOT_FOUND; *c = &cfg; return 0; }
void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *c) {}
int LIBUSB_CALL libusb_reset_device(libusb_device_handle *h) { mlog("reset"); memset(h->alt, 0, sizeof(h->alt)); return 0; }
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *h, unsigned char ep) { return 0; }

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, unsigned char *data, uint16_t len, unsigned int timeout)
{
    if (rt == 0x80 && req == LIBUSB_REQUEST_GET_DESCRIPTOR) {
        int type = val >> 8, n;
        if (type == 1) { n = len < 18 ? len : 18; memcpy(data, dev_desc, (size_t)n); return n; }
        if (type == 2) { n = len < cfg_len ? len : cfg_len; memcpy(data, cfg_desc, (size_t)n); return n; }
        if (type == 3) {
            char s[32];
            int i, k = 2;
            if ((val & 0xff) == 0) { uint8_t lang[4] = {4, 3, 0x09, 0x04}; n = len < 4 ? len : 4; memcpy(data, lang, (size_t)n); return n; }
            snprintf(s, sizeof(s), (val & 0xff) == 1 ? "Avisoft (simulated)" : (val & 0xff) == 2 ? "UltraSoundGate 1216H sim" : "MOCK%04d", h->dev->index + 1);
            for (i = 0; s[i] && k + 2 <= len && k < 64; i++) { data[k++] = (uint8_t)s[i]; data[k++] = 0; }
            if (len >= 2) { data[0] = (uint8_t)k; data[1] = 3; }
            return k;
        }
        return LIBUSB_ERROR_PIPE;
    }
    if (rt == 0x80 && req == LIBUSB_REQUEST_GET_STATUS) { if (len >= 2) { data[0] = 0; data[1] = 0; } return len < 2 ? len : 2; }
    if ((rt & 0x60) == 0x40) {   /* vendor */
        if (rt & 0x80) {
            int i;
            for (i = 0; i < len; i++) data[i] = i == 0 ? req : i == 1 ? (uint8_t)val : i == 2 ? (uint8_t)(val >> 8) : i == 3 ? (uint8_t)idx : 0xA5;
            mlog("vendor IN  req %02x val %04x idx %04x len %u", req, val, idx, len);
            return len;
        }
        mlog("vendor OUT req %02x val %04x idx %04x len %u", req, val, idx, len);
        return len;
    }
    return len;
}

int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t i, unsigned char *out, int n)
{
    return snprintf((char *)out, (size_t)n, "MOCK%04d", h->dev->index + 1);
}

/* ---- transfers ---------------------------------------------------------------------------------- */
struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    return calloc(1, sizeof(struct libusb_transfer) + (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));
}
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t) { free(t); }

static void enqueue(struct qitem *q)
{
    struct qitem **pp = &queue;
    while (*pp && (*pp)->due <= q->due) pp = &(*pp)->next;
    q->next = *pp;
    *pp = q;
    pthread_cond_broadcast(&cv);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t)
{
    struct qitem *q = calloc(1, sizeof(*q));
    libusb_device_handle *h = t->dev_handle;
    double tn = now();
    q->t = t;
    pthread_mutex_lock(&mu);
    if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        if (!ep_active(h, t->endpoint)) { pthread_mutex_unlock(&mu); free(q); return LIBUSB_ERROR_NOT_FOUND; }
        /* the stream's clock: packets go to the next free microframes, never into the past */
        if (h->iso_next < tn) {
            if (h->iso_next > 0) {
                uint64_t lost = (uint64_t)((tn - h->iso_next) / 125e-6);
                h->lost_frames += lost;
                if (log_gaps && lost) mlog("device %d: %llu microframes lost (no transfer queued)", h->dev->index, (unsigned long long)lost);
                h->iso_counter += lost * (iso_bytes / 2);
            }
            h->iso_next = tn;
        }
        h->iso_next += t->num_iso_packets * 125e-6;
        q->due = h->iso_next;
    } else {
        q->due = tn + 0.0005;
    }
    enqueue(q);
    pthread_mutex_unlock(&mu);
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t)
{
    struct qitem *q = NULL;
    pthread_mutex_lock(&mu);
    struct qitem **pp;
    for (pp = &queue; *pp; pp = &(*pp)->next)
        if ((*pp)->t == t) { q = *pp; *pp = q->next; q->cancelled = 1; q->due = 0; q->next = queue; queue = q; break; }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 0;
}

static void complete(struct qitem *q)
{
    struct libusb_transfer *t = q->t;
    libusb_device_handle *h = t->dev_handle;
    int i;
    if (q->cancelled) {
        t->status = LIBUSB_TRANSFER_CANCELLED;
        for (i = 0; i < t->num_iso_packets; i++) { t->iso_packet_desc[i].actual_length = 0; t->iso_packet_desc[i].status = LIBUSB_TRANSFER_CANCELLED; }
        return;
    }
    t->status = LIBUSB_TRANSFER_COMPLETED;
    if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        unsigned char *p = t->buffer;
        for (i = 0; i < t->num_iso_packets && frames_mode; i++) {
            uint64_t m = h->microframe++, f0 = m * rate / 8000, f1 = (m + 1) * rate / 8000, f;
            unsigned n = (unsigned)(f1 - f0) * (unsigned)channels * 2, k = 0;
            if (n > t->iso_packet_desc[i].length) n = t->iso_packet_desc[i].length - t->iso_packet_desc[i].length % (unsigned)(channels * 2);
            for (f = f0; k < n; f++) {
                int c;
                for (c = 0; c < channels; c++, k += 2) {
                    double ph = 2 * 3.141592653589793 * (c + 1) * 1000.0 * (double)f / rate;
                    int16_t v = (int16_t)(1000.0 * __builtin_sin(ph)) & ~1;
                    p[k] = (uint8_t)v; p[k + 1] = (uint8_t)((uint16_t)v >> 8);
                }
            }
            t->iso_packet_desc[i].actual_length = n;
            t->iso_packet_desc[i].status = LIBUSB_TRANSFER_COMPLETED;
            p += t->iso_packet_desc[i].length;
        }
        for (i = 0; i < t->num_iso_packets && !frames_mode; i++) {
            unsigned n = iso_bytes < t->iso_packet_desc[i].length ? iso_bytes : t->iso_packet_desc[i].length, k;
            for (k = 0; k + 1 < n; k += 2) { uint16_t v = (uint16_t)h->iso_counter++; p[k] = (uint8_t)v; p[k + 1] = (uint8_t)(v >> 8); }
            t->iso_packet_desc[i].actual_length = n;
            t->iso_packet_desc[i].status = LIBUSB_TRANSFER_COMPLETED;
            p += t->iso_packet_desc[i].length;
        }
    } else if (t->endpoint & 0x80) {
        int n = h->echo_len < t->length ? h->echo_len : t->length;
        memcpy(t->buffer, h->echo, (size_t)n);
        t->actual_length = n;
    } else {
        h->echo_len = t->length < (int)sizeof(h->echo) ? t->length : (int)sizeof(h->echo);
        memcpy(h->echo, t->buffer, (size_t)h->echo_len);
        t->actual_length = t->length;
    }
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
    double limit = now() + (tv ? tv->tv_sec + tv->tv_usec / 1e6 : 1.0);
    struct qitem *ready = NULL, **tail = &ready;
    pthread_mutex_lock(&mu);
    for (;;) {
        double tn = now();
        while (queue && queue->due <= tn) {
            struct qitem *q = queue;
            queue = q->next;
            complete(q);
            q->next = NULL;
            *tail = q;
            tail = &q->next;
        }
        if (ready || tn >= limit) break;
        {
            double wake = queue && queue->due < limit ? queue->due : limit;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            wake -= tn;
            ts.tv_sec += (time_t)wake;
            ts.tv_nsec += (long)((wake - (time_t)wake) * 1e9);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&cv, &mu, &ts);
        }
    }
    pthread_mutex_unlock(&mu);
    while (ready) {
        struct qitem *q = ready;
        ready = q->next;
        q->t->callback(q->t);
        free(q);
    }
    return 0;
}
