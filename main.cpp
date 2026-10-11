// -----------------------------------------------------------------------------
//  HardwareMonitor — 极简硬件信息查看器
//  Win32 GUI / 纯 GDI / 零第三方依赖 / 不需要管理员权限
//
//  数据源（全部普通用户权限可读）：
//    CPU 温度  PDH 性能计数器 \Thermal Zone Information(*)\Temperature   （开尔文）
//    磁盘温度  IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceTemperatureProperty
//    静态信息  SMBIOS (GetSystemFirmwareTable 'RSMB') + 注册表 + Win32 API
//
//  编译见 build.bat
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
// 注意：winsock2.h / ws2ipdef.h 必须先于 netioapi.h，netioapi.h 又必须先于 iphlpapi.h。
// MinGW 的 netioapi.h 用 `#ifdef __IPHLPAPI_H__` 做分支，若 iphlpapi.h 已先包含，
// 它会跳过 include ws2ipdef.h，导致 GetIfTable2 / MIB_IF_TABLE2 全部不可见。
#include <winsock2.h>
#include <ws2ipdef.h>
#include <netioapi.h>
#include <iphlpapi.h>
#include <dxgi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <cmath>

// ============================== 常量 ==============================

static const wchar_t* APP_TITLE   = L"硬件监视器";
// 必须与 app.rc 里的 FILEVERSION / PRODUCTVERSION 一致
static const wchar_t* APP_VERSION = L"v1.0.0.0";
// 客户区宽度（逻辑像素）。改这一个数即可整体变宽/变窄，卡片宽度随之三等分。
// 下限参考：最挤的是内存卡（大数值 + "/ 31 GB"），加上左右内边距约需 215 物理像素；
// 本机 168 DPI(1.75×) × uiScale 0.9 = 1.575，取 560 时卡片约 270 物理像素，留有余量。
static const int  CLIENT_W   = 520;
static const UINT_PTR TIMER_ID = 1;
static const int  HIST_MAX   = 180;     // 曲线缓冲：3 分钟
static const int  HIST_SHOW  = 120;     // 显示：2 分钟
static const double TEMP_MIN = 25.0;
static const double TEMP_MAX = 105.0;

// 布局（逻辑像素，经 PX() 缩放到物理像素）
static const int MARGIN    = 12;
static const int CARD_H    = 84;
static const int GAP       = 10;
static const int CHART_H   = 110;
static const int TOOL_H    = 34;    // 顶部工具栏（置顶 / 复制）
static const int HEAD_H    = 34;    // 可折叠区块的标题条
static const int INFO_ROW  = 22;    // 每行高度
static const int INFO_TOP  = 10;    // 信息区首行距内容区顶的内边距（InfoBlockH 必须算上）
static const int INFO_PAD  = 8;     // 末行距区块底的余量

// ============================== 主题 ==============================

struct Palette {
    COLORREF bg, card, border, text, text2, track, red, blue, green, amber, teal, violet;
};

static Palette g_pal;
static float   g_scale   = 1.0f;    // DPI 缩放（物理像素 / 逻辑像素）
static float   g_uiScale = 0.9f;    // 整体缩小系数

// 逻辑尺寸 -> 物理像素。用浮点相乘，避免 175% DPI 被取整成 2.0（那会放大 14%）
static inline int PX(float v) {
    int r = (int)(v * g_scale * g_uiScale + 0.5f);
    return r < 1 ? 1 : r;
}

static void LoadPalette(bool dark) {
    if (dark) {
        g_pal.bg     = RGB(0x1A, 0x1B, 0x1E);
        g_pal.card   = RGB(0x24, 0x26, 0x2A);
        g_pal.border = RGB(0x33, 0x36, 0x3B);
        g_pal.text   = RGB(0xE8, 0xEA, 0xED);
        g_pal.text2  = RGB(0x9A, 0xA0, 0xA6);
        g_pal.track  = RGB(0x2E, 0x31, 0x35);
        g_pal.red    = RGB(0xF0, 0x95, 0x95);
        g_pal.blue   = RGB(0x85, 0xB7, 0xEB);
        g_pal.green  = RGB(0x97, 0xC4, 0x59);
        g_pal.amber  = RGB(0xEF, 0x9F, 0x27);
        g_pal.teal   = RGB(0x5D, 0xCA, 0xA5);
        g_pal.violet = RGB(0xAF, 0xA9, 0xEC);
    } else {
        g_pal.bg     = RGB(0xF5, 0xF6, 0xF8);
        g_pal.card   = RGB(0xFF, 0xFF, 0xFF);
        g_pal.border = RGB(0xE3, 0xE5, 0xE9);
        g_pal.text   = RGB(0x1A, 0x1C, 0x1F);
        g_pal.text2  = RGB(0x6B, 0x72, 0x80);
        g_pal.track  = RGB(0xED, 0xEF, 0xF2);
        g_pal.red    = RGB(0xD8, 0x3B, 0x3A);
        g_pal.blue   = RGB(0x2A, 0x6F, 0xC2);
        g_pal.green  = RGB(0x4F, 0x82, 0x18);
        g_pal.amber  = RGB(0xC1, 0x7A, 0x0E);
        g_pal.teal   = RGB(0x0E, 0x7C, 0x86);
        g_pal.violet = RGB(0x5B, 0x4F, 0xC4);
    }
}

// ============================== 数据模型 ==============================

struct Metrics {
    double cpuTempC  = -1.0;
    double ssdTempC  = -1.0;
    double cpuUsage  = -1.0;
    double ramUsedGB = 0.0;
    double ramTotalGB= 0.0;
    double ramPct    = -1.0;
    double netRxBps  = -1.0;
    double netTxBps  = -1.0;
};

// 显卡信息（DXGI 取得，DedicatedVideoMemory 是 64 位，比 WMI 的 AdapterRAM 准）
struct GpuInfo {
    std::wstring name;
    unsigned long long dedicatedVideoMB = 0;
    unsigned long long dedicatedSysMB   = 0;
    unsigned long long sharedSysMB      = 0;
};

struct InfoRow {
    std::wstring label;
    std::wstring value;
};

static Metrics              g_m;
static std::vector<InfoRow> g_info;
static std::wstring         g_cpuName;
static std::vector<GpuInfo> g_gpuList;

static double g_histCpu[HIST_MAX];
static double g_histSsd[HIST_MAX];
static int    g_histCount = 0;
static int    g_histHead  = 0;

// ============================== 小工具 ==============================

static std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    return std::wstring(buf);
}

static std::wstring AnsiToWide(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

static std::wstring TrimW(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\n' || s.back() == L'\0'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\r' || s[i] == L'\n')) ++i;
    return s.substr(i);
}

// ============================== CPU 温度 (PDH) ==============================

static PDH_HQUERY   g_pdhQuery   = nullptr;
static PDH_HCOUNTER g_pdhCounter = nullptr;
static bool         g_pdhOk      = false;

typedef PDH_STATUS (WINAPI *PFN_PdhAddEnglishCounterW)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS (WINAPI *PFN_PdhGetFormattedCounterArrayW)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PDH_FMT_COUNTERVALUE_ITEM_W*);

static PFN_PdhAddEnglishCounterW        pAddEnglish = nullptr;
static PFN_PdhGetFormattedCounterArrayW pGetArray   = nullptr;

static bool CpuTempInit() {
    HMODULE hPdh = GetModuleHandleW(L"pdh.dll");
    if (!hPdh) hPdh = LoadLibraryW(L"pdh.dll");
    if (hPdh) {
        pAddEnglish = (PFN_PdhAddEnglishCounterW)GetProcAddress(hPdh, "PdhAddEnglishCounterW");
        pGetArray   = (PFN_PdhGetFormattedCounterArrayW)GetProcAddress(hPdh, "PdhGetFormattedCounterArrayW");
    }
    if (PdhOpenQueryW(nullptr, 0, &g_pdhQuery) != ERROR_SUCCESS) return false;

    const wchar_t* path = L"\\Thermal Zone Information(*)\\Temperature";
    PDH_STATUS st = PDH_CSTATUS_NO_OBJECT;
    if (pAddEnglish) st = pAddEnglish(g_pdhQuery, path, 0, &g_pdhCounter);
    if (st != ERROR_SUCCESS) st = PdhAddCounterW(g_pdhQuery, path, 0, &g_pdhCounter);
    if (st != ERROR_SUCCESS) {
        PdhCloseQuery(g_pdhQuery);
        g_pdhQuery = nullptr;
        return false;
    }
    PdhCollectQueryData(g_pdhQuery);
    PdhCollectQueryData(g_pdhQuery);
    g_pdhOk = true;
    return true;
}

static double CpuTempRead() {
    if (!g_pdhOk || !pGetArray) return -1.0;
    if (PdhCollectQueryData(g_pdhQuery) != ERROR_SUCCESS) return -1.0;

    DWORD size = 0, type = 0;
    PDH_STATUS st = pGetArray(g_pdhCounter, PDH_FMT_DOUBLE, &size, &type, nullptr);
    if ((DWORD)st != (DWORD)PDH_MORE_DATA || size == 0) return -1.0;

    std::vector<BYTE> buf(size);
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (pGetArray(g_pdhCounter, PDH_FMT_DOUBLE, &size, &type, items) != ERROR_SUCCESS) return -1.0;

    DWORD count = size / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W);
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
            return items[i].FmtValue.doubleValue - 273.15;   // 计数器单位是开尔文
        }
    }
    return -1.0;
}

// ============================== 磁盘温度 (IOCTL) ==============================

// MinGW 的 winioctl.h 未必定义，按 Win10 1809+ SDK 补上
#ifndef StorageAdapterTemperatureProperty
#define StorageAdapterTemperatureProperty ((STORAGE_PROPERTY_ID)51)
#endif
#ifndef StorageDeviceTemperatureProperty
#define StorageDeviceTemperatureProperty ((STORAGE_PROPERTY_ID)52)
#endif

// 实测布局（KIOXIA NVMe, Win11 26300）：
//   header 24 字节，之后每个温度项 16 字节
//   item: Index(2) Temperature(2) OverTemperature(2) Threshold(2) Max(4) Min(4)
#pragma pack(push, 1)
struct TempInfoRaw {
    USHORT Index;
    SHORT  Temperature;        // 摄氏度
    SHORT  OverTemperature;
    USHORT Threshold;
    ULONG  MaxTemperature;
    ULONG  MinTemperature;
};
struct TempDescRaw {
    ULONG  Version;
    ULONG  Size;
    USHORT DeprecatedTemperature;
    UCHAR  CriticalTemperature;
    UCHAR  WarningTemperature;
    USHORT InfoCount;
    UCHAR  Reserved0[2];
    ULONG  Reserved1[2];       // 8 字节 -> header 共 24 字节
    TempInfoRaw Info[1];
};
#pragma pack(pop)

static_assert(sizeof(TempInfoRaw) == 16, "TempInfoRaw must be 16 bytes");

struct DiskProbe {
    std::wstring model;
    int          busType = -1;
    double       tempC   = -1.0;
    int          sensors = 0;
};

static DiskProbe ProbeDisk(int index) {
    DiskProbe out;
    std::wstring dev = Fmt(L"\\\\.\\PhysicalDrive%d", index);

    // 只申请"查询属性"权限；GENERIC_READ 会被拒绝（错误 5）
    HANDLE h = CreateFileW(dev.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;

    STORAGE_PROPERTY_QUERY q{};
    q.QueryType = PropertyStandardQuery;
    DWORD ret = 0;

    // --- 型号 / 总线 / 容量 ---
    {
        q.PropertyId = StorageDeviceProperty;
        std::vector<BYTE> buf(4096);
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                            buf.data(), (DWORD)buf.size(), &ret, nullptr) &&
            ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)buf.data();
            std::wstring vendor, product;
            if (d->VendorIdOffset)
                vendor = TrimW(AnsiToWide((const char*)buf.data() + d->VendorIdOffset));
            if (d->ProductIdOffset)
                product = TrimW(AnsiToWide((const char*)buf.data() + d->ProductIdOffset));
            out.model = TrimW(vendor + L" " + product);
            out.busType = (int)d->BusType;
        }
    }

    // --- 温度 ---
    for (STORAGE_PROPERTY_ID pid : { StorageDeviceTemperatureProperty,
                                     StorageAdapterTemperatureProperty }) {
        q.PropertyId = pid;
        std::vector<BYTE> tb(512, 0);
        DWORD r2 = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                            tb.data(), (DWORD)tb.size(), &r2, nullptr) && r2 >= 24) {
            TempDescRaw* td = (TempDescRaw*)tb.data();
            int n = td->InfoCount;
            if (n < 1) n = 1;
            if (n > 8) n = 8;
            int fit = ((int)r2 - 24) / (int)sizeof(TempInfoRaw);
            if (n > fit) n = fit;
            double best = -1.0;
            for (int i = 0; i < n; ++i) {
                short t = td->Info[i].Temperature;
                if (t > 0 && t < 125 && t > best) best = t;
            }
            if (best > 0) {
                out.tempC   = best;
                out.sensors = n;
                break;
            }
        }
    }

    CloseHandle(h);
    return out;
}

// ============================== CPU 使用率 / 内存 ==============================

static ULONGLONG g_prevIdle = 0, g_prevKernel = 0, g_prevUser = 0;
static bool g_cpuFirst = true;

static double CpuUsageRead() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return -1.0;

    ULONGLONG i = ((ULONGLONG)idle.dwHighDateTime   << 32) | idle.dwLowDateTime;
    ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)user.dwHighDateTime   << 32) | user.dwLowDateTime;

    ULONGLONG di = i - g_prevIdle, dk = k - g_prevKernel, du = u - g_prevUser;
    g_prevIdle = i; g_prevKernel = k; g_prevUser = u;

    if (g_cpuFirst) { g_cpuFirst = false; return -1.0; }

    ULONGLONG total = dk + du;      // kernel 已含 idle
    if (total == 0) return -1.0;
    double pct = (double)(total - di) * 100.0 / (double)total;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static void RamRead(double& usedGB, double& totalGB, double& pct) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;
    totalGB = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    usedGB  = (double)(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    pct     = (double)ms.dwMemoryLoad;
}

// ============================== 网速 (GetIfTable2) ==============================

static ULONGLONG g_prevRx = 0, g_prevTx = 0, g_prevNetTick = 0;
static bool      g_netFirst = true;

// 汇总所有"物理且已连接"网卡的累计字节数（排除虚拟/回环/VPN 适配器）
static bool NetRaw(ULONGLONG& rx, ULONGLONG& tx) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return false;

    rx = 0; tx = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& r = table->Table[i];
        if (r.Type != IF_TYPE_ETHERNET_CSMACD && r.Type != IF_TYPE_IEEE80211) continue;
        if (r.OperStatus != IfOperStatusUp) continue;
        std::wstring desc = r.Description;
        if (desc.find(L"Virtual")  != std::wstring::npos) continue;
        if (desc.find(L"VPN")      != std::wstring::npos) continue;
        if (desc.find(L"TAP")      != std::wstring::npos) continue;
        if (desc.find(L"Loopback") != std::wstring::npos) continue;
        rx += r.InOctets;
        tx += r.OutOctets;
    }
    FreeMibTable(table);
    return true;
}

static void NetRead(double& rxBps, double& txBps) {
    ULONGLONG rx = 0, tx = 0;
    if (!NetRaw(rx, tx)) return;

    ULONGLONG now = GetTickCount64();
    if (g_netFirst) {                       // 第一次只建立基线
        g_netFirst = false;
        g_prevRx = rx; g_prevTx = tx; g_prevNetTick = now;
        return;
    }
    double dt = (double)(now - g_prevNetTick) / 1000.0;
    if (dt < 0.2) return;                   // 间隔太短，沿用上次结果

    // 网卡重连会让计数器归零，此时按 0 处理而不是回绕成天文数字
    double drx = (rx >= g_prevRx) ? (double)(rx - g_prevRx) : 0.0;
    double dtx = (tx >= g_prevTx) ? (double)(tx - g_prevTx) : 0.0;
    g_prevRx = rx; g_prevTx = tx; g_prevNetTick = now;

    rxBps = drx / dt;
    txBps = dtx / dt;
}

// 速率拆成"数值 + 单位"两部分，便于卡片里大字排版
static void FmtRate(double bps, std::wstring& val, std::wstring& unit) {
    const double K = 1024.0;
    if (bps < 0)            { val = L"--";  unit.clear(); return; }
    if (bps < K)            { val = Fmt(L"%.0f", bps);          unit = L"B/s";  return; }
    if (bps < K * K)        { val = Fmt(L"%.1f", bps / K);      unit = L"KB/s"; return; }
    if (bps < K * K * K)    { val = Fmt(L"%.2f", bps / (K * K)); unit = L"MB/s"; return; }
    val = Fmt(L"%.2f", bps / (K * K * K)); unit = L"GB/s";
}

// ============================== 显卡显存 (DXGI) ==============================

// MinGW 的 g++ 不支持 MSVC 的 __uuidof，这里直接写字面值
// {7b7166ec-21c7-44ae-b21a-c9ae321ae369} = IID_IDXGIFactory
static const GUID kIID_IDXGIFactory  = {
    0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 }
};
// {770aae78-f26f-4dba-a829-253c83d1b387} = IID_IDXGIFactory1
static const GUID kIID_IDXGIFactory1 = {
    0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 }
};

static void GpuCollect() {
    g_gpuList.clear();
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(kIID_IDXGIFactory1, (void**)&factory)) || !factory) return;

    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) != S_OK || !ad) break;
        // 必须用 DESC1 —— 只有它带 Flags 字段
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d))) {
            // 跳过软件渲染适配器（Microsoft Basic Render Driver 之类）
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                GpuInfo g;
                g.name             = TrimW(d.Description);
                g.dedicatedVideoMB = d.DedicatedVideoMemory  / (1024ULL * 1024ULL);
                g.dedicatedSysMB   = d.DedicatedSystemMemory / (1024ULL * 1024ULL);
                g.sharedSysMB      = d.SharedSystemMemory    / (1024ULL * 1024ULL);
                // 同名去重（EnumAdapters 可能对同一物理卡返回多次）
                bool dup = false;
                for (auto& e : g_gpuList) if (e.name == g.name) { dup = true; break; }
                if (!dup) g_gpuList.push_back(g);
            }
        }
        ad->Release();
    }
    factory->Release();
}

// ============================== SMBIOS ==============================

struct Smbios {
    std::vector<BYTE> raw;
    BYTE major = 0, minor = 0;
    const BYTE* table = nullptr;
    DWORD tableLen = 0;
};

static bool SmbiosLoad(Smbios& s) {
    // 'R''S''M''B' —— 用字面常量，避免多字符常量警告
    const DWORD RSMB_SIG = 0x52534D42;
    DWORD size = GetSystemFirmwareTable(RSMB_SIG, 0, nullptr, 0);
    if (size == 0 || size > 4 * 1024 * 1024) return false;
    s.raw.resize(size);
    if (GetSystemFirmwareTable(RSMB_SIG, 0, s.raw.data(), size) != size) return false;

    s.major = s.raw[1];
    s.minor = s.raw[2];
    DWORD len = *(DWORD*)(s.raw.data() + 4);
    if (8 + len > size) len = size - 8;
    s.table    = s.raw.data() + 8;
    s.tableLen = len;
    return true;
}

static std::wstring SmbiosString(const BYTE* strArea, const BYTE* tableEnd, int index) {
    if (index <= 0) return L"";
    const BYTE* p = strArea;
    for (int i = 1; i < index; ++i) {
        while (p < tableEnd && *p) ++p;
        if (p >= tableEnd) return L"";
        ++p;
    }
    if (p >= tableEnd || *p == 0) return L"";
    return TrimW(AnsiToWide((const char*)p));
}

// SMBIOS Type 17 Memory Device — Memory Type 编码（规范 3.7）
static const wchar_t* MemTypeName(BYTE t) {
    switch (t) {
        case 0x1A: return L"DDR4";
        case 0x1B: return L"LPDDR";
        case 0x1C: return L"LPDDR2";
        case 0x1D: return L"LPDDR3";
        case 0x1E: return L"LPDDR4";
        case 0x20: return L"HBM";
        case 0x21: return L"HBM2";
        case 0x22: return L"DDR5";
        case 0x23: return L"LPDDR5";     // 本机实测值
        case 0x24: return L"HBM3";
        default:   return L"";
    }
}

struct MemDeviceInfo {
    // locator = Device Locator（Type 17 偏移 0x10）
    // bank    = Bank Locator（偏移 0x11）。有些 OEM（本机 HONOR 就是）把 Device Locator
    //           全部填成 "DIMM 0"，四条内存区分不开，只有 Bank Locator 才是
    //           "P0 CHANNEL A/B/C/D"。所以两个都读，显示时按需拼接。
    std::wstring locator, bank, manufacturer, part, typeName;
    unsigned long long sizeMB = 0;
    int speed = 0;
};

struct SmbiosFacts {
    std::wstring sysMfr, sysProduct, bbMfr, bbProduct;
    std::wstring biosVendor, biosVer, biosDate;
    std::wstring cpuVersion;
    int cpuCores = 0, cpuThreads = 0, cpuMaxMHz = 0;
    std::vector<MemDeviceInfo> dimms;
};

static std::vector<MemDeviceInfo> g_dimms;   // 供"复制为文本"输出每条内存详情

static void SmbiosParse(Smbios& s, SmbiosFacts& f) {
    const BYTE* p   = s.table;
    const BYTE* end = s.table + s.tableLen;

    while (p + 4 <= end) {
        BYTE type = p[0];
        BYTE len  = p[1];
        if (len < 4) break;
        if (type == 127) break;
        if (p + len > end) break;

        const BYTE* strArea = p + len;
        const BYTE* q = strArea;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        const BYTE* next = q + 2;

        auto S = [&](int idx) { return SmbiosString(strArea, next, idx); };

        switch (type) {
        case 0:
            if (len > 0x08) {
                f.biosVendor = S(p[0x04]);
                f.biosVer    = S(p[0x05]);
                f.biosDate   = S(p[0x08]);
            }
            break;
        case 1:
            if (len > 0x06) { f.sysMfr = S(p[0x04]); f.sysProduct = S(p[0x05]); }
            break;
        case 2:
            if (len > 0x06) { f.bbMfr = S(p[0x04]); f.bbProduct = S(p[0x05]); }
            break;
        case 4: {
            if (len > 0x08) {
                f.cpuVersion = S(p[0x08]);
                f.cpuMaxMHz  = (len > 0x16) ? *(WORD*)(p + 0x14) : 0;
                // 0x2A = Core Count 2 (WORD)，0x2E = Thread Count 2 (WORD)
                if (len >= 0x30) {
                    WORD c2 = *(WORD*)(p + 0x2A);
                    WORD t2 = *(WORD*)(p + 0x2E);
                    f.cpuCores   = c2 ? c2 : (len > 0x23 ? p[0x23] : 0);
                    f.cpuThreads = t2 ? t2 : (len > 0x25 ? p[0x25] : 0);
                } else if (len > 0x25) {
                    f.cpuCores   = p[0x23];
                    f.cpuThreads = p[0x25];
                }
            }
            break;
        }
        case 17: {
            if (len > 0x1C) {
                WORD sz = *(WORD*)(p + 0x0C);
                if (sz != 0 && sz != 0xFFFF) {
                    MemDeviceInfo d;
                    d.sizeMB = (sz & 0x8000) ? ((unsigned long long)(sz & 0x7FFF) / 1024ULL) : sz;
                    d.locator      = S(p[0x10]);   // Device Locator
                    d.bank         = S(p[0x11]);   // Bank Locator
                    BYTE mt        = p[0x12];
                    d.typeName     = MemTypeName(mt);
                    d.speed        = (len > 0x16) ? *(WORD*)(p + 0x15) : 0;
                    d.manufacturer = S(p[0x17]);
                    d.part         = S(p[0x1A]);   // Part Number 在 0x1A
                    f.dimms.push_back(d);
                }
            }
            break;
        }
        default: break;
        }

        p = next;
        if (p >= end) break;
    }
}

// ============================== 静态信息采集 ==============================

static void CollectStatic() {
    g_info.clear();

    wchar_t cpuNameBuf[256] = { 0 };
    DWORD cb = sizeof(cpuNameBuf), type = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, &type,
                     cpuNameBuf, &cb) == ERROR_SUCCESS) {
        g_cpuName = TrimW(cpuNameBuf);
    }

    Smbios sm;
    SmbiosFacts f;
    bool haveSmbios = SmbiosLoad(sm);
    if (haveSmbios) {
        SmbiosParse(sm, f);
        g_dimms = f.dimms;
    }

    // --- 处理器 ---
    std::wstring cpu = g_cpuName.empty() ? f.cpuVersion : g_cpuName;
    if (!cpu.empty()) g_info.push_back({ L"处理器", cpu });

    // --- 核心 / 线程 ---
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    int logical = (int)si.dwNumberOfProcessors;
    if (f.cpuCores > 0 && f.cpuThreads > 0) {
        g_info.push_back({ L"核心 / 线程", Fmt(L"%d / %d", f.cpuCores, f.cpuThreads) });
    } else if (logical > 0) {
        g_info.push_back({ L"逻辑处理器", Fmt(L"%d", logical) });
    }

    // --- 最高频率 ---
    if (f.cpuMaxMHz > 0) {
        g_info.push_back({ L"最高频率", Fmt(L"%.2f GHz", f.cpuMaxMHz / 1000.0) });
    }

    // --- 主机 / 主板 ---
    if (!f.sysProduct.empty()) {
        g_info.push_back({ L"主机型号", TrimW(f.sysMfr + L" " + f.sysProduct) });
    }
    if (!f.bbProduct.empty()) {
        g_info.push_back({ L"主板", TrimW(f.bbMfr + L" " + f.bbProduct) });
    }

    // --- BIOS ---
    if (!f.biosVer.empty()) {
        std::wstring v = TrimW(f.biosVendor + L" " + f.biosVer);
        if (!f.biosDate.empty()) v += L"  (" + f.biosDate + L")";
        g_info.push_back({ L"BIOS", v });
    }

    // --- 内存 ---
    double usedGB = 0, totalGB = 0, pct = 0;
    RamRead(usedGB, totalGB, pct);

    unsigned long long dimmTotalMB = 0;
    for (auto& d : f.dimms) dimmTotalMB += d.sizeMB;
    (void)dimmTotalMB;

    std::wstring memVal;
    if (totalGB > 0) memVal = Fmt(L"%.1f GB", totalGB);
    if (!f.dimms.empty()) {
        std::wstring s = Fmt(L"%d × %.0f GB", (int)f.dimms.size(), f.dimms[0].sizeMB / 1024.0);
        if (!f.dimms[0].typeName.empty()) s += L" " + f.dimms[0].typeName;
        if (f.dimms[0].speed > 0) s += Fmt(L"-%d", f.dimms[0].speed);
        if (!memVal.empty()) memVal += L"  ·  ";
        memVal += s;
    }
    if (!memVal.empty()) g_info.push_back({ L"内存", memVal });

    if (!f.dimms.empty() && (!f.dimms[0].manufacturer.empty() || !f.dimms[0].part.empty())) {
        g_info.push_back({ L"内存颗粒", TrimW(f.dimms[0].manufacturer + L"  " + f.dimms[0].part) });
    }

    // --- 显卡 / 显存（DXGI；比 WMI 的 AdapterRAM 准，后者 32 位会截断） ---
    GpuCollect();
    if (!g_gpuList.empty()) {
        std::wstring names;
        for (size_t i = 0; i < g_gpuList.size(); ++i) {
            if (i) names += L"  /  ";
            names += g_gpuList[i].name;
        }
        g_info.push_back({ L"显示适配器", names });

        const GpuInfo& g0 = g_gpuList[0];
        std::wstring vm = Fmt(L"%.0f MB", (double)g0.dedicatedVideoMB);
        if (g0.sharedSysMB > 0) {
            vm += Fmt(L"  ·  共享 %.1f GB", g0.sharedSysMB / 1024.0);
        }
        g_info.push_back({ L"显存", vm });
    }

    // --- 存储设备（从 0 开始逐个探测型号，温度由定时器刷新） ---
    {
        std::wstring models;
        for (int i = 0; i < 4; ++i) {
            DiskProbe d = ProbeDisk(i);
            if (d.busType < 0 && d.model.empty()) continue;
            if (!models.empty()) models += L"  /  ";
            models += d.model.empty() ? Fmt(L"PhysicalDrive%d", i) : d.model;
        }
        if (!models.empty()) g_info.push_back({ L"存储设备", models });
    }

    // --- 操作系统 ---
    {
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        typedef LONG (WINAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
        HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
        PFN_RtlGetVersion pRtl = hNt ? (PFN_RtlGetVersion)GetProcAddress(hNt, "RtlGetVersion") : nullptr;
        if (pRtl && pRtl(&vi) == 0) {
            std::wstring name;
            if (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000) name = L"Windows 11";
            else if (vi.dwMajorVersion == 10)                     name = L"Windows 10";
            else                                                  name = Fmt(L"Windows %lu", vi.dwMajorVersion);

            std::wstring disp, editionId;
            HKEY k;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                              0, KEY_READ, &k) == ERROR_SUCCESS) {
                wchar_t b[128] = { 0 };
                DWORD bsz = sizeof(b);
                if (RegQueryValueExW(k, L"DisplayVersion", nullptr, nullptr, (LPBYTE)b, &bsz) == ERROR_SUCCESS)
                    disp = b;
                bsz = sizeof(b);
                ZeroMemory(b, sizeof(b));
                if (RegQueryValueExW(k, L"EditionID", nullptr, nullptr, (LPBYTE)b, &bsz) == ERROR_SUCCESS)
                    editionId = b;
                RegCloseKey(k);
            }
            std::wstring v = name;
            if (!disp.empty()) v += L" " + disp;
            v += Fmt(L"  (build %lu)", vi.dwBuildNumber);
            g_info.push_back({ L"操作系统", v });
        }
    }

    // --- SMBIOS 版本 ---
    if (haveSmbios && sm.major) {
        g_info.push_back({ L"SMBIOS 版本", Fmt(L"%d.%d", sm.major, sm.minor) });
    }
}

// ============================== 复制为文本 ==============================

// 显示宽度（全角算 2、半角算 1），用于把标签对齐
static int DispWidth(const std::wstring& s) {
    int w = 0;
    for (wchar_t c : s) {
        if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
            (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
            (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
            (c >= 0xFFE0 && c <= 0xFFE6))
            w += 2;
        else
            w += 1;
    }
    return w;
}

static std::wstring PadTo(const std::wstring& s, int width) {
    std::wstring out = s;
    for (int i = DispWidth(s); i < width; ++i) out += L' ';
    return out;
}

// 刻意避开 %llu —— MinGW 走的是 msvcrt 的 _vsnwprintf，老版本不认 C99 的 %ll
static std::wstring FmtMB(unsigned long long mb) {
    if (mb == 0) return L"0 MB";
    if (mb >= 1024) return Fmt(L"%.1f GB (%u MB)", (double)mb / 1024.0, (unsigned)mb);
    return Fmt(L"%u MB", (unsigned)mb);
}

static std::wstring NowString() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Fmt(L"%04d-%02d-%02d %02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static std::wstring BuildInfoText() {
    std::wstring t;
    auto line = [&](const std::wstring& label, const std::wstring& value) {
        t += PadTo(label, 16) + L": " + value + L"\r\n";
    };

    t += std::wstring(L"===== 硬件信息  ") + APP_TITLE + L" " + APP_VERSION + L" =====\r\n";
    for (auto& r : g_info) line(r.label, r.value);
    line(L"记录时间", NowString());

    // ---- 内存 ----
    t += L"\r\n===== 内存 =====\r\n";
    {
        double used = 0, total = 0, pct = 0;
        RamRead(used, total, pct);
        line(L"操作系统可见", Fmt(L"%.1f GB", total));
        if (!g_dimms.empty()) {
            unsigned long long sumMB = 0;
            for (auto& d : g_dimms) sumMB += d.sizeMB;
            line(L"插槽数", Fmt(L"%d 条", (int)g_dimms.size()));
            line(L"物理合计", Fmt(L"%.0f GB", sumMB / 1024.0));
            line(L"内存类型", g_dimms[0].typeName.empty() ? L"未知" : g_dimms[0].typeName);
            line(L"运行速率", g_dimms[0].speed > 0 ? Fmt(L"%d MT/s", g_dimms[0].speed) : L"未知");
            line(L"制造商", g_dimms[0].manufacturer.empty() ? L"未知" : g_dimms[0].manufacturer);
            line(L"型号 P/N", g_dimms[0].part.empty() ? L"未知" : g_dimms[0].part);
            // Device Locator 可能重复（本机四条全是 "DIMM 0"），
            // 此时 Bank Locator（P0 CHANNEL A/B/C/D）才是真正区分插槽的字段。
            bool dupLocator = false;
            for (size_t i = 1; i < g_dimms.size(); ++i)
                if (g_dimms[i].locator == g_dimms[0].locator) { dupLocator = true; break; }

            for (size_t i = 0; i < g_dimms.size(); ++i) {
                const MemDeviceInfo& d = g_dimms[i];
                std::wstring slot = d.locator;
                if (dupLocator && !d.bank.empty()) {
                    if (slot.empty()) slot = d.bank;
                    else              slot += L" · " + d.bank;
                }
                line(Fmt(L"第 %d 条", (int)i + 1),
                     Fmt(L"%.0f GB   插槽 %s", d.sizeMB / 1024.0,
                         slot.empty() ? L"(未标注)" : slot.c_str()));
            }
        }
        line(L"当前占用", Fmt(L"%.1f GB / %.1f GB  (%.0f%%)", used, total, pct));
    }

    // ---- 显卡 ----
    t += L"\r\n===== 显卡 =====\r\n";
    if (g_gpuList.empty()) {
        line(L"显示适配器", L"未取到");
    } else {
        for (size_t i = 0; i < g_gpuList.size(); ++i) {
            const GpuInfo& g = g_gpuList[i];
            std::wstring tag = (g_gpuList.size() > 1) ? Fmt(L"GPU %d", (int)i + 1) : L"显示适配器";
            line(tag, g.name);
            line(L"专用显存", FmtMB(g.dedicatedVideoMB));
            if (g.dedicatedSysMB > 0) line(L"专用系统内存", FmtMB(g.dedicatedSysMB));
            if (g.sharedSysMB > 0)    line(L"共享系统内存", FmtMB(g.sharedSysMB));
            line(L"可寻址显存合计", Fmt(L"%.1f GB",
                 (double)(g.dedicatedVideoMB + g.sharedSysMB) / 1024.0));
        }
    }

    // ---- 实时状态 ----
    t += L"\r\n===== 实时状态（复制时刻）=====\r\n";
    line(L"CPU 温度",  g_m.cpuTempC >= 0 ? Fmt(L"%.1f °C", g_m.cpuTempC) : L"读不到");
    line(L"SSD 温度",  g_m.ssdTempC >= 0 ? Fmt(L"%.1f °C", g_m.ssdTempC) : L"读不到");
    line(L"CPU 占用",  g_m.cpuUsage >= 0 ? Fmt(L"%.0f %%", g_m.cpuUsage) : L"未知");
    line(L"内存占用",  Fmt(L"%.1f / %.1f GB  (%.0f%%)", g_m.ramUsedGB, g_m.ramTotalGB, g_m.ramPct));
    {
        std::wstring a, b, c, d;
        FmtRate(g_m.netRxBps, a, b);
        FmtRate(g_m.netTxBps, c, d);
        line(L"网络", Fmt(L"↓ %s %s    ↑ %s %s", a.c_str(), b.c_str(), c.c_str(), d.c_str()));
    }
    return t;
}

static void CopyToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
        }
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// ============================== 绘制 ==============================

// 可折叠区块状态（默认折叠，点标题条展开）
static bool g_showChart = false;
static bool g_showInfo  = false;
static bool g_topmost   = false;

// 可点击区域（每帧重绘时刷新）
static RECT g_rcBtnTopmost{};
static RECT g_rcBtnCopy{};
static RECT g_rcHdrChart{};
static RECT g_rcHdrInfo{};

static const int SUB_GAP = 6;   // 折叠标题条与内容卡片之间的间隙

static int ChartBlockH() {
    return g_showChart ? PX(HEAD_H) + PX(SUB_GAP) + PX(CHART_H) : PX(HEAD_H);
}
// 注意：这里的 INFO_TOP 必须与 DrawInfo() 里第一行的起始内边距（r.top + PX(INFO_TOP)）一致。
// 之前漏了它，导致区块高度比实际需要矮 10 逻辑像素，最后一行（SMBIOS 版本）永远被裁掉。
static int InfoBlockH() {
    int rows = (int)g_info.size();
    return g_showInfo ? PX(HEAD_H) + PX(SUB_GAP) + PX(INFO_TOP)
                        + rows * PX(INFO_ROW) + PX(INFO_PAD)
                      : PX(HEAD_H);
}

static int YTool()   { return PX(MARGIN); }
static int YCards()  { return YTool() + PX(TOOL_H) + PX(GAP); }
static int YChart()  { return YCards() + PX(CARD_H) * 2 + PX(GAP) + PX(GAP); }
static int YInfo()   { return YChart() + ChartBlockH() + PX(GAP); }
static int ClientH() { return YInfo() + InfoBlockH() + PX(MARGIN); }

static HFONT g_fLabel = nullptr, g_fValue = nullptr, g_fBig = nullptr,
             g_fUnit = nullptr, g_fSmall = nullptr, g_fInfo = nullptr;

// "复制"按钮状态（点击或 Ctrl+C 把硬件信息以纯文本写入剪贴板）

static bool    g_copyFlash = false;
static DWORD   g_copyFlashAt = 0;

static void CreateFonts() {
    
    
    auto mk = [&](int px, int weight = FW_NORMAL) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    g_fLabel = mk(PX(13));
    g_fValue = mk(PX(15), FW_MEDIUM);
    g_fBig   = mk(PX(30), FW_SEMIBOLD);
    g_fUnit  = mk(PX(13));
    g_fSmall = mk(PX(12));
    g_fInfo  = mk(PX(13));
}

static void DestroyFonts() {
    HFONT all[] = { g_fLabel, g_fValue, g_fBig, g_fUnit, g_fSmall, g_fInfo };
    for (auto f : all) if (f) DeleteObject(f);
    g_fLabel = g_fValue = g_fBig = g_fUnit = g_fSmall = g_fInfo = nullptr;
}

static void Txt(HDC dc, int x, int y, const std::wstring& s, COLORREF c, HFONT f,
                UINT align = TA_LEFT) {
    if (!f || s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    SelectObject(dc, old);
}

// 超宽时自动省略号收尾，避免长值（如多张显卡的型号串）溢出到卡片外
static void TxtClip(HDC dc, int x, int y, int maxW, const std::wstring& s,
                    COLORREF c, HFONT f) {
    if (!f || s.empty() || maxW <= 0) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT r{ x, y - 2, x + maxW, y + 40 };
    DrawTextW(dc, s.c_str(), (int)s.size(), &r,
              DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void FillRound(HDC dc, RECT r, int rad, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN   p = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void StrokeRound(HDC dc, RECT r, int rad, COLORREF stroke) {
    HBRUSH b = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN   p = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(p);
}

static void DrawCard(HDC dc, RECT r, const wchar_t* label, const std::wstring& value,
                     const wchar_t* unit, double ratio, COLORREF accent) {
    
    

    FillRound(dc, r, PX(10), g_pal.card);
    StrokeRound(dc, r, PX(10), g_pal.border);

    int padX = PX(12);

    // 标题
    Txt(dc, r.left + padX, r.top + PX(9), label, g_pal.text2, g_fLabel);

    // 大数值
    int bigTop = r.top + PX(26);
    HGDIOBJ old = SelectObject(dc, g_fBig);
    SetTextColor(dc, accent);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    SIZE sz{};
    GetTextExtentPoint32W(dc, value.c_str(), (int)value.size(), &sz);
    TextOutW(dc, r.left + padX, bigTop, value.c_str(), (int)value.size());
    SelectObject(dc, old);

    // 单位：与大数值底部对齐
    if (unit && unit[0]) {
        int uy = bigTop + sz.cy - PX(17);
        Txt(dc, r.left + padX + sz.cx + PX(3), uy, unit, g_pal.text2, g_fUnit);
    }

    // 进度条
    if (ratio >= 0) {
        int by = r.bottom - PX(12);
        RECT tr{ r.left + padX, by, r.right - padX, by + PX(5) };
        FillRound(dc, tr, PX(3), g_pal.track);
        double rr = ratio;
        if (rr < 0) rr = 0;
        if (rr > 1) rr = 1;
        int w = (int)((tr.right - tr.left) * rr);
        if (w > 2) {
            RECT fr{ tr.left, tr.top, tr.left + w, tr.bottom };
            FillRound(dc, fr, PX(3), accent);
        }
    }
}

// 矩形内居中绘制单行文字
static void TxtCenter(HDC dc, const RECT& r, const std::wstring& s, COLORREF c, HFONT f) {
    if (!f || s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT rr = r;
    DrawTextW(dc, s.c_str(), (int)s.size(), &rr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void DrawButton(HDC dc, const RECT& r, const std::wstring& text, bool active) {
    FillRound(dc, r, PX(8), active ? g_pal.blue : g_pal.track);
    StrokeRound(dc, r, PX(8), active ? g_pal.blue : g_pal.border);
    TxtCenter(dc, r, text, active ? RGB(0xFF, 0xFF, 0xFF) : g_pal.text, g_fSmall);
}

// 顶部工具栏：左侧置顶开关，右侧复制按钮
static void DrawToolbar(HDC dc, RECT r) {
    FillRound(dc, r, PX(10), g_pal.card);
    StrokeRound(dc, r, PX(10), g_pal.border);

    int pad = PX(6);
    int top = r.top + pad, bot = r.bottom - pad;

    g_rcBtnTopmost = RECT{ r.left + pad, top, r.left + pad + PX(72), bot };
    DrawButton(dc, g_rcBtnTopmost, g_topmost ? L"已置顶" : L"置顶", g_topmost);

    bool flash = g_copyFlash && (GetTickCount() - g_copyFlashAt < 1200);
    g_rcBtnCopy = RECT{ r.right - pad - PX(84), top, r.right - pad, bot };
    if (flash) {
        FillRound(dc, g_rcBtnCopy, PX(8), g_pal.green);
        StrokeRound(dc, g_rcBtnCopy, PX(8), g_pal.green);
        TxtCenter(dc, g_rcBtnCopy, L"已复制", RGB(0xFF, 0xFF, 0xFF), g_fSmall);
    } else {
        DrawButton(dc, g_rcBtnCopy, L"复制信息", false);
    }

    Txt(dc, g_rcBtnTopmost.right + PX(12), (r.top + r.bottom) / 2 - PX(8),
        L"点击下方标题条展开 / 收起", g_pal.text2, g_fSmall);
}

// 可折叠区块的标题条：左侧三角指示 + 标题 + 右侧提示
static void DrawSectionHeader(HDC dc, RECT r, const wchar_t* title, bool expanded,
                              const std::wstring& hint) {
    FillRound(dc, r, PX(10), g_pal.card);
    StrokeRound(dc, r, PX(10), g_pal.border);

    int cy = (r.top + r.bottom) / 2;
    int ax = r.left + PX(14);
    int a  = PX(11);

    POINT pts[3];
    if (expanded) {                                  // ▼
        pts[0] = { ax,         cy - a / 2 };
        pts[1] = { ax + a,     cy - a / 2 };
        pts[2] = { ax + a / 2, cy + a / 2 };
    } else {                                         // ▶
        pts[0] = { ax,         cy - a / 2 };
        pts[1] = { ax + a,     cy };
        pts[2] = { ax,         cy + a / 2 };
    }
    HBRUSH b = CreateSolidBrush(g_pal.text2);
    HPEN   p = CreatePen(PS_SOLID, 1, g_pal.text2);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);

    Txt(dc, ax + a + PX(10), cy - PX(9), title, g_pal.text, g_fValue);

    if (!hint.empty())
        Txt(dc, r.right - PX(14), cy - PX(8), hint, g_pal.text2, g_fSmall, TA_RIGHT);
}

static void DrawChart(HDC dc, RECT r, RECT& hdrOut) {
    hdrOut = RECT{ r.left, r.top, r.right, r.top + PX(HEAD_H) };
    DrawSectionHeader(dc, hdrOut, L"温度曲线", g_showChart,
                      g_showChart ? L"" : L"最近 2 分钟");
    if (!g_showChart) return;
    r.top = hdrOut.bottom + PX(SUB_GAP);
    
    

    FillRound(dc, r, PX(10), g_pal.card);
    StrokeRound(dc, r, PX(10), g_pal.border);

    Txt(dc, r.left + PX(12), r.top + PX(8), L"温度曲线（最近 2 分钟）", g_pal.text2, g_fLabel);

    // 图例（右上）
    {
        int lx = r.right - PX(12) - PX(140);
        int ly = r.top + PX(9);
        RECT d1{ lx, ly + PX(3), lx + PX(8), ly + PX(11) };
        FillRound(dc, d1, PX(2), g_pal.red);
        Txt(dc, lx + PX(13), ly, L"CPU", g_pal.text2, g_fSmall);
        int lx2 = lx + PX(52);
        RECT d2{ lx2, ly + PX(3), lx2 + PX(8), ly + PX(11) };
        FillRound(dc, d2, PX(2), g_pal.blue);
        Txt(dc, lx2 + PX(13), ly, L"SSD", g_pal.text2, g_fSmall);
    }

    int padL = PX(40), padR = PX(12), padT = PX(28), padB = PX(14);
    RECT plot{ r.left + padL, r.top + padT, r.right - padR, r.bottom - padB };
    int pw = plot.right - plot.left;
    int ph = plot.bottom - plot.top;
    if (pw <= 0 || ph <= 0) return;

    HPEN grid = CreatePen(PS_SOLID, 1, g_pal.track);
    HGDIOBJ op = SelectObject(dc, grid);
    for (int t = 40; t <= 100; t += 20) {
        int y = plot.bottom - (int)((t - TEMP_MIN) / (TEMP_MAX - TEMP_MIN) * ph);
        if (y < plot.top || y > plot.bottom) continue;
        MoveToEx(dc, plot.left, y, nullptr);
        LineTo(dc, plot.right, y);
        Txt(dc, r.left + PX(8), y - PX(7), Fmt(L"%d°", t), g_pal.text2, g_fSmall);
    }
    SelectObject(dc, op);
    DeleteObject(grid);

    int n = std::min(g_histCount, HIST_SHOW);
    if (n >= 2) {
        auto X = [&](int i) { return plot.left + (int)((double)i / (double)(n - 1) * pw); };
        auto Y = [&](double t) {
            double v = (t - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
            v = std::max(0.0, std::min(1.0, v));
            return plot.bottom - (int)(v * ph);
        };
        auto series = [&](const double* hist, COLORREF color) {
            HPEN pen = CreatePen(PS_SOLID, 2, color);
            HGDIOBJ o = SelectObject(dc, pen);
            bool started = false;
            for (int i = 0; i < n; ++i) {
                int idx = ((g_histHead - n + i) % HIST_MAX + HIST_MAX) % HIST_MAX;
                double v = hist[idx];
                if (v < 0) { started = false; continue; }
                int x = X(i), y = Y(v);
                if (!started) { MoveToEx(dc, x, y, nullptr); started = true; }
                else LineTo(dc, x, y);
            }
            SelectObject(dc, o);
            DeleteObject(pen);
        };
        series(g_histSsd, g_pal.blue);
        series(g_histCpu, g_pal.red);

        auto dot = [&](const double* hist, COLORREF color) {
            int idx = ((g_histHead - 1) % HIST_MAX + HIST_MAX) % HIST_MAX;
            double v = hist[idx];
            if (v < 0) return;
            HBRUSH b = CreateSolidBrush(color);
            HPEN   p = CreatePen(PS_SOLID, 1, color);
            HGDIOBJ ob = SelectObject(dc, b), opn = SelectObject(dc, p);
            int x = X(n - 1), y = Y(v);
            Ellipse(dc, x - PX(3), y - PX(3), x + PX(3), y + PX(3));
            SelectObject(dc, ob); SelectObject(dc, opn);
            DeleteObject(b); DeleteObject(p);
        };
        dot(g_histSsd, g_pal.blue);
        dot(g_histCpu, g_pal.red);
    } else {
        Txt(dc, plot.left + PX(8), (plot.top + plot.bottom) / 2, L"正在采集…",
            g_pal.text2, g_fInfo);
    }
}

static void DrawInfo(HDC dc, RECT r, RECT& hdrOut) {
    hdrOut = RECT{ r.left, r.top, r.right, r.top + PX(HEAD_H) };
    DrawSectionHeader(dc, hdrOut, L"硬件信息", g_showInfo, L"");
    if (!g_showInfo) return;
    r.top = hdrOut.bottom + PX(SUB_GAP);
    
    

    FillRound(dc, r, PX(10), g_pal.card);
    StrokeRound(dc, r, PX(10), g_pal.border);

    int y = r.top + PX(INFO_TOP);
    int rowH = PX(INFO_ROW);
    int labelW = PX(92);

    for (size_t i = 0; i < g_info.size(); ++i) {
        if (y + rowH > r.bottom - PX(INFO_PAD)) break;
        Txt(dc, r.left + PX(14), y + PX(3), g_info[i].label, g_pal.text2, g_fInfo);
        int vx = r.left + PX(14) + labelW;
        TxtClip(dc, vx, y + PX(3), r.right - PX(14) - vx, g_info[i].value, g_pal.text, g_fInfo);
        y += rowH;
    }
}

// ============================== 窗口 ==============================

static int   g_clientH = 600;

static void Sample() {
    g_m.cpuTempC = CpuTempRead();
    g_m.cpuUsage = CpuUsageRead();
    RamRead(g_m.ramUsedGB, g_m.ramTotalGB, g_m.ramPct);
    NetRead(g_m.netRxBps, g_m.netTxBps);

    g_histCpu[g_histHead] = g_m.cpuTempC;
    g_histSsd[g_histHead] = g_m.ssdTempC;
    g_histHead = (g_histHead + 1) % HIST_MAX;
    if (g_histCount < HIST_MAX) ++g_histCount;
}

// --- 双缓冲位图缓存 ---
// 每秒重绘一次，如果每次都 CreateCompatibleBitmap + DeleteObject，
// 会反复分配/释放整屏大小的 DIB（本机 1161×1394×4B ≈ 6.5 MB），纯属浪费。
// 窗口尺寸固定，缓存一次复用即可。
// 另外把"是否重绘静态信息区"也一并管理：缓存里已有一帧完整内容时，
// 后续只重画上半部分（卡片 + 曲线），信息区直接沿用缓存内容。
static HDC     g_memDC     = nullptr;
static HBITMAP g_memBmp    = nullptr;
static HGDIOBJ g_memOldBmp = nullptr;
static int     g_memW = 0, g_memH = 0;
static bool    g_backBufferFresh = false;

static void ReleaseBackBuffer() {
    if (g_memDC && g_memOldBmp) SelectObject(g_memDC, g_memOldBmp);
    if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = nullptr; }
    if (g_memDC)  { DeleteDC(g_memDC);      g_memDC  = nullptr; }
    g_memOldBmp = nullptr;
    g_memW = g_memH = 0;
    g_backBufferFresh = false;
}

static HDC GetBackBuffer(HDC ref, int w, int h) {
    if (g_memDC && g_memW == w && g_memH == h) return g_memDC;
    ReleaseBackBuffer();
    if (w <= 0 || h <= 0) return ref;          // 尺寸非法时退化为直接画
    g_memDC  = CreateCompatibleDC(ref);
    g_memBmp = CreateCompatibleBitmap(ref, w, h);
    if (!g_memDC || !g_memBmp) { ReleaseBackBuffer(); return ref; }
    g_memOldBmp = SelectObject(g_memDC, g_memBmp);
    g_memW = w;
    g_memH = h;
    return g_memDC;
}

static void PaintAll(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    HDC mem = GetBackBuffer(dc, W, H);

    int infoTop = YInfo();
    bool needInfo = !g_backBufferFresh || ps.rcPaint.bottom > infoTop;

    HBRUSH bg = CreateSolidBrush(g_pal.bg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    int M = PX(MARGIN), GP = PX(GAP);

    // ---------- 顶部工具栏 ----------
    {
        int y = YTool();
        RECT r{ M, y, W - M, y + PX(TOOL_H) };
        DrawToolbar(mem, r);
    }

    // ---------- 六张卡片：3 列 × 2 行，按列分组 ----------
    {
        int cardW = (W - M * 2 - GP * 2) / 3;
        int cardH = PX(CARD_H);
        int y0    = YCards();
        auto cell = [&](int col, int row) {
            int x = M + (cardW + GP) * col;
            int y = y0 + (cardH + GP) * row;
            return RECT{ x, y, x + cardW, y + cardH };
        };

        // —— 左列：两个温度 ——
        {
            bool ok = g_m.cpuTempC >= 0;
            COLORREF c = !ok ? g_pal.text2
                       : (g_m.cpuTempC >= 90 ? g_pal.red
                       : (g_m.cpuTempC >= 75 ? g_pal.amber : g_pal.green));
            DrawCard(mem, cell(0, 0), L"CPU 温度",
                     ok ? Fmt(L"%.1f", g_m.cpuTempC) : L"--",
                     ok ? L"°C" : L"",
                     ok ? (g_m.cpuTempC - 30.0) / 70.0 : -1.0, c);
        }
        {
            bool ok = g_m.ssdTempC >= 0;
            COLORREF c = !ok ? g_pal.text2
                       : (g_m.ssdTempC >= 70 ? g_pal.red
                       : (g_m.ssdTempC >= 55 ? g_pal.amber : g_pal.blue));
            DrawCard(mem, cell(0, 1), L"SSD 温度",
                     ok ? Fmt(L"%.1f", g_m.ssdTempC) : L"--",
                     ok ? L"°C" : L"",
                     ok ? (g_m.ssdTempC - 25.0) / 60.0 : -1.0, c);
        }

        // —— 中列：内存 / CPU 占用 ——
        DrawCard(mem, cell(1, 0), L"内存占用",
                 Fmt(L"%.1f", g_m.ramUsedGB),
                 Fmt(L"/ %.0f GB", g_m.ramTotalGB).c_str(),
                 g_m.ramPct / 100.0, g_pal.green);
        {
            bool ok = g_m.cpuUsage >= 0;
            DrawCard(mem, cell(1, 1), L"CPU 占用",
                     ok ? Fmt(L"%.0f", g_m.cpuUsage) : L"--",
                     ok ? L"%" : L"",
                     ok ? g_m.cpuUsage / 100.0 : -1.0, g_pal.amber);
        }

        // —— 右列：下载 / 上传 ——
        {
            std::wstring v, u;
            FmtRate(g_m.netRxBps, v, u);
            DrawCard(mem, cell(2, 0), L"↓ 下载", v, u.c_str(), -1.0, g_pal.violet);
        }
        {
            std::wstring v, u;
            FmtRate(g_m.netTxBps, v, u);
            DrawCard(mem, cell(2, 1), L"↑ 上传", v, u.c_str(), -1.0, g_pal.teal);
        }
    }

    // ---------- 温度曲线（可折叠） ----------
    {
        int y = YChart();
        RECT r{ M, y, W - M, y + ChartBlockH() };
        DrawChart(mem, r, g_rcHdrChart);
    }

    // ---------- 硬件信息（可折叠；静态内容，缓存里已有一帧就跳过重画） ----------
    if (needInfo) {
        int y = YInfo();
        RECT r{ M, y, W - M, y + InfoBlockH() };
        DrawInfo(mem, r, g_rcHdrInfo);
    }

    RECT blit = g_backBufferFresh ? ps.rcPaint : RECT{ 0, 0, W, H };
    if (blit.right > blit.left && blit.bottom > blit.top) {
        BitBlt(dc, blit.left, blit.top, blit.right - blit.left, blit.bottom - blit.top,
               mem, blit.left, blit.top, SRCCOPY);
    }
    g_backBufferFresh = true;

    EndPaint(hwnd, &ps);
}

// 折叠 / 展开后按内容重算窗口高度（左上角位置保持不动）
static void Relayout(HWND hwnd) {
    int clientH = ClientH();
    RECT want{ 0, 0, (int)(CLIENT_W * g_scale * g_uiScale), clientH };
    AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);
    int winW = want.right - want.left;
    int winH = want.bottom - want.top;

    RECT cur{};
    GetWindowRect(hwnd, &cur);
    SetWindowPos(hwnd, nullptr, cur.left, cur.top, winW, winH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    ReleaseBackBuffer();
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void ToggleTopmost(HWND hwnd) {
    g_topmost = !g_topmost;
    SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DoCopy(HWND hwnd) {
    CopyToClipboard(hwnd, BuildInfoText());
    g_copyFlash = true;
    g_copyFlashAt = GetTickCount();
    InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(hwnd);
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
        if (dpi <= 0) dpi = 96;
        g_scale = (float)dpi / 96.0f;
        CreateFonts();

        CollectStatic();

        {
            DiskProbe d = ProbeDisk(0);
            g_m.ssdTempC = d.tempC;
        }
        CpuTempInit();
        Sample();
        g_histCount = 0;
        g_histHead  = 0;

        // 初始尺寸（默认全部折叠，窗口很小）
        {
            int clientH = ClientH();
            RECT want{ 0, 0, (int)(CLIENT_W * g_scale * g_uiScale), clientH };
            AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               FALSE, 0);
            int winW = want.right - want.left;
            int winH = want.bottom - want.top;

            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int sx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
            int sy = wa.top + ((wa.bottom - wa.top) - winH) / 2;
            if (sx < 0) sx = 0;
            if (sy < wa.top) sy = wa.top;
            SetWindowPos(hwnd, nullptr, sx, sy, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        SetTimer(hwnd, TIMER_ID, 1000, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ID) {
            static int tick = 0;
            ++tick;
            if (tick % 5 == 1) {                 // 磁盘温度 5 秒刷新一次
                DiskProbe d = ProbeDisk(0);
                if (d.tempC > 0) g_m.ssdTempC = d.tempC;
            }
            Sample();

            // 只失效上半部分（工具栏 + 卡片 + 曲线），静态信息区不参与每秒重绘
            RECT cr{};
            GetClientRect(hwnd, &cr);
            RECT dirty{ 0, 0, cr.right, YInfo() };
            InvalidateRect(hwnd, &dirty, FALSE);

            if (g_copyFlash && GetTickCount() - g_copyFlashAt > 1200) {
                g_copyFlash = false;
                InvalidateRect(hwnd, nullptr, FALSE);   // 工具栏也要恢复
            }
        }
        return 0;
    case WM_PAINT:
        PaintAll(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        ReleaseBackBuffer();     // 客户区尺寸变了，双缓冲缓存作废
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        if (PtInRect(&g_rcBtnTopmost, pt)) {
            ToggleTopmost(hwnd);
        } else if (PtInRect(&g_rcBtnCopy, pt)) {
            DoCopy(hwnd);
        } else if (PtInRect(&g_rcHdrChart, pt)) {
            g_showChart = !g_showChart;
            Relayout(hwnd);
        } else if (PtInRect(&g_rcHdrInfo, pt)) {
            g_showInfo = !g_showInfo;
            Relayout(hwnd);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&g_rcBtnTopmost, pt) || PtInRect(&g_rcBtnCopy, pt) ||
                PtInRect(&g_rcHdrChart, pt)  || PtInRect(&g_rcHdrInfo, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
        } else if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DoCopy(hwnd);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        if (g_pdhQuery) { PdhCloseQuery(g_pdhQuery); g_pdhQuery = nullptr; }
        ReleaseBackBuffer();
        DestroyFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    {
        DWORD light = 1, sz = sizeof(light), type = 0;
        if (RegGetValueW(HKEY_CURRENT_USER,
                         L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, &type, &light, &sz) == ERROR_SUCCESS) {
            LoadPalette(light == 0);
        } else {
            LoadPalette(false);
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"HWMonWndClass";
    // 图标资源 ID 1（来自 app.rc 嵌入的 HardwareMonitor.ico），取不到则回退系统默认
    wc.hIcon   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
                                   LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_SHARED);
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    HDC screen = GetDC(nullptr);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    if (dpi <= 0) dpi = 96;
    g_scale = (float)dpi / 96.0f;

    // 这里只是给个占位高度，WM_CREATE 里会按实际内容重算
    RECT want{ 0, 0, (int)(CLIENT_W * g_scale * g_uiScale), (int)(400 * g_scale * g_uiScale) };
    AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int winW = want.right - want.left;
    int winH = want.bottom - want.top;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (GetSystemMetrics(SM_CXSCREEN) - winW) / 2, 80, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
