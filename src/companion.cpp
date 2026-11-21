#include "common.h"
#include <MinHook.h>
#include "re5.h"
#include "config.h"
#include "companion.h"

// ---------------------------------------------------------------------------------------------------------------
// Companion AI tuning.
//
// The partner is driven by cPlIntelNpcThink::exec (0x00CBEF60), whose object holds 238 float tuning constants at
// +0x0004 .. +0x0460 (the map is documented by RE5PartnerAI's partner_ai_table.h).  The movement timers are the ones
// that decide how often the AI re-plans a step and how long it keeps walking, so shortening them makes the partner keep
// repositioning between shots instead of standing in one place while it fires.
//
// The engine reaches exec through the function entry (two plain calls in uPlIntel::update).  RE5PartnerAI redirects
// those two call sites, so this module hooks the entry instead: the two approaches do not collide.
// ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kFnThinkExec = 0x00CBEF60;
const BYTE kSigThinkExec[] = {0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x78, 0x08, 0x00, 0x00, 0x00, 0x0F, 0x84};

// Tuning slots, from the cPlIntelNpcThink constant map.
constexpr uint32_t kOffForwardMove = 0x00F0;  // TIMER_FORWARD_MOVE
constexpr uint32_t kOffMoveDir = 0x0110;      // TIMER_MOVE_DIR
constexpr uint32_t kOffMoveBack = 0x0114;     // TIMER_MOVE_BACK

typedef void(__fastcall* PFN_ThinkExec)(void* self, void* edx);
PFN_ThinkExec oThinkExec = nullptr;

int g_logged = 0;

void __fastcall HookThinkExec(void* self, void* edx) {
    const Config& c = Cfg();
    static uint32_t lastSelf = 0;
    static float base[3] = {0, 0, 0};
    static int logged = 0;
    static uint32_t calls = 0;
    if (c.enabled && c.companionMove && self && Readable((void*)((uintptr_t)self + 0x1160), 4)) {
        const uint32_t slots[3] = {kOffForwardMove, kOffMoveDir, kOffMoveBack};
        if (lastSelf != (uint32_t)(uintptr_t)self) {
            // New think object (level load, difficulty change): remember its stock values once.
            lastSelf = (uint32_t)(uintptr_t)self;
            for (int i = 0; i < 3; ++i) base[i] = re5::At<float>(lastSelf, slots[i]);
            logged = 0;
            Log("companion: think %08X base fwd %.2f dir %.2f back %.2f", lastSelf, base[0], base[1], base[2]);
        }
        // Absolute write from the remembered base: scaling every frame would decay the values to zero.
        const float scale = c.companionMoveScale > 0.05f ? c.companionMoveScale : 0.5f;
        for (int i = 0; i < 3; ++i) re5::At<float>(lastSelf, slots[i]) = base[i] * scale;
        if ((++calls % 600) == 1) {
            Log("companion: think calls=%u fwd %.2f dir %.2f back %.2f scale %.2f", calls,
                re5::At<float>(lastSelf, slots[0]), re5::At<float>(lastSelf, slots[1]),
                re5::At<float>(lastSelf, slots[2]), scale);
        }
    }
    if (oThinkExec) oThinkExec(self, edx);
}

}  // namespace

void InitCompanionHooks() {
    if (oThinkExec) return;
    if (!SafeEqual((const void*)kFnThinkExec, kSigThinkExec, sizeof(kSigThinkExec))) {
        Log("companion: think signature mismatch at %08X - left alone", kFnThinkExec);
        return;
    }
    if (MH_CreateHook((void*)kFnThinkExec, (void*)HookThinkExec, (void**)&oThinkExec) != MH_OK ||
        MH_QueueEnableHook((void*)kFnThinkExec) != MH_OK) {
        Log("companion: think hook failed");
        return;
    }
    Log("companion: think hook installed (CompanionMove=%d)", (int)Cfg().companionMove);
}
