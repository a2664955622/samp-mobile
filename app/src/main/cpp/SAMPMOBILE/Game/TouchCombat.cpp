#include "TouchCombat.h"
#include "Common.h"
#include "Game.h"
#include "Pad.h"
#include "PadInternal.h"
#include "PlayerPed.h"
#include "TouchWidgets.h"
#include "Util.h"
#include "Engine/PatchRegistry.h"
#include "Engine/Symbols.h"
#include "../Addr.h"
#include "../Client.h"
#include "../Net/NetGame.h"
#include "../Net/LocalPlayer.h"
#include "../UI/TouchHud/TouchHud.h"
#include "Enums/WidgetIDs.h"
#include <Memory.h>
#include <SA.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
// Disassembly-verified aim-entry camera direction alignment:
// All three aim camera processors read ped->m_fCurrentRotation (+0x6c4) when ResetStatics=1:
//   - CCam::Process_AimWeapon    @ 0x4a6098 (m_fHorizontalAngle = rot - pi/2 + crosshairX)
//   - CCam::Process_M16_1stPerson @ 0x4a3f74 (Sniper/Camera/M16: m_fHorizontalAngle = rot - pi/2)
//   - CCam::Process_Rocket       @ 0x4a54ec (RPG/HS Rocket: m_fHorizontalAngle = rot - pi/2)
// Aligning localPed->m_fCurrentRotation & m_fAimingRotation to the pre-aim camera look heading
// (atan2(-Front.x, Front.y)) right before ResetStatics=1 makes BOTH the camera and the character
// (including 1-handed pistols/Deagle/SMG where bAimWithArm skips ped rotation at 0x4a7104) turn
// immediately toward where the player is looking, with GTA's exact shoulder crosshair offset.
void alignLocalPedToCameraOnAimInit(sa::CCam* cam)
{
	if (!cam || !cam->ResetStatics || CurrentPlayerSlot() != 0) {
		return;
	}
	sa::CPed* localPed = GamePool_FindPlayerPed();
	if (!localPed || reinterpret_cast<void*>(cam->CamTargetEntity) != reinterpret_cast<void*>(localPed)) {
		return;
	}
	if (localPed->m_pEntLockOnTarget || localPed->IsInVehicle()) {
		return;
	}
	float camHeading = 0.0f;
	if (!TouchHud::getLastOnFootCamHeading(camHeading)) {
		return;
	}
	localPed->m_fCurrentRotation = camHeading;
	localPed->m_fAimingRotation = camHeading;
}

bool isLocalAircraftDriver(sa::CPed* pPed)
{
	if (CurrentPlayerSlot() != 0) return false;
	sa::CPed* localPed = GamePool_FindPlayerPed();
	if (!localPed || (pPed && pPed != localPed)) return false;
	sa::CVehicle* veh = localPed->pVehicle;
	if (!veh || veh->pDriver != localPed) return false;
	const uint8_t sub = GetVehicleSubtype(veh);
	return sub == VEHICLE_SUBTYPE_HELI || sub == VEHICLE_SUBTYPE_PLANE;
}
} // namespace

static std::atomic<bool> g_bStuffUseRequested{false};
static std::atomic<bool> g_bPassengerDriveByToggleRequested{false};

void RequestStuffUse()
{
	g_bStuffUseRequested.store(true, std::memory_order_release);
}

bool ConsumeStuffUseRequest()
{
	return g_bStuffUseRequested.exchange(false, std::memory_order_acq_rel);
}

void RequestPassengerDriveByToggle()
{
	g_bPassengerDriveByToggleRequested.store(true, std::memory_order_release);
}

bool ConsumePassengerDriveByToggle()
{
	return g_bPassengerDriveByToggleRequested.exchange(false, std::memory_order_acq_rel);
}

TouchCombatState g_touchCombat{};

bool IsLocalMeleeComboActive()
{
	return GetBlockableLocalPed() != nullptr
		&& static_cast<int32_t>(g_touchCombat.meleeComboUntilMs - GetTickCount()) > 0;
}

bool IsLocalMeleeFireActive()
{
	return GetBlockableLocalPed() != nullptr
		&& static_cast<int32_t>(g_touchCombat.meleeFireUntilMs - GetTickCount()) > 0;
}

bool IsLocalMeleeBlockActive()
{
	return GetBlockableLocalPed() != nullptr && g_touchCombat.blockDown && !IsLocalMeleeComboActive();
}

static inline bool IsAimableWeapon(uint8_t wep)
{
	return (wep >= 22 && wep <= 38) || wep == 41 || wep == 42 || wep == 43;
}

static inline bool IsMeleeOrUnarmedWeapon(uint8_t wep)
{
	return wep <= 15 || wep == 46;
}

static inline bool IsRapidFireWeapon(uint8_t wep)
{
	switch (wep) {
		case 28: // Uzi
		case 29: // MP5
		case 30: // AK-47
		case 31: // M4
		case 32: // Tec-9
		case 37: // Flamethrower
		case 38: // Minigun
		case 41: // Spraycan
		case 42: // Fire Extinguisher
			return true;
		default:
			return false;
	}
}

static inline bool IsNonHoldSingleShotWeapon(uint8_t wep)
{
	switch (wep) {
		case 16: // Grenade
		case 17: // Teargas
		case 18: // Molotov
		case 34: // Sniper
		case 35: // RPG
		case 36: // HS Rocket
		case 39: // Satchel
		case 40: // Detonator
		case 43: // Camera
			return true;
		default:
			return false;
	}
}

CPlayerPed* GetAimableLocalPed()
{
	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || !pLocal->m_ped || pLocal->IsDead() || pLocal->IsInVehicle()
			|| pLocal->IsInJetpack() || pLocal->IsCuffed() || pLocal->IsCarry()) {
		return nullptr;
	}
	return IsAimableWeapon(pLocal->GetCurrentWeapon()) ? pLocal : nullptr;
}

CPlayerPed* GetBlockableLocalPed()
{
	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || !pLocal->m_ped || pLocal->IsDead() || pLocal->IsInVehicle()
			|| pLocal->IsInJetpack() || pLocal->IsCuffed() || pLocal->IsCarry()
			|| pLocal->IsDancing() || pLocal->IsParachuting() || (pLocal->m_ped->m_nPhysicalFlags & 0x100u) != 0) {
		return nullptr;
	}
	return IsMeleeOrUnarmedWeapon(pLocal->GetCurrentWeapon()) ? pLocal : nullptr;
}

bool Using1stPersonWeaponCam()
{
	return eng::fn::CCamera_Using1stPersonWeaponMode(&sa::getTheCamera());
}

bool EngineIsAiming(CPlayerPed* pPed)
{
	if (!pPed || !pPed->m_ped) return false;
	if (Using1stPersonWeaponCam()) return true;
	if (pPed->m_ped->m_pPlayerData && pPed->m_ped->m_pPlayerData->m_bFreeAiming) return true;
	return pPed->m_ped->m_pEntLockOnTarget != nullptr;
}

bool CanLocalPedUseOnFootFire(uintptr_t pad, sa::CPed* pPed)
{
	if (!pad || *reinterpret_cast<uint16_t*>(pad + 272) != 0) {
		return false;
	}
	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || !pLocal->m_ped || pLocal->IsDead() || pLocal->IsCuffed() || pLocal->IsCarry()) {
		return false;
	}
	if (pPed && pPed != pLocal->m_ped) {
		return false;
	}
	return true;
}

bool EvaluateLocalTouchFireLevel(uint8_t weaponType)
{
	if (!g_touchCombat.fireDown) {
		return false;
	}
	if (IsRapidFireWeapon(weaponType)) {
		return true;
	}
	if (IsNonHoldSingleShotWeapon(weaponType)) {
		return g_touchCombat.fireJustPressed;
	}
	// Semi-auto firearms (pistols, shotguns, rifle): allow instant tap + comfortable
	// auto-repeat while held without locking into endless fire on short taps.
	if (g_touchCombat.fireJustPressed) {
		return true;
	}
	return (GetTickCount() - g_touchCombat.fireDownTickMs) >= 180;
}

// War Drum Mobile's CPlayerPed::ProcessControl clears m_bFreeAiming whenever
// WIDGET_ATTACK (1) is released for non-scoped guns, causing MODE_AIMWEAPON (53)
// to drop for 1 frame and re-enter with ResetStatics=1 (rightward shoulder yaw jump)
// while our touch aim-toggle latch is active. Suppress that release while latched.
DECL_HOOK(uint32_t, CTouchInterface_IsReleased_fromProcessControl, int32_t widgetID, void* pOutVec, int32_t frames)
{
	if (CurrentPlayerSlot() != 0 || (g_touchCombat.aimLatch && widgetID == sa::WidgetIDs::WIDGET_ATTACK)) {
		return 0;
	}
	return CTouchInterface_IsReleased_fromProcessControl(widgetID, pOutVec, frames);
}

// Virtual touch widget query overrides: feed zero-latency TouchHudCore state
// directly into GTA's engine (camera look, stick, car weapons, nitro, swim, etc.).
DECL_HOOK(uint32_t, CTouchInterface_IsTouched_hooked, int32_t widgetID, sa::CVector2D* pOutVec, int32_t frames)
{
	if (CurrentPlayerSlot() == 0) {
		uint32_t res = 0;
		if (TouchHud::queryWidgetTouched(static_cast<sa::WidgetIDs>(widgetID), pOutVec, res)) {
			return res;
		}
	}
	return CTouchInterface_IsTouched_hooked(widgetID, pOutVec, frames);
}

DECL_HOOK(uint32_t, CTouchInterface_IsReleased_hooked, int32_t widgetID, sa::CVector2D* pOutVec, int32_t frames)
{
	if (CurrentPlayerSlot() == 0) {
		uint32_t res = 0;
		if (TouchHud::queryWidgetReleased(static_cast<sa::WidgetIDs>(widgetID), pOutVec, res)) {
			return res;
		}
	}
	return CTouchInterface_IsReleased_hooked(widgetID, pOutVec, frames);
}

DECL_HOOK(uint32_t, CTouchInterface_IsHeldDown_hooked, int32_t widgetID, int32_t frames)
{
	if (CurrentPlayerSlot() == 0) {
		uint32_t res = 0;
		if (TouchHud::queryWidgetHeldDown(static_cast<sa::WidgetIDs>(widgetID), res)) {
			return res;
		}
	}
	return CTouchInterface_IsHeldDown_hooked(widgetID, frames);
}

DECL_HOOK(void, CTouchInterface_Clear)
{
	CTouchInterface_Clear();
	TouchHud::stepGameFrame();
}

void ApplyLocalCombatSyncKeys(uint16_t& wKeys, uint16_t& lrAnalog, uint16_t& udAnalog)
{
	(void)lrAnalog;
	(void)udAnalog;

	if (IsLocalMeleeComboActive()) {
		wKeys |= (128u | 16u); // KEY_HANDBRAKE | KEY_SECONDARY_ATTACK (FightStyle combo)
	}
	else if (IsLocalMeleeBlockActive()) {
		wKeys |= (128u | 32u); // KEY_HANDBRAKE | KEY_JUMP (Melee block)
	}
	else if (IsLocalMeleeFireActive()) {
		wKeys |= 4u; // KEY_FIRE
	}
}

DECL_HOOK(void, CCam_Process_AimWeapon, sa::CCam* _this, const sa::CVector& target, float orient, float speed, float speedVar)
{
	alignLocalPedToCameraOnAimInit(_this);
	CCam_Process_AimWeapon(_this, target, orient, speed, speedVar);
}

DECL_HOOK(void, CCam_Process_M16_1stPerson, sa::CCam* _this, const sa::CVector& target, float orient, float speed, float speedVar)
{
	alignLocalPedToCameraOnAimInit(_this);
	CCam_Process_M16_1stPerson(_this, target, orient, speed, speedVar);
}

DECL_HOOK(void, CCam_Process_Rocket, sa::CCam* _this, const sa::CVector& target, float orient, float speed, float speedVar, bool bHeatSeeking)
{
	alignLocalPedToCameraOnAimInit(_this);
	CCam_Process_Rocket(_this, target, orient, speed, speedVar, bHeatSeeking);
}

// War Drum Mobile explicitly returns 0 in CPad::AimWeaponLeftRight & AimWeaponUpDown
// whenever veh->m_nVehicleSubType is HELI (3) or PLANE (4), locking camera rotation
// while flying a Hydra / plane / helicopter. Feed TouchHud's WIDGET_LOOK delta directly.
DECL_HOOK(float, CPad_AimWeaponLeftRight, uintptr_t _this, sa::CPed* pPed, bool* bUsingTouch)
{
	if (_this && *reinterpret_cast<const uint16_t*>(_this + 272) == 0 && isLocalAircraftDriver(pPed)) {
		sa::CVector2D lookVec{};
		uint32_t touched = 0;
		if (TouchHud::queryWidgetTouched(sa::WidgetIDs::WIDGET_LOOK, &lookVec, touched) && touched != 0) {
			if (bUsingTouch) *bUsingTouch = true;
			return lookVec.x;
		}
	}
	return CPad_AimWeaponLeftRight(_this, pPed, bUsingTouch);
}

DECL_HOOK(int16_t, CPad_AimWeaponUpDown, uintptr_t _this, sa::CPed* pPed, bool* bUsingTouch)
{
	if (_this && *reinterpret_cast<const uint16_t*>(_this + 272) == 0 && isLocalAircraftDriver(pPed)) {
		sa::CVector2D lookVec{};
		uint32_t touched = 0;
		if (TouchHud::queryWidgetTouched(sa::WidgetIDs::WIDGET_LOOK, &lookVec, touched) && touched != 0) {
			if (bUsingTouch) *bUsingTouch = true;
			return static_cast<int16_t>(std::lround(-lookVec.y));
		}
	}
	return CPad_AimWeaponUpDown(_this, pPed, bUsingTouch);
}

// Disassembly-verified CCam::Process_FollowCar_SA (0x4a1390..0x4a14b8) ignores touch
// camera look for planes & helicopters (global state gate @ 0x4a0b8c + subtype skip @ 0x4a0ea8).
// Rotate GTA's already-computed _this->Front directly by TouchHud's (yawOff, pitchOff) so there
// is zero 180-degree sign flip or jump, then rebuild RW vectors and run GTA's collision check.
DECL_HOOK(void, CCam_Process_FollowCar_SA, sa::CCam* _this, const sa::CVector& target, float orient, float speed, float speedVar, bool bInReverse)
{
	CCam_Process_FollowCar_SA(_this, target, orient, speed, speedVar, bInReverse);

	if (!_this || CurrentPlayerSlot() != 0 || !isLocalAircraftDriver(nullptr)) {
		return;
	}
	float yawOff = 0.0f;
	float pitchOff = 0.0f;
	if (!TouchHud::getAircraftCamOrbit(yawOff, pitchOff)) {
		return;
	}

	const float dx = target.x - _this->Source.x;
	const float dy = target.y - _this->Source.y;
	const float dz = target.z - _this->Source.z;
	const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (dist < 1.0f || dist > 120.0f) {
		return;
	}

	sa::CVector aimPoint{
		_this->Source.x + _this->Front.x * dist,
		_this->Source.y + _this->Front.y * dist,
		_this->Source.z + _this->Front.z * dist,
	};

	const float baseYaw = std::atan2(_this->Front.y, _this->Front.x);
	const float basePitch = std::asin(std::clamp(_this->Front.z, -0.98f, 0.98f));
	const float newYaw = touch_hud::wrapAngleRad(baseYaw + yawOff);
	const float newPitch = std::clamp(basePitch + pitchOff, -0.78f, 0.95f);
	const float cosP = std::cos(newPitch);

	_this->Front.x = cosP * std::cos(newYaw);
	_this->Front.y = cosP * std::sin(newYaw);
	_this->Front.z = std::sin(newPitch);

	_this->Source.x = aimPoint.x - _this->Front.x * dist;
	_this->Source.y = aimPoint.y - _this->Front.y * dist;
	_this->Source.z = aimPoint.z - _this->Front.z * dist;

	eng::fn::CCam_GetVectorsReadyForRW(_this);
	eng::fn::CCamera_CameraColDetAndReact(&sa::getTheCamera(), &aimPoint, &_this->Source);
}

void InstallTouchCombatHooks()
{
	REG_HOOK_BL(Boot, addr::CPlayerPed_ProcessControl_AttackRelease_BranchBL, "CTouchInterface_IsReleased.ProcessControl",
		&CTouchInterface_IsReleased_fromProcessControl_hook, &CTouchInterface_IsReleased_fromProcessControl,
		"Keep m_bFreeAiming across touch fire release while aim-latched");
	REG_HOOK(Boot, CTouchInterface_Clear, CTouchInterface_Clear, "TouchHud per-frame input sampler");
	REG_HOOK(Boot, CTouchInterface_IsTouched, CTouchInterface_IsTouched_hooked, "TouchHud virtual widget IsTouched feed");
	REG_HOOK(Boot, CTouchInterface_IsReleased, CTouchInterface_IsReleased_hooked, "TouchHud virtual widget IsReleased feed");
	REG_HOOK(Boot, CTouchInterface_IsHeldDown, CTouchInterface_IsHeldDown_hooked, "TouchHud virtual widget IsHeldDown feed");
	REG_HOOK(Boot, CCam_Process_AimWeapon, CCam_Process_AimWeapon, "Align aim entry direction to camera view");
	REG_HOOK(Boot, CCam_Process_M16_1stPerson, CCam_Process_M16_1stPerson, "Align Sniper/Camera aim entry to camera view");
	REG_HOOK(Boot, CCam_Process_Rocket, CCam_Process_Rocket, "Align RPG aim entry to camera view");
	REG_HOOK(Boot, CCam_Process_FollowCar_SA, CCam_Process_FollowCar_SA, "360-degree aircraft manual camera orbit");
	REG_HOOK(Boot, CPad_AimWeaponLeftRight, CPad_AimWeaponLeftRight, "Unlock horizontal camera look in planes and helicopters");
	REG_HOOK(Boot, CPad_AimWeaponUpDown, CPad_AimWeaponUpDown, "Unlock vertical camera look in planes and helicopters");
}
