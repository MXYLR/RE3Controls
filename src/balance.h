#pragma once
#include <stdint.h>

// Fire rate, walk/run speed outside the aim, the yellow colour filter (balance.cpp).
void InitBalanceHooks();
void BalanceHooksReady();  // after all hooks are applied: code patches may be made from now on
void UpdateColorFilter();  // apply NoColorFilter (cheap when unchanged; called after every ini reload)
float FireRateFor(uint32_t character);                      // 1 = unchanged; guns only, not the knife
float MoveSpeedFor(uint32_t character, uint32_t actionId);  // walk/run clip speed factor for that action
