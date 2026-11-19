/* usbio-helper: the native half of usbio-shim. Listens on 127.0.0.1, owns the USB devices
 * through libusb, and answers the USBIO requests that RECORDER USGH (under Wine) sends
 * through the shim DLL. Linux and macOS.
 *
 *   usbio-helper [-p PORT] [-v] [-o SERIAL1,SERIAL2,...] [-V VID]
 *
 *   -o  device order: RECORDER treats the first UltraSoundGate in its list as the SYNC master,
 *       so list the master's serial number first. Default: by USB bus and port.
 *   -V  vendor id to serve (default 0x0547, Avisoft's UltraSoundGate xx16H series); for tests.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "helper.h"

int verbose;
static uint16_t serve_vid = 0x0547;
static char *order_list;
static libusb_context *ctx;
static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;
static struct device *devices;
static int conn_counter;

void logmsg(const char *fmt, ...)
{
    struct timespec ts;
    va_list ap;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%6ld.%03ld] ", (long)ts.tv_sec % 1000000, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *c = p;
    while (n) {
        ssize_t k = send(fd, c, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}
static int read_all(int fd, void *p, size_t n)
{
    uint8_t *c = p;
    while (n) {
        ssize_t k = recv(fd, c, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}

uint32_t conn_flags(struct conn *c)
{
    return (c->bound && c->pipe.type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) ? SHIM_F_ISO_PIPE : 0;
}

void send_reply(struct conn *c, uint32_t op, uint32_t id, uint32_t error, uint32_t bytes, const void *payload, uint32_t plen)
{
    struct shim_msg m = {SHIM_MAGIC, op, id, 0, 0, plen, error, bytes, 0};
    m.flags = conn_flags(c);
    pthread_mutex_lock(&c->send_lock);
    if (write_all(c->fd, &m, sizeof(m)) == 0 && plen) write_all(c->fd, payload, plen);
    pthread_mutex_unlock(&c->send_lock);
}

/* ---- device discovery ------------------------------------------------------------------------- */
struct found { libusb_device *dev; uint16_t pid; uint8_t bus, ports[7]; int nports; char serial[64]; };

static void read_serial(libusb_device *dev, const struct libusb_device_descriptor *dd, char *out, size_t n)
{
    libusb_device_handle *h;
    out[0] = 0;
    if (!dd->iSerialNumber || libusb_open(dev, &h)) return;
    if (libusb_get_string_descriptor_ascii(h, dd->iSerialNumber, (unsigned char *)out, (int)n) < 0) out[0] = 0;
    libusb_close(h);
}

static int order_rank(const char *serial)
{
    const char *p;
    int rank = 0;
    size_t n = strlen(serial);
    if (!order_list || !n) return 1000;
    for (p = order_list; *p; rank++) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, serial, n)) return rank;
        if (!e) break;
        p = e + 1;
    }
    return 1000;
}

static int cmp_found(const void *a, const void *b)
{
    const struct found *x = a, *y = b;
    int rx = order_rank(x->serial), ry = order_rank(y->serial), i;
    if (rx != ry) return rx - ry;
    if (x->bus != y->bus) return x->bus - y->bus;
    for (i = 0; i < 7; i++) if (x->ports[i] != y->ports[i]) return x->ports[i] - y->ports[i];
    return 0;
}

/* All attached devices of serve_vid:pid in the stable order. Caller frees list with libusb_free_device_list. */
static int find_devices(uint16_t pid, struct found *out, int max, libusb_device ***list)
{
    ssize_t n = libusb_get_device_list(ctx, list), i;
    int k = 0;
    for (i = 0; i < n && k < max; i++) {
        struct libusb_device_descriptor dd;
        libusb_device *dev = (*list)[i];
        if (libusb_get_device_descriptor(dev, &dd) || dd.idVendor != serve_vid || dd.idProduct != pid) continue;
        memset(&out[k], 0, sizeof(out[k]));
        out[k].dev = dev;
        out[k].pid = pid;
        out[k].bus = libusb_get_bus_number(dev);
        out[k].nports = libusb_get_port_numbers(dev, out[k].ports, 7);
        read_serial(dev, &dd, out[k].serial, sizeof(out[k].serial));
        k++;
    }
    qsort(out, (size_t)k, sizeof(*out), cmp_found);
    return k;
}

static void op_list(struct conn *c, struct shim_msg *m)
{
    struct found f[16];
    struct shim_dev d[16];
    libusb_device **list;
    int n = find_devices((uint16_t)m->arg, f, 16, &list), i;
    for (i = 0; i < n; i++) {
        memset(&d[i], 0, sizeof(d[i]));
        d[i].vid = serve_vid; d[i].pid = f[i].pid; d[i].index = (uint32_t)i; d[i].bus = f[i].bus;
        memcpy(d[i].port_path, f[i].ports, 7);
        snprintf(d[i].serial, sizeof(d[i].serial), "%s", f[i].serial);
    }
    libusb_free_device_list(list, 1);
    if (verbose || n) logmsg("c%d LIST %04x:%04x -> %d device(s)", c->id, serve_vid, m->arg, n);
    send_reply(c, m->op, m->id, 0, 0, d, (uint32_t)(n * (int)sizeof(d[0])));
}

static struct device *device_get(uint16_t pid, uint32_t index, uint32_t *err)
{
    struct found f[16];
    libusb_device **list;
    struct device *d;
    int n, r;
    pthread_mutex_lock(&devices_lock);
    n = find_devices(pid, f, 16, &list);
    if ((int)index >= n) { *err = ERROR_DEVICE_NOT_CONNECTED_; d = NULL; goto out; }
    for (d = devices; d; d = d->next) if (d->dev == f[index].dev) { d->refs++; goto out; }
    d = calloc(1, sizeof(*d));
    d->dev = libusb_ref_device(f[index].dev);
    d->vid = serve_vid; d->pid = pid; d->index = index; d->refs = 1;
    d->request_timeout_ms = 1000;
    snprintf(d->serial, sizeof(d->serial), "%s", f[index].serial);
    pthread_mutex_init(&d->lock, NULL);
    if ((r = libusb_open(d->dev, &d->h))) {
        logmsg("open %04x:%04x #%u failed: %s (on Linux: is there a udev rule for this device?)", serve_vid, pid, index, libusb_strerror(r));
        libusb_unref_device(d->dev);
        free(d);
        d = NULL;
        *err = r == LIBUSB_ERROR_ACCESS ? 5u /* ERROR_ACCESS_DENIED */ : ERROR_DEVICE_NOT_CONNECTED_;
        goto out;
    }
    libusb_set_auto_detach_kernel_driver(d->h, 1);
    device_rebuild_pipes(d);
    d->next = devices;
    devices = d;
    logmsg("opened %04x:%04x #%u serial '%s'", serve_vid, pid, index, d->serial);
out:
    libusb_free_device_list(list, 1);
    pthread_mutex_unlock(&devices_lock);
    return d;
}

static void device_put(struct device *d)
{
    struct device **pp;
    int i;
    pthread_mutex_lock(&devices_lock);
    if (--d->refs > 0) { pthread_mutex_unlock(&devices_lock); return; }
    for (pp = &devices; *pp; pp = &(*pp)->next) if (*pp == d) { *pp = d->next; break; }
    pthread_mutex_unlock(&devices_lock);
    /* last handle closed: like USBIO's UnconfigureOnClose, release everything */
    for (i = 0; i < d->n_ifs; i++) if (d->ifs[i].claimed) libusb_release_interface(d->h, d->ifs[i].num);
    libusb_close(d->h);
    libusb_unref_device(d->dev);
    pthread_mutex_destroy(&d->lock);
    logmsg("closed %04x:%04x #%u", d->vid, d->pid, d->index);
    free(d);
}

/* ---- connections ------------------------------------------------------------------------------ */
static void conn_unref(struct conn *c)
{
    int last;
    pthread_mutex_lock(&c->lock);
    last = --c->refs == 0;
    pthread_mutex_unlock(&c->lock);
    if (!last) return;
    if (c->dev) device_put(c->dev);
    close(c->fd);
    pthread_mutex_destroy(&c->lock);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
}
void helper_conn_unref(struct conn *c) { conn_unref(c); }

static void *conn_main(void *arg)
{
    struct conn *c = arg;
    uint8_t *pay = NULL, *out = NULL;
    size_t pay_cap = 0, out_cap = 0;
    for (;;) {
        struct shim_msg m;
        if (read_all(c->fd, &m, sizeof(m)) || m.magic != SHIM_MAGIC || m.len > (64u << 20)) break;
        if (m.len > pay_cap) { free(pay); pay_cap = m.len; pay = malloc(pay_cap); }
        if (m.len && read_all(c->fd, pay, m.len)) break;
        switch (m.op) {
        case OP_HELLO:
            send_reply(c, m.op, m.id, 0, SHIM_PROTO_VERSION, NULL, 0);
            break;
        case OP_LIST:
            op_list(c, &m);
            break;
        case OP_OPEN: {
            uint32_t err = 0;
            if (c->dev) { send_reply(c, m.op, m.id, ERROR_INVALID_PARAMETER_, 0, NULL, 0); break; }
            c->dev = device_get((uint16_t)m.arg, m.arg2, &err);
            if (c->dev) device_add_conn(c->dev, c);
            send_reply(c, m.op, m.id, c->dev ? 0 : err, 0, NULL, 0);
            break;
        }
        case OP_IOCTL: {
            uint32_t bytes = 0, err;
            if (m.arg2 > out_cap) { free(out); out_cap = m.arg2; out = calloc(1, out_cap); }
            if (out_cap) memset(out, 0, m.arg2);
            if (!c->dev) err = ERROR_DEVICE_NOT_CONNECTED_;
            else err = usbio_ioctl(c, m.arg, pay, m.len, out, m.arg2, &bytes);
            if (bytes > m.arg2) bytes = m.arg2;
            /* METHOD_IN_DIRECT (data went to the device): nothing to copy back */
            send_reply(c, m.op, m.id, err, bytes, out, (err || (m.arg & 3) == 1) ? 0 : bytes);
            break;
        }
        case OP_READ:
        case OP_WRITE: {
            uint32_t err = c->dev ? usbio_start_read_write(c, m.op, m.id, pay, m.len, m.arg) : ERROR_DEVICE_NOT_CONNECTED_;
            if (err) send_reply(c, m.op, m.id, err, 0, NULL, 0);
            break;
        }
        case OP_CANCEL:
            usbio_cancel_all(c);
            break;
        default:
            send_reply(c, m.op, m.id, ERROR_NOT_SUPPORTED_, 0, NULL, 0);
        }
    }
    if (verbose) logmsg("c%d closed", c->id);
    pthread_mutex_lock(&c->lock);
    c->closing = 1;
    pthread_mutex_unlock(&c->lock);
    usbio_cancel_all(c);
    usbio_conn_closed(c);
    free(pay);
    free(out);
    conn_unref(c);
    return NULL;
}

static void *event_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct timeval tv = {1, 0};
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int port = SHIM_DEFAULT_PORT, srv, one = 1, opt, r;
    struct sockaddr_in a;
    pthread_t ev;
    while ((opt = getopt(argc, argv, "p:vo:V:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'v': verbose++; break;
        case 'o': order_list = optarg; break;
        case 'V': serve_vid = (uint16_t)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: %s [-p port] [-v] [-o serial1,serial2,...] [-V vid]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if ((r = libusb_init(&ctx))) { fprintf(stderr, "libusb_init: %s\n", libusb_strerror(r)); return 1; }
    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) || listen(srv, 32)) {
        perror("usbio-helper: bind/listen");
        return 1;
    }
    pthread_create(&ev, NULL, event_main, NULL);
    logmsg("usbio-helper listening on 127.0.0.1:%d, serving vendor %04x (libusb %s)", port, serve_vid,
           libusb_get_version()->describe ? "ok" : "?");
    for (;;) {
        pthread_t t;
        struct conn *c;
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        c = calloc(1, sizeof(*c));
        c->fd = fd;
        c->refs = 1;
        c->id = ++conn_counter;
        pthread_mutex_init(&c->lock, NULL);
        pthread_mutex_init(&c->send_lock, NULL);
        pthread_create(&t, NULL, conn_main, c);
        pthread_detach(t);
    }
    return 0;
}
