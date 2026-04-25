#include "pch.h"
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// DS2 (original DX9, 32-bit) Configurable Camera
//
// Patches the original Dark Souls II (pre-SotFS) executable to apply a custom
// FOV, camera distance, camera height, and follow rate; optionally disable
// the camera's auto-rotation while walking and the camera's pitch
// auto-centering; optionally fix the double-click input delay; and optionally
// run the game in a borderless window sized to the monitor.
//
// All toggles are hot-reloadable: edit the config file in game and changes
// apply on the next config-file save.
//
// Original DS2 ships with Steamstub packing: the executable is decrypted into
// a VirtualAlloc region whose base address is randomized. We use AOB scans
// for everything that touches game code so the mod survives game updates.
// ============================================================================

HANDLE modThread = nullptr;
bool   threadRunning = true;

// ---------- Config ----------
float customFOV           = 0.768f;
float cameraDistance      = 3.6f;
float customHeight        = 1.42f;
float followRate          = 0.065f;
bool  disableAutoRotation = false;
bool  doubleClickFix      = false;
bool  borderlessEnabled   = false;
int   borderlessYOffset   = 0;

// ---------- Camera struct layout (identical to SotFS) ----------
#define FINGERPRINT_OFFSET  0xD00
#define FOV_OFFSET          0xD34
#define HEIGHT_OFFSET       0xD40
#define DISTANCE_OFFSET     0xD48
#define FOLLOW_RATE_OFFSET  0xD5C

// 24 bytes at struct+0xD00 = { 1.5, 10.0, 3.0, 0.1, 0.1, 0.8 }
static const unsigned char CAMERA_FINGERPRINT[24] = {
    0x00,0x00,0xC0,0x3F, 0x00,0x00,0x20,0x41, 0x00,0x00,0x40,0x40,
    0xCD,0xCC,0xCC,0x3D, 0xCD,0xCC,0xCC,0x3D, 0xCD,0xCC,0x4C,0x3F
};

// Cutscene defaults — we skip structs that match these.
static bool IsCutsceneStructValues(float fov, float h, float dist) {
    return (fov  > 0.784f && fov  < 0.787f)
        && (h    > 1.71f  && h    < 1.73f)
        && (dist > 4.99f  && dist < 5.01f);
}

// ---------- Runtime state ----------
FILETIME lastConfigWrite = { 0, 0 };
HMODULE  g_selfModule = NULL;

struct PatchSite {
    DWORD addr;
    unsigned char orig[8];
    unsigned char patched[8];
    int len;
};

// Auto-rotation patches.
std::vector<PatchSite> autoRotPatches;
bool autoRotPatchesPopulated = false;
bool autoRotPatchesApplied   = false;
bool lastAutoRotationDisabled = false;

// Double-click-fix patches.
std::vector<PatchSite> doubleClickPatches;
bool doubleClickPatchesPopulated = false;
bool doubleClickApplied = false;
bool lastDoubleClickFix = false;

// Camera struct list.
std::vector<DWORD> cameraStructs;
DWORD lastFullScan = 0;

// ============================================================================
// Filesystem / config
// ============================================================================

static std::string GetDllDir() {
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(g_selfModule, path, MAX_PATH);
    std::string s = path;
    size_t slash = s.find_last_of("\\");
    if (slash != std::string::npos) s = s.substr(0, slash + 1);
    return s;
}

static std::string GetConfigPath() {
    return GetDllDir() + "DS2Configurator.txt";
}

static bool HasConfigChanged() {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(GetConfigPath().c_str(), GetFileExInfoStandard, &d)) {
        if (CompareFileTime(&d.ftLastWriteTime, &lastConfigWrite) != 0) {
            lastConfigWrite = d.ftLastWriteTime;
            return true;
        }
    }
    return false;
}

static void LoadConfig() {
    std::ifstream cfg(GetConfigPath());
    if (!cfg.is_open()) return;
    std::string line;
    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t"));
        if (!s.empty()) s.erase(s.find_last_not_of(" \t\r\n") + 1);
    };
    while (std::getline(cfg, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        trim(k); trim(v);
        try {
            if      (k == "FOV")              customFOV      = std::stof(v);
            else if (k == "Distance")         cameraDistance = std::stof(v);
            else if (k == "Height")           customHeight   = std::stof(v);
            else if (k == "Follow Rate")      followRate     = std::stof(v);
            else if (k == "Camera Auto Rotation")
                disableAutoRotation = (v == "False" || v == "false");
            else if (k == "Double Click Fix")
                doubleClickFix = (v == "True" || v == "true");
            else if (k == "Borderless")
                borderlessEnabled = (v == "True" || v == "true");
            else if (k == "Y Offset")
                borderlessYOffset = std::stoi(v);
        } catch (...) {}
    }
}

// ============================================================================
// Safe memory helpers
// ============================================================================

static bool SafeRead(const void* addr, void* out, size_t n) {
    __try { memcpy(out, addr, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeWriteFloat(void* addr, float v) {
    __try { *(float*)addr = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteCode(void* addr, const void* data, size_t n) {
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        memcpy(addr, data, n);
        ok = memcmp(addr, data, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    VirtualProtect(addr, n, old, &old);
    return ok;
}

#define EXEC_MASK  (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
#define WRITE_MASK (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)

template <typename F>
static void ScanMemory(DWORD protMask, const unsigned char* pat, size_t patSize, size_t stride, F onHit) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    char* p    = (char*)si.lpMinimumApplicationAddress;
    char* pend = (char*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < pend) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool scanOk = mbi.State == MEM_COMMIT
                   && (mbi.Protect & protMask)
                   && !(mbi.Protect & PAGE_GUARD)
                   && !(mbi.Protect & PAGE_NOACCESS);
        if (scanOk) {
            char* rs = (char*)mbi.BaseAddress;
            char* re = rs + mbi.RegionSize - patSize;
            __try {
                for (char* c = rs; c <= re; c += stride) {
                    if (memcmp(c, pat, patSize) == 0) {
                        if (!onHit((DWORD)c)) return;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (char*)mbi.BaseAddress + mbi.RegionSize;
    }
}

// ============================================================================
// Auto-rotation patches
//
// Four patterns to neutralize so the camera stops rotating to follow the
// walking direction and stops auto-centering pitch:
//
//   movss [reg+0x6CC], xmm     F3 0F 11 86 CC 06 00 00   -> 8 NOPs
//   movss [reg+0x6D0], xmm     F3 0F 11 86 D0 06 00 00   -> 8 NOPs
//   fstp  [esi+0x690]           D9 9E 90 06 00 00         -> fstp st(0) + nops
//   fstp  [esi+0x68C]  near X   D9 9E 8C 06 00 00         -> fstp st(0) + nops
//
// The fstp variants must be replaced with `fstp st(0)` (DD D8) instead of
// NOPed: the fstp pops one value off the FPU stack, and replacing with NOPs
// would leave that value on the stack and corrupt later FPU code.
//
// The `fstp [esi+0x68C]` instruction has a fourth instance far from the
// 0x690 sites that belongs to a different code path; NOPing it crashes the
// game. We only patch instances within 128 bytes of a 0x690 site.
// ============================================================================

static const unsigned char NOP8[8]      = { 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
static const unsigned char FSTP_POP6[6] = { 0xDD,0xD8,0x90,0x90,0x90,0x90 };

static void CollectAutoRotPatches() {
    if (autoRotPatchesPopulated) return;

    static const unsigned char MOVSS_6CC[8] = { 0xF3,0x0F,0x11,0x86,0xCC,0x06,0x00,0x00 };
    static const unsigned char MOVSS_6D0[8] = { 0xF3,0x0F,0x11,0x86,0xD0,0x06,0x00,0x00 };
    ScanMemory(EXEC_MASK, MOVSS_6CC, 8, 1, [&](DWORD a) -> bool {
        PatchSite p; p.addr = a; p.len = 8;
        memcpy(p.orig, MOVSS_6CC, 8); memcpy(p.patched, NOP8, 8);
        autoRotPatches.push_back(p); return true;
    });
    ScanMemory(EXEC_MASK, MOVSS_6D0, 8, 1, [&](DWORD a) -> bool {
        PatchSite p; p.addr = a; p.len = 8;
        memcpy(p.orig, MOVSS_6D0, 8); memcpy(p.patched, NOP8, 8);
        autoRotPatches.push_back(p); return true;
    });

    static const unsigned char FSTP_X[6] = { 0xD9, 0x9E, 0x90, 0x06, 0x00, 0x00 };
    std::vector<DWORD> xSites;
    ScanMemory(EXEC_MASK, FSTP_X, 6, 1, [&](DWORD a) -> bool {
        xSites.push_back(a);
        PatchSite p; p.addr = a; p.len = 6;
        memcpy(p.orig, FSTP_X, 6); memcpy(p.patched, FSTP_POP6, 6);
        autoRotPatches.push_back(p); return true;
    });

    static const unsigned char FSTP_Y[6] = { 0xD9, 0x9E, 0x8C, 0x06, 0x00, 0x00 };
    ScanMemory(EXEC_MASK, FSTP_Y, 6, 1, [&](DWORD a) -> bool {
        int bestDelta = 0x7FFFFFFF;
        for (DWORD x : xSites) {
            int d = (int)a - (int)x; if (d < 0) d = -d;
            if (d < bestDelta) bestDelta = d;
        }
        if (bestDelta <= 128) {
            PatchSite p; p.addr = a; p.len = 6;
            memcpy(p.orig, FSTP_Y, 6); memcpy(p.patched, FSTP_POP6, 6);
            autoRotPatches.push_back(p);
        }
        return true;
    });

    autoRotPatchesPopulated = true;
}

static void ApplyAutoRotPatches() {
    bool stateChanged = (lastAutoRotationDisabled != disableAutoRotation);
    lastAutoRotationDisabled = disableAutoRotation;
    if (disableAutoRotation && (!autoRotPatchesApplied || stateChanged)) {
        for (const PatchSite& p : autoRotPatches) WriteCode((void*)p.addr, p.patched, p.len);
        autoRotPatchesApplied = true;
    } else if (!disableAutoRotation && autoRotPatchesApplied) {
        for (const PatchSite& p : autoRotPatches) WriteCode((void*)p.addr, p.orig, p.len);
        autoRotPatchesApplied = false;
    }
}

// ============================================================================
// Double-Click Fix
//
// The buggy delayed-attack input handler is gated by a `cmp` against a flag
// byte followed by a `jne` to the fixed path. We patch the jne to an
// unconditional jmp so the fixed path always runs.
//
// 24-byte AOB (uniquely identifies the click handler — there's a sibling
// site that matches the first 15 bytes but lives in a different state path
// and would crash if patched):
//
//   mov ecx,[esi+0x1F8]     8B 8E F8 01 00 00
//   cmp [ecx+0x39], dl      38 51 39
//   jne +0xFB               0F 85 FB 00 00 00      <- patched to jmp +0xFC
//   mov cl,[esi+0x268]      8A 8E 68 02 00 00
//   test cl, 1              F6 C1 01               <- distinguishes from sibling
//
// Patch (6 bytes replacing the jne): jmp +0xFC + nop = E9 FC 00 00 00 90.
// The +0xFC compensates for jmp's 5-byte length vs jne's 6-byte length so
// they land at the same target.
// ============================================================================

static void CollectDoubleClickPatches() {
    if (doubleClickPatchesPopulated) return;
    static const unsigned char DC_AOB[24] = {
        0x8B, 0x8E, 0xF8, 0x01, 0x00, 0x00,
        0x38, 0x51, 0x39,
        0x0F, 0x85, 0xFB, 0x00, 0x00, 0x00,
        0x8A, 0x8E, 0x68, 0x02, 0x00, 0x00,
        0xF6, 0xC1, 0x01
    };
    static const unsigned char DC_PATCHED[6] = { 0xE9, 0xFC, 0x00, 0x00, 0x00, 0x90 };
    ScanMemory(EXEC_MASK, DC_AOB, 24, 1, [&](DWORD a) -> bool {
        PatchSite p;
        p.addr = a + 9;            // start of jne
        p.len = 6;
        memcpy(p.orig, DC_AOB + 9, 6);
        memcpy(p.patched, DC_PATCHED, 6);
        doubleClickPatches.push_back(p);
        return true;
    });
    doubleClickPatchesPopulated = true;
}

static void ApplyDoubleClickFix() {
    bool stateChanged = (lastDoubleClickFix != doubleClickFix);
    lastDoubleClickFix = doubleClickFix;
    if (doubleClickFix && (!doubleClickApplied || stateChanged)) {
        for (const PatchSite& p : doubleClickPatches) WriteCode((void*)p.addr, p.patched, p.len);
        doubleClickApplied = true;
    } else if (!doubleClickFix && doubleClickApplied) {
        for (const PatchSite& p : doubleClickPatches) WriteCode((void*)p.addr, p.orig, p.len);
        doubleClickApplied = false;
    }
}

// ============================================================================
// Borderless windowed mode
// ============================================================================

static HWND FindGameWindow() {
    HWND best = nullptr;
    int bestArea = 0;
    HWND hwnd = GetTopWindow(NULL);
    while (hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
            RECT r;
            if (GetWindowRect(hwnd, &r)) {
                int w = r.right - r.left, h = r.bottom - r.top;
                int area = w * h;
                if (w >= 800 && h >= 600 && area > bestArea) {
                    bestArea = area; best = hwnd;
                }
            }
        }
        hwnd = GetNextWindow(hwnd, GW_HWNDNEXT);
    }
    return best;
}

static void ApplyBorderless() {
    if (!borderlessEnabled) return;
    HWND hwnd = FindGameWindow();
    if (!hwnd) return;
    if (!SendMessageTimeoutA(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 100, nullptr)) return;
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if (style & WS_CAPTION) {
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
        SetWindowLong(hwnd, GWL_STYLE, style);
    }
    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)) {
        ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        SetWindowLong(hwnd, GWL_EXSTYLE, ex);
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    int x = 0, y = borderlessYOffset, w = 1920, h = 1080;
    if (GetMonitorInfoW(mon, &mi)) {
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top + borderlessYOffset;
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    if (GetForegroundWindow() == hwnd) {
        SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
}

// ============================================================================
// Camera struct discovery / per-tick writes
// ============================================================================

static bool IsCutsceneStruct(DWORD s) {
    float fov, h, dist;
    if (!SafeRead((void*)(s + FOV_OFFSET),      &fov,  4)) return true;
    if (!SafeRead((void*)(s + HEIGHT_OFFSET),   &h,    4)) return true;
    if (!SafeRead((void*)(s + DISTANCE_OFFSET), &dist, 4)) return true;
    return IsCutsceneStructValues(fov, h, dist);
}

static bool FingerprintIntact(DWORD s) {
    unsigned char buf[sizeof(CAMERA_FINGERPRINT)];
    if (!SafeRead((void*)(s + FINGERPRINT_OFFSET), buf, sizeof(buf))) return false;
    return memcmp(buf, CAMERA_FINGERPRINT, sizeof(buf)) == 0;
}

static bool IsRealCameraStruct(DWORD s) {
    float v;
    auto check = [&](DWORD off, float expected) -> bool {
        if (!SafeRead((void*)(s + off), &v, 4)) return false;
        return (v > expected - 0.005f && v < expected + 0.005f);
    };
    return check(0xD4C, 0.3f)
        && check(0xD54, 0.99f)
        && check(0xD68, 0.7f)
        && check(0xD6C, 0.9f)
        && check(0xD74, 1.0f);
}

static void RescanCameraStructs() {
    cameraStructs.clear();
    ScanMemory(WRITE_MASK, CAMERA_FINGERPRINT, sizeof(CAMERA_FINGERPRINT), 4,
        [](DWORD hit) -> bool {
            DWORD s = hit - FINGERPRINT_OFFSET;
            float probe;
            if (!SafeRead((void*)(s + FOLLOW_RATE_OFFSET), &probe, 4)) return true;
            if (!IsRealCameraStruct(s)) return true;
            if (IsCutsceneStruct(s))    return true;
            cameraStructs.push_back(s);
            return true;
        });
    lastFullScan = GetTickCount();
}

static void WriteCameraValues() {
    for (auto it = cameraStructs.begin(); it != cameraStructs.end(); ) {
        if (!FingerprintIntact(*it)) { it = cameraStructs.erase(it); continue; }
        SafeWriteFloat((void*)(*it + FOV_OFFSET),         customFOV);
        SafeWriteFloat((void*)(*it + HEIGHT_OFFSET),      customHeight);
        SafeWriteFloat((void*)(*it + DISTANCE_OFFSET),    cameraDistance);
        SafeWriteFloat((void*)(*it + FOLLOW_RATE_OFFSET), followRate);
        ++it;
    }
    DWORD now = GetTickCount();
    if (cameraStructs.empty() || (now - lastFullScan) > 10000) {
        RescanCameraStructs();
    }
}

// ============================================================================
// Main thread
// ============================================================================

DWORD WINAPI MonitorThreadProc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    // Wait for Steamstub decryption and game init.
    Sleep(5000);

    LoadConfig();
    CollectAutoRotPatches();
    CollectDoubleClickPatches();
    ApplyAutoRotPatches();
    ApplyDoubleClickFix();

    while (threadRunning) {
        if (HasConfigChanged()) {
            LoadConfig();
            ApplyAutoRotPatches();
            ApplyDoubleClickFix();
        }
        WriteCameraValues();
        ApplyBorderless();
        Sleep(50);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        g_selfModule = hinstDLL;
        threadRunning = true;
        modThread = CreateThread(NULL, 0, MonitorThreadProc, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        if (modThread) {
            threadRunning = false;
            WaitForSingleObject(modThread, 1000);
            CloseHandle(modThread);
        }
        break;
    }
    return TRUE;
}
