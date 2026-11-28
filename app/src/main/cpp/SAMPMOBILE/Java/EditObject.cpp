#include "EditObject.h"

#include "../Client.h"
#include "../L10n.h"
#include "../UI/ObjectEditorCore.h"
#include "../UI/UI.h"
#include "../UI/ImGuiRenderer.h"
#include "../UI/TouchHud/TouchHud.h"
#include "../UI/TouchHud/TouchHudTheme.h"
#include "../UI/SAMPWidgets/Chat.h"
#include "../Game/Game.h"
#include "../Game/Object.h"
#include "../Game/PlayerPed.h"
#include "../Game/Util.h"
#include "../Net/NetGame.h"
#include "../Net/LocalPlayer.h"
#include "../Net/Proto/RpcOutbound.h"

#include <RakNet/RakClientInterface.h>
#include <RakNet/SAMP/SAMPRPC.h>
#include <spdlog/spdlog.h>

#include <imgui.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

using namespace object_editor;
using TouchHud::LauncherPalette;
using TouchHud::Rgb;
using TouchHud::rgba;

constexpr Rgb kAxisColorX = {245,  83,  94}; // #F5535E Coral Red
constexpr Rgb kAxisColorY = { 66, 214, 132}; // #42D684 Emerald Green
constexpr Rgb kAxisColorZ = { 74, 168, 255}; // #4AA8FF Sky Blue

enum ActionId : int {
	kActNone = 0,
	kActModeMove = 1,
	kActModeRotate,
	kActModeScale,
	kActToggleSpaceOrUniform,
	kActCycleZoom,
	kActToggleWalk,
	kActReset,
	kActCancel,
	kActSave,
	kActAxisX,
	kActAxisY,
	kActAxisZ,
	kActGearFine,
	kActGearNormal,
	kActGearFast,
};

enum class PtrRole : uint8_t {
	None = 0,
	UiButton,
	GizmoAxis,
	ScrubTrack,
	StepMinus,
	StepPlus,
	CameraOrbit,
};

struct UiHitButton {
	Rect rect{};
	int actionId = kActNone;
};

struct ActivePointer {
	bool active = false;
	int pointerId = -1;
	PtrRole role = PtrRole::None;
	int buttonAction = kActNone;
	EditAxis gizmoAxis = EditAxis::X;
	float lastX = 0.0f;
	float lastY = 0.0f;
	uint64_t downMs = 0;
};

struct EditorSharedState {
	EditMode mode = EditMode::Move;
	EditAxis axis = EditAxis::X;
	SpeedGear gear = SpeedGear::Normal;
	bool localSpace = true;
	bool uniformScale = false;

	// Pending continuous & discrete deltas drained on GameMain
	float pendingDelta = 0.0f;
	int pendingSteps = 0;
	bool pendingReset = false;
	bool pendingExit = false;
	bool pendingSave = false;
	bool pendingZoomCycle = false;

	// Visual feedback
	float scrubPhasePx = 0.0f;
	int pressedActionId = kActNone;
	bool draggingGizmo = false;
	bool draggingScrub = false;
	bool holdingMinus = false;
	bool holdingPlus = false;
	uint64_t holdRepeatStartMs = 0;

	// Snapshot of UI hit zones updated on GameMain render pass
	Rect bottomDockRect{};
	Rect scrubTrackRect{};
	Rect minusBtnRect{};
	Rect plusBtnRect{};
	std::vector<UiHitButton> buttons;
	std::array<GizmoScreenAxis, 3> gizmoAxes{};
	float badgeHitRadius = 30.0f;
	float shaftHitRadius = 16.0f;

	std::array<ActivePointer, 8> pointers{};
};

std::mutex g_mutex;
EditorSharedState g_state{};
std::atomic<bool> g_walkMode{false};

// GameMain-owned session snapshot & live state
bool g_hasOrigSnapshot = false;
sa::CVector g_origObjPos{};
sa::CVector g_origObjRot{};
Vec3 g_editObjRot{};
NEW_ATTACHED_OBJECT g_origAttach{};
bool g_dirtyObjSync = false;
uint64_t g_lastFrameSteadyMs = 0;

uint64_t nowSteadyMs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

Rgb axisRgb(EditAxis a)
{
	switch (a) {
		case EditAxis::X: return kAxisColorX;
		case EditAxis::Y: return kAxisColorY;
		case EditAxis::Z: return kAxisColorZ;
	}
	return kAxisColorX;
}

const char* axisShortName(EditAxis a)
{
	switch (a) {
		case EditAxis::X: return "X";
		case EditAxis::Y: return "Y";
		case EditAxis::Z: return "Z";
	}
	return "X";
}

const char* boneDisplayName(int boneId)
{
	switch (boneId) {
		case 1:  return L10N("Spine", "脊柱");
		case 2:  return L10N("Head", "头部");
		case 3:  return L10N("L.Arm", "左上臂");
		case 4:  return L10N("R.Arm", "右上臂");
		case 5:  return L10N("L.Hand", "左手");
		case 6:  return L10N("R.Hand", "右手");
		case 7:  return L10N("L.Thigh", "左大腿");
		case 8:  return L10N("R.Thigh", "右大腿");
		case 9:  return L10N("L.Foot", "左脚");
		case 10: return L10N("R.Foot", "右脚");
		case 11: return L10N("R.Calf", "右小腿");
		case 12: return L10N("L.Calf", "左小腿");
		case 13: return L10N("L.Forearm", "左前臂");
		case 14: return L10N("R.Forearm", "右前臂");
		case 15: return L10N("L.Shoulder", "左肩");
		case 16: return L10N("R.Shoulder", "右肩");
		case 17: return L10N("Neck", "颈部");
		case 18: return L10N("Jaw", "下颌");
		default: return L10N("Bone", "骨骼");
	}
}

void resetSessionUiState(EditObject::eEditType type)
{
	g_walkMode.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(g_mutex);
	const SpeedGear prevGear = g_state.gear;
	const bool prevLocal = g_state.localSpace;
	g_state = EditorSharedState{};
	g_state.mode = EditMode::Move;
	g_state.axis = EditAxis::X;
	g_state.gear = prevGear;
	g_state.localSpace = prevLocal;
	g_state.uniformScale = false;
	(void)type;
}

void refreshPointerFlagsLocked()
{
	g_state.pressedActionId = kActNone;
	g_state.draggingGizmo = false;
	g_state.draggingScrub = false;
	g_state.holdingMinus = false;
	g_state.holdingPlus = false;

	for (const auto& p : g_state.pointers) {
		if (!p.active) continue;
		switch (p.role) {
			case PtrRole::UiButton:
				g_state.pressedActionId = p.buttonAction;
				break;
			case PtrRole::GizmoAxis:
				g_state.draggingGizmo = true;
				break;
			case PtrRole::ScrubTrack:
				g_state.draggingScrub = true;
				break;
			case PtrRole::StepMinus:
				g_state.holdingMinus = true;
				break;
			case PtrRole::StepPlus:
				g_state.holdingPlus = true;
				break;
			default:
				break;
		}
	}
}

void executeButtonActionLocked(int actionId)
{
	switch (actionId) {
		case kActModeMove:
			g_state.mode = EditMode::Move;
			g_walkMode.store(false, std::memory_order_release);
			break;
		case kActModeRotate:
			g_state.mode = EditMode::Rotate;
			g_walkMode.store(false, std::memory_order_release);
			break;
		case kActModeScale:
			if (EditObject::m_type == EditObject::TYPE_ATTACHED_OBJECT) {
				g_state.mode = EditMode::Scale;
				g_walkMode.store(false, std::memory_order_release);
			}
			break;
		case kActToggleSpaceOrUniform:
			if (EditObject::m_type == EditObject::TYPE_OBJECT && g_state.mode == EditMode::Move) {
				g_state.localSpace = !g_state.localSpace;
			}
			else if (EditObject::m_type == EditObject::TYPE_ATTACHED_OBJECT && g_state.mode == EditMode::Scale) {
				g_state.uniformScale = !g_state.uniformScale;
			}
			break;
		case kActCycleZoom:
			g_state.pendingZoomCycle = true;
			break;
		case kActToggleWalk: {
			const bool nextWalk = !g_walkMode.load(std::memory_order_acquire);
			g_walkMode.store(nextWalk, std::memory_order_release);
			g_state.pendingDelta = 0.0f;
			g_state.pendingSteps = 0;
			break;
		}
		case kActReset:
			g_state.pendingReset = true;
			break;
		case kActCancel:
			g_state.pendingExit = true;
			break;
		case kActSave:
			g_state.pendingSave = true;
			break;
		case kActAxisX:
			g_state.axis = EditAxis::X;
			break;
		case kActAxisY:
			g_state.axis = EditAxis::Y;
			break;
		case kActAxisZ:
			g_state.axis = EditAxis::Z;
			break;
		case kActGearFine:
			g_state.gear = SpeedGear::Fine;
			break;
		case kActGearNormal:
			g_state.gear = SpeedGear::Normal;
			break;
		case kActGearFast:
			g_state.gear = SpeedGear::Fast;
			break;
		default:
			break;
	}
}

CObject* resolveLiveWorldObject()
{
	if (EditObject::m_type != EditObject::TYPE_OBJECT || EditObject::m_id < 0) return nullptr;
	if (!pNetGame || !pNetGame->GetObjectPool()) return nullptr;
	CObject* pObj = pNetGame->GetObjectPool()->GetAt(static_cast<OBJECTID>(EditObject::m_id));
	if (!pObj || !pObj->m_entity || !pObj->m_gtaId) return nullptr;
	if (GamePool_Object_GetAt(pObj->m_gtaId) != reinterpret_cast<sa::CObject*>(pObj->m_entity)) {
		return nullptr;
	}
	return pObj;
}

CPlayerPed* resolveLocalPedForAttach(NEW_ATTACHED_OBJECT** outInfo = nullptr, CObject** outObj = nullptr)
{
	if (EditObject::m_type != EditObject::TYPE_ATTACHED_OBJECT) return nullptr;
	if (EditObject::m_id < 0 || EditObject::m_id >= 10) return nullptr;
	if (!pNetGame || !pNetGame->GetPlayerPool() || !pNetGame->GetPlayerPool()->GetLocalPlayer()) return nullptr;
	CPlayerPed* pPed = pNetGame->GetPlayerPool()->GetLocalPlayer()->GetPlayerPed();
	if (!pPed || !pPed->m_ped || !pPed->IsValidAttach(EditObject::m_id)) return nullptr;
	if (outInfo) *outInfo = &pPed->m_attachedObjects[EditObject::m_id].info;
	if (outObj)  *outObj  = pPed->m_attachedObjects[EditObject::m_id].object;
	return pPed;
}

void sendObjectReplyRpc(CObject* pObject, EditObject::eResponse response, const Vec3& eulerRot)
{
	if (!pObject || !pObject->m_entity || !pNetGame || !pNetGame->GetRakClient()) return;
	const RwMatrix matrix = pObject->m_entity->GetMatrix().ToRwMatrix();

	RakNet::BitStream bsSend;
	proto::Writer writer(bsSend);
	proto::EditObjectReply reply{};
	reply.playerObject = EditObject::m_playerObj;
	reply.objectId = static_cast<uint16_t>(EditObject::m_id);
	reply.response = static_cast<uint32_t>(response);
	reply.position = {matrix.pos.x, matrix.pos.y, matrix.pos.z};
	reply.rotation = {eulerRot.x, eulerRot.y, eulerRot.z};
	reply.serialize(writer, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_EditObject, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void sendAttachReplyRpc(const NEW_ATTACHED_OBJECT& attach, EditObject::eResponse response)
{
	if (!pNetGame || !pNetGame->GetRakClient()) return;

	RakNet::BitStream bsSend;
	proto::Writer writer(bsSend);
	proto::EditAttachedObjectReply reply{};
	reply.response = static_cast<uint32_t>(response);
	reply.index = EditObject::m_id;
	reply.modelId = attach.iModel;
	reply.boneId = attach.iBoneID;
	reply.offset = {attach.vecOffset.x, attach.vecOffset.y, attach.vecOffset.z};
	reply.rotation = {attach.vecRot.x, attach.vecRot.y, attach.vecRot.z};
	reply.scale = {attach.vecScale.x, attach.vecScale.y, attach.vecScale.z};
	reply.color1 = attach.dwMaterialColor1;
	reply.color2 = attach.dwMaterialColor2;
	reply.serialize(writer, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_EditAttachedObject, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void drawCenteredText(ImDrawList* dl, ImFont* font, float fontSize,
	const ImVec2& center, const char* text, ImU32 col, ImU32 shadowCol)
{
	if (!dl || !font || !text || !text[0]) return;
	const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
	const ImVec2 pos(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f);
	if ((shadowCol >> IM_COL32_A_SHIFT) > 0) {
		dl->AddText(font, fontSize, ImVec2(pos.x + 1.1f, pos.y + 1.3f), shadowCol, text);
	}
	dl->AddText(font, fontSize, pos, col, text);
}

enum class PillStyle : uint8_t {
	Normal = 0,
	SelectedAccent,
	SelectedTinted,
	PrimarySave,
	DangerOutline,
};

void drawPillButton(ImDrawList* dl, ImFont* font, float fontSize, const Rect& r,
	const char* label, PillStyle style, bool pressed, const LauncherPalette& pal,
	Rgb customTint = {255, 255, 255})
{
	const ImVec2 pMin(r.x, r.y);
	const ImVec2 pMax(r.x + r.w, r.y + r.h);
	const float rad = std::min(9.0f, r.h * 0.28f);

	ImU32 bg = rgba(LauncherPalette::kSurface, 0.90f);
	ImU32 bd = rgba(255, 255, 255, 0.14f);
	ImU32 fg = rgba(LauncherPalette::kTextPrimary, 0.96f);
	ImU32 sh = rgba(0, 0, 0, 0.65f);
	float bdThick = 1.15f;

	switch (style) {
		case PillStyle::Normal:
			if (pressed) {
				bg = rgba(LauncherPalette::kSurface3, 0.96f);
				bd = rgba(pal.accent, 0.80f);
			}
			break;
		case PillStyle::SelectedAccent:
			bg = rgba(pal.accent, pressed ? 1.0f : 0.92f);
			bd = rgba(pal.accentBright, 1.0f);
			fg = rgba(pal.onAccent, 1.0f);
			sh = rgba(0, 0, 0, 0.18f);
			bdThick = 1.6f;
			break;
		case PillStyle::SelectedTinted:
			bg = rgba(customTint.r / 4, customTint.g / 4, customTint.b / 4, 0.94f);
			bd = rgba(customTint, pressed ? 1.0f : 0.92f);
			fg = rgba(LauncherPalette::kTextPrimary, 1.0f);
			bdThick = 1.85f;
			break;
		case PillStyle::PrimarySave:
			bg = rgba(pal.accent, pressed ? 1.0f : 0.95f);
			bd = rgba(pal.accentBright, 1.0f);
			fg = rgba(pal.onAccent, 1.0f);
			sh = rgba(0, 0, 0, 0.20f);
			bdThick = 1.7f;
			break;
		case PillStyle::DangerOutline:
			bg = pressed ? rgba(LauncherPalette::kDanger, 0.85f) : rgba(LauncherPalette::kSurface, 0.90f);
			bd = rgba(LauncherPalette::kDanger, pressed ? 1.0f : 0.65f);
			fg = pressed ? rgba(LauncherPalette::kTextPrimary, 1.0f) : rgba(LauncherPalette::kDanger, 0.96f);
			bdThick = 1.35f;
			break;
	}

	dl->AddRectFilled(pMin, pMax, bg, rad);
	if (style == PillStyle::Normal && !pressed) {
		dl->AddLine(ImVec2(pMin.x + rad * 0.6f, pMin.y + 1.0f),
			ImVec2(pMax.x - rad * 0.6f, pMin.y + 1.0f), rgba(255, 255, 255, 0.08f), 1.0f);
	}
	dl->AddRect(pMin, pMax, bd, rad, ImDrawCornerFlags_All, bdThick);
	drawCenteredText(dl, font, fontSize, ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f), label, fg, sh);
}

bool projectWorldPoint(const RwV3d& worldPt, Vec2& outScr, float& outDepth)
{
	RwV3d scr{};
	CalcScreenCoors(&worldPt, &scr, 0.0f, 0.0f, false, false);
	outDepth = scr.z;
	if (!std::isfinite(scr.x) || !std::isfinite(scr.y) || !std::isfinite(scr.z) || scr.z <= 0.25f) {
		return false;
	}
	outScr = {scr.x, scr.y};
	return true;
}

} // namespace

void EditObject::startEditAttachedObject(int slot)
{
	if (!pNetGame || !pNetGame->GetPlayerPool() || !pNetGame->GetPlayerPool()->GetLocalPlayer()) return;

	CPlayerPed* pPlayerPed = pNetGame->GetPlayerPool()->GetLocalPlayer()->GetPlayerPed();
	if (!pPlayerPed || !pPlayerPed->IsValidAttach(slot)) {
		if (pUI && pUI->chat()) pUI->chat()->addDebugMessage("Invalid attach slot %d", slot);
		return;
	}

	g_origAttach = pPlayerPed->m_attachedObjects[slot].info;
	g_hasOrigSnapshot = true;
	g_dirtyObjSync = false;
	g_lastFrameSteadyMs = nowSteadyMs();

	EditObject::m_id = slot;
	EditObject::m_type = TYPE_ATTACHED_OBJECT;
	EditObject::m_lastUpdate = GetTickCount();
	resetSessionUiState(TYPE_ATTACHED_OBJECT);
	EditObject::showGui(true);
}

void EditObject::startEditObject(uint16_t objectId)
{
	if (!pNetGame || !pNetGame->GetObjectPool()) return;

	CObject* pObject = pNetGame->GetObjectPool()->GetAt(objectId);
	if (!pObject || !pObject->m_entity) {
		if (pUI && pUI->chat()) pUI->chat()->addDebugMessage("Invalid object id %d", objectId);
		return;
	}

	g_origObjPos = pObject->m_entity->GetPosition();
	g_origObjRot = pObject->GetStoredRotation();
	g_editObjRot = {
		wrapEulerDeg(g_origObjRot.x),
		wrapEulerDeg(g_origObjRot.y),
		wrapEulerDeg(g_origObjRot.z),
	};
	g_hasOrigSnapshot = true;
	g_dirtyObjSync = false;
	g_lastFrameSteadyMs = nowSteadyMs();

	EditObject::m_id = objectId;
	EditObject::m_type = TYPE_OBJECT;
	EditObject::m_lastUpdate = GetTickCount();
	resetSessionUiState(TYPE_OBJECT);
	EditObject::showGui(true);
}

void EditObject::showGui(bool show)
{
	if (!show) {
		g_walkMode.store(false, std::memory_order_release);
		onTouchCancel();
	}
	m_visible.store(show, std::memory_order_release);
}

void EditObject::cancelEdit()
{
	g_hasOrigSnapshot = false;
	g_dirtyObjSync = false;
	EditObject::m_id = -1;
	EditObject::m_type = TYPE_NONE;
	EditObject::showGui(false);
}

void EditObject::exitEdit()
{
	applyExit();
	showGui(false);
}

bool EditObject::isWalkMode()
{
	return m_visible.load(std::memory_order_acquire) && g_walkMode.load(std::memory_order_acquire);
}

bool EditObject::isUiPanelHitSnapshot(float x, float y)
{
	if (!m_visible.load(std::memory_order_acquire)) {
		return false;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_state.bottomDockRect.contains(x, y, 4.0f);
}

void EditObject::onTouchCancel()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto& p : g_state.pointers) {
		p = ActivePointer{};
	}
	g_state.pendingDelta = 0.0f;
	g_state.pendingSteps = 0;
	refreshPointerFlagsLocked();
}

bool EditObject::handleTouch(int type, int pointerId, float x, float y)
{
	if (!m_visible.load(std::memory_order_acquire)) {
		return false;
	}

	const uint64_t nowMs = nowSteadyMs();
	float orbitDX = 0.0f;
	float orbitDY = 0.0f;
	bool sendOrbit = false;

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		auto findPtr = [&](int id) -> ActivePointer* {
			for (auto& s : g_state.pointers) {
				if (s.active && s.pointerId == id) return &s;
			}
			return nullptr;
		};

		auto allocPtr = [&](int id) -> ActivePointer* {
			for (auto& s : g_state.pointers) {
				if (!s.active) {
					s = ActivePointer{};
					s.active = true;
					s.pointerId = id;
					return &s;
				}
			}
			return nullptr;
		};

		if (type == 2) { // Down
			if (ActivePointer* old = findPtr(pointerId)) {
				*old = ActivePointer{};
			}

			const bool walkNow = g_walkMode.load(std::memory_order_acquire);

			// 1) Check interactive buttons inside the unified Bottom Studio Dock
			for (const auto& btn : g_state.buttons) {
				if (btn.rect.contains(x, y, 3.0f)) {
					if (ActivePointer* slot = allocPtr(pointerId)) {
						slot->role = PtrRole::UiButton;
						slot->buttonAction = btn.actionId;
						slot->lastX = x;
						slot->lastY = y;
						slot->downMs = nowMs;
						refreshPointerFlagsLocked();
					}
					return true;
				}
			}

			// If in Walk-Around mode, only the slim Bottom Header Dock belongs to EditObject;
			// let all other touches pass straight to TouchHud (Joystick + Jump/Sprint + Look)!
			if (walkNow) {
				if (g_state.bottomDockRect.contains(x, y, 4.0f)) {
					return true;
				}
				return false;
			}

			// 2) Step Minus / Step Plus / Precision Scrub Track inside Bottom Dock
			if (g_state.minusBtnRect.contains(x, y, 4.0f)) {
				if (ActivePointer* slot = allocPtr(pointerId)) {
					slot->role = PtrRole::StepMinus;
					slot->lastX = x;
					slot->lastY = y;
					slot->downMs = nowMs;
					g_state.pendingSteps -= 1;
					g_state.holdRepeatStartMs = nowMs + 240;
					refreshPointerFlagsLocked();
				}
				return true;
			}
			if (g_state.plusBtnRect.contains(x, y, 4.0f)) {
				if (ActivePointer* slot = allocPtr(pointerId)) {
					slot->role = PtrRole::StepPlus;
					slot->lastX = x;
					slot->lastY = y;
					slot->downMs = nowMs;
					g_state.pendingSteps += 1;
					g_state.holdRepeatStartMs = nowMs + 240;
					refreshPointerFlagsLocked();
				}
				return true;
			}
			if (g_state.scrubTrackRect.contains(x, y, 4.0f)) {
				if (ActivePointer* slot = allocPtr(pointerId)) {
					slot->role = PtrRole::ScrubTrack;
					slot->lastX = x;
					slot->lastY = y;
					slot->downMs = nowMs;
					refreshPointerFlagsLocked();
				}
				return true;
			}

			// Block clicks on Bottom Studio Dock padding from spinning camera or grabbing gizmo behind dock
			if (g_state.bottomDockRect.contains(x, y, 4.0f)) {
				return true;
			}

			// 3) 3D On-Object Gizmo Axis Handles (+X / +Y / +Z badges & shafts)
			const int hitAxis = hitTestGizmoAxes(
				g_state.gizmoAxes, x, y, g_state.badgeHitRadius, g_state.shaftHitRadius);
			if (hitAxis >= 0 && hitAxis < 3) {
				g_state.axis = static_cast<EditAxis>(hitAxis);
				if (ActivePointer* slot = allocPtr(pointerId)) {
					slot->role = PtrRole::GizmoAxis;
					slot->gizmoAxis = g_state.axis;
					slot->lastX = x;
					slot->lastY = y;
					slot->downMs = nowMs;
					refreshPointerFlagsLocked();
				}
				return true;
			}

			// 4) Empty viewport space -> smooth 360-degree Camera Orbit around player/object!
			if (ActivePointer* slot = allocPtr(pointerId)) {
				slot->role = PtrRole::CameraOrbit;
				slot->lastX = x;
				slot->lastY = y;
				slot->downMs = nowMs;
				refreshPointerFlagsLocked();
			}
			return true;
		}

		ActivePointer* slot = findPtr(pointerId);
		if (!slot) {
			return false;
		}

		const float dx = x - slot->lastX;
		const float dy = y - slot->lastY;
		slot->lastX = x;
		slot->lastY = y;

		if (type == 3) { // Move
			switch (slot->role) {
				case PtrRole::GizmoAxis: {
					const auto idx = static_cast<size_t>(slot->gizmoAxis);
					g_state.axis = slot->gizmoAxis;
					g_state.pendingDelta += computeGizmoDragDelta(
						g_state.mode, g_state.gizmoAxes[idx], dx, dy, g_state.gear);
					break;
				}
				case PtrRole::ScrubTrack: {
					g_state.pendingDelta += computeScrubTrackDelta(
						g_state.mode, dx, g_state.scrubTrackRect.w, g_state.gear);
					g_state.scrubPhasePx = std::fmod(g_state.scrubPhasePx + dx, 240.0f);
					break;
				}
				case PtrRole::CameraOrbit: {
					orbitDX = dx;
					orbitDY = dy;
					sendOrbit = true;
					break;
				}
				default:
					break;
			}
		}
		else if (type == 1) { // Up
			if (slot->role == PtrRole::UiButton) {
				for (const auto& btn : g_state.buttons) {
					if (btn.actionId == slot->buttonAction && btn.rect.contains(x, y, 10.0f)) {
						executeButtonActionLocked(slot->buttonAction);
						break;
					}
				}
			}
			*slot = ActivePointer{};
			refreshPointerFlagsLocked();
		}
	}

	if (sendOrbit) {
		TouchHud::injectExternalLookDelta(orbitDX, orbitDY);
	}
	return true;
}

void EditObject::processPending()
{
	if (!m_visible.load(std::memory_order_acquire) || m_type == TYPE_NONE) {
		return;
	}

	const uint64_t nowMs = nowSteadyMs();
	float dtSec = 0.016f;
	if (g_lastFrameSteadyMs != 0 && nowMs > g_lastFrameSteadyMs) {
		dtSec = std::clamp(static_cast<float>(nowMs - g_lastFrameSteadyMs) * 0.001f, 0.001f, 0.080f);
	}
	g_lastFrameSteadyMs = nowMs;

	EditMode mode = EditMode::Move;
	EditAxis axis = EditAxis::X;
	SpeedGear gear = SpeedGear::Normal;
	bool localSpace = true;
	bool uniformScale = false;
	float totalDelta = 0.0f;
	bool doReset = false;
	bool doExit = false;
	bool doSave = false;
	bool doZoom = false;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		mode = g_state.mode;
		axis = g_state.axis;
		gear = g_state.gear;
		localSpace = g_state.localSpace;
		uniformScale = g_state.uniformScale;

		// Safety: world objects have no scale in SA-MP
		if (m_type == TYPE_OBJECT && mode == EditMode::Scale) {
			g_state.mode = EditMode::Move;
			mode = EditMode::Move;
		}

		const float step = stepAmount(mode, gear);
		totalDelta = g_state.pendingDelta + static_cast<float>(g_state.pendingSteps) * step;
		g_state.pendingDelta = 0.0f;
		g_state.pendingSteps = 0;

		if ((g_state.holdingMinus || g_state.holdingPlus) && nowMs >= g_state.holdRepeatStartMs) {
			const float dir = g_state.holdingPlus ? 1.0f : -1.0f;
			totalDelta += dir * step * 14.0f * dtSec;
			g_state.scrubPhasePx = std::fmod(g_state.scrubPhasePx + dir * 90.0f * dtSec, 240.0f);
		}

		doReset = g_state.pendingReset;
		doExit = g_state.pendingExit;
		doSave = g_state.pendingSave;
		doZoom = g_state.pendingZoomCycle;
		g_state.pendingReset = false;
		g_state.pendingExit = false;
		g_state.pendingSave = false;
		g_state.pendingZoomCycle = false;
	}

	if (doZoom) {
		sa::CCamera& theCam = sa::getTheCamera();
		CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
		if (pLocal && pLocal->IsInVehicle()) {
			const int32_t cur = theCam.m_nCarZoom;
			theCam.m_nCarZoom = (cur >= 0 && cur < 4) ? (cur + 1) : 0;
		}
		else {
			const int32_t cur = theCam.m_nPedZoom;
			theCam.m_nPedZoom = (cur >= 1 && cur < 3) ? (cur + 1) : 1;
		}
	}

	if (doExit) {
		exitEdit();
		return;
	}
	if (doSave) {
		applySave();
		showGui(false);
		return;
	}

	// Verify live target entity/slot still exists
	if (m_type == TYPE_OBJECT) {
		CObject* pObject = resolveLiveWorldObject();
		if (!pObject) {
			cancelEdit();
			return;
		}
		if (doReset) {
			applyReset();
		}
		if (std::fabs(totalDelta) > 1e-7f) {
			if (mode == EditMode::Move) {
				const RwMatrix mat = pObject->m_entity->GetMatrix().ToRwMatrix();
				const Basis3 basis{
					{mat.right.x, mat.right.y, mat.right.z},
					{mat.up.x,    mat.up.y,    mat.up.z},
					{mat.at.x,    mat.at.y,    mat.at.z},
				};
				const Vec3 nextPos = applyMoveDelta(
					{mat.pos.x, mat.pos.y, mat.pos.z}, axis, totalDelta, localSpace, basis);
				pObject->TeleportTo(nextPos.x, nextPos.y, nextPos.z);
			}
			else if (mode == EditMode::Rotate) {
				g_editObjRot = applyRotateDelta(g_editObjRot, axis, totalDelta);
				sa::CVector vRot{g_editObjRot.x, g_editObjRot.y, g_editObjRot.z};
				pObject->SetRotation(&vRot);
			}
			g_dirtyObjSync = true;
		}

		if (g_dirtyObjSync && (GetTickCount() - m_lastUpdate >= 250)) {
			sendObjectReplyRpc(pObject, EDIT_RESPONSE_UPDATE, g_editObjRot);
			m_lastUpdate = GetTickCount();
			g_dirtyObjSync = false;
		}
	}
	else if (m_type == TYPE_ATTACHED_OBJECT) {
		NEW_ATTACHED_OBJECT* pAttach = nullptr;
		if (!resolveLocalPedForAttach(&pAttach, nullptr) || !pAttach) {
			cancelEdit();
			return;
		}
		if (doReset) {
			applyReset();
		}
		if (std::fabs(totalDelta) > 1e-7f) {
			if (mode == EditMode::Move) {
				// vecOffset is transformed directly by boneMatrix in ProcessAttachedObjects()
				const Vec3 nextOff = applyMoveDelta(
					{pAttach->vecOffset.x, pAttach->vecOffset.y, pAttach->vecOffset.z},
					axis, totalDelta, false, {});
				pAttach->vecOffset = sa::CVector{nextOff.x, nextOff.y, nextOff.z};
			}
			else if (mode == EditMode::Rotate) {
				const Vec3 nextRot = applyRotateDelta(
					{pAttach->vecRot.x, pAttach->vecRot.y, pAttach->vecRot.z}, axis, totalDelta);
				pAttach->vecRot = sa::CVector{nextRot.x, nextRot.y, nextRot.z};
			}
			else if (mode == EditMode::Scale) {
				const Vec3 nextScale = applyScaleDelta(
					{pAttach->vecScale.x, pAttach->vecScale.y, pAttach->vecScale.z},
					axis, totalDelta, uniformScale);
				pAttach->vecScale = sa::CVector{nextScale.x, nextScale.y, nextScale.z};
			}
		}
	}
}

void EditObject::applyReset()
{
	if (!g_hasOrigSnapshot) return;

	if (m_type == TYPE_OBJECT) {
		if (CObject* pObject = resolveLiveWorldObject()) {
			pObject->TeleportTo(g_origObjPos.x, g_origObjPos.y, g_origObjPos.z);
			pObject->SetRotation(&g_origObjRot);
			g_editObjRot = {
				wrapEulerDeg(g_origObjRot.x),
				wrapEulerDeg(g_origObjRot.y),
				wrapEulerDeg(g_origObjRot.z),
			};
			sendObjectReplyRpc(pObject, EDIT_RESPONSE_UPDATE, g_editObjRot);
			m_lastUpdate = GetTickCount();
			g_dirtyObjSync = false;
		}
	}
	else if (m_type == TYPE_ATTACHED_OBJECT) {
		NEW_ATTACHED_OBJECT* pAttach = nullptr;
		if (resolveLocalPedForAttach(&pAttach, nullptr) && pAttach) {
			*pAttach = g_origAttach;
		}
	}
}

void EditObject::applyExit()
{
	if (m_type == TYPE_OBJECT) {
		if (CObject* pObject = resolveLiveWorldObject()) {
			if (g_hasOrigSnapshot) {
				pObject->TeleportTo(g_origObjPos.x, g_origObjPos.y, g_origObjPos.z);
				pObject->SetRotation(&g_origObjRot);
				g_editObjRot = {
					wrapEulerDeg(g_origObjRot.x),
					wrapEulerDeg(g_origObjRot.y),
					wrapEulerDeg(g_origObjRot.z),
				};
			}
			sendObjectReplyRpc(pObject, EDIT_RESPONSE_CANCEL, g_editObjRot);
		}
	}
	else if (m_type == TYPE_ATTACHED_OBJECT) {
		NEW_ATTACHED_OBJECT* pAttach = nullptr;
		if (resolveLocalPedForAttach(&pAttach, nullptr) && pAttach) {
			if (g_hasOrigSnapshot) {
				*pAttach = g_origAttach;
			}
			sendAttachReplyRpc(*pAttach, EDIT_RESPONSE_CANCEL);
		}
	}

	g_hasOrigSnapshot = false;
	g_dirtyObjSync = false;
	EditObject::m_id = -1;
	EditObject::m_type = TYPE_NONE;
	EditObject::m_visible.store(false, std::memory_order_release);
}

void EditObject::applySave()
{
	if (m_type == TYPE_OBJECT) {
		if (CObject* pObject = resolveLiveWorldObject()) {
			sendObjectReplyRpc(pObject, EDIT_RESPONSE_FINAL, g_editObjRot);
		}
	}
	else if (m_type == TYPE_ATTACHED_OBJECT) {
		NEW_ATTACHED_OBJECT* pAttach = nullptr;
		if (resolveLocalPedForAttach(&pAttach, nullptr) && pAttach) {
			sendAttachReplyRpc(*pAttach, EDIT_RESPONSE_FINAL);
		}
	}

	g_hasOrigSnapshot = false;
	g_dirtyObjSync = false;
	EditObject::m_id = -1;
	EditObject::m_type = TYPE_NONE;
	EditObject::m_visible.store(false, std::memory_order_release);
}

void EditObject::render(ImGuiRenderer* renderer)
{
	(void)renderer;
	if (!m_visible.load(std::memory_order_acquire) || m_type == TYPE_NONE || Game::IsGamePaused()) {
		return;
	}

	ImDrawList* bgDl = ImGui::GetBackgroundDrawList();
	ImDrawList* fgDl = ImGui::GetForegroundDrawList();
	ImFont* font = ImGui::GetFont();
	if (!bgDl || !fgDl || !font) return;

	const ImVec2 disp = pUI ? pUI->size() : ImGui::GetIO().DisplaySize;
	const float screenW = std::max(320.0f, disp.x);
	const float screenH = std::max(240.0f, disp.y);
	const float u = std::min(screenW, screenH);
	const LauncherPalette pal = LauncherPalette::current();

	// Read current UI mode/axis/gear state
	EditMode mode = EditMode::Move;
	EditAxis selAxis = EditAxis::X;
	SpeedGear gear = SpeedGear::Normal;
	bool localSpace = true;
	bool uniformScale = false;
	int pressedAct = kActNone;
	bool draggingGizmo = false;
	bool draggingScrub = false;
	bool holdingMinus = false;
	bool holdingPlus = false;
	float scrubPhase = 0.0f;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		mode = g_state.mode;
		selAxis = g_state.axis;
		gear = g_state.gear;
		localSpace = g_state.localSpace;
		uniformScale = g_state.uniformScale;
		pressedAct = g_state.pressedActionId;
		draggingGizmo = g_state.draggingGizmo;
		draggingScrub = g_state.draggingScrub;
		holdingMinus = g_state.holdingMinus;
		holdingPlus = g_state.holdingPlus;
		scrubPhase = g_state.scrubPhasePx;
	}

	const bool walkNow = g_walkMode.load(std::memory_order_acquire);

	// Resolve target world origin, 3D gizmo basis, and live numeric values for display
	bool hasTarget = false;
	RwV3d worldOrigin{0.0f, 0.0f, 0.0f};
	Basis3 gizmoBasis{};
	Vec3 displayVals{};
	char targetBadge[96] = {};

	if (m_type == TYPE_OBJECT) {
		if (CObject* pObj = resolveLiveWorldObject()) {
			hasTarget = true;
			const RwMatrix mat = pObj->m_entity->GetMatrix().ToRwMatrix();
			worldOrigin = {mat.pos.x, mat.pos.y, mat.pos.z};
			if (mode != EditMode::Move || localSpace) {
				gizmoBasis = normalizedBasis({
					{mat.right.x, mat.right.y, mat.right.z},
					{mat.up.x,    mat.up.y,    mat.up.z},
					{mat.at.x,    mat.at.y,    mat.at.z},
				});
			}
			else {
				gizmoBasis = {};
			}
			if (mode == EditMode::Move) {
				// Display relative offset from session start so numbers stay short & meaningful!
				displayVals = {
					mat.pos.x - g_origObjPos.x,
					mat.pos.y - g_origObjPos.y,
					mat.pos.z - g_origObjPos.z,
				};
			}
			else {
				displayVals = g_editObjRot;
			}
			std::snprintf(targetBadge, sizeof(targetBadge), L10N("Obj #%d (%d)", "物体 #%d (%d)"),
				m_id, pObj->GetServerModelIndex());
		}
	}
	else if (m_type == TYPE_ATTACHED_OBJECT) {
		NEW_ATTACHED_OBJECT* pAttach = nullptr;
		CObject* pAttObj = nullptr;
		CPlayerPed* pPed = resolveLocalPedForAttach(&pAttach, &pAttObj);
		if (pPed && pAttach) {
			hasTarget = true;
			RwMatrix boneMat{};
			bool gotBone = false;
			const int boneId = pAttach->iBoneID;
			if (boneId >= 0 && boneId < 19 && pPed->m_ped->m_apBones[boneId]) {
				const int nodeId = pPed->m_ped->m_apBones[boneId]->m_nNodeId;
				gotBone = pPed->GetBoneMatrix(&boneMat, nodeId);
			}

			if (gotBone) {
				sa::CVector worldPos{};
				ProjectMatrix(&worldPos, reinterpret_cast<sa::CMatrix*>(&boneMat), &pAttach->vecOffset);
				worldOrigin = {worldPos.x, worldPos.y, worldPos.z};
				if (mode != EditMode::Move) {
					if (pAttach->vecRot.x != 0.0f) RwMatrixRotate(&boneMat, 0, pAttach->vecRot.x);
					if (pAttach->vecRot.y != 0.0f) RwMatrixRotate(&boneMat, 1, pAttach->vecRot.y);
					if (pAttach->vecRot.z != 0.0f) RwMatrixRotate(&boneMat, 2, pAttach->vecRot.z);
				}
				gizmoBasis = normalizedBasis({
					{boneMat.right.x, boneMat.right.y, boneMat.right.z},
					{boneMat.up.x,    boneMat.up.y,    boneMat.up.z},
					{boneMat.at.x,    boneMat.at.y,    boneMat.at.z},
				});
			}
			else if (pAttObj && pAttObj->m_entity) {
				const RwMatrix mat = pAttObj->m_entity->GetMatrix().ToRwMatrix();
				worldOrigin = {mat.pos.x, mat.pos.y, mat.pos.z};
				gizmoBasis = normalizedBasis({
					{mat.right.x, mat.right.y, mat.right.z},
					{mat.up.x,    mat.up.y,    mat.up.z},
					{mat.at.x,    mat.at.y,    mat.at.z},
				});
			}
			else {
				const sa::CVector pedPos = pPed->m_ped->GetPosition();
				worldOrigin = {pedPos.x, pedPos.y, pedPos.z + 0.45f};
			}

			if (mode == EditMode::Move) {
				displayVals = {pAttach->vecOffset.x, pAttach->vecOffset.y, pAttach->vecOffset.z};
			}
			else if (mode == EditMode::Rotate) {
				displayVals = {
					wrapEulerDeg(pAttach->vecRot.x),
					wrapEulerDeg(pAttach->vecRot.y),
					wrapEulerDeg(pAttach->vecRot.z),
				};
			}
			else {
				displayVals = {pAttach->vecScale.x, pAttach->vecScale.y, pAttach->vecScale.z};
			}
			std::snprintf(targetBadge, sizeof(targetBadge), L10N("Slot #%d · %s", "挂件 #%d · %s"),
				m_id, boneDisplayName(pAttach->iBoneID));
		}
	}

	if (!hasTarget) return;

	// ---- 1) Project & Render 3D On-Object Gizmo ----
	std::array<GizmoScreenAxis, 3> projAxes{};
	const float badgeR = std::clamp(u * 0.024f, 15.0f, 26.0f);
	const float fontTiny = std::max(12.0f, u * 0.019f);
	const float fontSmall = std::max(13.5f, u * 0.0215f);
	const float fontMed = std::max(15.0f, u * 0.024f);

	sa::CCamera& theCam = sa::getTheCamera();
	const int camIdx = (theCam.m_nActiveCam < 3) ? static_cast<int>(theCam.m_nActiveCam) : 0;
	const sa::CVector camSrc = theCam.m_aCams[camIdx].Source;
	const float camDist = std::sqrt(
		(worldOrigin.x - camSrc.x) * (worldOrigin.x - camSrc.x) +
		(worldOrigin.y - camSrc.y) * (worldOrigin.y - camSrc.y) +
		(worldOrigin.z - camSrc.z) * (worldOrigin.z - camSrc.z));
	const float gizmoWorldLen = computeGizmoWorldLength(camDist);

	Vec2 scrOrigin{};
	float originDepth = 0.0f;
	const bool originOnScreen = projectWorldPoint(worldOrigin, scrOrigin, originDepth);

	if (originOnScreen) {
		if (walkNow) {
			// Walk-Around mode: draw a clean target diamond beacon so object stays easy to track
			const float d = badgeR * 0.85f;
			const ImVec2 c(scrOrigin.x, scrOrigin.y);
			const ImVec2 pts[4] = {
				ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y),
				ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y),
			};
			bgDl->AddPolyline(pts, 4, rgba(0, 0, 0, 0.70f), true, 4.0f);
			bgDl->AddPolyline(pts, 4, rgba(pal.accentBright, 0.95f), true, 2.2f);
			bgDl->AddCircleFilled(c, 3.5f, rgba(pal.accent, 1.0f), 12);
		}
		else {
			const Vec3 dirs[3] = {gizmoBasis.right, gizmoBasis.up, gizmoBasis.at};
			for (int i = 0; i < 3; ++i) {
				const RwV3d pTipW{
					worldOrigin.x + dirs[i].x * gizmoWorldLen,
					worldOrigin.y + dirs[i].y * gizmoWorldLen,
					worldOrigin.z + dirs[i].z * gizmoWorldLen,
				};
				const RwV3d nTipW{
					worldOrigin.x - dirs[i].x * (gizmoWorldLen * 0.42f),
					worldOrigin.y - dirs[i].y * (gizmoWorldLen * 0.42f),
					worldOrigin.z - dirs[i].z * (gizmoWorldLen * 0.42f),
				};
				Vec2 sPos{}, sNeg{};
				float dPos = 0.0f, dNeg = 0.0f;
				if (projectWorldPoint(pTipW, sPos, dPos)) {
					if (!projectWorldPoint(nTipW, sNeg, dNeg)) {
						sNeg = scrOrigin;
					}
					projAxes[i] = {true, scrOrigin, sPos, sNeg, gizmoWorldLen};
				}
			}

			// Draw unselected axes first, then selected axis on top for crisp layering
			for (int pass = 0; pass < 2; ++pass) {
				for (int i = 0; i < 3; ++i) {
					if (!projAxes[i].valid) continue;
					const auto ax = static_cast<EditAxis>(i);
					const bool isSel = (ax == selAxis) || (mode == EditMode::Scale && uniformScale);
					if ((pass == 0 && isSel) || (pass == 1 && !isSel)) continue;

					const Rgb col = axisRgb(ax);
					const ImVec2 o(projAxes[i].origin.x, projAxes[i].origin.y);
					const ImVec2 tip(projAxes[i].posTip.x, projAxes[i].posTip.y);
					const ImVec2 neg(projAxes[i].negTip.x, projAxes[i].negTip.y);

					// Negative extension guide line
					bgDl->AddLine(o, neg, rgba(col, isSel ? 0.36f : 0.18f), 1.5f);

					// Positive axis shaft with dark contrast outline
					const float thick = isSel ? 3.8f : 2.3f;
					bgDl->AddLine(o, tip, rgba(0, 0, 0, 0.65f), thick + 2.2f);
					bgDl->AddLine(o, tip, rgba(col, isSel ? 1.0f : 0.78f), thick);

					// Mode-specific handle decoration + tactile circular X/Y/Z badge at tip
					const float r = isSel ? (badgeR * 1.12f) : (badgeR * 0.94f);
					if (isSel && draggingGizmo) {
						bgDl->AddCircle(tip, r + 5.5f, rgba(col, 0.45f), 28, 2.2f);
					}
					if (mode == EditMode::Rotate) {
						// Subtle orbital arc hint around rotation badge
						bgDl->AddCircle(tip, r + 3.2f, rgba(col, isSel ? 0.85f : 0.40f), 24, 1.4f);
					}
					else if (mode == EditMode::Scale) {
						// Square scale cap frame
						bgDl->AddRect(ImVec2(tip.x - r * 0.88f, tip.y - r * 0.88f),
							ImVec2(tip.x + r * 0.88f, tip.y + r * 0.88f),
							rgba(0, 0, 0, 0.65f), 4.0f, ImDrawCornerFlags_All, 3.0f);
					}

					bgDl->AddCircleFilled(ImVec2(tip.x, tip.y + 1.5f), r + 1.0f, rgba(0, 0, 0, 0.45f), 28);
					bgDl->AddCircleFilled(tip, r,
						isSel ? rgba(col, 0.95f) : rgba(LauncherPalette::kGlassDark, 0.88f), 28);
					bgDl->AddCircle(tip, r,
						isSel ? rgba(255, 255, 255, 0.95f) : rgba(col, 0.92f), 28, isSel ? 2.2f : 1.8f);

					drawCenteredText(bgDl, font, fontSmall, tip, axisShortName(ax),
						isSel ? rgba(LauncherPalette::kBgDeep, 1.0f) : rgba(col, 1.0f),
						isSel ? rgba(255, 255, 255, 0.0f) : rgba(0, 0, 0, 0.75f));
				}
			}

			// Center pivot dot
			bgDl->AddCircleFilled(ImVec2(scrOrigin.x, scrOrigin.y), 5.0f, rgba(0, 0, 0, 0.75f), 16);
			bgDl->AddCircleFilled(ImVec2(scrOrigin.x, scrOrigin.y), 3.4f, rgba(255, 255, 255, 0.95f), 16);

			// Live floating value readout beside pivot while actively dragging 3D gizmo!
			if (draggingGizmo) {
				char liveTip[64] = {};
				const float v = (selAxis == EditAxis::X) ? displayVals.x
					: ((selAxis == EditAxis::Y) ? displayVals.y : displayVals.z);
				if (mode == EditMode::Move) {
					std::snprintf(liveTip, sizeof(liveTip), "%s  %+0.3f m", axisShortName(selAxis), v);
				}
				else if (mode == EditMode::Rotate) {
					std::snprintf(liveTip, sizeof(liveTip), "R%s  %.1f°", axisShortName(selAxis), v);
				}
				else {
					if (uniformScale) {
						std::snprintf(liveTip, sizeof(liveTip), "XYZ  %.2fx", v);
					}
					else {
						std::snprintf(liveTip, sizeof(liveTip), "S%s  %.2fx", axisShortName(selAxis), v);
					}
				}
				const ImVec2 tSz = font->CalcTextSizeA(fontSmall, FLT_MAX, 0.0f, liveTip);
				const ImVec2 boxMin(scrOrigin.x - tSz.x * 0.5f - 10.0f, scrOrigin.y + 14.0f);
				const ImVec2 boxMax(scrOrigin.x + tSz.x * 0.5f + 10.0f, scrOrigin.y + 14.0f + tSz.y + 8.0f);
				bgDl->AddRectFilled(boxMin, boxMax, rgba(LauncherPalette::kBgDeep, 0.88f), 7.0f);
				bgDl->AddRect(boxMin, boxMax, rgba(axisRgb(selAxis), 0.90f), 7.0f, ImDrawCornerFlags_All, 1.4f);
				drawCenteredText(bgDl, font, fontSmall,
					ImVec2((boxMin.x + boxMax.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f),
					liveTip, rgba(LauncherPalette::kTextPrimary, 1.0f), rgba(0, 0, 0, 0.6f));
			}
		}
	}

	// ---- 2) Build & Render Unified Bottom-Center Studio Dock ----
	// All controls live in ONE bottom-center studio card (3 rows in Edit Mode; collapses to 1 slim
	// row above the RP strip in Walk Mode), leaving the top-left/top-center Chat 100% unobstructed
	// so players can freely tap Chat to talk or run /commands while editing!
	std::vector<UiHitButton> hitButtons;
	hitButtons.reserve(20);

	const float pad = std::max(6.0f, u * 0.008f);
	const float gap = std::max(5.0f, u * 0.0065f);
	const float row0H = std::clamp(u * 0.044f, 30.0f, 44.0f); // Mode + Space/Zoom/Walk + Reset/Cancel/Save
	const float row1H = std::clamp(u * 0.041f, 28.0f, 42.0f); // X / Y / Z Readouts + Speed Gears
	const float row2H = std::clamp(u * 0.049f, 34.0f, 48.0f); // [ - ] + Scrub Track + [ + ]

	struct TopChipSpec {
		const char* label;
		int actionId;
		PillStyle style;
		float weight;
	};

	const bool canScale = (m_type == TYPE_ATTACHED_OBJECT);
	const bool showSpaceToggle = (m_type == TYPE_OBJECT && mode == EditMode::Move)
		|| (m_type == TYPE_ATTACHED_OBJECT && mode == EditMode::Scale);

	const char* spaceLbl = "";
	bool spaceHighlighted = false;
	if (m_type == TYPE_OBJECT && mode == EditMode::Move) {
		spaceLbl = localSpace ? L10N("Local", "本地轴") : L10N("World", "世界轴");
		spaceHighlighted = localSpace;
	}
	else if (m_type == TYPE_ATTACHED_OBJECT && mode == EditMode::Scale) {
		spaceLbl = uniformScale ? L10N("Uniform", "等比") : L10N("Single", "单轴");
		spaceHighlighted = uniformScale;
	}

	std::vector<TopChipSpec> chips;
	chips.push_back({L10N("Move", "移动"), kActModeMove,
		(!walkNow && mode == EditMode::Move) ? PillStyle::SelectedAccent : PillStyle::Normal, 1.0f});
	chips.push_back({L10N("Rotate", "旋转"), kActModeRotate,
		(!walkNow && mode == EditMode::Rotate) ? PillStyle::SelectedAccent : PillStyle::Normal, 1.0f});
	if (canScale) {
		chips.push_back({L10N("Scale", "缩放"), kActModeScale,
			(!walkNow && mode == EditMode::Scale) ? PillStyle::SelectedAccent : PillStyle::Normal, 1.0f});
	}
	if (showSpaceToggle && !walkNow) {
		chips.push_back({spaceLbl, kActToggleSpaceOrUniform,
			spaceHighlighted ? PillStyle::SelectedTinted : PillStyle::Normal, 1.10f});
	}
	chips.push_back({L10N("Zoom", "镜头"), kActCycleZoom, PillStyle::Normal, 0.96f});
	chips.push_back({walkNow ? L10N("Edit", "回编辑") : L10N("Walk", "走动"), kActToggleWalk,
		walkNow ? PillStyle::SelectedAccent : PillStyle::Normal, 1.08f});
	chips.push_back({L10N("Reset", "重置"), kActReset, PillStyle::Normal, 0.96f});
	chips.push_back({L10N("Cancel", "取消"), kActCancel, PillStyle::DangerOutline, 0.98f});
	chips.push_back({L10N("Save", "保存"), kActSave, PillStyle::PrimarySave, 1.08f});

	const ImVec2 badgeTextSz = font->CalcTextSizeA(fontTiny, FLT_MAX, 0.0f, targetBadge);
	const float badgeW = badgeTextSz.x + pad * 2.4f;
	const float baseChipW = std::clamp(u * 0.074f, 48.0f, 84.0f);

	float totalWeight = 0.0f;
	for (const auto& ch : chips) {
		totalWeight += ch.weight;
	}
	const float naturalRow0InnerW = badgeW + gap * 1.3f
		+ baseChipW * totalWeight
		+ gap * static_cast<float>(chips.size() > 0 ? (chips.size() - 1) : 0);

	const float maxDockW = std::min(screenW - 24.0f, 900.0f);
	const float minDockW = walkNow ? 360.0f : 460.0f;
	const float targetDockW = walkNow
		? (naturalRow0InnerW + pad * 2.4f)
		: std::max(screenW * 0.60f, naturalRow0InnerW + pad * 2.4f);
	const float dockW = std::clamp(targetDockW, std::min(minDockW, maxDockW), maxDockW);
	const float dockH = walkNow
		? (pad * 2.2f + row0H)
		: (pad * 2.4f + row0H + gap * 1.05f + row1H + gap * 1.15f + row2H);
	const float dockX = (screenW - dockW) * 0.5f;
	// In Walk Mode, TouchHud renders the bottom RP Strip (~u*0.072 tall); float just above it so
	// neither RP Strip nor Joystick/Jump overlaps the editor pill!
	const float bottomMargin = walkNow
		? std::max(56.0f, u * 0.088f)
		: std::max(10.0f, u * 0.014f);
	const float dockY = std::max(8.0f, screenH - dockH - bottomMargin);
	const Rect bottomDockRect{dockX, dockY, dockW, dockH};

	// Outer glass shadow + obsidian studio dock card
	fgDl->AddRectFilled(ImVec2(dockX, dockY + 3.0f), ImVec2(dockX + dockW, dockY + dockH + 3.0f),
		rgba(0, 0, 0, 0.40f), 14.0f);
	fgDl->AddRectFilled(ImVec2(dockX, dockY), ImVec2(dockX + dockW, dockY + dockH),
		rgba(LauncherPalette::kBgDeep, 0.90f), 14.0f);
	fgDl->AddRect(ImVec2(dockX, dockY), ImVec2(dockX + dockW, dockY + dockH),
		rgba(pal.accent, 0.30f), 14.0f, ImDrawCornerFlags_All, 1.25f);

	const float innerX = dockX + pad * 1.2f;
	const float innerW = dockW - pad * 2.4f;
	const float r0Y = dockY + pad * 1.1f;

	// Row 0: Target Badge + Mode/Space/Zoom/Walk/Reset/Cancel/Save chips stretched cleanly across innerW
	const float badgeClampedW = std::min(badgeW, innerW * 0.28f);
	const Rect badgeRect{innerX, r0Y, badgeClampedW, row0H};
	fgDl->AddRectFilled(ImVec2(badgeRect.x, badgeRect.y),
		ImVec2(badgeRect.x + badgeRect.w, badgeRect.y + badgeRect.h),
		rgba(LauncherPalette::kSurface2, 0.86f), 8.0f);
	drawCenteredText(fgDl, font, fontTiny,
		ImVec2(badgeRect.x + badgeRect.w * 0.5f, badgeRect.y + badgeRect.h * 0.5f),
		targetBadge, rgba(pal.accentBright, 0.96f), rgba(0, 0, 0, 0.6f));

	float curX = innerX + badgeClampedW + gap * 1.3f;
	const float chipsAvailW = std::max(60.0f, innerW - badgeClampedW - gap * 1.3f
		- gap * static_cast<float>(chips.size() > 0 ? (chips.size() - 1) : 0));
	const float unitChipW = chipsAvailW / std::max(1.0f, totalWeight);

	for (const auto& ch : chips) {
		const float w = unitChipW * ch.weight;
		const Rect r{curX, r0Y, w, row0H};
		drawPillButton(fgDl, font, fontSmall, r, ch.label, ch.style,
			pressedAct == ch.actionId, pal, pal.accent);
		hitButtons.push_back({r, ch.actionId});
		curX += w + gap;
	}

	Rect scrubRect{};
	Rect minusRect{};
	Rect plusRect{};

	if (!walkNow) {
		const float r1Y = r0Y + row0H + gap * 1.05f;

		// Subtle divider line between Header/Action row and Precision Axis/Scrub rows
		fgDl->AddLine(ImVec2(innerX, r1Y - gap * 0.52f), ImVec2(innerX + innerW, r1Y - gap * 0.52f),
			rgba(255, 255, 255, 0.08f), 1.0f);

		// Row 1 Left (62% width): X / Y / Z Axis Selector Pills with Live Numeric Readout
		const float axesGroupW = innerW * 0.62f;
		const float axisPillW = (axesGroupW - gap * 2.0f) / 3.0f;
		const float vals[3] = {displayVals.x, displayVals.y, displayVals.z};
		const int axisActions[3] = {kActAxisX, kActAxisY, kActAxisZ};

		for (int i = 0; i < 3; ++i) {
			const auto ax = static_cast<EditAxis>(i);
			const bool isSel = (ax == selAxis) || (mode == EditMode::Scale && uniformScale);
			char lbl[48] = {};
			if (mode == EditMode::Move) {
				if (m_type == TYPE_OBJECT) {
					std::snprintf(lbl, sizeof(lbl), "%s %+0.2f", axisShortName(ax), vals[i]);
				}
				else {
					std::snprintf(lbl, sizeof(lbl), "%s %+0.3f", axisShortName(ax), vals[i]);
				}
			}
			else if (mode == EditMode::Rotate) {
				std::snprintf(lbl, sizeof(lbl), "%s %.1f°", axisShortName(ax), vals[i]);
			}
			else {
				std::snprintf(lbl, sizeof(lbl), "%s %.2fx", axisShortName(ax), vals[i]);
			}

			const Rect ar{innerX + i * (axisPillW + gap), r1Y, axisPillW, row1H};
			drawPillButton(fgDl, font, fontTiny, ar, lbl,
				isSel ? PillStyle::SelectedTinted : PillStyle::Normal,
				pressedAct == axisActions[i], pal, axisRgb(ax));
			hitButtons.push_back({ar, axisActions[i]});
		}

		// Row 1 Right (36% width): Speed Gear Pills (Fine / Normal / Fast)
		const float gearGroupX = innerX + axesGroupW + innerW * 0.02f;
		const float gearGroupW = innerW * 0.36f;
		const float gearW = (gearGroupW - gap * 2.0f) / 3.0f;
		const struct { const char* lbl; SpeedGear g; int act; } gears[3] = {
			{L10N("Fine", "微调"),   SpeedGear::Fine,   kActGearFine},
			{L10N("1x",   "标准"),   SpeedGear::Normal, kActGearNormal},
			{L10N("Fast", "快速"),   SpeedGear::Fast,   kActGearFast},
		};
		for (int i = 0; i < 3; ++i) {
			const Rect gr{gearGroupX + i * (gearW + gap), r1Y, gearW, row1H};
			drawPillButton(fgDl, font, fontTiny, gr, gears[i].lbl,
				(gear == gears[i].g) ? PillStyle::SelectedAccent : PillStyle::Normal,
				pressedAct == gears[i].act, pal);
			hitButtons.push_back({gr, gears[i].act});
		}

		// Row 2: [ - ]  [ <<< Precision Scrub Jog Track >>> ]  [ + ]
		const float r2Y = r1Y + row1H + gap * 1.15f;
		const float stepBtnW = std::clamp(row2H * 1.35f, 48.0f, 74.0f);
		minusRect = {innerX, r2Y, stepBtnW, row2H};
		plusRect = {innerX + innerW - stepBtnW, r2Y, stepBtnW, row2H};
		scrubRect = {innerX + stepBtnW + gap, r2Y, innerW - (stepBtnW + gap) * 2.0f, row2H};

		drawPillButton(fgDl, font, fontMed, minusRect, "-",
			holdingMinus ? PillStyle::SelectedAccent : PillStyle::Normal, holdingMinus, pal);
		drawPillButton(fgDl, font, fontMed, plusRect, "+",
			holdingPlus ? PillStyle::SelectedAccent : PillStyle::Normal, holdingPlus, pal);

		// Render recessed tactile Scrub / Jog Ruler Track
		const ImVec2 sMin(scrubRect.x, scrubRect.y);
		const ImVec2 sMax(scrubRect.x + scrubRect.w, scrubRect.y + scrubRect.h);
		const Rgb activeAxisCol = axisRgb(selAxis);
		const float sRad = std::min(9.0f, scrubRect.h * 0.28f);

		fgDl->AddRectFilled(sMin, sMax, rgba(LauncherPalette::kGlassDark, 0.95f), sRad);

		// Animated vernier ruler ticks inside scrub track
		fgDl->PushClipRect(ImVec2(sMin.x + 4.0f, sMin.y + 2.0f), ImVec2(sMax.x - 4.0f, sMax.y - 2.0f), true);
		const float tickSpacing = 14.0f;
		float phase = std::fmod(scrubPhase, tickSpacing);
		if (phase < 0.0f) phase += tickSpacing;
		int tickIdx = 0;
		for (float tx = sMin.x + phase; tx < sMax.x; tx += tickSpacing, ++tickIdx) {
			const bool major = ((tickIdx % 5) == 0);
			const float th = major ? (scrubRect.h * 0.34f) : (scrubRect.h * 0.18f);
			const ImU32 tc = draggingScrub
				? rgba(activeAxisCol, major ? 0.55f : 0.28f)
				: rgba(255, 255, 255, major ? 0.20f : 0.09f);
			fgDl->AddLine(ImVec2(tx, sMax.y - th - 2.0f), ImVec2(tx, sMax.y - 2.0f), tc, major ? 1.6f : 1.0f);
		}
		fgDl->PopClipRect();

		fgDl->AddRect(sMin, sMax,
			draggingScrub ? rgba(activeAxisCol, 0.95f) : rgba(activeAxisCol, 0.42f),
			sRad, ImDrawCornerFlags_All, draggingScrub ? 2.0f : 1.3f);

		const char* scrubHint = draggingScrub
			? L10N("<< Sliding Fine Adjustment >>", "<< 正在滑动精细调节 >>")
			: L10N("<< Slide Track or Drag 3D Axis · Drag Screen to Orbit >>",
			       "<< 左右滑动微调 · 拖动物体 3D 轴 · 空白处滑屏转视角 >>");
		drawCenteredText(fgDl, font, fontTiny,
			ImVec2(scrubRect.x + scrubRect.w * 0.5f, scrubRect.y + scrubRect.h * 0.44f),
			scrubHint,
			draggingScrub ? rgba(LauncherPalette::kTextPrimary, 1.0f) : rgba(LauncherPalette::kTextDim, 0.92f),
			rgba(0, 0, 0, 0.70f));
	}

	// Publish updated hit boxes & projected gizmo screen coords for NVEvent touch thread
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_state.bottomDockRect = bottomDockRect;
		g_state.scrubTrackRect = scrubRect;
		g_state.minusBtnRect = minusRect;
		g_state.plusBtnRect = plusRect;
		g_state.buttons = std::move(hitButtons);
		g_state.gizmoAxes = projAxes;
		g_state.badgeHitRadius = std::max(26.0f, badgeR * 1.50f);
		g_state.shaftHitRadius = std::max(15.0f, u * 0.022f);
	}
}
