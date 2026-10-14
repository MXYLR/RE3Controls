// Player damage multiplier, and the two mouse-mode aim behaviours turned on for gamepad players.
//
// Damage: every hit the players deal to enemies (bullets, pellets, grenades, knife, kicks and other melee) goes
// through one function, aGame's dynamic-difficulty scaler 0x71B010 (thiscall(DmgInfo* di, int dmg), ret 8),
// right before the enemy loses HP. It multiplies the hit by a rank factor (x1.4 at the start of Normal, drifting
// between x1.8 and x0.94) when the attacker resolves to a player. The mod replaces that factor with a fixed one
// from the ini (default x3: three handgun headshots kill a common Majini, 1500 HP, as in RE3); the knife gets an
// extra factor. Damage dealt to the players never passes here. In co-op the machine that owns an enemy applies
// the real damage, so both players need the same values: the guest takes the host's (net.cpp).
//
// Aim: the game damps the aim camera's breathing sway and kicks the aim pitch on recoil only for mouse players
// (FUN_00B5C4F0 "local player on keyboard/mouse" gates it inside the cPadEx aim function). The mod points those
// three gate calls at its own check, so a pad player gets the same: no breathing, and the camera kick of
// automatic weapons.
#include "common.h"
#include "re5.h"
#include "config.h"
#include "net.h"
#include "damage.h"
#include "MinHook.h"

using namespace re5;

namespace {

constexpr uint32_t kFnDamageScale = 0x0071B010;
constexpr uint32_t kFnAliveAttacker = 0x00C49530;  // thiscall(DmgInfo) -> attacker unit (di+0x20) if alive
constexpr uint32_t kFnDynCast = 0x00726090;        // thiscall(obj, DTI) -> obj or 0
constexpr uint32_t kFnRootOwner = 0x00BC26F0;      // thiscall(uWork) -> owner chain root (+0xFDC)
constexpr uint32_t kFnIsPlayer = 0x007E6370;       // thiscall(playerMgr, uPlayer) -> bool
constexpr uint32_t kDtiWork = 0x012050DC, kDtiPlayer = 0x012031D8;
constexpr uint32_t kFnKbMouse = 0x00B5C4F0;        // thiscall(ch) -> local player on keyboard/mouse
constexpr uint32_t kSiteSway = 0x00B88045, kSiteRecoilBack = 0x00B88192, kSiteRecoilKick = 0x00B882FE;

typedef int(__fastcall* PFN_DmgScale)(uint32_t game, void* edx, uint32_t di, int dmg);
PFN_DmgScale oDmgScale = nullptr;

uint32_t Rd(uint32_t a) { return Readable((void*)a, 4) ? *(uint32_t*)a : 0; }

// The attacker of a hit as the game resolves it: bullet -> weapon -> player, knife -> player, melee = player.
uint32_t AttackerPlayer(uint32_t di) {
    const uint32_t u = ((uint32_t(__fastcall*)(uint32_t, void*))kFnAliveAttacker)(di, nullptr);
    if (!u) return 0;
    uint32_t w = ((uint32_t(__fastcall*)(uint32_t, void*, uint32_t))kFnDynCast)(u, nullptr, kDtiWork);
    if (!w) return 0;
    if (Rd(w + 0xFDC)) w = ((uint32_t(__fastcall*)(uint32_t, void*))kFnRootOwner)(w, nullptr);
    if (!w) return 0;
    const uint32_t p = ((uint32_t(__fastcall*)(uint32_t, void*, uint32_t))kFnDynCast)(w, nullptr, kDtiPlayer);
    const uint32_t mgr = Rd(kPlayerMgrPtr);
    if (!p || !mgr) return 0;
    return ((bool(__fastcall*)(uint32_t, void*, uint32_t))kFnIsPlayer)(mgr, nullptr, p) ? p : 0;
}

int __fastcall HookDmgScale(uint32_t game, void* edx, uint32_t di, int dmg) {
    const int r = oDmgScale(game, edx, di, dmg);
    if (!Cfg().enabled || !di || dmg <= 0 || !Readable((void*)di, 0xA0)) return r;
    float mult = 0.0f, knife = 1.0f;
    NetDamageValues(&mult, &knife);
    if (mult <= 0.0f) return r;  // 0 = the game's own dynamic difficulty
    if (!AttackerPlayer(di)) return r;
    float k = mult;
    const uint32_t hitParam = Rd(di + 0x9C) & 0xFFFF;  // 1 = knife (Chris's and Sheva's), 2-9 guns, 10+ melee
    if (hitParam == 1) k *= knife;
    const float f = (float)dmg * k;
    int out = f >= 32767.0f ? 32767 : (int)(f + 0.5f);
    if (out < 1) out = 1;
    if (DevMode()) Log("damage: base %d, game %d, mod %d (hit param %u)", dmg, r, out, hitParam);
    return out;
}

bool LocalPlayer(uint32_t ch) { return Readable((void*)ch, 0x3000) && Rd(ch + ch::NetControl) == 0; }

bool __fastcall SwayGate(uint32_t ch) {
    return KbMouse(ch) || (Cfg().enabled && !Cfg().breathing && LocalPlayer(ch));
}

bool __fastcall RecoilGate(uint32_t ch) {
    return KbMouse(ch) || (Cfg().enabled && Cfg().padRecoil && LocalPlayer(ch));
}

// "call FUN_00B5C4F0" followed by "test al,al".
bool IsGateCall(uint32_t site) {
    return Readable((void*)site, 8) && At<uint8_t>(site, 0) == 0xE8 && site + 5 + At<int32_t>(site, 1) == kFnKbMouse &&
           At<uint8_t>(site, 5) == 0x84 && At<uint8_t>(site, 6) == 0xC0;
}

// Point such a call at our gate.
bool PatchGateCall(uint32_t site, void* gate, const char* name) {
    if (!IsGateCall(site)) {
        Log("patch %s: unexpected bytes at %08x", name, site);
        return false;
    }
    DWORD old;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    At<int32_t>(site, 1) = (int32_t)((uint32_t)(uintptr_t)gate - (site + 5));
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    Log("patch %s at %08x", name, site);
    return true;
}

}  // namespace

bool KbMouse(uint32_t ch) { return ((bool(__fastcall*)(uint32_t, void*))kFnKbMouse)(ch, nullptr); }

void InitDamageHooks() {
    static const BYTE sig[] = {0x53, 0x8B, 0xD9, 0x8B, 0x4C, 0x24, 0x08, 0x85, 0xC9, 0x75, 0x08,
                               0x8B, 0x44, 0x24, 0x0C, 0x5B, 0xC2, 0x08, 0x00};
    if (SafeEqual((void*)kFnDamageScale, sig, sizeof(sig)) &&
        MH_CreateHook((void*)kFnDamageScale, (void*)HookDmgScale, (void**)&oDmgScale) == MH_OK &&
        MH_QueueEnableHook((void*)kFnDamageScale) == MH_OK)
        Log("hook player damage scale at %08x", kFnDamageScale);
    else
        Log("hook player damage scale failed");
    PatchGateCall(kSiteSway, (void*)SwayGate, "aim sway gate");
    // The kick and its recovery go together, or the aim would drift.
    if (IsGateCall(kSiteRecoilKick) && IsGateCall(kSiteRecoilBack)) {
        PatchGateCall(kSiteRecoilKick, (void*)RecoilGate, "recoil kick gate");
        PatchGateCall(kSiteRecoilBack, (void*)RecoilGate, "recoil return gate");
    } else {
        Log("recoil gates: unexpected bytes, left alone");
    }
}
