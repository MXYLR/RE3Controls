// Balance and visual options: fire rate, walking/running speed outside the aim, the yellow colour filter.
//
// Fire rate. Every player gunshot runs one fire action (cPlAction_pl_act_weapon_fire, update 0xC89F30). It counts
// 60 Hz frames at action+0x20 (and seconds at +0x1C) and allows the next shot when the count reaches the weapon's
// mFireSpeed (M92F 26, automatics 6, shotguns 73 ...); semi-automatic guns may also wait for the end of the fire
// clip in slots 0-2. FireRate adds (k-1) x dt to both timers before the update runs, and setMotion (aimwalk.cpp)
// plays the fire clips k times faster, so both limits scale together. Reload, grenades and the knife are separate.
//
// Movement. Walking and running are clips in slots 0-2 whose root motion moves the character; their playback speed
// is set once when the action starts, so setMotion scales it (and the stop clip with the speed it stops from).
//
// Shotgun spread. All player shotguns (Ithaca M37, M3, Jail Breaker, Hydra) fire through uWpShotGun::Shot (0xBB61F0,
// weapon vt+0x1BC): every pellet is a shell on a fixed cross/diagonal pattern scaled by random factors, with the
// cone angle SHOT_GUN_RANGE (weapon+0x1C80, 12 degrees) and the start offset SHOT_GUN_OFFSET (+0x1C84, 30 cm).
// ShotgunSpread scales both for the duration of the call, so the pattern shrinks evenly at every distance; the
// pellet count (the Range upgrade) stays. It applies to every player's shots on this machine (the enemy's owner
// simulates them), with the host's value in co-op.
//
// Colour filter. The per-area light-linker grade copied into the colour-correction filter (0x7C2F80) includes the
// yellow tint as corrector 1; one jump at 0x7C2FF7 (je -> jmp) leaves that corrector off, as the community trainer
// and RE5VR do. The game's own Special Settings filters (classic horror, retro, noise) use other correctors.
#include "common.h"
#include "re5.h"
#include "config.h"
#include "net.h"
#include "balance.h"
#include "MinHook.h"

using namespace re5;

namespace {

constexpr uint32_t kFnFireUpdate = 0x00C89F30;  // thiscall(action), plain ret
constexpr uint32_t kFpsPtr = 0x011B209C;        // sFps: +0x28 mDeltaSec
constexpr uint32_t kWeaponInfo = 0x011B5170;     // WEAPON_INFO_STRUCT[idx], stride 0x140: +0xB8 mWepGroup
constexpr uint32_t ChTimeScale = 0xFD4;          // f32 per-character time scale (multiplies every action timer)
constexpr uint32_t ChInventory = 0x21A0;

typedef void(__fastcall* PFN_Action)(uint32_t act, void* edx);
PFN_Action oFireUpdate = nullptr;
constexpr uint32_t kFnShotgunShot = 0x00BB61F0;  // thiscall(weapon, item, const float* from, const float* to), ret 0xC
typedef void(__fastcall* PFN_SgShot)(uint32_t wep, void* edx, uint32_t item, const float* from, const float* to);
PFN_SgShot oShotgunShot = nullptr;
bool g_ready = false;  // hooks installed: the game code may be patched

uint32_t Rd(uint32_t a) { return Readable((void*)a, 4) ? *(uint32_t*)a : 0; }

// Item id of the gun in hand (inventory entry flagged equipped), 0 if none.
uint32_t WeaponInHand(uint32_t ch) {
    const uint32_t inv = ch + ChInventory;
    if (!Readable((void*)inv, 4 + 24 * 0x30)) return 0;
    for (int i = 0; i < 24; ++i) {
        const uint32_t e = inv + 4 + i * 0x30;
        if (Rd(e + 0x1C) == 1 && At<uint8_t>(e, 0x29) == 1) return Rd(e + 4);
    }
    return 0;
}

bool IsGun(uint32_t id) {
    if ((id & 0xFF00) != 0x100) return false;
    if (id == 0x115) return true;  // Gatling gun (group 0)
    const uint32_t info = kWeaponInfo + (id & 0xFF) * 0x140;
    if (!Readable((void*)(info + 0xB8), 4)) return false;
    switch (At<uint32_t>(info, 0xB8)) {
        case 4: case 8: case 16: case 64: case 128: case 256: case 512: case 4194304:  // handgun ... bow
            return true;
    }
    return false;  // knives, grenades, mines, eggs, mounted guns
}

void __fastcall HookFireUpdate(uint32_t act, void* edx) {
    const uint32_t ch = Readable((void*)act, 0x24) ? At<uint32_t>(act, 0x18) : 0;
    if (ch && Readable((void*)ch, 0x3000)) {
        const float k = FireRateFor(ch);
        const uint32_t fps = Rd(kFpsPtr);
        if (k != 1.0f && fps && Readable((void*)(fps + 0x28), 4)) {
            const float d = At<float>(fps, 0x28) * At<float>(ch, ChTimeScale) * (k - 1.0f);
            At<float>(act, 0x20) += d * 60.0f;
            At<float>(act, 0x1C) += d;
        }
    }
    oFireUpdate(act, edx);
}

#if RE3C_DEV
// Dev: every pellet of a shotgun shot goes into the bridge trace while trace_on == 2 (scratch spread tests):
// v[0..2] start, v[3..5] end (clipped by collision), v[6..8] the muzzle, v[9..11] the aim point, v[12] k, v[13] speed.
constexpr uint32_t kFnShellInit = 0x00B9A670;  // thiscall(shell, const float* start, const float* end, float speed, owner), ret 0x10
typedef void(__fastcall* PFN_ShellInit)(uint32_t shell, void* edx, const float* start, const float* end, float speed,
                                         uint32_t owner);
PFN_ShellInit oShellInit = nullptr;
const float* g_sgFrom = nullptr;
const float* g_sgTo = nullptr;
float g_sgK = 1.0f;
uint32_t g_sgShots = 0;

void __fastcall HookShellInit(uint32_t shell, void* edx, const float* start, const float* end, float speed, uint32_t owner) {
    BridgeShared* br = Bridge();
    if (g_sgFrom && br && br->trace_on == 2 && Readable((void*)start, 12) && Readable((void*)end, 12) &&
        Readable((void*)g_sgFrom, 12) && Readable((void*)g_sgTo, 12)) {
        TraceRow& r = br->trace[br->trace_head % 256];
        r.frame = g_sgShots;
        for (int i = 0; i < 3; ++i) {
            r.v[i] = start[i];
            r.v[3 + i] = end[i];
            r.v[6 + i] = g_sgFrom[i];
            r.v[9 + i] = g_sgTo[i];
        }
        r.v[12] = g_sgK;
        r.v[13] = speed;
        r.v[14] = 0.0f;
        ++br->trace_head;
    }
    oShellInit(shell, edx, start, end, speed, owner);
}
#endif

void __fastcall HookShotgunShot(uint32_t wep, void* edx, uint32_t item, const float* from, const float* to) {
    const float k = Cfg().enabled ? NetShotgunSpread() : 1.0f;
#if RE3C_DEV
    struct Mark {
        Mark(const float* f, const float* t, float k) { g_sgFrom = f; g_sgTo = t; g_sgK = k; ++g_sgShots; }
        ~Mark() { g_sgFrom = g_sgTo = nullptr; }
    } mark(from, to, k);
#endif
    if (k != 1.0f && wep && Readable((void*)(wep + 0x1C80), 8)) {
        float& range = At<float>(wep, 0x1C80);
        float& offset = At<float>(wep, 0x1C84);
        const float r0 = range, o0 = offset;
        if (r0 > 0.0f && r0 < 90.0f && o0 >= 0.0f && o0 < 500.0f) {
            range = r0 * k;
            offset = o0 * k;
            oShotgunShot(wep, edx, item, from, to);
            range = r0;
            offset = o0;
            return;
        }
    }
    oShotgunShot(wep, edx, item, from, to);
}

// ---------------------------------------------------------------------------------------------------- colour filter
constexpr uint32_t kFilterSite = 0x007C2FF3;  // cmp byte [edx+44h],0 ; je +0D  (corrector 1 enable)
int g_filterApplied = -1;

}  // namespace

float FireRateFor(uint32_t ch) {
    const Config& c = Cfg();
    if (!c.enabled || !Readable((void*)ch, 0x3000) || At<uint32_t>(ch, ch::ActionId) == aid::KnifeAttack) return 1.0f;
    const float k = NetFireRate();
    return k != 1.0f && IsGun(WeaponInHand(ch)) ? k : 1.0f;
}

float MoveSpeedFor(uint32_t ch, uint32_t actionId) {
    float walk = 1.0f, run = 1.0f;
    NetMoveSpeeds(ch, &walk, &run);
    switch (actionId) {
        case 2: case 3: case 4: case 6: return walk;  // walk, strafe left/right, back
        case 5: return run;
    }
    return 1.0f;
}

void UpdateColorFilter() {
    if (!g_ready) return;
    const Config& c = Cfg();
    const int want = c.enabled && c.noColorFilter ? 1 : 0;
    if (want == g_filterApplied) return;
    BYTE* p = (BYTE*)kFilterSite;
    const BYTE head[] = {0x80, 0x7A, 0x44, 0x00};
    if (!SafeEqual(p, head, sizeof(head)) || (p[4] != 0x74 && p[4] != 0xEB) || p[5] != 0x0D) {
        if (g_filterApplied != -2) Log("colour filter: unexpected code at %08x, left alone", kFilterSite);
        g_filterApplied = -2;
        return;
    }
    DWORD old;
    if (VirtualProtect(p + 4, 1, PAGE_EXECUTE_READWRITE, &old)) {
        p[4] = want ? 0xEB : 0x74;
        VirtualProtect(p + 4, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p + 4, 1);
        Log("colour filter: %s", want ? "yellow grade off" : "original");
    }
    g_filterApplied = want;
}

void InitBalanceHooks() {
    static const BYTE sig[] = {0x83, 0xEC, 0x18, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x18, 0x57, 0x33, 0xFF, 0x3B, 0xC7};
    if (SafeEqual((void*)kFnFireUpdate, sig, sizeof(sig)) &&
        MH_CreateHook((void*)kFnFireUpdate, (void*)HookFireUpdate, (void**)&oFireUpdate) == MH_OK &&
        MH_QueueEnableHook((void*)kFnFireUpdate) == MH_OK)
        Log("hook fire update at %08x", kFnFireUpdate);
    else
        Log("hook fire update failed");
    // Shotgun spread: the function's start and the two field reads the hook relies on.
    static const BYTE sigSg[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x08, 0x8B, 0x40, 0x04,
                                 0x81, 0xEC, 0xE4, 0x02, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8B, 0xF9};
    static const BYTE sigRange[] = {0xF3, 0x0F, 0x10, 0x97, 0x80, 0x1C, 0x00, 0x00};
    static const BYTE sigOffset[] = {0xF3, 0x0F, 0x10, 0x8F, 0x84, 0x1C, 0x00, 0x00};
    if (SafeEqual((void*)kFnShotgunShot, sigSg, sizeof(sigSg)) && SafeEqual((void*)0x00BB66D7, sigRange, sizeof(sigRange)) &&
        SafeEqual((void*)0x00BB6959, sigOffset, sizeof(sigOffset)) &&
        MH_CreateHook((void*)kFnShotgunShot, (void*)HookShotgunShot, (void**)&oShotgunShot) == MH_OK &&
        MH_QueueEnableHook((void*)kFnShotgunShot) == MH_OK)
        Log("hook shotgun shot at %08x", kFnShotgunShot);
    else
        Log("hook shotgun shot failed (ShotgunSpread off)");
#if RE3C_DEV
    static const BYTE sigShell[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x08, 0xF3,
                                    0x0F, 0x10, 0x45, 0x10, 0x83, 0xEC, 0x1C, 0x56, 0x8B, 0xF1};
    if (SafeEqual((void*)kFnShellInit, sigShell, sizeof(sigShell)) &&
        MH_CreateHook((void*)kFnShellInit, (void*)HookShellInit, (void**)&oShellInit) == MH_OK &&
        MH_QueueEnableHook((void*)kFnShellInit) == MH_OK)
        Log("dev: pellet log at %08x", kFnShellInit);
    else
        Log("dev: pellet log hook failed");
#endif
}

void BalanceHooksReady() {
    g_ready = true;
    UpdateColorFilter();
}
