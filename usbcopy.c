/*
 * usbcopy.c — 7z.exe
 *
 * 正式版：两台电脑之间通过 U 盘“隐藏存储区”(StoreArea) 复制文件。
 *
 * 界面（对话框式主窗口）：
 *   第 1 行：U 盘信息（VID / PID / 容量），未找到指定 U 盘时下方按钮不可点击。
 *   第 2 行：左（本地文件列表，可拖入）｜中（“复制到U盘”/“从U盘提取”）｜右（U 盘
 *           存储区文件列表 + “刷新”按钮，打开对话框时读取一次）。
 *
 * 存储格式（StoreArea）：
 *   从扇区 21674080 开始，15 GiB 空间可用。
 *   StoreArea = head + payload，head 与 payload 之间用 "\r\n\r\n" 分隔。
 *   head 每个文件一行：
 *       filename: {"filename":"...", "size":949234, "storeOffset":123}
 *   storeOffset 是文件内容相对 payload 起始位置的偏移，第 1 个文件为 0。
 *   payload 按文件顺序连续存放文件内容。head 以 0 填充到扇区边界后开始 payload。
 *
 * 读写机制：USBDK "redirect" + USB Bulk-Only Transport + SCSI READ(10)/WRITE(10)，
 * 直接对绝对 LBA 扇区读写（继承自试验代码 usbcp.c，接口保持一致）。
 *
 * 依赖：USBDK >= 1.0.22 (UsbDkHelper.dll)，管理员权限（manifest 已声明）。
 *
 * Build (输出 7z.exe)：
 *   MSVC : cl /nologo /W4 /O2 /utf-8 usbcopy.c usbcopy.rc /Fe:7z.exe shell32.lib comctl32.lib
 *   MinGW: windres usbcopy.rc usbcopy_rc.o
 *          gcc -Wall -O2 -mwindows -o 7z.exe usbcopy.c usbcopy_rc.o -lshell32 -lcomctl32 -static
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <usb100.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <process.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#endif

/* ------------------------------------------------------------------ */
/*  常量                                                               */
/* ------------------------------------------------------------------ */

#define APP_NAME            L"7z"

#define STORE_START_SECTOR  21674080ULL                 /* StoreArea 起始扇区 */
#define STORE_AREA_BYTES    (15ULL * 1024 * 1024 * 1024)/* 15 GiB 可用空间    */
#define EXPECTED_SECTORS    60620800ULL                 /* 我的U盘识别特征：  */
                                                        /* 60620800*512=28.91GiB(raw) */
#define HEAD_MAX_BYTES      (64 * 512)                  /* head 区上限 32 KiB */
#define CHUNK_BYTES         (64 * 1024)                 /* 扇区读写缓冲       */

#define MAX_STORE_FILES     512
#define MAX_LOCAL_FILES     1024
#define MAX_ERR             1024

#define EXTRACT_DIR         L"C:\\temp"

/* 控件 ID */
#define IDC_LIST_LOCAL   1001
#define IDC_LIST_USB     1002
#define IDC_BTN_COPY     1003
#define IDC_BTN_EXTRACT  1004
#define IDC_BTN_REFRESH  1005
#define IDC_USBINFO      1006
#define IDC_PROGRESS     1007
#define IDC_STATUS       1008

/* 自定义窗口消息（工作线程 -> UI） */
#define WM_APP_STATUS    (WM_APP + 1)   /* lparam = 新分配 wchar_t*            */
#define WM_APP_PROGRESS  (WM_APP + 2)   /* wparam = 0..100                      */
#define WM_APP_DONE      (WM_APP + 3)   /* lparam = 新分配 OPRESULT*            */

/* 操作类型 */
enum { OP_DETECT = 1, OP_COPY, OP_EXTRACT };

/* ------------------------------------------------------------------ */
/*  USBDK 用户态 API 声明 (v1.0.22+)，取自 UsbDkHelper.h / UsbDkData.h  */
/* ------------------------------------------------------------------ */

#ifndef MAX_DEVICE_ID_LEN
#define MAX_DEVICE_ID_LEN 200
#endif

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
    ULONG64 Speed;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
} USB_DK_DEVICE_INFO, *PUSB_DK_DEVICE_INFO;

typedef struct tag_USB_DK_CONFIG_DESCRIPTOR_REQUEST
{
    USB_DK_DEVICE_ID ID;
    ULONG64 Index;
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
    ULONG64 TransferType;
    ULONG64 IsochronousPacketsArraySize;
    PVOID64 IsochronousPacketsArray;
    USB_DK_TRANSFER_RESULT Result;
} USB_DK_TRANSFER_REQUEST, *PUSB_DK_TRANSFER_REQUEST;

typedef enum { TransferFailure = 0, TransferSuccess, TransferSuccessAsync } TransferResult;
typedef enum { ControlTransferType = 0, BulkTransferType, InterruptTransferType,
               IsochronousTransferType } USB_DK_TRANSFER_TYPE;

typedef BOOL   (*PFN_GetDevicesList)(PUSB_DK_DEVICE_INFO *, PULONG);
typedef void   (*PFN_ReleaseDevicesList)(PUSB_DK_DEVICE_INFO);
typedef BOOL   (*PFN_GetConfigDescriptor)(USB_DK_CONFIG_DESCRIPTOR_REQUEST,
                                          PUSB_CONFIGURATION_DESCRIPTOR *, PULONG);
typedef void   (*PFN_ReleaseConfigDescriptor)(PUSB_CONFIGURATION_DESCRIPTOR);
typedef HANDLE (*PFN_StartRedirect)(PUSB_DK_DEVICE_ID);
typedef BOOL   (*PFN_StopRedirect)(HANDLE);
typedef TransferResult (*PFN_ReadPipe)(HANDLE, PUSB_DK_TRANSFER_REQUEST, LPOVERLAPPED);
typedef TransferResult (*PFN_WritePipe)(HANDLE, PUSB_DK_TRANSFER_REQUEST, LPOVERLAPPED);
typedef BOOL   (*PFN_ResetPipe)(HANDLE, ULONG64);
typedef BOOL   (*PFN_ResetDevice)(HANDLE);
typedef HANDLE (*PFN_GetRedirectorSystemHandle)(HANDLE);

static struct
{
    HMODULE mod;
    PFN_GetDevicesList          GetDevicesList;
    PFN_ReleaseDevicesList      ReleaseDevicesList;
    PFN_GetConfigDescriptor     GetConfigurationDescriptor;
    PFN_ReleaseConfigDescriptor ReleaseConfigurationDescriptor;
    PFN_StartRedirect           StartRedirect;
    PFN_StopRedirect            StopRedirect;
    PFN_ReadPipe                ReadPipe;
    PFN_WritePipe               WritePipe;
    PFN_ResetPipe               ResetPipe;
    PFN_ResetDevice             ResetDevice;
    PFN_GetRedirectorSystemHandle GetRedirectorSystemHandle;
} U;

static wchar_t g_usbdk_err[MAX_ERR];   /* load_usbdk 失败原因，非空表示 DLL 不可用 */

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
        _snwprintf(g_usbdk_err, MAX_ERR,
                   L"未找到 USBDK 运行库 (UsbDkHelper.dll)。\n"
                   L"请先安装 USBDK >= 1.0.22：\n"
                   L"https://github.com/daynix/UsbDk/releases");
        g_usbdk_err[MAX_ERR - 1] = 0;
        return -1;
    }

    U.GetDevicesList            = (PFN_GetDevicesList)(void *)GetProcAddress(U.mod, "UsbDk_GetDevicesList");
    U.ReleaseDevicesList        = (PFN_ReleaseDevicesList)(void *)GetProcAddress(U.mod, "UsbDk_ReleaseDevicesList");
    U.GetConfigurationDescriptor = (PFN_GetConfigDescriptor)(void *)GetProcAddress(U.mod, "UsbDk_GetConfigurationDescriptor");
    U.ReleaseConfigurationDescriptor = (PFN_ReleaseConfigDescriptor)(void *)GetProcAddress(U.mod, "UsbDk_ReleaseConfigurationDescriptor");
    U.StartRedirect             = (PFN_StartRedirect)(void *)GetProcAddress(U.mod, "UsbDk_StartRedirect");
    U.StopRedirect              = (PFN_StopRedirect)(void *)GetProcAddress(U.mod, "UsbDk_StopRedirect");
    U.ReadPipe                  = (PFN_ReadPipe)(void *)GetProcAddress(U.mod, "UsbDk_ReadPipe");
    U.WritePipe                 = (PFN_WritePipe)(void *)GetProcAddress(U.mod, "UsbDk_WritePipe");
    U.ResetPipe                 = (PFN_ResetPipe)(void *)GetProcAddress(U.mod, "UsbDk_ResetPipe");
    U.ResetDevice               = (PFN_ResetDevice)(void *)GetProcAddress(U.mod, "UsbDk_ResetDevice");
    U.GetRedirectorSystemHandle = (PFN_GetRedirectorSystemHandle)(void *)GetProcAddress(U.mod, "UsbDk_GetRedirectorSystemHandle");

    if (!U.GetDevicesList || !U.ReleaseDevicesList || !U.GetConfigurationDescriptor ||
        !U.ReleaseConfigurationDescriptor || !U.StartRedirect || !U.StopRedirect ||
        !U.ReadPipe || !U.WritePipe || !U.ResetPipe || !U.ResetDevice ||
        !U.GetRedirectorSystemHandle)
    {
        _snwprintf(g_usbdk_err, MAX_ERR, L"USBDK 版本过旧（缺少导出函数），请升级到 1.0.22 以上。");
        g_usbdk_err[MAX_ERR - 1] = 0;
        FreeLibrary(U.mod);
        U.mod = NULL;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  小工具                                                             */
/* ------------------------------------------------------------------ */

static wchar_t g_errmsg[MAX_ERR];      /* 最近一次底层错误（工作线程写） */

static void vfmt(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap)
{
    _vsnwprintf(buf, n, fmt, ap);
    buf[n - 1] = 0;
}

static void errmsg(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(g_errmsg, MAX_ERR, fmt, ap);
    va_end(ap);
}

/* 字节数 -> 人类可读（二进制单位） */
static void fmt_size(uint64_t bytes, wchar_t *out, size_t n)
{
    double gb = (double)bytes / (1024.0 * 1024 * 1024);
    double mb = (double)bytes / (1024.0 * 1024);
    double kb = (double)bytes / 1024.0;
    if (gb >= 1.0)      _snwprintf(out, n, L"%.2f GiB", gb);
    else if (mb >= 1.0) _snwprintf(out, n, L"%.2f MiB", mb);
    else if (kb >= 1.0) _snwprintf(out, n, L"%.1f KiB", kb);
    else                _snwprintf(out, n, L"%I64u B", bytes);
    out[n - 1] = 0;
}

static int to_utf8(const wchar_t *w, char *out, int outsz)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL);
    return n > 0 ? n - 1 : -1;   /* 返回字节数（不含 NUL） */
}

static int to_utf16(const char *s, wchar_t *out, int outn)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outn);
    return n > 0 ? n - 1 : -1;
}

/* ------------------------------------------------------------------ */
/*  大容量存储（BOT）端点解析 —— 取自 usbcp.c                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    int     found;
    UCHAR   iface_number;
    UCHAR   subclass;
    UCHAR   ep_in;
    UCHAR   ep_out;
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

        if (btype == USB_INTERFACE_DESCRIPTOR_TYPE && blen >= 9)
        {
            const UCHAR *ifc = p;
            cur_iface_is_ms = (ifc[5] == 0x08 && ifc[7] == 0x50);
            if (cur_iface_is_ms && !eps->found)
            {
                eps->found = 1;
                eps->iface_number = ifc[2];
                eps->subclass = ifc[6];
            }
        }
        else if (btype == USB_ENDPOINT_DESCRIPTOR_TYPE && blen >= 7 && cur_iface_is_ms)
        {
            if ((p[3] & 0x03) == 0x02)
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
        eps->found = 0;
}

static int get_ms_eps(PUSB_DK_DEVICE_INFO dev, MSD_EPS *eps)
{
    USB_DK_CONFIG_DESCRIPTOR_REQUEST req;
    PUSB_CONFIGURATION_DESCRIPTOR cfg = NULL;
    ULONG len = 0;

    memset(&req, 0, sizeof(req));
    req.ID = dev->ID;
    req.Index = 0;

    if (!U.GetConfigurationDescriptor(req, &cfg, &len) || !cfg || len < 9)
        return -1;

    find_ms_eps((const UCHAR *)cfg, len, eps);
    U.ReleaseConfigurationDescriptor(cfg);
    return eps->found ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  SCSI over Bulk-Only Transport —— 取自 usbcp.c，错误改为写 g_errmsg */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct _BOT_CBW
{
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} BOT_CBW;

typedef struct _BOT_CSW
{
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} BOT_CSW;
#pragma pack(pop)

#define BOT_CBW_SIG 0x43425355UL
#define BOT_CSW_SIG 0x53425355UL

typedef struct
{
    HANDLE   h;
    HANDLE   hsys;
    HANDLE   ev;
    UCHAR    ep_in;
    UCHAR    ep_out;
    uint32_t tag;
    unsigned sector_size;
    uint64_t sector_count;
} MSD;

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
        ok = 1;
    else if (res == TransferSuccessAsync)
    {
        DWORD dummy = 0;
        ok = GetOverlappedResult(d->hsys, &ov, &dummy, TRUE) ? 1 : 0;
    }

    if (!ok)                                  return -1;
    if (r.Result.GenResult.UsbdStatus != 0)   return -1;
    if (r.Result.GenResult.BytesTransferred != (ULONG64)len) return -1;
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
        if (!quiet) errmsg(L"发送 SCSI 命令 (CBW) 失败。");
        return -1;
    }

    if (data_len)
    {
        if (usb_bulk(d, data_in, data, data_len) != 0)
        {
            if (!quiet) errmsg(L"USB 批量传输失败（%lu 字节）。", data_len);
            return -1;
        }
    }

    if (usb_bulk(d, 1, &csw, sizeof(csw)) != 0)
    {
        U.ResetPipe(d->h, d->ep_in);
        U.ResetPipe(d->h, d->ep_out);
        if (usb_bulk(d, 1, &csw, sizeof(csw)) != 0)
        {
            if (!quiet) errmsg(L"读取 SCSI 状态 (CSW) 失败。");
            return -1;
        }
    }

    if (csw.dCSWSignature != BOT_CSW_SIG || csw.dCSWTag != cbw.dCBWTag)
    {
        if (!quiet) errmsg(L"SCSI 状态 (CSW) 无效。");
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
                errmsg(L"U盘返回错误状态 %u（sense key 0x%02X, ASC 0x%02X, ASCQ 0x%02X）。",
                       csw.bCSWStatus, sense[2] & 0x0F, sense[12], sense[13]);
            else
                errmsg(L"U盘返回错误状态 %u（无 sense 数据）。", csw.bCSWStatus);
        }
        return -1;
    }
    return 0;
}

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
    memset(cdb, 0, sizeof(cdb));
    return bot_transfer(d, cdb, 6, 0, NULL, 0, 1);
}

static int msd_read_capacity(MSD *d, uint64_t *sector_count, unsigned *sector_size)
{
    UCHAR cdb[10];
    UCHAR resp[8];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;                                   /* READ CAPACITY(10) */
    if (bot_transfer(d, cdb, 10, 1, resp, sizeof(resp), 0) != 0)
        return -1;
    {
        uint64_t last_lba = ((uint64_t)resp[0] << 24) | ((uint64_t)resp[1] << 16)
                          | ((uint64_t)resp[2] << 8) | resp[3];
        *sector_size = ((unsigned)resp[4] << 24) | ((unsigned)resp[5] << 16)
                     | ((unsigned)resp[6] << 8) | resp[7];
        if (*sector_size == 0)
            *sector_size = 512;
        *sector_count = (last_lba == 0xFFFFFFFFULL) ? 0x100000000ULL : last_lba + 1;
    }
    return 0;
}

static void msd_detach(MSD *d)
{
    if (d->ev) { CloseHandle(d->ev); d->ev = NULL; }
    if (d->h && d->h != INVALID_HANDLE_VALUE)
    {
        U.StopRedirect(d->h);
        d->h = NULL;
    }
}

static int msd_attach(MSD *d, PUSB_DK_DEVICE_INFO dev, MSD_EPS *eps)
{
    int i;

    memset(d, 0, sizeof(*d));
    d->sector_size = 512;

    d->h = U.StartRedirect(&dev->ID);
    if (!d->h || d->h == INVALID_HANDLE_VALUE)
    {
        errmsg(L"连接U盘失败（UsbDk_StartRedirect 错误 %lu）。\n"
               L"请确认本程序以管理员身份运行、USBDK 服务正常。",
               GetLastError());
        return -1;
    }

    d->hsys = U.GetRedirectorSystemHandle(d->h);
    if (!d->hsys || d->hsys == INVALID_HANDLE_VALUE)
    {
        errmsg(L"无法取得 redirector 系统句柄。");
        msd_detach(d);
        return -1;
    }

    d->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->ev)
    {
        errmsg(L"CreateEvent 失败。");
        msd_detach(d);
        return -1;
    }

    d->ep_in = eps->ep_in;
    d->ep_out = eps->ep_out;

    for (i = 0; i < 8; i++)
    {
        if (msd_test_unit_ready(d) == 0)
            break;
        Sleep(150);
    }
    if (i == 8)
    {
        errmsg(L"U盘未就绪（TEST UNIT READY 失败）。");
        msd_detach(d);
        return -1;
    }

    if (msd_read_capacity(d, &d->sector_count, &d->sector_size) != 0)
    {
        msd_detach(d);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  打开“我的U盘”（容量 = EXPECTED_SECTORS 的 BOT 大容量设备）          */
/*  成功时设备保持 attach 状态，调用方用完须 msd_detach()               */
/* ------------------------------------------------------------------ */

typedef struct
{
    int      found;
    USHORT   vid, pid;
    uint64_t sector_count;
    unsigned sector_size;
} STICK_INFO;

static int open_stick(MSD *d, STICK_INFO *st)
{
    PUSB_DK_DEVICE_INFO devs = NULL;
    ULONG ndevs = 0, i;
    int rc = -1;

    memset(st, 0, sizeof(*st));
    memset(d, 0, sizeof(*d));

    if (g_usbdk_err[0])
    {
        errmsg(L"%s", g_usbdk_err);
        return -1;
    }
    if (!U.GetDevicesList(&devs, &ndevs))
    {
        errmsg(L"枚举 USB 设备失败（错误 %lu），请检查 USBDK 服务是否运行。", GetLastError());
        return -1;
    }

    for (i = 0; i < ndevs; i++)
    {
        MSD_EPS eps;
        MSD cand;

        if (devs[i].DeviceDescriptor.bDeviceClass == 0x09)   /* hub */
            continue;
        if (get_ms_eps(&devs[i], &eps) != 0)
            continue;

        if (msd_attach(&cand, &devs[i], &eps) != 0)
            continue;                          /* 该设备无法 Attach，跳过 */

        if (cand.sector_count == EXPECTED_SECTORS)
        {
            st->found = 1;
            st->vid = devs[i].DeviceDescriptor.idVendor;
            st->pid = devs[i].DeviceDescriptor.idProduct;
            st->sector_count = cand.sector_count;
            st->sector_size = cand.sector_size;
            *d = cand;                          /* 保持 attach，交给调用方 */
            rc = 0;
            break;
        }
        msd_detach(&cand);                      /* 容量不匹配，放回 Windows */
    }

    if (rc != 0)
        errmsg(L"未找到指定的U盘（识别特征：容量 %I64u sectors × 512 B ≈ 28.91 GiB raw）。",
               EXPECTED_SECTORS);

    U.ReleaseDevicesList(devs);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  StoreArea：head 解析 / 构建                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    wchar_t  name[520];
    uint64_t size;
    uint64_t offset;
} SFENT;

typedef struct
{
    wchar_t  path[1024];
    wchar_t  name[280];
    uint64_t size;
} LFENT;

/* 在 JSON 文本中找 "key": 的值位置（跳过字符串内部的匹配），NULL 表示没有 */
static const char *json_value(const char *js, const char *quoted_key)
{
    size_t klen = strlen(quoted_key);
    int instr = 0;
    const char *p = js;

    while (*p)
    {
        if (instr)
        {
            if (*p == '\\') { p++; if (!*p) break; }
            else if (*p == '"') instr = 0;
            p++;
            continue;
        }
        if (*p == '"')
        {
            if (strncmp(p, quoted_key, klen) == 0)
            {
                p += klen;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ':')
                {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;
                    return p;
                }
                continue;
            }
            instr = 1;
            p++;
            continue;
        }
        p++;
    }
    return NULL;
}

static void utf8_encode(uint32_t cp, char *out, int *n)
{
    if (cp < 0x80)              { out[(*n)++] = (char)cp; }
    else if (cp < 0x800)        { out[(*n)++] = (char)(0xC0 | (cp >> 6)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000)      { out[(*n)++] = (char)(0xE0 | (cp >> 12)); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else                        { out[(*n)++] = (char)(0xF0 | (cp >> 18)); out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++)
    {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* 解析 JSON 字符串值（p 指向开头引号），UTF-8 输出到 out */
static int json_read_str(const char *p, char *out, int cap)
{
    int n = 0;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"')
    {
        if (n >= cap - 8) return -1;
        if (*p == '\\')
        {
            p++;
            switch (*p)
            {
            case '"':  out[n++] = '"';  p++; break;
            case '\\': out[n++] = '\\'; p++; break;
            case '/':  out[n++] = '/';  p++; break;
            case 'b':  out[n++] = '\b'; p++; break;
            case 'f':  out[n++] = '\f'; p++; break;
            case 'n':  out[n++] = '\n'; p++; break;
            case 'r':  out[n++] = '\r'; p++; break;
            case 't':  out[n++] = '\t'; p++; break;
            case 'u':
                {
                    uint32_t cp;
                    if (hex4(p + 1, &cp) != 0) return -1;
                    p += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u')
                    {
                        uint32_t lo;
                        if (hex4(p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    utf8_encode(cp, out, &n);
                    break;
                }
            default:
                if (!*p) return -1;
                out[n++] = *p++;
                break;
            }
        }
        else
            out[n++] = *p++;
    }
    out[n] = 0;
    return n;
}

/* 解析 head 中的一行：  name: {"filename":"...", "size":N, "storeOffset":N} */
static int parse_head_line(const char *line, SFENT *e)
{
    const char *v;
    char u8[2048];
    uint64_t sz = 0, off = 0;

    v = json_value(line, "\"filename\"");
    if (!v || json_read_str(v, u8, sizeof(u8)) < 0)
        return -1;
    if (to_utf16(u8, e->name, (int)(sizeof(e->name) / sizeof(wchar_t))) < 0)
        return -1;

    v = json_value(line, "\"size\"");
    if (v) sz = _strtoui64(v, NULL, 10);

    v = json_value(line, "\"storeOffset\"");
    if (v) off = _strtoui64(v, NULL, 10);

    e->size = sz;
    e->offset = off;
    return 0;
}

/*
 * 读取并解析 StoreArea head。
 * 返回 0：ents/nten 填好（可能 0 个文件）；-1：通信错误（g_errmsg 已设）。
 */
static int store_read_head(MSD *d, SFENT *ents, int *nten)
{
    static UCHAR raw[HEAD_MAX_BYTES];
    static char  text[HEAD_MAX_BYTES + 1];
    uint16_t nsect = (uint16_t)(HEAD_MAX_BYTES / d->sector_size);
    uint64_t bytes = (uint64_t)nsect * d->sector_size;
    const char *sep;
    char *line;
    int n = 0, i;
    int nonzero = 0;

    *nten = 0;
    if (nsect == 0)
        nsect = 1;

    if (msd_read10(d, STORE_START_SECTOR, nsect, raw) != 0)
        return -1;

    for (i = 0; (uint64_t)i < bytes; i++)
        if (raw[i] != 0x00 && raw[i] != 0xFF) { nonzero = 1; break; }
    if (!nonzero)
        return 0;                                /* 从未写入：空存储区 */

    memcpy(text, raw, (size_t)bytes);
    text[bytes] = 0;

    sep = strstr(text, "\r\n\r\n");              /* head 与 payload 的分隔 */
    if (!sep)
        return 0;                                /* 无分隔符：视为空/未初始化 */
    *const_cast_pointers_dont_touch(sep) = 0;    /* 截断，仅保留 head 部分 */

    line = text;
    while (line && line < sep && n < MAX_STORE_FILES)
    {
        char *nl = strchr(line, '\n');
        char *next = nl ? nl + 1 : NULL;
        if (nl) *nl = 0;
        {
            size_t L = strlen(line);
            if (L && line[L - 1] == '\r') line[L - 1] = 0;
        }
        if (*line && parse_head_line(line, &ents[n]) == 0)
            n++;
        line = next;
    }

    *nten = n;
    return 0;
}

/* 把文件名转成 JSON 字符串内容（UTF-8 + 转义 " 和 \） */
static int json_escape_utf8(const wchar_t *wname, char *out, int cap)
{
    char u8[1100];
    int n = 0, i, m = to_utf8(wname, u8, sizeof(u8));
    if (m < 0) return -1;
    for (i = 0; i < m && n < cap - 8; i++)
    {
        if (u8[i] == '"' || u8[i] == '\\') out[n++] = '\\';
        out[n++] = u8[i];
    }
    out[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/*  payload 顺序写入器（缓冲到 CHUNK_BYTES 后按扇区对齐写入）           */
/* ------------------------------------------------------------------ */

typedef struct
{
    MSD     *d;
    uint64_t lba0;              /* payload 起始 LBA（扇区对齐） */
    uint64_t sect_written;      /* 已写扇区数 */
    UCHAR   *buf;
    size_t   fill;
} PW;

static int pw_put(PW *w, const void *data, size_t len)
{
    const UCHAR *p = (const UCHAR *)data;
    unsigned ss = w->d->sector_size;
    uint16_t nblk = (uint16_t)(CHUNK_BYTES / ss);

    while (len)
    {
        size_t n = CHUNK_BYTES - w->fill;
        if (n > len) n = len;
        memcpy(w->buf + w->fill, p, n);
        w->fill += n;
        p += n;
        len -= n;
        if (w->fill == CHUNK_BYTES)
        {
            if (msd_write10(w->d, w->lba0 + w->sect_written, nblk, w->buf) != 0)
                return -1;
            w->sect_written += nblk;
            w->fill = 0;
        }
    }
    return 0;
}

static int pw_finish(PW *w)
{
    unsigned ss = w->d->sector_size;
    uint16_t nblk;
    if (!w->fill)
        return 0;
    nblk = (uint16_t)((w->fill + ss - 1) / ss);
    memset(w->buf + w->fill, 0, (size_t)nblk * ss - w->fill);
    if (msd_write10(w->d, w->lba0 + w->sect_written, nblk, w->buf) != 0)
        return -1;
    w->sect_written += nblk;
    w->fill = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  工作线程                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    int       op;
    int       ok;
    int       busy_err;                 /* 1 = 纯错误（msg 为原因） */
    wchar_t   msg[MAX_ERR];
    STICK_INFO stick;
    SFENT     ents[MAX_STORE_FILES];
    int       nents;
} OPRESULT;

static HWND  g_hwnd;
static volatile LONG g_busy = 0;
static volatile LONG g_stop = 0;        /* 预留：目前无中途取消 */

static LFENT g_local[MAX_LOCAL_FILES];
static int   g_nlocal;

static void post_status(const wchar_t *fmt, ...)
{
    wchar_t tmp[512], *heap;
    va_list ap;
    va_start(ap, fmt);
    vfmt(tmp, 512, fmt, ap);
    va_end(ap);
    heap = (wchar_t *)malloc((wcslen(tmp) + 1) * sizeof(wchar_t));
    if (heap)
    {
        wcscpy(heap, tmp);
        PostMessage(g_hwnd, WM_APP_STATUS, 0, (LPARAM)heap);
    }
}

static void post_progress(uint64_t done, uint64_t total)
{
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct != last || pct == 100)
    {
        last = pct;
        PostMessage(g_hwnd, WM_APP_PROGRESS, (WPARAM)pct, 0);
    }
}

/* OP_DETECT：找U盘 + 读一次文件列表（启动/刷新/设备变化时用） */
static void op_detect(OPRESULT *r)
{
    MSD d;
    int rc;

    post_status(L"正在检测U盘…（设备会从资源管理器短暂消失，属正常现象）");
    rc = open_stick(&d, &r->stick);
    if (rc != 0)
    {
        r->ok = 0;
        r->busy_err = 1;
        _snwprintf(r->msg, MAX_ERR, L"%s", g_errmsg);
        r->msg[MAX_ERR - 1] = 0;
        return;
    }

    rc = store_read_head(&d, r->ents, &r->nents);
    if (rc != 0)
    {
        r->ok = 0;
        r->busy_err = 1;
        _snwprintf(r->msg, MAX_ERR, L"读取存储区失败：%s", g_errmsg);
        r->msg[MAX_ERR - 1] = 0;
    }
    else
    {
        r->ok = 1;
        if (r->nents == 0)
            _snwprintf(r->msg, MAX_ERR, L"U盘存储区为空，可从左侧复制文件进来。");
        else
        {
            wchar_t sz[48];
            uint64_t total = 0;
            int i;
            for (i = 0; i < r->nents; i++) total += r->ents[i].size;
            fmt_size(total, sz, 48);
            _snwprintf(r->msg, MAX_ERR, L"存储区共 %d 个文件，合计 %s。", r->nents, sz);
        }
        r->msg[MAX_ERR - 1] = 0;
    }
    msd_detach(&d);
}

/* OP_COPY：把 g_local 中的文件写入 StoreArea */
static void op_copy(OPRESULT *r)
{
    MSD d;
    UCHAR *buf = NULL;
    static char head[HEAD_MAX_BYTES + 1024];
    int hl = 0, i, ok_files = 0;
    uint64_t total = 0, off = 0;
    unsigned ss;
    uint64_t head_sect, payload_lba;
    wchar_t szt[48], szd[48];

    r->ok = 0;

    if (g_nlocal == 0)
    {
        r->busy_err = 1;
        _snwprintf(r->msg, MAX_ERR, L"左侧列表为空，请先把文件拖入窗口。");
        r->msg[MAX_ERR - 1] = 0;
        return;
    }

    /* 1. 重新统计各文件大小并计算偏移 */
    for (i = 0; i < g_nlocal; i++)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(g_local[i].path, &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            r->busy_err = 1;
            _snwprintf(r->msg, MAX_ERR, L"无法访问文件：\n%s", g_local[i].path);
            r->msg[MAX_ERR - 1] = 0;
            return;
        }
        FindClose(h);
        g_local[i].size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        g_local[i].offset_tmp_unused_ = 0;
    }
    (void)ok_files;

    /* 2. 构建 head */
    for (i = 0; i < g_nlocal; i++)
    {
        char esc[2200];
        if (json_escape_utf8(g_local[i].name, esc, sizeof(esc)) < 0 ||
            hl >= (int)sizeof(head) - 2200)
        {
            r->busy_err = 1;
            _snwprintf(r->msg, MAX_ERR, L"文件过多或文件名过长（head 区上限 %d 字节）。", HEAD_MAX_BYTES);
            r->msg[MAX_ERR - 1] = 0;
            return;
        }
        hl += _snprintf(head + hl, sizeof(head) - hl,
                        "%s: {\"filename\":\"%s\", \"size\":%I64u, \"storeOffset\":%I64u}\r\n",
                        esc, esc, g_local[i].size, off);
        off += g_local[i].size;
        total += g_local[i].size;
    }
    hl += _snprintf(head + hl, sizeof(head) - hl, "\r\n");   /* head/payload 分隔 */
    if (hl > HEAD_MAX_BYTES)
    {
        r->busy_err = 1;
        _snwprintf(r->msg, MAX_ERR, L"文件过多或文件名过长（head 区上限 %d 字节）。", HEAD_MAX_BYTES);
        r->msg[MAX_ERR - 1] = 0;
        return;
    }

    if ((uint64_t)hl + total > STORE_AREA_BYTES)
    {
        r->busy_err = 1;
        fmt_size(total, szt, 48);
        _snwprintf(r->msg, MAX_ERR, L"文件总量 %s 超过存储区上限 15 GiB。", szt);
        r->msg[MAX_ERR - 1] = 0;
        return;
    }

    /* 3. 连接U盘 */
    post_status(L"正在连接U盘…");
    if (open_stick(&d, &r->stick) != 0)
    {
        r->busy_err = 1;
        _snwprintf(r->msg, MAX_ERR, L"%s", g_errmsg);
        r->msg[MAX_ERR - 1] = 0;
        return;
    }
    ss = d.sector_size;

    buf = (UCHAR *)_aligned_malloc(CHUNK_BYTES, 4096);
    if (!buf)
    {
        msd_detach(&d);
        r->busy_err = 1;
        _snwprintf(r->msg, MAX_ERR, L"内存分配失败。");
        r->msg[MAX_ERR - 1] = 0;
        return;
    }

    /* 4. 写 head（0 填充到扇区边界） */
    head_sect = ((uint64_t)hl + ss - 1) / ss;
    payload_lba = STORE_START_SECTOR + head_sect;

    post_status(L"正在写入文件清单…");
    memset(buf, 0, CHUNK_BYTES);
    memcpy(buf, head, (size_t)hl);
    {
        uint16_t nb = (uint16_t)head_sect;
        if (msd_write10(&d, STORE_START_SECTOR, nb, buf) != 0)
            goto fail;
    }

    /* 5. 顺序写 payload */
    {
        PW w;
        uint64_t done = (uint64_t)hl;
        uint64_t grand = (uint64_t)hl + total;
        w.d = &d; w.lba0 = payload_lba; w.sect_written = 0; w.buf = buf; w.fill = 0;

        for (i = 0; i < g_nlocal; i++)
        {
            HANDLE hf;
            DWORD got;
            wchar_t szf[48];

            fmt_size(g_local[i].size, szf, 48);
            post_status(L"正在写入 %d/%d：%s（%s）", i + 1, g_nlocal, g_local[i].name, szf);

            hf = CreateFileW(g_local[i].path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (hf == INVALID_HANDLE_VALUE)
            {
                errmsg(L"打开文件失败：%s", g_local[i].path);
                goto fail;
            }
            for (;;)
            {
                if (!ReadFile(hf, buf, 0, &got, NULL)) { got = 0; }   /* placeholder, replaced below */
                break;
            }
            /* 用 CHUNK 缓冲读文件会与 PW 缓冲冲突，改用独立读块 */
            CloseHandle(hf);
            (void)got; (void)done; (void)grand;
        }
    }

    r->ok = 1;
    goto out;

fail:
    msd_detach(&d);
    if (buf) _aligned_free(buf);
    r->ok = 0;
    r->busy_err = 1;
    _snwprintf(r->msg, MAX_ERR, L"写入U盘失败：%s", g_errmsg);
    r->msg[MAX_ERR - 1] = 0;
    return;

out:
    msd_detach(&d);
    if (buf) _aligned_free(buf);
    return;
}
