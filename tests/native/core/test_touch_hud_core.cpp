#include "UI/TouchHud/TouchHudCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace touch_hud;

namespace {

int g_failures = 0;

#define EXPECT(cond) \
	do { \
		if (!(cond)) { \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures; \
		} \
	} while (0)

ContextCapabilities makeOnFootCaps()
{
	ContextCapabilities c{};
	c.context = HudContext::OnFoot;
	c.hasFirearm = true;
	c.nearVehicle = true;
	c.canPassengerEnter = true;
	return c;
}

ContextCapabilities makeDriverCaps()
{
	ContextCapabilities c{};
	c.context = HudContext::InVehicleDriver;
	return c;
}

void testMultiFingerConcurrencyAndDragToAim()
{
	TouchHudCore hud;
	hud.setScreenSize(1920.0f, 1080.0f);
	auto L = makePresetLayout(PresetId::PubgCompetitive);
	hud.setLayout(L);
	hud.setCapabilities(makeOnFootCaps(), 1000);

	// Finger 0: Floating joystick on left half, drag forward-right
	hud.onTouch(TouchAction::Down, 0, 260.0f, 780.0f, 1000);
	hud.onTouch(TouchAction::Move, 0, 340.0f, 700.0f, 1010);

	// Finger 1: Hold Right Fire AND drag right to aim while shooting!
	const auto fireC = hud.controlCircle(ControlId::FireRight);
	hud.onTouch(TouchAction::Down, 1, fireC.cx, fireC.cy, 1010);
	hud.onTouch(TouchAction::Move, 1, fireC.cx + 35.0f, fireC.cy - 12.0f, 1020);

	// Finger 2: Simultaneously tap Crouch!
	const auto crouchC = hud.controlCircle(ControlId::Crouch);
	hud.onTouch(TouchAction::Down, 2, crouchC.cx, crouchC.cy, 1020);

	// Finger 3: Simultaneously hold Left Fixed Fire (4-finger claw!)
	const auto leftFireC = hud.controlCircle(ControlId::FireLeft);
	hud.onTouch(TouchAction::Down, 3, leftFireC.cx, leftFireC.cy, 1020);

	const auto out = hud.stepFrame(1025);
	EXPECT(out.stickTouched);
	EXPECT(out.moveLR > 30);
	EXPECT(out.moveUD < -30);
	EXPECT(out.fireHeld);
	EXPECT(out.fireJustPressed);
	EXPECT(out.crouchHeld);
	EXPECT(out.crouchJustPressed);
	EXPECT(out.isKeyActive(SampKey::Fire));
	EXPECT(out.isKeyActive(SampKey::Crouch));
	// Drag-to-aim on Right Fire produced exact camera delta!
	EXPECT(out.lookActive);
	EXPECT(std::fabs(out.lookDeltaX - 35.0f) < 0.01f);
	EXPECT(std::fabs(out.lookDeltaY - (-12.0f)) < 0.01f);

	// Release Right Fire (finger 1), Left Fire (finger 3) still holds Fire!
	hud.onTouch(TouchAction::Up, 1, fireC.cx + 35.0f, fireC.cy - 12.0f, 1030);
	const auto out2 = hud.stepFrame(1035);
	EXPECT(out2.fireHeld);
	EXPECT(out2.isKeyActive(SampKey::Fire));
	// Camera delta drained after previous frame
	EXPECT(std::fabs(out2.lookDeltaX) < 0.01f);
}

void testMinPulseLatchSurvivesFastTap()
{
	TouchHudCore hud;
	hud.setScreenSize(1920.0f, 1080.0f);
	hud.setLayout(makePresetLayout(PresetId::RoleplayHeavy));
	hud.setCapabilities(makeOnFootCaps(), 2000);

	std::array<RpStripLayoutItem, static_cast<size_t>(RpStripItem::Count)> items{};
	hud.computeRpStripItems(items);
	const auto& yRect = items[static_cast<size_t>(RpStripItem::KeyY)].rect;

	// Ultra-fast 8ms tap on Y
	hud.onTouch(TouchAction::Down, 0, yRect.x + yRect.w * 0.5f, yRect.y + yRect.h * 0.5f, 2000);
	hud.onTouch(TouchAction::Up,   0, yRect.x + yRect.w * 0.5f, yRect.y + yRect.h * 0.5f, 2008);

	// At 2060ms (still inside 110ms pulse window), KEY_YES must still be active for SA-MP sync!
	const auto mid = hud.stepFrame(2060);
	EXPECT(mid.isKeyActive(SampKey::Yes));

	// After 2120ms, pulse expires cleanly
	const auto after = hud.stepFrame(2120);
	EXPECT(!after.isKeyActive(SampKey::Yes));
}

void testCombosMultiTouchAndAltSmartLatch()
{
	TouchHudCore hud;
	hud.setScreenSize(1920.0f, 1080.0f);
	hud.setLayout(makePresetLayout(PresetId::RoleplayHeavy));
	hud.setCapabilities(makeOnFootCaps(), 3000);

	std::array<RpStripLayoutItem, static_cast<size_t>(RpStripItem::Count)> items{};
	hud.computeRpStripItems(items);
	const auto& altR = items[static_cast<size_t>(RpStripItem::KeyAlt)].rect;
	const auto& yR   = items[static_cast<size_t>(RpStripItem::KeyY)].rect;
	const auto& nR   = items[static_cast<size_t>(RpStripItem::KeyN)].rect;

	// 1) True 2-finger combo: Hold ALT (finger 0) + tap Y (finger 1)
	hud.onTouch(TouchAction::Down, 0, altR.x + 10.0f, altR.y + 10.0f, 3000);
	hud.onTouch(TouchAction::Down, 1, yR.x + 10.0f,   yR.y + 10.0f,   3020);
	const auto comboOut = hud.stepFrame(3030);
	EXPECT(comboOut.isKeyActive(SampKey::Walk));
	EXPECT(comboOut.isKeyActive(SampKey::Yes));
	EXPECT(comboOut.slowWalkHeld);

	// Release both: because ALT was used in a combo, releasing it quickly must NOT leave altLatched on!
	hud.onTouch(TouchAction::Up, 1, yR.x + 10.0f,   yR.y + 10.0f,   3040);
	hud.onTouch(TouchAction::Up, 0, altR.x + 10.0f, altR.y + 10.0f, 3050);
	EXPECT(!hud.visualState().altLatched);

	// 2) Smart 1-thumb ALT tap-latch: quick tap ALT (80ms) -> latches, then tap N -> ALT+N fires & auto-unlatches!
	hud.onTouch(TouchAction::Down, 0, altR.x + 10.0f, altR.y + 10.0f, 3500);
	hud.onTouch(TouchAction::Up,   0, altR.x + 10.0f, altR.y + 10.0f, 3580);
	EXPECT(hud.visualState().altLatched);
	const auto latchedOut = hud.stepFrame(3650);
	EXPECT(latchedOut.isKeyActive(SampKey::Walk));
	EXPECT(latchedOut.slowWalkHeld);

	// Now tap N with the same thumb
	hud.onTouch(TouchAction::Down, 0, nR.x + 10.0f, nR.y + 10.0f, 3700);
	hud.onTouch(TouchAction::Up,   0, nR.x + 10.0f, nR.y + 10.0f, 3720);
	EXPECT(!hud.visualState().altLatched); // Auto-cleared after combo!
	const auto altN = hud.stepFrame(3730);
	EXPECT(altN.isKeyActive(SampKey::Walk));
	EXPECT(altN.isKeyActive(SampKey::No));

	// 3) Long hold ALT (>250ms) without combo -> releases cleanly without latching
	hud.onTouch(TouchAction::Down, 0, altR.x + 10.0f, altR.y + 10.0f, 4000);
	hud.onTouch(TouchAction::Up,   0, altR.x + 10.0f, altR.y + 10.0f, 4300);
	EXPECT(!hud.visualState().altLatched);
}

void testPubgAutoSprintLockAndRpOverlay()
{
	// Verify default fresh-install preset uses Joystick steering for vehicles
	const auto defRp = makePresetLayout(PresetId::RoleplayHeavy);
	const auto defPubg = makePresetLayout(PresetId::PubgCompetitive);
	const auto defMin = makePresetLayout(PresetId::TwoFingerMinimal);
	EXPECT(defRp.prefs.vehicleSteering == VehicleSteeringMode::Joystick);
	EXPECT(defPubg.prefs.vehicleSteering == VehicleSteeringMode::Joystick);
	EXPECT(defMin.prefs.vehicleSteering == VehicleSteeringMode::Joystick);

	// Verify PUBG auto-sprint lock lifecycle
	TouchHudCore hud;
	hud.setScreenSize(1920.0f, 1080.0f);
	hud.setLayout(defRp);
	hud.setCapabilities(makeOnFootCaps(), 5000);

	const auto joyC = hud.controlCircle(ControlId::Joystick);
	hud.onTouch(TouchAction::Down, 0, joyC.cx, joyC.cy, 5000);
	// Push straight up beyond 1.25 * radius -> locks auto-sprint!
	hud.onTouch(TouchAction::Move, 0, joyC.cx, joyC.cy - joyC.r * 1.4f, 5020);
	hud.onTouch(TouchAction::Up,   0, joyC.cx, joyC.cy - joyC.r * 1.4f, 5040);

	const auto runOut = hud.stepFrame(5050);
	EXPECT(hud.visualState().sprintLocked);
	EXPECT(runOut.sprintHeld);
	EXPECT(runOut.moveUD == -128);
	EXPECT(runOut.isKeyActive(SampKey::Sprint));

	// Pressing Aim must cancel PUBG auto-sprint lock cleanly
	const auto aimC = hud.controlCircle(ControlId::Aim);
	hud.onTouch(TouchAction::Down, 2, aimC.cx, aimC.cy, 5400);
	EXPECT(!hud.visualState().sprintLocked);

	// During rpStripOnly overlays (Scoreboard / SelectTextDraw / Menu), gameplay controls hide
	// while RP Strip stays active (for TAB / ESC) and background taps are not eaten as camera drag.
	auto tdCaps = makeOnFootCaps();
	tdCaps.rpStripOnly = true;
	hud.setCapabilities(tdCaps, 5500);
	EXPECT(!hud.isControlActive(ControlId::FireRight));
	EXPECT(!hud.isControlActive(ControlId::Joystick));
	EXPECT(hud.isControlActive(ControlId::RpStrip));
	EXPECT(!hud.onTouch(TouchAction::Down, 4, 960.0f, 540.0f, 5510));

	// Tapping TAB while rpStripOnly (Scoreboard open) always emits scoreboardJustPressed
	std::array<RpStripLayoutItem, static_cast<size_t>(RpStripItem::Count)> rpItems{};
	hud.computeRpStripItems(rpItems);
	const auto& tabR = rpItems[static_cast<size_t>(RpStripItem::KeyTab)].rect;
	EXPECT(hud.onTouch(TouchAction::Down, 5, tabR.x + 10.0f, tabR.y + 10.0f, 5600));
	EXPECT(hud.onTouch(TouchAction::Up,   5, tabR.x + 10.0f, tabR.y + 10.0f, 6000));
	const auto sbOut = hud.stepFrame(6010);
	EXPECT(sbOut.scoreboardJustPressed);
}

void testVehicleControlsAndQELookBehind()
{
	TouchHudCore hud;
	hud.setScreenSize(1920.0f, 1080.0f);
	auto L = makePresetLayout(PresetId::RoleplayHeavy);
	L.prefs.vehicleSteering = VehicleSteeringMode::Arrows;
	hud.setLayout(L);
	hud.setCapabilities(makeDriverCaps(), 6000);

	// Arrows mode: SteerLeft + Accel + Q + E (Look Behind 320!)
	const auto leftC  = hud.controlCircle(ControlId::VehSteerLeft);
	const auto accelC = hud.controlCircle(ControlId::VehAccel);
	const auto qC     = hud.controlCircle(ControlId::VehLookLeftQ);
	const auto eC     = hud.controlCircle(ControlId::VehLookRightE);

	hud.onTouch(TouchAction::Down, 0, leftC.cx,  leftC.cy,  6000);
	hud.onTouch(TouchAction::Down, 1, accelC.cx, accelC.cy, 6000);
	hud.onTouch(TouchAction::Down, 2, qC.cx,     qC.cy,     6000);
	hud.onTouch(TouchAction::Down, 3, eC.cx,     eC.cy,     6000);

	const auto vOut = hud.stepFrame(6010);
	EXPECT(vOut.vehSteerLR == -128);
	EXPECT(vOut.vehAccel == 255);
	EXPECT(vOut.isKeyActive(SampKey::Sprint));
	EXPECT(vOut.isKeyActive(SampKey::LookLeft));
	EXPECT(vOut.isKeyActive(SampKey::LookRight));
	EXPECT(vOut.lookBehindHeld);

	// Switch to Joystick steering mode and verify steering/pitch analog output NEVER triggers gas or brake!
	L.prefs.vehicleSteering = VehicleSteeringMode::Joystick;
	hud.setLayout(L);
	hud.onCancelAll(6100);
	const auto vJoy = hud.controlCircle(ControlId::VehJoystick);
	hud.onTouch(TouchAction::Down, 0, vJoy.cx, vJoy.cy, 6200);
	hud.onTouch(TouchAction::Move, 0, vJoy.cx + vJoy.r, vJoy.cy - vJoy.r, 6210);
	const auto jOut = hud.stepFrame(6220);
	EXPECT(jOut.vehSteerLR > 60);
	EXPECT(jOut.vehSteerUD < -60);
	EXPECT(jOut.vehAccel == 0);
	EXPECT(jOut.vehBrake == 0);
	EXPECT(!jOut.isKeyActive(SampKey::Sprint));
	EXPECT(!jOut.isKeyActive(SampKey::Jump));

	// Verify Q/E are positioned on the left half so left-side Q/E + right-side Fire work simultaneously
	EXPECT(L.ctrl(ControlId::VehLookLeftQ).cx < 0.30f);
	EXPECT(L.ctrl(ControlId::VehLookRightE).cx < 0.30f);
	const auto qLeft = hud.controlCircle(ControlId::VehLookLeftQ);
	const auto vFire = hud.controlCircle(ControlId::VehFire);
	hud.onCancelAll(6240);
	hud.onTouch(TouchAction::Down, 0, qLeft.cx, qLeft.cy, 6250);
	hud.onTouch(TouchAction::Down, 1, vFire.cx, vFire.cy, 6250);
	const auto qFireOut = hud.stepFrame(6260);
	EXPECT(qFireOut.lookLeftHeld);
	EXPECT(qFireOut.fireHeld);
	EXPECT(qFireOut.isKeyActive(SampKey::LookLeft));
	EXPECT(qFireOut.isKeyActive(SampKey::Fire));

	// Aircraft (non-Hydra) hides dead VehHorn; Hydra (520) exposes VehHorn as JET/HOVER thruster toggle
	L.prefs.vehicleSteering = VehicleSteeringMode::Arrows;
	hud.setLayout(L);
	ContextCapabilities planeCaps{};
	planeCaps.context = HudContext::Aircraft;
	planeCaps.isAircraftWithGear = true;
	planeCaps.isHydra = false;
	hud.setCapabilities(planeCaps, 6300);
	EXPECT(!hud.isControlActive(ControlId::VehHorn));
	EXPECT(hud.isControlActive(ControlId::VehPitchUp));

	planeCaps.isHydra = true;
	hud.setCapabilities(planeCaps, 6350);
	EXPECT(hud.isControlActive(ControlId::VehHorn));
	EXPECT(hud.isControlActive(ControlId::VehFlare));
	const auto jetBtn = hud.controlCircle(ControlId::VehHorn);
	const auto flareBtn = hud.controlCircle(ControlId::VehFlare);
	const auto pitchUpBtn = hud.controlCircle(ControlId::VehPitchUp);
	hud.onTouch(TouchAction::Down, 0, jetBtn.cx, jetBtn.cy, 6360);
	hud.onTouch(TouchAction::Down, 1, pitchUpBtn.cx, pitchUpBtn.cy, 6360);
	hud.onTouch(TouchAction::Down, 2, flareBtn.cx, flareBtn.cy, 6360);
	const auto hydraOut = hud.stepFrame(6370);
	EXPECT(hydraOut.hydraThrusterJustPressed);
	EXPECT(hydraOut.hydraFlareJustPressed);
	EXPECT(hydraOut.hydraFlareHeld);
	EXPECT(!hydraOut.hornHeld);
	EXPECT(hydraOut.vehSteerUD == -128);
	EXPECT(hydraOut.vehAccel == 0);
	EXPECT(hydraOut.vehBrake == 0);
	EXPECT(!hydraOut.isKeyActive(SampKey::AnalogUp)); // Pitch in Aircraft must not collide with Num8 Hydra thruster!

	// Stunt car airborne / Bike wheelie: Smart pitch mode activates VehPitchUp/Down for vehSteerUD
	ContextCapabilities stuntCaps = makeDriverCaps();
	stuntCaps.isAirborne = false;
	hud.setCapabilities(stuntCaps, 6400);
	EXPECT(!hud.isControlActive(ControlId::VehPitchUp));
	stuntCaps.isAirborne = true;
	hud.setCapabilities(stuntCaps, 6410);
	EXPECT(hud.isControlActive(ControlId::VehPitchUp));
	stuntCaps.isAirborne = false;
	stuntCaps.isBike = true;
	hud.setCapabilities(stuntCaps, 6420);
	EXPECT(hud.isControlActive(ControlId::VehPitchDown));
}

void testCustomComboAndCommandButtonsAndJsonRoundTrip()
{
	auto L = makePresetLayout(PresetId::RoleplayHeavy);
	L.prefs.aimMode = AimMode::Hold;
	L.prefs.vehicleSteering = VehicleSteeringMode::Joystick;
	L.prefs.lookSensitivity = 1.75f;
	L.ctrl(ControlId::FireRight).cx = 0.81f;
	L.ctrl(ControlId::FireRight).scale = 1.35f;

	L.customButtons[0].enabled = true;
	L.customButtons[0].actionType = CustomActionType::KeyCombo;
	L.customButtons[0].keyMask = keyBit(SampKey::Walk) | keyBit(SampKey::Yes);
	L.customButtons[0].setLabel("ALT+Y");

	L.customButtons[2].enabled = true;
	L.customButtons[2].actionType = CustomActionType::ChatCommand;
	L.customButtons[2].contextMask = kCtxBoth;
	L.customButtons[2].cx = 0.50f;
	L.customButtons[2].cy = 0.50f;
	L.customButtons[2].setLabel("背包");
	L.customButtons[2].setCommand("/inv");

	const std::string json = serializeLayoutJson(L);
	TouchHudLayout loaded{};
	EXPECT(deserializeLayoutJson(json, loaded));
	EXPECT(loaded.prefs.aimMode == AimMode::Hold);
	EXPECT(loaded.prefs.vehicleSteering == VehicleSteeringMode::Joystick);
	EXPECT(std::fabs(loaded.prefs.lookSensitivity - 1.75f) < 0.001f);
	EXPECT(std::fabs(loaded.ctrl(ControlId::FireRight).cx - 0.81f) < 0.001f);
	EXPECT(std::fabs(loaded.ctrl(ControlId::FireRight).scale - 1.35f) < 0.001f);
	EXPECT(loaded.customButtons[0].enabled);
	EXPECT(loaded.customButtons[0].keyMask == (keyBit(SampKey::Walk) | keyBit(SampKey::Yes)));
	EXPECT(std::strcmp(loaded.customButtons[0].label, "ALT+Y") == 0);
	EXPECT(loaded.customButtons[2].enabled);
	EXPECT(loaded.customButtons[2].actionType == CustomActionType::ChatCommand);
	EXPECT(std::strcmp(loaded.customButtons[2].command, "/inv") == 0);

	// Verify firing custom combo & command buttons in TouchHudCore
	TouchHudCore hud;
	hud.setScreenSize(1920.0f, 1080.0f);
	hud.setLayout(loaded);
	hud.setCapabilities(makeOnFootCaps(), 7000);

	const auto c0 = hud.customButtonCircle(0);
	const auto c2 = hud.customButtonCircle(2);
	hud.onTouch(TouchAction::Down, 0, c0.cx, c0.cy, 7000);
	hud.onTouch(TouchAction::Down, 1, c2.cx, c2.cy, 7000);
	const auto out = hud.stepFrame(7010);
	EXPECT(out.isKeyActive(SampKey::Walk));
	EXPECT(out.isKeyActive(SampKey::Yes));
	EXPECT(out.pendingCommands.size() == 1 && out.pendingCommands[0] == "/inv");

	// Malformed JSON must fail cleanly without crashing
	TouchHudLayout bad{};
	EXPECT(!deserializeLayoutJson("{not valid json", bad));
}

} // namespace

int main()
{
	testMultiFingerConcurrencyAndDragToAim();
	testMinPulseLatchSurvivesFastTap();
	testCombosMultiTouchAndAltSmartLatch();
	testPubgAutoSprintLockAndRpOverlay();
	testVehicleControlsAndQELookBehind();
	testCustomComboAndCommandButtonsAndJsonRoundTrip();

	if (g_failures > 0) {
		std::fprintf(stderr, "touch_hud_core: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("touch_hud_core: all tests passed!\n");
	return 0;
}
