/* usbio-shim: a stand-in for Thesycon's USBIO driver (Avisoft's usgh_x64.sys) for Avisoft
 * RECORDER USGH running under Wine.
 *
 * Built as a 32-bit "powrprof.dll" that sits next to rec_usgh.exe (rec_usgh.exe imports only
 * SetSuspendState from the real one, which we forward). When the loader runs our DllMain, the
 * exe's import table is already filled in; we re-point eleven of its entries
 * (SetupDi* device discovery, CreateFile, DeviceIoControl, ReadFile/WriteFile,
 * GetOverlappedResult, CancelIo, CloseHandle) at our versions. Calls on handles we did not
 * create go straight to the real functions. Calls on "USBIO device handles" are forwarded over
 * a localhost TCP socket to usbio-helper, a native program that does the USB work with libusb.
 *
 * Environment (Unix environment variables are visible inside Wine):
 *   USBIO_SHIM_PORT   helper port (default 47321)
 *   USBIO_SHIM_LOG    file to log every intercepted call to (e.g. Z:\tmp\usbio-shim.log)
 *   USBIO_SHIM_OFF=1  install no hooks (the DLL then only forwards SetSuspendState)
 *
 * Nothing here knows USBIO's IOCTL semantics; usbio-helper implements them.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "proto.h"

#ifndef STATUS_PENDING
#define STATUS_PENDING ((ULONG_PTR)0x00000103L)
#endif

/* ---- the eleven USBIO device-interface GUIDs from Avisoft's usgh_x64.inf, with their USB ids --- */
static const struct { GUID guid; USHORT pid; } avisoft_ifaces[] = {
    {{0x4ECB65A9,0xB2A2,0x415e,{0xA3,0x18,0xB3,0x85,0x45,0x32,0xBE,0x2A}}, 0x0412},
    {{0xDDB5E972,0x984B,0x4d3d,{0xB4,0xE9,0xDD,0x44,0x3A,0x5F,0xF8,0xCC}}, 0x0512},
    {{0xDA41AD02,0x1183,0x4ae4,{0x89,0xf8,0xFF,0x04,0x22,0xE7,0x00,0x88}}, 0x0612},
    {{0x7104E2D3,0x0F21,0x40b4,{0xBE,0x24,0xF9,0x02,0x98,0x92,0x51,0xA8}}, 0x0712},
    {{0xF0F8FDB9,0xCB20,0x4998,{0x92,0x1E,0x16,0x27,0x7C,0x4A,0x0C,0xA7}}, 0x0812},
    {{0xA75806F7,0xE40E,0x4ce1,{0xBA,0xA3,0x20,0x24,0xE4,0xBA,0xCE,0x37}}, 0x1012}, /* UltraSoundGate 1216H */
    {{0x8B6B28A3,0xAA09,0x4f10,{0xA3,0xD9,0xCE,0xF4,0xC8,0x69,0xA4,0xC0}}, 0x0912},
    {{0xCBF62BE1,0x600A,0x4896,{0x8E,0x49,0x01,0x33,0xB7,0xB1,0xDD,0x12}}, 0x1112},
    {{0x7DF3487E,0x4A1D,0x449b,{0xA8,0x3A,0x71,0x25,0x56,0x7D,0x53,0xB0}}, 0x1212},
    {{0x6793496f,0x82f5,0x4d44,{0xac,0xa5,0x6c,0x94,0xbc,0x9a,0xea,0xb8}}, 0x1312},
    {{0x84268eb3,0xc035,0x4683,{0x8e,0x35,0xe8,0x65,0x0d,0x38,0x6d,0x3c}}, 0x1412},
};
#define N_IFACES (sizeof(avisoft_ifaces) / sizeof(avisoft_ifaces[0]))
static const char path_prefix[] = "\\\\?\\usbioshim#";

/* ---- logging ---------------------------------------------------------------------------------- */
static CRITICAL_SECTION log_cs;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    DWORD w;
    if (log_file == INVALID_HANDLE_VALUE) return;
    n = _snprintf(buf, sizeof(buf) - 2, "[%5lu %08lu] ", GetCurrentThreadId(), GetTickCount());
    va_start(ap, fmt);
    n += _vsnprintf(buf + n, sizeof(buf) - 2 - n, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    EnterCriticalSection(&log_cs);
    WriteFile(log_file, buf, n, &w, NULL);
    LeaveCriticalSection(&log_cs);
}
static void hexdump_(const char *what, const void *p, DWORD n)
{
    char line[3 * 64 + 1];
    DWORD i, k = 0;
    if (log_file == INVALID_HANDLE_VALUE || !p) return;
    for (i = 0; i < n && i < 64; i++) k += sprintf(line + k, "%02x ", ((const BYTE *)p)[i]);
    line[k] = 0;
    logf_("    %s (%lu bytes): %s%s", what, n, line, n > 64 ? "..." : "");
}

/* ---- the real functions ------------------------------------------------------------------------ */
static HDEVINFO (WINAPI *real_SetupDiGetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
static BOOL (WINAPI *real_SetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
static BOOL (WINAPI *real_SetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
static BOOL (WINAPI *real_SetupDiDestroyDeviceInfoList)(HDEVINFO);
static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *real_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL (WINAPI *real_DeviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *real_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *real_WriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *real_GetOverlappedResult)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL (WINAPI *real_CancelIo)(HANDLE);
static BOOL (WINAPI *real_CloseHandle)(HANDLE);

/* ---- helper connections ------------------------------------------------------------------------ */
struct pending {
    struct pending *next;
    uint32_t id, op;
    BYTE *buf;          /* where the reply payload goes (READ buffer or IOCTL output buffer) */
    DWORD buf_len;
    OVERLAPPED *ov;     /* NULL for synchronous calls */
    HANDLE done;        /* synchronous calls wait on this */
    uint32_t error, bytes;
    BOOL finished;
};

struct conn {
    HANDLE handle;      /* the HANDLE the application sees: a manual-reset event we own */
    SOCKET sock;
    CRITICAL_SECTION lock; /* guards send + the pending list */
    struct pending *pending;
    uint32_t next_id;
    uint32_t flags;     /* SHIM_F_* from the last reply */
    HANDLE rx_thread;
    BOOL dead;
    char path[160];
};

#define MAX_CONNS 64
static CRITICAL_SECTION table_cs;
static struct conn *conns[MAX_CONNS];

static struct conn *find_conn(HANDLE h)
{
    struct conn *c = NULL;
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    EnterCriticalSection(&table_cs);
    for (i = 0; i < MAX_CONNS; i++) if (conns[i] && conns[i]->handle == h) { c = conns[i]; break; }
    LeaveCriticalSection(&table_cs);
    return c;
}

static int helper_port(void)
{
    char v[16];
    if (GetEnvironmentVariableA("USBIO_SHIM_PORT", v, sizeof(v)) && atoi(v) > 0) return atoi(v);
    return SHIM_DEFAULT_PORT;
}

static BOOL send_all(SOCKET s, const void *p, int n)
{
    const char *c = p;
    while (n > 0) {
        int k = send(s, c, n, 0);
        if (k <= 0) return FALSE;
        c += k; n -= k;
    }
    return TRUE;
}
static BOOL recv_all(SOCKET s, void *p, int n)
{
    char *c = p;
    while (n > 0) {
        int k = recv(s, c, n, 0);
        if (k <= 0) return FALSE;
        c += k; n -= k;
    }
    return TRUE;
}

static SOCKET helper_connect(void)
{
    static LONG wsa_ready;
    struct sockaddr_in a;
    SOCKET s;
    int one = 1;
    if (!InterlockedExchange(&wsa_ready, 1)) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)helper_port());
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return s;
}

/* One blocking request/reply on a private socket (device discovery). */
static BOOL simple_call(uint32_t op, uint32_t arg, uint32_t arg2, struct shim_msg *rep, void *out, DWORD out_len)
{
    struct shim_msg m = {SHIM_MAGIC, op, 1, arg, arg2, 0, 0, 0, 0};
    SOCKET s = helper_connect();
    BOOL ok = FALSE;
    if (s == INVALID_SOCKET) return FALSE;
    if (send_all(s, &m, sizeof(m)) && recv_all(s, rep, sizeof(*rep)) && rep->magic == SHIM_MAGIC) {
        DWORD take = rep->len < out_len ? rep->len : out_len;
        ok = recv_all(s, out, take);
        rep->len = take;
    }
    closesocket(s);
    return ok;
}

static void complete(struct conn *c, struct pending *p)
{
    (void)c;
    p->finished = TRUE;
    if (p->ov) {
        p->ov->InternalHigh = p->bytes;
        p->ov->Internal = p->error; /* 0 = success; read back by our GetOverlappedResult */
        if (p->ov->hEvent) SetEvent(p->ov->hEvent);
        else SetEvent(c->handle);
        HeapFree(GetProcessHeap(), 0, p);
    } else {
        SetEvent(p->done);
    }
}

static DWORD WINAPI rx_main(void *arg)
{
    struct conn *c = arg;
    for (;;) {
        struct shim_msg r;
        struct pending **pp, *p = NULL;
        if (!recv_all(c->sock, &r, sizeof(r)) || r.magic != SHIM_MAGIC) break;
        EnterCriticalSection(&c->lock);
        for (pp = &c->pending; *pp; pp = &(*pp)->next)
            if ((*pp)->id == r.id) { p = *pp; *pp = p->next; break; }
        c->flags = r.flags;
        LeaveCriticalSection(&c->lock);
        if (!p) { /* unknown id: drain */
            char sink[4096];
            uint32_t left = r.len;
            while (left) { uint32_t k = left < sizeof(sink) ? left : sizeof(sink); if (!recv_all(c->sock, sink, k)) goto out; left -= k; }
            continue;
        }
        {
            uint32_t take = r.len < p->buf_len ? r.len : p->buf_len;
            uint32_t left = r.len - take;
            if (take && !recv_all(c->sock, p->buf, take)) { p->error = ERROR_GEN_FAILURE; complete(c, p); break; }
            while (left) { char sink[4096]; uint32_t k = left < sizeof(sink) ? left : sizeof(sink); if (!recv_all(c->sock, sink, k)) break; left -= k; }
        }
        p->error = r.error;
        p->bytes = r.bytes;
        complete(c, p);
    }
out:
    /* connection gone: fail everything still pending */
    EnterCriticalSection(&c->lock);
    c->dead = TRUE;
    while (c->pending) {
        struct pending *p = c->pending;
        c->pending = p->next;
        p->error = ERROR_DEVICE_NOT_CONNECTED;
        p->bytes = 0;
        complete(c, p);
    }
    LeaveCriticalSection(&c->lock);
    return 0;
}

/* Send a request; payload = (pay, pay_len). Reply payload is written to (buf, buf_len).
 * With ov == NULL, waits and returns the reply's error code (and *bytes).
 * With ov, returns ERROR_IO_PENDING after queuing. */
static DWORD conn_request(struct conn *c, uint32_t op, uint32_t arg, uint32_t arg2, const void *pay, DWORD pay_len,
                          void *buf, DWORD buf_len, OVERLAPPED *ov, DWORD *bytes)
{
    struct pending *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
    struct shim_msg m;
    DWORD err;
    if (!p) return ERROR_NOT_ENOUGH_MEMORY;
    p->op = op; p->buf = buf; p->buf_len = buf_len; p->ov = ov;
    if (!ov) p->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    else {
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        if (ov->hEvent) ResetEvent(ov->hEvent);
        else ResetEvent(c->handle);
    }
    EnterCriticalSection(&c->lock);
    if (c->dead) {
        LeaveCriticalSection(&c->lock);
        if (p->done) real_CloseHandle(p->done);
        if (ov) ov->Internal = ERROR_DEVICE_NOT_CONNECTED;
        HeapFree(GetProcessHeap(), 0, p);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    p->id = ++c->next_id;
    p->next = c->pending;
    c->pending = p;
    m.magic = SHIM_MAGIC; m.op = op; m.id = p->id; m.arg = arg; m.arg2 = arg2; m.len = pay_len;
    m.error = m.bytes = m.flags = 0;
    if (!send_all(c->sock, &m, sizeof(m)) || (pay_len && !send_all(c->sock, pay, pay_len))) {
        /* the receiver thread will fail it when the socket closes */
        shutdown(c->sock, SD_BOTH);
    }
    LeaveCriticalSection(&c->lock);
    if (ov) return ERROR_IO_PENDING;
    WaitForSingleObject(p->done, INFINITE);
    real_CloseHandle(p->done);
    err = p->error;
    if (bytes) *bytes = p->bytes;
    HeapFree(GetProcessHeap(), 0, p);
    return err;
}

/* ---- fake device information sets -------------------------------------------------------------- */
#define DEVINFO_MAGIC 0x55534249u
struct devinfo {
    uint32_t magic;
    GUID guid;
    USHORT pid;
    DWORD count;
    struct shim_dev devs[16];
};

static int iface_index(const GUID *g)
{
    unsigned i;
    if (!g) return -1;
    for (i = 0; i < N_IFACES; i++) if (IsEqualGUID(g, &avisoft_ifaces[i].guid)) return (int)i;
    return -1;
}
static struct devinfo *devinfos[32]; /* the sets we handed out (guarded by table_cs) */
static BOOL is_ours(HDEVINFO h)
{
    BOOL ok = FALSE;
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    EnterCriticalSection(&table_cs);
    for (i = 0; i < 32; i++) if ((HDEVINFO)devinfos[i] == h) { ok = TRUE; break; }
    LeaveCriticalSection(&table_cs);
    return ok;
}
static void remember_devinfo(struct devinfo *d, BOOL add)
{
    int i;
    EnterCriticalSection(&table_cs);
    for (i = 0; i < 32; i++) {
        if (add && !devinfos[i]) { devinfos[i] = d; break; }
        if (!add && devinfos[i] == d) { devinfos[i] = NULL; break; }
    }
    LeaveCriticalSection(&table_cs);
}

static void make_path(char *out, size_t n, const struct devinfo *d, DWORD idx)
{
    const GUID *g = &d->guid;
    _snprintf(out, n, "%svid_%04x&pid_%04x#%lu#{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", path_prefix,
              d->devs[idx].vid, d->devs[idx].pid, (unsigned long)d->devs[idx].index,
              g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
              g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    out[n - 1] = 0;
}

static HDEVINFO WINAPI my_SetupDiGetClassDevsA(const GUID *guid, PCSTR enumerator, HWND hwnd, DWORD flags)
{
    int k = iface_index(guid);
    struct devinfo *d;
    struct shim_msg rep = {0};
    if (k < 0 || !(flags & DIGCF_DEVICEINTERFACE)) return real_SetupDiGetClassDevsA(guid, enumerator, hwnd, flags);
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    d->magic = DEVINFO_MAGIC;
    d->guid = *guid;
    d->pid = avisoft_ifaces[k].pid;
    if (simple_call(OP_LIST, d->pid, 0, &rep, d->devs, sizeof(d->devs)))
        d->count = rep.len / sizeof(struct shim_dev);
    remember_devinfo(d, TRUE);
    logf_("SetupDiGetClassDevsA(pid %04x, flags %#lx) -> %lu device(s)%s", d->pid, flags, d->count,
          rep.magic == SHIM_MAGIC ? "" : " (helper not reachable)");
    return (HDEVINFO)d;
}

static BOOL WINAPI my_SetupDiEnumDeviceInterfaces(HDEVINFO h, PSP_DEVINFO_DATA dev, const GUID *guid, DWORD index, PSP_DEVICE_INTERFACE_DATA out)
{
    struct devinfo *d = (struct devinfo *)h;
    if (!is_ours(h)) return real_SetupDiEnumDeviceInterfaces(h, dev, guid, index, out);
    if (index >= d->count) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    if (!out || out->cbSize != sizeof(*out)) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
    out->InterfaceClassGuid = d->guid;
    out->Flags = SPINT_ACTIVE;
    out->Reserved = index + 1;
    logf_("SetupDiEnumDeviceInterfaces(index %lu) -> device %04x:%04x serial '%s'", index, d->devs[index].vid, d->devs[index].pid, d->devs[index].serial);
    return TRUE;
}

static BOOL WINAPI my_SetupDiGetDeviceInterfaceDetailA(HDEVINFO h, PSP_DEVICE_INTERFACE_DATA iface, PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail,
                                                       DWORD size, PDWORD required, PSP_DEVINFO_DATA dev)
{
    struct devinfo *d = (struct devinfo *)h;
    char path[160];
    DWORD need, idx;
    if (!is_ours(h)) return real_SetupDiGetDeviceInterfaceDetailA(h, iface, detail, size, required, dev);
    if (!iface || !iface->Reserved || iface->Reserved > d->count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    idx = (DWORD)iface->Reserved - 1;
    make_path(path, sizeof(path), d, idx);
    need = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + strlen(path) + 1);
    if (required) *required = need;
    if (!detail || size < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    strcpy(detail->DevicePath, path);
    if (dev && dev->cbSize == sizeof(*dev)) { memset(&dev->ClassGuid, 0, sizeof(dev->ClassGuid)); dev->DevInst = idx + 1; }
    logf_("SetupDiGetDeviceInterfaceDetailA -> %s", path);
    return TRUE;
}

static BOOL WINAPI my_SetupDiDestroyDeviceInfoList(HDEVINFO h)
{
    if (!is_ours(h)) return real_SetupDiDestroyDeviceInfoList(h);
    remember_devinfo((struct devinfo *)h, FALSE);
    ((struct devinfo *)h)->magic = 0;
    HeapFree(GetProcessHeap(), 0, h);
    return TRUE;
}

/* ---- device handles ---------------------------------------------------------------------------- */
static HANDLE open_device(const char *path)
{
    unsigned vid, pid;
    unsigned long index;
    struct conn *c;
    struct shim_msg m, r;
    int i;
    if (sscanf(path + sizeof(path_prefix) - 1, "vid_%4x&pid_%4x#%lu#", &vid, &pid, &index) != 3) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    c->sock = helper_connect();
    if (c->sock == INVALID_SOCKET) {
        logf_("CreateFile(%s): helper not reachable on port %d", path, helper_port());
        HeapFree(GetProcessHeap(), 0, c);
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return INVALID_HANDLE_VALUE;
    }
    memset(&m, 0, sizeof(m));
    m.magic = SHIM_MAGIC; m.op = OP_OPEN; m.id = 0; m.arg = pid; m.arg2 = (uint32_t)index;
    if (!send_all(c->sock, &m, sizeof(m)) || !recv_all(c->sock, &r, sizeof(r)) || r.error) {
        DWORD e = r.error ? r.error : ERROR_DEVICE_NOT_CONNECTED;
        logf_("CreateFile(%s): helper refused the open, error %#lx", path, e);
        closesocket(c->sock);
        HeapFree(GetProcessHeap(), 0, c);
        SetLastError(e);
        return INVALID_HANDLE_VALUE;
    }
    InitializeCriticalSection(&c->lock);
    lstrcpynA(c->path, path, sizeof(c->path));
    c->handle = CreateEventA(NULL, TRUE, FALSE, NULL);
    EnterCriticalSection(&table_cs);
    for (i = 0; i < MAX_CONNS; i++) if (!conns[i]) { conns[i] = c; break; }
    LeaveCriticalSection(&table_cs);
    c->rx_thread = CreateThread(NULL, 0, rx_main, c, 0, NULL);
    SetThreadPriority(c->rx_thread, THREAD_PRIORITY_TIME_CRITICAL);
    logf_("CreateFile(%s) -> handle %p", path, c->handle);
    return c->handle;
}

static HANDLE WINAPI my_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (name && !_strnicmp(name, path_prefix, sizeof(path_prefix) - 1)) return open_device(name);
    return real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI my_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (name && !_wcsnicmp(name, L"\\\\?\\usbioshim#", 14)) {
        char a[160];
        WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof(a), NULL, NULL);
        return open_device(a);
    }
    return real_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static BOOL finish_call(DWORD err, LPOVERLAPPED ov, DWORD bytes, LPDWORD out_bytes)
{
    if (out_bytes) *out_bytes = bytes;
    if (ov) { /* completed synchronously: still report through the OVERLAPPED */
        ov->Internal = err;
        ov->InternalHigh = bytes;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    if (err) { SetLastError(err); return FALSE; }
    return TRUE;
}

static BOOL WINAPI my_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD ret, LPOVERLAPPED ov)
{
    struct conn *c = find_conn(h);
    DWORD err, bytes = 0;
    if (!c) return real_DeviceIoControl(h, code, in, in_len, out, out_len, ret, ov);
    logf_("DeviceIoControl(%p, code %#lx, in %lu, out %lu%s)", h, code, in_len, out_len, ov ? ", overlapped" : "");
    hexdump_("in", in, in_len);
    if ((code & 3) == 1 /* METHOD_IN_DIRECT: the output buffer is data for the device */) {
        BYTE *pay = HeapAlloc(GetProcessHeap(), 0, in_len + out_len + 1);
        if (in_len) memcpy(pay, in, in_len);
        if (out_len) memcpy(pay + in_len, out, out_len);
        hexdump_("data", out, out_len);
        err = conn_request(c, OP_IOCTL, code, out_len, pay, in_len + out_len, NULL, 0, NULL, &bytes);
        HeapFree(GetProcessHeap(), 0, pay);
    } else
        err = conn_request(c, OP_IOCTL, code, out_len, in, in_len, out, out_len, NULL, &bytes);
    logf_("  -> error %#lx, %lu bytes", err, bytes);
    if (!err && (code & 3) != 1) hexdump_("out", out, bytes);
    return finish_call(err, ov, bytes, ret);
}

static DWORD iso_header_len(struct conn *c, const BYTE *buf, DWORD len)
{
    DWORD n;
    if (!(c->flags & SHIM_F_ISO_PIPE) || len < 16) return 0;
    n = ((const DWORD *)buf)[0]; /* USBIO_ISO_TRANSFER.NumberOfPackets */
    if (n > 4096) n = 4096;
    n = 16 + 12 * n;             /* USBIO_ISO_TRANSFER + n * USBIO_ISO_PACKET */
    return n < len ? n : len;
}

static BOOL WINAPI my_ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD got, LPOVERLAPPED ov)
{
    struct conn *c = find_conn(h);
    DWORD err, bytes = 0, hdr;
    static LONG nlogged;
    if (!c) return real_ReadFile(h, buf, len, got, ov);
    hdr = iso_header_len(c, buf, len);
    if (InterlockedIncrement(&nlogged) <= 40) logf_("ReadFile(%p, %lu bytes, iso header %lu%s)", h, len, hdr, ov ? ", overlapped" : "");
    err = conn_request(c, OP_READ, len, 0, buf, hdr, buf, len, ov, &bytes);
    if (err == ERROR_IO_PENDING) { SetLastError(ERROR_IO_PENDING); return FALSE; }
    return finish_call(err, ov, bytes, got);
}

static BOOL WINAPI my_WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD put, LPOVERLAPPED ov)
{
    struct conn *c = find_conn(h);
    DWORD err, bytes = 0, hdr;
    if (!c) return real_WriteFile(h, buf, len, put, ov);
    hdr = iso_header_len(c, buf, len);
    logf_("WriteFile(%p, %lu bytes%s)", h, len, ov ? ", overlapped" : "");
    /* iso writes get the header back (packet status); bulk writes get nothing */
    err = conn_request(c, OP_WRITE, len, 0, buf, len, (void *)buf, hdr, ov, &bytes);
    if (err == ERROR_IO_PENDING) { SetLastError(ERROR_IO_PENDING); return FALSE; }
    return finish_call(err, ov, bytes, put);
}

static BOOL WINAPI my_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, BOOL wait)
{
    struct conn *c = find_conn(h);
    if (!c) return real_GetOverlappedResult(h, ov, bytes, wait);
    if (ov->Internal == STATUS_PENDING) {
        if (!wait) { SetLastError(ERROR_IO_INCOMPLETE); return FALSE; }
        while (ov->Internal == STATUS_PENDING) WaitForSingleObject(ov->hEvent ? ov->hEvent : h, INFINITE);
    }
    if (bytes) *bytes = (DWORD)ov->InternalHigh;
    if (ov->Internal) { SetLastError((DWORD)ov->Internal); return FALSE; }
    return TRUE;
}

static BOOL WINAPI my_CancelIo(HANDLE h)
{
    struct conn *c = find_conn(h);
    if (!c) return real_CancelIo(h);
    logf_("CancelIo(%p)", h);
    /* fire and forget: the helper completes every pending transfer with ERROR_OPERATION_ABORTED */
    EnterCriticalSection(&c->lock);
    if (!c->dead) {
        struct shim_msg m = {SHIM_MAGIC, OP_CANCEL, 0, 0, 0, 0, 0, 0, 0};
        send_all(c->sock, &m, sizeof(m));
    }
    LeaveCriticalSection(&c->lock);
    return TRUE;
}

static BOOL WINAPI my_CloseHandle(HANDLE h)
{
    struct conn *c = find_conn(h);
    int i;
    if (!c) return real_CloseHandle(h);
    logf_("CloseHandle(%p) %s", h, c->path);
    EnterCriticalSection(&table_cs);
    for (i = 0; i < MAX_CONNS; i++) if (conns[i] == c) conns[i] = NULL;
    LeaveCriticalSection(&table_cs);
    shutdown(c->sock, SD_BOTH);
    closesocket(c->sock);
    WaitForSingleObject(c->rx_thread, 5000);
    real_CloseHandle(c->rx_thread);
    real_CloseHandle(c->handle);
    DeleteCriticalSection(&c->lock);
    HeapFree(GetProcessHeap(), 0, c);
    return TRUE;
}

/* ---- import table patching --------------------------------------------------------------------- */
static int hook(HMODULE mod, const char *dll, const char *name, void *repl, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    FARPROC target = GetProcAddress(GetModuleHandleA(dll), name);
    int patched = 0;
    if (!dir->VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *iat, *names;
        if (lstrcmpiA((char *)(base + imp->Name), dll)) continue;
        iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk) : NULL;
        for (; iat->u1.Function; iat++, names = names ? names + 1 : NULL) {
            BOOL match = target && (FARPROC)iat->u1.Function == target;
            if (!match && names && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                match = !strcmp((char *)ibn->Name, name);
            }
            if (match) {
                DWORD old;
                if (!*orig) *orig = (void *)iat->u1.Function;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old);
                iat->u1.Function = (ULONG_PTR)repl;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old, &old);
                patched++;
            }
        }
    }
    if (!*orig && target) *orig = (void *)target;
    return patched;
}

#define HOOK(dll, fn) n += hook(exe, dll, #fn, (void *)my_##fn, (void **)&real_##fn)

static void install_hooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    char v[8];
    int n = 0;
    LoadLibraryA("setupapi.dll");
    if (GetEnvironmentVariableA("USBIO_SHIM_OFF", v, sizeof(v)) && v[0] == '1') { logf_("USBIO_SHIM_OFF=1: no hooks"); return; }
    HOOK("setupapi.dll", SetupDiGetClassDevsA);
    HOOK("setupapi.dll", SetupDiEnumDeviceInterfaces);
    HOOK("setupapi.dll", SetupDiGetDeviceInterfaceDetailA);
    HOOK("setupapi.dll", SetupDiDestroyDeviceInfoList);
    HOOK("kernel32.dll", CreateFileA);
    HOOK("kernel32.dll", CreateFileW);
    HOOK("kernel32.dll", DeviceIoControl);
    HOOK("kernel32.dll", ReadFile);
    HOOK("kernel32.dll", WriteFile);
    HOOK("kernel32.dll", GetOverlappedResult);
    HOOK("kernel32.dll", CancelIo);
    HOOK("kernel32.dll", CloseHandle);
    logf_("usbio-shim: %d import entries redirected in the main program (helper port %d)", n, helper_port());
}

/* ---- the one real export ----------------------------------------------------------------------- */
BOOLEAN WINAPI SetSuspendState(BOOLEAN hibernate, BOOLEAN force, BOOLEAN wakeup_disabled)
{
    static BOOLEAN (WINAPI *real)(BOOLEAN, BOOLEAN, BOOLEAN);
    if (!real) {
        char path[MAX_PATH];
        HMODULE m;
        GetSystemDirectoryA(path, MAX_PATH - 16);
        lstrcatA(path, "\\powrprof.dll");
        m = LoadLibraryA(path);
        if (m && m != GetModuleHandleA("powrprof.dll")) real = (void *)GetProcAddress(m, "SetSuspendState");
    }
    logf_("SetSuspendState(%d, %d, %d) %s", hibernate, force, wakeup_disabled, real ? "forwarded" : "not available");
    if (!real) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    return real(hibernate, force, wakeup_disabled);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&log_cs);
        InitializeCriticalSection(&table_cs);
        if (GetEnvironmentVariableA("USBIO_SHIM_LOG", path, sizeof(path)))
            log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        install_hooks();
    }
    return TRUE;
}
