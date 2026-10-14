#pragma once
#include <stdint.h>

// Called every frame from the local player's pad update (game thread). stickX/stickY are in the character's frame
// (y = forward). Returns true while the leg layer walks.
bool AimWalkUpdate(uint32_t character, float stickX, float stickY, bool walkAllowed, float pace, float dtSec,
                   bool run = false, bool bob = false, bool smooth = false, bool preStart = false);
// True while the character is in an action the leg layer can run under (aim, fire, reload, knife).
bool AimWalkState(uint32_t character);
// AimWalkState or a weapon switch: the actions the leg layer can walk.
bool LegLayerState(uint32_t character);
// Leg clip the local player is playing right now (for the co-op partner).
// blend: 0, or 0x80 | 0x40 back | 0x20 right | weight x 31 for the directional blend (v1.8 partners).
bool AimWalkLocalState(uint32_t character, uint16_t* clip, float* rate, float* twist, uint8_t* blend, bool* bob);
// Apply / stop the leg layer on the partner's copy of a character (from a received packet).
void AimWalkRemote(uint32_t character, bool walking, uint16_t clip, float rate, float twist, const float* pos,
                   float yaw, uint8_t blend, bool bob);
void AimWalkRemoteTick();
void InitAimWalkHooks();
