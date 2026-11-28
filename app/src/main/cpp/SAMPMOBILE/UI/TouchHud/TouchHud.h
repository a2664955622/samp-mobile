#pragma once

#include "TouchHudCore.h"
#include "../../GameSA/include/Core/Vector2D.h"
#include "../../GameSA/include/Enums/WidgetIDs.h"

#include <cstdint>

class ImGuiRenderer;

namespace TouchHud {

// Lifecycle & layout persistence (GameMain thread)
void initialize(float screenW, float screenH);
void setScreenSize(float screenW, float screenH);
bool loadLayout();
bool saveLayout();

// Screenshot visibility suppression (GameMain thread)
void setScreenshotHidden(bool hidden);

// Thread-safe touch entry points (called from NVEvent thread via InputArbiter)
bool isEditorOpen();
bool isRpStripHitSnapshot(float x, float y);
bool handleTouch(int type, int pointerId, float x, float y);
void handleCancel();
// Injects camera-orbit pixel deltas from overlays (e.g. Object Editor empty-space drag)
void injectExternalLookDelta(float dx, float dy);

// Physical Android keyboard / emulator key input (GameMain thread, from Java.cpp)
void handleHardwareKey(int androidKeyCode, bool isDown);
void clearHardwareKeys();

// In-game live HUD editor control (GameMain / any thread via openEditor)
void openEditor();
void closeEditor(bool saveChanges);

// Per-logic-frame engine update (called inside CTouchInterface::Clear hook on GameMain)
void stepGameFrame();

// Current logic-frame sampled state (GameMain thread only)
const touch_hud::TouchHudCore::FrameOutput& currentFrame();

// Aircraft manual camera orbit offset (radians, GameMain thread only)
bool getAircraftCamOrbit(float& outYaw, float& outPitch);

// Last valid on-foot camera look heading (GTA ped rotation radians, GameMain thread only)
bool getLastOnFootCamHeading(float& outHeading);

// Edge helpers for Pad.cpp hooks (GameMain thread, scoped strictly to current logic frame)
bool isInteractFJustPressed();
bool isJumpJustPressed();
bool consumeWeaponPrevEdge();
bool consumeWeaponNextEdge();
bool consumeCrouchEdge();

// Virtual CTouchInterface widget query overrides (GameMain thread)
// Returns true if TouchHud handled the query and wrote *outResult (and optional *outVec).
bool queryWidgetTouched(sa::WidgetIDs id, sa::CVector2D* outVec, uint32_t& outResult);
bool queryWidgetReleased(sa::WidgetIDs id, sa::CVector2D* outVec, uint32_t& outResult);
bool queryWidgetHeldDown(sa::WidgetIDs id, uint32_t& outResult);

// Render PUBG-style Touch HUD and (when open) the live HUD Editor (GameMain thread, ImGui pass)
void render(ImGuiRenderer* renderer);

// Internal shared accessors for TouchHudRenderer.cpp and TouchHudEditor.cpp
struct RenderSnapshot {
	touch_hud::TouchHudLayout layout{};
	touch_hud::ContextCapabilities caps{};
	touch_hud::TouchHudCore::VisualState vis{};
	float screenW = 1920.0f;
	float screenH = 1080.0f;
	bool visible = false;
	bool editorOpen = false;
};

RenderSnapshot captureRenderSnapshot();
touch_hud::TouchHudLayout getLayoutCopy();
void applyLayout(const touch_hud::TouchHudLayout& layout, bool persist);
void renderTouchHudOverlay(ImGuiRenderer* renderer, const RenderSnapshot& snap);
void renderTouchHudEditor(ImGuiRenderer* renderer, float screenW, float screenH);
void onEditorOpened(const touch_hud::TouchHudLayout& currentLayout, bool inVehicleNow);
bool handleEditorTouch(int type, int pointerId, float x, float y);

} // namespace TouchHud
