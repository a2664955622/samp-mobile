#include "Game.h"
#include "PlayerContextScope.h"
#include "../UI/UI.h"
#include "../Net/NetGame.h"
#include "../Net/Proto/Sync.h"
#include "../Client.h"
#include "RenderWare/rphanim.h"
#include "PlayerPed.h"
#include "../Java/EditObject.h"

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;
extern sa::CPlayerInfo* WORLD_PLAYERS;

CPlayerPed::CPlayerPed()
{
	m_gtaId = 1;
	m_ped = GamePool_FindPlayerPed();
	m_entity = GamePool_FindPlayerPed();
	m_bytePlayerNumber = 0;
	SetPlayerPedPtrRecord(m_bytePlayerNumber, m_ped);
	ScriptCommand(&set_actor_weapon_droppable, m_gtaId, 1);
	ScriptCommand(&set_actor_can_be_decapitated, m_gtaId, 0);

	m_bCuffedState = false;
	m_bCarryState = false;

	m_iDanceState = 0;
	m_bCellPhoneEnabled = false;
	m_bPissingState = 0;
	m_dwPissParticlesHandle = 0;

	m_dwArrow = 0;

	m_iParachuteState = 0;
	m_iParachuteAnim = 0;
	m_dwParachuteObject = 0;

	m_iDrunkLevel = 0;
	m_dwDrunkLastUpdateTick = 0;
	m_bDrunkVisualsActive = false;
	m_bDrunkHudHidden = false;

	for (auto& m_attachedObject : m_attachedObjects) {
		m_attachedObject.slotUsed = false;
		memset(&m_attachedObject.info, 0, sizeof(NEW_ATTACHED_OBJECT));
		m_attachedObject.object = nullptr;
	}
}

CPlayerPed::CPlayerPed(int iNum, int iSkin, float fX, float fY, float fZ, float fRotation)
{
	spdlog::debug("CPlayerPed::CPlayerPed({}, {}, {}, {}, {}, {})", iNum, iSkin, fX, fY, fZ, fRotation);

	uint32_t dwPlayerActorID;
	ScriptCommand(&create_player, &iNum, fX, fY, fZ, &dwPlayerActorID);
	ScriptCommand(&create_actor_from_player, &iNum, &dwPlayerActorID);

	m_gtaId = dwPlayerActorID;
	m_ped = GamePool_Ped_GetAt(m_gtaId);
	m_entity = m_ped;
	m_bytePlayerNumber = iNum;

	SetPlayerPedPtrRecord(iNum, m_ped);

	ScriptCommand(&set_actor_weapon_droppable, m_gtaId, 1);
	ScriptCommand(&set_actor_immunities, m_gtaId, 0, 0, 1, 0, 0);
	ScriptCommand(&set_actor_can_be_decapitated, m_gtaId, 0);

	if (pNetGame) {
		ScriptCommand(&set_actor_money, m_gtaId, 0);
		ScriptCommand(&set_actor_money, m_gtaId, pNetGame->m_netSet->deathDropMoney);
	}

	m_bCuffedState = false;
	m_bCarryState = false;

	m_dwArrow = 0;

	m_iParachuteState = 0;
	m_iParachuteAnim = 0;
	m_dwParachuteObject = 0;

	m_iDrunkLevel = 0;
	m_dwDrunkLastUpdateTick = 0;
	m_bDrunkVisualsActive = false;
	m_bDrunkHudHidden = false;
	m_iDanceState = 0;
	m_bCellPhoneEnabled = false;
	m_bPissingState = 0;
	m_dwPissParticlesHandle = 0;
	m_iStuffType = STUFF_TYPE_NONE;
	m_dwStuffObject = 0;

	if (m_bytePlayerNumber > 0 && m_bytePlayerNumber < PLAYER_PED_SLOTS) {
		memset(&RemotePlayerKeys[m_bytePlayerNumber], 0, sizeof(PAD_KEYS));
		CAMERA_AIM zeroAim{};
		GameStoreRemotePlayerAim(m_bytePlayerNumber, &zeroAim);
		GameSetPlayerCameraMode(4, m_bytePlayerNumber);
		GameSetPlayerCameraExtZoomAndAspect(m_bytePlayerNumber, 1.0f, 0.5f);
		for (int s = 0; s < kWeaponSkillCount; ++s) {
			GameSetRemotePlayerSkill(m_bytePlayerNumber, s, 999);
		}
	}

	SetModelIndex(iSkin);

	if (m_ped) {
		SetTargetRotation(fRotation);
		RwMatrix mat = m_ped->GetMatrix().ToRwMatrix();
		mat.pos.x = fX;
		mat.pos.y = fY;
		mat.pos.z = fZ + 0.15f;
		m_ped->SetMatrix(mat);
	}

	for (auto& m_attachedObject : m_attachedObjects) {
		m_attachedObject.slotUsed = false;
		memset(&m_attachedObject.info, 0, sizeof(NEW_ATTACHED_OBJECT));
		m_attachedObject.object = nullptr;
	}

	m_bHaveBulletData = false;
	memset(&m_bulletData, 0, sizeof(BULLET_DATA));
}

CPlayerPed::~CPlayerPed()
{
	spdlog::debug("Destroying PlayerPed({})", m_bytePlayerNumber);

	if (m_bytePlayerNumber > 0 && m_bytePlayerNumber < PLAYER_PED_SLOTS) {
		memset(&RemotePlayerKeys[m_bytePlayerNumber], 0, sizeof(PAD_KEYS));
		CAMERA_AIM zeroAim{};
		GameStoreRemotePlayerAim(m_bytePlayerNumber, &zeroAim);
		GameSetPlayerCameraMode(4, m_bytePlayerNumber);
		for (int s = 0; s < kWeaponSkillCount; ++s) {
			GameSetRemotePlayerSkill(m_bytePlayerNumber, s, 999);
		}
	}

	if (m_bPissingState || m_dwPissParticlesHandle) {
		TogglePissing(false);
	}

	SetPlayerPedPtrRecord(m_bytePlayerNumber, nullptr);

	// The engine destructor (below) clears CWorld::Players[n].m_pPed, but it is
	// skipped when the entity is placeable/recycled - clear our own record so
	// WORLD_PLAYERS never retains a dangling ped pointer.
	if (WORLD_PLAYERS && m_bytePlayerNumber < PLAYER_PED_SLOTS &&
			WORLD_PLAYERS[m_bytePlayerNumber].m_pPed == m_ped) {
		WORLD_PLAYERS[m_bytePlayerNumber].m_pPed = nullptr;
	}

	if (m_ped && (GamePool_Ped_GetAt(m_gtaId) != nullptr) && !IsEntityPlaceable(m_ped)) {
		if (m_dwParachuteObject) {
			ScriptCommand(&disassociate_object, m_dwParachuteObject, 0.0, 0.0, 0.0, 0);
			ScriptCommand(&destroy_object_with_fade, m_dwParachuteObject);
			m_dwParachuteObject = 0;
		}

		if (HasAttachedObject()) {
			RemoveAllAttachedObjects();
		}

		if (m_iStuffType != STUFF_TYPE_NONE || m_dwStuffObject != 0) {
			DropStuff();
		}

		if (m_ped->IsInVehicle()) {
			RemoveFromVehicleAndPutAt(100.0f, 100.0f, 20.0f);
		}

		if (m_ped->m_pPlayerData) {
			m_ped->m_pPlayerData->m_nPlayerGroup = 0;
		}

		// CPlayerPed::Destructor
		m_ped->Destructor();

		m_ped = nullptr;
		m_entity = nullptr;
	}
	else {
		m_ped = nullptr;
		m_entity = nullptr;
		m_gtaId = 0;
	}

	if (m_dwArrow) {
		ScriptCommand(&disable_marker, m_dwArrow);
		m_dwArrow = 0;
	}
}

void CPlayerPed::ShowMarker(int nIndex)
{
	if (m_dwArrow) {
		ScriptCommand(&disable_marker, m_dwArrow);
		m_dwArrow = 0;
	}
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}
	ScriptCommand(&create_arrow_above_actor, m_gtaId, &m_dwArrow);
	ScriptCommand(&set_marker_color, m_dwArrow, nIndex);
	ScriptCommand(&show_on_radar2, m_dwArrow, 2); //2==player marker
}

bool CPlayerPed::IsInVehicle()
{
	if (!m_ped) {
		return false;
	}

	if (m_ped->IsInVehicle()) {
		return true;
	}

	return false;
}

bool CPlayerPed::IsAPassenger()
{
	if (!m_ped) {
		return false;
	}

	if (m_ped->IsAPassenger()) {
		return true;
	}

	return false;
}

void CPlayerPed::RemoveFromVehicleAndPutAt(float fX, float fY, float fZ)
{
	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}
	if (m_ped && m_ped->IsInVehicle()) {
		ScriptCommand(&remove_actor_from_car_and_put_at, m_gtaId, fX, fY, fZ);
	}
}

void CPlayerPed::SetInitialState()
{
	if (!m_ped) return;
	m_ped->SetInitialState(false);
}

void CPlayerPed::TogglePlayerControllable(bool bControllable)
{
	RwMatrix mat;

	if (GamePool_Ped_GetAt(m_gtaId)) {
		if (bControllable) {
			ScriptCommand(&toggle_player_controllable, m_bytePlayerNumber, 1);
			ScriptCommand(&lock_actor, m_gtaId, 0);

			if (!IsInVehicle()) {
				mat = m_ped->GetMatrix().ToRwMatrix();
				TeleportTo(mat.pos.x, mat.pos.y, mat.pos.z);
			}
		}
		else {
			ScriptCommand(&toggle_player_controllable, m_bytePlayerNumber, 0);
			ScriptCommand(&lock_actor, m_gtaId, 1);
		}
	}
}

void CPlayerPed::RemoveWeaponWhenEnteringVehicle()
{
	if (m_ped) {
		// CPed::RemoveWeaponWhenEnteringVehicle
		eng::fn::CPed_RemoveWeaponWhenEnteringVehicle( m_ped, 0);
	}
}

uint8_t CPlayerPed::GetActionTrigger()
{
	return m_ped ? m_ped->m_nPedState : 0;
}

bool CPlayerPed::IsDead()
{
	if (!m_ped) {
		return true;
	}
	if (m_ped->m_fHealth > 0.0f) {
		return false;
	}
	return true;
}

void CPlayerPed::SetSpecialAction(uint8_t byteSpecialAction)
{
	// duck:start
	if (byteSpecialAction == SPECIAL_ACTION_DUCK && !IsCrouching()) {
		ApplyCrouch();
	}

	// duck:stop
	if (byteSpecialAction != SPECIAL_ACTION_DUCK && IsCrouching()) {
		ResetCrouch();
	}

	// cellphone:start
	if (byteSpecialAction == SPECIAL_ACTION_USECELLPHONE && !IsCellphoneEnabled()) {
		ToggleCellphone(true);
	}

	// cellphone:stop
	if (byteSpecialAction != SPECIAL_ACTION_USECELLPHONE && IsCellphoneEnabled()) {
		ToggleCellphone(false);
	}

	// jetpack:start
	if (byteSpecialAction == SPECIAL_ACTION_USEJETPACK && !IsInJetpack()) {
		StartJetpack();
	}

	// jetpack:stop
	if (byteSpecialAction != SPECIAL_ACTION_USEJETPACK && IsInJetpack()) {
		StopJetpack();
	}

	// carry:start
	if (byteSpecialAction == SPECIAL_ACTION_CARRY && !IsCarry()) {
		ToggleCarry(true);
	}

	// carry:stop
	if (byteSpecialAction != SPECIAL_ACTION_CARRY && IsCarry()) {
		ToggleCarry(false);
	}

	// cuffed:start
	if (byteSpecialAction == SPECIAL_ACTION_CUFFED && !IsCuffed()) {
		ToggleCuffed(true);
	}

	// cuffed:stop
	if (byteSpecialAction != SPECIAL_ACTION_CUFFED && IsCuffed()) {
		ToggleCuffed(false);
	}

	// handsup:start
	if (byteSpecialAction == SPECIAL_ACTION_HANDSUP && !HasHandsUp()) {
		HandsUp();
	}

	// handsup:stop
	if (byteSpecialAction != SPECIAL_ACTION_HANDSUP && HasHandsUp()) {
		TogglePlayerControllable(true);
	}

	// urinating:start
	if (byteSpecialAction == SPECIAL_ACTION_PISSING && !IsPissing()) {
		TogglePissing(true);
	}

	// urinating:stop
	if (byteSpecialAction != SPECIAL_ACTION_PISSING && IsPissing()) {
		TogglePissing(false);
	}

	// T17: dancing special actions (5..8)
	int targetDance = -1;
	switch (byteSpecialAction) {
		case SPECIAL_ACTION_DANCE1: targetDance = 0; break;
		case SPECIAL_ACTION_DANCE2: targetDance = 1; break;
		case SPECIAL_ACTION_DANCE3: targetDance = 2; break;
		case SPECIAL_ACTION_DANCE4: targetDance = 3; break;
		default: break;
	}
	if (targetDance != -1) {
		if (GetDanceStyle() != targetDance) {
			StartDancing(targetDance);
		}
	}
	else if (GetDanceStyle() != -1) {
		StopDancing();
	}

	// T17: drinking & smoking special actions (20..23)
	int targetStuff = STUFF_TYPE_NONE;
	switch (byteSpecialAction) {
		case SPECIAL_ACTION_DRINK_BEER:   targetStuff = STUFF_TYPE_BEER; break;
		case SPECIAL_ACTION_SMOKE_CIGGY:  targetStuff = STUFF_TYPE_CIGGI; break;
		case SPECIAL_ACTION_DRINK_WINE:   targetStuff = STUFF_TYPE_DYN_BEER; break;
		case SPECIAL_ACTION_DRINK_SPRUNK: targetStuff = STUFF_TYPE_PINT_GLASS; break;
		default: break;
	}
	if (targetStuff != STUFF_TYPE_NONE) {
		if (GetStuff() != targetStuff) {
			GiveStuff(targetStuff);
		}
		else if (!IsInVehicle() && GetCurrentWeapon() != 0) {
			SetArmedWeapon(0);
		}
	}
	else if (GetStuff() != STUFF_TYPE_NONE) {
		DropStuff();
	}

	ProcessDancing();
	ProcessParachutes();
}

uint8_t CPlayerPed::GetSpecialAction()
{
	if (IsEnteringVehicle() != 0) {
		return SPECIAL_ACTION_ENTER_VEHICLE;
	}

	if (IsExitingVehicle()) {
		return SPECIAL_ACTION_EXIT_VEHICLE;
	}

	if (IsInJetpack()) {
		return SPECIAL_ACTION_USEJETPACK;
	}

	if (IsCuffed()) {
		return SPECIAL_ACTION_CUFFED;
	}

	if (IsCarry()) {
		return SPECIAL_ACTION_CARRY;
	}

	if (GetDanceStyle() != -1) {
		switch (GetDanceStyle()) {
			case 0: return SPECIAL_ACTION_DANCE1;
			case 1: return SPECIAL_ACTION_DANCE2;
			case 2: return SPECIAL_ACTION_DANCE3;
			case 3: return SPECIAL_ACTION_DANCE4;
		}
	}

	if (HasHandsUp()) {
		return SPECIAL_ACTION_HANDSUP;
	}

	if (IsCellphoneEnabled()) {
		return SPECIAL_ACTION_USECELLPHONE;
	}

	if (IsPissing()) {
		return SPECIAL_ACTION_PISSING;
	}

	if (GetStuff() == STUFF_TYPE_BEER) {
		return SPECIAL_ACTION_DRINK_BEER;
	}

	if (GetStuff() == STUFF_TYPE_DYN_BEER) {
		return SPECIAL_ACTION_DRINK_WINE;
	}

	if (GetStuff() == STUFF_TYPE_PINT_GLASS) {
		return SPECIAL_ACTION_DRINK_SPRUNK;
	}

	if (GetStuff() == STUFF_TYPE_CIGGI) {
		return SPECIAL_ACTION_SMOKE_CIGGY;
	}

	if (IsSitTask()) {
		return SPECIAL_ACTION_SITTING;
	}

	if (IsCrouching()) {
		return SPECIAL_ACTION_DUCK;
	}

	return SPECIAL_ACTION_NONE;
}

char DanceStyleLibs[4][16] = {"WOP", "GFUNK", "RUNNINGMAN", "STRIP"};
char DanceIdleLoops[4][16] = {"DANCE_LOOP", "DANCE_LOOP", "DANCE_LOOP", "STR_Loop_B"};

char szDanceAnimNamesFemale[16][16] = {
		"DANCE_G1", "DANCE_G2", "DANCE_G3", "DANCE_G4",
		"DANCE_G5", "DANCE_G6", "DANCE_G7", "DANCE_G8",
		"DANCE_G9", "DANCE_G10", "DANCE_G11", "DANCE_G12",
		"DANCE_G13", "DANCE_G14", "DANCE_G15", "DANCE_G16"};

char szDanceAnimNamesMale[16][16] = {
		"DANCE_B1", "DANCE_B2", "DANCE_B3", "DANCE_B4",
		"DANCE_B5", "DANCE_B6", "DANCE_B7", "DANCE_B8",
		"DANCE_B9", "DANCE_B10", "DANCE_B11", "DANCE_B12",
		"DANCE_B13", "DANCE_B14", "DANCE_B15", "DANCE_B16"};

char szStripAnims[16][16] = {
		"strip_A", "strip_B", "strip_C", "strip_D",
		"strip_E", "strip_F", "strip_G", "STR_A2B",
		"STR_B2A", "STR_B2C", "STR_C1", "STR_C2",
		"STR_C2B", "STR_A2B", "STR_B2C", "STR_C2"};

bool CPlayerPed::IsDancing()
{
	if (m_iDanceState) {
		return true;
	}
	return false;
}

void CPlayerPed::StopDancing()
{
	if (!m_ped || !IsAdded()) {
		m_iDanceState = 0;
		m_iDanceStyle = -1;
		m_iLastDanceMove = -1;
		return;
	}

	m_iDanceState = 0;
	m_iDanceStyle = -1;
	m_iLastDanceMove = -1;
	RwMatrix mat = m_ped->GetMatrix().ToRwMatrix();
	TeleportTo(mat.pos.x, mat.pos.y, mat.pos.z);
}

void CPlayerPed::ProcessDancing()
{
	if (!m_ped || !IsAdded() || IsInVehicle()) {
		return;
	}

	if (!m_iDanceState || m_iDanceStyle < 0 || m_iDanceStyle > 3) return;
	if (m_bytePlayerNumber != 0) return;

	float fDummyBlend = 4.0f;
	if (GetCurrentAnimationIndex(fDummyBlend) == 0) {
		m_iLastDanceMove = -1;
		ApplyAnimation(DanceIdleLoops[m_iDanceStyle], DanceStyleLibs[m_iDanceStyle], 4.0f, 1, 0, 0, 0, 0);
	}

	const int16_t sUD = static_cast<int16_t>(LocalPlayerKeys.wKeyUD);
	const int16_t sLR = static_cast<int16_t>(LocalPlayerKeys.wKeyLR);
	const bool bExtra = LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT];

	int iNewMove = -1;
	if (sUD < 0) {
		iNewMove = (sLR < 0) ? 4 : (sLR > 0) ? 5 : 0; // UP / UP-LEFT / UP-RIGHT
	}
	else if (sUD > 0) {
		iNewMove = (sLR < 0) ? 6 : (sLR > 0) ? 7 : 1; // DOWN / DOWN-LEFT / DOWN-RIGHT
	}
	else if (sLR < 0) {
		iNewMove = 2; // LEFT
	}
	else if (sLR > 0) {
		iNewMove = 3; // RIGHT
	}
	else {
		return;
	}

	if (bExtra) {
		iNewMove += 8;
	}

	if (iNewMove == m_iLastDanceMove) return; // don't allow the same move twice
	m_iLastDanceMove = iNewMove;

	char* szAnimName = GetDanceAnimForMove(iNewMove);
	if (!szAnimName || !szAnimName[0]) return;

	ApplyAnimation(szAnimName, DanceStyleLibs[m_iDanceStyle], 4.0f, 0, 0, 0, 0, 0);
}

char* CPlayerPed::GetDanceAnimForMove(int iMove)
{
	if (m_iDanceStyle == -1) return ""; // shouldn't ever happen but..

	// style 0-2 have the same anim names, but M/F versions
	if (m_iDanceStyle >= 0 && m_iDanceStyle <= 2) {
		if (GetPedStat() == 5 || GetPedStat() == 22) { // female or pro (only female types)
			return szDanceAnimNamesFemale[iMove];
		}

		return szDanceAnimNamesMale[iMove];
	}
	else if (m_iDanceStyle == 3) {
		return szStripAnims[iMove];
	}

	return "";
}

void CPlayerPed::ToggleCellphone(bool on)
{
	if (!on) {
		m_bCellPhoneEnabled = false;
		if (m_ped && GamePool_Ped_GetAt(m_gtaId) && IsAdded()) {
			ScriptCommand(&toggle_actor_cellphone, m_gtaId, 0);
		}
		return;
	}
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsAdded()) {
		return;
	}

	m_bCellPhoneEnabled = true;
	ScriptCommand(&toggle_actor_cellphone, m_gtaId, 1);
}

void CPlayerPed::ApplyCrouch()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	spdlog::debug("Apply crouch");

	if (!IsCrouching()) {
		if (m_ped->m_pIntelligence) {
			// CPedIntelligence::SetTaskDuckSecondary
			eng::fn::CPedIntelligence_SetTaskDuckSecondary( m_ped->m_pIntelligence, 0);
		}
	}
}

void CPlayerPed::ResetCrouch()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	spdlog::debug("Reset crouch");

	m_ped->m_nPedFlags.bIsDucking = false;

	if (m_ped->m_pIntelligence) {
		// CPedIntelligence::ClearTaskDuckSecondary
		eng::fn::CPedIntelligence_ClearTaskDuckSecondary( m_ped->m_pIntelligence);
	}
}

bool CPlayerPed::IsCrouching()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return false;
	}

	return m_ped->bIsDucking;
}

void CPlayerPed::HandsUp()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || IsInVehicle() || !IsAdded()) {
		return;
	}

	ScriptCommand(&actor_task_handsup, m_gtaId, -2);
}

void CPlayerPed::GiveStuff(int iType)
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsAdded() || IsInVehicle()) {
		return;
	}

	if (m_dwStuffObject) {
		DropStuff();
	}

	int iModel = 0;
	double x = 0.0, y = 0.0, z = 0.0;
	switch (iType) {
		case STUFF_TYPE_BEER:
			iModel = 1543;
			x = 0.05; y = 0.03; z = -0.30;
			break;
		case STUFF_TYPE_DYN_BEER:
			iModel = 1486;
			x = 0.05; y = 0.03; z = -0.05;
			break;
		case STUFF_TYPE_PINT_GLASS:
			iModel = 1546;
			x = 0.04; y = 0.10; z = -0.02;
			break;
		case STUFF_TYPE_CIGGI:
			iModel = 1485;
			x = 0.0; y = 0.0; z = 0.0;
			break;
		default:
			return;
	}

	if (pGame && !pGame->IsModelLoaded(iModel)) {
		pGame->RequestModel(iModel);
		pGame->LoadRequestedModels();
	}

	RwMatrix mat = m_ped->GetMatrix().ToRwMatrix();
	uint32_t dwObj = 0;
	ScriptCommand(&create_object, iModel, mat.pos.x, mat.pos.y, mat.pos.z, &dwObj);
	if (!dwObj || !GamePool_Object_GetAt(dwObj)) {
		m_dwStuffObject = 0;
		return;
	}

	m_dwStuffObject = dwObj;
	ScriptCommand(&task_pick_up_object, m_gtaId, m_dwStuffObject, x, y, z, 6, 16, "NULL", "NULL", -1);
	m_iStuffType = iType;
	SetArmedWeapon(0);
}

void CPlayerPed::DropStuff()
{
	if (m_ped && GamePool_Ped_GetAt(m_gtaId) && m_ped->m_pIntelligence) {
		// Detach & delete CTaskSimpleHoldEntity in secondary slot 4 BEFORE freeing the object pool slot
		// so m_aSecondaryTasks[4] never retains a dangling CObject* across pool slot reuse.
		eng::fn::CTaskManager_SetTaskSecondary(&m_ped->m_pIntelligence->m_TaskMgr, nullptr, 4);
	}

	if (m_dwStuffObject) {
		if (GamePool_Object_GetAt(m_dwStuffObject)) {
			ScriptCommand(&destroy_object, m_dwStuffObject);
		}
		m_dwStuffObject = 0;
	}

	m_iStuffType = STUFF_TYPE_NONE;
}

bool CPlayerPed::ApplyStuff()
{
	if (m_iStuffType == STUFF_TYPE_NONE || !m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsAdded() || IsInVehicle()) {
		return false;
	}

	SetArmedWeapon(0);
	float fDummyBlend = 4.0f;
	if (GetCurrentAnimationIndex(fDummyBlend) != 0) {
		return false;
	}

	if (m_iStuffType == STUFF_TYPE_CIGGI) {
		ApplyAnimation("smkcig_prtl", "GANGS", 700.0f, 0, 0, 0, 0, 2750);
		return true;
	}

	if (m_iStuffType == STUFF_TYPE_BEER || m_iStuffType == STUFF_TYPE_DYN_BEER || m_iStuffType == STUFF_TYPE_PINT_GLASS) {
		if (GetPedStat() == 5 || GetPedStat() == 22) {
			ApplyAnimation("dnk_stndF_loop", "BAR", 4.0f, 0, 0, 0, 0, -1);
		}
		else {
			ApplyAnimation("dnk_stndM_loop", "BAR", 4.0f, 0, 0, 0, 0, -1);
		}
		if (m_bytePlayerNumber == 0 && m_iStuffType != STUFF_TYPE_PINT_GLASS) {
			SetDrunkLevel(GetDrunkLevel() + 1250);
		}
		return true;
	}

	return false;
}

int CPlayerPed::HasHandsUp()
{
	if (!m_ped || !m_ped->m_pIntelligence || !IsAdded()) {
		return false;
	}

	if (m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY] == nullptr) {
		return false;
	}

	uintptr_t vtable = *(uintptr_t*) m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];
	if (vtable == (uintptr_t) g_saSym->Abs(addr::vptr$CTaskSimpleHandsUp)) {
		return true;
	}

	return false;
}

void CPlayerPed::StartJetpack()
{
	if (!m_ped || m_ped->IsInVehicle()) {
		return;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	ScriptCommand(&actor_task_jetpack, m_gtaId);
}

void CPlayerPed::StopJetpack()
{
	if (!m_ped || !m_ped->m_pIntelligence || m_ped->IsInVehicle()) {
		return;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	if (m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY] == nullptr) {
		return;
	}

	if (IsInJetpack()) {
		sa::CTask* dwJetPackTask = m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];
		// CTaskSimpleJetPack::~CTaskSimpleJetPack(CTaskSimpleJetPack *this)
		eng::fn::CTaskSimpleJetPack_Dtor_CTaskSimpleJetPack( dwJetPackTask);
		m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY] = nullptr;
	}
}

bool CPlayerPed::IsInJetpack()
{
	if (!m_ped || !m_ped->m_pIntelligence || m_ped->IsInVehicle()) {
		return false;
	}

	if (m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY] == nullptr) {
		return false;
	}

	uintptr_t vtable = *(uintptr_t*) m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];
	if (vtable == (uintptr_t) g_saSym->Abs(addr::vptr$CTaskSimpleJetPack)) {
		return true;
	}

	return false;
}

bool CPlayerPed::IsTakeDamageFallTask()
{
	if (m_ped && !IsInVehicle() && m_ped->m_pIntelligence && m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		return GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) == sa::TASK_COMPLEX_FALL_AND_GET_UP;
	}

	return false;
}

uint8_t CPlayerPed::IsEnteringVehicle()
{
	if (m_ped && m_ped->m_pIntelligence && m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		int iType = GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]);
		if (iType == sa::TASK_COMPLEX_ENTER_CAR_AS_PASSENGER || iType == sa::TASK_COMPLEX_ENTER_CAR_AS_PASSENGER_TIMED) {
			return 2;
		}
		if (iType == sa::TASK_COMPLEX_ENTER_CAR_AS_DRIVER || iType == sa::TASK_COMPLEX_ENTER_CAR_AS_DRIVER_TIMED) {
			return 1;
		}
	}
	return 0;
}

bool CPlayerPed::IsExitingVehicle()
{
	if (m_ped && m_ped->m_pIntelligence && m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		return GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) == sa::TASK_COMPLEX_LEAVE_CAR;
	}

	return false;
}

bool CPlayerPed::IsJumpTask()
{
	if (m_ped && !IsInVehicle() && m_ped->m_pIntelligence && m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		return GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) == sa::TASK_COMPLEX_JUMP;
	}

	return false;
}

bool CPlayerPed::IsSitTask()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || IsInVehicle() || !m_ped->m_pIntelligence || !m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		return false;
	}

	return GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) == sa::TASK_SIMPLE_SIT_DOWN ||
			GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) == sa::TASK_SIMPLE_SIT_IDLE;
}

bool CPlayerPed::CanDoPassengerDriveBy()
{
	if (!m_ped || !m_ped->IsInVehicle() || !m_ped->pVehicle) {
		return false;
	}
	int iWeapon = GetCurrentWeapon();
	switch (iWeapon) {
		case WEAPON_COLT45:
		case WEAPON_SILENCED:
		case WEAPON_DEAGLE:
		case WEAPON_TEC9:
		case WEAPON_UZI:
		case WEAPON_MP5:
		case WEAPON_AK47:
		case WEAPON_M4:
		case WEAPON_SAWEDOFF:
		case WEAPON_SHOTGSPA:
		case WEAPON_MINIGUN:
		case WEAPON_GRENADE:
		case WEAPON_MOLOTOV:
		case WEAPON_SATCHEL:
			return true;
		default:
			return false;
	}
}

bool CPlayerPed::IsInPassengerDriveByMode()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsInVehicle() || !m_ped->m_pIntelligence ||
			!m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		return false;
	}

	return GetTaskTypeFromTask(&m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) == sa::TASK_SIMPLE_GANG_DRIVEBY;
}

bool CPlayerPed::StartPassengerDriveByMode()
{
	if (m_ped) {
		if (!m_ped->IsInVehicle() || !m_ped->pVehicle || m_ped->m_nPedState != ACTION_INCAR) {
			return false;
		}

		int iWeapon = GetCurrentWeapon();

		if (iWeapon == WEAPON_PARACHUTE) {
			SetArmedWeapon(0);
			return false;
		}

		if (!CanDoPassengerDriveBy()) {
			return false;
		}

		SetArmedWeapon(iWeapon);

		ScriptCommand(&enter_passenger_driveby, m_gtaId, -1, -1, 0.0f, 0.0f, 0.0f, 300.0f, 8, 1, 100);

		return true;
	}
	return false;
}

void CPlayerPed::StopPassengerDriveByMode()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsInVehicle()) {
		return;
	}

	if (IsInPassengerDriveByMode()) {
		sa::CTask* pDriveByTask = m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];
		eng::fn::CTaskSimpleGangDriveBy_Dtor_CTaskSimpleGangDriveBy(pDriveByTask); // CTaskSimpleGangDriveBy::~CTaskSimpleGangDriveBy
		m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY] = nullptr;
	}
}

bool IsTaskRunNamedAnim(sa::CTask* pTask)
{
	if (!pTask) return false;
	const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(pTask);
	// eng::vmtSlot(sym, 1) == sym + 2*sizeof(void*), which is the Itanium ABI object vptr.
	if (vtable == eng::vmtSlot(eng::Sym::vtable_CTaskSimpleRunNamedAnim, 1)) {
		return true;
	}
	return GetTaskTypeFromTask(&pTask) == 401;
}

sa::CTask* GetSubTaskFromTask(sa::CTask* pTask)
{
	uintptr_t vtable = *(uintptr_t*) pTask;
	return Memory::callFunction<sa::CTask*>(*reinterpret_cast<void**>(vtable + 3 * sizeof(void*)), pTask);
}

// Disassembly-verified CTaskSimpleRunNamedAnim field offsets from libGTASA.so
// (_ZN23CTaskSimpleRunNamedAnimC2EPKcS1_ifibbbb). Itanium C++ ABI packs derived
// fields directly into CTaskSimpleAnim's tail padding (+13 arm32 / +25 arm64).
static constexpr size_t kRunAnimNameOff  = VER_x32 ? 0x0D : 0x19;
static constexpr size_t kRunAnimGroupOff = VER_x32 ? 0x25 : 0x31;
static constexpr size_t kRunAnimBlendOff = VER_x32 ? 0x38 : 0x44;
static constexpr size_t kRunAnimTimeOff  = VER_x32 ? 0x40 : 0x50;
static constexpr size_t kRunAnimFlagsOff = VER_x32 ? 0x5C : 0x6C;

uint32_t CPlayerPed::GetCurrentAnimationIndexFlag()
{
	if (m_ped && m_gtaId && GamePool_Ped_GetAt(m_gtaId) && m_ped->m_pRwObject && m_ped->m_pIntelligence
			&& IsEnteringVehicle() == 0 && !IsExitingVehicle()) {
		sa::CTask* pTask = m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];
		while (pTask && !IsTaskRunNamedAnim(pTask)) {
			pTask = GetSubTaskFromTask(pTask);
		}
		if (pTask) {
			const auto* raw = reinterpret_cast<const uint8_t*>(pTask);
			const char* szName = reinterpret_cast<const char*>(raw + kRunAnimNameOff);
			const char* szGroup = reinterpret_cast<const char*>(raw + kRunAnimGroupOff);
			std::string szStr(szGroup);
			szStr += ":";
			szStr += szName;

			const int idx = GetAnimIdxByName(szStr.c_str());
			if (idx != -1) {
				const uint16_t usAnimIdx = static_cast<uint16_t>(idx + 1);
				const float fBlendDelta = *reinterpret_cast<const float*>(raw + kRunAnimBlendOff);
				int iSpeed = static_cast<int>(fBlendDelta);
				if (iSpeed < 0) iSpeed = 0;
				if (iSpeed > 255) iSpeed = 255;
				const uint32_t speed = static_cast<uint32_t>(iSpeed);

				const uint32_t animFlags = *reinterpret_cast<const uint32_t*>(raw + kRunAnimFlagsOff);
				const int32_t animTime = *reinterpret_cast<const int32_t*>(raw + kRunAnimTimeOff);
				const uint32_t direction = (animTime > 0) ? 2u : (animTime < 0 ? 1u : 0u);
				const uint32_t packedFlags =
						(direction << 4) |
						((animFlags & 0x08u) ? 0u : 8u) |
						((animFlags & 0x80u) ? 4u : 0u) |
						((animFlags & 0x40u) ? 2u : 0u) |
						((animFlags & 0x02u) ? 1u : 0u);

				return static_cast<uint32_t>(usAnimIdx) | (speed << 16) | (packedFlags << 24);
			}
		}
	}

	// Non-scripted movement: return last engine blended animation for local ped (PC parity)
	if (m_bytePlayerNumber == 0) {
		return PackLocalPedQueuedAnim();
	}
	return static_cast<uint32_t>(1189u) | (4u << 16) | (0x80u << 24);
}

int CPlayerPed::GetCurrentAnimationIndex(float& blendData)
{
	blendData = 4.0f;

	if (!m_ped || !m_gtaId) {
		return 0;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return 0;
	}

	if (!m_ped->m_pRwObject) {
		return 0;
	}
	sa::CPedIntelligence* pIntelligence = m_ped->m_pIntelligence;

	if (pIntelligence) {
		sa::CTask* pTask = pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY];

		if (pTask) {
			while (!IsTaskRunNamedAnim(pTask)) {
				pTask = GetSubTaskFromTask(pTask);
				if (!pTask) {
					return 0;
				}
			}

			const auto* raw = reinterpret_cast<const uint8_t*>(pTask);
			const char* szName = reinterpret_cast<const char*>(raw + kRunAnimNameOff);
			const char* szGroupName = reinterpret_cast<const char*>(raw + kRunAnimGroupOff);
			blendData = *reinterpret_cast<const float*>(raw + kRunAnimBlendOff);

			std::string szStr = std::string(szGroupName);
			szStr += ":";
			szStr += szName;

			int idx = GetAnimIdxByName(szStr.c_str());
			if (idx == -1) {
				return 0;
			}
			else {
				return idx + 1;
			}
		}
	}
	return 0;
}

void CPlayerPed::ToggleCuffed(bool cuff)
{
	if (!cuff) {
		m_bCuffedState = false;
		return;
	}
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsAdded()) {
		return;
	}

	SetArmedWeapon(0);
	m_bCuffedState = true;
}

void CPlayerPed::ToggleCarry(bool carry)
{
	if (!carry) {
		m_bCarryState = false;
		if (m_ped && GamePool_Ped_GetAt(m_gtaId) && IsAdded() && !IsInVehicle()) {
			ApplyAnimation("CRRY_PRTIAL", "CARRY", 4.00, 0, 0, 0, 0, 1);
		}
		return;
	}
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || !IsAdded()) {
		return;
	}

	m_bCarryState = true;
	ApplyAnimation("CRRY_PRTIAL", "CARRY", 4.1, 0, 0, 0, 1, 1);
}

void CPlayerPed::TogglePissing(bool piss)
{
	if (!piss) {
		if (m_dwPissParticlesHandle) {
			ScriptCommand(&destroy_particle, m_dwPissParticlesHandle);
			m_dwPissParticlesHandle = 0;
		}
		const bool wasPissing = m_bPissingState;
		m_bPissingState = false;
		if (wasPissing && m_ped && GamePool_Ped_GetAt(m_gtaId) && IsAdded() && !IsInVehicle()) {
			RwMatrix mat = m_ped->GetMatrix().ToRwMatrix();
			TeleportTo(mat.pos.x, mat.pos.y, mat.pos.z);
		}
		return;
	}

	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId) || IsInVehicle() || !IsAdded()) {
		return;
	}

	if (m_bPissingState) {
		return;
	}

	ApplyAnimation("PISS_LOOP", "PAULNMAC", 4.0f, 1, 0, 0, 0, -1);

	ScriptCommand(&attach_particle_to_actor2, "PETROLCAN", m_gtaId, 0.0f, 0.58f, -0.08f, 0.0f, 0.01f, 0.0f, 1, &m_dwPissParticlesHandle);
	ScriptCommand(&make_particle_visible, m_dwPissParticlesHandle);

	m_bPissingState = true;
}

void CPlayerPed::StartDancing(int danceId)
{
	if (!m_ped || IsInVehicle() || !IsAdded()) {
		return;
	}

	if (danceId < 0 || danceId > 3) return;

	m_iDanceState = 1;
	m_iDanceStyle = danceId;

	ApplyAnimation(DanceIdleLoops[m_iDanceStyle], DanceStyleLibs[m_iDanceStyle], 16.0, 1, 0, 0, 0, -1);
}

void CPlayerPed::ExtinguishFire()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	if (m_ped->m_pFire) {
		eng::fn::CFire_Extinguish(m_ped->m_pFire);
	}

	if (m_ped->m_nPedFlags.bInVehicle && m_ped->pVehicle && m_ped->pVehicle->m_pFire) {
		eng::fn::CFire_Extinguish(m_ped->pVehicle->m_pFire);
	}
}

void CPlayerPed::SetImmunities(int bullet, int flame, int explode, int collision, int melee)
{
	if (m_ped && GamePool_Ped_GetAt(m_gtaId)) {
		ScriptCommand(&set_actor_immunities, m_gtaId, bullet, flame, explode, collision, melee);
	}
}

void CPlayerPed::SetSkillLevel(int iSkillID, int iLevel)
{
	if (iSkillID < 0 || iSkillID >= kWeaponSkillCount) return;
	const uint16_t clamped = static_cast<uint16_t>(iLevel < 0 ? 0 : (iLevel > 1000 ? 1000 : iLevel));
	if (m_bytePlayerNumber == 0) {
		GameSetLocalPlayerSkill(iSkillID, clamped);
	}
	else {
		GameSetRemotePlayerSkill(m_bytePlayerNumber, iSkillID, clamped);
	}
}
void CPlayerPed::UpdateLocalPlayerSkill(int iSkillID, int iLevel)
{
	if (iSkillID < 0 || iSkillID >= kWeaponSkillCount) return;
	const uint16_t clamped = static_cast<uint16_t>(iLevel < 0 ? 0 : (iLevel > 1000 ? 1000 : iLevel));
	GameSetLocalPlayerSkill(iSkillID, clamped);
}

void CPlayerPed::SetFightingStyle(int iStyle)
{
	if (m_gtaId) {
		ScriptCommand(&set_fighting_style, m_gtaId, iStyle, 6);
	}
}

float CPlayerPed::GetHealth()
{
	return m_ped ? m_ped->m_fHealth : 0.0f;
}

int CPlayerPed::GetVehicleSeatID()
{
	if (!m_ped) return -1;
	if (m_ped->m_nPedState == ACTION_INCAR && m_ped->pVehicle) {
		sa::CVehicle* pVehicle = (sa::CVehicle*) m_ped->pVehicle;

		if (pVehicle->pDriver == m_ped) {
			return 0;
		}
		for (int i = 0; i < 8; i++) {
			if (pVehicle->m_apPassengers[i] == m_ped) {
				return i + 1;
			}
		}
	}

	return -1;
}

void CPlayerPed::SetStateFlags(sa::CPedFlags dwState)
{
	if (m_ped) {
		m_ped->m_nPedFlags = dwState;
	}
}

sa::CPedFlags CPlayerPed::GetStateFlags()
{
	if (m_ped) {
		return m_ped->m_nPedFlags;
	}
	return {};
}

bool CPlayerPed::IsOnGround()
{
	if (m_ped) {
		const auto& flags = m_ped->m_nPedFlags;
		return flags.bIsStanding || flags.bWasStanding;
	}
	return false;
}

void CPlayerPed::SetDead()
{
	RwMatrix mat;

	if (m_gtaId && m_ped) {
		if (IsInJetpack()) {
			StopJetpack();
		}
		if (m_iStuffType != STUFF_TYPE_NONE || m_dwStuffObject != 0) {
			DropStuff();
		}
		if (!m_ped->IsInVehicle()) {
			// ~
		}

		ExtinguishFire();
		mat = m_ped->GetMatrix().ToRwMatrix();
		TeleportTo(mat.pos.x, mat.pos.y, mat.pos.z);
		m_ped->m_fHealth = 0.0f;
		{
			ScopedPlayerContext ctx(m_bytePlayerNumber, ScopedPlayerContext::kFocusOnly);
			ScriptCommand(&kill_actor, m_gtaId);
		}
	}
}

void CPlayerPed::SetHealth(float fHealth)
{
	if (m_ped) {
		m_ped->m_fHealth = fHealth;
	}
}

float CPlayerPed::GetArmour()
{
	if (!m_ped) {
		return 0.0f;
	}
	return m_ped->m_fArmour;
}

void CPlayerPed::SetArmour(float fArmour)
{
	if (!m_ped) {
		return;
	}
	m_ped->m_fArmour = fArmour;
}

sa::CWeapon* CPlayerPed::GetCurrentWeaponSlot()
{
	if (!m_ped || m_ped->m_nActiveWeaponSlot >= 13) return nullptr;
	return &m_ped->m_aWeapons[m_ped->m_nActiveWeaponSlot];
}

uint8_t CPlayerPed::GetCameraMode()
{
	return m_bytePlayerNumber == 0 ? GameGetLocalPlayerCameraMode() : GameGetPlayerCameraMode(m_bytePlayerNumber);
}

float CPlayerPed::GetAimZ()
{
	if (!m_ped || !m_ped->m_pPlayerData) return 0;
	return m_ped->m_pPlayerData->m_fLookPitch;
}

CAMERA_AIM* CPlayerPed::GetCurrentAim()
{
	return GameGetInternalAim();
}

uint8_t CPlayerPed::GetCurrentWeapon()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return 0;
	}

	// crashes
	/*uint8_t weapon = 0;
	ScriptCommand(&get_actor_armed_weapon, m_gtaId, &weapon);
	return weapon;*/

	sa::CWeapon* pWeaponSlot = GetCurrentWeaponSlot();
	return pWeaponSlot ? pWeaponSlot->dwType : 0;
}

sa::CVehicle* CPlayerPed::GetGtaVehicle()
{
	return m_ped ? m_ped->pVehicle : nullptr;
}

void CPlayerPed::SatisfyHunger()
{
	// todo
}

void CPlayerPed::SetDrunkLevel(int32_t iLevel)
{
	if (iLevel > 50000) {
		iLevel = 50000;
	}
	else if (iLevel < 0) {
		iLevel = 0;
	}

	m_iDrunkLevel = iLevel;

	// Clear visuals/handling immediately when dropping to <= 2000 (including 0)
	// so SetPlayerDrunkLevel(playerid, 0) never leaves permanent camera sway.
	if (!m_bytePlayerNumber && m_ped) {
		if (m_iDrunkLevel <= 2000 && m_bDrunkVisualsActive) {
			ScriptCommand(&set_player_drunk_visuals, m_bytePlayerNumber, 0);
			ScriptCommand(&set_player_drunk_handling, m_bytePlayerNumber, 0);
			m_bDrunkVisualsActive = false;
		}
		if (m_iDrunkLevel <= 5000 && m_bDrunkHudHidden) {
			if (pGame) {
				pGame->SetHudHidden(Game::HUD_HIDE_DRUNK, false);
			}
			m_bDrunkHudHidden = false;
		}
	}
}

void CPlayerPed::DrunkProcess()
{
	if (!m_ped || m_bytePlayerNumber != 0) {
		return;
	}

	int32_t iDrunkLevel = GetDrunkLevel();
	if (iDrunkLevel <= 0) {
		if (m_bDrunkVisualsActive) {
			ScriptCommand(&set_player_drunk_visuals, m_bytePlayerNumber, 0);
			ScriptCommand(&set_player_drunk_handling, m_bytePlayerNumber, 0);
			m_bDrunkVisualsActive = false;
		}
		if (m_bDrunkHudHidden) {
			if (pGame) {
				pGame->SetHudHidden(Game::HUD_HIDE_DRUNK, false);
			}
			m_bDrunkHudHidden = false;
		}
		return;
	}

	// Hide HUD/radar when drunk level > 5000 (SA-MP PC & docs contract)
	if (iDrunkLevel > 5000) {
		if (!m_bDrunkHudHidden && pGame) {
			pGame->SetHudHidden(Game::HUD_HIDE_DRUNK, true);
			m_bDrunkHudHidden = true;
		}
	}
	else if (m_bDrunkHudHidden) {
		if (pGame) {
			pGame->SetHudHidden(Game::HUD_HIDE_DRUNK, false);
		}
		m_bDrunkHudHidden = false;
	}

	if (iDrunkLevel <= 2000) {
		m_iDrunkLevel = iDrunkLevel - 1;
		if (m_bDrunkVisualsActive) {
			ScriptCommand(&set_player_drunk_visuals, m_bytePlayerNumber, 0);
			ScriptCommand(&set_player_drunk_handling, m_bytePlayerNumber, 0);
			m_bDrunkVisualsActive = false;
		}
	}
	else {
		int iDrunkVisual = static_cast<int>(iDrunkLevel * 0.02f);
		if (iDrunkVisual <= 250) {
			if (iDrunkVisual < 5) {
				iDrunkVisual = 0;
			}
		}
		else {
			iDrunkVisual = 250;
		}

		m_iDrunkLevel = iDrunkLevel - 1;
		ScriptCommand(&set_player_drunk_visuals, m_bytePlayerNumber, iDrunkVisual);
		m_bDrunkVisualsActive = (iDrunkVisual > 0);

		if (IsInVehicle() && !IsAPassenger()) {
			sa::CVehicle* _pVehicle = GetGtaVehicle();
			if (_pVehicle) {
				if (!m_dwDrunkLastUpdateTick || (GetTickCount() - m_dwDrunkLastUpdateTick) > 200) {
					int iRandNumber = rand() % 40;
					float fRotation = 0.0f;
					if (iRandNumber >= 20) {
						fRotation = (iDrunkLevel >= 5000) ? 0.015f : 0.012f;
						if (iRandNumber <= 30) {
							fRotation = -fRotation;
						}
					}

					if (FloatOffset(_pVehicle->m_vecMoveSpeed.x, 0.0) > 0.050000001f ||
							FloatOffset(_pVehicle->m_vecMoveSpeed.y, 0.0) > 0.050000001f) {
						_pVehicle->m_vecTurnSpeed.z = fRotation + _pVehicle->m_vecTurnSpeed.z;
					}

					m_dwDrunkLastUpdateTick = GetTickCount();
				}
			}
		}
	}
}

void CPlayerPed::SetKeys(uint16_t lrAnalog, uint16_t udAnalog, uint16_t wKeys)
{
	PAD_KEYS* pad = &RemotePlayerKeys[m_bytePlayerNumber];
	// LEFT/RIGHT
	pad->wKeyLR = lrAnalog;
	// UP/DOWN
	pad->wKeyUD = udAnalog;

	// KEY_ACTION
	pad->bKeys[ePadKeys::KEY_ACTION] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_CROUCH
	pad->bKeys[ePadKeys::KEY_CROUCH] = (wKeys & 1);
	if (!pad->bKeys[ePadKeys::KEY_CROUCH]) {
		pad->bIgnoreDuck = false;
	}
	wKeys >>= 1;
	// KEY_FIRE
	pad->bKeys[ePadKeys::KEY_FIRE] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_SPRINT
	pad->bKeys[ePadKeys::KEY_SPRINT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_SECONDARY_ATTACK
	pad->bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_JUMP
	pad->bKeys[ePadKeys::KEY_JUMP] = (wKeys & 1);
	if (!pad->bKeys[ePadKeys::KEY_JUMP]) {
		pad->bIgnoreJump = false;
	}
	wKeys >>= 1;
	// KEY_LOOK_RIGHT
	pad->bKeys[ePadKeys::KEY_LOOK_RIGHT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_HANDBRAKE
	pad->bKeys[ePadKeys::KEY_HANDBRAKE] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_LOOK_LEFT
	pad->bKeys[ePadKeys::KEY_LOOK_LEFT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_SUBMISSION
	pad->bKeys[ePadKeys::KEY_SUBMISSION] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_WALK
	pad->bKeys[ePadKeys::KEY_WALK] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_UP
	pad->bKeys[ePadKeys::KEY_ANALOG_UP] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_DOWN
	pad->bKeys[ePadKeys::KEY_ANALOG_DOWN] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_LEFT
	pad->bKeys[ePadKeys::KEY_ANALOG_LEFT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_RIGHT
	pad->bKeys[ePadKeys::KEY_ANALOG_RIGHT] = (wKeys & 1);
}

uint16_t CPlayerPed::GetKeys(uint16_t* lrAnalog, uint16_t* udAnalog, uint8_t* ext, bool clear)
{
	*lrAnalog = LocalPlayerKeys.wKeyLR;
	*udAnalog = LocalPlayerKeys.wKeyUD;

	// ext
	if (ext != nullptr) {
		*ext = 0;
		if (LocalPlayerKeys.bKeys[ePadKeys::KEY_YES]) {
			*ext = 1;
		}
		if (LocalPlayerKeys.bKeys[ePadKeys::KEY_NO]) {
			*ext = 2;
		}
		if (LocalPlayerKeys.bKeys[ePadKeys::KEY_CTRL_BACK]) {
			*ext = 3;
		}
	}

	uint16_t wRet = 0;

	// KEY_ANALOG_RIGHT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_RIGHT]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_ANALOG_LEFT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_LEFT]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_ANALOG_DOWN
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_DOWN]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_ANALOG_UP
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_UP]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_WALK
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_WALK]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_SUBMISSION
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SUBMISSION]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_LOOK_LEFT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_LEFT]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_HANDBRAKE
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_LOOK_RIGHT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_RIGHT]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_JUMP
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_SECONDARY_ATTACK
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_SPRINT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_FIRE
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_CROUCH
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH]) {
		wRet |= 1;
	}
	wRet <<= 1;
	// KEY_ACTION
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ACTION]) {
		wRet |= 1;
	}

	if (clear) {
		memset(LocalPlayerKeys.bKeys, 0, ePadKeys::SIZE);
	}

	return wRet;
}

void CPlayerPed::ApplyAnimation(const char* szAnimName, const char* szAnimLib, float fT, int opt1, int opt2, int opt3, int opt4, int iTime)
{
	if (!m_ped) {
		return;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	if (!szAnimLib || !strlen(szAnimLib)) {
		return;
	}

	if (!strcasecmp(szAnimLib, "SEX")) {
		return;
	}
	if (!eng::fn::CAnimManager_GetAnimationBlock<void*>(szAnimLib)) {
		return;
	}

	int iWaitAnimLoad = 0;

	if (!pGame->IsAnimationLoaded(szAnimLib)) {
		pGame->RequestAnimation(szAnimLib);
		pGame->LoadRequestedModels();
		while (!pGame->IsAnimationLoaded(szAnimLib)) {
			usleep(1000);
			iWaitAnimLoad++;
			if (iWaitAnimLoad > 15) {
				spdlog::error("Animation {} not loaded!", szAnimLib);
				return;
			}
		}
	}

	spdlog::debug("Animation {} loaded!", szAnimLib);

	ScriptCommand(&apply_animation, m_gtaId, szAnimName, szAnimLib, fT, opt1, opt2, opt3, opt4, iTime);
}

void CPlayerPed::SetInterior(uint8_t byteInteriorId, bool bRefresh)
{
	if (!m_ped) return;
	if (m_bytePlayerNumber != 0) {
		ScriptCommand(&link_actor_to_interior, m_gtaId, byteInteriorId);
	}
	else {
		ScriptCommand(&select_interior, byteInteriorId);
		ScriptCommand(&link_actor_to_interior, m_gtaId, byteInteriorId);
		if (bRefresh) {
			RwMatrix mat = m_ped->GetMatrix().ToRwMatrix();
			spdlog::debug("refresh streaming at x: {}, y: {}", mat.pos.x, mat.pos.y);
			pGame->RefreshStreamingAt(mat.pos.x, mat.pos.y);
		}
	}
}

void CPlayerPed::ClearWeapons()
{
	if (!m_ped || !pbyteCurrentPlayer) {
		return;
	}

	ScopedPlayerContext ctx(m_bytePlayerNumber, ScopedPlayerContext::kFocusOnly);
	m_ped->ClearWeapons();
}

void CPlayerPed::ResetHealth()
{
	if (!m_ped) return;
	m_ped->m_fHealth = 100;
}

static RpAtomic* CountClumpVerticesCB(RpAtomic* atomic, void* data)
{
	if (atomic && data) {
		// Disassembly-verified RenderWare offsets in libGTASA.so v2.10 (matching Hooks.cpp)
		constexpr size_t kAtomicGeomOff   = VER_x32 ? 24 : 48;
		constexpr size_t kGeomNumVertsOff = VER_x32 ? 20 : 28;
		const auto* rawAtomic = reinterpret_cast<const uint8_t*>(atomic);
		const auto* geom = *reinterpret_cast<const uint8_t* const*>(rawAtomic + kAtomicGeomOff);
		if (geom) {
			const int32_t numVerts = *reinterpret_cast<const int32_t*>(geom + kGeomNumVertsOff);
			if (numVerts > 0) {
				*static_cast<int*>(data) += numVerts;
			}
		}
	}
	return atomic;
}

static int CountClumpVertices(RwObject* rwObj)
{
	if (!rwObj) {
		return 0;
	}
	int totalVerts = 0;
	using AtomicCB = RpAtomic* (*)(RpAtomic*, void*);
	eng::fn::RpClumpForAllAtomics(
		reinterpret_cast<RpClump*>(rwObj),
		static_cast<AtomicCB>(&CountClumpVerticesCB),
		&totalVerts);
	return totalVerts;
}

uint EnsureValidPedModelPrototype(uint uiModel)
{
	if (!IsValidPedModel(uiModel)) {
		spdlog::error("EnsureValidPedModelPrototype: invalid model {}", uiModel);
		uiModel = 0;
	}

	// P1-16: player.dff in gta3.img is a 1-atomic placeholder clump with only 2 triangles /
	// 6 vertices (skeleton holder); because clean SAMP/main.scm runs 0053 (create_player)
	// without 070D (rebuild_player), GetModelRWObject(0) is non-null with 1 atomic (6 verts)
	// and CJ renders invisible. Detect the placeholder via vertex count (< 100) and run
	// CClothes::ConstructPedModel(0, ...) once when texdb/player.img is present (no runtime
	// .text unpatch); fall back to skin 29 if unavailable.
	if (uiModel == 0 && pGame) {
		RwObject* rwModel0 = GetModelRWObject(0);
		if (!rwModel0) {
			pGame->RequestModel(0);
			pGame->LoadRequestedModels();
			rwModel0 = GetModelRWObject(0);
		}
		if (CountClumpVertices(rwModel0) < 100) {
			static int s_cjImgState = -1; // -1=unchecked, 0=missing, 1=present
			if (s_cjImgState < 0) {
				const char* dir = Client::gameDir();
				std::string base = dir ? std::string(dir) : std::string();
				if (!base.empty() && base.back() != '/') {
					base.push_back('/');
				}
				const bool hasPlayerImg = !base.empty() && (
					access((base + "texdb/player.img").c_str(), F_OK) == 0 ||
					access((base + "TEXDB/PLAYER.IMG").c_str(), F_OK) == 0 ||
					access((base + "texdb/PLAYER.IMG").c_str(), F_OK) == 0);
				s_cjImgState = hasPlayerImg ? 1 : 0;
				if (!hasPlayerImg) {
					spdlog::warn("EnsureValidPedModelPrototype(0): texdb/player.img missing; falling back to skin 29");
				}
			}

			if (s_cjImgState == 1 && rwModel0) {
				sa::CPed* pLocalGtaPed = GamePool_FindPlayerPed();
				if (pLocalGtaPed && pLocalGtaPed->m_pPlayerData && pLocalGtaPed->m_pPlayerData->m_pPedClothesDesc) {
					void* clothesDesc = pLocalGtaPed->m_pPlayerData->m_pPedClothesDesc;
					// Default classic CJ outfit (white tank top, default head, blue jeans, black sneakers)
					eng::fn::CPedClothesDesc_SetTextureAndModel(clothesDesc, "vest", "vest", 0);
					eng::fn::CPedClothesDesc_SetTextureAndModel(clothesDesc, "player_face", "head", 1);
					eng::fn::CPedClothesDesc_SetTextureAndModel(clothesDesc, "jeansdenim", "jeans", 2);
					eng::fn::CPedClothesDesc_SetTextureAndModel(clothesDesc, "sneakerbincblk", "sneaker", 3);
					eng::fn::CClothes_ConstructPedModel(0u, clothesDesc, static_cast<const void*>(nullptr), false);
					rwModel0 = GetModelRWObject(0);
					spdlog::info("EnsureValidPedModelPrototype(0): CClothes::ConstructPedModel built {} vertices",
						CountClumpVertices(rwModel0));
				}
			}

			if (CountClumpVertices(rwModel0) < 100) {
				spdlog::warn("EnsureValidPedModelPrototype(0): model 0 still placeholder (<100 verts); falling back to skin 29");
				uiModel = 29;
			}
		}
	}

	return uiModel;
}

void CPlayerPed::SetModelIndex(uint uiModel)
{
	spdlog::debug("SetModelIndex: {}", uiModel);

	if (!m_ped) {
		spdlog::error("SetModelIndex: m_ped is null");
		return;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		spdlog::error("SetModelIndex: invalid gta id");
		return;
	}

	uiModel = EnsureValidPedModelPrototype(uiModel);

	// Preserve seated vehicle state across DeleteRWObject -> SetModelIndex:
	// rebuilding RpClump/RpHAnimHierarchy while seated severs CAR_sit / BIKEs_Ride and T-poses.
	bool bRestoreVehicle = false;
	uint32_t savedVehGtaId = 0;
	uint8_t savedSeatId = 0;
	if (IsInVehicle()) {
		if (sa::CVehicle* pVeh = GetGtaVehicle()) {
			const uint32_t vehGtaId = GamePool_Vehicle_GetIndex(pVeh);
			const int seatId = GetVehicleSeatID();
			if (vehGtaId != 0 && GamePool_Vehicle_GetAt(vehGtaId) == pVeh && seatId >= 0) {
				bRestoreVehicle = true;
				savedVehGtaId = vehGtaId;
				savedSeatId = static_cast<uint8_t>(seatId);
				const RwMatrix vehMat = pVeh->GetMatrix().ToRwMatrix();
				RemoveFromVehicleAndPutAt(vehMat.pos.x, vehMat.pos.y, vehMat.pos.z + 0.5f);
			}
		}
	}

	// CClothes::RebuildPlayer (registered in PatchRegistry as Session patch)
	patches::set("CClothes::RebuildPlayer", true);

	DestroyFollowPedTask();

	CEntity::SetModelIndex(uiModel);

	// CAEPedSpeechAudioEntity::Initialise
	eng::fn::CAEPedSpeechAudioEntity_Initialise( &m_ped->m_PedSpeechAudioEntity, m_ped);

	if (bRestoreVehicle) {
		PutDirectlyInVehicle(savedVehGtaId, savedSeatId);
	}
}

void CPlayerPed::DestroyFollowPedTask()
{
	if (!m_ped || !m_ped->m_pIntelligence) {
		return;
	}

	sa::CTask* dwIK = m_ped->m_pIntelligence->m_TaskMgr.m_aSecondaryTasks[sa::TASK_SECONDARY_IK];
	if (dwIK) {
		// CTaskSimpleIKManager::~CTaskSimpleIKManager
		eng::fn::CTaskSimpleIKManager_Dtor_CTaskSimpleIKManager( dwIK);
		m_ped->m_pIntelligence->m_TaskMgr.m_aSecondaryTasks[sa::TASK_SECONDARY_IK] = nullptr;
	}
}

void CPlayerPed::RestartIfWastedAt(sa::CVector* vecRestart, float fRotation)
{
	ScriptCommand(&restart_if_wasted_at, vecRestart->x, vecRestart->y, vecRestart->z, fRotation, 0);
}

void CPlayerPed::SetTargetRotation(float fRotation)
{
	if (m_ped && GamePool_Ped_GetAt(m_gtaId)) {
		m_ped->m_fCurrentRotation = DegToRad(fRotation);
		m_ped->m_fAimingRotation = DegToRad(fRotation);
		ScriptCommand(&set_actor_z_angle, m_gtaId, fRotation);
	}
}

void CPlayerPed::GiveWeapon(int iWeaponId, int iAmmo)
{
	if (m_ped && GamePool_Ped_GetAt(m_gtaId)) {
		int iWeaponModelID = GameGetWeaponModelIDFromWeaponID(iWeaponId);
		if (iWeaponModelID != -1) {
			if (!pGame->IsModelLoaded(iWeaponModelID)) {
				pGame->RequestModel(iWeaponModelID);
				pGame->LoadRequestedModels();
				int waitTries = 0;
				while (!pGame->IsModelLoaded(iWeaponModelID)) {
					usleep(1000);
					if (++waitTries > 2000) {
						spdlog::error("CPlayerPed::GiveWeapon: weapon model {} wouldn't load in time", iWeaponModelID);
						return;
					}
				}
			}

			{
				const uint32_t ctxFlags = (m_bytePlayerNumber != 0)
					? (ScopedPlayerContext::kFocus | ScopedPlayerContext::kSkills)
					: ScopedPlayerContext::kFocusOnly;
				ScopedPlayerContext ctx(m_bytePlayerNumber, ctxFlags);
				// CPed::GiveWeapon
				eng::fn::CPed_GiveWeapon( m_ped, iWeaponId, iAmmo, true);
				SetArmedWeapon(iWeaponId);
			}
		}
	}
}

void CPlayerPed::SetArmedWeapon(uint8_t weapon)
{
	if (m_ped && GamePool_Ped_GetAt(m_gtaId)) {
		const uint32_t ctxFlags = (m_bytePlayerNumber != 0)
			? (ScopedPlayerContext::kFocus | ScopedPlayerContext::kSkills)
			: ScopedPlayerContext::kFocusOnly;
		ScopedPlayerContext ctx(m_bytePlayerNumber, ctxFlags);
		m_ped->SetCurrentWeapon(weapon);
	}
}

void CPlayerPed::SetAmmo(uint8_t byteWeapon, uint16_t wAmmo)
{
	if (m_ped) {
		sa::CWeapon* WeaponSlot = FindWeaponSlot(byteWeapon);
		if (WeaponSlot) {
			if (wAmmo == 0) {
				const bool wasArmed = (GetCurrentWeapon() == byteWeapon);
				WeaponSlot->dwAmmo = 0;
				WeaponSlot->dwAmmoInClip = 0;
				if (wasArmed) {
					SetArmedWeapon(0);
				}
			}
			else {
				WeaponSlot->dwAmmo = static_cast<uint32_t>(wAmmo);
				if (WeaponSlot->dwAmmoInClip > WeaponSlot->dwAmmo) {
					WeaponSlot->dwAmmoInClip = WeaponSlot->dwAmmo;
				}
			}
		}
	}
}

sa::CWeapon* CPlayerPed::FindWeaponSlot(uint8_t byteWeapon)
{
	if (!m_ped) {
		return nullptr;
	}

	for (auto& m_weaponSlot : m_ped->m_aWeapons) {
		if (m_weaponSlot.dwType == byteWeapon) {
			return &m_weaponSlot;
		}
	}

	return nullptr;
}

void CPlayerPed::ApplyCommandTask(const char* szTaskName, void* p1, int p2, int p3, sa::CVector* p4, uint8_t p5, float p6, int p7, int p8, uint8_t p9)
{
	if (!m_ped) {
		return;
	}

	// IKChainManager_c::LookAt
	eng::fn::IKChainManager_c_LookAt( eng::ptr(eng::Sym::g_ikChainMan), szTaskName, (uintptr_t) m_ped, p1, p2, p3, p4, p5, p6, p7, p8, p9);
}

void CPlayerPed::ProcessParachuteSkydiving()
{
	int16_t udAnalog = 0;
	if (m_bytePlayerNumber != 0 && m_bytePlayerNumber < PLAYER_PED_SLOTS) {
		udAnalog = static_cast<int16_t>(RemotePlayerKeys[m_bytePlayerNumber].wKeyUD);
	}
	else {
		uint16_t lrRaw = 0, udRaw = 0;
		uint8_t exKeys = 0;
		GetKeys(&lrRaw, &udRaw, &exKeys, false);
		udAnalog = static_cast<int16_t>(udRaw);
	}
	RwMatrix mat;

	if ((udAnalog > 0) && (m_iParachuteAnim != FALL_SKYDIVE_ACCEL)) {
		ApplyAnimation("FALL_SKYDIVE_ACCEL", "PARACHUTE", 1.0f, 1, 0, 0, 1, -2);
		m_iParachuteAnim = FALL_SKYDIVE_ACCEL;
	}
	else if ((udAnalog <= 0) && (m_iParachuteAnim != FALL_SKYDIVE)) {
		ApplyAnimation("FALL_SKYDIVE", "PARACHUTE", 1.0f, 1, 0, 0, 1, -2);
		m_iParachuteAnim = FALL_SKYDIVE;
	}

	// if the parachute object isn't created, do it now.
	if (!m_dwParachuteObject) {
		spdlog::debug("Creating parachute object");
		mat = m_ped->GetMatrix().ToRwMatrix();
		ScriptCommand(&create_object, OBJECT_PARACHUTE, mat.pos.x, mat.pos.y, mat.pos.z, &m_dwParachuteObject);

		if (!GamePool_Object_GetAt(m_dwParachuteObject)) {
			spdlog::info("Parachute object creation failed");
			m_dwParachuteObject = 0;
			return;
		}

		ScriptCommand(&attach_object_to_actor, m_dwParachuteObject, m_gtaId, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		ScriptCommand(&set_object_visible, m_dwParachuteObject, 0);
	}

	if (!GamePool_Object_GetAt(m_dwParachuteObject)) {
		spdlog::info("Parachute object creation failed");
		m_dwParachuteObject = 0;
		return;
	}

	// process parachute opening event
	if (m_bytePlayerNumber < PLAYER_PED_SLOTS && RemotePlayerKeys[m_bytePlayerNumber].bKeys[ePadKeys::KEY_FIRE]) {
		ApplyAnimation("PARA_OPEN", "PARACHUTE", 8.0f, 0, 0, 0, 1, -2);
		ScriptCommand(&apply_object_animation, m_dwParachuteObject, "PARA_OPEN_O", "PARACHUTE", 1000.0f, 0, 1);
		ScriptCommand(&set_object_visible, m_dwParachuteObject, 1);
		ScriptCommand(&set_object_scale, m_dwParachuteObject, 1.0f);
		m_iParachuteState = 2;
		m_iParachuteAnim = 0;
	}
}

void CPlayerPed::ProcessParachutes()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	if (!IsAdded()) {
		return;
	}

	if (m_iParachuteState == 0) {
		if (m_dwParachuteObject) {
			ScriptCommand(&disassociate_object, m_dwParachuteObject, 0.0f, 0.0f, 0.0f, 0);
			ScriptCommand(&destroy_object_with_fade, m_dwParachuteObject);
			m_dwParachuteObject = 0;
		}

		// See if we should enter the initial parachuting state.
		if (GetCurrentWeapon() == WEAPON_PARACHUTE) // armed with para
		{
			if (ScriptCommand(&is_actor_falling_think, m_gtaId)) // is falling
			{
				float fDistanceFromGround = 0.0f;
				ScriptCommand(&get_actor_distance_from_ground, m_gtaId, &fDistanceFromGround);
				if (fDistanceFromGround > 20.0f) {
					// ScriptCommand(&actor_set_collision, m_dwGTAId, 0);
					m_iParachuteState = 1;
					m_iParachuteAnim = 0;
				}
			}
		}
		return;
	}

	if ((GetCurrentWeapon() != WEAPON_PARACHUTE) || ScriptCommand(&is_actor_in_the_water, m_gtaId)) {
		// A parachuting state is active, but they no longer have the parachute
		// or they've ended up in the water.
		if (m_dwParachuteObject) {
			ScriptCommand(&disassociate_object, m_dwParachuteObject, 0.0f, 0.0f, 0.0f, 0);
			ScriptCommand(&destroy_object_with_fade, m_dwParachuteObject);
			// ScriptCommand(&actor_set_collision, m_dwGTAId, 1);
			RwMatrix mat = m_ped->GetMatrix().ToRwMatrix();
			TeleportTo(mat.pos.x, mat.pos.y, mat.pos.z);
			m_dwParachuteObject = 0;
		}

		m_iParachuteState = 0;
		m_iParachuteAnim = 0;
	}

	if (m_iParachuteState == 1) {
		ProcessParachuteSkydiving();
		return;
	}

	if (m_iParachuteState == 2) {
		ProcessParachuting();
		return;
	}
}

void CPlayerPed::ProcessParachuting()
{
	int16_t udAnalog = 0;
	if (m_bytePlayerNumber != 0 && m_bytePlayerNumber < PLAYER_PED_SLOTS) {
		udAnalog = static_cast<int16_t>(RemotePlayerKeys[m_bytePlayerNumber].wKeyUD);
	}
	else {
		uint16_t lrRaw = 0, udRaw = 0;
		uint8_t exKeys = 0;
		GetKeys(&lrRaw, &udRaw, &exKeys, false);
		udAnalog = static_cast<int16_t>(udRaw);
	}

	const bool hasParaObj = (m_dwParachuteObject != 0 && GamePool_Object_GetAt(m_dwParachuteObject) != nullptr);
	if ((udAnalog > 0) && (m_iParachuteAnim != PARA_DECEL)) {
		ApplyAnimation("PARA_DECEL", "PARACHUTE", 1.0f, 1, 0, 0, 1, -2);
		if (hasParaObj) {
			ScriptCommand(&apply_object_animation, m_dwParachuteObject, "PARA_DECEL_O", "PARACHUTE", 1.0f, 1, 1);
		}
		m_iParachuteAnim = PARA_DECEL;
	}
	else if ((udAnalog <= 0) && (m_iParachuteAnim != PARA_FLOAT)) {
		ApplyAnimation("PARA_FLOAT", "PARACHUTE", 1.0f, 1, 0, 0, 1, -2);
		if (hasParaObj) {
			ScriptCommand(&apply_object_animation, m_dwParachuteObject, "PARA_FLOAT_O", "PARACHUTE", 1.0f, 1, 1);
		}
		m_iParachuteAnim = PARA_FLOAT;
	}
}

void CPlayerPed::ClearPrimaryTask()
{
	if (m_ped && GamePool_Ped_GetAt(m_gtaId) && m_ped->m_pIntelligence
			&& m_ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[sa::TASK_PRIMARY_PRIMARY]) {
		eng::fn::CTaskManager_SetTask(&m_ped->m_pIntelligence->m_TaskMgr, nullptr, sa::TASK_PRIMARY_PRIMARY, false);
	}
}

void CPlayerPed::PlayAnimByIdx(int idx, float BlendData)
{
	if (!m_ped) return;
	if (!idx) {
		// Only clear primary task if it is a scripted CTaskSimpleRunNamedAnim (401);
		// never call TeleportTo() here (which wipes CTaskSimpleUseGun and CTaskComplexEnterCar).
		if (m_ped->m_pIntelligence) {
			if (eng::fn::CTaskManager_FindActiveTaskByType(&m_ped->m_pIntelligence->m_TaskMgr, 401) != nullptr) {
				ClearPrimaryTask();
			}
		}
		return;
	}

	std::string szAnim;
	std::string szBlock;

	char pszAnim[40] = {0};
	char pszBlock[40] = {0};

	bool bTest = false;
	const char* pBegin = GetAnimByIdx(idx - 1);
	if (!pBegin) {
		return;
	}
	while (*pBegin) {
		if (*pBegin == ':') {
			pBegin++;
			bTest = true;
			continue;
		}
		if (!bTest) {
			szBlock += *pBegin;
		}
		if (bTest) {
			szAnim += *pBegin;
		}
		pBegin++;
	}

	snprintf(pszAnim, sizeof(pszAnim), "%s", szAnim.c_str());
	snprintf(pszBlock, sizeof(pszBlock), "%s", szBlock.c_str());

	ApplyAnimation(&pszAnim[0], &pszBlock[0], BlendData, 0, 1, 1, 0, 0);
}

void CPlayerPed::ApplyPackedAnimation(uint32_t dwPacked)
{
	if (!m_ped) return;
	const uint16_t idx = static_cast<uint16_t>(dwPacked & 0xFFFF);
	if (idx == 0) {
		PlayAnimByIdx(0, 4.0f);
		return;
	}

	const char* pCombined = GetAnimByIdx(idx - 1);
	if (!pCombined) {
		return;
	}
	const char* pColon = strchr(pCombined, ':');
	if (!pColon || pColon == pCombined || !pColon[1]) {
		return;
	}

	char pszBlock[40] = {0};
	char pszAnim[40] = {0};
	const size_t blockLen = static_cast<size_t>(pColon - pCombined);
	snprintf(pszBlock, sizeof(pszBlock), "%.*s", static_cast<int>(blockLen), pCombined);
	snprintf(pszAnim, sizeof(pszAnim), "%s", pColon + 1);

	float fSpeed = static_cast<float>((dwPacked >> 16) & 0xFF);
	if (fSpeed <= 0.0f) {
		fSpeed = 4.0f;
	}
	const uint32_t flags = (dwPacked >> 24) & 0xFF;
	const uint32_t direction = (flags >> 4) & 3u;
	const int iTime = (direction == 2u) ? 1 : ((direction == 1u) ? -1 : 0);

	ApplyAnimation(
			pszAnim,
			pszBlock,
			fSpeed,
			(flags & 1u) ? 1 : 0,
			(flags & 2u) ? 1 : 0,
			(flags & 4u) ? 1 : 0,
			(flags & 8u) ? 1 : 0,
			iTime);
}

bool CPlayerPed::IsPlayingAnim(int idx)
{
	if (!m_ped || !m_gtaId || (idx == 0)) {
		return false;
	}
	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return false;
	}
	if (!m_ped->m_pRwObject) {
		return false;
	}

	const char* pAnim = GetAnimByIdx(idx - 1);
	if (!pAnim) {
		return false;
	}
	const char* pColon = strchr(pAnim, ':');
	if (!pColon || !pColon[1]) {
		return false;
	}
	const char* pNameAnim = pColon + 1;

	auto blendAssoc = eng::fn::RpAnimBlendClumpGetAssociation( m_ped->m_pRwObject, pNameAnim);

	return blendAssoc != 0;
}

void CPlayerPed::SetRotation(float fRotation)
{
	if (m_ped) {
		if (GamePool_Ped_GetAt(m_gtaId)) {
			m_ped->m_fCurrentRotation = DegToRad(fRotation);
			m_ped->m_fAimingRotation = DegToRad(fRotation);
		}
	}
}

void CPlayerPed::ExitCurrentVehicle()
{
	if (!m_ped) {
		return;
	}
	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	if (m_ped->IsInVehicle()) {
		int dwGtaVehicleID = GetCurrentVehicleID();
		if (dwGtaVehicleID) {
			sa::CVehicle* pGtaVehicle = GamePool_Vehicle_GetAt(dwGtaVehicleID);
			if (pGtaVehicle) {
				int iModelIndex = pGtaVehicle->m_nModelIndex;
				if (iModelIndex != TRAIN_PASSENGER && iModelIndex != TRAIN_PASSENGER_LOCO) {
					//trigger script command LeaveCar(ped,vehicle);
					ScriptCommand(&make_actor_leave_car, m_gtaId, dwGtaVehicleID);
				}
			}
		}
	}
	else if (IsEnteringVehicle() != 0) {
		ClearPrimaryTask();
	}
}

int CPlayerPed::GetCurrentVehicleID()
{
	if (m_ped) {
		return GamePool_Vehicle_GetIndex(m_ped->pVehicle);
	}

	return 0;
}

void CPlayerPed::CheckVehicleParachute()
{
	if (m_dwParachuteObject) {
		ScriptCommand(&disassociate_object, m_dwParachuteObject, 0.0, 0.0, 0.0, 0);
		ScriptCommand(&destroy_object, m_dwParachuteObject);
		m_dwParachuteObject = 0;
	}
}

// todo
void CPlayerPed::ProcessVehicleHorn()
{

}

void CPlayerPed::SetAttachedObject(int index, NEW_ATTACHED_OBJECT* pNewAttachedObject)
{
	if (m_ped && !IsEntityPlaceable(m_ped)) {
		if (m_ped->m_pRwObject) {
			if (index >= 0 && index < 10) {
				int iBoneID = pNewAttachedObject->iBoneID;
				if (iBoneID > 0 && iBoneID <= 18) {
					if (GetObjectSlotState(index)) {
						RemoveAttachedObject(index);
					}

					memcpy(&m_attachedObjects[index].info, pNewAttachedObject, sizeof(NEW_ATTACHED_OBJECT));

					RwMatrix matPlayer = m_ped->GetMatrix().ToRwMatrix();
					auto pNewObject = new CObject(pNewAttachedObject->iModel, {matPlayer.pos.x, matPlayer.pos.y, matPlayer.pos.z}, pNewAttachedObject->vecRot, 200.0f, true);
					if (!pNewObject || !pNewObject->m_entity) {
						delete pNewObject;
						memset(&m_attachedObjects[index].info, 0, sizeof(NEW_ATTACHED_OBJECT));
						return;
					}

					m_attachedObjects[index].object = pNewObject;
					m_attachedObjects[index].slotUsed = true;

					if (pNewAttachedObject->dwMaterialColor1) {
						pNewObject->SetMaterial(-1, 0, nullptr, nullptr, pNewAttachedObject->dwMaterialColor1);
					}
					if (pNewAttachedObject->dwMaterialColor2) {
						pNewObject->SetMaterial(-1, 1, nullptr, nullptr, pNewAttachedObject->dwMaterialColor2);
					}

					pNewObject->SetCollisionChecking(false);
				}
			}
		}
	}
}

void CPlayerPed::RemoveAttachedObject(int index)
{
	if (GetObjectSlotState(index)) {
		if (m_bytePlayerNumber == 0 && EditObject::m_visible && EditObject::m_type == EditObject::TYPE_ATTACHED_OBJECT && EditObject::m_id == index) {
			EditObject::cancelEdit();
		}
		if (m_attachedObjects[index].object) {
			delete m_attachedObjects[index].object;
			m_attachedObjects[index].object = nullptr;
		}

		memset(&m_attachedObjects[index].info, 0, sizeof(NEW_ATTACHED_OBJECT));
		m_attachedObjects[index].slotUsed = false;
	}
}

bool CPlayerPed::GetObjectSlotState(int index)
{
	if (index < 0 || index >= 10) {
		return false;
	}

	return m_attachedObjects[index].slotUsed;
}

bool CPlayerPed::HasAttachedObject()
{
	for (auto& m_attachedObject : m_attachedObjects) {
		if (m_attachedObject.slotUsed) {
			return true;
		}
	}

	return false;
}

void CPlayerPed::RemoveAllAttachedObjects()
{
	if (m_bytePlayerNumber == 0 && EditObject::m_visible && EditObject::m_type == EditObject::TYPE_ATTACHED_OBJECT) {
		EditObject::cancelEdit();
	}
	for (auto& m_attachedObject : m_attachedObjects) {
		if (m_attachedObject.slotUsed) {
			if (m_attachedObject.object) {
				delete m_attachedObject.object;
				m_attachedObject.object = nullptr;
			}

			memset(&m_attachedObject.info, 0, sizeof(NEW_ATTACHED_OBJECT));
			m_attachedObject.slotUsed = false;
		}
	}
}

sa::CAnimBlendClumpData** GetClumpData(RpClump* clump)
{
	RwInt32 ClumpOffset = *eng::as<RwInt32*>(eng::Sym::ClumpOffset);
	return RWPLUGINOFFSET(sa::CAnimBlendClumpData*, clump, ClumpOffset);
}

void UpdateElementRpHAnim(sa::CEntity* ent)
{
	if (ent) {
		// CEntity::UpdateRpHAnim
		eng::fn::CEntity_UpdateRpHAnim( ent);

		if (ent->m_nModelIndex == 0) { // CJ skin
			RpClump* clump = ent->m_pRpClump;
			if (clump) {
				// CPed::ShoulderBoneRotation
				eng::fn::CPed_ShoulderBoneRotation( clump);
			}
			else {
				spdlog::error("UpdateElementRpHAnim: clump is null");
			}
		}
	}
	else {
		spdlog::error("UpdateElementRpHAnim: ent is null");
	}
}

void CPlayerPed::ProcessCuff()
{
	if (!m_ped || !m_ped->m_pRpClump || !IsCuffed()) {
		return;
	}

	sa::CAnimBlendClumpData** ppClumpData = GetClumpData(m_ped->m_pRpClump);
	if (!ppClumpData || !*ppClumpData) {
		return;
	}
	sa::CAnimBlendClumpData* clumpData = *ppClumpData;

	int boneIds[] = {BONE_L_UPPER_ARM, BONE_L_FORE_ARM, BONE_L_HAND, BONE_R_UPPER_ARM, BONE_R_FORE_ARM, BONE_R_HAND};

	// yaw pitch roll
	float rotations[][3] = {
			{26.57374382019, 61.337575733622, 59.206573486328},
			{27.843754291534, 15.3639249801636, 46.40625},
			{-81.018516340527, 342.87482380867, 326.11833715439},
			{338.839179039, 53.49357098341, 298.45233917236},
			{307.68748283386, 22.110015869141, 313.59375},
			{96.047592163086, 357.88313293457, 56.739406585693}

	};

	for (int i = 0; i < 6; i++) {
		// updating the bone frame orientation will also update its children
		// This rotation is only applied when UpdateElementRpHAnim is called
		sa::AnimBlendFrameData* frameData = clumpData->GetFrameDataByNodeId(boneIds[i]);
		if (frameData) {
			RtQuat* boneOrientation = &frameData->GetFrameOrientation();
			RwV3d angles = {rotations[i][0], rotations[i][2], rotations[i][1]};
			// BoneNode_c::EulerToQuat(RwV3d_0 *euler, RtQuat_0 *quat)
			eng::fn::BoneNode_c_EulerToQuat( &angles, boneOrientation);
			m_ped->m_bDontUpdateHierarchy = false;
		}
	}

	UpdateElementRpHAnim(m_ped);
}

void CPlayerPed::ProcessAttachedObjects()
{
	bool bAnimUpdated = false;
	RwMatrix boneMatrix{};
	sa::CVector vecOut;

	for (auto& m_attachedObject : m_attachedObjects) {
		if (m_attachedObject.slotUsed) {
			CObject* pAttached = m_attachedObject.object;
			// re-verify the entity still owns its pool slot — the engine
			// recycles object slots, and Remove()/Add() on a recycled entity
			// would splice somebody else's collision records
			bool bEntityAlive = pAttached && pAttached->m_entity
					&& pAttached->m_gtaId
					&& GamePool_Object_GetAt(pAttached->m_gtaId) == pAttached->m_entity;
			if (bEntityAlive) {
				if (IsAdded()) {
					if (!bAnimUpdated) {
						if (m_ped) {
							UpdateElementRpHAnim(m_ped);
							bAnimUpdated = true;
						}
					}

					int iBoneID = m_attachedObject.info.iBoneID;
					int iBoneIndex = 0;

					// skip just this slot — returning here used to freeze
					// every attached object after the broken one
					if (!m_ped || iBoneID < 0 || iBoneID >= 19 || m_ped->m_apBones[iBoneID] == nullptr) {
						continue;
					}

					iBoneIndex = m_ped->m_apBones[iBoneID]->m_nNodeId;

					if (!GetBoneMatrix(&boneMatrix, iBoneIndex)) {
						continue;
					}

					// CPhysical::Remove
					m_attachedObject.object->m_entity->Remove();

					ProjectMatrix(&vecOut, reinterpret_cast<sa::CMatrix*>(&boneMatrix), &m_attachedObject.info.vecOffset);
					boneMatrix.pos = vecOut;

					if (m_attachedObject.info.vecRot.x != 0.0f) {
						RwMatrixRotate(&boneMatrix, 0, m_attachedObject.info.vecRot.x);
					}
					if (m_attachedObject.info.vecRot.y != 0.0f) {
						RwMatrixRotate(&boneMatrix, 1, m_attachedObject.info.vecRot.y);
					}
					if (m_attachedObject.info.vecRot.z != 0.0f) {
						RwMatrixRotate(&boneMatrix, 2, m_attachedObject.info.vecRot.z);
					}

					RwMatrixScale(&boneMatrix, &m_attachedObject.info.vecScale);

					m_attachedObject.object->m_entity->SetMatrix(boneMatrix);
					m_attachedObject.object->UpdateRwMatrixAndFrame();

					// Ped-attached objects are not in CObjectPool, so drive UVAnim here
					pAttached->UpdateUVAnim();

					// CPhysical::add
					m_attachedObject.object->m_entity->Add();
				}
				else {
					m_attachedObject.object->TeleportTo(0.0f, 0.0f, 0.0f);
				}
			}
		}
	}
}

void CPlayerPed::PutDirectlyInVehicle(uint32_t dwVehicleGTAId, uint8_t byteSeatID)
{
	spdlog::debug("PutDirectlyInVehicle: {} {}", dwVehicleGTAId, byteSeatID);

	if (!m_ped) {
		return;
	}

	if (!GamePool_Vehicle_GetAt(dwVehicleGTAId)) {
		return;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	if (GetCurrentWeapon() == WEAPON_PARACHUTE) {
		SetArmedWeapon(0);
	}

	sa::CVehicle* pGtaVehicle = GamePool_Vehicle_GetAt(dwVehicleGTAId);

	if (pGtaVehicle->fHealth != 0.0f && !IsEntityPlaceable(pGtaVehicle)) {
		uint8_t vehSubtype = 0;
		if (pNetGame && pNetGame->GetVehiclePool()) {
			VEHICLEID vid = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(pGtaVehicle);
			CVehicle* pVeh = (vid != INVALID_VEHICLE_ID) ? pNetGame->GetVehiclePool()->GetAt(vid) : nullptr;
			if (pVeh) vehSubtype = pVeh->GetVehicleSubtype();
		}
		if ((vehSubtype == VEHICLE_SUBTYPE_CAR || vehSubtype == VEHICLE_SUBTYPE_BIKE)
				&& byteSeatID > pGtaVehicle->m_nMaxPassengers) {
			return;
		}

		// Cleanly detach from any previous vehicle/seat first so pDriver / m_apPassengers[]
		// never retains a stale CPed* across direct seat or vehicle switches.
		if (m_ped->IsInVehicle()) {
			const bool bSameVehAndSeat = (m_ped->pVehicle == pGtaVehicle)
				&& (GetVehicleSeatID() == static_cast<int>(byteSeatID));
			if (bSameVehAndSeat) {
				return;
			}
			const RwMatrix curMat = pGtaVehicle->GetMatrix().ToRwMatrix();
			RemoveFromVehicleAndPutAt(curMat.pos.x, curMat.pos.y, curMat.pos.z + 0.5f);
		}
		else if (IsEnteringVehicle() != 0) {
			ClearPrimaryTask();
		}

		if (byteSeatID) {
			const int psgrSlot = static_cast<int>(byteSeatID) - 1;
			sa::CPed* pLocalGtaPed = GamePool_FindPlayerPed();
			// Never allow a remote PutDirectlyInVehicle (0x0430) to run CTaskSimpleCarSetPedOut
			// on the local player if both clients transiently target the same passenger slot.
			if (m_ped != pLocalGtaPed && psgrSlot >= 0 && psgrSlot < 8
					&& pLocalGtaPed && pGtaVehicle->m_apPassengers[psgrSlot] == pLocalGtaPed
					&& pLocalGtaPed->IsInVehicle()) {
				int fallbackSlot = -1;
				const int maxSlots = std::min<int>(static_cast<int>(pGtaVehicle->m_nMaxPassengers), 8);
				for (int s = 0; s < maxSlots; ++s) {
					if (pGtaVehicle->m_apPassengers[s] == nullptr) {
						fallbackSlot = s;
						break;
					}
				}
				if (fallbackSlot < 0) {
					return;
				}
				ScriptCommand(&put_actor_in_car2, m_gtaId, dwVehicleGTAId, fallbackSlot);
			}
			else {
				ScriptCommand(&put_actor_in_car2, m_gtaId, dwVehicleGTAId, psgrSlot);
			}
		}
		else {
			if (pGtaVehicle->pDriver && pGtaVehicle->pDriver != m_ped && pGtaVehicle->pDriver->IsInVehicle()) {
				sa::CPed* pLocalGtaPed = GamePool_FindPlayerPed();
				if (pGtaVehicle->pDriver == pLocalGtaPed && m_ped != pLocalGtaPed) {
					return;
				}
				// Evict stale remote driver (e.g. rapid seat swap before old driver's OnFootSync arrives)
				if (pNetGame && pNetGame->GetPlayerPool()) {
					PLAYERID oldDriverId = pNetGame->GetPlayerPool()->FindRemotePlayerIDFromGtaPtr(pGtaVehicle->pDriver);
					if (oldDriverId != INVALID_PLAYER_ID) {
						if (CRemotePlayer* pOldDriver = pNetGame->GetPlayerPool()->GetAt(oldDriverId)) {
							if (CPlayerPed* pOldPed = pOldDriver->GetPlayerPed()) {
								const RwMatrix curMat = pGtaVehicle->GetMatrix().ToRwMatrix();
								pOldPed->RemoveFromVehicleAndPutAt(curMat.pos.x, curMat.pos.y, curMat.pos.z + 1.0f);
							}
						}
					}
				}
				if (pGtaVehicle->pDriver && pGtaVehicle->pDriver != m_ped && pGtaVehicle->pDriver->IsInVehicle()) {
					return;
				}
			}

			ScriptCommand(&put_actor_in_car, m_gtaId, dwVehicleGTAId);
		}

		if (m_ped == GamePool_FindPlayerPed()) {
			if (m_ped->IsInVehicle()) {
				pGame->GetCamera()->SetBehindPlayer();
			}
		}

		if (pNetGame) {
			CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
			if (!pVehiclePool) {
				return;
			}
			VEHICLEID VehicleID = pVehiclePool->FindIDFromGtaPtr(pGtaVehicle);
			if (VehicleID != INVALID_VEHICLE_ID && VehicleID < MAX_VEHICLES) {
				CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
				if (pVehicle) {
					if (pVehicle->IsATrainPart()) {
						if (m_ped == GamePool_FindPlayerPed()) {
							ScriptCommand(&camera_on_vehicle, pVehicle->m_gtaId, 3, 2);
						}
					}
				}
			}
		}
	}
}

bool CPlayerPed::GetBoneMatrix(RwMatrix* matOut, int iBoneID)
{
	if (!matOut || !m_ped || IsEntityPlaceable(m_ped) || !m_ped->m_pRpClump) {
		return false;
	}
	auto pAnimHierarchy = eng::fn::GetAnimHierarchyFromSkinClump<RpHAnimHierarchy*>( m_ped->m_pRpClump); // GetAnimHierarchyFromSkinClump
	if (!pAnimHierarchy || !pAnimHierarchy->pMatrixArray) {
		return false;
	}
	int iAnimIndex = eng::fn::RpHAnimIDGetIndex( pAnimHierarchy, iBoneID); // RpHAnimIDGetIndex
	if (iAnimIndex < 0 || iAnimIndex >= pAnimHierarchy->numNodes) {
		return false;
	}
	memcpy(matOut, &pAnimHierarchy->pMatrixArray[iAnimIndex], sizeof(RwMatrix));
	return true;
}

void CPlayerPed::EnterVehicle(uint32_t dwVehicleGTAId, bool bPassenger)
{
	if (!m_ped) {
		return;
	}

	sa::CVehicle* pGtaVehicle = GamePool_Vehicle_GetAt(dwVehicleGTAId);
	if (!pGtaVehicle) {
		return;
	}

	if (!GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	// hide parachute
	if (GetCurrentWeapon() == WEAPON_PARACHUTE) {
		SetArmedWeapon(0);
	}

	// Pass time = -1 so CTaskComplexEnterCarAsPassengerTimed / DriverTimed uses GTA's
	// built-in untimed path (tbnz w8, #31) instead of warping into the seat at 3000ms
	// and skipping CarOpenDoorFromOutside / CarGetIn when walking >3s to reach the door.
	if (bPassenger) {
		if (pGtaVehicle->m_nModelIndex != TRAIN_PASSENGER || m_ped != GamePool_FindPlayerPed()) {
			ScriptCommand(&send_actor_to_car_passenger, m_gtaId, dwVehicleGTAId, -1, -1);
		}
		else {
			ScriptCommand(&put_actor_in_car2, m_gtaId, dwVehicleGTAId, -1);
		}
	}
	else {
		ScriptCommand(&send_actor_to_car_driverseat, m_gtaId, dwVehicleGTAId, -1);
	}
}

void CPlayerPed::SetCameraMode(uint8_t byteCameraMode)
{
	GameSetPlayerCameraMode(byteCameraMode, m_bytePlayerNumber);
}

void CPlayerPed::SetCurrentAim(CAMERA_AIM* pAim)
{
	GameStoreRemotePlayerAim(m_bytePlayerNumber, pAim);
}

void CPlayerPed::SetAimZ(float fAimZ)
{
	if (!isnan(fAimZ) && fAimZ <= 100.0f && fAimZ >= -100.0f) {
		if (m_ped && m_ped->m_pPlayerData) {
			m_ped->m_pPlayerData->m_fLookPitch = fAimZ;
		}
	}
}

uint8_t CPlayerPed::FindDeathReasonAndResponsiblePlayer(unsigned short* pPlayerID)
{
	uint8_t byteDeathReason;
	PLAYERID PlayerIDWhoKilled;

	CVehiclePool* pVehiclePool;
	CPlayerPool* pPlayerPool;

	if (pNetGame) {
		pVehiclePool = pNetGame->GetVehiclePool();
		pPlayerPool = pNetGame->GetPlayerPool();
	}
	else {
		*pPlayerID = INVALID_PLAYER_ID;
		return 0xFF;
	}

	if (m_ped) {
		byteDeathReason = m_ped->m_nLastDamagedWeaponType;
		if (byteDeathReason < WEAPON_CAMERA || byteDeathReason == WEAPON_HELIBLADES || byteDeathReason == WEAPON_EXPLOSION) {
			if (m_ped->m_pLastEntityDamage) {
				PlayerIDWhoKilled = pPlayerPool->FindRemotePlayerIDFromGtaPtr(static_cast<sa::CPed*>(m_ped->m_pLastEntityDamage));

				if (PlayerIDWhoKilled != INVALID_PLAYER_ID) {
					*pPlayerID = PlayerIDWhoKilled;
					return byteDeathReason;
				}
				else {
					if (pVehiclePool->FindIDFromGtaPtr(static_cast<sa::CVehicle*>(m_ped->m_pLastEntityDamage)) != INVALID_VEHICLE_ID) {
						sa::CVehicle* pGtaVehicle = static_cast<sa::CVehicle*>(m_ped->m_pLastEntityDamage);
						PlayerIDWhoKilled = pPlayerPool->FindRemotePlayerIDFromGtaPtr(pGtaVehicle->pDriver);

						if (PlayerIDWhoKilled != INVALID_PLAYER_ID) {
							*pPlayerID = PlayerIDWhoKilled;
							return byteDeathReason;
						}
					}
				}
			}
			*pPlayerID = INVALID_PLAYER_ID;
			return 0xFF;
		}
		else if (byteDeathReason == WEAPON_DROWN) {
			*pPlayerID = INVALID_PLAYER_ID;
			return WEAPON_DROWN;
		}
		else if (byteDeathReason == WEAPON_VEHICLE) {
			if (m_ped->m_pLastEntityDamage) {
				if (pVehiclePool->FindIDFromGtaPtr(static_cast<sa::CVehicle*>(m_ped->m_pLastEntityDamage)) != INVALID_VEHICLE_ID) {
					sa::CVehicle* pGtaVehicle = static_cast<sa::CVehicle*>(m_ped->m_pLastEntityDamage);

					PlayerIDWhoKilled = pPlayerPool->FindRemotePlayerIDFromGtaPtr(pGtaVehicle->pDriver);

					if (PlayerIDWhoKilled != INVALID_PLAYER_ID) {
						*pPlayerID = PlayerIDWhoKilled;
						return WEAPON_VEHICLE;
					}
				}
			}
			*pPlayerID = INVALID_PLAYER_ID;
			return 0xFF;
		}
		else if (byteDeathReason == WEAPON_COLLISION) {
			if (m_ped->m_pLastEntityDamage) {
				if (pVehiclePool->FindIDFromGtaPtr(static_cast<sa::CVehicle*>(m_ped->m_pLastEntityDamage)) != INVALID_VEHICLE_ID) {
					sa::CVehicle* pGtaVehicle = static_cast<sa::CVehicle*>(m_ped->m_pLastEntityDamage);

					PlayerIDWhoKilled = pPlayerPool->FindRemotePlayerIDFromGtaPtr(pGtaVehicle->pDriver);

					if (PlayerIDWhoKilled != INVALID_PLAYER_ID) {
						*pPlayerID = PlayerIDWhoKilled;
						return WEAPON_COLLISION;
					}
				}
			}
			*pPlayerID = INVALID_PLAYER_ID;
			return WEAPON_COLLISION;
		}
	}

	*pPlayerID = INVALID_PLAYER_ID;
	return 0xFF;
}

void CPlayerPed::ProcessBulletData(BULLET_DATA* btData)
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	BULLET_SYNC_DATA bulletSyncData;

	if (btData) {
		m_bHaveBulletData = true;
		m_bulletData.pEntity = btData->pEntity;
		m_bulletData.vecOrigin.x = btData->vecOrigin.x;
		m_bulletData.vecOrigin.y = btData->vecOrigin.y;
		m_bulletData.vecOrigin.z = btData->vecOrigin.z;

		m_bulletData.vecPos.x = btData->vecPos.x;
		m_bulletData.vecPos.y = btData->vecPos.y;
		m_bulletData.vecPos.z = btData->vecPos.z;

		m_bulletData.vecOffset.x = btData->vecOffset.x;
		m_bulletData.vecOffset.y = btData->vecOffset.y;
		m_bulletData.vecOffset.z = btData->vecOffset.z;

		uint8_t byteHitType = 0;
		unsigned short InstanceID = 0xFFFF;

		if (m_bytePlayerNumber == 0) {
			if (pNetGame) {
				CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
				if (pPlayerPool) {
					CPlayerPed* pPlayerPed = pPlayerPool->GetLocalPlayer()->GetPlayerPed();
					if (pPlayerPed) {
						memset(&bulletSyncData, 0, sizeof(BULLET_SYNC_DATA));
						if (pPlayerPed->GetCurrentWeapon() != WEAPON_SNIPER || btData->pEntity) {
							if (btData->pEntity) {
								CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
								CObjectPool* pObjectPool = pNetGame->GetObjectPool();

								PLAYERID PlayerID;
								VEHICLEID VehicleID;
								uint16_t ObjectID;

								if (pVehiclePool && pObjectPool) {
									PlayerID = pPlayerPool->FindRemotePlayerIDFromGtaPtr((sa::CPed*) btData->pEntity);
									if (PlayerID == INVALID_PLAYER_ID) {
										VehicleID = pVehiclePool->FindIDFromGtaPtr((sa::CVehicle*) btData->pEntity);
										if (VehicleID == INVALID_VEHICLE_ID) {
											ObjectID = pObjectPool->FindIDFromGtaPtr(btData->pEntity);
											if (ObjectID == INVALID_OBJECT_ID) {
												sa::CVector vecOut;
												vecOut.x = 0.0f;
												vecOut.y = 0.0f;
												vecOut.z = 0.0f;

												if (btData->pEntity->m_matrix) {
													ProjectMatrix(&vecOut, btData->pEntity->m_matrix, &btData->vecOffset);
													btData->vecOffset.x = vecOut.x;
													btData->vecOffset.y = vecOut.y;
													btData->vecOffset.z = vecOut.z;
												}
												else {
													btData->vecOffset.x = btData->pEntity->GetPosition().x + btData->vecOffset.x;
													btData->vecOffset.y = btData->pEntity->GetPosition().y + btData->vecOffset.y;
													btData->vecOffset.z = btData->pEntity->GetPosition().z + btData->vecOffset.z;
												}
											}
											else {
												// object
												byteHitType = 3;
												InstanceID = ObjectID;
											}
										}
										else {
											// vehicle
											byteHitType = 2;
											InstanceID = VehicleID;
										}
									}
									else {
										// player
										byteHitType = 1;
										InstanceID = PlayerID;
									}
								}
							}

							bulletSyncData.vecOrigin.x = btData->vecOrigin.x;
							bulletSyncData.vecOrigin.y = btData->vecOrigin.y;
							bulletSyncData.vecOrigin.z = btData->vecOrigin.z;

							bulletSyncData.vecPos.x = btData->vecPos.x;
							bulletSyncData.vecPos.y = btData->vecPos.y;
							bulletSyncData.vecPos.z = btData->vecPos.z;

							bulletSyncData.vecOffset.x = btData->vecOffset.x;
							bulletSyncData.vecOffset.y = btData->vecOffset.y;
							bulletSyncData.vecOffset.z = btData->vecOffset.z;

							bulletSyncData.targetType = byteHitType;
							bulletSyncData.targetId = InstanceID;
							bulletSyncData.weaponId = pPlayerPed->GetCurrentWeapon();

							RakNet::BitStream bsBullet;
							proto::Writer writer(bsBullet);
							proto::OutboundBulletSync pkt{};
							pkt.targetType = bulletSyncData.targetType;
							pkt.targetId = bulletSyncData.targetId;
							pkt.origin = {bulletSyncData.vecOrigin.x, bulletSyncData.vecOrigin.y, bulletSyncData.vecOrigin.z};
							pkt.pos = {bulletSyncData.vecPos.x, bulletSyncData.vecPos.y, bulletSyncData.vecPos.z};
							pkt.offset = {bulletSyncData.vecOffset.x, bulletSyncData.vecOffset.y, bulletSyncData.vecOffset.z};
							pkt.weaponId = bulletSyncData.weaponId;
							pkt.serialize(writer, proto::ProtoVersion::V037);
							pNetGame->GetRakClient()->Send(&bsBullet, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0);
						}
					}
				}
			}
		}
	}
	else {
		m_bHaveBulletData = false;
		memset(&m_bulletData, 0, sizeof(BULLET_DATA));
	}
}

extern uint32_t (* CWeapon_FireInstantHit)(sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CVector* vecOrigin, sa::CVector* muzzlePosn, sa::CEntity* targetEntity, sa::CVector* target, sa::CVector* originForDriveBy, bool arg6, bool muzzle);
extern uint32_t (* CWeapon_FireSniper)(sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CEntity* victim, sa::CVector* target);

CPlayerPed* g_pCurrentFiredPed = nullptr;
BULLET_DATA* g_pCurrentBulletData = nullptr;

void CPlayerPed::FireInstant()
{
	spdlog::debug("CPlayerPed::FireInstant");

	ScopedPlayerContext ctx(m_bytePlayerNumber, ScopedPlayerContext::kFireInstant, m_bytePlayerNumber != 0);

	g_pCurrentFiredPed = this;

	if (m_bHaveBulletData) {
		g_pCurrentBulletData = &m_bulletData;
	}
	else {
		g_pCurrentBulletData = nullptr;
	}

	sa::CVector vecBonePos{0.0f, 0.0f, 0.0f}, vecOut{0.0f, 0.0f, 0.0f};

	if (m_ped) {
		if (GetCurrentWeapon() == WEAPON_SNIPER) {
			if (sa::CWeapon* pSlot = GetCurrentWeaponSlot()) {
				CWeapon_FireSniper(pSlot, m_ped, nullptr, nullptr);
			}
		}
		else {
			GetWeaponInfoForFire(false, &vecBonePos, &vecOut);
			if (sa::CWeapon* pSlot = GetCurrentWeaponSlot()) {
				CWeapon_FireInstantHit(pSlot, m_ped, &vecBonePos, &vecOut, nullptr, nullptr, nullptr, false, true);
			}
		}
	}

	g_pCurrentFiredPed = nullptr;
	g_pCurrentBulletData = nullptr;
}

void CPlayerPed::GetWeaponInfoForFire(bool bLeftWrist, sa::CVector* vecBonePos, sa::CVector* vecOut)
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}

	sa::CVector* pFireOffset = GetCurrentWeaponFireOffset();
	if (pFireOffset && vecBonePos && vecOut) {
		vecOut->x = pFireOffset->x;
		vecOut->y = pFireOffset->y;
		vecOut->z = pFireOffset->z;

		int bone_id = 24;
		if (bLeftWrist) {
			bone_id = 34;
		}

		m_ped->GetBonePosition(vecBonePos, bone_id, false);

		vecBonePos->z += pFireOffset->z + 0.15f;
		GetTransformedBonePosition(bone_id, vecOut);
	}
}

uintptr_t GetWeaponInfo(int iWeapon, int iSkill)
{
	// CWeaponInfo::GetWeaponInfo
	return eng::fn::CWeaponInfo_GetWeaponInfo( iWeapon, iSkill);
}

sa::CVector* CPlayerPed::GetCurrentWeaponFireOffset()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return nullptr;
	}

	sa::CWeapon* pSlot = GetCurrentWeaponSlot();
	if (pSlot) {
		uintptr_t weaponInfo = GetWeaponInfo(pSlot->dwType, 1);
		if (weaponInfo) {
			return reinterpret_cast<sa::CVector*>(weaponInfo + 0x24);
		}
	}
	return nullptr;
}

void CPlayerPed::GetTransformedBonePosition(int iBoneID, sa::CVector* vecOut)
{
	if (!m_ped) {
		return;
	}

	// CPed::GetTransformedBonePosition
	eng::fn::CPed_GetTransformedBonePosition( m_ped, vecOut, iBoneID, false);
}

void CPlayerPed::SetCameraZoomAndAspect(float fExtZoom, float fAspectRatio)
{
	GameSetPlayerCameraExtZoomAndAspect(m_bytePlayerNumber, fExtZoom, fAspectRatio);
}

sa::CEntity* CPlayerPed::GetEntityUnderPlayer()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return nullptr;
	}

	sa::CEntity* entity = nullptr;
	sa::CVector vecStart;
	sa::CVector vecEnd;
	sa::CColPoint colPoint{};

	if (IsInVehicle()) {
		return nullptr;
	}

	const sa::CVector pos = m_ped->GetPosition();
	vecStart.x = pos.x;
	vecStart.y = pos.y;
	vecStart.z = pos.z - 0.25f;

	vecEnd.x = pos.x;
	vecEnd.y = pos.y;
	vecEnd.z = vecStart.z - 1.75f;

	ProcessLineOfSight(&vecStart, &vecEnd, &colPoint, &entity, false, true, false, true, false, false, false, false);
	return entity;
}

void CPlayerPed::ResetDamageEntity()
{
	if (!m_ped || !GamePool_Ped_GetAt(m_gtaId)) {
		return;
	}
	m_ped->m_pLastEntityDamage = nullptr;
	m_ped->m_nLastDamagedWeaponType = 0xFF;
	m_ped->m_pDamageEntity = nullptr;
}

int CPlayerPed::GetPedStat()
{
	if (!m_ped) {
		return -1;
	}

	auto* pModelInfo = reinterpret_cast<sa::CPedModelInfo*>(GetModelInfoByID(m_ped->m_nModelIndex));
	if (!pModelInfo) {
		return -1;
	}
	return (int) pModelInfo->m_defaultPedStats;
}
