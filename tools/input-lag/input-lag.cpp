// input-lag: how long a button press on a pad takes to reach the screen, in
// any program's window (ours and Supermodel's alike, the same way).
//
// A virtual Xbox 360 pad (the ViGEmBus driver, through ViGEmClient) presses a
// button at a random moment; DXGI desktop duplication watches the window's
// part of the screen, and the first desktop picture showing the button's
// effect is taken. Its time is the moment Windows' compositor put that
// picture together (DXGI_OUTDUPL_FRAME_INFO::LastPresentTime), on the same
// clock as the press (QueryPerformanceCounter). Left out: the pad's USB
// polling (none here; the same for every program with a real pad) and the
// monitor after the compositor (scan-out and the panel: PresentMon's
// "displayed" time covers the first part, tools/input-lag/README.md).
//
// The screen area to watch is found by the tool: the button is held and let
// go a few times, and the pixels that change with it (and not without it,
// such as a frame counter) make the area. The game's own test menu input
// screen suits it: the switch's ON/OFF, drawn by the game's code itself.
//
// --self-test measures the tool itself: a window of its own changes colour
// the moment XInput sees the button. That is the floor: what a program that
// did nothing but answer the pad would show.
//
// No keyboard presses are ever sent. The program's window must be the
// foreground window (a game reading its pad only while focused); the tool
// pauses while it isn't.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10 (per-monitor DPI)
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <xinput.h>

#include <ViGEm/Client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double g_qpc_ms = 0; // milliseconds per QPC tick

double ms_between(int64_t a, int64_t b) { return static_cast<double>(b - a) * g_qpc_ms; }

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// ---- The virtual pad ----

struct Pad {
    PVIGEM_CLIENT client = nullptr;
    PVIGEM_TARGET target = nullptr;
    XUSB_REPORT report{};

    bool open(std::string& error) {
        client = vigem_alloc();
        if (!client) return error = "vigem_alloc failed", false;
        if (const VIGEM_ERROR e = vigem_connect(client); !VIGEM_SUCCESS(e)) {
            char text[160];
            std::snprintf(text, sizeof text, "the ViGEmBus driver isn't there or can't be opened (error 0x%08X)", static_cast<unsigned>(e));
            return error = text, false;
        }
        target = vigem_target_x360_alloc();
        if (const VIGEM_ERROR e = vigem_target_add(client, target); !VIGEM_SUCCESS(e)) {
            char text[120];
            std::snprintf(text, sizeof text, "can't plug in the virtual pad (error 0x%08X)", static_cast<unsigned>(e));
            return error = text, false;
        }
        return true;
    }
    // Presses or lets go of `buttons`; the time just before the driver is told.
    int64_t set(USHORT buttons, bool down) {
        if (down) report.wButtons = static_cast<USHORT>(report.wButtons | buttons);
        else report.wButtons = static_cast<USHORT>(report.wButtons & ~buttons);
        const int64_t t = qpc();
        vigem_target_x360_update(client, target, report);
        return t;
    }
    ~Pad() {
        if (target) {
            report = {};
            vigem_target_x360_update(client, target, report);
            vigem_target_remove(client, target);
            vigem_target_free(target);
        }
        if (client) {
            vigem_disconnect(client);
            vigem_free(client);
        }
    }
};

struct ButtonName {
    const char* name;
    USHORT bits;
};
constexpr ButtonName kButtons[] = {
    {"a", XUSB_GAMEPAD_A},          {"b", XUSB_GAMEPAD_B},          {"x", XUSB_GAMEPAD_X},
    {"y", XUSB_GAMEPAD_Y},          {"back", XUSB_GAMEPAD_BACK},    {"start", XUSB_GAMEPAD_START},
    {"up", XUSB_GAMEPAD_DPAD_UP},   {"down", XUSB_GAMEPAD_DPAD_DOWN}, {"left", XUSB_GAMEPAD_DPAD_LEFT},
    {"right", XUSB_GAMEPAD_DPAD_RIGHT}, {"lb", XUSB_GAMEPAD_LEFT_SHOULDER}, {"rb", XUSB_GAMEPAD_RIGHT_SHOULDER},
    {"ls", XUSB_GAMEPAD_LEFT_THUMB}, {"rs", XUSB_GAMEPAD_RIGHT_THUMB}};

USHORT button_bits(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const ButtonName& b : kButtons)
        if (name == b.name) return b.bits;
    return 0;
}

// ---- Pictures ----

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px; // BGRA
};

bool same_pixel(uint32_t a, uint32_t b) {
    for (int s = 0; s < 32; s += 8) {
        const int d = static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF);
        if (d > 24 || d < -24) return false;
    }
    return true;
}

bool write_bmp(const std::string& path, const Image& img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t data = static_cast<uint32_t>(img.px.size() * 4);
    uint8_t head[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
    put32(2, 54 + data);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(img.w));
    put32(22, static_cast<uint32_t>(-img.h)); // top-down
    head[26] = 1;
    head[28] = 32;
    put32(34, data);
    f.write(reinterpret_cast<const char*>(head), sizeof head);
    f.write(reinterpret_cast<const char*>(img.px.data()), data);
    return static_cast<bool>(f);
}

// ---- Desktop duplication of one monitor, a box of it copied each picture ----

class Capture {
public:
    ~Capture() { close(); }

    bool open(HMONITOR monitor, std::string& error) {
        close();
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
            return error = "CreateDXGIFactory1 failed", false;
        IDXGIAdapter1* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            IDXGIOutput* o = nullptr;
            for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_OUTPUT_DESC d;
                o->GetDesc(&d);
                if (d.Monitor == monitor) {
                    output = o;
                    desktop_ = d.DesktopCoordinates;
                    break;
                }
                release(o);
            }
            if (!output) release(adapter);
        }
        release(factory);
        if (!output) return error = "the window's monitor isn't one DXGI knows", false;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device_,
                                       nullptr, &context_);
        release(adapter);
        if (FAILED(hr)) return release(output), error = "D3D11CreateDevice failed", false;
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        release(output);
        if (FAILED(hr)) return error = "no IDXGIOutput1", false;
        hr = output1->DuplicateOutput(device_, &dupl_);
        release(output1);
        if (FAILED(hr)) {
            char text[120];
            std::snprintf(text, sizeof text, "DuplicateOutput failed (0x%08lX)", static_cast<unsigned long>(hr));
            return error = text, false;
        }
        monitor_ = monitor;
        return true;
    }

    void close() {
        release(staging_);
        release(dupl_);
        release(context_);
        release(device_);
    }

    // The part of the screen to copy, in screen pixels.
    bool set_box(const RECT& screen, std::string& error) {
        if (screen.left < desktop_.left || screen.top < desktop_.top || screen.right > desktop_.right ||
            screen.bottom > desktop_.bottom || screen.right <= screen.left || screen.bottom <= screen.top)
            return error = "the window isn't wholly on one monitor", false;
        box_ = screen;
        release(staging_);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(screen.right - screen.left);
        d.Height = static_cast<UINT>(screen.bottom - screen.top);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging_))) return error = "staging texture failed", false;
        latest_.w = static_cast<int>(d.Width);
        latest_.h = static_cast<int>(d.Height);
        latest_.px.assign(static_cast<size_t>(d.Width) * d.Height, 0);
        have_ = false;
        return true;
    }

    // Waits up to `timeout_ms` for a new desktop picture. 1: one came (the
    // box copied into latest(), its time in `when`, the pictures it stands
    // for in `frames`); 0: none; -1: duplication lost and not reopened.
    int next(UINT timeout_ms, int64_t& when, UINT& frames) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        HRESULT hr = dupl_->AcquireNextFrame(timeout_ms, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::string error;
            const RECT box = box_;
            if (!open(monitor_, error) || !set_box(box, error)) return -1;
            ++lost_;
            return 0;
        }
        if (FAILED(hr)) return -1;
        int result = 0;
        if (info.LastPresentTime.QuadPart != 0) {
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
                D3D11_BOX b{static_cast<UINT>(box_.left - desktop_.left), static_cast<UINT>(box_.top - desktop_.top), 0,
                            static_cast<UINT>(box_.right - desktop_.left), static_cast<UINT>(box_.bottom - desktop_.top), 1};
                context_->CopySubresourceRegion(staging_, 0, 0, 0, 0, texture, 0, &b);
                release(texture);
                result = 1;
            }
            when = info.LastPresentTime.QuadPart;
            frames = info.AccumulatedFrames;
        }
        release(resource);
        dupl_->ReleaseFrame();
        if (result == 1) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &m))) return -1;
            for (int y = 0; y < latest_.h; ++y)
                std::memcpy(&latest_.px[static_cast<size_t>(y) * latest_.w], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                            static_cast<size_t>(latest_.w) * 4);
            context_->Unmap(staging_, 0);
            have_ = true;
        }
        return result;
    }

    // Takes pictures for `ms`, keeping the latest.
    bool settle(double ms) {
        const int64_t end = qpc() + static_cast<int64_t>(ms / g_qpc_ms);
        int64_t when;
        UINT frames;
        while (qpc() < end && !g_stop) {
            const double left = ms_between(qpc(), end);
            if (next(static_cast<UINT>(std::max(0.0, std::min(left, 50.0))), when, frames) < 0) return false;
        }
        return true;
    }

    const Image& latest() const { return latest_; }
    bool have() const { return have_; }
    const RECT& box() const { return box_; }
    int lost() const { return lost_; }

private:
    HMONITOR monitor_ = nullptr;
    RECT desktop_{}, box_{};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* dupl_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    Image latest_;
    bool have_ = false;
    int lost_ = 0;
};

// ---- Windows ----

std::string window_title(HWND w) {
    char text[512] = {};
    GetWindowTextA(w, text, sizeof text);
    return text;
}

bool client_on_screen(HWND w, RECT& r) {
    if (!GetClientRect(w, &r)) return false;
    POINT a{r.left, r.top}, b{r.right, r.bottom};
    ClientToScreen(w, &a);
    ClientToScreen(w, &b);
    r = {a.x, a.y, b.x, b.y};
    return true;
}

struct FindArgs {
    DWORD pid = 0;
    std::string title;
    std::vector<HWND> found;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

BOOL CALLBACK find_window(HWND w, LPARAM p) {
    auto& f = *reinterpret_cast<FindArgs*>(p);
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    if (f.pid && pid != f.pid) return TRUE;
    char cls[128] = {};
    GetClassNameA(w, cls, sizeof cls);
    if (!std::strcmp(cls, "ConsoleWindowClass") || !std::strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS")) return TRUE;
    const std::string title = window_title(w);
    if (!f.title.empty() && lower(title).find(lower(f.title)) == std::string::npos) return TRUE;
    RECT r;
    if (f.pid && (title.empty() || !GetClientRect(w, &r) || r.right < 64 || r.bottom < 64)) return TRUE;
    f.found.push_back(w);
    return TRUE;
}

// Gives `w` the focus without any key press: our thread attaches to the
// focused window's input for a moment (allowed to hand the focus on then).
void bring_forward(HWND w) {
    if (GetForegroundWindow() == w) return;
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
    if (fg && fg != me) AttachThreadInput(fg, me, TRUE);
    ShowWindow(w, SW_RESTORE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    if (fg && fg != me) AttachThreadInput(fg, me, FALSE);
}

int refresh_hz(HMONITOR m) {
    MONITORINFOEXA info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoA(m, &info)) return 0;
    DEVMODEA mode{};
    mode.dmSize = sizeof mode;
    return EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ? static_cast<int>(mode.dmDisplayFrequency) : 0;
}

// ---- The self test: a window answering XInput's button at once ----

std::atomic<HWND> g_self{nullptr};
std::atomic<bool> g_self_lit{false};
std::atomic<int64_t> g_self_seen{0}; // when XInput last showed a change

LRESULT CALLBACK self_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(g_self_lit ? WHITE_BRUSH : BLACK_BRUSH)));
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_CLOSE) return 0;
    return DefWindowProcA(w, msg, wp, lp);
}

void self_window(USHORT bits) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = self_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "InputLagSelfTest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "input-lag self test", WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE,
                             100, 100, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    g_self = w;
    while (!g_stop) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        bool down = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE s{};
            if (XInputGetState(i, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & bits)) down = true;
        }
        if (down != g_self_lit) {
            g_self_seen = qpc();
            g_self_lit = down;
            HDC dc = GetDC(w);
            RECT r;
            GetClientRect(w, &r);
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(down ? WHITE_BRUSH : BLACK_BRUSH)));
            ReleaseDC(w, dc);
            GdiFlush();
        }
        YieldProcessor();
    }
    DestroyWindow(w);
}

// ---- Results ----

struct Stats {
    std::vector<double> v;
    double at(double q) const {
        if (v.empty()) return 0;
        const double i = q * static_cast<double>(v.size() - 1);
        const size_t lo = static_cast<size_t>(i);
        const size_t hi = std::min(lo + 1, v.size() - 1);
        return v[lo] + (v[hi] - v[lo]) * (i - static_cast<double>(lo));
    }
    void print(const char* what) {
        std::sort(v.begin(), v.end());
        if (v.empty()) {
            std::printf("  %-8s no samples\n", what);
            return;
        }
        double sum = 0;
        for (double x : v) sum += x;
        const double mean = sum / static_cast<double>(v.size());
        double var = 0;
        for (double x : v) var += (x - mean) * (x - mean);
        std::printf("  %-8s n %4zu  median %6.2f  mean %6.2f  sd %5.2f  min %6.2f  p10 %6.2f  p90 %6.2f  max %6.2f ms"
                    "  (median %.2f game frames)\n",
                    what, v.size(), at(0.5), mean, std::sqrt(var / static_cast<double>(v.size())), v.front(), at(0.1), at(0.9),
                    v.back(), at(0.5) / (1000.0 / 57.524));
    }
};

void usage() {
    std::puts(
        "input-lag: a virtual pad's button to the screen, in a program's window.\n"
        "  input-lag (--window TEXT | --run \"COMMAND\" [--cwd DIR] | --self-test) [options]\n"
        "  input-lag [options] -- PROGRAM ARGS...   (as --run, each argument quoted as needed)\n"
        "Options:\n"
        "  --window TEXT        the window whose title holds TEXT (no case)\n"
        "  --run \"COMMAND\"      start the program (after the pad is plugged in), close it at the end\n"
        "  --cwd DIR            its folder\n"
        "  --self-test          the tool's own window answering XInput at once (the floor)\n"
        "  --button NAME        the button to time (a b x y back start up down left right lb rb ls rs; default a)\n"
        "  --presses N          presses to time (default 200; each let go is timed too)\n"
        "  --settle MS          wait after the window appears (default 4000)\n"
        "  --tap B[:MS],...     buttons tapped first, MS held (default 150), --tap-gap MS apart (default 600)\n"
        "  --after-tap MS       wait after the taps (default 2000)\n"
        "  --gap MIN-MAX        random wait between let go and the next press, ms (default 150-450)\n"
        "  --hold MIN-MAX       random hold after the press shows, ms (default 60-200)\n"
        "  --out FILE.csv       each sample\n"
        "  --shots PREFIX       pictures of the window let go and held (PREFIX-up.bmp, -down.bmp, -area.bmp)\n"
        "  --label TEXT         a name for the run in the output\n"
        "  --any-focus          press even when the window isn't the focused one\n"
        "  --plug-only SECONDS  only plug the pad in for that long (to set a program up)\n"
        "  --seed N             the random waits' seed (default from the clock)");
}

bool parse_range(const char* s, double& lo, double& hi) {
    char* end = nullptr;
    lo = std::strtod(s, &end);
    if (!end || *end != '-') return false;
    hi = std::strtod(end + 1, nullptr);
    return hi >= lo && lo >= 0;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_ms = 1000.0 / static_cast<double>(f.QuadPart);
    }
    std::string window_text, run, cwd, out_path, shots, label, taps;
    bool self_test = false, any_focus = false;
    std::string button = "a";
    int presses = 200;
    double settle = 4000, tap_gap = 600, after_tap = 2000, gap_lo = 150, gap_hi = 450, hold_lo = 60, hold_hi = 200, plug_only = -1;
    uint64_t seed = static_cast<uint64_t>(qpc());
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--") { // the program and its arguments, each quoted as needed
            for (++i; i < argc; ++i) {
                std::string arg = argv[i];
                if (!run.empty()) run += ' ';
                run += arg.find_first_of(" \t") == std::string::npos ? arg : "\"" + arg + "\"";
            }
            break;
        }
        if (a == "--window") window_text = value();
        else if (a == "--run") run = value();
        else if (a == "--cwd") cwd = value();
        else if (a == "--self-test") self_test = true;
        else if (a == "--button") button = value();
        else if (a == "--presses") presses = std::atoi(value());
        else if (a == "--settle") settle = std::atof(value());
        else if (a == "--tap") taps = value();
        else if (a == "--tap-gap") tap_gap = std::atof(value());
        else if (a == "--after-tap") after_tap = std::atof(value());
        else if (a == "--gap") {
            if (!parse_range(value(), gap_lo, gap_hi)) return std::fprintf(stderr, "--gap MIN-MAX\n"), 2;
        } else if (a == "--hold") {
            if (!parse_range(value(), hold_lo, hold_hi)) return std::fprintf(stderr, "--hold MIN-MAX\n"), 2;
        } else if (a == "--out") out_path = value();
        else if (a == "--shots") shots = value();
        else if (a == "--label") label = value();
        else if (a == "--any-focus") any_focus = true;
        else if (a == "--plug-only") plug_only = std::atof(value());
        else if (a == "--seed") seed = std::strtoull(value(), nullptr, 0);
        else if (a == "--help" || a == "-h") return usage(), 0;
        else return std::fprintf(stderr, "unknown option %s (--help)\n", a.c_str()), 2;
    }
    const USHORT bits = button_bits(button);
    if (!bits) return std::fprintf(stderr, "unknown button %s\n", button.c_str()), 2;
    if (plug_only < 0 && window_text.empty() && run.empty() && !self_test) return usage(), 2;

    Pad pad;
    std::string error;
    if (!pad.open(error)) return std::fprintf(stderr, "input-lag: %s\n", error.c_str()), 1;
    std::printf("input-lag: virtual pad plugged in\n");
    std::fflush(stdout);
    if (plug_only >= 0) {
        const int64_t end = qpc() + static_cast<int64_t>(plug_only * 1000.0 / g_qpc_ms);
        while (!g_stop && qpc() < end) Sleep(50);
        return 0;
    }
    Sleep(1000); // (XInput and programs see a new pad)

    PROCESS_INFORMATION proc{};
    HWND target = nullptr;
    std::thread self;
    if (self_test) {
        any_focus = true; // (XInput reads the pad whatever has the focus)
        self = std::thread(self_window, bits);
        while (!g_self) Sleep(1);
        target = g_self;
        settle = std::min(settle, 1000.0);
    } else {
        if (!run.empty()) {
            STARTUPINFOA si{};
            si.cb = sizeof si;
            std::vector<char> line(run.begin(), run.end());
            line.push_back(0);
            if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si,
                                &proc))
                return std::fprintf(stderr, "input-lag: can't start %s (error %lu)\n", run.c_str(), GetLastError()), 1;
        }
        FindArgs f;
        f.pid = proc.dwProcessId;
        f.title = window_text;
        for (int tries = 0; tries < 1200 && !g_stop; ++tries) { // up to 2 minutes
            f.found.clear();
            EnumWindows(find_window, reinterpret_cast<LPARAM>(&f));
            if (!f.found.empty()) break;
            if (proc.hProcess && WaitForSingleObject(proc.hProcess, 0) == WAIT_OBJECT_0)
                return std::fprintf(stderr, "input-lag: the program ended before its window came\n"), 1;
            Sleep(100);
        }
        if (f.found.empty()) return std::fprintf(stderr, "input-lag: no such window\n"), 1;
        if (f.found.size() > 1 && proc.dwProcessId == 0) {
            std::fprintf(stderr, "input-lag: %zu windows match:\n", f.found.size());
            for (HWND w : f.found) std::fprintf(stderr, "  %s\n", window_title(w).c_str());
            return 1;
        }
        target = f.found.front();
    }
    auto finish = [&](int code) {
        g_stop = true;
        if (self.joinable()) self.join();
        if (proc.hProcess) {
            if (target && IsWindow(target)) PostMessageA(target, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(proc.hProcess, 10000) != WAIT_OBJECT_0) TerminateProcess(proc.hProcess, 1);
            CloseHandle(proc.hProcess);
            CloseHandle(proc.hThread);
        }
        return code;
    };

    std::printf("input-lag: window \"%s\"\n", window_title(target).c_str());
    if (!self_test) {
        SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        bring_forward(target);
    }
    {
        const int64_t end = qpc() + static_cast<int64_t>(settle / g_qpc_ms);
        while (!g_stop && qpc() < end) Sleep(20);
    }

    auto focused = [&] { return any_focus || GetForegroundWindow() == target; };
    // Waits for the window to be the focused one (never taking the focus
    // from the person at the PC once the run is going). False: gave up.
    auto wait_focus = [&] {
        if (focused()) return true;
        std::printf("input-lag: the window isn't the focused one: waiting (click it to go on)\n");
        std::fflush(stdout);
        for (int i = 0; i < 1200 && !g_stop; ++i) { // 2 minutes
            if (focused()) return true;
            Sleep(100);
        }
        return false;
    };

    // The taps (a test menu to reach, say).
    if (!taps.empty()) {
        for (size_t at = 0; at < taps.size() && !g_stop;) {
            const size_t comma = std::min(taps.find(',', at), taps.size());
            std::string one = taps.substr(at, comma - at);
            at = comma + 1;
            double hold = 150;
            if (const size_t colon = one.find(':'); colon != std::string::npos) {
                hold = std::atof(one.c_str() + colon + 1);
                one.resize(colon);
            }
            const USHORT b = button_bits(one);
            if (!b) return finish((std::fprintf(stderr, "unknown button %s in --tap\n", one.c_str()), 2));
            if (!wait_focus()) return finish((std::fprintf(stderr, "input-lag: never focused\n"), 1));
            pad.set(b, true);
            Sleep(static_cast<DWORD>(hold));
            pad.set(b, false);
            Sleep(static_cast<DWORD>(tap_gap));
        }
        Sleep(static_cast<DWORD>(after_tap));
    }

    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    Capture cap;
    RECT client;
    if (!client_on_screen(target, client)) return finish((std::fprintf(stderr, "input-lag: no client area\n"), 1));
    if (!cap.open(monitor, error) || !cap.set_box(client, error))
        return finish((std::fprintf(stderr, "input-lag: %s\n", error.c_str()), 1));
    const int hz = refresh_hz(monitor);
    std::printf("input-lag: client area %ldx%ld at (%ld,%ld), monitor %d Hz\n", client.right - client.left, client.bottom - client.top,
                client.left, client.top, hz);
    std::fflush(stdout);

    // Finding the area that shows the button: held and let go three times.
    if (!wait_focus()) return finish((std::fprintf(stderr, "input-lag: never focused\n"), 1));
    cap.settle(600);
    if (!cap.have()) return finish((std::fprintf(stderr, "input-lag: no picture of the screen came\n"), 1));
    std::vector<Image> ups{cap.latest()}, downs;
    for (int i = 0; i < 3 && !g_stop; ++i) {
        if (!wait_focus()) return finish(1);
        pad.set(bits, true);
        cap.settle(700);
        downs.push_back(cap.latest());
        pad.set(bits, false);
        cap.settle(700);
        ups.push_back(cap.latest());
    }
    if (g_stop) return finish(1);
    const Image& up_full = ups.back();
    const Image& down_full = downs.back();
    const int W = up_full.w, H = up_full.h;
    std::vector<uint8_t> mask(static_cast<size_t>(W) * H, 0);
    int left = W, top = H, right = -1, bottom = -1;
    size_t marked = 0, noise = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            bool changes = true, steady = true;
            for (size_t k = 0; k < downs.size(); ++k) changes = changes && !same_pixel(downs[k].px[i], ups[k + 1].px[i]);
            for (size_t k = 1; k < ups.size(); ++k) steady = steady && same_pixel(ups[k].px[i], ups[0].px[i]);
            for (size_t k = 1; k < downs.size(); ++k) steady = steady && same_pixel(downs[k].px[i], downs[0].px[i]);
            if (!steady) ++noise;
            if (changes && steady) {
                mask[i] = 1;
                ++marked;
                left = std::min(left, x), right = std::max(right, x);
                top = std::min(top, y), bottom = std::max(bottom, y);
            }
        }
    if (!shots.empty()) {
        write_bmp(shots + "-up.bmp", up_full);
        write_bmp(shots + "-down.bmp", down_full);
        Image area = up_full;
        for (size_t i = 0; i < area.px.size(); ++i)
            if (mask[i]) area.px[i] = 0xFFFF0000; // red
        write_bmp(shots + "-area.bmp", area);
    }
    if (!marked)
        return finish((std::fprintf(stderr, "input-lag: the button changes nothing in the window (%zu pixels changed by themselves)\n",
                                    noise),
                       1));
    std::printf("input-lag: the button shows in %zu pixels, box %dx%d at (%d,%d) of the window; %zu pixels change by themselves\n",
                marked, right - left + 1, bottom - top + 1, left, top, noise);
    // The box only, from now on.
    const int bw = right - left + 1, bh = bottom - top + 1;
    std::vector<uint8_t> bmask(static_cast<size_t>(bw) * bh);
    std::vector<uint32_t> up_ref(bmask.size()), down_ref(bmask.size());
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t s = static_cast<size_t>(y + top) * W + (x + left), d = static_cast<size_t>(y) * bw + x;
            bmask[d] = mask[s], up_ref[d] = up_full.px[s], down_ref[d] = down_full.px[s];
        }
    const RECT box{client.left + left, client.top + top, client.left + right + 1, client.top + bottom + 1};
    if (!cap.set_box(box, error)) return finish((std::fprintf(stderr, "input-lag: %s\n", error.c_str()), 1));
    auto shows = [&](const std::vector<uint32_t>& ref) {
        const Image& img = cap.latest();
        size_t same = 0;
        for (size_t i = 0; i < bmask.size(); ++i)
            if (bmask[i] && same_pixel(img.px[i], ref[i])) ++same;
        return same * 10 >= marked * 6; // 60% of the area
    };

    std::FILE* out = out_path.empty() ? nullptr : std::fopen(out_path.c_str(), "w");
    if (out) std::fprintf(out, "label,n,kind,ms,frames_in_picture\n");
    std::mt19937_64 rng(seed);
    auto random_ms = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    Stats press_ms, release_ms, xinput_ms; // (xinput_ms: the self test's pad to XInput part)
    int misses = 0, merged = 0;
    // Waits for the area to show `ref`: its picture's time, or 0 (none in `ms`).
    auto wait_for = [&](const std::vector<uint32_t>& ref, int64_t since, double ms, UINT& frames) -> int64_t {
        const int64_t end = since + static_cast<int64_t>(ms / g_qpc_ms);
        int64_t when = 0;
        while (!g_stop && qpc() < end) {
            const int got = cap.next(static_cast<UINT>(std::max(1.0, std::min(ms_between(qpc(), end), 50.0))), when, frames);
            if (got < 0) return -1;
            if (got == 1 && shows(ref)) return when;
        }
        return 0;
    };
    RECT now_client;
    std::printf("input-lag: timing %d presses of %s%s%s\n", presses, button.c_str(), label.empty() ? "" : ", run ",
                label.c_str());
    std::fflush(stdout);
    UINT frames = 0;
    for (int n = 0; n < presses && !g_stop; ++n) {
        if (!client_on_screen(target, now_client) || std::memcmp(&now_client, &client, sizeof client) != 0) {
            std::fprintf(stderr, "input-lag: the window moved or closed: stopped\n");
            break;
        }
        if (!cap.settle(random_ms(gap_lo, gap_hi))) break;
        if (!shows(up_ref)) { // (not let go yet: wait for it)
            if (wait_for(up_ref, qpc(), 1000, frames) <= 0) {
                std::fprintf(stderr, "input-lag: the area doesn't show the button let go: stopped\n");
                break;
            }
            --n; // (this turn timed nothing)
            continue;
        }
        if (!wait_focus()) break;
        const int64_t pressed = pad.set(bits, true);
        const int64_t seen = wait_for(down_ref, pressed, 1000, frames);
        if (seen < 0) break;
        if (seen == 0) {
            ++misses;
            pad.set(bits, false);
            continue;
        }
        if (frames > 1) ++merged;
        const double d = ms_between(pressed, seen);
        press_ms.v.push_back(d);
        if (self_test) xinput_ms.v.push_back(ms_between(pressed, g_self_seen));
        if (out) std::fprintf(out, "%s,%d,press,%.3f,%u\n", label.c_str(), n, d, frames);
        if (!cap.settle(random_ms(hold_lo, hold_hi))) break;
        const int64_t let_go = pad.set(bits, false);
        const int64_t gone = wait_for(up_ref, let_go, 1000, frames);
        if (gone < 0) break;
        if (gone == 0) {
            ++misses;
            continue;
        }
        if (frames > 1) ++merged;
        const double r = ms_between(let_go, gone);
        release_ms.v.push_back(r);
        if (self_test) xinput_ms.v.push_back(ms_between(let_go, g_self_seen));
        if (out) std::fprintf(out, "%s,%d,release,%.3f,%u\n", label.c_str(), n, r, frames);
        if ((n + 1) % 25 == 0) {
            std::printf("  %d presses\n", n + 1);
            std::fflush(stdout);
        }
    }
    pad.set(bits, false);
    if (out) std::fclose(out);

    std::printf("input-lag results%s%s (button to the compositor's picture; monitor %d Hz; game frame %.2f ms):\n",
                label.empty() ? "" : ": ", label.c_str(), hz, 1000.0 / 57.524);
    Stats both;
    both.v = press_ms.v;
    both.v.insert(both.v.end(), release_ms.v.begin(), release_ms.v.end());
    press_ms.print("press");
    release_ms.print("let go");
    both.print("both");
    if (self_test) xinput_ms.print("xinput"); // (of those: the pad to XInput seeing it)
    std::printf("  missed %d (no change within 1 s), pictures standing for several %d, duplication restarted %d\n", misses, merged,
                cap.lost());
    return finish(0);
}
