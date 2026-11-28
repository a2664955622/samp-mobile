#pragma once

#include "TouchHudConfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace touch_hud {

enum class TouchAction : uint8_t {
	Down = 0,
	Move = 1,
	Up   = 2,
};

enum class HudContext : uint8_t {
	Hidden = 0,
	OnFoot,
	Swimming,
	InVehicleDriver,
	InVehiclePassenger,
	Aircraft,
};

struct ContextCapabilities {
	HudContext context = HudContext::Hidden;
	bool rpStripOnly = false; // Scoreboard / SelectTextDraw / ServerMenu / ObjectSelect: keep RP strip active, hide gameplay controls
	bool nearVehicle = false;
	bool canPassengerEnter = false;
	bool canPassengerDriveBy = false;
	bool isAiming = false;
	bool isScoped1stPerson = false;
	bool hasFirearm = false;
	bool hasMelee = false;
	bool isCrouched = false;
	bool isAircraftWithGear = false;
	bool isHydra = false;
	bool hydraJetForward = false;
	bool isHunter = false;
	bool isBike = false;
	bool isAirborne = false;
	bool isHydraulicsOrTurret = false;
	bool isInDriveBy = false;
};

struct ScreenRect {
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;

	bool contains(float px, float py, float pad = 0.0f) const
	{
		return px >= (x - pad) && px <= (x + w + pad) && py >= (y - pad) && py <= (y + h + pad);
	}
};

struct ScreenCircle {
	float cx = 0.0f;
	float cy = 0.0f;
	float r = 0.0f;

	bool contains(float px, float py, float pad = 0.0f) const
	{
		const float dx = px - cx;
		const float dy = py - cy;
		const float rr = r + pad;
		return (dx * dx + dy * dy) <= (rr * rr);
	}
};

// Items inside the single-row collapsible RP Action Strip.
enum class RpStripItem : uint8_t {
	ToggleCollapse = 0,
	KeyY,
	KeyN,
	KeyAlt,
	KeyH,
	KeyTab,
	Key2,
	NumDrawerToggle,
	Num8,
	Num4,
	Num6,
	Num2,
	KeyEsc,
	ScreenshotF8,
	OpenHudEditor,
	Count
};

struct RpStripLayoutItem {
	RpStripItem item = RpStripItem::ToggleCollapse;
	ScreenRect rect{};
	bool visible = false;
};

// Wrap angle difference into (-pi, pi]
inline float wrapAngleRad(float rad)
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kTwoPi = 2.0f * kPi;
	while (rad > kPi) rad -= kTwoPi;
	while (rad <= -kPi) rad += kTwoPi;
	return rad;
}

// Pure C++ multi-touch input engine for SAMP-Mobile Full Custom Touch HUD.
// Thread safety: caller synchronizes onTouch() (NVEvent thread) and stepFrame() (GameMain thread).
class TouchHudCore {
public:
	static constexpr int kMaxPointers = 10;
	static constexpr uint64_t kMinKeyPulseMs = 110;      // Guarantees >=1 SA-MP sync packet (40ms/100ms)
	static constexpr uint64_t kAltShortTapMaxMs = 220;   // Tap shorter than this toggles smart ALT latch

	struct VisualState {
		bool joystickActive = false;
		float joystickAnchorX = 0.0f;
		float joystickAnchorY = 0.0f;
		float joystickKnobX = 0.0f;
		float joystickKnobY = 0.0f;
		bool sprintLocked = false;
		bool altLatched = false;
		bool rpExpanded = true;
		bool numDrawerOpen = false;
		uint32_t pressedControlMask = 0; // Bits for ControlId (up to 32)
		uint32_t pressedRpItemMask = 0;
		uint8_t pressedCustomMask = 0;
	};

	struct FrameOutput {
		// Analog axes in GTA CPad scale [-128 .. +128]
		int16_t moveLR = 0;
		int16_t moveUD = 0;
		int16_t vehSteerLR = 0;
		int16_t vehSteerUD = 0;
		uint8_t vehAccel = 0; // 0..255
		uint8_t vehBrake = 0; // 0..255
		bool stickTouched = false;

		// Raw camera look delta (pixels * sensitivity) accumulated since last frame
		float lookDeltaX = 0.0f;
		float lookDeltaY = 0.0f;
		bool lookActive = false;

		// Effective SA-MP key bitmask (bits 0..17 matching SampKey / ePadKeys)
		uint32_t activeKeyMask = 0;

		// Direct physical hold flags (without pulse tail, for local GTA CPad feel)
		bool fireHeld = false;
		bool fireJustPressed = false;
		bool aimHeld = false;
		bool aimJustPressed = false;
		bool sprintHeld = false;
		bool jumpHeld = false;
		bool jumpJustPressed = false;
		bool crouchHeld = false;
		bool crouchJustPressed = false;
		bool handbrakeHeld = false;
		bool hornHeld = false;
		bool slowWalkHeld = false;
		bool lookLeftHeld = false;
		bool lookRightHeld = false;
		bool lookBehindHeld = false; // Q+E simultaneously

		// One-shot edge triggers consumed per frame by GameMain
		bool weaponPrevJustPressed = false;
		bool weaponNextJustPressed = false;
		bool interactFJustPressed = false;
		bool passengerGJustPressed = false;
		bool driveByToggleJustPressed = false;
		bool submission2JustPressed = false;
		bool hydraThrusterJustPressed = false;
		bool hydraFlareJustPressed = false;
		bool hydraFlareHeld = false;
		bool camZoomJustPressed = false;
		bool escJustPressed = false;
		bool scoreboardJustPressed = false;
		bool screenshotJustPressed = false;
		bool openEditorJustPressed = false;

		std::vector<std::string> pendingCommands;

		bool isKeyActive(SampKey k) const { return (activeKeyMask & keyBit(k)) != 0u; }
	};

	TouchHudCore()
	{
		m_layout = makePresetLayout(PresetId::RoleplayHeavy);
		m_rpExpanded = m_layout.prefs.rpStripExpanded;
	}

	void setScreenSize(float w, float h)
	{
		m_screenW = std::max(320.0f, w);
		m_screenH = std::max(240.0f, h);
	}

	float screenW() const { return m_screenW; }
	float screenH() const { return m_screenH; }
	float unit() const { return std::min(m_screenW, m_screenH); }

	void setLayout(const TouchHudLayout& layout)
	{
		m_layout = layout;
		m_layout.clampAll();
		m_rpExpanded = m_layout.prefs.rpStripExpanded;
	}

	const TouchHudLayout& layout() const { return m_layout; }
	TouchHudLayout& mutableLayout() { return m_layout; }

	void setCapabilities(const ContextCapabilities& caps, uint64_t nowMs)
	{
		if (caps.context != m_caps.context) {
			// Context switch (e.g. OnFoot <-> InVehicle or Hidden): release pointers bound
			// to controls from the previous page so nothing ever sticks!
			releaseIncompatiblePointers(caps.context, nowMs);
			if (caps.context == HudContext::Hidden) {
				m_sprintLocked = false;
				m_altLatched = false;
			}
		}
		if (caps.rpStripOnly && !m_caps.rpStripOnly) {
			// Entering modal overlay (Scoreboard / SelectTextDraw / ServerMenu / ObjectSelect):
			// cleanly release all gameplay pointers while preserving any active RP strip touch.
			for (auto& slot : m_pointers) {
				if (slot.active && slot.kind != BindKind::RpItem) {
					releasePointer(slot, nowMs);
				}
			}
			m_sprintLocked = false;
		}
		m_caps = caps;
	}

	const ContextCapabilities& capabilities() const { return m_caps; }

	// Returns base circular hit geometry for any standard control
	ScreenCircle controlCircle(ControlId id) const
	{
		const auto& c = m_layout.ctrl(id);
		const float u = unit();
		float baseFrac = 0.056f;
		switch (id) {
			case ControlId::Joystick:
			case ControlId::VehJoystick:    baseFrac = 0.125f; break;
			case ControlId::FireRight:
			case ControlId::VehAccel:       baseFrac = 0.068f; break;
			case ControlId::FireLeft:
			case ControlId::Aim:
			case ControlId::VehBrake:
			case ControlId::VehSteerLeft:
			case ControlId::VehSteerRight:  baseFrac = 0.060f; break;
			case ControlId::WeaponPrevQ:
			case ControlId::WeaponNextE:
			case ControlId::VehLookLeftQ:
			case ControlId::VehLookRightE:
			case ControlId::CamZoom:
			case ControlId::VehCamToggle:
			case ControlId::VehSubMission2: baseFrac = 0.044f; break;
			default:                        baseFrac = 0.054f; break;
		}
		return {c.cx * m_screenW, c.cy * m_screenH, u * baseFrac * c.scale};
	}

	ScreenCircle customButtonCircle(size_t idx) const
	{
		const auto& b = m_layout.customButtons[idx];
		return {b.cx * m_screenW, b.cy * m_screenH, unit() * 0.048f * b.scale};
	}

	// Returns whether a standard control should be visible & hittable in current context.
	bool isControlActive(ControlId id) const
	{
		if (m_caps.context == HudContext::Hidden) return false;
		const auto& c = m_layout.ctrl(id);
		if (!c.visible) return false;

		if (id == ControlId::RpStrip) {
			return true; // Available on foot, in vehicle, and during Scoreboard/TextDraw/Menu overlays (for TAB/ESC)
		}
		if (m_caps.rpStripOnly) {
			return false; // Keep screen unobstructed while Scoreboard/TextDraw/Menu is open
		}

		const bool inVeh = (m_caps.context == HudContext::InVehicleDriver
			|| m_caps.context == HudContext::InVehiclePassenger
			|| m_caps.context == HudContext::Aircraft);

		if (!inVeh) {
			if (isVehicleControl(id)) return false;
			switch (id) {
				case ControlId::FireLeft:
					if (m_layout.prefs.leftFireMode == LeftFireMode::Hidden) return false;
					if (m_layout.prefs.leftFireMode == LeftFireMode::AimOnly && !m_caps.isAiming) return false;
					return true;
				case ControlId::Aim:
					return m_caps.hasFirearm || m_caps.hasMelee;
				case ControlId::PassengerG:
					return m_layout.prefs.showPassengerAlways || m_caps.canPassengerEnter;
				default:
					return true;
			}
		}
		else {
			if (!isVehicleControl(id)) return false;
			const bool isPassenger = (m_caps.context == HudContext::InVehiclePassenger);
			const bool isAircraft = (m_caps.context == HudContext::Aircraft);
			const bool useJoy = (m_layout.prefs.vehicleSteering == VehicleSteeringMode::Joystick);

			switch (id) {
				case ControlId::VehSteerLeft:
				case ControlId::VehSteerRight:
					return !isPassenger && !useJoy;
				case ControlId::VehJoystick:
					return !isPassenger && useJoy;
				case ControlId::VehAccel:
				case ControlId::VehBrake:
					return !isPassenger;
				case ControlId::VehHorn:
					// Planes/helis have no horn; Hydra (520) reuses this slot as the dedicated JET/HOVER thruster toggle!
					return !isPassenger && (!isAircraft || m_caps.isHydra);
				case ControlId::VehHandbrake:
					return !isPassenger;
				case ControlId::VehDriveBy:
					return isPassenger && m_caps.canPassengerDriveBy;
				case ControlId::VehFire:
					return !isPassenger || m_caps.isInDriveBy;
				case ControlId::VehPitchUp:
				case ControlId::VehPitchDown: {
					if (isPassenger || useJoy) return false;
					if (m_layout.prefs.vehiclePitchMode == VehiclePitchMode::Always) return true;
					if (m_layout.prefs.vehiclePitchMode == VehiclePitchMode::AircraftOnly) {
						return isAircraft || m_caps.isHydraulicsOrTurret;
					}
					// Smart mode (default): Aircraft + Bikes/BMX/Quad (wheelie/lean) + Airborne stunt cars + Hydraulics/Turret
					return isAircraft || m_caps.isBike || m_caps.isAirborne || m_caps.isHydraulicsOrTurret;
				}
				case ControlId::VehFlare:
					return !isPassenger && m_caps.isHydra;
				case ControlId::VehSubMission2:
					return !isPassenger;
				case ControlId::VehLookLeftQ:
				case ControlId::VehLookRightE:
					return !isPassenger;
				default:
					return true;
			}
		}
	}

	bool isCustomButtonActive(size_t idx) const
	{
		if (idx >= kMaxCustomButtons || m_caps.context == HudContext::Hidden || m_caps.rpStripOnly) return false;
		const auto& b = m_layout.customButtons[idx];
		if (!b.enabled) return false;
		const bool inVeh = (m_caps.context == HudContext::InVehicleDriver
			|| m_caps.context == HudContext::InVehiclePassenger
			|| m_caps.context == HudContext::Aircraft);
		return inVeh ? ((b.contextMask & kCtxInVehicle) != 0) : ((b.contextMask & kCtxOnFoot) != 0);
	}

	// Computes the live single-row RP Action Strip rects (ordered by real-world frequency).
	void computeRpStripItems(std::array<RpStripLayoutItem, static_cast<size_t>(RpStripItem::Count)>& items,
		ScreenRect* outBounds = nullptr) const
	{
		for (size_t i = 0; i < items.size(); ++i) {
			items[i] = {static_cast<RpStripItem>(i), {}, false};
		}
		if (!isControlActive(ControlId::RpStrip)) {
			if (outBounds) *outBounds = {};
			return;
		}

		const auto& strip = m_layout.ctrl(ControlId::RpStrip);
		const float u = unit();
		const float btnW = u * 0.056f * strip.scale;
		const float btnH = u * 0.050f * strip.scale;
		const float gap = u * 0.009f * strip.scale;

		// Anchor represents the left edge center of the strip
		const float startX = strip.cx * m_screenW;
		const float topY = strip.cy * m_screenH - btnH * 0.5f;

		float curX = startX;
		auto place = [&](RpStripItem it, float widthMul = 1.0f) {
			const float w = btnW * widthMul;
			items[static_cast<size_t>(it)] = {it, {curX, topY, w, btnH}, true};
			curX += w + gap;
		};

		// Collapse/Expand button always shown first
		place(RpStripItem::ToggleCollapse, 0.78f);

		if (m_rpExpanded) {
			// Ordered by real RP frequency (audited on medieval-roleplay-main):
			// Y, N, ALT, H, TAB, 2, Num(->8/4/6/2), ESC, F8, Edit
			place(RpStripItem::KeyY);
			place(RpStripItem::KeyN);
			place(RpStripItem::KeyAlt, 1.15f);
			place(RpStripItem::KeyH);
			place(RpStripItem::KeyTab, 1.10f);
			place(RpStripItem::Key2, 0.85f);
			place(RpStripItem::NumDrawerToggle, 1.10f);
			place(RpStripItem::KeyEsc, 1.05f);
			place(RpStripItem::ScreenshotF8, 0.92f);
			place(RpStripItem::OpenHudEditor, 0.88f);

			// Sub-drawer pops right above the Num button as an ergonomic inverted-T D-pad (8 on top, 4/2/6 below)
			if (m_numDrawerOpen) {
				const auto& numRect = items[static_cast<size_t>(RpStripItem::NumDrawerToggle)].rect;
				const float subW = btnW * 1.12f;
				const float subH = btnH * 1.12f;
				const float subGap = gap * 1.1f;
				const float centerX = numRect.x + numRect.w * 0.5f;
				const float rowBotY = topY - subH - subGap * 1.4f;
				const float rowTopY = rowBotY - subH - subGap;
				items[static_cast<size_t>(RpStripItem::Num8)] = {RpStripItem::Num8, {centerX - subW * 0.5f,                  rowTopY, subW, subH}, true};
				items[static_cast<size_t>(RpStripItem::Num4)] = {RpStripItem::Num4, {centerX - subW * 1.5f - subGap,         rowBotY, subW, subH}, true};
				items[static_cast<size_t>(RpStripItem::Num2)] = {RpStripItem::Num2, {centerX - subW * 0.5f,                  rowBotY, subW, subH}, true};
				items[static_cast<size_t>(RpStripItem::Num6)] = {RpStripItem::Num6, {centerX + subW * 0.5f + subGap,         rowBotY, subW, subH}, true};
			}
		}

		if (outBounds) {
			float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
			bool any = false;
			for (const auto& it : items) {
				if (!it.visible) continue;
				any = true;
				minX = std::min(minX, it.rect.x);
				minY = std::min(minY, it.rect.y);
				maxX = std::max(maxX, it.rect.x + it.rect.w);
				maxY = std::max(maxY, it.rect.y + it.rect.h);
			}
			if (any) {
				*outBounds = {minX, minY, maxX - minX, maxY - minY};
			}
			else {
				*outBounds = {};
			}
		}
	}

	// Process a single multi-touch pointer event. Returns true if consumed by TouchHudCore.
	bool onTouch(TouchAction action, int pointerId, float x, float y, uint64_t nowMs)
	{
		if (m_caps.context == HudContext::Hidden) {
			return false;
		}

		if (action == TouchAction::Down) {
			// Guard duplicate Down for same pointerId
			if (PointerSlot* existing = findPointer(pointerId)) {
				releasePointer(*existing, nowMs);
			}
			PointerSlot* slot = allocPointer(pointerId);
			if (!slot) return true;

			slot->active = true;
			slot->pointerId = pointerId;
			slot->startX = slot->lastX = slot->curX = x;
			slot->startY = slot->lastY = slot->curY = y;
			slot->downMs = nowMs;

			// Priority 1: RP Strip buttons
			if (auto rpItem = hitTestRpStrip(x, y)) {
				slot->kind = BindKind::RpItem;
				slot->rpItem = *rpItem;
				onRpItemDown(*rpItem, nowMs);
				return true;
			}

			// Priority 2: Custom shortcut buttons
			for (size_t i = 0; i < kMaxCustomButtons; ++i) {
				if (!isCustomButtonActive(i)) continue;
				if (customButtonCircle(i).contains(x, y, unit() * 0.012f)) {
					slot->kind = BindKind::CustomButton;
					slot->customIndex = static_cast<uint8_t>(i);
					onCustomButtonDown(i, nowMs);
					return true;
				}
			}

			// Priority 3: Standard tactile buttons (check small/action buttons before Joystick!)
			if (auto ctrlId = hitTestStandardButtons(x, y)) {
				slot->kind = BindKind::Control;
				slot->controlId = *ctrlId;
				onControlDown(*ctrlId, x, y, nowMs);
				return true;
			}

			// Priority 4: Movement / Vehicle Joystick (fixed circle OR floating left zone)
			const ControlId joyId = activeJoystickControl();
			if (joyId != ControlId::Count && !hasActiveJoystickPointer()) {
				const ScreenCircle jc = controlCircle(joyId);
				const bool hitFixed = jc.contains(x, y, unit() * 0.025f);
				const bool hitFloatingZone = (m_layout.prefs.joystickMode == JoystickMode::Floating)
					&& (x < m_screenW * 0.44f) && (y > m_screenH * 0.28f);
				if (hitFixed || hitFloatingZone) {
					slot->kind = BindKind::Control;
					slot->controlId = joyId;
					if (m_layout.prefs.joystickMode == JoystickMode::Floating && !hitFixed) {
						m_joyAnchorX = x;
						m_joyAnchorY = y;
					}
					else {
						m_joyAnchorX = jc.cx;
						m_joyAnchorY = jc.cy;
					}
					m_joyKnobX = x;
					m_joyKnobY = y;
					m_joyActive = true;
					updateJoystickVector(joyId, x, y);
					return true;
				}
			}

			// Priority 5: Free camera look drag anywhere else on screen (disabled during rpStripOnly overlays!)
			if (m_caps.rpStripOnly) {
				slot->active = false;
				return false;
			}
			slot->kind = BindKind::FreeCameraLook;
			return true;
		}

		PointerSlot* slot = findPointer(pointerId);
		if (!slot) {
			return false;
		}

		const float dx = x - slot->lastX;
		const float dy = y - slot->lastY;
		slot->lastX = slot->curX;
		slot->lastY = slot->curY;
		slot->curX = x;
		slot->curY = y;

		if (action == TouchAction::Move) {
			if (slot->kind == BindKind::FreeCameraLook) {
				m_accumLookDX += dx * m_layout.prefs.lookSensitivity;
				m_accumLookDY += dy * m_layout.prefs.lookSensitivity;
				m_lookActive = true;
			}
			else if (slot->kind == BindKind::Control) {
				if (slot->controlId == ControlId::Joystick || slot->controlId == ControlId::VehJoystick) {
					updateJoystickVector(slot->controlId, x, y);
				}
				else if (slot->controlId == ControlId::FireRight && m_layout.prefs.fireRightDragAim) {
					// PUBG drag-to-aim while holding right fire button!
					m_accumLookDX += dx * m_layout.prefs.lookSensitivity;
					m_accumLookDY += dy * m_layout.prefs.lookSensitivity;
					m_lookActive = true;
				}
				else if (slot->controlId == ControlId::VehFire && m_caps.isInDriveBy && m_layout.prefs.fireRightDragAim) {
					m_accumLookDX += dx * m_layout.prefs.lookSensitivity;
					m_accumLookDY += dy * m_layout.prefs.lookSensitivity;
					m_lookActive = true;
				}
			}
			return true;
		}

		if (action == TouchAction::Up) {
			releasePointer(*slot, nowMs);
			return true;
		}

		return false;
	}

	void onCancelAll(uint64_t nowMs)
	{
		for (auto& slot : m_pointers) {
			if (slot.active) {
				releasePointer(slot, nowMs);
			}
		}
		m_joyActive = false;
		m_joyLR = 0;
		m_joyUD = 0;
		m_sprintLocked = false;
		m_accumLookDX = 0.0f;
		m_accumLookDY = 0.0f;
		m_lookActive = false;
	}

	// Called once per GameMain logic frame to produce consistent pad + sync output.
	FrameOutput stepFrame(uint64_t nowMs)
	{
		FrameOutput out{};

		// Drain camera look deltas
		out.lookDeltaX = m_accumLookDX;
		out.lookDeltaY = m_accumLookDY;
		out.lookActive = m_lookActive || (std::fabs(m_accumLookDX) > 0.001f) || (std::fabs(m_accumLookDY) > 0.001f);
		m_accumLookDX = 0.0f;
		m_accumLookDY = 0.0f;
		m_lookActive = hasAnyPointerOfKind(BindKind::FreeCameraLook);

		// Compute live physically-held SA-MP key mask from all active pointers
		uint32_t heldMask = 0;
		const bool inVeh = (m_caps.context == HudContext::InVehicleDriver
			|| m_caps.context == HudContext::InVehiclePassenger
			|| m_caps.context == HudContext::Aircraft);

		for (const auto& p : m_pointers) {
			if (!p.active) continue;
			if (p.kind == BindKind::Control) {
				switch (p.controlId) {
					case ControlId::FireRight:
					case ControlId::FireLeft:
						heldMask |= keyBit(SampKey::Fire);
						out.fireHeld = true;
						break;
					case ControlId::Aim:
						heldMask |= keyBit(SampKey::Handbrake);
						out.aimHeld = true;
						break;
					case ControlId::Sprint:
						heldMask |= keyBit(SampKey::Sprint);
						out.sprintHeld = true;
						break;
					case ControlId::Jump:
						heldMask |= keyBit(SampKey::Jump);
						out.jumpHeld = true;
						break;
					case ControlId::Crouch:
						heldMask |= keyBit(SampKey::Crouch);
						out.crouchHeld = true;
						break;
					case ControlId::InteractF:
					case ControlId::VehExitF:
						heldMask |= keyBit(SampKey::SecondaryAttack);
						break;
					case ControlId::WeaponPrevQ:
					case ControlId::VehLookLeftQ:
						heldMask |= keyBit(SampKey::LookLeft);
						out.lookLeftHeld = true;
						break;
					case ControlId::WeaponNextE:
					case ControlId::VehLookRightE:
						heldMask |= keyBit(SampKey::LookRight);
						out.lookRightHeld = true;
						break;
					case ControlId::VehAccel:
						heldMask |= keyBit(SampKey::Sprint);
						out.vehAccel = 255;
						break;
					case ControlId::VehBrake:
						heldMask |= keyBit(SampKey::Jump);
						out.vehBrake = 255;
						break;
					case ControlId::VehHandbrake:
						heldMask |= keyBit(SampKey::Handbrake);
						out.handbrakeHeld = true;
						break;
					case ControlId::VehFire:
						heldMask |= keyBit(SampKey::Fire);
						out.fireHeld = true;
						break;
					case ControlId::VehHorn:
						if (!m_caps.isHydra) {
							heldMask |= keyBit(SampKey::Crouch);
							out.hornHeld = true;
						}
						break;
					case ControlId::VehSubMission2:
						heldMask |= keyBit(SampKey::Submission);
						break;
					case ControlId::VehSteerLeft:
						out.vehSteerLR = -128;
						break;
					case ControlId::VehSteerRight:
						out.vehSteerLR = 128;
						break;
					case ControlId::VehPitchUp:
						if (m_caps.isHydraulicsOrTurret && m_caps.context != HudContext::Aircraft) {
							heldMask |= keyBit(SampKey::AnalogUp);
						}
						else {
							out.vehSteerUD = -128; // Aircraft nose pitch / Bike lean forward / Mid-air stunt pitch
						}
						break;
					case ControlId::VehPitchDown:
						if (m_caps.isHydraulicsOrTurret && m_caps.context != HudContext::Aircraft) {
							heldMask |= keyBit(SampKey::AnalogDown);
						}
						else {
							out.vehSteerUD = 128; // Aircraft nose pitch / Bike wheelie / Mid-air stunt pitch
						}
						break;
					case ControlId::VehFlare:
						out.hydraFlareHeld = true;
						break;
					default:
						break;
				}
			}
			else if (p.kind == BindKind::RpItem) {
				switch (p.rpItem) {
					case RpStripItem::KeyY:   heldMask |= keyBit(SampKey::Yes); break;
					case RpStripItem::KeyN:   heldMask |= keyBit(SampKey::No); break;
					case RpStripItem::KeyAlt:
						heldMask |= keyBit(SampKey::Walk);
						out.slowWalkHeld = true;
						break;
					case RpStripItem::KeyH:   heldMask |= keyBit(SampKey::CtrlBack); break;
					case RpStripItem::KeyTab: heldMask |= keyBit(SampKey::Action); break;
					case RpStripItem::Key2:   heldMask |= keyBit(SampKey::Submission); break;
					case RpStripItem::Num8:   heldMask |= keyBit(SampKey::AnalogUp); break;
					case RpStripItem::Num4:   heldMask |= keyBit(SampKey::AnalogLeft); break;
					case RpStripItem::Num6:   heldMask |= keyBit(SampKey::AnalogRight); break;
					case RpStripItem::Num2:   heldMask |= keyBit(SampKey::AnalogDown); break;
					default: break;
				}
			}
			else if (p.kind == BindKind::CustomButton && p.customIndex < kMaxCustomButtons) {
				const auto& b = m_layout.customButtons[p.customIndex];
				if (b.actionType == CustomActionType::KeyCombo) {
					heldMask |= b.keyMask;
					if (b.keyMask & keyBit(SampKey::Walk)) out.slowWalkHeld = true;
					if (b.keyMask & keyBit(SampKey::Fire)) out.fireHeld = true;
					if (b.keyMask & keyBit(SampKey::Handbrake)) { out.aimHeld = true; out.handbrakeHeld = true; }
					if (b.keyMask & keyBit(SampKey::LookLeft)) out.lookLeftHeld = true;
					if (b.keyMask & keyBit(SampKey::LookRight)) out.lookRightHeld = true;
				}
			}
		}

		// Smart ALT latch keeps KEY_WALK active
		if (m_altLatched) {
			heldMask |= keyBit(SampKey::Walk);
			out.slowWalkHeld = true;
		}

		// PUBG auto-sprint lock keeps forward movement + KEY_SPRINT active on foot
		if (!inVeh && m_sprintLocked) {
			heldMask |= keyBit(SampKey::Sprint);
			out.sprintHeld = true;
		}

		// Merge min-pulse latches so fast taps always reach >= 1 SA-MP sync packet!
		uint32_t pulseMask = 0;
		for (size_t i = 0; i < static_cast<size_t>(SampKey::Count); ++i) {
			if (nowMs < m_keyPulseUntilMs[i]) {
				pulseMask |= (1u << i);
			}
		}

		out.activeKeyMask = heldMask | pulseMask;

		// Look behind = holding BOTH Q (LookLeft) and E (LookRight) simultaneously (SA-MP bitmask 320)
		if ((out.activeKeyMask & (keyBit(SampKey::LookLeft) | keyBit(SampKey::LookRight)))
				== (keyBit(SampKey::LookLeft) | keyBit(SampKey::LookRight))) {
			out.lookBehindHeld = true;
		}

		// Joystick analog output
		if (!inVeh) {
			if (m_joyActive) {
				out.stickTouched = true;
				out.moveLR = m_joyLR;
				out.moveUD = m_joyUD;
			}
			else if (m_sprintLocked) {
				out.stickTouched = true;
				out.moveLR = 0;
				out.moveUD = -128; // Auto-run forward!
			}
		}
		else {
			if (m_joyActive && m_layout.prefs.vehicleSteering == VehicleSteeringMode::Joystick) {
				out.stickTouched = true;
				out.vehSteerLR = m_joyLR;
				out.vehSteerUD = m_joyUD;
			}
		}

		// Drain edge events
		out.fireJustPressed          = consumeFlag(m_edgeFire);
		out.aimJustPressed           = consumeFlag(m_edgeAim);
		out.jumpJustPressed          = consumeFlag(m_edgeJump);
		out.crouchJustPressed        = consumeFlag(m_edgeCrouch);
		out.weaponPrevJustPressed    = consumeFlag(m_edgeWeaponPrev);
		out.weaponNextJustPressed    = consumeFlag(m_edgeWeaponNext);
		out.interactFJustPressed     = consumeFlag(m_edgeInteractF);
		out.passengerGJustPressed    = consumeFlag(m_edgePassengerG);
		out.driveByToggleJustPressed = consumeFlag(m_edgeDriveByToggle);
		out.submission2JustPressed   = consumeFlag(m_edgeSubmission2);
		out.hydraThrusterJustPressed = consumeFlag(m_edgeHydraThruster);
		out.hydraFlareJustPressed    = consumeFlag(m_edgeHydraFlare);
		out.camZoomJustPressed       = consumeFlag(m_edgeCamZoom);
		out.escJustPressed           = consumeFlag(m_edgeEsc);
		out.scoreboardJustPressed    = consumeFlag(m_edgeScoreboard);
		out.screenshotJustPressed    = consumeFlag(m_edgeScreenshot);
		out.openEditorJustPressed    = consumeFlag(m_edgeOpenEditor);
		out.pendingCommands.swap(m_pendingCommands);

		return out;
	}

	VisualState visualState() const
	{
		VisualState v{};
		v.joystickActive = m_joyActive;
		v.joystickAnchorX = m_joyAnchorX;
		v.joystickAnchorY = m_joyAnchorY;
		v.joystickKnobX = m_joyKnobX;
		v.joystickKnobY = m_joyKnobY;
		v.sprintLocked = m_sprintLocked;
		v.altLatched = m_altLatched;
		v.rpExpanded = m_rpExpanded;
		v.numDrawerOpen = m_numDrawerOpen;
		for (const auto& p : m_pointers) {
			if (!p.active) continue;
			if (p.kind == BindKind::Control && static_cast<uint8_t>(p.controlId) < 32) {
				v.pressedControlMask |= (1u << static_cast<uint8_t>(p.controlId));
			}
			else if (p.kind == BindKind::RpItem && static_cast<uint8_t>(p.rpItem) < 32) {
				v.pressedRpItemMask |= (1u << static_cast<uint8_t>(p.rpItem));
			}
			else if (p.kind == BindKind::CustomButton && p.customIndex < kMaxCustomButtons) {
				v.pressedCustomMask |= static_cast<uint8_t>(1u << p.customIndex);
			}
		}
		return v;
	}

	bool isRpExpanded() const { return m_rpExpanded; }
	void setRpExpanded(bool v) { m_rpExpanded = v; if (!v) m_numDrawerOpen = false; }
	bool isNumDrawerOpen() const { return m_numDrawerOpen; }
	void setNumDrawerOpen(bool v) { m_numDrawerOpen = v && m_rpExpanded; }

private:
	enum class BindKind : uint8_t {
		None = 0,
		Control,
		RpItem,
		CustomButton,
		FreeCameraLook,
	};

	struct PointerSlot {
		bool active = false;
		int pointerId = -1;
		BindKind kind = BindKind::None;
		ControlId controlId = ControlId::Count;
		RpStripItem rpItem = RpStripItem::Count;
		uint8_t customIndex = 0;
		float startX = 0.0f;
		float startY = 0.0f;
		float lastX = 0.0f;
		float lastY = 0.0f;
		float curX = 0.0f;
		float curY = 0.0f;
		uint64_t downMs = 0;
	};

	static bool consumeFlag(bool& f)
	{
		const bool v = f;
		f = false;
		return v;
	}

	PointerSlot* findPointer(int pointerId)
	{
		for (auto& s : m_pointers) {
			if (s.active && s.pointerId == pointerId) return &s;
		}
		return nullptr;
	}

	PointerSlot* allocPointer(int pointerId)
	{
		for (auto& s : m_pointers) {
			if (!s.active) {
				s = PointerSlot{};
				s.pointerId = pointerId;
				return &s;
			}
		}
		return nullptr;
	}

	bool hasAnyPointerOfKind(BindKind k) const
	{
		for (const auto& s : m_pointers) {
			if (s.active && s.kind == k) return true;
		}
		return false;
	}

	bool hasActiveJoystickPointer() const
	{
		for (const auto& s : m_pointers) {
			if (s.active && s.kind == BindKind::Control
					&& (s.controlId == ControlId::Joystick || s.controlId == ControlId::VehJoystick)) {
				return true;
			}
		}
		return false;
	}

	ControlId activeJoystickControl() const
	{
		if (isControlActive(ControlId::Joystick)) return ControlId::Joystick;
		if (isControlActive(ControlId::VehJoystick)) return ControlId::VehJoystick;
		return ControlId::Count;
	}

	void latchPulse(SampKey k, uint64_t nowMs, uint64_t durMs = kMinKeyPulseMs)
	{
		const size_t idx = static_cast<size_t>(k);
		m_keyPulseUntilMs[idx] = std::max(m_keyPulseUntilMs[idx], nowMs + durMs);
	}

	void latchPulseMask(uint32_t mask, uint64_t nowMs, uint64_t durMs = kMinKeyPulseMs)
	{
		for (size_t i = 0; i < static_cast<size_t>(SampKey::Count); ++i) {
			if (mask & (1u << i)) {
				m_keyPulseUntilMs[i] = std::max(m_keyPulseUntilMs[i], nowMs + durMs);
			}
		}
	}

	// Called when any combo partner key (Y, N, H, Jump, F, Fire, etc.) is pressed:
	// - If ALT is physically held, marks altComboUsed so releasing ALT won't toggle latch.
	// - If ALT is smart-latched, guarantees KEY_WALK pulses alongside the combo key, then unlatches!
	void notifyComboPartnerPressed(uint64_t nowMs)
	{
		for (const auto& p : m_pointers) {
			if (p.active && p.kind == BindKind::RpItem && p.rpItem == RpStripItem::KeyAlt) {
				m_altComboUsed = true;
			}
		}
		if (m_altLatched) {
			latchPulse(SampKey::Walk, nowMs, kMinKeyPulseMs + 20);
			m_altLatched = false;
		}
	}

	struct OptionalRpItem {
		bool valid = false;
		RpStripItem value = RpStripItem::Count;
		explicit operator bool() const { return valid; }
		RpStripItem operator*() const { return value; }
	};

	OptionalRpItem hitTestRpStrip(float x, float y) const
	{
		std::array<RpStripLayoutItem, static_cast<size_t>(RpStripItem::Count)> items{};
		computeRpStripItems(items);
		const float pad = unit() * 0.005f;
		for (const auto& it : items) {
			if (it.visible && it.rect.contains(x, y, pad)) {
				return {true, it.item};
			}
		}
		return {};
	}

	struct OptionalControl {
		bool valid = false;
		ControlId value = ControlId::Count;
		explicit operator bool() const { return valid; }
		ControlId operator*() const { return value; }
	};

	OptionalControl hitTestStandardButtons(float x, float y) const
	{
		const float pad = unit() * 0.012f;
		ControlId bestId = ControlId::Count;
		float bestDistSq = 1e18f;

		for (size_t i = 0; i < kControlCount; ++i) {
			const auto id = static_cast<ControlId>(i);
			if (id == ControlId::Joystick || id == ControlId::VehJoystick || id == ControlId::RpStrip) {
				continue;
			}
			if (!isControlActive(id)) continue;
			const ScreenCircle c = controlCircle(id);
			if (c.contains(x, y, pad)) {
				const float dx = x - c.cx;
				const float dy = y - c.cy;
				const float d2 = dx * dx + dy * dy;
				if (d2 < bestDistSq) {
					bestDistSq = d2;
					bestId = id;
				}
			}
		}
		if (bestId != ControlId::Count) {
			return {true, bestId};
		}
		return {};
	}

	void onControlDown(ControlId id, float x, float y, uint64_t nowMs)
	{
		(void)x; (void)y;
		switch (id) {
			case ControlId::FireRight:
			case ControlId::FireLeft:
				m_edgeFire = true;
				latchPulse(SampKey::Fire, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case ControlId::Aim:
				m_sprintLocked = false;
				m_edgeAim = true;
				latchPulse(SampKey::Handbrake, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case ControlId::Sprint:
				m_sprintLocked = false;
				latchPulse(SampKey::Sprint, nowMs);
				break;
			case ControlId::Jump:
				m_edgeJump = true;
				latchPulse(SampKey::Jump, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case ControlId::Crouch:
				m_sprintLocked = false;
				m_edgeCrouch = true;
				latchPulse(SampKey::Crouch, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case ControlId::InteractF:
				m_sprintLocked = false;
				m_edgeInteractF = true;
				latchPulse(SampKey::SecondaryAttack, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case ControlId::PassengerG:
				m_edgePassengerG = true;
				break;
			case ControlId::WeaponPrevQ:
				m_edgeWeaponPrev = true;
				latchPulse(SampKey::LookLeft, nowMs);
				break;
			case ControlId::WeaponNextE:
				m_edgeWeaponNext = true;
				latchPulse(SampKey::LookRight, nowMs);
				break;
			case ControlId::CamZoom:
			case ControlId::VehCamToggle:
				m_edgeCamZoom = true;
				break;
			case ControlId::VehAccel:
				latchPulse(SampKey::Sprint, nowMs);
				break;
			case ControlId::VehBrake:
				latchPulse(SampKey::Jump, nowMs);
				break;
			case ControlId::VehHandbrake:
				latchPulse(SampKey::Handbrake, nowMs);
				break;
			case ControlId::VehFire:
				m_edgeFire = true;
				latchPulse(SampKey::Fire, nowMs);
				break;
			case ControlId::VehHorn:
				if (m_caps.isHydra) {
					m_edgeHydraThruster = true;
				}
				else {
					latchPulse(SampKey::Crouch, nowMs);
				}
				break;
			case ControlId::VehLookLeftQ:
				latchPulse(SampKey::LookLeft, nowMs);
				break;
			case ControlId::VehLookRightE:
				latchPulse(SampKey::LookRight, nowMs);
				break;
			case ControlId::VehExitF:
				m_edgeInteractF = true;
				latchPulse(SampKey::SecondaryAttack, nowMs);
				break;
			case ControlId::VehSubMission2:
				m_edgeSubmission2 = true;
				latchPulse(SampKey::Submission, nowMs);
				break;
			case ControlId::VehDriveBy:
				m_edgeDriveByToggle = true;
				break;
			case ControlId::VehPitchUp:
				if (m_caps.isHydraulicsOrTurret && m_caps.context != HudContext::Aircraft) {
					latchPulse(SampKey::AnalogUp, nowMs);
				}
				break;
			case ControlId::VehPitchDown:
				if (m_caps.isHydraulicsOrTurret && m_caps.context != HudContext::Aircraft) {
					latchPulse(SampKey::AnalogDown, nowMs);
				}
				break;
			case ControlId::VehFlare:
				m_edgeHydraFlare = true;
				break;
			default:
				break;
		}
	}

	void onRpItemDown(RpStripItem it, uint64_t nowMs)
	{
		switch (it) {
			case RpStripItem::ToggleCollapse:
				m_rpExpanded = !m_rpExpanded;
				if (!m_rpExpanded) m_numDrawerOpen = false;
				break;
			case RpStripItem::KeyY:
				latchPulse(SampKey::Yes, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case RpStripItem::KeyN:
				latchPulse(SampKey::No, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case RpStripItem::KeyAlt:
				m_altDownMs = nowMs;
				m_altComboUsed = false;
				latchPulse(SampKey::Walk, nowMs);
				break;
			case RpStripItem::KeyH:
				latchPulse(SampKey::CtrlBack, nowMs);
				notifyComboPartnerPressed(nowMs);
				break;
			case RpStripItem::KeyTab:
				latchPulse(SampKey::Action, nowMs);
				m_tabDownMs = nowMs;
				break;
			case RpStripItem::Key2:
				m_edgeSubmission2 = true;
				latchPulse(SampKey::Submission, nowMs);
				break;
			case RpStripItem::NumDrawerToggle:
				m_numDrawerOpen = !m_numDrawerOpen;
				break;
			case RpStripItem::Num8: latchPulse(SampKey::AnalogUp, nowMs); break;
			case RpStripItem::Num4: latchPulse(SampKey::AnalogLeft, nowMs); break;
			case RpStripItem::Num6: latchPulse(SampKey::AnalogRight, nowMs); break;
			case RpStripItem::Num2: latchPulse(SampKey::AnalogDown, nowMs); break;
			case RpStripItem::KeyEsc:
				m_edgeEsc = true;
				break;
			case RpStripItem::ScreenshotF8:
				m_edgeScreenshot = true;
				break;
			case RpStripItem::OpenHudEditor:
				m_edgeOpenEditor = true;
				break;
			default:
				break;
		}
	}

	void onCustomButtonDown(size_t idx, uint64_t nowMs)
	{
		const auto& b = m_layout.customButtons[idx];
		if (b.actionType == CustomActionType::KeyCombo) {
			latchPulseMask(b.keyMask, nowMs, kMinKeyPulseMs + 20);
			if (b.keyMask & keyBit(SampKey::Fire)) m_edgeFire = true;
			if (b.keyMask & keyBit(SampKey::Jump)) m_edgeJump = true;
			if (b.keyMask & keyBit(SampKey::Crouch)) m_edgeCrouch = true;
			if (b.keyMask & keyBit(SampKey::SecondaryAttack)) m_edgeInteractF = true;
			if (b.keyMask & keyBit(SampKey::Submission)) m_edgeSubmission2 = true;
		}
		else if (b.actionType == CustomActionType::ChatCommand && b.command[0] != '\0') {
			if (m_pendingCommands.size() < 4) {
				m_pendingCommands.emplace_back(b.command);
			}
		}
	}

	void updateJoystickVector(ControlId joyId, float x, float y)
	{
		const float radius = std::max(24.0f, controlCircle(joyId).r);
		const float dx = x - m_joyAnchorX;
		const float dy = y - m_joyAnchorY;
		const float dist = std::hypot(dx, dy);

		// Clamp visual knob to joystick ring (or slightly above for sprint lock)
		const float visualClamp = (joyId == ControlId::Joystick && m_layout.prefs.autoSprintLock)
			? (radius * 1.45f)
			: radius;
		if (dist > visualClamp && dist > 1e-4f) {
			m_joyKnobX = m_joyAnchorX + (dx / dist) * visualClamp;
			m_joyKnobY = m_joyAnchorY + (dy / dist) * visualClamp;
		}
		else {
			m_joyKnobX = x;
			m_joyKnobY = y;
		}

		// Radial deadzone (8% of radius) + smooth rescaling to [-128 .. +128]
		const float deadzone = radius * 0.08f;
		if (dist <= deadzone) {
			m_joyLR = 0;
			m_joyUD = 0;
			m_sprintLocked = false;
			return;
		}

		const float normMag = std::min(1.0f, (dist - deadzone) / std::max(1.0f, radius - deadzone));
		const float ux = dx / dist;
		const float uy = dy / dist;
		m_joyLR = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(ux * normMag * 128.0f)), -128, 128));
		m_joyUD = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(uy * normMag * 128.0f)), -128, 128));

		// PUBG auto-sprint lock: push straight up beyond 1.25x joystick radius
		if (joyId == ControlId::Joystick && m_layout.prefs.autoSprintLock) {
			if (dy < -radius * 1.25f && std::fabs(dx) < radius * 0.55f) {
				m_sprintLocked = true;
			}
			else if (dy > -radius * 0.55f) {
				m_sprintLocked = false;
			}
		}
		else {
			m_sprintLocked = false;
		}
	}

	void releasePointer(PointerSlot& slot, uint64_t nowMs)
	{
		if (!slot.active) return;

		if (slot.kind == BindKind::Control) {
			if (slot.controlId == ControlId::Joystick || slot.controlId == ControlId::VehJoystick) {
				m_joyActive = false;
				m_joyLR = 0;
				m_joyUD = 0;
				// Note: if m_sprintLocked is true on foot, keep auto-running until user touches joystick again!
			}
		}
		else if (slot.kind == BindKind::RpItem) {
			if (slot.rpItem == RpStripItem::KeyAlt && m_layout.prefs.altSmartLatch) {
				const uint64_t heldMs = (nowMs >= m_altDownMs) ? (nowMs - m_altDownMs) : 0;
				if (!m_altComboUsed && heldMs < kAltShortTapMaxMs) {
					m_altLatched = !m_altLatched;
				}
			}
			else if (slot.rpItem == RpStripItem::KeyTab) {
				// Quick tap (<350ms) or any tap while rpStripOnly (Scoreboard open) toggles Scoreboard;
				// long holding TAB during gameplay acts as sustained KEY_ACTION without toggling scoreboard.
				const uint64_t heldMs = (nowMs >= m_tabDownMs) ? (nowMs - m_tabDownMs) : 0;
				if (m_caps.rpStripOnly || heldMs < 350) {
					m_edgeScoreboard = true;
				}
			}
		}

		slot = PointerSlot{};
	}

	void releaseIncompatiblePointers(HudContext nextCtx, uint64_t nowMs)
	{
		const bool nextInVeh = (nextCtx == HudContext::InVehicleDriver
			|| nextCtx == HudContext::InVehiclePassenger
			|| nextCtx == HudContext::Aircraft);

		for (auto& slot : m_pointers) {
			if (!slot.active) continue;
			if (nextCtx == HudContext::Hidden) {
				releasePointer(slot, nowMs);
				continue;
			}
			if (slot.kind == BindKind::Control && slot.controlId != ControlId::RpStrip) {
				if (isVehicleControl(slot.controlId) != nextInVeh) {
					releasePointer(slot, nowMs);
				}
			}
		}
	}

	TouchHudLayout m_layout{};
	ContextCapabilities m_caps{};
	float m_screenW = 1920.0f;
	float m_screenH = 1080.0f;

	std::array<PointerSlot, kMaxPointers> m_pointers{};
	std::array<uint64_t, static_cast<size_t>(SampKey::Count)> m_keyPulseUntilMs{};

	// Joystick live state
	bool m_joyActive = false;
	float m_joyAnchorX = 0.0f;
	float m_joyAnchorY = 0.0f;
	float m_joyKnobX = 0.0f;
	float m_joyKnobY = 0.0f;
	int16_t m_joyLR = 0;
	int16_t m_joyUD = 0;
	bool m_sprintLocked = false;

	// Camera look state
	float m_accumLookDX = 0.0f;
	float m_accumLookDY = 0.0f;
	bool m_lookActive = false;

	// RP strip & ALT smart-latch state
	bool m_rpExpanded = true;
	bool m_numDrawerOpen = false;
	bool m_altLatched = false;
	bool m_altComboUsed = false;
	uint64_t m_altDownMs = 0;
	uint64_t m_tabDownMs = 0;

	// One-shot edge triggers
	bool m_edgeFire = false;
	bool m_edgeAim = false;
	bool m_edgeJump = false;
	bool m_edgeCrouch = false;
	bool m_edgeWeaponPrev = false;
	bool m_edgeWeaponNext = false;
	bool m_edgeInteractF = false;
	bool m_edgePassengerG = false;
	bool m_edgeDriveByToggle = false;
	bool m_edgeSubmission2 = false;
	bool m_edgeHydraThruster = false;
	bool m_edgeHydraFlare = false;
	bool m_edgeCamZoom = false;
	bool m_edgeEsc = false;
	bool m_edgeScoreboard = false;
	bool m_edgeScreenshot = false;
	bool m_edgeOpenEditor = false;

	std::vector<std::string> m_pendingCommands;
};

} // namespace touch_hud
