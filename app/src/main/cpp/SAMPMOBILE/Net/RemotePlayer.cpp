#include "../Game/Game.h"
#include "../Game/Animation.h"
#include "NetGame.h"
#include "../UI/UI.h"

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;

CRemotePlayer::CRemotePlayer()
{
	m_VehicleID = 0;
	m_byteState = PLAYER_STATE_NONE;
	m_PlayerID = INVALID_PLAYER_ID;
	m_pPlayerPed = nullptr;
	m_byteUpdateFromNetwork = UPDATE_TYPE_NONE;
	m_byteTeam = NO_TEAM;
	m_bShowNameTag = true;
	m_bGlobalMarkerShown = false;
	m_dwMarker = 0;
	m_byteSpecialAction = SPECIAL_ACTION_NONE;
	m_bIsNPC = false;
	m_dwLastRecvTick = GetTickCount();
	m_dwLastHeadUpdate = 0;
	m_dwLastStoredSyncDataTime = 0;
	m_quat = sa::CQuaternion(0.0f, 0.0f, 0.0f, 1.0f);

	m_bWasAnimSettedFlag = false;
	m_dwPlayingAnim = 0;

	ResetAllSyncAttributes();
}

CRemotePlayer::~CRemotePlayer()
{
	if (m_dwMarker) {
		pGame->DisableMarker(m_dwMarker);
		m_dwMarker = 0;
	}

	// field_1E9 = 0;

	if (m_pPlayerPed) {
		pGame->RemovePlayer(m_pPlayerPed);
		m_pPlayerPed = nullptr;
	}
}

void CRemotePlayer::Process()
{
	sa::CVector vecMoveSpeed;
	RwMatrix matPlayer;
	RwMatrix matVehicle;

	if (IsActive() && m_pPlayerPed && m_pPlayerPed->m_ped) {
		// walk handling
		if (GetState() == PLAYER_STATE_ONFOOT && m_byteUpdateFromNetwork == UPDATE_TYPE_ONFOOT && !m_pPlayerPed->IsInVehicle()) {
			// If the user has sent X, Y or Z offsets we need to 'attach' him to the car using these offsets
			if (!IsSurfing() || !ProcessSurfing()) {
				// If the user hasn't sent these (or the surfed entity isn't streamed in), sync normal movement/speed
				UpdateOnFootPositionAndSpeed(&m_onFootSync.vecPos, &m_onFootSync.vecMoveSpeed);
				if (m_pPlayerPed->IsEnteringVehicle() == 0) {
					UpdateOnFootTargetPosition();
				}
			}
			else {
				UpdateOnFootPositionAndSpeed(&m_onFootSync.vecPos, &m_onFootSync.vecMoveSpeed);
			}

			// update weapons
			WeaponSyncCheck(m_onFootSync.byteCurrentWeapon & 0x3F);

			// sniper / rocket launcher scope state (aligned with PC CRemotePlayer::Process)
			if (m_onFootSync.wKeys & 128) {
				const uint8_t curWeapon = m_pPlayerPed->GetCurrentWeapon();
				if (curWeapon == sa::WEAPONTYPE_SNIPERRIFLE ||
						curWeapon == sa::WEAPONTYPE_RLAUNCHER ||
						curWeapon == sa::WEAPONTYPE_RLAUNCHER_HS ||
						curWeapon == sa::WEAPONTYPE_CAMERA) {
					m_pPlayerPed->m_ped->m_nPedState = sa::PED_SNIPER_MODE;
				}
			}
			else if (m_pPlayerPed->m_ped->m_nPedState == sa::PED_SNIPER_MODE) {
				m_pPlayerPed->m_ped->m_nPedState = sa::PED_IDLE;
			}
			m_pCurrentVehicle = nullptr;
			m_byteUpdateFromNetwork = UPDATE_TYPE_NONE;
		}
			// Driving Network Processing
		else if (GetState() == PLAYER_STATE_DRIVER && m_byteUpdateFromNetwork == UPDATE_TYPE_INCAR && m_pPlayerPed->IsInVehicle()) {
			if (!m_pCurrentVehicle || !m_pCurrentVehicle->m_pVehicle || !m_pCurrentVehicle->VerifyInstance()) {
				return;
			}

			m_inCarSync.quat.Normalize();
			m_inCarSync.quat.GetMatrix(&matVehicle);
			matVehicle.pos = m_inCarSync.vecPos;

			// train movement
			if (m_pCurrentVehicle->GetModelIndex() == 538 || m_pCurrentVehicle->GetModelIndex() == 537 || m_pCurrentVehicle->GetModelIndex() == 449) {
				UpdateTrainDriverMatrixAndSpeed(&matVehicle, &m_inCarSync.vecMoveSpeed, m_inCarSync.fTrainSpeed);
			}
			else {
				UpdateInCarMatrixAndSpeed(&matVehicle, &m_inCarSync.vecPos, &m_inCarSync.vecMoveSpeed);
				UpdateInCarTargetPosition();
			}

			// If the current vehicle is Hydra
			if (m_pCurrentVehicle->GetModelIndex() == HYDRA) {
				// Sync thruster status
				m_pCurrentVehicle->SetHydraThrusters(*(uint32_t*) m_inCarSync.sHydraThrustAngle);
			}

			if (m_pCurrentVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_BIKE || m_pCurrentVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_PUSHBIKE) {
				m_pCurrentVehicle->SetRemoteBikeLean(m_inCarSync.fTrainSpeed);
			}

			// set alarm
			m_pCurrentVehicle->SetSirenOn(m_inCarSync.byteSirenOn);

			m_pCurrentVehicle->SetRemoteLandingGearState(m_inCarSync.byteLandingGearState);

			m_pCurrentVehicle->SetHealth(m_inCarSync.fCarHealth);

			PLAYERID TrailerID = m_inCarSync.TrailerID;
			// invalid trailer
			if ((TrailerID == 0 || TrailerID >= MAX_VEHICLES) && m_pCurrentVehicle->GetTrailer()) {
				// split trailer
				m_pCurrentVehicle->DetachTrailer();
				m_pCurrentVehicle->SetTrailer(nullptr);
			}

			// Detect weapon sync on vehicle
			WeaponSyncCheck(m_inCarSync.byteCurrentWeapon & 0x3F);

			m_byteUpdateFromNetwork = UPDATE_TYPE_NONE;
		}
			// Passenger Network Processing
		else if (GetState() == PLAYER_STATE_PASSENGER && m_byteUpdateFromNetwork == UPDATE_TYPE_PASSENGER && m_pPlayerPed->IsInVehicle()) {
			if (!m_pCurrentVehicle || !m_pCurrentVehicle->m_pVehicle || !m_pCurrentVehicle->VerifyInstance()) return;

			// UPDATE CURRENT WEAPON
			WeaponSyncCheck(m_passengerSync.byteCurrentWeapon & 0x3F);

			if (m_passengerSync.byteSeatFlags & 0x40) {
				if (!m_pPlayerPed->IsInPassengerDriveByMode()) {
					if (m_pPlayerPed->IsAdded()) {
						if (m_pPlayerPed->StartPassengerDriveByMode()) {
							m_bPassengerDriveByMode = true;
						}
					}
				}
				else {
					m_bPassengerDriveByMode = true;
				}
			}
			else {
				if (m_pPlayerPed->IsInPassengerDriveByMode() || m_bPassengerDriveByMode) {
					m_pPlayerPed->StopPassengerDriveByMode();
					m_bPassengerDriveByMode = false;
				}
			}

			if (m_passengerSync.byteSeatFlags & 0x80) {
				if (!m_pPlayerPed->IsCuffed()) {
					m_pPlayerPed->ToggleCuffed(true);
				}
			}
			else if (m_pPlayerPed->IsCuffed()) {
				m_pPlayerPed->ToggleCuffed(false);
			}

			m_byteUpdateFromNetwork = UPDATE_TYPE_NONE;
		}
		// ------ PROCESSED FOR ALL FRAMES -----
		if (GetState() == PLAYER_STATE_ONFOOT && !m_pPlayerPed->IsInVehicle()) {
			m_bPassengerDriveByMode = false;

			bool bSurfAttached = false;
			if (IsSurfing()) {
				bSurfAttached = ProcessSurfing();
			}

			const bool bEnteringOrExitingVeh = (m_pPlayerPed->IsEnteringVehicle() != 0) || m_pPlayerPed->IsExitingVehicle();
			if (!bEnteringOrExitingVeh) {
				InterpolateAndRotate();
			}

			ProcessSpecialAction(m_onFootSync.byteSpecialAction);

			if (!bEnteringOrExitingVeh) {
				ProcessAnimation();
			}

			m_pPlayerPed->SetKeys(m_onFootSync.lrAnalog, m_onFootSync.udAnalog, m_onFootSync.wKeys);

			const bool bMoving = m_onFootSync.vecMoveSpeed.x != 0.0f
				|| m_onFootSync.vecMoveSpeed.y != 0.0f
				|| m_onFootSync.vecMoveSpeed.z != 0.0f;
			if (!bMoving && !bEnteringOrExitingVeh) {
				m_pPlayerPed->m_ped->SetVelocity(m_onFootSync.vecMoveSpeed);
			}
			// Match PC AFK thresholds: 1500ms moving, 3000ms stationary
			const uint32_t afkThreshold = bMoving ? 1500u : 3000u;
			if ((GetTickCount() - m_dwLastRecvTick) > afkThreshold) {
				m_bIsAFK = true;
			}
			//Lock player position during afk
			if (m_bIsAFK && ((GetTickCount() - m_dwLastRecvTick) > 3000)) {
				m_onFootSync.lrAnalog = 0;
				m_onFootSync.udAnalog = 0;

				vecMoveSpeed.x = 0.0f;
				vecMoveSpeed.y = 0.0f;
				vecMoveSpeed.z = 0.0f;
				m_pPlayerPed->m_ped->SetVelocity(vecMoveSpeed);
				matPlayer = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
				matPlayer.pos.x = m_onFootSync.vecPos.x;
				matPlayer.pos.y = m_onFootSync.vecPos.y;
				matPlayer.pos.z = m_onFootSync.vecPos.z;
				m_pPlayerPed->m_ped->SetMatrix(matPlayer);
			}
			if ((bSurfAttached && SurfingOnVehicle())
					|| m_onFootSync.byteSpecialAction == SPECIAL_ACTION_ENTER_VEHICLE) {
				m_pPlayerPed->SetGravityProcessing(false);
				m_pPlayerPed->SetCollisionChecking(false);
			}
			else {
				m_pPlayerPed->SetGravityProcessing(true);
				m_pPlayerPed->SetCollisionChecking(true);
			}
		}
		else if (GetState() == PLAYER_STATE_DRIVER && m_pPlayerPed->IsInVehicle()) {
			if (m_pCurrentVehicle && m_pCurrentVehicle->GetModelIndex() != 538 && m_pCurrentVehicle->GetModelIndex() != 537 && m_pCurrentVehicle->GetModelIndex() != 449) {
				UpdateVehicleRotation();
			}

			m_bPassengerDriveByMode = false;

			m_pPlayerPed->CheckVehicleParachute();

			if (m_pCurrentVehicle && m_pCurrentVehicle->IsRCVehicle()) {
				m_pPlayerPed->Remove();
			}

			if (m_bIsNPC) {
				if (m_inCarSync.vecMoveSpeed.x == 0.0f && m_inCarSync.vecMoveSpeed.y == 0.0f && m_inCarSync.vecMoveSpeed.z == 0.0f) {
					if (m_pCurrentVehicle && m_pCurrentVehicle->m_pVehicle) {
						m_pCurrentVehicle->m_pVehicle->SetVelocity(m_inCarSync.vecMoveSpeed);
					}
				}
			}

			m_pPlayerPed->SetKeys(m_inCarSync.lrAnalog, m_inCarSync.udAnalog, m_inCarSync.wKeys);
			m_pPlayerPed->ProcessVehicleHorn();

			if (!m_bIsNPC && m_pPlayerPed->IsAdded() && Settings::headMove()
					&& m_pPlayerPed->GetStuff() == STUFF_TYPE_NONE && (GetTickCount() - m_dwLastHeadUpdate) > 500) {
				CAMERA_AIM* Aim = GameGetRemotePlayerAim(m_pPlayerPed->m_bytePlayerNumber);
				if (Aim && m_pPlayerPed->m_ped) {
					RwMatrix matHead = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
					sa::CVector LookAt;
					LookAt.x = matHead.pos.x + (Aim->f1x * 20.0f);
					LookAt.y = matHead.pos.y + (Aim->f1y * 20.0f);
					LookAt.z = matHead.pos.z + (Aim->f1z * 20.0f);
					m_pPlayerPed->ApplyCommandTask("FollowPedSA", nullptr, 2000, -1, &LookAt, 0, 0.1f, 500, 3, 0);
				}
				m_dwLastHeadUpdate = GetTickCount();
			}

			if (m_inCarSync.vecMoveSpeed.x != 0.0f || m_inCarSync.vecMoveSpeed.y != 0.0f || m_inCarSync.vecMoveSpeed.z != 0.0f) {
				if (!m_bIsAFK) {
					if ((GetTickCount() - m_dwLastRecvTick) > 1500) {
						m_bIsAFK = true;
					}
				}
			}
			else if (!m_bIsAFK) {
				if ((GetTickCount() - m_dwLastRecvTick) > 3000) {
					m_bIsAFK = true;
				}
			}
		}
		else if (GetState() == PLAYER_STATE_PASSENGER && m_pPlayerPed->IsInVehicle()) {
			m_pPlayerPed->CheckVehicleParachute();
			m_pPlayerPed->SetKeys(m_passengerSync.lrAnalog, m_passengerSync.udAnalog, m_passengerSync.wKeys);

			if (m_pPlayerPed->IsCuffed()) {
				m_pPlayerPed->ProcessCuff();
			}

			if (!m_bIsNPC && !m_bPassengerDriveByMode && m_pPlayerPed->IsAdded() && Settings::headMove()
					&& m_pPlayerPed->GetStuff() == STUFF_TYPE_NONE) {
				if ((GetTickCount() - m_dwLastHeadUpdate) > 500) {
					sa::CVector LookAt;
					CAMERA_AIM* Aim = GameGetRemotePlayerAim(m_pPlayerPed->m_bytePlayerNumber);
					if (Aim && m_pPlayerPed->m_ped) {
						RwMatrix matHead = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
						LookAt.x = matHead.pos.x + (Aim->f1x * 20.0f);
						LookAt.y = matHead.pos.y + (Aim->f1y * 20.0f);
						LookAt.z = matHead.pos.z + (Aim->f1z * 20.0f);
						m_pPlayerPed->ApplyCommandTask("FollowPedSA", nullptr, 2000, -1, &LookAt, 0, 0.1f, 500, 3, 0);
					}
					m_dwLastHeadUpdate = GetTickCount();
				}
			}

			if ((GetTickCount() - m_dwLastRecvTick) > 3000) {
				m_bIsAFK = true;
			}
		}
		else {
			m_bPassengerDriveByMode = false;
			m_pPlayerPed->SetKeys(0, 0, 0);
			vecMoveSpeed.x = 0.0f;
			vecMoveSpeed.y = 0.0f;
			vecMoveSpeed.z = 0.0f;
			m_pPlayerPed->m_ped->SetVelocity(vecMoveSpeed);
		}

		if (GetState() != PLAYER_STATE_WASTED) {
			m_pPlayerPed->SetHealth(1000.0f);
		}

		if ((GetTickCount() - m_dwLastRecvTick) < 1500) {
			m_bIsAFK = false;
		}
	}
	else {
		if (m_pPlayerPed) {
			ResetAllSyncAttributes();
			pGame->RemovePlayer(m_pPlayerPed);
			m_pPlayerPed = nullptr;
		}
	}
}

void CRemotePlayer::InterpolateAndRotate()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped) {
		return;
	}

	RwMatrix mat = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();

	sa::CQuaternion quat, slerpedQuat;
	quat.SetFromMatrix(&mat);
	slerpedQuat.Slerp(&m_onFootSync.quat, &quat, 0.75f);
	slerpedQuat.Normalize();
	slerpedQuat.GetMatrix(&mat);
	m_pPlayerPed->m_ped->SetMatrix(mat);

	float fZ = atan2(-mat.up.x, mat.up.y) * 57.295776; /* rad to deg */
	if (fZ > 360.0f) {
		fZ -= 360.0f;
	}
	if (fZ < 0.0f) {
		fZ += 360.0f;
	}
	m_pPlayerPed->SetRotation(fZ);
}

void CRemotePlayer::UpdateOnFootTargetPosition()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped) {
		return;
	}

	RwMatrix mat = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();

	if (!m_pPlayerPed->IsAdded()) {
		mat.pos.x = m_vecOnFootTargetPos.x;
		mat.pos.y = m_vecOnFootTargetPos.y;
		mat.pos.z = m_vecOnFootTargetPos.z;
		m_pPlayerPed->m_ped->SetMatrix(mat);
		return;
	}

	m_vecPositionInaccuracy.x = FloatOffset(m_vecOnFootTargetPos.x, mat.pos.x);
	m_vecPositionInaccuracy.y = FloatOffset(m_vecOnFootTargetPos.y, mat.pos.y);
	m_vecPositionInaccuracy.z = FloatOffset(m_vecOnFootTargetPos.z, mat.pos.z);

	if (m_vecPositionInaccuracy.x > 0.00001f || m_vecPositionInaccuracy.y > 0.00001f || m_vecPositionInaccuracy.z > 0.00001f) {
		if (m_vecPositionInaccuracy.x > 2.0f || m_vecPositionInaccuracy.y > 2.0f || m_vecPositionInaccuracy.z > 1.0f) {
			mat.pos.x = m_vecOnFootTargetPos.x;
			mat.pos.y = m_vecOnFootTargetPos.y;
			mat.pos.z = m_vecOnFootTargetPos.z;
			m_pPlayerPed->m_ped->SetMatrix(mat);
			return;
		}

		sa::CVector vecMoveSpeed = m_pPlayerPed->m_ped->GetMoveSpeed();
		if (m_vecPositionInaccuracy.x > 0.00001f) {
			vecMoveSpeed.x += (m_vecOnFootTargetPos.x - mat.pos.x) * 0.1f;
		}
		if (m_vecPositionInaccuracy.y > 0.00001f) {
			vecMoveSpeed.y += (m_vecOnFootTargetPos.y - mat.pos.y) * 0.1f;
		}
		if (m_vecPositionInaccuracy.z > 0.00001f) {
			vecMoveSpeed.z += (m_vecOnFootTargetPos.z - mat.pos.z) * 0.1f;
		}
		m_pPlayerPed->m_ped->SetVelocity(vecMoveSpeed);
	}
}

void CRemotePlayer::UpdateOnFootPositionAndSpeed(sa::CVector* vecPos, sa::CVector* vecMove)
{
	m_vecOnFootTargetPos.x = vecPos->x;
	m_vecOnFootTargetPos.y = vecPos->y;
	m_vecOnFootTargetPos.z = vecPos->z;

	m_vecOnFootTargetSpeed.x = vecMove->x;
	m_vecOnFootTargetSpeed.y = vecMove->y;
	m_vecOnFootTargetSpeed.z = vecMove->z;

	if (m_pPlayerPed && m_pPlayerPed->m_ped) {
		m_pPlayerPed->m_ped->SetVelocity(m_vecOnFootTargetSpeed);
	}
}

void CRemotePlayer::ResetAllSyncAttributes()
{
	m_VehicleID = 0;
	m_fReportedHealth = 0.0f;
	m_fReportedArmour = 0.0f;
	m_pCurrentVehicle = nullptr;
	m_byteSeatID = 0;
	m_bPassengerDriveByMode = false;
	m_byteSpecialAction = SPECIAL_ACTION_NONE;
	m_byteUpdateFromNetwork = UPDATE_TYPE_NONE;
	m_dwPlayingAnim = 0;
	m_bWasAnimSettedFlag = false;
	m_bGlobalMarkerShown = false;
	m_quat = sa::CQuaternion(0.0f, 0.0f, 0.0f, 1.0f);
	m_bIsAFK = true;

	memset(&m_onFootSync, 0, sizeof(ONFOOT_SYNC_DATA));
	memset(&m_inCarSync, 0, sizeof(INCAR_SYNC_DATA));
	memset(&m_passengerSync, 0, sizeof(PASSENGER_SYNC_DATA));

	if (m_dwMarker) {
		pGame->DisableMarker(m_dwMarker);
		m_dwMarker = 0;
	}

	// RakNet ID_TIMESTAMP uses RakNet::GetTime() (process uptime ms), NOT wall-clock GetTickCount().
	// Initialize to 0 so the first timestamped packet always establishes the baseline.
	m_dwLastStoredSyncDataTime = 0;
	m_dwEnterVehicleTick = 0;
	m_bEnteredVehicleSpecialActionSeen = false;
}

void CRemotePlayer::SetPlayerColor(uint32_t dwColor)
{
	SetRadarColor(m_PlayerID, dwColor);
}

uint32_t CRemotePlayer::GetPlayerColor()
{
	return TranslateColorCodeToRGBA(m_PlayerID);
}

uint32_t CRemotePlayer::GetPlayerColorAsARGB()
{
	return (TranslateColorCodeToRGBA(m_PlayerID) >> 8) | 0xFF000000;
}

void CRemotePlayer::Say(char* szMessage)
{
	char* szPlayerName = pNetGame->GetPlayerPool()->GetPlayerName(m_PlayerID);
	if (pUI) {
		pUI->chat()->addChatMessage(Encoding::cp2utf(szMessage), Encoding::cp2utf(szPlayerName ? szPlayerName : ""), TranslateColorCodeToRGBA(m_PlayerID));
	}
}

bool CRemotePlayer::Spawn(uint8_t byteTeam, int iSkin, sa::CVector* vecPos, float fRotation, uint32_t dwColor, uint8_t byteFightingStyle)
{
	if (m_pPlayerPed) {
		pGame->RemovePlayer(m_pPlayerPed);
		m_pPlayerPed = nullptr;
	}
	ResetAllSyncAttributes();

	spdlog::info("CRemotePlayer::Spawn: {} {} {} {} {} {} {}", m_PlayerID, byteTeam, iSkin, vecPos->x, vecPos->y, vecPos->z, fRotation);
	CPlayerPed* pPlayerPed = pGame->NewPlayer(iSkin, vecPos->x, vecPos->y, vecPos->z, fRotation, true, m_bIsNPC);
	if (pPlayerPed) {
		SetRadarColor(m_PlayerID, dwColor);

		if (m_dwMarker) {
			pGame->DisableMarker(m_dwMarker);
			m_dwMarker = 0;
		}
		if (pNetGame->m_netSet->showPlayerMarkers) {
			// Show near point markers (if the ped is loaded)
			pPlayerPed->ShowMarker(m_PlayerID);
		}

		m_byteTeam = byteTeam;
		m_pPlayerPed = pPlayerPed;
		m_fReportedHealth = pPlayerPed->GetHealth();
		pPlayerPed->SetKeys(0, 0, 0);
		if (byteFightingStyle != 4) {
			pPlayerPed->SetFightingStyle(byteFightingStyle);
		}

		SetState(PLAYER_STATE_SPAWNED);
		return true;
	}
	else {
		SetState(PLAYER_STATE_NONE);
		return false;
	}
}

void CRemotePlayer::Remove()
{
	if (m_pPlayerPed) {
		ResetAllSyncAttributes();
		pGame->RemovePlayer(m_pPlayerPed);
		m_pPlayerPed = nullptr;
	}

	SetState(PLAYER_STATE_NONE);
	m_bShowNameTag = true;
}

void CRemotePlayer::HandleDeath()
{
	if (m_pPlayerPed) {
		m_pPlayerPed->SetKeys(0, 0, 0);
		if (m_pPlayerPed->GetStuff() != STUFF_TYPE_NONE) m_pPlayerPed->DropStuff();
		if (m_pPlayerPed->IsDancing()) m_pPlayerPed->StopDancing();
		if (m_pPlayerPed->IsPissing()) m_pPlayerPed->TogglePissing(false);
		if (m_pPlayerPed->IsCarry()) m_pPlayerPed->ToggleCarry(false);
		if (m_pPlayerPed->IsCuffed()) m_pPlayerPed->ToggleCuffed(false);
		m_pPlayerPed->SetDead();
	}

	m_dwPlayingAnim = 0;
	m_bWasAnimSettedFlag = false;
	SetState(PLAYER_STATE_WASTED);
	ResetAllSyncAttributes();
}
//walk sync
void CRemotePlayer::StoreOnFootFullSyncData(ONFOOT_SYNC_DATA* ofSync, uint32_t dwTime)
{
	if (dwTime != 0 && m_dwLastStoredSyncDataTime != 0
			&& static_cast<int32_t>(dwTime - m_dwLastStoredSyncDataTime) < 0) {
		return;
	}

	m_pCurrentVehicle = nullptr;
	if (dwTime != 0) {
		m_dwLastStoredSyncDataTime = dwTime;
	}
	memcpy(&m_onFootSync, ofSync, sizeof(ONFOOT_SYNC_DATA));
	m_fReportedHealth = ofSync->byteHealth;
	m_fReportedArmour = ofSync->byteArmour;
	m_byteUpdateFromNetwork = UPDATE_TYPE_ONFOOT;
	m_dwLastRecvTick = GetTickCount();
	m_byteSpecialAction = ofSync->byteSpecialAction;

	if (m_byteSpecialAction == SPECIAL_ACTION_ENTER_VEHICLE) {
		m_bEnteredVehicleSpecialActionSeen = true;
	}

	if (m_pPlayerPed) {
		if (m_pPlayerPed->IsInVehicle() && !m_pPlayerPed->IsExitingVehicle()) {
			if (m_byteSpecialAction != SPECIAL_ACTION_ENTER_VEHICLE && m_byteSpecialAction != SPECIAL_ACTION_EXIT_VEHICLE) {
				RemoveFromVehicle();
			}
		}
		else if (m_pPlayerPed->IsEnteringVehicle() != 0 && m_byteSpecialAction != SPECIAL_ACTION_ENTER_VEHICLE) {
			// Abort stale enter-car task when the remote player cancelled entry after opening the door
			// (3 -> 0 transition) or never entered SPECIAL_ACTION_ENTER_VEHICLE within 600ms of RPC_EnterVehicle.
			if (m_bEnteredVehicleSpecialActionSeen
					|| m_dwEnterVehicleTick == 0
					|| (GetTickCount() - m_dwEnterVehicleTick) > 600) {
				m_pPlayerPed->ClearPrimaryTask();
				m_dwEnterVehicleTick = 0;
				m_bEnteredVehicleSpecialActionSeen = false;
			}
		}
	}

	SetState(PLAYER_STATE_ONFOOT);
}
//In-Car Sync
void CRemotePlayer::StoreInCarFullSyncData(INCAR_SYNC_DATA* picSync, uint32_t dwTime)
{
	if (dwTime == 0 || m_dwLastStoredSyncDataTime == 0
			|| static_cast<int32_t>(dwTime - m_dwLastStoredSyncDataTime) >= 0) {
		if (dwTime != 0) {
			m_dwLastStoredSyncDataTime = dwTime;
		}
		memcpy(&m_inCarSync, picSync, sizeof(INCAR_SYNC_DATA));
		m_VehicleID = picSync->VehicleID;

		CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
		CVehicle* pVehicle = nullptr;
		if (pVehiclePool) {
			pVehicle = pVehiclePool->GetAt(m_VehicleID);
		}

		m_pCurrentVehicle = pVehicle;
		m_byteSeatID = 0;
		m_fReportedHealth = picSync->bytePlayerHealth;
		m_fReportedArmour = picSync->bytePlayerArmour;
		m_byteUpdateFromNetwork = UPDATE_TYPE_INCAR;
		m_dwLastRecvTick = GetTickCount();
		m_byteSpecialAction = SPECIAL_ACTION_NONE;
		m_onFootSync.byteSpecialAction = SPECIAL_ACTION_NONE;
		m_dwEnterVehicleTick = 0;
		m_bEnteredVehicleSpecialActionSeen = false;

		if (m_pPlayerPed) {
			if (m_pPlayerPed->GetStuff() != STUFF_TYPE_NONE) m_pPlayerPed->DropStuff();
			if (m_pPlayerPed->IsDancing()) m_pPlayerPed->StopDancing();
			if (m_pPlayerPed->IsPissing()) m_pPlayerPed->TogglePissing(false);
			if (m_pPlayerPed->IsCarry()) m_pPlayerPed->ToggleCarry(false);
			if (m_pPlayerPed->IsCuffed()) m_pPlayerPed->ToggleCuffed(false);
		}

		SetState(PLAYER_STATE_DRIVER);

		// Put in if the player is not in the car or warped directly from another vehicle/seat
		if (m_pPlayerPed && (!m_pPlayerPed->IsInVehicle() || m_pPlayerPed->IsAPassenger()
				|| (pVehicle && m_pPlayerPed->GetGtaVehicle() != pVehicle->m_pVehicle))) {
			PutInCurrentVehicle();
		}
	}
}
//Sync of aiming status
void CRemotePlayer::StoreAimFullSyncData(AIM_SYNC_DATA* paimSync)
{
	if (!m_pPlayerPed) {
		return;
	}

	// detonator crash fix
	if (m_pPlayerPed->GetCurrentWeapon() == 40) // If you have a detonator in your hand
	{
		int CrashCam[] = {7, 8, 34, 45, 46, 51, 65}; // Detonator crash camera angle.
		int CrashCamSize = sizeof(CrashCam) / sizeof(CrashCam[0]); // calculate @CrashCam array size
		for (int i = 0; i < CrashCamSize; i++) {
			if (paimSync->byteCamMode == CrashCam[i]) //Determine if aiming mode is equal to detonator crash camera mode
			{
				paimSync->byteCamMode = 0;
				m_pPlayerPed->SetCameraMode(0);
				return;
			}
		}
	}

	m_pPlayerPed->SetCameraMode(paimSync->byteCamMode);

	CAMERA_AIM Aim;
	Aim.f1x = paimSync->vecAimf1.x;
	Aim.f1y = paimSync->vecAimf1.y;
	Aim.f1z = paimSync->vecAimf1.z;

	sa::CVector vec1, vec2;
	vec1.x = Aim.f1x;
	vec1.y = Aim.f1y;
	vec1.z = Aim.f1z;
	vec2.x = 0.0f;
	vec2.y = 0.0f;
	vec2.z = 0.0f;
	calculateAimVector(&vec1, &vec2);
	Aim.f2x = vec2.x;
	Aim.f2y = vec2.y;
	Aim.f2z = vec2.z;

	Aim.pos1x = paimSync->vecAimPos.x;
	Aim.pos1y = paimSync->vecAimPos.y;
	Aim.pos1z = paimSync->vecAimPos.z;
	Aim.pos2x = Aim.pos1x;
	Aim.pos2y = Aim.pos1y;
	Aim.pos2z = Aim.pos1z;
	m_pPlayerPed->SetCurrentAim(&Aim);

	m_pPlayerPed->SetAimZ(paimSync->fAimZ);

	float fExtZoom = paimSync->byteCamExtZoom * 0.015873017;
	float fAspectRatio = paimSync->byteAspectRatio * 0.0039215689;
	m_pPlayerPed->SetCameraZoomAndAspect(fExtZoom, fAspectRatio);

	sa::CWeapon* pwstWeapon = m_pPlayerPed->GetCurrentWeaponSlot();
	if (!pwstWeapon) return;

	//reload sync
	if (paimSync->byteWeaponState == WS_RELOADING) {
		pwstWeapon->dwState = sa::WEAPONSTATE_RELOADING; // Reloading
	}
	else if (paimSync->byteWeaponState != WS_MORE_BULLETS) {
		pwstWeapon->dwAmmoInClip = (uint32_t) paimSync->byteWeaponState;
	}
	else if (pwstWeapon->dwAmmoInClip < 2) {
		pwstWeapon->dwAmmoInClip = 2;
	}
}
//passenger sync
void CRemotePlayer::StorePassengerFullSyncData(PASSENGER_SYNC_DATA* psSync)
{
	memcpy(&m_passengerSync, psSync, sizeof(PASSENGER_SYNC_DATA));
	m_VehicleID = psSync->VehicleID;
	m_byteSeatID = psSync->byteSeatFlags & 0x3F;

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) return;
	CVehicle* pVehicle = pVehiclePool->GetAt(m_VehicleID);

	m_pCurrentVehicle = pVehicle;
	m_fReportedHealth = psSync->bytePlayerHealth;
	m_fReportedArmour = psSync->bytePlayerArmour;
	m_byteUpdateFromNetwork = UPDATE_TYPE_PASSENGER;
	m_dwLastRecvTick = GetTickCount();
	m_byteSpecialAction = SPECIAL_ACTION_NONE;
	m_onFootSync.byteSpecialAction = SPECIAL_ACTION_NONE;
	m_dwEnterVehicleTick = 0;
	m_bEnteredVehicleSpecialActionSeen = false;

	if (m_pPlayerPed) {
		if (m_pPlayerPed->GetStuff() != STUFF_TYPE_NONE) m_pPlayerPed->DropStuff();
		if (m_pPlayerPed->IsDancing()) m_pPlayerPed->StopDancing();
		if (m_pPlayerPed->IsPissing()) m_pPlayerPed->TogglePissing(false);
		if (m_pPlayerPed->IsCarry()) m_pPlayerPed->ToggleCarry(false);
	}

	if (m_pPlayerPed && (!m_pPlayerPed->IsInVehicle() || !m_pPlayerPed->IsAPassenger()
			|| (pVehicle && m_pPlayerPed->GetGtaVehicle() != pVehicle->m_pVehicle)
			|| (m_byteSeatID > 0 && m_pPlayerPed->GetVehicleSeatID() != static_cast<int>(m_byteSeatID)))) {
		PutInCurrentVehicle();
	}

	SetState(PLAYER_STATE_PASSENGER);
}
//trailer sync
void CRemotePlayer::StoreTrailerFullSyncData(TRAILER_SYNC_DATA* trSync)
{
	VEHICLEID trailerId = trSync->trailerId;
	if (!trailerId || trailerId >= MAX_VEHICLES) {
		return;
	}

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (pVehiclePool && m_pCurrentVehicle && m_pCurrentVehicle->VerifyInstance()) {
		sa::CVector vecTrailerPos = {0.0f, 0.0f, 0.0f};
		RwMatrix matTrailer;

		CVehicle* pTrailer = pVehiclePool->GetAt(trailerId);
		if (pTrailer && pTrailer->m_pVehicle && pTrailer->VerifyInstance()
				&& (pTrailer->IsATrailer() || m_pCurrentVehicle->IsATowTruck())) {
			if (m_pCurrentVehicle->GetTrailer() != pTrailer) {
				m_pCurrentVehicle->SetTrailer(pTrailer);
				m_pCurrentVehicle->AttachTrailer();
			}

			matTrailer = pTrailer->m_pVehicle->GetMatrix().ToRwMatrix();
			trSync->quat.GetMatrix(&matTrailer);

			vecTrailerPos.x = FloatOffset(trSync->vecPos.x, matTrailer.pos.x);
			vecTrailerPos.y = FloatOffset(trSync->vecPos.y, matTrailer.pos.y);
			vecTrailerPos.z = FloatOffset(trSync->vecPos.z, matTrailer.pos.z);

			if (vecTrailerPos.x > 0.5f || vecTrailerPos.y > 0.5f || vecTrailerPos.z > 0.5f) {
				if (vecTrailerPos.x > 6.0f || vecTrailerPos.y > 6.0f || vecTrailerPos.z > 3.0f) {
					matTrailer.pos.x = trSync->vecPos.x;
					matTrailer.pos.y = trSync->vecPos.y;
					matTrailer.pos.z = trSync->vecPos.z;

					pTrailer->m_pVehicle->SetMatrix(matTrailer);
					pTrailer->m_pVehicle->SetVelocity(trSync->vecMoveSpeed);
					pTrailer->m_pVehicle->SetTurnSpeed(trSync->vecTurnSpeed);
				}
				else {
					pTrailer->m_pVehicle->SetMatrix(matTrailer);
					pTrailer->m_pVehicle->SetTurnSpeed(trSync->vecTurnSpeed);

					sa::CVector vec = pTrailer->m_pVehicle->GetMoveSpeed();

					if (vecTrailerPos.x > 0.05) {
						vec.x += (trSync->vecPos.x - matTrailer.pos.x) * 0.025f;
					}
					if (vecTrailerPos.y > 0.05) {
						vec.y += (trSync->vecPos.y - matTrailer.pos.y) * 0.025f;
					}
					if (vecTrailerPos.z > 0.05) {
						vec.z += (trSync->vecPos.z - matTrailer.pos.z) * 0.025f;
					}

					pTrailer->m_pVehicle->SetVelocity(vec);
				}
			}
		}
	}
}

// Ported from reference CRemotePlayer::UpdateUnoccupied: applies a remote
// player's unoccupied-vehicle sync — snap on teleport distance, otherwise
// adopt orientation and fold residual offset into velocity.
void CRemotePlayer::StoreUnoccupiedFullSyncData(UNOCCUPIED_SYNC_DATA* ucSync)
{
	if (!ucSync->vehicleId || ucSync->vehicleId == INVALID_VEHICLE_ID) return;
	if (!pNetGame) return;

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) return;

	CVehicle* pVehicle = pVehiclePool->GetAt(ucSync->vehicleId);
	if (!pVehicle || !pVehicle->m_pVehicle || !pVehicle->VerifyInstance()) return;

	sa::CVehicle* pEnt = pVehicle->m_pVehicle;
	if (pEnt->pDriver || pEnt->m_pTowingVehicle) return;

	auto isFiniteVec = [](const sa::CVector& v) -> bool {
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	};
	if (!isFiniteVec(ucSync->vecRoll) || !isFiniteVec(ucSync->vecDirection)
			|| !isFiniteVec(ucSync->vecPos) || !isFiniteVec(ucSync->vecMoveSpeed)
			|| !isFiniteVec(ucSync->vecTurnSpeed)) {
		return;
	}
	if (ucSync->vecRoll.SquaredMagnitude() < 1e-4f || ucSync->vecDirection.SquaredMagnitude() < 1e-4f) {
		return;
	}

	sa::CMatrix mat = pEnt->GetMatrix();
	mat.m_right = ucSync->vecRoll;
	mat.m_forward = ucSync->vecDirection;
	mat.m_up = mat.m_right.Cross(mat.m_forward);
	if (mat.m_up.SquaredMagnitude() < 1e-4f) {
		return;
	}
	mat.m_right.Normalise();
	mat.m_up.Normalise();
	mat.m_forward = mat.m_up.Cross(mat.m_right);

	sa::CVector vecOffset;
	vecOffset.x = FloatOffset(ucSync->vecPos.x, mat.m_pos.x);
	vecOffset.y = FloatOffset(ucSync->vecPos.y, mat.m_pos.y);
	vecOffset.z = FloatOffset(ucSync->vecPos.z, mat.m_pos.z);

	// ignore noise-level updates
	if (vecOffset.x <= 0.1f && vecOffset.y <= 0.1f && vecOffset.z <= 0.1f) {
		return;
	}

	if (vecOffset.x > 6.0f || vecOffset.y > 6.0f || vecOffset.z > 3.0f) {
		mat.m_pos = ucSync->vecPos;
		pEnt->SetMatrix(mat);
		pEnt->SetVelocity(ucSync->vecMoveSpeed);
		pEnt->SetTurnSpeed(ucSync->vecTurnSpeed);
		return;
	}

	pEnt->SetMatrix(mat);
	pEnt->SetVelocity(ucSync->vecMoveSpeed);
	pEnt->SetTurnSpeed(ucSync->vecTurnSpeed);

	sa::CVector vecSpeed = pEnt->GetMoveSpeed();
	if (vecOffset.x > 0.05f) {
		vecSpeed.x += (ucSync->vecPos.x - mat.m_pos.x) * 0.06f;
	}
	if (vecOffset.y > 0.05f) {
		vecSpeed.y += (ucSync->vecPos.y - mat.m_pos.y) * 0.06f;
	}
	if (vecOffset.z > 0.05f) {
		vecSpeed.z += (ucSync->vecPos.z - mat.m_pos.z) * 0.06f;
	}
	pEnt->SetVelocity(vecSpeed);

	pVehicle->m_bRemoteUnoccupiedSync = true;
}

void CRemotePlayer::StoreBulletFullSyncData(BULLET_SYNC_DATA* btSync)
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped || !m_pPlayerPed->IsAdded()) return;

	BULLET_DATA btData;
	memset(&btData, 0, sizeof(BULLET_DATA));

	btData.vecOrigin.x = btSync->vecOrigin.x;
	btData.vecOrigin.y = btSync->vecOrigin.y;
	btData.vecOrigin.z = btSync->vecOrigin.z;

	btData.vecPos.x = btSync->vecPos.x;
	btData.vecPos.y = btSync->vecPos.y;
	btData.vecPos.z = btSync->vecPos.z;

	btData.vecOffset.x = btSync->vecOffset.x;
	btData.vecOffset.y = btSync->vecOffset.y;
	btData.vecOffset.z = btSync->vecOffset.z;

	if (!std::isfinite(btData.vecOrigin.x) || !std::isfinite(btData.vecOrigin.y) || !std::isfinite(btData.vecOrigin.z)
			|| !std::isfinite(btData.vecPos.x) || !std::isfinite(btData.vecPos.y) || !std::isfinite(btData.vecPos.z)
			|| !std::isfinite(btData.vecOffset.x) || !std::isfinite(btData.vecOffset.y) || !std::isfinite(btData.vecOffset.z)) {
		return;
	}

	if (btSync->targetType != BULLET_HIT_TYPE_NONE) {
		if (std::fabs(btData.vecOffset.x) > 300.0f ||
				std::fabs(btData.vecOffset.y) > 300.0f ||
				std::fabs(btData.vecOffset.z) > 300.0f) {
			return;
		}

		sa::CEntity* pEntity = nullptr;

		if (btSync->targetType == BULLET_HIT_TYPE_PLAYER) {
			CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
			if (pPlayerPool) {
				if (btSync->targetId == pPlayerPool->GetLocalPlayerID()) {
					CPlayerPed* pLocalPed = pGame->FindPlayerPed();
					if (pLocalPed && pLocalPed->m_ped) {
						pEntity = reinterpret_cast<sa::CEntity*>(pLocalPed->m_ped);
					}
				}
				else if (btSync->targetId == m_PlayerID) {
					return;
				}
				else if (pPlayerPool->GetSlotState(btSync->targetId)) {
					CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(btSync->targetId);
					CPlayerPed* pPlayerPed = pRemotePlayer ? pRemotePlayer->GetPlayerPed() : nullptr;
					if (pPlayerPed && pPlayerPed->m_ped) {
						pEntity = reinterpret_cast<sa::CEntity*>(pPlayerPed->m_ped);
					}
				}
			}
		}
		else if (btSync->targetType == BULLET_HIT_TYPE_VEHICLE) {
			CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
			if (pVehiclePool) {
				if (pVehiclePool->GetSlotState(btSync->targetId)) {
					CVehicle* pVehicle = pVehiclePool->GetAt(btSync->targetId);
					if (pVehicle && pVehicle->m_pVehicle && pVehicle->VerifyInstance()) {
						pEntity = reinterpret_cast<sa::CEntity*>(pVehicle->m_pVehicle);
					}
				}
			}
		}

		btData.pEntity = pEntity;
	}

	WeaponSyncCheck(btSync->weaponId);

	m_pPlayerPed->ProcessBulletData(&btData);
	m_pPlayerPed->FireInstant();
}

void CRemotePlayer::RemoveFromVehicle()
{
	RwMatrix mat;

	if (m_pPlayerPed && m_pPlayerPed->m_ped) {
		if (m_pPlayerPed->IsInVehicle()) {
			mat = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
			m_pPlayerPed->RemoveFromVehicleAndPutAt(mat.pos.x, mat.pos.y, mat.pos.z);
			m_pCurrentVehicle = nullptr;
		}
	}
}

void CRemotePlayer::UpdateTrainDriverMatrixAndSpeed(RwMatrix* matWorld, sa::CVector* vecMoveSpeed, float fTrainSpeed)
{
	if (!m_pPlayerPed || !m_pCurrentVehicle || !m_pCurrentVehicle->m_pVehicle) {
		return;
	}

	RwMatrix matVehicle;
	sa::CVector vecInternalMoveSpeed;

	matVehicle = m_pCurrentVehicle->m_pVehicle->GetMatrix().ToRwMatrix();

	if (FloatOffset(matWorld->pos.x, matVehicle.pos.x) > 10.0f || FloatOffset(matWorld->pos.y, matVehicle.pos.y) > 10.0f) {
		m_pCurrentVehicle->TeleportTo(matWorld->pos.x, matWorld->pos.y, matWorld->pos.z);
	}

	vecInternalMoveSpeed = m_pCurrentVehicle->m_pVehicle->GetMoveSpeed();

	vecInternalMoveSpeed.x = vecMoveSpeed->x;
	vecInternalMoveSpeed.y = vecMoveSpeed->y;
	vecInternalMoveSpeed.z = vecMoveSpeed->z;

	m_pCurrentVehicle->m_pVehicle->SetVelocity(vecInternalMoveSpeed);
	// m_pCurrentVehicle->SetTrainSpeed(fTrainSpeed);

	if (m_pCurrentVehicle->m_pVehicle) {
		sa::CVehicle* pVeh = m_pCurrentVehicle->m_pVehicle;
		const float safeSpeed = std::isfinite(fTrainSpeed) ? std::clamp(fTrainSpeed, -100.0f, 100.0f) : 0.0f;
		((sa::CTrain*) pVeh)->LinearSpeed = safeSpeed;
	}
}
//Vehicle speed update
void CRemotePlayer::UpdateInCarMatrixAndSpeed(RwMatrix* pMatrix, sa::CVector* pVecPos, sa::CVector* pVecMoveSpeed)
{
	m_quat.SetFromMatrix(pMatrix);
	m_vecInCarTargetPos.x = pVecPos->x;
	m_vecInCarTargetPos.y = pVecPos->y;
	m_vecInCarTargetPos.z = pVecPos->z;

	m_vecInCarTargetSpeed.x = pVecMoveSpeed->x;
	m_vecInCarTargetSpeed.y = pVecMoveSpeed->y;
	m_vecInCarTargetSpeed.z = pVecMoveSpeed->z;

	m_pCurrentVehicle->m_pVehicle->SetVelocity(m_vecInCarTargetSpeed);
}
//Vehicle position update
void CRemotePlayer::UpdateInCarTargetPosition()
{
	if (!m_pCurrentVehicle || !m_pCurrentVehicle->m_pVehicle || !m_pCurrentVehicle->VerifyInstance()) {
		return;
	}

	RwMatrix matEnt = m_pCurrentVehicle->m_pVehicle->GetMatrix().ToRwMatrix();

	if (m_pCurrentVehicle->IsAdded()) {
		m_vecPositionInaccuracy.x = FloatOffset(m_vecInCarTargetPos.x, matEnt.pos.x);
		m_vecPositionInaccuracy.y = FloatOffset(m_vecInCarTargetPos.y, matEnt.pos.y);
		m_vecPositionInaccuracy.z = FloatOffset(m_vecInCarTargetPos.z, matEnt.pos.z);

		if (m_vecPositionInaccuracy.x > 0.05f ||
				m_vecPositionInaccuracy.y > 0.05f ||
				m_vecPositionInaccuracy.z > 0.05f) {
			float fTestZInaccuracy = 0.5f;
			if (m_pCurrentVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_BOAT ||
					m_pCurrentVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_PLANE ||
					m_pCurrentVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_HELI) {
				fTestZInaccuracy = 2.0f;
			}

			if (m_vecPositionInaccuracy.x > 8.0f ||
					m_vecPositionInaccuracy.y > 8.0f ||
					m_vecPositionInaccuracy.z > fTestZInaccuracy) {
				matEnt.pos.x = m_vecInCarTargetPos.x;
				matEnt.pos.y = m_vecInCarTargetPos.y;
				matEnt.pos.z = m_vecInCarTargetPos.z;
				m_pCurrentVehicle->m_pVehicle->SetMatrix(matEnt);
				m_pCurrentVehicle->m_pVehicle->SetVelocity(m_vecInCarTargetSpeed);
			}
			else {
				sa::CVector vecMoveSpeed = m_pCurrentVehicle->m_pVehicle->GetMoveSpeed();

				if (m_vecPositionInaccuracy.x > 0.05f) {
					vecMoveSpeed.x += (m_vecInCarTargetPos.x - matEnt.pos.x) * 0.06f;
				}
				if (m_vecPositionInaccuracy.y > 0.05f) {
					vecMoveSpeed.y += (m_vecInCarTargetPos.y - matEnt.pos.y) * 0.06f;
				}
				if (m_vecPositionInaccuracy.z > 0.05f) {
					vecMoveSpeed.z += (m_vecInCarTargetPos.z - matEnt.pos.z) * 0.06f;
				}
				if (FloatOffset(vecMoveSpeed.x, 0.0f) > 0.01f ||
						FloatOffset(vecMoveSpeed.y, 0.0f) > 0.01f ||
						FloatOffset(vecMoveSpeed.z, 0.0f) > 0.01f) {
					m_pCurrentVehicle->m_pVehicle->SetVelocity(vecMoveSpeed);
				}
			}
		}
	}
	else {
		matEnt.pos.x = m_vecInCarTargetPos.x;
		matEnt.pos.y = m_vecInCarTargetPos.y;
		matEnt.pos.z = m_vecInCarTargetPos.z;
		m_pCurrentVehicle->m_pVehicle->SetMatrix(matEnt);
	}
}

void CRemotePlayer::UpdateVehicleRotation()
{
	sa::CQuaternion quat, qresult;
	RwMatrix matEnt;
	sa::CVector vec = {0.0f, 0.0f, 0.0f};

	if (!m_pCurrentVehicle || !m_pCurrentVehicle->m_pVehicle || !m_pCurrentVehicle->VerifyInstance()) {
		return;
	}

	vec = m_pCurrentVehicle->m_pVehicle->GetTurnSpeed();
	if (vec.x <= 0.02f) {
		if (vec.x < -0.02f) {
			vec.x = -0.02f;
		}
	}
	else {
		vec.x = 0.02f;
	}

	if (vec.y <= 0.02f) {
		if (vec.y < -0.02f) {
			vec.y = -0.02f;
		}
	}
	else {
		vec.y = 0.02f;
	}

	if (vec.z <= 0.02f) {
		if (vec.z < -0.02f) {
			vec.z = -0.02f;
		}
	}
	else {
		vec.z = 0.02f;
	}

	m_pCurrentVehicle->m_pVehicle->SetTurnSpeed(vec);

	matEnt = m_pCurrentVehicle->m_pVehicle->GetMatrix().ToRwMatrix();
	quat.SetFromMatrix(&matEnt);
	qresult.Slerp(&m_quat, &quat, 0.75f);
	qresult.Normalize();
	qresult.GetMatrix(&matEnt);
	m_pCurrentVehicle->m_pVehicle->SetMatrix(matEnt);
}

void CRemotePlayer::PutInCurrentVehicle()
{
	spdlog::info("PutInCurrentVehicle");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();

	if (pVehiclePool && m_pPlayerPed) {
		CVehicle* pVehicle = pVehiclePool->GetAt(m_VehicleID);

		if (pVehicle) {
			uint8_t byteCurWeapon;
			if (m_byteSeatID != 0) {
				byteCurWeapon = m_passengerSync.byteCurrentWeapon;
			}
			else {
				byteCurWeapon = m_inCarSync.byteCurrentWeapon;
			}

			WeaponSyncCheck(byteCurWeapon & 0x3F);

			uint32_t dwGTAId = pVehiclePool->FindGtaIDFromID(m_VehicleID);
			m_pPlayerPed->PutDirectlyInVehicle(dwGTAId, m_byteSeatID);
		}
	}
}

float CRemotePlayer::GetDistanceFromLocalPlayer()
{
	if (!m_pPlayerPed) {
		return 10000.0f;
	}

	if (GetState() == PLAYER_STATE_DRIVER && m_pCurrentVehicle && m_pPlayerPed->IsInVehicle()) {
		return m_pCurrentVehicle->GetDistanceFromLocalPlayerPed();
	}
	else {
		return m_pPlayerPed->GetDistanceFromLocalPlayerPed();
	}
}
//Other players enter the vehicle
void CRemotePlayer::EnterVehicle(VEHICLEID VehicleID, bool bPassenger)
{
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) return;
	CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);

	if (m_pPlayerPed && pVehicle && !m_pPlayerPed->IsInVehicle()) {
		uint32_t dwGTAId = pVehiclePool->FindGtaIDFromID(VehicleID);
		if (dwGTAId != INVALID_VEHICLE_ID) {
			m_dwEnterVehicleTick = GetTickCount();
			m_bEnteredVehicleSpecialActionSeen = false;
			m_pPlayerPed->SetKeys(0, 0, 0);
			m_pPlayerPed->EnterVehicle(dwGTAId, bPassenger);
		}
	}
}

void CRemotePlayer::ExitVehicle()
{
	m_dwEnterVehicleTick = 0;
	m_bEnteredVehicleSpecialActionSeen = false;
	if (m_pPlayerPed && (m_pPlayerPed->IsInVehicle() || m_pPlayerPed->IsEnteringVehicle() != 0)) {
		m_pPlayerPed->SetKeys(0, 0, 0);
		m_pPlayerPed->ExitCurrentVehicle();
	}
}
//Remote player global flags
void CRemotePlayer::ShowGlobalMarker(short sPosX, short sPosY, short sPosZ)
{
	m_bGlobalMarkerShown = true;

	if (m_dwMarker) {
		pGame->DisableMarker(m_dwMarker);
		m_dwMarker = 0;
	}
	// Remote markers must be disabled when the ped is loaded, using the markers in the PlayerPed to update in real time, there is a 1 second delay for this remote marker
	if (!m_pPlayerPed) {
		m_dwMarker = pGame->CreateRadarMarkerIcon(0, sPosX, sPosY, sPosZ, GetPlayerColor(), 0);
	}
}

void CRemotePlayer::HideGlobalMarker()
{
	if (m_dwMarker) {
		pGame->DisableMarker(m_dwMarker);
		m_dwMarker = 0;
	}

	m_bGlobalMarkerShown = false;
}

void CRemotePlayer::StateChange(uint8_t byteNewState, uint8_t byteOldState)
{
	if (byteNewState == PLAYER_STATE_DRIVER && byteOldState != PLAYER_STATE_DRIVER) {
		CPlayerPed* pPlayerPed = pGame->FindPlayerPed();

		if (pPlayerPed) {
			// if the player is the driver
			if (pPlayerPed->IsInVehicle() && !pPlayerPed->IsAPassenger()) {
				CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
				sa::CVehicle* pGtaVehicle = pPlayerPed->GetGtaVehicle();

				if (pVehiclePool && pVehiclePool->FindIDFromGtaPtr(pGtaVehicle) == m_VehicleID) {
					// Match PC: warp local ped out immediately when car-jacked
					RwMatrix mat = pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
					pPlayerPed->RemoveFromVehicleAndPutAt(mat.pos.x, mat.pos.y, mat.pos.z + 1.0f);
					if (pGame) {
						pGame->DisplayGameText("~r~Car Jacked", 2000, 5);
					}
				}
			}
		}
	}
}

void CRemotePlayer::ProcessSpecialAction(unsigned char byteSpecialAction)
{
	if (!m_pPlayerPed || !m_pPlayerPed->IsAdded()) {
		return;
	}

	if (GetState() != PLAYER_STATE_ONFOOT) {
		byteSpecialAction = SPECIAL_ACTION_NONE;
		m_onFootSync.byteSpecialAction = SPECIAL_ACTION_NONE;
	}

	m_pPlayerPed->SetSpecialAction(byteSpecialAction);

	if (!m_bIsNPC && GetState() == PLAYER_STATE_ONFOOT && m_pPlayerPed->IsAdded()
			&& Settings::headMove() && m_pPlayerPed->GetStuff() == STUFF_TYPE_NONE
			&& (m_onFootSync.wKeys & 0x84) == 0
			&& m_pPlayerPed->IsEnteringVehicle() == 0 && !m_pPlayerPed->IsExitingVehicle()) {
		//head turn sync
		if ((GetTickCount() - m_dwLastHeadUpdate) > 500) {
			CAMERA_AIM* Aim = GameGetRemotePlayerAim(m_pPlayerPed->m_bytePlayerNumber);
			if (Aim && m_pPlayerPed->m_ped) {
				RwMatrix matHead = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
				sa::CVector LookAt;
				LookAt.x = matHead.pos.x + (Aim->f1x * 20.0f);
				LookAt.y = matHead.pos.y + (Aim->f1y * 20.0f);
				LookAt.z = matHead.pos.z + (Aim->f1z * 20.0f);
				m_pPlayerPed->ApplyCommandTask("FollowPedSA", nullptr, 2000, -1, &LookAt, 0, 0.1f, 500, 3, 0);
			}
			m_dwLastHeadUpdate = GetTickCount();
		}
	}
}

void CRemotePlayer::ProcessAnimation()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped) {
		return;
	}

	const uint32_t dwAnim = m_onFootSync.dwAnimation;
	const bool bNonCustom = ((dwAnim >> 31) != 0) || ((dwAnim & 0xFFFF) == 0);

	if (!bNonCustom) {
		if (dwAnim != m_dwPlayingAnim) {
			m_dwPlayingAnim = dwAnim;
			m_pPlayerPed->ApplyPackedAnimation(dwAnim);
		}
		else {
			// Only re-apply if the animation is flagged loop (bit 0) or freeze (bit 3)
			const uint32_t flags = (dwAnim >> 24) & 0xFF;
			const uint16_t animIdx = static_cast<uint16_t>(dwAnim & 0xFFFF);
			if ((flags & 0x09u) != 0 && !m_pPlayerPed->IsPlayingAnim(animIdx)) {
				m_pPlayerPed->ApplyPackedAnimation(dwAnim);
			}
		}
		m_bWasAnimSettedFlag = true;
	}
	else if (m_bWasAnimSettedFlag) {
		m_dwPlayingAnim = 0;
		// reset animation
		m_pPlayerPed->PlayAnimByIdx(0, 4.0f);
		m_bWasAnimSettedFlag = false;
	}
}

bool CRemotePlayer::IsSurfing()
{
	if (SurfingOnObject() || SurfingOnVehicle()) {
		return true;
	}

	return false;
}

bool CRemotePlayer::SurfingOnVehicle()
{
	if (GetState() == PLAYER_STATE_ONFOOT) {
		if (m_onFootSync.wSurfInfo != 0 && m_onFootSync.wSurfInfo != INVALID_VEHICLE_ID && m_onFootSync.wSurfInfo < MAX_VEHICLES) {
			return true;
		}
	}
	return false;
}

bool CRemotePlayer::SurfingOnObject()
{
	if (GetState() == PLAYER_STATE_ONFOOT) {
		if (m_onFootSync.wSurfInfo != INVALID_VEHICLE_ID && m_onFootSync.wSurfInfo >= MAX_VEHICLES &&
				m_onFootSync.wSurfInfo < MAX_VEHICLES + MAX_OBJECTS) // its an object
		{
			return true;
		}
	}
	return false;
}

void CRemotePlayer::WeaponSyncCheck(uint8_t netCurrentWeapon)
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped) {
		return;
	}

	//weapon sync
	if (m_pPlayerPed->GetCurrentWeapon() != netCurrentWeapon) {
		//Equipped with network-synced weapons
		m_pPlayerPed->SetArmedWeapon(netCurrentWeapon);
		//Second inspection
		if (m_pPlayerPed->GetCurrentWeapon() != netCurrentWeapon) {
			//If still doesn't exist, means there is no such weapon
			//give weapon, and then equip
			m_pPlayerPed->GiveWeapon(netCurrentWeapon, 9999);
			m_pPlayerPed->SetArmedWeapon(netCurrentWeapon);
		}
	}
}

bool CRemotePlayer::ProcessSurfing()
{
	if (!m_pPlayerPed || !m_pPlayerPed->m_ped || GetState() != PLAYER_STATE_ONFOOT
			|| m_onFootSync.wSurfInfo == 0 || m_onFootSync.wSurfInfo == INVALID_VEHICLE_ID) {
		return false;
	}

	const sa::CVector& off = m_onFootSync.vecSurfOffsets;
	if (!std::isfinite(off.x) || !std::isfinite(off.y) || !std::isfinite(off.z)
			|| std::fabs(off.x) > 100.0f || std::fabs(off.y) > 100.0f || std::fabs(off.z) > 100.0f) {
		return false;
	}

	CVehicle* pVehicleSurfing = nullptr;
	CObject* pObjectSurfing = nullptr;

	if (SurfingOnVehicle()) {
		CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
		if (pVehiclePool) {
			CVehicle* pVehicle = pVehiclePool->GetAt(m_onFootSync.wSurfInfo);
			if (pVehicle && pVehicle->VerifyInstance()) {
				pVehicleSurfing = pVehicle;
			}
		}
	}
	else if (SurfingOnObject()) {
		const uint16_t objId = static_cast<uint16_t>(m_onFootSync.wSurfInfo - MAX_VEHICLES);
		CObjectPool* pObjectPool = pNetGame->GetObjectPool();
		if (pObjectPool) {
			CObject* pObject = pObjectPool->GetAt(objId);
			if (pObject && pObject->m_entity && GamePool_Object_GetAt(pObject->m_gtaId) == pObject->m_entity) {
				pObjectSurfing = pObject;
			}
		}
	}

	if (pVehicleSurfing && pVehicleSurfing->m_pVehicle) {
		m_pPlayerPed->SetCollisionChecking(0);
		RwMatrix matEntity = pVehicleSurfing->m_pVehicle->GetMatrix().ToRwMatrix();
		RwMatrix matPlayer = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
		ProjectMatrix(&matPlayer.pos, &matEntity, &m_onFootSync.vecSurfOffsets);
		m_pPlayerPed->m_ped->SetMatrix(matPlayer);
		m_pPlayerPed->UpdateRwMatrixAndFrame();
		return true;
	}
	else if (pObjectSurfing && pObjectSurfing->m_entity) {
		if (!m_pPlayerPed->m_ped->m_bUsesCollision) {
			m_pPlayerPed->SetCollisionChecking(1);
		}
		RwMatrix matEntity = pObjectSurfing->m_entity->GetMatrix().ToRwMatrix();
		RwMatrix matPlayer = m_pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
		ProjectMatrix(&matPlayer.pos, &matEntity, &m_onFootSync.vecSurfOffsets);
		m_pPlayerPed->m_ped->SetMatrix(matPlayer);
		m_pPlayerPed->UpdateRwMatrixAndFrame();
		return true;
	}
	return false;
}
