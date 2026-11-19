/* usbio_client.exe: exercises the USBIO interface the way a USBIO application does, through the
 * shim (load it with the same powrprof.dll override as rec_usgh.exe, or link nothing: this exe
 * imports powrprof.dll's SetSuspendState only to pull the shim in).
 *
 *   usbio_client.exe [seconds=5] [devices=1] [packets=256] [buffers=32]
 *
 * Per device (one thread each): find it via SetupDi with the 1216H's interface GUID, open,
 * GET_DRIVER_INFO, device/config/string descriptors, SET_CONFIGURATION (interface 0, alt 1),
 * GET_CONFIGURATION_INFO, a vendor IN and OUT request, a bulk OUT -> bulk IN echo, then streams
 * the isochronous IN pipe for N seconds with `buffers` overlapped reads of `packets` packets,
 * checking the simulated device's uint16 counter for gaps, and finally CancelIo + close.
 * Exit code 0 = every check passed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../usbio_defs.h"

static const GUID guid_1216h = {0xA75806F7, 0xE40E, 0x4ce1, {0xBA, 0xA3, 0x20, 0x24, 0xE4, 0xBA, 0xCE, 0x37}};
static int seconds = 5, ndev = 1, npk = 256, nbuf = 32;
static volatile LONG failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("dev%d FAIL: ", id); printf(__VA_ARGS__); printf(" (error %#lx)\n", GetLastError()); InterlockedIncrement(&failures); goto out; } } while (0)

static BOOL ioctl(HANDLE h, DWORD code, void *in, DWORD in_len, void *out, DWORD out_len, DWORD *ret)
{
    OVERLAPPED ov = {0};
    DWORD n = 0;
    BOOL ok;
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ok = DeviceIoControl(h, code, in, in_len, out, out_len, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &n, TRUE);
    CloseHandle(ov.hEvent);
    if (ret) *ret = n;
    return ok;
}

static HANDLE open_path(const char *path)
{
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
}

static char paths[8][256];

static DWORD WINAPI run_device(void *arg)
{
    int id = (int)(INT_PTR)arg, i;
    HANDLE h = INVALID_HANDLE_VALUE, hiso = INVALID_HANDLE_VALUE, hout = INVALID_HANDLE_VALUE, hin = INVALID_HANDLE_VALUE;
    USBIO_DRIVER_INFO di;
    USBIO_DESCRIPTOR_REQUEST dr = {0};
    UCHAR desc[512];
    USBIO_SET_CONFIGURATION sc = {0};
    static USBIO_CONFIGURATION_INFO ci;
    USBIO_CLASS_OR_VENDOR_REQUEST vr = {0};
    USBIO_BIND_PIPE bp;
    DWORD n, slot = 1024, buf_len, t0, got_packets = 0, gaps = 0, short_reads = 0;
    ULONGLONG bytes = 0;
    UCHAR **bufs = NULL;
    OVERLAPPED *ovs = NULL;
    int expect = -1;

    h = open_path(paths[id]);
    CHECK(h != INVALID_HANDLE_VALUE, "open %s", paths[id]);
    CHECK(ioctl(h, IOCTL_USBIO_GET_DRIVER_INFO, NULL, 0, &di, sizeof(di), &n) && n == sizeof(di), "GET_DRIVER_INFO");
    printf("dev%d: driver API %#x, driver %#x\n", id, di.APIVersion, di.DriverVersion);
    dr.Recipient = RecipientDevice; dr.DescriptorType = 1;
    CHECK(ioctl(h, IOCTL_USBIO_GET_DESCRIPTOR, &dr, sizeof(dr), desc, sizeof(desc), &n) && n == 18, "device descriptor (%lu bytes)", n);
    CHECK(desc[8] == 0x47 && desc[9] == 0x05 && desc[10] == 0x12 && desc[11] == 0x10, "VID/PID %02x%02x:%02x%02x", desc[9], desc[8], desc[11], desc[10]);
    dr.DescriptorType = 2;
    CHECK(ioctl(h, IOCTL_USBIO_GET_DESCRIPTOR, &dr, sizeof(dr), desc, sizeof(desc), &n) && n >= 9 && desc[1] == 2, "config descriptor");
    dr.DescriptorType = 3; dr.DescriptorIndex = 3; dr.LanguageId = 0x0409;
    CHECK(ioctl(h, IOCTL_USBIO_GET_DESCRIPTOR, &dr, sizeof(dr), desc, sizeof(desc), &n) && n > 2 && desc[1] == 3, "serial string");
    {
        char s[64]; DWORD k;
        for (k = 2; k + 1 < n && (k - 2) / 2 < sizeof(s) - 1; k += 2) s[(k - 2) / 2] = (char)desc[k];
        s[(n - 2) / 2] = 0;
        printf("dev%d: serial '%s'\n", id, s);
    }
    sc.ConfigurationIndex = 0; sc.NbOfInterfaces = 1;
    sc.InterfaceList[0].InterfaceIndex = 0; sc.InterfaceList[0].AlternateSettingIndex = 1; sc.InterfaceList[0].MaximumTransferSize = 1 << 20;
    CHECK(ioctl(h, IOCTL_USBIO_SET_CONFIGURATION, &sc, sizeof(sc), NULL, 0, NULL), "SET_CONFIGURATION");
    CHECK(ioctl(h, IOCTL_USBIO_GET_CONFIGURATION_INFO, NULL, 0, &ci, sizeof(ci), &n) && n == sizeof(ci), "GET_CONFIGURATION_INFO");
    CHECK(ci.NbOfInterfaces == 1 && ci.NbOfPipes == 3, "config info: %lu interfaces, %lu pipes", ci.NbOfInterfaces, ci.NbOfPipes);
    for (i = 0; i < (int)ci.NbOfPipes; i++)
        printf("dev%d: pipe %02x type %d maxpkt %u\n", id, ci.PipeInfo[i].EndpointAddress, (int)ci.PipeInfo[i].PipeType, ci.PipeInfo[i].MaximumPacketSize);

    vr.Type = RequestTypeVendor; vr.Recipient = RecipientDevice; vr.Request = 0x10; vr.Value = 0x1234; vr.Index = 5;
    CHECK(ioctl(h, IOCTL_USBIO_CLASS_OR_VENDOR_IN_REQUEST, &vr, sizeof(vr), desc, 8, &n) && n == 8, "vendor IN");
    CHECK(desc[0] == 0x10 && desc[1] == 0x34 && desc[2] == 0x12 && desc[3] == 5, "vendor IN data %02x %02x %02x %02x", desc[0], desc[1], desc[2], desc[3]);
    vr.Request = 0x11; memcpy(desc, "\x01\x02\x03\x04", 4);
    CHECK(ioctl(h, IOCTL_USBIO_CLASS_OR_VENDOR_OUT_REQUEST, &vr, sizeof(vr), desc, 4, &n) && n == 4, "vendor OUT (%lu bytes)", n);
    CHECK(!memcmp(desc, "\x01\x02\x03\x04", 4), "vendor OUT changed the caller's buffer");

    /* bulk echo through two more handles */
    hout = open_path(paths[id]); hin = open_path(paths[id]);
    bp.EndpointAddress = 0x02; CHECK(ioctl(hout, IOCTL_USBIO_BIND_PIPE, &bp, sizeof(bp), NULL, 0, NULL), "bind 02");
    bp.EndpointAddress = 0x86; CHECK(ioctl(hin, IOCTL_USBIO_BIND_PIPE, &bp, sizeof(bp), NULL, 0, NULL), "bind 86");
    bp.EndpointAddress = 0x02; CHECK(!ioctl(hin, IOCTL_USBIO_BIND_PIPE, &bp, sizeof(bp), NULL, 0, NULL) && GetLastError() == USBIO_ERR_ALREADY_BOUND, "second bind refused");
    {
        OVERLAPPED ov = {0};
        char msg[64], back[512] = {0};
        sprintf(msg, "hello from device %d", id);
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        CHECK(WriteFile(hout, msg, (DWORD)strlen(msg) + 1, NULL, &ov) || GetLastError() == ERROR_IO_PENDING, "bulk write");
        CHECK(GetOverlappedResult(hout, &ov, &n, TRUE) && n == strlen(msg) + 1, "bulk write result");
        CHECK(ReadFile(hin, back, sizeof(back), NULL, &ov) || GetLastError() == ERROR_IO_PENDING, "bulk read");
        CHECK(GetOverlappedResult(hin, &ov, &n, TRUE) && n == strlen(msg) + 1 && !strcmp(back, msg), "bulk echo ('%s', %lu bytes)", back, n);
        CloseHandle(ov.hEvent);
        printf("dev%d: bulk echo ok\n", id);
    }

    /* isochronous stream */
    hiso = open_path(paths[id]);
    bp.EndpointAddress = 0x82;
    CHECK(ioctl(hiso, IOCTL_USBIO_BIND_PIPE, &bp, sizeof(bp), NULL, 0, NULL), "bind 82");
    buf_len = (DWORD)(sizeof(USBIO_ISO_TRANSFER) + npk * sizeof(USBIO_ISO_PACKET) + npk * slot);
    bufs = calloc(nbuf, sizeof(*bufs));
    ovs = calloc(nbuf, sizeof(*ovs));
    for (i = 0; i < nbuf; i++) {
        bufs[i] = malloc(buf_len);
        ovs[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
#define SUBMIT(i) do { \
        USBIO_ISO_TRANSFER_HEADER *hd = (USBIO_ISO_TRANSFER_HEADER *)bufs[i]; int p_; \
        DWORD base_ = (DWORD)(sizeof(USBIO_ISO_TRANSFER) + npk * sizeof(USBIO_ISO_PACKET)); \
        memset(hd, 0, base_); hd->IsoTransfer.NumberOfPackets = npk; hd->IsoTransfer.Flags = USBIO_START_TRANSFER_ASAP | USBIO_SHORT_TRANSFER_OK; \
        for (p_ = 0; p_ < npk; p_++) hd->IsoPacket[p_].Offset = base_ + p_ * slot; \
        CHECK(ReadFile(hiso, bufs[i], buf_len, NULL, &ovs[i]) || GetLastError() == ERROR_IO_PENDING, "iso read submit"); } while (0)
    for (i = 0; i < nbuf; i++) SUBMIT(i);
    t0 = GetTickCount();
    for (i = 0; GetTickCount() - t0 < (DWORD)seconds * 1000; i = (i + 1) % nbuf) {
        USBIO_ISO_TRANSFER_HEADER *hd = (USBIO_ISO_TRANSFER_HEADER *)bufs[i];
        int p;
        CHECK(GetOverlappedResult(hiso, &ovs[i], &n, TRUE), "iso read");
        CHECK(hd->IsoTransfer.ErrorCount == 0, "iso ErrorCount %lu", hd->IsoTransfer.ErrorCount);
        for (p = 0; p < npk; p++) {
            const USHORT *v = (const USHORT *)(bufs[i] + hd->IsoPacket[p].Offset);
            DWORD k, cnt = hd->IsoPacket[p].Length / 2;
            if (hd->IsoPacket[p].Status) gaps++;
            if (hd->IsoPacket[p].Length < 750) short_reads++;
            for (k = 0; k < cnt; k++) {
                if (expect >= 0 && v[k] != (USHORT)expect) gaps++;
                expect = (USHORT)(v[k] + 1);
            }
            bytes += hd->IsoPacket[p].Length;
            got_packets++;
        }
        SUBMIT(i);
    }
    {
        double secs = (GetTickCount() - t0) / 1000.0;
        printf("dev%d: %lu packets, %.1f MB in %.1f s = %.2f MB/s, counter gaps %lu, short packets %lu\n",
               id, got_packets, bytes / 1e6, secs, bytes / 1e6 / secs, gaps, short_reads);
        CHECK(gaps == 0, "data gaps");
        CHECK(bytes / secs > 5.5e6, "rate below 5.5 MB/s");
    }
    /* cancel everything still queued: each must finish with USBIO_ERR_CANCELED */
    CHECK(CancelIo(hiso), "CancelIo");
    {
        int cancelled = 0, ok = 0;
        for (i = 0; i < nbuf; i++) {
            if (GetOverlappedResult(hiso, &ovs[i], &n, TRUE)) ok++;
            else if (GetLastError() == USBIO_ERR_CANCELED) cancelled++;
            else CHECK(0, "after CancelIo: unexpected error");
        }
        printf("dev%d: after CancelIo %d cancelled, %d had already completed\n", id, cancelled, ok);
    }
    printf("dev%d: OK\n", id);
out:
    if (hiso != INVALID_HANDLE_VALUE) CloseHandle(hiso);
    if (hin != INVALID_HANDLE_VALUE) CloseHandle(hin);
    if (hout != INVALID_HANDLE_VALUE) CloseHandle(hout);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}

int main(int argc, char **argv)
{
    HDEVINFO di;
    SP_DEVICE_INTERFACE_DATA ifd;
    HANDLE th[8];
    int i, found = 0;
    if (argc > 1) seconds = atoi(argv[1]);
    if (argc > 2) ndev = atoi(argv[2]);
    if (argc > 3) npk = atoi(argv[3]);
    if (argc > 4) nbuf = atoi(argv[4]);
    if (argc > 99) SetSuspendState(FALSE, FALSE, FALSE);   /* keeps the powrprof.dll import (the shim) */
    di = SetupDiGetClassDevsA(&guid_1216h, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    for (i = 0; i < 8; i++) {
        BYTE buf[512];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *d = (void *)buf;
        DWORD need;
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(di, NULL, &guid_1216h, i, &ifd)) break;
        d->cbSize = sizeof(*d);
        if (!SetupDiGetDeviceInterfaceDetailA(di, &ifd, d, sizeof(buf), &need, NULL)) break;
        strcpy(paths[i], d->DevicePath);
        printf("found %s\n", paths[i]);
        found++;
    }
    SetupDiDestroyDeviceInfoList(di);
    if (found < ndev) { printf("FAIL: found %d device(s), need %d\n", found, ndev); return 1; }
    for (i = 0; i < ndev; i++) th[i] = CreateThread(NULL, 0, run_device, (void *)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(ndev, th, TRUE, INFINITE);
    printf(failures ? "FAILED (%ld)\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
