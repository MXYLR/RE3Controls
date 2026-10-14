#pragma once
#include <stdint.h>

// InventoryMove (inventory.cpp): walking and running with the inventory grid open.
bool InventoryMoveReady();                     // both call sites patched (done on the game thread, first local pad)
void InventoryPadUpdated(uint32_t padObj);     // right after PlayerPad::Update of a local player's pad
// This frame the command builder walks from a stick-only copy of the pad (`view` = the canonical bits it gets);
// the buttons as the game built them go back to the pad before the grid runs.
// keepRight: the right stick stays for the builder (the game steers when the free camera does not).
void InventoryStash(uint32_t padObj, uint32_t inventory, uint32_t view, bool keepRight);
