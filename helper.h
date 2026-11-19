/* usbio-helper internals shared by helper.c (server, devices, transfers) and usbio.c (the
 * USBIO request semantics). */
#ifndef USBIO_HELPER_H
#define USBIO_HELPER_H
#include <pthread.h>
#include <stdint.h>
#include <libusb.h>
#include "proto.h"

#define MAX_PIPES 32
#define MAX_IFACES 8

struct pipe_info {
    uint8_t ep;          /* endpoint address */
    uint8_t type;        /* LIBUSB_TRANSFER_TYPE_* */
    uint16_t maxpkt;     /* wMaxPacketSize as in the descriptor (incl. high-bandwidth bits) */
    uint8_t interval;
    uint8_t iface, alt;
    uint32_t flags;      /* USBIO pipe parameters (e.g. short transfer ok) */
};

struct device {
    struct device *next;
    libusb_device *dev;
    libusb_device_handle *h;
    uint16_t vid, pid;
    uint32_t index;
    int refs;
    pthread_mutex_t lock;
    int configured;
    uint8_t cfg_value;
    int n_ifs;
    struct { int num, alt, claimed; } ifs[MAX_IFACES];
    int npipes;
    struct pipe_info pipes[MAX_PIPES];
    uint32_t dev_options, request_timeout_ms, power_state;
    uint32_t max_transfer[MAX_IFACES];
    struct conn *conns[64];     /* open handles on this device (guarded by lock) */
    char serial[64];
};

struct xfer;

struct conn {
    int fd;
    pthread_mutex_t send_lock;
    pthread_mutex_t lock;       /* pending list, binding */
    struct device *dev;
    int bound;
    struct pipe_info pipe;      /* copy of the bound pipe */
    struct xfer *pending;
    int closing;
    int refs;                   /* 1 for the reader thread + 1 per pending transfer */
    int id;                     /* for logs */
    /* pipe statistics (QUERY_PIPE_STATISTICS) */
    uint64_t st_bytes, st_bytes_window;
    uint32_t st_ok, st_failed, st_interval_ms;
    double st_window_start;
    uint64_t iso_next_uf;       /* microframe (125 us) where the next queued iso transfer starts */
};

struct xfer {
    struct xfer *next;
    struct conn *c;
    uint32_t id, op;
    struct libusb_transfer *t;
    uint8_t *buf;               /* image of the application's buffer */
    uint32_t len;               /* application buffer length */
    uint32_t hdr_len;           /* iso: header bytes at the start of buf */
    uint8_t *ubuf;              /* iso: libusb's contiguous packet buffer */
    uint32_t *slot;             /* iso: where packet i lives in buf (USBIO Offset) */
    uint32_t *cap;              /* iso: bytes requested for packet i */
    uint64_t start_uf;          /* iso: microframe of packet 0 */
};

extern int verbose;
void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void send_reply(struct conn *c, uint32_t op, uint32_t id, uint32_t error, uint32_t bytes, const void *payload, uint32_t plen);
uint32_t conn_flags(struct conn *c);

/* usbio.c */
uint32_t usbio_ioctl(struct conn *c, uint32_t code, const uint8_t *in, uint32_t in_len,
                     uint8_t *out, uint32_t out_len, uint32_t *bytes);
uint32_t usbio_start_read_write(struct conn *c, uint32_t op, uint32_t id, const uint8_t *pay, uint32_t plen, uint32_t len);
void usbio_cancel_all(struct conn *c);
void usbio_conn_closed(struct conn *c);
void device_rebuild_pipes(struct device *d);
void helper_conn_unref(struct conn *c);
void device_add_conn(struct device *d, struct conn *c);

/* Win32 / USBIO error codes the helper returns (GetLastError() in the application) */
#define ERROR_GEN_FAILURE_ 31u
#define ERROR_NOT_SUPPORTED_ 50u
#define ERROR_INVALID_PARAMETER_ 87u
#define ERROR_INSUFFICIENT_BUFFER_ 122u
#define ERROR_OPERATION_ABORTED_ 995u
#define ERROR_DEVICE_NOT_CONNECTED_ 1167u
#endif
