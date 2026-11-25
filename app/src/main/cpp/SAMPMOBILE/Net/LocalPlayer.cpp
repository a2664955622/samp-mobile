#include "../Client.h"
#include "../Game/Game.h"
#include "../Game/Util.h"
#include "Enums/WidgetIDs.h"
#include "../Net/NetGame.h"
#include "Proto/RpcOutbound.h"
#include "Proto/Sync.h"
#include "../UI/UI.h"
#include "../Java/EditObject.h"

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;

uint32_t dwEnterVehTimeElasped = -1;
bool bFirstSpawn = true;
// bool g_bLockEnterVehicleWidget = false;

CLocalPlayer::CLocalPlayer()
{
	m_bInRCMode = false;

	m_pPlayerPed = pGame->FindPlayerPed();
	m_bIsActive = false;
	m_bIsWasted = false;

	m_iDisplayZoneTick = 0;
	m_dwLastSendTick = GetTickCount();
	m_dwLastSendSpecTick = GetTickCount();
	m_dwLastInCarSyncTick = GetTickCount();
	m_dwLastPassengerSyncTick = GetTickCount();
	m_dwLastTrailerSyncTick = GetTickCount();
	m_dwLastUnoccupiedSyncTick = GetTickCount();
	m_dwLastOnFootSyncTick = GetTickCount();
	m_dwLastAimDataSyncTick = GetTickCount();
	m_dwLastSendAimSyncTick = GetTickCount();
	m_dwLastStatsUpdateTick = GetTickCount();
	m_bIsSpectating = false;
	m_byteSpectateType = SPECTATE_TYPE_NONE;
	m_SpectateID = 0xFFFFFFFF;
	m_bSpawnDialogShowed = false;
	ResetAllSyncAttributes();

	m_statsSync.dwLastMoney = 0;
	m_statsSync.dwLastDrunkLevel = 0;

	m_surfSync.bIsActive = false;
	m_surfSync.pSurfInst = 0;
	m_surfSync.bIsVehicle = false;
	m_surfSync.dwSurfVehID = INVALID_VEHICLE_ID;
	m_surfSync.vecOffsetPos = sa::CVector{0.0f, 0.0f, 0.0f};

	m_DamageVehicle = INVALID_VEHICLE_ID;
	m_dwDamagePanels = 0;
	m_dwDamageDoors = 0;
	m_byteDamageLights = 0;
	m_byteDamageTires = 0;

	for (int i = 0; i < 13; i++) {
		m_byteLastWeapon[i] = 0;
		m_dwLastAmmo[i] = 0;
	}

	m_byteTeam = NO_TEAM;
}

CLocalPlayer::~CLocalPlayer()
{
	// ~
}

static void ApplyEffectiveLocalOnFootKeys(CPlayerPed* pPlayerPed, uint16_t& wKeys, uint16_t& lrAnalog, uint16_t& udAnalog)
{
	if (!pPlayerPed || !pPlayerPed->m_ped || pPlayerPed->IsDead() || pPlayerPed->IsInVehicle()) {
		return;
	}
	ApplyLocalCombatSyncKeys(wKeys, lrAnalog, udAnalog);

	sa::CCamera* pTheCam = &sa::getTheCamera();
	const bool bAimingNow =
		(pPlayerPed->m_ped->m_pPlayerData && pPlayerPed->m_ped->m_pPlayerData->m_bFreeAiming)
		|| eng::fn::CCamera_Using1stPersonWeaponMode(pTheCam)
		|| eng::fn::CCamera_IsTargetingActive(pTheCam);
	if (bAimingNow) {
		wKeys |= 128u; // KEY_HANDBRAKE
	}
	// Match PC playerped.cpp:1577-1582: suppress KEY_FIRE when total ammo == 0 (weapons > 15, except parachute 46)
	if ((wKeys & 4u) != 0) {
		sa::CWeapon* pSlot = pPlayerPed->GetCurrentWeaponSlot();
		if (pSlot && pSlot->dwType > 15 && pSlot->dwType != 46 && pSlot->dwAmmo == 0) {
			wKeys &= static_cast<uint16_t>(~4u);
		}
	}
}

bool CLocalPlayer::Process()
{
	uint32_t dwThisTick;

	if (Game::IsGamePaused()) {
		// Suppress outbound player sync while pause menu is open so server/remotes detect AFK (PC parity)
		return true;
	}

	if (m_bIsActive && m_pPlayerPed != nullptr) {
		// local player is dead
		if (!m_bIsWasted && (m_pPlayerPed->GetActionTrigger() == ACTION_DEATH || m_pPlayerPed->IsDead())) {
			ToggleSpectating(false);

			if (m_pPlayerPed->IsDancing()) {
				m_pPlayerPed->StopDancing();
			}

			if (m_pPlayerPed->IsCellphoneEnabled()) {
				m_pPlayerPed->ToggleCellphone(false);
			}

			if (m_pPlayerPed->IsPissing()) {
				m_pPlayerPed->TogglePissing(false);
			}

			if (m_pPlayerPed->IsCuffed()) {
				m_pPlayerPed->ToggleCuffed(false);
			}

			if (m_pPlayerPed->IsCarry()) {
				m_pPlayerPed->ToggleCarry(false);
			}

			if (m_pPlayerPed->IsInJetpack()) {
				m_pPlayerPed->StopJetpack();
			}

			if (m_pPlayerPed->GetStuff() != STUFF_TYPE_NONE) {
				m_pPlayerPed->DropStuff();
			}

			if (m_pPlayerPed->GetDrunkLevel() > 0) {
				m_pPlayerPed->SetDrunkLevel(0);
				m_pPlayerPed->DrunkProcess();
			}

			m_pPlayerPed->ExtinguishFire();

			// reset tasks/anims
			m_pPlayerPed->TogglePlayerControllable(true);

			if (m_bInRCMode) {
				m_bInRCMode = false;
				m_pPlayerPed->Add();
			}

			if (m_pPlayerPed->IsInVehicle() && !m_pPlayerPed->IsAPassenger()) {
				SendInCarFullSyncData(); // for explosion
				CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
				if (pVehiclePool) {
					m_LastVehicle = pVehiclePool->FindIDFromGtaPtr(m_pPlayerPed->GetGtaVehicle());
				}
			}

			m_pPlayerPed->ExtinguishFire();
			SendWastedNotification();
			m_bIsActive = false;
			m_bIsWasted = true;
			if (m_bWantsAnotherClass) {
				m_bHasSpawnInfo = false;
				m_bClearedToSpawn = false;
			}
			pGame->EnableZoneNames(false);
			return true;
		}

		uint16_t wKeys, lrAnalog, udAnalog;
		uint8_t extKeys;
		wKeys = m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, &extKeys, false);
		ApplyEffectiveLocalOnFootKeys(m_pPlayerPed, wKeys, lrAnalog, udAnalog);

		// HANDLE DANCING LOCAL PED
		if (m_pPlayerPed->IsDancing()) {
			m_pPlayerPed->ProcessDancing();
			if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] || m_pPlayerPed->IsInVehicle()) {
				m_pPlayerPed->StopDancing();
			}
		}

		// HANDLE I GOT MY HANDS UP BUT DON'T WANT TO ANYMORE
		if (m_pPlayerPed->HasHandsUp()) {
			if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] || m_pPlayerPed->IsInVehicle()) {
				m_pPlayerPed->TogglePlayerControllable(true);
			}
		}

		// HANDLE CELLPHONE LOCAL PED: PC keeps cellphone active in vehicles & across Enter

		// HANDLE PISSING LOCAL PED
		if (m_pPlayerPed->IsPissing()) {
			if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] || m_pPlayerPed->IsInVehicle()) {
				m_pPlayerPed->TogglePissing(false);
			}
		}

		// HANDLE CUFF LOCAL PED (server-controlled only; enforce unarmed while cuffed on foot)
		if (m_pPlayerPed->IsCuffed()) {
			if (!m_pPlayerPed->IsInVehicle() && m_pPlayerPed->GetCurrentWeapon() != 0) {
				m_pPlayerPed->SetArmedWeapon(0);
			}
		}

		// HANDLE DRINKING / SMOKING LOCAL PED (T17)
		if (m_pPlayerPed->GetStuff() != STUFF_TYPE_NONE) {
			if (!m_pPlayerPed->IsInVehicle() && m_pPlayerPed->GetCurrentWeapon() != 0) {
				m_pPlayerPed->SetArmedWeapon(0);
			}
			if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] || m_pPlayerPed->IsInVehicle()) {
				ConsumeStuffUseRequest();
				m_pPlayerPed->DropStuff();
			}
			else if (LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] || ConsumeStuffUseRequest()) {
				m_pPlayerPed->ApplyStuff();
			}
		}
		else {
			ConsumeStuffUseRequest();
		}

		// HANDLE CARRY LOCAL PED (server-controlled only; enforce unarmed while carrying on foot)
		if (m_pPlayerPed->IsCarry()) {
			if (!m_pPlayerPed->IsInVehicle() && m_pPlayerPed->GetCurrentWeapon() != 0) {
				m_pPlayerPed->SetArmedWeapon(0);
			}
		}

		// HANDLE JETPACK LOCAL PED
		if (m_pPlayerPed->IsInJetpack()) {
			if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] || m_pPlayerPed->IsInVehicle()) {
				m_pPlayerPed->StopJetpack();
			}
		}

		m_pPlayerPed->DrunkProcess();

		m_pPlayerPed->SatisfyHunger();

		dwThisTick = GetTickCount();

		if (dwEnterVehTimeElasped != -1 && (dwThisTick - dwEnterVehTimeElasped) > 5000 && !m_pPlayerPed->IsInVehicle()) {
			pGame->GetCamera()->SetBehindPlayer();
			dwEnterVehTimeElasped = -1;
		}

		if (dwThisTick >= m_iDisplayZoneTick) {
			pGame->EnableZoneNames(pNetGame->m_netSet->zoneNames);
		}

		pGame->UpdateCheckpoints();

		if ((dwThisTick - m_dwLastStatsUpdateTick) > 1000) {
			SendStatsUpdate();
			m_dwLastStatsUpdateTick = dwThisTick;
		}

		CheckWeapons();

		uint8_t byteInterior = pGame->GetActiveInterior();
		if (byteInterior != m_byteCurInterior) {
			UpdateRemoteInterior(byteInterior);
		}

		// T21: AllowInteriorWeapons(false) disarms the local player inside interiors
		if (pNetGame->m_netSet && !pNetGame->m_netSet->allowWeapons && byteInterior != 0) {
			if (m_pPlayerPed->GetCurrentWeapon() != 0) {
				m_pPlayerPed->SetArmedWeapon(0);
			}
		}

		UpdateCameraTarget();

		// Local head-turn look-at sync (runs every 500ms while on foot or in vehicle)
		if (m_pPlayerPed->IsAdded() && Settings::headMove()
				&& !m_bIsSpectating && !EditObject::m_visible
				&& m_pPlayerPed->GetStuff() == STUFF_TYPE_NONE
				&& (wKeys & 0x84) == 0
				&& m_pPlayerPed->IsEnteringVehicle() == 0 && !m_pPlayerPed->IsExitingVehicle()
				&& (dwThisTick - m_dwLastHeadUpdate) > 500) {
			CAMERA_AIM* Aim = GameGetInternalAim();
			if (Aim && m_pPlayerPed->m_ped) {
				RwMatrix matPed = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
				sa::CVector LookAt;
				LookAt.x = matPed.pos.x + (Aim->f1x * 20.0f);
				LookAt.y = matPed.pos.y + (Aim->f1y * 20.0f);
				LookAt.z = matPed.pos.z + (Aim->f1z * 20.0f);
				m_pPlayerPed->ApplyCommandTask("FollowPedSA", nullptr, 2000, -1, &LookAt, 0, 0.1f, 500, 3, 0);
				m_dwLastHeadUpdate = dwThisTick;
			}
		}

		// PLAYER DATA UPDATES
		if (m_bIsSpectating) {
			if (m_surfSync.bIsActive) DisableSurfing();
			ProcessSpectating();
			m_bPassengerDriveByMode = false;
		}
			// DRIVER CONDITIONS
		else if (m_pPlayerPed->GetActionTrigger() == ACTION_INCAR && !m_pPlayerPed->IsAPassenger()) {
			if (m_surfSync.bIsActive) DisableSurfing();
			// g_bLockEnterVehicleWidget = false;

			m_pPlayerPed->RemoveWeaponWhenEnteringVehicle();

			ProcessInCarWorldBounds();

			if ((dwThisTick - m_dwLastSendAimSyncTick) > 1000) {
				m_dwLastSendAimSyncTick = dwThisTick;
				SendAimSyncData();
			}

			CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
			CVehicle* pVehicle = nullptr;

			if (pVehiclePool) {
				m_CurrentVehicle = pVehiclePool->FindIDFromGtaPtr(m_pPlayerPed->GetGtaVehicle());
			}

			UpdateVehicleDamage(m_CurrentVehicle);

			if (pVehiclePool) {
				pVehicle = pVehiclePool->GetAt(m_CurrentVehicle);
			}
			if (pVehicle && !m_bInRCMode && pVehicle->IsRCVehicle()) {
				m_pPlayerPed->Remove();
				m_bInRCMode = true;
			}

			if (m_bInRCMode && !pVehicle) {
				m_pPlayerPed->SetHealth(0.0f);
				m_pPlayerPed->SetDead();
			}

			if (m_bInRCMode && pVehicle && pVehicle->GetHealth() == 0.0f) {
				m_pPlayerPed->SetHealth(0.0f);
				m_pPlayerPed->SetDead();
			}

			if (CompareInCarSyncKeys(wKeys, lrAnalog, udAnalog) || (dwThisTick - m_dwLastSendTick) > GetOptimumInCarSendRate()) {
				m_dwLastSendTick = GetTickCount();
				SendInCarFullSyncData();
			}

			m_bPassengerDriveByMode = false;
		}
			// ONFOOT CONDITIONS
		else if (m_pPlayerPed->GetActionTrigger() == ACTION_NORMAL || m_pPlayerPed->GetActionTrigger() == ACTION_SCOPE) {
			// g_bLockEnterVehicleWidget = true;

			ProcessSurfing();
			UpdateSurfing();

			if (m_bInRCMode) {
				m_bInRCMode = false;
				m_pPlayerPed->Add();
			}

			ProcessOnFootWorldBounds();

			if (m_CurrentVehicle != 0xFFFF) {
				m_LastVehicle = m_CurrentVehicle;
				m_CurrentVehicle = 0xFFFF;
			}

			if (CompareOnFootSyncKeys(wKeys, lrAnalog, udAnalog) || (dwThisTick - m_dwLastSendTick) > GetOptimumOnFootSendRate()) {
				m_dwLastSendTick = GetTickCount();
				SendOnFootFullSyncData();
			}

			// Broadcast camera mode transitions (4 <-> 53/7/8/51/46) immediately in lockstep
			// with OnFootSync KEY_HANDBRAKE so PC & Mobile observers never see un-aimed sliding.
			bool bWannaSync = (m_pPlayerPed->GetCameraMode() != m_aimSync.byteCamMode);
			if (!bWannaSync) {
				if (wKeys & 0x84) {
					if (pNetGame->session().lagCompensationMode == 2) {
						bWannaSync = (dwThisTick - m_dwLastSendAimSyncTick) > pNetGame->session().firingSendRate;
					}
					else {
						bWannaSync = (dwThisTick - m_dwLastSendAimSyncTick) > 100;
					}
				}
				else {
					bWannaSync = (dwThisTick - m_dwLastSendAimSyncTick) > 1000;
				}
			}

			if (bWannaSync) {
				m_dwLastSendAimSyncTick = dwThisTick;
				SendAimSyncData();
			}

			m_bPassengerDriveByMode = false;
		}
			// PASSENGER CONDITIONS
		else if (m_pPlayerPed->GetActionTrigger() == ACTION_INCAR && m_pPlayerPed->IsAPassenger()) {
			// g_bLockEnterVehicleWidget = false;

			if (m_bInRCMode) {
				m_bInRCMode = false;
				m_pPlayerPed->Add();
			}

			// FOR ENTERING PASSENGER DRIVEBY MODE
			const bool bToggleDriveBy = LocalPlayerKeys.bKeys[ePadKeys::KEY_CTRL_BACK] || ConsumePassengerDriveByToggle();
			if (!m_bPassengerDriveByMode && bToggleDriveBy) {
				if (m_pPlayerPed->StartPassengerDriveByMode()) {
					m_bPassengerDriveByMode = true;
				}
			}
			else if (m_bPassengerDriveByMode && bToggleDriveBy) {
				m_pPlayerPed->StopPassengerDriveByMode();
				m_bPassengerDriveByMode = false;
			}

			if (m_bPassengerDriveByMode) {
				sa::CWeapon* pWeapSlot = m_pPlayerPed->GetCurrentWeaponSlot();
				if (m_pPlayerPed->GetCurrentWeapon() == 0 || !pWeapSlot || pWeapSlot->dwAmmo == 0) {
					m_pPlayerPed->StopPassengerDriveByMode();
					m_bPassengerDriveByMode = false;
				}
			}

			if (dwThisTick - m_dwLastSendTick > GetOptimumInCarSendRate()) {
				m_dwLastSendTick = GetTickCount();
				SendPassengerFullSyncData();
			}
		}
	}

	// Matches PC localplayer.cpp: whenever inactive (spectating / wasted / class selection),
	// immediately reset drunk level & clear drunk visuals/HUD override.
	if (!m_bIsActive && m_pPlayerPed && m_pPlayerPed->GetDrunkLevel() > 0) {
		m_pPlayerPed->SetDrunkLevel(0);
		m_pPlayerPed->DrunkProcess();
	}

	// HANDLE !IsActive spectating
	if (m_bIsSpectating && !m_bIsActive) {
		if (m_bSpawnDialogShowed) {
			m_bSpawnDialogShowed = false;
			if (pUI) {
				pUI->spawn()->setVisible(false);
			}
		}

		ProcessSpectating();
		return true;
	}

	// HANDLE NEEDS TO RESPAWN AFTER DEATH
	if (m_bIsWasted && m_pPlayerPed->GetActionTrigger() != ACTION_WASTED && m_pPlayerPed->GetActionTrigger() != ACTION_DEATH) {
		if (m_pPlayerPed->HasAttachedObject()) {
			m_pPlayerPed->RemoveAllAttachedObjects();
		}

		if (IsClearedToSpawn() && !m_bWantsAnotherClass && pNetGame->GetGameState() == GAMESTATE_CONNECTED) {
			if (m_pPlayerPed->GetHealth() > 0.0f) {
				Spawn();
			}
		}
		else {
			m_bIsWasted = false;
			m_bSpawnDialogShowed = false;
			HandleClassSelection();
			m_bWantsAnotherClass = false;
		}

		return true;
	}

	if (m_pPlayerPed->GetActionTrigger() != ACTION_WASTED && m_pPlayerPed->GetActionTrigger() != ACTION_DEATH && pNetGame->GetGameState() == GAMESTATE_CONNECTED && !m_bIsActive && !m_bIsSpectating) {
		ProcessClassSelection();
	}

	return true;
}

void CLocalPlayer::ProcessClassSelection()
{
	if (!m_bSpawnDialogShowed) {
		if (pUI) {
			pUI->spawn()->setVisible(true);
		}
		RequestClass(m_iSelectedClass);
		m_bSpawnDialogShowed = true;
	}
}

void CLocalPlayer::ResetAllSyncAttributes()
{
	m_bHasSpawnInfo = false;
	m_iCustomSpawnSkin = 0;
	m_bWantsAnotherClass = false;
	m_bWaitingForSpawnRequestReply = false;
	m_bSpawnDialogShowed = false;
	m_byteTeam = NO_TEAM;
	m_iSelectedClass = 0;
	m_byteCurInterior = 0;
	m_LastVehicle = 0xFFFF;
	m_bInRCMode = false;
	m_lastTargetPlayerId = INVALID_PLAYER_ID;
	m_lastTargetActorId = INVALID_PLAYER_ID;
	DisableSurfing();

	memset(&m_ofSync, 0, sizeof(ONFOOT_SYNC_DATA));
	memset(&m_icSync, 0, sizeof(INCAR_SYNC_DATA));
	memset(&m_psSync, 0, sizeof(PASSENGER_SYNC_DATA));
	memset(&m_trSync, 0, sizeof(TRAILER_SYNC_DATA));
	memset(&m_ucSync, 0, sizeof(UNOCCUPIED_SYNC_DATA));
	memset(&m_aimSync, 0, sizeof(AIM_SYNC_DATA));
	memset(&m_statsSync, 0, sizeof(m_statsSync));
	memset(m_byteLastWeapon, 0, sizeof(m_byteLastWeapon));
	memset(m_dwLastAmmo, 0, sizeof(m_dwLastAmmo));
	for (auto& target : m_wCameraTargets) {
		target = 0xFFFF;
	}

	m_dwLastWeaponsUpdateTick = GetTickCount();
	m_byteCurrentWeapon = 0;

	m_CurrentVehicle = INVALID_VEHICLE_ID;
	m_DamageVehicle = INVALID_VEHICLE_ID;
	m_dwDamagePanels = 0;
	m_dwDamageDoors = 0;
	m_byteDamageLights = 0;
	m_byteDamageTires = 0;
}

void CLocalPlayer::ToggleSpectating(bool bToggle)
{
	if (m_bIsSpectating && !m_bWantsAnotherClass) {
		if (!bToggle) {
			Spawn();
		}
	}
	else if (m_bWantsAnotherClass && !bToggle) {
		m_bIsActive = false;
		m_bHasSpawnInfo = false;
		m_bClearedToSpawn = false;
		m_bSpawnDialogShowed = false;
		m_bWantsAnotherClass = false;
	}

	m_bIsSpectating = bToggle;
	if (pGame) {
		pGame->SetHudHidden(Game::HUD_HIDE_SPECTATE, bToggle);
	}
	if (bToggle && m_surfSync.bIsActive) {
		DisableSurfing();
	}
	m_byteSpectateType = SPECTATE_TYPE_NONE;
	m_bSpectateProcessed = false;
	m_SpectateID = 0xFFFFFFFF;
}

void CLocalPlayer::ProcessSpectating()
{
	if (!m_pPlayerPed || !pGame || !pNetGame) return;

	RakNet::BitStream bsSpectatorSync;
	SPECTATOR_SYNC_DATA spSync{};
	RwMatrix matPos{};

	pGame->GetCamera()->GetMatrix(&matPos);

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();

	if (!pPlayerPool || !pVehiclePool) {
		return;
	}

	spSync.vecPos.x = matPos.pos.x;
	spSync.vecPos.y = matPos.pos.y;
	spSync.vecPos.z = matPos.pos.z;

	if ((GetTickCount() - m_dwLastSendSpecTick) > GetOptimumOnFootSendRate()) {
		uint16_t lrAnalog = 0, udAnalog = 0;
		uint8_t exKeys = 0;
		uint16_t wKeys = m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, &exKeys);
		spSync.lrAnalog = lrAnalog;
		spSync.udAnalog = udAnalog;
		spSync.wKeys = wKeys;

		m_dwLastSendSpecTick = GetTickCount();
		proto::OutboundSpectatorSync pkt{};
		pkt.lrAnalog = spSync.lrAnalog;
		pkt.udAnalog = spSync.udAnalog;
		pkt.keys = spSync.wKeys;
		pkt.position = {spSync.vecPos.x, spSync.vecPos.y, spSync.vecPos.z};
		proto::Writer w(bsSpectatorSync);
		pkt.serialize(w, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->Send(&bsSpectatorSync, HIGH_PRIORITY, UNRELIABLE, 0);

		if ((GetTickCount() - m_dwLastSendAimSyncTick) > (GetOptimumOnFootSendRate() * 2)) {
			m_dwLastSendAimSyncTick = GetTickCount();
			SendAimSyncData();
		}
	}

	pGame->DisplayHUD(false);

	m_pPlayerPed->SetHealth(100.0f);
	GetPlayerPed()->TeleportTo(spSync.vecPos.x, spSync.vecPos.y, spSync.vecPos.z + 20.0f);

	// handle spectate player left the server or is no longer active (ie Died)
	if (m_byteSpectateType == SPECTATE_TYPE_PLAYER) {
		CRemotePlayer* pSpecPlayer = (m_SpectateID < MAX_PLAYERS) ? pPlayerPool->GetAt((PLAYERID) m_SpectateID) : nullptr;
		if (!pSpecPlayer || !pSpecPlayer->IsActive() || pSpecPlayer->GetState() == PLAYER_STATE_WASTED) {
			m_byteSpectateType = SPECTATE_TYPE_NONE;
			m_bSpectateProcessed = false;
		}
	}

	if (m_bSpectateProcessed) {
		return;
	}

	if (m_byteSpectateType == SPECTATE_TYPE_NONE) {
		GetPlayerPed()->RemoveFromVehicleAndPutAt(0.0f, 0.0f, 10.0f);
		pGame->GetCamera()->SetPosition(50.0f, 50.0f, 50.0f, 0.0f, 0.0f, 0.0f);
		pGame->GetCamera()->LookAtPoint(60.0f, 60.0f, 50.0f, 2);
		m_bSpectateProcessed = true;
	}
	else if (m_byteSpectateType == SPECTATE_TYPE_PLAYER) {
		CRemotePlayer* pSpecPlayer = (m_SpectateID < MAX_PLAYERS) ? pPlayerPool->GetAt((PLAYERID) m_SpectateID) : nullptr;
		if (pSpecPlayer) {
			CPlayerPed* pPlayerPed = pSpecPlayer->GetPlayerPed();
			if (pPlayerPed) {
				ScriptCommand(&camera_on_actor, pPlayerPed->m_gtaId, m_byteSpectateMode, 2);
				m_bSpectateProcessed = true;
			}
		}
	}
	else if (m_byteSpectateType == SPECTATE_TYPE_VEHICLE) {
		CVehicle* pVehicle = (m_SpectateID < MAX_VEHICLES) ? pVehiclePool->GetAt((VEHICLEID) m_SpectateID) : nullptr;
		if (pVehicle) {
			ScriptCommand(&camera_on_vehicle, pVehicle->m_gtaId, m_byteSpectateMode, 2);
			m_bSpectateProcessed = true;
		}
	}
}

bool CLocalPlayer::Spawn()
{
	if (!m_bHasSpawnInfo) {
		return false;
	}

	if (m_bSpawnDialogShowed) {
		m_bSpawnDialogShowed = false;
		if (pUI) {
			pUI->spawn()->setVisible(false);
		}
	}

	spdlog::info("Spawn localplayer");
	DisableSurfing();

	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets || !widgets[sa::WidgetIDs::WIDGET_PED_MOVE] || !widgets[sa::WidgetIDs::WIDGET_ATTACK]) {
		spdlog::info("Widgets not inited, calling Menu_SwitchOffToGame");
		eng::fn::Menu_SwitchOffToGame();
	}
	EnsureTouchWidgetsCreated();

	pGame->GetCamera()->Restore();
	pGame->GetCamera()->SetBehindPlayer();
	Settings::applyToGtaEngine(true);
	pGame->SetHudHidden(Game::HUD_HIDE_CLASS_SELECT | Game::HUD_HIDE_SPECTATE, false);
	pGame->DisplayHUD(true);
	m_pPlayerPed->TogglePlayerControllable(true);

	if (!bFirstSpawn) {
		if (m_pPlayerPed->IsInVehicle()) {
			m_pPlayerPed->RemoveFromVehicleAndPutAt(
				m_SpawnInfo.vecPos.x, m_SpawnInfo.vecPos.y, m_SpawnInfo.vecPos.z + 0.5f);
		}
		m_pPlayerPed->SetInitialState();
	}
	else {
		bFirstSpawn = false;
	}

	pGame->RefreshStreamingAt(m_SpawnInfo.vecPos.x, m_SpawnInfo.vecPos.y);

	if (m_pPlayerPed->IsCuffed()) {
		m_pPlayerPed->ToggleCuffed(false);
	}

	m_pPlayerPed->RestartIfWastedAt(&m_SpawnInfo.vecPos, m_SpawnInfo.fRotation);
	m_pPlayerPed->SetModelIndex(static_cast<uint>(ResolveEffectivePedSkin(m_SpawnInfo.iSkin, m_iCustomSpawnSkin)));
	m_pPlayerPed->ClearWeapons();
	m_pPlayerPed->ResetDamageEntity();

	ApplySpecialAction(0);

	if (m_SpawnInfo.iSpawnWeapons[2] != -1) {
		m_pPlayerPed->GiveWeapon(m_SpawnInfo.iSpawnWeapons[2], m_SpawnInfo.iSpawnWeaponsAmmo[2]);
	}

	if (m_SpawnInfo.iSpawnWeapons[1] != -1) {
		m_pPlayerPed->GiveWeapon(m_SpawnInfo.iSpawnWeapons[1], m_SpawnInfo.iSpawnWeaponsAmmo[1]);
	}

	if (m_SpawnInfo.iSpawnWeapons[0] != -1) {
		m_pPlayerPed->GiveWeapon(m_SpawnInfo.iSpawnWeapons[0], m_SpawnInfo.iSpawnWeaponsAmmo[0]);
	}

	pGame->DisableTrainTraffic();

	m_pPlayerPed->TeleportTo(m_SpawnInfo.vecPos.x, m_SpawnInfo.vecPos.y, m_SpawnInfo.vecPos.z + 0.5f);
	m_pPlayerPed->SetTargetRotation(m_SpawnInfo.fRotation);

	m_bIsWasted = false;
	m_bIsActive = true;
	m_bWaitingForSpawnRequestReply = false;

	m_iDisplayZoneTick = GetTickCount() + 1000;

	m_surfSync.bIsActive = false;
	ResetLocalPedQueuedAnim();

	RakNet::BitStream bsSendSpawn;
	proto::SpawnReq req{};
	proto::Writer w(bsSendSpawn);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_Spawn, &bsSendSpawn, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
	return true;
}

void CLocalPlayer::HandleClassSelection()
{
	m_bClearedToSpawn = false;
	if (pGame) {
		pGame->SetHudHidden(Game::HUD_HIDE_CLASS_SELECT, true);
	}
	if (m_pPlayerPed) {
		m_pPlayerPed->SetInitialState();
		m_pPlayerPed->SetHealth(100.0f);
		m_pPlayerPed->TogglePlayerControllable(false);
	}
}

void CLocalPlayer::SendWastedNotification()
{
	uint8_t byteDeathReason;
	PLAYERID WhoWasResponsible;
	RakNet::BitStream bsPlayerDeath;
	byteDeathReason = m_pPlayerPed->FindDeathReasonAndResponsiblePlayer(&WhoWasResponsible);
	proto::DeathReq req{};
	req.reason = byteDeathReason;
	req.killerId = WhoWasResponsible;
	proto::Writer w(bsPlayerDeath);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_Death, &bsPlayerDeath, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::SendOnFootFullSyncData()
{
	RakNet::BitStream bsPlayerSync;
	RwMatrix matPlayer;
	sa::CVector vecMoveSpeed;
	uint16_t lrAnalog, udAnalog;
	uint8_t exKeys;
	uint16_t wKeys = m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, &exKeys);
	ApplyEffectiveLocalOnFootKeys(m_pPlayerPed, wKeys, lrAnalog, udAnalog);

	ONFOOT_SYNC_DATA ofSync{};

	matPlayer = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
	vecMoveSpeed = m_pPlayerPed->m_ped->GetMoveSpeed();

	ofSync.lrAnalog = lrAnalog;
	ofSync.udAnalog = udAnalog;
	ofSync.wKeys = wKeys;

	ofSync.vecPos.x = matPlayer.pos.x;
	ofSync.vecPos.y = matPlayer.pos.y;
	ofSync.vecPos.z = matPlayer.pos.z;

	ofSync.quat.SetFromMatrix(&matPlayer);
	ofSync.quat.Normalize();

	if (FloatOffset(ofSync.quat.w, m_ofSync.quat.w) < 0.00001 &&
			FloatOffset(ofSync.quat.x, m_ofSync.quat.x) < 0.00001 &&
			FloatOffset(ofSync.quat.y, m_ofSync.quat.y) < 0.00001 &&
			FloatOffset(ofSync.quat.z, m_ofSync.quat.z) < 0.00001) {
		ofSync.quat.Set(m_ofSync.quat);
	}

	ofSync.byteHealth = (uint8_t) m_pPlayerPed->GetHealth();
	ofSync.byteArmour = (uint8_t) m_pPlayerPed->GetArmour();

	ofSync.byteCurrentWeapon = (exKeys << 6) | (ofSync.byteCurrentWeapon & 0x3F);
	ofSync.byteCurrentWeapon ^= (ofSync.byteCurrentWeapon ^ m_pPlayerPed->GetCurrentWeapon()) & 0x3F;
	ofSync.byteSpecialAction = GetSpecialAction();

	ofSync.vecMoveSpeed.x = vecMoveSpeed.x;
	ofSync.vecMoveSpeed.y = vecMoveSpeed.y;
	ofSync.vecMoveSpeed.z = vecMoveSpeed.z;

	ofSync.vecSurfOffsets.x = 0.0f;
	ofSync.vecSurfOffsets.y = 0.0f;
	ofSync.vecSurfOffsets.z = 0.0f;
	ofSync.wSurfInfo = 0;
	if (m_surfSync.bIsActive) {
		ofSync.vecSurfOffsets.x = m_surfSync.vecOffsetPos.x;
		ofSync.vecSurfOffsets.y = m_surfSync.vecOffsetPos.y;
		ofSync.vecSurfOffsets.z = m_surfSync.vecOffsetPos.z;
		if (m_surfSync.bIsVehicle) {
			ofSync.wSurfInfo = (uint16_t) m_surfSync.dwSurfVehID;
		}
		else {
			// object surfing is sent as objectId + MAX_VEHICLES
			ofSync.wSurfInfo = (uint16_t) (m_surfSync.dwSurfVehID + MAX_VEHICLES);
		}
	}

	ofSync.dwAnimation = m_pPlayerPed->GetCurrentAnimationIndexFlag();

	if ((GetTickCount() - m_dwLastOnFootSyncTick) > 500 || memcmp(&m_ofSync, &ofSync, sizeof(ONFOOT_SYNC_DATA))) {
		m_dwLastOnFootSyncTick = GetTickCount();

		proto::OutboundPlayerSync pkt{};
		pkt.lrAnalog = ofSync.lrAnalog;
		pkt.udAnalog = ofSync.udAnalog;
		pkt.keys = ofSync.wKeys;
		pkt.position = {ofSync.vecPos.x, ofSync.vecPos.y, ofSync.vecPos.z};
		pkt.quat = {ofSync.quat.w, ofSync.quat.x, ofSync.quat.y, ofSync.quat.z};
		pkt.health = ofSync.byteHealth;
		pkt.armour = ofSync.byteArmour;
		pkt.currentWeapon = ofSync.byteCurrentWeapon;
		pkt.specialAction = ofSync.byteSpecialAction;
		pkt.moveSpeed = {ofSync.vecMoveSpeed.x, ofSync.vecMoveSpeed.y, ofSync.vecMoveSpeed.z};
		pkt.surfOffsets = {ofSync.vecSurfOffsets.x, ofSync.vecSurfOffsets.y, ofSync.vecSurfOffsets.z};
		pkt.surfInfo = ofSync.wSurfInfo;
		pkt.animation = ofSync.dwAnimation;

		proto::Writer w(bsPlayerSync);
		pkt.serialize(w, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->Send(&bsPlayerSync, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 1);
		memcpy(&m_ofSync, &ofSync, sizeof(ONFOOT_SYNC_DATA));
	}
}

void CLocalPlayer::SendInCarFullSyncData()
{
	RakNet::BitStream bsVehicleSync;
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	CPlayerPed* pPlayerPed = m_pPlayerPed;

	if (!pPlayerPed || !pVehiclePool) {
		return;
	}

	uint16_t lrAnalog, udAnalog;
	uint8_t exKeys;
	uint16_t wKeys = m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, &exKeys);

	INCAR_SYNC_DATA icSync;
	memset(&icSync, 0, sizeof(INCAR_SYNC_DATA));

	sa::CVehicle* pGtaVehicle = pPlayerPed->GetGtaVehicle();
	icSync.VehicleID = pVehiclePool->FindIDFromGtaPtr(pGtaVehicle);

	if (icSync.VehicleID == INVALID_VEHICLE_ID ||
			icSync.VehicleID >= MAX_VEHICLES) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(icSync.VehicleID);
	if (!pVehicle || !pVehicle->m_pVehicle || !pVehicle->VerifyInstance()) {
		return;
	}

	icSync.lrAnalog = lrAnalog;
	icSync.udAnalog = udAnalog;
	icSync.wKeys = wKeys;

	RwMatrix mat = pVehicle->m_pVehicle->GetMatrix().ToRwMatrix();
	sa::CVector vecMoveSpeed = pVehicle->m_pVehicle->GetMoveSpeed();

	icSync.quat.SetFromMatrix(&mat);
	icSync.quat.Normalize();

	if (FloatOffset(icSync.quat.w, m_icSync.quat.w) < 0.00001f
			&& FloatOffset(icSync.quat.x, m_icSync.quat.x) < 0.00001f
			&& FloatOffset(icSync.quat.y, m_icSync.quat.y) < 0.00001f
			&& FloatOffset(icSync.quat.z, m_icSync.quat.z) < 0.00001f) {
		icSync.quat.Set(m_icSync.quat);
	}

	icSync.vecPos.x = mat.pos.x;
	icSync.vecPos.y = mat.pos.y;
	icSync.vecPos.z = mat.pos.z;

	icSync.vecMoveSpeed.x = vecMoveSpeed.x;
	icSync.vecMoveSpeed.y = vecMoveSpeed.y;
	icSync.vecMoveSpeed.z = vecMoveSpeed.z;

	icSync.fCarHealth = pVehicle->GetHealth();
	icSync.bytePlayerHealth = m_pPlayerPed->GetHealth();
	icSync.bytePlayerArmour = m_pPlayerPed->GetArmour();

	icSync.byteCurrentWeapon = (exKeys << 6) | (icSync.byteCurrentWeapon & 0x3F);
	icSync.byteCurrentWeapon ^=
			(icSync.byteCurrentWeapon ^ m_pPlayerPed->GetCurrentWeapon()) & 0x3F;
	icSync.TrailerID = 0;

	CVehicle* pTrailer = pVehicle->GetTrailer();
	if (pTrailer && pTrailer->m_pVehicle && pTrailer->GetTractor() == pVehicle) {
		pVehicle->SetTrailer(pTrailer);
		icSync.TrailerID = pVehiclePool->FindIDFromGtaPtr(pTrailer->m_pVehicle);
	}
	else {
		pVehicle->SetTrailer(nullptr);
	}

	if (pVehicle->GetModelIndex() == TRAIN_PASSENGER_LOCO || pVehicle->GetModelIndex() == TRAIN_FREIGHT_LOCO || pVehicle->GetModelIndex() == TRAIN_TRAM) {
		icSync.fTrainSpeed = pVehicle->GetTrainSpeed();
	}
	else if (pVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_BIKE || pVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_PUSHBIKE) {
		icSync.fTrainSpeed = pVehicle->GetBikeLean();
	}
	else if (pVehicle->GetModelIndex() == HYDRA) {
		*(uint32_t*) icSync.sHydraThrustAngle = pVehicle->GetHydraThrusters();
	}
	else {
		icSync.fTrainSpeed = 0.0f;
	}

	icSync.byteSirenOn = pVehicle->IsSirenOn();
	icSync.byteLandingGearState = pVehicle->GetLandingGearState();

	if (IsNeedSyncDataSend(&m_icSync, &icSync, sizeof(INCAR_SYNC_DATA), m_dwLastInCarSyncTick)) {
		proto::OutboundVehicleSync pkt{};
		pkt.vehicleId = icSync.VehicleID;
		pkt.lrAnalog = icSync.lrAnalog;
		pkt.udAnalog = icSync.udAnalog;
		pkt.keys = icSync.wKeys;
		pkt.quat = {icSync.quat.w, icSync.quat.x, icSync.quat.y, icSync.quat.z};
		pkt.position = {icSync.vecPos.x, icSync.vecPos.y, icSync.vecPos.z};
		pkt.moveSpeed = {icSync.vecMoveSpeed.x, icSync.vecMoveSpeed.y, icSync.vecMoveSpeed.z};
		pkt.carHealth = icSync.fCarHealth;
		pkt.playerHealth = icSync.bytePlayerHealth;
		pkt.playerArmour = icSync.bytePlayerArmour;
		pkt.currentWeapon = icSync.byteCurrentWeapon;
		pkt.sirenOn = icSync.byteSirenOn;
		pkt.landingGearState = icSync.byteLandingGearState;
		pkt.trailerId = icSync.TrailerID;
		memcpy(&pkt.trainOrHydra, &icSync.fTrainSpeed, sizeof(pkt.trainOrHydra));

		proto::Writer w(bsVehicleSync);
		pkt.serialize(w, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->Send(&bsVehicleSync, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, true);

		memcpy(&m_icSync, &icSync, sizeof(INCAR_SYNC_DATA));
	}

	if (icSync.TrailerID && icSync.TrailerID < MAX_VEHICLES) {
		SendTrailerFullSyncData(icSync.TrailerID);
	}

	if (pVehicle->HasTurret() || (GetTickCount() - m_dwLastSendAimSyncTick) > 1000) {
		m_dwLastSendAimSyncTick = GetTickCount();
		SendAimSyncData();
	}
}

void CLocalPlayer::SendPassengerFullSyncData()
{
	RakNet::BitStream bsData;
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool || !m_pPlayerPed || !m_pPlayerPed->m_ped) {
		return;
	}
	uint16_t lrAnalog, udAnalog;
	uint8_t exKeys;
	uint16_t wkeys = m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, &exKeys);

	PASSENGER_SYNC_DATA psSync;
	memset(&psSync, 0, sizeof(PASSENGER_SYNC_DATA));

	sa::CVehicle* pGtaVehicle = m_pPlayerPed->GetGtaVehicle();
	psSync.VehicleID = pVehiclePool->FindIDFromGtaPtr(pGtaVehicle);
	if (psSync.VehicleID == INVALID_VEHICLE_ID) {
		return;
	}

	psSync.lrAnalog = lrAnalog;
	psSync.udAnalog = udAnalog;
	psSync.wKeys = wkeys;

	psSync.bytePlayerHealth = m_pPlayerPed->GetHealth();
	psSync.bytePlayerArmour = m_pPlayerPed->GetArmour();

	psSync.byteSeatFlags ^= (psSync.byteSeatFlags ^ m_pPlayerPed->GetVehicleSeatID()) & 0x3F;
	uint8_t byteUnk = psSync.byteSeatFlags & 0x7F;
	if (m_pPlayerPed->IsCuffed()) byteUnk = psSync.byteSeatFlags | 0x80;
	psSync.byteSeatFlags = ((byteUnk ^ (m_bPassengerDriveByMode << 6)) & 0x40) ^ byteUnk;

	psSync.byteCurrentWeapon = (exKeys << 6) | (psSync.byteCurrentWeapon & 0x3F);
	psSync.byteCurrentWeapon ^= (psSync.byteCurrentWeapon ^ m_pPlayerPed->GetCurrentWeapon()) & 0x3F;

	RwMatrix mat = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
	psSync.vecPos.x = mat.pos.x;
	psSync.vecPos.y = mat.pos.y;
	psSync.vecPos.z = mat.pos.z;

	if (IsNeedSyncDataSend(&m_psSync, &psSync, sizeof(PASSENGER_SYNC_DATA), m_dwLastPassengerSyncTick)) {
		proto::OutboundPassengerSync pkt{};
		pkt.vehicleId = psSync.VehicleID;
		pkt.seatFlags = psSync.byteSeatFlags;
		pkt.currentWeapon = psSync.byteCurrentWeapon;
		pkt.playerHealth = psSync.bytePlayerHealth;
		pkt.playerArmour = psSync.bytePlayerArmour;
		pkt.lrAnalog = psSync.lrAnalog;
		pkt.udAnalog = psSync.udAnalog;
		pkt.keys = psSync.wKeys;
		pkt.position = {psSync.vecPos.x, psSync.vecPos.y, psSync.vecPos.z};

		proto::Writer w(bsData);
		pkt.serialize(w, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->Send(&bsData, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 1);
		memcpy(&m_psSync, &psSync, sizeof(PASSENGER_SYNC_DATA));
	}

	const uint32_t aimRate = m_bPassengerDriveByMode ? 100u : 1000u;
	if ((GetTickCount() - m_dwLastSendAimSyncTick) > aimRate) {
		m_dwLastSendAimSyncTick = GetTickCount();
		SendAimSyncData();
	}
}

void CLocalPlayer::SendTrailerFullSyncData(VEHICLEID vehicleId)
{
	RwMatrix matTrailer;

	TRAILER_SYNC_DATA trSync;
	memset(&trSync, 0, sizeof(TRAILER_SYNC_DATA));

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pTrailer = pVehiclePool->GetAt(vehicleId);
	if (pTrailer && pTrailer->m_pVehicle && pTrailer->VerifyInstance()) {
		matTrailer = pTrailer->m_pVehicle->GetMatrix().ToRwMatrix();

		trSync.quat.SetFromMatrix(&matTrailer);
		trSync.quat.Normalize();

		trSync.trailerId = vehicleId;

		trSync.vecPos.x = matTrailer.pos.x;
		trSync.vecPos.y = matTrailer.pos.y;
		trSync.vecPos.z = matTrailer.pos.z;

		trSync.vecMoveSpeed = pTrailer->m_pVehicle->GetMoveSpeed();
		trSync.vecTurnSpeed = pTrailer->m_pVehicle->GetTurnSpeed();
		auto clampTurn = [](float v) -> float {
			if (!std::isfinite(v)) return 0.0f;
			return std::clamp(v, -1.0f, 1.0f);
		};
		trSync.vecTurnSpeed.x = clampTurn(trSync.vecTurnSpeed.x);
		trSync.vecTurnSpeed.y = clampTurn(trSync.vecTurnSpeed.y);
		trSync.vecTurnSpeed.z = clampTurn(trSync.vecTurnSpeed.z);

		if (IsNeedSyncDataSend(&m_trSync, &trSync, sizeof(TRAILER_SYNC_DATA), m_dwLastTrailerSyncTick)) {
			RakNet::BitStream bsTrailerSync;
			proto::OutboundTrailerSync pkt{};
			pkt.trailerId = trSync.trailerId;
			pkt.position = {trSync.vecPos.x, trSync.vecPos.y, trSync.vecPos.z};
			pkt.quat = {trSync.quat.w, trSync.quat.x, trSync.quat.y, trSync.quat.z};
			pkt.moveSpeed = {trSync.vecMoveSpeed.x, trSync.vecMoveSpeed.y, trSync.vecMoveSpeed.z};
			pkt.turnSpeed = {trSync.vecTurnSpeed.x, trSync.vecTurnSpeed.y, trSync.vecTurnSpeed.z};

			proto::Writer w(bsTrailerSync);
			pkt.serialize(w, proto::ProtoVersion::V037);
			pNetGame->GetRakClient()->Send(&bsTrailerSync, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 1);
			memcpy(&m_trSync, &trSync, sizeof(TRAILER_SYNC_DATA));
		}
	}
}

// Ported from reference CLocalPlayer::ProcessUnoccupiedSync: the local client
// claims unoccupied sync for a vehicle when it is the closest eligible player
// (or the recorded "ghost" passenger), then streams the vehicle state.
bool CLocalPlayer::ProcessUnoccupiedSync(VEHICLEID vehicleId, CVehicle* pVehicle)
{
	static const float fLocalUnoccupiedRange = 90.0f;

	if (!pNetGame || !pVehicle || !pVehicle->VerifyInstance()
			|| !m_pPlayerPed || !m_pPlayerPed->m_ped || pVehicle->IsATrainPart()) {
		return false;
	}

	sa::CVehicle* pEnt = pVehicle->m_pVehicle;

	bool bClear;
	if (pEnt->pDriver && pEnt->pDriver->m_nPedFlags.bInVehicle) {
		bClear = true;
	}
	else {
		float fDist = sa::DistanceBetweenPoints(pEnt->GetMatrix().m_pos,
				m_pPlayerPed->m_ped->GetMatrix().m_pos);
		bool bStationary = pEnt->GetMoveSpeed().Magnitude() < 0.01f
				&& pEnt->GetTurnSpeed().Magnitude() < 0.01f;
		bClear = fDist > fLocalUnoccupiedRange || bStationary || pEnt->m_pTowingVehicle != nullptr;
	}

	if (bClear) {
		pVehicle->m_bLocalUnoccupiedSync = false;
		pVehicle->m_bRemoteUnoccupiedSync = false;
		return false;
	}

	// ghost passengers: peds recorded in the seat array (player ped type) —
	// the local one claims sync authority for the vehicle
	for (int seat = 0; seat < 8; ++seat) {
		sa::CPed* pPassenger = pEnt->m_apPassengers[seat];
		if (pPassenger && pPassenger->m_nPedType == sa::PEDTYPE_PLAYER1) {
			if (pPassenger == m_pPlayerPed->m_ped) {
				SendUnoccupiedFullSyncData(vehicleId, (uint8_t)(seat + 1));
				return true;
			}
			return false;
		}
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return false;
	}

	sa::CVector vecVehiclePos = pEnt->GetMatrix().m_pos;
	PLAYERID iClosest = INVALID_PLAYER_ID;
	int iLargest = pPlayerPool->m_iLastPlayerID;
	if (iLargest < pPlayerPool->m_LocalPlayerID) {
		iLargest = pPlayerPool->m_LocalPlayerID;
	}

	float fBest = 100000.0f;
	for (int index = 0; index <= iLargest; ++index) {
		sa::CPed* pPed;
		if (index == pPlayerPool->m_LocalPlayerID) {
			pPed = m_pPlayerPed->m_ped;
		}
		else {
			CRemotePlayer* pRemote = pPlayerPool->GetAt((PLAYERID) index);
			if (!pRemote || !pRemote->GetPlayerPed()) {
				continue;
			}
			pPed = pRemote->GetPlayerPed()->m_ped;
		}
		if (!pPed) {
			continue;
		}
		float fDist = sa::DistanceBetweenPoints(pPed->GetMatrix().m_pos, vecVehiclePos);
		if (iClosest == INVALID_PLAYER_ID || fDist < fBest) {
			fBest = fDist;
			iClosest = (PLAYERID) index;
		}
	}

	if (iClosest == pPlayerPool->m_LocalPlayerID && fBest <= fLocalUnoccupiedRange) {
		SendUnoccupiedFullSyncData(vehicleId, 0);
		return true;
	}

	return false;
}

void CLocalPlayer::SendUnoccupiedFullSyncData(VEHICLEID vehicleId, uint8_t byteSeat)
{
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(vehicleId);
	if (!pVehicle || !pVehicle->VerifyInstance()) {
		return;
	}

	sa::CVehicle* pEnt = pVehicle->m_pVehicle;
	sa::CMatrix mat = pEnt->GetMatrix();

	// Orthonormalize vecRoll (right) & vecDirection (forward) in double precision
	// so open.mp's strict unit-length & orthogonality checks never drop the packet.
	double rx = mat.m_right.x, ry = mat.m_right.y, rz = mat.m_right.z;
	double rLen = std::sqrt(rx * rx + ry * ry + rz * rz);
	if (rLen > 1e-6 && std::isfinite(rLen)) {
		rx /= rLen; ry /= rLen; rz /= rLen;
	}
	else {
		rx = 1.0; ry = 0.0; rz = 0.0;
	}

	double fx = mat.m_forward.x, fy = mat.m_forward.y, fz = mat.m_forward.z;
	double dot = rx * fx + ry * fy + rz * fz;
	fx -= dot * rx; fy -= dot * ry; fz -= dot * rz;
	double fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
	if (fLen > 1e-6 && std::isfinite(fLen)) {
		fx /= fLen; fy /= fLen; fz /= fLen;
	}
	else {
		fx = 0.0; fy = 1.0; fz = 0.0;
		rx = 1.0; ry = 0.0; rz = 0.0;
	}

	auto clampTurn = [](float v) -> float {
		if (!std::isfinite(v)) return 0.0f;
		return std::clamp(v, -1.0f, 1.0f);
	};

	UNOCCUPIED_SYNC_DATA ucSync;
	memset(&ucSync, 0, sizeof(UNOCCUPIED_SYNC_DATA));
	ucSync.vehicleId = vehicleId;
	ucSync.byteSeatId = byteSeat;
	ucSync.vecRoll = sa::CVector(static_cast<float>(rx), static_cast<float>(ry), static_cast<float>(rz));
	ucSync.vecDirection = sa::CVector(static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(fz));
	ucSync.vecPos = mat.m_pos;
	ucSync.vecMoveSpeed = pEnt->GetMoveSpeed();
	ucSync.vecTurnSpeed = pEnt->GetTurnSpeed();
	ucSync.vecTurnSpeed.x = clampTurn(ucSync.vecTurnSpeed.x);
	ucSync.vecTurnSpeed.y = clampTurn(ucSync.vecTurnSpeed.y);
	ucSync.vecTurnSpeed.z = clampTurn(ucSync.vecTurnSpeed.z);
	ucSync.fCarHealth = pEnt->fHealth;

	// Per-vehicle 100ms throttle & 3/tick cap are already enforced in CVehiclePool::Process();
	// send directly matching PC localplayer.cpp:574-602 so slots aren't wasted.
	{
		RakNet::BitStream bsSend;
		proto::OutboundUnoccupiedSync pkt{};
		pkt.vehicleId = ucSync.vehicleId;
		pkt.seatId = ucSync.byteSeatId;
		pkt.roll = {ucSync.vecRoll.x, ucSync.vecRoll.y, ucSync.vecRoll.z};
		pkt.direction = {ucSync.vecDirection.x, ucSync.vecDirection.y, ucSync.vecDirection.z};
		pkt.position = {ucSync.vecPos.x, ucSync.vecPos.y, ucSync.vecPos.z};
		pkt.moveSpeed = {ucSync.vecMoveSpeed.x, ucSync.vecMoveSpeed.y, ucSync.vecMoveSpeed.z};
		pkt.turnSpeed = {ucSync.vecTurnSpeed.x, ucSync.vecTurnSpeed.y, ucSync.vecTurnSpeed.z};
		pkt.carHealth = ucSync.fCarHealth;

		proto::Writer w(bsSend);
		pkt.serialize(w, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->Send(&bsSend, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 1);
		memcpy(&m_ucSync, &ucSync, sizeof(UNOCCUPIED_SYNC_DATA));
		pVehicle->m_bLocalUnoccupiedSync = true;
	}
}

void CLocalPlayer::SendAimSyncData()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped || !pNetGame || !pNetGame->GetRakClient()) return;

	AIM_SYNC_DATA aimSync{};

	CAMERA_AIM* caAim = m_pPlayerPed->GetCurrentAim();
	if (!caAim) return;

	aimSync.byteCamMode = m_pPlayerPed->GetCameraMode();
	aimSync.vecAimf1.x = caAim->f1x;
	aimSync.vecAimf1.y = caAim->f1y;
	aimSync.vecAimf1.z = caAim->f1z;
	aimSync.vecAimPos.x = caAim->pos1x;
	aimSync.vecAimPos.y = caAim->pos1y;
	aimSync.vecAimPos.z = caAim->pos1z;

	aimSync.fAimZ = m_pPlayerPed->GetAimZ();

	aimSync.byteCamExtZoom = (uint8_t) (m_pPlayerPed->GetCameraExtendedZoom() * 63.0f) & 0x3F;
	aimSync.byteAspectRatio = (uint8_t) (GameGetAspectRatio() * 255.0f);

	sa::CWeapon* pwstWeapon = m_pPlayerPed->GetCurrentWeaponSlot();
	if (pwstWeapon) {
		if (pwstWeapon->dwState == 2) {
			aimSync.byteWeaponState = WS_RELOADING;
		}
		else {
			aimSync.byteWeaponState = (pwstWeapon->dwAmmoInClip > 1) ? WS_MORE_BULLETS : pwstWeapon->dwAmmoInClip;
		}
	}

	if ((GetTickCount() - m_dwLastAimDataSyncTick) > 500 || memcmp(&m_aimSync, &aimSync, sizeof(AIM_SYNC_DATA))) {
		m_dwLastAimDataSyncTick = GetTickCount();
		RakNet::BitStream bsAimSync;
		proto::OutboundAimSync pkt{};
		pkt.camMode = aimSync.byteCamMode;
		pkt.aimF1 = {aimSync.vecAimf1.x, aimSync.vecAimf1.y, aimSync.vecAimf1.z};
		pkt.aimPos = {aimSync.vecAimPos.x, aimSync.vecAimPos.y, aimSync.vecAimPos.z};
		pkt.aimZ = aimSync.fAimZ;
		pkt.zoomWeaponState = static_cast<uint8_t>(((aimSync.byteWeaponState & 0x03) << 6) | (aimSync.byteCamExtZoom & 0x3F));
		pkt.aspectRatio = aimSync.byteAspectRatio;

		proto::Writer w(bsAimSync);
		pkt.serialize(w, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->Send(&bsAimSync, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 1);
		memcpy(&m_aimSync, &aimSync, sizeof(AIM_SYNC_DATA));
	}
}

void CLocalPlayer::SendStatsUpdate()
{
	// Match PC localplayer.cpp: send unconditionally on the 1000ms cadence so an
	// unreliable drop never permanently desyncs server money/drunk/FPS tracking.
	if (!pGame || !m_pPlayerPed || !pNetGame || !pNetGame->GetRakClient()) return;

	m_statsSync.dwLastMoney = static_cast<uint32_t>(pGame->GetLocalMoney());
	m_statsSync.dwLastDrunkLevel = static_cast<uint32_t>(m_pPlayerPed->GetDrunkLevel());

	RakNet::BitStream bsStats;
	proto::OutboundStatsUpdate pkt{};
	pkt.money = pGame->GetLocalMoney();
	pkt.drunkLevel = m_pPlayerPed->GetDrunkLevel();
	proto::Writer w(bsStats);
	pkt.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->Send(&bsStats, HIGH_PRIORITY, UNRELIABLE, 0);
}

void CLocalPlayer::FindAimedTargets(PLAYERID& outPlayerId, PLAYERID& outActorId)
{
	outPlayerId = INVALID_PLAYER_ID;
	outActorId = INVALID_PLAYER_ID;

	if (!m_pPlayerPed || !m_pPlayerPed->m_ped || !pNetGame) {
		return;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pPlayerPool && !pActorPool) {
		return;
	}

	auto resolvePedTarget = [&](sa::CEntity* ent) -> bool {
		if (!ent || ent == m_pPlayerPed->m_ped) return false;
		if (pPlayerPool) {
			const PLAYERID pid = pPlayerPool->FindRemotePlayerIDFromGtaPtr(reinterpret_cast<sa::CPed*>(ent));
			if (pid != INVALID_PLAYER_ID) {
				outPlayerId = pid;
				return true;
			}
		}
		if (pActorPool) {
			const PLAYERID aid = pActorPool->FindIDFromGtaPtr(reinterpret_cast<sa::CPed*>(ent));
			if (aid != INVALID_PLAYER_ID) {
				outActorId = aid;
				return true;
			}
		}
		return false;
	};

	// 1) Engine lock-on target if available (auto-aim / melee lock)
	if (m_pPlayerPed->m_ped->m_pEntLockOnTarget && resolvePedTarget(m_pPlayerPed->m_ped->m_pEntLockOnTarget)) {
		return;
	}

	// 2) Free-aim camera ray when actively aiming a weapon
	const uint8_t camMode = m_pPlayerPed->GetCameraMode();
	const bool aimCam = (camMode == 7 || camMode == 8 || camMode == 46 || camMode == 51 ||
		camMode == 53 || camMode == 55 || camMode == 65);
	const bool isAiming = aimCam || m_pPlayerPed->m_ped->m_nPedFlags.bIsAimingGun ||
		(!m_pPlayerPed->IsInVehicle() && LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE]);
	if (!isAiming) {
		return;
	}

	CAMERA_AIM* pCam = GameGetInternalAim();
	if (!pCam) {
		return;
	}

	const float lenSq = pCam->f1x * pCam->f1x + pCam->f1y * pCam->f1y + pCam->f1z * pCam->f1z;
	if (lenSq < 1e-6f) {
		return;
	}
	const float invLen = 1.0f / std::sqrt(lenSq);
	const float dx = pCam->f1x * invLen;
	const float dy = pCam->f1y * invLen;
	const float dz = pCam->f1z * invLen;

	constexpr float kMaxRange = 70.0f;
	sa::CVector vecStart{pCam->pos1x + dx * 1.2f, pCam->pos1y + dy * 1.2f, pCam->pos1z + dz * 1.2f};
	sa::CVector vecEnd{pCam->pos1x + dx * kMaxRange, pCam->pos1y + dy * kMaxRange, pCam->pos1z + dz * kMaxRange};

	sa::CColPoint colPoint{};
	sa::CEntity* hitEntity = nullptr;
	ProcessLineOfSight(&vecStart, &vecEnd, &colPoint, &hitEntity,
		true, true, true, true, false, false, false, false);
	if (hitEntity && resolvePedTarget(hitEntity)) {
		return;
	}

	// 3) Crosshair cone fallback with building/object LOS check (covers free-aim edge misses)
	float bestT = kMaxRange;
	constexpr float kMaxRadiusSq = 0.70f * 0.70f;

	auto testPedCandidate = [&](sa::CPed* ped, PLAYERID id, bool isActor) {
		if (!ped || ped == m_pPlayerPed->m_ped) return;
		const sa::CVector pos = ped->GetPosition();
		const float rx = pos.x - pCam->pos1x;
		const float ry = pos.y - pCam->pos1y;
		const float rz = (pos.z + 0.2f) - pCam->pos1z;
		const float t = rx * dx + ry * dy + rz * dz;
		if (t < 1.0f || t >= bestT) return;
		const float px = rx - t * dx;
		const float py = ry - t * dy;
		const float pz = rz - t * dz;
		if (px * px + py * py + pz * pz > kMaxRadiusSq) return;

		int clear = ScriptCommand(&get_line_of_sight,
			pCam->pos1x, pCam->pos1y, pCam->pos1z,
			pos.x, pos.y, pos.z + 0.2f,
			1, 0, 0, 1, 0);
		if (!clear) return;

		bestT = t;
		if (isActor) {
			outPlayerId = INVALID_PLAYER_ID;
			outActorId = id;
		}
		else {
			outPlayerId = id;
			outActorId = INVALID_PLAYER_ID;
		}
	};

	if (pPlayerPool) {
		for (PLAYERID i = 0; i <= pPlayerPool->m_iLastPlayerID && i < MAX_PLAYERS; ++i) {
			if (!pPlayerPool->GetSlotState(i)) continue;
			CRemotePlayer* pRemote = pPlayerPool->GetAt(i);
			if (!pRemote || !pRemote->IsActive()) continue;
			CPlayerPed* pRemotePed = pRemote->GetPlayerPed();
			if (!pRemotePed || !pRemotePed->m_ped || !pRemotePed->IsAdded()) continue;
			testPedCandidate(pRemotePed->m_ped, i, false);
		}
	}

	if (pActorPool) {
		for (PLAYERID i = 0; i < MAX_ACTORS; ++i) {
			CActor* pActor = pActorPool->GetAt(i);
			if (!pActor || !pActor->m_actor) continue;
			testPedCandidate(pActor->m_actor, i, true);
		}
	}
}

void CLocalPlayer::CheckWeapons()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped || !pNetGame || !pNetGame->GetRakClient()) {
		return;
	}

	uint8_t byteCurWeapon = m_pPlayerPed->GetCurrentWeapon();
	PLAYERID TargetPlayerID = INVALID_PLAYER_ID;
	PLAYERID TargetActorID = INVALID_PLAYER_ID;
	FindAimedTargets(TargetPlayerID, TargetActorID);

	bool bChanged = false;
	if (TargetPlayerID != m_lastTargetPlayerId || TargetActorID != m_lastTargetActorId) {
		m_lastTargetPlayerId = TargetPlayerID;
		m_lastTargetActorId = TargetActorID;
		bChanged = true;
	}
	else if (byteCurWeapon == m_byteCurrentWeapon) {
		if (GetTickCount() - m_dwLastWeaponsUpdateTick < 100) {
			return;
		}
	}

	if (byteCurWeapon != m_byteCurrentWeapon) {
		m_byteCurrentWeapon = byteCurWeapon;
	}

	proto::OutboundWeaponsUpdate pkt{};
	pkt.targetPlayerId = m_lastTargetPlayerId;
	pkt.targetActorId = m_lastTargetActorId;

	for (int i = 0; i < 13; i++) {
		if (m_byteLastWeapon[i] != m_pPlayerPed->m_ped->m_aWeapons[i].dwType || m_dwLastAmmo[i] != m_pPlayerPed->m_ped->m_aWeapons[i].dwAmmo) {
			m_byteLastWeapon[i] = m_pPlayerPed->m_ped->m_aWeapons[i].dwType;
			m_dwLastAmmo[i] = m_pPlayerPed->m_ped->m_aWeapons[i].dwAmmo;
			pkt.slots.push_back({static_cast<uint8_t>(i), m_byteLastWeapon[i], static_cast<uint16_t>(m_dwLastAmmo[i])});
			bChanged = true;
		}
	}

	if (bChanged) {
		RakNet::BitStream bsSend;
		proto::Writer w(bsSend);
		pkt.serialize(w, proto::ProtoVersion::V037);
		m_dwLastWeaponsUpdateTick = GetTickCount();
		pNetGame->GetRakClient()->Send(&bsSend, PacketPriority::HIGH_PRIORITY, UNRELIABLE, 0);
	}
}

void CLocalPlayer::UpdateRemoteInterior(uint8_t byteInterior)
{
	m_byteCurInterior = byteInterior;
	RakNet::BitStream bsUpdateInterior;
	proto::SetInteriorReq req{};
	req.interiorId = byteInterior;
	proto::Writer w(bsUpdateInterior);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_SetInteriorId, &bsUpdateInterior, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::UpdateCameraTarget()
{
	if (!pNetGame || !pNetGame->GetRakClient()) {
		return;
	}

	if (!pNetGame->session().cameraTargetEnabled) {
		m_wCameraTargets[0] = INVALID_OBJECT_ID;
		m_wCameraTargets[1] = INVALID_VEHICLE_ID;
		m_wCameraTargets[2] = INVALID_PLAYER_ID;
		m_wCameraTargets[3] = 0xFFFF;
		return;
	}

	const uint32_t now = GetTickCount();
	if (now - m_dwLastCameraTargetTick <= 100) {
		return;
	}
	m_dwLastCameraTargetTick = now;

	uint16_t targets[4] = {
		INVALID_OBJECT_ID,
		INVALID_VEHICLE_ID,
		INVALID_PLAYER_ID,
		0xFFFF
	};

	CAMERA_AIM* pCam = GameGetInternalAim();
	if (pCam) {
		const float fLenSq = pCam->f1x * pCam->f1x + pCam->f1y * pCam->f1y + pCam->f1z * pCam->f1z;
		if (fLenSq > 1e-6f) {
			const float invLen = 1.0f / std::sqrt(fLenSq);
			const float dx = pCam->f1x * invLen;
			const float dy = pCam->f1y * invLen;
			const float dz = pCam->f1z * invLen;

			// PC SA-MP starts ray 1m ahead of camera position along front vector
			const sa::CVector origin{pCam->pos1x + dx, pCam->pos1y + dy, pCam->pos1z + dz};

			auto clipRay = [&](float maxDist, bool checkVeh, bool checkObj) -> float {
				sa::CVector rayEnd{origin.x + dx * maxDist, origin.y + dy * maxDist, origin.z + dz * maxDist};
				sa::CColPoint col{};
				sa::CEntity* hitEnt = nullptr;
				ProcessLineOfSight(const_cast<sa::CVector*>(&origin), &rayEnd, &col, &hitEnt,
					true, checkVeh, 0, checkObj, false, false, false, false);
				if (hitEnt) {
					const float hx = col.m_vecPoint.x - origin.x;
					const float hy = col.m_vecPoint.y - origin.y;
					const float hz = col.m_vecPoint.z - origin.z;
					const float d = std::sqrt(hx * hx + hy * hy + hz * hz);
					if (std::isfinite(d) && d < maxDist) {
						return d;
					}
				}
				return maxDist;
			};

			auto rayPointMetrics = [&](const sa::CVector& pos, float maxT, float& outT, float& outPerpSq) -> bool {
				const float rx = pos.x - origin.x;
				const float ry = pos.y - origin.y;
				const float rz = pos.z - origin.z;
				const float t = rx * dx + ry * dy + rz * dz;
				if (t < 0.0f || t > maxT) return false;
				const float px = rx - dx * t;
				const float py = ry - dy * t;
				const float pz = rz - dz * t;
				outT = t;
				outPerpSq = px * px + py * py + pz * pz;
				return true;
			};

			// Stage 0: Object (max 40m, clipped by buildings)
			if (CObjectPool* pObjPool = pNetGame->GetObjectPool()) {
				const float maxObjDist = clipRay(40.0f, false, false);
				sa::CVector rayEnd{origin.x + dx * maxObjDist, origin.y + dy * maxObjDist, origin.z + dz * maxObjDist};
				sa::CColPoint col{};
				sa::CEntity* hitEnt = nullptr;
				ProcessLineOfSight(const_cast<sa::CVector*>(&origin), &rayEnd, &col, &hitEnt,
					false, false, 0, true, false, false, false, false);
				if (hitEnt) {
					const OBJECTID hitId = pObjPool->FindIDFromGtaPtr(hitEnt);
					if (hitId != INVALID_OBJECT_ID) {
						targets[0] = hitId;
					}
				}
				if (targets[0] == INVALID_OBJECT_ID) {
					float bestT = maxObjDist;
					for (OBJECTID id = 0; id < MAX_OBJECTS; ++id) {
						CObject* pObj = pObjPool->GetAt(id);
						if (!pObj || !pObj->m_gtaId) continue;
						sa::CEntity* liveEnt = GamePool_Object_GetAt(pObj->m_gtaId);
						if (!liveEnt || liveEnt != pObj->m_entity) continue;
						const RwMatrix mat = liveEnt->GetMatrix().ToRwMatrix();
						float rad = GetModelColSphereRadius(pObj->GetModelIndex());
						if (!std::isfinite(rad) || rad < 0.65f) rad = 0.65f;
						if (rad > 6.0f) rad = 6.0f;
						float t = 0.0f, perpSq = 0.0f;
						if (rayPointMetrics({mat.pos.x, mat.pos.y, mat.pos.z}, bestT, t, perpSq) && perpSq <= rad * rad) {
							bestT = t;
							targets[0] = id;
						}
					}
				}
			}

			// Stage 1: Vehicle (max 40m, clipped by buildings + objects)
			if (CVehiclePool* pVehPool = pNetGame->GetVehiclePool()) {
				const float maxVehDist = clipRay(40.0f, false, true);
				sa::CVector rayEnd{origin.x + dx * maxVehDist, origin.y + dy * maxVehDist, origin.z + dz * maxVehDist};
				sa::CColPoint col{};
				sa::CEntity* hitEnt = nullptr;
				ProcessLineOfSight(const_cast<sa::CVector*>(&origin), &rayEnd, &col, &hitEnt,
					false, true, 0, false, false, false, false, false);
				if (hitEnt) {
					const VEHICLEID hitId = pVehPool->FindIDFromGtaPtr(reinterpret_cast<sa::CVehicle*>(hitEnt));
					if (hitId != INVALID_VEHICLE_ID) {
						targets[1] = hitId;
					}
				}
				if (targets[1] == INVALID_VEHICLE_ID) {
					float bestT = maxVehDist;
					for (VEHICLEID id = 0; id < MAX_VEHICLES; ++id) {
						CVehicle* pVeh = pVehPool->GetAt(id);
						if (!pVeh || !pVeh->m_pVehicle || !pVeh->VerifyInstance()) continue;
						const RwMatrix mat = pVeh->m_pVehicle->GetMatrix().ToRwMatrix();
						float rad = GetModelColSphereRadius(pVeh->m_pVehicle->m_nModelIndex);
						if (!std::isfinite(rad) || rad < 1.5f) rad = 1.5f;
						if (rad > 6.0f) rad = 6.0f;
						float t = 0.0f, perpSq = 0.0f;
						if (rayPointMetrics({mat.pos.x, mat.pos.y, mat.pos.z}, bestT, t, perpSq) && perpSq <= rad * rad) {
							bestT = t;
							targets[1] = id;
						}
					}
				}
			}

			// Stage 2: Player & Actor (max 20m, clipped by buildings + vehicles + objects)
			const float maxPedDist = clipRay(20.0f, true, true);
			constexpr float kPedRadiusSq = 0.85f * 0.85f;

			if (CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool()) {
				float bestT = maxPedDist;
				for (PLAYERID id = 0; id < MAX_PLAYERS; ++id) {
					CRemotePlayer* pRemote = pPlayerPool->GetAt(id);
					if (!pRemote || !pRemote->IsActive()) continue;
					CPlayerPed* pPed = pRemote->GetPlayerPed();
					if (!pPed || !pPed->m_ped || !pPed->IsAdded()) continue;
					const sa::CVector pos = pPed->m_ped->GetPosition();
					float t = 0.0f, perpSq = 0.0f;
					if (rayPointMetrics({pos.x, pos.y, pos.z + 0.15f}, bestT, t, perpSq) && perpSq <= kPedRadiusSq) {
						bestT = t;
						targets[2] = id;
					}
				}
			}

			if (CActorPool* pActorPool = pNetGame->GetActorPool()) {
				float bestT = maxPedDist;
				for (uint16_t id = 0; id < MAX_ACTORS; ++id) {
					CActor* pActor = pActorPool->GetAt(id);
					if (!pActor || !pActor->m_actor || !pActor->IsAdded()) continue;
					const sa::CVector pos = pActor->m_actor->GetPosition();
					float t = 0.0f, perpSq = 0.0f;
					if (rayPointMetrics({pos.x, pos.y, pos.z + 0.15f}, bestT, t, perpSq) && perpSq <= kPedRadiusSq) {
						bestT = t;
						targets[3] = id;
					}
				}
			}
		}
	}

	if (targets[0] != m_wCameraTargets[0] || targets[1] != m_wCameraTargets[1] ||
		targets[2] != m_wCameraTargets[2] || targets[3] != m_wCameraTargets[3]) {
		proto::CameraTargetReply reply{};
		reply.objectId = targets[0];
		reply.vehicleId = targets[1];
		reply.playerId = targets[2];
		reply.actorId = targets[3];

		RakNet::BitStream bs;
		proto::Writer writer(bs);
		reply.serialize(writer, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->RPC(&RPC_UpdateCameraTarget, &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);

		for (int i = 0; i < 4; ++i) {
			m_wCameraTargets[i] = targets[i];
		}
	}
}

bool CLocalPlayer::EnterVehicleAsPassenger()
{
	if (!pNetGame || !m_pPlayerPed || !m_pPlayerPed->m_ped
			|| m_pPlayerPed->IsInVehicle() || m_pPlayerPed->IsCuffed() || m_pPlayerPed->IsCarry()) {
		return false;
	}

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (pVehiclePool) {
		VEHICLEID ClosetVehicleID = pVehiclePool->FindNearestToLocalPlayerPed();
		if (ClosetVehicleID < MAX_VEHICLES && pVehiclePool->GetSlotState(ClosetVehicleID)) {
			CVehicle* pVehicle = pVehiclePool->GetAt(ClosetVehicleID);
			if (pVehicle && pVehicle->m_pVehicle && !pVehicle->GetDoorState() && pVehicle->GetDistanceFromLocalPlayerPed() < 8.0f) {
				// Prevent 0x05CA (seat = -1) from falling back to checkSeatFree = false and
				// slow-dragging an existing passenger out when all passenger seats are occupied.
				const uint16_t modelId = pVehicle->m_pVehicle->m_nModelIndex;
				if (modelId != TRAIN_PASSENGER) {
					const int maxSeats = std::min<int>(static_cast<int>(pVehicle->m_pVehicle->m_nMaxPassengers), 8);
					bool bHasFreeSeat = false;
					for (int s = 0; s < maxSeats; ++s) {
						if (pVehicle->m_pVehicle->m_apPassengers[s] == nullptr) {
							bHasFreeSeat = true;
							break;
						}
					}
					if (!bHasFreeSeat) {
						return false;
					}
				}

				if (m_pPlayerPed->GetCurrentWeapon() == WEAPON_PARACHUTE) {
					m_pPlayerPed->SetArmedWeapon(0);
				}

				m_pPlayerPed->EnterVehicle(pVehicle->m_gtaId, true);
				SendEnterVehicleNotification(ClosetVehicleID, true);
				return true;
			}
		}
	}

	return false;
}

bool CLocalPlayer::HandlePassengerEntry()
{
	return false;
}

void CLocalPlayer::ProcessOnFootWorldBounds()
{
	if (pGame->GetActiveInterior() != 0) {
		return;
	} // can't enforce inside interior

	if (m_pPlayerPed->EnforceWorldBoundries(pNetGame->m_netSet->worldBounds[0], pNetGame->m_netSet->worldBounds[1], pNetGame->m_netSet->worldBounds[2], pNetGame->m_netSet->worldBounds[3])) {
		m_pPlayerPed->SetArmedWeapon(0);
		pGame->DisplayGameText("Stay within the ~r~world boundries", 1000, 5);
	}
}

void CLocalPlayer::ProcessInCarWorldBounds()
{
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (pVehiclePool) {
		if (pGame->GetActiveInterior() != 0) {
			return;
		} // can't enforce inside interior

		VEHICLEID vehicleId = pVehiclePool->FindIDFromGtaPtr(m_pPlayerPed->GetGtaVehicle());
		if (vehicleId != INVALID_VEHICLE_ID) {
			CVehicle* pVehicle = pVehiclePool->GetAt(vehicleId);
			if (pVehicle) {
				if (pVehicle->EnforceWorldBoundries(
						pNetGame->m_netSet->worldBounds[0], pNetGame->m_netSet->worldBounds[1],
						pNetGame->m_netSet->worldBounds[2],
						pNetGame->m_netSet->worldBounds[3])) {
					pGame->DisplayGameText("Stay within the ~r~world boundries", 1000, 5);
				}
			}
		}
	}
}

bool CLocalPlayer::CompareOnFootSyncKeys(uint16_t wKeys, uint16_t lrAnalog, uint16_t udAnalog)
{
	return wKeys != m_ofSync.wKeys || udAnalog != m_ofSync.udAnalog || lrAnalog != m_ofSync.lrAnalog;
}

bool CLocalPlayer::CompareInCarSyncKeys(uint16_t wKeys, uint16_t lrAnalog, uint16_t udAnalog)
{
	return wKeys != m_icSync.wKeys || udAnalog != m_icSync.udAnalog || lrAnalog != m_icSync.lrAnalog;
}

int CLocalPlayer::GetOptimumOnFootSendRate()
{
	if (!m_pPlayerPed) {
		return 1000;
	}
	if (pNetGame->session().lanMode) {
		return 15;
	}

	return pNetGame->session().onFootSendRate + pGame->GetPedSlotsUsed();
}

int CLocalPlayer::GetOptimumInCarSendRate()
{
	if (!m_pPlayerPed) {
		return 1000;
	}
	if (pNetGame->session().lanMode) {
		return 15;
	}

	return pNetGame->session().inCarSendRate + pGame->GetPedSlotsUsed();
}

void CLocalPlayer::UpdateVehicleDamage(VEHICLEID vehicleID)
{
	if (!pNetGame || vehicleID == INVALID_VEHICLE_ID) {
		return;
	}

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(vehicleID);
	if (!pVehicle) {
		m_DamageVehicle = INVALID_VEHICLE_ID;
		return;
	}

	uint32_t dwPanels, dwDoors;
	uint8_t byteLights, byteTires;
	pVehicle->GetDamageStatus(&dwPanels, &dwDoors, &byteLights, &byteTires);

	if (m_DamageVehicle != vehicleID) {
		m_DamageVehicle = vehicleID;
		m_dwDamagePanels = dwPanels;
		m_dwDamageDoors = dwDoors;
		m_byteDamageLights = byteLights;
		m_byteDamageTires = byteTires;
		return;
	}

	if (dwPanels == m_dwDamagePanels && dwDoors == m_dwDamageDoors
			&& byteLights == m_byteDamageLights && byteTires == m_byteDamageTires) {
		return;
	}

	m_dwDamagePanels = dwPanels;
	m_dwDamageDoors = dwDoors;
	m_byteDamageLights = byteLights;
	m_byteDamageTires = byteTires;

	RakNet::BitStream bsDamage;
	proto::DamageVehicleReq req{};
	req.vehicleId = vehicleID;
	req.panelDamageStatus = dwPanels;
	req.doorDamageStatus = dwDoors;
	req.lightDamageStatus = byteLights;
	req.tireDamageStatus = byteTires;
	proto::Writer w(bsDamage);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_DamageVehicle, &bsDamage, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::SyncVehicleDamageCache(VEHICLEID vehicleID, uint32_t panels, uint32_t doors, uint8_t lights, uint8_t tires)
{
	if (m_DamageVehicle == vehicleID) {
		m_dwDamagePanels = panels;
		m_dwDamageDoors = doors;
		m_byteDamageLights = lights;
		m_byteDamageTires = tires;
	}
}

void CLocalPlayer::SendNextClass()
{
	RwMatrix matPlayer;

	if (!m_bSpawnDialogShowed) {
		return;
	}

	m_bClearedToSpawn = false;
	matPlayer = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();

	if (m_iSelectedClass == (pNetGame->m_netSet->spawnsAvailable - 1)) {
		m_iSelectedClass = 0;
	}
	else {
		m_iSelectedClass++;
	}

	pGame->PlaySound(1052, matPlayer.pos.x, matPlayer.pos.y, matPlayer.pos.z);
	RequestClass(m_iSelectedClass);
}

void CLocalPlayer::SendPrevClass()
{
	RwMatrix matPlayer;

	if (!m_bSpawnDialogShowed) {
		return;
	}

	m_bClearedToSpawn = false;
	matPlayer = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();

	if (m_iSelectedClass == 0) {
		m_iSelectedClass = (pNetGame->m_netSet->spawnsAvailable - 1);
	}
	else {
		m_iSelectedClass--;
	}

	pGame->PlaySound(1053, matPlayer.pos.x, matPlayer.pos.y, matPlayer.pos.z);
	RequestClass(m_iSelectedClass);
}

void CLocalPlayer::SendSpawn()
{
	if (!m_bSpawnDialogShowed) {
		return;
	}

	RequestSpawn();
	m_bWaitingForSpawnRequestReply = true;
}

void CLocalPlayer::RequestClass(int iClass)
{
	RakNet::BitStream bsClassRequest;
	proto::RequestClassReq req{};
	req.classId = iClass;
	proto::Writer w(bsClassRequest);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_RequestClass, &bsClassRequest, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::RequestSpawn()
{
	RakNet::BitStream bsSpawnRequest;
	proto::RequestSpawnReq req{};
	proto::Writer w(bsSpawnRequest);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_RequestSpawn, &bsSpawnRequest, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::ApplySpecialAction(uint8_t byteSpecialAction)
{
	if (!m_pPlayerPed) {
		return;
	}

	switch (byteSpecialAction) {
		case SPECIAL_ACTION_NONE:
			if (m_pPlayerPed->IsDancing()) m_pPlayerPed->StopDancing();
			if (m_pPlayerPed->HasHandsUp()) m_pPlayerPed->TogglePlayerControllable(true);
			if (m_pPlayerPed->IsInJetpack()) m_pPlayerPed->StopJetpack();
			if (m_pPlayerPed->IsPissing()) m_pPlayerPed->TogglePissing(false);
			if (m_pPlayerPed->IsCuffed()) m_pPlayerPed->ToggleCuffed(false);
			if (m_pPlayerPed->IsCarry()) m_pPlayerPed->ToggleCarry(false);
			if (m_pPlayerPed->IsCellphoneEnabled()) m_pPlayerPed->ToggleCellphone(false);
			if (m_pPlayerPed->GetStuff() != STUFF_TYPE_NONE) m_pPlayerPed->DropStuff();
			break;
		case SPECIAL_ACTION_USEJETPACK:
			if (!m_pPlayerPed->IsInJetpack()) m_pPlayerPed->StartJetpack();
			break;
		case SPECIAL_ACTION_DANCE1:
			m_pPlayerPed->StartDancing(0);
			break;
		case SPECIAL_ACTION_DANCE2:
			m_pPlayerPed->StartDancing(1);
			break;
		case SPECIAL_ACTION_DANCE3:
			m_pPlayerPed->StartDancing(2);
			break;
		case SPECIAL_ACTION_DANCE4:
			m_pPlayerPed->StartDancing(3);
			break;
		case SPECIAL_ACTION_HANDSUP:
			if (!m_pPlayerPed->HasHandsUp()) m_pPlayerPed->HandsUp();
			break;
		case SPECIAL_ACTION_USECELLPHONE:
			if (!m_pPlayerPed->IsCellphoneEnabled()) m_pPlayerPed->ToggleCellphone(true);
			break;
		case SPECIAL_ACTION_STOPUSECELLPHONE:
			if (m_pPlayerPed->IsCellphoneEnabled()) m_pPlayerPed->ToggleCellphone(false);
			break;
		case SPECIAL_ACTION_DRINK_BEER:
			m_pPlayerPed->GiveStuff(STUFF_TYPE_BEER);
			break;
		case SPECIAL_ACTION_SMOKE_CIGGY:
			m_pPlayerPed->GiveStuff(STUFF_TYPE_CIGGI);
			break;
		case SPECIAL_ACTION_DRINK_WINE:
			m_pPlayerPed->GiveStuff(STUFF_TYPE_DYN_BEER);
			break;
		case SPECIAL_ACTION_DRINK_SPRUNK:
			m_pPlayerPed->GiveStuff(STUFF_TYPE_PINT_GLASS);
			break;
		case SPECIAL_ACTION_CUFFED:
			m_pPlayerPed->ToggleCuffed(true);
			break;
		case SPECIAL_ACTION_CARRY:
			m_pPlayerPed->ToggleCarry(true);
			break;
		case SPECIAL_ACTION_PISSING:
			m_pPlayerPed->TogglePissing(true);
			break;
		default:
			break;
	}
}

void CLocalPlayer::SetSpawnInfo(PLAYER_SPAWN_INFO* pSpawnInfo, int iCustomSkin)
{
	memcpy(&m_SpawnInfo, pSpawnInfo, sizeof(PLAYER_SPAWN_INFO));
	m_iCustomSpawnSkin = iCustomSkin;
	m_bHasSpawnInfo = true;
}

void CLocalPlayer::HandleClassSelectionOutcome(bool bOutcome)
{
	if (bOutcome) {
		if (m_pPlayerPed) {
			m_pPlayerPed->ClearWeapons();
			m_pPlayerPed->SetModelIndex(static_cast<uint>(ResolveEffectivePedSkin(m_SpawnInfo.iSkin, m_iCustomSpawnSkin)));
		}

		m_bClearedToSpawn = true;
	}
}

uint8_t CLocalPlayer::GetSpecialAction()
{
	if (m_pPlayerPed) {
		return m_pPlayerPed->GetSpecialAction();
	}

	return SPECIAL_ACTION_NONE;
}

uint32_t CLocalPlayer::GetPlayerColorAsARGB()
{
	return (TranslateColorCodeToRGBA(pNetGame->GetPlayerPool()->GetLocalPlayerID()) >> 8) | 0xFF000000;
}

uint32_t CLocalPlayer::GetPlayerColorAsRGBA()
{
	return TranslateColorCodeToRGBA(pNetGame->GetPlayerPool()->GetLocalPlayerID());
}

bool CLocalPlayer::IsNeedSyncDataSend(const void* data1, const void* data2, size_t size, uint32_t& lastSyncTick)
{
	if (GetTickCount() - lastSyncTick <= 500 && !memcmp(data1, data2, size)) {
		return false;
	}

	lastSyncTick = GetTickCount();
	return true;
}

void CLocalPlayer::SendEnterVehicleNotification(VEHICLEID VehicleID, bool bPassenger)
{
	// open.mp drops RPC_EnterVehicle unless peer.getState() == PlayerState_OnFoot.
	// When re-entering immediately after exiting a vehicle before CLocalPlayer::Process()
	// has emitted an OnFootSync packet, flush one OnFootSync first so the server transitions state.
	if (m_CurrentVehicle != 0xFFFF && m_pPlayerPed && !m_pPlayerPed->IsInVehicle()) {
		m_LastVehicle = m_CurrentVehicle;
		m_CurrentVehicle = 0xFFFF;
		SendOnFootFullSyncData();
	}

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (pVehiclePool) {
		CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
		if (pVehicle) {
			if (pVehicle->IsATrainPart()) {
				ScriptCommand(&camera_on_vehicle, pVehicle->m_gtaId, 3, 2);
				dwEnterVehTimeElasped = GetTickCount();
			}
		}
	}

	RakNet::BitStream bsSend;
	proto::EnterVehicleReq req{};
	req.vehicleId = VehicleID;
	req.passenger = static_cast<uint8_t>(bPassenger);
	proto::Writer w(bsSend);
	req.serialize(w, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_EnterVehicle, &bsSend, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::SpectatePlayer(PLAYERID PlayerID)
{
	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();

	if (pPlayerPool && pPlayerPool->GetSlotState(PlayerID)) {
		CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(PlayerID);
		if (pRemotePlayer && pRemotePlayer->GetState() != PLAYER_STATE_NONE && pRemotePlayer->GetState() != PLAYER_STATE_WASTED) {
			m_byteSpectateType = SPECTATE_TYPE_PLAYER;
			m_SpectateID = PlayerID;
			m_bSpectateProcessed = false;
		}
	}
}

void CLocalPlayer::SpectateVehicle(VEHICLEID VehicleID)
{
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();

	if (pVehiclePool && pVehiclePool->GetSlotState(VehicleID)) {
		m_byteSpectateType = SPECTATE_TYPE_VEHICLE;
		m_SpectateID = VehicleID;
		m_bSpectateProcessed = false;
	}
}

void CLocalPlayer::SetPlayerColor(uint32_t dwColor)
{
	if (pNetGame && pNetGame->GetPlayerPool()) {
		SetRadarColor(pNetGame->GetPlayerPool()->GetLocalPlayerID(), dwColor);
	}
}

void CLocalPlayer::SendExitVehicleNotification(VEHICLEID VehicleID)
{
	RakNet::BitStream bsSend;

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}
	CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
	if (pVehicle) {
		if (!m_pPlayerPed->IsAPassenger()) {
			m_LastVehicle = VehicleID;
		}

		if (pVehicle->IsATrainPart()) {
			pGame->GetCamera()->SetBehindPlayer();
		}

		if (!pVehicle->IsRCVehicle()) {
			proto::ExitVehicleReq req{};
			req.vehicleId = VehicleID;
			proto::Writer w(bsSend);
			req.serialize(w, proto::ProtoVersion::V037);
			pNetGame->GetRakClient()->RPC(&RPC_ExitVehicle, &bsSend, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
		}
	}
}

void CLocalPlayer::SendTakeDamageEvent(PLAYERID PlayerID, float fDamageFactor, int weaponType, int pedPieceType)
{
	spdlog::info("SendTakeDamageEvent: PlayerID: {}, fDamageFactor: {}, weaponType: {}, pedPieceType: {}", PlayerID, fDamageFactor, weaponType, pedPieceType);

	fDamageFactor *= 0.33f;

	RakNet::BitStream bsSend;
	proto::GiveTakeDamageReq req{};
	req.taking = true;
	req.playerId = PlayerID;
	req.damage = fDamageFactor;
	req.weaponId = weaponType;
	req.bodyPart = pedPieceType;
	proto::Writer w(bsSend);
	req.serialize(w, proto::ProtoVersion::V037);

	pNetGame->GetRakClient()->RPC(&RPC_PlayerGiveTakeDamage, &bsSend, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::SendGiveDamageEvent(PLAYERID PlayerID, float fDamageFactor, int weaponType, int pedPieceType)
{
	spdlog::info("SendGiveDamageEvent: PlayerID: {}, fDamageFactor: {}, weaponType: {}, pedPieceType: {}", PlayerID, fDamageFactor, weaponType, pedPieceType);

	fDamageFactor *= 0.33f;

	RakNet::BitStream bsSend;
	proto::GiveTakeDamageReq req{};
	req.taking = false;
	req.playerId = PlayerID;
	req.damage = fDamageFactor;
	req.weaponId = weaponType;
	req.bodyPart = pedPieceType;
	proto::Writer w(bsSend);
	req.serialize(w, proto::ProtoVersion::V037);

	pNetGame->GetRakClient()->RPC(&RPC_PlayerGiveTakeDamage, &bsSend, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::SendGiveDamageActorEvent(PLAYERID ActorID, float fDamageFactor, int weaponType, int pedPieceType)
{
	RakNet::BitStream bsSend;
	proto::GiveDamageActorReq req{};
	req.unknown = false;
	req.actorId = ActorID;
	req.damage = fDamageFactor;
	req.weaponId = weaponType;
	req.bodyPart = pedPieceType;
	proto::Writer w(bsSend);
	req.serialize(w, proto::ProtoVersion::V037);

	pNetGame->GetRakClient()->RPC(&RPC_GiveDamageActor, &bsSend, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void CLocalPlayer::ProcessSurfing()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped || !pNetGame) {
		DisableSurfing();
		return;
	}

	bool bEnteringOrExitingVeh = false;
	if (m_pPlayerPed->m_ped->m_pIntelligence) {
		void* pTaskMgr = &m_pPlayerPed->m_ped->m_pIntelligence->m_TaskMgr;
		bEnteringOrExitingVeh =
			eng::fn::CTaskManager_FindActiveTaskByType(pTaskMgr, 700) != 0 ||
			eng::fn::CTaskManager_FindActiveTaskByType(pTaskMgr, 701) != 0 ||
			eng::fn::CTaskManager_FindActiveTaskByType(pTaskMgr, 702) != 0;
	}

	if (m_pPlayerPed->IsDead() || m_pPlayerPed->IsInVehicle() || bEnteringOrExitingVeh
			|| LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] || m_pPlayerPed->IsJumpTask()) {
		DisableSurfing();
		return;
	}

	const uint32_t nowMs = GetTickCount();
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	auto contactEntity = m_pPlayerPed->GetEntityUnderPlayer();
	if (contactEntity) {
		VEHICLEID vehicleId = pVehiclePool ? pVehiclePool->FindIDFromGtaPtr(reinterpret_cast<sa::CVehicle*>(contactEntity)) : INVALID_VEHICLE_ID;
		if (vehicleId && vehicleId != INVALID_VEHICLE_ID) {
			CVehicle* pVeh = pVehiclePool->GetAt(vehicleId);
			if (pVeh && pVeh->m_pVehicle && pVeh->VerifyInstance()
					&& (pVeh->HasADriver() || pVeh->m_pVehicle->GetModelId() == 569 || pVeh->m_pVehicle->GetModelId() == 570)
					&& pVeh->GetDistanceFromLocalPlayerPed() < 30.0f) {
				if (m_surfSync.bIsActive && m_surfSync.bIsVehicle && m_surfSync.dwSurfVehID == vehicleId) {
					m_surfSync.dwLastContactTick = nowMs;
					return;
				}
				memset(&m_surfSync, 0, sizeof(m_surfSync));
				m_surfSync.vecOffsetPos = sa::CVector{0.0f, 0.0f, 0.0f};
				m_surfSync.dwSurfVehID = vehicleId;
				m_surfSync.pSurfInst = (uintptr_t) pVeh;
				m_surfSync.bIsVehicle = true;
				m_surfSync.dwLastContactTick = nowMs;

				RwMatrix matVeh = pVeh->m_pVehicle->GetMatrix().ToRwMatrix();
				RwMatrix matPed = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
				RwMatrix matOut{};
				RwMatrixInvert(&matOut, &matVeh);
				ProjectMatrix(&m_surfSync.vecOffsetPos, &matOut, &matPed.pos);
				m_surfSync.bIsActive = true;
				return;
			}
		}
		else if (pObjectPool) {
			uint32_t objectId = pObjectPool->FindIDFromGtaPtr(reinterpret_cast<sa::CObject*>(contactEntity));
			if (objectId && objectId != INVALID_OBJECT_ID) {
				CObject* pObject = pObjectPool->GetAt(objectId);
				if (pObject && pObject->m_entity && ((pObject->m_byteMoving & 1) || pObject->AttachedToMovingEntity())) {
					if (m_surfSync.bIsActive && !m_surfSync.bIsVehicle && m_surfSync.dwSurfVehID == (int) objectId) {
						m_surfSync.dwLastContactTick = nowMs;
						return;
					}
					memset(&m_surfSync, 0, sizeof(m_surfSync));
					m_surfSync.vecOffsetPos = sa::CVector{0.0f, 0.0f, 0.0f};
					m_surfSync.bIsVehicle = false;
					m_surfSync.dwSurfVehID = (int) objectId;
					m_surfSync.pSurfInst = (uintptr_t) pObject;
					m_surfSync.dwLastContactTick = nowMs;

					RwMatrix matObj = pObject->m_entity->GetMatrix().ToRwMatrix();
					RwMatrix matPed = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
					RwMatrix matOut{};
					RwMatrixInvert(&matOut, &matObj);
					ProjectMatrix(&m_surfSync.vecOffsetPos, &matOut, &matPed.pos);
					m_surfSync.bIsActive = true;
					return;
				}
			}
		}
	}

	// 2000ms grace period while briefly airborne over bumpy terrain
	if (m_surfSync.bIsActive && (nowMs - m_surfSync.dwLastContactTick) <= 2000) {
		if (m_surfSync.bIsVehicle && pVehiclePool && m_surfSync.dwSurfVehID >= 0 && m_surfSync.dwSurfVehID < MAX_VEHICLES) {
			CVehicle* pVeh = pVehiclePool->GetAt(static_cast<VEHICLEID>(m_surfSync.dwSurfVehID));
			if (pVeh && pVeh->VerifyInstance() && pVeh->GetDistanceFromLocalPlayerPed() < 30.0f) {
				return;
			}
		}
		else if (!m_surfSync.bIsVehicle && pObjectPool && m_surfSync.dwSurfVehID >= 0 && m_surfSync.dwSurfVehID < MAX_OBJECTS) {
			CObject* pObj = pObjectPool->GetAt(static_cast<OBJECTID>(m_surfSync.dwSurfVehID));
			if (pObj && pObj->m_entity && ((pObj->m_byteMoving & 1) || pObj->AttachedToMovingEntity())
					&& pObj->GetDistanceFromLocalPlayerPed() < 30.0f) {
				return;
			}
		}
	}

	m_surfSync.bIsActive = false;
	m_surfSync.dwSurfVehID = INVALID_VEHICLE_ID;
	m_surfSync.pSurfInst = 0;
	m_surfSync.dwLastContactTick = 0;
	m_surfSync.vecOffsetPos = sa::CVector{0.0f, 0.0f, 0.0f};
}

void CLocalPlayer::UpdateSurfing()
{
	static RwMatrix surfInstMatrix;
	static RwMatrix surfPedMatrix;
	static sa::CVector surfInstMoveSpeed;
	static sa::CVector surfInstTurnSpeed;

	if (m_pPlayerPed && m_pPlayerPed->m_ped) {
		if (LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] || m_pPlayerPed->IsJumpTask()) {
			return;
		}
		if (m_surfSync.bIsActive) {
			if (m_surfSync.bIsVehicle) {
				// re-resolve the wrapper every frame: the pool slot may have been deleted/reused
				CVehicle* pVeh = nullptr;
				if (pNetGame && pNetGame->GetVehiclePool()
						&& m_surfSync.dwSurfVehID >= 0 && m_surfSync.dwSurfVehID < MAX_VEHICLES) {
					pVeh = pNetGame->GetVehiclePool()->GetAt((VEHICLEID) m_surfSync.dwSurfVehID);
				}
				if (!pVeh || !pVeh->m_pVehicle || !pVeh->VerifyInstance()) {
					DisableSurfing();
					return;
				}
				m_surfSync.pSurfInst = (uintptr_t) pVeh;
				surfInstMatrix = pVeh->m_pVehicle->GetMatrix().ToRwMatrix();
				surfPedMatrix = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
				surfInstMoveSpeed = pVeh->m_pVehicle->GetMoveSpeed();
				surfInstTurnSpeed = pVeh->m_pVehicle->GetTurnSpeed();

				uint16_t lrAnalog;
				uint16_t udAnalog;
				m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, nullptr, false);

				if (lrAnalog || udAnalog) {
					static RwMatrix matOut;
					RwMatrixInvert(&matOut, &surfInstMatrix);
					ProjectMatrix(&m_surfSync.vecOffsetPos, &matOut, &surfPedMatrix.pos);
				}
				else {
					ProjectMatrix(&surfPedMatrix.pos, &surfInstMatrix, &m_surfSync.vecOffsetPos);

					m_pPlayerPed->m_ped->SetMatrix(surfPedMatrix);
					m_pPlayerPed->UpdateRwMatrixAndFrame();
					sa::CVector vecMoveSpeed;
					vecMoveSpeed = m_pPlayerPed->m_ped->GetMoveSpeed();
					m_pPlayerPed->m_ped->SetVelocity(sa::CVector{surfInstMoveSpeed.x, surfInstMoveSpeed.y, vecMoveSpeed.z});

					sa::CVector vecTurnSpeed = m_pPlayerPed->m_ped->GetTurnSpeed();
					m_pPlayerPed->m_ped->SetTurnSpeed(sa::CVector{vecTurnSpeed.x, vecTurnSpeed.y, surfInstTurnSpeed.z});
				}
			}
			else {
				CObject* pObject = nullptr;
				if (pNetGame && pNetGame->GetObjectPool()
						&& m_surfSync.dwSurfVehID >= 0 && m_surfSync.dwSurfVehID < MAX_OBJECTS) {
					pObject = pNetGame->GetObjectPool()->GetAt((OBJECTID) m_surfSync.dwSurfVehID);
				}
				if (!pObject || !pObject->m_entity || GamePool_Object_GetAt(pObject->m_gtaId) != pObject->m_entity) {
					DisableSurfing();
					return;
				}
				m_surfSync.pSurfInst = (uintptr_t) pObject;
				if ((pObject->m_byteMoving & 1) || pObject->AttachedToMovingEntity()) {
					surfInstMatrix = pObject->m_entity->GetMatrix().ToRwMatrix();
					surfPedMatrix = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
					surfInstMoveSpeed = pObject->m_entity->GetMoveSpeed();
					surfInstTurnSpeed = pObject->m_entity->GetTurnSpeed();

					uint16_t lrAnalog;
					uint16_t udAnalog;
					m_pPlayerPed->GetKeys(&lrAnalog, &udAnalog, nullptr, false);

					if (lrAnalog || udAnalog) {
						static RwMatrix matOut;
						RwMatrixInvert(&matOut, &surfInstMatrix);
						ProjectMatrix(&m_surfSync.vecOffsetPos, &matOut, &surfPedMatrix.pos);
					}
					else {
						ProjectMatrix(&surfPedMatrix.pos, &surfInstMatrix, &m_surfSync.vecOffsetPos);

						m_pPlayerPed->m_ped->SetMatrix(surfPedMatrix);
						m_pPlayerPed->UpdateRwMatrixAndFrame();
						sa::CVector vecMoveSpeed = m_pPlayerPed->m_ped->GetMoveSpeed();
						m_pPlayerPed->m_ped->SetVelocity(sa::CVector{surfInstMoveSpeed.x, surfInstMoveSpeed.y, vecMoveSpeed.z});

						sa::CVector vecTurnSpeed = m_pPlayerPed->m_ped->GetTurnSpeed();
						m_pPlayerPed->m_ped->SetTurnSpeed(sa::CVector{vecTurnSpeed.x, vecTurnSpeed.y, surfInstTurnSpeed.z});
					}
				}
			}
		}
	}
}
