#include "TouchHud.h"

#include "../../Client.h"
#include "../../InputArbiter.h"
#include "../../Settings.h"
#include "../../Game/Game.h"
#include "../../Game/Hooks.h"
#include "../../Game/Pad.h"
#include "../../Game/PadInternal.h"
#include "../../Game/PlayerPed.h"
#include "../../Game/TouchCombat.h"
#include "../../Game/TouchWidgets.h"
#include "../../Game/Util.h"
#include "../../Game/Vehicle.h"
#include "../../Game/VehicleControl.h"
#include "../../Game/Engine/Symbols.h"
#include "../../GameSA/include/SA.h"
#include "../../Java/EditObject.h"
#include "../../Java/Java.h"
#include "../../Net/LocalPlayer.h"
#include "../../Net/MenuPool.h"
#include "../../Net/NetGame.h"
#include "../../Net/PlayerPool.h"
#include "../../Net/TextDrawPool.h"
#include "../../Net/VehiclePool.h"
#include "../../Game/Artwork/ArtworkManager.h"
#include "../UI.h"
#include "../SAMPWidgets/Chat.h"
#include "../SAMPWidgets/Keyboard.h"
#include "../SAMPWidgets/NetStats.h"
#include "../SAMPWidgets/Scoreboard.h"
#include "../SAMPWidgets/Spawn.h"
#include "../SAMPWidgets/SplashScreen.h"
#include "../SAMPWidgets/VoiceButton.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace TouchHud {

namespace {

std::mutex g_coreMutex;
touch_hud::TouchHudCore g_core;
bool g_initialized = false;

std::atomic<bool> g_editorOpen{false};
std::atomic<bool> g_screenshotHidden{false};
std::atomic<bool> g_pendingOpenEditor{false};

// Snapshot of RP strip outer bounds for lock-free NVEvent hit check during TextDraw selection
std::mutex g_snapMutex;
touch_hud::ScreenRect g_rpBoundsSnap{};
bool g_hudVisibleSnap = false;

// External camera look deltas (e.g. Object Editor 360-degree viewport orbit)
std::mutex g_extLookMutex;
float g_extLookDX = 0.0f;
float g_extLookDY = 0.0f;

// GameMain-owned per-frame state
touch_hud::TouchHudCore::FrameOutput g_frame{};
float g_frameLookVecX = 0.0f;
float g_frameLookVecY = 0.0f;
bool g_frameLookNonZero = false;

// Frame-scoped edges for Pad.cpp hooks (reset every stepGameFrame; never sticky across frames)
bool g_edgeWeaponPrev = false;
bool g_edgeWeaponNext = false;
bool g_edgeInteractF = false;
bool g_edgeCrouch = false;
bool g_edgeJump = false;

// Pending one-shot hardware key edges queued between logic frames
bool g_hwPendingWeaponPrev = false;
bool g_hwPendingWeaponNext = false;
bool g_hwPendingInteractF = false;
bool g_hwPendingJump = false;

// Aircraft manual camera orbit state (GameMain thread, applied inside CCam::Process_FollowCar_SA hook)
float g_airCamYawOff = 0.0f;
float g_airCamPitchOff = 0.0f;
uint64_t g_airCamHoldUntilMs = 0;

// Last valid on-foot camera look heading (GTA ped rotation = atan2(-Front.x, Front.y))
bool g_hasOnFootCamHeading = false;
float g_lastOnFootCamHeading = 0.0f;

// Airborne stunt pitch button hysteresis (keeps Up/Down pitch controls visible cleanly over jumps)
uint64_t g_airborneUntilMs = 0;

// Hardware keyboard state (GameMain thread, migrated from legacy ButtonPanel)
bool g_hwHeldBKeys[ePadKeys::SIZE] = {};
bool g_hwW = false, g_hwS = false, g_hwA = false, g_hwD = false;
bool g_hwDpadUp = false, g_hwDpadDown = false, g_hwDpadLeft = false, g_hwDpadRight = false;
int16_t g_hwLR = 0;
int16_t g_hwUD = 0;

void recomputeHwAxes()
{
	const bool left  = g_hwA || g_hwDpadLeft;
	const bool right = g_hwD || g_hwDpadRight;
	const bool up    = g_hwW || g_hwDpadUp;
	const bool down  = g_hwS || g_hwDpadDown;
	g_hwLR = (left && !right) ? -128 : (right && !left ? 128 : 0);
	g_hwUD = (up && !down) ? -128 : (down && !up ? 128 : 0);
}

uint64_t nowSteadyMs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string layoutFilePath()
{
	const char* gameDir = Client::gameDir();
	if (!gameDir || !gameDir[0]) return {};
	std::string base(gameDir);
	if (base.back() != '/') base.push_back('/');
	return base + "SAMP/hud_layout.json";
}

bool isAircraftModel(int modelId)
{
	switch (modelId) {
		case 417: case 425: case 447: case 460: case 469: case 476:
		case 487: case 488: case 497: case 511: case 512: case 513:
		case 519: case 520: case 548: case 553: case 563: case 577:
		case 592: case 593:
			return true;
		default:
			return false;
	}
}

bool isPlaneWithGear(int modelId)
{
	switch (modelId) {
		case 519: case 520: case 553: case 577: case 592: case 476:
			return true;
		default:
			return false;
	}
}

bool isHydraulicsOrTurretVehicle(int modelId)
{
	switch (modelId) {
		case 407: // Firetruck (water cannon)
		case 432: // Rhino (turret)
		case 525: // Towtruck
		case 530: // Forklift
		case 531: // Tractor
		case 578: // DFT-30
		case 601: // SWAT water cannon
		case 534: case 535: case 536: case 566: case 567: case 575: case 576: // Lowriders
			return true;
		default:
			return false;
	}
}

bool isNearAnyVehicle(CPlayerPed* pLocal, float maxDistSq, bool& outCanPassenger)
{
	outCanPassenger = false;
	if (!pLocal || !pLocal->m_ped || !pNetGame || !pNetGame->GetVehiclePool()) return false;
	CVehiclePool* pool = pNetGame->GetVehiclePool();
	const sa::CVector pedPos = pLocal->m_ped->GetPosition();
	VEHICLEID nearest = pool->FindNearestToLocalPlayerPed();
	if (nearest == INVALID_VEHICLE_ID) return false;
	CVehicle* veh = pool->GetAt(nearest);
	if (!veh || !veh->m_pVehicle) return false;
	const sa::CVector vPos = veh->m_pVehicle->GetPosition();
	const float dx = vPos.x - pedPos.x;
	const float dy = vPos.y - pedPos.y;
	const float dz = vPos.z - pedPos.z;
	const float d2 = dx * dx + dy * dy + dz * dz;
	if (d2 <= maxDistSq) {
		outCanPassenger = (d2 <= 64.0f); // <= 8m for passenger G
		return true;
	}
	return false;
}

bool isLocalCancellableSpecialAction(CPlayerPed* pLocal)
{
	if (!pLocal) return false;
	return pLocal->GetStuff() != STUFF_TYPE_NONE
		|| pLocal->IsDancing()
		|| pLocal->IsPissing()
		|| pLocal->HasHandsUp();
}

int scopedAimCamMode(uint8_t wep)
{
	switch (wep) {
		case 34: return 7;  // Sniper -> MODE_SNIPER
		case 35: return 8;  // RPG -> MODE_ROCKETLAUNCHER
		case 36: return 51; // HS Rocket -> MODE_ROCKETLAUNCHER_HS
		case 43: return 46; // Camera -> MODE_CAMERA
		default: return 0;
	}
}

touch_hud::ContextCapabilities computeLiveCapabilities()
{
	touch_hud::ContextCapabilities caps{};
	caps.context = touch_hud::HudContext::Hidden;

	if (g_editorOpen.load(std::memory_order_acquire)
		|| g_screenshotHidden.load(std::memory_order_acquire)
		|| Game::IsGamePaused()) {
		return caps;
	}

	if (!pGame || !pGame->IsGameInputEnabled()) {
		return caps;
	}

	if (pUI) {
		if ((pUI->splashscreen() && pUI->splashscreen()->visible())
			|| (pUI->spawn() && pUI->spawn()->visible())
			|| (pUI->keyboard() && pUI->keyboard()->visible())) {
			return caps;
		}
	}

	if (EditObject::m_visible.load(std::memory_order_acquire) && !EditObject::isWalkMode()) {
		return caps;
	}

	if (artwork::ArtworkManager::instance().isDownloadGateActive()) {
		return caps;
	}

	// Hide during widescreen cutscenes
	if (sa::getTheCamera().m_WideScreenOn) {
		return caps;
	}

	// Hide completely only when Java Dialog/Keyboard owns focus (NOT when Scoreboard is open!)
	const bool scoreboardOpen = (pUI && pUI->scoreboard() && pUI->scoreboard()->visible());
	if (!scoreboardOpen && InputArbiter::shouldBypassExternalTouch()) {
		return caps;
	}

	CPlayerPed* pLocal = pGame->FindPlayerPed();
	if (!pLocal || !pLocal->m_ped) {
		return caps;
	}

	if (!pNetGame && !Client::offlineMode()) {
		return caps;
	}

	// Hide HUD completely while connecting / awaiting join / restarting (no buttons on loading screen!)
	if (pNetGame) {
		if (pNetGame->GetGameState() != GAMESTATE_CONNECTED) {
			return caps;
		}
	}

	// Keep bottom RP Strip visible & clickable during Scoreboard, SelectTextDraw, ServerMenu, or ObjectSelect
	// so the player can always tap TAB or ESC directly without reaching for the phone Back button!
	const bool modalRpOnly = scoreboardOpen
		|| (pNetGame && pNetGame->GetTextDrawPool() && pNetGame->GetTextDrawPool()->GetState())
		|| (pNetGame && pNetGame->GetMenuPool() && pNetGame->GetMenuPool()->IsVisible())
		|| (pNetGame && pNetGame->GetObjectPool() && pNetGame->GetObjectPool()->IsSelectState());
	if (modalRpOnly) {
		caps.rpStripOnly = true;
	}

	// Online spawn / spectate / death lifecycle gating
	if (pNetGame) {
		CLocalPlayer* lp = pNetGame->GetPlayerPool() ? pNetGame->GetPlayerPool()->GetLocalPlayer() : nullptr;
		if (!lp) {
			return caps;
		}
		if (lp->IsSpectating()) {
			// Spectators get Joystick (WASD flymode/target) + Sprint/Jump/Fire/F + FreeCameraLook + RP Strip;
			// if a modal overlay (Scoreboard / TextDraw / Menu) is open, rpStripOnly hides gameplay buttons.
			caps.context = touch_hud::HudContext::Spectating;
			return caps;
		}
		if (!lp->m_bIsActive) {
			// Before spawning (connecting camera, login screen, class selection):
			// show RP Strip ONLY if a server TextDraw/Menu/Scoreboard is active; otherwise hide completely!
			if (modalRpOnly) {
				caps.context = touch_hud::HudContext::OnFoot;
				caps.rpStripOnly = true;
				return caps;
			}
			return caps;
		}
	}

	if (pLocal->IsDead()) {
		if (modalRpOnly) {
			caps.context = touch_hud::HudContext::OnFoot;
			caps.rpStripOnly = true;
			return caps;
		}
		return caps;
	}

	if (pLocal->IsInVehicle() && pLocal->m_ped->pVehicle) {
		sa::CVehicle* pVeh = pLocal->m_ped->pVehicle;
		const int modelId = pVeh->m_nModelIndex;
		const uint8_t subType = GetVehicleSubtype(pVeh);
		if (pLocal->IsAPassenger()) {
			caps.context = touch_hud::HudContext::InVehiclePassenger;
			caps.canPassengerDriveBy = pLocal->CanDoPassengerDriveBy();
			caps.isInDriveBy = pLocal->IsInPassengerDriveByMode();
		}
		else if (isAircraftModel(modelId) || subType == VEHICLE_SUBTYPE_HELI || subType == VEHICLE_SUBTYPE_PLANE) {
			caps.context = touch_hud::HudContext::Aircraft;
			caps.isAircraftWithGear = isPlaneWithGear(modelId);
			caps.isHydra = (modelId == 520);
			caps.isHunter = (modelId == 425);
			caps.hydraJetForward = caps.isHydra && IsLocalHydraJetForward();
		}
		else {
			caps.context = touch_hud::HudContext::InVehicleDriver;
			caps.isHydraulicsOrTurret = isHydraulicsOrTurretVehicle(modelId);
			caps.isBike = (subType == VEHICLE_SUBTYPE_BIKE
				|| subType == VEHICLE_SUBTYPE_PUSHBIKE
				|| subType == VEHICLE_SUBTYPE_QUADBIKE);
			const uint64_t now = nowSteadyMs();
			const bool currentlyAirborne = (pVeh->m_pEntityWeAreOn == nullptr)
				&& (std::fabs(pVeh->GetMoveSpeed().z) > 0.08f);
			if (currentlyAirborne) {
				g_airborneUntilMs = now + 1000;
			}
			caps.isAirborne = (now < g_airborneUntilMs);
		}
		return caps;
	}

	caps.context = ((pLocal->m_ped->m_nPhysicalFlags & 0x100u) != 0)
		? touch_hud::HudContext::Swimming
		: touch_hud::HudContext::OnFoot;

	bool canPass = false;
	caps.nearVehicle = isNearAnyVehicle(pLocal, 100.0f, canPass);
	caps.canPassengerEnter = canPass;
	caps.hasFirearm = (GetAimableLocalPed() != nullptr);
	caps.hasMelee = (GetBlockableLocalPed() != nullptr);
	caps.isAiming = g_touchCombat.aimLatch || EngineIsAiming(pLocal);
	caps.isScoped1stPerson = Using1stPersonWeaponCam();
	caps.isCrouched = pLocal->IsCrouching();
	return caps;
}

} // namespace

void initialize(float screenW, float screenH)
{
	{
		std::lock_guard<std::mutex> lock(g_coreMutex);
		g_core.setScreenSize(screenW, screenH);
		g_initialized = true;
	}
	loadLayout();
}

void setScreenSize(float screenW, float screenH)
{
	std::lock_guard<std::mutex> lock(g_coreMutex);
	g_core.setScreenSize(screenW, screenH);
}

bool loadLayout()
{
	const std::string path = layoutFilePath();
	if (path.empty()) return false;
	FILE* fp = std::fopen(path.c_str(), "rb");
	if (!fp) return false;

	std::fseek(fp, 0, SEEK_END);
	const long sz = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz > 256 * 1024) {
		std::fclose(fp);
		return false;
	}

	std::string content(static_cast<size_t>(sz), '\0');
	const size_t nRead = std::fread(content.data(), 1, static_cast<size_t>(sz), fp);
	std::fclose(fp);
	if (nRead != static_cast<size_t>(sz)) return false;

	touch_hud::TouchHudLayout parsed{};
	if (!touch_hud::deserializeLayoutJson(content, parsed)) {
		spdlog::warn("TouchHud: failed to parse {}, using default preset", path);
		return false;
	}

	std::lock_guard<std::mutex> lock(g_coreMutex);
	g_core.setLayout(parsed);
	spdlog::info("TouchHud: loaded layout from {}", path);
	return true;
}

bool saveLayout()
{
	const std::string path = layoutFilePath();
	if (path.empty()) return false;

	std::string json;
	{
		std::lock_guard<std::mutex> lock(g_coreMutex);
		json = touch_hud::serializeLayoutJson(g_core.layout());
	}

	const std::string tmpPath = path + ".tmp";
	FILE* fp = std::fopen(tmpPath.c_str(), "wb");
	if (!fp) {
		spdlog::warn("TouchHud: failed to open {} for writing", tmpPath);
		return false;
	}
	const size_t written = std::fwrite(json.data(), 1, json.size(), fp);
	const bool ok = (written == json.size()) && (std::fflush(fp) == 0);
	std::fclose(fp);

	if (ok && std::rename(tmpPath.c_str(), path.c_str()) == 0) {
		spdlog::info("TouchHud: saved layout to {}", path);
		return true;
	}
	std::remove(tmpPath.c_str());
	return false;
}

void setScreenshotHidden(bool hidden)
{
	g_screenshotHidden.store(hidden, std::memory_order_release);
}

bool isEditorOpen()
{
	return g_editorOpen.load(std::memory_order_acquire);
}

bool isRpStripHitSnapshot(float x, float y)
{
	std::lock_guard<std::mutex> lock(g_snapMutex);
	if (!g_hudVisibleSnap) return false;
	return g_rpBoundsSnap.contains(x, y, 8.0f);
}

bool handleTouch(int type, int pointerId, float x, float y)
{
	if (g_editorOpen.load(std::memory_order_acquire)) {
		return handleEditorTouch(type, pointerId, x, y);
	}

	touch_hud::TouchAction act;
	if (type == 2) act = touch_hud::TouchAction::Down;
	else if (type == 3) act = touch_hud::TouchAction::Move;
	else if (type == 1) act = touch_hud::TouchAction::Up;
	else return false;

	std::lock_guard<std::mutex> lock(g_coreMutex);
	if (!g_initialized) return false;
	return g_core.onTouch(act, pointerId, x, y, nowSteadyMs());
}

void handleCancel()
{
	clearHardwareKeys();
	{
		std::lock_guard<std::mutex> lock(g_extLookMutex);
		g_extLookDX = 0.0f;
		g_extLookDY = 0.0f;
	}
	std::lock_guard<std::mutex> lock(g_coreMutex);
	if (!g_initialized) return;
	g_core.onCancelAll(nowSteadyMs());
}

void injectExternalLookDelta(float dx, float dy)
{
	std::lock_guard<std::mutex> lock(g_extLookMutex);
	g_extLookDX += dx;
	g_extLookDY += dy;
}

void clearHardwareKeys()
{
	std::memset(g_hwHeldBKeys, 0, sizeof(g_hwHeldBKeys));
	g_hwW = g_hwS = g_hwA = g_hwD = false;
	g_hwDpadUp = g_hwDpadDown = g_hwDpadLeft = g_hwDpadRight = false;
	g_hwLR = 0;
	g_hwUD = 0;
}

void handleHardwareKey(int androidKeyCode, bool isDown)
{
	if (!Settings::kmEnabled()) {
		if (!isDown) clearHardwareKeys();
		return;
	}
	if (!pUI || !pGame) return;

	if (isDown && pUI->keyboard() && pUI->keyboard()->visible()) {
		if (androidKeyCode == 111 /* KEYCODE_ESCAPE */) {
			pUI->keyboard()->hide();
		}
		return;
	}

	if (isDown && pNetGame && pNetGame->GetMenuPool() && pNetGame->GetMenuPool()->IsVisible()) {
		if (androidKeyCode == 19 || androidKeyCode == 51) { pNetGame->GetMenuPool()->MoveSelection(-1); return; }
		if (androidKeyCode == 20 || androidKeyCode == 47) { pNetGame->GetMenuPool()->MoveSelection(1); return; }
		if (androidKeyCode == 66 || androidKeyCode == 62) { pNetGame->GetMenuPool()->SelectCurrentRow(); return; }
		if (androidKeyCode == 34 || androidKeyCode == 111) { pNetGame->GetMenuPool()->ExitMenu(); return; }
	}

	CPlayerPed* pPed = pGame->FindPlayerPed();
	const bool inVeh = (pPed && pPed->IsInVehicle());

	switch (androidKeyCode) {
		case 48:  // T
		case 136: // F6
			if (isDown) {
				clearHardwareKeys();
				if (Settings::systemkeyboard()) {
					if (g_java) g_java->showKeyboard(true);
				}
				else if (pUI->keyboard() && pUI->chat()) {
					pUI->keyboard()->show(pUI->chat());
				}
			}
			break;
		case 61: // TAB
			if (isDown) {
				if (pUI->scoreboard()) pUI->scoreboard()->setVisible(!pUI->scoreboard()->visible());
				if (!inVeh) LocalPlayerKeys.bKeys[ePadKeys::KEY_ACTION] = true;
			}
			break;
		case 135: // F5
			if (isDown && pUI->netstats()) pUI->netstats()->setClientStatsVisible(!pUI->netstats()->clientStatsVisible());
			break;
		case 137: // F7
			if (isDown && pUI->chat()) pUI->chat()->setVisible(!pUI->chat()->visible());
			break;
		case 138: // F8
			if (isDown) Hooks::requestScreenshot(-1);
			break;
		case 140: // F10
			if (isDown && pUI->netstats()) pUI->netstats()->setServerStatsVisible(!pUI->netstats()->serverStatsVisible());
			break;
		case 111: // ESC
			if (isDown) InputArbiter::dismissTopNativeModal();
			break;
		case 35: // G
			if (isDown && pNetGame && pNetGame->GetPlayerPool() && pNetGame->GetPlayerPool()->GetLocalPlayer()) {
				pNetGame->GetPlayerPool()->GetLocalPlayer()->EnterVehicleAsPassenger();
			}
			break;
		case 34: // F
		case 66: // ENTER
			g_hwHeldBKeys[ePadKeys::KEY_SECONDARY_ATTACK] = isDown;
			if (isDown) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = true;
				g_hwPendingInteractF = true;
			}
			break;
		case 53: // Y
			g_hwHeldBKeys[ePadKeys::KEY_YES] = isDown;
			if (isDown) LocalPlayerKeys.bKeys[ePadKeys::KEY_YES] = true;
			break;
		case 42: // N
			g_hwHeldBKeys[ePadKeys::KEY_NO] = isDown;
			if (isDown) LocalPlayerKeys.bKeys[ePadKeys::KEY_NO] = true;
			break;
		case 36: // H
			if (inVeh && pPed && !pPed->IsAPassenger()) {
				g_hwHeldBKeys[ePadKeys::KEY_CROUCH] = isDown;
				if (isDown) LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = true;
			}
			else {
				g_hwHeldBKeys[ePadKeys::KEY_CTRL_BACK] = isDown;
				if (isDown) LocalPlayerKeys.bKeys[ePadKeys::KEY_CTRL_BACK] = true;
			}
			break;
		case 31: // C
			if (inVeh) {
				g_hwHeldBKeys[ePadKeys::KEY_CROUCH] = isDown;
				if (isDown) LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = true;
			}
			else {
				NotifyLocalHardwareCrouchKey(isDown);
			}
			break;
		case 9: // 2
			g_hwHeldBKeys[ePadKeys::KEY_SUBMISSION] = isDown;
			if (isDown) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_SUBMISSION] = true;
				if (inVeh && pPed && !pPed->IsAPassenger()) {
					sa::CVehicle* pVeh = pPed->GetGtaVehicle();
					if (pVeh && GetVehicleSubtype(pVeh) == VEHICLE_SUBTYPE_PLANE) {
						RequestLocalPlaneGearToggle();
					}
				}
			}
			break;
		case 45: // Q
			g_hwHeldBKeys[ePadKeys::KEY_LOOK_LEFT] = isDown;
			if (isDown && !inVeh) g_hwPendingWeaponPrev = true;
			break;
		case 33: // E
			g_hwHeldBKeys[ePadKeys::KEY_LOOK_RIGHT] = isDown;
			if (isDown && !inVeh) g_hwPendingWeaponNext = true;
			break;
		case 57: // ALT_LEFT
		case 58: // ALT_RIGHT
			if (inVeh) g_hwHeldBKeys[ePadKeys::KEY_FIRE] = isDown;
			else       g_hwHeldBKeys[ePadKeys::KEY_WALK] = isDown;
			break;
		case 113: // CTRL_LEFT
		case 114: // CTRL_RIGHT
			g_hwHeldBKeys[ePadKeys::KEY_FIRE] = isDown;
			if (isDown && pNetGame) pNetGame->SendSpawn();
			break;
		case 62: // SPACE
			if (inVeh) g_hwHeldBKeys[ePadKeys::KEY_HANDBRAKE] = isDown;
			else       g_hwHeldBKeys[ePadKeys::KEY_SPRINT] = isDown;
			break;
		case 59: // SHIFT_LEFT
		case 60: // SHIFT_RIGHT
			if (inVeh) g_hwHeldBKeys[ePadKeys::KEY_HANDBRAKE] = isDown;
			else {
				g_hwHeldBKeys[ePadKeys::KEY_JUMP] = isDown;
				if (isDown) g_hwPendingJump = true;
			}
			break;
		case 51: g_hwW = isDown; recomputeHwAxes(); break;
		case 19: g_hwDpadUp = isDown; recomputeHwAxes(); break;
		case 47: g_hwS = isDown; recomputeHwAxes(); break;
		case 20: g_hwDpadDown = isDown; recomputeHwAxes(); break;
		case 29:
			g_hwA = isDown; recomputeHwAxes();
			if (isDown && pNetGame) pNetGame->SendPrevClass();
			break;
		case 21:
			g_hwDpadLeft = isDown; recomputeHwAxes();
			if (isDown && pNetGame) pNetGame->SendPrevClass();
			break;
		case 32:
			g_hwD = isDown; recomputeHwAxes();
			if (isDown && pNetGame) pNetGame->SendNextClass();
			break;
		case 22:
			g_hwDpadRight = isDown; recomputeHwAxes();
			if (isDown && pNetGame) pNetGame->SendNextClass();
			break;
		case 152: g_hwHeldBKeys[ePadKeys::KEY_ANALOG_UP] = isDown; break;
		case 146: g_hwHeldBKeys[ePadKeys::KEY_ANALOG_DOWN] = isDown; break;
		case 148: g_hwHeldBKeys[ePadKeys::KEY_ANALOG_LEFT] = isDown; break;
		case 150: g_hwHeldBKeys[ePadKeys::KEY_ANALOG_RIGHT] = isDown; break;
		case 30: // B
		case 50: // V
			if (pUI->voicebutton() && Settings::voice() && Settings::voiceMicEnable()) {
				if (Settings::voiceMicMode() == 1) {
					if (isDown) pUI->voicebutton()->setRecording(!pUI->voicebutton()->recording());
				}
				else {
					pUI->voicebutton()->setRecording(isDown);
				}
			}
			break;
		default:
			break;
	}
}

void openEditor()
{
	g_pendingOpenEditor.store(true, std::memory_order_release);
}

void closeEditor(bool saveChanges)
{
	if (saveChanges) {
		saveLayout();
	}
	g_editorOpen.store(false, std::memory_order_release);
}

touch_hud::TouchHudLayout getLayoutCopy()
{
	std::lock_guard<std::mutex> lock(g_coreMutex);
	return g_core.layout();
}

void applyLayout(const touch_hud::TouchHudLayout& layout, bool persist)
{
	{
		std::lock_guard<std::mutex> lock(g_coreMutex);
		g_core.setLayout(layout);
	}
	if (persist) {
		saveLayout();
	}
}

void stepGameFrame()
{
	// Handle deferred editor open request safely on GameMain thread
	if (g_pendingOpenEditor.exchange(false, std::memory_order_acq_rel)) {
		if (pUI && pUI->scoreboard() && pUI->scoreboard()->visible()) {
			pUI->scoreboard()->setVisible(false);
		}
		touch_hud::TouchHudLayout curLayout;
		bool inVehNow = false;
		if (pGame && pGame->FindPlayerPed()) {
			inVehNow = pGame->FindPlayerPed()->IsInVehicle();
		}
		{
			std::lock_guard<std::mutex> lock(g_coreMutex);
			g_core.onCancelAll(nowSteadyMs());
			curLayout = g_core.layout();
		}
		onEditorOpened(curLayout, inVehNow);
		g_editorOpen.store(true, std::memory_order_release);
	}

	const uint64_t nowMs = nowSteadyMs();
	const touch_hud::ContextCapabilities caps = computeLiveCapabilities();

	touch_hud::TouchHudCore::FrameOutput frame{};
	touch_hud::ScreenRect rpBounds{};
	float screenH = 1080.0f;
	touch_hud::AimMode aimMode = touch_hud::AimMode::Toggle;

	{
		std::lock_guard<std::mutex> lock(g_coreMutex);
		if (!g_initialized) return;
		g_core.setCapabilities(caps, nowMs);
		frame = g_core.stepFrame(nowMs);
		screenH = g_core.screenH();
		aimMode = g_core.layout().prefs.aimMode;

		std::array<touch_hud::RpStripLayoutItem, static_cast<size_t>(touch_hud::RpStripItem::Count)> items{};
		g_core.computeRpStripItems(items, &rpBounds);
	}

	{
		std::lock_guard<std::mutex> lock(g_snapMutex);
		g_rpBoundsSnap = rpBounds;
		g_hudVisibleSnap = (caps.context != touch_hud::HudContext::Hidden);
	}

	// Reset frame-scoped edges cleanly every logic frame (never sticky across frames)
	g_frame = frame;
	g_edgeWeaponPrev = frame.weaponPrevJustPressed || std::exchange(g_hwPendingWeaponPrev, false);
	g_edgeWeaponNext = frame.weaponNextJustPressed || std::exchange(g_hwPendingWeaponNext, false);
	g_edgeInteractF  = frame.interactFJustPressed  || std::exchange(g_hwPendingInteractF, false);
	g_edgeCrouch     = frame.crouchJustPressed;
	g_edgeJump       = frame.jumpJustPressed       || std::exchange(g_hwPendingJump, false);

	// Merge held hardware keyboard keys & axes into current frame
	for (int i = 0; i < ePadKeys::SIZE; ++i) {
		if (g_hwHeldBKeys[i]) {
			g_frame.activeKeyMask |= (1u << i);
			LocalPlayerKeys.bKeys[i] = true;
		}
	}
	if (g_hwHeldBKeys[ePadKeys::KEY_WALK]) g_frame.slowWalkHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_SPRINT]) g_frame.sprintHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_JUMP]) g_frame.jumpHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_FIRE]) g_frame.fireHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_HANDBRAKE]) g_frame.handbrakeHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_LOOK_LEFT]) g_frame.lookLeftHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_LOOK_RIGHT]) g_frame.lookRightHeld = true;
	if (g_hwHeldBKeys[ePadKeys::KEY_LOOK_LEFT] && g_hwHeldBKeys[ePadKeys::KEY_LOOK_RIGHT]) g_frame.lookBehindHeld = true;
	if (g_frame.moveLR == 0 && g_hwLR != 0) { g_frame.moveLR = g_hwLR; g_frame.stickTouched = true; }
	if (g_frame.moveUD == 0 && g_hwUD != 0) { g_frame.moveUD = g_hwUD; g_frame.stickTouched = true; }
	// PC GTA:SA vehicle parity: strictly separate W/S (throttle/brake) from Arrow Up/Down (pitch/lean/udAnalog)!
	// Never let Arrow Up/Down step on gas/brake, and never let W/S pitch aircraft/bikes.
	const bool inVehCtx = (caps.context == touch_hud::HudContext::InVehicleDriver
		|| caps.context == touch_hud::HudContext::InVehiclePassenger
		|| caps.context == touch_hud::HudContext::Aircraft);
	const int16_t hwDpadUD = (g_hwDpadUp && !g_hwDpadDown) ? -128 : ((g_hwDpadDown && !g_hwDpadUp) ? 128 : 0);
	if (g_frame.vehSteerLR == 0 && g_hwLR != 0) g_frame.vehSteerLR = g_hwLR;
	if (g_frame.vehSteerUD == 0 && hwDpadUD != 0) g_frame.vehSteerUD = hwDpadUD;
	if (g_frame.syncLR == 0 && g_hwLR != 0) g_frame.syncLR = g_hwLR;
	if (g_frame.syncUD == 0) {
		const int16_t hwSyncUD = (caps.context == touch_hud::HudContext::InVehicleDriver
			|| caps.context == touch_hud::HudContext::Aircraft)
			? hwDpadUD
			: g_hwUD;
		if (hwSyncUD != 0) g_frame.syncUD = hwSyncUD;
	}
	if (inVehCtx && g_hwW && !g_hwS && g_frame.vehAccel == 0) {
		g_frame.vehAccel = 255;
		g_frame.activeKeyMask |= touch_hud::keyBit(touch_hud::SampKey::Sprint);
	}
	if (inVehCtx && g_hwS && !g_hwW && g_frame.vehBrake == 0) {
		g_frame.vehBrake = 255;
		g_frame.activeKeyMask |= touch_hud::keyBit(touch_hud::SampKey::Jump);
	}

	// ---- 1) Camera look deltas & aircraft 360-degree manual orbit ----
	float extDX = 0.0f;
	float extDY = 0.0f;
	float lookSens = 1.0f;
	{
		std::lock_guard<std::mutex> lock(g_extLookMutex);
		extDX = g_extLookDX;
		extDY = g_extLookDY;
		g_extLookDX = 0.0f;
		g_extLookDY = 0.0f;
	}
	{
		std::lock_guard<std::mutex> lock(g_coreMutex);
		lookSens = g_core.layout().prefs.lookSensitivity;
	}
	const float rawLookDX = frame.lookDeltaX + extDX * lookSens;
	const float rawLookDY = frame.lookDeltaY + extDY * lookSens;

	sa::CCamera& theCam = sa::getTheCamera();
	const int activeCamIdx = (theCam.m_nActiveCam < 3) ? static_cast<int>(theCam.m_nActiveCam) : 0;
	sa::CCam& cam = theCam.m_aCams[activeCamIdx];

	// Convert screen pixel deltas to GTA CWidgetRegionLook units (normalized to 1080p height)
	const float dpiNorm = (screenH > 1.0f) ? (1080.0f / screenH) : 1.0f;
	constexpr float kPixelToGtaLook = 5.0f;
	g_frameLookVecX = std::clamp(rawLookDX * dpiNorm * kPixelToGtaLook, -420.0f, 420.0f);
	g_frameLookVecY = std::clamp(rawLookDY * dpiNorm * kPixelToGtaLook, -420.0f, 420.0f);
	g_frameLookNonZero = (std::fabs(g_frameLookVecX) > 0.01f) || (std::fabs(g_frameLookVecY) > 0.01f);

	// ---- 1b) Aircraft 360-degree manual camera orbit (applied in CCam::Process_FollowCar_SA hook) ----
	if (caps.context == touch_hud::HudContext::Aircraft) {
		constexpr float kAirYawRadPerPx = 0.0044f;
		constexpr float kAirPitchRadPerPx = 0.0032f;
		if (std::fabs(rawLookDX) > 0.01f || std::fabs(rawLookDY) > 0.01f) {
			g_airCamYawOff = touch_hud::wrapAngleRad(g_airCamYawOff - rawLookDX * dpiNorm * kAirYawRadPerPx);
			// Drag UP (rawLookDY < 0) looks UP (+Front.z); drag DOWN (rawLookDY > 0) looks DOWN (-Front.z)
			g_airCamPitchOff = std::clamp(g_airCamPitchOff - rawLookDY * dpiNorm * kAirPitchRadPerPx, -0.72f, 0.85f);
		}
		if (frame.lookActive) {
			g_airCamHoldUntilMs = nowMs + 1400;
		}
		else if (nowMs >= g_airCamHoldUntilMs) {
			g_airCamYawOff *= 0.94f;
			g_airCamPitchOff *= 0.94f;
			if (std::fabs(g_airCamYawOff) < 0.003f) g_airCamYawOff = 0.0f;
			if (std::fabs(g_airCamPitchOff) < 0.003f) g_airCamPitchOff = 0.0f;
		}
	}
	else {
		g_airCamYawOff = 0.0f;
		g_airCamPitchOff = 0.0f;
		g_airCamHoldUntilMs = 0;
	}

	// ---- 1c) Sample on-foot camera heading so entering Aim faces where the camera is looking ----
	if (caps.context == touch_hud::HudContext::OnFoot || caps.context == touch_hud::HudContext::Swimming) {
		const uint16_t mode = cam.m_nMode;
		const bool validOnFootCam = (mode == sa::MODE_FOLLOWPED || mode == sa::MODE_FOLLOW_PED_WITH_BIND
			|| mode == sa::MODE_AIMWEAPON || mode == sa::MODE_SNIPER
			|| mode == sa::MODE_ROCKETLAUNCHER || mode == sa::MODE_ROCKETLAUNCHER_HS
			|| mode == sa::MODE_CAMERA || mode == sa::MODE_M16_1STPERSON);
		if (validOnFootCam) {
			const float fx = cam.Front.x;
			const float fy = cam.Front.y;
			if ((fx * fx + fy * fy) > 1e-4f) {
				// Matches CCam::Process_AimWeapon @ 0x4a7138: atan2f(-Front.x, Front.y)
				g_lastOnFootCamHeading = std::atan2(-fx, fy);
				g_hasOnFootCamHeading = true;
			}
		}
	}
	else {
		g_hasOnFootCamHeading = false;
	}

	// ---- 2) Update unified TouchCombatState (consumed by Pad.cpp & UseGun hooks) ----
	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	const bool bOnFoot = (caps.context == touch_hud::HudContext::OnFoot || caps.context == touch_hud::HudContext::Swimming);
	const bool bPassengerDriveBy = (caps.context == touch_hud::HudContext::InVehiclePassenger && caps.isInDriveBy);

	g_touchCombat.onFootStickSampled = bOnFoot && !IsLocalActivePedMovementLocked();
	g_touchCombat.stickTouched = g_touchCombat.onFootStickSampled && g_frame.stickTouched;
	g_touchCombat.stickLR = g_touchCombat.stickTouched ? g_frame.moveLR : 0;
	g_touchCombat.stickUD = g_touchCombat.stickTouched ? g_frame.moveUD : 0;

	if (pLocal && bOnFoot && pLocal->IsDancing() && g_touchCombat.stickTouched) {
		g_touchCombat.stickLR = (g_touchCombat.stickLR < -32) ? -128 : (g_touchCombat.stickLR > 32 ? 128 : 0);
		g_touchCombat.stickUD = (g_touchCombat.stickUD < -32) ? -128 : (g_touchCombat.stickUD > 32 ? 128 : 0);
	}

	// Fire edge + hold + Stuff item use
	const bool bFireNow = (bOnFoot || bPassengerDriveBy) && g_frame.fireHeld;
	g_touchCombat.fireJustPressed = (bOnFoot || bPassengerDriveBy) && (g_frame.fireJustPressed || (bFireNow && !g_touchCombat.firePrevDown));
	if (g_touchCombat.fireJustPressed) {
		g_touchCombat.fireDownTickMs = GetTickCount();
		if (bOnFoot && pLocal && pLocal->GetStuff() != STUFF_TYPE_NONE) {
			RequestStuffUse();
		}
	}
	g_touchCombat.firePrevDown = bFireNow;
	g_touchCombat.fireDown = bFireNow;

	// Aim toggle / hold + 1st-person scoped weapon camera transitions
	if (CPlayerPed* pAimPed = GetAimableLocalPed()) {
		const uint8_t curWep = pAimPed->GetCurrentWeapon();
		if (g_touchCombat.lastAimWeapon != curWep) {
			g_touchCombat.aimLatch = false;
			g_touchCombat.lastAimWeapon = curWep;
		}

		if (aimMode == touch_hud::AimMode::Hold) {
			g_touchCombat.aimLatch = g_frame.aimHeld;
		}
		else if (g_frame.aimJustPressed) {
			g_touchCombat.aimLatch = !g_touchCombat.aimLatch;
		}

		const int scopedMode = scopedAimCamMode(curWep);
		if (scopedMode != 0) {
			if (g_touchCombat.aimLatch && !Using1stPersonWeaponCam()) {
				eng::fn::CCamera_SetNewPlayerWeaponMode(&sa::getTheCamera(), scopedMode, 0, 0);
				eng::fn::CPed_SetPedState(pAimPed->m_ped, sa::ePedState::PED_SNIPER_MODE);
			}
			else if (!g_touchCombat.aimLatch && Using1stPersonWeaponCam()) {
				eng::fn::CCamera_ClearPlayerWeaponMode(&sa::getTheCamera());
				if (pAimPed->m_ped->m_pPlayerData) {
					pAimPed->m_ped->m_pPlayerData->m_bFreeAiming = false;
				}
				eng::fn::CPlayerPed_ClearWeaponTarget(pAimPed->m_ped);
			}
		}
	}
	else {
		g_touchCombat.aimLatch = false;
		g_touchCombat.lastAimWeapon = 0;
	}

	// Melee block: holding Aim button while unarmed / holding melee weapon
	g_touchCombat.blockDown = (GetBlockableLocalPed() != nullptr) && g_frame.aimHeld;

	// ---- 3) One-shot actions & UI/gameplay edges ----
	if (g_frame.driveByToggleJustPressed && caps.canPassengerDriveBy) {
		RequestPassengerDriveByToggle();
	}

	if (g_frame.passengerGJustPressed && pNetGame && pNetGame->GetPlayerPool()) {
		if (CLocalPlayer* lp = pNetGame->GetPlayerPool()->GetLocalPlayer()) {
			lp->EnterVehicleAsPassenger();
		}
	}

	if (g_frame.submission2JustPressed && caps.isAircraftWithGear) {
		RequestLocalPlaneGearToggle();
	}

	if (g_frame.hydraThrusterJustPressed && caps.isHydra) {
		RequestLocalHydraThrusterToggle();
	}

	if (g_frame.camZoomJustPressed && !Using1stPersonWeaponCam()) {
		if (bOnFoot) {
			int32_t cur = theCam.m_nPedZoom;
			theCam.m_nPedZoom = (cur >= 1 && cur < 3) ? (cur + 1) : 1;
		}
		else {
			int32_t cur = theCam.m_nCarZoom;
			theCam.m_nCarZoom = (cur >= 0 && cur < 4) ? (cur + 1) : 0;
		}
	}

	if (g_frame.escJustPressed) {
		InputArbiter::dismissTopNativeModal();
	}

	if (g_frame.scoreboardJustPressed && pUI && pUI->scoreboard()) {
		pUI->scoreboard()->setVisible(!pUI->scoreboard()->visible());
	}

	if (g_frame.screenshotJustPressed) {
		Hooks::requestScreenshot();
	}

	if (g_frame.openEditorJustPressed) {
		openEditor();
	}

	for (const auto& cmd : g_frame.pendingCommands) {
		if (pUI && pUI->chat() && !cmd.empty()) {
			pUI->chat()->keyboardEvent(cmd);
		}
	}

	// ---- 4) Inject all active SA-MP keys (held + >=110ms min-pulse latch) into LocalPlayerKeys ----
	if (caps.context != touch_hud::HudContext::Hidden) {
		for (int i = 0; i < static_cast<int>(touch_hud::SampKey::Count); ++i) {
			if ((g_frame.activeKeyMask & (1u << i)) != 0u) {
				LocalPlayerKeys.bKeys[i] = true;
			}
		}

		// Authoritative aim KEY_HANDBRAKE on foot
		if (bOnFoot && pLocal && (g_touchCombat.aimLatch || EngineIsAiming(pLocal))) {
			LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = true;
		}

		// Cancel Stuff / Dance / HandsUp / Pissing via F on foot
		if (bOnFoot && g_frame.interactFJustPressed && isLocalCancellableSpecialAction(pLocal)) {
			LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = true;
		}

		// PC SA-MP parity: authoritatively sync discrete directional keys {-128, 0, +128}
		// (KEY_LEFT / KEY_RIGHT / KEY_UP / KEY_DOWN) across ALL active contexts — including
		// OnFoot, Swimming, Slow-Walk (ALT), TogglePlayerControllable(0) frozen prompts/menus,
		// ApplyAnimation (horse-riding / sitting / custom combat stances), Spectating (/flymode),
		// InVehicleDriver, Aircraft, and InVehiclePassenger!
		LocalPlayerKeys.wKeyLR = static_cast<uint16_t>(g_frame.syncLR);
		LocalPlayerKeys.wKeyUD = static_cast<uint16_t>(g_frame.syncUD);
	}
}

const touch_hud::TouchHudCore::FrameOutput& currentFrame()
{
	return g_frame;
}

bool getAircraftCamOrbit(float& outYaw, float& outPitch)
{
	if (std::fabs(g_airCamYawOff) < 0.001f && std::fabs(g_airCamPitchOff) < 0.001f) {
		outYaw = 0.0f;
		outPitch = 0.0f;
		return false;
	}
	outYaw = g_airCamYawOff;
	outPitch = g_airCamPitchOff;
	return true;
}

bool getLastOnFootCamHeading(float& outHeading)
{
	if (!g_hasOnFootCamHeading) {
		return false;
	}
	outHeading = g_lastOnFootCamHeading;
	return true;
}

bool isInteractFJustPressed()
{
	return g_edgeInteractF;
}

bool isJumpJustPressed()
{
	return g_edgeJump;
}

bool consumeWeaponPrevEdge()
{
	const bool v = g_edgeWeaponPrev;
	g_edgeWeaponPrev = false;
	return v;
}

bool consumeWeaponNextEdge()
{
	const bool v = g_edgeWeaponNext;
	g_edgeWeaponNext = false;
	return v;
}

bool consumeCrouchEdge()
{
	const bool v = g_edgeCrouch;
	g_edgeCrouch = false;
	return v;
}

bool queryWidgetTouched(sa::WidgetIDs id, sa::CVector2D* outVec, uint32_t& outResult)
{
	if (Game::IsGamePaused()) {
		return false;
	}
	if (IsNativeDisplayWidget(id)) {
		return false;
	}

	switch (id) {
		case sa::WidgetIDs::WIDGET_LOOK:
			if (g_frameLookNonZero) {
				if (outVec) {
					outVec->x = g_frameLookVecX;
					outVec->y = g_frameLookVecY;
				}
				outResult = 1;
				return true;
			}
			outResult = 0;
			return true;

		case sa::WidgetIDs::WIDGET_PED_MOVE:
			if (g_touchCombat.stickTouched) {
				if (outVec) {
					outVec->x = static_cast<float>(g_touchCombat.stickLR);
					outVec->y = static_cast<float>(g_touchCombat.stickUD);
				}
				outResult = 1;
				return true;
			}
			outResult = 0;
			return true;

		case sa::WidgetIDs::WIDGET_ATTACK:
		case sa::WidgetIDs::WIDGET_CAR_SHOOT:
		case sa::WidgetIDs::WIDGET_NITRO:
		case sa::WidgetIDs::WIDGET_AIR_GUN:
		case sa::WidgetIDs::WIDGET_ROCKET:
		case sa::WidgetIDs::WIDGET_VEHICLE_BOMB:
		case sa::WidgetIDs::WIDGET_BUTTON_DIVE:
			outResult = g_frame.fireHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_SPRINT:
		case sa::WidgetIDs::WIDGET_BUTTON_SPRINT:
		case sa::WidgetIDs::WIDGET_BUTTON_SWIM:
			outResult = g_frame.sprintHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_BUTTON_CROUCH:
			outResult = g_frame.crouchHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_ACCELERATE:
			outResult = (g_frame.vehAccel > 0) ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_BRAKE:
			outResult = (g_frame.vehBrake > 0) ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_DRIVE_HYBRID:
		case sa::WidgetIDs::WIDGET_VEHICLE_LANECORRECTION:
		case sa::WidgetIDs::WIDGET_VEHICLE_FLICK:
		case sa::WidgetIDs::WIDGET_VEHICLE_SLIDER_LEFT_BUTTON:
		case sa::WidgetIDs::WIDGET_VEHICLE_SLIDER_RIGHT_BUTTON:
			outResult = 0u;
			return true;

		case sa::WidgetIDs::WIDGET_HANDBRAKE:
			outResult = g_frame.handbrakeHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_HORN:
			outResult = g_frame.hornHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_VEHICLE_STEER_LEFT:
			outResult = (g_frame.vehSteerLR < -24) ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_VEHICLE_STEER_RIGHT:
			outResult = (g_frame.vehSteerLR > 24) ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_VEHICLE_STEER_ANALOG:
			if (g_frame.vehSteerLR != 0 || g_frame.vehSteerUD != 0) {
				if (outVec) {
					outVec->x = static_cast<float>(g_frame.vehSteerLR);
					outVec->y = static_cast<float>(g_frame.vehSteerUD);
				}
				outResult = 1;
				return true;
			}
			outResult = 0;
			return true;

		case sa::WidgetIDs::WIDGET_VEHICLE_SHOOT_LEFT:
		case sa::WidgetIDs::WIDGET_VEHICLE_TURRET_LEFT:
			outResult = g_frame.lookLeftHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_VEHICLE_SHOOT_RIGHT:
		case sa::WidgetIDs::WIDGET_VEHICLE_TURRET_RIGHT:
			outResult = g_frame.lookRightHeld ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_ENTER_CAR:
			outResult = g_frame.isKeyActive(touch_hud::SampKey::SecondaryAttack) ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_PARACHUTE:
			outResult = g_frame.fireJustPressed ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_ZOOM_OUT:
			outResult = (Using1stPersonWeaponCam() && g_frame.lookLeftHeld) ? 1u : 0u;
			return true;

		case sa::WidgetIDs::WIDGET_ZOOM_IN:
			outResult = (Using1stPersonWeaponCam() && g_frame.lookRightHeld) ? 1u : 0u;
			return true;

		default:
			return false;
	}
}

bool queryWidgetReleased(sa::WidgetIDs id, sa::CVector2D* outVec, uint32_t& outResult)
{
	(void)outVec;
	if (Game::IsGamePaused() || IsNativeDisplayWidget(id)) {
		return false;
	}
	if (id == sa::WidgetIDs::WIDGET_ENTER_CAR) {
		outResult = g_edgeInteractF ? 1u : 0u;
		return true;
	}
	return false;
}

bool queryWidgetHeldDown(sa::WidgetIDs id, uint32_t& outResult)
{
	if (Game::IsGamePaused() || IsNativeDisplayWidget(id)) {
		return false;
	}
	if (id == sa::WidgetIDs::WIDGET_LOOK) {
		// GTA uses IsHeldDown(WIDGET_LOOK) for look-behind in car/ped — drive it via Q+E!
		outResult = g_frame.lookBehindHeld ? 1u : 0u;
		return true;
	}
	return false;
}

RenderSnapshot captureRenderSnapshot()
{
	RenderSnapshot snap{};
	std::lock_guard<std::mutex> lock(g_coreMutex);
	snap.layout = g_core.layout();
	snap.caps = g_core.capabilities();
	snap.vis = g_core.visualState();
	snap.screenW = g_core.screenW();
	snap.screenH = g_core.screenH();
	snap.editorOpen = g_editorOpen.load(std::memory_order_acquire);
	snap.visible = g_initialized
		&& !snap.editorOpen
		&& !g_screenshotHidden.load(std::memory_order_acquire)
		&& (snap.caps.context != touch_hud::HudContext::Hidden);
	return snap;
}

void render(ImGuiRenderer* renderer)
{
	if (!renderer) return;
	const RenderSnapshot snap = captureRenderSnapshot();
	if (snap.editorOpen) {
		renderTouchHudEditor(renderer, snap.screenW, snap.screenH);
	}
	else if (snap.visible) {
		renderTouchHudOverlay(renderer, snap);
	}
}

} // namespace TouchHud
