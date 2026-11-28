#include "TouchWidgets.h"
#include "Engine/Symbols.h"
#include "Enums/WidgetIDs.h"

uintptr_t* g_pWidgets = nullptr;

uintptr_t* GetTouchWidgets()
{
	uintptr_t* live = eng::live::touchWidgets();
	if (live) {
		g_pWidgets = live;
	}
	return g_pWidgets;
}

sa::WidgetIDs GetWidgetTypeFromWidget(uintptr_t pWidget)
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets || !pWidget) return sa::WidgetIDs::NUM_WIDGETS;

	for (int i = 0; i < sa::WidgetIDs::NUM_WIDGETS; ++i) {
		if (widgets[i] == pWidget) {
			return static_cast<sa::WidgetIDs>(i);
		}
	}
	return sa::WidgetIDs::NUM_WIDGETS;
}

bool IsNativeDisplayWidget(sa::WidgetIDs id)
{
	switch (id) {
		case sa::WidgetIDs::WIDGET_MAP:
		case sa::WidgetIDs::WIDGET_PLAYER_INFO:
		case sa::WidgetIDs::WIDGET_RADAR:
		case sa::WidgetIDs::WIDGET_HELP_TEXT:
		case sa::WidgetIDs::WIDGET_MISSION_TIMERS:
		case sa::WidgetIDs::WIDGET_VITAL_STATS:
		case sa::WidgetIDs::WIDGET_SAVE_NOTIFICATION:
			return true;
		default:
			return false;
	}
}

eWidgetState ProcessFixedWidget(uintptr_t pWidget)
{
	const sa::WidgetIDs id = GetWidgetTypeFromWidget(pWidget);
	if (id == sa::WidgetIDs::NUM_WIDGETS) {
		return STATE_NONE;
	}
	return IsNativeDisplayWidget(id) ? STATE_NONE : STATE_DISABLED;
}

void DisableAllStockTouchControls()
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets) return;
	for (int i = 0; i < sa::WidgetIDs::NUM_WIDGETS; ++i) {
		const auto id = static_cast<sa::WidgetIDs>(i);
		if (!IsNativeDisplayWidget(id) && widgets[i]) {
			eng::fn::CWidget_SetEnabled(widgets[i], false);
		}
	}
}
