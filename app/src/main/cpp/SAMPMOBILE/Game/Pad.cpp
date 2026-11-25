#include "Game.h"
#include "PlayerContextScope.h"
#include "../UI/UI.h"
#include "../Net/NetGame.h"
#include "../Client.h"
#include "Enums/WidgetIDs.h"
#include <atomic>

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;

extern CPlayerPed* g_pCurrentFiredPed;
extern BULLET_DATA* g_pCurrentBulletData;
extern sa::CPlayerInfo* WORLD_PLAYERS;

PAD_KEYS LocalPlayerKeys;
PAD_KEYS RemotePlayerKeys[PLAYER_PED_SLOTS];

sa::CPed* dwCurPlayerActor = nullptr;
uint8_t byteInternalPlayer = 0;
uint8_t byteCurPlayer = 0;
static uintptr_t g_lastLocalPadPtr = 0;

void* ghook_CWorld_ProcessLineOfSight = nullptr;

static CPlayerPed* GetBlockableLocalPed();
static CPlayerPed* GetAimableLocalPed();
static bool Using1stPersonWeaponCam();

static inline bool IsPadMovementDisabled(uintptr_t pad)
{
	// Disassembly-verified on both arm32 (0x3fa220..0x3fa226) and arm64 (0x4dcd50..0x4dcd58):
	// CPad::DisablePlayerControls @ +272 (uint16_t), CPad::bDisablePlayerDuck/Move @ +287 (uint8_t).
	return !pad
		|| *reinterpret_cast<const uint16_t*>(pad + 272) != 0
		|| *reinterpret_cast<const uint8_t*>(pad + 287) != 0;
}

static inline bool IsLocalActivePedMovementLocked()
{
	if (!g_lastLocalPadPtr || !IsPadMovementDisabled(g_lastLocalPadPtr)) {
		return false;
	}
	if (pNetGame && pNetGame->GetPlayerPool()) {
		if (CLocalPlayer* pLocal = pNetGame->GetPlayerPool()->GetLocalPlayer()) {
			// Keep ButtonPanel directional keys working during inactive class-selection / spectating,
			// where servers read lrAnalog/udAnalog while the local ped is intentionally frozen.
			if (!pLocal->m_bIsActive || pLocal->IsSpectating()) {
				return false;
			}
		}
	}
	return true;
}

// Unified state for touch-screen combat & on-foot movement controls.
// Sampled once per game-logic frame right after CTouchInterface::Clear()
// so every CPad / UseGun hook and outbound sync packet reads consistent,
// zero-lag state without starving WIDGET_PED_MOVE's frame counter.
struct TouchCombatState {
	bool aimLatch = false;
	uint8_t lastAimWeapon = 0;
	bool onFootStickSampled = false;
	bool stickTouched = false;
	int16_t stickLR = 0;
	int16_t stickUD = 0;
	bool fireDown = false;
	bool firePrevDown = false;
	bool fireJustPressed = false;
	uint32_t fireDownTickMs = 0;
	bool blockDown = false;
	uint32_t meleeComboUntilMs = 0;
	uint32_t meleeFireUntilMs = 0;
};

static TouchCombatState g_touchCombat{};

static inline bool IsLocalMeleeComboActive()
{
	return GetBlockableLocalPed() != nullptr
		&& static_cast<int32_t>(g_touchCombat.meleeComboUntilMs - GetTickCount()) > 0;
}

static inline bool IsLocalMeleeFireActive()
{
	return GetBlockableLocalPed() != nullptr
		&& static_cast<int32_t>(g_touchCombat.meleeFireUntilMs - GetTickCount()) > 0;
}

static inline bool IsLocalMeleeBlockActive()
{
	return GetBlockableLocalPed() != nullptr && g_touchCombat.blockDown && !IsLocalMeleeComboActive();
}

static inline bool LocalPedHasCancellableSpecialAction(CPlayerPed* pPed)
{
	return pPed && !pPed->IsDead() && !pPed->IsInVehicle()
		&& (pPed->GetStuff() != STUFF_TYPE_NONE || pPed->IsDancing()
			|| pPed->HasHandsUp() || pPed->IsPissing());
}

// Tap vs hold threshold for regular firearms: a normal touch tap lasts
// ~40-150ms (3-9 frames), which would otherwise fire 2-3 rounds on automatic
// weapons. Press edge fires immediately; sustained hold repeats after 180ms.
static constexpr uint32_t kFireHoldRepeatDelayMs = 180;

// CWidget field offsets verified on arm32 and arm64 libGTASA.so
static constexpr uintptr_t kWidgetSpriteOffset      = VER_x32 ? 8   : 16;
static constexpr uintptr_t kWidgetPosOffset         = VER_x32 ? 12  : 24;
static constexpr uintptr_t kWidgetAlphaOffset       = VER_x32 ? 76  : 88;
static constexpr uintptr_t kWidgetEnabledOffset     = VER_x32 ? 77  : 89;
static constexpr uintptr_t kWidgetCachedTouchOffset = VER_x32 ? 120 : 132;

static uintptr_t g_activeScaledJoystickWidget = 0;
static float g_activeJoystickOrigSx = 0.0f;

// Temporarily scales CWidgetRegionMove's WidgetPosition.sx (outer ring radius,
// inner thumb radius, and max drag distance) while CWidget_Update always uses
// unscaled origSx so m_ScreenRect / m_TouchRect span the full left half of the
// screen (preventing CoorsAreInWidget in CWidgetRegionMove::Update from setting
// alpha = 0 mid-drag when joystickScale < 1.0f, and avoiding nested double-scale).
struct ScopedJoystickScale {
	uintptr_t w = 0;
	float origSx = 0.0f;

	explicit ScopedJoystickScale(uintptr_t widget)
	{
		float scale = Settings::joystickScale();
		if (!widget || scale == 1.0f || scale < 0.4f || scale > 2.0f) return;
		if (g_activeScaledJoystickWidget == widget) return;
		w = widget;
		origSx = *reinterpret_cast<float*>(w + kWidgetPosOffset + 8);
		g_activeScaledJoystickWidget = w;
		g_activeJoystickOrigSx = origSx;
		*reinterpret_cast<float*>(w + kWidgetPosOffset + 8) = origSx * scale;
	}

	~ScopedJoystickScale()
	{
		if (w) {
			*reinterpret_cast<float*>(w + kWidgetPosOffset + 8) = origSx;
			g_activeScaledJoystickWidget = 0;
			g_activeJoystickOrigSx = 0.0f;
		}
	}
};

void RunWithUnscaledJoystickSx(uintptr_t widget, void (*fn)(uintptr_t))
{
	if (widget && widget == g_activeScaledJoystickWidget) {
		float& sx = *reinterpret_cast<float*>(widget + kWidgetPosOffset + 8);
		const float scaledSx = sx;
		sx = g_activeJoystickOrigSx;
		fn(widget);
		sx = scaledSx;
		return;
	}
	fn(widget);
}

// Protect an actively held left thumbstick (WIDGET_PED_MOVE) from having its
// bound touch slot & clamped anchor overwritten when a second finger touches
// empty space on the left half of the screen during multi-touch combat.
DECL_HOOK(void, CWidget_SetInitialTouchWidget, int touchNum, uintptr_t widget)
{
	if (widget && touchNum >= 0 && touchNum < 4) {
		uintptr_t* widgets = GetTouchWidgets();
		if (widgets && widget == widgets[sa::WidgetIDs::WIDGET_PED_MOVE]) {
			auto* initialWidgets = eng::as<uintptr_t*>(eng::Sym::CWidget_m_pInitialTouchWidget);
			const int32_t existingTouch = *reinterpret_cast<const int32_t*>(widget + kWidgetCachedTouchOffset);
			if (initialWidgets && existingTouch >= 0 && existingTouch < 4
					&& existingTouch != touchNum
					&& initialWidgets[existingTouch] == widget
					&& eng::fn::CTouchInterface_IsTouchDown(existingTouch) != 0) {
				initialWidgets[touchNum] = 0;
				return;
			}
		}
	}
	CWidget_SetInitialTouchWidget(touchNum, widget);
}

DECL_HOOK(void, CWidgetRegionMove_Update, uintptr_t _this)
{
	ScopedJoystickScale guard(_this);
	CWidgetRegionMove_Update(_this);
}

DECL_HOOK(void, CWidgetRegionMove_ClampInternalTouchAnchor, uintptr_t _this)
{
	ScopedJoystickScale guard(_this);
	CWidgetRegionMove_ClampInternalTouchAnchor(_this);
}

DECL_HOOK(void, CWidgetRegionMove_Draw, uintptr_t _this)
{
	ScopedJoystickScale guard(_this);
	CWidgetRegionMove_Draw(_this);
}

DECL_HOOK(uint32_t, CWidgetRegionMove_IsTouched, uintptr_t _this, uintptr_t pVec2D)
{
	ScopedJoystickScale guard(_this);
	return CWidgetRegionMove_IsTouched(_this, pVec2D);
}

// CTaskSimplePlayerOnFoot::m_nTimer offset (2000ms idle fighter timeout)
static constexpr uintptr_t kTaskPlayerOnFootTimerOffset = VER_x32 ? 40 : 56;

extern std::atomic<bool> g_suppressWidgetSetEnabled;
static std::atomic<bool> g_suppressPedUpdatePosition{false};
static bool g_bEnableLineOfSightHook = false;

static inline uint8_t CurrentPlayerSlot()
{
	return pbyteCurrentPlayer ? *pbyteCurrentPlayer : 0;
}

struct ScopedAtomicBool {
	std::atomic<bool>& flag;
	bool prev;
	ScopedAtomicBool(std::atomic<bool>& f, bool next)
		: flag(f), prev(f.exchange(next, std::memory_order_acq_rel)) {}
	~ScopedAtomicBool() { flag.store(prev, std::memory_order_release); }
};

DECL_HOOK(void, CPed_UpdatePosition_fromProcessControl, sa::CPed* _this)
{
	if (g_suppressPedUpdatePosition.load(std::memory_order_acquire) && _this == dwCurPlayerActor) {
		return;
	}
	CPed_UpdatePosition_fromProcessControl(_this);
}

DECL_HOOK(void, CTaskSimplePlayerOnFoot_PlayerControlZelda, void* _this, sa::CPed* pPed, bool bUnk)
{
	// Allow local player and non-player actors to update m_fAimingRotation from camera;
	// suppress camera-driven rotation for remote players without rewriting .text or global flags.
	if (!pPed || pPed == GamePool_FindPlayerPed() || FindPlayerNumFromPedPtr(pPed) == 0) {
		CTaskSimplePlayerOnFoot_PlayerControlZelda(_this, pPed, bUnk);
		return;
	}
	const float savedAimRot = pPed->m_fAimingRotation;
	CTaskSimplePlayerOnFoot_PlayerControlZelda(_this, pPed, bUnk);
	pPed->m_fAimingRotation = savedAimRot;
}

DECL_HOOK(void, CPed_ProcessControl, sa::CPed* _this)
{
	sa::CPed* prevCurActor = dwCurPlayerActor;
	uint8_t prevCurPlayer = byteCurPlayer;

	dwCurPlayerActor = _this;
	byteInternalPlayer = CurrentPlayerSlot();
	uint8_t curPlayer = FindPlayerNumFromPedPtr(_this);

	if (dwCurPlayerActor && (curPlayer != 0) && byteInternalPlayer == 0) {
		byteCurPlayer = curPlayer;
		// CPad::GetExitTargeting is only called inside local CCamera::CamControl and
		// never runs for remote peds. Clear stale remote targeting state as soon as
		// both KEY_HANDBRAKE and KEY_FIRE are released.
		if (curPlayer < PLAYER_PED_SLOTS
				&& !RemotePlayerKeys[curPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]
				&& !RemotePlayerKeys[curPlayer].bKeys[ePadKeys::KEY_FIRE]) {
			if (_this->m_pPlayerData && _this->m_pPlayerData->m_bFreeAiming) {
				_this->m_pPlayerData->m_bFreeAiming = false;
			}
			if (_this->m_pEntLockOnTarget) {
				eng::fn::CPlayerPed_ClearWeaponTarget(_this);
			}
		}
		{
			ScopedPlayerContext ctx(curPlayer, ScopedPlayerContext::kFull);

			// Suppress CPed::UpdatePosition (via hookBL) and CWidget::SetEnabled (via hook)
			// for remote peds without rewriting .text every frame (nesting-safe RAII guards).
			ScopedAtomicBool posGuard(g_suppressPedUpdatePosition, true);
			ScopedAtomicBool widgetGuard(g_suppressWidgetSetEnabled, true);

			CPed_ProcessControl(_this);
		}
	}
	else {
		CPed_ProcessControl(_this);
	}

	dwCurPlayerActor = prevCurActor;
	byteCurPlayer = prevCurPlayer;
}

static inline int16_t AdjustRemoteWalkAxis(int16_t val, int16_t other, const PAD_KEYS& keys)
{
	if (keys.bKeys[ePadKeys::KEY_WALK]) {
		if (val == -128) return -64;
		if (val == 128) return 64;
	}
	// CTaskSimpleUseGun::SetMoveAnim requires (|lr| + |ud|) / 128.0f >= 0.1f (~13)
	// to play GUNMOVE walk legs; gentle touch-stick tilts can fall just below that.
	if (val != 0 && keys.bKeys[ePadKeys::KEY_HANDBRAKE]) {
		const int absSum = std::abs(static_cast<int>(val)) + std::abs(static_cast<int>(other));
		if (absSum < 16) {
			return static_cast<int16_t>(val < 0 ? -16 : 16);
		}
	}
	return val;
}

DECL_HOOK(int16_t, CPad_GetPedWalkLeftRight, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		const PAD_KEYS& rk = RemotePlayerKeys[byteCurPlayer];
		return AdjustRemoteWalkAxis(static_cast<int16_t>(rk.wKeyLR), static_cast<int16_t>(rk.wKeyUD), rk);
	}
	else {
		g_lastLocalPadPtr = _this;
		if (IsPadMovementDisabled(_this)) {
			LocalPlayerKeys.wKeyLR = 0;
			return 0;
		}
		int16_t lr = g_touchCombat.onFootStickSampled ? g_touchCombat.stickLR : 0;
		if (!g_touchCombat.stickTouched) {
			const int16_t padLR = CPad_GetPedWalkLeftRight(_this);
			if (padLR != 0) {
				lr = padLR;
			}
		}
		if (lr == 0 && pUI && pUI->buttonpanel() && pUI->buttonpanel()->heldLR() != 0) {
			lr = static_cast<int16_t>(pUI->buttonpanel()->heldLR());
		}
		LocalPlayerKeys.wKeyLR = static_cast<uint16_t>(lr);
		return lr;
	}
}
DECL_HOOK(int16_t, CPad_GetPedWalkUpDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		const PAD_KEYS& rk = RemotePlayerKeys[byteCurPlayer];
		return AdjustRemoteWalkAxis(static_cast<int16_t>(rk.wKeyUD), static_cast<int16_t>(rk.wKeyLR), rk);
	}
	else {
		g_lastLocalPadPtr = _this;
		if (IsPadMovementDisabled(_this)) {
			LocalPlayerKeys.wKeyUD = 0;
			return 0;
		}
		int16_t ud = g_touchCombat.onFootStickSampled ? g_touchCombat.stickUD : 0;
		if (!g_touchCombat.stickTouched) {
			const int16_t padUD = CPad_GetPedWalkUpDown(_this);
			if (padUD != 0) {
				ud = padUD;
			}
		}
		if (ud == 0 && pUI && pUI->buttonpanel() && pUI->buttonpanel()->heldUD() != 0) {
			ud = static_cast<int16_t>(pUI->buttonpanel()->heldUD());
		}
		LocalPlayerKeys.wKeyUD = static_cast<uint16_t>(ud);
		return ud;
	}
}

// Fix GTA:SA CTaskSimpleUseGun::StartAnim bug when aiming starts while already
// moving: for non-MOVEFIRE weapons (Rifle 33, Sniper 34, RPG 35/36, Flamethrower 37,
// Minigun 38, Camera 43, or non-Hitman skill) and crouch-moving, StartAnim returns
// early at 0x5dec5c without advancing m_nLastCommand from 0 to 1, causing
// CTaskSimpleUseGun::ProcessPed (0x5deaa0) to run AbortIK every frame instead of AimGun.
DECL_HOOK(void, CTaskSimpleUseGun_StartAnim, uintptr_t _this, sa::CPed* pPed)
{
	CTaskSimpleUseGun_StartAnim(_this, pPed);
	if (!_this) return;

	constexpr uintptr_t kNextCmdOff = VER_x32 ? 0x0F : 0x17;
	constexpr uintptr_t kLastCmdOff = VER_x32 ? 0x10 : 0x18;
	constexpr uintptr_t kAnimPtrOff = VER_x32 ? 0x2C : 0x40;

	const uintptr_t pAnim = *reinterpret_cast<const uintptr_t*>(_this + kAnimPtrOff);
	int8_t& nextCmd = *reinterpret_cast<int8_t*>(_this + kNextCmdOff);
	int8_t& lastCmd = *reinterpret_cast<int8_t*>(_this + kLastCmdOff);
	if (pAnim == 0 && lastCmd == 0 && nextCmd == 1) {
		lastCmd = 1;
		nextCmd = 0;
	}
}

// Enable movement while scoped in 1st-person weapon modes (Sniper=34, RPG=35, HS-RPG=36, Camera=43):
// Stock GTA:SA Mobile zeroes vecMove2D in PlayerControlZeldaWeapon when (weaponInfo->m_nFlags & 0x04)
// and !IsPlayerUse2PlayerControls, locking local movement and causing remote scoped players to slide.
DECL_HOOK(void, CTaskSimplePlayerOnFoot_PlayerControlZeldaWeapon, void* _this, sa::CPed* pPed)
{
	const bool bIsRemote = pPed && pPed != GamePool_FindPlayerPed() && FindPlayerNumFromPedPtr(pPed) != 0;
	const float savedAimRot = bIsRemote ? pPed->m_fAimingRotation : 0.0f;

	CTaskSimplePlayerOnFoot_PlayerControlZeldaWeapon(_this, pPed);

	if (bIsRemote) {
		pPed->m_fAimingRotation = savedAimRot;
	}

	if (!pPed || !pPed->m_pIntelligence || pPed->m_pAttachedTo) {
		return;
	}

	uintptr_t taskUseGun = eng::fn::CPedIntelligence_GetTaskUseGun(pPed->m_pIntelligence);
	if (!taskUseGun) {
		return;
	}

	constexpr uintptr_t kTaskUseGunWeaponInfoOff = VER_x32 ? 0x30 : 0x48;
	const uintptr_t weaponInfo = *reinterpret_cast<const uintptr_t*>(taskUseGun + kTaskUseGunWeaponInfoOff);
	if (!weaponInfo) {
		return;
	}

	const uint8_t weapFlags0 = *reinterpret_cast<const uint8_t*>(weaponInfo + 0x18);
	if ((weapFlags0 & 0x04u) == 0) {
		return;
	}

	int16_t lr = 0;
	int16_t ud = 0;
	if (CurrentPlayerSlot() != 0) {
		const PAD_KEYS& rk = RemotePlayerKeys[byteCurPlayer];
		lr = AdjustRemoteWalkAxis(static_cast<int16_t>(rk.wKeyLR), static_cast<int16_t>(rk.wKeyUD), rk);
		ud = AdjustRemoteWalkAxis(static_cast<int16_t>(rk.wKeyUD), static_cast<int16_t>(rk.wKeyLR), rk);
	}
	else if (!IsLocalActivePedMovementLocked()) {
		lr = static_cast<int16_t>(LocalPlayerKeys.wKeyLR);
		ud = static_cast<int16_t>(LocalPlayerKeys.wKeyUD);
	}

	sa::CVector2D move{static_cast<float>(lr) * (1.0f / 128.0f), static_cast<float>(ud) * (1.0f / 128.0f)};
	const float magSq = move.x * move.x + move.y * move.y;
	if (magSq > 1.0f) {
		const float invMag = 1.0f / std::sqrt(magSq);
		move.x *= invMag;
		move.y *= invMag;
	}
	eng::fn::CTaskSimpleUseGun_ControlGunMove(taskUseGun, &move);
}

DECL_HOOK(uint16_t, CPad_JumpJustDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		if (!RemotePlayerKeys[byteCurPlayer].bIgnoreJump && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP] && !RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]) {
			RemotePlayerKeys[byteCurPlayer].bIgnoreJump = true;
			return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
		}

		return 0;
	}
	else {
		if (CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr) {
			if (!pLocal->IsInVehicle() && pLocal->IsCarry()) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = false;
				return 0;
			}
		}
		const uint16_t jumpDown = CPad_JumpJustDown(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = (jumpDown != 0) || IsLocalMeleeBlockActive();
		return jumpDown;
	}
}
DECL_HOOK(uint16_t, CPad_GetJump, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		if (RemotePlayerKeys[byteCurPlayer].bIgnoreJump || RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]) {
			return 0;
		}
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
	}
	else {
		if (CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr) {
			if (!pLocal->IsInVehicle() && pLocal->IsCarry()) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = false;
				return 0;
			}
		}
		const uint16_t jumpHeld = CPad_GetJump(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = (jumpHeld != 0) || IsLocalMeleeBlockActive();
		return jumpHeld;
	}
}

DECL_HOOK(uint16_t, CPad_GetDuck, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		if (CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr) {
			if (!pLocal->IsInVehicle() && pLocal->IsCarry()) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = false;
				return 0;
			}
		}
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = CPad_GetDuck(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH];
	}
}

DECL_HOOK(uint16_t, CPad_MeleeAttackJustDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		if (RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE] && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SECONDARY_ATTACK]) {
			return 2;
		}

		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE];
	}
	else {
		uint32_t dwResult = CPad_MeleeAttackJustDown(_this);
		if (dwResult == 0 && GetBlockableLocalPed() != nullptr
				&& LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE]
				&& LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK]) {
			dwResult = 2;
		}
		if (dwResult == 2) {
			g_touchCombat.meleeComboUntilMs = GetTickCount() + 130;
			LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = true;
			LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = true;
			return 2;
		}
		if (dwResult == 1) {
			g_touchCombat.meleeFireUntilMs = GetTickCount() + 100;
			LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = true;
			return 1;
		}
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = IsLocalMeleeFireActive();
		return dwResult;
	}
}

DECL_HOOK(uint16_t, CPad_DuckJustDown, uintptr_t _this, sa::CPed* pPed)
{
	if (const uint8_t slot = CurrentPlayerSlot(); slot > 0 && slot < PLAYER_PED_SLOTS) {
		// Remote stand-up is controlled by SPECIAL_ACTION_DUCK clearing via ResetCrouch();
		// suppress DuckJustDown while pPed is already ducking so GTA doesn't toggle crouch back off.
		if (pPed && pPed->bIsDucking) {
			RemotePlayerKeys[slot].bIgnoreDuck = true;
			return 0;
		}
		if (!RemotePlayerKeys[slot].bIgnoreDuck && RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_CROUCH]) {
			RemotePlayerKeys[slot].bIgnoreDuck = true;
			return 1;
		}
		return 0;
	}
	else {
		if (CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr) {
			if (!pLocal->IsInVehicle() && pLocal->IsCarry()) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = false;
				return 0;
			}
		}
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = CPad_DuckJustDown(_this, pPed);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH];
	}
}

DECL_HOOK(bool, CPad_GetTarget, uintptr_t _this, bool bCheckVehicle)
{
	if (const uint8_t slot = CurrentPlayerSlot(); slot > 0 && slot < PLAYER_PED_SLOTS) {
		return RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_HANDBRAKE];
	}
	if (IsLocalMeleeComboActive()) {
		return true;
	}
	return CPad_GetTarget(_this, bCheckVehicle);
}

DECL_HOOK(uint32_t, CPad_GetSprint, uintptr_t _this, uint32_t unk)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SPRINT];
	}
	else {
		if (CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr) {
			if (!pLocal->IsInVehicle() && (pLocal->IsCuffed() || pLocal->IsCarry())) {
				LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = false;
				return 0;
			}
		}
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = CPad_GetSprint(_this, unk);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT];
	}
}

DECL_HOOK(uint32_t, CPad_GetAutoClimb, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		if (RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]) {
			return 0;
		}
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
	}
	else {
		const uint32_t climb = CPad_GetAutoClimb(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = (climb != 0) || IsLocalMeleeBlockActive();
		return climb;
	}
}

DECL_HOOK(uint32_t, CPad_GetAbortClimb, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SECONDARY_ATTACK];
	}
	else {
		const uint32_t abortClimb = CPad_GetAbortClimb(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] =
			(abortClimb != 0) || IsLocalMeleeComboActive() || LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK];
		return abortClimb;
	}
}

DECL_HOOK(uint32_t, CPad_DiveJustDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = CPad_DiveJustDown(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE];
	}
}

DECL_HOOK(uint32_t, CPad_SwimJumpJustDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = CPad_SwimJumpJustDown(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP];
	}
}

// CTaskSimplePlayerOnFoot::PlayerControlFighter accumulates m_nTimer when
// ped->m_pTargetedObject == nullptr and CHID::GetInputType() != 2, forcing
// QUIT_FIGHTING (cmd 15) at >1999ms (~2s). Reset that idle fighter timeout
// whenever either the local player or a remote player is actively holding block.
DECL_HOOK(void, CTaskSimplePlayerOnFoot_PlayerControlFighter, uintptr_t _this, sa::CPed* pPed)
{
	if (_this && pPed) {
		const uint8_t slot = CurrentPlayerSlot();
		if (slot == 0) {
			if (pPed == GamePool_FindPlayerPed() && g_touchCombat.blockDown) {
				*reinterpret_cast<uint32_t*>(_this + kTaskPlayerOnFootTimerOffset) = 0;
			}
		}
		else if (slot < PLAYER_PED_SLOTS
				&& RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_JUMP]
				&& RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_HANDBRAKE]) {
			*reinterpret_cast<uint32_t*>(_this + kTaskPlayerOnFootTimerOffset) = 0;
		}
	}
	CTaskSimplePlayerOnFoot_PlayerControlFighter(_this, pPed);
}

DECL_HOOK(uint32_t, CPad_GetBlock, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return (RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP] && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]);
	}
	if (!_this || *reinterpret_cast<uint16_t*>(_this + 272) != 0) {
		return 0;
	}
	if (GetBlockableLocalPed()) {
		const bool bBlocking = g_touchCombat.blockDown
			|| (LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] && LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE]);
		return bBlocking ? 1u : 0u;
	}
	return 0;
}

DECL_HOOK(int16_t, CPad_GetSteeringLeftRight, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return static_cast<int16_t>(RemotePlayerKeys[byteCurPlayer].wKeyLR);
	}
	else {
		g_lastLocalPadPtr = _this;
		if (IsPadMovementDisabled(_this)) {
			LocalPlayerKeys.wKeyLR = 0;
			return 0;
		}
		int16_t lr = CPad_GetSteeringLeftRight(_this);
		if (lr == 0 && pUI && pUI->buttonpanel() && pUI->buttonpanel()->heldLR() != 0) {
			lr = static_cast<int16_t>(pUI->buttonpanel()->heldLR());
		}
		LocalPlayerKeys.wKeyLR = static_cast<uint16_t>(lr);
		return lr;
	}
}

DECL_HOOK(int16_t, CPad_GetSteeringUpDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return static_cast<int16_t>(RemotePlayerKeys[byteCurPlayer].wKeyUD);
	}
	else {
		g_lastLocalPadPtr = _this;
		if (IsPadMovementDisabled(_this)) {
			LocalPlayerKeys.wKeyUD = 0;
			return 0;
		}
		int16_t ud = CPad_GetSteeringUpDown(_this);
		if (ud == 0 && pUI && pUI->buttonpanel() && pUI->buttonpanel()->heldUD() != 0) {
			ud = static_cast<int16_t>(pUI->buttonpanel()->heldUD());
		}
		LocalPlayerKeys.wKeyUD = static_cast<uint16_t>(ud);
		return ud;
	}
}

DECL_HOOK(uint16_t, CPad_GetAccelerate, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SPRINT] ? 0xFF : 0x00;
	}
	else {
		uint16_t wResult = CPad_GetAccelerate(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = wResult;
		sa::CPed* local = GamePool_FindPlayerPed();
		sa::CVehicle* pVehicle = local ? local->pVehicle : nullptr;
		if (pVehicle && pVehicle->pDriver == local && !pVehicle->m_nVehicleFlags.bEngineOn) {
			return 0;
		}
		return wResult;
	}
}

DECL_HOOK(uint16_t, CPad_GetBrake, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP] ? 0xFF : 0x00;
	}
	else {
		uint16_t wResult = CPad_GetBrake(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = wResult;
		sa::CPed* local = GamePool_FindPlayerPed();
		sa::CVehicle* pVehicle = local ? local->pVehicle : nullptr;
		if (pVehicle && pVehicle->pDriver == local && !pVehicle->m_nVehicleFlags.bEngineOn) {
			return 0;
		}
		return wResult;
	}
}

DECL_HOOK(uint32_t, CPad_GetHandBrake, uintptr_t _this)
{
	if (const uint8_t slot = CurrentPlayerSlot(); slot > 0 && slot < PLAYER_PED_SLOTS) {
		return RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_HANDBRAKE] ? 0xFF : 0x00;
	}
	else {
		uint32_t dwResult = CPad_GetHandBrake(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = dwResult;
		return dwResult;
	}
}

DECL_HOOK(uint32_t, CPad_GetHorn, uintptr_t _this, bool bCheckVehicle)
{
	if (const uint8_t slot = CurrentPlayerSlot(); slot > 0 && slot < PLAYER_PED_SLOTS) {
		return RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		uint32_t dwResult = CPad_GetHorn(_this, bCheckVehicle);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = dwResult;
		return dwResult;
	}
}

DECL_HOOK(uint32_t, CPad_ExitVehicleJustDown, uintptr_t _this, int a2, uintptr_t vehicle, int a4, uintptr_t vec)
{
	static uint32_t dwPassengerEnterExit = 0;
	if (dwPassengerEnterExit != 0 && GetTickCount() - dwPassengerEnterExit < 500) {
		return 0;
	}

	if (CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr) {
		if (!pLocal->IsInVehicle() && (pLocal->IsCuffed() || pLocal->IsCarry())) {
			return 0;
		}
	}

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
			if (pLocalPlayer) {
				if (pLocalPlayer->HandlePassengerEntry()) {
					dwPassengerEnterExit = GetTickCount();
					return 0;
				}
			}
		}
	}

	return CPad_ExitVehicleJustDown(_this, a2, vehicle, a4, vec);
}

DECL_HOOK(int, CPad_GetTurretLeft, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_LOOK_LEFT];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_LEFT] = CPad_GetTurretLeft(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_LEFT];
	}
}

DECL_HOOK(int, CPad_GetTurretRight, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_LOOK_RIGHT];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_RIGHT] = CPad_GetTurretRight(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_RIGHT];
	}
}

DECL_HOOK(int, CPad_GetNitroFired, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = CPad_GetNitroFired(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE];
	}
}

DECL_HOOK(int, CPad_GetHydraulicJump, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = CPad_GetHydraulicJump(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH];
	}
}

static inline void ApplyVehicleTyreBurstFix(sa::CVehicle* pVehicle)
{
	if (pVehicle->pDriver) {
		if (pVehicle->m_nVehicleFlags.bTyresDontBurst) {
			pVehicle->m_nVehicleFlags.bTyresDontBurst = 0;
		}
		if (!pVehicle->m_nVehicleFlags.bCanBeDamaged) {
			pVehicle->m_nVehicleFlags.bCanBeDamaged = true;
		}
	}
	else {
		if (!pVehicle->m_nVehicleFlags.bTyresDontBurst) {
			pVehicle->m_nVehicleFlags.bTyresDontBurst = 1;
		}
		if (pVehicle->m_nVehicleFlags.bCanBeDamaged) {
			pVehicle->m_nVehicleFlags.bCanBeDamaged = false;
		}
	}
}

template <bool CallAudioService, typename OrigFn>
static void ProcessRemoteVehicleControl(OrigFn orig, sa::CVehicle* pVehicle)
{
	const uint8_t prevCurPlayer = byteCurPlayer;
	byteInternalPlayer = CurrentPlayerSlot();

	CVehicle* pSampVeh = nullptr;
	if (pVehicle && pNetGame && pNetGame->GetVehiclePool()) {
		VEHICLEID vid = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(pVehicle);
		if (vid != INVALID_VEHICLE_ID) {
			pSampVeh = pNetGame->GetVehiclePool()->GetAt(vid);
		}
	}

	const bool bManualEngineOff = pSampVeh && (
		pSampVeh->m_iEngineState == 0 ||
		(pNetGame && pNetGame->m_netSet && pNetGame->m_netSet->manualVehicleEngineAndLight && pSampVeh->m_iEngineState < 0));
	const bool bIsHeli = pSampVeh && (pSampVeh->GetVehicleSubtype() == VEHICLE_SUBTYPE_HELI);

	const uint8_t curPlayer = (pVehicle && pVehicle->pDriver && pVehicle->pDriver->m_nPedType == 0
		&& pVehicle->pDriver != GamePool_FindPlayerPed() && byteInternalPlayer == 0)
		? FindPlayerNumFromPedPtr(pVehicle->pDriver)
		: 0;

	if (curPlayer != 0) {
		byteCurPlayer = curPlayer;

		ScopedPlayerContext camAimCtx(curPlayer, ScopedPlayerContext::kVehicleCamAim);

		// Tyre burst fix
		ApplyVehicleTyreBurstFix(pVehicle);

		pVehicle->pDriver->m_nPedType = sa::PEDTYPE_CIVMALE; // So CPed::IsPlayer returns FALSE
		const uint8_t savedControlFlags = pVehicle->nControlFlags;
		pVehicle->nControlFlags = 26;

		if constexpr (CallAudioService) {
			eng::fn::CAEVehicleAudioEntity_Service(&pVehicle->m_VehicleAudioEntity);
		}

		pVehicle->nControlFlags = savedControlFlags;
		pVehicle->pDriver->m_nPedType = sa::PEDTYPE_PLAYER1;

		// Preserve Z position & velocity for boats/aircraft and distant non-train vehicles
		// so unstreamed collision geometry cannot drop remote vehicles through the map.
		const float savedZ = pVehicle->GetMatrix().m_pos.z;
		const float savedVelZ = pVehicle->GetMoveSpeed().z;
		bool bLockZ = false;
		if (pSampVeh) {
			const uint8_t sub = pSampVeh->GetVehicleSubtype();
			bLockZ = (sub == VEHICLE_SUBTYPE_BOAT || sub == VEHICLE_SUBTYPE_PLANE || sub == VEHICLE_SUBTYPE_HELI)
				|| (!pSampVeh->IsATrainPart() && pSampVeh->GetDistanceFromLocalPlayerPed() > 160.0f);
		}

		if (bManualEngineOff) {
			pVehicle->m_nVehicleFlags.bEngineOn = 0;
		}
		if (bIsHeli) {
			if (!pVehicle->m_nVehicleFlags.bEngineOn) pVehicle->nControlFlags |= 0x20;
			else pVehicle->nControlFlags &= ~0x20;
		}
		{
			ScopedPlayerContext focusCtx(curPlayer, ScopedPlayerContext::kFocusOnly);
			orig(pVehicle);
		}
		if (bManualEngineOff) {
			pVehicle->m_nVehicleFlags.bEngineOn = 0;
		}
		if (bIsHeli) {
			if (!pVehicle->m_nVehicleFlags.bEngineOn) pVehicle->nControlFlags |= 0x20;
			else pVehicle->nControlFlags &= ~0x20;
		}
		if (bLockZ) {
			sa::CMatrix mat = pVehicle->GetMatrix();
			mat.m_pos.z = savedZ;
			pVehicle->SetMatrix(mat);
			sa::CVector vel = pVehicle->GetMoveSpeed();
			vel.z = savedVelZ;
			pVehicle->SetVelocity(vel);
		}
	}
	else {
		if (pVehicle && !pVehicle->pDriver && !pVehicle->m_pTowingVehicle
			&& pSampVeh && !pSampVeh->IsATrainPart()
			&& pSampVeh->GetDistanceFromLocalPlayerPed() > 160.0f) {
			// Skip physics for distant unmanned vehicles outside loaded collision sector
			byteCurPlayer = prevCurPlayer;
			return;
		}

		if (pVehicle && pVehicle->pDriver && pVehicle->pDriver->m_nPedType == 0
			&& GamePool_FindPlayerPed() == pVehicle->pDriver) {
			ApplyVehicleTyreBurstFix(pVehicle);
		}

		if (bManualEngineOff && pVehicle) {
			pVehicle->m_nVehicleFlags.bEngineOn = 0;
		}
		if (bIsHeli && pVehicle) {
			if (!pVehicle->m_nVehicleFlags.bEngineOn) pVehicle->nControlFlags |= 0x20;
			else pVehicle->nControlFlags &= ~0x20;
		}

		if constexpr (CallAudioService) {
			eng::fn::CAEVehicleAudioEntity_Service(&pVehicle->m_VehicleAudioEntity);
		}

		orig(pVehicle);

		if (bManualEngineOff && pVehicle) {
			pVehicle->m_nVehicleFlags.bEngineOn = 0;
		}
		if (bIsHeli && pVehicle) {
			if (!pVehicle->m_nVehicleFlags.bEngineOn) pVehicle->nControlFlags |= 0x20;
			else pVehicle->nControlFlags &= ~0x20;
		}
	}

	byteCurPlayer = prevCurPlayer;
}

DECL_HOOK(void, CAutomobile_ProcessControl, sa::CVehicle* _this)   { ProcessRemoteVehicleControl<true>(CAutomobile_ProcessControl, _this); }
DECL_HOOK(void, CBoat_ProcessControl, sa::CVehicle* _this)         { ProcessRemoteVehicleControl<true>(CBoat_ProcessControl, _this); }
DECL_HOOK(void, CBike_ProcessControl, sa::CVehicle* _this)
{
	CVehicle* pSampVeh = nullptr;
	if (pNetGame && pNetGame->GetVehiclePool()) {
		VEHICLEID vid = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(_this);
		if (vid != INVALID_VEHICLE_ID) {
			pSampVeh = pNetGame->GetVehiclePool()->GetAt(vid);
		}
	}

	ProcessRemoteVehicleControl<true>(CBike_ProcessControl, _this);

	// T20: hold remote bike lean angle per-frame so CBike::ProcessControl does not
	// decay RideAnimData.m_fLeanAngle back toward 0 between sync packets.
	if (pSampVeh && pSampVeh->VerifyInstance()) {
		if (pSampVeh->IsDriverLocalPlayer()) {
			pSampVeh->m_bHasRemoteBikeLean = false;
			pSampVeh->m_fRemoteBikeLean = 0.0f;
		}
		else if (pSampVeh->m_bHasRemoteBikeLean) {
			if (!pSampVeh->HasADriver()) {
				pSampVeh->m_bHasRemoteBikeLean = false;
				pSampVeh->m_fRemoteBikeLean = 0.0f;
			}
			else {
				pSampVeh->SetBikeLean(pSampVeh->m_fRemoteBikeLean);
			}
		}
	}
}
DECL_HOOK(void, CHeli_ProcessControl, sa::CVehicle* _this)         { ProcessRemoteVehicleControl<false>(CHeli_ProcessControl, _this); }
static std::atomic<bool> g_bPlaneGearToggleRequested{false};
static std::atomic<bool> g_bStuffUseRequested{false};
static std::atomic<bool> g_bPassengerDriveByToggleRequested{false};

void RequestLocalPlaneGearToggle()
{
	g_bPlaneGearToggleRequested.store(true, std::memory_order_release);
}

bool ConsumeStuffUseRequest()
{
	return g_bStuffUseRequested.exchange(false, std::memory_order_acq_rel);
}

bool ConsumePassengerDriveByToggle()
{
	return g_bPassengerDriveByToggleRequested.exchange(false, std::memory_order_acq_rel);
}

DECL_HOOK(void, CPlane_ProcessControl, sa::CVehicle* _this)
{
	CVehicle* pSampVeh = nullptr;
	if (pNetGame && pNetGame->GetVehiclePool()) {
		VEHICLEID vid = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(_this);
		if (vid != INVALID_VEHICLE_ID) {
			pSampVeh = pNetGame->GetVehiclePool()->GetAt(vid);
		}
	}

	// Sample logical gear state BEFORE CPlane::ProcessControlInputs runs, because
	// GTA Mobile's 30m auto-gear mutates m_fLGearAngle mid-frame (e.g. 0.0 -> 0.02).
	uint8_t prevLocalGear = 0;
	if (pSampVeh && pSampVeh->VerifyInstance() && pSampVeh->IsDriverLocalPlayer()) {
		prevLocalGear = (pSampVeh->m_iLocalLandingGear >= 0)
				? static_cast<uint8_t>(pSampVeh->m_iLocalLandingGear)
				: pSampVeh->GetLandingGearState();
	}

	ProcessRemoteVehicleControl<false>(CPlane_ProcessControl, _this);

	if (pSampVeh && pSampVeh->VerifyInstance()) {
		auto* pPlane = reinterpret_cast<sa::CPlane*>(_this);
		if (pSampVeh->IsDriverLocalPlayer()) {
			pSampVeh->m_iRemoteLandingGear = -1;
			if (g_bPlaneGearToggleRequested.exchange(false, std::memory_order_acq_rel)) {
				const uint8_t next = (prevLocalGear == 0) ? 1 : 0;
				pSampVeh->m_iLocalLandingGear = static_cast<int8_t>(next);
				pSampVeh->SetLandingGearState(next);
			}
			else if (pSampVeh->m_iLocalLandingGear >= 0) {
				// Preserve manual gear choice against GTA Mobile's 30m auto-gear logic in CPlane::ProcessControlInputs.
				const float target = (pSampVeh->m_iLocalLandingGear == 1) ? 1.0f : 0.0f;
				if (pPlane->m_fLGearAngle != target) {
					pSampVeh->SetLandingGearState(static_cast<uint8_t>(pSampVeh->m_iLocalLandingGear));
				}
			}
		}
		else {
			pSampVeh->m_iLocalLandingGear = -1;
			if (pSampVeh->m_iRemoteLandingGear >= 0) {
				if (!pSampVeh->HasADriver()) {
					pSampVeh->m_iRemoteLandingGear = -1;
				}
				else {
					const float target = (pSampVeh->m_iRemoteLandingGear == 1) ? 1.0f : 0.0f;
					if (pPlane->m_fLGearAngle != target) {
						pSampVeh->SetLandingGearState(static_cast<uint8_t>(pSampVeh->m_iRemoteLandingGear));
					}
				}
			}
		}
	}
}
DECL_HOOK(void, CBmx_ProcessControl, sa::CVehicle* _this)          { ProcessRemoteVehicleControl<false>(CBmx_ProcessControl, _this); }
DECL_HOOK(void, CMonsterTruck_ProcessControl, sa::CVehicle* _this) { ProcessRemoteVehicleControl<false>(CMonsterTruck_ProcessControl, _this); }
DECL_HOOK(void, CQuadBike_ProcessControl, sa::CVehicle* _this)     { ProcessRemoteVehicleControl<false>(CQuadBike_ProcessControl, _this); }
DECL_HOOK(void, CTrain_ProcessControl, sa::CVehicle* _this)        { ProcessRemoteVehicleControl<true>(CTrain_ProcessControl, _this); }
DECL_HOOK(void, CTrailer_ProcessControl, sa::CVehicle* _this)      { ProcessRemoteVehicleControl<false>(CTrailer_ProcessControl, _this); }

static bool g_bCancelCarTask = false;

bool NotifyEnterVehicle(sa::CVehicle* _pVehicle)
{
	spdlog::info("NotifyEnterVehicle: 0x{:X}", reinterpret_cast<uintptr_t>(_pVehicle));

	if (!pNetGame || !_pVehicle) {
		return false;
	}

	VEHICLEID vehicleId = INVALID_VEHICLE_ID;

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return false;
	}

	vehicleId = pVehiclePool->FindIDFromGtaPtr(_pVehicle);
	if (vehicleId == INVALID_VEHICLE_ID || !pVehiclePool->GetSlotState(vehicleId)) {
		return false;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
	if (!pLocalPlayer || !pLocalPlayer->GetPlayerPed()) {
		return false;
	}

	if (pLocalPlayer->GetPlayerPed()->IsCuffed() || pLocalPlayer->GetPlayerPed()->IsCarry()) {
		return false;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(vehicleId);
	if (pVehicle && pVehicle->m_pVehicle) {
		if (pVehicle->GetDoorState()) {
			return false;
		}

		if (pVehicle->m_pVehicle->m_nModelIndex == TRAIN_PASSENGER) {
			return false;
		}

		if (pVehicle->m_pVehicle->pDriver) {
			if (pVehicle->m_pVehicle->pDriver->m_nPedType != 0) {
				return false;
			}
			PLAYERID driverId = pPlayerPool->FindRemotePlayerIDFromGtaPtr(pVehicle->m_pVehicle->pDriver);
			if (driverId != INVALID_PLAYER_ID) {
				CRemotePlayer* pDriver = pPlayerPool->GetAt(driverId);
				if (pDriver && pDriver->IsNPC()) {
					return false;
				}
			}
		}
	}

	if (pLocalPlayer->GetPlayerPed()->GetCurrentWeapon() == WEAPON_PARACHUTE) {
		pLocalPlayer->GetPlayerPed()->SetArmedWeapon(0);
	}

	pLocalPlayer->SendEnterVehicleNotification(vehicleId, false);
	return true;
}

DECL_HOOK(void, CTaskComplexEnterCarAsDriver, uintptr_t** _this, sa::CVehicle* pVehicle)
{
	if (!NotifyEnterVehicle(pVehicle)) {
		g_bCancelCarTask = true;
	}
	// Must always invoke constructor because CPlayerInfo::Process immediately passes _this to SetTask
	CTaskComplexEnterCarAsDriver(_this, pVehicle);
}

DECL_HOOK(void, CTaskComplexLeaveCar, uintptr_t** _this, sa::CVehicle* pVehicle, int iTargetDoor, int iDelayTime, bool bSensibleLeaveCar, bool bForceGetOut)
{
	if (pNetGame && pVehicle) {
		sa::CPed* pLocalPed = GamePool_FindPlayerPed();
		if (pLocalPed && pLocalPed->pVehicle == pVehicle) {
			CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
			CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
			if (pVehiclePool && pPlayerPool) {
				VEHICLEID VehicleID = pVehiclePool->FindIDFromGtaPtr(pLocalPed->pVehicle);
				if (VehicleID != INVALID_VEHICLE_ID) {
					CVehicle* pVeh = pVehiclePool->GetAt(VehicleID);
					CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
					if (pVeh && pVeh->m_pVehicle && pLocalPlayer) {
						pLocalPlayer->SendExitVehicleNotification(VehicleID);
						if (pVeh->IsATrainPart()) {
							RwMatrix mat = pVeh->m_pVehicle->GetMatrix().ToRwMatrix();
							if (pLocalPlayer->GetPlayerPed()) {
								pLocalPlayer->GetPlayerPed()->RemoveFromVehicleAndPutAt(mat.pos.x + 2.5f, mat.pos.y + 2.5f, mat.pos.z);
							}
							if (pGame && pGame->GetCamera()) {
								pGame->GetCamera()->SetBehindPlayer();
							}
							pLocalPlayer->m_LastVehicle = VehicleID;
							g_bCancelCarTask = true;
						}
					}
				}
			}
		}
	}

	CTaskComplexLeaveCar(_this, pVehicle, iTargetDoor, iDelayTime, bSensibleLeaveCar, bForceGetOut);
}

DECL_HOOK(uint32_t, CTaskSimpleUseGun_SetPedPosition, uintptr_t _this, sa::CPed* ped)
{
	sa::CPed* prevCurActor = dwCurPlayerActor;
	const uint8_t prevCurPlayer = byteCurPlayer;

	byteInternalPlayer = CurrentPlayerSlot();
	const uint8_t curPlayer = ped ? FindPlayerNumFromPedPtr(ped) : 0;

	if (ped && (curPlayer != 0) && byteInternalPlayer == 0) // remote player
	{
		dwCurPlayerActor = ped;
		byteCurPlayer = curPlayer;
		ScopedPlayerContext ctx(curPlayer, ScopedPlayerContext::kFull);
		CTaskSimpleUseGun_SetPedPosition(_this, ped);
	}
	else // localplayer
	{
		CTaskSimpleUseGun_SetPedPosition(_this, ped);
	}

	dwCurPlayerActor = prevCurActor;
	byteCurPlayer = prevCurPlayer;
	return 0;
}

// Replicates CPad::GetWeapon's top-level safety gates: pad controls disabled
// (+272), script fire disable (+303), ped alive/on-foot, and not entering a
// vehicle (tasks 700/701).
static bool CanLocalPedUseOnFootFire(uintptr_t pad, sa::CPed* pPed)
{
	if (!pad || !pPed || !pPed->m_pPlayerData) return false;
	if (*reinterpret_cast<uint16_t*>(pad + 272) != 0) return false;
	if (*reinterpret_cast<uint8_t*>(pad + 303) != 0) return false;

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || pLocal->m_ped != pPed || pLocal->IsDead()
		|| pLocal->IsInVehicle() || pLocal->IsInJetpack()) {
		return false;
	}

	if (pPed->m_pIntelligence) {
		void* pTaskMgr = &pPed->m_pIntelligence->m_TaskMgr;
		if (eng::fn::CTaskManager_FindActiveTaskByType( pTaskMgr, 700)
			|| eng::fn::CTaskManager_FindActiveTaskByType( pTaskMgr, 701)) {
			return false;
		}
	}
	return true;
}

// Weapons that natively require continuous hold (throwables & continuous spray)
static bool WeaponUsesHoldFire(uint8_t type)
{
	switch (type) {
		case sa::eWeaponType::WEAPONTYPE_GRENADE:
		case sa::eWeaponType::WEAPONTYPE_TEARGAS:
		case sa::eWeaponType::WEAPONTYPE_MOLOTOV:
		case sa::eWeaponType::WEAPONTYPE_FLAMETHROWER:
		case sa::eWeaponType::WEAPONTYPE_SPRAYCAN:
		case sa::eWeaponType::WEAPONTYPE_EXTINGUISHER:
			return true;
	}
	return false;
}

// Single-shot / scoped / trigger-only weapons: fire strictly on press edge
static bool WeaponUsesEdgeOnlyFire(uint8_t type)
{
	switch (type) {
		case sa::eWeaponType::WEAPONTYPE_SNIPERRIFLE:
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER:
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER_HS:
		case sa::eWeaponType::WEAPONTYPE_DETONATOR:
		case sa::eWeaponType::WEAPONTYPE_CAMERA:
		case sa::eWeaponType::WEAPONTYPE_NIGHTVISION:
		case sa::eWeaponType::WEAPONTYPE_INFRARED:
			return true;
	}
	return false;
}

static bool EvaluateLocalTouchFireLevel(uint8_t weaponType)
{
	if (!g_touchCombat.fireDown) {
		return false;
	}
	if (WeaponUsesHoldFire(weaponType)) {
		return true;
	}
	if (WeaponUsesEdgeOnlyFire(weaponType)) {
		return g_touchCombat.fireJustPressed;
	}
	// Regular firearms: immediate shot on press edge; sustained hold enters
	// continuous fire only after kFireHoldRepeatDelayMs so a tap never bursts.
	if (g_touchCombat.fireJustPressed) {
		return true;
	}
	return (GetTickCount() - g_touchCombat.fireDownTickMs) >= kFireHoldRepeatDelayMs;
}

DECL_HOOK(uint32_t, CPad_GetWeapon, uintptr_t _this, sa::CPed* pPed, bool bShoulderPossible)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE] ? 0xFF : 0x00;
	}

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || pLocal->IsInJetpack()) {
		uint32_t dwResult = CPad_GetWeapon(_this, pPed, bShoulderPossible);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = dwResult;
		return dwResult;
	}

	if (pLocal->IsInVehicle()) {
		if (pLocal->IsAPassenger() && pLocal->IsInPassengerDriveByMode()) {
			const uint32_t orig = CPad_GetWeapon(_this, pPed, bShoulderPossible);
			const bool bFire = g_touchCombat.fireDown || LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] || (orig != 0);
			LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = bFire;
			return bFire ? 0xFF : 0x00;
		}
		uint32_t dwResult = CPad_GetWeapon(_this, pPed, bShoulderPossible);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = dwResult;
		return dwResult;
	}

	uint32_t dwResult = 0;
	if (CanLocalPedUseOnFootFire(_this, pPed)) {
		dwResult = EvaluateLocalTouchFireLevel(pLocal->GetCurrentWeapon()) ? 0xFF : 0x00;
	}
	LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = (dwResult != 0) || IsLocalMeleeFireActive();
	return dwResult;
}

// Mobile libGTASA bug fix: CPlayerInfo::Process() probes whether the player
// can enter a nearby vehicle EVERY frame by calling
// activeTask->MakeAbortable(ped, ABORT_PRIORITY_URGENT=1, nullptr) BEFORE
// checking ExitVehicleJustDown(). For CTaskSimplePlayerOnFoot, MakeAbortable
// unconditionally aborts secondary task 0 (CTaskSimpleUseGun), which:
//   1) kills hipfire every frame near vehicles (stuck in raise-gun frame 1);
//   2) destroys & recreates CTaskSimpleUseGun between aim-tap shots, skipping
//      the weapon's post-shot cooldown animation (~160ms instead of ~720ms).
// Scope-guard CPlayerInfo::Process and suppress its speculative probe abort
// unless the player actually pressed/released the enter-car widget.
static bool s_bInPlayerInfoProcess = false;

DECL_HOOK(void, CPlayerInfo_Process, uintptr_t _this, int playerNumber)
{
	bool prev = s_bInPlayerInfoProcess;
	if (playerNumber == 0) {
		s_bInPlayerInfoProcess = true;
	}
	CPlayerInfo_Process(_this, playerNumber);
	s_bInPlayerInfoProcess = prev;

	if (playerNumber == 0 && g_bCancelCarTask) {
		g_bCancelCarTask = false;
		CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
		if (pLocal && pLocal->m_ped && pLocal->m_ped->m_pIntelligence && pLocal->m_gtaId) {
			sa::CTask* pPrimary = pLocal->m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];
			const int taskType = pPrimary ? GetTaskTypeFromTask(&pPrimary) : -1;
			// Only clear if GTA actually installed an enter/leave car task (700/704),
			// avoiding wiping unrelated scripted or Carry/Dance animations.
			if (taskType == 700 || taskType == 704 || taskType == 711) {
				ScriptCommand(&clear_char_tasks, pLocal->m_gtaId);
			}
		}
	}
}

DECL_HOOK(uint32_t, CTaskSimplePlayerOnFoot_MakeAbortable, uintptr_t _this, sa::CPed* pPed, int priority, uintptr_t pEvent)
{
	if (s_bInPlayerInfoProcess && !CurrentPlayerSlot() && priority == 1 && pEvent == 0
		&& pPed && pPed == GamePool_FindPlayerPed() && pPed->m_pIntelligence) {
		uintptr_t useGun = eng::fn::CPedIntelligence_GetTaskUseGun( pPed->m_pIntelligence);
		if (useGun) {
			bool bEnterCarInput = false;
			uintptr_t* widgets = GetTouchWidgets();
			if (widgets && widgets[sa::WidgetIDs::WIDGET_ENTER_CAR]) {
				bEnterCarInput =
					eng::fn::CTouchInterface_IsTouched(
						sa::WidgetIDs::WIDGET_ENTER_CAR, static_cast<void*>(nullptr), 0) != 0
					|| eng::fn::CTouchInterface_IsReleased(
						sa::WidgetIDs::WIDGET_ENTER_CAR, static_cast<void*>(nullptr), 0) != 0;
			}
			if (!bEnterCarInput) {
				bool bAiming = (pPed->m_pPlayerData && pPed->m_pPlayerData->m_bFreeAiming)
					|| Using1stPersonWeaponCam();
				// Aiming: report non-abortable (no enter-car prompt);
				// Hipfire: report abortable so enter-car prompt stays visible,
				// without prematurely killing CTaskSimpleUseGun.
				return bAiming ? 0u : 1u;
			}
		}
	}
	return CTaskSimplePlayerOnFoot_MakeAbortable(_this, pPed, priority, pEvent);
}

DECL_HOOK(uint32_t, CPad_WeaponJustDown, uintptr_t _this, sa::CPed* pPed, bool bCheckControl, bool bCheckVehicle)
{
	if (CurrentPlayerSlot()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE] ? 1u : 0u;
	}

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || pLocal->IsInJetpack()) {
		return CPad_WeaponJustDown(_this, pPed, bCheckControl, bCheckVehicle);
	}
	if (pLocal->IsInVehicle()) {
		if (pLocal->IsAPassenger() && pLocal->IsInPassengerDriveByMode()) {
			const uint32_t orig = CPad_WeaponJustDown(_this, pPed, bCheckControl, bCheckVehicle);
			return (g_touchCombat.fireJustPressed || orig != 0) ? 1u : 0u;
		}
		return CPad_WeaponJustDown(_this, pPed, bCheckControl, bCheckVehicle);
	}

	if (!CanLocalPedUseOnFootFire(_this, pPed)) {
		return 0;
	}
	return g_touchCombat.fireJustPressed ? 1u : 0u;
}

DECL_HOOK(bool, CPad_CycleWeaponLeftJustDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return false;
	}
	else {
		bool isPressed = CPad_CycleWeaponLeftJustDown(_this);
		if (isPressed) {
			spdlog::info("CycleWeaponLeftJustDown isPressed");
		}
		return isPressed;
	}
}

DECL_HOOK(uint32_t, CPad_CycleWeaponRightJustDown, uintptr_t _this)
{
	if (CurrentPlayerSlot()) {
		return 0;
	}
	else {
		return CPad_CycleWeaponRightJustDown(_this);
	}
}

// the engine only polls the targeting buttons (widgets 0x13/0x14) for
// scoped/special weapons {33..36,43,44,45} while in lock-on aim mode —
// regular guns get no aim button unless the aim-mode setting is free-aim,
// and its free-aim set additionally drops flamethrower/minigun/spraycan
// which SAMP lets you aim. Show the button for everything aimable:
// any weapon <=45 except the knife/katana melee specials.
static bool WeaponShowsAimButton(uint8_t type)
{
	if (type == sa::eWeaponType::WEAPONTYPE_UNARMED
		|| type > sa::eWeaponType::WEAPONTYPE_INFRARED) {
		return false;
	}
	return type != sa::eWeaponType::WEAPONTYPE_KNIFE
		&& type != sa::eWeaponType::WEAPONTYPE_KATANA;
}

static CPlayerPed* GetAimableLocalPed()
{
	CPlayerPed* pPed = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pPed || pPed->IsDead() || pPed->IsInVehicle() || pPed->IsInJetpack()
		|| !WeaponShowsAimButton(pPed->GetCurrentWeapon())) {
		return nullptr;
	}
	return pPed;
}

// blocking is melee-only: fists and melee weapons (unarmed..cane).
static CPlayerPed* GetBlockableLocalPed()
{
	CPlayerPed* pPed = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pPed || pPed->IsDead() || pPed->IsInVehicle() || pPed->IsInJetpack()
		|| pPed->GetCurrentWeapon() > sa::eWeaponType::WEAPONTYPE_CANE) {
		return nullptr;
	}
	return pPed;
}

// scoped weapons aim through a camera weapon-mode + PED_SNIPER_MODE, not the
// free-aim flag: ProcessPlayerWeapon's enter jump-table maps weapon -> mode
// (34->7, 35->8, 36->51, 43->46). For the projectiles (35/36) the native
// enter-poll is unreachable entirely (ProcessPlayerWeapon diverts before it),
// so TickTouchCombatWidgets drives this path directly.
static int ScopedAimCamMode(uint8_t type)
{
	switch (type) {
		case sa::eWeaponType::WEAPONTYPE_SNIPERRIFLE: return 7;
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER: return 8;
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER_HS: return 51;
		case sa::eWeaponType::WEAPONTYPE_CAMERA: return 46;
	}
	return 0;
}

static bool Using1stPersonWeaponCam()
{
	return eng::fn::CCamera_Using1stPersonWeaponMode(
		&sa::getTheCamera());
}

// Engine-side aim state: CPlayerPedData::m_bFreeAiming, lock-on target, or
// first-person weapon camera (CCamera::IsTargetingActive covers all three).
static bool EngineIsAiming(CPlayerPed* pPed)
{
	if (!pPed || !pPed->m_ped) return false;
	if (pPed->m_ped->m_pPlayerData && pPed->m_ped->m_pPlayerData->m_bFreeAiming) {
		return true;
	}
	return Using1stPersonWeaponCam()
		|| eng::fn::CCamera_IsTargetingActive(&sa::getTheCamera());
}

static uint32_t PollTouchRelease(int32_t widgetID)
{
	return eng::fn::CTouchInterface_IsReleased(
		widgetID, static_cast<void*>(nullptr), 1);
}

extern void FixTouchWidgetLayout();

// Custom aim-toggle button lives in slot 0x98 (WIDGET_CAR_CHEATS — unused in SAMP).
// Engine widgets 19/20 stay disabled so their icons never cover the attack slot.
static constexpr int32_t kAimToggleWidget = sa::WidgetIDs::WIDGET_CAR_CHEATS;
static constexpr int32_t kBlockWidget     = sa::WidgetIDs::WIDGET_MISSION_SKIP;

static bool PollTouchDown(int32_t widgetID)
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets || !widgets[widgetID]) return false;
	return eng::fn::CTouchInterface_IsTouched(
		widgetID, static_cast<void*>(nullptr), 1) != 0;
}

// Render-phase helper (called from Render2dStuff): keeps layout and button
// visibility/sprites in sync without mutating game-logic input state.
void UpdateTouchCombatVisuals()
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets) return;

	FixTouchWidgetLayout();

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	bool bOnFoot = pLocal && !pLocal->IsDead() && !pLocal->IsInVehicle() && !pLocal->IsInJetpack();
	bool bPassenger = pLocal && !pLocal->IsDead() && pLocal->IsInVehicle() && pLocal->IsAPassenger();
	bool bPassengerDriveBy = bPassenger && pLocal->IsInPassengerDriveByMode();

	if ((bOnFoot || bPassengerDriveBy) && widgets[sa::WidgetIDs::WIDGET_ATTACK]) {
		uintptr_t wAtk = widgets[sa::WidgetIDs::WIDGET_ATTACK];
		*reinterpret_cast<uint8_t*>(wAtk + kWidgetEnabledOffset) = 1;
		*reinterpret_cast<uint8_t*>(wAtk + kWidgetAlphaOffset) = 0xff;
	}

	// Keep sprint button visible while dancing (WIDGET_PED_MOVE is natively kept
	// enabled every logic frame on foot via TickTouchCombatInput).
	if (bOnFoot && pLocal->IsDancing()) {
		if (uintptr_t wSprint = widgets[sa::WidgetIDs::WIDGET_SPRINT]) {
			*reinterpret_cast<uint8_t*>(wSprint + kWidgetEnabledOffset) = 1;
			*reinterpret_cast<uint8_t*>(wSprint + kWidgetAlphaOffset) = 0xff;
		}
	}

	// Keep enter-car button visible on foot while holding Stuff / Dancing / HandsUp / Pissing
	// so touch players can cancel/drop them (KEY_SECONDARY_ATTACK) even when no vehicle is nearby.
	if (bOnFoot && LocalPedHasCancellableSpecialAction(pLocal)) {
		if (uintptr_t wEnter = widgets[sa::WidgetIDs::WIDGET_ENTER_CAR]) {
			*reinterpret_cast<uint8_t*>(wEnter + kWidgetEnabledOffset) = 1;
			*reinterpret_cast<uint8_t*>(wEnter + kWidgetAlphaOffset) = 0xff;
		}
	}

	// On-foot camera toggle (widget 0x12): GTA:SA Mobile only polls
	// CycleCameraModeDownJustDown while in a vehicle, leaving the top-right
	// camera button hidden on foot. Keep it visible on foot (outside 1st-person
	// weapon scope) without touching it in vehicles where the engine owns it.
	if (bOnFoot && widgets[sa::WidgetIDs::WIDGET_CAM_TOGGLE]) {
		bool bShowCamToggle = !Using1stPersonWeaponCam();
		uintptr_t wCam = widgets[sa::WidgetIDs::WIDGET_CAM_TOGGLE];
		*reinterpret_cast<uint8_t*>(wCam + kWidgetEnabledOffset) = bShowCamToggle ? 1 : 0;
		*reinterpret_cast<uint8_t*>(wCam + kWidgetAlphaOffset) = bShowCamToggle ? 0xff : 0;
	}

	if (uintptr_t wAim = widgets[kAimToggleWidget]) {
		bool bAimable = (GetAimableLocalPed() != nullptr) || (bPassenger && pLocal->CanDoPassengerDriveBy());
		*reinterpret_cast<uint8_t*>(wAim + kWidgetEnabledOffset) = bAimable ? 1 : 0;
		*reinterpret_cast<uint8_t*>(wAim + kWidgetAlphaOffset) = bAimable ? 0xff : 0;
		uintptr_t wSrc = widgets[sa::WidgetIDs::WIDGET_ENTER_TARGETING];
		if (wSrc && *reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset)) {
			*reinterpret_cast<uintptr_t*>(wAim + kWidgetSpriteOffset) =
				*reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset);
		}
	}

	if (uintptr_t wBlk = widgets[kBlockWidget]) {
		bool bBlockable = GetBlockableLocalPed() != nullptr;
		*reinterpret_cast<uint8_t*>(wBlk + kWidgetEnabledOffset) = bBlockable ? 1 : 0;
		*reinterpret_cast<uint8_t*>(wBlk + kWidgetAlphaOffset) = bBlockable ? 0xff : 0;
		uintptr_t wSrc = widgets[sa::WidgetIDs::WIDGET_ATTACK];
		if (wSrc && *reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset)
			&& !*reinterpret_cast<uintptr_t*>(wBlk + kWidgetSpriteOffset)) {
			*reinterpret_cast<uintptr_t*>(wBlk + kWidgetSpriteOffset) =
				*reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset);
		}
	}
}

// Logic-phase input sampler (called right after CTouchInterface::Clear() inside
// CGame::Process(), before CWorld::Process / CPlayerPed::ProcessControl).
// Samples fire/block/aim once per logic frame so every CPad hook sees coherent,
// zero-lag state.
static void TickTouchCombatInput()
{
	// Reset analog axes and per-frame stick state each logic frame so tasks that
	// bypass CPad::GetPedWalk* never broadcast stale non-zero lr/ud.
	LocalPlayerKeys.wKeyLR = 0;
	LocalPlayerKeys.wKeyUD = 0;
	g_touchCombat.onFootStickSampled = false;
	g_touchCombat.stickTouched = false;
	g_touchCombat.stickLR = 0;
	g_touchCombat.stickUD = 0;

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	const bool bOnFoot = pLocal && !pLocal->IsDead() && !pLocal->IsInVehicle() && !pLocal->IsInJetpack();
	const bool bPassenger = pLocal && !pLocal->IsDead() && pLocal->IsInVehicle() && pLocal->IsAPassenger();
	const bool bPassengerDriveBy = bPassenger && pLocal->IsInPassengerDriveByMode();

	// Clear on-foot KEY_HANDBRAKE before re-applying ButtonPanel held keys so
	// frames that did not call GetKeys(clear=true) never retain stale aim state.
	if (bOnFoot) {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = false;
	}

	if (pUI && pUI->buttonpanel() && !IsLocalActivePedMovementLocked()) {
		pUI->buttonpanel()->applyHeldKeys();
	}

	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets) return;

	// 1) Fire button edge + hold tracking (on foot & passenger drive-by)
	const bool bFirePrev = g_touchCombat.firePrevDown;
	bool bFireNow = (bOnFoot || bPassengerDriveBy) && PollTouchDown(sa::WidgetIDs::WIDGET_ATTACK);
	g_touchCombat.fireJustPressed = bFireNow && !bFirePrev;
	if (g_touchCombat.fireJustPressed) {
		g_touchCombat.fireDownTickMs = GetTickCount();
		if (bOnFoot && pLocal && pLocal->GetStuff() != STUFF_TYPE_NONE) {
			g_bStuffUseRequested.store(true, std::memory_order_release);
		}
	}
	g_touchCombat.firePrevDown = bFireNow;
	g_touchCombat.fireDown = bFireNow;

	// Allow cancelling Stuff / Dance / HandsUp / Pissing via WIDGET_ENTER_CAR on foot
	if (bOnFoot && LocalPedHasCancellableSpecialAction(pLocal)
			&& widgets[sa::WidgetIDs::WIDGET_ENTER_CAR]
			&& PollTouchRelease(sa::WidgetIDs::WIDGET_ENTER_CAR) != 0) {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = true;
	}

	// 1.2) Aim button toggles passenger drive-by on touch screens
	if (bPassenger && pLocal->CanDoPassengerDriveBy() && widgets[kAimToggleWidget] && PollTouchRelease(kAimToggleWidget) != 0) {
		g_bPassengerDriveByToggleRequested.store(true, std::memory_order_release);
	}

	// 2) Aim-toggle & scoped camera mode state machine (runs BEFORE stick & key
	// sampling so entering/exiting 1st-person scope takes effect with 0-frame lag).
	if (CPlayerPed* pAimPed = GetAimableLocalPed()) {
		const uint8_t curAimWep = pAimPed->GetCurrentWeapon();
		if (g_touchCombat.lastAimWeapon != curAimWep) {
			g_touchCombat.aimLatch = false;
			g_touchCombat.lastAimWeapon = curAimWep;
		}

		if (widgets[kAimToggleWidget] && PollTouchRelease(kAimToggleWidget) != 0) {
			g_touchCombat.aimLatch = !g_touchCombat.aimLatch;
		}

		const int scopedMode = ScopedAimCamMode(curAimWep);
		if (scopedMode) {
			if (g_touchCombat.aimLatch && !Using1stPersonWeaponCam()) {
				eng::fn::CCamera_SetNewPlayerWeaponMode(
					&sa::getTheCamera(), scopedMode, 0, 0);
				eng::fn::CPed_SetPedState(
					pAimPed->m_ped, sa::ePedState::PED_SNIPER_MODE);
			}
			else if (!g_touchCombat.aimLatch && Using1stPersonWeaponCam()) {
				eng::fn::CCamera_ClearPlayerWeaponMode(
					&sa::getTheCamera());
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

	// 3) Unified on-foot virtual thumbstick poll: always poll WIDGET_PED_MOVE once
	// per logic frame on foot (when movement is unlocked) so m_nFrameCount never
	// starves to 0 across 3rd-person Sniper/RPG hipfire, scope transitions, duck/roll,
	// jump/climb, dancing, or skydiving.
	// CWidgetRegionMove::IsTouched outputs stick.x / stick.y in [-128.0f .. +128.0f].
	if (bOnFoot && !IsLocalActivePedMovementLocked()) {
		if (widgets[sa::WidgetIDs::WIDGET_PED_MOVE]) {
			sa::CVector2D stick{0.0f, 0.0f};
			g_touchCombat.onFootStickSampled = true;
			if (eng::fn::CTouchInterface_IsTouched(sa::WidgetIDs::WIDGET_PED_MOVE, &stick, 1) != 0) {
				g_touchCombat.stickTouched = true;
				if (pLocal->IsDancing()) {
					if (stick.x < -32.0f)      g_touchCombat.stickLR = -128;
					else if (stick.x > 32.0f)  g_touchCombat.stickLR = 128;
					else                       g_touchCombat.stickLR = 0;

					if (stick.y < -32.0f)      g_touchCombat.stickUD = -128;
					else if (stick.y > 32.0f)  g_touchCombat.stickUD = 128;
					else                       g_touchCombat.stickUD = 0;
				}
				else {
					int lr = std::clamp(static_cast<int>(stick.x), -128, 128);
					int ud = std::clamp(static_cast<int>(stick.y), -128, 128);
					// Ensure non-zero stick tilts exceed CTaskSimpleUseGun::SetMoveAnim's
					// (|lr|+|ud|)/128 >= 0.1f (~13) threshold locally and on PC/Mobile observers.
					const int absSum = std::abs(lr) + std::abs(ud);
					if (absSum > 0 && absSum < 16) {
						if (std::abs(lr) >= std::abs(ud)) {
							lr = (lr < 0) ? -16 : 16;
						}
						else {
							ud = (ud < 0) ? -16 : 16;
						}
					}
					g_touchCombat.stickLR = static_cast<int16_t>(lr);
					g_touchCombat.stickUD = static_cast<int16_t>(ud);
				}
			}
		}

		int16_t finalLR = g_touchCombat.stickLR;
		int16_t finalUD = g_touchCombat.stickUD;
		if (finalLR == 0 && pUI && pUI->buttonpanel() && pUI->buttonpanel()->heldLR() != 0) {
			finalLR = static_cast<int16_t>(pUI->buttonpanel()->heldLR());
		}
		if (finalUD == 0 && pUI && pUI->buttonpanel() && pUI->buttonpanel()->heldUD() != 0) {
			finalUD = static_cast<int16_t>(pUI->buttonpanel()->heldUD());
		}
		LocalPlayerKeys.wKeyLR = static_cast<uint16_t>(finalLR);
		LocalPlayerKeys.wKeyUD = static_cast<uint16_t>(finalUD);

		if (pLocal->IsDancing() && PollTouchDown(sa::WidgetIDs::WIDGET_SPRINT)) {
			LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = true;
		}
	}

	// 4) Block button & authoritative on-foot KEY_HANDBRAKE state
	g_touchCombat.blockDown = (GetBlockableLocalPed() != nullptr) && PollTouchDown(kBlockWidget);
	if (IsLocalMeleeBlockActive()) {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = true;
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = true;
	}
	else if (IsLocalMeleeComboActive()) {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = true;
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = true;
	}
	else if (bOnFoot && pLocal && (g_touchCombat.aimLatch || EngineIsAiming(pLocal))) {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = true;
	}

	// 5) On-foot camera distance toggle (close=1, normal=2, far=3)
	if (bOnFoot && !Using1stPersonWeaponCam()
		&& widgets[sa::WidgetIDs::WIDGET_CAM_TOGGLE]
		&& PollTouchRelease(sa::WidgetIDs::WIDGET_CAM_TOGGLE) != 0) {
		auto& cam = sa::getTheCamera();
		int32_t cur = cam.m_nPedZoom;
		cam.m_nPedZoom = (cur >= 1 && cur < 3) ? (cur + 1) : 1;
	}
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

DECL_HOOK(void, CTouchInterface_Clear)
{
	CTouchInterface_Clear();
	TickTouchCombatInput();
}

DECL_HOOK(uint32_t, CPad_GetEnterTargeting, uintptr_t _this)
{
	const uint8_t slot = CurrentPlayerSlot();
	if (slot != 0) {
		if (slot >= PLAYER_PED_SLOTS || !RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_HANDBRAKE]) {
			return 0;
		}
		sa::CPed* pRemotePed = (WORLD_PLAYERS != nullptr) ? WORLD_PLAYERS[slot].m_pPed : dwCurPlayerActor;
		if (pRemotePed && pRemotePed->m_pPlayerData && !pRemotePed->m_pPlayerData->m_bFreeAiming) {
			return 1u;
		}
		return 0;
	}
	g_lastLocalPadPtr = _this;
	if (!_this || *reinterpret_cast<uint16_t*>(_this + 272) != 0) {
		return 0;
	}
	CPlayerPed* pPed = GetAimableLocalPed();
	return (pPed && g_touchCombat.aimLatch) ? 1u : 0u;
}

// CPad::GetExitTargeting is only called inside local CCamera::CamControl
// (remote exit is handled directly in CPed_ProcessControl).
DECL_HOOK(uint32_t, CPad_GetExitTargeting, uintptr_t _this)
{
	if (CurrentPlayerSlot() != 0) {
		return 0;
	}
	CPlayerPed* pPed = GetAimableLocalPed();
	const bool bFiring = g_touchCombat.fireDown || LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE];
	if (pPed && !g_touchCombat.aimLatch && !bFiring && EngineIsAiming(pPed)) {
		eng::fn::CPlayerPed_ClearWeaponTarget(pPed->m_ped);
		if (pPed->m_ped->m_pPlayerData) {
			pPed->m_ped->m_pPlayerData->m_bFreeAiming = false;
		}
		return 1;
	}
	return 0;
}

void ApplyLocalCombatSyncKeys(uint16_t& wKeys, uint16_t& lrAnalog, uint16_t& udAnalog)
{
	if (pUI && pUI->buttonpanel() && !IsLocalActivePedMovementLocked()) {
		if (lrAnalog == 0 && pUI->buttonpanel()->heldLR() != 0) {
			lrAnalog = pUI->buttonpanel()->heldLR();
		}
		if (udAnalog == 0 && pUI->buttonpanel()->heldUD() != 0) {
			udAnalog = pUI->buttonpanel()->heldUD();
		}
	}

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

DECL_HOOK(uint32_t, CCamera_IsTargetingActive, uintptr_t _this, sa::CPed* pPed)
{
	if (pPed == GamePool_FindPlayerPed()) {
		const uint32_t bIsTargeting = CCamera_IsTargetingActive(_this, pPed);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] =
			(bIsTargeting != 0) || IsLocalMeleeBlockActive() || IsLocalMeleeComboActive();
		return bIsTargeting;
	}

	const uint8_t slot = pPed ? FindPlayerNumFromPedPtr(pPed) : CurrentPlayerSlot();
	if (slot == 0 || slot >= PLAYER_PED_SLOTS) {
		return CCamera_IsTargetingActive(_this, pPed);
	}
	if (!RemotePlayerKeys[slot].bKeys[ePadKeys::KEY_HANDBRAKE]) {
		return 0;
	}
	// Delegate to native check (m_bFreeAiming, m_pTargetedObject, or scoped
	// PlayerWeaponMode set by ScopedPlayerContext) so Frame 1 of non-scoped
	// aiming returns 0 and enters through CPad_GetEnterTargeting cleanly
	// (setting m_bFreeAiming = 1 for all guns including Rifle 33 & Flamethrower 37).
	return CCamera_IsTargetingActive(_this, pPed);
}

RwMatrix* RwMatrixOrthoNormalize(RwMatrix* matrixOut, const RwMatrix* matrixIn)
{
	return eng::fn::RwMatrixOrthoNormalize<RwMatrix*>( matrixOut, matrixIn);
}

void SendBulletSync(sa::CVector* vecOrigin, sa::CVector* vecPos, sa::CEntity** ppEntity)
{
	static BULLET_DATA bulletData;
	memset(&bulletData, 0, sizeof(BULLET_DATA));

	bulletData.vecOrigin.x = vecOrigin->x;
	bulletData.vecOrigin.y = vecOrigin->y;
	bulletData.vecOrigin.z = vecOrigin->z;
	bulletData.vecPos.x = vecPos->x;
	bulletData.vecPos.y = vecPos->y;
	bulletData.vecPos.z = vecPos->z;

	if (ppEntity) {
		static sa::CEntity* pEntity;
		pEntity = *ppEntity;
		if (pEntity) {
			if (pEntity->m_matrix) {
				const int lagComp = pNetGame ? pNetGame->session().lagCompensationMode : 0;
				if (lagComp) {
					bulletData.vecOffset.x = vecPos->x - pEntity->m_matrix->m_pos.x;
					bulletData.vecOffset.y = vecPos->y - pEntity->m_matrix->m_pos.y;
					bulletData.vecOffset.z = vecPos->z - pEntity->m_matrix->m_pos.z;
				}
				else {
					static RwMatrix mat1;
					memset(&mat1, 0, sizeof(mat1));

					static RwMatrix mat2;
					memset(&mat2, 0, sizeof(mat2));

					RwMatrix entityMat = pEntity->m_matrix->ToRwMatrix();
					RwMatrixOrthoNormalize(&mat2, &entityMat);
					RwMatrixInvert(&mat1, &mat2);
					ProjectMatrix(&bulletData.vecOffset, reinterpret_cast<sa::CMatrix*>(&mat1), vecPos);
				}
			}

			bulletData.pEntity = pEntity;
		}
	}

	pGame->FindPlayerPed()->ProcessBulletData(&bulletData);
}

struct ScopedLineOfSightHook {
	bool prev;
	ScopedLineOfSightHook() : prev(g_bEnableLineOfSightHook) { g_bEnableLineOfSightHook = true; }
	~ScopedLineOfSightHook() { g_bEnableLineOfSightHook = prev; }
};

DECL_HOOK(uint32_t, CWeapon_FireInstantHit, sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CVector* vecOrigin, sa::CVector* muzzlePosn, sa::CEntity* targetEntity, sa::CVector* target, sa::CVector* originForDriveBy, bool arg6, bool muzzle)
{
	ScopedLineOfSightHook losGuard;

	if (pFiringEntity != GamePool_FindPlayerPed()) {
		return muzzle;
	}

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			pPlayerPool->ApplyCollisionChecking();
		}
	}

	if (pGame) {
		CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
		if (pPlayerPed) {
			pPlayerPed->FireInstant();
		}
	}

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			pPlayerPool->ResetCollisionChecking();
		}
	}

	return muzzle;
}

DECL_HOOK(uint32_t, CWeapon_FireInstantHit_full, sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CVector* vecOrigin, sa::CVector* muzzlePosn, sa::CEntity* targetEntity, sa::CVector* target, sa::CVector* originForDriveBy, bool arg6, bool muzzle)
{
	ScopedLineOfSightHook losGuard;
	return CWeapon_FireInstantHit_full(_this, pFiringEntity, vecOrigin, muzzlePosn, targetEntity, target, originForDriveBy, arg6, muzzle);
}

DECL_HOOK(uint32_t, CWeapon_FireSniper, sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CEntity* victim, sa::CVector* target)
{
	if (pFiringEntity == GamePool_FindPlayerPed()) {
		CPlayerPool* pPlayerPool = pNetGame ? pNetGame->GetPlayerPool() : nullptr;
		if (pPlayerPool) {
			pPlayerPool->ApplyCollisionChecking();
		}
		if (pGame) {
			CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
			if (pPlayerPed) {
				pPlayerPed->FireInstant();
			}
		}
		if (pPlayerPool) {
			pPlayerPool->ResetCollisionChecking();
		}
	}

	return true;
}

DECL_HOOK(uint32_t, CWorld_ProcessLineOfSight, sa::CVector* vecOrigin, sa::CVector* vecEnd, sa::CColPoint* colPoint, sa::CEntity** ppEntity, bool b1, bool b2, bool b3, bool b4, bool b5, bool b6, bool b7, bool b8)
{
	if (!g_bEnableLineOfSightHook) {
		return CWorld_ProcessLineOfSight(vecOrigin, vecEnd, colPoint, ppEntity, b1, b2, b3, b4, b5, b6, b7, b8);
	}

	static sa::CVector vecPosPlusOffset;
	CPlayerPed* pLocalPlayerPed = pGame ? pGame->FindPlayerPed() : nullptr;

	const int lagComp = pNetGame ? pNetGame->session().lagCompensationMode : 0;

	if (lagComp != 2 && vecOrigin && vecEnd) {
		if (g_pCurrentFiredPed && g_pCurrentFiredPed != pLocalPlayerPed) {
			if (g_pCurrentBulletData && g_pCurrentBulletData->pEntity) {
				if (!IsEntityPlaceable(g_pCurrentBulletData->pEntity) && g_pCurrentBulletData->pEntity->m_matrix) {
					if (lagComp) {
						vecPosPlusOffset.x = g_pCurrentBulletData->pEntity->m_matrix->m_pos.x + g_pCurrentBulletData->vecOffset.x;
						vecPosPlusOffset.y = g_pCurrentBulletData->pEntity->m_matrix->m_pos.y + g_pCurrentBulletData->vecOffset.y;
						vecPosPlusOffset.z = g_pCurrentBulletData->pEntity->m_matrix->m_pos.z + g_pCurrentBulletData->vecOffset.z;
					}
					else {
						ProjectMatrix(&vecPosPlusOffset, g_pCurrentBulletData->pEntity->m_matrix, &g_pCurrentBulletData->vecOffset);
					}

					vecEnd->x = vecPosPlusOffset.x - vecOrigin->x + vecPosPlusOffset.x;
					vecEnd->y = vecPosPlusOffset.y - vecOrigin->y + vecPosPlusOffset.y;
					vecEnd->z = vecPosPlusOffset.z - vecOrigin->z + vecPosPlusOffset.z;
				}
			}
		}
	}

	uint32_t result = CWorld_ProcessLineOfSight(vecOrigin, vecEnd, colPoint, ppEntity, b1, b2, b3, b4, b5, b6, b7, b8);

	if (lagComp == 2) {
		if (g_pCurrentFiredPed && g_pCurrentFiredPed == pLocalPlayerPed && vecOrigin && colPoint) {
			SendBulletSync(vecOrigin, &colPoint->m_vecPoint, ppEntity);
		}
		return result;
	}

	if (g_pCurrentFiredPed) {
		if (g_pCurrentFiredPed != pLocalPlayerPed) {
			if (g_pCurrentBulletData && g_pCurrentBulletData->pEntity == nullptr && ppEntity) {
				sa::CPed* pLocalPed = GamePool_FindPlayerPed();
				if (pLocalPed && (*ppEntity == pLocalPed || (pLocalPed->IsInVehicle() && *ppEntity == pLocalPed->pVehicle))) {
					result = 0;
					*ppEntity = nullptr;
					if (colPoint) {
						colPoint->m_vecPoint.x = 0.0f;
						colPoint->m_vecPoint.y = 0.0f;
						colPoint->m_vecPoint.z = 0.0f;
					}
					return result;
				}
			}
		}
		else if (vecOrigin && colPoint) {
			SendBulletSync(vecOrigin, &colPoint->m_vecPoint, ppEntity);
		}
	}

	return result;
}

DECL_HOOK(sa::CEntity *, CBulletInfo_Update, uintptr_t _this)
{
	ScopedLineOfSightHook losGuard;
	return CBulletInfo_Update(_this);
}

#pragma pack(push, 1)
struct CPedDamageResponseCalculator {
	sa::CPed* m_pInflictor;
	float m_fRawDamage;
	int32_t m_eHitZone;
	int32_t m_eWeaponUsed;
	bool m_bJumpedOutOfMovingCar;
	uint8_t pad[3];
};
#pragma pack(pop)
VALIDATE_SIZE(CPedDamageResponseCalculator, (VER_x32 ? 0x14 : 0x18));

PLAYERID FindPlayerIDFromGtaPtr(sa::CEntity* pEntity)
{
	if (pEntity == nullptr || !pNetGame) return INVALID_PLAYER_ID;

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pPlayerPool) return INVALID_PLAYER_ID;

	PLAYERID PlayerID = pPlayerPool->FindRemotePlayerIDFromGtaPtr((sa::CPed*) pEntity);
	if (PlayerID != INVALID_PLAYER_ID) return PlayerID;

	if (pVehiclePool) {
		auto* pGtaVehicle = (sa::CVehicle*) pEntity;
		VEHICLEID VehicleID = pVehiclePool->FindIDFromGtaPtr(pGtaVehicle);
		if (VehicleID != INVALID_VEHICLE_ID) {
			if (pGtaVehicle->pDriver) {
				PLAYERID driverId = pPlayerPool->FindRemotePlayerIDFromGtaPtr(pGtaVehicle->pDriver);
				if (driverId != INVALID_PLAYER_ID) {
					return driverId;
				}
			}
			for (PLAYERID i = 0; i < MAX_PLAYERS; i++) {
				CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(i);
				if (pRemotePlayer && pRemotePlayer->IsActive()
						&& pRemotePlayer->GetState() == PLAYER_STATE_DRIVER
						&& pRemotePlayer->CurrentVehicleID() == VehicleID) {
					return i;
				}
			}
		}
	}

	return INVALID_PLAYER_ID;
}

PLAYERID FindActorIDFromGtaPtr(sa::CPed* pPed)
{
	if (pPed && pNetGame && pNetGame->GetActorPool()) {
		return pNetGame->GetActorPool()->FindIDFromGtaPtr(pPed);
	}

	return INVALID_PLAYER_ID;
}

bool ComputeDamageResponse(CPedDamageResponseCalculator* calculator, sa::CPed* pPed)
{
	sa::CPed* pGamePed = GamePool_FindPlayerPed();
	bool isLocalPed = false;

	if (!pNetGame || !pGamePed) {
		return false;
	}

	sa::CPed* pDamager = calculator->m_pInflictor;
	if (pDamager != pGamePed && (pPed && *(uintptr_t*) pPed == g_saSym->Abs<uintptr_t>(addr::vptr$CCivilianPed))) { /* CCivilianPed */
		return true;
	}

	if (pPed == pGamePed) {
		isLocalPed = true;
	}
	else if (pDamager != pGamePed) {
		return false;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
	if (!pLocalPlayer) {
		return false;
	}
	PLAYERID PlayerID = INVALID_PLAYER_ID;

	// open.mp rejects Give/TakeDamage & GiveDamageActor if bodypart is outside [3..9] (Torso..Head).
	const int bodyPart = (calculator->m_eHitZone >= 3 && calculator->m_eHitZone <= 9)
		? calculator->m_eHitZone : 3;

	if (isLocalPed) {
		if (pLocalPlayer->m_surfSync.bIsActive && pLocalPlayer->m_surfSync.dwSurfVehID >= 0) {
			// Matches PC hooks.cpp: while surfing, block both collision damage (54) and
			// any impact coming from the surfed vehicle/object itself.
			if (calculator->m_eWeaponUsed == WEAPON_COLLISION) {
				return true;
			}
			if (pLocalPlayer->m_surfSync.bIsVehicle) {
				CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
				CVehicle* pSurfingVehicle = (pVehiclePool && pLocalPlayer->m_surfSync.dwSurfVehID < MAX_VEHICLES)
						? pVehiclePool->GetAt((VEHICLEID) pLocalPlayer->m_surfSync.dwSurfVehID) : nullptr;
				if (pSurfingVehicle && pSurfingVehicle->m_entity == (sa::CEntity*) pDamager) {
					return true;
				}
			}
			else {
				CObjectPool* pObjectPool = pNetGame->GetObjectPool();
				CObject* pSurfingObject = (pObjectPool && pLocalPlayer->m_surfSync.dwSurfVehID < MAX_OBJECTS)
						? pObjectPool->GetAt((OBJECTID) pLocalPlayer->m_surfSync.dwSurfVehID) : nullptr;
				if (pSurfingObject && pSurfingObject->m_entity == (sa::CEntity*) pDamager) {
					return true;
				}
			}
		}

		PlayerID = FindPlayerIDFromGtaPtr(pDamager);
	}
	else {
		PlayerID = FindPlayerIDFromGtaPtr(pPed);
	}

	if (isLocalPed) {
		pLocalPlayer->SendTakeDamageEvent(PlayerID, calculator->m_fRawDamage, calculator->m_eWeaponUsed, bodyPart);
	}
	else {
		if (PlayerID != INVALID_PLAYER_ID) {
			pLocalPlayer->SendGiveDamageEvent(PlayerID, calculator->m_fRawDamage, calculator->m_eWeaponUsed, bodyPart);
			CRemotePlayer* pRemote = pPlayerPool->GetAt(PlayerID);
			if (pRemote && pRemote->IsNPC()) {
				return true;
			}
		}
		else {
			PLAYERID ActorID = FindActorIDFromGtaPtr(pPed);
			if (ActorID != INVALID_PLAYER_ID) {
				CActorPool* pActorPool = pNetGame->GetActorPool();
				CActor* pActor = pActorPool ? pActorPool->GetAt(ActorID) : nullptr;
				if (pActor && !pActor->m_invulnerable) {
					pLocalPlayer->SendGiveDamageActorEvent(ActorID, calculator->m_fRawDamage, calculator->m_eWeaponUsed, bodyPart);
				}
				return true;
			}
		}
	}

	// Matches PC hooks.cpp:2839-2849 — ALWAYS emit Give/TakeDamage RPCs first
	// (required by weapon-config & server-side damage gamemodes that place all
	// players on the same team), then suppress native GTA HP loss on same-team.
	if (pNetGame->m_netSet && pNetGame->m_netSet->friendlyFire && PlayerID != INVALID_PLAYER_ID) {
		uint8_t byteTeam = pLocalPlayer->m_byteTeam;
		CRemotePlayer* pTargetRemote = pPlayerPool->GetAt(PlayerID);
		if (byteTeam != NO_TEAM && pTargetRemote && pTargetRemote->m_byteTeam == byteTeam) {
			return true;
		}
	}

	return false;
}

DECL_HOOK(void, CPedDamageResponseCalculator_ComputeDamageResponse, CPedDamageResponseCalculator* _this, sa::CPed* pVictim, uintptr_t damageResponse, const bool bTriggerPainAudio)
{
	if (_this != nullptr && pVictim != nullptr) {
		if (ComputeDamageResponse(_this, pVictim)) {
			return;
		}
	}

	return CPedDamageResponseCalculator_ComputeDamageResponse(_this, pVictim, damageResponse, bTriggerPainAudio);
}

// Intercept CWeapon::GenerateDamageEvent BEFORE it blends unsynced local melee hit/knockdown
// animations onto managed CActors (0x706a58) or pushes CEventDamage into CPedIntelligence (0x706bd8).
DECL_HOOK(void, CWeapon_GenerateDamageEvent, sa::CPed* pVictim, sa::CEntity* pCreator, int weaponType, int nDamageFactor, int pedPiece, int direction)
{
	if (pNetGame && pVictim) {
		const PLAYERID actorId = FindActorIDFromGtaPtr(pVictim);
		if (actorId != INVALID_PLAYER_ID) {
			sa::CPed* pLocalPed = GamePool_FindPlayerPed();
			if (pLocalPed && pCreator == reinterpret_cast<sa::CEntity*>(pLocalPed)) {
				CActorPool* pActorPool = pNetGame->GetActorPool();
				CActor* pActor = pActorPool ? pActorPool->GetAt(actorId) : nullptr;
				CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
				CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
				if (pActor && !pActor->m_invulnerable && pLocalPlayer) {
					const int bodyPart = (pedPiece >= 3 && pedPiece <= 9) ? pedPiece : 3;
					pLocalPlayer->SendGiveDamageActorEvent(actorId, static_cast<float>(nDamageFactor), weaponType, bodyPart);
				}
			}
			return;
		}
	}

	CWeapon_GenerateDamageEvent(pVictim, pCreator, weaponType, nDamageFactor, pedPiece, direction);
}

// T21: EnableVehicleFriendlyFire — block damage to vehicles driven by teammates
DECL_HOOK(void, CVehicle_InflictDamage, sa::CVehicle* _this, sa::CEntity* pDamager, int eWeaponType, float fIntensity, sa::CVector vecCoords)
{
	if (_this && pDamager && pNetGame && pNetGame->m_netSet && pNetGame->m_netSet->vehicleFriendlyFire) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
		if (pLocalPlayer && pLocalPlayer->m_byteTeam != NO_TEAM && pLocalPlayer->GetPlayerPed()) {
			const uint8_t localTeam = pLocalPlayer->m_byteTeam;
			sa::CPed* pLocalGtaPed = pLocalPlayer->GetPlayerPed()->m_ped;

			// Case 1: local player is driving _this, attacker is a teammate (ped or vehicle driven by teammate)
			if (_this->pDriver == pLocalGtaPed) {
				PLAYERID attackerId = FindPlayerIDFromGtaPtr(reinterpret_cast<sa::CPed*>(pDamager));
				if (attackerId == INVALID_PLAYER_ID && pNetGame->GetVehiclePool()) {
					VEHICLEID attVehId = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(reinterpret_cast<sa::CVehicle*>(pDamager));
					if (attVehId != INVALID_VEHICLE_ID) {
						auto* attVeh = reinterpret_cast<sa::CVehicle*>(pDamager);
						if (attVeh->pDriver) {
							attackerId = FindPlayerIDFromGtaPtr(attVeh->pDriver);
						}
					}
				}
				if (attackerId != INVALID_PLAYER_ID) {
					CRemotePlayer* pAttacker = pPlayerPool->GetAt(attackerId);
					if (pAttacker && pAttacker->m_byteTeam == localTeam) {
						return;
					}
				}
			}
			// Case 2: teammate is driving _this, attacker is local player (or local player's vehicle)
			else if (_this->pDriver) {
				PLAYERID driverId = FindPlayerIDFromGtaPtr(_this->pDriver);
				if (driverId != INVALID_PLAYER_ID) {
					CRemotePlayer* pDriver = pPlayerPool->GetAt(driverId);
					if (pDriver && pDriver->m_byteTeam == localTeam) {
						sa::CVehicle* pLocalVeh = pLocalPlayer->GetPlayerPed()->GetGtaVehicle();
						if (pDamager == reinterpret_cast<sa::CEntity*>(pLocalGtaPed) ||
								(pLocalVeh && pDamager == reinterpret_cast<sa::CEntity*>(pLocalVeh))) {
							return;
						}
					}
				}
			}
		}
	}

	CVehicle_InflictDamage(_this, pDamager, eWeaponType, fIntensity, vecCoords);
}

DECL_HOOK(uint32_t, CBulletInfo_AddBullet, sa::CEntity* creator, int weaponType, sa::CVector pos, sa::CVector velocity)
{
	velocity.x *= 50.0f;
	velocity.y *= 50.0f;
	velocity.z *= 50.0f;

	CBulletInfo_AddBullet(creator, weaponType, pos, velocity);

	// CBulletInfo::Update
	eng::fn::CBulletInfo_Update();
	return true;
}

void Hooks::installPadHooks()
{
	spdlog::info("Installing pad hooks...");

	memset(&LocalPlayerKeys, 0, sizeof(PAD_KEYS));

	// playerped & per-frame guard hooks (replaces per-frame .text nop/restoreNop)
	REG_HOOK(Boot, CPlayerPed_ProcessControl, CPed_ProcessControl, "Remote player ped control context switch");
	REG_HOOK_BL(Boot, addr::CPed_UpdatePosition_BranchBL1, "CPed_UpdatePosition.ProcessControl",
		&CPed_UpdatePosition_fromProcessControl_hook, &CPed_UpdatePosition_fromProcessControl,
		"Suppress CPed::UpdatePosition for remote peds");
	REG_HOOK_BL(Boot, addr::CPlayerPed_ProcessControl_AttackRelease_BranchBL, "CTouchInterface_IsReleased.ProcessControl",
		&CTouchInterface_IsReleased_fromProcessControl_hook, &CTouchInterface_IsReleased_fromProcessControl,
		"Keep m_bFreeAiming across touch fire release while aim-latched");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_PlayerControlZelda, CTaskSimplePlayerOnFoot_PlayerControlZelda,
		"Preserve remote ped m_fAimingRotation from camera");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_PlayerControlZeldaWeapon, CTaskSimplePlayerOnFoot_PlayerControlZeldaWeapon,
		"Enable 1st-person scoped weapon movement and preserve remote aim rotation");

	// vehicles
	REG_HOOK(Boot, CAutomobile_ProcessControl, CAutomobile_ProcessControl, "Remote automobile control context switch");
	REG_HOOK(Boot, CBoat_ProcessControl, CBoat_ProcessControl, "Remote boat control context switch");
	REG_HOOK(Boot, CBike_ProcessControl, CBike_ProcessControl, "Remote bike control context switch");
	REG_HOOK(Boot, CPlane_ProcessControl, CPlane_ProcessControl, "Remote plane control context switch");
	REG_HOOK(Boot, CHeli_ProcessControl, CHeli_ProcessControl, "Remote heli control context switch");
	REG_HOOK(Boot, CBmx_ProcessControl, CBmx_ProcessControl, "Remote bmx control context switch");
	REG_HOOK(Boot, CMonsterTruck_ProcessControl, CMonsterTruck_ProcessControl, "Remote monster truck control context switch");
	REG_HOOK(Boot, CQuadBike_ProcessControl, CQuadBike_ProcessControl, "Remote quad bike control context switch");
	REG_HOOK(Boot, CTrain_ProcessControl, CTrain_ProcessControl, "Remote train control context switch");
	REG_HOOK(Boot, CTrailer_ProcessControl, CTrailer_ProcessControl, "Remote trailer control context switch");

	REG_HOOK(Boot, CPad_GetPedWalkLeftRight, CPad_GetPedWalkLeftRight, "Pad walk left/right sync");
	REG_HOOK(Boot, CPad_GetPedWalkUpDown, CPad_GetPedWalkUpDown, "Pad walk up/down sync");

	REG_HOOK(Boot, CPad_GetSprint, CPad_GetSprint, "Pad sprint sync");

	REG_HOOK(Boot, CPad_GetJump, CPad_GetJump, "Pad jump sync");
	REG_HOOK(Boot, CPad_JumpJustDown, CPad_JumpJustDown, "Pad jump just-down sync");

	REG_HOOK(Boot, CPad_GetDuck, CPad_GetDuck, "Pad duck sync");
	REG_HOOK(Boot, CPad_DuckJustDown, CPad_DuckJustDown, "Pad duck just-down sync");

	REG_HOOK(Boot, CPad_MeleeAttackJustDown, CPad_MeleeAttackJustDown, "Pad melee attack sync");
	REG_HOOK(Boot, CPad_GetBlock, CPad_GetBlock, "Pad block widget override");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_PlayerControlFighter, CTaskSimplePlayerOnFoot_PlayerControlFighter, "Touch block timeout guard");
	REG_HOOK(Boot, CTouchInterface_Clear, CTouchInterface_Clear, "Multi-touch pinch-zoom guard");

	REG_HOOK(Boot, CWidget_SetInitialTouchWidget, CWidget_SetInitialTouchWidget, "Protect active left thumbstick from multi-touch hijack");
	REG_HOOK(Boot, CWidgetRegionMove_Update, CWidgetRegionMove_Update, "Virtual joystick scale (Update)");
	REG_HOOK(Boot, CWidgetRegionMove_ClampInternalTouchAnchor, CWidgetRegionMove_ClampInternalTouchAnchor, "Virtual joystick scale (Anchor)");
	REG_HOOK(Boot, CWidgetRegionMove_Draw, CWidgetRegionMove_Draw, "Virtual joystick scale (Draw)");
	REG_HOOK(Boot, CWidgetRegionMove_IsTouched, CWidgetRegionMove_IsTouched, "Virtual joystick scale (IsTouched)");

#if VER_x32
	REG_HOOK_ADDR(Boot, addr::CPad_GetAbortClimb_arm32, CPad_GetAbortClimb, true, "Pad abort climb sync (arm32)");
#else
	REG_HOOK(Boot, CPad_GetAbortClimb, CPad_GetAbortClimb, "Pad abort climb sync");
#endif
	REG_HOOK(Boot, CPad_GetAutoClimb, CPad_GetAutoClimb, "Pad auto climb sync");

	REG_HOOK(Boot, CPad_SwimJumpJustDown, CPad_SwimJumpJustDown, "Pad swim jump sync");
	REG_HOOK(Boot, CPad_DiveJustDown, CPad_DiveJustDown, "Pad dive sync");

	REG_HOOK(Boot, CPad_GetSteeringUpDown, CPad_GetSteeringUpDown, "Pad vehicle steer up/down sync");
	REG_HOOK(Boot, CPad_GetSteeringLeftRight, CPad_GetSteeringLeftRight, "Pad vehicle steer left/right sync");

	REG_HOOK(Boot, CPad_GetHandBrake, CPad_GetHandBrake, "Pad handbrake sync");
	REG_HOOK(Boot, CPad_GetBrake, CPad_GetBrake, "Pad brake sync");
	REG_HOOK(Boot, CPad_GetAccelerate, CPad_GetAccelerate, "Pad accelerate sync");
	REG_HOOK(Boot, CPad_GetHorn, CPad_GetHorn, "Pad horn sync");

	REG_HOOK(Boot, CPad_ExitVehicleJustDown, CPad_ExitVehicleJustDown, "Pad exit vehicle sync");

	REG_HOOK(Boot, CPad_GetTurretRight, CPad_GetTurretRight, "Pad turret right sync");
	REG_HOOK(Boot, CPad_GetTurretLeft, CPad_GetTurretLeft, "Pad turret left sync");

	REG_HOOK(Boot, CPad_GetHydraulicJump, CPad_GetHydraulicJump, "Pad hydraulic jump sync");
	REG_HOOK(Boot, CPad_GetNitroFired, CPad_GetNitroFired, "Pad nitro sync");

	REG_HOOK_BL(Boot, addr::CTaskComplexEnterCarAsDriver_Branch1, "CTaskComplexEnterCarAsDriver.Branch1",
		&CTaskComplexEnterCarAsDriver_hook, &CTaskComplexEnterCarAsDriver, "Notify server on vehicle enter");
	REG_HOOK_BL(Boot, addr::CTaskComplexLeaveCar_Branch1, "CTaskComplexLeaveCar.Branch1",
		&CTaskComplexLeaveCar_hook, &CTaskComplexLeaveCar, "Notify server on vehicle exit (branch 1)");
	REG_HOOK_BL(Boot, addr::CTaskComplexLeaveCar_Branch2, "CTaskComplexLeaveCar.Branch2",
		&CTaskComplexLeaveCar_hook, &CTaskComplexLeaveCar, "Notify server on vehicle exit (branch 2)");

	REG_HOOK(Boot, CTaskSimpleUseGun_StartAnim, CTaskSimpleUseGun_StartAnim, "Fix UseGun aim-while-moving m_nLastCommand transition");
	REG_HOOK(Boot, CTaskSimpleUseGun_SetPedPosition, CTaskSimpleUseGun_SetPedPosition, "Remote player aim/gun context switch");
	REG_HOOK(Boot, CPlayerInfo_Process, CPlayerInfo_Process, "Car-proximity hipfire abort guard");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_MakeAbortable, CTaskSimplePlayerOnFoot_MakeAbortable, "Protect UseGun from proximity probe");

	REG_HOOK(Boot, CPad_CycleWeaponRightJustDown, CPad_CycleWeaponRightJustDown, "Pad weapon cycle right");
	REG_HOOK(Boot, CPad_CycleWeaponLeftJustDown, CPad_CycleWeaponLeftJustDown, "Pad weapon cycle left");
	REG_HOOK(Boot, CPad_GetWeapon, CPad_GetWeapon, "Pad weapon fire sync");
	REG_HOOK(Boot, CPad_WeaponJustDown, CPad_WeaponJustDown, "Pad weapon just-down sync");
	REG_HOOK(Boot, CCamera_IsTargetingActivePed, CCamera_IsTargetingActive, "Remote player targeting check");
	REG_HOOK(Boot, CPad_GetTarget, CPad_GetTarget, "Remote player aim/roll target sync");
	REG_HOOK(Boot, CPad_GetEnterTargeting, CPad_GetEnterTargeting, "Touch aim button enter");
	REG_HOOK(Boot, CPad_GetExitTargeting, CPad_GetExitTargeting, "Touch aim button exit");

	REG_HOOK_BL(Boot, addr::CWeapon_FireInstantHit_branch1, "CWeapon_FireInstantHit.Branch1",
		&CWeapon_FireInstantHit_hook, &CWeapon_FireInstantHit, "Bullet sync fire hook (branch 1)");
	REG_HOOK_BL(Boot, addr::CWeapon_FireInstantHit_branch2, "CWeapon_FireInstantHit.Branch2",
		&CWeapon_FireInstantHit_hook, &CWeapon_FireInstantHit, "Bullet sync fire hook (branch 2)");

	REG_HOOK(Boot, CWeapon_FireInstantHit, CWeapon_FireInstantHit_full, "Lag-comp instant hit full hook");
	REG_HOOK(Boot, CWeapon_FireSniper, CWeapon_FireSniper, "Sniper fire sync");

	ghook_CWorld_ProcessLineOfSight = REG_HOOK(Boot, CWorld_ProcessLineOfSight, CWorld_ProcessLineOfSight,
		"Bullet raycast hit capture (gated in hook)");

	REG_HOOK(Boot, CBulletInfo_Update, CBulletInfo_Update, "Projectile bullet update hook");
	REG_HOOK(Boot, CPedDamageResponseCalculator_ComputeDamageResponse, CPedDamageResponseCalculator_ComputeDamageResponse,
		"Give/take damage reporting");
	REG_HOOK(Boot, CWeapon_GenerateDamageEvent, CWeapon_GenerateDamageEvent,
		"Suppress unsynced local melee hit anims & AI damage events on managed Actors");
	REG_HOOK(Boot, CVehicle_InflictDamage, CVehicle_InflictDamage, "Vehicle friendly-fire damage filter");
	REG_HOOK(Boot, CBulletInfo_AddBullet, CBulletInfo_AddBullet, "Bullet tracer sync");
}
