// Gameplay layer: RE3-style controls.
//
// RE5 builds a "canonical" button set (Type A layout) for every human character in a PlayerPad (mode 1) by passing
// the raw pad through the control-type remap table; character actions read only that. Menus, the inventory and
// context/QTE prompts read the raw pad. So the mod rewrites the canonical buttons of the local character from the
// raw pad after the game builds them, and separately lets A answer the non-QTE "X" context prompts.
#include "common.h"
#include "re5.h"
#include "config.h"
#include "camera.h"
#include "aimwalk.h"
#include "net.h"
#include "crosshair.h"
#include "damage.h"
#include "melee.h"
#include "balance.h"
#include "inventory.h"
#include "kbinput.h"
#include <d3d9.h>
#include <math.h>
#include "MinHook.h"

using namespace re5;

typedef void(__fastcall* PFN_PlayerPadUpdate)(uint32_t self, void* edx);
typedef bool(__fastcall* PFN_ActionCheck)(uint32_t self, void* edx, uint32_t pad, uint32_t slot);
static PFN_PlayerPadUpdate oPlayerPadUpdate = nullptr;
static PFN_ActionCheck oActionCheck = nullptr;

static double Now() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static uint32_t ReadPtr(uint32_t addr) { return Readable((void*)addr, 4) ? *(uint32_t*)addr : 0; }

struct RawSticks { float lx, ly, rx, ry; };

// Raw (physical) MT pad bits of sPad pad[index]; 0 when the pad is missing or input is locked.
static bool RawPad(uint32_t index, uint32_t* bits, RawSticks* st = nullptr) {
    uint32_t spad = ReadPtr(kSPadPtr);
    if (!spad || index > 1 || !Readable((void*)spad, 0x600)) return false;
    if (At<uint8_t>(spad, spad::InputLocked)) return false;
    const uint32_t pad = spad + spad::Pads + index * spad::PadStride;
    if (At<uint8_t>(pad, spad::PadType) == 0) return false;  // no device: keyboard play, leave the game alone
    *bits = At<uint32_t>(pad, spad::On);
    if (st) {
        const float k = 1.0f / 32767.0f;
        st->rx = At<int32_t>(pad, spad::Rx) * k;
        st->ry = At<int32_t>(pad, spad::Ry) * k;
        st->lx = At<int32_t>(pad, spad::Lx) * k;
        st->ly = At<int32_t>(pad, spad::Ly) * k;
    }
    return true;
}

// The game switches between pad and keyboard/mouse by itself, to the device used last (sPad+0x5F0 for the player
// that owns the keyboard). The mod's controls are made for the pad; in keyboard mode the game is left alone.
static bool KeyboardMode(uint32_t player) {
    const uint32_t spad = ReadPtr(kSPadPtr);
    if (!spad || !Readable((void*)spad, 0x620)) return false;
    return At<int32_t>(spad, spad::KeyboardPlayer) == (int32_t)player && At<uint8_t>(spad, spad::DeviceIsPad) == 0;
}

static uint16_t MotionNo(uint32_t character, int slot) {
    return At<uint16_t>(character, ch::Motion0 + slot * ch::MotionStride + 4);
}

static uint32_t ActionVtable(uint32_t character) {
    const uint32_t act = At<uint32_t>(character, ch::CurAction);
    return Readable((void*)act, 4) ? At<uint32_t>(act, 0) : 0;
}

// Free locomotion: standing, walking, running, turning on the spot.
// The inventory grid ([character+0x2654], state machine 0xC6C420) is open: the game's own test 0xC69BF0.
static bool InventoryOpen(uint32_t character) {
    const uint32_t inv = At<uint32_t>(character, 0x2654);
    return Readable((void*)inv, 0x80) && At<uint8_t>(inv, 0x68) != 0 && At<float>(inv, 0x50) > 0.0f;
}

static bool InLocomotion(uint32_t character) {
    switch (ActionVtable(character)) {
        case plact::Relax: case plact::Wait: case plact::Walk: case plact::WalkL: case plact::WalkR:
        case plact::Run: case plact::Back: case plact::TurnL: case plact::TurnR:
            return true;
    }
    return false;
}

static float DeadZone(float v, float dz) {
    if (fabsf(v) < dz) return 0.0f;
    return (v - (v > 0 ? dz : -dz)) / (1.0f - dz);
}

// Wesker's dash in The Mercenaries: the knee_bazooka actions (start, run, Knee Cannon, stop, turns right/left/back).
static bool IsDash(uint32_t id) { return id >= 0x47 && id <= 0x4D; }

// Character kind from the game's function at 0x715B90 (DTI id >> 8 & 0xFFFF).  Known values on this build:
// 0 = Chris, 1 = Sheva, 3 = Wesker (the dash check).  The kind is logged once per character so the shoulder table
// (ShoulderLeftKinds) can be corrected from a single run.
static int CharacterKind(uint32_t character) {
    static int ok = 0;  // 1 = the function is the expected one; -1 = not (then the kind is unknown)
    if (!ok) {
        static const BYTE sig[] = {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x01, 0x8B, 0x50, 0x10,
                                   0xFF, 0xD2, 0x8B, 0x40, 0x20, 0xC1, 0xE8, 0x08};
        ok = SafeEqual((void*)0x00715B90, sig, sizeof(sig)) ? 1 : -1;
        if (ok < 0) Log("character kind: unexpected code at 00715b90, shoulder auto and WeskerDash off");
    }
    if (ok < 0) return -1;
    return (int)(((uint32_t(__cdecl*)(uint32_t))0x00715B90)(character) & 0xFFu);
}
static bool IsWesker(uint32_t character) { return CharacterKind(character) == 3; }
// Plain-gun zoom state, read by the camera through ZoomFovExtra().
static int g_zoomStep = 0;
static float g_zoomFov = 0.0f;
static bool g_zoomFPrev = false, g_zoomCPrev = false;
float ZoomFovExtra() { return g_zoomFov; }

// Whether the game would reload now: its own test before a reload (0xB83E67) - the item in hand (0xC4B700(inv, 0xE))
// is a gun whose magazine is not full and that has spare ammo (0xC4DE50). Without the expected code: always.
static bool ReloadPossible(uint32_t character) {
    static int ok = 0;
    if (!ok) {
        static const BYTE a[] = {0x51, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x83, 0xFE, 0x0E};
        static const BYTE b[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x46, 0x04, 0x57, 0x8B, 0xF9};
        ok = SafeEqual((void*)0x00C4B700, a, sizeof(a)) && SafeEqual((void*)0x00C4DE50, b, sizeof(b)) ? 1 : -1;
        if (ok < 0) Log("reload test: unexpected code at 00c4b700/00c4de50, X always asks for a reload");
    }
    if (ok < 0) return true;
    const uint32_t inv = character + 0x21A0;
    const uint32_t item = ((uint32_t(__fastcall*)(uint32_t, void*, int))0x00C4B700)(inv, nullptr, 0xE);
    return item && ((bool(__fastcall*)(uint32_t, void*, uint32_t))0x00C4DE50)(inv, nullptr, item);
}

// The buttons the prompts on screen take, per player (mask table 0xF58E50): ActionSlot::Check runs every frame for
// each active prompt, so a prompt counts as up for 0.15 s after its last check (a few at once: the last 8 checks).
struct PromptSeen { double t; uint32_t mask; };
static PromptSeen g_prompt[4][8];
static uint32_t g_promptNext[4];

static void PromptSeenNow(uint32_t player, uint32_t mask) {
    if (player < 4) g_prompt[player][g_promptNext[player]++ % 8] = {Now(), mask};
}

static bool PromptUp(uint32_t player, uint32_t bits) {
    if (player >= 4) return false;
    const double now = Now();
    for (const PromptSeen& p : g_prompt[player])
        if (now - p.t < 0.15 && (p.mask & bits)) return true;
    return false;
}

// Each human-controlled character owns a PlayerPad (mode 1: physical pad through the button-config table) at
// [character+0x2D64] + 0x708; that is the one character actions read. Network partners use mode 2.
// A player who passed the title screen with the keyboard has no pad index (0xFF, like the AI); that one is local too
// when it is the keyboard player (sPad+0x614).
static bool IsLocalPlayerPad(uint32_t padObj, uint32_t* outChar) {
    if (At<uint8_t>(padObj, pp::Enabled) != 1) return false;
    if (At<uint32_t>(padObj, pp::Mode) != 1) return false;
    const uint32_t ctl = padObj - kCharPadOffset;
    if (!Readable((void*)ctl, 0x20)) return false;
    const uint32_t ch = At<uint32_t>(ctl, 0x18);
    if (!ch || !Readable((void*)ch, 0x3000) || At<uint32_t>(ch, ch::Controller) != ctl) return false;
    if (At<uint32_t>(padObj, pp::PadIndex) > 3) {
        const uint32_t spad = ReadPtr(kSPadPtr);
        if (At<uint32_t>(ch, ch::NetControl) != 0 || !spad || !Readable((void*)spad, 0x620) ||
            At<int32_t>(spad, spad::KeyboardPlayer) != (int32_t)At<uint8_t>(ch, ch::NetSlot))
            return false;
    }
    if (outChar) *outChar = ch;
    return true;
}

// ------------------------------------------------------------------------------------------------ controls state
// One-button reload: the mod gives the game its reload command (cPadEx+0x23) until the reload starts or the game
// turns it down (full magazine, no spare ammo).
enum class ReloadPhase { Idle, Request, Reloading };

struct ControlsState {
    uint32_t pad = 0;          // PlayerPad this state belongs to
    double last = 0;
    uint32_t prevPhys = 0;
    bool runToggled = false;
    double runIdleTime = 0;
    ReloadPhase reload = ReloadPhase::Idle;
    double reloadT = 0;        // time in the current reload phase
    int reloadTries = 0;       // frames the command was offered to an action that takes it at once
    bool reloadMove = false;   // reload started without aiming: keep walking, free camera stays on
    bool aimFast = false;      // L3 while aiming: fast walk
    double quickTurnT = -1;
    bool dirLock = false;      // after a quick turn: the held stick keeps its world direction
    float lockYaw = 0, lockSx = 0, lockSy = 0;
    bool wasAiming = false;
    uint32_t character = 0;
    bool gunAim = false;       // gun raised by the player (not the knife)
    bool rsHeld = false;       // Mode=MeleeCamera: right stick still held since it turned the camera in a melee attack
    bool bHeldFromTurn = false;  // B that started a quick turn: no partner command until it is released
    bool prevQuick = false;     // keyboard: quick turn key last frame
    double swingT = -1;         // quick turn: the camera swings round to the new facing
    float swingFrom = 0, swingTo = 0;
    bool tauntL3 = false;       // the L3 of a taunt (R3 held + L3): the game's until it is released
    bool knifeLast = false;     // LB pressed after LT: the knife comes out of the gun aim (KnifeFromAim)
    bool snapPending = false;   // aim pressed: turn to the camera once the character may aim that way
    float snapYaw = 0;          // the camera's view at that press (the game's camera owns fc.yaw while aiming)
    uint32_t runSig = 0;        // the canonical button this run uses (A; L3 without the free camera in types C/D)
    bool kbRunPress = false;    // keyboard: the run key went down while moving (in the inventory it runs, no confirm)
};
static ControlsState g_states[8];

static ControlsState& StateFor(uint32_t pad) {
    for (auto& s : g_states)
        if (s.pad == pad) return s;
    // A pad object seen for the first time (new game, chapter restart). Nothing releases the entries of pad objects
    // that are gone, so the table fills up over restarts (two entries per restart in split screen). Take a free
    // entry or, when there is none, the one unused for longest. Always reusing one fixed entry made the two local
    // players take it from each other every frame: their state started from scratch each time (no frame time, no
    // run toggle, no steering).
    ControlsState* slot = nullptr;
    for (auto& s : g_states)
        if (!s.pad) { slot = &s; break; }
    if (!slot) {
        slot = &g_states[0];
        for (auto& s : g_states)
            if (s.last < slot->last) slot = &s;
    }
    *slot = ControlsState{};
    slot->pad = pad;
    return *slot;
}

bool GunAimHeld(uint32_t character) {
    for (auto& s : g_states)
        if (s.pad && s.character == character) return s.gunAim;
    return false;
}

static inline float Rad(float deg) { return deg * 0.01745329252f; }

static void SetStickDirBits(uint32_t& bits, float angleDeg, float mag) {
    bits &= ~P_LS_DIRS;
    if (mag <= 0.0f) return;
    float x = -sinf(Rad(angleDeg)) * mag, y = cosf(Rad(angleDeg)) * mag;
    if (y > 0.25f) bits |= P_LS_UP;
    if (y < -0.25f) bits |= P_LS_DOWN;
    if (x < -0.25f) bits |= P_LS_LEFT;
    if (x > 0.25f) bits |= P_LS_RIGHT;
}

// World yaw the left stick points to, seen from the free camera.
static float StickWorldYaw(const FreeCam& fc, float lx, float ly) {
    const float sy = sinf(fc.yaw), cy = cosf(fc.yaw);
    return atan2f(lx * -cy + ly * sy, lx * sy + ly * cy);
}

// Mode=MeleeCamera: the game's own controls everywhere; during a melee attack (which the mod keeps from switching to
// the attack cinematic, melee.cpp) the right stick turns the orbit camera instead of the character.
static void MeleeOnlyControls(ControlsState& S, uint32_t padObj, uint32_t character, const RawSticks& st, float dt,
                              const KbFrame* kb) {
    const Config& c = Cfg();
    FreeCam& fc = CamFor(character);
    const uint32_t id = At<uint32_t>(character, ch::ActionId);
    // Wesker's dash: the right stick steers it and the game's camera follows it (WeskerDash).
    const bool dash = c.weskerDash && IsDash(id);
    bool melee = FreeCameraAction(character) && !dash;  // a melee attack or (GrabCamera) an enemy grab
#if RE3C_DEV
    if (DevMode() && Bridge()->reserved[17]) melee = true;  // dev: act as if a melee attack were running
#endif
    const float rx = DeadZone(st.rx, 0.2f), ry = DeadZone(st.ry, 0.2f);
    if (melee) S.rsHeld = true;
    else if (rx == 0.0f && ry == 0.0f) S.rsHeld = false;
    fc.active = melee;
    fc.activeTime = Now();
    fc.noAimRig = false;
    fc.leftShoulder = false;  // the shoulder swap belongs to the full mode
    if (kb) {
        // Keyboard/mouse: the mouse turns the camera during the attack (the game's own mouse turning is off then).
        if (melee && fc.initialised && !JustActivated()) {
            const float k = MouseUnitRadians() * c.mouseSensitivity;
            const uint32_t inv = MouseInvert();
            fc.yaw = WrapPi(fc.yaw - kb->mouseX * k * ((inv & 2) ? -1.0f : 1.0f));
            fc.pitch += kb->mouseY * k * c.mouseVertical * ((inv & 1) ? -1.0f : 1.0f);
            if (fc.pitch > Rad(55.0f)) fc.pitch = Rad(55.0f);
            if (fc.pitch < Rad(-40.0f)) fc.pitch = Rad(-40.0f);
        }
        if (melee) {
            static const uint32_t kMouse[] = {0x200, 0x204, 0x20C, 0x210};
            for (uint32_t off : kMouse) At<float>(padObj, off) = 0.0f;
        }
    } else if ((melee || S.rsHeld) && !dash) {
        if (melee && fc.initialised) {
            fc.yaw = WrapPi(fc.yaw - rx * Rad(c.cameraSpeed) * dt);
            fc.pitch -= (c.invertCameraY ? -ry : ry) * Rad(c.cameraPitchSpeed) * dt;
            if (fc.pitch > Rad(55.0f)) fc.pitch = Rad(55.0f);
            if (fc.pitch < Rad(-40.0f)) fc.pitch = Rad(-40.0f);
        }
        // The stick turns the camera, not the character (control types C/D), until it is let go.
        At<float>(padObj, pp::RAngle) = 0;
        At<float>(padObj, pp::RMag) = 0;
        At<uint32_t>(padObj, pp::On) &= ~P_RS_DIRS;
        At<uint32_t>(padObj, pp::Trg) &= ~P_RS_DIRS;
        At<uint32_t>(padObj, pp::Rel) &= ~P_RS_DIRS;
        At<uint32_t>(padObj, pp::TrgRep) &= ~P_RS_DIRS;
    }
    S.character = character;
    AimWalkUpdate(character, 0.0f, 0.0f, false, 0.0f, dt);
    NetLocalFrame(character, false, false);
}

// InventoryMove: the left stick walks (L3 runs) while the inventory is open, the grid cursor takes the d-pad only.
// The builder gets a stick-only copy of the pad and the grid its buttons back (inventory.cpp).
static void InventoryWalk(ControlsState& S, uint32_t padObj, uint32_t character, uint32_t phys, uint32_t physTrg,
                          const RawSticks& st, float dt, bool freeCam, RunMode runMode, bool keyboard) {
    const Config& c = Cfg();
    FreeCam& fc = CamFor(character);
    static const uint32_t kBits[] = {pp::On, pp::Trg, pp::Rel, pp::Rep, pp::TrgRep};
    for (uint32_t off : kBits) At<uint32_t>(padObj, off) &= ~P_LS_DIRS;
    float& lAng = At<float>(padObj, pp::LAngle);
    float& lMag = At<float>(padObj, pp::LMag);
    const float stickMag = sqrtf(st.lx * st.lx + st.ly * st.ly);
    // The game gives the run key pad A on keyboard, and A confirms in the grid: a run key that went down while moving
    // only runs here; pressed while standing it is the grid's A, as in the original game.
    if (keyboard) {
        if (physTrg & P_L3) S.kbRunPress = stickMag >= 0.3f;
        if (!(phys & P_L3)) S.kbRunPress = false;
        if (S.kbRunPress)
            for (uint32_t off : kBits) At<uint32_t>(padObj, off) &= ~P_A;
    }
    const uint32_t action = ActionVtable(character);
    // Using an item, wall cover, a quick turn ...: the game keeps the character still as usual.
    const bool free = InLocomotion(character);
    // Equipping a weapon from the grid, or a reload on the move that was going when the grid opened: the action
    // plays while the leg layer keeps walking; once it may be cancelled the builder gets the stick again and the game
    // walks on. Not in cover, riding or in the scope.
    const bool reloading = S.reloadMove && c.reloadWalk && action == plact::Reload;
    const bool changing = (reloading || (c.switchWalk && action == plact::WeaponChange)) &&
                          !(At<uint32_t>(character, 0x2D8C) & 0x4F00) && !At<uint8_t>(character, ch::Scope);
    const bool window = changing && (At<uint32_t>(character, 0x2D7C) & 0x20000000);
    S.aimFast = false;
    S.wasAiming = false;
    S.gunAim = false;
    S.character = character;

    // Running: L3 in the inventory whatever RunButton says (RB opens the partner's items there); not with Run=Stock
    // (A belongs to the grid).
    bool wantRun = false;
    if (runMode == RunMode::Toggle) {
        if ((physTrg & P_L3) && (!keyboard || stickMag >= 0.3f)) S.runToggled = !S.runToggled;
        if (stickMag < 0.3f) {
            S.runIdleTime += dt;
            if (S.runIdleTime > 0.5) S.runToggled = false;
        } else {
            S.runIdleTime = 0;
        }
        wantRun = S.runToggled;
    } else if (runMode == RunMode::Hold) {
        wantRun = (phys & P_L3) && (!keyboard || S.kbRunPress);
    }
    if (keyboard) {
        // The mouse is the inventory's pointer here: it neither turns the camera nor (the game's own mouse turning)
        // the character.
        At<float>(padObj, 0x200) = 0.0f;
        At<float>(padObj, 0x204) = 0.0f;
    }

    bool back = false;  // BackStep, as outside the grid
    if (freeCam) {
        // The camera stays free: the grid never reads the right stick.
        fc.active = true;
        fc.noAimRig = S.reloadMove;  // the reload sets the game's aim camera flag
        const float rx = DeadZone(st.rx, 0.2f), ry = DeadZone(st.ry, 0.2f);
        fc.yaw = WrapPi(fc.yaw - rx * Rad(c.cameraSpeed) * dt);
        fc.pitch -= (c.invertCameraY ? -ry : ry) * Rad(c.cameraPitchSpeed) * dt;
        if (fc.pitch > Rad(55.0f)) fc.pitch = Rad(55.0f);
        if (fc.pitch < Rad(-40.0f)) fc.pitch = Rad(-40.0f);
        if (physTrg & P_R3) {
            fc.yaw = CharacterYaw(character);
            fc.pitch = DefaultCameraPitch();
        }
        At<float>(padObj, pp::RAngle) = 0;
        At<float>(padObj, pp::RMag) = 0;
        if (stickMag >= 0.3f && fc.initialised && (free || changing)) {
            back = c.backStep && !wantRun && st.ly < -0.85f * stickMag;
            const float want = back ? fc.yaw : StickWorldYaw(fc, st.lx, st.ly);
            const float cur = CharacterYaw(character);
            const float diff = WrapPi(want - cur);
            const float step = Rad(c.turnSpeed) * dt;
            SetCharacterYaw(character, fabsf(diff) <= step ? want : WrapPi(cur + (diff > 0 ? step : -step)));
            if (free || window) {
                lAng = back ? 180.0f : 0.0f;
                lMag = stickMag > 1.0f ? 1.0f : stickMag;
            }
        }
    } else if (keyboard && stickMag >= 0.3f && (free || window)) {
        lAng = atan2f(-st.lx, st.ly) * 57.29578f;  // no free camera: WASD relative to the character, as in the game
        lMag = stickMag > 1.0f ? 1.0f : stickMag;
    }
    const bool orbit = freeCam && fc.initialised;
    const bool runGait = changing && wantRun && stickMag >= 0.3f;  // the gait goes on, as in ApplyControls
    const bool legs = AimWalkUpdate(character, changing && !orbit ? st.lx : 0.0f,
                                    changing ? (orbit ? (back ? -stickMag : stickMag) : st.ly) : 0.0f, changing,
                                    MoveSpeedFor(character, runGait ? 5 : 2), dt, runGait, changing, keyboard);
    NetLocalFrame(character, free, legs);
    // Standing with the stick let go: the game's own inventory stop (nothing to walk).
    if (!(free || window) || (lMag < 0.3f && (action == plact::Wait || action == plact::Relax))) return;
    uint32_t view = 0;
    SetStickDirBits(view, lAng, lMag);
    if (wantRun && lMag >= 0.3f) {
        // Without the free camera control types C/D run on L3 (A with the stick to the side turns on the spot).
        if (!freeCam && !keyboard && At<uint8_t>(padObj - 0x4E8, 0x4CE)) view |= P_L3;
        else if (fabsf(lAng) < 100.0f) view |= P_A;
    }
    // Without the free camera the game steers: control types C/D turn with the right stick.
    if (!freeCam) view |= At<uint32_t>(padObj, pp::On) & P_RS_DIRS;
    InventoryStash(padObj, At<uint32_t>(character, 0x2654), view, !freeCam);
}

// Where the game itself takes a quick turn: free locomotion, lowering the gun or the knife, a switch from the aim
// (0xB83C10 is called from those actions).
static bool QuickTurnStance(uint32_t character, uint32_t action, uint32_t id) {
    return InLocomotion(character) || action == plact::ReadyEnd || id == aid::KnifeEnd || id == 0x21;
}

static void ApplyControls(uint32_t padObj, uint32_t character) {
    const Config& c = Cfg();
    const uint32_t padIndex = At<uint32_t>(padObj, pp::PadIndex);
    const uint8_t slot = At<uint8_t>(character, ch::NetSlot);
    // The game switches between pad and keyboard/mouse by itself, to the device used last (sPad+0x5F0, +0x614).
    const bool keyboard = KeyboardMode(slot);
    const bool kbm = keyboard && c.keyboardMouse;  // the mod's controls on keyboard/mouse (kbinput.cpp)
    uint32_t phys = 0;
    RawSticks st = {};
    KbFrame kb;
    if (kbm) {
        if (!KbRead(character, padObj, &kb)) {
            // Input locked (window in the background, system message, the moment after coming back): nothing may
            // stay held, and mouse motion is dropped like the keys (the game's own mouse turning reads it too).
            static const uint32_t kMouse[] = {0x200, 0x204, 0x20C, 0x210};
            for (uint32_t off : kMouse) At<float>(padObj, off) = 0.0f;
            KbSetDriving(slot, false, false, true, false);
            AimWalkUpdate(character, 0.0f, 0.0f, false, 0.0f, 0.0f);
            NetLocalFrame(character, InLocomotion(character), false);
            return;
        }
        st.lx = kb.moveX;
        st.ly = kb.moveY;
    } else {
        KbSetDriving(slot, false, false, true, false);
    }
    if (!kbm && !RawPad(padIndex, &phys, &st)) {
        // The pad is gone (unplugged, battery): nothing the mod drove may stay stuck. While input is only locked
        // (e.g. the window is in the background) the camera stays as it is, but the legs stop either way: with no
        // input the walk clip would carry the character on. Co-op data keeps flowing (speeds, host values).
        const uint32_t spad = ReadPtr(kSPadPtr);
        const bool padGone = padIndex < 4 && spad && Readable((void*)spad, 0x600) && !At<uint8_t>(spad, spad::InputLocked) &&
                             At<uint8_t>(spad + spad::Pads + padIndex * spad::PadStride, spad::PadType) == 0;
        if (padGone) {
            FreeCam& fc = CamFor(character);
            fc.active = false;
            fc.noAimRig = false;
            fc.leftShoulder = false;
        }
        AimWalkUpdate(character, 0.0f, 0.0f, false, 0.0f, 0.0f);
        NetLocalFrame(character, InLocomotion(character), false);
        return;
    }
    ControlsState& S = StateFor(padObj);
    if (S.character != character) {
        // A restarted chapter brings new pad objects; an old entry may still name the same character address.
        for (auto& o : g_states)
            if (&o != &S && o.character == character) o = ControlsState{};
        S.character = character;
    }
    const double now = Now();
    double dt = S.last ? now - S.last : 0.0;
    if (dt > 0.25) {
        // Back after a pause (window in the background, loading): the game finished a quick turn or turned a reload
        // request down meanwhile; nothing of it is replayed.
        dt = 0.25;
        S.quickTurnT = -1;
        S.swingT = -1;
        if (S.reload == ReloadPhase::Request) S.reload = ReloadPhase::Idle;
    }
    S.last = now;

    uint32_t& on = At<uint32_t>(padObj, pp::On);
    float& lAng = At<float>(padObj, pp::LAngle);
    float& lMag = At<float>(padObj, pp::LMag);
    // Pad-layout bits for the logic below. On keyboard/mouse the run key always works like L3 and the locate key like
    // RB (RunButton is about pad buttons).
    const bool rbRuns = !kbm && c.remapButtons && c.runButton == RunButton::RB;
    const uint32_t runBit = rbRuns ? P_RB : P_L3;
    const uint32_t partnerBit = rbRuns ? P_L3 : P_RB;
    const RunMode runMode = kbm ? c.kbRunMode : c.runMode;
    if (kbm)
        phys = (kb.aim ? P_LT : 0) | (kb.knife ? P_LB : 0) | (kb.fire ? P_RT : 0) | (kb.run ? runBit : 0) |
               (kb.reload ? P_X : 0) | (kb.partner ? P_B : 0) | (kb.locate ? partnerBit : 0) | (kb.map ? P_BACK : 0) |
               (kb.inventory ? P_Y : 0) | (kb.cameraReset ? P_R3 : 0);
    const bool quickKey = kbm && kb.quickTurn && !S.prevQuick;
    S.prevQuick = kbm && kb.quickTurn;
    uint32_t physTrg = phys & ~S.prevPhys;
    S.prevPhys = phys;
    // The original taunt (hold R3, click L3; StickClicks): that L3 click is the game's, not the mod's run, aim pace
    // or partner button, until it is released.
    bool tauntPress = false;
    if (!kbm && c.stickClicks) {
        tauntPress = (phys & P_R3) && (physTrg & P_L3);
        if (tauntPress) S.tauntL3 = true;
        else if (!(phys & P_L3)) S.tauntL3 = false;
        if (S.tauntL3) {
            phys &= ~P_L3;
            physTrg &= ~P_L3;
        }
    }
    const bool aiming = (phys & P_LT) != 0;
    const bool knife = (phys & P_LB) != 0;
    const bool aimHeld = aiming || knife;
    // KnifeFromAim: LB pressed while LT is held readies the knife from the gun aim (the game's own knife + gun bits
    // together); LT pressed while the knife is out still goes to the gun.
    if (physTrg & P_LT) S.knifeLast = false;
    if (physTrg & P_LB) S.knifeLast = true;
    if (!(phys & P_LB)) S.knifeLast = false;
    const bool knifeWins = c.knifeFromAim && c.remapButtons && knife && aiming && S.knifeLast;
    const uint32_t action = ActionVtable(character);
    const float stickMag = sqrtf(st.lx * st.lx + st.ly * st.ly);
    // Every local player has a free camera of their own (split screen: on their own camera controller, whatever
    // device each one plays with).
    FreeCam& fc = CamFor(character);
    // Upstream semantics, nothing on top: fc.leftShoulder is the side, one flag for the orbit and the aim mirror.
    // The per-character defaults (ShoulderAuto / ShoulderLeftKinds / ShoulderGameLeftKinds) and the runtime
    // measurement are disabled; the keys still read as documented in the README.
    if (fc.character != character) {
        g_zoomStep = 0;
        g_zoomFov = 0.0f;
    }
    fc.character = character;
    // Plain-gun zoom (F in, C out), keyboard only, never in a scope (that one has its own zoom); the level is
    // dropped when the aim ends so every aim starts at 1x, like the scope.
    if (c.zoomKeys && kbm && KbLayerFor(character)) {
        if (!aimHeld || At<uint8_t>(character, ch::Scope)) {
            g_zoomStep = 0;
        } else {
            const bool fDown = (GetAsyncKeyState('F') & 0x8000) != 0;
            const bool cDown = (GetAsyncKeyState('C') & 0x8000) != 0;
            if (fDown && !g_zoomFPrev && g_zoomStep < c.zoomMaxStep) ++g_zoomStep;
            if (cDown && !g_zoomCPrev && g_zoomStep > 0) --g_zoomStep;
            g_zoomFPrev = fDown;
            g_zoomCPrev = cDown;
        }
        g_zoomFov = c.zoomFovStep * (float)g_zoomStep;
        static int loggedStep = 0;
        if (g_zoomStep != loggedStep) {
            loggedStep = g_zoomStep;
            if (g_zoomStep != 0) Log("zoom: step %d, fov %+.0f", g_zoomStep, g_zoomFov);
        }
    } else {
        g_zoomStep = 0;
        g_zoomFov = 0.0f;
    }
    // Keyboard shoulder key (Controls/ShoulderKey, default B): raw key, aiming or not, independent of the game's own
    // bindings.  It flips the same single flag the pad uses (fc.leftShoulder), so walking and aiming always flip
    // together, exactly as upstream's RB swap does.
    if (c.shoulderSwapAlways && c.shoulderKey[0] && !At<uint8_t>(character, ch::Scope)) {
        const int vk = toupper((unsigned char)c.shoulderKey[0]);
        const bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        static bool keyPrev = false;
        if (down && !keyPrev) {
            fc.leftShoulder = !fc.leftShoulder;
            Log("shoulder: key '%c' -> %s shoulder", vk, fc.leftShoulder ? "left" : "right");
        }
        keyPrev = down;
    }
    // ch+0x2D8C: 0x4000 riding something, with 0x80 a gun emplacement, 0x40 the jeep gun, 0x20 the airboat driver
    // (the airboat passenger has only 0x4000 and keeps the mod's controls).
    const uint32_t ride = At<uint32_t>(character, 0x2D8C);
    // Low cover and wall cover (0x100, 0x200, 0x400, 0x800): the game's own camera and sticks, as with FreeCamera=0
    // (the camera follows the game's own, the sticks move and aim as the game does them).
    const bool covered = (ride & 0xF00) != 0;
    const bool freeCam = c.freeCamera && !covered;

    // The in-game inventory reads this pad with the player's own button layout (A = item menu, RB = partner's
    // items): while it is open the game's buttons are left alone. With KeyboardMouse=0 keyboard/mouse play is left to
    // the game entirely (its own camera too). On a mounted gun or a vehicle gun the game's own controls apply.
    const uint32_t idNow = At<uint32_t>(character, ch::ActionId);
    const bool mounted = ((ride & 0x4000) && (ride & 0xE0)) || (idNow >= 0x6A && idNow <= 0x6E);
    const bool dash = c.weskerDash && IsDash(idNow);  // Wesker's dash: the game's controls and camera
#if RE3C_DEV
    Bridge()->reserved[6] = (keyboard ? 1u : 0u) | (fc.active ? 2u : 0u) |
                            (IsMeleeAction(At<uint32_t>(character, ch::ActionId)) ? 4u : 0u) | (fc.leftShoulder ? 8u : 0u);
#endif
    if ((keyboard && !kbm) || mounted || InventoryOpen(character)) {
        const bool invMove = !mounted && (!keyboard || kbm) && c.inventoryMove && !c.meleeOnly && InventoryMoveReady() &&
                             InventoryOpen(character);
        KbSetDriving(slot, kbm && invMove, true, true, freeCam && fc.initialised);
        S.reload = ReloadPhase::Idle;
        // A reload on the move goes on with the grid open (it does not close it): it keeps walking there.
        S.reloadMove = S.reloadMove && invMove && action == plact::Reload;
        S.quickTurnT = -1;
        S.swingT = -1;
        S.dirLock = false;
        if (invMove) {
            InventoryWalk(S, padObj, character, phys, physTrg, st, (float)dt, freeCam, runMode, kbm);
            return;
        }
        S.runToggled = false;
        S.runIdleTime = 0;
        S.aimFast = false;
        S.wasAiming = false;
        S.gunAim = false;
        S.character = character;
        fc.noAimRig = false;  // in the inventory fc.active and fc.yaw stay: the camera keeps its place
        if ((keyboard && !kbm) || mounted) {
            fc.active = false;
            fc.leftShoulder = false;  // the game's own cameras must not stay mirrored
        }
        AimWalkUpdate(character, 0.0f, 0.0f, false, 0.0f, (float)dt);
        NetLocalFrame(character, InLocomotion(character), false);
        return;  // On/Trg/Rel/TrgRep/LAngle/LMag stay exactly as PlayerPad::Update built them
    }
    if (c.meleeOnly) {
        S.swingT = -1;
        KbSetDriving(slot, false, false, true, false);
        MeleeOnlyControls(S, padObj, character, st, (float)dt, kbm ? &kb : nullptr);
        return;
    }
    if (kbm) S.kbRunPress = (phys & runBit) != 0;  // a run key held into the inventory keeps running there

    // Canonical (Type A) bits the character understands:
    //   RT = ready gun, X = fire / knife slash, LT = ready knife, LB = locate partner, RB = map,
    //   A = run / reload / quick turn (with stick down), B = partner, Y = inventory.
    // RunButton=RB swaps L3 and RB: RB runs and switches the aim pace, L3 locates the partner and swaps the
    // shoulder. Only with the mod's layout (with RemapButtons=0, RB keeps its own game action).
    uint32_t out = on & (P_START | P_UP | P_RIGHT | P_DOWN | P_LEFT | P_LS_DIRS | P_RS_DIRS);
    uint32_t forceTrg = 0, dropTrg = 0;  // press edges the game must see this frame / must not see
    bool stickSet = false;               // the mod wrote lAng/lMag: the LS direction bits follow them
    if (c.remapButtons) {
        if (aiming) out |= P_RT;
        if (knife && (!aiming || knifeWins)) out |= P_LT;
        // RT is the game's fire button everywhere, as in control type D (in the aim, and e.g. Wesker's Knee Cannon).
        if (phys & P_RT) out |= P_X;
        // RB (L3 with RunButton=RB): swap the camera to the other shoulder.  With ShoulderSwapAlways (default) it
        // works whether aiming or not, like a modern TPS: the physical LB keeps the knife outside the aim and the
        // RB-to-knife forwarding is gone.  In a scope the key stays the ScopeWalk zoom button.
        if (c.shoulderSwapAlways) {
            if ((physTrg & partnerBit) && !At<uint8_t>(character, ch::Scope)) fc.leftShoulder = !fc.leftShoulder;
        } else if (aimHeld) {
            if ((physTrg & partnerBit) && !At<uint8_t>(character, ch::Scope)) fc.leftShoulder = !fc.leftShoulder;
        } else if (phys & partnerBit) {
            out |= P_LB;
        }
        if (phys & P_BACK) out |= P_RB;  // map
        if (phys & P_Y) out |= P_Y;
        if (phys & P_B) out |= P_B;
        // R3 where the game has a use for it: in the aim (RPG-7 NVS night vision; StickClicks) and for Wesker outside
        // the aim (the dash; WeskerDash). Outside the aim it also puts the camera behind you (below).
        if (!kbm && (phys & P_R3) && (aimHeld ? c.stickClicks : (c.weskerDash && IsWesker(character)))) out |= P_R3;
        if (tauntPress) {
            out |= P_R3 | P_L3;
            forceTrg |= P_L3;
        }
        // In the dash A (keyboard: the run key, as in the game) is the game's turn button.
        if (dash && (kbm ? (phys & runBit) : (phys & P_A))) out |= P_A;
    } else {
        out = on;
        fc.leftShoulder = false;  // no swap button in the original layout
    }
    if (covered) fc.leftShoulder = false;  // the game's own cover camera must not be mirrored
    S.character = character;
    S.gunAim = c.remapButtons ? (aiming && !knifeWins) : (on & P_RT) != 0;

    // L3 (RB with RunButton=RB): run (toggle or hold); while aiming it switches between normal and fast walking.
    // The pace carries over: run -> aim starts fast, fast walk -> releasing the aim runs on.
    if (aimHeld && !S.wasAiming) S.aimFast = runMode == RunMode::Hold ? (phys & runBit) != 0 : S.runToggled;
    if (aimHeld) {
        if (runMode == RunMode::Hold) S.aimFast = (phys & runBit) != 0;
        else if (physTrg & runBit) S.aimFast = !S.aimFast;
    } else if (S.wasAiming && runMode == RunMode::Toggle) {
        S.runToggled = S.aimFast;
        S.runIdleTime = 0;
    }
    bool wantRun = false;
    if (runMode == RunMode::Toggle) {
        if (!aimHeld && (physTrg & runBit)) S.runToggled = !S.runToggled;
        if (stickMag < 0.3f) {
            S.runIdleTime += dt;
            if (S.runIdleTime > 0.5) S.runToggled = false;
        } else {
            S.runIdleTime = 0;
        }
        wantRun = S.runToggled;
    } else if (runMode == RunMode::Hold) {
        wantRun = (phys & runBit) != 0;
    }

    // X = reload. The game's own reload command (cPadEx+0x23, which it builds from RT+A on the pad or from the
    // keyboard's reload key): the reload starts straight from standing, walking or running, without raising the gun
    // first, and from the aim. The builder clears its commands before this pad update and only sets them after, so
    // the command reaches the action handlers this frame. Wall cover has its own reload (standing).
    if (c.oneButtonReload) {
        const uint32_t padEx = padObj - 0x4E8;  // PlayerPad = cPadEx+0x4E8
        // Not while a prompt or QTE that X answers is on screen (X answers it), and only when the game would reload
        // the gun in hand (a refused request would hold back RT for a while).
        if ((physTrg & P_X) && S.reload == ReloadPhase::Idle && !knife && Readable((void*)padEx, 0x30) &&
            (kbm || !PromptUp(slot, P_X)) && ReloadPossible(character)) {
            S.reload = ReloadPhase::Request;
            S.reloadT = 0;
            S.reloadTries = 0;
            S.reloadMove = !aiming && !(ride & 0xF00);
        }
        S.reloadT += dt;
        // The prompt check may come after the pad in a frame: a request that meets an X prompt right away is dropped.
        if (S.reload == ReloadPhase::Request && !kbm && S.reloadT < 0.15 && PromptUp(slot, P_X))
            S.reload = ReloadPhase::Idle;
        switch (S.reload) {
            case ReloadPhase::Request:
                if (action == plact::Reload) {
                    S.reload = ReloadPhase::Reloading;
                    S.reloadT = 0;
                    break;
                }
                // Walking, running, standing or aiming take the command in the frame it is given; anything else
                // (lowering the gun, a quick turn) only in its own window, so it is offered for a while.
                if (((InLocomotion(character) || action == plact::ReadyLoop) && ++S.reloadTries > 2) ||
                    S.reloadT > 0.4) {
                    S.reload = ReloadPhase::Idle;  // full magazine, no spare ammo, or not now
                    break;
                }
                At<uint8_t>(padEx, 0x23) = 1;
                break;
            case ReloadPhase::Reloading:
                if (action != plact::Reload) S.reload = ReloadPhase::Idle;
                break;
            default:
                break;
        }
        if (S.reload != ReloadPhase::Idle) out &= ~P_X;
    } else {
        S.reload = ReloadPhase::Idle;  // switched off in the ini meanwhile
    }
    if (S.reloadMove && (aimHeld || (S.reload == ReloadPhase::Idle && !AimWalkState(character)))) S.reloadMove = false;
    const bool reloadMove = S.reloadMove;
    // Switching weapons (d-pad, keys 1-9, the wheel): the switch plays standing; the leg layer keeps walking, as for
    // a reload on the move. While aiming, the aim walk carries on through the switch by itself (SwitchWalk too).
    const bool switching = action == plact::WeaponChange || action == plact::WeaponChangeReady;
    const bool changeMove =
        c.switchWalk && switching && !aimHeld && !(ride & 0x4F00) && !At<uint8_t>(character, ch::Scope);
    // Once a moving reload or switch may be cancelled (ch+0x2D7C 0x20000000), the game sees the stick and goes
    // straight on to walking or running instead of stopping for a frame.
    const bool handover = (reloadMove || changeMove) && (At<uint32_t>(character, 0x2D7C) & 0x20000000) &&
                          stickMag >= 0.3f;

    // B with the stick pulled back = quick turn (RE5: stick down + A). With the free camera "back" means toward the
    // camera. The camera swings behind the character during a quick turn; the held stick keeps its world direction
    // until it is released or moved.
    if (S.dirLock && (stickMag < 0.3f || (st.lx * S.lockSx + st.ly * S.lockSy) / stickMag < 0.7f)) S.dirLock = false;
    const float stickYaw = S.dirLock ? S.lockYaw : StickWorldYaw(fc, st.lx, st.ly);
    if (kbm) {
        // Keyboard: the quick turn key turns around (no stick-behind gesture); a held direction keeps its world
        // direction afterwards, as on the pad.
        if (c.quickTurn && quickKey && S.quickTurnT < 0 && !aimHeld && S.reload == ReloadPhase::Idle &&
            QuickTurnStance(character, action, idNow)) {
            const float target = WrapPi(CharacterYaw(character) + 3.14159265f);
            if (freeCam && fc.initialised && stickMag >= 0.3f) {
                S.dirLock = true;
                S.lockYaw = target;
                S.lockSx = st.lx / stickMag;
                S.lockSy = st.ly / stickMag;
            }
            if (freeCam && fc.initialised) {
                S.swingT = 0;
                S.swingFrom = fc.yaw;
                S.swingTo = target;
            }
            S.quickTurnT = 0;
        }
    } else if (c.quickTurn && (physTrg & P_B) && S.quickTurnT < 0 && !aimHeld && S.reload == ReloadPhase::Idle &&
               !PromptUp(slot, P_B)) {
        bool stickBack;
        if (freeCam && fc.initialised) {
            // The stick held toward the camera (any time, not only while the character still faces away): turn round
            // to face that way; the camera swings round after the character.
            stickBack = stickMag > 0.5f && st.ly < -0.64f * stickMag && QuickTurnStance(character, action, idNow);
            if (stickBack) {
                const float target = StickWorldYaw(fc, st.lx, st.ly);
                // Already facing that way (running toward the camera): no turn, only the camera goes behind.
                // Otherwise the game turns 180 degrees from where the character faces now (the steering may have
                // started turning it a frame before) and the steering takes it the rest of the way afterwards.
                if (fabsf(WrapPi(CharacterYaw(character) - target)) < Rad(90.0f)) {
                    stickBack = false;
                    S.bHeldFromTurn = true;
                }
                S.dirLock = true;
                S.lockYaw = target;
                S.lockSx = st.lx / stickMag;
                S.lockSy = st.ly / stickMag;
                S.swingT = 0;
                S.swingFrom = fc.yaw;
                S.swingTo = target;
            }
        } else {
            stickBack = lMag > 0.5f && fabsf(lAng) > 135.0f && QuickTurnStance(character, action, idNow);
        }
        if (stickBack) {
            S.quickTurnT = 0;
            S.bHeldFromTurn = true;
        }
    }

    // Free camera: the right stick orbits the camera, the left stick moves relative to it.
    float walkX = st.lx, walkY = st.ly;  // leg layer stick, in the character frame
    const bool busy = aimHeld || dash || (S.reload != ReloadPhase::Idle && !reloadMove);
    bool snapNow = false;
    bool backStep = false;  // BackStep: this frame steps back toward the camera
    if (freeCam) {
        // Aim where you look: raising the gun or knife turns the character to the camera, only where the game lets it
        // aim any way (free locomotion, lowering the gun or knife, a reload or a weapon switch, which go straight on
        // to the aim) or once the aim begins after a jump, a ladder or a pickup; never in cover, grabs, QTEs or the
        // dash (the game aims as it faces there).
        const bool pressNow = aimHeld && !S.wasAiming;
        if (pressNow) {
            S.snapPending = true;
            S.snapYaw = fc.yaw;
        }
        if (!aimHeld) S.snapPending = false;
        if (S.snapPending && fc.initialised && !(ride & 0x4F00) && !dash && !PromptUp(slot, P_LT | P_RT | P_LB)) {
            // On the aiming PRESS the facing must go to the free view immediately, whatever the game was doing:
            // the old condition also required a locomotion/transition state, so aiming from idle (and every scope
            // entry, where the scope camera is the weapon camera and never reaches this hook) kept the character's
            // old aim yaw -- the scope then pointed somewhere other than where the player was looking.  The later
            // frames still use the narrow test, which is what keeps a scripted turn from being undone.
            const bool stance =
                pressNow ? true
                         : (action == plact::ReadyBegin || idNow == aid::KnifeBegin);
            if (stance) {
                SetCharacterYaw(character, S.snapYaw);
                S.snapPending = false;
                snapNow = true;
            }
        }
        // dev: keep the orbit camera while aiming (reserved[24] & 1024), to look at the legs from outside
        const bool devOrbit = DevMode() && (Bridge()->reserved[24] & 1024);
        // A melee/knife swing is "busy", which used to turn the orbit off; melee.cpp then skips the game's action
        // camera for the swing, so NOBODY drove the view and the mouse did nothing at all during the slash.  While
        // such an action runs, let the orbit own the view (it is the camera the mod already shows in the melee).
        const bool camFreeAction = FreeCameraAction(character);
        fc.active = !busy || devOrbit || camFreeAction;
        // While the mod owns the view during an attack, the aim rig must go too.  camera.cpp only lets the orbit run
        // when cam::AimRig == 0; a knife thrown from an aim hold (KnifeFromGun) keeps the rig set, so the orbit was
        // skipped even though the action camera had been suppressed -- nobody updated the view and the mouse did
        // nothing.  noAimRig drops the rig for the duration of the action, which is the same mechanism the mod uses
        // for a reload on the move.
        fc.noAimRig = reloadMove || changeMove || devOrbit || camFreeAction;
        // Diagnostic: log every entry/exit of "the mod owns the view during an action" with the real action id, so
        // one playthrough shows which id the knife uses and whether the orbit took the camera.
        {
            static int lastCamFree = -1;
            const int nowFree = camFreeAction ? 1 : 0;
            if (nowFree != lastCamFree) {
                lastCamFree = nowFree;
                Log("camera owner -> %s (action %02X busy=%d active=%d)", nowFree ? "MOD" : "GAME",
                    (unsigned)At<uint32_t>(character, ch::ActionId), (int)busy, (int)fc.active);
            }
        }
        // The sniper scope is left entirely to the game.  Its view is the weapon camera (uCameraWep) and its aim is
        // the game's own scope aim; the mod must only stay out of the way, which is what the `!scopedNow` above does
        // (it no longer zeroes the game's mouse turn while scoped).  Everything the mod used to inject here -- the
        // orbit pitch drive, the lookV write in the controller hook -- is gone: two drivers fighting the stock one
        // is what kept the scope from following in the first place.
        if (busy && camFreeAction) {
            // Camera-only turn during the swing: the movement/steering part of the !busy block below must not run.
            if (kbm) {
                if (!JustActivated()) {
                    const float k = MouseUnitRadians() * c.mouseSensitivity;
                    const uint32_t inv = MouseInvert();
                    fc.yaw = WrapPi(fc.yaw - kb.mouseX * k * ((inv & 2) ? -1.0f : 1.0f));
                    fc.pitch += kb.mouseY * k * c.mouseVertical * ((inv & 1) ? -1.0f : 1.0f);
                }
            } else {
                const float rx = DeadZone(st.rx, 0.2f), ry = DeadZone(st.ry, 0.2f);
                fc.yaw = WrapPi(fc.yaw - rx * Rad(c.cameraSpeed) * (float)dt);
                fc.pitch -= (c.invertCameraY ? -ry : ry) * Rad(c.cameraPitchSpeed) * (float)dt;
            }
            if (fc.pitch > Rad(55.0f)) fc.pitch = Rad(55.0f);
            if (fc.pitch < Rad(-40.0f)) fc.pitch = Rad(-40.0f);
            if (physTrg & P_R3) {
                fc.yaw = CharacterYaw(character);
                fc.pitch = DefaultCameraPitch();
            }
            // Cover turns the free camera off entirely in this branch of the code (see the cover handling in
            // ApplyControls), so the right stick is always neutralised while the mod owns the view here.
            At<float>(padObj, pp::RAngle) = 0;
            At<float>(padObj, pp::RMag) = 0;
            out &= ~P_RS_DIRS;
        }
        if (aimHeld || dash) S.swingT = -1;
        if (!busy && S.swingT >= 0) {
            // Quick turn: the camera swings round behind the new facing (eased, about a third of a second).
            S.swingT += dt;
            const float u = S.swingT >= 0.35 ? 1.0f : (float)(S.swingT / 0.35);
            const float e = u * u * (3.0f - 2.0f * u);
            fc.yaw = WrapPi(S.swingFrom + WrapPi(S.swingTo - S.swingFrom) * e);
            if (u >= 1.0f) S.swingT = -1;
        }
        if (!busy) {
            if (kbm) {
                // The mouse turns the camera by the same angle per unit as the game's own mouse aim (and its invert
                // option); the first frame after the window comes back may carry motion made elsewhere.
                if (!JustActivated()) {
                    const float k = MouseUnitRadians() * c.mouseSensitivity;
                    const uint32_t inv = MouseInvert();
                    fc.yaw = WrapPi(fc.yaw - kb.mouseX * k * ((inv & 2) ? -1.0f : 1.0f));
                    fc.pitch += kb.mouseY * k * c.mouseVertical * ((inv & 1) ? -1.0f : 1.0f);
                }
            } else {
                const float rx = DeadZone(st.rx, 0.2f), ry = DeadZone(st.ry, 0.2f);
                fc.yaw = WrapPi(fc.yaw - rx * Rad(c.cameraSpeed) * (float)dt);
                // Up on the stick = look up (the orbit camera goes down and closer).
                fc.pitch -= (c.invertCameraY ? -ry : ry) * Rad(c.cameraPitchSpeed) * (float)dt;
            }
            if (fc.pitch > Rad(55.0f)) fc.pitch = Rad(55.0f);
            if (fc.pitch < Rad(-40.0f)) fc.pitch = Rad(-40.0f);
            if (physTrg & P_R3) {
                fc.yaw = CharacterYaw(character);
                fc.pitch = DefaultCameraPitch();
            }
            // The game must not turn the character with the right stick any more.
            At<float>(padObj, pp::RAngle) = 0;
            At<float>(padObj, pp::RMag) = 0;
            out &= ~P_RS_DIRS;
            // Only once the reload has started (a refused request leaves the walk as it is).
            const bool moveSteer = (reloadMove && c.reloadWalk && action == plact::Reload) || changeMove;
            if (stickMag >= 0.3f && fc.initialised && S.quickTurnT < 0 && (InLocomotion(character) || moveSteer)) {
                // BackStep: toward the camera without running steps back, facing where the camera looks (the game's
                // own walk back), as in the original game; a reload or switch on the move steps back too. A counts
                // only where it is the run (Run=Stock, the original buttons); otherwise it is the action button.
                const bool runHeld =
                    wantRun || (!kbm && (runMode == RunMode::Stock || !c.remapButtons) && (phys & P_A) != 0);
                backStep = c.backStep && !S.dirLock && !runHeld && st.ly < -0.85f * stickMag;
                const float want = backStep ? fc.yaw : stickYaw;
                const float cur = CharacterYaw(character);
                const float diff = WrapPi(want - cur);
                const float step = Rad(c.turnSpeed) * (float)dt;
                SetCharacterYaw(character, fabsf(diff) <= step ? want : WrapPi(cur + (diff > 0 ? step : -step)));
                if (moveSteer) {
                    walkX = 0.0f;  // reloading or switching on the move: face the way you go (or step back)
                    walkY = backStep ? -stickMag : stickMag;
                    if (handover) {
                        lAng = backStep ? 180.0f : 0.0f;
                        lMag = stickMag > 1.0f ? 1.0f : stickMag;
                        stickSet = true;
                    }
                } else {
                    lAng = backStep ? 180.0f : 0.0f;  // the game walks/runs straight ahead (or back), the mod steers
                    lMag = stickMag > 1.0f ? 1.0f : stickMag;
                    stickSet = true;
                }
            }
        }
    } else {
        S.swingT = -1;
        fc.active = false;
        fc.noAimRig = false;
    }
    if (kbm) {
        const bool orbit = freeCam && fc.initialised;
        if (!orbit && stickMag > 0.0f) {
            // No free camera (FreeCamera=0): WASD the way the game adds them itself, relative to the character, and
            // the mouse turns the body as in the game.
            lAng = atan2f(-st.lx, st.ly) * 57.29578f;
            lMag = stickMag > 1.0f ? 1.0f : stickMag;
            stickSet = true;
        }
        // With the free camera the game's own mouse turning of the body is off (it steers Wesker's dash); outside the
        // aim the mouse belongs to the camera, in the aim the game's mouse aim gets it (scaled by MouseSensitivity).
        // NOT in the scope: the scope is drawn by the weapon camera (uCameraWep), which the mod's orbit never
        // touches, so zeroing the game's mouse turn here leaves the scope with no way to pitch at all.  `orbit` is
        // "freeCam && fc.initialised" and stays true while aiming, which is why scoping was affected on every build.
        const bool scopedNow = At<uint8_t>(character, ch::Scope) != 0;
        if (orbit && !dash && !scopedNow) {
            At<float>(padObj, 0x200) = 0.0f;
            At<float>(padObj, 0x204) = 0.0f;
        }
        if (aimHeld) {
            At<float>(padObj, 0x20C) *= c.mouseSensitivity;
            At<float>(padObj, 0x210) *= c.mouseSensitivity * c.mouseVertical;
        } else if (orbit && !dash) {
            At<float>(padObj, 0x20C) = 0.0f;
            At<float>(padObj, 0x210) = 0.0f;
            KbSyncTargetYaw(character);  // keep the game's own turn target on the facing the mod set
        }
        // The game keeps its own aim/turn target at ch+0x2A10 (KbSyncTargetYaw writes it from the quaternion and
        // clears the 0x2A14 re-capture flag).  It has to be re-synced whenever the mod snaps the facing, and the
        // old `&& orbit` guard skipped exactly the case that needed it: on an aim press the orbit is already off
        // (the game's aim/scope camera owns the view), so the sniper scope kept aiming at the pre-snap target yaw.
        if (snapNow) KbSyncTargetYaw(character);
    }
    S.wasAiming = aimHeld;

    // Run = the game's run held: canonical A while moving forward, or without the free camera in control types C/D
    // the game's own L3 run (any direction, the right stick steers it); not while aiming or during a reload, except
    // when a moving reload or switch hands over to the run.
    // Only where the character can run (standing, walking, running, turning, lowering the gun or knife): in cover A
    // is the reload, in Wesker's dash a turn.
    const bool runStance = handover || InLocomotion(character) || action == plact::Turn180 ||
                           action == plact::ReadyEnd || idNow == aid::KnifeEnd;
    const bool canRun = !aimHeld && !dash && S.quickTurnT < 0 && runStance &&
                        (handover || InLocomotion(character) || (S.reload == ReloadPhase::Idle && !reloadMove));
    if (runMode != RunMode::Stock) {
        if (wantRun && canRun && lMag >= 0.3f) {
            // Without the free camera control types C/D run on L3 (A with the stick to the side turns on the spot);
            // chosen once per run, so no new L3 press appears while R3 is held (that would be a taunt).
            if (!S.runSig)
                S.runSig = (!kbm && !(freeCam && fc.initialised) && At<uint8_t>(padObj - 0x4E8, 0x4CE) &&
                            !(out & P_R3)) ? P_L3 : P_A;
            if (S.runSig == P_L3 || fabsf(lAng) < 100.0f) out |= S.runSig;
        } else {
            S.runSig = 0;
        }
    } else if ((phys & P_A) && canRun) {
        out |= P_A;
    }
    // The game's own reload while aiming, LT + A (with the RE3 layout A is otherwise the prompt button).
    if (c.remapButtons && !kbm && aiming && !knifeWins && (phys & P_A) && !PromptUp(slot, P_A)) out |= P_A;

    // Walk while aiming / reloading (legs from the walk clip, the upper body keeps its action).
    const uint32_t actionId = At<uint32_t>(character, ch::ActionId);
    // Each kind of walking has its own switch: some players like to walk with the knife and reload on the move
    // but shoot standing.
    const bool knifeState = actionId >= aid::KnifeBegin && actionId <= aid::KnifeAttack;
    // In a scope the left stick zooms (control types B and D): no walking there, unless ScopeWalk, where RB held
    // gives the stick back to the zoom (the legs stop meanwhile) and the keyboard zooms with the wheel as usual. In A
    // and C the left stick aims in the scope.
    const bool scope = At<uint8_t>(character, ch::Scope) != 0;
    const bool scopeWalk = scope && c.scopeWalk && (kbm || PadAimsWithRightStick(character));
    const bool zoomHold = scope && !kbm && (phys & partnerBit);
    const bool walkAllowed = ((aiming && !knifeWins && c.gunWalk) || (knife && (!aiming || knifeWins) && c.knifeWalk) ||
                              (reloadMove && c.reloadWalk) || (action == plact::ReadyEnd && c.gunWalk) ||
                              (knifeState && !aimHeld && c.knifeWalk) || changeMove) &&
                             !(switching && !c.switchWalk) && (!scope || (scopeWalk && !zoomHold)) &&
                             !(ride & 0x4000);  // riding: anchored
    if (kbm)
        KbSetDriving(slot, (InLocomotion(character) || AimWalkState(character) || changeMove ||
                            (aimHeld && switching) || S.quickTurnT >= 0 || action == plact::Turn180) &&
                               (!scope || scopeWalk),
                     false, walkAllowed, freeCam && fc.initialised);
    // A reload or switch on the move keeps the gait: running plays the weapon set's run clip at the run speed (with
    // the hips' own bob), walking the walk at the normal walking speed.  The knife is deliberately NOT included:
    // the .x build tried that and lost the omnidirectional movement the leg layer was giving during a slash.
    const bool keepGait = reloadMove || changeMove;
    const bool runIntent = runMode == RunMode::Stock ? (phys & (kbm ? runBit : P_A)) != 0 : wantRun;
    const bool runGait = keepGait && runIntent && stickMag >= 0.3f;
    const float pace = runGait    ? MoveSpeedFor(character, 5)
                       : keepGait ? MoveSpeedFor(character, 2)
                       : S.aimFast ? c.aimWalkFastSpeed
                                   : c.aimWalkAnimSpeed;
    // The aim, the knife or a reload asked for this frame while walking: the legs keep walking from this frame on.
    // (Not the reload with a full magazine, which the game turns down.)
    const bool reloadPre = S.reload == ReloadPhase::Request && S.reloadTries == 1 && reloadMove && c.reloadWalk &&
                           ReloadPossible(character);
    const bool preStart = InLocomotion(character) && stickMag >= 0.3f &&
                          ((c.remapButtons && (physTrg & P_LT) && c.gunWalk) ||
                           (c.remapButtons && (physTrg & P_LB) && !aiming && c.knifeWalk) || reloadPre);
    if (reloadPre && freeCam && fc.initialised) {  // the legs walk the way the character faces, as moveSteer does
        walkX = 0.0f;
        walkY = backStep ? -stickMag : stickMag;
    }
    const bool aimWalking =
        AimWalkUpdate(character, walkX, walkY, walkAllowed, pace, (float)dt, runGait, keepGait, kbm, preStart);
    // In the preStart frame the game is still walking: it keeps the stick (if it turns the action down, the walk
    // just goes on).
    if (aimWalking && !handover && !(preStart && InLocomotion(character))) {
        lAng = 0.0f;
        lMag = 0.0f;
        stickSet = true;
    }
    NetLocalFrame(character, InLocomotion(character), aimWalking);

    // The B of a quick turn: no partner command while the turn runs; afterwards it counts as held without a press, so
    // the d-pad gives partner orders and letting go gives no "Come on".
    if (S.bHeldFromTurn) {
        if (!(phys & P_B)) S.bHeldFromTurn = false;
        else if (S.quickTurnT >= 0) out &= ~P_B;
        else dropTrg |= P_B;
    }
    if (S.quickTurnT >= 0) {
        out &= ~P_B;
        out |= P_A;
        // The game turns on an A press with the stick back, from lowering the gun only in its own window: the press
        // is given every frame until the turn starts (while running A is held already, so it would be no press).
        if (action != plact::Turn180) forceTrg |= P_A;
        lAng = 180.0f;
        lMag = 1.0f;
        stickSet = true;
        S.quickTurnT += dt;
        if (S.quickTurnT > (InLocomotion(character) || action == plact::Turn180 ? 0.2 : 0.6)) S.quickTurnT = -1;
    } else if (action == plact::Turn180 && freeCam && fc.initialised && stickMag >= 0.3f && !aimHeld) {
        // During the turn the held stick points where the character will face: the game goes on forward afterwards
        // (not a step back for a frame before the steering takes over).
        lAng = 0.0f;
        lMag = stickMag > 1.0f ? 1.0f : stickMag;
        stickSet = true;
    }

    // The LS direction bits follow the stick the mod wrote; otherwise they stay the game's own.
    if (stickSet || kbm) SetStickDirBits(out, lAng, lMag);

    // Rebuild the edge masks against what the game saw last frame (Old holds our previous output).
    const uint32_t old = At<uint32_t>(padObj, pp::Old);
    const uint32_t trg = ((out & ~old) | (forceTrg & out)) & ~dropTrg;
    on = out;
    At<uint32_t>(padObj, pp::Trg) = trg;
    At<uint32_t>(padObj, pp::Rel) = old & ~out;
    At<uint32_t>(padObj, pp::TrgRep) = trg | (At<uint32_t>(padObj, pp::Rep) & out);
#if RE3C_DEV
    Bridge()->reserved[0] = phys;
    Bridge()->reserved[1] = out;
    Bridge()->reserved[2] = padObj;
    Bridge()->reserved[3] = character;
    Bridge()->reserved[4] = (uint32_t)S.reload | (reloadMove ? 0x10 : 0) | (S.aimFast ? 0x100 : 0) |
                            (aimWalking ? 0x1000 : 0) | (S.runToggled ? 0x10000 : 0);
    Bridge()->reserved[31] = (dash ? 1u : 0u) | (knifeWins ? 2u : 0u) |
                             (S.snapPending ? 8u : 0u) | (S.runSig == P_L3 ? 0x10u : 0u) | (S.tauntL3 ? 0x20u : 0u) |
                             (S.bHeldFromTurn ? 0x40u : 0u) | (snapNow ? 0x80u : 0u);
#endif
}

#if RE3C_DEV
typedef bool(__fastcall* PFN_SetMotion)(uint32_t model, void* edx, uint32_t slot, uint32_t motionNo, float inter,
                                        float startFrame, float speed, uint32_t flags);

// Dev experiments driven through the bridge, executed on the game thread:
//   reserved[8] = op (1 = setMotion once, 2 = keep forcing while aiming, 3 = stop forcing)
//   reserved[9] = slot, reserved[10] = motionNo, reserved[11] = inter frames (float bits),
//   reserved[12] = slot weight (float bits) for op 2
static void DevExperiments(uint32_t character) {
    BridgeShared* b = Bridge();
    static uint32_t keepSlot = 0, keepMotion = 0xFFFF;
    static float keepWeight = 1.0f;
    const uint32_t op = b->reserved[8];
    if (op) {
        b->reserved[8] = 0;
        if (op == 1) {
            auto setMotion = (PFN_SetMotion)kFnSetMotion;
            float inter = *(float*)&b->reserved[11];
            bool r = setMotion(character, nullptr, b->reserved[9], b->reserved[10], inter, 0.0f, 1.0f, 0);
            Log("dev: setMotion(slot %u, %04x) -> %d", b->reserved[9], b->reserved[10], r);
        } else if (op == 2) {
            keepSlot = b->reserved[9];
            keepMotion = b->reserved[10];
            keepWeight = *(float*)&b->reserved[12];
            Log("dev: keep slot %u motion %04x w %.2f", keepSlot, keepMotion, keepWeight);
        } else if (op == 3) {
            keepMotion = 0xFFFF;
        } else if (op == 4) {
            // Run a fake hit by this character through the player damage scaler (and the mod's hook):
            // reserved[9] = hit param (1 knife, 2-9 guns), reserved[10] = base damage; result in reserved[13].
            static uint8_t di[0x100];
            memset(di, 0, sizeof(di));
            *(uint32_t*)(di + 0x20) = character;
            *(uint32_t*)(di + 0x9C) = b->reserved[9];
            const uint32_t game = ((uint32_t(__cdecl*)())0x00768C10)();
            const int r = ((int(__fastcall*)(uint32_t, void*, uint32_t, int))0x0071B010)(
                game, nullptr, (uint32_t)(uintptr_t)di, (int)b->reserved[10]);
            b->reserved[13] = (uint32_t)r;
            Log("dev: damage test hit param %u base %u -> %d", b->reserved[9], b->reserved[10], r);
        }
    }
    if (keepMotion != 0xFFFF) {
        const uint32_t slotObj = character + ch::Motion0 + keepSlot * ch::MotionStride;
        if (At<uint16_t>(slotObj, 4) != (uint16_t)keepMotion) {
            auto setMotion = (PFN_SetMotion)kFnSetMotion;
            setMotion(character, nullptr, keepSlot, keepMotion, 8.0f, 0.0f, 1.0f, 0);
        }
        if (keepSlot > 0) At<float>(slotObj, 0x14) = keepWeight;
    }
}
#endif

// ------------------------------------------------------------------------------------------------ held X with A
// Two kinds of object read the raw X button themselves after their prompt started them: held switches (uOmSwitch,
// read at 0xB370CA: released once X is let go) and the co-op "push together" boxes (0xA9A26D: ready while X is held).
// With the RE3 layout their prompt shows A, so there A held counts as X too. Patched from the game thread.
static uint32_t __fastcall RawOnHoldX(uint32_t spad, void* edx, uint32_t index) {
    uint32_t v = ((uint32_t(__fastcall*)(uint32_t, void*, uint32_t))0x007997C0)(spad, edx, index);
    const Config& c = Cfg();
    if (c.enabled && c.remapButtons && !c.meleeOnly && !c.interactX && (v & P_A)) v |= P_X;
    return v;
}

static void InstallRawHoldPatches() {
    static bool done = false;
    if (done) return;
    done = true;
    struct Site { uint32_t call; BYTE tail[5]; size_t n; };
    static const Site sites[] = {
        {0x00B370CA, {0xC1, 0xE8, 0x0F}, 3},              // shr eax,0Fh
        {0x00A9A26D, {0xA9, 0x00, 0x80, 0x00, 0x00}, 5},  // test eax,8000h
    };
    for (const Site& s : sites) {
        const bool ok = Readable((void*)s.call, 10) && At<uint8_t>(s.call, 0) == 0xE8 &&
                        s.call + 5 + At<int32_t>(s.call, 1) == 0x007997C0 && SafeEqual((void*)(s.call + 5), s.tail, s.n);
        DWORD old;
        if (ok && VirtualProtect((void*)s.call, 5, PAGE_EXECUTE_READWRITE, &old)) {
            At<int32_t>(s.call, 1) = (int32_t)((uint32_t)(uintptr_t)&RawOnHoldX - (s.call + 5));
            VirtualProtect((void*)s.call, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)s.call, 5);
            Log("held X with A: %08x", s.call);
        } else {
            Log("held X with A: unexpected code at %08x, left alone", s.call);
        }
    }
}

// Split screen: the game updates the local players' pads on its worker threads, at the same time; the mod's own
// per-player work shares state (the ini, prompts, the leg layer, the keyboard layer), so it runs one player at a time.
static CRITICAL_SECTION g_padLock;
static volatile LONG g_padLockInit = 0;

static void __fastcall HookPlayerPadUpdate(uint32_t self, void* edx) {
    oPlayerPadUpdate(self, edx);
    if (InterlockedCompareExchange(&g_padLockInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_padLock);
        InterlockedExchange(&g_padLockInit, 2);
    }
    while (g_padLockInit != 2) Sleep(0);
    struct Lock {
        Lock() { EnterCriticalSection(&g_padLock); }
        ~Lock() { LeaveCriticalSection(&g_padLock); }
    } lock;
    PollConfig();
    UpdateColorFilter();
    uint32_t character = 0;
    if (IsLocalPlayerPad(self, &character)) {
        const Config& cfg = Cfg();
        // Inventory pad rewrite and raw-hold patches are only needed in full control modes.
        if (cfg.enabled && !cfg.meleeOnly) {
            InventoryPadUpdated(self);
            InstallRawHoldPatches();
        }
        if (cfg.enabled) {
            ApplyControls(self, character);
#if RE3C_DEV
            if (DevMode()) DevExperiments(character);
#endif
        } else {
            AimWalkUpdate(character, 0.0f, 0.0f, false, 0.0f, 0.0f);  // turned off mid-walk: the legs stop
        }
        // Remote leg-layer smoothing is free when Sync is off / no remote layers are live.
        if (cfg.coopSync) AimWalkRemoteTick();
    }
}

// ------------------------------------------------------------------------------------------------ context prompts
// Prompt requests carry a button type (request+0x20: 1 = X, 2 = Y, 3 = A, 4 = B ...) that selects both the button
// the game waits for (mask table 0xF58E50) and the icon it draws. Ordinary "X" prompts (doors, items, melee
// finishers) are turned into "A" prompts, as in RE3; strict QTE prompts (request+0x6C) keep their button.
// Keyboard prompts follow the player's key bindings for the original type, so they are left alone; a request
// changed while the pad was in use gets its type back when the player switches to the keyboard.
// Requests are reused; the prompt manager tells a reused one apart by its stamp (request+0x68).
struct ChangedPrompt { uint32_t req, stamp; };
static ChangedPrompt g_changed[32];
static uint32_t g_changedNext = 0;

static bool __fastcall HookActionCheck(uint32_t self, void* edx, uint32_t pad, uint32_t slot) {
    const Config& c = Cfg();
    const uint32_t req = Readable((void*)self, 0x40) ? At<uint32_t>(self, act::Request) : 0;
    if (req && Readable((void*)req, 0x80) && At<uint8_t>(req, act::ReqStrict) == 0 && Readable((void*)pad, 0x50) &&
        At<uint32_t>(pad, pp::PadIndex) <= 3) {
        uint32_t& type = At<uint32_t>(req, act::ReqType);
        const bool keyboard = KeyboardMode(slot);  // slot = the player index the game's own prompt check uses
#if RE3C_DEV
        Bridge()->reserved[15] = type;  // the prompt checked last and a counter (tools can see a prompt is up)
        Bridge()->reserved[16]++;
#endif
        if (c.enabled && c.remapButtons && !c.interactX && !keyboard) {
            const uint32_t devType = DevMode() ? Bridge()->reserved[23] : 0;  // dev: test on another prompt type
            if (type == act::TypeX || (devType && type == devType)) {
                type = act::TypeA;
                const uint32_t stamp = At<uint32_t>(req, 0x68);
                bool known = false;
                for (auto& e : g_changed) known |= e.req == req && e.stamp == stamp;
                if (!known) g_changed[g_changedNext++ % 32] = {req, stamp};
            }
        } else if (type == act::TypeA) {
            for (auto& e : g_changed)
                if (e.req == req) {
                    if (e.stamp == At<uint32_t>(req, 0x68)) type = act::TypeX;
                    e = {0, 0};
                }
        }
    }
    // The buttons the prompts on screen take, per player (strict QTEs too): the mod leaves those presses to them.
    if (req && Readable((void*)req, 0x80) && slot < 4) {
        const uint32_t type = At<uint32_t>(req, act::ReqType);
        if (type != 0 && type < 0x1E && At<uint8_t>(req, 0x0C) != 1) {
            const uint32_t mask = At<uint32_t>(act::MaskTable + type * 4, 0);
            PromptSeenNow(slot, mask);
#if RE3C_DEV
            Bridge()->reserved[30] = mask;
#endif
        }
    }
    return oActionCheck(self, edx, pad, slot);
}

// ------------------------------------------------------------------------------------------------ install
// Signature check for a code address that also accepts an execute-only page.
// SafeEqual() goes through Readable(), which requires a READ protection bit; a
// page mapped PAGE_EXECUTE only (no read) then compares as a mismatch even when
// the bytes are exactly right.  ActionSlot::Check was reported as a mismatch at
// 0x766F00 by the 01:18 run although the dumped image has the expected
// 53 56 8B F1 8B 46 0C 32 DB there, so that is the most likely cause.
static bool CodeMatches(uint32_t addr, const BYTE* sig, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    if ((const BYTE*)addr + n > (const BYTE*)mbi.BaseAddress + mbi.RegionSize) return false;
    if (memcmp((void*)addr, sig, n) == 0) return true;
    // Another component (RE5Fix's dinput8.dll) may already have hooked the entry.  A jump there is still hookable:
    // MinHook chains on top of it.  Without this the ActionSlot hook was skipped in every session.
    const uint8_t b0 = *(const uint8_t*)addr;
    return b0 == 0xE9 || b0 == 0xEB;
}

static bool HookAt(uint32_t addr, const BYTE* expect, size_t n, void* detour, void** orig, const char* name) {
    if (!CodeMatches(addr, expect, n)) {
        if (Readable((void*)addr, 8)) {
            const uint8_t* b = (const uint8_t*)addr;
            Log("hook %s: signature mismatch at %08x (bytes %02X %02X %02X %02X %02X %02X %02X %02X)",
                name, addr, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        } else {
            Log("hook %s: signature mismatch at %08x (page not readable)", name, addr);
        }
        return false;
    }
    if (MH_CreateHook((void*)addr, detour, orig) != MH_OK || MH_QueueEnableHook((void*)addr) != MH_OK) {
        Log("hook %s: MinHook failed", name);
        return false;
    }
    Log("hook %s at %08x", name, addr);
    return true;
}

void InitGameHooks() {
    LoadConfig();
    InitCameraHooks();
    InitAimWalkHooks();
    InitCrosshairHooks();
    InitDamageHooks();
    InitMeleeHooks();
    InitBalanceHooks();
    InitKeyboardHooks();
    static const BYTE sigPad[] = {0x83, 0xEC, 0x28, 0x0F, 0x57, 0xC0, 0x53, 0x56, 0x8B, 0xF1};
    bool ok = HookAt(kFnPlayerPadUpdate, sigPad, sizeof(sigPad), (void*)HookPlayerPadUpdate,
                     (void**)&oPlayerPadUpdate, "PlayerPad::Update");
    static const BYTE sigAct[] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x0C, 0x32, 0xDB};
    ok &= HookAt(kFnActionCheck, sigAct, sizeof(sigAct), (void*)HookActionCheck, (void**)&oActionCheck,
                 "ActionSlot::Check");
    // All hooks go live together: the game's threads are paused once instead of once per hook.
    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        Log("hooks: MH_ApplyQueued failed (%d)", (int)applied);
        ok = false;
    }
    BalanceHooksReady();
    Bridge()->hooks_ok = ok ? 1 : 0;
}

void OnRenderFrame(IDirect3DDevice9* /*dev*/) {}
