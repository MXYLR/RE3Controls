// Dev only: a virtual keyboard and mouse driven from the bridge (tools/kbm.py), so keyboard/mouse play can be
// tested on a machine without anyone at it; plus the switches for "no pad connected" (xinput_proxy.cpp) and for
// the game's window-focus check.
//
// The game reads the keyboard and the mouse once per frame through DirectInput (sBioMain::update 0x783AD0 calls
// sMouse::update 0x6658C0, then sKeyboard::update 0x665030, then the device switch, before any character moves).
// Both keep their state in their own objects: the keyboard as Windows VK bitmaps (On +0x120, Old +0x140,
// Trg +0x160, Rel +0x180, Chg +0x1A0, Rep +0x1C0), the mouse as deltas (+0x2C/+0x30/+0x34, already times
// MouseBaseSpeed +0x9C), buttons (On +0x38, Old +0x3C, Trg +0x40, Rel +0x44, Chg +0x48, Rep +0x4C) and the cursor
// (+0x20/+0x24, render pixels). The hooks below add the virtual input after the real read and redo the edges the
// same way the game does, so everything downstream (device switch, key queries, the PlayerPad mouse fields, menus)
// sees it as real input. Both devices are read only while the game is the active app; the virtual input is added
// regardless, so tests work while another window has the focus.
#include "common.h"
#include "re5.h"

#if RE3C_DEV
#include <limits.h>
#include "MinHook.h"

using namespace re5;

namespace {

constexpr uint32_t kFnKeyboardUpdate = 0x00665030;  // sKeyboard::update, thiscall, plain ret
constexpr uint32_t kFnMouseUpdate = 0x006658C0;     // sMouse::update, thiscall, plain ret
constexpr uint32_t kFnWindowActive = 0x0077F4B0;    // thiscall bool(sBioMain): not minimised, foreground, focused
constexpr uint32_t kRenderPtr = 0x012345D4;         // +0x50 / +0x54 render width / height

typedef void(__fastcall* PFN_Update)(uint32_t self, void* edx);
typedef bool(__fastcall* PFN_Active)(uint32_t self, void* edx);
PFN_Update oKeyboard = nullptr, oMouse = nullptr;
PFN_Active oActive = nullptr;

uint32_t Rd(uint32_t a) { return Readable((void*)a, 4) ? *(uint32_t*)a : 0; }

// Auto-skip for the test tools (RE3C_autoskip.flag next to the DLL): story scenes and the intro movies end at once.
// The game's skip input is BACK / Esc (not START): it records a per-player request in sBioNetwork+0x186C (0x7915E0,
// offline path), which every skippable scene polls through 0x785E40 (events, aGame sequences, area scripts, FSM
// camera demos). The intro movies (cDemoMovie) test their own skip input 0xBFA8E0. Offline only.
constexpr uint32_t kEventPlayerPtr = 0x011B2018;  // sEventPlayer: +0x20 u8 state (3 = playing), +0x2C row
constexpr uint32_t kBioNetworkPtr = 0x01249C1C;   // sBioNetwork: +0x186C u8 skip requests, bit = player
constexpr uint32_t kMainPtr = 0x012340A4;         // sBioMain: +0x1042C session (+0x56C online)
constexpr uint32_t kFnMovieSkip = 0x00BFA8E0;     // bool cDemoMovie skip-input test, no args, plain ret

bool AutoSkipOn() {
    static int on = -1;
    if (on < 0) {
        char p[MAX_PATH];
        wsprintfA(p, "%sRE3C_autoskip.flag", ModDir());
        on = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
        if (on) Log("dev: auto-skip of scenes and intro movies on");
    }
    return on == 1;
}

void AutoSkipTick() {
    if (!AutoSkipOn()) return;
    const uint32_t main = Rd(kMainPtr), net = Rd(kBioNetworkPtr);
    const uint32_t ses = main && Readable((void*)(main + 0x1042C), 4) ? Rd(main + 0x1042C) : 0;
    if (!net || !ses || !Readable((void*)(ses + 0x56C), 1) || At<uint8_t>(ses, 0x56C)) return;  // offline only
    if (!Readable((void*)(net + 0x186C), 1)) return;
    At<uint8_t>(net, 0x186C) |= 1;
    const uint32_t evp = Rd(kEventPlayerPtr);
    const uint8_t st = evp && Readable((void*)evp, 0x30) ? At<uint8_t>(evp, 0x20) : 0;
    static uint8_t last = 0;
    if (st == 3 && last != 3) Log("dev: auto-skip event row %u", Rd(evp + 0x2C));
    last = st;
}

typedef bool(__cdecl* PFN_MovieSkip)();
PFN_MovieSkip oMovieSkip = nullptr;
bool __cdecl HookMovieSkip() {
    const bool pressed = oMovieSkip();
    return pressed || AutoSkipOn();
}

void __fastcall HookKeyboard(uint32_t kb, void* edx) {
    oKeyboard(kb, edx);
    BridgeKbm& k = Bridge()->kbm;
    if (k.kbd_mode && Readable((void*)(kb + 0x120), 0xC0)) {
        uint32_t v[8];
        for (int i = 0; i < 8; ++i) v[i] = k.vk[i];
        auto has = [&](int vk) { return (v[vk >> 5] >> (vk & 31)) & 1u; };
        auto set = [&](int vk) { v[vk >> 5] |= 1u << (vk & 31); };
        // The game derives the generic modifiers from the left/right ones (0x664DD0).
        if (has(0xA0) || has(0xA1)) set(0x10);
        if (has(0xA2) || has(0xA3)) set(0x11);
        if (has(0xA4) || has(0xA5)) set(0x12);
        uint32_t* on = (uint32_t*)(kb + 0x120);
        const uint32_t* old = (const uint32_t*)(kb + 0x140);
        uint32_t* trg = (uint32_t*)(kb + 0x160);
        uint32_t* rel = (uint32_t*)(kb + 0x180);
        uint32_t* chg = (uint32_t*)(kb + 0x1A0);
        uint32_t* rep = (uint32_t*)(kb + 0x1C0);
        for (int i = 0; i < 8; ++i) {
            on[i] = k.kbd_mode == 1 ? v[i] : (on[i] | v[i]);
            chg[i] = on[i] ^ old[i];
            trg[i] = chg[i] & on[i];
            rel[i] = chg[i] & old[i];
            rep[i] |= trg[i];
        }
    }
    k.kbm_frame++;
    const uint32_t spad = Rd(0x01249C40);
    if (spad && Readable((void*)spad, 0x600)) {
        k.last_device = At<uint8_t>(spad, 0x5F0);
        k.input_locked = At<uint8_t>(spad, 0x5DC);
    }
    AutoSkipTick();
}

void __fastcall HookMouse(uint32_t m, void* edx) {
    oMouse(m, edx);
    BridgeKbm& k = Bridge()->kbm;
    k.applied_dx = k.applied_dy = 0;
    if (!k.mouse_mode || !Readable((void*)(m + 0x20), 0x80)) return;
    uint32_t& on = At<uint32_t>(m, 0x38);
    const uint32_t old = At<uint32_t>(m, 0x3C);
    on = k.mouse_mode == 1 ? k.mouse_buttons : (on | k.mouse_buttons);
    At<uint32_t>(m, 0x48) = on ^ old;
    At<uint32_t>(m, 0x40) = (on ^ old) & on;
    At<uint32_t>(m, 0x44) = (on ^ old) & old;
    At<uint32_t>(m, 0x4C) |= (on ^ old) & on;
    const uint32_t frames = k.mouse_frames;
    if (frames) {
        const float base = At<float>(m, 0x9C);
        k.applied_dx = (int32_t)(k.mouse_dx * base);
        k.applied_dy = (int32_t)(k.mouse_dy * base);
        At<int32_t>(m, 0x2C) += k.applied_dx;
        At<int32_t>(m, 0x30) += k.applied_dy;
        At<int32_t>(m, 0x34) += (int32_t)(k.mouse_wheel * base);
        if (frames != 0xFFFFFFFFu) k.mouse_frames = frames - 1;
    }
    // Mouse buttons count only with the cursor inside the render area (sPad+0x610, 0x79C200).
    const uint32_t render = Rd(kRenderPtr);
    if (k.cursor_x != INT32_MIN) {
        At<int32_t>(m, 0x20) = k.cursor_x;
        At<int32_t>(m, 0x24) = k.cursor_y;
    } else if (render && Readable((void*)(render + 0x50), 8)) {
        At<int32_t>(m, 0x20) = At<int32_t>(render, 0x50) / 2;
        At<int32_t>(m, 0x24) = At<int32_t>(render, 0x54) / 2;
    }
}

bool __fastcall HookActive(uint32_t self, void* edx) {
    switch (Bridge()->kbm.window_active) {
        case 1: return oActive(self, edx);
        case 2: return false;
    }
    return true;
}

bool Hook(uint32_t addr, const BYTE* sig, size_t n, void* detour, void** orig, const char* name) {
    if (!SafeEqual((void*)addr, sig, n) || MH_CreateHook((void*)addr, detour, orig) != MH_OK ||
        MH_QueueEnableHook((void*)addr) != MH_OK) {
        Log("dev: hook %s failed", name);
        return false;
    }
    return true;
}

}  // namespace

bool DevVirtualPadOn();

// Installed with the other hooks (MH_ApplyQueued in InitGameHooks).
void DevPatchWindowActive() {
    if (!DevVirtualPadOn()) return;
    BridgeKbm& k = Bridge()->kbm;
    k.cursor_x = k.cursor_y = INT32_MIN;
    static const BYTE sigActive[] = {0x80, 0xB9, 0x48, 0x05, 0x01, 0x00, 0x00};
    static const BYTE sigKb[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x8D, 0xB1, 0x20, 0x01};
    static const BYTE sigMouse[] = {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x38};
    const bool a = Hook(kFnWindowActive, sigActive, sizeof(sigActive), (void*)HookActive, (void**)&oActive, "window active");
    const bool b = Hook(kFnKeyboardUpdate, sigKb, sizeof(sigKb), (void*)HookKeyboard, (void**)&oKeyboard, "keyboard");
    const bool c = Hook(kFnMouseUpdate, sigMouse, sizeof(sigMouse), (void*)HookMouse, (void**)&oMouse, "mouse");
    static const BYTE sigMovie[] = {0x8B, 0x0D, 0xC4, 0x20, 0x1B, 0x01, 0x56, 0x6A, 0x00, 0x6A, 0x00};
    const bool m = Hook(kFnMovieSkip, sigMovie, sizeof(sigMovie), (void*)HookMovieSkip, (void**)&oMovieSkip, "movie skip");
    Log("dev: window-active check %s, virtual keyboard %s, virtual mouse %s, movie skip %s", a ? "hooked" : "-",
        b ? "on" : "-", c ? "on" : "-", m ? "hooked" : "-");
}

#endif
