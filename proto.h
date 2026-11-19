/* Wire protocol between the Windows-side shim (powrprof.dll inside rec_usgh.exe under Wine)
 * and the native helper (usbio-helper, libusb). TCP on 127.0.0.1, one connection per
 * USBIO handle the application opens. All integers little-endian.
 *
 * Every message = struct shim_msg header + `len` payload bytes.
 * Requests carry a client-chosen id; the reply echoes it. READ/WRITE replies may arrive out of
 * order and long after the request (isochronous streaming); everything else is answered in order.
 */
#ifndef USBIO_SHIM_PROTO_H
#define USBIO_SHIM_PROTO_H
#include <stdint.h>

#define SHIM_DEFAULT_PORT 47321
#define SHIM_MAGIC 0x4F494253u /* "SBIO" */

enum shim_op {
    OP_HELLO  = 1, /* arg = protocol version; reply arg = helper version */
    OP_LIST   = 2, /* arg = USB product id (vendor 0x0547 implied unless arg2 != 0); reply payload = shim_dev[] */
    OP_OPEN   = 3, /* arg = product id, arg2 = device index from OP_LIST */
    OP_IOCTL  = 4, /* arg = IOCTL code, arg2 = output buffer size; payload = input buffer */
    OP_READ   = 5, /* arg = transfer length; payload = iso header (iso pipes) or nothing */
    OP_WRITE  = 6, /* arg = transfer length; payload = whole buffer */
    OP_CANCEL = 7, /* cancel every pending READ/WRITE of this connection */
};

#pragma pack(push, 1)
struct shim_msg {
    uint32_t magic;
    uint32_t op;
    uint32_t id;
    uint32_t arg;
    uint32_t arg2;
    uint32_t len;      /* payload bytes that follow */
    /* reply-only fields (zero in requests) */
    uint32_t error;    /* Win32 / USBIO error code for GetLastError(); 0 = success */
    uint32_t bytes;    /* bytes transferred (BytesReturned / lpNumberOfBytesTransferred) */
    uint32_t flags;    /* SHIM_F_* */
};

struct shim_dev {
    uint16_t vid, pid;
    uint32_t index;    /* position in the helper's stable device order */
    uint8_t  bus, port_path[7];
    char     serial[64];
};
#pragma pack(pop)

#define SHIM_F_ISO_PIPE 1u /* this connection is bound to an isochronous pipe: READ/WRITE send the iso header */

#define SHIM_PROTO_VERSION 1
#endif
