// xinput1_3.dll proxy: forwards to the system XInput and lets the mod rewrite pad 0 before the game sees it.
#include "common.h"

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XState*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, void*);
typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, void*);
typedef void(WINAPI* PFN_Enable)(BOOL);
typedef DWORD(WINAPI* PFN_GetBattery)(DWORD, BYTE, void*);
typedef DWORD(WINAPI* PFN_GetKeystroke)(DWORD, DWORD, void*);
typedef DWORD(WINAPI* PFN_WaitGuide)(DWORD, DWORD, void*);
typedef DWORD(WINAPI* PFN_DwordOnly)(DWORD);

static struct {
    HMODULE mod;
    PFN_GetState getState, getStateEx;
    PFN_SetState setState;
    PFN_GetCaps getCaps;
    PFN_Enable enable;
    PFN_GetBattery getBattery;
    PFN_GetKeystroke getKeystroke;
    PFN_WaitGuide waitGuide;
    PFN_DwordOnly cancelGuide, powerOff;
} R;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    const char* candidates[] = {"XInput1_4.dll", "XInput9_1_0.dll"};
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    for (const char* name : candidates) {
        char path[MAX_PATH];
        wsprintfA(path, "%s\\%s", sys, name);
        R.mod = LoadLibraryA(path);
        if (R.mod) {
            Log("xinput: forwarding to %s", path);
            break;
        }
    }
    if (!R.mod) {
        Log("xinput: no system XInput found");
        return TRUE;
    }
    R.getState = (PFN_GetState)GetProcAddress(R.mod, "XInputGetState");
    R.getStateEx = (PFN_GetState)GetProcAddress(R.mod, (LPCSTR)100);
    R.setState = (PFN_SetState)GetProcAddress(R.mod, "XInputSetState");
    R.getCaps = (PFN_GetCaps)GetProcAddress(R.mod, "XInputGetCapabilities");
    R.enable = (PFN_Enable)GetProcAddress(R.mod, "XInputEnable");
    R.getBattery = (PFN_GetBattery)GetProcAddress(R.mod, "XInputGetBatteryInformation");
    R.getKeystroke = (PFN_GetKeystroke)GetProcAddress(R.mod, "XInputGetKeystroke");
    R.waitGuide = (PFN_WaitGuide)GetProcAddress(R.mod, (LPCSTR)101);
    R.cancelGuide = (PFN_DwordOnly)GetProcAddress(R.mod, (LPCSTR)102);
    R.powerOff = (PFN_DwordOnly)GetProcAddress(R.mod, (LPCSTR)103);
    return TRUE;
}

static void EnsureReal() { InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr); }

static volatile LONG g_xinputCalls = 0;
LONG XInputCallCount() { return g_xinputCalls; }

void InitXInputProxy() {}

#if RE3C_DEV
// Dev only: with RE3C_dev.flag next to the DLL, pad 0 always reports as connected so the
// tools can drive the game through the virtual pad without a physical controller.
static bool DevVirtualPad() {
    static int cached = -1;
    if (cached < 0) {
        char path[MAX_PATH];
        wsprintfA(path, "%sRE3C_dev.flag", ModDir());
        cached = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        if (cached) Log("dev: virtual pad 0 always connected");
    }
    return cached == 1;
}


bool DevVirtualPadOn() { return DevVirtualPad(); }
#endif

// Hook point for the controls layer (input.cpp). Default: pass-through.
void TransformPad(DWORD user, XGamepad& pad);

static DWORD GetStateCommon(PFN_GetState fn, DWORD user, XState* st) {
    EnsureReal();
    DWORD rc = fn ? fn(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (user != 0 || !st) return rc;
#if RE3C_DEV
    BridgeShared* b = Bridge();
    if (b->kbm.pad_disconnect) {  // dev: test keyboard-only play
        b->last_real = b->last_out = XGamepad{};
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (rc != ERROR_SUCCESS && DevVirtualPad()) {
        st->Gamepad = XGamepad{};
        st->dwPacketNumber = 0;
        rc = ERROR_SUCCESS;
    }
    if (rc == ERROR_SUCCESS) b->last_real = st->Gamepad;
    else b->last_real = XGamepad{};
    if (b->vpad_enabled == 1) {
        st->Gamepad = b->vpad;
        st->dwPacketNumber = b->vpad_seq;
        rc = ERROR_SUCCESS;
    } else if (b->vpad_enabled == 2) {
        if (rc != ERROR_SUCCESS) { st->Gamepad = XGamepad{}; st->dwPacketNumber = 0; }
        XGamepad& g = st->Gamepad;
        g.wButtons |= b->vpad.wButtons;
        if (b->vpad.bLeftTrigger) g.bLeftTrigger = b->vpad.bLeftTrigger;
        if (b->vpad.bRightTrigger) g.bRightTrigger = b->vpad.bRightTrigger;
        if (b->vpad.sThumbLX || b->vpad.sThumbLY) { g.sThumbLX = b->vpad.sThumbLX; g.sThumbLY = b->vpad.sThumbLY; }
        if (b->vpad.sThumbRX || b->vpad.sThumbRY) { g.sThumbRX = b->vpad.sThumbRX; g.sThumbRY = b->vpad.sThumbRY; }
        st->dwPacketNumber += b->vpad_seq;
        rc = ERROR_SUCCESS;
    }
    if (rc == ERROR_SUCCESS) TransformPad(user, st->Gamepad);
    b->last_out = (rc == ERROR_SUCCESS) ? st->Gamepad : XGamepad{};
    b->mod_frame++;
#else
    if (rc == ERROR_SUCCESS) TransformPad(user, st->Gamepad);
#endif
    return rc;
}

// The Steam overlay detours the exported XInputGetState of whatever xinput1_3.dll is loaded (Steam Input),
// so the game's import slots are pointed at these internal entry points instead. They call the system
// XInput (which Steam may hook too, so Steam Input keeps working) and then apply the mod's controls layer.
static DWORD WINAPI Game_XInputGetState(DWORD user, XState* st) {
    EnsureReal();
    return GetStateCommon(R.getState, user, st);
}
static DWORD WINAPI Game_XInputGetCapabilities(DWORD user, DWORD flags, void* caps);

void RedirectGameXInputImports() {
    struct Slot { DWORD va; void* fn; const char* name; } slots[] = {
        {0x00F4A40C, (void*)Game_XInputGetState, "XInputGetState"},
        {0x00F4A408, (void*)Game_XInputGetCapabilities, "XInputGetCapabilities"},
    };
    // The Steam stub fills the import table after it decrypts the code, so wait for the slots to be set,
    // patch, and re-check a few times in case the stub writes them again.
    for (int round = 0; round < 40; ++round) {
        bool allOurs = true;
        for (auto& s : slots) {
            void** p = (void**)s.va;
            if (!Readable(p, 4) || *p == nullptr) { allOurs = false; continue; }
            if (*p == s.fn) continue;
            DWORD old;
            VirtualProtect(p, 4, PAGE_READWRITE, &old);  // an import slot is data
            void* prev = *p;
            *p = s.fn;
            VirtualProtect(p, 4, old, &old);
            Log("xinput: import %s %p -> %p (round %d)", s.name, prev, s.fn, round);
        }
        if (allOurs && round >= 20) break;
        Sleep(100);
    }
}

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD user, XState* st) {
    InterlockedIncrement(&g_xinputCalls);
    EnsureReal();
    return GetStateCommon(R.getState, user, st);
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD user, XState* st) {
    EnsureReal();
    return GetStateCommon(R.getStateEx ? R.getStateEx : R.getState, user, st);
}

DWORD WINAPI Proxy_XInputSetState(DWORD user, void* vib) {
    EnsureReal();
    return R.setState ? R.setState(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
    InterlockedIncrement(&g_xinputCalls);
    return Game_XInputGetCapabilities(user, flags, caps);
}

}  // extern "C"

static DWORD WINAPI Game_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
    EnsureReal();
    DWORD rc = R.getCaps ? R.getCaps(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
#if RE3C_DEV
    if (user <= 1 && Bridge()->kbm.pad_disconnect) return ERROR_DEVICE_NOT_CONNECTED;
    if (rc != ERROR_SUCCESS && user == 0 && (Bridge()->vpad_enabled || DevVirtualPad()) && caps) {
        // Pretend a standard gamepad is connected so the virtual pad works without hardware.
        BYTE* c = (BYTE*)caps;
        ZeroMemory(c, 20);
        c[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
        c[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
        *(WORD*)(c + 4) = 0xF3FF;
        c[6] = c[7] = 0xFF;
        *(SHORT*)(c + 8) = *(SHORT*)(c + 10) = *(SHORT*)(c + 12) = *(SHORT*)(c + 14) = (SHORT)0xFFC0;
        *(WORD*)(c + 16) = *(WORD*)(c + 18) = 0xFF;
        rc = ERROR_SUCCESS;
    }
#endif
    return rc;
}

extern "C" {

void WINAPI Proxy_XInputEnable(BOOL enable) {
    EnsureReal();
    if (R.enable) R.enable(enable);
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD, GUID* render, GUID* capture) {
    if (render) ZeroMemory(render, sizeof(GUID));
    if (capture) ZeroMemory(capture, sizeof(GUID));
    return ERROR_SUCCESS;
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE type, void* info) {
    EnsureReal();
    return R.getBattery ? R.getBattery(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, void* ks) {
    EnsureReal();
    return R.getKeystroke ? R.getKeystroke(user, reserved, ks) : ERROR_EMPTY;
}

DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD user, DWORD flags, void* listen) {
    EnsureReal();
    return R.waitGuide ? R.waitGuide(user, flags, listen) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD user) {
    EnsureReal();
    return R.cancelGuide ? R.cancelGuide(user) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputPowerOffController(DWORD user) {
    EnsureReal();
    return R.powerOff ? R.powerOff(user) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"
