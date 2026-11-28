#include "HookDomains.h"
#include "../Hooks.h"
#include "../Game.h"
#include "../TouchWidgets.h"
#include "../Engine/PatchRegistry.h"
#include "../Engine/Symbols.h"
#include "../../InputArbiter.h"
#include "../../Client.h"
#include <Memory.h>
#include <spdlog/spdlog.h>
#include <atomic>

DECL_HOOK(void, AND_TouchEvent, int type, int num, int posX, int posY)
{
	InputArbiter::routeGtaTouch(type, num, posX, posY, AND_TouchEvent);
}

DECL_HOOK(int, GetInputType)
{
	return 0;
}

// No HID device may claim to natively provide a control: DrawAll hides a
// touch widget whenever Replaces(mapping) is true, and on emulators a virtual
// keyboard device claims button mappings.
DECL_HOOK(int, CHID_Replaces, uint32_t mapping)
{
	return 0;
}

DECL_HOOK(int, CHID_Implements, uint32_t mapping)
{
	return 0;
}

DECL_HOOK(void, CWidget_Update, uintptr_t pWidget)
{
	if (!Game::IsGamePaused() && ProcessFixedWidget(pWidget) == STATE_DISABLED) {
		return;
	}
	CWidget_Update(pWidget);
}

std::atomic<bool> g_suppressWidgetSetEnabled{false};

DECL_HOOK(void, CWidget_SetEnabled, uintptr_t pWidget, bool bEnabled)
{
	if (g_suppressWidgetSetEnabled.load(std::memory_order_acquire)) {
		return;
	}
	if (!Game::IsGamePaused() && ProcessFixedWidget(pWidget) == STATE_DISABLED) {
		bEnabled = false;
	}
	CWidget_SetEnabled(pWidget, bEnabled);
}

void Hooks::installWidgetFixHooks()
{
	spdlog::info("Installing widget takeover hooks...");
	GetTouchWidgets();
	REG_HOOK(Boot, CWidget_Update, CWidget_Update, "Suppress stock GTA touch control widget updates");
	REG_HOOK(Boot, CWidget_SetEnabled, CWidget_SetEnabled, "Suppress stock GTA touch control widgets & remote ped leak");
}

void InstallInputHooks()
{
	REG_HOOK(Boot, AND_TouchEvent, AND_TouchEvent, "Touch routing (TouchHud/UI/CLEO/Monet/TextDraw)");
	REG_HOOK(Boot, CHID_GetInputType, GetInputType, "Keyboard/controller input type override");
	REG_HOOK(Boot, CHID_Replaces, CHID_Replaces, "HID mapping replaces override");
	REG_HOOK(Boot, CHID_Implements, CHID_Implements, "HID mapping implements override");
}
