// 动态壁纸 — Ultra-thin Win32 + WebView2 wallpaper shell
// Based on QiangQiang framework. Embeds WebView2 behind desktop icons
// to play MP4 video as dynamic wallpaper.
//
// Build:
//   cl /EHsc /O2 /std:c++20 /utf-8 main.cpp
//      /I<webview2_include> /I<json_include>
//      /Fe:app.exe /link /SUBSYSTEM:WINDOWS <WebView2LoaderStatic.lib>
//
// Usage:
//   app.exe                              -> Production (virtual host -> dist/)
//   app.exe --dev http://localhost:3000  -> Dev mode

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <cstdio>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <shobjidl.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <string>
#include <functional>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <set>
#include <algorithm>
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wincrypt.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "crypt32.lib")

#include "resource.h"

#include "json.hpp"
using json = nlohmann::json;
using namespace Microsoft::WRL;
namespace fspath = std::filesystem;

// ================================================================
//  String helpers
// ================================================================

static std::string W2U(const wchar_t* w, int len = -1) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    if (len == -1 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
static std::string W2U(const std::wstring& w) { return W2U(w.c_str(), (int)w.size()); }

static std::wstring U2W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p);
    return p;
}

// ================================================================
//  Shell thumbnail → base64 JPEG data URI
// ================================================================

static std::string getShellThumbnail(const std::wstring& path, int tw = 256, int th = 144) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return {};

    ComPtr<IShellItemImageFactory> fac;
    if (FAILED(item.As(&fac))) return {};

    HBITMAP hbmp = nullptr;
    SIZE sz = { tw, th };
    if (FAILED(fac->GetImage(sz, SIIGBF_RESIZETOFIT, &hbmp)) || !hbmp) return {};

    // Encode via WIC
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        DeleteObject(hbmp); return {};
    }
    ComPtr<IWICBitmap> wb;
    if (FAILED(wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapIgnoreAlpha, &wb))) {
        DeleteObject(hbmp); return {};
    }
    DeleteObject(hbmp);

    ComPtr<IStream> stm;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stm))) return {};

    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) return {};
    if (FAILED(enc->Initialize(stm.Get(), WICBitmapEncoderNoCache))) return {};

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr))) return {};
    if (FAILED(frame->Initialize(nullptr))) return {};

    UINT bw, bh; wb->GetSize(&bw, &bh);
    frame->SetSize(bw, bh);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WriteSource(wb.Get(), nullptr))) return {};
    frame->Commit(); enc->Commit();

    STATSTG stat; stm->Stat(&stat, STATFLAG_NONAME);
    DWORD size = stat.cbSize.LowPart;
    LARGE_INTEGER zero{}; stm->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<BYTE> buf(size);
    ULONG rd; stm->Read(buf.data(), size, &rd);

    DWORD b64Len = 0;
    CryptBinaryToStringA(buf.data(), rd,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len);
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA(buf.data(), rd,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &b64Len);
    b64.resize(b64Len);
    return "data:image/jpeg;base64," + b64;
}

// ================================================================
//  Global state
// ================================================================

static HWND                              g_hwnd;
static ComPtr<ICoreWebView2Environment>  g_env;
static ComPtr<ICoreWebView2Controller>   g_ctrl;
static ComPtr<ICoreWebView2>             g_view;
static std::wstring                      g_devUrl;

// Config
static json g_cfg;

// Tray
#define WM_TRAYICON (WM_USER + 1)
static NOTIFYICONDATAW g_nid = {};
static bool            g_trayActive = false;

// Wallpaper
static HWND g_workerW = nullptr;
static HWND g_msgWnd = nullptr;   // hidden top-level window for tray & menu

// Panel (control panel window with its own WebView2)
static HWND                             g_panelHwnd = nullptr;
static ComPtr<ICoreWebView2Controller>  g_panelCtrl;
static ComPtr<ICoreWebView2>            g_panelView;

// Wallpaper state (shared between wallpaper + panel WebView2)
static json g_wpState = {
    {"speed", 1.0}, {"volume", 1.0}, {"muted", true},
    {"paused", false}, {"videoPath", ""}
};

// Embedded assets (single-exe mode)
#ifdef SINGLE_EXE
struct PakEntry { std::string path; const char* data; uint32_t size; };
static std::vector<PakEntry> g_pakEntries;
#endif

static std::wstring g_appDataDir;
static std::wstring app_data_dir() {
    if (!g_appDataDir.empty()) return g_appDataDir;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        auto title = g_cfg.value("/window/title"_json_pointer, std::string{"KoDesktop"});
        g_appDataDir = std::wstring(p) + L"\\" + U2W(title);
        CoTaskMemFree(p);
    } else {
        g_appDataDir = exe_dir() + L"\\data";
    }
    fspath::create_directories(g_appDataDir);
    return g_appDataDir;
}

// ================================================================
//  Embedded resource loader (single-exe mode)
// ================================================================

#ifdef SINGLE_EXE
static std::string loadResourceString(int id) {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCE(id), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return {};
    DWORD sz = SizeofResource(nullptr, hRes);
    auto* ptr = (const char*)LockResource(hData);
    return std::string(ptr, sz);
}

static void loadPak() {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCE(IDR_HTML), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return;
    DWORD totalSize = SizeofResource(nullptr, hRes);
    const char* base = (const char*)LockResource(hData);
    if (!base || totalSize < 4 || base[0] != 'Q' || base[1] != 'Q') return;
    const char* end = base + totalSize;
    const char* p = base + 2;
    uint16_t count; memcpy(&count, p, 2); p += 2;
    for (uint16_t i = 0; i < count && p < end; i++) {
        if (p + 2 > end) break;
        uint16_t pathLen; memcpy(&pathLen, p, 2); p += 2;
        if (p + pathLen > end) break;
        std::string path(p, pathLen); p += pathLen;
        if (p + 4 > end) break;
        uint32_t dataLen; memcpy(&dataLen, p, 4); p += 4;
        if (p + dataLen > end) break;
        g_pakEntries.push_back({path, p, dataLen});
        p += dataLen;
    }
}

static const PakEntry* findPakEntry(const std::string& path) {
    for (auto& e : g_pakEntries)
        if (e.path == path) return &e;
    return nullptr;
}

static std::wstring guessMimeType(const std::string& path) {
    auto ext = path.substr(path.rfind('.') + 1);
    if (ext == "html" || ext == "htm") return L"text/html";
    if (ext == "js" || ext == "mjs")   return L"application/javascript";
    if (ext == "css")                  return L"text/css";
    if (ext == "json")                 return L"application/json";
    if (ext == "svg")                  return L"image/svg+xml";
    if (ext == "png")                  return L"image/png";
    if (ext == "jpg" || ext == "jpeg") return L"image/jpeg";
    if (ext == "mp4")                  return L"video/mp4";
    if (ext == "webm")                 return L"video/webm";
    if (ext == "ogg")                  return L"video/ogg";
    return L"application/octet-stream";
}
#endif

// ================================================================
//  Config loader
// ================================================================

static json loadConfig(const std::wstring& dir) {
    for (auto& name : { L"\\app.config.json", L"\\..\\app.config.json" }) {
        auto path = dir + name;
        std::ifstream f(path);
        if (f) { json j; f >> j; return j; }
    }
#ifdef SINGLE_EXE
    auto cfg = loadResourceString(IDR_CONFIG);
    if (!cfg.empty()) {
        try { return json::parse(cfg); } catch (...) {}
    }
#endif
    return json::object();
}

// ================================================================
//  IPC bridge
// ================================================================

using IpcFn = std::function<json(const json&)>;
static std::unordered_map<std::string, IpcFn> g_cmds;

static void ipc_on(const std::string& cmd, IpcFn fn) {
    g_cmds[cmd] = std::move(fn);
}

static void ipc_emit(const std::string& ev, const json& data = {}) {
    if (!g_view) return;
    json m = {{"event", ev}, {"data", data}};
    g_view->PostWebMessageAsJson(U2W(m.dump()).c_str());
}

static void ipc_emit_panel(const std::string& ev, const json& data = {}) {
    if (!g_panelView) return;
    json m = {{"event", ev}, {"data", data}};
    g_panelView->PostWebMessageAsJson(U2W(m.dump()).c_str());
}

static void ipc_dispatch(ICoreWebView2* respondTo, LPCWSTR raw) {
    try {
        auto req = json::parse(W2U(raw));
        json resp;
        resp["id"] = req.value("id", -1);
        auto cmd  = req.value("cmd", std::string{});
        auto args = req.value("args", json::object());
        if (auto it = g_cmds.find(cmd); it != g_cmds.end()) {
            try { resp["result"] = it->second(args); }
            catch (const std::exception& e) { resp["error"] = e.what(); }
        } else {
            resp["error"] = "unknown: " + cmd;
        }
        respondTo->PostWebMessageAsJson(U2W(resp.dump()).c_str());
    } catch (...) {}
}

// ================================================================
//  Wallpaper embedding — place window behind desktop icons
// ================================================================

// Clean up stale mirror from a previous crash/force-kill
static void cleanupStaleMirror() {
    // Find any leftover WallpaperMirror window
    HWND stale = FindWindowW(L"WallpaperMirror", nullptr);
    if (stale) {
        DestroyWindow(stale);
        // Re-apply wallpaper to reset CLR_NONE
        wchar_t wp[MAX_PATH] = {};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wp, SPIF_SENDCHANGE);
    }
    // Also hide any stale WorkerW child of Progman from previous 0x052C
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND ww = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        if (ww && IsWindowVisible(ww)) {
            ShowWindow(ww, SW_HIDE);
            wchar_t wp[MAX_PATH] = {};
            SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
            SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wp, SPIF_SENDCHANGE);
        }
    }
}

static void embedAsWallpaper() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return;

    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);

    // 1) Send 0x052C to Progman — spawns WorkerW for wallpaper layer
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

    // 2) Find the correct WorkerW to host our content
    //    On Win11: WorkerW is a child of Progman
    //    On Win10: WorkerW is a separate top-level window
    g_workerW = nullptr;
    HWND child = nullptr;
    while ((child = FindWindowExW(progman, child, L"WorkerW", nullptr)) != nullptr) {
        RECT r; GetWindowRect(child, &r);
        if ((r.right - r.left) >= cx && (r.bottom - r.top) >= cy) {
            g_workerW = child;
            break;
        }
    }
    if (!g_workerW) {
        // Fallback: search top-level WorkerW
        HWND w2 = nullptr;
        while ((w2 = FindWindowExW(nullptr, w2, L"WorkerW", nullptr)) != nullptr) {
            if (FindWindowExW(w2, nullptr, L"SHELLDLL_DefView", nullptr))
                continue;
            RECT r; GetWindowRect(w2, &r);
            if ((r.right - r.left) >= cx && (r.bottom - r.top) >= cy) {
                g_workerW = w2;
                break;
            }
        }
    }
    if (!g_workerW) return;

    // 3) Reparent WebView2 directly into WorkerW using the proper API
    if (g_ctrl) {
        g_ctrl->put_ParentWindow(g_workerW);
        RECT rc = {0, 0, cx, cy};
        g_ctrl->put_Bounds(rc);
    }

    // 4) Hide the original window from taskbar
    LONG exStyle = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    exStyle = (exStyle | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
    SetWindowLongW(g_hwnd, GWL_EXSTYLE, exStyle);
    ShowWindow(g_hwnd, SW_HIDE);
}

static void restoreDesktop() {
    // Reparent WebView2 back to g_hwnd before cleanup
    if (g_ctrl && g_hwnd && IsWindow(g_hwnd)) {
        g_ctrl->put_ParentWindow(g_hwnd);
    }
    g_workerW = nullptr;
    // Force Windows to re-apply the wallpaper
    wchar_t wallpaper[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaper, 0);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wallpaper, SPIF_SENDCHANGE);
}

// ================================================================
//  Panel — control panel window with second WebView2
// ================================================================

static bool g_panelVisible = false;
static const int RESIZE_BORDER = 6;
static void applyPanelDwmAttrs();

static LRESULT CALLBACK PanelWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (g_panelCtrl) {
            RECT b; GetClientRect(h, &b);
            g_panelCtrl->put_Bounds(b);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        g_panelVisible = false;
        return 0;
    case WM_NCCALCSIZE:
        if (w == TRUE) return 0;
        break;
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        RECT rc; GetWindowRect(h, &rc);
        bool left   = pt.x - rc.left   < RESIZE_BORDER;
        bool right  = rc.right  - pt.x < RESIZE_BORDER;
        bool top    = pt.y - rc.top    < RESIZE_BORDER;
        bool bottom = rc.bottom - pt.y < RESIZE_BORDER;
        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize = { 600, 400 };
        return 0;
    }
    case WM_ACTIVATE:
        // DWM resets border color on focus change — reassert ours every time.
        applyPanelDwmAttrs();
        break;
    case WM_NCACTIVATE:
        // Reassert DWM attrs then tell DefWindowProc "handled, don't repaint NC"
        // by passing -1 as lParam. Without this DWM redraws the outer border
        // in the system accent color when the window loses focus.
        applyPanelDwmAttrs();
        return DefWindowProcW(h, m, w, -1);
    }
    return DefWindowProcW(h, m, w, l);
}

static void showPanel();
static void hidePanel();

static void setupPanelWebView(ICoreWebView2Controller* ctrl) {
    g_panelCtrl = ctrl;
    ctrl->get_CoreWebView2(&g_panelView);

    ComPtr<ICoreWebView2Settings> settings;
    g_panelView->get_Settings(&settings);
    if (settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDevToolsEnabled(!g_devUrl.empty() ? TRUE : FALSE);
        settings->put_IsScriptEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
    }

    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(g_panelCtrl.As(&ctrl2))) {
        ctrl2->put_DefaultBackgroundColor({255, 13, 11, 31}); // #0d0b1f dark purple
    }

    // Virtual host mapping for panel
    ComPtr<ICoreWebView2_3> v3;
    if (SUCCEEDED(g_panelView.As(&v3))) {
        auto base = exe_dir();
        v3->SetVirtualHostNameToFolderMapping(
            L"app.localhost", base.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }

    // IPC handler for panel
    g_panelView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
            LPWSTR m; a->get_WebMessageAsJson(&m);
            ipc_dispatch(sender, m);
            CoTaskMemFree(m);
            return S_OK;
        }).Get(), nullptr);

    RECT rc; GetClientRect(g_panelHwnd, &rc);
    g_panelCtrl->put_Bounds(rc);
    g_panelCtrl->put_IsVisible(TRUE);

    if (!g_devUrl.empty()) {
        auto panelUrl = g_devUrl + std::wstring(L"/panel.html");
        g_panelView->Navigate(panelUrl.c_str());
    } else {
        g_panelView->Navigate(L"https://app.localhost/panel.html");
    }
}

static void createPanelWindow(HINSTANCE hi) {
    WNDCLASSEXW pc{sizeof(pc)};
    pc.lpfnWndProc   = PanelWndProc;
    pc.hInstance      = hi;
    pc.lpszClassName  = L"KoDesktop_Panel";
    pc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground  = CreateSolidBrush(RGB(13, 11, 31)); // #0d0b1f dark purple
    pc.hIcon          = LoadIconW(hi, MAKEINTRESOURCE(IDI_APP));
    pc.hIconSm        = (HICON)LoadImageW(hi, MAKEINTRESOURCE(IDI_APP),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!pc.hIcon) { pc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); pc.hIconSm = pc.hIcon; }
    RegisterClassExW(&pc);

    // Primary monitor work area — DPI aware via PER_MONITOR_AWARE_V2 context
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int screenW = wa.right - wa.left;
    int screenH = wa.bottom - wa.top;

    // ~83% of work area, with sane floor; respects DPI implicitly
    int panelW = max(900, screenW * 83 / 100);
    int panelH = max(600, screenH * 83 / 100);
    int sx = wa.left + (screenW - panelW) / 2;
    int sy = wa.top  + (screenH - panelH) / 2;

    // Pure frameless + resizable. WS_SYSMENU/WS_MINIMIZEBOX would make DWM paint
    // a caption strip at the top (the white/light bar that flashes on focus).
    // WS_EX_APPWINDOW keeps the window in taskbar; SW_MINIMIZE works without MINIMIZEBOX.
    g_panelHwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"KoDesktop_Panel", L"动态壁纸",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
        sx, sy, panelW, panelH,
        nullptr, nullptr, hi, nullptr);

    if (g_panelHwnd) applyPanelDwmAttrs();
}

// Apply DWM visual attributes. Called at creation AND on WM_ACTIVATE / WM_NCACTIVATE
// because DWM re-asserts its default border color on focus change in Win11.
static void applyPanelDwmAttrs() {
    if (!g_panelHwnd) return;

    // Rounded corners + native shadow
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g_panelHwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
        &corner, sizeof(corner));

    // DWMWA_SYSTEMBACKDROP_TYPE = 38; DWMSBT_NONE = 1. Disable any backdrop
    // effects (Mica/Acrylic) so DWM has no reason to paint a frame.
    DWORD backdrop = 1;
    DwmSetWindowAttribute(g_panelHwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/,
        &backdrop, sizeof(backdrop));

    // DWMWA_BORDER_COLOR = 34. COLOR_NONE hides the outer 1px border entirely
    // — like Tauri/Wails frameless windows.
    COLORREF border = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(g_panelHwnd, 34 /*DWMWA_BORDER_COLOR*/,
        &border, sizeof(border));

    // Tell DWM the entire client area is glass with no extended frame.
    // This is the piece Tauri/Wails do that we were missing — without it DWM
    // reserves/paints a system frame outside our content.
    MARGINS m = {0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(g_panelHwnd, &m);
}

static void initPanelWebView() {
    if (!g_env || !g_panelHwnd || g_panelView) return;
    g_env->CreateCoreWebView2Controller(g_panelHwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr) || !ctrl) return hr;
            setupPanelWebView(ctrl);
            return S_OK;
        }).Get());
}

static void showPanel() {
    if (!g_panelHwnd) return;
    if (g_panelVisible) {
        if (IsIconic(g_panelHwnd)) ShowWindow(g_panelHwnd, SW_RESTORE);
        SetForegroundWindow(g_panelHwnd);
        return;
    }
    ShowWindow(g_panelHwnd, SW_SHOW);
    SetForegroundWindow(g_panelHwnd);
    g_panelVisible = true;
    ipc_emit_panel("wallpaper.stateChanged", g_wpState);
}

static void hidePanel() {
    if (!g_panelHwnd || !g_panelVisible) return;
    ShowWindow(g_panelHwnd, SW_HIDE);
    g_panelVisible = false;
}

// ── Wallpaper control IPC (used by panel) ──────

static json show_open_file_dialog(const json&); // forward decl

static void reg_wallpaper_ctrl() {
    ipc_on("wallpaper.setState", [](const json& a) -> json {
        g_wpState = a;
        return true;
    });

    ipc_on("wallpaper.getState", [](const json&) -> json {
        return g_wpState;
    });

    ipc_on("wallpaper.play", [](const json&) -> json {
        ipc_emit("wallpaper.play");
        g_wpState["paused"] = false;
        return true;
    });

    ipc_on("wallpaper.pause", [](const json&) -> json {
        ipc_emit("wallpaper.pause");
        g_wpState["paused"] = true;
        return true;
    });

    ipc_on("wallpaper.restart", [](const json&) -> json {
        ipc_emit("wallpaper.restart");
        return true;
    });

    ipc_on("wallpaper.setSpeed", [](const json& a) -> json {
        double rate = a.value("rate", 1.0);
        g_wpState["speed"] = rate;
        ipc_emit("wallpaper.setSpeed", {{"rate", rate}});
        return true;
    });

    ipc_on("wallpaper.setVolume", [](const json& a) -> json {
        double vol = a.value("volume", 1.0);
        g_wpState["volume"] = vol;
        ipc_emit("wallpaper.setVolume", {{"volume", vol}});
        return true;
    });

    ipc_on("wallpaper.setMuted", [](const json& a) -> json {
        bool muted = a.value("muted", true);
        g_wpState["muted"] = muted;
        ipc_emit("wallpaper.setMuted", {{"muted", muted}});
        return true;
    });

    ipc_on("wallpaper.pickVideo", [](const json&) -> json {
        ipc_emit("wallpaper.pickVideo");
        return true;
    });

    ipc_on("wallpaper.setVideo", [](const json& a) -> json {
        auto filePath = a.value("path", std::string{});
        if (filePath.empty()) return false;
        auto wPath  = U2W(filePath);
        auto parent = fspath::path(wPath).parent_path().wstring();
        ComPtr<ICoreWebView2_3> v3;
        if (SUCCEEDED(g_view.As(&v3))) {
            v3->SetVirtualHostNameToFolderMapping(
                L"media.localhost", parent.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
        auto filename = fspath::path(wPath).filename().string();
        ipc_emit("wallpaper.setVideo", {{"path", filePath}, {"filename", filename}});
        return true;
    });

    // ── Library management ──────────
    ipc_on("library.load", [](const json&) -> json {
        auto path = app_data_dir() + L"\\library.json";
        std::ifstream f(path);
        if (!f) return json::array();
        try { json j; f >> j; return j; } catch (...) { return json::array(); }
    });

    ipc_on("library.save", [](const json& a) -> json {
        auto path = app_data_dir() + L"\\library.json";
        std::ofstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot write library.json");
        f << a.value("entries", json::array()).dump(2);
        return true;
    });

    ipc_on("library.addFiles", [](const json&) -> json {
        json dlgArgs = {
            {"multiple", true},
            {"filters", json::array({
                {{"name", "\u89C6\u9891\u6587\u4EF6"}, {"extensions", {"mp4","webm","ogg","mkv","avi","mov","wmv","flv"}}},
                {{"name", "\u6240\u6709\u6587\u4EF6"}, {"extensions", {"*"}}}
            })}
        };
        auto result = show_open_file_dialog(dlgArgs);
        if (result.is_null()) return json::array();
        json arr = json::array();
        auto paths = result.is_array() ? result : json::array({result});
        for (auto& p : paths) {
            auto wPath = U2W(p.get<std::string>());
            auto sz = fspath::exists(wPath) ? (int64_t)fspath::file_size(wPath) : 0;
            arr.push_back({
                {"path", p},
                {"name", fspath::path(wPath).filename().string()},
                {"size", sz}
            });
        }
        return arr;
    });

    ipc_on("library.addFolder", [](const json&) -> json {
        ComPtr<IFileDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))))
            return json::array();
        FILEOPENDIALOGOPTIONS opts;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        if (FAILED(dlg->Show(nullptr))) return json::array();
        ComPtr<IShellItem> item;
        dlg->GetResult(&item);
        LPWSTR folderW; item->GetDisplayName(SIGDN_FILESYSPATH, &folderW);
        std::wstring folder(folderW); CoTaskMemFree(folderW);

        static const std::set<std::wstring> exts = {
            L".mp4",L".webm",L".ogg",L".mkv",L".avi",L".mov",L".wmv",L".flv"
        };
        json arr = json::array();
        std::error_code ec;
        for (auto& entry : fspath::recursive_directory_iterator(folder, ec)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (exts.count(ext)) {
                arr.push_back({
                    {"path", entry.path().string()},
                    {"name", entry.path().filename().string()},
                    {"size", (int64_t)entry.file_size()}
                });
            }
        }
        return arr;
    });

    ipc_on("library.mapForPanel", [](const json& a) -> json {
        auto filePath = a.value("path", std::string{});
        if (filePath.empty()) return nullptr;
        auto wPath  = U2W(filePath);
        auto parent = fspath::path(wPath).parent_path().wstring();
        ComPtr<ICoreWebView2_3> v3;
        if (SUCCEEDED(g_panelView.As(&v3))) {
            v3->SetVirtualHostNameToFolderMapping(
                L"video.localhost", parent.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
        return fspath::path(wPath).filename().string();
    });

    ipc_on("panel.moveBy", [](const json& a) -> json {
        if (!g_panelHwnd) return false;
        int dx = a.value("x", 0), dy = a.value("y", 0);
        RECT r; GetWindowRect(g_panelHwnd, &r);
        SetWindowPos(g_panelHwnd, nullptr, r.left + dx, r.top + dy, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    });

    // Start a native resize drag. Frontend invokes this when user mousedowns
    // at a window edge — lets Windows drive the resize (correct cursors, snap,
    // aero-shake, etc.) without us needing to paint a resize border.
    ipc_on("panel.startResize", [](const json& a) -> json {
        if (!g_panelHwnd) return false;
        auto edge = a.value("edge", std::string{});
        WPARAM hit = 0;
        if      (edge == "left")         hit = HTLEFT;
        else if (edge == "right")        hit = HTRIGHT;
        else if (edge == "top")          hit = HTTOP;
        else if (edge == "bottom")       hit = HTBOTTOM;
        else if (edge == "top-left")     hit = HTTOPLEFT;
        else if (edge == "top-right")    hit = HTTOPRIGHT;
        else if (edge == "bottom-left")  hit = HTBOTTOMLEFT;
        else if (edge == "bottom-right") hit = HTBOTTOMRIGHT;
        if (!hit) return false;
        ReleaseCapture();
        PostMessageW(g_panelHwnd, WM_NCLBUTTONDOWN, hit, 0);
        return true;
    });

    ipc_on("panel.show", [](const json&) -> json {
        showPanel();
        return true;
    });

    ipc_on("panel.hide", [](const json&) -> json {
        hidePanel();
        return true;
    });

    ipc_on("panel.minimize", [](const json&) -> json {
        if (g_panelHwnd) {
            ShowWindow(g_panelHwnd, SW_MINIMIZE);
        }
        return true;
    });

    ipc_on("panel.toggle", [](const json&) -> json {
        g_panelVisible ? hidePanel() : showPanel();
        return true;
    });

    ipc_on("library.thumbnail", [](const json& a) -> json {
        auto p = a.value("path", std::string{});
        if (p.empty()) return nullptr;
        auto result = getShellThumbnail(U2W(p));
        if (result.empty()) return nullptr;
        return result;
    });
}

// ================================================================
//  Commands: Dialog (openFile only)
// ================================================================

static json show_open_file_dialog(const json& a) {
    ComPtr<IFileDialog> dlg;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg));
    if (FAILED(hr)) throw std::runtime_error("Failed to create file dialog");

    FILEOPENDIALOGOPTIONS opts;
    dlg->GetOptions(&opts);
    bool multi = a.value("multiple", false);
    if (multi) opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);

    // Filters
    std::vector<COMDLG_FILTERSPEC> specs;
    std::vector<std::wstring> names, pats;
    if (a.contains("filters") && a["filters"].is_array()) {
        for (auto& f : a["filters"]) {
            names.push_back(U2W(f.value("name", "")));
            std::string p;
            for (auto& ext : f["extensions"]) {
                if (!p.empty()) p += ";";
                auto e = ext.get<std::string>();
                p += (e == "*") ? "*.*" : ("*." + e);
            }
            pats.push_back(U2W(p));
        }
        for (size_t i = 0; i < names.size(); i++)
            specs.push_back({names[i].c_str(), pats[i].c_str()});
        dlg->SetFileTypes((UINT)specs.size(), specs.data());
    }

    // Show dialog with desktop as owner to ensure it appears on top
    if (FAILED(dlg->Show(nullptr))) return nullptr;

    if (multi) {
        ComPtr<IFileOpenDialog> od;
        dlg.As(&od);
        ComPtr<IShellItemArray> items;
        od->GetResults(&items);
        DWORD count; items->GetCount(&count);
        json arr = json::array();
        for (DWORD i = 0; i < count; i++) {
            ComPtr<IShellItem> item;
            items->GetItemAt(i, &item);
            LPWSTR path; item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            arr.push_back(W2U(path));
            CoTaskMemFree(path);
        }
        return arr;
    } else {
        ComPtr<IShellItem> item;
        dlg->GetResult(&item);
        LPWSTR path; item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        auto result = W2U(path);
        CoTaskMemFree(path);
        return result;
    }
}

static void reg_dialog() {
    ipc_on("dialog.openFile", [](const json& a) -> json {
        return show_open_file_dialog(a);
    });
}

// ================================================================
//  Commands: File system (read/write/exists only)
// ================================================================

static void reg_fs() {
    ipc_on("fs.readTextFile", [](const json& a) -> json {
        auto path = a.value("path", std::string{});
        std::ifstream f(U2W(path), std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open: " + path);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    });
    ipc_on("fs.writeTextFile", [](const json& a) -> json {
        auto path    = a.value("path", std::string{});
        auto content = a.value("content", std::string{});
        auto wpath   = U2W(path);
        auto parent  = fspath::path(wpath).parent_path();
        fspath::create_directories(parent);
        std::ofstream f(wpath, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot write: " + path);
        f.write(content.data(), content.size());
        return true;
    });
    ipc_on("fs.exists", [](const json& a) -> json {
        return fspath::exists(U2W(a.value("path", std::string{})));
    });
}

// ================================================================
//  Commands: App
// ================================================================

static void reg_app() {
    ipc_on("app.exit", [](const json& a) -> json {
        restoreDesktop();
        PostQuitMessage(a.value("code", 0));
        return true;
    });
    ipc_on("app.dataDir", [](const json&) -> json {
        return W2U(app_data_dir());
    });
    ipc_on("app.exeDir", [](const json&) -> json {
        return W2U(exe_dir());
    });
}

// ================================================================
//  Commands: System tray
// ================================================================

static void reg_tray() {
    ipc_on("tray.create", [](const json& a) -> json {
        if (g_trayActive) return true;
        g_nid.cbSize           = sizeof(g_nid);
        g_nid.hWnd             = g_msgWnd;
        g_nid.uID              = 1;
        g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        g_nid.uCallbackMessage = WM_TRAYICON;
        // Try custom icon at tray size, fallback to default
        int smCx = GetSystemMetrics(SM_CXSMICON);
        int smCy = GetSystemMetrics(SM_CYSMICON);
        g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, smCx, smCy, LR_DEFAULTCOLOR);
        if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        auto tip = U2W(a.value("tooltip", std::string{"App"}));
        wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_trayActive = true;
        return true;
    });
    ipc_on("tray.remove", [](const json&) -> json {
        if (!g_trayActive) return false;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayActive = false;
        return true;
    });
}

// ================================================================
//  Commands: Context menu
// ================================================================

static void reg_menu() {
    ipc_on("menu.popup", [](const json& a) -> json {
        if (!a.contains("items") || !a["items"].is_array()) return nullptr;
        HMENU hMenu = CreatePopupMenu();
        int idx = 1;
        for (auto& item : a["items"]) {
            if (item.is_string() && item.get<std::string>() == "-") {
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            } else if (item.is_object()) {
                auto label = U2W(item.value("label", std::string{""}));
                UINT flags = MF_STRING;
                if (item.value("disabled", false)) flags |= MF_GRAYED;
                if (item.value("checked", false))  flags |= MF_CHECKED;
                AppendMenuW(hMenu, flags, idx, label.c_str());
            }
            idx++;
        }
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(g_msgWnd);
        int cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, g_msgWnd, nullptr);
        DestroyMenu(hMenu);
        PostMessageW(g_hwnd, WM_NULL, 0, 0); // dismiss menu cleanly
        if (cmd == 0) return nullptr;
        return cmd - 1; // 0-based index
    });
}

// ================================================================
//  Commands: Global hotkeys
// ================================================================

static void reg_hotkey() {
    ipc_on("hotkey.register", [](const json& a) -> json {
        int id  = a.value("id", 0);
        int mod = a.value("modifiers", 0);
        int key = a.value("key", 0);
        bool ok = RegisterHotKey(g_msgWnd, id, mod | MOD_NOREPEAT, key);
        return ok;
    });
    ipc_on("hotkey.unregister", [](const json& a) -> json {
        return (bool)UnregisterHotKey(g_msgWnd, a.value("id", 0));
    });
}

// ================================================================
//  Commands: DevTools
// ================================================================

static void reg_devtools() {
    ipc_on("devtools.open", [](const json&) -> json {
        if (g_view) g_view->OpenDevToolsWindow();
        return true;
    });
}

// ================================================================
//  Commands: Media folder mapping (wallpaper-specific)
// ================================================================

static void reg_media() {
    ipc_on("media.mapFolder", [](const json& a) -> json {
        auto filePath = a.value("path", std::string{});
        if (filePath.empty()) return nullptr;
        auto wPath  = U2W(filePath);
        auto parent = fspath::path(wPath).parent_path().wstring();

        ComPtr<ICoreWebView2_3> v3;
        if (SUCCEEDED(g_view.As(&v3))) {
            v3->SetVirtualHostNameToFolderMapping(
                L"media.localhost", parent.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }

        auto filename = fspath::path(wPath).filename().string();
        return filename;
    });
}

// ================================================================
//  WebView2 initialization
// ================================================================

static void setupWebView(ICoreWebView2Controller* ctrl) {
    g_ctrl = ctrl;
    g_ctrl->get_CoreWebView2(&g_view);

    RECT b; GetClientRect(g_hwnd, &b);
    g_ctrl->put_Bounds(b);

    // Transparent black background
    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(g_ctrl.As(&ctrl2))) {
        ctrl2->put_DefaultBackgroundColor({255, 0, 0, 0});
    }

    // Settings
    ComPtr<ICoreWebView2Settings> s;
    g_view->get_Settings(&s);
    s->put_IsScriptEnabled(TRUE);
    s->put_AreDefaultScriptDialogsEnabled(TRUE);
    s->put_IsWebMessageEnabled(TRUE);
    bool dev = !g_devUrl.empty();
    s->put_AreDevToolsEnabled(dev ? TRUE : FALSE);
    s->put_AreDefaultContextMenusEnabled(FALSE);
    s->put_IsStatusBarEnabled(FALSE);

    // Auto-allow permissions
    g_view->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
            return S_OK;
        }).Get(), nullptr);

    // IPC handler
    g_view->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
            LPWSTR m; a->get_WebMessageAsJson(&m);
            ipc_dispatch(sender, m);
            CoTaskMemFree(m);
            return S_OK;
        }).Get(), nullptr);

    // Log navigation events
    g_view->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            COREWEBVIEW2_WEB_ERROR_STATUS status;
            args->get_WebErrorStatus(&status);
            LPWSTR uri = nullptr;
            sender->get_Source(&uri);
            auto logPath = exe_dir() + L"\\webview_debug.log";
            FILE* f = _wfopen(logPath.c_str(), L"a");
            if (f) {
                fprintf(f, "NavCompleted: success=%d, status=%d, uri=%ls\n",
                    (int)success, (int)status, uri ? uri : L"(null)");
                fclose(f);
            }
            if (uri) CoTaskMemFree(uri);
            return S_OK;
        }).Get(), nullptr);

    // Navigate
    if (dev) {
        g_view->Navigate(g_devUrl.c_str());
    } else {
#ifdef SINGLE_EXE
        if (!g_pakEntries.empty()) {
            g_view->AddWebResourceRequestedFilter(
                L"http://app.localhost/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            g_view->add_WebResourceRequested(
                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    args->get_Request(&request);
                    LPWSTR uri;
                    request->get_Uri(&uri);
                    std::wstring wUri(uri);
                    CoTaskMemFree(uri);

                    const std::wstring prefix = L"http://app.localhost/";
                    std::string path;
                    if (wUri.size() > prefix.size())
                        path = W2U(wUri.substr(prefix.size()));
                    auto qpos = path.find('?');
                    if (qpos != std::string::npos) path = path.substr(0, qpos);
                    if (path.empty()) path = "index.html";

                    auto* entry = findPakEntry(path);
                    if (!entry) return S_OK;

                    auto mime = guessMimeType(path);
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(
                        reinterpret_cast<const BYTE*>(entry->data), entry->size));
                    if (!stream) return S_OK;

                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    g_env->CreateWebResourceResponse(
                        stream.Get(), 200, L"OK",
                        (L"Content-Type: " + mime).c_str(),
                        &response);
                    args->put_Response(response.Get());
                    return S_OK;
                }).Get(), nullptr);
            g_view->Navigate(L"http://app.localhost/index.html");
        } else
#endif
        {
            auto dir = exe_dir();
            ComPtr<ICoreWebView2_3> v3;
            if (SUCCEEDED(g_view.As(&v3))) {
                v3->SetVirtualHostNameToFolderMapping(
                    L"app.localhost", dir.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                // Pre-register media.localhost (will be re-mapped when user selects video)
                v3->SetVirtualHostNameToFolderMapping(
                    L"media.localhost", dir.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            }
            g_view->Navigate(L"https://app.localhost/index.html");
        }
    }

    g_view->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            if (success && g_devUrl.empty()) {
                embedAsWallpaper();
                int cx = GetSystemMetrics(SM_CXSCREEN);
                int cy = GetSystemMetrics(SM_CYSCREEN);
                RECT rc = {0, 0, cx, cy};
                g_ctrl->put_Bounds(rc);
            }
            g_ctrl->put_IsVisible(FALSE);
            g_ctrl->put_IsVisible(TRUE);
            return S_OK;
        }).Get(), nullptr);
}

static void init_webview() {
    auto dataDir = app_data_dir();
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--disable-features=msSmartScreenProtection,RendererCodeIntegrity,msWebOOUI,msPdfOOUI"
        L" --disable-background-networking --no-proxy-server"
        L" --autoplay-policy=no-user-gesture-required");
    CreateCoreWebView2EnvironmentWithOptions(nullptr, dataDir.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr)) {
                MessageBoxW(nullptr,
                    L"WebView2 初始化失败。\n请安装 WebView2 Runtime:\nhttps://go.microsoft.com/fwlink/p/?LinkId=2124703",
                    L"错误", MB_ICONERROR);
                PostQuitMessage(1);
                return hr;
            }
            g_env = env;
            return g_env->CreateCoreWebView2Controller(g_hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                    if (FAILED(hr)) { PostQuitMessage(1); return hr; }
                    setupWebView(ctrl);
                    initPanelWebView();
                    return S_OK;
                }).Get());
        }).Get());
}

// ================================================================
//  Message window procedure (tray, hotkey, display change)
// ================================================================

static LRESULT CALLBACK MsgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TRAYICON:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
            showPanel();
            ipc_emit("tray.click");
            break;
        case WM_LBUTTONDBLCLK:
            showPanel();
            ipc_emit("tray.doubleClick");
            break;
        case WM_RBUTTONUP:    ipc_emit("tray.rightClick"); break;
        }
        return 0;
    case WM_HOTKEY:
        if ((int)w == 42) {
            g_panelVisible ? hidePanel() : showPanel();
        } else {
            ipc_emit("hotkey.triggered", {{"id", (int)w}});
        }
        return 0;
    case WM_DISPLAYCHANGE:
        // Screen resolution changed — resize wallpaper to match
        if (g_workerW) {
            RECT rc;
            GetClientRect(g_workerW, &rc);
            SetWindowPos(g_hwnd, HWND_TOP, 0, 0,
                rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================================================================
//  Wallpaper window procedure
// ================================================================

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (g_ctrl) {
            RECT b; GetClientRect(h, &b);
            g_ctrl->put_Bounds(b);
        }
        return 0;

    case WM_KEYDOWN:
        if (w == VK_F12 && !g_devUrl.empty()) {
            if (g_view) g_view->OpenDevToolsWindow();
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_panelHwnd) DestroyWindow(g_panelHwnd);
        restoreDesktop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================================================================
//  Entry point
// ================================================================

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Clean up stale mirror/WorkerW from previous crash
    cleanupStaleMirror();

    // Parse --dev <url>
    int argc;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; i++) {
        if (wcscmp(argv[i], L"--dev") == 0) { g_devUrl = argv[i + 1]; break; }
    }
    LocalFree(argv);

    // Load config
    g_cfg = loadConfig(exe_dir());
#ifdef SINGLE_EXE
    loadPak();
#endif
    auto winCfg = g_cfg.value("window", json::object());

    // Single instance lock
    bool singleInstance = winCfg.value("singleInstance", true);
    HANDLE hMutex = nullptr;
    if (singleInstance) {
        auto mutexName = U2W("KoDesktop_" + winCfg.value("title", std::string{"wallpaper"}));
        hMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex) CloseHandle(hMutex);
            CoUninitialize();
            return 0;
        }
    }

    // Window class
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hi;
    wc.lpszClassName  = L"KoDesktop";
    wc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon   = LoadIconW(hi, MAKEINTRESOURCE(IDI_APP));
    wc.hIconSm = LoadIconW(hi, MAKEINTRESOURCE(IDI_APP));
    if (!wc.hIcon) {
        wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    }
    RegisterClassExW(&wc);

    // Get primary monitor size
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Create popup window covering the screen
    g_hwnd = CreateWindowExW(0, L"KoDesktop", L"动态壁纸",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, screenW, screenH,
        nullptr, nullptr, hi, nullptr);

    // Create hidden top-level message window for tray icon & menu
    {
        WNDCLASSEXW mc{sizeof(mc)};
        mc.lpfnWndProc   = MsgWndProc;
        mc.hInstance      = hi;
        mc.lpszClassName  = L"KoDesktop_Msg";
        RegisterClassExW(&mc);
        g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"KoDesktop_Msg", L"",
            WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
        // stays hidden — no ShowWindow call
    }

    // Register global hotkey for panel toggle (Ctrl+Shift+P)
    RegisterHotKey(g_msgWnd, 42, MOD_CONTROL | MOD_SHIFT, 'P');

    // In dev mode use a normal window for debugging
    if (!g_devUrl.empty()) {
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
        int cx = GetSystemMetrics(SM_CXSCREEN);
        int cy = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_hwnd, nullptr, (cx-1280)/2, (cy-720)/2, 1280, 720,
            SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    // NOTE: wallpaper embedding is done AFTER WebView2 navigation completes
    // (matching Sucrose's flow: create → render → embed)

    // Register IPC commands
    reg_dialog();
    reg_fs();
    reg_app();
    reg_tray();
    reg_menu();
    reg_hotkey();
    reg_devtools();
    reg_media();
    reg_wallpaper_ctrl();

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // Create panel window (WebView2 created after env ready)
    createPanelWindow(hi);

    init_webview();

    // Show panel by default on launch
    showPanel();

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup — restore desktop icons
    restoreDesktop();
    if (g_trayActive) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (hMutex) CloseHandle(hMutex);
    CoUninitialize();
    return (int)msg.wParam;
}
