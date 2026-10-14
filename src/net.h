#pragma once
#include <stdint.h>

// Co-op: called every frame for the local player (game thread).
void NetLocalFrame(uint32_t character, bool inLocomotion, bool aimWalking);
// Where the partner's crosshair points (world), for their copy on this machine; false if not aiming.
bool NetRemoteAim(uint32_t character, float* aimPoint);
// Damage multipliers in effect: the ini's, or the host's while this machine is the guest of a modded host.
void NetDamageValues(float* mult, float* knife);
// Fire rate in effect (the host's for a guest), and walk/run speed factors for a character (a partner's copy
// uses the partner's own values).
float NetFireRate();
// Shotgun spread for every player's shots on this machine: the host's in co-op (1 = original without it).
float NetShotgunSpread();
void NetMoveSpeeds(uint32_t character, float* walk, float* run);
