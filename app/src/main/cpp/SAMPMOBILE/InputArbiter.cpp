#include "InputArbiter.h"
#include "InputArbiterCore.h"

#include "Client.h"
#include "Settings.h"
#include "Game/CleoEngine.h"
#include "Game/Game.h"
#include "Java/EditObject.h"
#include "Net/MenuPool.h"
#include "Net/NetGame.h"
#include "Net/ObjectPool.h"
#include "Net/TextDrawPool.h"
#include "UI/UI.h"
#include "UI/SAMPWidgets/Keyboard.h"
#include "UI/SAMPWidgets/Scoreboard.h"
#include "UI/TouchHud/TouchHud.h"

#include <atomic>
#include <chrono>

namespace {

std::atomic<uint32_t> g_overlayFlags{input_arbiter::kOverlayNone};
std::atomic<int64_t> g_javaKeyboardClosedMs{0};
std::atomic<bool> g_pendingBackDismiss{false};
std::atomic<InputArbiter::ExternalTouchFilterFn> g_externalTouchFilter{nullptr};
input_arbiter::GtaPointerTracker g_gtaPointers;

// NVEvent-thread-only per-pointer ownership bitmasks (pointerId 0..15) so a finger
// tapping Chat / Voice / Radar / RP Strip never steals or interferes with other layers.
uint32_t g_imguiOwnedPointers = 0;
uint32_t g_radarOwnedPointers = 0;
uint32_t g_rpModalOwnedPointers = 0;

inline bool pointerBit(int num, uint32_t& mask)
{
	if (num < 0 || num >= 16) return false;
	mask = 1u << static_cast<unsigned>(num);
	return true;
}

int64_t nowSteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

void updateOverlayBit(uint32_t bit, bool active)
{
	if (active) {
		g_overlayFlags.fetch_or(bit, std::memory_order_acq_rel);
	}
	else {
		g_overlayFlags.fetch_and(~bit, std::memory_order_acq_rel);
	}
}

uint32_t computeGameMainOverlayBits()
{
	uint32_t bits = input_arbiter::kOverlayNone;

	if (TouchHud::isEditorOpen()) {
		bits |= input_arbiter::kOverlayHudEditor;
	}
	if (pUI) {
		if (pUI->keyboard() && pUI->keyboard()->visible()) {
			bits |= input_arbiter::kOverlayImGuiKeyboard;
		}
		if (pUI->scoreboard() && pUI->scoreboard()->visible()) {
			bits |= input_arbiter::kOverlayScoreboard;
		}
	}
	if (CleoEngine::isMenuOpen()) {
		bits |= input_arbiter::kOverlayCleoMenu;
	}
	if (EditObject::m_visible.load(std::memory_order_acquire)) {
		bits |= input_arbiter::kOverlayEditObject;
	}
	if (pNetGame) {
		if (CMenuPool* pMenuPool = pNetGame->GetMenuPool(); pMenuPool && pMenuPool->IsVisible()) {
			bits |= input_arbiter::kOverlayServerMenu;
		}
		if (CObjectPool* pObjPool = pNetGame->GetObjectPool(); pObjPool && pObjPool->IsSelectState()) {
			bits |= input_arbiter::kOverlayObjectSelect;
		}
		if (CTextDrawPool* pTdPool = pNetGame->GetTextDrawPool(); pTdPool && pTdPool->GetState()) {
			bits |= input_arbiter::kOverlayTextDrawSelect;
		}
	}

	return bits;
}

void sendToImGui(int type, int num, int posX, int posY)
{
	if (!pUI) return;
	const ImVec2 pt(static_cast<float>(posX), static_cast<float>(posY));
	switch (type) {
		case 2: pUI->enqueueTouchEvent(pt, TouchType::push, false, num); break;
		case 3: pUI->enqueueTouchEvent(pt, TouchType::move, false, num); break;
		case 1: pUI->enqueueTouchEvent(pt, TouchType::pop,  false, num); break;
	}
}

} // namespace

extern "C" __attribute__((visibility("default")))
void SAMPMOBILE_RegisterTouchFilter(InputArbiter::ExternalTouchFilterFn fn)
{
	InputArbiter::registerExternalTouchFilter(fn);
}

namespace InputArbiter {

void setJavaDialogVisible(bool visible)
{
	updateOverlayBit(input_arbiter::kOverlayJavaDialog, visible);
}

void setJavaKeyboardVisible(bool visible)
{
	const uint32_t prev = visible
			? g_overlayFlags.fetch_or(input_arbiter::kOverlayJavaKeyboard, std::memory_order_acq_rel)
			: g_overlayFlags.fetch_and(~input_arbiter::kOverlayJavaKeyboard, std::memory_order_acq_rel);
	const bool wasVisible = (prev & input_arbiter::kOverlayJavaKeyboard) != 0u;
	if (wasVisible && !visible) {
		g_javaKeyboardClosedMs.store(nowSteadyMs(), std::memory_order_release);
	}
}

bool requestNativeBackDismiss()
{
	const uint32_t flags = g_overlayFlags.load(std::memory_order_acquire);
	if (!input_arbiter::hasNativeBackTarget(flags)) {
		return false;
	}
	g_pendingBackDismiss.store(true, std::memory_order_release);
	return true;
}

void publishGameMainState()
{
	const uint32_t gameBits = computeGameMainOverlayBits();
	if (pGame) {
		pGame->SetHudHidden(Game::HUD_HIDE_SCOREBOARD, (gameBits & input_arbiter::kOverlayScoreboard) != 0u);
	}

	uint32_t cur = g_overlayFlags.load(std::memory_order_relaxed);
	while (true) {
		const uint32_t next = (cur & input_arbiter::kJavaOverlayMask) | (gameBits & input_arbiter::kGameMainOverlayMask);
		if (g_overlayFlags.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			break;
		}
	}
}

void processPendingBackDismiss()
{
	if (g_pendingBackDismiss.exchange(false, std::memory_order_acq_rel)) {
		dismissTopNativeModal();
	}
}

void dismissTopNativeModal()
{
	const uint32_t gameBits = computeGameMainOverlayBits();
	switch (input_arbiter::resolveTopBackTarget(gameBits)) {
		case input_arbiter::NativeBackTarget::ImGuiKeyboard:
			if (pUI && pUI->keyboard()) {
				pUI->keyboard()->hide();
			}
			break;

		case input_arbiter::NativeBackTarget::HudEditor:
			TouchHud::closeEditor(false);
			break;

		case input_arbiter::NativeBackTarget::Scoreboard:
			if (pUI && pUI->scoreboard()) {
				pUI->scoreboard()->setVisible(false);
			}
			break;

		case input_arbiter::NativeBackTarget::CleoMenu:
			CleoEngine::dismissMenu();
			break;

		case input_arbiter::NativeBackTarget::ServerMenu:
			if (pNetGame && pNetGame->GetMenuPool()) {
				pNetGame->GetMenuPool()->ExitMenu();
			}
			break;

		case input_arbiter::NativeBackTarget::EditObject:
			EditObject::exitEdit();
			break;

		case input_arbiter::NativeBackTarget::ObjectSelect:
			if (pNetGame && pNetGame->GetObjectPool()) {
				pNetGame->GetObjectPool()->SetSelectState(false);
			}
			break;

		case input_arbiter::NativeBackTarget::TextDrawSelect:
			if (pNetGame && pNetGame->GetTextDrawPool()) {
				pNetGame->GetTextDrawPool()->SetSelectState(false, 0, true);
			}
			break;

		case input_arbiter::NativeBackTarget::None:
			break;
	}
}

bool isRadarSuppressed()
{
	const uint32_t flags = g_overlayFlags.load(std::memory_order_acquire);
	const int64_t closedAt = g_javaKeyboardClosedMs.load(std::memory_order_acquire);
	return input_arbiter::isRadarSuppressed(flags, nowSteadyMs(), closedAt);
}

bool shouldBypassExternalTouch()
{
	return input_arbiter::shouldBypassExternalTouch(g_overlayFlags.load(std::memory_order_acquire));
}

void onTouchCancel()
{
	g_imguiOwnedPointers = 0;
	g_radarOwnedPointers = 0;
	g_rpModalOwnedPointers = 0;
	EditObject::onTouchCancel();
	TouchHud::handleCancel();
	if (pUI != nullptr) {
		pUI->enqueueTouchEvent(ImVec2(0.0f, 0.0f), TouchType::pop, true);
	}
}

void registerExternalTouchFilter(ExternalTouchFilterFn fn)
{
	g_externalTouchFilter.store(fn, std::memory_order_release);
}

void routeGtaTouch(int type, int num, int posX, int posY, ForwardGtaTouchFn forwardToGta)
{
	if (!forwardToGta) return;

	if (Game::IsGamePaused()) {
		g_imguiOwnedPointers = 0;
		g_radarOwnedPointers = 0;
		TouchHud::handleCancel();
		g_gtaPointers.forward(type, num, posX, posY, forwardToGta);
		return;
	}

	// 0. Live In-Game Touch HUD Editor owns 100% of touch input while open
	if (TouchHud::isEditorOpen()) {
		g_imguiOwnedPointers = 0;
		g_radarOwnedPointers = 0;
		g_gtaPointers.releaseAll(forwardToGta);
		TouchHud::handleTouch(type, num, static_cast<float>(posX), static_cast<float>(posY));
		return;
	}

	const uint32_t flags = g_overlayFlags.load(std::memory_order_acquire);
	const bool javaDialogOpen = (flags & input_arbiter::kOverlayJavaDialog) != 0u;
	const bool modalBlocking = input_arbiter::isModalBlockingGtaTouch(flags);

	// 1. Java Dialog owns full-screen modal focus
	if (javaDialogOpen) {
		onTouchCancel();
		g_gtaPointers.releaseAll(forwardToGta);
		return;
	}

	// 2. Modal native ImGui overlays (ImGui Keyboard, Scoreboard)
	if (modalBlocking) {
		g_radarOwnedPointers = 0;
		g_gtaPointers.releaseAll(forwardToGta);

		const float fx = static_cast<float>(posX);
		const float fy = static_cast<float>(posY);
		uint32_t pBit = 0;
		const bool hasBit = pointerBit(num, pBit);

		// When ONLY Scoreboard is open (no keyboard/dialog), keep bottom RP Strip live
		// so the player can tap TAB or ESC directly to close Scoreboard!
		const bool scoreboardOnly = (flags & input_arbiter::kOverlayScoreboard) != 0u
			&& (flags & (input_arbiter::kOverlayJavaDialog | input_arbiter::kOverlayJavaKeyboard | input_arbiter::kOverlayImGuiKeyboard)) == 0u;
		if (scoreboardOnly) {
			const bool ownsRp = hasBit && ((g_rpModalOwnedPointers & pBit) != 0u);
			if (ownsRp || (type == 2 && TouchHud::isRpStripHitSnapshot(fx, fy))) {
				if (hasBit) {
					if (type == 2) g_rpModalOwnedPointers |= pBit;
					else if (type == 1) g_rpModalOwnedPointers &= ~pBit;
				}
				TouchHud::handleTouch(type, num, fx, fy);
				return;
			}
		}
		else {
			g_rpModalOwnedPointers = 0;
			TouchHud::handleCancel();
		}

		sendToImGui(type, num, posX, posY);
		return;
	}
	g_rpModalOwnedPointers = 0;

	const ImVec2 disp = pUI ? pUI->displaySizeSnapshot() : ImVec2(1920.0f, 1080.0f);
	const float fx = static_cast<float>(posX);
	const float fy = static_cast<float>(posY);
	uint32_t pBit = 0;
	const bool hasBit = pointerBit(num, pBit);

	// 3. Non-modal external overlays (CLEO menu/gestures, MonetLoader touch filter)
	if (CleoEngine::handleTouch(type, num, posX, posY, disp.x, disp.y)) {
		g_gtaPointers.releasePointer(num, forwardToGta);
		return;
	}
	if (auto touchFilter = g_externalTouchFilter.load(std::memory_order_acquire)) {
		if (touchFilter(type, num, posX, posY)) {
			g_gtaPointers.releasePointer(num, forwardToGta);
			return;
		}
	}

	// 4. Continue pointer already owned by ImGui overlay (Chat / Voice / Spawn) or GTA Radar
	if (hasBit && (g_imguiOwnedPointers & pBit) != 0u) {
		sendToImGui(type, num, posX, posY);
		if (type == 1) g_imguiOwnedPointers &= ~pBit;
		return;
	}
	if (hasBit && (g_radarOwnedPointers & pBit) != 0u) {
		g_gtaPointers.forward(type, num, posX, posY, forwardToGta);
		if (type == 1) g_radarOwnedPointers &= ~pBit;
		return;
	}

	// 5. Server interactive pools (Selectable TextDraws / Server Menus / SelectObject)
	if (pNetGame) {
		const bool tdSelecting = pNetGame->GetTextDrawPool() && pNetGame->GetTextDrawPool()->GetState();
		const bool menuVisible = pNetGame->GetMenuPool() && pNetGame->GetMenuPool()->IsVisible();
		const bool objSelecting = pNetGame->GetObjectPool() && pNetGame->GetObjectPool()->IsSelectState();

		if (tdSelecting || menuVisible || objSelecting) {
			const bool ownsRp = hasBit && ((g_rpModalOwnedPointers & pBit) != 0u);
			if (ownsRp || (type == 2 && TouchHud::isRpStripHitSnapshot(fx, fy))) {
				if (hasBit) {
					if (type == 2) g_rpModalOwnedPointers |= pBit;
					else if (type == 1) g_rpModalOwnedPointers &= ~pBit;
				}
				TouchHud::handleTouch(type, num, fx, fy);
				return;
			}
		}

		// During TextDraw selection, give TextDraws priority everywhere except RP Strip (ESC/TAB)
		if (tdSelecting) {
			if (!pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY, num)) {
				g_gtaPointers.releasePointer(num, forwardToGta);
				return;
			}
			// Prevent background drags during TextDraw selection from spinning the camera
			return;
		}

		if (pNetGame->GetMenuPool() && !pNetGame->GetMenuPool()->enqueueTouchEvent(type, posX, posY)) {
			g_gtaPointers.releasePointer(num, forwardToGta);
			return;
		}
		if (pNetGame->GetTextDrawPool() && !pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY, num)) {
			g_gtaPointers.releasePointer(num, forwardToGta);
			return;
		}
		if (pNetGame->GetObjectPool() && !pNetGame->GetObjectPool()->enqueueTouchEvent(type, posX, posY)) {
			g_gtaPointers.releasePointer(num, forwardToGta);
			return;
		}
	}

	// 6a. Object Editor Studio Dock panel buttons take priority over background overlays
	// while keeping Chat & VoiceButton 100% clickable outside the bottom Studio Dock!
	if (type == 2 && EditObject::isUiPanelHitSnapshot(fx, fy)) {
		if (EditObject::handleTouch(type, num, fx, fy)) {
			g_gtaPointers.releasePointer(num, forwardToGta);
			return;
		}
	}

	// 6b. On finger Down, check interactive ImGui widgets (Chat, VoiceButton, Spawn)
	if (type == 2 && pUI && pUI->isInteractiveOverlayHitSnapshot(ImVec2(fx, fy))) {
		if (hasBit) g_imguiOwnedPointers |= pBit;
		sendToImGui(type, num, posX, posY);
		return;
	}

	// 6c. Native 3D Object / Attached-Object Editor (Gizmo + Precision Dock + 360-degree Camera Orbit;
	// when Walk Mode is toggled on, non-bar touches fall through to TouchHud below!)
	if (EditObject::m_visible.load(std::memory_order_acquire)) {
		if (EditObject::handleTouch(type, num, fx, fy)) {
			g_gtaPointers.releasePointer(num, forwardToGta);
			return;
		}
	}

	// 7. On finger Down, check top-left GTA Radar tap-to-open-map region
	if (type == 2 && Settings::showRadar() && Settings::radarTapMap() && !isRadarSuppressed()
			&& !TouchHud::isRpStripHitSnapshot(fx, fy)) {
		const float radarMax = disp.y * 0.275f;
		if (fx >= 0.0f && fy >= 0.0f && fx <= radarMax && fy <= radarMax) {
			if (hasBit) g_radarOwnedPointers |= pBit;
			g_gtaPointers.forward(type, num, posX, posY, forwardToGta);
			return;
		}
	}

	// 8. Full-takeover multi-touch PUBG/RP HUD & zero-latency camera look
	if (pGame && pGame->IsGameInputEnabled()) {
		if (TouchHud::handleTouch(type, num, fx, fy)) {
			return;
		}
		// Fallback (e.g. during Splash / Spawn / Hidden HUD): let ImGui process taps cleanly
		sendToImGui(type, num, posX, posY);
	}
	else {
		TouchHud::handleCancel();
		g_gtaPointers.releaseAll(forwardToGta);
	}
}

} // namespace InputArbiter
