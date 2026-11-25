#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <shellapi.h>

#pragma comment(linker, "/ENTRY:DllMainCRTStartup")
#pragma comment(linker, "/SUBSYSTEM:CONSOLE")

// offsets - Fortnite 43.11
enum : uint64_t {
    O_GEngine = 0x1C14F058, O_GNames = 0x1BFFC240, O_GWorld = 0x1C14D610,
    O_Viewport = 0xB70, O_World = 0x78,
    O_Levels = 0x1E8, O_Actors = 0x208, O_GameState = 0x1D0, O_GameInstance = 0x248,
    O_LocalPlayers = 0x38, O_Ctrl = 0x30, O_Pawn = 0x320, O_FovScale = 0x37C,
    O_CamLoc = 0x170, O_CamRot = 0x180, O_PlayerArray = 0x290,
    O_PawnPrivate = 0x2F0, O_Team = 0xF81, O_Dying = 0x770, O_PlayerState = 0x298,
    O_Root = 0x1B0, O_Loc = 0x140, O_Vel = 0x188,
    O_Name = 0x8, O_Class = 0x20, O_Searched = 0xD2A
};

struct V3 { float x, y, z; };
static uint64_t base, world, gstate, lpawn, ctrl;
static int team = -1;
static V3 cam{}; static float cyaw, cpitch, cfov = 90.f;
static bool chestESP = true, playerESP = true, autoFarm = false, overlay = true;

static uint64_t rp(uint64_t a) { uint64_t v = 0; __try { v = *(uint64_t*)a; } __except (1) {} return v; }
template <class T> static T rd(uint64_t a) { T v{}; __try { v = *(T*)a; } __except (1) {} return v; }
template <class T> static void wr(uint64_t a, T v) { __try { *(T*)a = v; } __except (1) {} }
static bool scopy(void* d, uint64_t s, size_t n) {
    __try { memcpy(d, (void*)s, n); } __except (1) { return false; } return true;
}
static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static uint64_t rotl64(uint64_t v, int n) { return (v << n) | (v >> (64 - n)); }

static uint32_t dName(uint32_t e) {
    if (!e) return 0;
    uint32_t v = (rotl32(((e - 1u) ^ 0xB9C081u), 2) ^ 0x9575403Au) * 0x8EC51E6Du;
    return (v + 1u) ? v + 1u : 0x667B692Eu;
}
static void dBytes(uint8_t* b, size_t n, uint32_t len) {
    uint32_t p = len * 0x2188u;
    uint32_t s = ((p + 0x4895A8u) >> 18) ^ (0xFFB76A50u - p);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = pass; i < n; i += 2) {
            uint8_t r = (uint8_t)(b[i] ^ 0xBBu);
            r = (uint8_t)((r << 6) | (r >> 2));
            b[i] = (uint8_t)((r ^ (uint8_t)s) * 0x35u);
            uint32_t m = s * 0x2188u + 0x4895AFu;
            s = ~(m ^ (m >> 18));
        }
}
static std::wstring nameOf(uint64_t obj) {
    std::wstring out;
    if (!obj) return out;
    uint32_t idx = dName(rd<uint32_t>(obj + O_Name));
    if (!idx) return out;
    uint64_t pool = rp(base + O_GNames);
    if (!pool) return out;
    uint64_t blk = rp(pool + 0x10 + (uint64_t)(idx >> 16) * 8);
    if (!blk) return out;
    uint64_t e = blk + (uint64_t)(idx & 0xFFFF) * 2;
    uint16_t h = rd<uint16_t>(e);
    int len = h >> 6;
    if ((h & 1) || len <= 0 || len > 64) return out;
    uint8_t b[66]{};
    if (!scopy(b, e + 2, len)) return out;
    dBytes(b, len, len);
    for (int i = 0; i < len && b[i]; ++i) out.push_back((wchar_t)b[i]);
    return out;
}

static std::unordered_map<uint64_t, bool> cache;
static std::vector<V3> players, chests;
static float nearestD = 1e9f; static V3 nearestP{};

static void resolve() {
    uint64_t eng = rp(base + O_GEngine);
    if (!eng) { world = 0; return; }
    uint64_t vp = rp(eng + O_Viewport);
    if (!vp) { world = 0; return; }
    world = rp(vp + O_World);
    if (!world) world = (rotl64(((rp(base + O_GWorld)) ^ 0xC2A8FFull), 16) ^ 0xF5136F13ull) * 0x4F207DBE8FB1498Dull;
    if (!world) return;
    gstate = rp(world + O_GameState);
    uint64_t gi = rp(world + O_GameInstance);
    ctrl = gi ? rp(rp(gi + O_LocalPlayers) + O_Ctrl) : 0;
    lpawn = ctrl ? rp(ctrl + O_Pawn) : 0;
    team = lpawn ? rd<uint8_t>(rp(lpawn + O_PlayerState) + O_Team) : -1;
    uint64_t cl = rp(world + O_CamLoc), cr = rp(world + O_CamRot);
    if (cl) cam = rd<V3>(cl);
    if (cr) { V3 rot = rd<V3>(cr); cpitch = rot.x; cyaw = rot.y; }
    float s = ctrl ? rd<float>(ctrl + O_FovScale) : 1.f;
    cfov = (s > 0.4f && s < 2.f) ? s * 90.f : 90.f;
}

static V3 posOf(uint64_t a) { uint64_t r = rp(a + O_Root); return r ? rd<V3>(r + O_Loc) : V3{}; }
static float dist(V3 p) {
    float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void scan() {
    players.clear(); chests.clear();
    nearestD = 1e9f; nearestP = V3{};
    if (!world || !lpawn) return;

    if (playerESP && gstate) {
        uint64_t arr = rp(gstate + O_PlayerArray);
        int n = rd<int>(gstate + O_PlayerArray + 8);
        if (arr && n > 0 && n < 200)
            for (int i = 0; i < n; ++i) {
                uint64_t ps = rp(arr + (uint64_t)i * 8);
                uint64_t p = ps ? rp(ps + O_PawnPrivate) : 0;
                if (!p || p == lpawn) continue;
                if ((rd<uint8_t>(p + O_Dying) >> 5) & 1) continue;
                if (team >= 0 && rd<uint8_t>(ps + O_Team) == team) continue;
                players.push_back(posOf(p));
            }
    }

    if (!chestESP && !autoFarm) return;
    uint64_t lv = rp(world + O_Levels);
    int ln = rd<int>(world + O_Levels + 8);
    if (!lv || ln <= 0 || ln > 4096) return;
    for (int l = 0; l < ln; ++l) {
        uint64_t lvl = rp(lv + (uint64_t)l * 8);
        uint64_t acts = lvl ? rp(lvl + O_Actors) : 0;
        int an = lvl ? rd<int>(lvl + O_Actors + 8) : 0;
        if (!acts || an <= 0 || an > 300000) continue;
        for (int a = 0; a < an; ++a) {
            uint64_t act = rp(acts + (uint64_t)a * 8);
            if (!act) continue;
            auto it = cache.find(act);
            bool isChest;
            if (it != cache.end()) isChest = it->second;
            else {
                std::wstring n = nameOf(rp(act + O_Class));
                isChest = n.find(L"Tiered_Chest") != std::wstring::npos ||
                          n.find(L"TreasureChest") != std::wstring::npos;
                if (cache.size() > 200000) cache.clear();
                cache[act] = isChest;
            }
            if (!isChest) continue;
            if ((rd<uint8_t>(act + O_Searched) >> 4) & 1) continue;
            V3 p = posOf(act);
            float d = dist(p) / 100.f;
            if (d > 300.f) continue;
            chests.push_back(p);
            if (d < nearestD) { nearestD = d; nearestP = p; }
        }
    }
}

static bool w2s(V3 p, int W, int H, int& sx, int& sy) {
    float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    const float D = 3.14159265f / 180.f;
    float sp = sinf(cpitch * D), cp = cosf(cpitch * D), syw = sinf(cyaw * D), cyw = cosf(cyaw * D);
    float fx = cp * cyw, fy = cp * syw, fz = sp, rx = -syw, ry = cyw;
    float ux = fy - fz * ry, uy = fz * rx - fx, uz = fx * ry - fy * rx;
    float z = dx * fx + dy * fy + dz * fz;
    if (z < 1.f) return false;
    float sc = (W * 0.5f) / tanf(cfov * 0.5f * D);
    sx = (int)(W * 0.5f + (dx * rx + dy * ry) / z * sc);
    sy = (int)(H * 0.5f - (dx * ux + dy * uy + dz * uz) / z * sc);
    return true;
}

static void tpTo(V3 p) {
    uint64_t r = lpawn ? rp(lpawn + O_Root) : 0;
    if (!r) return;
    p.z += 20.f;
    wr<V3>(r + O_Loc, p);
    wr<V3>(r + O_Vel, V3{ 0, 0, 0 });
}

// ---------------------------------------------------------------- overlay
static HWND ovl, gwnd;
static HDC mem; static HBITMAP bmp; static int W, H; static HFONT font;

static HWND gameWindow() {
    struct C { DWORD pid; HWND h; } c{ GetCurrentProcessId(), 0 };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        C* c = (C*)l; DWORD p = 0; GetWindowThreadProcessId(h, &p);
        if (p != c->pid) return TRUE;
        wchar_t n[64]{}; GetClassNameW(h, n, 63);
        if (!wcscmp(n, L"UnrealWindow")) { c->h = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.h;
}

static void render() {
    if (!gwnd) return;
    RECT rc{}; GetClientRect(gwnd, &rc);
    POINT pt{ 0,0 }; ClientToScreen(gwnd, &pt);
    int w = rc.right, h = rc.bottom;
    if (w < 16 || h < 16) { ShowWindow(ovl, SW_HIDE); return; }

    if (w != W || h != H || !mem) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        HDC sdc = GetDC(0);
        mem = CreateCompatibleDC(sdc);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits; bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, 0, 0);
        SelectObject(mem, bmp);
        W = w; H = h;
    }

    HBRUSH bg = CreateSolidBrush(RGB(1, 1, 1));
    RECT full{ 0, 0, W, H };
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    SelectObject(mem, font);
    SelectObject(mem, (HBRUSH)GetStockObject(NULL_BRUSH));
    wchar_t s[160];

    if (overlay) {
        swprintf_s(s, L"INS hide | F1 chests[%s] | F2 players[%s] | F3 farm[%s] | F4 tp",
            chestESP ? L"on" : L"off", playerESP ? L"on" : L"off", autoFarm ? L"on" : L"off");
        SetTextColor(mem, RGB(90, 220, 255));
        TextOutW(mem, 14, 14, s, (int)wcslen(s));
        swprintf_s(s, L"nearest chest %.0fm", nearestD > 1e8f ? 0.f : nearestD);
        SetTextColor(mem, RGB(255, 205, 60));
        TextOutW(mem, 14, 34, s, (int)wcslen(s));
    }

    HPEN red = CreatePen(PS_SOLID, 2, RGB(255, 70, 70));
    HPEN gold = CreatePen(PS_SOLID, 2, RGB(255, 205, 60));

    if (playerESP) {
        SelectObject(mem, red);
        for (auto& p : players) {
            int sx, sy, hx, hy;
            if (!w2s(p, W, H, sx, sy)) continue;
            V3 head = p; head.z += 62.f;
            if (!w2s(head, W, H, hx, hy)) continue;
            int hh = (sy > hy ? sy - hy : hy - sy) * 2 + 8, ww = hh / 2;
            if (hh < 5) continue;
            Rectangle(mem, sx - ww / 2, hy, sx + ww / 2, hy + hh);
        }
    }
    if (chestESP) {
        SelectObject(mem, gold);
        for (auto& p : chests) {
            int sx, sy;
            if (!w2s(p, W, H, sx, sy)) continue;
            Rectangle(mem, sx - 5, sy - 5, sx + 5, sy + 5);
            swprintf_s(s, L"%.0fm", dist(p) / 100.f);
            SetTextColor(mem, RGB(255, 205, 60));
            TextOutW(mem, sx + 8, sy - 6, s, (int)wcslen(s));
        }
    }

    HDC dc = GetDC(ovl);
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    ReleaseDC(ovl, dc);
    DeleteObject(red); DeleteObject(gold);
}

// ---------------------------------------------------------------- thread
static DWORD WINAPI run(LPVOID) {
    base = (uint64_t)GetModuleHandleW(L"FortniteClient-Win64-Shipping.exe");
    if (!base) base = (uint64_t)GetModuleHandleW(0);
    font = CreateFontW(15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"VNOVL";
    RegisterClassExW(&wc);
    ovl = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"VNOVL", L"vn", WS_POPUP, 0, 0, 640, 480, 0, 0, wc.hInstance, 0);
    SetLayeredWindowAttributes(ovl, RGB(1, 1, 1), 255, LWA_COLORKEY);
    ShowWindow(ovl, SW_SHOWNOACTIVATE);

    DWORD lastTp = 0;
    static bool k[5]{};
    for (;;) {
        Sleep(10);
        bool now[5] = {
            (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0, (GetAsyncKeyState(VK_F1) & 0x8000) != 0,
            (GetAsyncKeyState(VK_F2) & 0x8000) != 0, (GetAsyncKeyState(VK_F3) & 0x8000) != 0,
            (GetAsyncKeyState(VK_F4) & 0x8000) != 0 };
        if (now[0] && !k[0]) overlay = !overlay;
        if (now[1] && !k[1]) chestESP = !chestESP;
        if (now[2] && !k[2]) playerESP = !playerESP;
        if (now[3] && !k[3]) autoFarm = !autoFarm;
        if (now[4] && !k[4]) tpTo(nearestP);
        for (int i = 0; i < 5; ++i) k[i] = now[i];

        if (!gwnd || !IsWindow(gwnd)) gwnd = gameWindow();
        if (!gwnd || GetForegroundWindow() != gwnd) { ShowWindow(ovl, SW_HIDE); continue; }

        RECT rc{}; GetClientRect(gwnd, &rc);
        POINT pt{ 0,0 }; ClientToScreen(gwnd, &pt);
        SetWindowPos(ovl, HWND_TOPMOST, pt.x, pt.y, rc.right, rc.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);

        resolve();
        scan();
        if (autoFarm && nearestD < 300.f && GetTickCount() - lastTp > 400) {
            tpTo(nearestP);
            lastTp = GetTickCount();
        }
        render();
    }
}

// ---------------------------------------------------------------- injector
static DWORD findPid(const wchar_t* nm) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{}; e.dwSize = sizeof(e);
    DWORD pid = 0;
    if (Process32FirstW(s, &e)) do {
        if (!_wcsicmp(e.szExeFile, nm)) { pid = e.th32ProcessID; break; }
    } while (Process32NextW(s, &e));
    CloseHandle(s);
    return pid;
}

static int inject() {
    printf("VeniceFN - Fortnite 43.11\n");
    DWORD pid = findPid(L"FortniteClient-Win64-Shipping.exe");
    if (!pid) {
        printf("launching Fortnite...\n");
        ShellExecuteW(0, L"open",
            L"com.epicgames.launcher://apps/fn%3A4fe75bbc5a674f4f9b356b5c90567da5%3AFortnite?action=launch&silent=true",
            0, 0, SW_SHOWNORMAL);
        for (int i = 0; i < 240 && !pid; ++i) { Sleep(1000); pid = findPid(L"FortniteClient-Win64-Shipping.exe"); }
    }
    if (!pid) { printf("game not found\n"); getchar(); return 1; }

    wchar_t self[MAX_PATH]{}, tmp[MAX_PATH]{}, dll[MAX_PATH]{};
    GetModuleFileNameW(GetModuleHandleW(0), self, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(dll, L"%sVN_%lu.dll", tmp, GetTickCount());
    if (!CopyFileW(self, dll, FALSE)) { printf("copy failed\n"); getchar(); return 1; }

    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { printf("OpenProcess failed (%lu) - run as admin\n", GetLastError()); getchar(); return 1; }

    size_t n = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* rem = VirtualAllocEx(h, 0, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem || !WriteProcessMemory(h, rem, dll, n, 0)) { printf("write failed\n"); getchar(); return 1; }

    HANDLE th = CreateRemoteThread(h, 0, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"), rem, 0, 0);
    if (!th) { printf("thread failed (%lu) - run as admin\n", GetLastError()); getchar(); return 1; }
    WaitForSingleObject(th, 15000);
    DWORD code = 0; GetExitCodeThread(th, &code);
    if (!code) { printf("LoadLibrary failed in game\n"); getchar(); return 1; }

    printf("injected. INS = hide text, F1 chests, F2 players, F3 farm, F4 tp\n");
    Sleep(4000);
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        wchar_t p[MAX_PATH]{};
        GetModuleFileNameW(hInst, p, MAX_PATH);
        size_t n = wcslen(p);
        bool isDll = n > 4 && !_wcsicmp(p + n - 4, L".dll");
        if (isDll) CreateThread(0, 0, run, 0, 0, 0);
        else inject();
    }
    return TRUE;
}
