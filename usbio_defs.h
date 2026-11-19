/*
 * usbio_defs.h: the requests, data layouts and status values that RECORDER USGH
 * exchanges with its USB driver, for usbio-helper (which answers them) and the
 * test client (which sends them).
 *
 * The driver Avisoft ships (usgh_x64.sys) is a renamed build of Thesycon's
 * USBIO generic driver, so an application talks to it with DeviceIoControl,
 * ReadFile and WriteFile in USBIO's format. To interoperate, the names, numeric
 * values and byte layouts below follow that format. The descriptions are ours
 * and say only what usbio-helper relies on.
 *
 * Layouts are packed (1-byte alignment), contain no pointers, and use 32-bit
 * fields where the Windows API has enums, so a 32-bit application and a 64-bit
 * helper see the same bytes. Every size is checked at compile time below.
 */
#ifndef USBIO_DEFS_H
#define USBIO_DEFS_H

#include <stdint.h>

#ifndef _WIN32
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint8_t UCHAR;
#endif

/* ---- Request codes ---------------------------------------------------------------------------
 * A Windows I/O control code packs a device type, an access mask, a function number and a
 * buffer method: (type << 16) | (access << 14) | (function << 2) | method. USBIO uses device
 * type 0x8094, access 0 and function numbers from 0x801 up, so each code is
 * 0x80942000 + 4 * n + method. The method says which buffer carries the data stage. */
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED   0
#define METHOD_IN_DIRECT  1   /* the "output" buffer carries data TO the device */
#define METHOD_OUT_DIRECT 2   /* the "output" buffer receives data FROM the device */
#define METHOD_NEITHER    3
#endif
#define USBIO_REQUEST(n, method) ((ULONG)(0x80942000u + 4u * (n) + (method)))

/* Requests on the device (any handle). */
#define IOCTL_USBIO_GET_DESCRIPTOR             USBIO_REQUEST(1, METHOD_OUT_DIRECT)  /* 0x80942006 */
#define IOCTL_USBIO_SET_DESCRIPTOR             USBIO_REQUEST(2, METHOD_IN_DIRECT)   /* 0x80942009 */
#define IOCTL_USBIO_SET_FEATURE                USBIO_REQUEST(3, METHOD_BUFFERED)    /* 0x8094200C */
#define IOCTL_USBIO_CLEAR_FEATURE              USBIO_REQUEST(4, METHOD_BUFFERED)    /* 0x80942010 */
#define IOCTL_USBIO_GET_STATUS                 USBIO_REQUEST(5, METHOD_BUFFERED)    /* 0x80942014 */
#define IOCTL_USBIO_GET_CONFIGURATION          USBIO_REQUEST(6, METHOD_BUFFERED)    /* 0x80942018 */
#define IOCTL_USBIO_GET_INTERFACE              USBIO_REQUEST(7, METHOD_BUFFERED)    /* 0x8094201C */
#define IOCTL_USBIO_STORE_CONFIG_DESCRIPTOR    USBIO_REQUEST(8, METHOD_BUFFERED)    /* 0x80942020 */
#define IOCTL_USBIO_SET_CONFIGURATION          USBIO_REQUEST(9, METHOD_BUFFERED)    /* 0x80942024 */
#define IOCTL_USBIO_UNCONFIGURE_DEVICE         USBIO_REQUEST(10, METHOD_BUFFERED)   /* 0x80942028 */
#define IOCTL_USBIO_SET_INTERFACE              USBIO_REQUEST(11, METHOD_BUFFERED)   /* 0x8094202C */
#define IOCTL_USBIO_CLASS_OR_VENDOR_IN_REQUEST USBIO_REQUEST(12, METHOD_OUT_DIRECT) /* 0x80942032 */
#define IOCTL_USBIO_CLASS_OR_VENDOR_OUT_REQUEST USBIO_REQUEST(13, METHOD_IN_DIRECT) /* 0x80942035 */
#define IOCTL_USBIO_GET_DEVICE_PARAMETERS      USBIO_REQUEST(15, METHOD_BUFFERED)   /* 0x8094203C */
#define IOCTL_USBIO_SET_DEVICE_PARAMETERS      USBIO_REQUEST(16, METHOD_BUFFERED)   /* 0x80942040 */
#define IOCTL_USBIO_GET_CONFIGURATION_INFO     USBIO_REQUEST(20, METHOD_BUFFERED)   /* 0x80942050 */
#define IOCTL_USBIO_RESET_DEVICE               USBIO_REQUEST(21, METHOD_BUFFERED)   /* 0x80942054 */
#define IOCTL_USBIO_GET_CURRENT_FRAME_NUMBER   USBIO_REQUEST(22, METHOD_BUFFERED)   /* 0x80942058 */
#define IOCTL_USBIO_SET_DEVICE_POWER_STATE     USBIO_REQUEST(23, METHOD_BUFFERED)   /* 0x8094205C */
#define IOCTL_USBIO_GET_DEVICE_POWER_STATE     USBIO_REQUEST(24, METHOD_BUFFERED)   /* 0x80942060 */
#define IOCTL_USBIO_GET_BANDWIDTH_INFO         USBIO_REQUEST(25, METHOD_BUFFERED)   /* 0x80942064 */
#define IOCTL_USBIO_GET_DEVICE_INFO            USBIO_REQUEST(26, METHOD_BUFFERED)   /* 0x80942068 */
#define IOCTL_USBIO_GET_DRIVER_INFO            USBIO_REQUEST(27, METHOD_BUFFERED)   /* 0x8094206C */
#define IOCTL_USBIO_CYCLE_PORT                 USBIO_REQUEST(28, METHOD_BUFFERED)   /* 0x80942070 */
#define IOCTL_USBIO_ACQUIRE_DEVICE             USBIO_REQUEST(50, METHOD_BUFFERED)   /* 0x809420C8 */
#define IOCTL_USBIO_RELEASE_DEVICE             USBIO_REQUEST(51, METHOD_BUFFERED)   /* 0x809420CC */

/* Requests on a pipe: the handle must first be attached to one endpoint with BIND_PIPE. */
#define IOCTL_USBIO_BIND_PIPE                  USBIO_REQUEST(30, METHOD_BUFFERED)   /* 0x80942078 */
#define IOCTL_USBIO_UNBIND_PIPE                USBIO_REQUEST(31, METHOD_BUFFERED)   /* 0x8094207C */
#define IOCTL_USBIO_RESET_PIPE                 USBIO_REQUEST(32, METHOD_BUFFERED)   /* 0x80942080 */
#define IOCTL_USBIO_ABORT_PIPE                 USBIO_REQUEST(33, METHOD_BUFFERED)   /* 0x80942084 */
#define IOCTL_USBIO_GET_PIPE_PARAMETERS        USBIO_REQUEST(35, METHOD_BUFFERED)   /* 0x8094208C */
#define IOCTL_USBIO_SET_PIPE_PARAMETERS        USBIO_REQUEST(36, METHOD_BUFFERED)   /* 0x80942090 */
#define IOCTL_USBIO_SETUP_PIPE_STATISTICS      USBIO_REQUEST(37, METHOD_BUFFERED)   /* 0x80942094 */
#define IOCTL_USBIO_QUERY_PIPE_STATISTICS      USBIO_REQUEST(38, METHOD_BUFFERED)   /* 0x80942098 */
#define IOCTL_USBIO_PIPE_CONTROL_TRANSFER_IN   USBIO_REQUEST(40, METHOD_OUT_DIRECT) /* 0x809420A2 */
#define IOCTL_USBIO_PIPE_CONTROL_TRANSFER_OUT  USBIO_REQUEST(41, METHOD_IN_DIRECT)  /* 0x809420A5 */

/* ---- Flags and limits ------------------------------------------------------------------------ */
#define USBIO_SHORT_TRANSFER_OK         0x00010000u /* a short IN transfer counts as success */
#define USBIO_START_TRANSFER_ASAP       0x00020000u /* isochronous: start at the next frame */
#define USBIO_MAX_INTERFACES            32
#define USBIO_MAX_PIPES                 32
#define USBIO_DEVICE_INFOFLAG_HIGH_SPEED 0x00100000u
/* QUERY_PIPE_STATISTICS: which counters to zero once they have been read */
#define USBIO_QPS_FLAG_RESET_BYTES_TRANSFERRED  0x1u
#define USBIO_QPS_FLAG_RESET_REQUESTS_SUCCEEDED 0x2u
#define USBIO_QPS_FLAG_RESET_REQUESTS_FAILED    0x4u

/* 32-bit values in the layouts below. Pipe types equal the endpoint's bmAttributes & 3;
 * recipients and request types are bmRequestType's fields (bits 4..0 and 6..5). */
typedef ULONG USBIO_PIPE_TYPE;
typedef ULONG USBIO_REQUEST_RECIPIENT;
typedef ULONG USBIO_REQUEST_TYPE;
typedef ULONG USBIO_DEVICE_POWER_STATE;          /* 0 = D0 (on) ... 3 = D3 (off) */
enum { PipeTypeControl = 0, PipeTypeIsochronous = 1, PipeTypeBulk = 2, PipeTypeInterrupt = 3 };
enum { RecipientDevice = 0, RecipientInterface = 1, RecipientEndpoint = 2, RecipientOther = 3 };
enum { RequestTypeClass = 1, RequestTypeVendor = 2 };

/* ---- Layouts --------------------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct {                       /* GET_DRIVER_INFO, out */
    USHORT APIVersion;                 /* BCD major.minor of the interface */
    USHORT DriverVersion;
    ULONG  DriverBuildNumber;
    ULONG  Flags;
} USBIO_DRIVER_INFO;

typedef struct {                       /* GET_BANDWIDTH_INFO, out (kbit/s) */
    ULONG TotalBandwidth;
    ULONG ConsumedBandwidth;
    ULONG reserved1, reserved2;
} USBIO_BANDWIDTH_INFO;

typedef struct {                       /* GET_DEVICE_INFO, out */
    ULONG Flags;                       /* USBIO_DEVICE_INFOFLAG_HIGH_SPEED */
    ULONG OpenCount;
    ULONG reserved1, reserved2;
} USBIO_DEVICE_INFO;

typedef struct {                       /* GET/SET_DESCRIPTOR, in */
    USBIO_REQUEST_RECIPIENT Recipient;
    UCHAR  DescriptorType;             /* high byte of wValue */
    UCHAR  DescriptorIndex;            /* low byte of wValue */
    USHORT LanguageId;                 /* wIndex */
} USBIO_DESCRIPTOR_REQUEST;

typedef struct {                       /* SET/CLEAR_FEATURE, in */
    USBIO_REQUEST_RECIPIENT Recipient;
    USHORT FeatureSelector;            /* wValue */
    USHORT Index;                      /* wIndex */
} USBIO_FEATURE_REQUEST;

typedef struct {                       /* GET_STATUS, in */
    USBIO_REQUEST_RECIPIENT Recipient;
    USHORT Index;
} USBIO_STATUS_REQUEST;
typedef struct { USHORT Status; } USBIO_STATUS_REQUEST_DATA;               /* GET_STATUS, out */
typedef struct { UCHAR ConfigurationValue; } USBIO_GET_CONFIGURATION_DATA; /* 0 = unconfigured */
typedef struct { USHORT Interface; } USBIO_GET_INTERFACE;                  /* GET_INTERFACE, in */
typedef struct { UCHAR AlternateSetting; } USBIO_GET_INTERFACE_DATA;       /* GET_INTERFACE, out */

typedef struct {                       /* one interface in SET_CONFIGURATION; SET_INTERFACE, in */
    USHORT InterfaceIndex;             /* bInterfaceNumber */
    USHORT AlternateSettingIndex;      /* bAlternateSetting */
    ULONG  MaximumTransferSize;        /* largest read/write the app will issue on its pipes */
} USBIO_INTERFACE_SETTING;

typedef struct {                       /* SET_CONFIGURATION, in */
    USHORT ConfigurationIndex;         /* position of the configuration descriptor, from 0 */
    USHORT NbOfInterfaces;
    USBIO_INTERFACE_SETTING InterfaceList[USBIO_MAX_INTERFACES];
} USBIO_SET_CONFIGURATION;

typedef struct {                       /* CLASS_OR_VENDOR_IN/OUT_REQUEST, in */
    ULONG  Flags;                      /* USBIO_SHORT_TRANSFER_OK */
    USBIO_REQUEST_TYPE Type;
    USBIO_REQUEST_RECIPIENT Recipient;
    UCHAR  RequestTypeReservedBits;    /* ORed into bmRequestType */
    UCHAR  Request;                    /* bRequest */
    USHORT Value;                      /* wValue */
    USHORT Index;                      /* wIndex; wLength is the data buffer's size */
} USBIO_CLASS_OR_VENDOR_REQUEST;

typedef struct {                       /* GET/SET_DEVICE_PARAMETERS */
    ULONG Options;
    ULONG RequestTimeout;              /* ms */
} USBIO_DEVICE_PARAMETERS;

typedef struct {                       /* one interface in GET_CONFIGURATION_INFO */
    UCHAR InterfaceNumber;
    UCHAR AlternateSetting;
    UCHAR Class, SubClass, Protocol;
    UCHAR NumberOfPipes;
    UCHAR reserved1, reserved2;
} USBIO_INTERFACE_CONFIGURATION_INFO;

typedef struct {                       /* one pipe in GET_CONFIGURATION_INFO */
    USBIO_PIPE_TYPE PipeType;
    ULONG  MaximumTransferSize;
    USHORT MaximumPacketSize;          /* wMaxPacketSize */
    UCHAR  EndpointAddress;            /* bit 7 set = IN */
    UCHAR  Interval;
    UCHAR  InterfaceNumber;
    UCHAR  reserved1, reserved2, reserved3;
} USBIO_PIPE_CONFIGURATION_INFO;

typedef struct {                       /* GET_CONFIGURATION_INFO, out: the current setting */
    ULONG NbOfInterfaces;
    ULONG NbOfPipes;                   /* endpoint 0 is not listed */
    USBIO_INTERFACE_CONFIGURATION_INFO InterfaceInfo[USBIO_MAX_INTERFACES];
    USBIO_PIPE_CONFIGURATION_INFO      PipeInfo[USBIO_MAX_PIPES];
} USBIO_CONFIGURATION_INFO;

typedef struct { ULONG FrameNumber; } USBIO_FRAME_NUMBER;                  /* 1 ms frames */
typedef struct { USBIO_DEVICE_POWER_STATE DevicePowerState; } USBIO_DEVICE_POWER;
typedef struct { UCHAR EndpointAddress; } USBIO_BIND_PIPE;                 /* BIND_PIPE, in */
typedef struct { ULONG Flags; } USBIO_PIPE_PARAMETERS;                     /* USBIO_SHORT_TRANSFER_OK */
typedef struct { ULONG Flags; } USBIO_QUERY_PIPE_STATISTICS;               /* USBIO_QPS_FLAG_* */

typedef struct {                       /* QUERY_PIPE_STATISTICS, out */
    ULONG ActualAveragingInterval;     /* ms; 0 = no rate measured */
    ULONG AverageRate;                 /* bytes/s */
    ULONG BytesTransferred_L;          /* 64-bit byte count, low half */
    ULONG BytesTransferred_H;          /* and high half */
    ULONG RequestsSucceeded;
    ULONG RequestsFailed;
    ULONG reserved1, reserved2;
} USBIO_PIPE_STATISTICS;

typedef struct {                       /* PIPE_CONTROL_TRANSFER_IN/OUT, in */
    ULONG Flags;
    UCHAR SetupPacket[8];
} USBIO_PIPE_CONTROL_TRANSFER;

/* An isochronous ReadFile/WriteFile buffer begins with this header, followed by one entry per
 * packet; the packet data comes after. Each entry's Offset counts from the start of the whole
 * buffer. On a read the helper fills in each packet's Length and Status, the number of failed
 * packets, and the frame packet 0 used. */
typedef struct {
    ULONG NumberOfPackets;
    ULONG Flags;                       /* USBIO_SHORT_TRANSFER_OK, USBIO_START_TRANSFER_ASAP */
    ULONG StartFrame;
    ULONG ErrorCount;
} USBIO_ISO_TRANSFER;

typedef struct {
    ULONG Offset;
    ULONG Length;
    ULONG Status;                      /* 0 or a USBIO_ERR_ value */
} USBIO_ISO_PACKET;

typedef struct {
    USBIO_ISO_TRANSFER IsoTransfer;
    USBIO_ISO_PACKET   IsoPacket[1];   /* NumberOfPackets entries in the real buffer */
} USBIO_ISO_TRANSFER_HEADER;

#pragma pack(pop)

#define USBIO_CHECK_SIZE(type, n) _Static_assert(sizeof(type) == (n), #type " has the wrong size")
USBIO_CHECK_SIZE(USBIO_DRIVER_INFO, 12);
USBIO_CHECK_SIZE(USBIO_BANDWIDTH_INFO, 16);
USBIO_CHECK_SIZE(USBIO_DEVICE_INFO, 16);
USBIO_CHECK_SIZE(USBIO_DESCRIPTOR_REQUEST, 8);
USBIO_CHECK_SIZE(USBIO_FEATURE_REQUEST, 8);
USBIO_CHECK_SIZE(USBIO_STATUS_REQUEST, 6);
USBIO_CHECK_SIZE(USBIO_STATUS_REQUEST_DATA, 2);
USBIO_CHECK_SIZE(USBIO_GET_CONFIGURATION_DATA, 1);
USBIO_CHECK_SIZE(USBIO_GET_INTERFACE, 2);
USBIO_CHECK_SIZE(USBIO_GET_INTERFACE_DATA, 1);
USBIO_CHECK_SIZE(USBIO_INTERFACE_SETTING, 8);
USBIO_CHECK_SIZE(USBIO_SET_CONFIGURATION, 260);
USBIO_CHECK_SIZE(USBIO_CLASS_OR_VENDOR_REQUEST, 18);
USBIO_CHECK_SIZE(USBIO_DEVICE_PARAMETERS, 8);
USBIO_CHECK_SIZE(USBIO_INTERFACE_CONFIGURATION_INFO, 8);
USBIO_CHECK_SIZE(USBIO_PIPE_CONFIGURATION_INFO, 16);
USBIO_CHECK_SIZE(USBIO_CONFIGURATION_INFO, 776);
USBIO_CHECK_SIZE(USBIO_FRAME_NUMBER, 4);
USBIO_CHECK_SIZE(USBIO_DEVICE_POWER, 4);
USBIO_CHECK_SIZE(USBIO_BIND_PIPE, 1);
USBIO_CHECK_SIZE(USBIO_PIPE_PARAMETERS, 4);
USBIO_CHECK_SIZE(USBIO_QUERY_PIPE_STATISTICS, 4);
USBIO_CHECK_SIZE(USBIO_PIPE_STATISTICS, 32);
USBIO_CHECK_SIZE(USBIO_PIPE_CONTROL_TRANSFER, 12);
USBIO_CHECK_SIZE(USBIO_ISO_TRANSFER, 16);
USBIO_CHECK_SIZE(USBIO_ISO_PACKET, 12);
USBIO_CHECK_SIZE(USBIO_ISO_TRANSFER_HEADER, 28);

/* ---- Status values ---------------------------------------------------------------------------
 * A failed request completes with one of these; the application reads it from GetLastError().
 * They carry the 0x20000000 "customer code" bit, which Windows and Wine pass through to
 * GetLastError() unchanged. Only the values usbio-helper returns or the test client checks. */
#define USBIO_ERR_SUCCESS                          0x00000000u
/* transfer-level errors */
#define USBIO_ERR_STALL_PID                        0xE0000004u
#define USBIO_ERR_DATA_OVERRUN                     0xE0000008u
#define USBIO_ERR_XACT_ERROR                       0xE0000011u
#define USBIO_ERR_ENDPOINT_HALTED                  0xE0000030u
#define USBIO_ERR_INVALID_PARAMETER                0xE0000300u
#define USBIO_ERR_ERROR_BUSY                       0xE0000400u
#define USBIO_ERR_REQUEST_FAILED                   0xE0000500u
#define USBIO_ERR_ERROR_SHORT_TRANSFER             0xE0000900u
#define USBIO_ERR_ISOCH_REQUEST_FAILED             0xE0000B00u
#define USBIO_ERR_INVALID_CONFIGURATION_DESCRIPTOR 0xE0000F00u
#define USBIO_ERR_SET_CONFIG_FAILED                0xE0002000u
#define USBIO_ERR_DEVICE_GONE                      0xE0007000u
#define USBIO_ERR_CANCELED                         0xE0010000u
/* request-level errors */
#define USBIO_ERR_INVALID_INBUFFER                 0xE0001001u
#define USBIO_ERR_INVALID_OUTBUFFER                0xE0001002u
#define USBIO_ERR_NOT_CONFIGURED                   0xE0001006u
#define USBIO_ERR_ALREADY_BOUND                    0xE0001008u
#define USBIO_ERR_NOT_BOUND                        0xE0001009u
#define USBIO_ERR_CONTROL_NOT_SUPPORTED            0xE000100Bu
#define USBIO_ERR_TIMEOUT                          0xE000100Cu
#define USBIO_ERR_INVALID_IOCTL                    0xE000100Fu
#define USBIO_ERR_INVALID_DIRECTION                0xE0001010u
#define USBIO_ERR_TOO_MUCH_ISO_PACKETS             0xE0001011u
#define USBIO_ERR_PIPE_NOT_FOUND                   0xE0001013u
#define USBIO_ERR_INVALID_ISO_PACKET               0xE0001014u
#define USBIO_ERR_INTERFACE_NOT_FOUND              0xE0001016u
#define USBIO_ERR_INVALID_PARAM                    0xE0001018u
#define USBIO_ERR_SET_CONFIGURATION_FAILED         0xE000101Du

#endif /* USBIO_DEFS_H */
