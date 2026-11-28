#pragma once

#include <cstdint>
#include "Enums/WidgetIDs.h"

enum eWidgetState {
	STATE_NONE,
	STATE_DISABLED
};

extern uintptr_t* g_pWidgets;

// Resolves the live CTouchInterface::m_pWidgets pointer from libGTASA.so's GOT
// slot (and updates g_pWidgets). Must be used instead of caching once at boot
// because AML plugins like libSAUtils64.so redirect the GOT slot to an expanded
// pNewWidgets array after Hooks::install().
uintptr_t* GetTouchWidgets();

sa::WidgetIDs GetWidgetTypeFromWidget(uintptr_t pWidget);

// True only for GTA's non-control HUD display/map widgets (Health/Weapon/Ammo,
// Radar tap-to-map, HelpText, Pause Map, Mission Timers, Vital Stats).
// All stock GTA touch buttons, joysticks, and pedals are suppressed so TouchHud
// owns 100% of gameplay touch rendering and multi-touch routing.
bool IsNativeDisplayWidget(sa::WidgetIDs id);

eWidgetState ProcessFixedWidget(uintptr_t pWidget);
void DisableAllStockTouchControls();
