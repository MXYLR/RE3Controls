#pragma once
#include <stdint.h>

// Keyboard and mouse (kbinput.cpp): the mod's RE3 controls for a player on keyboard/mouse.

// What the player does this frame, read from the game's own key bindings and its gated mouse fields.
struct KbFrame {
    bool aim, knife, fire, run, reload, partner, locate, map, inventory, quickTurn, cameraReset;
    float moveX, moveY;  // WASD: x right, y forward, length 0 or 1
    float mouseX, mouseY;  // mouse in the game's units (raw counts x MouseBaseSpeed x the in-game sensitivity); y down
};

void InitKeyboardHooks();
// The local player `player` (ch+0x2D9C) is on keyboard/mouse with the mod's keyboard layer on.
bool KbLayerFor(uint32_t character);
// Reads the frame; false while the game's input is locked (window in the background, system message).
bool KbRead(uint32_t character, uint32_t padObj, KbFrame* out);
// The mod drives this player this frame: the game's own WASD/turn reads in the character code answer 0;
// `inventory`: walking with the inventory open, the builder's aim/X/T reads answer 0 too; `keyAimWalk`: WASD walk
// in the aim (the game's key aim answers 0); `orbit`: the free camera turns the view (the look keys answer 0).
void KbSetDriving(uint32_t player, bool driving, bool inventory, bool keyAimWalk, bool orbit);
// Radians the game's own mouse aim turns per mouse unit (0.02 x aim speed x 1.2 x render width / 1280, in degrees).
float MouseUnitRadians();
// The in-game mouse invert option for this player: bit 0 vertical, bit 1 horizontal.
uint32_t MouseInvert();
// One frame after the window became active again: mouse motion collected meanwhile is not used.
bool JustActivated();
// Keep the game's target yaw (used by its own mouse turning) on the character's facing.
void KbSyncTargetYaw(uint32_t character);
