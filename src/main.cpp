// CapView: low latency capture card viewer for consoles.
// Video: Media Foundation (low latency source reader) -> D3D11 video processor -> flip model swap chain.
// Audio: WASAPI capture of the card's endpoint -> converter (any PCM / float format, any channel layout,
//        any sample rate, adaptive drift correction) -> WASAPI render on the chosen output device.
// Frame generation: CapView's own optical flow interpolation (any GPU), NVIDIA Smooth Motion through the
//        driver profile, AMD Fluid Motion Frames through ADLX, Intel XeSS frame generation through a
//        D3D12 proxy swap chain.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "xess/xefg_swapchain_d3d12.h"
#include "xess/xell_d3d12.h"
#pragma comment(lib, "d3d12.lib")
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <powrprof.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "powrprof.lib")
#include <shellscalingapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shcore.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Property keys defined locally so no INITGUID games are needed.
static const DEVPROPKEY kDevInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
static const DEVPROPKEY kDevContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
static const PROPERTYKEY kPkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
static const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// The "this computer" container that every internal device shares. Useless for matching.
static const GUID kComputerContainer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

// ------------------------------------------------------------------------------------------------
// Logging and small helpers
// ------------------------------------------------------------------------------------------------
static std::mutex g_logMx;
static FILE* g_log = nullptr;
static bool g_console = false;

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_logMx);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(g_log, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(g_log);
    }
    if (g_console) {
        wprintf(L"%s\n", buf);
        fflush(stdout);
    }
}

static std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static bool Contains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

static bool UsableContainer(const GUID& g) {
    return g != GUID_NULL && g != kComputerContainer;
}

// ------------------------------------------------------------------------------------------------
// Settings and shared state
// ------------------------------------------------------------------------------------------------
enum FgMode { FG_OFF = 0, FG_CAPVIEW = 1, FG_SMOOTH = 2, FG_AMD = 3, FG_XESS = 4, FG_COUNT };
static const wchar_t* kFgKeys[] = {L"off", L"capview", L"smoothmotion", L"afmf", L"xess"};

struct Settings {
    std::wstring videoName;  // substring match, empty = auto
    std::wstring audioName;  // substring match, empty = auto, "none" = no audio
    std::wstring outputId;   // audio output endpoint id, empty = follow the Windows default
    std::wstring outputName;
    UINT reqW = 0, reqH = 0;
    double reqFps = 0;
    bool fullscreen = false;
    bool exclusive = false;  // fullscreen takes the display through DXGI instead of a borderless window
    bool vsync = false;
    bool mute = false;
    float volume = 1.0f;
    int fgMode = FG_OFF;
    int fgMult = 2;
    bool fgPredict = false;  // CapView frame gen predicts past the newest frame instead of waiting for it
    bool showFps = false;
    bool hdr = true;  // HDR10 output for HDR sources when Windows HDR is on
    bool debug = false;  // detailed log for bug reports
    bool list = false;
    int nvSmooth = -1;      // --nv-smooth-motion 0|1, used by the elevated helper run
};
static Settings g_set;
static std::mutex g_setMx;  // guards the strings and the requested mode in g_set
static std::wstring g_iniPath;

static Settings GetSettings() {
    std::lock_guard<std::mutex> l(g_setMx);
    return g_set;
}

static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_vsync{false};
static std::atomic<bool> g_exclusive{false};   // wanted: fullscreen is exclusive
static std::atomic<bool> g_modeSwitch{false};  // SetFullscreenState is running on the window thread
static std::atomic<bool> g_mute{false};
static std::atomic<float> g_volume{1.0f};
static std::atomic<bool> g_videoActive{false};
static std::atomic<int> g_videoPick{-1};  // -1 auto, else index into the device list
static std::atomic<int> g_videoCurIdx{-1};
static std::atomic<int> g_videoCount{0};
static std::atomic<bool> g_videoRestart{false};
static std::atomic<int> g_audioPick{-1};  // -1 auto, -2 none, else index
static std::atomic<int> g_audioCurIdx{-1};
static std::atomic<int> g_audioCount{0};
static std::atomic<bool> g_audioRestart{false};
static std::atomic<bool> g_audioReeval{false};
static std::atomic<float> g_audioLatencyMs{0};
static std::atomic<uint32_t> g_framesPresented{0};
static std::atomic<int> g_fgMode{FG_OFF};
static std::atomic<int> g_fgMult{2};
static std::atomic<bool> g_fgActive{false};  // CapView interpolation produced a frame recently
static std::atomic<float> g_srcFps{0};

// Output frame counting. g_presents counts every Present call. g_displayed counts presents that DXGI
// frame statistics say reached the screen, which leaves out frames the compositor or the flip queue
// dropped. g_framesIn counts captured frames as they arrive from the card.
static std::atomic<uint64_t> g_presents{0};
static std::atomic<uint64_t> g_displayed{0};
static std::atomic<uint64_t> g_framesIn{0};
static std::atomic<bool> g_statsOk{false};
static std::atomic<uint32_t> g_worstUs{0};  // longest gap between two displayed frames since the last read
static std::atomic<bool> g_showFps{false};
// CapView frame generation predicts forward from the newest frame instead of interpolating up to it.
static std::atomic<bool> g_fgPredict{false};
static double g_timingSecs = 30;  // video and audio timing log interval, CAPVIEW_TIMING_SECS
static double g_timingSecsEnv = 0;  // CAPVIEW_TIMING_SECS when set, which debug mode leaves alone
// Debug mode: a system report at start, a stats line every second, focus and notification changes,
// window messages, a 5 s timing log and fuller crash dumps.
static std::atomic<bool> g_debug{false};
static double g_dbgWorstMs = 0;  // longest displayed frame gap since the last debug stats line (UI thread)
static ULONGLONG g_stallHoldMs = 0;  // how long a stalled video stream stays open, CAPVIEW_STALL_HOLD_SECS
static std::mutex g_overlayMx;
static std::wstring g_overlayText;  // guarded by g_overlayMx
// RTX Encore's module name when it's loaded (FindRtxEncore). Set once at start, before any thread
// reads it; modules loaded from the exe folder don't change later.
static std::wstring g_encore;
// True while RTX Encore's status file says Smooth Motion is active (refreshed every 2 s). The
// capture thread reads it.
static std::atomic<bool> g_encoreSmoothOn{false};

// One frame generator at a time. CapView frame generation waits while RTX Encore's Smooth Motion
// runs.
static bool FgPausedForSmooth() { return g_fgMode == FG_CAPVIEW && g_encoreSmoothOn; }

// HDR. The UI thread checks every two seconds whether the monitor under the window is in Windows
// HDR mode. The renderer switches the swap chain to HDR10 when that is true, the source is HDR10
// or HLG, and the user allows it.
static std::atomic<bool> g_displayHdr{false};
static std::atomic<float> g_displayMaxNits{0};
static std::atomic<float> g_sdrWhiteNits{203};
static std::atomic<bool> g_hdrAllow{true};
static std::atomic<int> g_hdrOut{0};      // 0 SDR swap chain, 1 HDR10 swap chain
static std::atomic<int> g_srcHdr{0};      // 0 SDR, 1 HDR10 (PQ), 2 HLG

static std::mutex g_statusMx;
static std::wstring g_videoStatus = L"Starting";
static std::wstring g_audioStatus = L"Audio: starting";
struct CurVideoDev {
    std::wstring name;
    GUID container = GUID_NULL;
};
static CurVideoDev g_curVideo;  // guarded by g_statusMx

// ------------------------------------------------------------------------------------------------
// capview.ini next to the exe. Command line options override it.
// ------------------------------------------------------------------------------------------------
static std::wstring IniGet(const wchar_t* key, const wchar_t* def = L"") {
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"CapView", key, def, buf, 1024, g_iniPath.c_str());
    return buf;
}
static void IniPut(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"CapView", key, v.c_str(), g_iniPath.c_str());
}

static std::wstring ModeString(UINT w, UINT h, double fps) {
    if (!w && fps <= 0) return L"";
    if (!w) return Fmt(L"@%.2f", fps);
    return fps > 0 ? Fmt(L"%ux%u@%.2f", w, h, fps) : Fmt(L"%ux%u", w, h);
}
static void ParseMode(const std::wstring& m, Settings& s) {
    UINT w = 0, h = 0;
    double fps = 0;
    s.reqW = s.reqH = 0;
    s.reqFps = 0;
    if (swscanf_s(m.c_str(), L"%ux%u@%lf", &w, &h, &fps) >= 2) {
        s.reqW = w;
        s.reqH = h;
        s.reqFps = fps;
    } else if (swscanf_s(m.c_str(), L"@%lf", &fps) == 1) {
        s.reqFps = fps;
    }
}

static void LoadIni() {
    Settings& s = g_set;
    s.videoName = IniGet(L"video");
    s.audioName = IniGet(L"audio");
    s.outputId = IniGet(L"output");
    s.outputName = IniGet(L"outputName");
    ParseMode(IniGet(L"mode"), s);
    s.volume = (float)_wtof(IniGet(L"volume", L"100").c_str()) / 100.0f;
    s.mute = IniGet(L"mute", L"0") == L"1";
    s.vsync = IniGet(L"vsync", L"0") == L"1";
    s.fullscreen = IniGet(L"fullscreen", L"0") == L"1";
    s.exclusive = IniGet(L"exclusive", L"0") == L"1";
    std::wstring fg = Lower(IniGet(L"framegen", L"off"));
    for (int i = 0; i < FG_COUNT; i++)
        if (fg == kFgKeys[i]) s.fgMode = i;
    s.fgMult = std::min(4, std::max(2, _wtoi(IniGet(L"multiplier", L"2").c_str())));
    s.fgPredict = IniGet(L"predict", L"0") == L"1";
    s.showFps = IniGet(L"showFps", L"0") == L"1";
    s.hdr = Lower(IniGet(L"hdr", L"auto")) != L"off";
    s.debug = IniGet(L"debug", L"0") == L"1";
}

static std::vector<std::pair<std::wstring, std::wstring>> IniValues(const Settings& s) {
    return {
        {L"video", s.videoName},
        {L"mode", ModeString(s.reqW, s.reqH, s.reqFps)},
        {L"audio", s.audioName},
        {L"output", s.outputId},
        {L"outputName", s.outputName},
        {L"volume", Fmt(L"%.0f", s.volume * 100)},
        {L"mute", s.mute ? L"1" : L"0"},
        {L"vsync", s.vsync ? L"1" : L"0"},
        {L"fullscreen", s.fullscreen ? L"1" : L"0"},
        {L"exclusive", s.exclusive ? L"1" : L"0"},
        {L"framegen", kFgKeys[std::min(FG_COUNT - 1, std::max(0, s.fgMode))]},
        {L"multiplier", Fmt(L"%d", s.fgMult)},
        {L"predict", s.fgPredict ? L"1" : L"0"},
        {L"showFps", s.showFps ? L"1" : L"0"},
        {L"hdr", s.hdr ? L"auto" : L"off"},
        {L"debug", s.debug ? L"1" : L"0"},
    };
}

// Values given on the command line apply to that run only. They reach the ini only once the user
// changes them in the app.
static Settings g_cliSet;
static std::vector<std::wstring> g_cliKeys;

static void SaveIni() {
    auto cur = IniValues(GetSettings());
    auto cli = IniValues(g_cliSet);
    for (size_t i = 0; i < cur.size(); i++) {
        bool fromCli = std::find(g_cliKeys.begin(), g_cliKeys.end(), cur[i].first) != g_cliKeys.end();
        if (fromCli && cur[i].second == cli[i].second) continue;
        IniPut(cur[i].first.c_str(), cur[i].second);
    }
}

static void SetVideoStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> l(g_statusMx);
    if (g_videoStatus != s) Log(L"[video] %s", s.c_str());
    g_videoStatus = s;
}
static void SetAudioStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> l(g_statusMx);
    if (g_audioStatus != s) Log(L"[audio] %s", s.c_str());
    g_audioStatus = s;
}

// ------------------------------------------------------------------------------------------------
// Device name scoring. Consoles go through capture cards, so prefer anything that looks like one
// and stay away from webcams, virtual cameras and microphones.
// ------------------------------------------------------------------------------------------------
static int CaptureNameScore(const std::wstring& name) {
    std::wstring n = Lower(name);
    static const wchar_t* strong[] = {
        L"cam link", L"camlink", L"elgato", L"game capture", L"hd60", L"4k60", L"4k pro", L"4k x", L"4k s",
        L"avermedia", L"live gamer", L"live streamer", L"gc311", L"gc513", L"gc553", L"gc573",
        L"gc575", L"gc551", L"magewell", L"usb capture", L"ripsaw", L"genki", L"shadowcast",
        L"nzxt signal", L"signal hd", L"signal 4k", L"ezcap", L"mirabox", L"decklink",
        L"intensity", L"ultrastudio", L"hauppauge", L"hd pvr", L"pengo", L"guermok", L"acasis",
        L"j5create", L"razer", L"capture card", L"video capture", L"hdmi capture", L"hdmi"};
    static const wchar_t* weak[] = {L"capture", L"usb video", L"usb3 video", L"uvc", L"digital input", L"line in"};
    static const wchar_t* bad[] = {
        L"webcam", L"web cam", L"integrated", L"facetime", L"virtual", L"obs", L"snap camera",
        L"droidcam", L"ir camera", L"front", L"rear", L"broadcast", L"xsplit", L"streamlabs",
        L"camo", L"iriun", L"c920", L"c922", L"c930", L"brio", L"kiyo", L"lifecam", L"microphone",
        L"headset", L"headphone", L"steam streaming", L"bigscreen", L"voicemeeter", L"vb-audio",
        L"cable output", L"mixed reality", L"stereo mix", L"what u hear", L"array"};
    int s = 0;
    for (auto k : strong)
        if (n.find(k) != std::wstring::npos) { s = 50; break; }
    if (s == 0)
        for (auto k : weak)
            if (n.find(k) != std::wstring::npos) { s = 20; break; }
    for (auto k : bad)
        if (n.find(k) != std::wstring::npos) {
            // "Razer Kiyo" is a webcam, "Razer Ripsaw" is a capture card.
            s -= 80;
            break;
        }
    if (n.find(L"camera") != std::wstring::npos) s -= 30;
    return s;
}

// ------------------------------------------------------------------------------------------------
// Video device enumeration
// ------------------------------------------------------------------------------------------------
struct VideoDev {
    std::wstring name, link;
    GUID container = GUID_NULL;
    int score = 0;
};

static GUID ContainerFromInterface(const std::wstring& link) {
    GUID g = GUID_NULL;
    wchar_t inst[512];
    ULONG size = sizeof(inst);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(link.c_str(), &kDevInstanceId, &type, (PBYTE)inst, &size, 0) != CR_SUCCESS)
        return g;
    DEVINST di = 0;
    if (CM_Locate_DevNodeW(&di, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return g;
    size = sizeof(g);
    if (CM_Get_DevNode_PropertyW(di, &kDevContainerId, &type, (PBYTE)&g, &size, 0) != CR_SUCCESS) return GUID_NULL;
    return g;
}

static std::vector<VideoDev> EnumVideo() {
    std::vector<VideoDev> out;
    ComPtr<IMFAttributes> a;
    if (FAILED(MFCreateAttributes(&a, 1))) return out;
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** arr = nullptr;
    UINT32 n = 0;
    if (FAILED(MFEnumDeviceSources(a.Get(), &arr, &n))) return out;
    for (UINT32 i = 0; i < n; i++) {
        VideoDev d;
        wchar_t* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(arr[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
            d.name = s;
            CoTaskMemFree(s);
        }
        if (SUCCEEDED(arr[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) {
            d.link = s;
            CoTaskMemFree(s);
        }
        d.container = ContainerFromInterface(d.link);
        d.score = CaptureNameScore(d.name);
        out.push_back(d);
        arr[i]->Release();
    }
    CoTaskMemFree(arr);
    return out;
}

// Returns index or -1.
static int PickVideo(const std::vector<VideoDev>& list) {
    int pick = g_videoPick.load();
    if (pick >= 0 && pick < (int)list.size()) return pick;
    std::wstring want = GetSettings().videoName;
    if (!want.empty()) {
        for (size_t i = 0; i < list.size(); i++)
            if (Contains(list[i].name, want)) return (int)i;
        return -1;
    }
    int best = -1;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].score >= 0 && (best < 0 || list[i].score > list[best].score)) best = (int)i;
    return best;
}

// ------------------------------------------------------------------------------------------------
// Video formats
// ------------------------------------------------------------------------------------------------
struct FrameFormat {
    GUID subtype = GUID_NULL;
    UINT w = 0, h = 0;
    UINT fpsN = 0, fpsD = 1;
    UINT parN = 1, parD = 1;
    LONG stride = 0;
    UINT matrix = 0, range = 0, transfer = 0, primaries = 0, interlace = 0;
};

static std::wstring SubtypeName(const GUID& g) {
    struct {
        const GUID* g;
        const wchar_t* n;
    } names[] = {
        {&MFVideoFormat_NV12, L"NV12"}, {&MFVideoFormat_YUY2, L"YUY2"}, {&MFVideoFormat_UYVY, L"UYVY"},
        {&MFVideoFormat_RGB32, L"RGB32"}, {&MFVideoFormat_ARGB32, L"ARGB32"}, {&MFVideoFormat_RGB24, L"RGB24"},
        {&MFVideoFormat_I420, L"I420"}, {&MFVideoFormat_IYUV, L"IYUV"}, {&MFVideoFormat_YV12, L"YV12"},
        {&MFVideoFormat_P010, L"P010"}, {&MFVideoFormat_P016, L"P016"}, {&MFVideoFormat_AYUV, L"AYUV"},
        {&MFVideoFormat_Y210, L"Y210"}, {&MFVideoFormat_Y216, L"Y216"}, {&MFVideoFormat_v210, L"v210"},
        {&MFVideoFormat_MJPG, L"MJPG"}, {&MFVideoFormat_H264, L"H264"}, {&MFVideoFormat_HEVC, L"HEVC"},
    };
    for (auto& e : names)
        if (*e.g == g) return e.n;
    wchar_t cc[5] = {(wchar_t)(g.Data1 & 0xff), (wchar_t)((g.Data1 >> 8) & 0xff), (wchar_t)((g.Data1 >> 16) & 0xff),
                     (wchar_t)((g.Data1 >> 24) & 0xff), 0};
    for (int i = 0; i < 4; i++)
        if (cc[i] < 32 || cc[i] > 126) return Fmt(L"0x%08X", g.Data1);
    return cc;
}

static DXGI_FORMAT DxgiFromSubtype(const GUID& g) {
    if (g == MFVideoFormat_NV12) return DXGI_FORMAT_NV12;
    if (g == MFVideoFormat_YUY2) return DXGI_FORMAT_YUY2;
    if (g == MFVideoFormat_P010) return DXGI_FORMAT_P010;
    if (g == MFVideoFormat_P016) return DXGI_FORMAT_P016;
    if (g == MFVideoFormat_RGB32) return DXGI_FORMAT_B8G8R8X8_UNORM;
    if (g == MFVideoFormat_ARGB32) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (g == MFVideoFormat_AYUV) return DXGI_FORMAT_AYUV;
    if (g == MFVideoFormat_Y210) return DXGI_FORMAT_Y210;
    if (g == MFVideoFormat_Y216) return DXGI_FORMAT_Y216;
    return DXGI_FORMAT_UNKNOWN;
}

static bool IsCompressed(const GUID& g) {
    return g == MFVideoFormat_MJPG || g == MFVideoFormat_H264 || g == MFVideoFormat_HEVC ||
           g == MFVideoFormat_H264_ES || g == MFVideoFormat_HEVC_ES || g == MFVideoFormat_MP4V ||
           g == MFVideoFormat_VP90 || g == MFVideoFormat_AV1;
}

static bool IsTenBit(const GUID& g) {
    return g == MFVideoFormat_P010 || g == MFVideoFormat_P016 || g == MFVideoFormat_Y210 ||
           g == MFVideoFormat_Y216 || g == MFVideoFormat_v210 || g == MFVideoFormat_v410;
}

static int SubtypeRank(const GUID& g) {
    if (g == MFVideoFormat_NV12) return 6;
    if (g == MFVideoFormat_YUY2 || g == MFVideoFormat_P010) return 5;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return 4;
    if (g == MFVideoFormat_MJPG) return 2;
    if (IsCompressed(g)) return 1;
    return 3;  // other uncompressed formats, converted by the source reader
}

static FrameFormat ReadFormat(IMFMediaType* t, IMFMediaType* fallback) {
    FrameFormat f;
    t->GetGUID(MF_MT_SUBTYPE, &f.subtype);
    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &f.w, &f.h);
    MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &f.fpsN, &f.fpsD);
    if (FAILED(MFGetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, &f.parN, &f.parD)) || !f.parN || !f.parD) f.parN = f.parD = 1;
    UINT32 s = 0;
    if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) f.stride = (LONG)(INT32)s;
    auto get = [&](REFGUID key) -> UINT {
        UINT32 v = 0;
        if (SUCCEEDED(t->GetUINT32(key, &v)) && v) return v;
        if (fallback && SUCCEEDED(fallback->GetUINT32(key, &v))) return v;
        return 0;
    };
    f.matrix = get(MF_MT_YUV_MATRIX);
    f.range = get(MF_MT_VIDEO_NOMINAL_RANGE);
    f.transfer = get(MF_MT_TRANSFER_FUNCTION);
    f.primaries = get(MF_MT_VIDEO_PRIMARIES);
    f.interlace = get(MF_MT_INTERLACE_MODE);
    if (!f.fpsN || !f.fpsD) {
        f.fpsN = 60;
        f.fpsD = 1;
    }
    return f;
}

static double Fps(const FrameFormat& f) { return f.fpsD ? (double)f.fpsN / f.fpsD : 0; }

static std::wstring ModeText(const FrameFormat& f) {
    return Fmt(L"%ux%u %s %.2f fps", f.w, f.h, SubtypeName(f.subtype).c_str(), Fps(f));
}

// The same mode with its rate written as the signal's refresh rate, for the title bar, where the
// frame rate shown is the game's.
static std::wstring ModeHzText(const FrameFormat& f) {
    double hz = Fps(f);
    bool whole = std::fabs(hz - std::round(hz)) < 0.005;
    return Fmt(whole ? L"%ux%u %s %.0f Hz" : L"%ux%u %s %.2f Hz", f.w, f.h, SubtypeName(f.subtype).c_str(), hz);
}

// Bytes in one row of a frame's first plane.
static UINT RowBytes(DXGI_FORMAT fmt, UINT w) {
    switch (fmt) {
        case DXGI_FORMAT_NV12: return w;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
        case DXGI_FORMAT_YUY2: return w * 2;
        default: return w * 4;
    }
}

// Offset of a luma byte (or the green byte for RGB) in each 4-byte group of the first plane.
static UINT LumaOffset(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM: return 1;
        case DXGI_FORMAT_AYUV: return 2;
        default: return 0;
    }
}

// Content frame rate. A console sends 60 Hz over HDMI whatever the game renders, and repeats the
// last frame when the game is slower, so the card's rate says nothing about the game. Each captured
// frame is compared with the one before on a grid of luma samples (every second row, every second
// pixel), and only frames that changed count as content frames. HDMI is digital, so a repeated
// frame matches exactly; the noise margin covers cards that dither.
static std::atomic<uint64_t> g_framesNew{0};
static std::atomic<uint64_t> g_nearRepeats{0};   // frames with a few changed samples, below the cutoff
static std::atomic<uint64_t> g_smallChanges{0};  // Smooth Motion frames that only passed under the exact-repeat rule

// The noise margin also hides real frames. In a dark or slowly fading scene a game frame can move
// every sample by less than the margin, so CapView skipped it and the picture held for 67 to 133 ms.
// This learns whether the card passes repeats through bit-exact: after 4 captures identical to the
// capture before them, any change counts. A card that dithers never shows an exact repeat and keeps
// the margin, and 120 captures in a row without one (2 s at 60 Hz) go back to it. Learning has to
// use the capture before, not the latest content frame. While the margin hides a dark scene's
// frames, that content frame goes stale and nothing matches it exactly.
struct ExactRepeats {
    bool on = false;
    int seen = 0, since = 0;
    // Takes whether this capture matched the one before it sample for sample.
    void Learn(bool exact) {
        if (exact) {
            since = 0;
            if (!on && ++seen >= 4) on = true;
        } else if (on && ++since >= 120) {
            on = false;
            seen = 0;
        }
    }
    // The count to hold against the cutoff: samples that changed at all, or past the noise margin.
    size_t Pick(size_t margin, size_t any) const { return on ? any : margin; }
    void Reset() { *this = ExactRepeats(); }
};

class FrameDiff {
public:
    // Returns true when the frame differs from the last one checked.
    bool Check(const BYTE* scan0, LONG pitch, UINT rowBytes, UINT rows, UINT offset) {
        size_t cols = rowBytes > offset ? (rowBytes - offset + 3) / 4 : 0;
        size_t n = cols * ((rows + 1) / 2);
        bool fresh = prev_.size() != n;
        if (fresh) {
            prev_.assign(n, 0);
            exact_.Reset();
        }
        size_t changed = 0, any = 0, k = 0;
        for (UINT r = 0; r < rows; r += 2) {
            const BYTE* p = scan0 + (ptrdiff_t)r * pitch + offset;
            BYTE* q = prev_.data() + k;
            for (size_t c = 0; c < cols; c++) {
                int d = (int)p[c * 4] - q[c];
                if (d > kNoise || d < -kNoise) changed++;
                if (d) any++;
                q[c] = p[c * 4];
            }
            k += cols;
        }
        if (fresh) return true;
        exact_.Learn(any == 0);
        size_t count = exact_.Pick(changed, any);
        if (count > 0 && count < kMinChanged) g_nearRepeats++;
        return count >= kMinChanged;
    }
    void Reset() {
        prev_.clear();
        exact_.Reset();
    }

private:
    static const int kNoise = 3;           // luma steps a sample may move and still count as unchanged
    static const size_t kMinChanged = 8;   // a 16x16 cursor moving covers about 64 samples
    std::vector<BYTE> prev_;
    ExactRepeats exact_;
};

// ------------------------------------------------------------------------------------------------
// Frame generation shaders. Motion comes from block matching on a luma pyramid: each 4x4 cell of
// a level searches for the vector v where prev(p - v/2) best matches cur(p + v/2), seeded from the
// coarser level, then a 3x3 vector median cleans the field. The draw pass warps both frames to the
// in-between time and falls back toward the nearer frame where they disagree (occlusions).
// ------------------------------------------------------------------------------------------------
static const char kFlowHlsl[] = R"HLSL(
cbuffer P : register(b0) {
    float2 lvlSize; float2 invLvl;
    int2 fieldSize; int2 parentSize;
    int radius; int hasParent; float lambda; float pad0;
};
SamplerState lin : register(s0);
Texture2D<float4> rgbIn : register(t0);
Texture2D<float> lumIn : register(t1);
Texture2D<float> prevL : register(t2);
Texture2D<float> curL : register(t3);
Texture2D<float2> parentF : register(t4);
Texture2D<float2> fieldIn : register(t5);
RWTexture2D<float> lumOut : register(u0);
RWTexture2D<float2> fieldOut : register(u1);

static const float3 kY = float3(0.299, 0.587, 0.114);

[numthreads(8, 8, 1)]
void CsLuma(uint3 id : SV_DispatchThreadID) {
    uint w, h, sw, sh;
    lumOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    rgbIn.GetDimensions(sw, sh);
    int2 p = int2(id.xy) * 2, mx = int2(sw - 1, sh - 1);
    float y = dot(rgbIn.Load(int3(min(p, mx), 0)).rgb, kY) + dot(rgbIn.Load(int3(min(p + int2(1, 0), mx), 0)).rgb, kY) +
              dot(rgbIn.Load(int3(min(p + int2(0, 1), mx), 0)).rgb, kY) + dot(rgbIn.Load(int3(min(p + int2(1, 1), mx), 0)).rgb, kY);
    lumOut[id.xy] = y * 0.25;
}

[numthreads(8, 8, 1)]
void CsDown(uint3 id : SV_DispatchThreadID) {
    uint w, h, sw, sh;
    lumOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    lumIn.GetDimensions(sw, sh);
    int2 p = int2(id.xy) * 2, mx = int2(sw - 1, sh - 1);
    float y = lumIn.Load(int3(min(p, mx), 0)) + lumIn.Load(int3(min(p + int2(1, 0), mx), 0)) +
              lumIn.Load(int3(min(p + int2(0, 1), mx), 0)) + lumIn.Load(int3(min(p + int2(1, 1), mx), 0));
    lumOut[id.xy] = y * 0.25;
}

// Zero-mean SAD over an 8x8 block. Taking each block's mean out first means a brightness change
// that moves across a still surface, such as a headlight beam sweeping over the road, doesn't
// pull the road's motion along with the light. The texture decides the match, not the lighting.
float Cost(float2 c, float2 v) {
    float2 hv = v * 0.5;
    float ma = 0, mb = 0;
    [loop] for (int y = 0; y < 8; y++) {
        [unroll] for (int x = 0; x < 8; x++) {
            float2 q = c + float2(x - 3.5, y - 3.5);
            ma += prevL.SampleLevel(lin, (q - hv) * invLvl, 0);
            mb += curL.SampleLevel(lin, (q + hv) * invLvl, 0);
        }
    }
    float dm = (ma - mb) * (1.0 / 64.0);
    float s = 0;
    [loop] for (int y2 = 0; y2 < 8; y2++) {
        [unroll] for (int x = 0; x < 8; x++) {
            float2 q = c + float2(x - 3.5, y2 - 3.5);
            s += abs(prevL.SampleLevel(lin, (q - hv) * invLvl, 0) - curL.SampleLevel(lin, (q + hv) * invLvl, 0) - dm);
        }
    }
    return s * (1.0 / 64.0);
}

[numthreads(8, 8, 1)]
void CsFlow(uint3 id : SV_DispatchThreadID) {
    if (any(int2(id.xy) >= fieldSize)) return;
    float2 c = float2(id.xy) * 4.0 + 2.0;
    float2 pred = float2(0, 0);
    int2 pc = int2(id.xy) / 2;
    if (hasParent) pred = parentF.Load(int3(clamp(pc, int2(0, 0), parentSize - 1), 0)) * 2.0;
    float2 best = float2(0, 0);
    float bestCost = Cost(c, best) + lambda * length(pred) - 0.002;
    if (!hasParent) {
        [loop] for (int dy = -radius; dy <= radius; dy++) {
            [loop] for (int dx = -radius; dx <= radius; dx++) {
                float2 v = float2(dx, dy);
                float k = Cost(c, v) + lambda * length(v);
                if (k < bestCost) { bestCost = k; best = v; }
            }
        }
    } else {
        const int2 offs[5] = {int2(0, 0), int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
        [unroll] for (int i = 0; i < 5; i++) {
            float2 v = parentF.Load(int3(clamp(pc + offs[i], int2(0, 0), parentSize - 1), 0)) * 2.0;
            float k = Cost(c, v) + lambda * length(v - pred);
            if (k < bestCost) { bestCost = k; best = v; }
        }
        float2 c0 = best;
        [loop] for (int dy = -radius; dy <= radius; dy++) {
            [loop] for (int dx = -radius; dx <= radius; dx++) {
                if (dx == 0 && dy == 0) continue;
                float2 v = c0 + float2(dx, dy);
                float k = Cost(c, v) + lambda * length(v - pred);
                if (k < bestCost) { bestCost = k; best = v; }
            }
        }
    }
    float2 c1 = best;
    [loop] for (int j = 0; j < 9; j++) {
        if (j == 4) continue;
        float2 v = c1 + float2(j % 3 - 1, j / 3 - 1) * 0.5;
        float k = Cost(c, v) + lambda * length(v - pred);
        if (k < bestCost) { bestCost = k; best = v; }
    }
    fieldOut[id.xy] = best;
}

[numthreads(8, 8, 1)]
void CsMedian(uint3 id : SV_DispatchThreadID) {
    if (any(int2(id.xy) >= fieldSize)) return;
    float2 v[9];
    [unroll] for (int y = 0; y < 3; y++) {
        [unroll] for (int x = 0; x < 3; x++)
            v[y * 3 + x] = fieldIn.Load(int3(clamp(int2(id.xy) + int2(x - 1, y - 1), int2(0, 0), fieldSize - 1), 0));
    }
    float bestD = 1e30;
    float2 best = v[4];
    [unroll] for (int i = 0; i < 9; i++) {
        float d = 0;
        [unroll] for (int j = 0; j < 9; j++) d += abs(v[i].x - v[j].x) + abs(v[i].y - v[j].y);
        if (d < bestD) { bestD = d; best = v[i]; }
    }
    fieldOut[id.xy] = best;
}

// Compares half-resolution luma samples of a new capture (lumIn) with the latest content frame
// (prevL) and with the capture before it, which Differs binds in curL's slot. diffOut[0] counts
// samples that moved from the content frame by more than 2.5 of 255 steps, diffOut[1] samples that
// moved from it at all, and diffOut[2] samples that differ from the capture before.
RWStructuredBuffer<uint> diffOut : register(u2);
groupshared uint gsCount;
groupshared uint gsAny;
groupshared uint gsLast;

[numthreads(8, 8, 1)]
void CsDiff(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) {
        gsCount = 0;
        gsAny = 0;
        gsLast = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint w, h;
    lumIn.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        float l = lumIn.Load(int3(id.xy, 0));
        float d = abs(l - prevL.Load(int3(id.xy, 0)));
        if (d > 2.5 / 255.0) InterlockedAdd(gsCount, 1);
        if (d > 0) InterlockedAdd(gsAny, 1);
        if (l != curL.Load(int3(id.xy, 0))) InterlockedAdd(gsLast, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gsCount) InterlockedAdd(diffOut[0], gsCount);
    if (gi == 0 && gsAny) InterlockedAdd(diffOut[1], gsAny);
    if (gi == 0 && gsLast) InterlockedAdd(diffOut[2], gsLast);
}
)HLSL";

static const char kDrawHlsl[] = R"HLSL(
cbuffer D : register(b0) { float2 dstOrg; float2 invDst; float2 invFlow; float t; int mode; };
SamplerState lin : register(s0);
Texture2D<float4> prevT : register(t0);
Texture2D<float4> curT : register(t1);
Texture2D<float2> flowT : register(t2);

float4 VsTri(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float MaxC(float3 c) { return max(max(c.r, c.g), c.b); }

// How badly the two motion-compensated samples for uv disagree. A large value means the motion
// vector there is wrong, as it is for headlights and other small bright lights that move apart
// from what surrounds them.
float WarpErr(float2 uv) {
    float2 v = flowT.SampleLevel(lin, uv, 0) * invFlow;
    return MaxC(abs(prevT.SampleLevel(lin, uv - t * v, 0).rgb - curT.SampleLevel(lin, uv + (1 - t) * v, 0).rgb));
}

float4 PsDraw(float4 pos : SV_Position) : SV_Target {
    float2 uv = (pos.xy - dstOrg) * invDst;
    if (mode == 0) return float4(curT.SampleLevel(lin, uv, 0).rgb, 1);
    float2 v = flowT.SampleLevel(lin, uv, 0) * invFlow;
    if (mode == 2) {
        // Extrapolation t of a content interval past the current frame: carry the current frame
        // on along its motion. Where the vector traced one more interval back doesn't land on
        // matching colour in the previous frame, the motion is wrong there and the pixel stays.
        float3 c = curT.SampleLevel(lin, uv - t * v, 0).rgb;
        float3 p = prevT.SampleLevel(lin, uv - (1 + t) * v, 0).rgb;
        float w2 = saturate((MaxC(abs(c - p)) - 0.08) * 8);
        return float4(lerp(c, curT.SampleLevel(lin, uv, 0).rgb, w2), 1);
    }
    float3 a = prevT.SampleLevel(lin, uv - t * v, 0).rgb;
    float3 b = curT.SampleLevel(lin, uv + (1 - t) * v, 0).rgb;
    // The worst disagreement at this pixel and 2 motion-field pixels to each side, so the
    // fallback covers a light's halo as well as its core and doesn't leave a fringe.
    float2 d = invFlow * 2;
    float err = MaxC(abs(a - b));
    err = max(err, max(WarpErr(uv + float2(d.x, 0)), WarpErr(uv - float2(d.x, 0))));
    err = max(err, max(WarpErr(uv + float2(0, d.y)), WarpErr(uv - float2(0, d.y))));
    // Where the motion can't be trusted, cross-fade the two frames in place instead of following
    // the vector. That leaves a soft ghost at both positions in place of a smear.
    float w = saturate((err - 0.08) * 8);
    float3 warped = lerp(a, b, t);
    float3 fade = lerp(prevT.SampleLevel(lin, uv, 0).rgb, curT.SampleLevel(lin, uv, 0).rgb, t);
    return float4(lerp(warped, fade, w), 1);
}
)HLSL";

// Composites the fps overlay (premultiplied BGRA from Direct2D). For HDR10 output the sRGB text is
// converted to linear light at the SDR white level, moved to BT.2020 primaries and PQ encoded.
static const char kOverlayHlsl[] = R"HLSL(
cbuffer O : register(b0) { float2 org; float2 unused; float hdr; float whiteNits; float2 pad; };
Texture2D<float4> ov : register(t0);

float4 VsTri(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 Pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

float4 PsOverlay(float4 pos : SV_Position) : SV_Target {
    float4 c = ov.Load(int3(pos.xy - org, 0));
    if (hdr < 0.5 || c.a <= 0) return c;
    float3 s = c.rgb / c.a;
    float3 lin = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    float3x3 to2020 = float3x3(0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956);
    return float4(Pq(mul(to2020, lin) * whiteNits) * c.a, c.a);
}
)HLSL";

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ------------------------------------------------------------------------------------------------
// Renderer
// ------------------------------------------------------------------------------------------------
// Logs any timed step on the render path that takes 20 ms or more, to find startup hitches.
struct SlowStep {
    const wchar_t* what;
    LARGE_INTEGER t0;
    explicit SlowStep(const wchar_t* w) : what(w) { QueryPerformanceCounter(&t0); }
    ~SlowStep() {
        LARGE_INTEGER t1, fq;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        if (ms >= 20) Log(L"Slow step: %s took %.1f ms", what, ms);
    }
};

// ---- Intel XeSS frame generation ---------------------------------------------------------------
// XeSS-FG is Intel's proxy swap chain for D3D12 (libxess_fg.dll, which needs libxell.dll for its
// latency markers). It runs on Intel Arc and on any other GPU with shader model 6.4, with one
// generated frame per drawn frame on non-Intel GPUs. CapView draws with D3D11, so in this mode it
// draws into a texture shared with a D3D12 device on the same adapter. At each present the D3D12
// queue waits on a shared fence for the D3D11 work, copies the texture into the proxy swap chain's
// back buffer and presents, and the D3D11 context waits on the same fence before it draws again.
// A capture has no motion vectors or depth, so XeSS-FG gets zero textures for both and works from
// the pictures. The DLLs load on first use from next to CapView.exe.
// Frames XeSS put on screen, real and generated, as its own present status reports them.
static std::atomic<uint64_t> g_xessShown{0};

namespace xefg {
struct Api {
    HMODULE fg = nullptr;
    decltype(&xefgSwapChainGetVersion) GetVersion = nullptr;
    decltype(&xefgSwapChainD3D12CreateContext) CreateContext = nullptr;
    decltype(&xefgSwapChainSetLoggingCallback) SetLoggingCallback = nullptr;
    decltype(&xefgSwapChainSetLatencyReduction) SetLatencyReduction = nullptr;
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) InitFromDesc = nullptr;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) GetSwapChainPtr = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) TagResource = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) TagConstants = nullptr;
    decltype(&xefgSwapChainSetEnabled) SetEnabled = nullptr;
    decltype(&xefgSwapChainSetPresentId) SetPresentId = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) LastStatus = nullptr;
    decltype(&xefgSwapChainDestroy) Destroy = nullptr;
    decltype(&xellD3D12CreateContext) XellCreate = nullptr;
    decltype(&xellDestroyContext) XellDestroy = nullptr;
    decltype(&xellSetSleepMode) XellSleepMode = nullptr;
    decltype(&xellAddMarkerData) XellMarker = nullptr;
};
static Api g_api;
static std::wstring g_loadError;

template <class F>
static bool GetFn(HMODULE m, const char* name, F& fn) {
    fn = reinterpret_cast<F>(GetProcAddress(m, name));
    if (!fn && g_loadError.empty()) g_loadError = Fmt(L"Intel's DLL has no %S, so it's a version CapView doesn't know.", name);
    return fn != nullptr;
}

// Loads libxell.dll and libxess_fg.dll from CapView's folder once. Later calls return the first result.
static bool Load() {
    static bool tried = false, ok = false;
    if (tried) return ok;
    tried = true;
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring dir(p);
    dir.resize(dir.find_last_of(L'\\') + 1);
    HMODULE ll = LoadLibraryExW((dir + L"libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD e = GetLastError();
    HMODULE fg = ll ? LoadLibraryExW((dir + L"libxess_fg.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
    if (ll && !fg) e = GetLastError();
    if (!fg) {
        g_loadError = Fmt(L"%s didn't load from %s (Windows error %lu). Put Intel's libxess_fg.dll and libxell.dll next "
                          L"to CapView.exe.",
                          ll ? L"libxess_fg.dll" : L"libxell.dll", dir.c_str(), e);
        return false;
    }
    Api& a = g_api;
    a.fg = fg;
    ok = GetFn(fg, "xefgSwapChainGetVersion", a.GetVersion) && GetFn(fg, "xefgSwapChainD3D12CreateContext", a.CreateContext) &&
         GetFn(fg, "xefgSwapChainSetLoggingCallback", a.SetLoggingCallback) &&
         GetFn(fg, "xefgSwapChainSetLatencyReduction", a.SetLatencyReduction) &&
         GetFn(fg, "xefgSwapChainD3D12InitFromSwapChainDesc", a.InitFromDesc) &&
         GetFn(fg, "xefgSwapChainD3D12GetSwapChainPtr", a.GetSwapChainPtr) &&
         GetFn(fg, "xefgSwapChainD3D12TagFrameResource", a.TagResource) &&
         GetFn(fg, "xefgSwapChainTagFrameConstants", a.TagConstants) && GetFn(fg, "xefgSwapChainSetEnabled", a.SetEnabled) &&
         GetFn(fg, "xefgSwapChainSetPresentId", a.SetPresentId) &&
         GetFn(fg, "xefgSwapChainGetLastPresentStatus", a.LastStatus) && GetFn(fg, "xefgSwapChainDestroy", a.Destroy) &&
         GetFn(ll, "xellD3D12CreateContext", a.XellCreate) && GetFn(ll, "xellDestroyContext", a.XellDestroy) &&
         GetFn(ll, "xellSetSleepMode", a.XellSleepMode) && GetFn(ll, "xellAddMarkerData", a.XellMarker);
    if (ok) {
        xefg_swapchain_version_t v = {};
        a.GetVersion(&v);
        Log(L"XeSS frame generation %u.%u.%u loaded", v.major, v.minor, v.patch);
    }
    return ok;
}

static const wchar_t* ResultText(int r) {
    switch (r) {
        case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return L"this GPU isn't supported";
        case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return L"the GPU driver is too old";
        case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED: return L"not supported here";
        case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY: return L"a library it needs didn't load";
        case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED: return L"XeLL isn't supported here";
        case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING: return L"libxell.dll is too old";
        case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return L"out of GPU memory";
        case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL: return L"DXGI refused the swap chain";
        case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT: return L"invalid argument";
        default: return L"error";
    }
}

static std::atomic<int> g_logLines{0};
static void LogCallback(const char* msg, xefg_swapchain_logging_level_t level, void*) {
    // Per-frame warnings would fill the log, so it keeps the first 40 lines of a run.
    if (level < XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING || g_logLines++ >= 40) return;
    Log(L"XeSS-FG %s: %S", level == XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? L"error" : L"warning", msg);
}
}  // namespace xefg

class XessOut {
public:
    ~XessOut() { Shutdown(); }

    // Makes the proxy swap chain on hwnd, which must have no other swap chain. On failure err says why,
    // and everything made so far is gone again, so the caller can make a D3D11 swap chain.
    bool Init(ID3D11Device* dev11, IDXGIFactory2* factory, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
              std::wstring& err) {
        if (!Init2(dev11, factory, hwnd, w, h, fmt, flags, err)) {
            Shutdown();
            return false;
        }
        return true;
    }

    IDXGISwapChain1* SwapChain() const { return sc_.Get(); }
    ID3D11Texture2D* Target() const { return tex11_.Get(); }
    uint32_t FramesShown() const { return shown_; }

    // Resizes the proxy swap chain and the textures that follow its size.
    HRESULT Resize(UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        WaitIdle();
        DropTextures();
        // Test hook: CAPVIEW_TEST_XESS_RESIZE_FAIL makes the first resize fail, as it does under some wrappers.
        static bool failOnce = GetEnvironmentVariableW(L"CAPVIEW_TEST_XESS_RESIZE_FAIL", nullptr, 0) > 0;
        if (failOnce) {
            failOnce = false;
            return DXGI_ERROR_INVALID_CALL;
        }
        HRESULT hr = sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags);
        if (FAILED(hr)) return hr;
        std::wstring err;
        if (!MakeTextures(w, h, fmt, err)) {
            Log(L"XeSS textures at %ux%u: %s", w, h, err.c_str());
            return E_FAIL;
        }
        reset_ = true;
        return S_OK;
    }

    // Called with the frame drawn into Target(), right before Present on SwapChain().
    void BeforePresent() {
        const xefg::Api& a = xefg::g_api;
        a.XellMarker(xell_, id_, XELL_SIMULATION_START);
        a.XellMarker(xell_, id_, XELL_SIMULATION_END);
        a.XellMarker(xell_, id_, XELL_RENDERSUBMIT_START);
        ctx4_->Signal(fence11_.Get(), ++fv_);
        ctx4_->Flush();
        UINT64 drawn = fv_;
        WaitCpu(lastCopy_);  // the allocator is free once the previous copy ran
        alloc_->Reset();
        list_->Reset(alloc_.Get(), nullptr);
        ComPtr<ID3D12Resource> bb;
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(sc_.As(&sc3)) && SUCCEEDED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = bb.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list_->ResourceBarrier(1, &b);
            list_->CopyResource(bb.Get(), tex12_.Get());
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list_->ResourceBarrier(1, &b);
        }
        list_->Close();
        queue_->Wait(fence12_.Get(), drawn);
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence12_.Get(), ++fv_);
        lastCopy_ = fv_;
        ctx4_->Wait(fence11_.Get(), lastCopy_);  // D3D11 draws the next frame after the copy

        xefg_swapchain_d3d12_resource_data_t r = {};
        r.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        r.resourceSize = {w_, h_};
        r.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        r.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        r.pResource = mv_.Get();
        a.TagResource(fg_, nullptr, id_, &r);
        r.type = XEFG_SWAPCHAIN_RES_DEPTH;
        r.pResource = depth_.Get();
        a.TagResource(fg_, nullptr, id_, &r);
        xefg_swapchain_frame_constant_data_t c = {};
        for (int i = 0; i < 4; i++) c.viewMatrix[i * 5] = c.projectionMatrix[i * 5] = 1.0f;
        c.motionVectorScaleX = c.motionVectorScaleY = 1.0f;
        c.resetHistory = reset_ ? 1 : 0;
        LARGE_INTEGER now, fq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&fq);
        c.frameRenderTime = lastT_ ? (float)((now.QuadPart - lastT_) * 1000.0 / fq.QuadPart) : 16.7f;
        lastT_ = now.QuadPart;
        a.TagConstants(fg_, id_, &c);
        reset_ = false;
        a.XellMarker(xell_, id_, XELL_RENDERSUBMIT_END);
        a.SetPresentId(fg_, id_);
        a.XellMarker(xell_, id_, XELL_PRESENT_START);
    }

    void AfterPresent() {
        const xefg::Api& a = xefg::g_api;
        a.XellMarker(xell_, id_, XELL_PRESENT_END);
        xefg_swapchain_present_status_t st = {};
        if (a.LastStatus(fg_, &st) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            shown_ += st.framesPresented;
            g_xessShown += st.framesPresented;
            int r = st.frameGenResult;
            if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && r != lastResult_ && resultLogs_ < 20) {
                resultLogs_++;
                Log(L"XeSS frame generation result %d on frame %u (%s), %u frames presented", r, id_,
                    r > 0 ? L"warning" : xefg::ResultText(r), st.framesPresented);
            }
            lastResult_ = r;
        }
        id_++;
        // A few early reports of how many frames XeSS put on screen per CapView frame.
        if (id_ % 300 == 0 && rateLogs_ < 3) {
            rateLogs_++;
            Log(L"XeSS frame generation: %u frames presented for the last 300 CapView frames", shown_ - lastShown_);
            lastShown_ = shown_;
        }
    }

    // The next frame starts a new sequence, after a pause or a scene change.
    void ResetHistory() { reset_ = true; }

    void Shutdown() {
        if (queue_ && fence12_) WaitIdle();
        DropTextures();
        sc_.Reset();
        if (fg_) xefg::g_api.Destroy(fg_);
        fg_ = nullptr;
        if (xell_) xefg::g_api.XellDestroy(xell_);
        xell_ = nullptr;
        list_.Reset();
        alloc_.Reset();
        fence11_.Reset();
        fence12_.Reset();
        ctx4_.Reset();
        queue_.Reset();
        dev12_.Reset();
        if (ev_) CloseHandle(ev_);
        ev_ = nullptr;
    }

private:
    bool Init2(ID3D11Device* dev11, IDXGIFactory2* factory, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
               std::wstring& err) {
        if (!xefg::Load()) {
            err = xefg::g_loadError;
            return false;
        }
        const xefg::Api& a = xefg::g_api;
        ComPtr<IDXGIDevice> dd;
        ComPtr<IDXGIAdapter> ad;
        ComPtr<ID3D11Device5> d5;
        ComPtr<ID3D11DeviceContext> ctx;
        dev11->GetImmediateContext(&ctx);
        if (FAILED(dev11->QueryInterface(IID_PPV_ARGS(&dd))) || FAILED(dd->GetAdapter(&ad)) ||
            FAILED(dev11->QueryInterface(IID_PPV_ARGS(&d5))) || FAILED(ctx.As(&ctx4_))) {
            err = L"Direct3D 11.4 shared fences aren't available on this system.";
            return false;
        }
        HRESULT hr = D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12_));
        if (FAILED(hr)) {
            err = Fmt(L"Direct3D 12 didn't start on this GPU (0x%08X).", (unsigned)hr);
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_4};
        if (FAILED(dev12_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
            sm.HighestShaderModel < D3D_SHADER_MODEL_6_4) {
            err = L"XeSS frame generation needs a GPU and driver with shader model 6.4.";
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HANDLE fh = nullptr;
        if (FAILED(hr = dev12_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_))) ||
            FAILED(hr = dev12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_))) ||
            FAILED(hr = dev12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_))) ||
            FAILED(hr = list_->Close()) ||
            FAILED(hr = dev12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_))) ||
            FAILED(hr = dev12_->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &fh))) {
            err = Fmt(L"Setting up the Direct3D 12 queue failed (0x%08X).", (unsigned)hr);
            return false;
        }
        hr = d5->OpenSharedFence(fh, IID_PPV_ARGS(&fence11_));
        CloseHandle(fh);
        if (FAILED(hr)) {
            err = Fmt(L"Direct3D 11 couldn't open the shared fence (0x%08X).", (unsigned)hr);
            return false;
        }
        ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        int r = a.XellCreate(dev12_.Get(), &xell_);
        if (r != XELL_RESULT_SUCCESS) {
            err = Fmt(L"Intel's XeLL didn't start (result %d).", r);
            xell_ = nullptr;
            return false;
        }
        xell_sleep_params_t sp = {};
        sp.bLowLatencyMode = 1;
        a.XellSleepMode(xell_, &sp);
        r = a.CreateContext(dev12_.Get(), &fg_);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            err = Fmt(L"XeSS frame generation didn't start: %s (result %d).", xefg::ResultText(r), r);
            fg_ = nullptr;
            return false;
        }
        a.SetLoggingCallback(fg_, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, xefg::LogCallback, nullptr);
        if ((r = a.SetLatencyReduction(fg_, xell_)) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            err = Fmt(L"XeSS frame generation refused XeLL: %s (result %d).", xefg::ResultText(r), r);
            return false;
        }
        xefg_swapchain_d3d12_init_params_t ip = {};
        ip.maxInterpolatedFrames = 1;
        ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = w;
        d.Height = h;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Flags = flags;
        r = a.InitFromDesc(fg_, hwnd, &d, nullptr, queue_.Get(), factory, &ip);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            err = Fmt(L"XeSS frame generation couldn't make its swap chain: %s (result %d).", xefg::ResultText(r), r);
            return false;
        }
        if ((r = a.GetSwapChainPtr(fg_, IID_PPV_ARGS(&sc_))) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            err = Fmt(L"XeSS frame generation gave no swap chain (result %d).", r);
            return false;
        }
        if (!MakeTextures(w, h, fmt, err)) return false;
        if ((r = a.SetEnabled(fg_, 1)) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            err = Fmt(L"XeSS frame generation didn't switch on: %s (result %d).", xefg::ResultText(r), r);
            return false;
        }
        reset_ = true;
        return true;
    }

    bool MakeTextures(UINT w, UINT h, DXGI_FORMAT fmt, std::wstring& err) {
        ComPtr<ID3D11Device> dev11;
        ctx4_->GetDevice(&dev11);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &tex11_);
        ComPtr<IDXGIResource1> r1;
        HANDLE th = nullptr;
        if (SUCCEEDED(hr)) hr = tex11_.As(&r1);
        if (SUCCEEDED(hr))
            hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &th);
        if (SUCCEEDED(hr)) {
            hr = dev12_->OpenSharedHandle(th, IID_PPV_ARGS(&tex12_));
            CloseHandle(th);
        }
        if (FAILED(hr)) {
            err = Fmt(L"The texture shared between Direct3D 11 and 12 failed (0x%08X).", (unsigned)hr);
            return false;
        }
        // Default heap textures start zeroed: no motion, and every pixel at the same depth.
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Format = DXGI_FORMAT_R16G16_FLOAT;
        hr = dev12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             nullptr, IID_PPV_ARGS(&mv_));
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        if (SUCCEEDED(hr))
            hr = dev12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&depth_));
        if (FAILED(hr)) {
            err = Fmt(L"The motion vector and depth textures failed (0x%08X).", (unsigned)hr);
            return false;
        }
        w_ = w;
        h_ = h;
        return true;
    }

    void DropTextures() {
        tex11_.Reset();
        tex12_.Reset();
        mv_.Reset();
        depth_.Reset();
    }

    void WaitCpu(UINT64 v) {
        if (!fence12_ || fence12_->GetCompletedValue() >= v) return;
        fence12_->SetEventOnCompletion(v, ev_);
        WaitForSingleObject(ev_, 1000);
    }

    void WaitIdle() {
        queue_->Signal(fence12_.Get(), ++fv_);
        WaitCpu(fv_);
    }

    ComPtr<ID3D12Device> dev12_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> alloc_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence12_;
    ComPtr<ID3D11Fence> fence11_;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    ComPtr<IDXGISwapChain4> sc_;
    ComPtr<ID3D11Texture2D> tex11_;
    ComPtr<ID3D12Resource> tex12_, mv_, depth_;
    xefg_swapchain_handle_t fg_ = nullptr;
    xell_context_handle_t xell_ = nullptr;
    HANDLE ev_ = nullptr;
    UINT64 fv_ = 0, lastCopy_ = 0;
    LONGLONG lastT_ = 0;
    uint32_t id_ = 0, shown_ = 0, w_ = 0, h_ = 0;
    int lastResult_ = 0, resultLogs_ = 0, rateLogs_ = 0;
    uint32_t lastShown_ = 0;
    bool reset_ = true;
};

class Renderer {
public:
    bool Init(HWND hwnd) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // Render on the GPU that drives the window's monitor, so frames don't cross GPUs (and the NVIDIA
        // driver's Smooth Motion sees the swap chain). Fall back to the high performance GPU, then the default.
        std::vector<ComPtr<IDXGIAdapter1>> tries;
        ComPtr<IDXGIFactory1> fac;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
            ComPtr<IDXGIAdapter1> a;
            for (UINT i = 0; SUCCEEDED(fac->EnumAdapters1(i, &a)); i++, a.Reset()) {
                ComPtr<IDXGIOutput> o;
                for (UINT j = 0; SUCCEEDED(a->EnumOutputs(j, &o)); j++, o.Reset()) {
                    DXGI_OUTPUT_DESC od;
                    if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) tries.push_back(a);
                }
            }
            ComPtr<IDXGIFactory6> f6;
            ComPtr<IDXGIAdapter1> hp;
            if (SUCCEEDED(fac.As(&f6)) &&
                SUCCEEDED(f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&hp))))
                tries.push_back(hp);
        }
        tries.push_back(nullptr);
        HRESULT hr = E_FAIL;
        for (auto& a : tries) {
            dev_.Reset();
            ctx_.Reset();
            hr = D3D11CreateDevice(a.Get(), a ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2,
                                   D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
            ComPtr<ID3D11VideoDevice> vd;
            if (SUCCEEDED(hr) && SUCCEEDED(dev_.As(&vd))) break;
            if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
        }
        if (FAILED(hr)) {
            Log(L"D3D11CreateDevice failed 0x%08X", hr);
            return false;
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        if (FAILED(dev_.As(&vdev_)) || FAILED(ctx_.As(&vctx_))) {
            Log(L"GPU has no D3D11 video support");
            return false;
        }
        ctx_.As(&vctx1_);
        StartFgCompile();
        hr = MFCreateDXGIDeviceManager(&resetToken_, &mgr_);
        if (SUCCEEDED(hr)) hr = mgr_->ResetDevice(dev_.Get(), resetToken_);
        if (FAILED(hr)) Log(L"DXGI device manager failed 0x%08X, decoding stays on the CPU", hr);

        ComPtr<IDXGIDevice1> dd;
        dev_.As(&dd);
        dd->SetMaximumFrameLatency(1);
        ComPtr<IDXGIAdapter> ad;
        dd->GetAdapter(&ad);
        DXGI_ADAPTER_DESC adesc = {};
        ad->GetDesc(&adesc);
        adapterName_ = adesc.Description;
        ComPtr<IDXGIFactory2> f;
        ad->GetParent(IID_PPV_ARGS(&f));
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(f.As(&f5))) {
            BOOL t = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t)))) tearing_ = t != FALSE;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = std::max<LONG>(1, rc.right - rc.left);
        d.Height = std::max<LONG>(1, rc.bottom - rc.top);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        d.Flags = scFlags_;
        hr = f->CreateSwapChainForHwnd(dev_.Get(), hwnd, &d, nullptr, nullptr, &sc_);
        if (FAILED(hr)) {
            Log(L"CreateSwapChainForHwnd failed 0x%08X", hr);
            return false;
        }
        f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(sc_.As(&sc2))) {
            sc2->SetMaximumFrameLatency(1);
            waitable_ = sc2->GetFrameLatencyWaitableObject();
        }
        bbW_ = d.Width;
        bbH_ = d.Height;
        hwnd_ = hwnd;
        {
            LARGE_INTEGER fq;
            QueryPerformanceFrequency(&fq);
            qpfStats_ = (double)fq.QuadPart;
        }
        ready_ = true;
        Log(L"GPU: %s, tearing %s", adapterName_.c_str(), tearing_ ? L"supported" : L"not supported");
        return true;
    }

    bool Ready() const { return ready_; }
    IMFDXGIDeviceManager* Manager() const { return mgr_.Get(); }
    const std::wstring& AdapterName() const { return adapterName_; }

    bool SupportsInput(DXGI_FORMAT fmt, UINT w, UINT h) {
        std::lock_guard<std::mutex> l(m_);
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {60, 1};
        cd.InputWidth = w;
        cd.InputHeight = h;
        cd.OutputFrameRate = {60, 1};
        cd.OutputWidth = bbW_;
        cd.OutputHeight = bbH_;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        ComPtr<ID3D11VideoProcessorEnumerator> e;
        if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &e))) return false;
        UINT fl = 0;
        return SUCCEEDED(e->CheckVideoProcessorFormat(fmt, &fl)) && (fl & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT);
    }

    void Resize(UINT w, UINT h) {
        std::lock_guard<std::mutex> l(m_);
        if (!ready_ || w == 0 || h == 0 || (w == bbW_ && h == bbH_)) return;
        ReleaseOverlayTarget();
        DropCachedViews();
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();
        HRESULT hr = xo_ ? xo_->Resize(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, scFlags_)
                         : sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, scFlags_);
        if (FAILED(hr) && xo_) {
            // A dxgi.dll wrapper (RTX Encore with some of its present features on) can hold the
            // native back buffers, and then XeSS can't resize them. Building XeSS again at the
            // new size gets around that; if even that fails, the D3D11 swap chain comes back.
            Log(L"XeSS frame generation couldn't resize (0x%08X), starting it again at %ux%u", (unsigned)hr, w, h);
            std::wstring err;
            if (!XessSwitchLocked(false, err) || !XessSwitchLocked(true, err)) Log(L"XeSS frame generation lost: %s", err.c_str());
            return;
        }
        if (FAILED(hr)) {
            Log(L"ResizeBuffers failed 0x%08X", hr);
            return;
        }
        bbW_ = w;
        bbH_ = h;
    }

    // Takes or gives up exclusive fullscreen on the window's monitor. The swap chain has no
    // ALLOW_MODE_SWITCH flag, so the display keeps its desktop mode and refresh rate. DXGI sends
    // WM_SIZE while it switches; the window procedure skips it (g_modeSwitch), since this holds m_,
    // and the buffers are resized here instead.
    // DXGI refuses SetFullscreenState for a swap chain made with FRAME_LATENCY_WAITABLE_OBJECT
    // (DXGI_ERROR_INVALID_CALL), so exclusive mode runs on a swap chain made without it, and
    // leaving exclusive mode makes the waitable one again. On WM_DESTROY there's no point in
    // making a new one, so the caller passes restore = false.
    void SetExclusive(bool on, bool restore = true) {
        std::lock_guard<std::mutex> l(m_);
        if (!ready_ || on == exclusive_) return;
        if (on && xo_) {
            Log(L"Exclusive fullscreen stays off, XeSS frame generation only runs in a window or borderless fullscreen");
            return;
        }
        ReleaseOverlayTarget();
        DropCachedViews();
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();
        if (on && !RecreateSwapChain(false)) return;
        g_modeSwitch = true;
        HRESULT hr = sc_->SetFullscreenState(on ? TRUE : FALSE, nullptr);
        g_modeSwitch = false;
        BOOL fs = FALSE;
        sc_->GetFullscreenState(&fs, nullptr);
        exclusive_ = fs != FALSE;
        if (FAILED(hr)) Log(L"SetFullscreenState(%s) failed 0x%08X", on ? L"exclusive" : L"windowed", (unsigned)hr);
        else Log(L"Exclusive fullscreen %s", exclusive_ ? L"on" : L"off");
        if (!exclusive_ && restore) {
            RecreateSwapChain(true);
            return;
        }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        UINT w = std::max<LONG>(1, rc.right - rc.left), h = std::max<LONG>(1, rc.bottom - rc.top);
        hr = sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, scFlags_);
        if (FAILED(hr)) {
            Log(L"ResizeBuffers after the fullscreen switch failed 0x%08X", (unsigned)hr);
            return;
        }
        bbW_ = w;
        bbH_ = h;
    }
    bool Exclusive() const { return exclusive_; }

    // Moves the window's output to XeSS frame generation's proxy swap chain, or back to the plain
    // D3D11 swap chain. Only one swap chain can hold a window, so the old one goes first. If XeSS
    // doesn't start, err says why and the D3D11 swap chain comes back.
    bool SetXess(bool on, std::wstring& err) {
        std::lock_guard<std::mutex> l(m_);
        return XessSwitchLocked(on, err);
    }
    bool XessOn() const { return xo_ != nullptr; }

private:
    bool XessSwitchLocked(bool on, std::wstring& err) {
        if (on == (xo_ != nullptr)) return true;
        if (on && exclusive_) {
            err = L"CapView is in exclusive fullscreen.";
            return false;
        }
        ComPtr<IDXGIDevice1> dd;
        ComPtr<IDXGIAdapter> ad;
        ComPtr<IDXGIFactory2> f;
        if (FAILED(dev_.As(&dd)) || FAILED(dd->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&f)))) {
            err = L"CapView can't reach the DXGI factory.";
            return false;
        }
        ReleaseOverlayTarget();
        DropCachedViews();
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        if (waitable_) {
            CloseHandle(waitable_);
            waitable_ = nullptr;
        }
        sc_.Reset();
        ctx_->ClearState();
        ctx_->Flush();
        prevValid_ = false;
        lastPc_ = 0;
        lastSync_ = 0;
        if (on) {
            if (hdrOut_) {  // XeSS mode draws SDR; HDR comes back with the D3D11 swap chain
                hdrOut_ = false;
                g_hdrOut = 0;
                vp_.Reset();
                vpe_.Reset();
                fgW_ = fgH_ = 0;
            }
            RECT rc;
            GetClientRect(hwnd_, &rc);
            UINT w = std::max<LONG>(1, rc.right - rc.left), h = std::max<LONG>(1, rc.bottom - rc.top);
            auto x = std::make_unique<XessOut>();
            UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            if (x->Init(dev_.Get(), f.Get(), hwnd_, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, flags, err)) {
                xo_ = std::move(x);
                sc_ = xo_->SwapChain();
                scFlags_ = flags;
                dd->SetMaximumFrameLatency(1);
                f->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
                bbW_ = w;
                bbH_ = h;
                ready_ = true;
                Log(L"XeSS frame generation on, proxy swap chain %ux%u", w, h);
                return true;
            }
            Log(L"XeSS frame generation didn't start: %s", err.c_str());
        } else {
            xo_.reset();
            Log(L"XeSS frame generation off");
        }
        RecreateSwapChain(true);
        return !on;
    }

public:

    void PresentBlack() {
        std::lock_guard<std::mutex> l(m_);
        if (!ready_) return;
        WaitFrame();
        ComPtr<ID3D11RenderTargetView> rtv = BackRtv();
        if (!rtv) return;
        const float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(rtv.Get(), black);
        Present();
    }

    // ts is the sample time in 100 ns units. Its offset from the clock is unknown, so the queue
    // delay is measured against the least delayed frame since capture started.
    void PresentFrame(IMFSample* sample, const FrameFormat& f, LONGLONG ts) {
        std::lock_guard<std::mutex> l(m_);
        if (!ready_) return;
        g_framesIn++;
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        TimingFrame(t0.QuadPart, ts);
        struct EndWork {
            Renderer* r;
            LONGLONG t0;
            double w0;
            ~EndWork() {
                LARGE_INTEGER t1;
                QueryPerformanceCounter(&t1);
                double w = (double)(t1.QuadPart - t0) - (r->tmWaitAcc_ - w0);
                r->tmWork_ += w;
                r->tmWorkMax_ = std::max(r->tmWorkMax_, w);
            }
        } endWork{this, t0.QuadPart, tmWaitAcc_};
        SlowStep slowAll(L"PresentFrame in total");
        int srcHdr = f.transfer == MFVideoTransFunc_2084 ? 1 : f.transfer == MFVideoTransFunc_HLG ? 2 : 0;
        g_srcHdr = srcHdr;
        bool wantHdr = srcHdr && g_displayHdr && g_hdrAllow && vctx1_ && !hdrVpFail_ && !xo_;  // XeSS mode is SDR
        if (wantHdr != hdrOut_) {
            SlowStep s(L"SetHdr");
            if (!SetHdr(wantHdr) && wantHdr) hdrVpFail_ = true;
        }
        int mode = g_fgMode;
        // Frame generators don't stack. While another frame generator runs (Smooth Motion, RTX Encore's
        // Smooth Motion in any mode, AMD Fluid Motion Frames or XeSS), CapView draws each new picture
        // once, so it interpolates game frames, not repeated signal frames, and CapView's own
        // in-between frames wait.
        bool smoothOn = mode == FG_SMOOTH || mode == FG_AMD || (mode == FG_XESS && xo_) ||
                        g_encoreSmoothOn;
        bool stackBlocked = mode == FG_CAPVIEW && smoothOn;
        bool fg;
        {
            SlowStep s(L"FgReady");
            fg = (mode == FG_CAPVIEW || smoothOn) && FgReady();
        }
        if (!fg) {
            prevValid_ = false;
            g_fgActive = false;
            WaitFrame();
        }

        ComPtr<IMFMediaBuffer> buf;
        DWORD count = 0;
        sample->GetBufferCount(&count);
        HRESULT hr = count == 1 ? sample->GetBufferByIndex(0, &buf) : sample->ConvertToContiguousBuffer(&buf);
        if (FAILED(hr)) return;

        ComPtr<ID3D11Texture2D> tex;
        UINT sub = 0;
        ComPtr<IMFDXGIBuffer> dx;
        if (SUCCEEDED(buf.As(&dx)) && SUCCEEDED(dx->GetResource(IID_PPV_ARGS(&tex)))) {
            dx->GetSubresourceIndex(&sub);
            gpuPath_ = true;
            SlowStep s(L"DiffGpu");
            DiffGpu(tex.Get(), sub, f);
        } else {
            if (!Upload(buf.Get(), f)) return;
            tex = upload_;
            sub = 0;
            gpuPath_ = false;
        }
        if (loggedPath_ != (gpuPath_ ? 2 : 1)) {
            loggedPath_ = gpuPath_ ? 2 : 1;
            Log(L"Frames arrive in %s memory", gpuPath_ ? L"GPU" : L"system");
        }
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        {
            SlowStep s(L"EnsureProcessor");
            if (!EnsureProcessor(td.Format, f)) return;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.ArraySlice = sub;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        hr = vdev_->CreateVideoProcessorInputView(tex.Get(), vpe_.Get(), &ivd, &iv);
        if (FAILED(hr)) {
            LogOnce(L"CreateVideoProcessorInputView failed", hr);
            return;
        }

        // Letterbox to the display aspect ratio of the source.
        double srcAspect = (double)f.w * f.parN / ((double)f.h * f.parD);
        double dstAspect = (double)bbW_ / bbH_;
        RECT dst;
        if (dstAspect > srcAspect) {
            LONG w = (LONG)std::lround(bbH_ * srcAspect);
            dst = {(LONG)(bbW_ - w) / 2, 0, (LONG)(bbW_ - w) / 2 + w, (LONG)bbH_};
        } else {
            LONG h = (LONG)std::lround(bbW_ / srcAspect);
            dst = {0, (LONG)(bbH_ - h) / 2, (LONG)bbW_, (LONG)(bbH_ - h) / 2 + h};
        }
        if (fg) {
            SlowStep s(L"SmoothFrame or FgFrame");
            if (mode != FG_CAPVIEW || stackBlocked ? SmoothFrame(iv.Get(), f, dst) : FgFrame(iv.Get(), f, dst)) return;
            prevValid_ = false;
            g_fgActive = false;
            WaitFrame();
        }

        ComPtr<ID3D11Texture2D> bb;
        if (!BackTex(bb)) return;
        // The output view is kept while the back buffer texture and the video processor stay the same.
        if (!bbOv_ || bbOvTex_ != bb || bbOvVpe_ != vpe_) {
            bbOv_.Reset();
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            hr = vdev_->CreateVideoProcessorOutputView(bb.Get(), vpe_.Get(), &ovd, &bbOv_);
            if (FAILED(hr)) {
                LogOnce(L"CreateVideoProcessorOutputView failed", hr);
                return;
            }
            bbOvTex_ = bb;
            bbOvVpe_ = vpe_;
        }
        ID3D11VideoProcessorOutputView* ov = bbOv_.Get();
        RECT src = {0, 0, (LONG)f.w, (LONG)f.h};
        RECT full = {0, 0, (LONG)bbW_, (LONG)bbH_};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
        vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
        vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);

        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = iv.Get();
        hr = vctx_->VideoProcessorBlt(vp_.Get(), ov, frameIndex_++, 1, &st);
        if (FAILED(hr)) {
            LogOnce(L"VideoProcessorBlt failed", hr);
            return;
        }
        Present();
        g_framesPresented++;
    }

    bool GpuPath() const { return gpuPath_; }

private:
    // ---- CapView frame generation -------------------------------------------------------------
    struct FlowCB {
        float lvlW, lvlH, invW, invH;
        int fieldW, fieldH, parentW, parentH;
        int radius, hasParent;
        float lambda, pad;
    };
    struct DrawCB {
        float orgX, orgY, invDW, invDH;
        float invFW, invFH, t;
        int mode;
    };
    static const int kLevels = 4;
    static const int kSlots = 3;

    static bool Compile(const char* src, size_t len, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(src, len, "capview_fg", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                0, &out, &err);
        if (FAILED(hr)) {
            Log(L"Shader %S failed 0x%08X: %S", entry, hr, err ? (const char*)err->GetBufferPointer() : "");
            return false;
        }
        return true;
    }

    // D3DCompile of the seven frame generation shaders takes about 0.9 s. On the first captured frame
    // that stalled the picture for over a second at every start, in Smooth Motion mode too, so they
    // compile on their own thread from Init and FgReady reports false until they're done.
    struct FgBlobs {
        ComPtr<ID3DBlob> b[7];
        std::atomic<bool> done{false};
        bool ok = false;
        double ms = 0;
    };
    void StartFgCompile() {
        if (fgBlobs_) return;
        fgBlobs_ = std::make_shared<FgBlobs>();
        std::thread([j = fgBlobs_] {
            LARGE_INTEGER t0, t1, fq;
            QueryPerformanceCounter(&t0);
            const char* cs[5] = {"CsLuma", "CsDown", "CsFlow", "CsMedian", "CsDiff"};
            bool ok = true;
            for (int i = 0; i < 5 && ok; i++) ok = Compile(kFlowHlsl, sizeof(kFlowHlsl) - 1, cs[i], "cs_5_0", j->b[i]);
            ok = ok && Compile(kDrawHlsl, sizeof(kDrawHlsl) - 1, "VsTri", "vs_5_0", j->b[5]);
            ok = ok && Compile(kDrawHlsl, sizeof(kDrawHlsl) - 1, "PsDraw", "ps_5_0", j->b[6]);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&fq);
            j->ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
            j->ok = ok;
            j->done = true;
        }).detach();
    }

    bool FgReady() {
        if (fgInit_) return fgOk_;
        if (!fgBlobs_) StartFgCompile();
        if (!fgBlobs_->done) return false;
        fgInit_ = true;
        auto& bl = fgBlobs_->b;
        if (!fgBlobs_->ok) return false;
        ComPtr<ID3D11ComputeShader>* dst[5] = {&csLuma_, &csDown_, &csFlow_, &csMedian_, &csDiff_};
        for (int i = 0; i < 5; i++)
            if (FAILED(dev_->CreateComputeShader(bl[i]->GetBufferPointer(), bl[i]->GetBufferSize(), nullptr,
                                                 dst[i]->GetAddressOf())))
                return false;
        if (FAILED(dev_->CreateVertexShader(bl[5]->GetBufferPointer(), bl[5]->GetBufferSize(), nullptr, &vs_)))
            return false;
        if (FAILED(dev_->CreatePixelShader(bl[6]->GetBufferPointer(), bl[6]->GetBufferSize(), nullptr, &ps_)))
            return false;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev_->CreateSamplerState(&sd, &samp_))) return false;
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.ByteWidth = sizeof(FlowCB);
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &flowCb_))) return false;
        bd.ByteWidth = sizeof(DrawCB);
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &drawCb_))) return false;
        D3D11_BUFFER_DESC db = {};
        db.ByteWidth = 12;
        db.Usage = D3D11_USAGE_DEFAULT;
        db.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        db.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        db.StructureByteStride = 4;
        if (FAILED(dev_->CreateBuffer(&db, nullptr, &diffBuf_))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 3;
        if (FAILED(dev_->CreateUnorderedAccessView(diffBuf_.Get(), &ud, &diffUav_))) return false;
        db.Usage = D3D11_USAGE_STAGING;
        db.BindFlags = 0;
        db.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateBuffer(&db, nullptr, &diffRead_))) return false;
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        qpf_ = (double)fq.QuadPart;
        fgOk_ = true;
        Log(L"Frame generation shaders ready, compiled in %.0f ms off the capture thread", fgBlobs_->ms);
        return true;
    }

    bool MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, ComPtr<ID3D11Texture2D>& t,
                 ComPtr<ID3D11ShaderResourceView>& srv, ComPtr<ID3D11UnorderedAccessView>* uav) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = std::max(1u, w);
        td.Height = std::max(1u, h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t))) return false;
        if (FAILED(dev_->CreateShaderResourceView(t.Get(), nullptr, &srv))) return false;
        if (uav && FAILED(dev_->CreateUnorderedAccessView(t.Get(), nullptr, uav->GetAddressOf()))) return false;
        return true;
    }

    bool EnsureFgTex(UINT w, UINT h) {
        if (fgW_ == w && fgH_ == h) return true;
        fgW_ = fgH_ = 0;
        prevValid_ = false;
        exact_.Reset();
        for (auto& o : fgOv_) o.Reset();
        UINT lw = (w + 1) / 2, lh = (h + 1) / 2;
        for (int k = 0; k < kLevels; k++) {
            lumW_[k] = lw;
            lumH_[k] = lh;
            fldW_[k] = (lw + 3) / 4;
            fldH_[k] = (lh + 3) / 4;
            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
        }
        for (int i = 0; i < kSlots; i++) {
            if (!MakeTex(w, h, hdrOut_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET,
                         rgb_[i], rgbSrv_[i], nullptr))
                return false;
            for (int k = 0; k < kLevels; k++)
                if (!MakeTex(lumW_[k], lumH_[k], DXGI_FORMAT_R16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, lum_[i][k], lumSrv_[i][k],
                             &lumUav_[i][k]))
                    return false;
        }
        for (int k = 0; k < kLevels; k++)
            if (!MakeTex(fldW_[k], fldH_[k], DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, fld_[k], fldSrv_[k], &fldUav_[k]))
                return false;
        if (!MakeTex(fldW_[0], fldH_[0], DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, med_, medSrv_, &medUav_))
            return false;
        if (!MakeTex(lumW_[0], lumH_[0], DXGI_FORMAT_R16_FLOAT, 0, lumLast_, lumLastSrv_, nullptr)) return false;
        lastValid_ = false;
        fgW_ = w;
        fgH_ = h;
        Log(L"Frame generation buffers %ux%u, motion field %ux%u", w, h, fldW_[0], fldH_[0]);
        return true;
    }

    void Dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT srvStart, UINT srvCount,
                  ID3D11UnorderedAccessView* uav, UINT uavSlot, UINT w, UINT h) {
        ctx_->CSSetShader(cs, nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, flowCb_.GetAddressOf());
        ctx_->CSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->CSSetShaderResources(srvStart, srvCount, srvs);
        ctx_->CSSetUnorderedAccessViews(uavSlot, 1, &uav, nullptr);
        ctx_->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* nulls[6] = {};
        ID3D11UnorderedAccessView* nullu = nullptr;
        ctx_->CSSetShaderResources(srvStart, srvCount, nulls);
        ctx_->CSSetUnorderedAccessViews(uavSlot, 1, &nullu, nullptr);
    }

    // Luma level 0 is all the repeat check needs, so every capture gets only that. The coarser
    // levels feed the motion search and are built for new content frames only (BuildCoarse).
    void BuildLuma(int c) {
        ID3D11ShaderResourceView* s = rgbSrv_[c].Get();
        Dispatch(csLuma_.Get(), &s, 0, 1, lumUav_[c][0].Get(), 0, lumW_[0], lumH_[0]);
    }

    void BuildCoarse(int c) {
        for (int k = 1; k < kLevels; k++) {
            ID3D11ShaderResourceView* s = lumSrv_[c][k - 1].Get();
            Dispatch(csDown_.Get(), &s, 1, 1, lumUav_[c][k].Get(), 0, lumW_[k], lumH_[k]);
        }
    }

    void ComputeFlow(int p, int c) {
        for (int k = kLevels - 1; k >= 0; k--) {
            bool top = k == kLevels - 1;
            FlowCB cb = {(float)lumW_[k], (float)lumH_[k], 1.0f / lumW_[k], 1.0f / lumH_[k], (int)fldW_[k], (int)fldH_[k],
                         top ? 1 : (int)fldW_[k + 1], top ? 1 : (int)fldH_[k + 1], top ? 8 : 2, top ? 0 : 1,
                         top ? 0.002f : 0.004f, 0};
            ctx_->UpdateSubresource(flowCb_.Get(), 0, nullptr, &cb, 0, 0);
            ID3D11ShaderResourceView* s[3] = {lumSrv_[p][k].Get(), lumSrv_[c][k].Get(), top ? nullptr : fldSrv_[k + 1].Get()};
            Dispatch(csFlow_.Get(), s, 2, 3, fldUav_[k].Get(), 1, fldW_[k], fldH_[k]);
        }
        FlowCB cb = {};
        cb.fieldW = (int)fldW_[0];
        cb.fieldH = (int)fldH_[0];
        ctx_->UpdateSubresource(flowCb_.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11ShaderResourceView* s = fldSrv_[0].Get();
        Dispatch(csMedian_.Get(), &s, 5, 1, medUav_.Get(), 1, fldW_[0], fldH_[0]);
    }

    // mode 0 shows the current frame, mode 1 an interpolated one at time t between prev and cur.
    void DrawFg(int mode, float t, int p, int c, const RECT& dst) {
        WaitFrame();
        ComPtr<ID3D11RenderTargetView> rtv = BackRtv();
        if (!rtv) return;
        const float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(rtv.Get(), black);
        float dw = (float)std::max(1L, dst.right - dst.left), dh = (float)std::max(1L, dst.bottom - dst.top);
        DrawCB cb = {(float)dst.left, (float)dst.top, 1.0f / dw, 1.0f / dh, 1.0f / lumW_[0], 1.0f / lumH_[0], t, mode};
        ctx_->UpdateSubresource(drawCb_.Get(), 0, nullptr, &cb, 0, 0);
        D3D11_VIEWPORT vp = {(float)dst.left, (float)dst.top, dw, dh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(nullptr);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->OMSetDepthStencilState(nullptr, 0);
        ctx_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, drawCb_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ID3D11ShaderResourceView* s[3] = {rgbSrv_[p].Get(), rgbSrv_[c].Get(), medSrv_.Get()};
        ctx_->PSSetShaderResources(0, 3, s);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = {};
        ctx_->PSSetShaderResources(0, 3, nulls);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        Present();
        g_framesPresented++;
    }

    void WaitUntil(LONGLONG target) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        for (;;) {
            LARGE_INTEGER n;
            QueryPerformanceCounter(&n);
            double ms = (target - n.QuadPart) * 1000.0 / qpf_;
            if (ms <= 0) {
                tmWaitAcc_ += (double)(n.QuadPart - start.QuadPart);
                return;
            }
            if (ms > 1.5) {
                if (timer_) {
                    LARGE_INTEGER due;
                    due.QuadPart = -(LONGLONG)((ms - 1.0) * 10000.0);
                    SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
                    WaitForSingleObject(timer_, 50);
                } else if (ms > 2.5) {
                    Sleep(1);
                }
            } else {
                YieldProcessor();
            }
        }
    }

    // True when the capture in slot s has a different picture from the latest content frame in
    // slot b. HDMI repeats the last frame when the game renders slower than the signal, so 8
    // changed half-resolution luma samples is enough to call it new. ExactRepeats decides whether
    // a sample has to move past the noise margin or only has to change.
    bool Differs(int s, int b) {
        const UINT zero[4] = {};
        ctx_->ClearUnorderedAccessViewUint(diffUav_.Get(), zero);
        ID3D11ShaderResourceView* srv[3] = {lumSrv_[s][0].Get(), lumSrv_[b][0].Get(),
                                            lastValid_ ? lumLastSrv_.Get() : lumSrv_[s][0].Get()};
        Dispatch(csDiff_.Get(), srv, 1, 3, diffUav_.Get(), 2, lumW_[0], lumH_[0]);
        ctx_->CopyResource(lumLast_.Get(), lum_[s][0].Get());
        bool learn = lastValid_;
        lastValid_ = true;
        ctx_->CopyResource(diffRead_.Get(), diffBuf_.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(diffRead_.Get(), 0, D3D11_MAP_READ, 0, &m))) return true;
        const UINT* c = (const UINT*)m.pData;
        UINT margin = c[0], any = c[1], last = c[2];
        ctx_->Unmap(diffRead_.Get(), 0);
        bool was = exact_.on;
        if (learn) exact_.Learn(last == 0);
        bool isNew = exact_.Pick(margin, any) >= 8;
        if (exact_.on != was)
            Log(exact_.on ? L"Capture repeats are bit-exact, so any changed sample counts toward a new frame"
                          : L"No exact repeat in 120 captures, new frames need the 2.5 step margin again");
        if (isNew && margin < 8) g_smallChanges++;
        return isNew;
    }

    // Converts a capture to RGB in the spare slot (the one neither fgA_ nor fgB_ holds) and builds
    // its luma pyramid. Returns the slot, or -1 to fall back to the plain path.
    int FgCapture(ID3D11VideoProcessorInputView* iv, const FrameFormat& f) {
        if (!EnsureFgTex(f.w, f.h)) {
            LogOnce(L"Frame generation buffers failed", E_FAIL);
            return -1;
        }
        int s = 3 - fgA_ - fgB_;
        // Each slot keeps its output view until the buffers or the video processor change.
        if (fgOvVpe_ != vpe_) {
            for (auto& o : fgOv_) o.Reset();
            fgOvVpe_ = vpe_;
        }
        HRESULT hr;
        if (!fgOv_[s]) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            hr = vdev_->CreateVideoProcessorOutputView(rgb_[s].Get(), vpe_.Get(), &ovd, &fgOv_[s]);
            if (FAILED(hr)) {
                LogOnce(L"Frame generation output view failed", hr);
                return -1;
            }
        }
        ID3D11VideoProcessorOutputView* ov = fgOv_[s].Get();
        RECT full = {0, 0, (LONG)f.w, (LONG)f.h};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &full);
        vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &full);
        vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);
        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = iv;
        hr = vctx_->VideoProcessorBlt(vp_.Get(), ov, frameIndex_++, 1, &st);
        if (FAILED(hr)) {
            LogOnce(L"VideoProcessorBlt failed", hr);
            return -1;
        }
        BuildLuma(s);
        return s;
    }

    // NVIDIA Smooth Motion and AMD Fluid Motion Frames interpolate between the frames CapView presents.
    // Presenting the HDMI repeats of a 30 fps game would hand them pairs of identical pictures, so
    // only new content frames get presented, as they arrive. A still picture is presented again every
    // half second so the overlay keeps updating. Returns false to fall back to the plain path.
    bool SmoothFrame(ID3D11VideoProcessorInputView* iv, const FrameFormat& f, const RECT& dst) {
        int s = FgCapture(iv, f);
        if (s < 0) return false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        bool isNew = !prevValid_ || Differs(s, fgB_);
        if (isNew) {
            fgA_ = fgB_;
            fgB_ = s;
            tNew_ = now.QuadPart;
            showPending_ = true;
        }
        if (isNew || now.QuadPart - smoothLast_ > qpf_ / 2) {
            DrawFg(0, 0, fgA_, fgB_, dst);
            ShownNewest();
            smoothLast_ = now.QuadPart;
        }
        g_fgActive = false;
        prevValid_ = true;
        return true;
    }

    // Converts each capture into the spare slot and checks it against the latest content frame.
    // A new content frame starts a schedule of M output frames spread across the game's own frame
    // interval: M-1 in-between frames from the older to the newer content frame, then the newer
    // one. Repeats only drain that schedule. Returns false to fall back to the plain path.
    bool FgFrame(ID3D11VideoProcessorInputView* iv, const FrameFormat& f, const RECT& dst) {
        int s = FgCapture(iv, f);
        if (s < 0) return false;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double period = (double)f.fpsD / f.fpsN * qpf_;
        if (!prevValid_) {
            nGaps_ = 0;
            pairValid_ = false;
        }
        bool isNew = !prevValid_ || Differs(s, fgB_);
        if (isNew) {
            double gap = (double)(now.QuadPart - tNew_);
            tNew_ = now.QuadPart;
            fgA_ = fgB_;
            fgB_ = s;
            showPending_ = true;
            BuildCoarse(s);
            pairValid_ = prevValid_ && gap < period * 4.5;
            if (pairValid_) {
                // The content interval in whole signal periods, averaged over the last 4 so a
                // 30 fps game that sometimes drops to 20 doesn't throw the spacing around.
                gaps_[gapPos_] = std::max(1.0, std::floor(gap / period + 0.5));
                gapPos_ = (gapPos_ + 1) % 4;
                nGaps_ = std::min(4, nGaps_ + 1);
                double sum = 0;
                for (int i = 0; i < nGaps_; i++) sum += gaps_[i];
                schedT_ = sum / nGaps_ * period;
                schedM_ = std::min(4, std::max(2, g_fgMult.load()));
                schedK_ = 1;
                schedT0_ = (double)now.QuadPart;
                ComputeFlow(fgA_, fgB_);
            } else {
                nGaps_ = 0;
                schedK_ = schedM_ + 1;
                DrawFg(0, 0, fgA_, fgB_, dst);
                ShownNewest();
                lastMode_ = 0;
                lastT_ = 0;
            }
        }
        bool drew = isNew && !pairValid_;
        // Interpolation shows M-1 in-between frames and then the new frame, so the new frame waits
        // (M-1)/M of the content interval. Extrapolation shows the new frame at once and then
        // carries it on along its motion for the rest of the interval.
        bool extrap = g_fgPredict;
        while (pairValid_ && schedK_ <= schedM_) {
            double tk = schedT0_ + schedT_ * (schedK_ - 1) / schedM_;
            if (tk > (double)now.QuadPart + period * 0.75) break;
            WaitUntil((LONGLONG)tk);
            if (extrap) {
                lastMode_ = schedK_ == 1 ? 0 : 2;
                lastT_ = (float)(schedK_ - 1) / schedM_;
            } else {
                lastMode_ = schedK_ < schedM_ ? 1 : 0;
                lastT_ = lastMode_ ? (float)schedK_ / schedM_ : 0.0f;
            }
            DrawFg(lastMode_, lastT_, fgA_, fgB_, dst);
            if (lastMode_ == 0) ShownNewest();
            schedK_++;
            drew = true;
        }
        // A still picture leaves nothing scheduled. Present the last output again so the overlay
        // and the output counter keep updating. An extrapolated frame would hold the picture past
        // where the game left it, so a still picture goes back to the real frame.
        if (!drew && schedK_ > schedM_) {
            if (lastMode_ == 2) lastMode_ = 0, lastT_ = 0;
            DrawFg(lastMode_, lastT_, fgA_, fgB_, dst);
        }
        g_fgActive = pairValid_;
        prevValid_ = true;
        return true;
    }

    // Waits for the swap chain to take another frame, and counts the time for the timing log.
    void WaitFrame() {
        if (!waitable_) return;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        WaitForSingleObjectEx(waitable_, 100, FALSE);
        QueryPerformanceCounter(&b);
        tmWaitAcc_ += (double)(b.QuadPart - a.QuadPart);
    }

    // Frame generation: time from a new content frame's capture to the present that shows it.
    void ShownNewest() {
        if (!showPending_) return;
        showPending_ = false;
        LARGE_INTEGER n;
        QueryPerformanceCounter(&n);
        tmShow_ += (double)(n.QuadPart - tNew_);
        tmShowN_++;
    }

    // The current back buffer's render target view. The view is kept while GetBuffer keeps
    // returning the same texture, which it does for a flip model swap chain in D3D11. A wrapper
    // that hands out a different texture gets a new view.
    ComPtr<ID3D11RenderTargetView> BackRtv() {
        ComPtr<ID3D11Texture2D> bb;
        if (!BackTex(bb)) return nullptr;
        if (rtv_ && rtvBb_ == bb) return rtv_;
        if (rtvBb_ && !rtvSwapLogged_) {
            rtvSwapLogged_ = true;
            Log(L"Back buffer texture changes between presents; its view is made again each time");
        }
        rtv_.Reset();
        if (FAILED(dev_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_))) {
            rtvBb_.Reset();
            return nullptr;
        }
        rtvBb_ = bb;
        return rtv_;
    }

    // The texture the next frame is drawn into: the swap chain's back buffer, or in XeSS mode the
    // texture that the D3D12 side copies into the proxy swap chain.
    bool BackTex(ComPtr<ID3D11Texture2D>& t) {
        if (xo_) {
            t = xo_->Target();
            return t != nullptr;
        }
        return sc_ && SUCCEEDED(sc_->GetBuffer(0, IID_PPV_ARGS(&t)));
    }

    void DropCachedViews() {
        rtv_.Reset();
        rtvBb_.Reset();
        bbOv_.Reset();
        bbOvTex_.Reset();
        bbOvVpe_.Reset();
    }

    // Per capture: queue delay, and the timing log line every 30 seconds.
    void TimingFrame(LONGLONG now, LONGLONG ts) {
        double nowMs = now * 1000.0 / qpfStats_;
        double age = nowMs - ts / 10000.0;
        if (!tmHaveMin_ || age < tmMinAge_) {
            tmMinAge_ = age;
            tmHaveMin_ = true;
        }
        double q = age - tmMinAge_;
        if (q > 1000) {  // the timestamps restarted
            tmMinAge_ = age;
            q = 0;
        }
        tmQueue_ += q;
        tmQueueMax_ = std::max(tmQueueMax_, q);
        tmN_++;
        if (!tmT0_) tmT0_ = now;
        double secs = (now - tmT0_) / qpfStats_;
        if (secs < g_timingSecs) return;
        double toMs = 1000.0 / qpfStats_;
        Log(L"Video timing %.0f s: %d captures, capture queue avg %.1f max %.1f ms, swap chain wait avg %.1f ms, "
            L"CapView work avg %.2f max %.2f ms, new frame shown after avg %.1f ms (%d new frames)",
            secs, tmN_, tmQueue_ / tmN_, tmQueueMax_, tmWaitAcc_ * toMs / tmN_, tmWork_ * toMs / tmN_,
            tmWorkMax_ * toMs, tmShowN_ ? tmShow_ * 1000.0 / qpf_ / tmShowN_ : 0.0, tmShowN_);
        tmT0_ = now;
        tmN_ = tmShowN_ = 0;
        tmQueue_ = tmQueueMax_ = tmWaitAcc_ = tmWork_ = tmWorkMax_ = tmShow_ = 0;
    }

    void Present() {
        if (g_showFps) {
            SlowStep s(L"DrawOverlay");
            DrawOverlay();
        }
        bool vs = g_vsync.load();
        // DXGI refuses ALLOW_TEARING in exclusive fullscreen, where sync interval 0 tears anyway.
        UINT pf = (!vs && tearing_ && !exclusive_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr;
        {
            SlowStep s(L"swap chain Present");
            if (xo_) xo_->BeforePresent();
            hr = sc_->Present(vs ? 1 : 0, pf);
            if (xo_) xo_->AfterPresent();
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) LogOnce(L"GPU device lost", hr);
        g_presents++;
        CollectStats();
    }

    // Reads which present the display last showed. PresentCount only moves when a frame really
    // reached the screen, so its growth is the output frame rate, and the time between two moves
    // is the frame time the viewer saw.
    void CollectStats() {
        DXGI_FRAME_STATISTICS st = {};
        HRESULT hr = sc_->GetFrameStatistics(&st);
        if (hr == E_NOTIMPL) {
            // RTX Encore wraps the swap chain and its wrapper leaves GetFrameStatistics out. The
            // media variant reports the same counts, if the wrapper passes it through.
            ComPtr<IDXGISwapChainMedia> media;
            DXGI_FRAME_STATISTICS_MEDIA sm = {};
            if (SUCCEEDED(sc_.As(&media))) {
                hr = media->GetFrameStatisticsMedia(&sm);
                st.PresentCount = sm.PresentCount;
                st.PresentRefreshCount = sm.PresentRefreshCount;
                st.SyncRefreshCount = sm.SyncRefreshCount;
                st.SyncQPCTime = sm.SyncQPCTime;
                st.SyncGPUTime = sm.SyncGPUTime;
            }
            if (!mediaLogged_) {
                mediaLogged_ = true;
                Log(L"GetFrameStatistics isn't implemented on this swap chain; GetFrameStatisticsMedia %s 0x%08X",
                    media ? L"returned" : L"isn't available either,", (unsigned)hr);
            }
        }
        if (FAILED(hr) || st.PresentCount == 0) {
            // Log each distinct failure once, so the reason the display count is missing is on record.
            HRESULT code = FAILED(hr) ? hr : S_FALSE;
            if (code != statsErr_) {
                statsErr_ = code;
                if (FAILED(hr)) Log(L"Frame statistics unavailable: GetFrameStatistics returned 0x%08X after %llu presents", (unsigned)hr, (unsigned long long)g_presents.load());
                else Log(L"Frame statistics unavailable: PresentCount is 0 after %llu presents", (unsigned long long)g_presents.load());
            }
            g_statsOk = false;
            return;
        }
        if (statsErr_ != S_OK) {
            Log(L"Frame statistics available again after %llu presents", (unsigned long long)g_presents.load());
            statsErr_ = S_OK;
        }
        g_statsOk = true;
        if (st.PresentCount == lastPc_) {
            if (++stuck_ == 300) Log(L"Frame statistics: display count stuck at %u", st.PresentCount);
            return;
        }
        if (!lastPc_) Log(L"Frame statistics: present %u shown at refresh %u", st.PresentCount, st.PresentRefreshCount);
        stuck_ = 0;
        if (lastPc_ && st.PresentCount > lastPc_ && st.PresentCount - lastPc_ < 1000) {
            g_displayed += st.PresentCount - lastPc_;
            if (lastSync_ && st.SyncQPCTime.QuadPart > lastSync_ && qpfStats_ > 0) {
                uint32_t us = (uint32_t)std::min(1e9, (st.SyncQPCTime.QuadPart - lastSync_) * 1e6 / qpfStats_);
                uint32_t cur = g_worstUs.load();
                while (us > cur && !g_worstUs.compare_exchange_weak(cur, us)) {
                }
            }
        }
        lastPc_ = st.PresentCount;
        lastSync_ = st.SyncQPCTime.QuadPart;
    }

    // Draws the fps text into the top left corner of the back buffer with Direct2D.
    void DrawOverlay() {
        std::wstring text;
        {
            std::lock_guard<std::mutex> l(g_overlayMx);
            text = g_overlayText;
        }
        if (text.empty() || overlayBroken_) return;
        if (!d2f_) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2f_.GetAddressOf())) ||
                FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(dwf_.GetAddressOf())))) {
                Log(L"fps overlay: Direct2D isn't available");
                overlayBroken_ = true;
                return;
            }
        }
        if (!ovVs_) {
            ComPtr<ID3DBlob> b;
            D3D11_BUFFER_DESC bd = {};
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.ByteWidth = sizeof(OverlayCB);
            D3D11_BLEND_DESC bl = {};
            bl.RenderTarget[0].BlendEnable = TRUE;
            bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (!Compile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "VsTri", "vs_5_0", b) ||
                FAILED(dev_->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ovVs_)) ||
                !Compile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "PsOverlay", "ps_5_0", b) ||
                FAILED(dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ovPs_)) ||
                FAILED(dev_->CreateBuffer(&bd, nullptr, &ovCb_)) || FAILED(dev_->CreateBlendState(&bl, &ovBlend_))) {
                Log(L"fps overlay: shaders failed");
                ovVs_.Reset();
                overlayBroken_ = true;
                return;
            }
        }
        float fs = std::max(14.0f, std::round(bbH_ / 42.0f));
        // The text changes a few times a second. The drawn texture is kept until it does.
        if (d2rt_ && tf_ && fs == tfSize_ && text == ovLastText_) {
            CompositeOverlay(ovPad_, ovOw_, ovOh_);
            return;
        }
        ovLastText_.clear();
        if (!tf_ || fs != tfSize_) {
            tf_.Reset();
            if (FAILED(dwf_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, fs, L"en-us", &tf_)))
                return;
            tfSize_ = fs;
        }
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(dwf_->CreateTextLayout(text.c_str(), (UINT32)text.size(), tf_.Get(), 4000, 4000, &lay))) return;
        DWRITE_TEXT_METRICS m;
        lay->GetMetrics(&m);
        float pad = fs * 0.5f;
        UINT ow = (UINT)std::ceil(m.width + pad * 2), oh = (UINT)std::ceil(m.height + pad * 2);
        ow = std::min(ow, bbW_ > (UINT)pad ? bbW_ - (UINT)pad : 1u);
        oh = std::min(oh, bbH_ > (UINT)pad ? bbH_ - (UINT)pad : 1u);

        // Text goes into a BGRA texture first. Direct2D can't draw on the 10-bit HDR back buffer.
        if (!d2rt_ || ovTexW_ < ow || ovTexH_ < oh) {
            ReleaseOverlayTarget();
            UINT tw = std::max(ow, 256u), th = std::max(oh, 64u);
            if (!MakeTex(tw, th, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET, ovTex_, ovSrv_, nullptr)) return;
            ComPtr<IDXGISurface> s;
            ovTex_.As(&s);
            D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            HRESULT hr = s ? d2f_->CreateDxgiSurfaceRenderTarget(s.Get(), &p, &d2rt_) : E_NOINTERFACE;
            if (FAILED(hr)) {
                LogOnce(L"fps overlay: Direct2D target failed", hr);
                return;
            }
            d2rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &txt_);
            d2rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.65f), &bgBrush_);
            ovTexW_ = tw;
            ovTexH_ = th;
        }
        d2rt_->BeginDraw();
        d2rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        d2rt_->FillRectangle(D2D1::RectF(0, 0, (float)ow, (float)oh), bgBrush_.Get());
        d2rt_->DrawTextLayout(D2D1::Point2F(pad, pad), lay.Get(), txt_.Get());
        HRESULT hr = d2rt_->EndDraw();
        if (FAILED(hr)) {
            if (hr == D2DERR_RECREATE_TARGET) ReleaseOverlayTarget();
            return;
        }
        ovLastText_ = text;
        ovPad_ = pad;
        ovOw_ = ow;
        ovOh_ = oh;
        CompositeOverlay(pad, ow, oh);
    }

    void CompositeOverlay(float pad, UINT ow, UINT oh) {
        ComPtr<ID3D11RenderTargetView> rtv = BackRtv();
        if (!rtv) return;
        // RTX Encore puts its "Insert opens the menu" tag in the top left corner, about 30 pixels
        // tall, so the overlay starts below it when RTX Encore is loaded.
        float x = std::round(pad), y = std::round(pad) + (g_encore.empty() ? 0.0f : 40.0f);
        if (y + oh > bbH_) return;
        OverlayCB cb = {x, y, 0, 0, hdrOut_ ? 1.0f : 0.0f, g_sdrWhiteNits.load(), 0, 0};
        ctx_->UpdateSubresource(ovCb_.Get(), 0, nullptr, &cb, 0, 0);
        D3D11_VIEWPORT vp = {x, y, (float)ow, (float)oh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(nullptr);
        ctx_->OMSetBlendState(ovBlend_.Get(), nullptr, 0xffffffff);
        ctx_->OMSetDepthStencilState(nullptr, 0);
        ctx_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(ovVs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ovPs_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, ovCb_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, ovSrv_.GetAddressOf());
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx_->PSSetShaderResources(0, 1, &nullSrv);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void ReleaseOverlayTarget() {
        ovLastText_.clear();
        txt_.Reset();
        bgBrush_.Reset();
        d2rt_.Reset();
        ovSrv_.Reset();
        ovTex_.Reset();
        ovTexW_ = ovTexH_ = 0;
    }

    // ---- HDR ---------------------------------------------------------------------------------
    // Switches the swap chain between 8-bit SDR (sRGB, BT.709) and 10-bit HDR10 (PQ, BT.2020).
    // Makes the swap chain again at the window's size, with or without the frame latency
    // waitable object, keeping the current SDR or HDR10 format. The caller holds m_ and has
    // dropped every view of the old back buffers.
    bool RecreateSwapChain(bool waitable) {
        ComPtr<IDXGIDevice1> dd;
        ComPtr<IDXGIAdapter> ad;
        ComPtr<IDXGIFactory2> f;
        if (FAILED(dev_.As(&dd)) || FAILED(dd->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&f)))) {
            Log(L"Can't reach the DXGI factory to make the swap chain again");
            return false;
        }
        if (waitable_) {
            CloseHandle(waitable_);
            waitable_ = nullptr;
        }
        sc_.Reset();
        ctx_->ClearState();
        ctx_->Flush();  // DXGI holds an HWND's flip swap chain until the context lets go of it
        RECT rc;
        GetClientRect(hwnd_, &rc);
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = std::max<LONG>(1, rc.right - rc.left);
        d.Height = std::max<LONG>(1, rc.bottom - rc.top);
        d.Format = hdrOut_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr = E_FAIL;
        for (bool w : {waitable, !waitable}) {
            scFlags_ = (w ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0) |
                       (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
            d.Flags = scFlags_;
            hr = f->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &d, nullptr, nullptr, &sc_);
            if (SUCCEEDED(hr)) break;
            Log(L"CreateSwapChainForHwnd (%s) failed 0x%08X", w ? L"waitable" : L"for exclusive mode", (unsigned)hr);
        }
        if (FAILED(hr)) {
            ready_ = false;
            return false;
        }
        f->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> sc2;
        if ((scFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) && SUCCEEDED(sc_.As(&sc2))) {
            sc2->SetMaximumFrameLatency(1);
            waitable_ = sc2->GetFrameLatencyWaitableObject();
        } else {
            dd->SetMaximumFrameLatency(1);  // without the waitable object, Present blocks instead
        }
        if (hdrOut_) ApplyHdrColorSpace(true);
        bbW_ = d.Width;
        bbH_ = d.Height;
        lastPc_ = 0;  // a new swap chain counts presents from zero
        lastSync_ = 0;
        Log(L"Swap chain made again %ux%u, %s", d.Width, d.Height, waitable_ ? L"waitable" : L"without the waitable object");
        return true;
    }

    // Sets the swap chain's color space and HDR10 metadata for SDR or HDR10 output.
    void ApplyHdrColorSpace(bool on) {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(sc_.As(&sc3)))
            sc3->SetColorSpace1(on ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        ComPtr<IDXGISwapChain4> sc4;
        if (SUCCEEDED(sc_.As(&sc4))) {
            if (on) {
                // Capture cards don't pass the console's mastering metadata, so this describes a
                // generic 1000 nit HDR10 grade on BT.2020 primaries with a D65 white point.
                DXGI_HDR_METADATA_HDR10 md = {};
                md.RedPrimary[0] = 35400;  // 0.708 in units of 0.00002
                md.RedPrimary[1] = 14600;
                md.GreenPrimary[0] = 8500;
                md.GreenPrimary[1] = 39850;
                md.BluePrimary[0] = 6550;
                md.BluePrimary[1] = 2300;
                md.WhitePoint[0] = 15635;
                md.WhitePoint[1] = 16450;
                md.MaxMasteringLuminance = 1000;
                md.MinMasteringLuminance = 50;  // 0.005 nits in units of 0.0001
                md.MaxContentLightLevel = 1000;
                md.MaxFrameAverageLightLevel = 400;
                sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(md), &md);
            } else {
                sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
            }
        }
    }

    bool SetHdr(bool on) {
        if (on == hdrOut_) return true;
        if (on) {
            ComPtr<IDXGISwapChain3> sc3;
            UINT support = 0;
            if (FAILED(sc_.As(&sc3)) ||
                FAILED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support)) ||
                !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                LogOnce(L"HDR10 output isn't supported by this swap chain", E_FAIL);
                return false;
            }
        }
        ReleaseOverlayTarget();
        DropCachedViews();
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->ClearState();
        ctx_->Flush();
        DXGI_FORMAT fmt = on ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        HRESULT hr = sc_->ResizeBuffers(0, bbW_, bbH_, fmt, scFlags_);
        if (FAILED(hr)) {
            Log(L"Switching the swap chain to %s failed 0x%08X", on ? L"HDR10" : L"SDR", hr);
            return false;
        }
        ApplyHdrColorSpace(on);
        hdrOut_ = on;
        g_hdrOut = on ? 1 : 0;
        vp_.Reset();  // output color space and format change
        vpe_.Reset();
        fgW_ = fgH_ = 0;  // frame generation textures follow the output format
        Log(L"Output switched to %s", on ? L"HDR10 (10-bit PQ, BT.2020)" : L"SDR (8-bit, BT.709)");
        return true;
    }

    void LogOnce(const wchar_t* what, HRESULT hr) {
        if (lastErr_ == hr) return;
        lastErr_ = hr;
        Log(L"%s 0x%08X", what, hr);
    }

    bool EnsureProcessor(DXGI_FORMAT fmt, const FrameFormat& f) {
        if (vp_ && fmt == vpFmt_ && f.w == vpF_.w && f.h == vpF_.h && f.fpsN == vpF_.fpsN && f.fpsD == vpF_.fpsD &&
            f.matrix == vpF_.matrix && f.range == vpF_.range && f.transfer == vpF_.transfer && f.interlace == vpF_.interlace)
            return true;
        vp_.Reset();
        vpe_.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {f.fpsN, f.fpsD};
        cd.InputWidth = f.w;
        cd.InputHeight = f.h;
        cd.OutputFrameRate = {f.fpsN, f.fpsD};
        cd.OutputWidth = bbW_;
        cd.OutputHeight = bbH_;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        HRESULT hr = vdev_->CreateVideoProcessorEnumerator(&cd, &vpe_);
        if (FAILED(hr)) {
            LogOnce(L"CreateVideoProcessorEnumerator failed", hr);
            return false;
        }
        UINT fl = 0;
        if (FAILED(vpe_->CheckVideoProcessorFormat(fmt, &fl)) || !(fl & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
            LogOnce(L"GPU video processor cannot take this input format", E_FAIL);
            vpe_.Reset();
            return false;
        }
        if (hdrOut_ && (FAILED(vpe_->CheckVideoProcessorFormat(DXGI_FORMAT_R10G10B10A2_UNORM, &fl)) ||
                        !(fl & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))) {
            Log(L"GPU video processor can't write 10-bit output, staying in SDR");
            hdrVpFail_ = true;
            vpe_.Reset();
            return false;
        }
        hr = vdev_->CreateVideoProcessor(vpe_.Get(), 0, &vp_);
        if (FAILED(hr)) {
            LogOnce(L"CreateVideoProcessor failed", hr);
            vpe_.Reset();
            return false;
        }
        D3D11_VIDEO_FRAME_FORMAT ff = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        if (f.interlace == MFVideoInterlace_FieldInterleavedUpperFirst) ff = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
        if (f.interlace == MFVideoInterlace_FieldInterleavedLowerFirst) ff = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
        vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, ff);
        vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
        D3D11_VIDEO_COLOR bg = {};
        bg.RGBA.A = 1.0f;
        vctx_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), FALSE, &bg);

        bool rgb = fmt == DXGI_FORMAT_B8G8R8X8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
        bool hdr = f.transfer == MFVideoTransFunc_2084;
        bool hlg = f.transfer == MFVideoTransFunc_HLG;
        bool bt2020 = f.matrix == MFVideoTransferMatrix_BT2020_10 || f.matrix == MFVideoTransferMatrix_BT2020_12 ||
                      f.primaries == MFVideoPrimaries_BT2020;
        bool bt601 = f.matrix == MFVideoTransferMatrix_BT601 || (f.matrix == 0 && f.h < 720);
        bool fullRange = f.range == MFNominalRange_0_255;
        if (vctx1_) {
            DXGI_COLOR_SPACE_TYPE in;
            if (rgb) in = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            else if (hdr) in = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
            else if (hlg) in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
            else if (bt2020) in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
            else if (bt601) in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
            else in = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
            vctx1_->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, in);
            vctx1_->VideoProcessorSetOutputColorSpace1(
                vp_.Get(), hdrOut_ ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
            in.RGB_Range = 0;
            in.YCbCr_Matrix = bt601 ? 0 : 1;
            in.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
            vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
        }
        vpFmt_ = fmt;
        vpF_ = f;
        lastErr_ = S_OK;
        Log(L"Video processor ready: %s, matrix %u range %u transfer %u, %s in, %s out", ModeText(f).c_str(), f.matrix,
            f.range, f.transfer, rgb ? L"RGB" : (hdr ? L"HDR10" : (hlg ? L"HLG" : L"YUV")), hdrOut_ ? L"HDR10" : L"SDR");
        return true;
    }

    // Copies a system memory frame into a GPU texture.
    bool Upload(IMFMediaBuffer* buf, const FrameFormat& f) {
        DXGI_FORMAT fmt = DxgiFromSubtype(f.subtype);
        if (fmt == DXGI_FORMAT_UNKNOWN) {
            LogOnce(L"Unsupported system memory format", E_FAIL);
            return false;
        }
        bool planar = fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010 || fmt == DXGI_FORMAT_P016;
        UINT rowBytes = RowBytes(fmt, f.w);
        if (!upload_ || upFmt_ != fmt || upW_ != f.w || upH_ != f.h) {
            upload_.Reset();
            staging_.Reset();
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = f.w;
            td.Height = f.h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            // NVIDIA refuses a video processor input view on a texture bound only as a
            // shader resource (E_INVALIDARG), so try render target, then no bind flags.
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &upload_))) {
                td.BindFlags = 0;
                if (FAILED(dev_->CreateTexture2D(&td, nullptr, &upload_))) {
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &upload_);
                    if (FAILED(hr)) {
                        LogOnce(L"Create upload texture failed", hr);
                        return false;
                    }
                }
            }
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &staging_);
            if (FAILED(hr)) {
                upload_.Reset();
                LogOnce(L"Create staging texture failed", hr);
                return false;
            }
            upFmt_ = fmt;
            upW_ = f.w;
            upH_ = f.h;
        }

        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* start = nullptr;
        DWORD len = 0;
        int kind = 0;
        ComPtr<IMF2DBuffer2> b2;
        ComPtr<IMF2DBuffer> b1;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) &&
            SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len))) {
            kind = 2;
        } else if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b1))) && SUCCEEDED(b1->Lock2D(&scan0, &pitch))) {
            kind = 1;
        } else {
            DWORD maxLen = 0;
            if (FAILED(buf->Lock(&start, &maxLen, &len))) return false;
            kind = 3;
            pitch = f.stride ? f.stride : (LONG)rowBytes;
            scan0 = pitch < 0 ? start + (size_t)(-pitch) * (f.h - 1) : start;
        }

        // Some cards pad the luma plane (1080 -> 1088). Work out where chroma starts.
        UINT yRows = f.h;
        if (planar && pitch > 0 && len > 0) {
            size_t usable = len - (size_t)(scan0 - start);
            UINT rows = (UINT)(usable / (size_t)pitch);
            UINT guess = rows * 2 / 3;
            if (guess >= f.h && guess <= f.h + 64) yRows = guess;
        }
        size_t needed = (size_t)std::abs(pitch) * (planar ? (yRows + f.h / 2) : f.h);
        bool ok = (UINT)std::abs(pitch) >= rowBytes && (len == 0 || kind == 2 || len >= needed - (std::abs(pitch) - rowBytes));

        if (ok) {
            if (diff_.Check(scan0, pitch, rowBytes, f.h, LumaOffset(fmt))) g_framesNew++;
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_WRITE, 0, &map))) {
                BYTE* dst = (BYTE*)map.pData;
                for (UINT r = 0; r < f.h; r++) memcpy(dst + (size_t)r * map.RowPitch, scan0 + (ptrdiff_t)r * pitch, rowBytes);
                if (planar) {
                    BYTE* srcUV = scan0 + (ptrdiff_t)pitch * yRows;
                    BYTE* dstUV = dst + (size_t)map.RowPitch * f.h;
                    for (UINT r = 0; r < f.h / 2; r++)
                        memcpy(dstUV + (size_t)r * map.RowPitch, srcUV + (ptrdiff_t)r * pitch, rowBytes);
                }
                ctx_->Unmap(staging_.Get(), 0);
                ctx_->CopyResource(upload_.Get(), staging_.Get());
            } else {
                ok = false;
            }
        } else {
            LogOnce(L"Frame buffer smaller than expected", E_FAIL);
        }
        if (kind == 2) b2->Unlock2D();
        else if (kind == 1) b1->Unlock2D();
        else buf->Unlock();
        return ok;
    }

    // Frames already in GPU memory: copy each one to a staging texture and read it back two frames
    // later, by which time the copy has finished, so the map doesn't stall the pipeline.
    static const int kDiffRing = 3;
    void DiffGpu(ID3D11Texture2D* tex, UINT sub, const FrameFormat& f) {
        if (diffBroken_) return;
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        UINT w = std::min(f.w, td.Width) & ~1u, h = std::min(f.h, td.Height) & ~1u;
        if (!diffStage_[0] || diffFmt_ != td.Format || diffW_ != w || diffH_ != h) {
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = w;
            sd.Height = h;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = td.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& s : diffStage_) {
                s.Reset();
                HRESULT hr = dev_->CreateTexture2D(&sd, nullptr, &s);
                if (FAILED(hr)) {
                    for (auto& t : diffStage_) t.Reset();
                    LogOnce(L"Frame compare staging texture failed, content fps unavailable", hr);
                    diffBroken_ = true;
                    return;
                }
            }
            diffFmt_ = td.Format;
            diffW_ = w;
            diffH_ = h;
            diffIdx_ = 0;
            diffQueued_ = 0;
            diff_.Reset();
        }
        D3D11_BOX box = {0, 0, 0, w, h, 1};
        ctx_->CopySubresourceRegion(diffStage_[diffIdx_].Get(), 0, 0, 0, 0, tex, sub, &box);
        diffIdx_ = (diffIdx_ + 1) % kDiffRing;
        if (++diffQueued_ < kDiffRing) return;
        diffQueued_ = kDiffRing - 1;
        ID3D11Texture2D* old = diffStage_[diffIdx_].Get();  // copied kDiffRing - 1 frames ago
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(old, 0, D3D11_MAP_READ, 0, &m))) {
            if (diff_.Check((const BYTE*)m.pData, (LONG)m.RowPitch, RowBytes(diffFmt_, w), h, LumaOffset(diffFmt_)))
                g_framesNew++;
            ctx_->Unmap(old, 0);
        }
    }

    std::mutex m_;
    bool ready_ = false;
    bool tearing_ = false;
    std::atomic<bool> exclusive_{false};
    HWND hwnd_ = nullptr;
    bool gpuPath_ = false;
    UINT scFlags_ = 0;
    HANDLE waitable_ = nullptr;
    UINT bbW_ = 1, bbH_ = 1;
    UINT resetToken_ = 0;
    UINT frameIndex_ = 0;
    HRESULT lastErr_ = S_OK;
    std::wstring adapterName_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoContext1> vctx1_;
    ComPtr<IMFDXGIDeviceManager> mgr_;
    // Declared before sc_ so it's destroyed after it: XeSS-FG won't shut down while the proxy
    // swap chain still has a reference.
    std::unique_ptr<XessOut> xo_;
    ComPtr<IDXGISwapChain1> sc_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpe_;
    ComPtr<ID3D11VideoProcessor> vp_;
    DXGI_FORMAT vpFmt_ = DXGI_FORMAT_UNKNOWN;
    FrameFormat vpF_;
    ComPtr<ID3D11Texture2D> upload_, staging_;
    DXGI_FORMAT upFmt_ = DXGI_FORMAT_UNKNOWN;
    UINT upW_ = 0, upH_ = 0;
    FrameDiff diff_;
    ComPtr<ID3D11Texture2D> diffStage_[kDiffRing];
    DXGI_FORMAT diffFmt_ = DXGI_FORMAT_UNKNOWN;
    UINT diffW_ = 0, diffH_ = 0;
    int diffIdx_ = 0, diffQueued_ = 0;
    bool diffBroken_ = false;
    int loggedPath_ = 0;

    std::shared_ptr<FgBlobs> fgBlobs_;
    bool fgInit_ = false, fgOk_ = false, prevValid_ = false, pairValid_ = false;
    ExactRepeats exact_;  // for Differs, which compares with the latest content frame
    ComPtr<ID3D11Texture2D> lumLast_;  // luma level 0 of the capture before, for ExactRepeats
    ComPtr<ID3D11ShaderResourceView> lumLastSrv_;
    bool lastValid_ = false;
    int fgA_ = 0, fgB_ = 1;  // older and latest content frame slots; the third slot takes each capture
    double gaps_[4] = {};
    int nGaps_ = 0, gapPos_ = 0;
    LONGLONG tNew_ = 0;
    LONGLONG smoothLast_ = 0;  // last present in Smooth Motion mode
    double schedT0_ = 0, schedT_ = 0;
    int schedM_ = 2, schedK_ = 3;
    int lastMode_ = 0;
    float lastT_ = 0;
    ComPtr<ID3D11Buffer> diffBuf_, diffRead_;
    ComPtr<ID3D11UnorderedAccessView> diffUav_;
    ComPtr<ID3D11ComputeShader> csDiff_;
    UINT fgW_ = 0, fgH_ = 0;
    UINT lumW_[kLevels] = {}, lumH_[kLevels] = {}, fldW_[kLevels] = {}, fldH_[kLevels] = {};
    double qpf_ = 1;
    double qpfStats_ = 0;
    UINT lastPc_ = 0;
    int stuck_ = 0;
    LONGLONG tmT0_ = 0;
    int tmN_ = 0;
    bool tmHaveMin_ = false;
    double tmMinAge_ = 0, tmQueue_ = 0, tmQueueMax_ = 0, tmWaitAcc_ = 0, tmWork_ = 0, tmWorkMax_ = 0, tmShow_ = 0;
    int tmShowN_ = 0;
    bool showPending_ = false;
    // Views kept across frames.
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> rtvBb_, bbOvTex_;
    ComPtr<ID3D11VideoProcessorOutputView> bbOv_, fgOv_[kSlots];
    ComPtr<ID3D11VideoProcessorEnumerator> bbOvVpe_, fgOvVpe_;
    bool rtvSwapLogged_ = false;
    HRESULT statsErr_ = S_OK;  // last frame statistics failure logged, S_FALSE for PresentCount 0
    bool mediaLogged_ = false;
    LONGLONG lastSync_ = 0;
    bool overlayBroken_ = false;
    float tfSize_ = 0;
    std::wstring ovLastText_;  // text in the overlay texture, empty when it must be drawn again
    float ovPad_ = 0;
    UINT ovOw_ = 0, ovOh_ = 0;
    ComPtr<ID2D1Factory> d2f_;
    ComPtr<IDWriteFactory> dwf_;
    ComPtr<ID2D1RenderTarget> d2rt_;
    struct OverlayCB {
        float orgX, orgY, unused0, unused1, hdr, whiteNits, pad0, pad1;
    };
    ComPtr<ID3D11VertexShader> ovVs_;
    ComPtr<ID3D11PixelShader> ovPs_;
    ComPtr<ID3D11Buffer> ovCb_;
    ComPtr<ID3D11BlendState> ovBlend_;
    ComPtr<ID3D11Texture2D> ovTex_;
    ComPtr<ID3D11ShaderResourceView> ovSrv_;
    UINT ovTexW_ = 0, ovTexH_ = 0;
    bool hdrOut_ = false;
    bool hdrVpFail_ = false;
    ComPtr<ID2D1SolidColorBrush> txt_, bgBrush_;
    ComPtr<IDWriteTextFormat> tf_;
    HANDLE timer_ = nullptr;
    ComPtr<ID3D11ComputeShader> csLuma_, csDown_, csFlow_, csMedian_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> flowCb_, drawCb_;
    ComPtr<ID3D11Texture2D> rgb_[kSlots], lum_[kSlots][kLevels], fld_[kLevels], med_;
    ComPtr<ID3D11ShaderResourceView> rgbSrv_[kSlots], lumSrv_[kSlots][kLevels], fldSrv_[kLevels], medSrv_;
    ComPtr<ID3D11UnorderedAccessView> lumUav_[kSlots][kLevels], fldUav_[kLevels], medUav_;
};

static Renderer g_renderer;

// ------------------------------------------------------------------------------------------------
// Video capture session
// ------------------------------------------------------------------------------------------------
struct VideoSession {
    std::mutex mx;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    FrameFormat fmt;
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
    std::atomic<HRESULT> failHr{S_OK};
    std::wstring statusText;
    std::atomic<ULONGLONG> lastFrameTick{0};
    HANDLE flushEvt = nullptr;
    ~VideoSession() {
        if (flushEvt) CloseHandle(flushEvt);
    }
};

class ReaderCallback : public IMFSourceReaderCallback {
public:
    explicit ReaderCallback(std::shared_ptr<VideoSession> s) : s_(std::move(s)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG ts, IMFSample* sample) override {
        VideoSession& S = *s_;
        if (S.stopping) return S_OK;
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            if (S.failHr != hr) Log(L"Capture stream stopped (hr 0x%08X flags 0x%X)", hr, flags);
            S.failHr = hr;
            S.failed = true;
            return S_OK;
        }
        if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
            Log(L"Input signal changed, reopening");
            S.failed = true;
            return S_OK;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            std::lock_guard<std::mutex> l(S.mx);
            ComPtr<IMFMediaType> cur;
            if (S.reader && SUCCEEDED(S.reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
                S.fmt = ReadFormat(cur.Get(), nullptr);
                Log(L"Output format changed to %s", ModeText(S.fmt).c_str());
            }
        }
        if (sample) {
            S.lastFrameTick = GetTickCount64();
            FrameFormat f;
            {
                std::lock_guard<std::mutex> l(S.mx);
                f = S.fmt;
            }
            g_renderer.PresentFrame(sample, f, ts);
        }
        std::lock_guard<std::mutex> l(S.mx);
        if (!S.stopping && S.reader) {
            hr = S.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(hr)) {
                Log(L"ReadSample failed 0x%08X", hr);
                S.failed = true;
            }
        }
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
        SetEvent(s_->flushEvt);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<VideoSession> s_;
};

static HRESULT CreateSource(const std::wstring& link, IMFMediaSource** src) {
    ComPtr<IMFAttributes> a;
    HRESULT hr = MFCreateAttributes(&a, 2);
    if (FAILED(hr)) return hr;
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    return MFCreateDeviceSource(a.Get(), src);
}

struct NativeMode {
    ComPtr<IMFMediaType> type;
    FrameFormat f;
};

static std::vector<NativeMode> ListNativeModes(IMFSourceReader* r) {
    std::vector<NativeMode> v;
    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
        NativeMode m;
        m.type = t;
        m.f = ReadFormat(t.Get(), nullptr);
        v.push_back(m);
    }
    return v;
}

// A Cam Link 4K flashed with the enjoy-digital open firmware enumerates as 1209:0001. Its NV12
// and M420 modes need the card's DDR3 frame buffer and stream a test pattern when that fails to
// come up, and they never scale, so they carry video only for a matching 4K input. YUY2 works
// without the frame buffer, so on that card YUY2 wins over everything else.
static bool YuyvFirst(const std::wstring& link, const std::wstring& name) {
    return Lower(link).find(L"vid_1209&pid_0001") != std::wstring::npos &&
           Lower(name).find(L"camlink") != std::wstring::npos;
}

// Highest resolution first, then frame rate, then the cheapest format to get onto the GPU.
static int PickMode(const std::vector<NativeMode>& modes, UINT reqW, UINT reqH, double reqFps, bool yuyvFirst) {
    int best = -1;
    auto better = [yuyvFirst](const FrameFormat& a, const FrameFormat& b) {
        bool ya = a.subtype == MFVideoFormat_YUY2, yb = b.subtype == MFVideoFormat_YUY2;
        if (yuyvFirst && ya != yb) return ya;
        UINT64 pa = (UINT64)a.w * a.h, pb = (UINT64)b.w * b.h;
        if (pa != pb) return pa > pb;
        double fa = std::round(Fps(a) * 10), fb = std::round(Fps(b) * 10);
        if (fa != fb) return fa > fb;
        // An HDR10 or HLG signal loses its 10 bits in an 8-bit format, so 10-bit wins there.
        bool hdrA = a.transfer == MFVideoTransFunc_2084 || a.transfer == MFVideoTransFunc_HLG;
        bool hdrB = b.transfer == MFVideoTransFunc_2084 || b.transfer == MFVideoTransFunc_HLG;
        if ((hdrA || hdrB) && IsTenBit(a.subtype) != IsTenBit(b.subtype)) return IsTenBit(a.subtype);
        return SubtypeRank(a.subtype) > SubtypeRank(b.subtype);
    };
    for (size_t i = 0; i < modes.size(); i++) {
        const FrameFormat& f = modes[i].f;
        if (reqW && (f.w != reqW || f.h != reqH)) continue;
        if (reqFps > 0 && std::fabs(Fps(f) - reqFps) > 0.6) continue;
        if (best < 0) {
            best = (int)i;
            continue;
        }
        // With a requested rate, the closest rate wins (59.94 versus 60), then the usual order.
        double da = reqFps > 0 ? std::fabs(Fps(f) - reqFps) : 0, db = reqFps > 0 ? std::fabs(Fps(modes[best].f) - reqFps) : 0;
        if (std::fabs(da - db) > 0.005 ? da < db : better(f, modes[best].f)) best = (int)i;
    }
    if (best < 0 && (reqW || reqFps > 0)) {
        Log(L"Requested mode not offered, using the best available one");
        best = PickMode(modes, 0, 0, 0, yuyvFirst);
    }
    return best;
}
static int PickMode(const std::vector<NativeMode>& modes, const std::wstring& link, const std::wstring& name) {
    Settings s = GetSettings();
    return PickMode(modes, s.reqW, s.reqH, s.reqFps, YuyvFirst(link, name));
}

// The current device's modes, for the settings window.
static std::vector<FrameFormat> g_modes;  // guarded by g_statusMx

static std::shared_ptr<VideoSession> OpenSession(const VideoDev& dev, std::wstring& err) {
    auto S = std::make_shared<VideoSession>();
    S->flushEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CreateSource(dev.link, &S->source);
    if (FAILED(hr)) {
        err = Fmt(L"Could not open %s (0x%08X)", dev.name.c_str(), hr);
        return nullptr;
    }
    ReaderCallback* cb = new ReaderCallback(S);
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 6);
    a->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);
    cb->Release();
    a->SetUINT32(MF_LOW_LATENCY, TRUE);
    a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (g_renderer.Manager()) {
        a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g_renderer.Manager());
        a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    } else {
        a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    hr = MFCreateSourceReaderFromMediaSource(S->source.Get(), a.Get(), &S->reader);
    if (FAILED(hr)) {
        S->source->Shutdown();
        err = Fmt(L"Could not create reader for %s (0x%08X)", dev.name.c_str(), hr);
        return nullptr;
    }
    auto modes = ListNativeModes(S->reader.Get());
    static std::wstring lastModes;
    std::wstring modeList = dev.name;
    for (auto& m : modes) modeList += L"\n    " + ModeText(m.f);
    if (modeList != lastModes) {
        Log(L"%s offers %u modes:", dev.name.c_str(), (unsigned)modes.size());
        for (auto& m : modes) Log(L"    %s", ModeText(m.f).c_str());
        lastModes = modeList;
    }
    {
        std::lock_guard<std::mutex> l(g_statusMx);
        g_modes.clear();
        for (auto& m : modes) g_modes.push_back(m.f);
    }
    int mi = PickMode(modes, dev.link, dev.name);
    static bool toldYuyv = false;
    if (YuyvFirst(dev.link, dev.name) && !toldYuyv && (toldYuyv = true))
        Log(L"%s runs the open firmware: preferring YUY2, since its NV12 and M420 modes need the frame buffer",
            dev.name.c_str());
    if (mi < 0) {
        S->source->Shutdown();
        err = Fmt(L"%s offers no video modes (no signal?)", dev.name.c_str());
        return nullptr;
    }
    const NativeMode& nm = modes[mi];
    S->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    S->reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFSourceReaderEx> rex;
    if (SUCCEEDED(S->reader.As(&rex))) {
        DWORD fl = 0;
        hr = rex->SetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nm.type.Get(), &fl);
        if (FAILED(hr)) Log(L"SetNativeMediaType failed 0x%08X", hr);
    }

    // Pass the native format straight to the GPU when the video processor takes it,
    // otherwise let Media Foundation decode or convert it.
    std::vector<GUID> outs;
    DXGI_FORMAT nd = DxgiFromSubtype(nm.f.subtype);
    if (nd != DXGI_FORMAT_UNKNOWN && g_renderer.SupportsInput(nd, nm.f.w, nm.f.h)) outs.push_back(nm.f.subtype);
    if (IsTenBit(nm.f.subtype) || nm.f.subtype == MFVideoFormat_HEVC) outs.push_back(MFVideoFormat_P010);
    outs.push_back(MFVideoFormat_NV12);
    outs.push_back(MFVideoFormat_YUY2);
    outs.push_back(MFVideoFormat_RGB32);
    hr = E_FAIL;
    for (auto& o : outs) {
        if (o == nm.f.subtype) {
            hr = S->reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nm.type.Get());
        } else {
            ComPtr<IMFMediaType> t;
            MFCreateMediaType(&t);
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            t->SetGUID(MF_MT_SUBTYPE, o);
            MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, nm.f.w, nm.f.h);
            MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, nm.f.fpsN, nm.f.fpsD);
            hr = S->reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
        }
        if (SUCCEEDED(hr)) break;
        Log(L"Output %s refused (0x%08X)", SubtypeName(o).c_str(), hr);
    }
    if (FAILED(hr)) {
        S->source->Shutdown();
        err = Fmt(L"No usable output format for %s", ModeText(nm.f).c_str());
        return nullptr;
    }
    ComPtr<IMFMediaType> cur;
    S->reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    S->fmt = ReadFormat(cur.Get(), nm.type.Get());
    g_srcFps = (float)Fps(S->fmt);
    static std::wstring lastCap;
    std::wstring capText = ModeText(nm.f) + L" as " + SubtypeName(S->fmt.subtype);
    if (capText != lastCap) Log(L"Capturing %s", capText.c_str());
    lastCap = capText;

    hr = S->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        S->source->Shutdown();
        err = Fmt(L"Could not start %s (0x%08X)", dev.name.c_str(), hr);
        return nullptr;
    }
    std::wstring conv = nm.f.subtype == S->fmt.subtype ? L"" : Fmt(L" > %s", SubtypeName(S->fmt.subtype).c_str());
    S->statusText = Fmt(L"%s  %s%s", dev.name.c_str(), ModeHzText(nm.f).c_str(), conv.c_str());
    return S;
}

static void CloseSession(std::shared_ptr<VideoSession>& S) {
    if (!S) return;
    ULONGLONG t0 = GetTickCount64();
    S->stopping = true;
    ComPtr<IMFSourceReader> r;
    {
        std::lock_guard<std::mutex> l(S->mx);
        r = S->reader;
    }
    if (r) {
        ResetEvent(S->flushEvt);
        // A stalled stream never answers the flush. The callback holds its own reference to the
        // session, so a flush that finishes after this wait still finds the event alive.
        if (SUCCEEDED(r->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS))) WaitForSingleObject(S->flushEvt, 250);
    }
    ULONGLONG t1 = GetTickCount64();
    {
        std::lock_guard<std::mutex> l(S->mx);
        S->reader.Reset();
    }
    r.Reset();
    if (S->source) S->source->Shutdown();
    S->source.Reset();
    S.reset();
    ULONGLONG t2 = GetTickCount64();
    if (t2 - t0 > 100)
        Log(L"Closing the video device took %llu ms (flush %llu ms, release and shutdown %llu ms)", t2 - t0, t1 - t0,
            t2 - t1);
}

// CAPVIEW_TEST_SOURCE=1 replaces the capture card with a made-up 1080p60 YUY2 signal that carries
// a 30 fps picture, each frame sent twice the way a console game arrives over HDMI. A white box
// crosses a checkerboard, so frame generation can be checked without a console. With
// CAPVIEW_TEST_SOURCE=dim the box is 1 luma step above a near-black background, like motion in a
// dark scene, so no sample moves past the 2.5 step margin and only the exact-repeat rule sees it.
// A 2 step box isn't enough, since the video processor's rounding to 8-bit RGB makes it 3.
static void TestSource() {
    wchar_t mode[16] = {};
    GetEnvironmentVariableW(L"CAPVIEW_TEST_SOURCE", mode, 16);
    bool dim = _wcsicmp(mode, L"dim") == 0;
    FrameFormat f;
    f.subtype = MFVideoFormat_YUY2;
    f.w = 1920;
    f.h = 1080;
    f.fpsN = 60;
    f.stride = 1920 * 2;
    f.matrix = MFVideoTransferMatrix_BT709;
    f.range = MFNominalRange_16_235;
    const DWORD size = f.w * f.h * 2;
    std::vector<BYTE> pic(size);
    Log(L"Test source: 1920x1080 YUY2 60 fps, a new picture every second frame%s", dim ? L", dim" : L"");
    SetVideoStatus(L"Test source, 1080p60 with a 30 fps picture");
    g_videoActive = true;
    LARGE_INTEGER fq, t0;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    for (UINT64 n = 0; !g_quit; n++) {
        if (n % 2 == 0) {
            int bx = (int)((n / 2 * 24) % (f.w + 240)) - 240;
            for (UINT y = 0; y < f.h; y++) {
                BYTE* row = pic.data() + (size_t)y * f.stride;
                bool boxRow = y >= 420 && y < 660;
                for (UINT x = 0; x < f.w; x += 2) {
                    BYTE luma = dim ? 18 : ((x / 60 + y / 60) % 2) ? 70 : 110;
                    if (boxRow && (int)x >= bx && (int)x < bx + 240) luma = dim ? 19 : 235;
                    row[x * 2] = row[x * 2 + 2] = luma;
                    row[x * 2 + 1] = row[x * 2 + 3] = 128;
                }
            }
        }
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> s;
        BYTE* p = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &buf)) || FAILED(buf->Lock(&p, nullptr, nullptr))) break;
        memcpy(p, pic.data(), size);
        buf->Unlock();
        buf->SetCurrentLength(size);
        if (FAILED(MFCreateSample(&s)) || FAILED(s->AddBuffer(buf.Get()))) break;
        LONGLONG ts = (LONGLONG)(n * 10000000 / 60);
        s->SetSampleTime(ts);
        g_renderer.PresentFrame(s.Get(), f, ts);
        LONGLONG due = t0.QuadPart + (LONGLONG)((n + 1) * fq.QuadPart / 60);
        for (LARGE_INTEGER now; QueryPerformanceCounter(&now), now.QuadPart < due;)
            if (due - now.QuadPart > fq.QuadPart / 500) Sleep(1);
    }
    g_videoActive = false;
}

static void VideoThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (GetEnvironmentVariableW(L"CAPVIEW_TEST_SOURCE", nullptr, 0)) {
        TestSource();
        CoUninitialize();
        return;
    }
    std::shared_ptr<VideoSession> cur;
    VideoDev curDev;
    ULONGLONG openedAt = 0, lastEnum = 0, nextTry = 0, stallFrom = 0, holdFrom = 0;
    int fails = 0;
    bool shown = false;
    while (!g_quit) {
        ULONGLONG now = GetTickCount64();
        if (cur) {
            bool reopen = false;
            if (cur->lastFrameTick && !shown) {
                SetVideoStatus(cur->statusText);
                shown = true;
                fails = 0;
                if (stallFrom) {
                    Log(L"Picture back %llu ms after the last frame before the stall", cur->lastFrameTick - stallFrom);
                    stallFrom = 0;
                }
            }
            if (cur->failed) reopen = true;
            ULONGLONG last = cur->lastFrameTick;
            if (holdFrom && last != holdFrom) {
                Log(L"Frames came back on their own %llu ms after the last frame before the stall", last - holdFrom);
                holdFrom = 0;
            }
            // A card sends 60 frames a second even for a still picture, and the slowest present
            // logged so far held the next frame back about 0.4 s. A Cam Link whose video stops
            // mid-stream starts again when it is reopened, so 1 s without a frame is enough to act on.
            // CAPVIEW_STALL_HOLD_SECS keeps a stalled stream open that much longer, so the card can
            // be looked at in its stalled state.
            if (last ? now - last > 1000 : now - openedAt > 6000) {
                if (last && g_stallHoldMs && now - last <= 1000 + g_stallHoldMs) {
                    if (holdFrom != last) {
                        holdFrom = last;
                        Log(L"No frames from %s for %llu ms, holding the stream open for %llu s", curDev.name.c_str(),
                            now - last, g_stallHoldMs / 1000);
                    }
                } else if (last) {
                    Log(L"No frames from %s for %llu ms, reopening", curDev.name.c_str(), now - last);
                    stallFrom = last;
                    reopen = true;
                } else {
                    Log(L"No frames from %s, reopening", curDev.name.c_str());
                    reopen = true;
                }
            }
            if (g_videoRestart.exchange(false)) reopen = true;
            if (!reopen && g_videoPick < 0 && GetSettings().videoName.empty() && now - lastEnum > 3000) {
                // A better capture card was plugged in while something else was showing.
                lastEnum = now;
                auto list = EnumVideo();
                g_videoCount = (int)list.size();
                int b = PickVideo(list);
                if (b >= 0 && list[b].link != curDev.link && list[b].score > curDev.score) reopen = true;
            }
            if (reopen) {
                holdFrom = 0;
                bool gotFrames = cur->lastFrameTick != 0;
                HRESULT why = cur->failHr;
                CloseSession(cur);
                g_videoActive = false;
                g_renderer.PresentBlack();
                fails = gotFrames ? 0 : std::min(fails + 1, 4);
                nextTry = now + (fails ? (250u << fails) : 250u);
                if (why == MF_E_HW_MFT_FAILED_START_STREAMING || why == (HRESULT)0xC00D3EA3)
                    SetVideoStatus(curDev.name + L" is in use by another app (close Elgato Studio, OBS, Discord camera...)");
                else if (!gotFrames)
                    SetVideoStatus(curDev.name + L": no picture (no signal, or HDCP is on)");
            }
            Sleep(30);
            continue;
        }
        if (now < nextTry) {
            Sleep(30);
            continue;
        }
        g_videoRestart = false;
        auto list = EnumVideo();
        g_videoCount = (int)list.size();
        int idx = PickVideo(list);
        if (idx < 0) {
            g_videoCurIdx = -1;
            SetVideoStatus(list.empty() ? L"Waiting for a capture card"
                                        : L"No capture card found. Press D to pick a video device");
            nextTry = now + 1000;
            continue;
        }
        g_videoCurIdx = idx;
        if (g_videoRestart.exchange(false)) fails = 0;
        std::wstring err;
        cur = OpenSession(list[idx], err);
        if (!cur) {
            SetVideoStatus(err);
            nextTry = now + 1500;
            continue;
        }
        bool changed;
        {
            std::lock_guard<std::mutex> l(g_statusMx);
            changed = g_curVideo.name != list[idx].name;
            g_curVideo.name = list[idx].name;
            g_curVideo.container = list[idx].container;
        }
        if (changed) g_audioReeval = true;
        curDev = list[idx];
        shown = false;
        if (fails == 0) SetVideoStatus(L"Opening " + curDev.name);
        openedAt = GetTickCount64();
        g_videoActive = true;
    }
    CloseSession(cur);
    CoUninitialize();
}

// ------------------------------------------------------------------------------------------------
// Audio: format conversion
// ------------------------------------------------------------------------------------------------
enum : DWORD {
    SPK_FL = 0x1, SPK_FR = 0x2, SPK_FC = 0x4, SPK_LFE = 0x8, SPK_BL = 0x10, SPK_BR = 0x20,
    SPK_FLC = 0x40, SPK_FRC = 0x80, SPK_BC = 0x100, SPK_SL = 0x200, SPK_SR = 0x400,
};

struct PcmFmt {
    int ch = 0, rate = 0, container = 0, bits = 0, block = 0;
    bool isFloat = false;
    DWORD mask = 0;
};

static DWORD DefaultMask(int ch) {
    switch (ch) {
        case 1: return SPK_FC;
        case 2: return SPK_FL | SPK_FR;
        case 3: return SPK_FL | SPK_FR | SPK_FC;
        case 4: return SPK_FL | SPK_FR | SPK_BL | SPK_BR;
        case 5: return SPK_FL | SPK_FR | SPK_FC | SPK_BL | SPK_BR;
        case 6: return SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_BL | SPK_BR;
        case 7: return SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_BL | SPK_BR | SPK_BC;
        case 8: return SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_BL | SPK_BR | SPK_SL | SPK_SR;
        default: return 0;
    }
}

static bool ParseFormat(const WAVEFORMATEX* w, PcmFmt& p) {
    p.ch = w->nChannels;
    p.rate = (int)w->nSamplesPerSec;
    p.block = w->nBlockAlign;
    p.container = p.ch ? w->nBlockAlign / p.ch : 0;
    p.bits = w->wBitsPerSample;
    p.isFloat = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    p.mask = 0;
    bool pcm = w->wFormatTag == WAVE_FORMAT_PCM || p.isFloat;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        auto x = (const WAVEFORMATEXTENSIBLE*)w;
        // KSDATAFORMAT_SUBTYPE_PCM is 1, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT is 3.
        p.isFloat = x->SubFormat.Data1 == 3;
        pcm = x->SubFormat.Data1 == 1 || p.isFloat;
        p.mask = x->dwChannelMask;
        if (x->Samples.wValidBitsPerSample) p.bits = x->Samples.wValidBitsPerSample;
    }
    if (!p.mask) p.mask = DefaultMask(p.ch);
    if (!pcm || p.ch < 1 || p.ch > 32 || p.rate < 1000) return false;
    if (p.isFloat) return p.container == 4 || p.container == 8;
    return p.container >= 1 && p.container <= 4;
}

static std::wstring FormatText(const PcmFmt& p) {
    return Fmt(L"%d Hz %dch %d-bit %s", p.rate, p.ch, p.bits, p.isFloat ? L"float" : L"int");
}

static inline float ReadSample(const BYTE* s, const PcmFmt& f) {
    switch (f.container) {
        case 1: return ((int)s[0] - 128) / 128.0f;
        case 2: return *(const int16_t*)s / 32768.0f;
        case 3: {
            int32_t v = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24);
            return v / 2147483648.0f;
        }
        case 4:
            if (f.isFloat) return *(const float*)s;
            return *(const int32_t*)s / 2147483648.0f;
        case 8: return (float)*(const double*)s;
    }
    return 0;
}

static inline void WriteSample(BYTE* d, const PcmFmt& f, float v) {
    v = std::min(1.0f, std::max(-1.0f, v));
    switch (f.container) {
        case 1: d[0] = (BYTE)std::lround(v * 127.0f + 128.0f); break;
        case 2: *(int16_t*)d = (int16_t)std::lround(v * 32767.0f); break;
        case 3: {
            int32_t x = (int32_t)std::lround((double)v * 8388607.0);
            d[0] = (BYTE)x;
            d[1] = (BYTE)(x >> 8);
            d[2] = (BYTE)(x >> 16);
            break;
        }
        case 4:
            if (f.isFloat) *(float*)d = v;
            else *(int32_t*)d = (int32_t)std::llround((double)v * 2147483647.0);
            break;
        case 8: *(double*)d = v; break;
    }
}

static std::vector<DWORD> SpeakerSlots(const PcmFmt& f) {
    std::vector<DWORD> s;
    for (int b = 0; b < 32 && (int)s.size() < f.ch; b++)
        if (f.mask & (1u << b)) s.push_back(1u << b);
    while ((int)s.size() < f.ch) s.push_back(0);
    return s;
}

// out x in mixing matrix, row major.
static std::vector<float> BuildMatrix(const PcmFmt& in, const PcmFmt& out) {
    std::vector<float> m((size_t)out.ch * in.ch, 0.0f);
    auto si = SpeakerSlots(in), so = SpeakerSlots(out);
    auto find = [&](DWORD s) -> int {
        for (int i = 0; i < out.ch; i++)
            if (so[i] == s) return i;
        return -1;
    };
    auto add = [&](int o, int i, float g) {
        if (o >= 0) m[(size_t)o * in.ch + i] += g;
    };
    if (in.ch == 1) {
        int l = find(SPK_FL), r = find(SPK_FR), c = find(SPK_FC);
        if (l >= 0 || r >= 0) {
            add(l, 0, 1);
            add(r, 0, 1);
        } else if (c >= 0) add(c, 0, 1);
        else add(0, 0, 1);
        return m;
    }
    const float h = 0.7071f;
    for (int i = 0; i < in.ch; i++) {
        DWORD s = si[i];
        int o = s ? find(s) : -1;
        if (o >= 0) {
            add(o, i, 1);
            continue;
        }
        int fl = find(SPK_FL), fr = find(SPK_FR), fc = find(SPK_FC);
        switch (s) {
            case SPK_FC: add(fl, i, h); add(fr, i, h); break;
            case SPK_LFE: break;
            case SPK_BL: if (find(SPK_SL) >= 0) add(find(SPK_SL), i, 1); else add(fl >= 0 ? fl : fc, i, h); break;
            case SPK_BR: if (find(SPK_SR) >= 0) add(find(SPK_SR), i, 1); else add(fr >= 0 ? fr : fc, i, h); break;
            case SPK_SL: if (find(SPK_BL) >= 0) add(find(SPK_BL), i, 1); else add(fl >= 0 ? fl : fc, i, h); break;
            case SPK_SR: if (find(SPK_BR) >= 0) add(find(SPK_BR), i, 1); else add(fr >= 0 ? fr : fc, i, h); break;
            case SPK_FLC: add(fl >= 0 ? fl : fc, i, 1); break;
            case SPK_FRC: add(fr >= 0 ? fr : fc, i, 1); break;
            case SPK_FL: add(fc, i, h); break;
            case SPK_FR: add(fc, i, h); break;
            case SPK_BC:
                if (find(SPK_BL) >= 0 && find(SPK_BR) >= 0) { add(find(SPK_BL), i, h); add(find(SPK_BR), i, h); }
                else { add(fl, i, 0.5f); add(fr, i, 0.5f); }
                break;
            default:
                if (i < out.ch && !s) add(i, i, 1);
                else if (fl >= 0 && fr >= 0) { add(fl, i, 0.5f); add(fr, i, 0.5f); }
                else add(fc, i, h);
                break;
        }
    }
    return m;
}

struct AudioRing {
    std::vector<float> d;
    size_t cap = 0, ch = 0, rd = 0, n = 0;
    void Init(size_t frames, size_t channels) {
        cap = frames;
        ch = channels;
        d.assign(cap * ch, 0.0f);
        rd = n = 0;
    }
    void Push(const float* f) {
        if (n == cap) Pop(1);
        memcpy(&d[((rd + n) % cap) * ch], f, ch * sizeof(float));
        n++;
    }
    const float* At(size_t j) const { return &d[((rd + j) % cap) * ch]; }
    void Pop(size_t k) {
        k = std::min(k, n);
        rd = (rd + k) % cap;
        n -= k;
    }
};

// ------------------------------------------------------------------------------------------------
// Audio: devices and the streaming loop
// ------------------------------------------------------------------------------------------------
struct AudioEp {
    std::wstring id, name;
    GUID container = GUID_NULL;
};

static std::wstring EndpointName(IMMDevice* d) {
    std::wstring n;
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) n = v.pwszVal;
        PropVariantClear(&v);
    }
    return n;
}

static std::vector<AudioEp> EnumEndpoints(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<AudioEp> out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        AudioEp e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) {
            e.id = id;
            CoTaskMemFree(id);
        }
        e.name = EndpointName(d.Get());
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) e.container = *v.puuid;
            PropVariantClear(&v);
        }
        out.push_back(e);
    }
    return out;
}
static std::vector<AudioEp> EnumCapture(IMMDeviceEnumerator* en) { return EnumEndpoints(en, eCapture); }

// The output the user chose, or the Windows default when it is unset or missing.
static ComPtr<IMMDevice> ResolveOutput(IMMDeviceEnumerator* en, std::wstring& id, bool& fellBack) {
    Settings s = GetSettings();
    fellBack = false;
    ComPtr<IMMDevice> d;
    if (!s.outputId.empty() || !s.outputName.empty()) {
        DWORD state = 0;
        if (!s.outputId.empty() && SUCCEEDED(en->GetDevice(s.outputId.c_str(), &d)) && SUCCEEDED(d->GetState(&state)) &&
            state == DEVICE_STATE_ACTIVE) {
            id = s.outputId;
            return d;
        }
        d.Reset();
        if (!s.outputName.empty())
            for (auto& e : EnumEndpoints(en, eRender))
                if (Contains(e.name, s.outputName) && SUCCEEDED(en->GetDevice(e.id.c_str(), &d))) {
                    id = e.id;
                    return d;
                }
        fellBack = true;
    }
    d.Reset();
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return nullptr;
    LPWSTR p = nullptr;
    if (SUCCEEDED(d->GetId(&p))) {
        id = p;
        CoTaskMemFree(p);
    }
    return d;
}

static int AudioScore(const AudioEp& e, const CurVideoDev& v) {
    int s = 0;
    if (UsableContainer(e.container) && e.container == v.container) s += 100;
    if (!v.name.empty() && (Contains(e.name, v.name) || Contains(v.name, e.name))) s += 80;
    s += CaptureNameScore(e.name) / 2;
    return s;
}

static int PickAudio(const std::vector<AudioEp>& eps) {
    std::wstring want = GetSettings().audioName;
    int pick = g_audioPick.load();
    if (pick == -2) return -1;
    if (pick >= 0 && pick < (int)eps.size()) return pick;
    if (Lower(want) == L"none") return -1;
    if (!want.empty()) {
        for (size_t i = 0; i < eps.size(); i++)
            if (Contains(eps[i].name, want)) return (int)i;
        return -1;
    }
    CurVideoDev v;
    {
        std::lock_guard<std::mutex> l(g_statusMx);
        v = g_curVideo;
    }
    int best = -1, bestScore = 0;
    for (size_t i = 0; i < eps.size(); i++) {
        int s = AudioScore(eps[i], v);
        if (s >= 20 && s > bestScore) {
            best = (int)i;
            bestScore = s;
        }
    }
    return best;
}

class NotifyClient : public IMMNotificationClient {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        // Following the default: switch now. A chosen output only matters if it went missing.
        if (flow == eRender && role == eConsole) {
            if (GetSettings().outputId.empty()) g_audioRestart = true;
            else g_audioReeval = true;
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { g_audioReeval = true; return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { g_audioReeval = true; return S_OK; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { g_audioReeval = true; return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
};

struct AudioClient {
    ComPtr<IAudioClient> ac;
    WAVEFORMATEX* fmt = nullptr;
    UINT32 period = 0, buf = 0;
    bool lowLatency = false;
    ~AudioClient() {
        if (fmt) CoTaskMemFree(fmt);
    }
};

// Shared mode with the smallest engine period the driver allows (IAudioClient3), else the default.
static HRESULT OpenClient(IMMDevice* d, AudioClient& c, HANDLE evt) {
    ComPtr<IAudioClient3> ac3;
    HRESULT hr = d->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)ac3.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = ac3->GetMixFormat(&c.fmt);
        if (FAILED(hr)) return hr;
        UINT32 def = 0, fund = 0, mn = 0, mx = 0;
        if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(c.fmt, &def, &fund, &mn, &mx)) &&
            SUCCEEDED(ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, c.fmt, nullptr))) {
            c.ac = ac3;
            c.period = mn;
            c.lowLatency = mn < def;
        }
    }
    if (!c.ac) {
        if (c.fmt) {
            CoTaskMemFree(c.fmt);
            c.fmt = nullptr;
        }
        hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)c.ac.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = c.ac->GetMixFormat(&c.fmt);
        if (FAILED(hr)) return hr;
        hr = c.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, c.fmt, nullptr);
        if (FAILED(hr)) return hr;
        REFERENCE_TIME dp = 100000;
        c.ac->GetDevicePeriod(&dp, nullptr);
        c.period = (UINT32)(dp * c.fmt->nSamplesPerSec / 10000000);
    }
    hr = c.ac->SetEventHandle(evt);
    if (FAILED(hr)) return hr;
    return c.ac->GetBufferSize(&c.buf);
}

static void WaitForAudioChange(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_quit && GetTickCount64() < end && !g_audioRestart && !g_audioReeval) Sleep(25);
}

// Streams until something needs a restart. Returns false on failure.
static bool RunAudio(IMMDeviceEnumerator* en, const AudioEp& ep, HANDLE capEvt, HANDLE renEvt) {
    ComPtr<IMMDevice> capDev, renDev;
    HRESULT hr = en->GetDevice(ep.id.c_str(), &capDev);
    if (FAILED(hr)) return false;
    std::wstring renId;
    bool fellBack = false;
    renDev = ResolveOutput(en, renId, fellBack);
    if (!renDev) {
        SetAudioStatus(L"Audio: no output device");
        return false;
    }
    std::wstring renName = EndpointName(renDev.Get());
    if (fellBack) {
        Log(L"Chosen audio output %s is missing, using the default", GetSettings().outputName.c_str());
        renName += L" (chosen output missing)";
    }
    AudioClient cap, ren;
    hr = OpenClient(capDev.Get(), cap, capEvt);
    if (FAILED(hr)) {
        SetAudioStatus(Fmt(L"Audio: could not open %s (0x%08X)", ep.name.c_str(), hr));
        return false;
    }
    hr = OpenClient(renDev.Get(), ren, renEvt);
    if (FAILED(hr)) {
        SetAudioStatus(Fmt(L"Audio: could not open %s (0x%08X)", renName.c_str(), hr));
        return false;
    }
    PcmFmt in, out;
    if (!ParseFormat(cap.fmt, in) || !ParseFormat(ren.fmt, out)) {
        SetAudioStatus(L"Audio: device format is not PCM");
        return false;
    }
    ComPtr<IAudioCaptureClient> cc;
    ComPtr<IAudioRenderClient> rc;
    if (FAILED(cap.ac->GetService(IID_PPV_ARGS(&cc))) || FAILED(ren.ac->GetService(IID_PPV_ARGS(&rc)))) return false;

    auto matrix = BuildMatrix(in, out);
    AudioRing ring;
    ring.Init((size_t)in.rate, (size_t)in.ch);
    const double baseStep = (double)in.rate / out.rate;
    // The queue target starts at one card period plus one output period. Each underrun adds 2 ms of
    // headroom, up to 20 ms, so a card that delivers its packets unevenly settles at the smallest
    // delay that plays without gaps.
    // The target never comes back down. On a Cam Link 4K it settles at 17.7 ms within 40 s, and a
    // test that took 1 ms off after every 30 s without an underrun got an underrun within 3 s of
    // each step. A smaller start (half a period) only reached 17.7 ms through more underruns.
    const double baseTarget = cap.period + ren.period * baseStep + in.rate * 0.001 + 3;
    double target = baseTarget;
    // Audio timing log: average total delay as the title shows it, every CAPVIEW_TIMING_SECS.
    double latSum = 0;
    UINT64 latN = 0, latUnderruns = 0;
    ULONGLONG latT0 = GetTickCount64();
    const UINT32 targetPad = std::min(ren.buf, ren.period * 2);
    bool priming = true;
    double frac = 0, smooth = target;

    Log(L"Audio in:  %s, %s, period %u frames%s", ep.name.c_str(), FormatText(in).c_str(), cap.period,
        cap.lowLatency ? L" (low latency)" : L"");
    Log(L"Audio out: %s, %s, period %u frames, buffer %u%s", renName.c_str(), FormatText(out).c_str(), ren.period,
        ren.buf, ren.lowLatency ? L" (low latency)" : L"");
    SetAudioStatus(Fmt(L"%s (%s) > %s", ep.name.c_str(), FormatText(in).c_str(), renName.c_str()));

    if (FAILED(cap.ac->Start()) || FAILED(ren.ac->Start())) return false;

    std::vector<float> inF(in.ch), outF(out.ch);
    HANDLE evts[2] = {capEvt, renEvt};
    ULONGLONG lastStat = 0, lastReeval = 0;
    bool ok = true;

    // Glitch accounting, logged every 5 seconds while anything goes wrong. The measured card rate
    // shows whether its clock is further off than the drift control's +-0.4% range can follow.
    LARGE_INTEGER qpf, statT0;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&statT0);
    UINT64 capFrames = 0, renFrames = 0, underruns = 0, dropEvents = 0, dropFrames = 0, discont = 0, renStarve = 0;
    double stepMin = 1e9, stepMax = 0;
    bool firstReport = true;
    // Card rate from the packet timestamps: device frame position against the QPC time WASAPI
    // stamped on each packet (100 ns units). Counting frames per window instead reads up to one
    // capture period low, about 96 Hz at 480 frames over 5 s, and the first window adds startup.
    bool posValid = false;
    UINT64 pos0 = 0, qpc0 = 0, pos1 = 0, qpc1 = 0;
    while (!g_quit) {
        WaitForMultipleObjects(2, evts, FALSE, 200);
        if (g_audioRestart) break;
        if (g_audioReeval && GetTickCount64() - lastReeval > 250) {
            lastReeval = GetTickCount64();
            g_audioReeval = false;
            auto eps = EnumCapture(en);
            g_audioCount = (int)eps.size();
            int p = PickAudio(eps);
            if (p < 0 || eps[p].id != ep.id) break;
            g_audioCurIdx = p;
            std::wstring nowId;
            bool fb = false;
            if (!ResolveOutput(en, nowId, fb) || nowId != renId) break;
        }

        // Pull everything the card has delivered.
        UINT32 pk = 0;
        while (SUCCEEDED(hr = cc->GetNextPacketSize(&pk)) && pk > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            hr = cc->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
            if (FAILED(hr)) break;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                discont++;
                posValid = false;  // the device position may have jumped
            }
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpcPos) {
                if (!posValid) {
                    posValid = true;
                    pos0 = devPos;
                    qpc0 = qpcPos;
                }
                pos1 = devPos;
                qpc1 = qpcPos;
            }
            capFrames += frames;
            if (ring.n + frames > ring.cap) dropFrames += ring.n + frames - ring.cap;
            for (UINT32 f = 0; f < frames; f++) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(inF.begin(), inF.end(), 0.0f);
                else
                    for (int c = 0; c < in.ch; c++) inF[c] = ReadSample(data + (size_t)f * in.block + (size_t)c * in.container, in);
                ring.Push(inF.data());
            }
            cc->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            Log(L"Audio capture error 0x%08X", hr);
            ok = false;
            break;
        }

        // Keep the output queue at about two engine periods.
        UINT32 pad = 0;
        hr = ren.ac->GetCurrentPadding(&pad);
        if (FAILED(hr)) {
            Log(L"Audio render error 0x%08X", hr);
            ok = false;
            break;
        }
        if (pad >= targetPad) continue;
        if (pad == 0 && renFrames > 0) renStarve++;  // the device played out everything: a gap
        UINT32 want = targetPad - pad;
        BYTE* dst = nullptr;
        hr = rc->GetBuffer(want, &dst);
        if (FAILED(hr)) {
            Log(L"Audio render GetBuffer 0x%08X", hr);
            ok = false;
            break;
        }

        // Drift control: nudge the resampling ratio so the queue stays near the target.
        double fill = (double)ring.n;
        if (!priming && fill > target + in.rate * 0.040) {
            dropEvents++;
            dropFrames += (UINT64)(fill - target);
            ring.Pop((size_t)(fill - target));
            fill = target;
            smooth = target;
        }
        smooth = smooth * 0.97 + fill * 0.03;
        double err = (smooth - target) / in.rate;
        double step = baseStep * (1.0 + std::min(0.004, std::max(-0.004, err * 0.6)));
        stepMin = std::min(stepMin, step);
        stepMax = std::max(stepMax, step);
        renFrames += want;

        float gain = g_mute ? 0.0f : g_volume.load();
        for (UINT32 k = 0; k < want; k++) {
            BYTE* o = dst + (size_t)k * out.block;
            if (priming && ring.n >= target + 3) {
                priming = false;
                frac = 0;
                smooth = (double)ring.n;
            }
            if (!priming && ring.n < 4) {
                priming = true;
                underruns++;
                latUnderruns++;
                if (target < baseTarget + in.rate * 0.020) {
                    target = std::min(target + in.rate * 0.002, baseTarget + in.rate * 0.020);
                    Log(L"Audio underrun, queue target raised to %.1f ms", target * 1000.0 / in.rate);
                }
            }
            if (priming) {
                memset(o, out.container == 1 ? 0x80 : 0, out.block);
                continue;
            }
            const float *x0 = ring.At(0), *x1 = ring.At(1), *x2 = ring.At(2), *x3 = ring.At(3);
            float t = (float)frac;
            for (int c = 0; c < in.ch; c++) {
                float c1 = 0.5f * (x2[c] - x0[c]);
                float c2 = x0[c] - 2.5f * x1[c] + 2.0f * x2[c] - 0.5f * x3[c];
                float c3 = 0.5f * (x3[c] - x0[c]) + 1.5f * (x1[c] - x2[c]);
                inF[c] = ((c3 * t + c2) * t + c1) * t + x1[c];
            }
            for (int oc = 0; oc < out.ch; oc++) {
                const float* row = &matrix[(size_t)oc * in.ch];
                float v = 0;
                for (int c = 0; c < in.ch; c++) v += row[c] * inF[c];
                WriteSample(o + (size_t)oc * out.container, out, v * gain);
            }
            frac += step;
            if (frac >= 1.0) {
                size_t adv = (size_t)frac;
                ring.Pop(adv);
                frac -= (double)adv;
            }
        }
        rc->ReleaseBuffer(want, 0);

        ULONGLONG now = GetTickCount64();
        if (!priming) {
            latSum = latSum + (ring.n / (double)in.rate + pad / (double)out.rate) * 1000.0 +
                     (cap.period / (double)in.rate + ren.period / (double)out.rate) * 1000.0;
            latN++;
        }
        if (now - latT0 >= (ULONGLONG)(g_timingSecs * 1000)) {
            Log(L"Audio timing %.0f s: delay avg %.1f ms, underruns %llu, queue target %.1f ms",
                (now - latT0) / 1000.0, latN ? latSum / latN : 0.0, latUnderruns, target * 1000.0 / in.rate);
            latT0 = now;
            latSum = 0;
            latN = latUnderruns = 0;
        }
        if (now - lastStat > 500) {
            lastStat = now;
            float ms = (float)((ring.n / (double)in.rate + pad / (double)out.rate) * 1000.0 +
                               (cap.period / (double)in.rate + ren.period / (double)out.rate) * 1000.0);
            g_audioLatencyMs = priming ? 0.0f : ms;
        }

        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        double secs = (double)(t.QuadPart - statT0.QuadPart) / qpf.QuadPart;
        if (secs >= 5.0) {
            double capHz = capFrames / secs, renHz = renFrames / secs;
            if (posValid && qpc1 > qpc0 + 10000000) capHz = (double)(pos1 - pos0) * 1e7 / (double)(qpc1 - qpc0);
            pos0 = pos1;
            qpc0 = qpc1;
            bool trouble = underruns || dropEvents || dropFrames || discont || renStarve ||
                           std::fabs(capHz / in.rate - 1.0) > 0.003;
            if (trouble || firstReport)
                Log(L"Audio %.0f s: card %.1f Hz (%+.2f%%), output %.1f Hz, underruns %llu, overflow drops %llu (%llu frames), "
                    L"discontinuities %llu, output starved %llu, ratio %.4f to %.4f, queue %u of target %.0f",
                    secs, capHz, (capHz / in.rate - 1.0) * 100.0, renHz, underruns, dropEvents, dropFrames, discont, renStarve,
                    stepMin / baseStep, stepMax / baseStep, (unsigned)ring.n, target);
            firstReport = false;
            statT0 = t;
            capFrames = renFrames = underruns = dropEvents = dropFrames = discont = renStarve = 0;
            renFrames = 1;  // keeps the starvation check armed after the reset
            stepMin = 1e9;
            stepMax = 0;
        }
    }
    cap.ac->Stop();
    ren.ac->Stop();
    g_audioLatencyMs = 0;
    return ok;
}

static void AudioThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        SetAudioStatus(L"Audio: unavailable");
        return;
    }
    NotifyClient* nc = new NotifyClient;
    en->RegisterEndpointNotificationCallback(nc);
    DWORD ti = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &ti);
    HANDLE capEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    while (!g_quit) {
        g_audioRestart = false;
        g_audioReeval = false;
        auto eps = EnumCapture(en.Get());
        g_audioCount = (int)eps.size();
        int p = PickAudio(eps);
        g_audioCurIdx = p;
        if (p < 0) {
            bool off = g_audioPick == -2 || Lower(GetSettings().audioName) == L"none";
            SetAudioStatus(off ? L"Audio off (press A to pick an input)"
                               : L"No capture audio found (press A to pick an input)");
            WaitForAudioChange(1000);
            continue;
        }
        if (!RunAudio(en.Get(), eps[p], capEvt, renEvt)) WaitForAudioChange(700);
    }
    en->UnregisterEndpointNotificationCallback(nc);
    nc->Release();
    if (task) AvRevertMmThreadCharacteristics(task);
    CloseHandle(capEvt);
    CloseHandle(renEvt);
    CoUninitialize();
}

// ------------------------------------------------------------------------------------------------
// NVIDIA Smooth Motion, switched in the driver profile for this exe through NVAPI DRS. The driver
// reads the profile when the process starts, so a change applies on the next start.
// ------------------------------------------------------------------------------------------------
namespace nvdrs {
#pragma pack(push, 8)
struct Setting {
    uint32_t version;
    wchar_t name[2048];
    uint32_t id, type, location, isCurrentPredefined, isPredefinedValid;
    union Val {
        uint32_t u32;
        struct {
            uint32_t len;
            uint8_t data[4096];
        } bin;
        wchar_t str[2048];
    } pre, cur;
};
struct Application {
    uint32_t version, isPredefined;
    wchar_t appName[2048], friendlyName[2048], launcher[2048], fileInFolder[2048];
    uint32_t flags;
    wchar_t commandLine[2048];
};
struct ProfileInfo {
    uint32_t version;
    wchar_t name[2048];
    uint32_t gpuSupport, isPredefined, numApps, numSettings;
};
#pragma pack(pop)
static_assert(sizeof(Setting) == 12320, "NVDRS_SETTING_V1 size");
static_assert(sizeof(Application) == 20492, "NVDRS_APPLICATION_V4 size");
static_assert(sizeof(ProfileInfo) == 4116, "NVDRS_PROFILE_V1 size");

enum : int {
    kOk = 0, kNoPrivilege = -137, kSettingNotFound = -160, kProfileNotFound = -163, kExeNotFound = -166,
    kNoDriver = -1001, kInitFailed = -1002, kDeclined = -1003,
};
const uint32_t kSmoothMotionEnable = 0xB0D384C0, kSmoothMotionApis = 0xB0CC0875;

typedef void* Session;
typedef void* Profile;
typedef void*(__cdecl* QueryFn)(unsigned int);
struct Api {
    HMODULE dll = nullptr;
    int(__cdecl* Initialize)() = nullptr;
    int(__cdecl* Unload)() = nullptr;
    int(__cdecl* CreateSession)(Session*) = nullptr;
    int(__cdecl* DestroySession)(Session) = nullptr;
    int(__cdecl* LoadSettings)(Session) = nullptr;
    int(__cdecl* SaveSettings)(Session) = nullptr;
    int(__cdecl* CreateProfile)(Session, ProfileInfo*, Profile*) = nullptr;
    int(__cdecl* FindProfileByName)(Session, wchar_t*, Profile*) = nullptr;
    int(__cdecl* CreateApplication)(Session, Profile, Application*) = nullptr;
    int(__cdecl* FindApplicationByName)(Session, const wchar_t*, Profile*, Application*) = nullptr;
    int(__cdecl* SetSetting)(Session, Profile, Setting*) = nullptr;
    ~Api() {
        if (dll) FreeLibrary(dll);
    }
    bool Load() {
        dll = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return false;
        auto q = (QueryFn)GetProcAddress(dll, "nvapi_QueryInterface");
        if (!q) return false;
#define NVF(f, id) \
    f = (decltype(f))q(id); \
    if (!f) return false;
        NVF(Initialize, 0x0150e828)
        NVF(Unload, 0xd22bdd7e)
        NVF(CreateSession, 0x0694d52e)
        NVF(DestroySession, 0xdad9cff8)
        NVF(LoadSettings, 0x375dbd6b)
        NVF(SaveSettings, 0xfcbc7e14)
        NVF(CreateProfile, 0xcc176068)
        NVF(FindProfileByName, 0x7e4a9a0b)
        NVF(CreateApplication, 0x4347a9de)
        NVF(FindApplicationByName, 0xeee566b2)
        NVF(SetSetting, 0x577dd202)
#undef NVF
        return true;
    }
};

static uint32_t Ver(size_t size, uint32_t v) { return (uint32_t)size | (v << 16); }
}  // namespace nvdrs

static std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Returns an NVAPI status (0 on success).
static int ApplySmoothMotion(bool on) {
    using namespace nvdrs;
    auto api = std::make_unique<Api>();
    if (!api->Load()) return kNoDriver;
    if (api->Initialize() != kOk) return kInitFailed;
    Session s = nullptr;
    int st = api->CreateSession(&s);
    if (st != kOk) {
        api->Unload();
        return st;
    }
    st = api->LoadSettings(s);
    std::wstring exe = ExePath();
    exe = Lower(exe.substr(exe.find_last_of(L"\\/") + 1));
    Profile prof = nullptr;
    if (st == kOk) {
        auto app = std::make_unique<Application>();
        app->version = Ver(sizeof(Application), 4);
        st = api->FindApplicationByName(s, exe.c_str(), &prof, app.get());
        if (st == kExeNotFound) {
            wchar_t name[2048] = L"CapView";
            st = api->FindProfileByName(s, name, &prof);
            if (st == kProfileNotFound) {
                auto pi = std::make_unique<ProfileInfo>();
                pi->version = Ver(sizeof(ProfileInfo), 1);
                wcscpy_s(pi->name, L"CapView");
                st = api->CreateProfile(s, pi.get(), &prof);
            }
            if (st == kOk) {
                auto na = std::make_unique<Application>();
                na->version = Ver(sizeof(Application), 4);
                wcscpy_s(na->appName, exe.c_str());
                wcscpy_s(na->friendlyName, L"CapView");
                st = api->CreateApplication(s, prof, na.get());
            }
        }
    }
    if (st == kOk) {
        auto set = std::make_unique<Setting>();
        set->version = Ver(sizeof(Setting), 1);
        set->id = kSmoothMotionEnable;
        set->cur.u32 = on ? 1 : 0;
        st = api->SetSetting(s, prof, set.get());
        if (st == kOk && on) {
            // Allow DX11 (CapView's API) along with DX12 and Vulkan. Older drivers may not know the key.
            auto apis = std::make_unique<Setting>();
            apis->version = Ver(sizeof(Setting), 1);
            apis->id = kSmoothMotionApis;
            apis->cur.u32 = 7;
            int a = api->SetSetting(s, prof, apis.get());
            if (a != kOk) Log(L"Smooth Motion API list not set (NVAPI %d)", a);
        }
    }
    if (st == kOk) st = api->SaveSettings(s);
    api->DestroySession(s);
    api->Unload();
    return st;
}

// RTX Encore (github.com/SilyNoMeta/rtx-encore) runs Smooth Motion on RTX 20 and 30 cards from inside
// the program, as a DLL copied next to the exe under a name Windows loads from there, such as
// dxgi.dll. CapView links dxgi.dll and d3d11.dll itself, so those two load from the exe folder.
// version.dll and winmm.dll come in through System32 DLLs and never do. It keeps rtx-encore.jsonc and rtx-encore-logs beside itself. Returns the file name of
// the loaded copy, or an empty string.
static std::wstring FindRtxEncore() {
    std::wstring exe = ExePath();
    std::wstring dir = Lower(exe.substr(0, exe.find_last_of(L"\\/") + 1));
    DWORD a = GetFileAttributesW((dir + L"rtx-encore.jsonc").c_str());
    DWORD b = GetFileAttributesW((dir + L"rtx-encore-logs").c_str());
    bool marked = a != INVALID_FILE_ATTRIBUTES || (b != INVALID_FILE_ATTRIBUTES && (b & FILE_ATTRIBUTE_DIRECTORY));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    static const wchar_t* kNames[] = {L"rtx-encore.dll", L"rtx-encore.asi", L"version.dll", L"dinput8.dll",
                                      L"winmm.dll", L"dxgi.dll", L"d3d11.dll", L"dsound.dll", L"wininet.dll",
                                      L"winhttp.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"};
    std::wstring found;
    MODULEENTRY32W me = {sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok && found.empty(); ok = Module32NextW(snap, &me)) {
        std::wstring path = Lower(me.szExePath);
        size_t cut = path.find_last_of(L"\\/") + 1;
        if (path.compare(0, cut, dir) != 0) continue;
        std::wstring name = path.substr(cut);
        for (const wchar_t* n : kNames) {
            // A proxy name alone could be ReShade or Special K, so it also needs RTX Encore's files.
            if (name == n && (marked || name.rfind(L"rtx-encore", 0) == 0)) found = me.szModule;
        }
    }
    CloseHandle(snap);
    return found;
}

// RTX Encore rewrites rtx-encore-logs\rtx-encore-status.json while it runs. Returns its
// smoothMotion state ("active" and so on) when the file belongs to this process, or an empty
// string. The file has no frame rate: RTX Encore's own counter multiplies the source rate by 2
// for Smooth Motion too, since the driver doesn't report the frames it adds.
static std::wstring EncoreSmoothState() {
    std::wstring exe = ExePath();
    std::wstring path = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"rtx-encore-logs\\rtx-encore-status.json";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    char buf[16384];
    DWORD n = 0;
    BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    if (!ok) return L"";
    std::string s(buf, n);
    // The value after "key": within the object that starts at "section".
    auto value = [&](const char* section, const char* key) -> std::string {
        size_t a = s.find(section);
        if (a == std::string::npos) return "";
        size_t k = s.find(key, a);
        size_t end = s.find('}', a);
        if (k == std::string::npos || k > end) return "";
        size_t c = s.find(':', k);
        if (c == std::string::npos) return "";
        size_t v = s.find_first_not_of(" \t\r\n\"", c + 1);
        size_t e = v == std::string::npos ? v : s.find_first_of("\",}\r\n", v);
        return v == std::string::npos || e == std::string::npos ? "" : s.substr(v, e - v);
    };
    if (value("\"process\"", "\"id\"") != std::to_string(GetCurrentProcessId())) return L"";
    std::string st = value("\"smoothMotion\"", "\"state\"");
    return std::wstring(st.begin(), st.end());
}
static std::wstring g_encoreSmooth;  // EncoreSmoothState(), refreshed every 2 s

// Whether rtx-encore.jsonc has Smooth Motion switched on. RTX Encore writes its status file a
// moment after the first frames, so this covers the start until the status arrives.
static bool EncoreSmoothConfigured() {
    std::wstring exe = ExePath();
    std::wstring path = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"rtx-encore.jsonc";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD n = 0;
    BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    std::string s(buf, n);
    size_t k = s.find("\"smoothMotionEnabled\"");
    if (k == std::string::npos) return false;
    size_t v = s.find_first_not_of(" \t:", k + 21);
    return v != std::string::npos && s.compare(v, 4, "true") == 0;
}

// Switches Smooth Motion in rtx-encore.jsonc, the same setting as RTX Encore's menu, and sets its
// graphics API to Direct3D 11 when turning it on. Only those values change; the rest of the file,
// comments and line endings included, stays byte for byte. RTX Encore reads it at start, so it
// applies after a restart. Returns 1 when the file changed, 0 when it already matched, -1 on failure.
static int EncoreSetSmooth(bool on) {
    std::wstring exe = ExePath();
    std::wstring path = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"rtx-encore.jsonc";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    std::string s((size_t)std::min<LONGLONG>(size.QuadPart, 1 << 22), '\0');
    DWORD n = 0;
    BOOL ok = ReadFile(f, &s[0], (DWORD)s.size(), &n, nullptr);
    CloseHandle(f);
    if (!ok || n != s.size()) return -1;
    std::string orig = s;
    // Replaces the bare value after "key": with want, when it is one of the given alternatives.
    auto set = [&](const char* key, std::initializer_list<const char*> from, const char* want) {
        size_t k = s.find(key);
        if (k == std::string::npos) return false;
        size_t v = s.find_first_not_of(" \t:", k + strlen(key));
        if (v == std::string::npos) return false;
        for (const char* a : from) {
            size_t len = strlen(a);
            if (s.compare(v, len, a) == 0) {
                s.replace(v, len, want);
                return true;
            }
        }
        return false;
    };
    if (!set("\"smoothMotionEnabled\"", {"true", "false"}, on ? "true" : "false")) return -1;
    if (on) set("\"smoothMotionApi\"", {"\"d3d12\"", "\"vulkan\"", "\"d3d11\""}, "\"d3d11\"");
    if (s == orig) return 0;
    std::wstring tmp = path + L".capview-tmp";
    HANDLE w = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w == INVALID_HANDLE_VALUE) return -1;
    DWORD wrote = 0;
    ok = WriteFile(w, s.data(), (DWORD)s.size(), &wrote, nullptr) && wrote == s.size();
    CloseHandle(w);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return -1;
    }
    return 1;
}

// True on GPUs where NVIDIA's own driver runs Smooth Motion (RTX 40 and 50). Set from the adapter
// name by UpdateTitle.
static bool g_smoothNative = false;

// Whether the driver is adding Smooth Motion frames. RTX Encore says so in its status file.
// Without it, only RTX 40 and 50 cards run Smooth Motion.
static bool SmoothRunning() { return g_encore.empty() ? g_smoothNative : g_encoreSmooth == L"active"; }

// Frames on screen with Smooth Motion. Through RTX Encore the swap chain's display count already
// includes the driver's frames, so a count well above what CapView drew is a measurement. Otherwise
// the driver's frames aren't in it, and twice CapView's rate is the estimate.
static double SmoothShownFps(double out, double drawn, bool statsOk, bool& measured) {
    measured = statsOk && drawn > 1 && out > drawn * 1.5;
    return measured ? out : out * 2;
}

static std::wstring SmoothMotionError(int st) {
    switch (st) {
        case nvdrs::kNoDriver: return L"No NVIDIA driver found (nvapi64.dll is missing), so Smooth Motion isn't available.";
        case nvdrs::kInitFailed: return L"The NVIDIA driver didn't start NVAPI. Is an NVIDIA GPU active?";
        case nvdrs::kDeclined: return L"Changing the NVIDIA profile needs administrator rights, and the prompt was declined.";
        case nvdrs::kNoPrivilege: return L"Changing the NVIDIA profile needs administrator rights.";
        case nvdrs::kSettingNotFound:
            // The driver only registers the Smooth Motion settings on GPUs it supports, so on an
            // unsupported card even writing the setting fails with NVAPI_SETTING_NOT_FOUND.
            return L"This NVIDIA driver doesn't offer Smooth Motion on this GPU (NVAPI status -160, setting not found). "
                   L"NVIDIA supports it on RTX 40 and 50 cards. An RTX 30 card needs the community RTX 30 Smooth Motion "
                   L"patch, and this driver doesn't have it.\n\nRTX Encore (github.com/SilyNoMeta/rtx-encore) adds Smooth "
                   L"Motion on RTX 30 cards for drivers up to 621.24. Copy its rtx-encore.dll next to CapView.exe as "
                   L"dxgi.dll, start CapView and pick NVIDIA Smooth Motion again.\n\nCapView interpolation works on any GPU.";
    }
    return Fmt(L"The NVIDIA profile update failed (NVAPI status %d).", st);
}

// Tries without elevation first, then runs a copy of CapView elevated just for the profile write.
static int ApplySmoothMotionElevating(bool on, HWND owner) {
    int st = ApplySmoothMotion(on);
    if (st != nvdrs::kNoPrivilege) return st;
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = on ? L"--nv-smooth-motion 1" : L"--nv-smooth-motion 0";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return nvdrs::kDeclined;
    WaitForSingleObject(sei.hProcess, 20000);
    DWORD code = (DWORD)nvdrs::kInitFailed;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

// ------------------------------------------------------------------------------------------------
// AMD Fluid Motion Frames (AFMF), the Radeon driver's own frame generation, switched through AMD's
// ADLX library (amdadlx64.dll, installed with the Radeon driver). AFMF is one global driver setting,
// so CapView turns it on only while it runs in AFMF mode, remembers in the ini that it did, and turns
// it off again when CapView leaves that mode or exits. If AFMF was already on, CapView leaves it alone.
namespace afmf {
enum { kOk = 0, kNoDriver = -1, kInitFailed = -2, kNoInterface = -3, kNotSupported = -4, kSetFailed = -5 };
typedef int(__cdecl* InitFn)(uint64_t version, void** system);
typedef int(__cdecl* VersionFn)(uint64_t* version);
typedef int(__cdecl* TermFn)();
// ADLX interfaces are C++ classes. The slot numbers come from the C vtables in AMD's ADLX headers:
// IADLXSystem::Get3DSettingsServices is 7. Every IADLXInterface starts with Acquire 0, Release 1 and
// QueryInterface 2. IADLX3DSettingsServices1::GetAMDFluidMotionFrames is 17, and
// IADLX3DAMDFluidMotionFrames has IsSupported 3, IsEnabled 4 and SetEnabled 5.
template <class F>
static F Slot(void* obj, int i) {
    return reinterpret_cast<F>((*reinterpret_cast<void***>(obj))[i]);
}
static void Release(void* obj) {
    if (obj) Slot<long(__stdcall*)(void*)>(obj, 1)(obj);
}
// Starts ADLX, gets the AFMF interface, runs fn on it and shuts ADLX down again. Returns a status
// above, or what fn returns.
template <class Fn>
static int With(Fn fn) {
    HMODULE dll = LoadLibraryExW(L"amdadlx64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return kNoDriver;
    auto init = (InitFn)GetProcAddress(dll, "ADLXInitialize");
    auto ver = (VersionFn)GetProcAddress(dll, "ADLXQueryFullVersion");
    auto term = (TermFn)GetProcAddress(dll, "ADLXTerminate");
    int st = kInitFailed;
    if (init && term) {
        const uint64_t mine = (2ull << 48) | 125;  // ADLX SDK 2.0.0.125, the headers the slots come from
        void* sys = nullptr;
        int r = init(mine, &sys);
        uint64_t theirs = 0;
        if (r != 0 && ver && ver(&theirs) == 0 && theirs && theirs < mine) {
            sys = nullptr;
            r = init(theirs, &sys);  // an older driver's ADLX, the slots used here haven't moved
        }
        if (r == 0 && sys) {
            st = kNoInterface;
            void *svc = nullptr, *svc1 = nullptr, *fm = nullptr;
            if (Slot<int(__stdcall*)(void*, void**)>(sys, 7)(sys, &svc) == 0 && svc &&
                Slot<int(__stdcall*)(void*, const wchar_t*, void**)>(svc, 2)(svc, L"IADLX3DSettingsServices1", &svc1) == 0 &&
                svc1 && Slot<int(__stdcall*)(void*, void**)>(svc1, 17)(svc1, &fm) == 0 && fm)
                st = fn(fm);
            Release(fm);
            Release(svc1);
            Release(svc);
            term();
        }
        if (st == kInitFailed) Log(L"ADLX didn't start (ADLX result %d, driver ADLX version %llX)", r, theirs);
    }
    FreeLibrary(dll);
    return st;
}
// Reads whether this driver offers AFMF and whether it's on.
static int Query(bool* supported, bool* enabled) {
    return With([&](void* fm) {
        bool s = false, e = false;
        if (Slot<int(__stdcall*)(void*, bool*)>(fm, 3)(fm, &s) != 0) return (int)kNotSupported;
        if (s && Slot<int(__stdcall*)(void*, bool*)>(fm, 4)(fm, &e) != 0) e = false;
        *supported = s;
        *enabled = e;
        return (int)kOk;
    });
}
// Switches AFMF in the driver. Returns kNotSupported when this GPU or driver doesn't offer it.
static int Set(bool on) {
    return With([&](void* fm) {
        bool s = false;
        if (Slot<int(__stdcall*)(void*, bool*)>(fm, 3)(fm, &s) != 0 || !s) return (int)kNotSupported;
        return Slot<int(__stdcall*)(void*, bool)>(fm, 5)(fm, on) == 0 ? (int)kOk : (int)kSetFailed;
    });
}
}  // namespace afmf

// What CapView last heard from the driver about AFMF, for the overlay, the title and settings.
static std::atomic<int> g_afmfStatus{afmf::kOk};
static std::atomic<bool> g_afmfOn{false};
// XeSS frame generation: whether the proxy swap chain runs, and why not when it doesn't.
static std::atomic<bool> g_xessOn{false};
static std::mutex g_xessMx;
static std::wstring g_xessErr;
static std::wstring XessErr() {
    std::lock_guard<std::mutex> l(g_xessMx);
    return g_xessErr;
}
static bool XessFollow(bool want) {
    std::wstring err;
    bool ok = g_renderer.SetXess(want, err);
    g_xessOn = want && ok;
    std::lock_guard<std::mutex> l(g_xessMx);
    g_xessErr = err;
    return ok;
}

// Makes AFMF follow CapView's mode. want=true turns it on in the driver if it's off, and records in
// the ini (afmfOwned) that CapView did it. want=false turns it off again, but only when CapView was
// the one that turned it on, so a user who runs AFMF for games keeps it.
static int AfmfFollow(bool want) {
    bool owned = IniGet(L"afmfOwned", L"0") == L"1";
    if (!want && !owned) {
        g_afmfOn = false;
        return afmf::kOk;
    }
    ULONGLONG t0 = GetTickCount64();
    bool sup = false, on = false;
    int st = afmf::Query(&sup, &on);
    if (st == afmf::kOk && !sup) st = afmf::kNotSupported;
    if (st == afmf::kOk && want != on) {
        st = afmf::Set(want);
        if (st == afmf::kOk) {
            on = want;
            if (want) IniPut(L"afmfOwned", L"1");
        }
    }
    // Off, or gone with the GPU or driver, means there's nothing left to undo later.
    if (!want && (st == afmf::kOk || st == afmf::kNoDriver || st == afmf::kNotSupported)) IniPut(L"afmfOwned", L"0");
    g_afmfStatus = st;
    g_afmfOn = st == afmf::kOk && on && want;
    Log(L"AMD Fluid Motion Frames %s: status %d, supported %d, on %d, CapView turned it on %d, %llu ms",
        want ? L"wanted" : L"not wanted", st, (int)sup, (int)on, IniGet(L"afmfOwned", L"0") == L"1",
        GetTickCount64() - t0);
    return st;
}

static std::wstring AfmfError(int st) {
    switch (st) {
        case afmf::kNoDriver:
            return L"No AMD Radeon driver found (amdadlx64.dll is missing), so AMD Fluid Motion Frames isn't available.";
        case afmf::kNotSupported:
            return L"The AMD driver doesn't offer Fluid Motion Frames on this PC. AMD supports it on Radeon RX 6000 "
                   L"cards and newer and on Radeon 700M and 800M graphics, with a current Adrenalin driver.";
        case afmf::kSetFailed:
            return L"The AMD driver didn't accept the Fluid Motion Frames setting. Turn it on in AMD Software, "
                   L"Gaming > Graphics > AMD Fluid Motion Frames.";
        default:
            return Fmt(L"CapView couldn't reach AMD's ADLX library to switch Fluid Motion Frames (status %d). "
                       L"Updating the Adrenalin driver usually fixes this.", st);
    }
}

// Short form for the overlay and the title.
static const wchar_t* AfmfShort(int st) {
    switch (st) {
        case afmf::kNoDriver: return L"no AMD driver";
        case afmf::kNotSupported: return L"not available on this GPU";
        default: return L"driver error";
    }
}

static bool IsAmdGpu(const std::wstring& gpu) {
    return Contains(gpu, L"AMD") || Contains(gpu, L"Radeon");
}

// ------------------------------------------------------------------------------------------------
// --list
// ------------------------------------------------------------------------------------------------
static HANDLE g_out = nullptr;
static void Out(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!g_out || g_out == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    if (GetFileType(g_out) == FILE_TYPE_CHAR) {
        WriteConsoleW(g_out, buf, (DWORD)wcslen(buf), &w, nullptr);
        return;
    }
    char u8[8192];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
    if (n > 1) WriteFile(g_out, u8, n - 1, &w, nullptr);
}
static void ListDevices() {
    auto vids = EnumVideo();
    int auto_v = PickVideo(vids);
    Out(L"Video devices:\n");
    for (size_t i = 0; i < vids.size(); i++) {
        Out(L"  [%zu] %s  (score %d)%s\n", i, vids[i].name.c_str(), vids[i].score, (int)i == auto_v ? L"  <- auto" : L"");
        ComPtr<IMFMediaSource> src;
        if (FAILED(CreateSource(vids[i].link, &src))) continue;
        ComPtr<IMFSourceReader> r;
        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &r))) {
            auto modes = ListNativeModes(r.Get());
            int best = PickMode(modes, vids[i].link, vids[i].name);
            for (size_t m = 0; m < modes.size(); m++)
                Out(L"        %s%s\n", ModeText(modes[m].f).c_str(), (int)m == best ? L"  <- auto" : L"");
        }
        src->Shutdown();
    }
    if (auto_v >= 0) {
        std::lock_guard<std::mutex> l(g_statusMx);
        g_curVideo.name = vids[auto_v].name;
        g_curVideo.container = vids[auto_v].container;
    }
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    auto eps = EnumCapture(en.Get());
    int auto_a = PickAudio(eps);
    Out(L"Audio inputs:\n");
    for (size_t i = 0; i < eps.size(); i++) {
        std::wstring fmt;
        ComPtr<IMMDevice> d;
        ComPtr<IAudioClient> ac;
        WAVEFORMATEX* wf = nullptr;
        PcmFmt p;
        if (SUCCEEDED(en->GetDevice(eps[i].id.c_str(), &d)) &&
            SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf())) &&
            SUCCEEDED(ac->GetMixFormat(&wf))) {
            fmt = ParseFormat(wf, p) ? FormatText(p) : L"unsupported format";
            CoTaskMemFree(wf);
        }
        CurVideoDev v;
        {
            std::lock_guard<std::mutex> l(g_statusMx);
            v = g_curVideo;
        }
        Out(L"  [%zu] %s  %s  (score %d)%s\n", i, eps[i].name.c_str(), fmt.c_str(), AudioScore(eps[i], v),
                (int)i == auto_a ? L"  <- auto" : L"");
    }
    std::wstring outId;
    bool fb = false;
    ResolveOutput(en.Get(), outId, fb);
    Out(L"Audio outputs:\n");
    auto outs = EnumEndpoints(en.Get(), eRender);
    for (size_t i = 0; i < outs.size(); i++)
        Out(L"  [%zu] %s%s\n", i, outs[i].name.c_str(), outs[i].id == outId ? L"  <- used" : L"");
}

// ------------------------------------------------------------------------------------------------
// Window
// ------------------------------------------------------------------------------------------------
static HWND g_hwnd = nullptr;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_wp = {sizeof(g_wp)};
static ULONGLONG g_lastFpsTick = 0;
static uint32_t g_lastFpsFrames = 0;
static double g_shownFps = 0;

// While fullscreen, CapView is a topmost window whenever nothing the user is working in covers it.
// A window maximized on the next monitor reaches 8 px onto CapView's screen with its invisible
// resize border. If that window sits above CapView in z-order, DWM composes CapView instead of
// flipping its frames straight to the screen, and about one frame in seven never shows. So CapView
// goes topmost when it gets focus, and when focus moves to a window on another monitor while no
// visible window covers it. It drops to just below a window that is brought up on its own screen
// or dragged onto it.
static HWINEVENTHOOK g_fgHook = nullptr, g_moveHook = nullptr;
static DWORD g_moveHookPid = 0;

static bool VisibleFrame(HWND w, RECT* r) {
    if (DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)) != S_OK && !GetWindowRect(w, r)) return false;
    return r->right > r->left && r->bottom > r->top;
}

// Windows that never decide where CapView goes: hidden, minimized and cloaked ones, topmost ones
// (Start, the task switcher, the taskbar, tooltips), and the desktop.
static bool IgnoredWindow(HWND w) {
    if (!IsWindowVisible(w) || IsIconic(w)) return true;
    if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) return true;
    DWORD cloaked = 0;
    if (DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK && cloaked) return true;
    wchar_t cls[16];
    return GetClassNameW(w, cls, ARRAYSIZE(cls)) && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"));
}

static bool CoversCapView(HWND w) {
    RECT cv, r, both;
    return GetWindowRect(g_hwnd, &cv) && VisibleFrame(w, &r) && IntersectRect(&both, &cv, &r);
}

// "chrome.exe [Chrome_WidgetWin_1] at 0,0 to 3440,1392" for the log.
static std::wstring DescribeWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::wstring exe = L"?";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            exe = path;
            size_t slash = exe.find_last_of(L'\\');
            if (slash != std::wstring::npos) exe.erase(0, slash + 1);
        }
        CloseHandle(p);
    }
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    RECT r = {};
    VisibleFrame(w, &r);
    return Fmt(L"%s [%s] at %d,%d to %d,%d", exe.c_str(), cls, r.left, r.top, r.right, r.bottom);
}

static void PlaceCapView() {
    if (!g_fullscreen) return;
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    bool topmost = (GetWindowLongW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (fg == g_hwnd || GetAncestor(fg, GA_ROOTOWNER) == g_hwnd) {
        if (!topmost) {
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
            Log(L"Topmost: CapView has focus");
        }
        return;
    }
    if (IgnoredWindow(fg)) return;
    if (CoversCapView(fg)) {
        // Placed right after a window that isn't topmost, CapView stops being topmost too.
        if (topmost) {
            SetWindowPos(g_hwnd, fg, 0, 0, 0, 0, flags);
            Log(L"Topmost off: the window in front is on CapView's screen, %s", DescribeWindow(fg).c_str());
        }
        return;
    }
    if (topmost) return;
    for (HWND w = GetWindow(g_hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (!IgnoredWindow(w) && GetAncestor(w, GA_ROOTOWNER) != g_hwnd && CoversCapView(w)) return;
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    Log(L"Topmost: the window in front is on another screen, %s", DescribeWindow(fg).c_str());
}

static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND w, LONG obj, LONG child, DWORD, DWORD);

// Follows the moves of the window in front, so one dragged onto CapView's screen comes out from
// behind CapView as soon as it gets there. The hook covers only that window's process.
static void WatchMoves(HWND fg) {
    DWORD pid = 0;
    if (g_fullscreen && fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) pid = 0;
    if (pid == g_moveHookPid) return;
    if (g_moveHook) UnhookWinEvent(g_moveHook);
    g_moveHook = pid ? SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent, pid,
                                       0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)
                     : nullptr;
    g_moveHookPid = g_moveHook ? pid : 0;
}

static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND w, LONG obj, LONG child, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) WatchMoves(w);
    else if (obj != OBJID_WINDOW || child != CHILDID_SELF || w != GetForegroundWindow()) return;
    PlaceCapView();
}

static void FollowFocus(bool on) {
    if (on && !g_fgHook)
        g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                   WINEVENT_OUTOFCONTEXT);
    if (!on && g_fgHook) {
        UnhookWinEvent(g_fgHook);
        g_fgHook = nullptr;
    }
    WatchMoves(on ? GetForegroundWindow() : nullptr);
}

static void ToggleFullscreen(HWND h) {
    DWORD style = GetWindowLongW(h, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetWindowPlacement(h, &g_wp) && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
            if (g_exclusive && g_fgMode != FG_XESS) g_renderer.SetExclusive(true);
            FollowFocus(true);
            PlaceCapView();
        }
    } else {
        g_renderer.SetExclusive(false);
        g_fullscreen = false;
        FollowFocus(false);
        bool topmost = (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_wp);
        // A topmost CapView becomes the highest ordinary window. Otherwise its place stays as it is.
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (topmost ? 0 : SWP_NOZORDER));
    }
}

static double DisplayHz(HWND h) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
}

// Asks DXGI whether the monitor under the window is in Windows HDR mode. A fresh factory is used each
// time because a factory caches output state from when it was created.
static void CheckDisplayHdr(HWND h) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    bool hdr = false;
    float maxNits = 0;
    ComPtr<IDXGIFactory1> f;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++, o.Reset()) {
                ComPtr<IDXGIOutput6> o6;
                DXGI_OUTPUT_DESC1 d;
                if (FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&d)) || d.Monitor != mon) continue;
                hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                maxNits = d.MaxLuminance;
            }
        }
    }
    // SDR content brightness slider for this monitor, in units where 1000 means 80 nits.
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    float white = 203;
    UINT32 np = 0, nm = 0;
    if (GetMonitorInfoW(mon, &mi) && GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
                sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sn.header.size = sizeof(sn);
                sn.header.adapterId = paths[i].sourceInfo.adapterId;
                sn.header.id = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, mi.szDevice)) continue;
                DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
                wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                wl.header.size = sizeof(wl);
                wl.header.adapterId = paths[i].targetInfo.adapterId;
                wl.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel)
                    white = wl.SDRWhiteLevel / 1000.0f * 80.0f;
                break;
            }
        }
    }
    if (hdr != g_displayHdr.load() || std::fabs(maxNits - g_displayMaxNits.load()) > 1)
        Log(L"Display: Windows HDR %s, peak %.0f nits, SDR white %.0f nits", hdr ? L"on" : L"off", maxNits, white);
    g_displayHdr = hdr;
    g_displayMaxNits = maxNits;
    g_sdrWhiteNits = white;
}

// Output frame rate measured from what reached the screen, updated by UpdateTitle.
static double g_outFps = 0, g_inFps = 0, g_drawFps = 0, g_contentFps = 0, g_xessFps = 0;
static uint64_t g_lastDisp = 0, g_lastIn = 0, g_lastPres = 0, g_lastNew = 0;

// Content and card rates over a 1 second window, so a 30 fps game on a 60 Hz signal doesn't flicker
// between readings when the title updates more often than that.
struct RateWindow {
    uint64_t newF = 0, inF = 0;
    double secs = 0;
};

static void LogContentRate(double content, double card) {
    static ULONGLONG next = 0;
    static uint64_t lastNear = 0, lastSmall = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 60000;
    uint64_t nearNow = g_nearRepeats, smallNow = g_smallChanges;
    Log(L"Content %.1f fps from a %.1f fps signal, %llu near-repeat frames and %llu small-change frames since the last report",
        content, card, nearNow - lastNear, smallNow - lastSmall);
    lastNear = nearNow;
    lastSmall = smallNow;
}

static void UpdateFps(HWND h, double dt) {
    uint64_t disp = g_displayed, in = g_framesIn, pres = g_presents, fresh = g_framesNew;
    static RateWindow win;
    if (dt > 0 && g_lastPres) {
        g_outFps = (disp - g_lastDisp) / dt;
        g_drawFps = (pres - g_lastPres) / dt;
        win.newF += fresh - g_lastNew;
        win.inF += in - g_lastIn;
        win.secs += dt;
        if (win.secs >= 1.0) {
            g_contentFps = win.newF / win.secs;
            g_inFps = win.inF / win.secs;
            win = RateWindow();
            if (g_videoActive && g_inFps > 1) LogContentRate(g_contentFps, g_inFps);
        }
    }
    if (!g_videoActive) {
        g_contentFps = g_inFps = 0;
        win = RateWindow();
    }
    if (g_xessOn && !g_renderer.XessOn()) {  // it didn't come back after a failed resize
        g_xessOn = false;
        std::lock_guard<std::mutex> l(g_xessMx);
        g_xessErr = L"It stopped after a window resize it couldn't recover from; capview.log has the details.";
    }
    static uint64_t lastXess = 0;
    uint64_t xs = g_xessShown;
    g_xessFps = (dt > 0 && lastXess && xs >= lastXess) ? (xs - lastXess) / dt : 0;
    lastXess = xs;
    g_lastDisp = disp;
    g_lastIn = in;
    g_lastPres = pres;
    g_lastNew = fresh;
    double worst = g_worstUs.exchange(0) / 1000.0;
    g_dbgWorstMs = std::max(g_dbgWorstMs, worst);
    if (!g_showFps) return;
    bool ok = g_statsOk;
    double hz = DisplayHz(h);
    double out = ok ? g_outFps : g_drawFps;
    std::wstring t;
    if (g_videoActive && g_inFps > 1) {
        double rep = std::max(0.0, g_inFps - g_contentFps);
        t = Fmt(L"Content  %6.1f fps  (signal %.0f Hz, %.0f repeats/s)", g_contentFps, g_inFps, rep < 0.5 ? 0.0 : rep);
        if (g_contentFps < 0.5) t += L", picture not changing";
    } else {
        t = Fmt(L"Content  %6.1f fps  (no video)", 0.0);
    }
    if (ok) {
        t += Fmt(L"\nOutput   %6.1f fps", out);
        if (out > 0.5) t += Fmt(L"  %5.1f ms avg  %5.1f ms worst", 1000.0 / out, worst);
    } else {
        t += Fmt(L"\nDrawn    %6.1f fps  (display count not available)", out);
    }
    if (ok) {
        double lost = std::max(0.0, g_drawFps - g_outFps);
        t += Fmt(L"\nDrawn    %6.1f fps, %.1f/s never shown", g_drawFps, lost < 0.5 ? 0.0 : lost);
    }
    t += hz > 0 ? Fmt(L"\nDisplay  %4.0f Hz, %s%s", hz, g_vsync ? L"vsync" : L"vsync off",
                      g_renderer.Exclusive() ? L", exclusive fullscreen" : L"")
                : std::wstring();
    static const wchar_t* srcNames[] = {L"SDR", L"HDR10", L"HLG"};
    t += Fmt(L"\nColor    %s in, %s out%s", g_videoActive ? srcNames[g_srcHdr.load()] : L"no video",
             g_hdrOut ? L"HDR10" : L"SDR", g_displayHdr ? L", Windows HDR on" : L"");
    switch (g_fgMode.load()) {
        case FG_CAPVIEW:
            if (FgPausedForSmooth())
                t += L"\nFrame gen paused, RTX Encore Smooth Motion is on\n  Turn Smooth Motion off with Insert, Frame rate, or pick\n  Smooth Motion as the frame generation mode";
            else
                t += g_fgActive ? Fmt(L"\nFrame gen %dx", g_fgMult.load()) : std::wstring(L"\nFrame gen waiting for video");
            break;
        case FG_SMOOTH:
            if (SmoothRunning()) {
                bool measured;
                double sm = SmoothShownFps(out, g_drawFps, ok, measured);
                t += measured ? Fmt(L"\nSmooth Motion 2x, %.0f fps on screen, %.0f from CapView", sm, g_drawFps)
                              : Fmt(L"\nSmooth Motion 2x, about %.0f fps on screen\n  (output x 2, the driver doesn't report its frames)", sm);
            } else if (g_encore.empty())
                t += L"\nSmooth Motion not available on this GPU\n  (RTX 40 and 50 only, RTX 30 needs RTX Encore)";
            else if (g_encoreSmooth.empty())
                t += L"\nSmooth Motion waiting for RTX Encore's status";
            else
                t += Fmt(L"\nSmooth Motion not running in RTX Encore (%s)\n  Insert, Frame rate tab, turn Smooth Motion on",
                         g_encoreSmooth.empty() ? L"no status" : g_encoreSmooth.c_str());
            break;
        case FG_AMD:
            if (!g_afmfOn)
                t += std::wstring(L"\nAMD Fluid Motion Frames off, ") + AfmfShort(g_afmfStatus);
            else if (!g_fullscreen)
                t += L"\nAMD Fluid Motion Frames on, it adds frames\n  in fullscreen only, press F11";
            else
                t += Fmt(L"\nAMD Fluid Motion Frames 2x, about %.0f fps on screen\n  (output x 2, the driver doesn't report its frames)",
                         g_drawFps * 2);
            break;
        case FG_XESS:
            if (!g_xessOn)
                t += L"\nXeSS frame generation off, see settings (S)";
            else
                t += Fmt(L"\nXeSS frame generation 2x, %.0f fps presented, %.0f from CapView", g_xessFps, g_drawFps);
            break;
    }
    if (g_fgMode == FG_OFF && g_encoreSmoothOn)
        t += L"\nRTX Encore Smooth Motion is on and doubles\n  this output. Turn it off with Insert, Frame rate";
    std::lock_guard<std::mutex> l(g_overlayMx);
    g_overlayText = t;
}

// ------------------------------------------------------------------------------------------------
// Debug mode, crash dumps and debug reports
// ------------------------------------------------------------------------------------------------
static const wchar_t* kVersion = L"1.0.0";
static const wchar_t* kBuild = L"" __DATE__ " " __TIME__;

static std::wstring ExeDir() {
    std::wstring e = ExePath();
    return e.substr(0, e.find_last_of(L"\\/") + 1);
}

static std::wstring RegStr(HKEY root, const wchar_t* path, const wchar_t* name) {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}
static bool RegDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD& v) {
    DWORD size = sizeof(v);
    return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS;
}

static std::wstring ProcessName(DWORD pid) {
    std::wstring n;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, buf, &len)) {
            n = buf;
            n = n.substr(n.find_last_of(L"\\/") + 1);
        }
        CloseHandle(p);
    }
    return n.empty() ? Fmt(L"pid %u", pid) : n;
}

// Reports go to the developer, often in public, so the Windows user folder becomes %USERPROFILE%.
static std::wstring HidePaths(std::wstring s) {
    wchar_t prof[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH) || !prof[0]) return s;
    std::wstring p = Lower(prof);
    for (size_t at = 0; (at = Lower(s).find(p, at)) != std::wstring::npos;) {
        s.replace(at, p.size(), L"%USERPROFILE%");
        at += 13;
    }
    return s;
}
// The same for a UTF-8 or ANSI file's bytes, with single and doubled backslashes (JSON escapes them).
static std::string HidePathsBytes(std::string s) {
    wchar_t prof[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH) || !prof[0]) return s;
    char u8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, prof, -1, u8, sizeof(u8), nullptr, nullptr);
    std::string one = u8, two;
    for (char c : one) {
        two += c;
        if (c == '\\') two += '\\';
    }
    auto lower = [](std::string v) {
        for (auto& c : v) c = (char)tolower((unsigned char)c);
        return v;
    };
    for (const std::string& pat : {two, one}) {
        std::string lp = lower(pat), ls = lower(s);
        std::string out;
        size_t from = 0;
        for (size_t at; (at = ls.find(lp, from)) != std::string::npos; from = at + lp.size())
            out += s.substr(from, at - from) + "%USERPROFILE%";
        out += s.substr(from);
        s = out;
    }
    return s;
}

// The monitor's own name, such as "AW3423DWF", for a GDI device name such as \\.\DISPLAY1.
static std::wstring MonitorName(const wchar_t* gdiName) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return L"";
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return L"";
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || _wcsicmp(src.viewGdiDeviceName, gdiName)) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) return tgt.monitorFriendlyDeviceName;
    }
    return L"";
}

static int NotificationState() {
    QUERY_USER_NOTIFICATION_STATE q = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&q))) return 0;
    return (int)q;
}
static const wchar_t* NotificationStateText(int q) {
    switch (q) {
        case QUNS_NOT_PRESENT: return L"screen saver or locked";
        case QUNS_BUSY: return L"busy (a full-screen window)";
        case QUNS_RUNNING_D3D_FULL_SCREEN: return L"Direct3D exclusive fullscreen";
        case QUNS_PRESENTATION_MODE: return L"presentation mode";
        case QUNS_ACCEPTS_NOTIFICATIONS: return L"accepts notifications";
        case QUNS_QUIET_TIME: return L"quiet time after Windows setup";
        case QUNS_APP: return L"a Windows Store app is full screen";
    }
    return L"unknown";
}

// Everything about this PC and CapView's state that helps with a bug report, one fact per line.
static std::wstring SystemReport() {
    std::wstring r;
    auto line = [&](const std::wstring& s) { r += s + L"\r\n"; };
    line(Fmt(L"CapView %s built %s, %s", kVersion, kBuild, ExePath().c_str()));
    line(Fmt(L"Command line: %s", GetCommandLineW()));
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    DWORD ubr = 0;
    RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR", ubr);
    std::wstring build = RegStr(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild");
    // Windows 11 still says "Windows 10" in ProductName.
    std::wstring product = RegStr(HKEY_LOCAL_MACHINE, cv, L"ProductName");
    if (_wtoi(build.c_str()) >= 22000 && product.rfind(L"Windows 10", 0) == 0) product.replace(8, 2, L"11");
    line(Fmt(L"%s, version %s, build %s.%u", product.c_str(), RegStr(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion").c_str(),
             build.c_str(), ubr));
    std::wstring cpu = RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    while (!cpu.empty() && cpu.back() == L' ') cpu.pop_back();
    line(Fmt(L"CPU: %s, %u logical cores", cpu.c_str(), GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    line(Fmt(L"Memory: %.1f GB, %.1f GB free", ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0));
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS) {
        wchar_t name[256] = {};
        DWORD size = sizeof(name);
        PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, (UCHAR*)name, &size);
        line(Fmt(L"Power plan: %s", name));
        LocalFree(scheme);
    }

    line(L"");
    line(L"GPUs and displays:");
    HMONITOR capMon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    ComPtr<IDXGIFactory1> f;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        ComPtr<IDXGIAdapter1> ad;
        // A GPU can show up again with its own LUID and no displays, so such repeats are skipped.
        std::vector<std::wstring> listed;
        for (UINT i = 0; f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++, ad.Reset()) {
            DXGI_ADAPTER_DESC1 d = {};
            ad->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            ComPtr<IDXGIOutput> first;
            bool hasOutputs = ad->EnumOutputs(0, &first) != DXGI_ERROR_NOT_FOUND;
            std::wstring key = Fmt(L"%s %04X %04X", d.Description, d.VendorId, d.DeviceId);
            if (!hasOutputs && std::find(listed.begin(), listed.end(), key) != listed.end()) continue;
            listed.push_back(key);
            LARGE_INTEGER drv = {};
            std::wstring dv = L"unknown";
            if (SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &drv)))
                dv = Fmt(L"%u.%u.%u.%u", HIWORD(drv.HighPart), LOWORD(drv.HighPart), HIWORD(drv.LowPart), LOWORD(drv.LowPart));
            line(Fmt(L"  %s, %.1f GB, vendor %04X device %04X, driver %s%s", d.Description, d.DedicatedVideoMemory / 1073741824.0,
                     d.VendorId, d.DeviceId, dv.c_str(), g_renderer.AdapterName() == d.Description ? L", CapView renders here" : L""));
            ComPtr<IDXGIOutput> o;
            for (UINT k = 0; ad->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++, o.Reset()) {
                ComPtr<IDXGIOutput6> o6;
                DXGI_OUTPUT_DESC1 od = {};
                if (FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&od))) continue;
                DEVMODEW dm = {};
                dm.dmSize = sizeof(dm);
                EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
                MONITORINFOEXW mi = {};
                mi.cbSize = sizeof(mi);
                GetMonitorInfoW(od.Monitor, &mi);
                UINT dpi = 96, dpiY = 96;
                GetDpiForMonitor(od.Monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
                bool hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                line(Fmt(L"    %s \"%s\": %ux%u at %u Hz, position %d,%d, scale %u%%, %u bits, Windows HDR %s, peak %.0f nits%s%s",
                         od.DeviceName, MonitorName(od.DeviceName).c_str(), dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                         od.DesktopCoordinates.left, od.DesktopCoordinates.top, dpi * 100 / 96, od.BitsPerColor, hdr ? L"on" : L"off",
                         od.MaxLuminance, (mi.dwFlags & MONITORINFOF_PRIMARY) ? L", primary" : L"",
                         od.Monitor == capMon ? L", CapView is here" : L""));
            }
        }
    }

    line(L"");
    line(L"Windows settings:");
    DWORD v = 0;
    line(Fmt(L"  Hardware-accelerated GPU scheduling: %s",
             RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", v)
                 ? (v == 2 ? L"on" : L"off")
                 : L"not set"));
    line(Fmt(L"  Game Mode: %s", RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", v)
                                     ? (v ? L"on" : L"off")
                                     : L"on (default)"));
    const wchar_t* gpuPref = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
    line(Fmt(L"  DirectX graphics settings: %s", RegStr(HKEY_CURRENT_USER, gpuPref, L"DirectXUserGlobalSettings").c_str()));
    std::wstring exe = ExePath();
    std::wstring mine = RegStr(HKEY_CURRENT_USER, gpuPref, exe.c_str());
    if (!mine.empty()) line(Fmt(L"  Graphics settings for CapView.exe: %s", mine.c_str()));
    const wchar_t* layers = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    std::wstring compat = RegStr(HKEY_CURRENT_USER, layers, exe.c_str()) + L" " + RegStr(HKEY_LOCAL_MACHINE, layers, exe.c_str());
    if (compat != L" ") line(Fmt(L"  Compatibility flags on CapView.exe: %s", compat.c_str()));
    int q = NotificationState();
    line(Fmt(L"  Notification state now: %d, %s", q, NotificationStateText(q)));

    line(L"");
    line(L"Other programs running that can affect capture, overlays or frame pacing:");
    static const wchar_t* known[] = {L"obs", L"streamlabs", L"xsplit", L"losslessscaling", L"rtss", L"msiafterburner",
                                     L"discord", L"nvidia overlay", L"nvidia app", L"nvidia broadcast", L"gamebar",
                                     L"steam.exe", L"steamwebhelper", L"wallpaper", L"displayfusion", L"flux",
                                     L"camera hub", L"camerahub", L"elgato", L"4kcaptureutility", L"streamdeck",
                                     L"medal", L"overwolf", L"nzxt", L"icue", L"razer", L"armourycrate", L"amdrsserv",
                                     L"radeonsoftware", L"vtube", L"nvidia share", L"shadowplay", L"specialk", L"reshade",
                                     L"rtx-encore", L"dxgi"};
    std::vector<std::wstring> seen;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            std::wstring n = Lower(pe.szExeFile);
            for (auto k : known)
                if (n.rfind(k, 0) == 0 && std::find(seen.begin(), seen.end(), n) == seen.end()) seen.push_back(n);
        }
        CloseHandle(snap);
    }
    line(seen.empty() ? L"  none of the known ones" : L"  " + [&] {
        std::wstring s;
        for (auto& n : seen) s += (s.empty() ? L"" : L", ") + n;
        return s;
    }());

    line(L"");
    line(L"Capture devices:");
    for (auto& d : EnumVideo()) line(Fmt(L"  video: %s (score %d)", d.name.c_str(), d.score));
    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        for (auto& a : EnumCapture(en.Get())) line(Fmt(L"  audio in: %s", a.name.c_str()));
        for (auto& a : EnumEndpoints(en.Get(), eRender)) line(Fmt(L"  audio out: %s", a.name.c_str()));
    }
    {
        std::lock_guard<std::mutex> l(g_statusMx);
        line(Fmt(L"  current video: %s, %s", g_curVideo.name.c_str(), g_videoStatus.c_str()));
        line(Fmt(L"  current audio: %s", g_audioStatus.c_str()));
        std::wstring m;
        for (auto& fm : g_modes) m += (m.empty() ? L"" : L"; ") + ModeText(fm);
        line(L"  modes offered: " + (m.empty() ? std::wstring(L"none yet") : m));
    }

    line(L"");
    line(L"CapView state:");
    for (auto& kv : IniValues(GetSettings())) line(Fmt(L"  %s=%s", kv.first.c_str(), kv.second.c_str()));
    static const wchar_t* srcNames[] = {L"SDR", L"HDR10", L"HLG"};
    line(Fmt(L"  fullscreen %s, exclusive %s, video %s, source %s, output %s, Windows HDR %s on CapView's display",
             g_fullscreen ? L"yes" : L"no", g_renderer.Exclusive() ? L"active" : L"no", g_videoActive ? L"running" : L"stopped",
             srcNames[std::min(2, std::max(0, g_srcHdr.load()))], g_hdrOut ? L"HDR10" : L"SDR", g_displayHdr ? L"on" : L"off"));
    line(Fmt(L"  content %.1f fps, signal %.1f fps, drawn %.1f fps, shown %.1f fps (%s), display %.0f Hz",
             g_contentFps, g_inFps, g_drawFps, g_outFps, g_statsOk ? L"from frame statistics" : L"no frame statistics",
             g_hwnd ? DisplayHz(g_hwnd) : 0.0));
    line(Fmt(L"  totals: %llu captured, %llu presented, %llu shown, audio latency %.0f ms",
             (unsigned long long)g_framesIn.load(), (unsigned long long)g_presents.load(),
             (unsigned long long)g_displayed.load(), g_audioLatencyMs.load()));
    line(Fmt(L"  frame generation %s, %s; RTX Encore %s%s", kFgKeys[g_fgMode.load()],
             g_fgActive ? L"producing frames" : L"idle", g_encore.empty() ? L"not loaded" : g_encore.c_str(),
             g_encore.empty() ? L"" : (L", Smooth Motion " + (g_encoreSmooth.empty() ? std::wstring(L"unknown") : g_encoreSmooth)).c_str()));
    line(Fmt(L"  AMD Fluid Motion Frames %s, ADLX status %d, CapView turned it on %s", g_afmfOn ? L"on" : L"off",
             g_afmfStatus.load(), IniGet(L"afmfOwned", L"0") == L"1" ? L"yes" : L"no"));
    line(Fmt(L"  XeSS frame generation %s%s%s", g_xessOn ? L"on" : L"off", XessErr().empty() ? L"" : L", ",
             XessErr().c_str()));
    return r;
}

static void LogReport() {
    std::wstring r = SystemReport();
    for (size_t at = 0; at < r.size();) {
        size_t e = r.find(L"\r\n", at);
        if (e == std::wstring::npos) e = r.size();
        if (e > at) Log(L"[report] %s", r.substr(at, e - at).c_str());
        at = e + 2;
    }
}

static void SetDebug(bool on) {
    if (on == g_debug) return;
    g_debug = on;
    if (!g_timingSecsEnv) g_timingSecs = on ? 5 : 30;
    Log(L"Debug mode %s", on ? L"on" : L"off");
    if (on) LogReport();
}

// Called every 500 ms by UpdateTitle. Logs focus and notification changes at once and a stats line
// every second.
static void DebugTick(HWND h) {
    if (!g_debug) return;
    static HWND lastFg = (HWND)-1;
    static int lastQ = -1;
    static int tick = 0;
    static ULONGLONG lastCpu = 0, lastWall = 0;
    HWND fg = GetForegroundWindow();
    if (fg != lastFg) {
        lastFg = fg;
        if (!fg) Log(L"[debug] Foreground: no window");
        else if (fg == h) Log(L"[debug] Foreground: CapView");
        else {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            wchar_t cls[128] = {};
            GetClassNameW(fg, cls, 128);
            RECT wr = {}, ov = {};
            GetWindowRect(fg, &wr);
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            bool covers = IntersectRect(&ov, &wr, &mi.rcMonitor) != FALSE;
            Log(L"[debug] Foreground: %s (class %s) at %d,%d-%d,%d%s", ProcessName(pid).c_str(), cls, wr.left, wr.top,
                wr.right, wr.bottom,
                covers ? Fmt(L", overlaps CapView's monitor by %dx%d px", ov.right - ov.left, ov.bottom - ov.top).c_str()
                       : L", not on CapView's monitor");
        }
    }
    int q = NotificationState();
    if (q != lastQ) {
        lastQ = q;
        Log(L"[debug] Notification state: %d, %s", q, NotificationStateText(q));
    }
    if (++tick % 2) return;
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULONGLONG cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    ULONGLONG wall = GetTickCount64();
    double cpuPct = lastWall && wall > lastWall
                        ? (cpu - lastCpu) / 10000.0 / (wall - lastWall) * 100.0 / GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
                        : 0;
    lastCpu = cpu;
    lastWall = wall;
    Log(L"[debug] content %.1f signal %.1f drawn %.1f shown %.1f%s fps, worst gap %.1f ms, audio %.0f ms, CPU %.1f%%%s%s",
        g_contentFps, g_inFps, g_drawFps, g_outFps, g_statsOk ? L"" : L" (no stats)", g_dbgWorstMs, g_audioLatencyMs.load(),
        cpuPct, g_renderer.Exclusive() ? L", exclusive" : L"", g_fgActive ? L", frame gen on" : L"");
    g_dbgWorstMs = 0;
}

// Window messages worth having in a debug log.
static void DebugMsg(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_debug) return;
    switch (msg) {
        case WM_ACTIVATEAPP: Log(L"[debug] WM_ACTIVATEAPP %s", wp ? L"activated" : L"deactivated"); break;
        case WM_DISPLAYCHANGE: Log(L"[debug] WM_DISPLAYCHANGE %ux%u, %u bpp", LOWORD(lp), HIWORD(lp), (unsigned)wp); break;
        case WM_DPICHANGED: Log(L"[debug] WM_DPICHANGED %u dpi", LOWORD(wp)); break;
        case WM_POWERBROADCAST: Log(L"[debug] WM_POWERBROADCAST %u", (unsigned)wp); break;
        case WM_DEVICECHANGE: Log(L"[debug] WM_DEVICECHANGE 0x%04X", (unsigned)wp); break;
        case WM_SIZE: Log(L"[debug] WM_SIZE %ux%u type %u", LOWORD(lp), HIWORD(lp), (unsigned)wp); break;
        case WM_DWMCOMPOSITIONCHANGED: Log(L"[debug] WM_DWMCOMPOSITIONCHANGED"); break;
        case WM_QUERYENDSESSION: Log(L"[debug] WM_QUERYENDSESSION"); break;
        case WM_SYSCOMMAND: Log(L"[debug] WM_SYSCOMMAND 0x%04X", (unsigned)(wp & 0xFFF0)); break;
    }
}

// Crash dumps: capview-crash-<time>.dmp next to the exe. Debug mode adds the memory that the stacks
// point at, so the dump shows variable contents, not only the call stacks.
static void WriteCrashDump(EXCEPTION_POINTERS* ep, const wchar_t* why) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring path = ExeDir() + Fmt(L"capview-crash-%04d%02d%02d-%02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay,
                                       st.wHour, st.wMinute, st.wSecond);
    std::wstring where;
    if (ep && ep->ExceptionRecord) {
        HMODULE mod = nullptr;
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)addr, &mod))
            GetModuleFileNameW(mod, name, MAX_PATH);
        where = Fmt(L"exception 0x%08X at %s+0x%llX", ep->ExceptionRecord->ExceptionCode, name,
                    (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    }
    // The crashing thread may hold the log lock, so this doesn't wait for it.
    bool locked = g_logMx.try_lock();
    if (g_log) {
        fwprintf(g_log, L"%02d:%02d:%02d.%03d CRASH (%s): %s, thread %u, writing %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, why, where.c_str(), GetCurrentThreadId(), path.c_str());
        fflush(g_log);
    }
    if (locked) g_logMx.unlock();
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData |
                                         (g_debug ? (MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData |
                                                     MiniDumpWithFullMemoryInfo)
                                                  : 0));
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), fh, type, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(fh);
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    WriteCrashDump(ep, L"unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH;  // Windows Error Reporting still runs
}
static void InstallCrashHandlers() {
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] {
        WriteCrashDump(nullptr, L"std::terminate");
        abort();
    });
    _set_purecall_handler([] {
        WriteCrashDump(nullptr, L"pure virtual call");
        abort();
    });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        WriteCrashDump(nullptr, L"invalid parameter to a C runtime function");
        abort();
    });
}

// ---- debug report zip ---------------------------------------------------------------------------
static uint32_t Crc32(const std::string& s) {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = t[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Writes an uncompressed zip. Names are UTF-8 and may hold forward slashes for folders.
static bool WriteZip(const std::wstring& path, const std::vector<std::pair<std::string, std::string>>& files) {
    auto p16 = [](std::string& o, uint16_t v) { o += (char)(v & 0xFF); o += (char)(v >> 8); };
    auto p32 = [&](std::string& o, uint32_t v) { p16(o, (uint16_t)(v & 0xFFFF)); p16(o, (uint16_t)(v >> 16)); };
    SYSTEMTIME st;
    GetLocalTime(&st);
    uint16_t dosTime = (uint16_t)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
    uint16_t dosDate = (uint16_t)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    std::string out, central;
    for (auto& fl : files) {
        uint32_t crc = Crc32(fl.second), size = (uint32_t)fl.second.size(), off = (uint32_t)out.size();
        p32(out, 0x04034b50); p16(out, 20); p16(out, 0x0800); p16(out, 0); p16(out, dosTime); p16(out, dosDate);
        p32(out, crc); p32(out, size); p32(out, size); p16(out, (uint16_t)fl.first.size()); p16(out, 0);
        out += fl.first;
        out += fl.second;
        p32(central, 0x02014b50); p16(central, 20); p16(central, 20); p16(central, 0x0800); p16(central, 0);
        p16(central, dosTime); p16(central, dosDate); p32(central, crc); p32(central, size); p32(central, size);
        p16(central, (uint16_t)fl.first.size()); p16(central, 0); p16(central, 0); p16(central, 0); p16(central, 0);
        p32(central, 0); p32(central, off);
        central += fl.first;
    }
    uint32_t cdOff = (uint32_t)out.size();
    out += central;
    p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, (uint16_t)files.size()); p16(out, (uint16_t)files.size());
    p32(out, (uint32_t)central.size()); p32(out, cdOff); p16(out, 0);
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(fh, out.data(), (DWORD)out.size(), &w, nullptr) && w == out.size();
    CloseHandle(fh);
    return ok;
}

static bool ReadWholeFile(const std::wstring& path, std::string& data, ULONGLONG cap) {
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(fh, &sz);
    bool ok = (ULONGLONG)sz.QuadPart <= cap;
    if (ok) {
        data.resize((size_t)sz.QuadPart);
        DWORD r = 0;
        ok = data.empty() || (ReadFile(fh, &data[0], (DWORD)data.size(), &r, nullptr) && r == data.size());
    }
    CloseHandle(fh);
    return ok;
}

// The newest files in dir matching pattern, newest first.
static std::vector<std::wstring> NewestFiles(const std::wstring& dir, const wchar_t* pattern, size_t count) {
    std::vector<std::pair<ULONGLONG, std::wstring>> v;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((dir + pattern).c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!fd.nFileSizeLow && !fd.nFileSizeHigh) continue;
            v.push_back({(ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime, fd.cFileName});
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (size_t i = 0; i < v.size() && i < count; i++) out.push_back(v[i].second);
    return out;
}

static std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}

// Puts the system report, CapView's logs and settings, recent crash dumps and RTX Encore's config and
// logs in one zip, on the desktop unless a folder is given. Returns the zip's path, or an empty string
// after showing an error.
static std::wstring WriteDebugReport(HWND owner, const std::wstring& folder = L"") {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Log(L"Saving a debug report");
    std::wstring dir = ExeDir();
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({"report.txt", "\xEF\xBB\xBF" + Utf8(HidePaths(SystemReport()))});
    auto add = [&](const std::wstring& rel, bool text) {
        std::string data;
        if (!ReadWholeFile(dir + rel, data, 512ull << 20)) return;
        std::wstring name = rel;
        std::replace(name.begin(), name.end(), L'\\', L'/');
        files.push_back({Utf8(name), text ? HidePathsBytes(data) : data});
    };
    add(L"capview.log", true);
    add(L"capview.prev.log", true);
    add(L"capview.ini", true);
    for (auto& d : NewestFiles(dir, L"capview-crash-*.dmp", 3)) add(d, false);
    if (!g_encore.empty()) {
        add(L"rtx-encore.jsonc", true);
        std::wstring ld = L"rtx-encore-logs\\";
        add(ld + L"rtx-encore-status.json", true);
        add(ld + L"rtx-encore.log", true);
        for (auto& n : NewestFiles(dir + ld, L"rtx-encore.log.previous-*", 2)) add(ld + n, true);
        // Encore writes a crash log at every start, mostly a module list. Only ones that caught an
        // exception are worth sending.
        int kept = 0;
        for (auto& n : NewestFiles(dir + ld, L"*.crash.log", 40)) {
            std::string data;
            if (kept < 3 && ReadWholeFile(dir + ld + n, data, 64ull << 20) &&
                data.find("FIRST_CHANCE_EXCEPTION") != std::string::npos) {
                files.push_back({Utf8(L"rtx-encore-logs/" + n), HidePathsBytes(data)});
                kept++;
            }
        }
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring name = Fmt(L"CapView-debug-%04d%02d%02d-%02d%02d%02d.zip", st.wYear, st.wMonth, st.wDay, st.wHour,
                            st.wMinute, st.wSecond);
    std::wstring dest;
    if (!folder.empty()) {
        dest = folder + name;
    } else {
        PWSTR desk = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) dest = std::wstring(desk) + L"\\" + name;
        CoTaskMemFree(desk);
    }
    bool ok = !dest.empty() && WriteZip(dest, files);
    if (!ok && folder.empty()) {
        dest = dir + name;
        ok = WriteZip(dest, files);
    }
    SetCursor(old);
    if (!ok) {
        Log(L"Debug report failed: couldn't write %s (error %u)", dest.c_str(), GetLastError());
        MessageBoxW(owner, L"CapView couldn't write the debug report.", L"CapView", MB_OK | MB_ICONERROR);
        return L"";
    }
    Log(L"Debug report saved: %s, %zu files", dest.c_str(), files.size());
    return dest;
}

static void ShowInExplorer(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void SaveDebugReport(HWND owner) {
    std::wstring dest = WriteDebugReport(owner);
    if (dest.empty()) return;
    std::wstring msg = L"CapView saved a debug report to\n\n" + dest +
                       L"\n\nIt holds a description of this PC, CapView's logs and settings, any crash dumps, and "
                       L"RTX Encore's settings and logs. Your Windows user folder shows as %USERPROFILE%.\n\nAttach "
                       L"this file to your bug report. For the most detail, turn on debug mode, make the problem "
                       L"happen, then save the report.";
    MessageBoxW(owner, msg.c_str(), L"CapView", MB_OK | MB_ICONINFORMATION);
    ShowInExplorer(dest);
}

// ---- report a bug -------------------------------------------------------------------------------
// "Report a bug" sends the reporter's description and a debug report zip to a small relay service at
// capview.walker-computers.com, which files them as an issue in a private GitHub repository. The
// reporter needs no account, and the GitHub token stays on the server.
static const wchar_t* kReportHost = L"capview.walker-computers.com";
static const size_t kReportMaxZip = 19u << 20;  // the relay refuses zips over 20 MB
enum { IDC_BUGTEXT = 10, IDC_BUGCONTACT, IDC_BUGSTATUS };
static const UINT WM_BUGSENT = WM_APP + 40;

// The lines of the system report that matter most, sent with the report so they show in the issue.
static std::wstring BugSummary() {
    std::wstring sys = HidePaths(SystemReport()), picked;
    static const wchar_t* keep[] = {L"Windows ", L"CPU:", L"CapView renders here", L"CapView is here",
                                    L"current video:", L"current audio:", L"frame generation ",
                                    L"Hardware-accelerated", L"Other programs", L"content "};
    bool nextToo = false;
    for (size_t at = 0; at < sys.size();) {
        size_t e = sys.find(L"\r\n", at);
        if (e == std::wstring::npos) e = sys.size();
        std::wstring ln = sys.substr(at, e - at);
        at = e + 2;
        bool want = nextToo;
        nextToo = false;
        for (auto k : keep)
            if (ln.find(k) != std::wstring::npos) want = true;
        if (ln.find(L"Other programs") != std::wstring::npos) nextToo = true;  // the list is on the next line
        while (!ln.empty() && ln[0] == L' ') ln.erase(0, 1);
        if (want) picked += ln + L"\n";
    }
    if (g_debug) picked += L"Debug mode was on\n";
    return picked;
}

struct BugSendResult {
    DWORD status = 0;  // HTTP status, or 0 when the request never got an answer
    DWORD error = 0;   // WinHTTP error when status is 0
    std::string body;
};

static BugSendResult PostBugReport(const std::string& body, const std::string& boundary) {
    BugSendResult r;
    std::wstring agent = L"CapView/" + std::wstring(kBuild);
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, kReportHost, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET q = c ? WinHttpOpenRequest(c, L"POST", L"/report", nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (s) WinHttpSetTimeouts(s, 15000, 15000, 180000, 60000);
    std::wstring hdr = L"Content-Type: multipart/form-data; boundary=";
    hdr.append(boundary.begin(), boundary.end());
    DWORD len = (DWORD)body.size();
    if (q && WinHttpSendRequest(q, hdr.c_str(), (DWORD)-1L, (LPVOID)body.data(), len, len, 0) &&
        WinHttpReceiveResponse(q, nullptr)) {
        DWORD size = sizeof(r.status);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &r.status, &size, WINHTTP_NO_HEADER_INDEX);
        char buf[4096];
        DWORD got = 0;
        while (r.body.size() < 65536 && WinHttpReadData(q, buf, sizeof(buf), &got) && got) r.body.append(buf, got);
    } else {
        r.error = GetLastError();
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return r;
}

// Reads one string field from the relay's small JSON answers, which never hold escaped quotes.
static std::wstring JsonField(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    size_t a = json.find(k);
    if (a == std::string::npos) return L"";
    a += k.size();
    size_t e = json.find('"', a);
    if (e == std::string::npos) return L"";
    std::string v = json.substr(a, e - a);
    int n = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), nullptr, 0);
    std::wstring o(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), &o[0], n);
    return o;
}

struct BugDialog {
    HWND owner = nullptr;
    bool sending = false;
    std::wstring zip;
};

static std::wstring WindowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

static void BugBusy(HWND h, bool busy, const wchar_t* status) {
    auto* d = (BugDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    d->sending = busy;
    for (int id : {IDOK, IDCANCEL, (int)IDC_BUGTEXT, (int)IDC_BUGCONTACT}) EnableWindow(GetDlgItem(h, id), !busy);
    SetDlgItemTextW(h, IDC_BUGSTATUS, status);
    UpdateWindow(h);
}

static void SendBug(HWND h) {
    auto* d = (BugDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    std::wstring text = WindowText(GetDlgItem(h, IDC_BUGTEXT));
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        SetDlgItemTextW(h, IDC_BUGSTATUS, L"Describe the problem first.");
        SetFocus(GetDlgItem(h, IDC_BUGTEXT));
        return;
    }
    std::wstring contact = WindowText(GetDlgItem(h, IDC_BUGCONTACT));
    BugBusy(h, true, L"Saving the debug report...");
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH, tmp);
    d->zip = WriteDebugReport(h, tmp);
    std::string data;
    if (d->zip.empty() || !ReadWholeFile(d->zip, data, 512ull << 20)) {
        BugBusy(h, false, L"CapView couldn't save the debug report.");
        return;
    }
    if (data.size() > kReportMaxZip) {
        DeleteFileW(d->zip.c_str());
        Log(L"Bug report not sent: the debug report is %zu bytes", data.size());
        BugBusy(h, false, L"The debug report is too large to send. Use \"Save debug report\" instead.");
        return;
    }
    char rnd[17];
    snprintf(rnd, sizeof(rnd), "%08x%08x", GetTickCount(), GetCurrentProcessId() * 2654435761u);
    std::string b = std::string("CapViewReport") + rnd, body;
    auto part = [&](const char* name, const std::string& value) {
        body += "--" + b + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
    };
    part("description", Utf8(text));
    part("contact", Utf8(contact));
    part("version", Utf8(kBuild));
    part("summary", Utf8(BugSummary()));
    body += "--" + b + "\r\nContent-Disposition: form-data; name=\"report\"; filename=\"report.zip\"\r\n"
            "Content-Type: application/zip\r\n\r\n";
    body += data;
    body += "\r\n--" + b + "--\r\n";
    Log(L"Sending a bug report to %s, %zu bytes", kReportHost, body.size());
    BugBusy(h, true, L"Sending...");
    std::thread([h, body = std::move(body), b]() {
        auto* r = new BugSendResult(PostBugReport(body, b));
        if (!PostMessageW(h, WM_BUGSENT, 0, (LPARAM)r)) delete r;
    }).detach();
}

static void BugSent(HWND h, BugSendResult* r) {
    auto* d = (BugDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!d->zip.empty()) DeleteFileW(d->zip.c_str());
    d->zip.clear();
    std::wstring id = JsonField(r->body, "id");
    if (r->status == 201 && !id.empty()) {
        Log(L"Bug report sent, ID %s", id.c_str());
        d->sending = false;
        std::wstring msg = L"Thanks. CapView sent your report.\n\nIts ID is " + id +
                           L". Quote it if you write to the developer about this problem.";
        MessageBoxW(h, msg.c_str(), L"CapView", MB_OK | MB_ICONINFORMATION);
        PostMessageW(h, WM_CLOSE, 0, 0);
        return;
    }
    std::wstring why;
    if (r->status == 0) {
        Log(L"Bug report failed: no answer from %s (error %u)", kReportHost, r->error);
        why = Fmt(L"CapView couldn't reach the report server (error %u). Check the internet connection and try "
                  L"again, or use \"Save debug report\".", r->error);
    } else {
        std::wstring err = JsonField(r->body, "error");
        Log(L"Bug report failed: the server answered %u %s", r->status, err.c_str());
        why = err.empty() ? Fmt(L"The report server answered %u. Try again later.", r->status) : err;
    }
    BugBusy(h, false, why.c_str());
}

static LRESULT CALLBACK BugProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (BugDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && d && !d->sending) SendBug(h);
            if (LOWORD(wp) == IDCANCEL) PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        case WM_BUGSENT: {
            auto* r = (BugSendResult*)lp;
            BugSent(h, r);
            delete r;
            return 0;
        }
        case WM_CLOSE:
            if (d && d->sending) return 0;  // the upload thread posts back to this window
            if (d && d->owner) EnableWindow(d->owner, TRUE);
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ReportBug(HWND owner) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = BugProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"CapViewBug";
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = GetDpiForWindow(owner);
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    const int M = 12, W = 440, introH = g_debug ? 70 : 122, textH = 130;
    int clientH = M + introH + 8 + 20 + textH + 10 + 20 + 24 + 12 + 40 + M;
    RECT rc = {0, 0, S(M + W + M), S(clientH)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, dpi);
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    int x = (orc.left + orc.right - w) / 2, top = std::max<int>(orc.top, (orc.top + orc.bottom - hgt) / 2);
    BugDialog d;
    d.owner = owner;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CapViewBug", L"Report a bug", style, x, top,
                             w, hgt, owner, nullptr, inst, nullptr);
    if (!h) {
        Log(L"Bug report window failed to open (error %u)", GetLastError());
        return;
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&d);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    auto ctl = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, S(cx), S(cy), S(cw), S(ch), h,
                                 (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    std::wstring intro =
        L"Your description goes to the developer with a debug report: a description of this PC, CapView's logs "
        L"and settings, any crash dumps, and RTX Encore's settings and logs. Your Windows user folder shows as "
        L"%USERPROFILE%. Only the developer can see reports.";
    if (!g_debug)
        intro += L"\n\nIf the problem comes and goes, cancel, tick \"Debug mode\" in settings, make the problem "
                 L"happen, then report it.";
    int y = M;
    ctl(L"STATIC", intro.c_str(), SS_LEFT, M, y, W, introH, -1);
    y += introH + 8;
    ctl(L"STATIC", L"What happened, and what were you doing when it happened?", SS_LEFT, M, y, W, 20, -1);
    y += 20;
    HWND text = ctl(L"EDIT", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                    M, y, W, textH, IDC_BUGTEXT);
    SendMessageW(text, EM_SETLIMITTEXT, 8000, 0);
    y += textH + 10;
    ctl(L"STATIC", L"Email or Discord name, if you'd like a reply (optional)", SS_LEFT, M, y, W, 20, -1);
    y += 20;
    HWND contact = ctl(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, M, y, W, 24, IDC_BUGCONTACT);
    SendMessageW(contact, EM_SETLIMITTEXT, 200, 0);
    y += 24 + 12;
    ctl(L"STATIC", L"", SS_LEFT, M, y, W - 190, 40, IDC_BUGSTATUS);
    ctl(L"BUTTON", L"Send", BS_DEFPUSHBUTTON | WS_TABSTOP, M + W - 180, y, 85, 26, IDOK);
    ctl(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, M + W - 85, y, 85, 26, IDCANCEL);

    Log(L"Opened the bug report window");
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(text);
    MSG m;
    while (IsWindow(h)) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)m.wParam);
            break;
        }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (IsWindow(h)) {
        d.sending = false;
        EnableWindow(owner, TRUE);
        DestroyWindow(h);
    }
    if (!d.zip.empty()) DeleteFileW(d.zip.c_str());
    DeleteObject(font);
    SetForegroundWindow(owner);
}

static void UpdateTitle(HWND h) {
    LARGE_INTEGER q, fq;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&fq);
    ULONGLONG now = (ULONGLONG)q.QuadPart;
    uint32_t frames = g_framesPresented.load();
    double dt = g_lastFpsTick && now > g_lastFpsTick ? (double)(now - g_lastFpsTick) / fq.QuadPart : 0;
    if (dt > 0) g_shownFps = (frames - g_lastFpsFrames) / dt;
    g_lastFpsTick = now;
    g_lastFpsFrames = frames;
    static int hdrTick = 0;
    // RTX Encore keeps Smooth Motion on from its own menu whatever mode CapView is in, so its
    // status is read in every mode.
    if (hdrTick % 4 == 0 && !g_encore.empty()) {
        g_encoreSmooth = EncoreSmoothState();
        if (!g_encoreSmooth.empty()) g_encoreSmoothOn = g_encoreSmooth == L"active";
    }
    if (hdrTick % 4 == 0 && g_fgMode == FG_SMOOTH) {
        std::wstring gpu = g_renderer.AdapterName();
        g_smoothNative = Contains(gpu, L"RTX 40") || Contains(gpu, L"RTX 50") || Contains(gpu, L"RTX PRO");
    }
    if (hdrTick++ % 4 == 0) CheckDisplayHdr(h);
    UpdateFps(h, dt);
    DebugTick(h);
    if (g_statsOk && dt > 0) g_shownFps = g_outFps;
    std::wstring v, a;
    {
        std::lock_guard<std::mutex> l(g_statusMx);
        v = g_videoStatus;
        a = g_audioStatus;
    }
    std::wstring t = L"CapView  |  " + v;
    if (g_videoActive && g_inFps > 1)
        t += g_contentFps < 0.5 ? Fmt(L"  (still picture, %.0f fps out)", g_shownFps)
                                : Fmt(L"  (%.0f fps, %.0f out)", g_contentFps, g_shownFps);
    else if (g_videoActive)
        t += Fmt(L"  (%.0f fps out)", g_shownFps);
    t += L"  |  " + a;
    float lat = g_audioLatencyMs;
    if (lat > 0) t += Fmt(L"  ~%.0f ms", lat);
    if (g_mute) t += L"  |  muted";
    else if (std::fabs(g_volume - 1.0f) > 0.01f) t += Fmt(L"  |  vol %.0f%%", g_volume * 100);
    if (g_vsync) t += L"  |  vsync";
    if (g_hdrOut) t += L"  |  HDR10";
    switch (g_fgMode.load()) {
        case FG_CAPVIEW:
            t += FgPausedForSmooth() ? std::wstring(L"  |  frame gen paused for Smooth Motion")
                 : g_fgActive        ? Fmt(L"  |  frame gen %dx", g_fgMult.load())
                                     : std::wstring(L"  |  frame gen idle");
            break;
        case FG_SMOOTH:
            t += g_encore.empty() ? L"  |  Smooth Motion" : L"  |  Smooth Motion (RTX Encore)";
            if (!SmoothRunning()) {
                t += g_encore.empty() ? L" not available on this GPU" : g_encoreSmooth.empty() ? L"" : L" off";
            } else if (g_videoActive) {
                bool measured;
                double sm = SmoothShownFps(g_shownFps, g_drawFps, g_statsOk, measured);
                t += Fmt(measured ? L" %.0f fps" : L" ~%.0f fps", sm);
            }
            break;
        case FG_AMD:
            t += !g_afmfOn       ? std::wstring(L"  |  AFMF ") + AfmfShort(g_afmfStatus)
                 : g_fullscreen ? std::wstring(L"  |  AFMF")
                                : std::wstring(L"  |  AFMF, fullscreen only");
            break;
        case FG_XESS:
            t += g_xessOn ? L"  |  XeSS frame generation" : L"  |  XeSS frame generation off";
            break;
    }
    if (g_fgMode == FG_OFF && g_encoreSmoothOn) t += L"  |  RTX Encore Smooth Motion on";
    if (g_debug) t += L"  |  debug";
    t += L"  |  S settings, F1 help";
    SetWindowTextW(h, t.c_str());
}

static const wchar_t* kHelp =
    L"CapView keys\n\n"
    L"S, F2, right-click                Settings\n"
    L"F11, Alt+Enter, double-click   Fullscreen\n"
    L"Esc                             Leave fullscreen\n"
    L"V                               Toggle vsync (off = lowest latency, may tear)\n"
    L"M                               Mute\n"
    L"Up / Down                       Volume\n"
    L"D                               Next video device (then back to auto)\n"
    L"A                               Next audio input (then none, then auto)\n"
    L"G                               CapView frame generation on or off\n"
    L"F                               Show fps (frames that reached the screen)\n"
    L"R                               Rescan and reopen everything\n"
    L"F1                              This help\n\n"
    L"Command line: --video <name> --audio <name|none> --output <name> --mode 1920x1080@60\n"
    L"              --framegen off|capview|smoothmotion|afmf|xess --multiplier 2|3|4 --hdr auto|off\n"
    L"              --fullscreen --exclusive --vsync --volume <0-200> --debug --list\n\n"
    L"Settings are saved in capview.ini and the log in capview.log, both next to the exe.";

// ------------------------------------------------------------------------------------------------
// Frame generation mode changes and restarting
// ------------------------------------------------------------------------------------------------
static DWORD g_afterPid = 0;

// extra is appended to the new command line, for run-only options that must survive the restart.
static void RestartSelf(const std::wstring& extra = L"") {
    SaveIni();
    std::wstring exe = ExePath();
    std::wstring cl = Fmt(L"\"%s\" --after %u", exe.c_str(), GetCurrentProcessId()) + extra;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe.c_str(), &cl[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

static bool IsRtx30(const std::wstring& gpu) {
    return Contains(gpu, L"RTX 30") || Contains(gpu, L"RTX A");
}

static void SetFgMode(int m, HWND owner) {
    int old = GetSettings().fgMode;
    if (m == old) return;
    {
        std::lock_guard<std::mutex> l(g_setMx);
        g_set.fgMode = m;
    }
    g_fgMode = m;
    SaveIni();
    Log(L"Frame generation: %s", kFgKeys[m]);
    if (m == FG_XESS || old == FG_XESS) {
        bool on = m == FG_XESS;
        if (on) g_renderer.SetExclusive(false);
        if (!XessFollow(on) && on) {
            {
                std::lock_guard<std::mutex> l(g_setMx);
                g_set.fgMode = old;
            }
            g_fgMode = old;
            SaveIni();
            MessageBoxW(owner, (L"XeSS frame generation didn't start. " + XessErr()).c_str(), L"CapView",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (!g_videoActive) g_renderer.PresentBlack();
    }
    if (m == FG_AMD || old == FG_AMD) {
        bool on = m == FG_AMD;
        int st = AfmfFollow(on);
        if (on && st != afmf::kOk) {
            {
                std::lock_guard<std::mutex> l(g_setMx);
                g_set.fgMode = old;
            }
            g_fgMode = old;
            SaveIni();
            MessageBoxW(owner, AfmfError(st).c_str(), L"CapView", MB_OK | MB_ICONWARNING);
            return;
        }
        if (on) {
            std::wstring gpu = g_renderer.AdapterName();
            std::wstring msg =
                L"AMD Fluid Motion Frames is on in the Radeon driver while CapView runs in this mode. CapView turns "
                L"it off again when you pick another mode or close CapView, unless it was already on.\n\nThe driver "
                L"only adds frames while CapView is fullscreen, so press F11.";
            if (!IsAmdGpu(gpu))
                msg += L"\n\nCapView is drawing on " + gpu + L". AFMF only runs on a Radeon GPU, so plug the monitor "
                       L"into the Radeon card and restart CapView.";
            MessageBoxW(owner, msg.c_str(), L"CapView", MB_OK | MB_ICONINFORMATION);
        }
    }
    if (!g_encore.empty()) {
        // RTX Encore runs Smooth Motion from its own config, and the driver rejects the profile setting
        // on the cards it is for, so the profile is left alone. CapView switches Smooth Motion on in
        // RTX Encore for Smooth Motion mode and off for every other mode, so only one frame generator runs.
        bool on = m == FG_SMOOTH;
        int r = EncoreSetSmooth(on);
        Log(L"RTX Encore Smooth Motion %s in rtx-encore.jsonc: %s", on ? L"on" : L"off",
            r > 0 ? L"changed" : r == 0 ? L"already set" : L"write failed");
        bool restart = false;
        if (r < 0) {
            MessageBoxW(owner,
                        (std::wstring(L"CapView couldn't change Smooth Motion in rtx-encore.jsonc. Press Insert for RTX "
                                      L"Encore's menu and turn Smooth Motion ") +
                         (on ? L"on on the Frame rate tab, with Graphics API Direct3D 11" : L"off on the Frame rate tab") +
                         L", then restart CapView.")
                            .c_str(),
                        L"CapView", MB_OK | MB_ICONWARNING);
        } else if (r > 0 || on != (bool)g_encoreSmoothOn) {
            std::wstring msg = std::wstring(L"Smooth Motion is now ") + (on ? L"on" : L"off") +
                               L" in RTX Encore's settings. RTX Encore applies it when CapView starts.\n\nRestart CapView now?";
            restart = MessageBoxW(owner, msg.c_str(), L"CapView", MB_YESNO | MB_ICONQUESTION) == IDYES;
        }
        if (restart) {
            RestartSelf();
            return;
        }
    } else if (m == FG_SMOOTH || old == FG_SMOOTH) {
        bool on = m == FG_SMOOTH;
        int st = ApplySmoothMotionElevating(on, owner);
        if (st != nvdrs::kOk) {
            Log(L"Smooth Motion profile change failed (%d)", st);
            if (on) {
                {
                    std::lock_guard<std::mutex> l(g_setMx);
                    g_set.fgMode = old;
                }
                g_fgMode = old;
                SaveIni();
            }
            MessageBoxW(owner, SmoothMotionError(st).c_str(), L"CapView", MB_OK | MB_ICONWARNING);
        } else {
            Log(L"Smooth Motion %s in the NVIDIA profile", on ? L"on" : L"off");
            std::wstring msg = on ? L"Smooth Motion is now on in the NVIDIA profile for CapView." : L"Smooth Motion is now off in the NVIDIA profile for CapView.";
            if (on && IsRtx30(g_renderer.AdapterName()))
                msg += L"\n\nThis is an RTX 30 card. The driver only runs Smooth Motion on RTX 30 when the community RTX 30 "
                       L"Smooth Motion patch is installed. Without it the driver ignores this setting.";
            msg += L"\n\nThe driver applies this when CapView starts. Restart CapView now?";
            if (MessageBoxW(owner, msg.c_str(), L"CapView", MB_YESNO | MB_ICONQUESTION) == IDYES) RestartSelf();
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Settings window
// ------------------------------------------------------------------------------------------------
enum {
    IDC_VIDEO = 100, IDC_MODE, IDC_AUDIOIN, IDC_AUDIOOUT, IDC_VOLUME, IDC_VOLTEXT, IDC_MUTE, IDC_VSYNC, IDC_FULL,
    IDC_FG, IDC_MULT, IDC_INFO, IDC_CLOSE, IDC_SHOWFPS, IDC_HDR, IDC_PREDICT, IDC_EXCL,
    IDC_DEBUG, IDC_REPORT, IDC_BUG,
};
static HWND g_settings = nullptr;
static HFONT g_font = nullptr;
struct SettingsLists {
    std::vector<VideoDev> vids;
    std::vector<FrameFormat> modes;
    std::vector<AudioEp> ins, outs;
};
static SettingsLists g_sl;

static int ComboSel(HWND h, int id) { return (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0); }
static void ComboAdd(HWND h, int id, const std::wstring& s) { SendDlgItemMessageW(h, id, CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
static void ComboSet(HWND h, int id, int i) { SendDlgItemMessageW(h, id, CB_SETCURSEL, i, 0); }
static void Check(HWND h, int id, bool on) { CheckDlgButton(h, id, on ? BST_CHECKED : BST_UNCHECKED); }
static bool Checked(HWND h, int id) { return IsDlgButtonChecked(h, id) == BST_CHECKED; }

static void UpdateInfo(HWND h) {
    Settings s = GetSettings();
    std::wstring t;
    float fps = g_srcFps > 1 ? g_srcFps.load() : 60.0f;
    switch (s.fgMode) {
        case FG_OFF:
            t = L"Frame generation is off. CapView shows each captured frame the moment it arrives, which is the lowest delay.";
            break;
        case FG_CAPVIEW: {
            int m = s.fgMult;
            t = Fmt(L"CapView works out the motion between two captured frames on the GPU and draws %d extra "
                    L"frame%s, so %.0f fps capture shows as %.0f fps. It runs on any GPU. The display needs at least "
                    L"%.0f Hz, with vsync off. Fast motion, text and HUDs can smear. ",
                    m - 1, m > 2 ? L"s" : L"", fps, fps * m, fps * m);
            if (s.fgPredict)
                t += L"Predict ahead is on. Each real frame shows the moment it arrives and the extra frames carry the "
                     L"motion past it, so nothing waits. Objects that stop or turn overshoot for a frame.";
            else
                t += Fmt(L"Each real frame waits about %.0f ms so the in-between frames can go first. Predict ahead "
                         L"removes that wait at the cost of overshoot when motion changes.",
                         1000.0 * (m - 1) / m / fps);
            break;
        }
        case FG_SMOOTH: {
            std::wstring gpu = g_renderer.AdapterName();
            t = L"NVIDIA Smooth Motion is the driver's own frame generation. CapView turns it on in the NVIDIA profile for "
                L"CapView.exe, and the driver applies it when CapView starts. GPU: " + gpu + L".";
            if (!g_encore.empty())
                t = L"NVIDIA Smooth Motion through RTX Encore, loaded as " + g_encore + L". CapView turns it on in "
                    L"rtx-encore.jsonc when you pick this mode and off when you pick another, and offers to restart, "
                    L"since RTX Encore applies it at start. GPU: " + gpu + L".";
            else if (!Contains(gpu, L"NVIDIA")) t += L" CapView isn't running on an NVIDIA GPU, so the driver won't apply it.";
            else if (IsRtx30(gpu)) t += L" RTX 30 cards need a community patch such as RTX Encore, copied next to CapView.exe as dxgi.dll.";
            else if (!Contains(gpu, L"RTX 40") && !Contains(gpu, L"RTX 50")) t += L" The driver supports it on RTX 40 and 50 cards.";
            break;
        }
        case FG_AMD: {
            std::wstring gpu = g_renderer.AdapterName();
            t = L"AMD Fluid Motion Frames is the Radeon driver's own frame generation. CapView turns it on through "
                L"AMD's ADLX while this mode is picked and off again afterwards. It only adds frames in fullscreen. "
                L"GPU: " + gpu + L".";
            if (!g_afmfOn && g_afmfStatus != afmf::kOk) t += L" " + AfmfError(g_afmfStatus);
            else if (!IsAmdGpu(gpu)) t += L" CapView isn't drawing on a Radeon GPU, so the driver won't apply it.";
            break;
        }
        case FG_XESS: {
            t = L"Intel XeSS frame generation adds one frame between each pair of new game frames. It runs on Intel "
                L"Arc and on other GPUs with shader model 6.4, through Intel's libxess_fg.dll and libxell.dll next to "
                L"CapView.exe. It works in a window and in borderless fullscreen, not exclusive fullscreen, and the "
                L"picture stays SDR. GPU: " + g_renderer.AdapterName() + L".";
            std::wstring e = XessErr();
            if (!g_xessOn && !e.empty()) t += L" Not running: " + e;
            break;
        }
    }
    if (!g_encore.empty() && s.fgMode != FG_SMOOTH && s.fgMode != FG_OFF)
        t += L" One frame generator runs at a time. While RTX Encore's Smooth Motion is on, CapView frame generation "
             L"pauses.";
    SetDlgItemTextW(h, IDC_INFO, t.c_str());
    EnableWindow(GetDlgItem(h, IDC_MULT), s.fgMode == FG_CAPVIEW);
    EnableWindow(GetDlgItem(h, IDC_PREDICT), s.fgMode == FG_CAPVIEW);
}

static void SyncSettingsUi() {
    if (!g_settings) return;
    HWND h = g_settings;
    int v = (int)std::lround(g_volume * 100);
    SendDlgItemMessageW(h, IDC_VOLUME, TBM_SETPOS, TRUE, v);
    SetDlgItemTextW(h, IDC_VOLTEXT, Fmt(L"%d%%", v).c_str());
    Check(h, IDC_MUTE, g_mute);
    Check(h, IDC_VSYNC, g_vsync);
    Check(h, IDC_SHOWFPS, g_showFps);
    Check(h, IDC_HDR, g_hdrAllow);
    ComboSet(h, IDC_FG, g_fgMode);
    UpdateInfo(h);
}

static void PopulateSettings(HWND h) {
    Settings s = GetSettings();
    g_sl.vids = EnumVideo();
    {
        std::lock_guard<std::mutex> l(g_statusMx);
        g_sl.modes.clear();
        for (auto& f : g_modes) {
            bool dup = false;
            for (auto& m : g_sl.modes)
                if (m.w == f.w && m.h == f.h && std::fabs(Fps(m) - Fps(f)) < 0.01) dup = true;
            if (!dup) g_sl.modes.push_back(f);
        }
    }
    std::sort(g_sl.modes.begin(), g_sl.modes.end(), [](const FrameFormat& a, const FrameFormat& b) {
        if (std::fabs(Fps(a) - Fps(b)) > 0.01) return Fps(a) > Fps(b);
        return (UINT64)a.w * a.h > (UINT64)b.w * b.h;
    });
    ComPtr<IMMDeviceEnumerator> en;
    g_sl.ins.clear();
    g_sl.outs.clear();
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        g_sl.ins = EnumCapture(en.Get());
        g_sl.outs = EnumEndpoints(en.Get(), eRender);
    }

    SendDlgItemMessageW(h, IDC_VIDEO, CB_RESETCONTENT, 0, 0);
    ComboAdd(h, IDC_VIDEO, L"Auto (best capture card)");
    int sel = 0;
    for (size_t i = 0; i < g_sl.vids.size(); i++) {
        ComboAdd(h, IDC_VIDEO, g_sl.vids[i].name);
        if (!sel && ((g_videoPick >= 0 && (int)i == g_videoPick) || (g_videoPick < 0 && Contains(g_sl.vids[i].name, s.videoName))))
            sel = (int)i + 1;
    }
    ComboSet(h, IDC_VIDEO, sel);

    SendDlgItemMessageW(h, IDC_MODE, CB_RESETCONTENT, 0, 0);
    ComboAdd(h, IDC_MODE, L"Auto (highest resolution, then frame rate)");
    sel = 0;
    for (size_t i = 0; i < g_sl.modes.size(); i++) {
        auto& f = g_sl.modes[i];
        ComboAdd(h, IDC_MODE, Fmt(L"%ux%u at %.2f fps", f.w, f.h, Fps(f)));
        if (!sel && (s.reqW || s.reqFps > 0) && (!s.reqW || (f.w == s.reqW && f.h == s.reqH)) &&
            (s.reqFps <= 0 || std::fabs(Fps(f) - s.reqFps) < 0.02))
            sel = (int)i + 1;
    }
    if (g_sl.modes.empty()) ComboAdd(h, IDC_MODE, L"(no modes yet, waiting for the capture card)");
    ComboSet(h, IDC_MODE, sel);

    SendDlgItemMessageW(h, IDC_AUDIOIN, CB_RESETCONTENT, 0, 0);
    ComboAdd(h, IDC_AUDIOIN, L"Auto (the capture card's audio)");
    ComboAdd(h, IDC_AUDIOIN, L"None");
    sel = 0;
    if (g_audioPick == -2 || Lower(s.audioName) == L"none") sel = 1;
    for (size_t i = 0; i < g_sl.ins.size(); i++) {
        ComboAdd(h, IDC_AUDIOIN, g_sl.ins[i].name);
        if (!sel && ((g_audioPick >= 0 && (int)i == g_audioPick) || (g_audioPick == -1 && Contains(g_sl.ins[i].name, s.audioName))))
            sel = (int)i + 2;
    }
    ComboSet(h, IDC_AUDIOIN, sel);

    SendDlgItemMessageW(h, IDC_AUDIOOUT, CB_RESETCONTENT, 0, 0);
    ComboAdd(h, IDC_AUDIOOUT, L"Windows default output");
    sel = 0;
    for (size_t i = 0; i < g_sl.outs.size(); i++) {
        ComboAdd(h, IDC_AUDIOOUT, g_sl.outs[i].name);
        if (!sel && !s.outputId.empty() && g_sl.outs[i].id == s.outputId) sel = (int)i + 1;
    }
    if (!sel && !s.outputId.empty()) {
        ComboAdd(h, IDC_AUDIOOUT, s.outputName + L" (not connected)");
        sel = (int)g_sl.outs.size() + 1;
    }
    ComboSet(h, IDC_AUDIOOUT, sel);

    ComboSet(h, IDC_MULT, std::min(2, std::max(0, s.fgMult - 2)));
    Check(h, IDC_PREDICT, s.fgPredict);
    Check(h, IDC_FULL, s.fullscreen);
    Check(h, IDC_EXCL, g_exclusive);
    Check(h, IDC_DEBUG, g_debug);
    SyncSettingsUi();
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_CLOSE || id == IDCANCEL) {
                DestroyWindow(h);
                return 0;
            }
            if (code == CBN_SELCHANGE) {
                int i = ComboSel(h, id);
                if (i < 0) return 0;
                std::lock_guard<std::mutex> l(g_setMx);
                switch (id) {
                    case IDC_VIDEO:
                        g_set.videoName = i == 0 ? L"" : g_sl.vids[i - 1].name;
                        g_videoPick = -1;
                        g_videoRestart = true;
                        break;
                    case IDC_MODE:
                        if (i == 0 || i > (int)g_sl.modes.size()) {
                            g_set.reqW = g_set.reqH = 0;
                            g_set.reqFps = 0;
                        } else {
                            g_set.reqW = g_sl.modes[i - 1].w;
                            g_set.reqH = g_sl.modes[i - 1].h;
                            g_set.reqFps = Fps(g_sl.modes[i - 1]);
                        }
                        g_videoRestart = true;
                        break;
                    case IDC_AUDIOIN:
                        g_set.audioName = i == 0 ? L"" : i == 1 ? L"none" : g_sl.ins[i - 2].name;
                        g_audioPick = -1;
                        g_audioRestart = true;
                        break;
                    case IDC_AUDIOOUT:
                        if (i == 0) {
                            g_set.outputId.clear();
                            g_set.outputName.clear();
                        } else if (i <= (int)g_sl.outs.size()) {
                            g_set.outputId = g_sl.outs[i - 1].id;
                            g_set.outputName = g_sl.outs[i - 1].name;
                        }
                        g_audioRestart = true;
                        break;
                    case IDC_MULT:
                        g_set.fgMult = i + 2;
                        g_fgMult = i + 2;
                        break;
                    case IDC_FG:
                        break;
                }
            }
            if (code == CBN_SELCHANGE && id == IDC_FG) {
                SetFgMode(ComboSel(h, IDC_FG), h);
                ComboSet(h, IDC_FG, g_fgMode);  // a mode the driver refused falls back to the old one
                UpdateInfo(h);
            }
            if (code == BN_CLICKED) {
                if (id == IDC_MUTE) g_mute = Checked(h, IDC_MUTE);
                if (id == IDC_VSYNC) g_vsync = Checked(h, IDC_VSYNC);
                if (id == IDC_HDR) g_hdrAllow = Checked(h, IDC_HDR);
                if (id == IDC_PREDICT) {
                    g_fgPredict = Checked(h, IDC_PREDICT);
                    {
                        std::lock_guard<std::mutex> l(g_setMx);
                        g_set.fgPredict = g_fgPredict;
                    }
                    UpdateInfo(h);
                }
                if (id == IDC_SHOWFPS) {
                    g_showFps = Checked(h, IDC_SHOWFPS);
                    UpdateFps(g_hwnd, 0);
                    if (!g_videoActive) g_renderer.PresentBlack();
                }
                if (id == IDC_EXCL) {
                    g_exclusive = Checked(h, IDC_EXCL);
                    if (g_fullscreen) g_renderer.SetExclusive(g_exclusive);
                }
                if (id == IDC_DEBUG) SetDebug(Checked(h, IDC_DEBUG));
                if (id == IDC_REPORT) SaveDebugReport(h);
                if (id == IDC_BUG) ReportBug(h);
                std::lock_guard<std::mutex> l(g_setMx);
                g_set.debug = g_debug;
                g_set.mute = g_mute;
                g_set.vsync = g_vsync;
                g_set.showFps = g_showFps;
                g_set.hdr = g_hdrAllow;
                g_set.exclusive = g_exclusive;
                if (id == IDC_FULL) g_set.fullscreen = Checked(h, IDC_FULL);
            }
            if (code == CBN_SELCHANGE || code == BN_CLICKED) {
                SaveIni();
                if (g_settings) UpdateInfo(h);
            }
            return 0;
        }
        case WM_HSCROLL:
            if ((HWND)lp == GetDlgItem(h, IDC_VOLUME)) {
                int v = (int)SendDlgItemMessageW(h, IDC_VOLUME, TBM_GETPOS, 0, 0);
                g_volume = v / 100.0f;
                SetDlgItemTextW(h, IDC_VOLTEXT, Fmt(L"%d%%", v).c_str());
                {
                    std::lock_guard<std::mutex> l(g_setMx);
                    g_set.volume = g_volume;
                }
                if (LOWORD(wp) == TB_ENDTRACK || LOWORD(wp) == TB_THUMBPOSITION) SaveIni();
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            g_settings = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenSettings() {
    if (g_settings) {
        SetForegroundWindow(g_settings);
        return;
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"CapViewSettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = GetDpiForWindow(g_hwnd);
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    const int M = 12, LW = 130, CW = 330, RH = 32;
    int clientW = M + LW + 8 + CW + M;
    int y = M, rows = 12;
    int infoH = 130;
    int clientH = M + rows * RH + infoH + 12 + 28 + M;
    RECT rc = {0, 0, S(clientW), S(clientH)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    RECT mr;
    GetWindowRect(g_hwnd, &mr);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    int x = std::max<int>(mr.left, (mr.left + mr.right - w) / 2), top = std::max<int>(mr.top, (mr.top + mr.bottom - hgt) / 2);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CapViewSettings", L"CapView settings", style, x, top, w, hgt, g_hwnd,
                             nullptr, inst, nullptr);
    if (!h) {
        Log(L"Settings window failed to open (error %u)", GetLastError());
        return;
    }
    g_settings = h;
    auto ctl = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, S(cx), S(cy), S(cw), S(ch), h,
                                 (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        return c;
    };
    const int cx = M + LW + 8;
    auto label = [&](const wchar_t* t) { ctl(L"STATIC", t, SS_LEFT, M, y + 4, LW, 20, -1); };
    auto combo = [&](int id, int width) { ctl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, width, 300, id); };

    label(L"Video device");
    combo(IDC_VIDEO, CW);
    y += RH;
    label(L"Video mode");
    combo(IDC_MODE, CW);
    y += RH;
    label(L"Audio input");
    combo(IDC_AUDIOIN, CW);
    y += RH;
    label(L"Audio output");
    combo(IDC_AUDIOOUT, CW);
    y += RH;
    label(L"Volume");
    HWND tb = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, cx - 4, y, CW - 52, 26, IDC_VOLUME);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
    ctl(L"STATIC", L"100%", SS_LEFT, cx + CW - 44, y + 4, 44, 20, IDC_VOLTEXT);
    y += RH;
    ctl(L"BUTTON", L"Mute", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y + 2, 80, 22, IDC_MUTE);
    ctl(L"BUTTON", L"Vsync", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 90, y + 2, 80, 22, IDC_VSYNC);
    ctl(L"BUTTON", L"Start fullscreen", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 180, y + 2, 150, 22, IDC_FULL);
    y += RH;
    label(L"Frame generation");
    combo(IDC_FG, CW);
    y += RH;
    label(L"Multiplier");
    combo(IDC_MULT, 90);
    ctl(L"BUTTON", L"Predict ahead (no frame wait)", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 100, y + 2, 230, 22,
        IDC_PREDICT);
    y += RH;
    label(L"Display");
    ctl(L"BUTTON", L"HDR output for HDR sources", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y + 2, 200, 22, IDC_HDR);
    ctl(L"BUTTON", L"Show fps (F)", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 210, y + 2, 120, 22, IDC_SHOWFPS);
    y += RH;
    label(L"Fullscreen");
    ctl(L"BUTTON", L"Exclusive (takes the display while CapView has focus)", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y + 2,
        CW, 22, IDC_EXCL);
    y += RH;
    label(L"Troubleshooting");
    ctl(L"BUTTON", L"Debug mode (detailed log)", BS_AUTOCHECKBOX | WS_TABSTOP, cx, y + 2, CW, 22, IDC_DEBUG);
    y += RH;
    ctl(L"BUTTON", L"Report a bug", BS_PUSHBUTTON | WS_TABSTOP, cx, y, 160, 26, IDC_BUG);
    ctl(L"BUTTON", L"Save debug report", BS_PUSHBUTTON | WS_TABSTOP, cx + 170, y, CW - 170, 26, IDC_REPORT);
    y += RH;
    ctl(L"STATIC", L"", SS_LEFT, M, y + 4, clientW - 2 * M, infoH, IDC_INFO);
    y += infoH + 12;
    ctl(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, clientW - M - 90, y, 90, 28, IDC_CLOSE);

    const wchar_t* fg[] = {L"Off (lowest delay)", L"CapView interpolation (any GPU)", L"NVIDIA Smooth Motion (driver)",
                           L"AMD Fluid Motion Frames (driver)",
                           L"Intel XeSS frame generation"};
    for (auto s : fg) ComboAdd(h, IDC_FG, s);
    for (auto s : {L"2x", L"3x", L"4x"}) ComboAdd(h, IDC_MULT, s);
    PopulateSettings(h);
    ShowWindow(h, SW_SHOW);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));  // WM_SETCURSOR waits for the mouse to move
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_debug) DebugMsg(msg, wp, lp);
    switch (msg) {
        case WM_ACTIVATEAPP:
            // Exclusive fullscreen is given up while another app has focus, and taken back on return.
            if (g_fullscreen && g_exclusive) g_renderer.SetExclusive(wp != FALSE);
            break;
        case WM_SIZE:
            if (g_modeSwitch) return 0;  // SetExclusive resizes the buffers itself
            if (g_renderer.Ready() && wp != SIZE_MINIMIZED) {
                g_renderer.Resize(LOWORD(lp), HIWORD(lp));
                if (!g_videoActive) g_renderer.PresentBlack();
            }
            return 0;
        case WM_PAINT:
            ValidateRect(h, nullptr);
            if (g_renderer.Ready() && !g_videoActive) g_renderer.PresentBlack();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            UpdateTitle(h);
            PlaceCapView();  // catches a change the window event hooks missed
            if (g_showFps && !g_videoActive && g_renderer.Ready()) g_renderer.PresentBlack();
            return 0;
        case WM_SETCURSOR:
            // Fullscreen hides the pointer over the picture, except while settings is open.
            if (g_fullscreen && !g_settings && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_LBUTTONDBLCLK: ToggleFullscreen(h); return 0;
        case WM_RBUTTONUP: OpenSettings(); return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (lp & (1 << 29))) {
                ToggleFullscreen(h);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wp >= 'A' && wp <= 'Z' && (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)) return 0;
            switch (wp) {
                case 'S':
                case VK_F2: OpenSettings(); break;
                case 'G': SetFgMode(g_fgMode == FG_CAPVIEW ? FG_OFF : FG_CAPVIEW, h); break;
                case 'F':
                    g_showFps = !g_showFps;
                    {
                        std::lock_guard<std::mutex> l(g_setMx);
                        g_set.showFps = g_showFps;
                    }
                    SaveIni();
                    SyncSettingsUi();
                    UpdateFps(h, 0);
                    if (!g_videoActive) g_renderer.PresentBlack();
                    break;
                case VK_F11: ToggleFullscreen(h); break;
                case VK_ESCAPE: if (g_fullscreen) ToggleFullscreen(h); break;
                case 'V': g_vsync = !g_vsync; break;
                case 'M': g_mute = !g_mute; break;
                case VK_UP: g_volume = std::min(2.0f, g_volume + 0.1f); break;
                case VK_DOWN: g_volume = std::max(0.0f, g_volume - 0.1f); break;
                case 'D': {
                    int n = g_videoCount, c = g_videoCurIdx;
                    int next = g_videoPick < 0 ? (c + 1) : (g_videoPick + 1);
                    g_videoPick = next >= n ? -1 : next;
                    g_videoRestart = true;
                    break;
                }
                case 'A': {
                    int n = g_audioCount, p = g_audioPick;
                    int next;
                    if (p == -2) next = -1;  // none -> auto
                    else if (p == -1) next = g_audioCurIdx + 1;
                    else next = p + 1;
                    if (next >= n) next = -2;  // past the end -> none
                    g_audioPick = next;
                    g_audioRestart = true;
                    break;
                }
                case 'R': g_videoRestart = true; g_audioRestart = true; break;
                case VK_F1: MessageBoxW(h, kHelp, L"CapView", MB_OK | MB_ICONINFORMATION); break;
            }
            if (wp == 'V' || wp == 'M' || wp == VK_UP || wp == VK_DOWN) {
                {
                    std::lock_guard<std::mutex> l(g_setMx);
                    g_set.vsync = g_vsync;
                    g_set.mute = g_mute;
                    g_set.volume = g_volume;
                }
                SaveIni();
                SyncSettingsUi();
            }
            UpdateTitle(h);
            return 0;
        case WM_DESTROY:
            g_renderer.SetExclusive(false, false);  // DXGI wants the swap chain windowed before it goes away
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ParseArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = Lower(argv[i]);
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        static const wchar_t* keyFor[][2] = {
            {L"--video", L"video"},         {L"--audio", L"audio"},           {L"--fullscreen", L"fullscreen"},
            {L"--vsync", L"vsync"},         {L"--volume", L"volume"},         {L"--mode", L"mode"},
            {L"--output", L"output"},       {L"--output", L"outputName"},     {L"--framegen", L"framegen"},
            {L"--multiplier", L"multiplier"}, {L"--hdr", L"hdr"},     {L"--exclusive", L"exclusive"},
            {L"--debug", L"debug"},
        };
        for (auto& k : keyFor)
            if (a == k[0]) g_cliKeys.push_back(k[1]);
        if (a == L"--video") g_set.videoName = next();
        else if (a == L"--audio") g_set.audioName = next();
        else if (a == L"--fullscreen") g_set.fullscreen = true;
        else if (a == L"--exclusive") g_set.exclusive = true;
        else if (a == L"--debug") g_set.debug = true;
        else if (a == L"--vsync") g_set.vsync = true;
        else if (a == L"--list") g_set.list = true;
        else if (a == L"--volume") g_set.volume = (float)_wtof(next().c_str()) / 100.0f;
        else if (a == L"--mode") ParseMode(next(), g_set);
        else if (a == L"--output") {
            g_set.outputName = next();
            g_set.outputId.clear();
        } else if (a == L"--framegen") {
            std::wstring v = Lower(next());
            for (int k = 0; k < FG_COUNT; k++)
                if (v == kFgKeys[k]) g_set.fgMode = k;
        } else if (a == L"--multiplier") g_set.fgMult = std::min(4, std::max(2, _wtoi(next().c_str())));
        else if (a == L"--nv-smooth-motion") g_set.nvSmooth = _wtoi(next().c_str()) ? 1 : 0;
        else if (a == L"--hdr") g_set.hdr = Lower(next()) != L"off";
        else if (a == L"--after") g_afterPid = (DWORD)_wtoi(next().c_str());
    }
    LocalFree(argv);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        std::wstring exe = ExePath();
        g_iniPath = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"capview.ini";
    }
    LoadIni();
    ParseArgs();
    {
        wchar_t v[32] = {};
        if (GetEnvironmentVariableW(L"CAPVIEW_TIMING_SECS", v, 32)) g_timingSecs = g_timingSecsEnv = std::max(5.0, _wtof(v));
        if (GetEnvironmentVariableW(L"CAPVIEW_STALL_HOLD_SECS", v, 32))
            g_stallHoldMs = (ULONGLONG)std::clamp(_wtoi(v), 0, 600) * 1000;
    }
    g_cliSet = g_set;
    if (g_set.nvSmooth >= 0) return ApplySmoothMotion(g_set.nvSmooth == 1);
    if (g_afterPid) {
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, g_afterPid)) {
            WaitForSingleObject(p, 5000);
            CloseHandle(p);
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    if (g_set.list) {
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!g_out || GetFileType(g_out) == FILE_TYPE_UNKNOWN) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
            g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        ListDevices();
        fflush(stdout);
        MFShutdown();
        return 0;
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring logPath = path;
    logPath = logPath.substr(0, logPath.find_last_of(L"\\/") + 1) + L"capview.log";
    // Keep the previous run's log, so a restart after a fault doesn't wipe what led up to it.
    MoveFileExW(logPath.c_str(), (logPath.substr(0, logPath.size() - 4) + L".prev.log").c_str(),
                MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
    InstallCrashHandlers();
    Log(L"CapView %s built %s%s", kVersion, kBuild, g_set.debug ? L", debug mode on" : L"");
    g_debug = g_set.debug;
    if (g_debug && !g_timingSecsEnv) g_timingSecs = 5;
    // AFMF follows the mode: on in AFMF mode, and back off if an earlier run turned it on and didn't get
    // to undo it. It's switched before the swap chain exists, so the driver sees it from the start.
    if (g_set.fgMode == FG_AMD || IniGet(L"afmfOwned", L"0") == L"1") AfmfFollow(g_set.fgMode == FG_AMD);

    g_vsync = g_set.vsync;
    g_exclusive = g_set.exclusive;
    g_volume = std::min(2.0f, std::max(0.0f, g_set.volume));
    g_mute = g_set.mute;
    g_showFps = g_set.showFps;
    g_hdrAllow = g_set.hdr;
    g_fgMode = std::min(FG_COUNT - 1, std::max(0, g_set.fgMode));
    g_fgMult = std::min(4, std::max(2, g_set.fgMult));
    g_fgPredict = g_set.fgPredict;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"CapViewWindow";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT rc = {0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"CapView", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !g_renderer.Init(g_hwnd)) {
        MessageBoxW(nullptr, L"Could not set up Direct3D 11 video output on this GPU.", L"CapView", MB_ICONERROR);
        return 1;
    }
    if (g_fgMode == FG_XESS && !XessFollow(true)) Log(L"Startup: XeSS frame generation not running");
    ShowWindow(g_hwnd, show);
    if (g_set.fullscreen) ToggleFullscreen(g_hwnd);
    g_renderer.PresentBlack();
    SetTimer(g_hwnd, 1, 500, nullptr);
    Log(L"Settings file %s, frame generation %s %dx", g_iniPath.c_str(), kFgKeys[g_fgMode.load()], g_fgMult.load());
    g_encore = FindRtxEncore();
    if (!g_encore.empty()) {
        g_encoreSmoothOn = EncoreSmoothConfigured();
        Log(L"RTX Encore loaded as %s, Smooth Motion %s in its config", g_encore.c_str(),
            g_encoreSmoothOn ? L"on" : L"off");
    }

    std::thread vt(VideoThread);
    std::thread at(AudioThread);
    // RTX Encore's Smooth Motion follows CapView's mode: on in Smooth Motion mode, off in the others.
    // RTX Encore read its config before CapView started, so a change here needs a restart.
    bool restarting = false;
    bool wantSmooth = g_fgMode == FG_SMOOTH;
    if (!g_encore.empty() && g_encoreSmoothOn != wantSmooth) {
        int r = EncoreSetSmooth(wantSmooth);
        Log(L"Startup: RTX Encore Smooth Motion %s in rtx-encore.jsonc for %s mode: %s", wantSmooth ? L"on" : L"off",
            kFgKeys[g_fgMode.load()], r > 0 ? L"changed" : r == 0 ? L"already set" : L"write failed");
        if (r >= 0) {
            std::wstring msg = std::wstring(L"RTX Encore had Smooth Motion ") + (wantSmooth ? L"off" : L"on") +
                               L", which doesn't match CapView's frame generation mode, so CapView turned it " +
                               (wantSmooth ? L"on" : L"off") +
                               L" in RTX Encore's settings. RTX Encore applies it when CapView starts.\n\nRestart CapView now?";
            if (MessageBoxW(g_hwnd, msg.c_str(), L"CapView", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                // A --framegen from the command line isn't saved, so it goes along, or the new copy would
                // start in the saved mode and switch RTX Encore back.
                bool cliMode = std::find(g_cliKeys.begin(), g_cliKeys.end(), L"framegen") != g_cliKeys.end();
                RestartSelf(cliMode ? std::wstring(L" --framegen ") + kFgKeys[g_fgMode.load()] : L"");
                restarting = true;
            }
        }
    }
    // First run: a notification on screen makes the compositor draw CapView, and with Smooth Motion
    // that shows about 30 of 60 frames. Windows' Do Not Disturb rule for full-screen apps stops it.
    if (g_debug) LogReport();
    // CAPVIEW_CRASH_TEST=1 crashes on purpose, to check that crash dumps get written.
    if (GetEnvironmentVariableW(L"CAPVIEW_CRASH_TEST", nullptr, 0)) {
        Log(L"CAPVIEW_CRASH_TEST is set, crashing on purpose");
        *(volatile int*)nullptr = 0;
    }
    if (!restarting && IniGet(L"dndTipShown", L"0") != L"1") {
        IniPut(L"dndTipShown", L"1");
        Log(L"First run: showing the Do Not Disturb tip");
        const int kOpenSettings = 100;
        TASKDIALOG_BUTTON buttons[] = {{kOpenSettings, L"Open notification settings"}};
        TASKDIALOGCONFIG tc = {sizeof(tc)};
        tc.hwndParent = g_hwnd;
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tc.pszWindowTitle = L"CapView";
        tc.pszMainIcon = TD_INFORMATION_ICON;
        tc.pszContent =
            L"Windows notifications make CapView stutter. While a notification is on screen, Windows draws "
            L"CapView through the desktop compositor, and the picture can drop to about 30 fps.\n\n"
            L"To stop this, turn on Do Not Disturb for full-screen apps. Open Settings > System > "
            L"Notifications, and under \"Turn on do not disturb automatically\" tick \"When using an app in "
            L"full-screen mode\". Notifications then wait while CapView is fullscreen (F11).\n\n"
            L"CapView shows this message only once.";
        tc.cButtons = ARRAYSIZE(buttons);
        tc.pButtons = buttons;
        tc.dwCommonButtons = TDCBF_OK_BUTTON;
        tc.nDefaultButton = IDOK;
        int pressed = IDOK;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) pressed = IDOK;
        if (pressed == kOpenSettings) {
            Log(L"Opening Windows notification settings from the tip");
            ShellExecuteW(nullptr, L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_quit = true;
    vt.join();
    at.join();
    if (IniGet(L"afmfOwned", L"0") == L"1") AfmfFollow(false);
    SetThreadExecutionState(ES_CONTINUOUS);
    Log(L"Exit");
    MFShutdown();
    if (g_log) fclose(g_log);
    return 0;
}
