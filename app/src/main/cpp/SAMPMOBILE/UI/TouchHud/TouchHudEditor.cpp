#include "TouchHud.h"
#include "TouchHudTheme.h"

#include "../ImGuiRenderer.h"
#include "../../L10n.h"

#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace TouchHud {

namespace {

enum class EditorPage : uint8_t {
	OnFoot = 0,
	InVehicle = 1,
};

enum class SubPanel : uint8_t {
	None = 0,
	Prefs = 1,
	CustomButtons = 2,
};

enum class SelKind : uint8_t {
	None = 0,
	Standard = 1,
	Custom = 2,
};

enum class DragKind : uint8_t {
	None = 0,
	Control = 1,
	SliderScale = 2,
	SliderOpacity = 3,
	SliderLookSens = 4,
	SliderGlobalOpacity = 5,
};

struct UiHitBox {
	touch_hud::ScreenRect rect{};
	int actionId = 0;
};

struct EditorState {
	touch_hud::TouchHudLayout draft{};
	EditorPage page = EditorPage::OnFoot;
	SubPanel subPanel = SubPanel::None;

	SelKind selKind = SelKind::Standard;
	touch_hud::ControlId selControl = touch_hud::ControlId::FireRight;
	int selCustomIdx = 0;

	// Active drag state
	int dragPointer = -1;
	DragKind dragKind = DragKind::None;
	float dragOffsetX = 0.0f;
	float dragOffsetY = 0.0f;
	touch_hud::ScreenRect activeSliderRect{};

	float screenW = 1920.0f;
	float screenH = 1080.0f;

	// Hit boxes populated every render frame for direct touch clicks
	std::vector<UiHitBox> buttons;
	touch_hud::ScreenRect scaleSliderRect{};
	touch_hud::ScreenRect opacitySliderRect{};
	touch_hud::ScreenRect lookSensRect{};
	touch_hud::ScreenRect globalOpacityRect{};
};

std::mutex g_edMutex;
EditorState g_ed{};

// Action IDs for top bar & panels
enum ActionId : int {
	kActPageFoot = 100,
	kActPageVeh,
	kActPresetRp,
	kActPresetPubg,
	kActPresetMinimal,
	kActTogglePrefs,
	kActToggleCustom,
	kActReset,
	kActCancel,
	kActSave,

	kActSelToggleVis = 200,
	kActSelResetOne,

	// Pref toggles
	kActPrefAimMode = 300,
	kActPrefJoyMode,
	kActPrefAutoSprint,
	kActPrefLeftFire,
	kActPrefFireDragAim,
	kActPrefVehSteer,
	kActPrefVehPitch,
	kActPrefAltLatch,
	kActPrefRpExpanded,
	kActPrefPassAlways,
	kActCloseSubPanel,

	// Custom slot actions (base + slotIdx 0..7)
	kActCustomSelectBase = 400,
	kActCustomToggleEnable = 420,
	kActCustomCycleTemplate = 421,
	kActCustomCycleContext = 422,
};

struct CustomTemplate {
	const char* label;
	touch_hud::CustomActionType type;
	uint32_t keyMask;
	const char* cmd;
};

constexpr CustomTemplate kCustomTemplates[] = {
	{"ALT+Y",  touch_hud::CustomActionType::KeyCombo,    touch_hud::keyBit(touch_hud::SampKey::Walk) | touch_hud::keyBit(touch_hud::SampKey::Yes), ""},
	{"ALT+N",  touch_hud::CustomActionType::KeyCombo,    touch_hud::keyBit(touch_hud::SampKey::Walk) | touch_hud::keyBit(touch_hud::SampKey::No), ""},
	{"ALT+跳", touch_hud::CustomActionType::KeyCombo,    touch_hud::keyBit(touch_hud::SampKey::Walk) | touch_hud::keyBit(touch_hud::SampKey::Jump), ""},
	{"Q+E",    touch_hud::CustomActionType::KeyCombo,    touch_hud::keyBit(touch_hud::SampKey::LookLeft) | touch_hud::keyBit(touch_hud::SampKey::LookRight), ""},
	{"F+跳",   touch_hud::CustomActionType::KeyCombo,    touch_hud::keyBit(touch_hud::SampKey::SecondaryAttack) | touch_hud::keyBit(touch_hud::SampKey::Jump), ""},
	{"背包",   touch_hud::CustomActionType::ChatCommand, 0, "/inv"},
	{"引擎",   touch_hud::CustomActionType::ChatCommand, 0, "/engine"},
	{"锁车",   touch_hud::CustomActionType::ChatCommand, 0, "/lock"},
	{"状态",   touch_hud::CustomActionType::ChatCommand, 0, "/stats"},
	{"动作",   touch_hud::CustomActionType::ChatCommand, 0, "/animlist"},
};

const char* controlDisplayName(touch_hud::ControlId id)
{
	switch (id) {
		case touch_hud::ControlId::Joystick:       return L10N("Move Stick", "移动摇杆");
		case touch_hud::ControlId::FireRight:      return L10N("Right Fire (Drag Aim)", "右开火 (拖动跟枪)");
		case touch_hud::ControlId::FireLeft:       return L10N("Left Fixed Fire", "左固定开火");
		case touch_hud::ControlId::Aim:            return L10N("Aim / Scope / Block", "瞄准 / 开镜 / 格挡");
		case touch_hud::ControlId::Sprint:         return L10N("Sprint (Space)", "疾跑 (Space)");
		case touch_hud::ControlId::Jump:           return L10N("Jump (LShift)", "跳跃 (Shift)");
		case touch_hud::ControlId::Crouch:         return L10N("Crouch (C)", "蹲下 (C)");
		case touch_hud::ControlId::InteractF:      return L10N("Interact / Car (F)", "交互 / 上车 (F)");
		case touch_hud::ControlId::PassengerG:     return L10N("Passenger (G)", "副驾驶 (G)");
		case touch_hud::ControlId::WeaponPrevQ:    return L10N("Prev Weapon", "切枪左");
		case touch_hud::ControlId::WeaponNextE:    return L10N("Next Weapon", "切枪右");
		case touch_hud::ControlId::CamZoom:        return L10N("Cam Distance", "视角距离");
		case touch_hud::ControlId::RpStrip:        return L10N("RP Action Strip", "RP 拓展按键条");
		case touch_hud::ControlId::VehSteerLeft:   return L10N("Steer Left", "载具左转");
		case touch_hud::ControlId::VehSteerRight:  return L10N("Steer Right", "载具右转");
		case touch_hud::ControlId::VehJoystick:    return L10N("Vehicle Stick", "载具摇杆");
		case touch_hud::ControlId::VehAccel:       return L10N("Gas (W)", "油门 (W)");
		case touch_hud::ControlId::VehBrake:       return L10N("Brake / Rev (S)", "刹车 / 倒车 (S)");
		case touch_hud::ControlId::VehHandbrake:   return L10N("Handbrake / Drift", "手刹 / 漂移");
		case touch_hud::ControlId::VehFire:        return L10N("Vehicle Fire / Nitro", "载具开火 / 氮气");
		case touch_hud::ControlId::VehHorn:        return L10N("Horn / Hydra Jet (H)", "喇叭 / 战斗机喷口 (H)");
		case touch_hud::ControlId::VehLookLeftQ:   return L10N("Look Left / Yaw L", "左看 / 飞机左旋");
		case touch_hud::ControlId::VehLookRightE:  return L10N("Look Right / Yaw R", "右看 / 飞机右旋");
		case touch_hud::ControlId::VehExitF:       return L10N("Exit Vehicle (F)", "下车 (F)");
		case touch_hud::ControlId::VehSubMission2: return L10N("Sub / Gear (2)", "副任务 / 起落架 (2)");
		case touch_hud::ControlId::VehDriveBy:     return L10N("Passenger Drive-By", "副驾探头射击");
		case touch_hud::ControlId::VehPitchUp:     return L10N("Pitch Up / Wheelie", "俯仰上 / 特技压头");
		case touch_hud::ControlId::VehPitchDown:   return L10N("Pitch Down / Wheelie", "俯仰下 / 翘头特技");
		case touch_hud::ControlId::VehFlare:       return L10N("Hydra Flares", "战斗机干扰弹 (白光弹)");
		case touch_hud::ControlId::VehCamToggle:   return L10N("Vehicle Cam", "载具视角");
		default: return "Control";
	}
}

bool controlBelongsToPage(touch_hud::ControlId id, EditorPage page, const touch_hud::TouchHudPrefs& prefs)
{
	if (id == touch_hud::ControlId::RpStrip) return true;
	const bool vehCtrl = touch_hud::isVehicleControl(id);
	if (page == EditorPage::OnFoot) return !vehCtrl;
	if (!vehCtrl) return false;
	if (prefs.vehicleSteering == touch_hud::VehicleSteeringMode::Joystick) {
		if (id == touch_hud::ControlId::VehSteerLeft
			|| id == touch_hud::ControlId::VehSteerRight
			|| id == touch_hud::ControlId::VehPitchUp
			|| id == touch_hud::ControlId::VehPitchDown) {
			return false;
		}
	}
	else {
		if (id == touch_hud::ControlId::VehJoystick) return false;
	}
	return true;
}

void drawCenteredText(ImDrawList* dl, ImFont* font, float size, const ImVec2& c, const char* txt, ImU32 col)
{
	if (!dl || !font || !txt) return;
	const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
	dl->AddText(font, size, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), col, txt);
}

// Launcher-style warm-obsidian glass card with hairline border & subtle top sheen
void drawGlassCard(ImDrawList* dl, const ImVec2& pMin, const ImVec2& pMax, float radius,
	const LauncherPalette& pal, bool accentTopBar)
{
	// Drop shadow
	dl->AddRectFilled(ImVec2(pMin.x, pMin.y + 3.0f), ImVec2(pMax.x, pMax.y + 4.0f), rgba(0, 0, 0, 0.48f), radius + 1.0f);
	// Warm dark obsidian surface (#17161A)
	dl->AddRectFilled(pMin, pMax, rgba(LauncherPalette::kSurface, 0.95f), radius);
	// Top subtle sheen
	dl->AddLine(ImVec2(pMin.x + radius, pMin.y + 1.0f), ImVec2(pMax.x - radius, pMin.y + 1.0f), rgba(255, 255, 255, 0.10f), 1.0f);
	// Crisp hairline border
	dl->AddRect(pMin, pMax, rgba(255, 255, 255, 0.14f), radius, ImDrawCornerFlags_All, 1.2f);
	// Optional Launcher signature accent top strip
	if (accentTopBar) {
		dl->AddRectFilled(
			ImVec2(pMin.x + radius * 1.4f, pMin.y),
			ImVec2(pMax.x - radius * 1.4f, pMin.y + 3.0f),
			rgba(pal.accent, 0.95f), 2.0f);
	}
}

void drawButton(ImDrawList* dl, ImFont* font, std::vector<UiHitBox>& boxes,
	const touch_hud::ScreenRect& r, int actionId, const char* label,
	ImU32 bg, ImU32 border, ImU32 fg, float fontSz, bool leftAccentBar = false, ImU32 accentBarCol = 0)
{
	boxes.push_back({r, actionId});
	const ImVec2 pMin(r.x, r.y);
	const ImVec2 pMax(r.x + r.w, r.y + r.h);
	const float round = 8.0f;
	dl->AddRectFilled(pMin, pMax, bg, round);
	dl->AddRect(pMin, pMax, border, round, ImDrawCornerFlags_All, 1.35f);
	if (leftAccentBar && accentBarCol != 0) {
		dl->AddRectFilled(
			ImVec2(pMin.x + 2.5f, pMin.y + 7.0f),
			ImVec2(pMin.x + 6.0f, pMax.y - 7.0f),
			accentBarCol, 2.0f);
	}
	drawCenteredText(dl, font, fontSz, ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f), label, fg);
}

// Launcher GtaSaSliderView-inspired slider with dark surface2 track, accent fill, and crisp thumb
void drawSlider(ImDrawList* dl, ImFont* font, const touch_hud::ScreenRect& r,
	const char* label, float val, float minV, float maxV, float fontSz, const LauncherPalette& pal)
{
	const float t = std::clamp((val - minV) / std::max(0.0001f, maxV - minV), 0.0f, 1.0f);
	const ImVec2 pMin(r.x, r.y);
	const ImVec2 pMax(r.x + r.w, r.y + r.h);
	const float round = 8.0f;

	// Surface2 track backdrop
	dl->AddRectFilled(pMin, pMax, rgba(LauncherPalette::kSurface2, 0.96f), round);
	// Soft accent fill wash
	const float fillW = std::max(round * 1.2f, r.w * t);
	if (t > 0.005f) {
		dl->AddRectFilled(pMin, ImVec2(std::min(pMax.x, r.x + fillW), pMax.y), rgba(pal.accent, 0.34f), round);
		// Bottom accent progress bar (crisp 3px line matching GtaSaSliderView)
		dl->AddRectFilled(
			ImVec2(r.x + 3.0f, pMax.y - 4.0f),
			ImVec2(std::min(pMax.x - 3.0f, r.x + r.w * t), pMax.y - 1.0f),
			rgba(pal.accent, 0.96f), 2.0f);
	}
	dl->AddRect(pMin, pMax, rgba(255, 255, 255, 0.14f), round, ImDrawCornerFlags_All, 1.2f);

	// Crisp circular thumb knob
	const float kx = std::clamp(r.x + r.w * t, r.x + 8.0f, pMax.x - 8.0f);
	const float kr = std::min(11.0f, r.h * 0.34f);
	dl->AddCircleFilled(ImVec2(kx, r.y + r.h * 0.5f), kr + 1.5f, rgba(0, 0, 0, 0.40f), 18);
	dl->AddCircleFilled(ImVec2(kx, r.y + r.h * 0.5f), kr, rgba(LauncherPalette::kTextPrimary, 0.98f), 18);
	dl->AddCircle(ImVec2(kx, r.y + r.h * 0.5f), kr, rgba(pal.accent, 1.0f), 18, 2.2f);

	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s  %.0f%%", label, static_cast<double>(val * 100.0f));
	const ImVec2 center(r.x + r.w * 0.5f, r.y + r.h * 0.5f - 1.0f);
	drawCenteredText(dl, font, fontSz, ImVec2(center.x + 1.0f, center.y + 1.0f), buf, rgba(0, 0, 0, 0.72f));
	drawCenteredText(dl, font, fontSz, center, buf, rgba(LauncherPalette::kTextPrimary, 1.0f));
}

void applySliderX(float x, const touch_hud::ScreenRect& r, float minV, float maxV, float& target)
{
	if (r.w <= 1.0f) return;
	const float t = std::clamp((x - r.x) / r.w, 0.0f, 1.0f);
	target = minV + t * (maxV - minV);
}

void executeAction(int id)
{
	switch (id) {
		case kActPageFoot:
			g_ed.page = EditorPage::OnFoot;
			g_ed.selKind = SelKind::Standard;
			g_ed.selControl = touch_hud::ControlId::FireRight;
			break;
		case kActPageVeh:
			g_ed.page = EditorPage::InVehicle;
			g_ed.selKind = SelKind::Standard;
			g_ed.selControl = touch_hud::ControlId::VehAccel;
			break;
		case kActPresetRp:
			g_ed.draft = touch_hud::makePresetLayout(touch_hud::PresetId::RoleplayHeavy);
			break;
		case kActPresetPubg:
			g_ed.draft = touch_hud::makePresetLayout(touch_hud::PresetId::PubgCompetitive);
			break;
		case kActPresetMinimal:
			g_ed.draft = touch_hud::makePresetLayout(touch_hud::PresetId::TwoFingerMinimal);
			break;
		case kActTogglePrefs:
			g_ed.subPanel = (g_ed.subPanel == SubPanel::Prefs) ? SubPanel::None : SubPanel::Prefs;
			break;
		case kActToggleCustom:
			g_ed.subPanel = (g_ed.subPanel == SubPanel::CustomButtons) ? SubPanel::None : SubPanel::CustomButtons;
			break;
		case kActCloseSubPanel:
			g_ed.subPanel = SubPanel::None;
			break;
		case kActReset:
			g_ed.draft = touch_hud::makePresetLayout(g_ed.draft.activePreset);
			break;
		case kActCancel:
			closeEditor(false);
			break;
		case kActSave:
			g_ed.draft.clampAll();
			applyLayout(g_ed.draft, true);
			closeEditor(false); // already saved above
			break;

		case kActSelToggleVis:
			if (g_ed.selKind == SelKind::Standard) {
				auto& c = g_ed.draft.ctrl(g_ed.selControl);
				c.visible = !c.visible;
			}
			else if (g_ed.selKind == SelKind::Custom && g_ed.selCustomIdx >= 0 && g_ed.selCustomIdx < static_cast<int>(touch_hud::kMaxCustomButtons)) {
				auto& b = g_ed.draft.customButtons[g_ed.selCustomIdx];
				b.enabled = !b.enabled;
			}
			break;

		case kActSelResetOne:
			if (g_ed.selKind == SelKind::Standard) {
				const auto def = touch_hud::makePresetLayout(g_ed.draft.activePreset);
				g_ed.draft.ctrl(g_ed.selControl) = def.ctrl(g_ed.selControl);
			}
			break;

		// Preferences
		case kActPrefAimMode:
			g_ed.draft.prefs.aimMode = (g_ed.draft.prefs.aimMode == touch_hud::AimMode::Toggle)
				? touch_hud::AimMode::Hold : touch_hud::AimMode::Toggle;
			break;
		case kActPrefJoyMode:
			g_ed.draft.prefs.joystickMode = (g_ed.draft.prefs.joystickMode == touch_hud::JoystickMode::Floating)
				? touch_hud::JoystickMode::Fixed : touch_hud::JoystickMode::Floating;
			break;
		case kActPrefAutoSprint:
			g_ed.draft.prefs.autoSprintLock = !g_ed.draft.prefs.autoSprintLock;
			break;
		case kActPrefLeftFire: {
			int next = (static_cast<int>(g_ed.draft.prefs.leftFireMode) + 1) % 3;
			g_ed.draft.prefs.leftFireMode = static_cast<touch_hud::LeftFireMode>(next);
			break;
		}
		case kActPrefFireDragAim:
			g_ed.draft.prefs.fireRightDragAim = !g_ed.draft.prefs.fireRightDragAim;
			break;
		case kActPrefVehSteer:
			g_ed.draft.prefs.vehicleSteering = (g_ed.draft.prefs.vehicleSteering == touch_hud::VehicleSteeringMode::Arrows)
				? touch_hud::VehicleSteeringMode::Joystick : touch_hud::VehicleSteeringMode::Arrows;
			break;
		case kActPrefVehPitch: {
			int next = (static_cast<int>(g_ed.draft.prefs.vehiclePitchMode) + 1) % 3;
			g_ed.draft.prefs.vehiclePitchMode = static_cast<touch_hud::VehiclePitchMode>(next);
			break;
		}
		case kActPrefAltLatch:
			g_ed.draft.prefs.altSmartLatch = !g_ed.draft.prefs.altSmartLatch;
			break;
		case kActPrefRpExpanded:
			g_ed.draft.prefs.rpStripExpanded = !g_ed.draft.prefs.rpStripExpanded;
			break;
		case kActPrefPassAlways:
			g_ed.draft.prefs.showPassengerAlways = !g_ed.draft.prefs.showPassengerAlways;
			break;

		case kActCustomToggleEnable:
			if (g_ed.selCustomIdx >= 0 && g_ed.selCustomIdx < static_cast<int>(touch_hud::kMaxCustomButtons)) {
				auto& b = g_ed.draft.customButtons[g_ed.selCustomIdx];
				b.enabled = !b.enabled;
				if (b.enabled && b.label[0] == '\0') {
					const auto& tpl = kCustomTemplates[g_ed.selCustomIdx % (sizeof(kCustomTemplates) / sizeof(kCustomTemplates[0]))];
					b.actionType = tpl.type;
					b.keyMask = tpl.keyMask;
					b.setLabel(tpl.label);
					b.setCommand(tpl.cmd);
					b.cx = 0.44f + (g_ed.selCustomIdx % 4) * 0.08f;
					b.cy = 0.56f + (g_ed.selCustomIdx / 4) * 0.14f;
				}
				g_ed.selKind = SelKind::Custom;
			}
			break;

		case kActCustomCycleTemplate:
			if (g_ed.selCustomIdx >= 0 && g_ed.selCustomIdx < static_cast<int>(touch_hud::kMaxCustomButtons)) {
				auto& b = g_ed.draft.customButtons[g_ed.selCustomIdx];
				constexpr size_t kCount = sizeof(kCustomTemplates) / sizeof(kCustomTemplates[0]);
				size_t cur = 0;
				for (size_t i = 0; i < kCount; ++i) {
					if (std::strcmp(b.label, kCustomTemplates[i].label) == 0) {
						cur = (i + 1) % kCount;
						break;
					}
				}
				const auto& tpl = kCustomTemplates[cur];
				b.enabled = true;
				b.actionType = tpl.type;
				b.keyMask = tpl.keyMask;
				b.setLabel(tpl.label);
				b.setCommand(tpl.cmd);
				g_ed.selKind = SelKind::Custom;
			}
			break;

		case kActCustomCycleContext:
			if (g_ed.selCustomIdx >= 0 && g_ed.selCustomIdx < static_cast<int>(touch_hud::kMaxCustomButtons)) {
				auto& b = g_ed.draft.customButtons[g_ed.selCustomIdx];
				if (b.contextMask == touch_hud::kCtxOnFoot) b.contextMask = touch_hud::kCtxInVehicle;
				else if (b.contextMask == touch_hud::kCtxInVehicle) b.contextMask = touch_hud::kCtxBoth;
				else b.contextMask = touch_hud::kCtxOnFoot;
			}
			break;

		default:
			if (id >= kActCustomSelectBase && id < kActCustomSelectBase + static_cast<int>(touch_hud::kMaxCustomButtons)) {
				g_ed.selCustomIdx = id - kActCustomSelectBase;
				g_ed.selKind = SelKind::Custom;
			}
			break;
	}
}

} // namespace

void onEditorOpened(const touch_hud::TouchHudLayout& currentLayout, bool inVehicleNow)
{
	std::lock_guard<std::mutex> lock(g_edMutex);
	g_ed.draft = currentLayout;
	g_ed.page = inVehicleNow ? EditorPage::InVehicle : EditorPage::OnFoot;
	g_ed.subPanel = SubPanel::None;
	g_ed.selKind = SelKind::Standard;
	g_ed.selControl = inVehicleNow ? touch_hud::ControlId::VehAccel : touch_hud::ControlId::FireRight;
	g_ed.dragPointer = -1;
	g_ed.dragKind = DragKind::None;
}

bool handleEditorTouch(int type, int pointerId, float x, float y)
{
	std::lock_guard<std::mutex> lock(g_edMutex);
	const float W = std::max(320.0f, g_ed.screenW);
	const float H = std::max(240.0f, g_ed.screenH);

	if (type == 2 /* Down */) {
		if (g_ed.dragPointer != -1 && g_ed.dragPointer != pointerId) {
			return true; // Single-pointer clean editing
		}

		// 1. Check UI action buttons (top bar, bottom inspector, sub-panels)
		for (auto it = g_ed.buttons.rbegin(); it != g_ed.buttons.rend(); ++it) {
			if (it->rect.contains(x, y, 4.0f)) {
				const int act = it->actionId;
				executeAction(act);
				return true;
			}
		}

		// 2. Check sliders in Prefs subpanel
		if (g_ed.subPanel == SubPanel::Prefs) {
			if (g_ed.lookSensRect.contains(x, y, 8.0f)) {
				g_ed.dragPointer = pointerId;
				g_ed.dragKind = DragKind::SliderLookSens;
				g_ed.activeSliderRect = g_ed.lookSensRect;
				applySliderX(x, g_ed.lookSensRect, 0.25f, 3.0f, g_ed.draft.prefs.lookSensitivity);
				return true;
			}
			if (g_ed.globalOpacityRect.contains(x, y, 8.0f)) {
				g_ed.dragPointer = pointerId;
				g_ed.dragKind = DragKind::SliderGlobalOpacity;
				g_ed.activeSliderRect = g_ed.globalOpacityRect;
				applySliderX(x, g_ed.globalOpacityRect, 0.20f, 1.0f, g_ed.draft.prefs.globalOpacity);
				return true;
			}
			return true; // Swallow taps while modal sub-panel is open
		}

		if (g_ed.subPanel == SubPanel::CustomButtons) {
			return true;
		}

		// 3. Check Bottom Inspector sliders (Scale & Opacity)
		if (g_ed.selKind != SelKind::None) {
			if (g_ed.scaleSliderRect.contains(x, y, 8.0f)) {
				g_ed.dragPointer = pointerId;
				g_ed.dragKind = DragKind::SliderScale;
				g_ed.activeSliderRect = g_ed.scaleSliderRect;
				if (g_ed.selKind == SelKind::Standard) {
					applySliderX(x, g_ed.scaleSliderRect, 0.50f, 2.00f, g_ed.draft.ctrl(g_ed.selControl).scale);
				}
				else {
					applySliderX(x, g_ed.scaleSliderRect, 0.50f, 2.00f, g_ed.draft.customButtons[g_ed.selCustomIdx].scale);
				}
				return true;
			}
			if (g_ed.opacitySliderRect.contains(x, y, 8.0f)) {
				g_ed.dragPointer = pointerId;
				g_ed.dragKind = DragKind::SliderOpacity;
				g_ed.activeSliderRect = g_ed.opacitySliderRect;
				if (g_ed.selKind == SelKind::Standard) {
					applySliderX(x, g_ed.opacitySliderRect, 0.15f, 1.00f, g_ed.draft.ctrl(g_ed.selControl).opacity);
				}
				else {
					applySliderX(x, g_ed.opacitySliderRect, 0.15f, 1.00f, g_ed.draft.customButtons[g_ed.selCustomIdx].opacity);
				}
				return true;
			}
		}

		// 4. Hit-test draggable controls on canvas
		touch_hud::TouchHudCore geom;
		geom.setScreenSize(W, H);
		geom.setLayout(g_ed.draft);

		// Check Custom Buttons first
		for (size_t i = 0; i < touch_hud::kMaxCustomButtons; ++i) {
			const auto& b = g_ed.draft.customButtons[i];
			if (!b.enabled) continue;
			const bool pageMatch = (g_ed.page == EditorPage::OnFoot)
				? ((b.contextMask & touch_hud::kCtxOnFoot) != 0)
				: ((b.contextMask & touch_hud::kCtxInVehicle) != 0);
			if (!pageMatch) continue;
			const auto c = geom.customButtonCircle(i);
			if (c.contains(x, y, 14.0f)) {
				g_ed.selKind = SelKind::Custom;
				g_ed.selCustomIdx = static_cast<int>(i);
				g_ed.dragPointer = pointerId;
				g_ed.dragKind = DragKind::Control;
				g_ed.dragOffsetX = c.cx - x;
				g_ed.dragOffsetY = c.cy - y;
				return true;
			}
		}

		// Check RP Strip bounding box
		{
			touch_hud::ContextCapabilities dummyCaps{};
			dummyCaps.context = touch_hud::HudContext::OnFoot;
			geom.setCapabilities(dummyCaps, 0);
			geom.setRpExpanded(true);
			std::array<touch_hud::RpStripLayoutItem, static_cast<size_t>(touch_hud::RpStripItem::Count)> items{};
			touch_hud::ScreenRect rpBounds{};
			geom.computeRpStripItems(items, &rpBounds);
			if (rpBounds.w > 0.0f && rpBounds.contains(x, y, 10.0f)) {
				const auto& rp = g_ed.draft.ctrl(touch_hud::ControlId::RpStrip);
				g_ed.selKind = SelKind::Standard;
				g_ed.selControl = touch_hud::ControlId::RpStrip;
				g_ed.dragPointer = pointerId;
				g_ed.dragKind = DragKind::Control;
				g_ed.dragOffsetX = rp.cx * W - x;
				g_ed.dragOffsetY = rp.cy * H - y;
				return true;
			}
		}

		// Check Standard Circular Controls
		touch_hud::ControlId bestId = touch_hud::ControlId::Count;
		float bestDistSq = 1e18f;
		for (size_t i = 0; i < touch_hud::kControlCount; ++i) {
			const auto id = static_cast<touch_hud::ControlId>(i);
			if (id == touch_hud::ControlId::RpStrip) continue;
			if (!controlBelongsToPage(id, g_ed.page, g_ed.draft.prefs)) continue;
			const auto c = geom.controlCircle(id);
			if (c.contains(x, y, 14.0f)) {
				const float dx = x - c.cx;
				const float dy = y - c.cy;
				const float d2 = dx * dx + dy * dy;
				if (d2 < bestDistSq) {
					bestDistSq = d2;
					bestId = id;
				}
			}
		}
		if (bestId != touch_hud::ControlId::Count) {
			const auto c = geom.controlCircle(bestId);
			g_ed.selKind = SelKind::Standard;
			g_ed.selControl = bestId;
			g_ed.dragPointer = pointerId;
			g_ed.dragKind = DragKind::Control;
			g_ed.dragOffsetX = c.cx - x;
			g_ed.dragOffsetY = c.cy - y;
			return true;
		}
		return true;
	}

	if (type == 3 /* Move */ && pointerId == g_ed.dragPointer) {
		switch (g_ed.dragKind) {
			case DragKind::Control: {
				const float nx = std::clamp((x + g_ed.dragOffsetX) / W, 0.03f, 0.97f);
				const float ny = std::clamp((y + g_ed.dragOffsetY) / H, 0.08f, 0.94f);
				if (g_ed.selKind == SelKind::Standard) {
					auto& c = g_ed.draft.ctrl(g_ed.selControl);
					c.cx = nx;
					c.cy = ny;
				}
				else if (g_ed.selKind == SelKind::Custom) {
					auto& b = g_ed.draft.customButtons[g_ed.selCustomIdx];
					b.cx = nx;
					b.cy = ny;
				}
				break;
			}
			case DragKind::SliderScale:
				if (g_ed.selKind == SelKind::Standard) {
					applySliderX(x, g_ed.activeSliderRect, 0.50f, 2.00f, g_ed.draft.ctrl(g_ed.selControl).scale);
				}
				else if (g_ed.selKind == SelKind::Custom) {
					applySliderX(x, g_ed.activeSliderRect, 0.50f, 2.00f, g_ed.draft.customButtons[g_ed.selCustomIdx].scale);
				}
				break;
			case DragKind::SliderOpacity:
				if (g_ed.selKind == SelKind::Standard) {
					applySliderX(x, g_ed.activeSliderRect, 0.15f, 1.00f, g_ed.draft.ctrl(g_ed.selControl).opacity);
				}
				else if (g_ed.selKind == SelKind::Custom) {
					applySliderX(x, g_ed.activeSliderRect, 0.15f, 1.00f, g_ed.draft.customButtons[g_ed.selCustomIdx].opacity);
				}
				break;
			case DragKind::SliderLookSens:
				applySliderX(x, g_ed.activeSliderRect, 0.25f, 3.00f, g_ed.draft.prefs.lookSensitivity);
				break;
			case DragKind::SliderGlobalOpacity:
				applySliderX(x, g_ed.activeSliderRect, 0.20f, 1.00f, g_ed.draft.prefs.globalOpacity);
				break;
			default:
				break;
		}
		return true;
	}

	if (type == 1 /* Up */ && pointerId == g_ed.dragPointer) {
		g_ed.dragPointer = -1;
		g_ed.dragKind = DragKind::None;
		return true;
	}

	return true;
}

void renderTouchHudEditor(ImGuiRenderer* renderer, float screenW, float screenH)
{
	(void)renderer;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	ImFont* font = ImGui::GetFont();
	if (!dl || !font) return;

	const LauncherPalette pal = LauncherPalette::current();

	std::lock_guard<std::mutex> lock(g_edMutex);
	g_ed.screenW = std::max(320.0f, screenW);
	g_ed.screenH = std::max(240.0f, screenH);
	g_ed.buttons.clear();

	const float W = g_ed.screenW;
	const float H = g_ed.screenH;
	const float unit = std::min(W, H);
	const float uiFont = std::max(14.5f, unit * 0.021f);

	// 1. Launcher dark obsidian scrim (#070608) & subtle theme-accent alignment grid
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), rgba(LauncherPalette::kBgDeep, 0.62f));
	const float step = unit * 0.08f;
	const ImU32 gridCol = rgba(pal.accent, 0.085f);
	for (float gx = step; gx < W; gx += step) {
		dl->AddLine(ImVec2(gx, 0), ImVec2(gx, H), gridCol, 1.0f);
	}
	for (float gy = step; gy < H; gy += step) {
		dl->AddLine(ImVec2(0, gy), ImVec2(W, gy), gridCol, 1.0f);
	}
	// Subtle center crosshair guide lines
	dl->AddLine(ImVec2(W * 0.5f, 0), ImVec2(W * 0.5f, H), rgba(pal.accent, 0.16f), 1.2f);
	dl->AddLine(ImVec2(0, H * 0.5f), ImVec2(W, H * 0.5f), rgba(pal.accent, 0.16f), 1.2f);

	// 2. Render live preview of all controls on the current page
	touch_hud::TouchHudCore geom;
	geom.setScreenSize(W, H);
	geom.setLayout(g_ed.draft);
	{
		touch_hud::ContextCapabilities caps{};
		caps.context = (g_ed.page == EditorPage::OnFoot)
			? touch_hud::HudContext::OnFoot
			: touch_hud::HudContext::InVehicleDriver;
		geom.setCapabilities(caps, 0);
		geom.setRpExpanded(true);
	}

	// Draw RP Strip preview
	{
		std::array<touch_hud::RpStripLayoutItem, static_cast<size_t>(touch_hud::RpStripItem::Count)> items{};
		touch_hud::ScreenRect rpBounds{};
		geom.computeRpStripItems(items, &rpBounds);
		const auto& rpCfg = g_ed.draft.ctrl(touch_hud::ControlId::RpStrip);
		const bool selected = (g_ed.selKind == SelKind::Standard && g_ed.selControl == touch_hud::ControlId::RpStrip);
		const float alpha = rpCfg.visible ? std::clamp(rpCfg.opacity, 0.28f, 1.0f) : 0.24f;
		const ImU32 bg = selected
			? rgba(pal.accentDeep, 0.90f)
			: rgba(LauncherPalette::kSurface, alpha * 0.88f);
		const ImU32 bd = selected
			? rgba(pal.accent, 1.0f)
			: (rpCfg.visible ? rgba(255, 255, 255, alpha * 0.34f) : rgba(LauncherPalette::kDanger, 0.72f));
		if (rpBounds.w > 0.0f) {
			if (selected) {
				dl->AddRect(
					ImVec2(rpBounds.x - 3.0f, rpBounds.y - 3.0f),
					ImVec2(rpBounds.x + rpBounds.w + 3.0f, rpBounds.y + rpBounds.h + 3.0f),
					rgba(pal.accent, 0.35f), 11.0f, ImDrawCornerFlags_All, 3.0f);
			}
			dl->AddRectFilled(ImVec2(rpBounds.x, rpBounds.y), ImVec2(rpBounds.x + rpBounds.w, rpBounds.y + rpBounds.h), bg, 9.0f);
			dl->AddRect(ImVec2(rpBounds.x, rpBounds.y), ImVec2(rpBounds.x + rpBounds.w, rpBounds.y + rpBounds.h), bd, 9.0f, ImDrawCornerFlags_All, selected ? 2.6f : 1.4f);
			drawCenteredText(dl, font, uiFont * 0.92f,
				ImVec2(rpBounds.x + rpBounds.w * 0.5f, rpBounds.y + rpBounds.h * 0.5f),
				L10N("RP Strip: Y  N  ALT  H  TAB  2  Num  ESC  F8", "RP按键条: Y  N  ALT  H  TAB  2  Num  ESC  F8"),
				rgba(selected ? pal.accentBright : LauncherPalette::kTextPrimary, alpha));
		}
	}

	// Draw Standard Controls on current page
	for (size_t i = 0; i < touch_hud::kControlCount; ++i) {
		const auto id = static_cast<touch_hud::ControlId>(i);
		if (id == touch_hud::ControlId::RpStrip) continue;
		if (!controlBelongsToPage(id, g_ed.page, g_ed.draft.prefs)) continue;

		const auto& cfg = g_ed.draft.ctrl(id);
		const auto c = geom.controlCircle(id);
		const bool selected = (g_ed.selKind == SelKind::Standard && g_ed.selControl == id);
		const float alpha = cfg.visible ? std::clamp(cfg.opacity * g_ed.draft.prefs.globalOpacity, 0.25f, 1.0f) : 0.22f;

		if (selected) {
			dl->AddCircle(ImVec2(c.cx, c.cy), c.r + 4.0f, rgba(pal.accent, 0.38f), 36, 3.2f);
		}
		const ImU32 fill = selected
			? rgba(pal.accentDeep, 0.90f)
			: rgba(LauncherPalette::kSurface, alpha * 0.80f);
		const ImU32 ring = selected
			? rgba(pal.accent, 1.0f)
			: (cfg.visible ? rgba(LauncherPalette::kTextPrimary, alpha * 0.65f) : rgba(LauncherPalette::kDanger, 0.72f));

		dl->AddCircleFilled(ImVec2(c.cx, c.cy), c.r, fill, 36);
		dl->AddCircle(ImVec2(c.cx, c.cy), c.r, ring, 36, selected ? 2.8f : 1.6f);

		const char* shortLbl = touch_hud::controlConfigKey(id);
		switch (id) {
			case touch_hud::ControlId::Joystick:       shortLbl = "JOY"; break;
			case touch_hud::ControlId::FireRight:      shortLbl = "FIRE"; break;
			case touch_hud::ControlId::FireLeft:       shortLbl = "L-FIRE"; break;
			case touch_hud::ControlId::Aim:            shortLbl = "AIM"; break;
			case touch_hud::ControlId::Sprint:         shortLbl = "RUN"; break;
			case touch_hud::ControlId::Jump:           shortLbl = "JUMP"; break;
			case touch_hud::ControlId::Crouch:         shortLbl = "C"; break;
			case touch_hud::ControlId::InteractF:      shortLbl = "F"; break;
			case touch_hud::ControlId::PassengerG:     shortLbl = "G"; break;
			case touch_hud::ControlId::WeaponPrevQ:    shortLbl = "<<"; break;
			case touch_hud::ControlId::WeaponNextE:    shortLbl = ">>"; break;
			case touch_hud::ControlId::CamZoom:        shortLbl = "CAM"; break;
			case touch_hud::ControlId::VehSteerLeft:   shortLbl = "<"; break;
			case touch_hud::ControlId::VehSteerRight:  shortLbl = ">"; break;
			case touch_hud::ControlId::VehJoystick:    shortLbl = "V-JOY"; break;
			case touch_hud::ControlId::VehAccel:       shortLbl = L10N("GAS", "油门"); break;
			case touch_hud::ControlId::VehBrake:       shortLbl = L10N("BRK", "刹车"); break;
			case touch_hud::ControlId::VehHandbrake:   shortLbl = L10N("DRIFT", "手刹"); break;
			case touch_hud::ControlId::VehFire:        shortLbl = L10N("FIRE", "开火"); break;
			case touch_hud::ControlId::VehHorn:        shortLbl = L10N("HORN", "喇叭"); break;
			case touch_hud::ControlId::VehLookLeftQ:   shortLbl = L10N("L-LOOK", "左看"); break;
			case touch_hud::ControlId::VehLookRightE:  shortLbl = L10N("R-LOOK", "右看"); break;
			case touch_hud::ControlId::VehExitF:       shortLbl = "F"; break;
			case touch_hud::ControlId::VehSubMission2: shortLbl = "2"; break;
			case touch_hud::ControlId::VehDriveBy:     shortLbl = "DB"; break;
			case touch_hud::ControlId::VehPitchUp:     shortLbl = "UP"; break;
			case touch_hud::ControlId::VehPitchDown:   shortLbl = "DN"; break;
			case touch_hud::ControlId::VehFlare:       shortLbl = "FLARE"; break;
			case touch_hud::ControlId::VehCamToggle:   shortLbl = "CAM"; break;
			default: break;
		}
		drawCenteredText(dl, font, std::max(14.0f, c.r * 0.40f), ImVec2(c.cx, c.cy), shortLbl,
			selected ? rgba(pal.accentBright, 1.0f) : rgba(LauncherPalette::kTextPrimary, 0.94f));
	}

	// Draw Enabled Custom Buttons on current page
	for (size_t i = 0; i < touch_hud::kMaxCustomButtons; ++i) {
		const auto& b = g_ed.draft.customButtons[i];
		if (!b.enabled) continue;
		const bool pageMatch = (g_ed.page == EditorPage::OnFoot)
			? ((b.contextMask & touch_hud::kCtxOnFoot) != 0)
			: ((b.contextMask & touch_hud::kCtxInVehicle) != 0);
		if (!pageMatch) continue;
		const auto c = geom.customButtonCircle(i);
		const bool selected = (g_ed.selKind == SelKind::Custom && g_ed.selCustomIdx == static_cast<int>(i));
		if (selected) {
			dl->AddCircle(ImVec2(c.cx, c.cy), c.r + 4.0f, rgba(pal.accent, 0.42f), 32, 3.2f);
		}
		dl->AddCircleFilled(ImVec2(c.cx, c.cy), c.r,
			selected ? rgba(pal.accentDeep, 0.94f) : rgba(LauncherPalette::kSurface2, 0.88f), 32);
		dl->AddCircle(ImVec2(c.cx, c.cy), c.r,
			selected ? rgba(pal.accent, 1.0f) : rgba(pal.accent, 0.72f), 32, selected ? 2.8f : 1.7f);
		drawCenteredText(dl, font, std::max(14.0f, c.r * 0.42f), ImVec2(c.cx, c.cy),
			b.label[0] ? b.label : "KEY",
			selected ? rgba(pal.accentBright, 1.0f) : rgba(LauncherPalette::kTextPrimary, 0.96f));
	}

	// 3. Top Toolbar (Launcher obsidian glass header with dynamic accent top bar)
	const float barH = std::max(46.0f, H * 0.074f);
	const float pad = 8.0f;
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, barH + pad), rgba(LauncherPalette::kSurface, 0.96f));
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, 2.5f), rgba(pal.accent, 0.85f));
	dl->AddLine(ImVec2(0, barH + pad), ImVec2(W, barH + pad), rgba(255, 255, 255, 0.13f), 1.2f);

	const float btnH = barH - 6.0f;
	const float btnY = pad * 0.5f + 4.0f;
	const float gap = 6.0f;

	// Right-aligned Reset / Cancel / Save (compute first so left items never collide on narrow screens)
	const float saveW = std::clamp(W * 0.090f, 84.0f, 140.0f);
	const float canW  = std::clamp(W * 0.064f, 64.0f, 102.0f);
	const float rstW  = std::clamp(W * 0.064f, 64.0f, 102.0f);
	const float rightZoneX = W - pad - saveW - gap - canW - gap - rstW - 10.0f;

	// Solid emerald Save, coral-tinted Cancel, surface2 Reset
	drawButton(dl, font, g_ed.buttons, {W - pad - saveW, btnY, saveW, btnH}, kActSave,
		L10N("Save", "保存生效"),
		rgba(LauncherPalette::kGood, 0.94f), rgba(LauncherPalette::kGood, 1.0f),
		rgba(LauncherPalette::kOnAccentDk, 1.0f), uiFont * 0.94f);
	drawButton(dl, font, g_ed.buttons, {W - pad - saveW - gap - canW, btnY, canW, btnH}, kActCancel,
		L10N("Cancel", "取消"),
		rgba(LauncherPalette::kDanger, 0.22f), rgba(LauncherPalette::kDanger, 0.85f),
		rgba(LauncherPalette::kDanger, 1.0f), uiFont * 0.90f);
	drawButton(dl, font, g_ed.buttons, {W - pad - saveW - gap * 2.0f - canW - rstW, btnY, rstW, btnH}, kActReset,
		L10N("Reset", "重置"),
		rgba(LauncherPalette::kSurface2, 0.95f), rgba(255, 255, 255, 0.16f),
		rgba(LauncherPalette::kTextDim, 1.0f), uiFont * 0.90f);

	// Left-aligned tabs & tools (auto-scaled to fit cleanly within rightZoneX)
	struct TopTabDef { float weight; int actId; const char* lbl; bool active; };
	const TopTabDef leftTabs[7] = {
		{1.05f, kActPageFoot,      L10N("On-Foot", "步行布局"),   g_ed.page == EditorPage::OnFoot},
		{1.05f, kActPageVeh,       L10N("Vehicle", "载具布局"),   g_ed.page == EditorPage::InVehicle},
		{0.96f, kActPresetRp,      L10N("RP Preset", "RP预设"),   g_ed.draft.activePreset == touch_hud::PresetId::RoleplayHeavy},
		{1.02f, kActPresetPubg,    L10N("PUBG", "PUBG竞技"),      g_ed.draft.activePreset == touch_hud::PresetId::PubgCompetitive},
		{0.98f, kActPresetMinimal, L10N("Minimal", "两指极简"),   g_ed.draft.activePreset == touch_hud::PresetId::TwoFingerMinimal},
		{1.08f, kActTogglePrefs,   L10N("Settings", "手感偏好"),  g_ed.subPanel == SubPanel::Prefs},
		{1.14f, kActToggleCustom,  L10N("+Custom", "+自定义键"),  g_ed.subPanel == SubPanel::CustomButtons},
	};
	const float availLeftW = std::max(320.0f, rightZoneX - pad - gap * 6.0f);
	constexpr float kTotalWeight = 1.05f + 1.05f + 0.96f + 1.02f + 0.98f + 1.08f + 1.14f;
	const float unitTabW = std::min(126.0f, availLeftW / kTotalWeight);
	float curX = pad;
	for (const auto& t : leftTabs) {
		const float bw = unitTabW * t.weight;
		const ImU32 bg = t.active ? rgba(pal.accent, 0.95f) : rgba(LauncherPalette::kSurface2, 0.92f);
		const ImU32 bd = t.active ? rgba(pal.accentBright, 1.0f) : rgba(255, 255, 255, 0.14f);
		const ImU32 fg = t.active ? rgba(pal.onAccent, 1.0f) : rgba(LauncherPalette::kTextPrimary, 0.92f);
		drawButton(dl, font, g_ed.buttons, {curX, btnY, bw, btnH}, t.actId, t.lbl, bg, bd, fg, uiFont * 0.88f);
		curX += bw + gap;
	}

	// 4. Bottom Inspector Bar (Launcher dark glass card with dynamic theme accent title & sliders)
	if (g_ed.subPanel == SubPanel::None && g_ed.selKind != SelKind::None) {
		const float inspH = std::max(56.0f, H * 0.090f);
		const float inspW = std::min(W - 24.0f, std::max(720.0f, W * 0.74f));
		const float inspX = (W - inspW) * 0.5f;
		const float inspY = H - inspH - 12.0f;
		drawGlassCard(dl, ImVec2(inspX, inspY), ImVec2(inspX + inspW, inspY + inspH), 12.0f, pal, false);

		// Left accent indicator pill (matching Launcher nav/row selection marker)
		dl->AddRectFilled(
			ImVec2(inspX + 4.0f, inspY + 12.0f),
			ImVec2(inspX + 8.0f, inspY + inspH - 12.0f),
			rgba(pal.accent, 0.95f), 2.0f);

		const char* selTitle = (g_ed.selKind == SelKind::Standard)
			? controlDisplayName(g_ed.selControl)
			: g_ed.draft.customButtons[g_ed.selCustomIdx].label;
		const float selScale = (g_ed.selKind == SelKind::Standard)
			? g_ed.draft.ctrl(g_ed.selControl).scale
			: g_ed.draft.customButtons[g_ed.selCustomIdx].scale;
		const float selOpacity = (g_ed.selKind == SelKind::Standard)
			? g_ed.draft.ctrl(g_ed.selControl).opacity
			: g_ed.draft.customButtons[g_ed.selCustomIdx].opacity;
		const bool selVis = (g_ed.selKind == SelKind::Standard)
			? g_ed.draft.ctrl(g_ed.selControl).visible
			: g_ed.draft.customButtons[g_ed.selCustomIdx].enabled;

		const float innerY = inspY + 10.0f;
		const float innerH = inspH - 20.0f;
		const float nameW = inspW * 0.24f;
		drawCenteredText(dl, font, uiFont * 0.95f, ImVec2(inspX + nameW * 0.5f + 8.0f, inspY + inspH * 0.5f),
			selTitle, rgba(pal.accent, 1.0f));

		const float sW = inspW * 0.245f;
		g_ed.scaleSliderRect   = {inspX + nameW + 14.0f, innerY, sW, innerH};
		g_ed.opacitySliderRect = {inspX + nameW + 24.0f + sW, innerY, sW, innerH};
		drawSlider(dl, font, g_ed.scaleSliderRect,   L10N("Size", "大小"), selScale, 0.50f, 2.00f, uiFont * 0.86f, pal);
		drawSlider(dl, font, g_ed.opacitySliderRect, L10N("Alpha", "透明"), selOpacity, 0.15f, 1.00f, uiFont * 0.86f, pal);

		const float visX = g_ed.opacitySliderRect.x + sW + 12.0f;
		const float visW = inspW * 0.11f;
		drawButton(dl, font, g_ed.buttons, {visX, innerY, visW, innerH}, kActSelToggleVis,
			selVis ? L10N("Shown", "显示") : L10N("Hidden", "隐藏"),
			selVis ? rgba(LauncherPalette::kGood, 0.22f) : rgba(LauncherPalette::kDanger, 0.22f),
			selVis ? rgba(LauncherPalette::kGood, 0.85f) : rgba(LauncherPalette::kDanger, 0.85f),
			selVis ? rgba(LauncherPalette::kGood, 1.0f) : rgba(LauncherPalette::kDanger, 1.0f),
			uiFont * 0.88f);

		if (g_ed.selKind == SelKind::Standard) {
			const float rX = visX + visW + 8.0f;
			const float rW = std::max(56.0f, (inspX + inspW - 12.0f) - rX);
			drawButton(dl, font, g_ed.buttons, {rX, innerY, rW, innerH}, kActSelResetOne,
				L10N("Def", "复原"),
				rgba(LauncherPalette::kSurface2, 0.95f), rgba(255, 255, 255, 0.16f),
				rgba(LauncherPalette::kTextPrimary, 0.92f), uiFont * 0.86f);
		}
	}

	// 5. Preferences Sub-Panel (Launcher Settings Card aesthetic)
	if (g_ed.subPanel == SubPanel::Prefs) {
		const float pw = std::min(W * 0.78f, 920.0f);
		const float ph = std::min(H * 0.74f, 560.0f);
		const float px = (W - pw) * 0.5f;
		const float py = barH + pad + (H - barH - pad - ph) * 0.5f;
		drawGlassCard(dl, ImVec2(px, py), ImVec2(px + pw, py + ph), 14.0f, pal, true);

		drawCenteredText(dl, font, uiFont * 1.15f, ImVec2(px + pw * 0.5f, py + 28.0f),
			L10N("Control & Gesture Preferences", "操作手感与手势偏好"), rgba(pal.accent, 1.0f));
		drawButton(dl, font, g_ed.buttons, {px + pw - 90.0f, py + 12.0f, 76.0f, 32.0f}, kActCloseSubPanel,
			L10N("Done", "完成"),
			rgba(pal.accent, 0.95f), rgba(pal.accentBright, 1.0f),
			rgba(pal.onAccent, 1.0f), uiFont * 0.90f);

		// Divider hairline under header
		dl->AddLine(ImVec2(px + 18.0f, py + 50.0f), ImVec2(px + pw - 18.0f, py + 50.0f), rgba(255, 255, 255, 0.10f), 1.0f);

		const float colGap = 14.0f;
		const float colW = (pw - 36.0f - colGap) * 0.5f;
		const float rowH = std::max(38.0f, (ph - 102.0f) / 6.6f);
		const float startY = py + 60.0f;

		auto prefToggle = [&](int col, int row, int actId, const char* txt, bool on) {
			const float bx = px + 18.0f + col * (colW + colGap);
			const float by = startY + row * (rowH + 8.0f);
			drawButton(dl, font, g_ed.buttons, {bx, by, colW, rowH}, actId, txt,
				on ? rgba(pal.accentDeep, 0.92f) : rgba(LauncherPalette::kSurface2, 0.92f),
				on ? rgba(pal.accent, 0.92f) : rgba(255, 255, 255, 0.14f),
				on ? rgba(LauncherPalette::kTextPrimary, 1.0f) : rgba(LauncherPalette::kTextDim, 1.0f),
				uiFont * 0.88f, on, rgba(pal.accent, 1.0f));
		};

		const auto& pf = g_ed.draft.prefs;
		prefToggle(0, 0, kActPrefAimMode,
			pf.aimMode == touch_hud::AimMode::Toggle ? L10N("Aim Mode: Tap Toggle", "瞄准模式：点按切换") : L10N("Aim Mode: Hold", "瞄准模式：长按开镜"),
			true);
		prefToggle(1, 0, kActPrefVehSteer,
			pf.vehicleSteering == touch_hud::VehicleSteeringMode::Arrows ? L10N("Vehicle Steer: Arrows", "载具转向：左右箭头") : L10N("Vehicle Steer: Joystick", "载具转向：左侧摇杆"),
			true);
		prefToggle(0, 1, kActPrefJoyMode,
			pf.joystickMode == touch_hud::JoystickMode::Floating ? L10N("Joystick: Floating (PUBG)", "摇杆模式：动态浮动 (PUBG)") : L10N("Joystick: Fixed", "摇杆模式：固定位置"),
			pf.joystickMode == touch_hud::JoystickMode::Floating);
		prefToggle(1, 1, kActPrefAutoSprint,
			pf.autoSprintLock ? L10N("Up-Drag Sprint Lock: ON", "上推锁定自动疾跑：开") : L10N("Up-Drag Sprint Lock: OFF", "上推锁定自动疾跑：关"),
			pf.autoSprintLock);
		const char* lfTxt = (pf.leftFireMode == touch_hud::LeftFireMode::Always)
			? L10N("Left Fire: Always", "左侧开火键：常显 (三/四指)")
			: (pf.leftFireMode == touch_hud::LeftFireMode::AimOnly ? L10N("Left Fire: Aim Only", "左侧开火键：仅瞄准显示") : L10N("Left Fire: Hidden", "左侧开火键：隐藏"));
		prefToggle(0, 2, kActPrefLeftFire, lfTxt, pf.leftFireMode != touch_hud::LeftFireMode::Hidden);
		prefToggle(1, 2, kActPrefFireDragAim,
			pf.fireRightDragAim ? L10N("Right Fire Drag-Aim: ON", "右开火拖动跟枪：开") : L10N("Right Fire Drag-Aim: OFF", "右开火拖动跟枪：关"),
			pf.fireRightDragAim);
		prefToggle(0, 3, kActPrefAltLatch,
			pf.altSmartLatch ? L10N("ALT Smart Tap-Latch: ON", "ALT 轻点锁定组合键：开") : L10N("ALT Smart Tap-Latch: OFF", "ALT 轻点锁定组合键：关"),
			pf.altSmartLatch);
		prefToggle(1, 3, kActPrefRpExpanded,
			pf.rpStripExpanded ? L10N("RP Strip Default: Expanded", "RP 按键条默认：展开") : L10N("RP Strip Default: Collapsed", "RP 按键条默认：折叠"),
			pf.rpStripExpanded);
		prefToggle(0, 4, kActPrefPassAlways,
			pf.showPassengerAlways ? L10N("Passenger G: Always", "副驾 G 键：常显") : L10N("Passenger G: Near Car", "副驾 G 键：靠近载具显示"),
			pf.showPassengerAlways);
		const char* pitchTxt = (pf.vehiclePitchMode == touch_hud::VehiclePitchMode::Smart)
			? L10N("Stunt Pitch: Smart (Air/Bike)", "载具特技俯仰：智能(飞机/摩托/腾空)")
			: (pf.vehiclePitchMode == touch_hud::VehiclePitchMode::Always
				? L10N("Stunt Pitch: Always", "载具特技俯仰：载具常显")
				: L10N("Stunt Pitch: Aircraft Only", "载具特技俯仰：仅飞机/特种车"));
		prefToggle(1, 4, kActPrefVehPitch, pitchTxt, pf.vehiclePitchMode != touch_hud::VehiclePitchMode::AircraftOnly);

		const float sRowY = startY + 5 * (rowH + 8.0f);
		g_ed.globalOpacityRect = {px + 18.0f,               sRowY, colW, rowH};
		g_ed.lookSensRect      = {px + 18.0f + colW + colGap, sRowY, colW, rowH};
		drawSlider(dl, font, g_ed.globalOpacityRect, L10N("Global Alpha", "全局透明"), pf.globalOpacity, 0.20f, 1.00f, uiFont * 0.86f, pal);
		drawSlider(dl, font, g_ed.lookSensRect,      L10N("Look Sens", "视角灵敏"), pf.lookSensitivity, 0.25f, 3.00f, uiFont * 0.86f, pal);
	}

	// 6. Custom Combo / Command Buttons Sub-Panel (Launcher Card aesthetic)
	if (g_ed.subPanel == SubPanel::CustomButtons) {
		const float pw = std::min(W * 0.74f, 840.0f);
		const float ph = std::min(H * 0.64f, 440.0f);
		const float px = (W - pw) * 0.5f;
		const float py = barH + pad + (H - barH - pad - ph) * 0.5f;
		drawGlassCard(dl, ImVec2(px, py), ImVec2(px + pw, py + ph), 14.0f, pal, true);

		drawCenteredText(dl, font, uiFont * 1.12f, ImVec2(px + pw * 0.5f, py + 28.0f),
			L10N("Custom 1-Tap Combo & Command Buttons", "自定义一键组合键 / 指令快捷键"), rgba(pal.accent, 1.0f));
		drawButton(dl, font, g_ed.buttons, {px + pw - 90.0f, py + 12.0f, 76.0f, 32.0f}, kActCloseSubPanel,
			L10N("Done", "完成"),
			rgba(pal.accent, 0.95f), rgba(pal.accentBright, 1.0f),
			rgba(pal.onAccent, 1.0f), uiFont * 0.90f);

		dl->AddLine(ImVec2(px + 18.0f, py + 50.0f), ImVec2(px + pw - 18.0f, py + 50.0f), rgba(255, 255, 255, 0.10f), 1.0f);

		// 8 slot selector tabs (2 rows x 4 cols)
		const float slotGap = 10.0f;
		const float slotW = (pw - 36.0f - slotGap * 3.0f) * 0.25f;
		const float slotH = 44.0f;
		for (int i = 0; i < static_cast<int>(touch_hud::kMaxCustomButtons); ++i) {
			const auto& b = g_ed.draft.customButtons[i];
			const int row = i / 4;
			const int col = i % 4;
			const float sx = px + 18.0f + col * (slotW + slotGap);
			const float sy = py + 62.0f + row * (slotH + 8.0f);
			char lbl[40];
			std::snprintf(lbl, sizeof(lbl), "#%d %s", i + 1, b.enabled ? (b.label[0] ? b.label : "ON") : L10N("(Off)", "(未启用)"));
			const bool isSel = (g_ed.selCustomIdx == i);
			drawButton(dl, font, g_ed.buttons, {sx, sy, slotW, slotH}, kActCustomSelectBase + i, lbl,
				isSel ? rgba(pal.accent, 0.94f) : (b.enabled ? rgba(pal.accentDeep, 0.85f) : rgba(LauncherPalette::kSurface2, 0.92f)),
				isSel ? rgba(pal.accentBright, 1.0f) : (b.enabled ? rgba(pal.accent, 0.72f) : rgba(255, 255, 255, 0.14f)),
				isSel ? rgba(pal.onAccent, 1.0f) : (b.enabled ? rgba(LauncherPalette::kTextPrimary, 1.0f) : rgba(LauncherPalette::kTextDim, 1.0f)),
				uiFont * 0.86f);
		}

		// Selected slot controls
		const auto& sb = g_ed.draft.customButtons[g_ed.selCustomIdx];
		const float ctrlY = py + 182.0f;
		const float cw = (pw - 52.0f) / 3.0f;
		drawButton(dl, font, g_ed.buttons, {px + 18.0f, ctrlY, cw, 48.0f}, kActCustomToggleEnable,
			sb.enabled ? L10N("Status: ENABLED", "状态：已启用") : L10N("Status: DISABLED", "状态：已关闭"),
			sb.enabled ? rgba(LauncherPalette::kGood, 0.24f) : rgba(LauncherPalette::kDanger, 0.22f),
			sb.enabled ? rgba(LauncherPalette::kGood, 0.90f) : rgba(LauncherPalette::kDanger, 0.85f),
			sb.enabled ? rgba(LauncherPalette::kGood, 1.0f) : rgba(LauncherPalette::kDanger, 1.0f),
			uiFont * 0.90f);

		char bindTxt[64];
		if (sb.actionType == touch_hud::CustomActionType::ChatCommand) {
			std::snprintf(bindTxt, sizeof(bindTxt), "%s %s", L10N("Bind:", "绑定:"), sb.command[0] ? sb.command : "/inv");
		}
		else {
			std::snprintf(bindTxt, sizeof(bindTxt), "%s %s", L10N("Bind:", "绑定:"), sb.label[0] ? sb.label : "ALT+Y");
		}
		drawButton(dl, font, g_ed.buttons, {px + 26.0f + cw, ctrlY, cw, 48.0f}, kActCustomCycleTemplate,
			bindTxt,
			rgba(pal.accentDeep, 0.92f), rgba(pal.accent, 0.90f),
			rgba(LauncherPalette::kTextPrimary, 1.0f), uiFont * 0.90f);

		const char* ctxTxt = (sb.contextMask == touch_hud::kCtxOnFoot)
			? L10N("Show: On-Foot", "场景：仅步行")
			: (sb.contextMask == touch_hud::kCtxInVehicle ? L10N("Show: Vehicle", "场景：仅载具") : L10N("Show: Both", "场景：步行+载具"));
		drawButton(dl, font, g_ed.buttons, {px + 34.0f + cw * 2.0f, ctrlY, cw, 48.0f}, kActCustomCycleContext,
			ctxTxt,
			rgba(LauncherPalette::kSurface2, 0.96f), rgba(255, 255, 255, 0.18f),
			rgba(LauncherPalette::kTextPrimary, 0.94f), uiFont * 0.90f);

		drawCenteredText(dl, font, uiFont * 0.85f, ImVec2(px + pw * 0.5f, ctrlY + 84.0f),
			L10N("Tip: Enable a slot, tap 'Done', then drag the button anywhere on screen!",
				 "提示：启用槽位并点“完成”后，即可在画布上自由拖动位置、缩放大小。"),
			rgba(LauncherPalette::kTextDim, 0.92f));
	}
}

} // namespace TouchHud
