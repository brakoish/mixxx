// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "preferences/usersettings.h"
class WDeckLab;
// Explicit developer-only offscreen check; ordinary launches do nothing.
void startDeckLabSmoke(WDeckLab* surface, UserSettingsPointer config);
