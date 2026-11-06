/*
 * usbcp.c — raw USB flash disk sector read/write tool (Windows 10/11)
 *
 * Mechanism (USBDK "redirect"):
 *   1. usbcp talks to UsbDkHelper.dll (user-mode API of USBDK driver).
 *   2. UsbDk_StartRedirect() detaches ("hijacks") the USB mass-storage
 *      device from the Windows storage stack (usbstor.sys): the disk
 *      disappears from Explorer for the duration of the command.
 *   3. usbcp then speaks SCSI directly to the stick over the bulk
 *      endpoints using the USB Bulk-Only Transport (BBB: CBW/data/CSW)
 *      and issues READ(10)/WRITE(10) commands on absolute LBA.
 *   4. UsbDk_StopRedirect() returns the device to Windows (re-mounted).
 *
 * Commands:
 *   usbcp list
 *   usbcp info    [-d <index>]
 *   usbcp write <file> <start_sector> [-d <index>] [-y]
 *   usbcp read  <start_sector> <count> [outfile] [-d <index>]
 *
 * Requirements:
 *   - USBDK >= 1.0.22 installed: https://github.com/daynix/UsbDk/releases
 *   - Run from an elevated (Administrator) console.
 *
 * Build:
 *   MSVC : cl /nologo /W4 /O2 usbcp.c /Fe:usbcp.exe shell32.lib
 *   MinGW: gcc -Wall -O2 -o usbcp.exe usbcp.c -lshell32 -static
 *
 * USBDK API declarations below are copied from UsbDkHelper.h / UsbDkData.h
 * (Copyright 2013-2014 Red Hat, Inc. / Daynix Computing LTD, Apache-2.0),
 * so no USBDK SDK installation is needed to build usbcp.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <usb100.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <malloc.h>
#include <wchar.h>

#ifndef MAX_DEVICE_ID_LEN
#define MAX_DEVICE_ID_LEN 200
#endif

/* ------------------------------------------------------------------ */
/*  USBDK user-mode API declarations (v1.0.22+)                        */
/* ------------------------------------------------------------------ */

typedef struct tag_USB_DK_DEVICE_ID
{
    WCHAR DeviceID[MAX_DEVICE_ID_LEN];
    WCHAR InstanceID[MAX_DEVICE_ID_LEN];
} USB_DK_DEVICE_ID, *PUSB_DK_DEVICE_ID;

typedef struct tag_USB_DK_DEVICE_INFO
{
    USB_DK_DEVICE_ID ID;
    ULONG64 FilterID;
    ULONG64 Port;
    ULONG64 Speed;                          /* 0 none,1 low,2 full,3 high,4 super */
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
} USB_DK_DEVICE_INFO, *PUSB_DK_DEVICE_INFO;

typedef struct tag_USB_DK_CONFIG_DESCRIPTOR_REQUEST
{
    USB_DK_DEVICE_ID ID;
    ULONG64 Index;                          /* configuration index */
} USB_DK_CONFIG_DESCRIPTOR_REQUEST, *PUSB_DK_CONFIG_DESCRIPTOR_REQUEST;

typedef struct tag_USB_DK_GEN_TRANSFER_RESULT
{
    ULONG64 BytesTransferred;
    ULONG64 UsbdStatus;
} USB_DK_GEN_TRANSFER_RESULT, *PUSB_DK_GEN_TRANSFER_RESULT;

typedef struct tag_USB_DK_TRANSFER_RESULT
{
    USB_DK_GEN_TRANSFER_RESULT GenResult;
    PVOID64 IsochronousResultsArray;
} USB_DK_TRANSFER_RESULT, *PUSB_DK_TRANSFER_RESULT;

typedef struct tag_USB_DK_TRANSFER_REQUEST
{
    ULONG64 EndpointAddress;
    PVOID64 Buffer;
    ULONG64 BufferLength;
    ULONG64 TransferType;                   /* 0 ctl, 1 bulk, 2 intr, 3 iso */
    ULONG64 IsochronousPacketsArraySize;
    PVOID64 IsochronousPacketsArray;
    USB_DK_TRANSFER_RESULT Result;
} USB_DK_TRANSFER_REQUEST, *PUSB_DK_TRANSFER_REQUEST;

typedef enum
{
    TransferFailure = 0,
    TransferSuccess,
    TransferSuccessAsync
} TransferResult;

typedef enum
{
    ControlTransferType = 0,
    BulkTransferType,
    InterruptTransferType,
    IsochronousTransferType
} USB_DK_TRANSFER_TYPE;

/* function pointer types */
typedef BOOL         (*PFN_GetDevicesList)(PUSB_DK_DEVICE_INFO *, PULONG);
typedef void         (*PFN_ReleaseDevicesList)(PUSB_DK_DEVICE_INFO);
typedef BOOL         (*PFN_GetConfigDescriptor)(USB_DK_CONFIG_DESCRIPTOR_REQUEST,
                                                PUSB_CONFIGURATION_DESCRIPTOR *, PULONG);
typedef void         (*PFN_ReleaseConfigDescriptor)(PUSB_CONFIGURATION_DESCRIPTOR);
typedef HANDLE       (*PFN_StartRedirect)(PUSB_DK_DEVICE_ID);
typedef BOOL         (*PFN_StopRedirect)(HANDLE);
typedef TransferResult (*PFN_ReadPipe)(HANDLE, PUSB_DK_TRANSFER_REQUEST, LPOVERLAPPED);
typedef TransferResult (*PFN_WritePipe)(HANDLE, PUSB_DK_TRANSFER_REQUEST, LPOVERLAPPED);
typedef BOOL         (*PFN_ResetPipe)(HANDLE, ULONG64);
typedef BOOL         (*PFN_ResetDevice)(HANDLE);
typedef HANDLE       (*PFN_GetRedirectorSystemHandle)(HANDLE);

static struct
{
    HMODULE mod;
    PFN_GetDevicesList         GetDevicesList;
    PFN_ReleaseDevicesList     ReleaseDevicesList;
    PFN_GetConfigDescriptor    GetConfigurationDescriptor;
    PFN_ReleaseConfigDescriptor ReleaseConfigurationDescriptor;
    PFN_StartRedirect          StartRedirect;
    PFN_StopRedirect           StopRedirect;
    PFN_ReadPipe               ReadPipe;
    PFN_WritePipe              WritePipe;
    PFN_ResetPipe              ResetPipe;
    PFN_ResetDevice            ResetDevice;
    PFN_GetRedirectorSystemHandle GetRedirectorSystemHandle;
} U;

static int load_usbdk(void)
{
    const char *names[] = { "UsbDkHelper.dll", "UsbDkAPI.dll" };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        U.mod = LoadLibraryA(names[i]);
        if (U.mod)
            break;
    }
    if (!U.mod)
    {
        fprintf(stderr,
            "error: USBDK runtime DLL not found.\n"
            "       Install USBDK (>= 1.0.22) from https://github.com/daynix/UsbDk/releases\n"
            "       (e.g. UsbDk_1.0.22_x64.msi), then run usbcp again.\n");
        return -1;
    }

    U.GetDevicesList          = (PFN_GetDevicesList)(void *)GetProcAddress(U.mod, "UsbDk_GetDevicesList");
    U.ReleaseDevicesList      = (PFN_ReleaseDevicesList)(void *)GetProcAddress(U.mod, "UsbDk_ReleaseDevicesList");
    U.GetConfigurationDescriptor = (PFN_GetConfigDescriptor)(void *)GetProcAddress(U.mod, "UsbDk_GetConfigurationDescriptor");
    U.ReleaseConfigurationDescriptor = (PFN_ReleaseConfigDescriptor)(void *)GetProcAddress(U.mod, "UsbDk_ReleaseConfigurationDescriptor");
    U.StartRedirect           = (PFN_StartRedirect)(void *)GetProcAddress(U.mod, "UsbDk_StartRedirect");
    U.StopRedirect            = (PFN_StopRedirect)(void *)GetProcAddress(U.mod, "UsbDk_StopRedirect");
    U.ReadPipe                = (PFN_ReadPipe)(void *)GetProcAddress(U.mod, "UsbDk_ReadPipe");
    U.WritePipe               = (PFN_WritePipe)(void *)GetProcAddress(U.mod, "UsbDk_WritePipe");
    U.ResetPipe               = (PFN_ResetPipe)(void *)GetProcAddress(U.mod, "UsbDk_ResetPipe");
    U.ResetDevice             = (PFN_ResetDevice)(void *)GetProcAddress(U.mod, "UsbDk_ResetDevice");
    U.GetRedirectorSystemHandle = (PFN_GetRedirectorSystemHandle)(void *)GetProcAddress(U.mod, "UsbDk_GetRedirectorSystemHandle");

    if (!U.GetDevicesList || !U.ReleaseDevicesList || !U.GetConfigurationDescriptor ||
        !U.ReleaseConfigurationDescriptor || !U.StartRedirect || !U.StopRedirect ||
        !U.ReadPipe || !U.WritePipe || !U.ResetPipe || !U.ResetDevice ||
        !U.GetRedirectorSystemHandle)
    {
        fprintf(stderr,
            "error: %s found but it does not export the expected API.\n"
            "       Installed USBDK version is too old; please upgrade to >= 1.0.22.\n",
            names[i]);
        FreeLibrary(U.mod);
        U.mod = NULL;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int is_elevated(void)
{
    HANDLE tok;
    DWORD elev = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        DWORD ret = 0;
        if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &ret))
            elev = elev ? 1 : 0;   /* TOKEN_ELEVATION.TokenIsElevated */
        CloseHandle(tok);
    }
    return elev != 0;
}

static const char *speed_name(ULONG64 s)
{
    switch (s)
    {
    case 1: return "LowSpeed";
    case 2: return "FullSpeed";
    case 3: return "HighSpeed (USB 2.0)";
    case 4: return "SuperSpeed (USB 3.x)";
    default: return "Unknown";
    }
}

static const char *size_h(uint64_t bytes)
{
    static char buf[64];
    if (bytes >= 1024ULL * 1024 * 1024 * 1024)
        sprintf(buf, "%.2f TiB", bytes / (1024.0 * 1024 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024 * 1024)
        sprintf(buf, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        sprintf(buf, "%.2f MiB", bytes / (1024.0 * 1024));
    else
        sprintf(buf, "%llu bytes", (unsigned long long)bytes);
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Mass-storage endpoints extracted from configuration descriptor     */
/* ------------------------------------------------------------------ */

typedef struct
{
    int     found;              /* BOT mass-storage interface present */
    UCHAR   iface_number;
    UCHAR   subclass;
    UCHAR   ep_in;              /* bulk-in  endpoint address (0x8x) */
    UCHAR   ep_out;             /* bulk-out endpoint address (0x0x) */
    USHORT  maxpkt_in;
    USHORT  maxpkt_out;
} MSD_EPS;

static void find_ms_eps(const UCHAR *desc, ULONG total, MSD_EPS *eps)
{
    const UCHAR *p = desc;
    const UCHAR *end = desc + total;
    int cur_iface_is_ms = 0;

    memset(eps, 0, sizeof(*eps));

    while (p + 2 <= end)
    {
        UCHAR blen = p[0];
        UCHAR btype = p[1];
        if (blen < 2 || p + blen > end)
            break;

        if (btype == USB_INTERFACE_DESCRIPTOR_TYPE && blen >= 9)        /* 0x04 */
        {
            const UCHAR *ifc = p;
            cur_iface_is_ms = (ifc[5] == 0x08 /* class: mass storage */
                            && ifc[7] == 0x50 /* protocol: Bulk-Only */);
            if (cur_iface_is_ms && !eps->found)
            {
                eps->found = 1;
                eps->iface_number = ifc[2];
                eps->subclass = ifc[6];
            }
        }
        else if (btype == USB_ENDPOINT_DESCRIPTOR_TYPE && blen >= 7 && cur_iface_is_ms)  /* 0x05 */
        {
            if ((p[3] & 0x03) == 0x02)     /* bulk endpoint */
            {
                UCHAR addr = p[2];
                USHORT maxpkt = (USHORT)(p[4] | (p[5] << 8));
                if (addr & 0x80)
                {
                    if (!eps->ep_in)  { eps->ep_in = addr;  eps->maxpkt_in = maxpkt; }
                }
                else
                {
                    if (!eps->ep_out) { eps->ep_out = addr; eps->maxpkt_out = maxpkt; }
                }
            }
        }
        p += blen;
    }

    if (eps->found && (!eps->ep_in || !eps->ep_out))
        eps->found = 0;   /* incomplete BOT interface */
}

static int get_ms_eps(PUSB_DK_DEVICE_INFO dev, MSD_EPS *eps)
{
    USB_DK_CONFIG_DESCRIPTOR_REQUEST req;
    PUSB_CONFIGURATION_DESCRIPTOR cfg = NULL;
    ULONG len = 0;
    BOOL ok;

    memset(&req, 0, sizeof(req));
    req.ID = dev->ID;
    req.Index = 0;

    ok = U.GetConfigurationDescriptor(req, &cfg, &len);
    if (!ok || !cfg || len < 9)
        return -1;

    find_ms_eps((const UCHAR *)cfg, len, eps);
    U.ReleaseConfigurationDescriptor(cfg);
    return eps->found ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Device enumeration / selection                                     */
/* ------------------------------------------------------------------ */

static int enum_devices(PUSB_DK_DEVICE_INFO *list, ULONG *count)
{
    if (!U.GetDevicesList(list, count))
    {
        fprintf(stderr, "error: UsbDk_GetDevicesList failed (err=%lu).\n"
                        "       Is the USBDK service running? Is this console elevated?\n",
                GetLastError());
        return -1;
    }
    return 0;
}

static void print_device_line(ULONG idx, PUSB_DK_DEVICE_INFO d, int ms)
{
    printf("  [%lu] USB\\VID_%04X&PID_%04X  %-22s  %ls%s\n",
           idx,
           d->DeviceDescriptor.idVendor,
           d->DeviceDescriptor.idProduct,
           speed_name(d->Speed),
           d->ID.DeviceID,
           ms ? "   <== MASS STORAGE (usable by usbcp)" : "");
}

static int select_device(int want_idx,
                         PUSB_DK_DEVICE_INFO devs, ULONG ndevs,
                         ULONG *out_idx, MSD_EPS *out_eps)
{
    ULONG i;
    ULONG candidate = (ULONG)-1;
    int n_ms = 0;

    for (i = 0; i < ndevs; i++)
    {
        MSD_EPS eps;
        if (devs[i].DeviceDescriptor.bDeviceClass == 0x09)   /* hubs are never listed */
            continue;
        if (get_ms_eps(&devs[i], &eps) == 0)
        {
            n_ms++;
            if (candidate == (ULONG)-1)
                candidate = i;
        }
    }

    if (want_idx >= 0)
    {
        if ((ULONG)want_idx >= ndevs)
        {
            fprintf(stderr, "error: device index %d out of range (0..%lu). Run 'usbcp list'.\n",
                    want_idx, ndevs ? ndevs - 1 : 0);
            return -1;
        }
        if (get_ms_eps(&devs[want_idx], out_eps) != 0)
        {
            fprintf(stderr, "error: device [%d] is not a USB mass-storage device.\n", want_idx);
            return -1;
        }
        *out_idx = (ULONG)want_idx;
        return 0;
    }

    if (n_ms == 0)
    {
        fprintf(stderr, "error: no USB mass-storage (BOT) device found.\n");
        return -1;
    }
    if (n_ms > 1)
    {
        fprintf(stderr, "error: multiple mass-storage devices found, select one with -d:\n");
        for (i = 0; i < ndevs; i++)
        {
            MSD_EPS eps;
            if (get_ms_eps(&devs[i], &eps) == 0)
                print_device_line(i, &devs[i], 1);
        }
        return -1;
    }

    if (get_ms_eps(&devs[candidate], out_eps) != 0)
        return -1;
    *out_idx = candidate;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SCSI over Bulk-Only Transport                                      */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct _BOT_CBW
{
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;      /* 0x80 = data-IN */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} BOT_CBW;                    /* 31 bytes */

typedef struct _BOT_CSW
{
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} BOT_CSW;                    /* 13 bytes */
#pragma pack(pop)

#define BOT_CBW_SIG 0x43425355UL   /* 'USBC' */
#define BOT_CSW_SIG 0x53425355UL   /* 'USBS' */

typedef struct
{
    HANDLE   h;               /* USBDK opaque redirect handle (StartRedirect) */
    HANDLE   hsys;            /* system handle, needed by GetOverlappedResult */
    HANDLE   ev;              /* manual-reset event reused for every transfer */
    UCHAR    ep_in;
    UCHAR    ep_out;
    uint32_t tag;
    unsigned sector_size;
    uint64_t sector_count;
} MSD;

/*
 * The redirector file handle is created by the driver as ASYNCHRONOUS
 * (no FO_SYNCHRONOUS_IO), therefore ReadPipe/WritePipe must be given a
 * real OVERLAPPED structure; we wait with GetOverlappedResult on the
 * system handle obtained from UsbDk_GetRedirectorSystemHandle().
 */
static int usb_bulk(MSD *d, int dir_in, void *buf, ULONG len)
{
    USB_DK_TRANSFER_REQUEST r;
    OVERLAPPED ov;
    TransferResult res;
    int ok = 0;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = d->ev;
    ResetEvent(d->ev);

    memset(&r, 0, sizeof(r));
    r.EndpointAddress = dir_in ? d->ep_in : d->ep_out;
    r.Buffer = (PVOID64)(ULONG_PTR)buf;
    r.BufferLength = len;
    r.TransferType = BulkTransferType;

    res = dir_in ? U.ReadPipe(d->h, &r, &ov) : U.WritePipe(d->h, &r, &ov);

    if (res == TransferSuccess)
    {
        ok = 1;                                     /* completed synchronously */
    }
    else if (res == TransferSuccessAsync)
    {
        DWORD dummy = 0;
        ok = GetOverlappedResult(d->hsys, &ov, &dummy, TRUE) ? 1 : 0;
    }

    if (!ok)
        return -1;
    /* USBD_STATUS_SUCCESS == 0; nonzero e.g. 0xC0000004 = STALL */
    if (r.Result.GenResult.UsbdStatus != 0)
        return -1;
    if (r.Result.GenResult.BytesTransferred != (ULONG64)len)
        return -1;
    return 0;
}

static int bot_transfer(MSD *d, const UCHAR *cdb, UCHAR cdb_len,
                        int data_in, void *data, ULONG data_len, int quiet)
{
    BOT_CBW cbw;
    BOT_CSW csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = BOT_CBW_SIG;
    cbw.dCBWTag = ++d->tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = data_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    if (usb_bulk(d, 0, &cbw, sizeof(cbw)) != 0)
    {
        if (!quiet) fprintf(stderr, "\nerror: failed to send CBW (endpoint 0x%02X).\n", d->ep_out);
        return -1;
    }

    if (data_len)
    {
        if (usb_bulk(d, data_in, data, data_len) != 0)
        {
            if (!quiet) fprintf(stderr, "\nerror: bulk %s transfer failed (%lu bytes, ep 0x%02X).\n",
                                data_in ? "IN" : "OUT", data_len,
                                data_in ? d->ep_in : d->ep_out);
            return -1;
        }
    }

    if (usb_bulk(d, 1, &csw, sizeof(csw)) != 0)
    {
        /* endpoint may be stalled: reset pipes and retry the CSW read once */
        U.ResetPipe(d->h, d->ep_in);
        U.ResetPipe(d->h, d->ep_out);
        if (usb_bulk(d, 1, &csw, sizeof(csw)) != 0)
        {
            if (!quiet) fprintf(stderr, "\nerror: failed to read CSW.\n");
            return -1;
        }
    }

    if (csw.dCSWSignature != BOT_CSW_SIG || csw.dCSWTag != cbw.dCBWTag)
    {
        if (!quiet) fprintf(stderr, "\nerror: invalid CSW received.\n");
        return -1;
    }

    if (csw.bCSWStatus != 0)
    {
        if (!quiet)
        {
            UCHAR sense_cdb[6] = { 0 };
            UCHAR sense[18];
            sense_cdb[0] = 0x03;              /* REQUEST SENSE */
            sense_cdb[4] = sizeof(sense);
            if (bot_transfer(d, sense_cdb, 6, 1, sense, sizeof(sense), 1) == 0)
                fprintf(stderr, "\nerror: SCSI status %u (sense key 0x%02X, ASC 0x%02X, ASCQ 0x%02X).\n",
                        csw.bCSWStatus, sense[2] & 0x0F, sense[12], sense[13]);
            else
                fprintf(stderr, "\nerror: SCSI status %u (no sense data).\n", csw.bCSWStatus);
        }
        return -1;
    }
    return 0;
}

/* ---- SCSI CDB builders ---- */

static int msd_read10(MSD *d, uint64_t lba, uint16_t nblocks, void *buf)
{
    UCHAR cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;                                   /* READ(10) */
    cdb[2] = (UCHAR)(lba >> 24); cdb[3] = (UCHAR)(lba >> 16);
    cdb[4] = (UCHAR)(lba >> 8);  cdb[5] = (UCHAR)(lba);
    cdb[7] = (UCHAR)(nblocks >> 8); cdb[8] = (UCHAR)nblocks;
    return bot_transfer(d, cdb, 10, 1, buf, (ULONG)nblocks * d->sector_size, 0);
}

static int msd_write10(MSD *d, uint64_t lba, uint16_t nblocks, void *buf)
{
    UCHAR cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A;                                   /* WRITE(10) */
    cdb[2] = (UCHAR)(lba >> 24); cdb[3] = (UCHAR)(lba >> 16);
    cdb[4] = (UCHAR)(lba >> 8);  cdb[5] = (UCHAR)(lba);
    cdb[7] = (UCHAR)(nblocks >> 8); cdb[8] = (UCHAR)nblocks;
    return bot_transfer(d, cdb, 10, 0, buf, (ULONG)nblocks * d->sector_size, 0);
}

static int msd_test_unit_ready(MSD *d)
{
    UCHAR cdb[6];
    memset(cdb, 0, sizeof(cdb));                     /* opcode 0x00 */
    return bot_transfer(d, cdb, 6, 0, NULL, 0, 1);
}

static int msd_inquiry(MSD *d, char vendor[9], char product[17], char rev[5])
{
    UCHAR cdb[6];
    UCHAR resp[36];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;                                   /* INQUIRY */
    cdb[4] = sizeof(resp);
    if (bot_transfer(d, cdb, 6, 1, resp, sizeof(resp), 0) != 0)
        return -1;
    memcpy(vendor,  resp + 8, 8);  vendor[8] = 0;
    memcpy(product, resp + 16, 16); product[16] = 0;
    memcpy(rev,     resp + 32, 4); rev[4] = 0;
    return 0;
}

static int msd_read_capacity(MSD *d, uint64_t *sector_count, unsigned *sector_size)
{
    UCHAR cdb[10];
    UCHAR resp[8];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;                                   /* READ CAPACITY(10) */
    if (bot_transfer(d, cdb, 10, 1, resp, sizeof(resp), 0) != 0)
        return -1;
    uint64_t last_lba = ((uint64_t)resp[0] << 24) | ((uint64_t)resp[1] << 16)
                      | ((uint64_t)resp[2] << 8) | resp[3];
    *sector_size = ((unsigned)resp[4] << 24) | ((unsigned)resp[5] << 16)
                 | ((unsigned)resp[6] << 8) | resp[7];
    if (*sector_size == 0)
        *sector_size = 512;
    if (last_lba == 0xFFFFFFFF)
        *sector_count = 0x100000000ULL;              /* >2TiB: needs READ16, report lower bound */
    else
        *sector_count = last_lba + 1;
    return 0;
}

/* ---- partition table parsing (MBR primary + EBR chain + GPT) ---- */

#define MAX_PARTS 136

typedef struct
{
    int            num;         /* 1-based; 1..4 primary, 5.. logical (MBR) */
    char           kind[12];    /* "Primary" / "Logical" / "GPT" */
    unsigned char  mbr_type;    /* 0 for GPT entries */
    unsigned char  guid[16];    /* GPT partition type GUID (raw bytes) */
    uint64_t       start_lba;
    uint64_t       sectors;
} PART;

static uint32_t le32b(const UCHAR *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64b(const UCHAR *p)
{
    return (uint64_t)le32b(p) | ((uint64_t)le32b(p + 4) << 32);
}

static const char *mbr_type_name(unsigned char t)
{
    switch (t)
    {
    case 0x01: return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0E: return "FAT16";
    case 0x05:
    case 0x0F: return "Extended";
    case 0x07: return "NTFS/exFAT";
    case 0x0B:
    case 0x0C: return "FAT32";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    default:   return "Unknown";
    }
}

static const char *gpt_type_name(const UCHAR *guid)
{
    static const struct { const char *bytes; const char *name; } tab[] = {
        /* GUIDs stored as raw little/big-endian bytes on disk */
        { "\x28\x73\x2a\xc1\x1f\xf8\xd2\x11\xba\x4b\x00\xa0\xc9\x3e\xc9\x3b", "EFI System" },
        { "\xa2\xa0\xd0\xeb\xb9\xe5\x44\x33\x87\xc0\x68\xb6\xb7\x26\x99\xc7", "Microsoft basic data" },
        { "\xaf\x3d\xc6\x0f\x83\x84\x47\x72\x8e\x79\x3d\x69\xd8\x47\x7d\xe4", "Linux filesystem" },
        { "\xe6\xd6\xd3\x79\xf5\x07\x44\xc2\xa2\x3c\x23\x8f\x2a\x3d\xf9\x28", "Linux LVM" },
        { "\xa4\xbb\x94\xde\xd1\x06\x40\x4d\xa1\x6a\xbf\xd5\x01\x79\xd6\xac", "Windows recovery" },
    };
    size_t i;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (memcmp(guid, tab[i].bytes, 16) == 0)
            return tab[i].name;
    return "GPT";
}

/*
 * Reads and parses the on-disk partition table through SCSI READ(10).
 * Returns number of partitions found (0 = unpartitioned/blank),
 * or -1 on transfer failure.
 * Numbering matches Windows convention: 1-4 primary, 5+ logical.
 */
static int parse_partitions(MSD *d, PART *parts)
{
    UCHAR mbr[512];
    int n = 0, i;

    memset(parts, 0, sizeof(PART) * MAX_PARTS);

    if (msd_read10(d, 0, 1, mbr) != 0)
        return -1;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;                                    /* blank / no table */

    /* GPT protective MBR -> parse GUID table */
    if (mbr[0x1C2] == 0xEE)
    {
        UCHAR hdr[512];
        uint64_t entry_lba;
        uint32_t nent, esize;
        UCHAR *tbl;

        if (msd_read10(d, 1, 1, hdr) != 0)
            return 0;
        if (memcmp(hdr, "EFI PART", 8) != 0)
            return 0;
        entry_lba = le64b(hdr + 72);
        nent = le32b(hdr + 80);
        esize = le32b(hdr + 84);
        if (esize < 128 || esize > 512)
            esize = 128;
        if (nent > 128)
            nent = 128;

        tbl = (UCHAR *)_aligned_malloc(32 * 512, 4096);
        if (!tbl)
            return -1;
        if (msd_read10(d, entry_lba, 32, tbl) != 0)
        {
            _aligned_free(tbl);
            return -1;
        }
        for (i = 0; i < (int)nent && n < MAX_PARTS; i++)
        {
            const UCHAR *e = tbl + (size_t)i * esize;
            uint64_t first, last;
            int z = 1, j;
            for (j = 0; j < 16; j++)
                if (e[j]) { z = 0; break; }
            if (z)
                continue;                            /* unused entry */
            first = le64b(e + 32);
            last = le64b(e + 40);
            if (last < first)
                continue;
            parts[n].num = n + 1;
            strcpy(parts[n].kind, "GPT");
            parts[n].mbr_type = 0;
            memcpy(parts[n].guid, e, 16);
            parts[n].start_lba = first;
            parts[n].sectors = last - first + 1;
            n++;
        }
        _aligned_free(tbl);
        return n;
    }

    /* plain MBR: primary partitions first */
    {
        uint64_t ext_base = 0;
        int has_ext = 0;

        for (i = 0; i < 4; i++)
        {
            const UCHAR *e = mbr + 0x1BE + i * 16;
            unsigned char t = e[4];
            if (t == 0)
                continue;
            if (t == 0x05 || t == 0x0F)
            {
                has_ext = 1;
                ext_base = le32b(e + 8);
                continue;
            }
            if (n >= MAX_PARTS)
                break;
            parts[n].num = n + 1;
            strcpy(parts[n].kind, "Primary");
            parts[n].mbr_type = t;
            parts[n].start_lba = le32b(e + 8);
            parts[n].sectors = le32b(e + 12);
            n++;
        }

        /* walk EBR chain for logical partitions (5, 6, 7, ...) */
        if (has_ext)
        {
            uint64_t ebr = ext_base;
            int guard = 0, logical_no = 5;

            while (guard++ < 128 && n < MAX_PARTS)
            {
                UCHAR ebrbuf[512];
                const UCHAR *e1, *e2;

                if (msd_read10(d, ebr, 1, ebrbuf) != 0)
                    break;
                if (ebrbuf[510] != 0x55 || ebrbuf[511] != 0xAA)
                    break;

                e1 = ebrbuf + 0x1BE;
                e2 = ebrbuf + 0x1CE;

                if (e1[4] != 0x00 && e1[4] != 0x05)
                {
                    parts[n].num = logical_no++;
                    strcpy(parts[n].kind, "Logical");
                    parts[n].mbr_type = e1[4];
                    parts[n].start_lba = ebr + le32b(e1 + 8);
                    parts[n].sectors = le32b(e1 + 12);
                    n++;
                }
                if (e2[4] == 0x05)
                {
                    uint64_t next = ext_base + le32b(e2 + 8);
                    if (next <= ebr)
                        break;                        /* corrupt chain, avoid loop */
                    ebr = next;
                    continue;
                }
                break;
            }
        }
    }
    return n;
}

static void print_partitions(const PART *parts, int n)
{
    int i;
    if (n <= 0)
    {
        printf("  partitions     : none (blank disk or no known table)\n");
        return;
    }
    printf("  partitions     : %d\n", n);
    for (i = 0; i < n; i++)
    {
        printf("    [%d] %-8s ", parts[i].num, parts[i].kind);
        if (parts[i].mbr_type)
            printf("type 0x%02X (%-20s)", parts[i].mbr_type, mbr_type_name(parts[i].mbr_type));
        else
            printf("%-27s", gpt_type_name(parts[i].guid));
        printf(" start LBA %-10llu %10llu sectors (%s)\n",
               (unsigned long long)parts[i].start_lba,
               (unsigned long long)parts[i].sectors,
               size_h(parts[i].sectors * (uint64_t)512));
    }
}

/* Resolve -p <part>: returns 0 and fills base/limit on success. */
static int resolve_part(MSD *d, int part_idx,
                        uint64_t *base_lba, uint64_t *limit_sectors)
{
    PART parts[MAX_PARTS];
    int np;

    np = parse_partitions(d, parts);
    if (np < 0)
    {
        fprintf(stderr, "error: failed to read the partition table from disk.\n");
        return -1;
    }
    if (np == 0)
    {
        fprintf(stderr, "error: disk has no partition table; use absolute LBA (without -p).\n");
        return -1;
    }
    if (part_idx > np)
    {
        fprintf(stderr, "error: partition %d does not exist (%d found). Run 'usbcp info' to list them.\n",
                part_idx, np);
        return -1;
    }

    *base_lba = parts[part_idx - 1].start_lba;
    *limit_sectors = parts[part_idx - 1].sectors;
    printf("Partition %d: %s, start LBA %llu, %llu sectors.\n",
           parts[part_idx - 1].num, parts[part_idx - 1].kind,
           (unsigned long long)*base_lba, (unsigned long long)*limit_sectors);
    return 0;
}

/* ---- attach / detach the hijacked device ---- */

static void msd_detach(MSD *d);

static int msd_attach(MSD *d, PUSB_DK_DEVICE_INFO dev, MSD_EPS *eps)
{
    int i;

    memset(d, 0, sizeof(*d));
    d->sector_size = 512;

    d->h = U.StartRedirect(&dev->ID);
    if (!d->h || d->h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "error: UsbDk_StartRedirect failed (err=%lu).%s\n",
                GetLastError(),
                is_elevated() ? "" : "\n       usbcp must run as Administrator — try an elevated console.");
        return -1;
    }

    d->hsys = U.GetRedirectorSystemHandle(d->h);
    if (!d->hsys || d->hsys == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "error: cannot obtain redirector system handle.\n");
        U.StopRedirect(d->h);
        d->h = NULL;
        return -1;
    }

    d->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->ev)
    {
        fprintf(stderr, "error: CreateEvent failed.\n");
        U.StopRedirect(d->h);
        d->h = NULL;
        return -1;
    }

    d->ep_in = eps->ep_in;
    d->ep_out = eps->ep_out;

    /* some sticks need a few moments to become ready after being re-attached */
    for (i = 0; i < 8; i++)
    {
        if (msd_test_unit_ready(d) == 0)
            break;
        Sleep(150);
    }
    if (i == 8)
    {
        fprintf(stderr, "error: device does not become ready (TEST UNIT READY failed).\n");
        msd_detach(d);
        return -1;
    }

    msd_read_capacity(d, &d->sector_count, &d->sector_size);
    return 0;
}

static void msd_detach(MSD *d)
{
    if (d->ev)
    {
        CloseHandle(d->ev);
        d->ev = NULL;
    }
    if (d->h && d->h != INVALID_HANDLE_VALUE)
    {
        U.StopRedirect(d->h);
        d->h = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Progress helper                                                    */
/* ------------------------------------------------------------------ */

static void progress(uint64_t done, uint64_t total, uint64_t sectors_done, unsigned ssize)
{
    int pct = total ? (int)(done * 100 / total) : 100;
    printf("\r  %llu/%llu sectors (%d%%, %s)",
           (unsigned long long)sectors_done, (unsigned long long)total, pct,
           size_h(sectors_done * (uint64_t)ssize));
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  Commands                                                           */
/* ------------------------------------------------------------------ */

#define CHUNK_BYTES (64 * 1024)

static int cmd_list(void)
{
    PUSB_DK_DEVICE_INFO devs = NULL;
    ULONG ndevs = 0, i;

    if (enum_devices(&devs, &ndevs) != 0)
        return 1;

    printf("USB devices visible to USBDK (%lu):\n", ndevs);
    for (i = 0; i < ndevs; i++)
    {
        MSD_EPS eps;
        int ms = (get_ms_eps(&devs[i], &eps) == 0);
        print_device_line(i, &devs[i], ms);

        /* raw capacity: briefly hijack the stick and ask READ CAPACITY(10) */
        if (ms)
        {
            MSD d;
            if (msd_attach(&d, &devs[i], &eps) == 0)
            {
                printf("      capacity: %llu sectors x %u B = %s (raw)\n",
                       (unsigned long long)d.sector_count, d.sector_size,
                       size_h(d.sector_count * (uint64_t)d.sector_size));
                msd_detach(&d);
            }
            else
                printf("      capacity: unavailable (device did not respond)\n");
        }
    }
    if (ndevs == 0)
        printf("  (none)\n");

    U.ReleaseDevicesList(devs);
    return 0;
}

static int cmd_info(int dev_idx, int part_idx)
{
    PUSB_DK_DEVICE_INFO devs = NULL;
    ULONG ndevs = 0, sel;
    MSD_EPS eps;
    MSD d;
    char vendor[9], product[17], rev[5];
    int rc = 1;

    if (enum_devices(&devs, &ndevs) != 0)
        return 1;

    if (select_device(dev_idx, devs, ndevs, &sel, &eps) != 0)
        goto out;

    printf("Hijacking device [%lu] (USB\\VID_%04X&PID_%04X) via USBDK redirect...\n",
           sel, devs[sel].DeviceDescriptor.idVendor, devs[sel].DeviceDescriptor.idProduct);
    if (msd_attach(&d, &devs[sel], &eps) != 0)
        goto out;

    printf("  bulk endpoints : OUT 0x%02X (maxpkt %u), IN 0x%02X (maxpkt %u)\n",
           eps.ep_out, eps.maxpkt_out, eps.ep_in, eps.maxpkt_in);

    if (msd_inquiry(&d, vendor, product, rev) == 0)
        printf("  SCSI inquiry   : \"%s\" \"%s\" rev %s\n", vendor, product, rev);
    else
        printf("  SCSI inquiry   : failed\n");

    printf("  sector size    : %u\n", d.sector_size);
    printf("  capacity       : %llu sectors (%s)\n",
           (unsigned long long)d.sector_count,
           size_h(d.sector_count * (uint64_t)d.sector_size));

    {
        PART parts[MAX_PARTS];
        int np = parse_partitions(&d, parts);
        print_partitions(parts, np);
        if (part_idx > 0)
        {
            uint64_t base, limit;
            if (resolve_part(&d, part_idx, &base, &limit) == 0)
                printf("  with -p %d      : sector numbers are offsets inside partition %d\n",
                       part_idx, part_idx);
        }
    }

    msd_detach(&d);
    printf("Device returned to Windows.\n");
    rc = 0;

out:
    U.ReleaseDevicesList(devs);
    return rc;
}

static int cmd_write(const wchar_t *file, uint64_t start_sector,
                     int dev_idx, int part_idx, int assume_yes)
{
    PUSB_DK_DEVICE_INFO devs = NULL;
    ULONG ndevs = 0, sel;
    MSD_EPS eps;
    MSD d;
    FILE *f = NULL;
    UCHAR *buf = NULL;
    uint64_t fsize, nsect, done;
    uint64_t base = 0, limit;
    unsigned chunk_sect;
    int rc = 1;
    char ans[16];

    if (enum_devices(&devs, &ndevs) != 0)
        return 1;
    if (select_device(dev_idx, devs, ndevs, &sel, &eps) != 0)
        goto out;

    f = _wfopen(file, L"rb");
    if (!f)
    {
        fprintf(stderr, "error: cannot open input file (errno=%d).\n", errno);
        goto out;
    }
    _fseeki64(f, 0, SEEK_END);
    fsize = (uint64_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (fsize == 0)
    {
        fprintf(stderr, "error: input file is empty.\n");
        goto out;
    }

    printf("Hijacking device [%lu] (USB\\VID_%04X&PID_%04X) via USBDK redirect...\n",
           sel, devs[sel].DeviceDescriptor.idVendor, devs[sel].DeviceDescriptor.idProduct);
    if (msd_attach(&d, &devs[sel], &eps) != 0)
        goto out;

    limit = d.sector_count;
    if (part_idx > 0 && resolve_part(&d, part_idx, &base, &limit) != 0)
        goto out_detached;

    nsect = (fsize + d.sector_size - 1) / d.sector_size;
    printf("Target : %s %llu sectors, sector size %u\n",
           part_idx > 0 ? "partition" : "disk",
           (unsigned long long)limit, d.sector_size);
    printf("Plan   : write %llu sectors (%s) at LBA %llu (absolute)\n",
           (unsigned long long)nsect, size_h(nsect * d.sector_size),
           (unsigned long long)(base + start_sector));

    if (limit &&
        (start_sector >= limit || start_sector + nsect > limit))
    {
        fprintf(stderr, "error: write exceeds %s capacity (%llu sectors).\n",
                part_idx > 0 ? "partition" : "device",
                (unsigned long long)limit);
        goto out_detached;
    }

    if (!assume_yes)
    {
        printf("WARNING: this OVERWRITES %llu sectors at LBA %llu of the disk.\n"
               "         Data previously stored there will be lost!\n"
               "Continue? [y/N] ",
               (unsigned long long)nsect,
               (unsigned long long)(base + start_sector));
        fflush(stdout);
        if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y'))
        {
            printf("\nAborted by user.\n");
            goto out_detached;
        }
    }

    buf = (UCHAR *)_aligned_malloc(CHUNK_BYTES, 4096);
    if (!buf)
    {
        fprintf(stderr, "error: out of memory.\n");
        goto out_detached;
    }

    chunk_sect = CHUNK_BYTES / d.sector_size;
    done = 0;
    while (done < nsect)
    {
        uint16_t k = (uint16_t)((nsect - done < chunk_sect) ? (nsect - done) : chunk_sect);
        size_t chunk_bytes = (size_t)k * d.sector_size;
        size_t copy_bytes = (size_t)((fsize - done * d.sector_size) < (uint64_t)chunk_bytes
                                     ? (fsize - done * d.sector_size) : chunk_bytes);

        /* partial final chunk: preserve the untouched tail of the last sector */
        if (copy_bytes < chunk_bytes)
        {
            if (msd_read10(&d, base + start_sector + done, k, buf) != 0)
                goto fail;
        }

        if (fread(buf, 1, copy_bytes, f) != copy_bytes)
        {
            fprintf(stderr, "\nerror: short read on input file.\n");
            goto fail;
        }
        if (msd_write10(&d, base + start_sector + done, k, buf) != 0)
            goto fail;

        done += k;
        progress(done, nsect, done, d.sector_size);
    }
    printf("\nWrite completed: %llu sectors at LBA %llu.\n",
           (unsigned long long)nsect, (unsigned long long)(base + start_sector));

    msd_detach(&d);
    printf("Device returned to Windows (re-mounted; Explorer may ask to format if\n"
           "filesystem structures were overwritten — that is expected).\n");
    rc = 0;
    goto out;

fail:
    printf("\n");
    msd_detach(&d);
    printf("Device returned to Windows.\n");

out_detached:
    if (rc != 0 && d.h)
        msd_detach(&d);

out:
    if (buf) _aligned_free(buf);
    if (f) fclose(f);
    if (devs) U.ReleaseDevicesList(devs);
    return rc;
}

static int cmd_read(uint64_t start_sector, uint64_t count,
                    const wchar_t *outfile, int dev_idx, int part_idx)
{
    PUSB_DK_DEVICE_INFO devs = NULL;
    ULONG ndevs = 0, sel;
    MSD_EPS eps;
    MSD d;
    FILE *f = NULL;
    UCHAR *buf = NULL;
    uint64_t done;
    uint64_t base = 0, limit;
    unsigned chunk_sect;
    int rc = 1;

    if (count == 0)
    {
        fprintf(stderr, "error: sector count must be > 0.\n");
        return 1;
    }
    if (enum_devices(&devs, &ndevs) != 0)
        return 1;
    if (select_device(dev_idx, devs, ndevs, &sel, &eps) != 0)
        goto out;

    f = _wfopen(outfile, L"wb");
    if (!f)
    {
        fprintf(stderr, "error: cannot create output file (errno=%d).\n", errno);
        goto out;
    }

    printf("Hijacking device [%lu] (USB\\VID_%04X&PID_%04X) via USBDK redirect...\n",
           sel, devs[sel].DeviceDescriptor.idVendor, devs[sel].DeviceDescriptor.idProduct);
    if (msd_attach(&d, &devs[sel], &eps) != 0)
        goto out;

    limit = d.sector_count;
    if (part_idx > 0 && resolve_part(&d, part_idx, &base, &limit) != 0)
        goto out_detached;

    if (limit &&
        (start_sector >= limit || start_sector + count > limit))
    {
        fprintf(stderr, "error: read exceeds %s capacity (%llu sectors).\n",
                part_idx > 0 ? "partition" : "device",
                (unsigned long long)limit);
        goto out_detached;
    }

    buf = (UCHAR *)_aligned_malloc(CHUNK_BYTES, 4096);
    if (!buf)
    {
        fprintf(stderr, "error: out of memory.\n");
        goto out_detached;
    }

    chunk_sect = CHUNK_BYTES / d.sector_size;
    done = 0;
    while (done < count)
    {
        uint16_t k = (uint16_t)((count - done < chunk_sect) ? (count - done) : chunk_sect);

        if (msd_read10(&d, base + start_sector + done, k, buf) != 0)
            goto fail;
        if (fwrite(buf, d.sector_size, k, f) != k)
        {
            fprintf(stderr, "\nerror: short write on output file.\n");
            goto fail;
        }
        done += k;
        progress(done, count, done, d.sector_size);
    }
    printf("\nRead completed: %llu sectors at LBA %llu -> \"%ls\" (%s)\n",
           (unsigned long long)count, (unsigned long long)(base + start_sector),
           outfile, size_h(count * (uint64_t)d.sector_size));

    msd_detach(&d);
    printf("Device returned to Windows.\n");
    rc = 0;
    goto out;

fail:
    printf("\n");
    msd_detach(&d);
    printf("Device returned to Windows.\n");

out_detached:
    if (rc != 0 && d.h)
        msd_detach(&d);

out:
    if (buf) _aligned_free(buf);
    if (f) fclose(f);
    if (devs) U.ReleaseDevicesList(devs);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Usage / main                                                       */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
        "usbcp — raw USB flash disk sector read/write via USBDK redirect\n"
        "\n"
        "Usage:\n"
        "  usbcp list\n"
        "  usbcp info    [-d <index>] [-p <part>]\n"
        "  usbcp write <file> <start_sector> [-p <part>] [-d <index>] [-y]\n"
        "  usbcp read  <start_sector> <count> [outfile] [-p <part>] [-d <index>]\n"
        "\n"
        "Examples:\n"
        "  usbcp write 1.txt 409600      copy file 1.txt to disk starting at sector 409600\n"
        "  usbcp read 409600 1024        read 1024 sectors starting at sector 409600 into 1.txt\n"
        "  usbcp read 0 2048 -p 2 p2.img read first 1 MiB of partition 2 into p2.img\n"
        "\n"
        "Options:\n"
        "  -d <index>   select device by index shown by 'usbcp list'\n"
        "               (optional when exactly one USB disk is plugged in)\n"
        "  -p <part>    operate relative to partition <part> (numbering as shown\n"
        "               by 'usbcp info'); sector numbers become offsets inside\n"
        "               that partition instead of absolute disk LBAs\n"
        "  -y           do not ask for confirmation before writing\n"
        "\n"
        "Notes:\n"
        "  * Requires USBDK >= 1.0.22: https://github.com/daynix/UsbDk/releases\n"
        "  * Must run from an elevated (Administrator) console.\n"
        "  * While a command runs the disk is detached from Windows (it\n"
        "    disappears from Explorer); it is returned automatically at the end.\n"
        "  * 'write' destroys existing data at the target sectors.\n"
        "  * 'read' saves to 1.txt when outfile is omitted.\n");
}

static int parse_u64(const wchar_t *s, uint64_t *out)
{
    wchar_t *end = NULL;
    unsigned long long v;

    if (!s || !*s)
        return -1;
#ifdef _MSC_VER
    v = _wcstoui64(s, &end, 10);
#else
    v = wcstoull(s, &end, 10);
#endif
    if (!end || *end != L'\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

int main(void)
{
    int argc = 0, i, rc = 1;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *pos[8];
    int npos = 0, dev_idx = -1, part_idx = 0, assume_yes = 0;
    const wchar_t *cmd;

    if (!argv)
    {
        fprintf(stderr, "error: CommandLineToArgvW failed.\n");
        return 1;
    }

    if (argc < 2)
    {
        usage();
        LocalFree(argv);
        return 0;
    }
    cmd = argv[1];

    for (i = 2; i < argc && npos < 8; i++)
    {
        if (!wcscmp(argv[i], L"-d") || !wcscmp(argv[i], L"/d"))
        {
            uint64_t v;
            if (i + 1 >= argc || parse_u64(argv[i + 1], &v) != 0)
            {
                fprintf(stderr, "error: -d requires a numeric device index.\n");
                goto done;
            }
            dev_idx = (int)v;
            i++;
        }
        else if (!wcscmp(argv[i], L"-p") || !wcscmp(argv[i], L"/p"))
        {
            uint64_t v;
            if (i + 1 >= argc || parse_u64(argv[i + 1], &v) != 0 || v == 0 || v > 1000)
            {
                fprintf(stderr, "error: -p requires a partition number (1..N), see 'usbcp info'.\n");
                goto done;
            }
            part_idx = (int)v;
            i++;
        }
        else if (!wcscmp(argv[i], L"-y") || !wcscmp(argv[i], L"/y"))
        {
            assume_yes = 1;
        }
        else if (!wcscmp(argv[i], L"-h") || !wcscmp(argv[i], L"/?") ||
                 !wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-?"))
        {
            usage();
            rc = 0;
            goto done;
        }
        else
        {
            pos[npos++] = argv[i];
        }
    }

    if (!wcscmp(cmd, L"list") || !wcscmp(cmd, L"/list"))
    {
        if (load_usbdk() != 0) goto done;
        rc = cmd_list();
    }
    else if (!wcscmp(cmd, L"info") || !wcscmp(cmd, L"/info"))
    {
        if (npos > 1) { fprintf(stderr, "error: bad arguments for 'info'.\n"); usage(); goto done; }
        if (load_usbdk() != 0) goto done;
        rc = cmd_info(dev_idx, part_idx);
    }
    else if (!wcscmp(cmd, L"write") || !wcscmp(cmd, L"/write"))
    {
        uint64_t sector;
        if (npos < 2 || parse_u64(pos[1], &sector) != 0)
        {
            fprintf(stderr, "error: usage: usbcp write <file> <start_sector> [-p N] [-d N] [-y]\n");
            goto done;
        }
        if (load_usbdk() != 0) goto done;
        rc = cmd_write(pos[0], sector, dev_idx, part_idx, assume_yes);
    }
    else if (!wcscmp(cmd, L"read") || !wcscmp(cmd, L"/read"))
    {
        uint64_t sector, count;
        if (npos < 2 || parse_u64(pos[0], &sector) != 0 || parse_u64(pos[1], &count) != 0)
        {
            fprintf(stderr, "error: usage: usbcp read <start_sector> <count> [outfile] [-p N] [-d N]\n");
            goto done;
        }
        if (npos > 3)
        {
            fprintf(stderr, "error: too many arguments for 'read'.\n");
            goto done;
        }
        if (load_usbdk() != 0) goto done;
        rc = cmd_read(sector, count, npos >= 3 ? pos[2] : L"1.txt", dev_idx, part_idx);
    }
    else
    {
        fprintf(stderr, "error: unknown command \"%ls\".\n\n", cmd);
        usage();
    }

done:
    if (U.mod) FreeLibrary(U.mod);
    LocalFree(argv);
    return rc;
}
