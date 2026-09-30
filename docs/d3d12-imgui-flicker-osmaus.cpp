// Thema 97 Schritt 8: real OS mouse sweep (SendInput) over the D3D12 editor.
// Moves only, never clicks. Aborts at once on any REAL (non-injected) mouse/keyboard
// event, or when the window under the cursor stops belonging to the editor process.
// Usage: t97mouse.exe <editorPid> <editorLog> <seconds> [--dry] [--minidle=S]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

static std::atomic<int> g_abort{0};        // 0 ok, 1 real mouse, 2 real key, 3 window, 4 done
static std::atomic<long> g_realEvents{0};
static DWORD g_editorPid = 0;

static LRESULT CALLBACK MouseLL(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        if (!(m->flags & LLMHF_INJECTED)) { ++g_realEvents; int z = 0; g_abort.compare_exchange_strong(z, 1); }
    }
    return CallNextHookEx(nullptr, code, w, l);
}
static LRESULT CALLBACK KeyLL(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        if (!(k->flags & LLKHF_INJECTED)) { ++g_realEvents; int z = 0; g_abort.compare_exchange_strong(z, 2); }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

struct Rect { double x, y, w, h; std::string name; };
struct Layout { bool ok = false; double mx = 0, my = 0, dw = 0, dh = 0; std::vector<Rect> wins; };

static Layout ReadLayout(const char* path)
{
    Layout L;
    std::ifstream f(path);
    std::string line;
    std::vector<Rect> cur;
    while (std::getline(f, line)) {
        size_t p;
        if ((p = line.find("T97WIN main pos=(")) != std::string::npos) {
            double a, b, c, d, e, g;
            if (sscanf_s(line.c_str() + p, "T97WIN main pos=(%lf,%lf) size=(%lf,%lf) display=(%lf,%lf)", &a, &b, &c, &d, &e, &g) == 6) {
                L.mx = a; L.my = b; L.dw = e; L.dh = g; L.ok = true; cur.clear();
            }
        } else if ((p = line.find("T97WIN win rect=(")) != std::string::npos) {
            Rect r;
            if (sscanf_s(line.c_str() + p, "T97WIN win rect=(%lf,%lf,%lf,%lf)", &r.x, &r.y, &r.w, &r.h) == 4) {
                size_t n = line.find("name=", p);
                r.name = n != std::string::npos ? line.substr(n + 5) : "";
                while (!r.name.empty() && (r.name.back() == '\r' || r.name.back() == '\n')) r.name.pop_back();
                cur.push_back(r);
            }
        }
        if (L.ok) L.wins = cur;
    }
    return L;
}

struct FindCtx { DWORD pid; HWND best; };
static BOOL CALLBACK EnumMain(HWND h, LPARAM lp)
{
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
    wchar_t t[256] = {}; GetWindowTextW(h, t, 255);
    if (wcsncmp(t, L"Horizon Engine", 14) == 0 && wcschr(t, L'\x2014')) { c->best = h; return FALSE; }
    return TRUE;
}

static bool OwnedByEditor(POINT pt)
{
    HWND h = WindowFromPoint(pt);
    if (!h) return false;
    DWORD pid = 0; GetWindowThreadProcessId(GetAncestor(h, GA_ROOT), &pid);
    return pid == g_editorPid;
}

static void SendAbs(int x, int y)
{
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(((x - vx) * 65535.0) / (vw - 1) + 0.5);
    in.mi.dy = static_cast<LONG>(((y - vy) * 65535.0) / (vh - 1) + 0.5);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

int main(int argc, char** argv)
{
    if (argc < 4) { std::puts("usage: t97mouse <pid> <log> <secs> [--dry] [--minidle=S]"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_editorPid = static_cast<DWORD>(std::atoi(argv[1]));
    const char* log = argv[2];
    const double secs = std::atof(argv[3]);
    bool dry = false; double minIdle = 0;
    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dry")) dry = true;
        else if (!std::strncmp(argv[i], "--minidle=", 10)) minIdle = std::atof(argv[i] + 10);
    }

    // 1) editor main window (not the splash) + a published layout
    FindCtx fc{g_editorPid, nullptr};
    Layout L;
    for (int i = 0; i < 1200 && (!fc.best || !L.ok || L.wins.empty()); ++i) {
        fc.best = nullptr; EnumWindows(EnumMain, reinterpret_cast<LPARAM>(&fc));
        L = ReadLayout(log);
        Sleep(100);
    }
    if (!fc.best || !L.ok) { std::printf("RESULT fail=no-editor-window-or-layout hwnd=%p layout=%d\n", (void*)fc.best, (int)L.ok); return 3; }
    RECT cr; GetClientRect(fc.best, &cr);
    POINT org{0, 0}; ClientToScreen(fc.best, &org);
    const double sx = cr.right / L.dw, sy = cr.bottom / L.dh;
    std::printf("editor hwnd=%p client=%ldx%ld at (%ld,%ld) imguiDisplay=%.0fx%.0f imguiMain=(%.0f,%.0f) scale=%.3f,%.3f\n",
        (void*)fc.best, cr.right, cr.bottom, org.x, org.y, L.dw, L.dh, L.mx, L.my, sx, sy);

    // 2) targets: the step's four areas, by ImGui window name
    // The toolbar is the strip along the top of the "Scene" window (ViewportToolbar).
    const char* want[] = {"Scene", "Quick", "Outliner", "Content Browser", "##MainMenuBar", "##EditorTabBar"};
    std::vector<RECT> targets;
    for (Rect r : L.wins) {
        bool hit = false;
        for (const char* w : want) if (r.name.find(w) != std::string::npos) hit = true;
        if (!hit || r.name.find("DockSpace") != std::string::npos) continue;
        if (r.name == "Scene") { r.h = 64; r.name = "Scene toolbar strip"; }
        RECT s;
        s.left = org.x + static_cast<LONG>((r.x - L.mx) * sx) + 4;
        s.top = org.y + static_cast<LONG>((r.y - L.my) * sy) + 4;
        s.right = org.x + static_cast<LONG>((r.x - L.mx + r.w) * sx) - 4;
        s.bottom = org.y + static_cast<LONG>((r.y - L.my + r.h) * sy) - 4;
        if (s.right - s.left < 16 || s.bottom - s.top < 8) continue;
        targets.push_back(s);
        std::printf("target '%s' screen=(%ld,%ld)-(%ld,%ld)\n", r.name.c_str(), s.left, s.top, s.right, s.bottom);
    }
    // always add the whole client area as a last target (zigzag across everything)
    targets.push_back(RECT{org.x + 4, org.y + 4, org.x + cr.right - 4, org.y + cr.bottom - 4});
    std::printf("targets=%zu (last = whole client)\n", targets.size());
    if (dry) { std::puts("RESULT dry-run"); return 0; }

    // 3) precondition: nobody touched the machine recently
    LASTINPUTINFO li{sizeof(li)}; GetLastInputInfo(&li);
    const double idle = (GetTickCount() - li.dwTime) / 1000.0;
    if (idle < minIdle) { std::printf("RESULT fail=human-active idle=%.1f minidle=%.0f\n", idle, minIdle); return 4; }

    POINT orig; GetCursorPos(&orig);
    std::thread hooks([] {
        HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, MouseLL, GetModuleHandleW(nullptr), 0);
        HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, KeyLL, GetModuleHandleW(nullptr), 0);
        MSG m;
        while (g_abort.load() == 0) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        }
        UnhookWindowsHookEx(hm); UnhookWindowsHookEx(hk);
    });
    Sleep(200);

    // 4) foreground: best effort; the per-move WindowFromPoint check is what guards us
    SetForegroundWindow(fc.best);
    Sleep(300);
    const bool fg = GetForegroundWindow() == fc.best;

    timeBeginPeriod(1);
    long long moves = 0, notEditor = 0;
    const DWORD t0 = GetTickCount();
    const int speed = 7;   // px per ~1 ms step: ~4000-7000 px/s, a fast flick
    size_t ti = 0;
    int cx = orig.x, cy = orig.y;
    while (g_abort.load() == 0 && (GetTickCount() - t0) < secs * 1000.0) {
        const RECT& r = targets[ti % targets.size()];
        ++ti;
        // zigzag rows 23 px apart across this target, then fly to the next one
        std::vector<POINT> path;
        path.push_back(POINT{r.left, r.top});
        bool ltr = true;
        for (LONG y = r.top; y <= r.bottom; y += 23) {
            path.push_back(POINT{ltr ? r.left : r.right, y});
            path.push_back(POINT{ltr ? r.right : r.left, y});
            ltr = !ltr;
        }
        for (const POINT& goal : path) {
            if (g_abort.load() != 0) break;
            const double dx = goal.x - cx, dy = goal.y - cy;
            const int n = static_cast<int>(std::max(1.0, std::sqrt(dx * dx + dy * dy) / speed));
            const int x0 = cx, y0 = cy;
            for (int s = 1; s <= n && g_abort.load() == 0; ++s) {
                cx = x0 + static_cast<int>(dx * s / n); cy = y0 + static_cast<int>(dy * s / n);
                POINT pt{cx, cy};
                if (!OwnedByEditor(pt)) {
                    if (++notEditor > 20) { int z = 0; g_abort.compare_exchange_strong(z, 3); break; }
                    continue;  // don't hover someone else's window
                }
                SendAbs(cx, cy);
                ++moves;
                Sleep(1);
            }
        }
    }
    const int reason = g_abort.exchange(4);
    hooks.join();
    timeEndPeriod(1);
    SetCursorPos(orig.x, orig.y);
    const char* why[] = {"completed", "real-mouse", "real-key", "not-editor-window", "completed"};
    std::printf("RESULT %s secs=%.1f moves=%lld notEditorSkips=%lld realEvents=%ld foreground=%d\n",
        why[reason], (GetTickCount() - t0) / 1000.0, moves, notEditor, g_realEvents.load(), (int)fg);
    return reason == 0 ? 0 : 10 + reason;
}

