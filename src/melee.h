#pragma once
#include <stdint.h>

// Free camera during melee attacks (MeleeCamera=1, and Mode=MeleeCamera).
void InitMeleeHooks();
bool IsMeleeAction(uint32_t actionId);  // the player's melee / context attack actions
// The player is held by an ordinary enemy (until shaken free or freed by the partner); not a kill, not a boss scene.
bool InEnemyGrab(uint32_t character);
// The free camera stays through what the character does now (MeleeCamera: melee attacks, GrabCamera: enemy grabs).
bool FreeCameraAction(uint32_t character);
