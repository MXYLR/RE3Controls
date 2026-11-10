#pragma once
#include <stdint.h>

// Companion AI tuning (companion.cpp): hooks cPlIntelNpcThink::exec and, when [Characters] CompanionMove=1, shortens
// the partner's movement timers so it keeps repositioning while it shoots.
void InitCompanionHooks();
