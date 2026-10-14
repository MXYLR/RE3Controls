#pragma once

// Player damage multiplier and the mouse-mode aim behaviours (no breathing, recoil kick) for pad players.
void InitDamageHooks();

// The game's own test (0xB5C4F0): this local character is played with keyboard/mouse right now.
bool KbMouse(uint32_t ch);
