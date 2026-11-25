#include "../Client.h"
#include "../Game/Game.h"
#include "NetGame.h"
#include "NetCap.h"
#include "RpcTable.h"
#include "Proto/Gpci.h"
#include "Proto/RpcOutbound.h"
#include "Proto/Sync.h"
#include <RakNet/SAMP/NetCapHook.h>
#include "../UI/UI.h"
#include "../Java/Java.h"
#include "../Java/EditObject.h"
#include "../Game/Animation.h"
#include "RakNet/SAMP/samp_auth.h"
#include "../Game/AudioStream.h"
#include "../Voice/SpeakerList.h"
#include "../Voice/Network.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>

namespace {

std::string loadOrCreateDeviceGpci()
{
	static std::string cachedGpci;
	if (!cachedGpci.empty()) {
		return cachedGpci;
	}

	std::string seed;
	const std::string dir = Client::gameDir();
	const std::string seedPath = dir.empty() ? std::string() : (dir + "/gpci_seed.dat");

	if (!seedPath.empty()) {
		std::ifstream in(seedPath, std::ios::binary);
		if (in.is_open()) {
			std::getline(in, seed);
			while (!seed.empty() && (seed.back() == '\r' || seed.back() == '\n' || seed.back() == ' ')) {
				seed.pop_back();
			}
		}
	}

	if (seed.size() < 16) {
		uint8_t rnd[32]{};
		bool gotRandom = false;
		if (FILE* f = std::fopen("/dev/urandom", "rb")) {
			gotRandom = (std::fread(rnd, 1, sizeof(rnd), f) == sizeof(rnd));
			std::fclose(f);
		}
		if (!gotRandom) {
			std::random_device rd;
			const uint64_t t = static_cast<uint64_t>(
				std::chrono::high_resolution_clock::now().time_since_epoch().count());
			for (size_t i = 0; i < sizeof(rnd); i += 4) {
				const uint32_t v = rd() ^ static_cast<uint32_t>(t >> ((i % 8) * 4));
				rnd[i + 0] = static_cast<uint8_t>(v & 0xFFu);
				rnd[i + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
				rnd[i + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
				rnd[i + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
			}
		}

		static const char kHex[] = "0123456789ABCDEF";
		seed.clear();
		seed.reserve(sizeof(rnd) * 2);
		for (uint8_t b : rnd) {
			seed.push_back(kHex[b >> 4]);
			seed.push_back(kHex[b & 0x0Fu]);
		}

		if (!seedPath.empty()) {
			const std::string tmpPath = seedPath + ".tmp";
			std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
			if (out.is_open()) {
				out.write(seed.data(), static_cast<std::streamsize>(seed.size()));
				out.flush();
				const bool ok = out.good();
				out.close();
				if (ok) {
					std::rename(tmpPath.c_str(), seedPath.c_str());
				}
				else {
					std::remove(tmpPath.c_str());
				}
			}
		}
	}

	cachedGpci = proto::generateGpci(seed);
	spdlog::info("NetGame: device GPCI initialized (len={})", cachedGpci.size());
	return cachedGpci;
}

} // namespace

extern UI* pUI;
extern Game* pGame;
extern AudioStream* pAudioStream;
extern NetGame* pNetGame;

using MonetNetGameCallback = void (*)(void* rakClient, const char* host, int port);
static std::mutex g_monetNetGameMutex;
static MonetNetGameCallback g_monetNetGameCb = nullptr;
static void* g_monetLastAttachedRakClient = nullptr;

static void notifyMonetNetGameLocked(void* rakClient, const char* host, int port)
{
	if (!g_monetNetGameCb || !rakClient || g_monetLastAttachedRakClient == rakClient) {
		return;
	}
	g_monetLastAttachedRakClient = rakClient;
	g_monetNetGameCb(rakClient, host ? host : "", port);
}

extern "C" __attribute__((visibility("default"))) void SAMPMOBILE_RegisterNetGameCallback(MonetNetGameCallback cb)
{
	std::lock_guard<std::mutex> lock(g_monetNetGameMutex);
	g_monetNetGameCb = cb;
	if (!cb) {
		g_monetLastAttachedRakClient = nullptr;
		return;
	}
	if (pNetGame && pNetGame->GetRakClient()) {
		notifyMonetNetGameLocked(pNetGame->GetRakClient(), pNetGame->m_host, pNetGame->m_port);
	}
}

void InstallVehicleEngineLightPatches();

unsigned char GetPacketID(Packet* p)
{
	if (p == 0 || p->data == 0 || p->length == 0) return 255;

	if ((unsigned char) p->data[0] == ID_TIMESTAMP) {
		if (p->length <= sizeof(unsigned char) + sizeof(RakNetTime)) return 255;
		return (unsigned char) p->data[sizeof(unsigned char) + sizeof(RakNetTime)];
	}
	else {
		return (unsigned char) p->data[0];
	}
}

NetGame::NetGame(const char* host, int port, const char* playerName, const char* password)
{
	spdlog::info("NetGame initializing..");

	m_netSet = &m_session.settings;
	m_session.protoVersion = proto::versionFromNetgame(static_cast<uint32_t>(Client::netgameVersion()));
	m_session.reset(SessionState::ResetReason::Connect);

	memset(m_host, 0, sizeof(m_host));
	if (host) {
		strncpy(m_host, host, sizeof(m_host) - 1);
		m_host[sizeof(m_host) - 1] = '\0';
	}
	m_port = port;

	NetCap::startIfEnabled(Client::gameDir(), (uint32_t) Client::netgameVersion());

	m_rakClient = RakNetworkFactory::GetRakClientInterface();
	{
		std::lock_guard<std::mutex> lock(g_monetNetGameMutex);
		notifyMonetNetGameLocked(m_rakClient, m_host, m_port);
	}
	InitializePools();

	GetPlayerPool()->SetLocalPlayerName(Encoding::utf2cp(playerName).c_str());

	RpcTable::registerAll(m_rakClient, m_session.protoVersion);
	m_rakClient->SetPassword(password);

	if (Settings::voice()) {
		spdlog::info("NetGame::NetGame: voice connect {}:{}", host, port);
		Network::OnRaknetConnect(host, port);
	}

	memset(m_mapIcon, 0, sizeof(m_mapIcon));

	pGame->EnableClock(false);
	pGame->EnableZoneNames(false);

	m_lastConnectAttempt = GetTickCount();
	SetGameState(GAMESTATE_WAIT_CONNECT);

	if (pUI) {
		pUI->chat()->addDebugMessage("{FFFFFF}SA-MP {B9C9BF}%s {FFFFFF}Started", Client::sampVersion().c_str());
		pUI->chat()->addInfoMessage("-> Use /clientcmds for client commands");
	}
}

NetGame::~NetGame()
{
	{
		std::lock_guard<std::mutex> lock(g_monetNetGameMutex);
		if (g_monetLastAttachedRakClient == m_rakClient) {
			g_monetLastAttachedRakClient = nullptr;
		}
	}
	m_rakClient->Disconnect(0);
	RpcTable::unregisterAll(m_rakClient, m_session.protoVersion);
	RakNetworkFactory::DestroyRakClientInterface(m_rakClient);
	m_rakClient = nullptr;

	UninitializePools();

	if (Settings::voice()) {
		Network::OnRaknetDisconnect();
	}

	m_netSet = nullptr;
}

void NetGame::InitializePools()
{
	m_pools = new NET_POOLS;
	m_pools->playerPool = new CPlayerPool();
	m_pools->vehiclePool = new CVehiclePool();
	m_pools->objectPool = new CObjectPool();
	m_pools->actorPool = new CActorPool();
	m_pools->pickupPool = new CPickupPool();
	m_pools->textLabelPool = new CTextLabelPool();
	m_pools->textDrawPool = new CTextDrawPool();
	m_pools->playerBubblePool = new CPlayerBubblePool();
	m_pools->gangZonePool = new CGangZonePool();
	m_pools->menuPool = new CMenuPool();
}

void NetGame::UninitializePools()
{
	if (m_pools->playerPool) {
		delete m_pools->playerPool;
		m_pools->playerPool = nullptr;
	}

	if (m_pools->vehiclePool) {
		delete m_pools->vehiclePool;
		m_pools->vehiclePool = nullptr;
	}

	if (m_pools->objectPool) {
		delete m_pools->objectPool;
		m_pools->objectPool = nullptr;
	}

	if (m_pools->actorPool) {
		delete m_pools->actorPool;
		m_pools->actorPool = nullptr;
	}

	if (m_pools->pickupPool) {
		delete m_pools->pickupPool;
		m_pools->pickupPool = nullptr;
	}

	if (m_pools->textLabelPool) {
		delete m_pools->textLabelPool;
		m_pools->textLabelPool = nullptr;
	}

	if (m_pools->textDrawPool) {
		delete m_pools->textDrawPool;
		m_pools->textDrawPool = nullptr;
	}

	if (m_pools->playerBubblePool) {
		delete m_pools->playerBubblePool;
		m_pools->playerBubblePool = nullptr;
	}

	if (m_pools->gangZonePool) {
		delete m_pools->gangZonePool;
		m_pools->gangZonePool = nullptr;
	}

	if (m_pools->menuPool) {
		delete m_pools->menuPool;
		m_pools->menuPool = nullptr;
	}

	if (m_pools) {
		delete m_pools;
		m_pools = nullptr;
	}
}

void NetGame::Process()
{
	UpdateNetwork();
	ProcessDlArtworkHandshake();
	if (m_netSet->holdTime) {
		pGame->SetWorldTime(m_netSet->worldTime_Hour, m_netSet->worldTime_Minute);
	}

	pGame->PreloadObjectsAnims();

	if (GetGameState() == GAMESTATE_CONNECTED) {
		ProcessPools();
		if (pGame) {
			pGame->ProcessAudio();
		}
	}
	else {
		ProcessLoadingScreen();
	}

	if (GetGameState() == GAMESTATE_WAIT_CONNECT) {
		ProcessConnecting();
	}
}

void NetGame::UpdateNetwork()
{
	Packet* pkt = nullptr;
	unsigned char packetIdentifier;
	while ((pkt = m_rakClient->Receive())) {
		if (pkt->data && pkt->length > 0) {
			const unsigned int bits = (pkt->bitSize > 0 && pkt->bitSize <= pkt->length * 8)
				? pkt->bitSize
				: pkt->length * 8;
			invokeRakNetCapHook(0, 1, pkt->data[0], pkt->data, bits);
		}
		packetIdentifier = GetPacketID(pkt);

		switch (packetIdentifier) {
			case ID_AUTH_KEY: Packet_AuthKey(pkt);
				break;

			case ID_CONNECTION_ATTEMPT_FAILED: Packet_ConnectAttemptFailed(pkt);
				break;

			case ID_NO_FREE_INCOMING_CONNECTIONS: Packet_NoFreeIncomingConnections(pkt);
				break;

			case ID_DISCONNECTION_NOTIFICATION: Packet_DisconnectionNotification(pkt);
				break;

			case ID_CONNECTION_LOST: Packet_ConnectionLost(pkt);
				break;

			case ID_CONNECTION_REQUEST_ACCEPTED: Packet_ConnectionSucceeded(pkt);
				break;

			case ID_FAILED_INITIALIZE_ENCRIPTION: Packet_FailedInitializeEncription(pkt);
				break;

			case ID_CONNECTION_BANNED: Packet_ConnectionBanned(pkt);
				break;

			case ID_INVALID_PASSWORD: Packet_InvalidPassword(pkt);
				break;

			case ID_VEHICLE_SYNC: Packet_VehicleSync(pkt);
				break;

			case ID_AIM_SYNC: Packet_AimSync(pkt);
				break;

			case ID_BULLET_SYNC: Packet_BulletSync(pkt);
				break;

			case ID_PLAYER_SYNC: Packet_PlayerSync(pkt);
				break;

			case ID_MARKERS_SYNC: Packet_MarkerSync(pkt);
				break;

			case ID_UNOCCUPIED_SYNC: Packet_UnoccupiedSync(pkt);
				break;

			case ID_TRAILER_SYNC: Packet_TrailerSync(pkt);
				break;

			case ID_PASSENGER_SYNC: Packet_PassengerSync(pkt);
				break;

			case ID_VOICE_SYNC:
				if (Settings::voice()) {
					Network::OnRaknetReceive(pkt);
				}
				break;
		}

		m_rakClient->DeallocatePacket(pkt);
	}
}

void NetGame::ShutdownForGameModeRestart(SessionState::ResetReason reason)
{
	for (PLAYERID playerId = 0; playerId < MAX_PLAYERS; playerId++) {
		CRemotePlayer* pRemotePlayer = GetPlayerPool()->GetAt(playerId);
		if (pRemotePlayer) {
			pRemotePlayer->SetTeam(NO_TEAM);
			pRemotePlayer->ResetAllSyncAttributes();
		}
	}

	GetPlayerPool()->GetLocalPlayer()->ResetAllSyncAttributes();
	GetPlayerPool()->GetLocalPlayer()->ToggleSpectating(false);
	GameResetStats();

	if (pAudioStream) {
		pAudioStream->Stop(true);
	}

	SetGameState(GAMESTATE_RESTARTING);
	GetPlayerPool()->DeactivateAll();
	GetPlayerPool()->Process();
	ResetVehiclePool();
	ResetActorPool();
	ResetTextDrawPool();
	ResetPlayerBubblePool();
	ResetGangZonePool();
	ResetTextLabelPool();
	ResetMapIcons();
	ResetPickupPool();
	ResetObjectPool();
	ResetMenuPool();
	ResetDlArtworkState();
	if (pUI && pUI->netstats()) {
		pUI->netstats()->resetServerStats();
	}

	m_session.reset(reason);

	if (EditObject::m_visible) {
		EditObject::cancelEdit();
	}
	if (g_java) {
		g_java->hideDialog();
	}
	if (pUI && pUI->spawn()) {
		pUI->spawn()->setVisible(false);
	}
	ResetLocalPedQueuedAnim();

	if (pGame) {
		pGame->ClearImGuiGameText();
		pGame->ClearAllHudHideReasons();
		pGame->EnableCheckpoints(false);
		pGame->EnableRaceCheckpoints(false);
		pGame->UpdateCheckpoints();

		if (CPlayerPed* pLocalPed = pGame->FindPlayerPed()) {
			if (pLocalPed->IsDancing()) pLocalPed->StopDancing();
			if (pLocalPed->IsCellphoneEnabled()) pLocalPed->ToggleCellphone(false);
			if (pLocalPed->IsPissing()) pLocalPed->TogglePissing(false);
			if (pLocalPed->IsCuffed()) pLocalPed->ToggleCuffed(false);
			if (pLocalPed->IsCarry()) pLocalPed->ToggleCarry(false);
			if (pLocalPed->IsInJetpack()) pLocalPed->StopJetpack();
			if (pLocalPed->GetStuff() != STUFF_TYPE_NONE) pLocalPed->DropStuff();
			if (pLocalPed->HasAttachedObject()) pLocalPed->RemoveAllAttachedObjects();
			pLocalPed->SetDrunkLevel(0);
			pLocalPed->DrunkProcess();
			pLocalPed->SetFightingStyle(4);
			pLocalPed->SetInterior(0, true);
			pLocalPed->SetDead();
			pLocalPed->SetArmour(0.0f);
		}
		pGame->ResetLocalMoney();
		pGame->EnableZoneNames(false);
		GameResetRadarColors();
		pGame->SetGravity(m_netSet->gravity);
		pGame->SetWantedLevel(0);
		pGame->EnableClock(false);
		ScriptCommand(&toggle_widescreen, 0);
	}

	Network::OnRaknetDisconnect();
	SpeakerList::Hide();

	if (pUI) {
		pUI->chat()->addInfoMessage("The server is restarting..");
	}
}

void NetGame::ProcessPools()
{
	if (GetPlayerPool()) {
		GetPlayerPool()->Process();
	}

	static int iVehiclePoolProcessFlag = 0;
	if (GetVehiclePool() && iVehiclePoolProcessFlag > 2) {
		GetVehiclePool()->Process();
		iVehiclePoolProcessFlag = 0;
	}
	else {
		iVehiclePoolProcessFlag++;
	}

	static int iPickupPoolProcessFlag = 0;
	if (GetPickupPool() && iPickupPoolProcessFlag > 5) {
		GetPickupPool()->Process();
		iPickupPoolProcessFlag = 0;
	}
	else {
		iPickupPoolProcessFlag++;
	}
}

void NetGame::ProcessLoadingScreen()
{
	CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
	if (!pPlayerPed || !pPlayerPed->m_ped) {
		return;
	}
	if (pPlayerPed->IsInVehicle()) {
		pPlayerPed->RemoveFromVehicleAndPutAt(1093.4, -2036.5, 82.710602);
	}
	else {
		pPlayerPed->TeleportTo(1133.0504, -2038.4034, 69.099998);
	}

	pGame->GetCamera()->SetPosition(1093.0, -2036.0, 90.0, 0.0, 0.0, 0.0);
	pGame->GetCamera()->LookAtPoint(384.0, -1557.0, 20.0, 2);
	pGame->SetWorldWeather(1);
	pGame->DisplayHUD(false);
}

void NetGame::ProcessConnecting()
{
	if ((GetTickCount() - m_lastConnectAttempt) > 3000) {
		if (pUI) {
			pUI->chat()->addDebugMessage("Connecting to %s:%d...", m_host, m_port);
		}

		m_rakClient->Connect(m_host, m_port, 0, 0, 2);

		if (Settings::voice()) {
			spdlog::info("NetGame::ProcessConnecting: voice connect {}:{}", m_host, m_port);
			Network::OnRaknetConnect(m_host, m_port);
		}

		m_lastConnectAttempt = GetTickCount();
		SetGameState(GAMESTATE_CONNECTING);
	}
}

void NetGame::Packet_AuthKey(Packet* pkt)
{
	if (!pkt || !pkt->data || pkt->length < 1) return;
	if (GetGameState() < GAMESTATE_WAIT_CONNECT || GetGameState() > GAMESTATE_AWAIT_JOIN) {
		return;
	}

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::AuthKeyPacket req{};
	const auto ver = proto::versionFromNetgame(static_cast<uint32_t>(Client::netgameVersion()));
	if (!req.serialize(reader, ver)) {
		return;
	}

	const char* auth_key = nullptr;

	for (auto& x : AuthKeyTable) {
		if (!strcmp(req.key.c_str(), x[0])) {
			auth_key = x[1];
			break;
		}
	}

	char szGenKey[260];
	if (!auth_key) {
		// table only covers 0.3.7 challenge names; 0.3.DL (and newer 0.3.7)
		// servers may send anything - compute the real algorithm
		gen_auth_key(szGenKey, req.key.c_str());
		auth_key = szGenKey;
		spdlog::info("[AUTH] challenge not in table, computed: {}", req.key);
	}

	RakNet::BitStream bsKey;
	proto::Writer writer(bsKey);
	proto::AuthKeyPacket resp{};
	resp.key = auth_key;
	resp.serialize(writer, ver);

	m_rakClient->Send(&bsKey, SYSTEM_PRIORITY, RELIABLE, NULL);

	spdlog::info("[AUTH] {} -> {}", req.key, auth_key);
}

void NetGame::Packet_ConnectAttemptFailed(Packet* pkt)
{
	if (pUI) {
		pUI->chat()->addDebugMessage("The server didn't respond. Retrying..");
	}

	if (pAudioStream) {
		pAudioStream->Stop(true);
	}

	if (Settings::voice()) {
		SpeakerList::Hide();
	}

	SetGameState(GAMESTATE_WAIT_CONNECT);
}

void NetGame::Packet_NoFreeIncomingConnections(Packet* pkt)
{
	if (pUI) {
		pUI->chat()->addDebugMessage("The server is full. Retrying..");
	}

	if (Settings::voice()) {
		SpeakerList::Hide();
	}

	SetGameState(GAMESTATE_WAIT_CONNECT);
}

void NetGame::Packet_DisconnectionNotification(Packet* pkt)
{
	if (pUI) {
		pUI->chat()->addDebugMessage("Server closed the connection.");
	}

	if (pAudioStream) {
		pAudioStream->Stop(true);
	}

	Network::OnRaknetDisconnect();
	SpeakerList::Hide();

	m_rakClient->Disconnect(100);
}

void NetGame::Packet_ConnectionSucceeded(Packet* pkt)
{
	if (!pkt || !pkt->data || pkt->length < 1) return;

	const int iVersion = Client::netgameVersion();
	const auto ver = proto::versionFromNetgame(static_cast<uint32_t>(iVersion));
	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::ConnectionSucceededPacket succ{};
	if (!succ.serialize(reader, ver)) {
		return;
	}

	uint8_t byteMod = 1;
	unsigned int uiChallenge = succ.challenge ^ static_cast<unsigned int>(iVersion);

	if (pUI) {
		pUI->chat()->addDebugMessage("Connected. Joining the game...");
	}

	SetGameState(GAMESTATE_AWAIT_JOIN);

	RakNet::BitStream bsSend;
	proto::Writer writer(bsSend);
	proto::ClientJoin join{};
	join.version = iVersion;
	join.mod = byteMod;
	join.nickname = GetPlayerPool()->GetLocalPlayerName();
	join.challengeResponse = uiChallenge;
	join.authKey = loadOrCreateDeviceGpci();
	join.clientVersion = Client::sampVersion();
	join.serialize(writer, ver);

	if (Settings::voice()) {
		Network::OnRaknetRpc(RPC_ClientJoin, bsSend);
		SpeakerList::Hide();
	}

	m_rakClient->RPC(&RPC_ClientJoin, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void NetGame::Packet_FailedInitializeEncription(Packet* pkt)
{
	if (pUI) {
		pUI->chat()->addDebugMessage("Failed to initialize encryption.");
	}
}

void NetGame::Packet_ConnectionBanned(Packet* pkt)
{
	if (pUI) {
		pUI->chat()->addDebugMessage("You are banned from this server.");
	}
	if (pAudioStream) {
		pAudioStream->Stop(true);
	}
	Network::OnRaknetDisconnect();
	SpeakerList::Hide();
	m_rakClient->Disconnect(0);
}

void NetGame::Packet_InvalidPassword(Packet* pkt)
{
	if (pUI) {
		pUI->chat()->addDebugMessage("Wrong server password.");
	}
	if (pAudioStream) {
		pAudioStream->Stop(true);
	}
	Network::OnRaknetDisconnect();
	SpeakerList::Hide();
	m_rakClient->Disconnect(0);
}

void NetGame::Packet_ConnectionLost(Packet* pkt)
{
	if (m_rakClient) {
		m_rakClient->Disconnect(0);
	}

	if (pUI) {
		pUI->chat()->addDebugMessage("Lost connection to the server. Reconnecting..");
	}

	ShutdownForGameModeRestart(SessionState::ResetReason::Disconnect);

	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (pPlayerPool) {
		for (PLAYERID playerId = 0; playerId < MAX_PLAYERS; playerId++) {
			if (pPlayerPool->GetSlotState(playerId)) {
				pPlayerPool->Delete(playerId, 0);
			}
		}
	}

	if (pAudioStream) {
		pAudioStream->Stop(true);
	}

	SetGameState(GAMESTATE_WAIT_CONNECT);
}

namespace {
inline bool isFiniteVec3(float x, float y, float z) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
inline bool isFiniteQuat(float x, float y, float z, float w) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
}
}

void NetGame::Packet_PlayerSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED) return;
	if (!pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundPlayerSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;
	if (!isFiniteVec3(p.position.x, p.position.y, p.position.z)
			|| !isFiniteQuat(p.quat.x, p.quat.y, p.quat.z, p.quat.w)
			|| !isFiniteVec3(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z)) {
		return;
	}

	ONFOOT_SYNC_DATA ofSync{};
	ofSync.lrAnalog = p.hasLR ? p.lrAnalog : 0;
	ofSync.udAnalog = p.hasUD ? p.udAnalog : 0;
	ofSync.wKeys = p.keys;
	ofSync.vecPos = sa::CVector(p.position.x, p.position.y, p.position.z);
	ofSync.quat.Set(p.quat.x, p.quat.y, p.quat.z, p.quat.w);
	ofSync.byteHealth = p.health;
	ofSync.byteArmour = p.armour;
	ofSync.byteCurrentWeapon = p.currentWeapon & 0x3F;
	ofSync.byteSpecialAction = p.specialAction;
	ofSync.vecMoveSpeed = sa::CVector(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z);
	ofSync.wSurfInfo = p.hasSurfing ? p.surfInfo : INVALID_VEHICLE_ID;
	ofSync.vecSurfOffsets = p.hasSurfing ? sa::CVector(p.surfOffsets.x, p.surfOffsets.y, p.surfOffsets.z) : sa::CVector{};
	ofSync.dwAnimation = p.hasAnim ? p.animation : 0x80000000u;

	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (!pPlayerPool) return;
	CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
	if (pRemotePlayer) {
		pRemotePlayer->StoreOnFootFullSyncData(&ofSync, p.timestamp);
	}
}
// vehicle sync
void NetGame::Packet_VehicleSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED) return;
	if (!pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundVehicleSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;
	if (!isFiniteVec3(p.position.x, p.position.y, p.position.z)
			|| !isFiniteQuat(p.quat.x, p.quat.y, p.quat.z, p.quat.w)
			|| !isFiniteVec3(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z)) {
		return;
	}

	INCAR_SYNC_DATA icSync{};
	icSync.VehicleID = p.vehicleId;
	icSync.lrAnalog = p.lrAnalog;
	icSync.udAnalog = p.udAnalog;
	icSync.wKeys = p.keys;
	icSync.quat.Set(p.quat.x, p.quat.y, p.quat.z, p.quat.w);
	icSync.vecPos = sa::CVector(p.position.x, p.position.y, p.position.z);
	icSync.vecMoveSpeed = sa::CVector(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z);
	icSync.fCarHealth = static_cast<float>(p.carHealth);
	icSync.bytePlayerHealth = p.playerHealth;
	icSync.bytePlayerArmour = p.playerArmour;
	icSync.byteCurrentWeapon = p.currentWeapon & 0x3F;
	icSync.byteSirenOn = p.sirenOn ? 1 : 0;
	icSync.byteLandingGearState = p.landingGearState ? 1 : 0;
	if (p.hasTrainOrHydra) {
		memcpy(&icSync.fTrainSpeed, &p.trainOrHydra, sizeof(icSync.fTrainSpeed));
		if (!std::isfinite(icSync.fTrainSpeed)) {
			icSync.fTrainSpeed = 0.0f;
		}
	}
	if (p.hasTrailer) {
		icSync.TrailerID = p.trailerId;
	}

	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (!pPlayerPool) return;
	CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
	if (pRemotePlayer) {
		pRemotePlayer->StoreInCarFullSyncData(&icSync, p.timestamp);
	}
}

void NetGame::Packet_AimSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundAimSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;
	if (!isFiniteVec3(p.aimF1.x, p.aimF1.y, p.aimF1.z)
			|| !isFiniteVec3(p.aimPos.x, p.aimPos.y, p.aimPos.z)
			|| !std::isfinite(p.aimZ)) {
		return;
	}

	AIM_SYNC_DATA aimSync{};
	aimSync.byteCamMode = p.camMode;
	aimSync.vecAimf1 = sa::CVector(p.aimF1.x, p.aimF1.y, p.aimF1.z);
	aimSync.vecAimPos = sa::CVector(p.aimPos.x, p.aimPos.y, p.aimPos.z);
	aimSync.fAimZ = p.aimZ;
	aimSync.byteCamExtZoom = p.zoomWeaponState & 0x3F;
	aimSync.byteWeaponState = (p.zoomWeaponState >> 6) & 0x03;
	aimSync.byteAspectRatio = p.aspectRatio;

	CPlayerPool* pPlayerPool = GetPlayerPool();
	CRemotePlayer* pPlayer = pPlayerPool ? pPlayerPool->GetAt(p.playerId) : nullptr;
	if (pPlayer) {
		pPlayer->StoreAimFullSyncData(&aimSync);
	}
}

void NetGame::Packet_BulletSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundBulletSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;

	BULLET_SYNC_DATA btSync{};
	btSync.targetType = p.targetType;
	btSync.targetId = p.targetId;
	btSync.vecOrigin = sa::CVector(p.origin.x, p.origin.y, p.origin.z);
	btSync.vecPos = sa::CVector(p.pos.x, p.pos.y, p.pos.z);
	btSync.vecOffset = sa::CVector(p.offset.x, p.offset.y, p.offset.z);
	btSync.weaponId = p.weaponId;

	CPlayerPool* pPlayerPool = GetPlayerPool();
	CRemotePlayer* pRemotePlayer = pPlayerPool ? pPlayerPool->GetAt(p.playerId) : nullptr;
	if (pRemotePlayer) {
		pRemotePlayer->StoreBulletFullSyncData(&btSync);
	}
}

void NetGame::Packet_PassengerSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundPassengerSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;
	if (!isFiniteVec3(p.position.x, p.position.y, p.position.z)) return;

	PASSENGER_SYNC_DATA psSync{};
	psSync.VehicleID = p.vehicleId;
	psSync.byteSeatFlags = p.seatFlags;
	psSync.byteCurrentWeapon = p.currentWeapon;
	psSync.bytePlayerHealth = p.playerHealth;
	psSync.bytePlayerArmour = p.playerArmour;
	psSync.lrAnalog = p.lrAnalog;
	psSync.udAnalog = p.udAnalog;
	psSync.wKeys = p.keys;
	psSync.vecPos = sa::CVector(p.position.x, p.position.y, p.position.z);

	CPlayerPool* pPlayerPool = GetPlayerPool();
	CRemotePlayer* pRemotePlayer = pPlayerPool ? pPlayerPool->GetAt(p.playerId) : nullptr;
	if (pRemotePlayer) {
		pRemotePlayer->StorePassengerFullSyncData(&psSync);
	}
}

void NetGame::Packet_MarkerSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !pkt || !pkt->data || pkt->length < 1) return;

	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (!pPlayerPool) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundMarkersSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;

	for (const auto& entry : p.entries) {
		if (entry.playerId < MAX_PLAYERS && pPlayerPool->GetSlotState(entry.playerId)) {
			CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(entry.playerId);
			if (pRemotePlayer) {
				if (entry.active) {
					pRemotePlayer->ShowGlobalMarker(entry.x, entry.y, entry.z);
				}
				else {
					pRemotePlayer->HideGlobalMarker();
				}
			}
		}
	}
}

void NetGame::Packet_TrailerSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundTrailerSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;
	if (!isFiniteVec3(p.position.x, p.position.y, p.position.z)
			|| !isFiniteQuat(p.quat.x, p.quat.y, p.quat.z, p.quat.w)
			|| !isFiniteVec3(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z)
			|| !isFiniteVec3(p.turnSpeed.x, p.turnSpeed.y, p.turnSpeed.z)) {
		return;
	}

	TRAILER_SYNC_DATA trSync{};
	trSync.trailerId = p.trailerId;
	trSync.vecPos = sa::CVector(p.position.x, p.position.y, p.position.z);
	trSync.quat.Set(p.quat.x, p.quat.y, p.quat.z, p.quat.w);
	trSync.vecMoveSpeed = sa::CVector(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z);
	trSync.vecTurnSpeed = sa::CVector(p.turnSpeed.x, p.turnSpeed.y, p.turnSpeed.z);

	if (GetPlayerPool()) {
		CRemotePlayer* pPlayer = GetPlayerPool()->GetAt(p.playerId);
		if (pPlayer) {
			pPlayer->StoreTrailerFullSyncData(&trSync);
		}
	}
}

void NetGame::Packet_UnoccupiedSync(Packet* pkt)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !pkt || !pkt->data || pkt->length < 1) return;

	proto::Reader reader(pkt->data, pkt->length * 8);
	proto::InboundUnoccupiedSync p{};
	if (!p.serialize(reader, proto::ProtoVersion::V037)) return;

	UNOCCUPIED_SYNC_DATA unocSync{};
	unocSync.vehicleId = p.vehicleId;
	unocSync.byteSeatId = p.seatId;
	unocSync.vecRoll = sa::CVector(p.roll.x, p.roll.y, p.roll.z);
	unocSync.vecDirection = sa::CVector(p.direction.x, p.direction.y, p.direction.z);
	unocSync.vecPos = sa::CVector(p.position.x, p.position.y, p.position.z);
	unocSync.vecMoveSpeed = sa::CVector(p.moveSpeed.x, p.moveSpeed.y, p.moveSpeed.z);
	unocSync.vecTurnSpeed = sa::CVector(p.turnSpeed.x, p.turnSpeed.y, p.turnSpeed.z);
	unocSync.fCarHealth = p.carHealth;

	if (GetPlayerPool()) {
		CRemotePlayer* pPlayer = GetPlayerPool()->GetAt(p.playerId);
		if (pPlayer) {
			pPlayer->StoreUnoccupiedFullSyncData(&unocSync);
		}
	}
}

void NetGame::UpdatePlayerScoresAndPings()
{
	static uint32_t dwLastUpdateTick = 0;

	if (GetTickCount() - dwLastUpdateTick > 3000) {
		dwLastUpdateTick = GetTickCount();

		RakNet::BitStream bsSend;
		proto::Writer writer(bsSend);
		proto::UpdateScoresPingsReq req{};
		req.serialize(writer, proto::ProtoVersion::V037);
		m_rakClient->RPC(&RPC_UpdateScoresPingsIPs, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false,
				UNASSIGNED_NETWORK_ID, nullptr);
	}
}

void NetGame::SendDialogResponse(uint16_t wDialogID, uint8_t byteButtonID, uint16_t wListBoxItem, const char* szInput)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !m_rakClient) return;
	if (!szInput) szInput = "";

	size_t rawLen = strlen(szInput);
	if (rawLen > 128) rawLen = 128;

	RakNet::BitStream bsSend;
	proto::Writer writer(bsSend);
	proto::DialogResponse resp{};
	resp.id = static_cast<int16_t>(wDialogID);
	resp.response = byteButtonID;
	resp.listItem = static_cast<int16_t>(wListBoxItem);
	resp.input.assign(szInput, rawLen);
	resp.serialize(writer, proto::ProtoVersion::V037);
	m_rakClient->RPC(&RPC_DialogResponse, &bsSend, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

void NetGame::SendChatMessage(const char* szMsg)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !m_rakClient || !szMsg) return;

	size_t rawLen = strlen(szMsg);
	if (rawLen > 144) rawLen = 144;

	RakNet::BitStream bsSend;
	proto::Writer writer(bsSend);
	proto::ChatReq req{};
	req.message.assign(szMsg, rawLen);
	req.serialize(writer, proto::ProtoVersion::V037);

	m_rakClient->RPC(&RPC_Chat, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID,
			NULL);
}

void NetGame::SendChatCommand(const char* szCommand)
{
	if (GetGameState() != GAMESTATE_CONNECTED || !m_rakClient || !szCommand) return;

	size_t rawLen = strlen(szCommand);
	if (rawLen > 255) rawLen = 255;

	RakNet::BitStream bsParams;
	proto::Writer writer(bsParams);
	proto::ServerCommandReq req{};
	req.command.assign(szCommand, rawLen);
	req.serialize(writer, proto::ProtoVersion::V037);
	m_rakClient->RPC(&RPC_ServerCommand, &bsParams, HIGH_PRIORITY, RELIABLE, 0, false,
			UNASSIGNED_NETWORK_ID, NULL);
}

void NetGame::SetMapIcon(uint8_t byteIconID, float fPosX, float fPosY, float fPosZ, uint8_t byteType, uint32_t dwColor, uint8_t byteStyle)
{
	if (byteIconID < MAX_MAP_ICONS) {
		if (m_mapIcon[byteIconID] != 0) {
			DisableMapIcon(byteIconID);
		}

		m_mapIcon[byteIconID] = pGame->CreateRadarMarkerIcon(byteType, fPosX, fPosY, fPosZ, dwColor, byteStyle);
	}
}

void NetGame::DisableMapIcon(uint8_t byteIconID)
{
	if (byteIconID < MAX_MAP_ICONS) {
		if (m_mapIcon[byteIconID] != 0) {
			if (pGame) {
				pGame->DisableMarker(m_mapIcon[byteIconID]);
			}
			m_mapIcon[byteIconID] = 0;
		}
	}
}

void NetGame::ResetVehiclePool()
{
	if (m_pools->vehiclePool) {
		delete m_pools->vehiclePool;
	}

	m_pools->vehiclePool = new CVehiclePool();
}

void NetGame::ResetActorPool()
{
	if (m_pools->actorPool) {
		delete m_pools->actorPool;
	}

	m_pools->actorPool = new CActorPool();
}

void NetGame::ResetTextDrawPool()
{
	// Reset in-place (never delete/reallocate while NVEvent/Java threads may access it)
	if (m_pools->textDrawPool) {
		m_pools->textDrawPool->Reset();
	}
	else {
		m_pools->textDrawPool = new CTextDrawPool();
	}
}

void NetGame::ResetPlayerBubblePool()
{
	if (m_pools->playerBubblePool) {
		delete m_pools->playerBubblePool;
	}

	m_pools->playerBubblePool = new CPlayerBubblePool();
}

void NetGame::ResetGangZonePool()
{
	if (m_pools->gangZonePool) {
		delete m_pools->gangZonePool;
	}

	m_pools->gangZonePool = new CGangZonePool();
}

void NetGame::ResetTextLabelPool()
{
	if (m_pools->textLabelPool) {
		delete m_pools->textLabelPool;
	}

	m_pools->textLabelPool = new CTextLabelPool();
}

void NetGame::ResetMapIcons()
{
	for (unsigned int& i : m_mapIcon) {
		if (i) {
			ScriptCommand(&disable_marker, i);
			i = 0;
		}
	}
}

void NetGame::ResetPickupPool()
{
	if (m_pools->pickupPool) {
		delete m_pools->pickupPool;
	}

	m_pools->pickupPool = new CPickupPool();
}

void NetGame::ResetObjectPool()
{
	// Reset in-place (never delete/reallocate while NVEvent/Java threads may access it)
	if (m_pools->objectPool) {
		m_pools->objectPool->Reset();
	}
	else {
		m_pools->objectPool = new CObjectPool();
	}
}

void NetGame::ResetMenuPool()
{
	// Reset in-place (never delete/reallocate while NVEvent/Java threads may access it)
	if (m_pools->menuPool) {
		m_pools->menuPool->Reset();
	}
	else {
		m_pools->menuPool = new CMenuPool();
	}
}

void NetGame::InitGameLogic()
{
	if (m_netSet->manualVehicleEngineAndLight) {
		InstallVehicleEngineLightPatches();
	}

	m_netSet->worldBounds[0] = 20000.0f;
	m_netSet->worldBounds[1] = -20000.0f;
	m_netSet->worldBounds[2] = 20000.0f;
	m_netSet->worldBounds[3] = -20000.0f;
}

void NetGame::SendPrevClass()
{
	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (pPlayerPool) {
		pPlayerPool->GetLocalPlayer()->SendPrevClass();
	}
}

void NetGame::SendSpawn()
{
	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (pPlayerPool) {
		pPlayerPool->GetLocalPlayer()->SendSpawn();
	}
}

void NetGame::SendNextClass()
{
	CPlayerPool* pPlayerPool = GetPlayerPool();
	if (pPlayerPool) {
		pPlayerPool->GetLocalPlayer()->SendNextClass();
	}
}

void NetGame::NotifyDlVirtualWorld(int32_t virtualWorld)
{
	if (!m_dlVirtualWorldKnown || virtualWorld != m_dlVirtualWorld) {
		m_dlVirtualWorld = virtualWorld;
		m_dlVirtualWorldKnown = true;
		m_dlFinishDownloadPending = true;
	}
}

void NetGame::ResetDlArtworkState()
{
	m_dlVirtualWorld = 0;
	m_dlVirtualWorldKnown = false;
	m_dlFinishDownloadPending = false;
}

void NetGame::ProcessDlArtworkHandshake()
{
	if (!m_dlFinishDownloadPending) {
		return;
	}
	if (!m_rakClient || !m_rakClient->IsConnected() || m_session.protoVersion != proto::ProtoVersion::V03DL) {
		return;
	}
	m_dlFinishDownloadPending = false;

	RakNet::BitStream bsOut;
	proto::Writer writer(bsOut);
	proto::DlFinishDownloadReq req{};
	req.serialize(writer, proto::ProtoVersion::V03DL);
	m_rakClient->RPC(&RPC_DlFinishDownload, &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
	spdlog::debug("NetGame: sent 0.3.DL FinishDownload (RPC 184) for vw={}", m_dlVirtualWorld);
}