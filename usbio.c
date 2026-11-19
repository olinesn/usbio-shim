/* USBIO's requests carried out with libusb. Request codes, layouts and status values:
 * usbio_defs.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "helper.h"
#include "usbio_defs.h"

#define IOCTL_FN(code) (((code) >> 2) & 0xFFF)
#define IOCTL_METHOD(code) ((code) & 3)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* bytes one packet of this endpoint can carry per (micro)frame */
static uint32_t ep_packet_bytes(uint16_t wmax)
{
    return (uint32_t)(wmax & 0x7ff) * (1u + ((wmax >> 11) & 3u));
}

/* ---- device state ------------------------------------------------------------------------------ */
void device_add_conn(struct device *d, struct conn *c)
{
    int i;
    pthread_mutex_lock(&d->lock);
    for (i = 0; i < 64; i++) if (!d->conns[i]) { d->conns[i] = c; break; }
    pthread_mutex_unlock(&d->lock);
}

/* Pipes of the claimed interfaces in their current alternate settings. Caller holds d->lock
 * (or the device is not shared yet). */
void device_rebuild_pipes(struct device *d)
{
    struct libusb_config_descriptor *cfg;
    int i, a, e;
    d->npipes = 0;
    if (!d->configured || libusb_get_active_config_descriptor(d->dev, &cfg)) return;
    for (i = 0; i < d->n_ifs; i++) {
        if (!d->ifs[i].claimed) continue;
        for (a = 0; a < cfg->bNumInterfaces; a++) {
            const struct libusb_interface *itf = &cfg->interface[a];
            int s;
            for (s = 0; s < itf->num_altsetting; s++) {
                const struct libusb_interface_descriptor *id = &itf->altsetting[s];
                if (id->bInterfaceNumber != d->ifs[i].num || id->bAlternateSetting != d->ifs[i].alt) continue;
                for (e = 0; e < id->bNumEndpoints && d->npipes < MAX_PIPES; e++) {
                    const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                    struct pipe_info *p = &d->pipes[d->npipes++];
                    p->ep = ed->bEndpointAddress;
                    p->type = ed->bmAttributes & 3;
                    p->maxpkt = ed->wMaxPacketSize;
                    p->interval = ed->bInterval;
                    p->iface = id->bInterfaceNumber;
                    p->alt = id->bAlternateSetting;
                    p->flags = USBIO_SHORT_TRANSFER_OK; /* registry ShortTransferOk=1 in Avisoft's INF */
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

static void unbind_conn(struct conn *c)
{
    pthread_mutex_lock(&c->lock);
    c->bound = 0;
    pthread_mutex_unlock(&c->lock);
    usbio_cancel_all(c);
}

/* SET_CONFIGURATION / SET_INTERFACE / UNCONFIGURE drop bindings (of one interface, or all) */
static void unbind_device(struct device *d, int iface)
{
    int i;
    for (i = 0; i < 64; i++) {
        struct conn *c = d->conns[i];
        if (c && c->bound && (iface < 0 || c->pipe.iface == iface)) unbind_conn(c);
    }
}

static void release_all(struct device *d)
{
    int i;
    for (i = 0; i < d->n_ifs; i++)
        if (d->ifs[i].claimed) libusb_release_interface(d->h, d->ifs[i].num);
    d->n_ifs = 0;
    d->configured = 0;
    d->npipes = 0;
}

void usbio_conn_closed(struct conn *c)
{
    struct device *d = c->dev;
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    for (i = 0; i < 64; i++) if (d->conns[i] == c) d->conns[i] = NULL;
    pthread_mutex_unlock(&d->lock);
}

/* ---- requests ---------------------------------------------------------------------------------- */
static uint32_t control(struct device *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint8_t *data, uint16_t len, uint32_t *bytes, int short_ok)
{
    int r = libusb_control_transfer(d->h, rt, req, val, idx, data, len, d->request_timeout_ms);
    if (r < 0) {
        if (verbose) logmsg("  control %02x %02x %04x %04x len %u failed: %s", rt, req, val, idx, len, libusb_strerror(r));
        switch (r) {
        case LIBUSB_ERROR_TIMEOUT: return USBIO_ERR_TIMEOUT;
        case LIBUSB_ERROR_PIPE: return USBIO_ERR_STALL_PID;
        case LIBUSB_ERROR_NO_DEVICE: return USBIO_ERR_DEVICE_GONE;
        case LIBUSB_ERROR_OVERFLOW: return USBIO_ERR_DATA_OVERRUN;
        default: return USBIO_ERR_REQUEST_FAILED;
        }
    }
    if (bytes) *bytes = (uint32_t)r;
    if ((rt & 0x80) && r < len && !short_ok) return USBIO_ERR_ERROR_SHORT_TRANSFER;
    return 0;
}

static uint32_t set_configuration(struct device *d, const USBIO_SET_CONFIGURATION *sc)
{
    struct libusb_config_descriptor *cfg;
    int cur = -1, r, i;
    uint8_t value;
    if (sc->NbOfInterfaces > USBIO_MAX_INTERFACES || sc->NbOfInterfaces > MAX_IFACES) return USBIO_ERR_INVALID_PARAM;
    if (libusb_get_config_descriptor(d->dev, (uint8_t)sc->ConfigurationIndex, &cfg)) return USBIO_ERR_INVALID_CONFIGURATION_DESCRIPTOR;
    value = cfg->bConfigurationValue;
    libusb_free_config_descriptor(cfg);
    unbind_device(d, -1);
    release_all(d);
    libusb_get_configuration(d->h, &cur);
    if (cur != value && (r = libusb_set_configuration(d->h, value))) {
        logmsg("  SET_CONFIGURATION %u failed: %s", value, libusb_strerror(r));
        return USBIO_ERR_SET_CONFIGURATION_FAILED;
    }
    d->cfg_value = value;
    d->configured = 1;
    for (i = 0; i < sc->NbOfInterfaces; i++) {
        const USBIO_INTERFACE_SETTING *s = &sc->InterfaceList[i];
        if ((r = libusb_claim_interface(d->h, s->InterfaceIndex))) {
            logmsg("  claim interface %u failed: %s", s->InterfaceIndex, libusb_strerror(r));
            release_all(d);
            return r == LIBUSB_ERROR_BUSY ? USBIO_ERR_ERROR_BUSY : USBIO_ERR_INTERFACE_NOT_FOUND;
        }
        d->ifs[d->n_ifs].num = s->InterfaceIndex;
        d->ifs[d->n_ifs].alt = s->AlternateSettingIndex;
        d->ifs[d->n_ifs].claimed = 1;
        d->max_transfer[d->n_ifs] = s->MaximumTransferSize;
        d->n_ifs++;
        if ((r = libusb_set_interface_alt_setting(d->h, s->InterfaceIndex, s->AlternateSettingIndex))) {
            logmsg("  interface %u alt %u failed: %s", s->InterfaceIndex, s->AlternateSettingIndex, libusb_strerror(r));
            release_all(d);
            return r == LIBUSB_ERROR_NOT_FOUND ? USBIO_ERR_INVALID_PARAM : USBIO_ERR_SET_CONFIG_FAILED;
        }
    }
    device_rebuild_pipes(d);
    logmsg("  configuration %u set, %d interface(s), %d pipe(s)", value, d->n_ifs, d->npipes);
    return 0;
}

static uint32_t set_interface(struct device *d, const USBIO_INTERFACE_SETTING *s)
{
    int i, r;
    if (!d->configured) return USBIO_ERR_NOT_CONFIGURED;
    for (i = 0; i < d->n_ifs; i++) if (d->ifs[i].num == s->InterfaceIndex) break;
    if (i == d->n_ifs) {
        if (d->n_ifs >= MAX_IFACES) return USBIO_ERR_INTERFACE_NOT_FOUND;
        if ((r = libusb_claim_interface(d->h, s->InterfaceIndex))) return USBIO_ERR_INTERFACE_NOT_FOUND;
        d->ifs[i].num = s->InterfaceIndex;
        d->ifs[i].claimed = 1;
        d->n_ifs++;
    }
    unbind_device(d, s->InterfaceIndex);
    if ((r = libusb_set_interface_alt_setting(d->h, s->InterfaceIndex, s->AlternateSettingIndex))) {
        logmsg("  SET_INTERFACE %u alt %u failed: %s", s->InterfaceIndex, s->AlternateSettingIndex, libusb_strerror(r));
        return r == LIBUSB_ERROR_NOT_FOUND ? USBIO_ERR_INVALID_PARAM : USBIO_ERR_REQUEST_FAILED;
    }
    d->ifs[i].alt = s->AlternateSettingIndex;
    d->max_transfer[i] = s->MaximumTransferSize;
    device_rebuild_pipes(d);
    logmsg("  interface %u -> alt %u, %d pipe(s)", s->InterfaceIndex, s->AlternateSettingIndex, d->npipes);
    return 0;
}

static uint32_t config_info(struct device *d, USBIO_CONFIGURATION_INFO *ci)
{
    struct libusb_config_descriptor *cfg;
    int i, k;
    memset(ci, 0, sizeof(*ci));
    if (!d->configured || libusb_get_active_config_descriptor(d->dev, &cfg)) return 0;
    for (i = 0; i < d->n_ifs && i < USBIO_MAX_INTERFACES; i++) {
        USBIO_INTERFACE_CONFIGURATION_INFO *ii = &ci->InterfaceInfo[ci->NbOfInterfaces++];
        ii->InterfaceNumber = (UCHAR)d->ifs[i].num;
        ii->AlternateSetting = (UCHAR)d->ifs[i].alt;
        for (k = 0; k < cfg->bNumInterfaces; k++) {
            int s;
            for (s = 0; s < cfg->interface[k].num_altsetting; s++) {
                const struct libusb_interface_descriptor *id = &cfg->interface[k].altsetting[s];
                if (id->bInterfaceNumber == d->ifs[i].num && id->bAlternateSetting == d->ifs[i].alt) {
                    ii->Class = id->bInterfaceClass;
                    ii->SubClass = id->bInterfaceSubClass;
                    ii->Protocol = id->bInterfaceProtocol;
                    ii->NumberOfPipes = id->bNumEndpoints;
                }
            }
        }
    }
    for (i = 0; i < d->npipes && i < USBIO_MAX_PIPES; i++) {
        USBIO_PIPE_CONFIGURATION_INFO *pi = &ci->PipeInfo[ci->NbOfPipes++];
        int j;
        pi->PipeType = (USBIO_PIPE_TYPE)d->pipes[i].type;
        pi->MaximumPacketSize = d->pipes[i].maxpkt;
        pi->EndpointAddress = d->pipes[i].ep;
        pi->Interval = d->pipes[i].interval;
        pi->InterfaceNumber = d->pipes[i].iface;
        pi->MaximumTransferSize = 4096;
        for (j = 0; j < d->n_ifs; j++)
            if (d->ifs[j].num == d->pipes[i].iface && d->max_transfer[j]) pi->MaximumTransferSize = d->max_transfer[j];
    }
    libusb_free_config_descriptor(cfg);
    return 0;
}

#define NEED_IN(t) do { if (in_len < sizeof(t)) return USBIO_ERR_INVALID_INBUFFER; } while (0)
#define NEED_OUT(t) do { if (out_len < sizeof(t)) return USBIO_ERR_INVALID_OUTBUFFER; *bytes = sizeof(t); } while (0)

static uint32_t ioctl_locked(struct conn *c, struct device *d, uint32_t code, const uint8_t *in, uint32_t in_len,
                             uint8_t *out, uint32_t out_len, const uint8_t *send_data, uint32_t *bytes)
{
    switch (code) {
    case IOCTL_USBIO_GET_DESCRIPTOR: {
        const USBIO_DESCRIPTOR_REQUEST *r = (const void *)in;
        NEED_IN(USBIO_DESCRIPTOR_REQUEST);
        return control(d, 0x80 | (r->Recipient & 0x1f), LIBUSB_REQUEST_GET_DESCRIPTOR,
                       (uint16_t)((r->DescriptorType << 8) | r->DescriptorIndex), r->LanguageId, out, (uint16_t)out_len, bytes, 1);
    }
    case IOCTL_USBIO_SET_DESCRIPTOR: {
        const USBIO_DESCRIPTOR_REQUEST *r = (const void *)in;
        NEED_IN(USBIO_DESCRIPTOR_REQUEST);
        return control(d, r->Recipient & 0x1f, LIBUSB_REQUEST_SET_DESCRIPTOR,
                       (uint16_t)((r->DescriptorType << 8) | r->DescriptorIndex), r->LanguageId, (uint8_t *)send_data, (uint16_t)out_len, bytes, 1);
    }
    case IOCTL_USBIO_SET_FEATURE:
    case IOCTL_USBIO_CLEAR_FEATURE: {
        const USBIO_FEATURE_REQUEST *r = (const void *)in;
        NEED_IN(USBIO_FEATURE_REQUEST);
        return control(d, r->Recipient & 0x1f, code == IOCTL_USBIO_SET_FEATURE ? 3 : 1, r->FeatureSelector, r->Index, NULL, 0, NULL, 1);
    }
    case IOCTL_USBIO_GET_STATUS: {
        const USBIO_STATUS_REQUEST *r = (const void *)in;
        NEED_IN(USBIO_STATUS_REQUEST);
        NEED_OUT(USBIO_STATUS_REQUEST_DATA);
        return control(d, 0x80 | (r->Recipient & 0x1f), LIBUSB_REQUEST_GET_STATUS, 0, r->Index, out, 2, NULL, 0);
    }
    case IOCTL_USBIO_GET_CONFIGURATION:
        NEED_OUT(USBIO_GET_CONFIGURATION_DATA);
        out[0] = d->configured ? d->cfg_value : 0;   /* Windows-like: unconfigured until SET_CONFIGURATION */
        return 0;
    case IOCTL_USBIO_GET_INTERFACE: {
        const USBIO_GET_INTERFACE *r = (const void *)in;
        int i;
        NEED_IN(USBIO_GET_INTERFACE);
        NEED_OUT(USBIO_GET_INTERFACE_DATA);
        for (i = 0; i < d->n_ifs; i++) if (d->ifs[i].num == r->Interface) { out[0] = (uint8_t)d->ifs[i].alt; return 0; }
        return USBIO_ERR_INTERFACE_NOT_FOUND;
    }
    case IOCTL_USBIO_STORE_CONFIG_DESCRIPTOR:
        return 0;
    case IOCTL_USBIO_SET_CONFIGURATION:
        NEED_IN(USBIO_SET_CONFIGURATION);
        return set_configuration(d, (const void *)in);
    case IOCTL_USBIO_UNCONFIGURE_DEVICE:
        unbind_device(d, -1);
        release_all(d);
        return 0;
    case IOCTL_USBIO_SET_INTERFACE:
        NEED_IN(USBIO_INTERFACE_SETTING);
        return set_interface(d, (const void *)in);
    case IOCTL_USBIO_CLASS_OR_VENDOR_IN_REQUEST:
    case IOCTL_USBIO_CLASS_OR_VENDOR_OUT_REQUEST: {
        const USBIO_CLASS_OR_VENDOR_REQUEST *r = (const void *)in;
        int is_in = code == IOCTL_USBIO_CLASS_OR_VENDOR_IN_REQUEST;
        uint8_t rt = (uint8_t)((is_in ? 0x80 : 0) | ((r->Type & 3) << 5) | (r->Recipient & 0x1f) | r->RequestTypeReservedBits);
        NEED_IN(USBIO_CLASS_OR_VENDOR_REQUEST);
        return control(d, rt, r->Request, r->Value, r->Index, is_in ? out : (uint8_t *)send_data, (uint16_t)out_len, bytes,
                       (r->Flags & USBIO_SHORT_TRANSFER_OK) != 0);
    }
    case IOCTL_USBIO_GET_DEVICE_PARAMETERS: {
        USBIO_DEVICE_PARAMETERS *p = (void *)out;
        NEED_OUT(USBIO_DEVICE_PARAMETERS);
        p->Options = d->dev_options;
        p->RequestTimeout = d->request_timeout_ms;
        return 0;
    }
    case IOCTL_USBIO_SET_DEVICE_PARAMETERS: {
        const USBIO_DEVICE_PARAMETERS *p = (const void *)in;
        NEED_IN(USBIO_DEVICE_PARAMETERS);
        d->dev_options = p->Options;
        d->request_timeout_ms = p->RequestTimeout;
        return 0;
    }
    case IOCTL_USBIO_GET_CONFIGURATION_INFO:
        NEED_OUT(USBIO_CONFIGURATION_INFO);
        return config_info(d, (void *)out);
    case IOCTL_USBIO_RESET_DEVICE:
    case IOCTL_USBIO_CYCLE_PORT: {
        int r;
        unbind_device(d, -1);
        release_all(d);
        r = libusb_reset_device(d->h);
        logmsg("  %s: libusb_reset_device -> %s", code == IOCTL_USBIO_CYCLE_PORT ? "CYCLE_PORT" : "RESET_DEVICE", r ? libusb_strerror(r) : "ok");
        return r && r != LIBUSB_ERROR_NOT_FOUND ? USBIO_ERR_REQUEST_FAILED : 0;
    }
    case IOCTL_USBIO_GET_CURRENT_FRAME_NUMBER: {
        USBIO_FRAME_NUMBER *f = (void *)out;
        NEED_OUT(USBIO_FRAME_NUMBER);
        f->FrameNumber = (ULONG)(now_s() * 1000.0);
        return 0;
    }
    case IOCTL_USBIO_SET_DEVICE_POWER_STATE: {
        const USBIO_DEVICE_POWER *p = (const void *)in;
        NEED_IN(USBIO_DEVICE_POWER);
        d->power_state = p->DevicePowerState;
        return 0;
    }
    case IOCTL_USBIO_GET_DEVICE_POWER_STATE: {
        USBIO_DEVICE_POWER *p = (void *)out;
        NEED_OUT(USBIO_DEVICE_POWER);
        p->DevicePowerState = (USBIO_DEVICE_POWER_STATE)d->power_state;
        return 0;
    }
    case IOCTL_USBIO_GET_BANDWIDTH_INFO: {
        USBIO_BANDWIDTH_INFO *b = (void *)out;
        NEED_OUT(USBIO_BANDWIDTH_INFO);
        memset(b, 0, sizeof(*b));
        b->TotalBandwidth = libusb_get_device_speed(d->dev) >= LIBUSB_SPEED_HIGH ? 480000 : 12000;
        return 0;
    }
    case IOCTL_USBIO_GET_DEVICE_INFO: {
        USBIO_DEVICE_INFO *i = (void *)out;
        NEED_OUT(USBIO_DEVICE_INFO);
        memset(i, 0, sizeof(*i));
        if (libusb_get_device_speed(d->dev) >= LIBUSB_SPEED_HIGH) i->Flags = USBIO_DEVICE_INFOFLAG_HIGH_SPEED;
        i->OpenCount = (ULONG)d->refs;
        return 0;
    }
    case IOCTL_USBIO_GET_DRIVER_INFO: {
        USBIO_DRIVER_INFO *i = (void *)out;
        const char *v = getenv("USBIO_HELPER_API_VERSION");
        NEED_OUT(USBIO_DRIVER_INFO);
        memset(i, 0, sizeof(*i));
        /* the app requires our major == its major and our minor >= its minor */
        i->APIVersion = (USHORT)(v ? strtoul(v, NULL, 0) : 0x0299);
        i->DriverVersion = 0x0240;   /* usgh_x64.sys 2.40.2.0 */
        i->DriverBuildNumber = 2;
        return 0;
    }
    case IOCTL_USBIO_ACQUIRE_DEVICE:
    case IOCTL_USBIO_RELEASE_DEVICE:
        return 0;
    case IOCTL_USBIO_BIND_PIPE: {
        const USBIO_BIND_PIPE *b = (const void *)in;
        int i, j;
        NEED_IN(USBIO_BIND_PIPE);
        if (!d->configured) return USBIO_ERR_NOT_CONFIGURED;
        if (c->bound) return USBIO_ERR_ALREADY_BOUND;
        for (i = 0; i < d->npipes; i++) if (d->pipes[i].ep == b->EndpointAddress) break;
        if (i == d->npipes) return USBIO_ERR_PIPE_NOT_FOUND;
        for (j = 0; j < 64; j++)
            if (d->conns[j] && d->conns[j] != c && d->conns[j]->bound && d->conns[j]->pipe.ep == b->EndpointAddress) return USBIO_ERR_ALREADY_BOUND;
        pthread_mutex_lock(&c->lock);
        c->pipe = d->pipes[i];
        c->bound = 1;
        c->st_bytes = c->st_bytes_window = 0;
        c->st_ok = c->st_failed = 0;
        pthread_mutex_unlock(&c->lock);
        logmsg("c%d bound to endpoint %02x (%s, wMaxPacketSize %#x)", c->id, c->pipe.ep,
               (const char *[]){"control", "isochronous", "bulk", "interrupt"}[c->pipe.type & 3], c->pipe.maxpkt);
        return 0;
    }
    case IOCTL_USBIO_UNBIND_PIPE:
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        unbind_conn(c);
        return 0;
    case IOCTL_USBIO_RESET_PIPE:
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        if (c->pipe.type != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) libusb_clear_halt(d->h, c->pipe.ep);
        return 0;
    case IOCTL_USBIO_ABORT_PIPE:
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        usbio_cancel_all(c);
        return 0;
    case IOCTL_USBIO_GET_PIPE_PARAMETERS: {
        USBIO_PIPE_PARAMETERS *p = (void *)out;
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        NEED_OUT(USBIO_PIPE_PARAMETERS);
        p->Flags = c->pipe.flags;
        return 0;
    }
    case IOCTL_USBIO_SET_PIPE_PARAMETERS: {
        const USBIO_PIPE_PARAMETERS *p = (const void *)in;
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        NEED_IN(USBIO_PIPE_PARAMETERS);
        c->pipe.flags = p->Flags;
        return 0;
    }
    case IOCTL_USBIO_SETUP_PIPE_STATISTICS: {
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        if (in_len < 4) return USBIO_ERR_INVALID_INBUFFER;
        c->st_interval_ms = ((const ULONG *)in)[0];
        if (c->st_interval_ms > 5000) return USBIO_ERR_INVALID_PARAMETER;
        c->st_window_start = now_s();
        c->st_bytes_window = 0;
        return 0;
    }
    case IOCTL_USBIO_QUERY_PIPE_STATISTICS: {
        const USBIO_QUERY_PIPE_STATISTICS *q = (const void *)in;
        USBIO_PIPE_STATISTICS *s = (void *)out;
        double t = now_s();
        if (!c->bound) return USBIO_ERR_NOT_BOUND;
        NEED_IN(USBIO_QUERY_PIPE_STATISTICS);
        NEED_OUT(USBIO_PIPE_STATISTICS);
        memset(s, 0, sizeof(*s));
        pthread_mutex_lock(&c->lock);
        s->ActualAveragingInterval = c->st_interval_ms;
        if (c->st_interval_ms && t > c->st_window_start) s->AverageRate = (ULONG)(c->st_bytes_window / (t - c->st_window_start));
        if (c->st_interval_ms && (t - c->st_window_start) * 1000.0 >= c->st_interval_ms) { c->st_window_start = t; c->st_bytes_window = 0; }
        s->BytesTransferred_L = (ULONG)c->st_bytes;
        s->BytesTransferred_H = (ULONG)(c->st_bytes >> 32);
        s->RequestsSucceeded = c->st_ok;
        s->RequestsFailed = c->st_failed;
        if (q->Flags & USBIO_QPS_FLAG_RESET_BYTES_TRANSFERRED) c->st_bytes = 0;
        if (q->Flags & USBIO_QPS_FLAG_RESET_REQUESTS_SUCCEEDED) c->st_ok = 0;
        if (q->Flags & USBIO_QPS_FLAG_RESET_REQUESTS_FAILED) c->st_failed = 0;
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    case IOCTL_USBIO_PIPE_CONTROL_TRANSFER_IN:
    case IOCTL_USBIO_PIPE_CONTROL_TRANSFER_OUT:
        return USBIO_ERR_CONTROL_NOT_SUPPORTED;   /* control transfers on a non-default pipe: not in libusb */
    default:
        logmsg("c%d unknown IOCTL %#x (function %#x, method %u)", c->id, code, IOCTL_FN(code), IOCTL_METHOD(code));
        return USBIO_ERR_INVALID_IOCTL;
    }
}

uint32_t usbio_ioctl(struct conn *c, uint32_t code, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, uint32_t *bytes)
{
    struct device *d = c->dev;
    const uint8_t *send_data = NULL;
    uint32_t err;
    /* METHOD_IN_DIRECT: the "output" buffer holds data for the device; the shim appends it */
    if (IOCTL_METHOD(code) == METHOD_IN_DIRECT) {
        if (in_len < out_len) return USBIO_ERR_INVALID_OUTBUFFER;
        in_len -= out_len;
        send_data = in + in_len;
    }
    *bytes = 0;
    pthread_mutex_lock(&d->lock);
    err = ioctl_locked(c, d, code, in, in_len, out, out_len, send_data, bytes);
    pthread_mutex_unlock(&d->lock);
    if (IOCTL_METHOD(code) == METHOD_IN_DIRECT) *bytes = err ? 0 : *bytes; /* nothing to copy back */
    if (verbose) logmsg("c%d IOCTL fn %u -> %#x, %u bytes", c->id, IOCTL_FN(code) - 0x800, err, *bytes);
    return err;
}

/* ---- data transfers on a bound pipe ------------------------------------------------------------ */
static uint32_t transfer_error(enum libusb_transfer_status st)
{
    switch (st) {
    case LIBUSB_TRANSFER_COMPLETED: return 0;
    case LIBUSB_TRANSFER_CANCELLED: return USBIO_ERR_CANCELED;
    case LIBUSB_TRANSFER_NO_DEVICE: return USBIO_ERR_DEVICE_GONE;
    case LIBUSB_TRANSFER_STALL: return USBIO_ERR_ENDPOINT_HALTED;
    case LIBUSB_TRANSFER_OVERFLOW: return USBIO_ERR_DATA_OVERRUN;
    case LIBUSB_TRANSFER_TIMED_OUT: return USBIO_ERR_TIMEOUT;
    default: return USBIO_ERR_REQUEST_FAILED;
    }
}
static uint32_t packet_status(enum libusb_transfer_status st)
{
    switch (st) {
    case LIBUSB_TRANSFER_COMPLETED: return 0;
    case LIBUSB_TRANSFER_OVERFLOW: return USBIO_ERR_DATA_OVERRUN;
    case LIBUSB_TRANSFER_CANCELLED: return USBIO_ERR_CANCELED;
    case LIBUSB_TRANSFER_STALL: return USBIO_ERR_STALL_PID;
    default: return USBIO_ERR_XACT_ERROR;
    }
}

/* USBIO_HELPER_DUMP=<file>: append every completed isochronous IN packet as
 * [uint32 endpoint][uint32 length][uint32 status][data], for USBIO_HELPER_DUMP_SECONDS (default 10)
 * after the first one: a record of exactly what the device sent, for debugging on real hardware. */
static void dump_packets(struct conn *c, struct xfer *x, struct libusb_transfer *t)
{
    static FILE *f;
    static double t0;
    static int disabled;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    const char *path = getenv("USBIO_HELPER_DUMP");
    double limit = getenv("USBIO_HELPER_DUMP_SECONDS") ? atof(getenv("USBIO_HELPER_DUMP_SECONDS")) : 10.0;
    uint32_t i, upos = 0;
    if (!path || disabled) return;
    pthread_mutex_lock(&mu);
    if (!f) { f = fopen(path, "ab"); t0 = now_s(); if (!f) disabled = 1; else logmsg("dumping iso packets to %s for %.0f s", path, limit); }
    if (f && now_s() - t0 > limit) { fclose(f); f = NULL; disabled = 1; logmsg("dump finished"); }
    for (i = 0; f && i < (uint32_t)t->num_iso_packets; i++) {
        uint32_t h[3] = {t->endpoint | ((uint32_t)c->dev->index << 8), t->iso_packet_desc[i].actual_length, (uint32_t)t->iso_packet_desc[i].status};
        fwrite(h, sizeof(h), 1, f);
        fwrite(x->ubuf + upos, 1, t->iso_packet_desc[i].actual_length, f);
        upos += x->cap[i];
    }
    pthread_mutex_unlock(&mu);
}

static void xfer_unlink(struct conn *c, struct xfer *x)
{
    struct xfer **pp;
    pthread_mutex_lock(&c->lock);
    for (pp = &c->pending; *pp; pp = &(*pp)->next) if (*pp == x) { *pp = x->next; break; }
    pthread_mutex_unlock(&c->lock);
}

static void xfer_free(struct xfer *x)
{
    if (x->t) libusb_free_transfer(x->t);
    free(x->buf);
    free(x->ubuf);
    free(x->slot);
    free(x->cap);
    free(x);
}

static void LIBUSB_CALL xfer_done(struct libusb_transfer *t)
{
    struct xfer *x = t->user_data;
    struct conn *c = x->c;
    uint32_t err = transfer_error(t->status), bytes = 0, plen = 0;

    xfer_unlink(c, x);
    if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        USBIO_ISO_TRANSFER *iso = (USBIO_ISO_TRANSFER *)x->buf;
        USBIO_ISO_PACKET *pk = (USBIO_ISO_PACKET *)(x->buf + sizeof(USBIO_ISO_TRANSFER));
        uint32_t i, errors = 0, end = x->hdr_len, upos = 0, moved = 0;
        for (i = 0; i < (uint32_t)t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            pk[i].Status = packet_status(d->status);
            if (pk[i].Status) errors++;
            if (x->op == OP_READ) {
                /* libusb's buffer holds packet i at the sum of the requested lengths before it;
                 * the application wants it at its own Offset */
                pk[i].Length = d->actual_length;
                memcpy(x->buf + x->slot[i], x->ubuf + upos, d->actual_length);
                if (x->slot[i] + d->actual_length > end) end = x->slot[i] + d->actual_length;
                moved += d->actual_length;
            }
            upos += x->cap[i];
        }
        if (x->op == OP_READ) dump_packets(c, x, t);
        iso->ErrorCount = errors;
        iso->StartFrame = (ULONG)(x->start_uf / 8);   /* frame (ms) number of packet 0 */
        if (!err && errors == (uint32_t)t->num_iso_packets) err = USBIO_ERR_ISOCH_REQUEST_FAILED;
        {
            /* undocumented: what a read's byte count means for iso. USBIO_HELPER_ISO_BYTES=data reports
             * the payload bytes, =hdrdata header + payload; default the whole buffer */
            const char *m = getenv("USBIO_HELPER_ISO_BYTES");
            bytes = err ? 0 : !m ? x->len : !strcmp(m, "data") ? moved : x->hdr_len + moved;
        }
        plen = x->op == OP_READ ? end : x->hdr_len;
        pthread_mutex_lock(&c->lock);
        c->st_bytes += moved; c->st_bytes_window += moved;
        c->st_ok += (uint32_t)t->num_iso_packets - errors; c->st_failed += errors;
        pthread_mutex_unlock(&c->lock);
    } else {
        bytes = (uint32_t)t->actual_length;
        if (x->op == OP_READ) plen = bytes;
        pthread_mutex_lock(&c->lock);
        c->st_bytes += bytes; c->st_bytes_window += bytes;
        if (err) c->st_failed++; else c->st_ok++;
        pthread_mutex_unlock(&c->lock);
    }
    if (verbose > 1) logmsg("c%d %s done: status %d err %#x bytes %u", c->id, x->op == OP_READ ? "read" : "write", t->status, err, bytes);
    send_reply(c, x->op, x->id, err, bytes, x->buf, plen);
    xfer_free(x);
    helper_conn_unref(c);
}

uint32_t usbio_start_read_write(struct conn *c, uint32_t op, uint32_t id, const uint8_t *pay, uint32_t plen, uint32_t len)
{
    struct device *d = c->dev;
    struct pipe_info p;
    struct xfer *x;
    int r, in;

    pthread_mutex_lock(&c->lock);
    if (c->closing) { pthread_mutex_unlock(&c->lock); return USBIO_ERR_CANCELED; }
    if (!c->bound) { pthread_mutex_unlock(&c->lock); return USBIO_ERR_NOT_BOUND; }
    p = c->pipe;
    pthread_mutex_unlock(&c->lock);
    in = (p.ep & 0x80) != 0;
    if ((op == OP_READ) != in) return USBIO_ERR_INVALID_DIRECTION;

    x = calloc(1, sizeof(*x));
    x->c = c; x->id = id; x->op = op; x->len = len;
    x->buf = calloc(1, len ? len : 1);
    memcpy(x->buf, pay, plen < len ? plen : len);

    if (p.type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        const USBIO_ISO_TRANSFER *iso = (const USBIO_ISO_TRANSFER *)x->buf;
        const USBIO_ISO_PACKET *pk = (const USBIO_ISO_PACKET *)(x->buf + sizeof(USBIO_ISO_TRANSFER));
        uint32_t n, i, total = 0, pkt_max = ep_packet_bytes(p.maxpkt);
        if (len < sizeof(USBIO_ISO_TRANSFER)) goto bad_packet;
        n = iso->NumberOfPackets;
        if (n > 1024) { xfer_free(x); return USBIO_ERR_TOO_MUCH_ISO_PACKETS; }
        x->hdr_len = (uint32_t)(sizeof(USBIO_ISO_TRANSFER) + n * sizeof(USBIO_ISO_PACKET));
        if (!n || x->hdr_len > len || plen < x->hdr_len) goto bad_packet;
        x->slot = calloc(n, sizeof(uint32_t));
        x->cap = calloc(n, sizeof(uint32_t));
        for (i = 0; i < n; i++) {
            /* Offset counts from the start of the whole buffer; packets are back to back */
            uint32_t off = pk[i].Offset, cap;
            if (off < x->hdr_len || off > len) goto bad_packet;
            if (op == OP_READ) {
                uint32_t next = i + 1 < n ? pk[i + 1].Offset : len;
                if (next < off) goto bad_packet;
                cap = next - off;
                if (cap > pkt_max) cap = pkt_max;
            } else {
                cap = pk[i].Length;
                if (off + cap > len || cap > pkt_max) goto bad_packet;
            }
            x->slot[i] = off;
            x->cap[i] = cap;
            total += cap;
        }
        if (verbose > 1) {
            static int shown;
            if (shown++ < 3)
                logmsg("c%d iso %s: %u packets, flags %#x, start frame %u, offsets %u %u %u ... last %u, buffer %u",
                       c->id, op == OP_READ ? "read" : "write", n, iso->Flags, iso->StartFrame, pk[0].Offset,
                       n > 1 ? pk[1].Offset : 0, n > 2 ? pk[2].Offset : 0, pk[n - 1].Offset, len);
        }
        x->ubuf = malloc(total ? total : 1);
        x->t = libusb_alloc_transfer((int)n);
        if (op == OP_WRITE) {
            uint32_t upos = 0;
            for (i = 0; i < n; i++) { memcpy(x->ubuf + upos, x->buf + x->slot[i], x->cap[i]); upos += x->cap[i]; }
        }
        libusb_fill_iso_transfer(x->t, d->h, p.ep, x->ubuf, (int)total, (int)n, xfer_done, x, 0);
        for (i = 0; i < n; i++) x->t->iso_packet_desc[i].length = x->cap[i];
    } else {
        x->t = libusb_alloc_transfer(0);
        if (p.type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
            libusb_fill_interrupt_transfer(x->t, d->h, p.ep, x->buf, (int)len, xfer_done, x, 0);
        else
            libusb_fill_bulk_transfer(x->t, d->h, p.ep, x->buf, (int)len, xfer_done, x, 0);
        if (in && !(p.flags & USBIO_SHORT_TRANSFER_OK)) x->t->flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    }
    pthread_mutex_lock(&c->lock);
    if (p.type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        /* The host controller schedules a queued iso transfer right after the previous one, so
         * frame numbers run on seamlessly; after the queue ran dry the stream restarts "now". */
        uint64_t now_uf = (uint64_t)(now_s() * 8000.0) + 8;
        int queued_iso = 0;
        struct xfer *q;
        for (q = c->pending; q; q = q->next) if (q->t && q->t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) queued_iso = 1;
        x->start_uf = (queued_iso && c->iso_next_uf) ? c->iso_next_uf : (c->iso_next_uf > now_uf ? c->iso_next_uf : now_uf);
        c->iso_next_uf = x->start_uf + (uint64_t)x->t->num_iso_packets * (p.maxpkt && p.interval > 1 ? (1u << (p.interval - 1)) : 1u);
    }
    c->refs++;
    x->next = c->pending;
    c->pending = x;
    pthread_mutex_unlock(&c->lock);
    if ((r = libusb_submit_transfer(x->t))) {
        xfer_unlink(c, x);
        logmsg("c%d submit on endpoint %02x failed: %s", c->id, p.ep, libusb_strerror(r));
        xfer_free(x);
        helper_conn_unref(c);
        return r == LIBUSB_ERROR_NO_DEVICE ? USBIO_ERR_DEVICE_GONE : USBIO_ERR_REQUEST_FAILED;
    }
    return 0;
bad_packet:
    xfer_free(x);
    return USBIO_ERR_INVALID_ISO_PACKET;
}

void usbio_cancel_all(struct conn *c)
{
    struct xfer *x;
    pthread_mutex_lock(&c->lock);
    for (x = c->pending; x; x = x->next) libusb_cancel_transfer(x->t);
    pthread_mutex_unlock(&c->lock);
}
