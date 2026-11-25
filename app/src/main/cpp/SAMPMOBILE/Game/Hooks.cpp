#include <atomic>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <zlib.h>
#include <thread>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>

#include "Game.h"
#include "Animation.h"
#include "../Net/Proto/RpcOutbound.h"
#include "../GameSA/include/Enums/WidgetIDs.h"
#include "Core/Rect.h"
#include <cmath>
#include "../UI/UI.h"
#include "../Net/NetGame.h"
#include "../Client.h"
#include "../Settings.h"
#include "ModLoader.h"
#include "CleoEngine.h"
#include "ThreadOptimizer.h"
#include "../Java/Java.h"
#include "Pickups.h"

extern UI *pUI;
extern Game *pGame;
extern NetGame *pNetGame;

static bool resolve_case_insensitive(const char* base, const char* relIn, char* relOut, size_t outSize)
{
	relOut[0] = 0;
	char cur[0x600];
	snprintf(cur, sizeof(cur), "%s", base);

	char tmp[0x1ff];
	snprintf(tmp, sizeof(tmp), "%s", relIn);
	char* save = nullptr;
	for (char* comp = strtok_r(tmp, "/\\", &save); comp; comp = strtok_r(nullptr, "/\\", &save))
	{
		DIR* d = opendir(cur);
		if (!d) return false;
		struct dirent* e;
		char found[0x1ff] = {0};
		while ((e = readdir(d)))
		{
			if (!strcasecmp(e->d_name, comp))
			{
				snprintf(found, sizeof(found), "%s", e->d_name);
				break;
			}
		}
		closedir(d);
		if (!found[0]) return false;
		size_t curLen = strlen(relOut);
		snprintf(relOut + curLen, outSize - curLen, "%s%s", curLen ? "/" : "", found);
		snprintf(cur, sizeof(cur), "%s/%s", base, relOut);
	}
	return relOut[0] != 0;
}

using MonetTouchFilterFn = bool (*)(int type, int num, int posX, int posY);
static std::atomic<MonetTouchFilterFn> g_monetTouchFilter{nullptr};

extern "C" __attribute__((visibility("default")))
void SAMPMOBILE_RegisterTouchFilter(MonetTouchFilterFn fn)
{
	g_monetTouchFilter.store(fn, std::memory_order_release);
}

DECL_HOOK(void, AND_TouchEvent, int type, int num, int posX, int posY)
{
	// Track which pointer slots (0..3) are currently pressed inside libGTASA.so.
	// Previously every intercepted touch called AND_TouchEvent(1, 0, 0, 0),
	// which left fingers 1..3 permanently stuck down whenever they hit an overlay
	// (buttonpanel / TextDraw / Monet / CLEO) or a modal dialog opened — and also
	// spuriously killed finger 0's joystick when finger 1 tapped an overlay.
	static bool s_gtaPointerDown[4] = {false, false, false, false};

	auto forwardToGta = [&](int t, int n, int x, int y) {
		if (n >= 0 && n < 4) {
			if (t == 2) s_gtaPointerDown[n] = true;
			else if (t == 1) s_gtaPointerDown[n] = false;
		}
		AND_TouchEvent(t, n, x, y);
	};

	auto releaseGtaPointer = [&](int n) {
		if (n >= 0 && n < 4 && s_gtaPointerDown[n]) {
			s_gtaPointerDown[n] = false;
			AND_TouchEvent(1, n, 0, 0);
		}
	};

	auto releaseAllGtaPointers = [&]() {
		for (int i = 0; i < 4; ++i) {
			if (s_gtaPointerDown[i]) {
				s_gtaPointerDown[i] = false;
				AND_TouchEvent(1, i, 0, 0);
			}
		}
	};

	if (Game::IsGamePaused())
	{
		forwardToGta(type, num, posX, posY);
		return;
	}

	const bool javaDialogOpen = Java::isJavaDialogVisible();
	const bool modalUiOpen = javaDialogOpen || (pUI != nullptr && pUI->isModalOpenSnapshot());
	if (javaDialogOpen)
	{
		if (pUI != nullptr) {
			pUI->enqueueTouchEvent(ImVec2(0.0f, 0.0f), TouchType::pop, true);
		}
		releaseAllGtaPointers();
		return;
	}

	if (!modalUiOpen)
	{
		ImVec2 disp = pUI ? pUI->displaySizeSnapshot() : ImVec2(1920.0f, 1080.0f);
		if (CleoEngine::handleTouch(type, num, posX, posY, disp.x, disp.y))
		{
			releaseGtaPointer(num);
			return;
		}
		if (auto touchFilter = g_monetTouchFilter.load(std::memory_order_acquire))
		{
			if (touchFilter(type, num, posX, posY))
			{
				releaseGtaPointer(num);
				return;
			}
		}
	}

	if (pUI != nullptr)
	{
		const bool tdSelecting = !pUI->isModalOpenSnapshot()
			&& pNetGame && pNetGame->GetTextDrawPool() && pNetGame->GetTextDrawPool()->GetState();
		const bool hitBtnPanel = pUI->isButtonPanelHitSnapshot(ImVec2(posX, posY));

		// Give selectable TextDraws priority over background ImGui widgets (chat/etc.)
		// while still letting the player tap ButtonPanel (e.g. ESC) to cancel selection.
		if (tdSelecting && !hitBtnPanel)
		{
			if (!pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY))
			{
				releaseGtaPointer(num);
				return;
			}
		}

		switch (type)
		{
		case 2: // push
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::push, false, num);
			break;

		case 3: // move
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::move, false, num);
			break;

		case 1: // pop
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::pop, false, num);
			break;
		}

		if (pUI->isModalOpenSnapshot())
		{
			releaseAllGtaPointers();
			return;
		}
		else
		{
			if (pNetGame && pNetGame->GetMenuPool())
			{
				if (!pNetGame->GetMenuPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}

			if (hitBtnPanel)
			{
				releaseGtaPointer(num);
				return;
			}

			if (pNetGame && pNetGame->GetTextDrawPool())
			{
				if (!pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}

			if (pNetGame && pNetGame->GetObjectPool())
			{
				if (!pNetGame->GetObjectPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}
		}
	}

	if (pGame && pGame->IsGameInputEnabled())
	{
		forwardToGta(type, num, posX, posY);
	}
	else
	{
		releaseAllGtaPointers();
	}
}

DECL_HOOK(void, DisplayScreen)
{
	// Loading screen is rendered on the Java layer; suppress the native GTA screen.
}

DECL_HOOK(int, OS_FileOpen, int a1, uintptr_t handle, char *name, int a2)
{
	char path[0x1ff] = {0};
	char modPath[0x1ff] = {0};
	const char *origName = name;
	bool isProtectedOnlineScript = false;

	if (name && !strncmp(name, "data\\script\\mainV1.scm", 22))
	{
		spdlog::info("Loading mainV1.scm..");
		snprintf(path, sizeof(path), "SAMP\\main.scm");
		name = path;
		isProtectedOnlineScript = !Client::offlineMode();
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\SCRIPT\\SCRIPTV1.IMG", 24))
	{
		spdlog::info("Loading scriptV1.img..");
		snprintf(path, sizeof(path), "SAMP\\script.img");
		name = path;
		isProtectedOnlineScript = !Client::offlineMode();
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\GTA.DAT", 12))
	{
		spdlog::info("Loading gta.dat..");
		snprintf(path, sizeof(path), "SAMP\\gta.dat");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\PEDS.IDE", 13))
	{
		spdlog::info("Loading peds.ide..");
		snprintf(path, sizeof(path), "SAMP\\peds.ide");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\TIMECYC.DAT", 16))
	{
		spdlog::info("Loading timecyc.dat..");
		snprintf(path, sizeof(path), "SAMP\\timecyc.dat");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "data\\paths\\tracks2.dat", 22))
	{
		spdlog::info("Loading tracks2.dat...");
		snprintf(path, sizeof(path), "SAMP\\tracks2.dat");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "data\\paths\\tracks4.dat", 22))
	{
		spdlog::info("Loading tracks4.dat...");
		snprintf(path, sizeof(path), "SAMP\\tracks4.dat");
		name = path;
		goto check_mod;
	}

check_mod:
	if (!isProtectedOnlineScript && ModLoader::isEnabled() && name)
	{
		// Check both the redirected name (e.g. SAMP\timecyc.dat) and original name (e.g. DATA\TIMECYC.DAT)
		if (ModLoader::resolve(name, modPath, sizeof(modPath)) ||
			(origName && origName != name && ModLoader::resolve(origName, modPath, sizeof(modPath))))
		{
			spdlog::info("ModLoader override: {} -> {}", origName ? origName : name, modPath);
			name = modPath;
		}
	}

	int result = OS_FileOpen(a1, handle, name, a2);
	if (result != 0 && name)
	{
		char relIn[0x1ff] = {0};
		snprintf(relIn, sizeof(relIn), "%s", name);
		size_t inLen = strlen(relIn);
		while (inLen > 0 && (relIn[inLen - 1] == ' ' || relIn[inLen - 1] == '\t'))
			relIn[--inLen] = 0;
		char fixed[0x1ff] = {0};
		if (resolve_case_insensitive(Client::gameDir(), relIn, fixed, sizeof(fixed)))
		{
			spdlog::info("OS_FileOpen retry (case-insensitive): {}", fixed);
			result = OS_FileOpen(a1, handle, fixed, a2);
		}
	}
	if (result != 0)
		spdlog::error("OS_FileOpen failed ({}): {}", result, name ? name : "<null>");
	return result;
}

DECL_HOOK(sa::CPedModelInfo *, CModelInfo_AddPedModel, int id)
{
	if (id < 0 || id >= 25000) return nullptr;
	auto model = new sa::CPedModelInfo{};
	model->vtable = eng::vmtSlot(eng::Sym::vtable_CPedModelInfo, 1); // CPedModelInfo::~CPedModelInfo

	typedef sa::CStore<sa::CPedModelInfo, 278> PedModelStore;
	auto ms_pedModelStore = g_saSym->Abs<PedModelStore *>(addr::ms_pedModelStore);
	ms_pedModelStore->AddItem();

	// CClumpModelInfo::Init
	model->Init();

	auto ms_modelInfoPtrs = g_saSym->Abs<sa::CBaseModelInfo **>(addr::CModelInfo_ms_modelInfoPtrs);
	ms_modelInfoPtrs[id] = model;
	return model;
}

DECL_HOOK(sa::CAtomicModelInfo *, CModelInfo_AddAtomicModel, int id)
{
	if (id < 0 || id >= 25000) return nullptr;
	auto model = new sa::CAtomicModelInfo{};
	model->vtable = eng::vmtSlot(eng::Sym::vtable_CAtomicModelInfo, 1); // CAtomicModelInfo::~CAtomicModelInfo

	typedef sa::CStore<sa::CAtomicModelInfo, 14000> AtomicModelStore;
	auto ms_atomicModelStore = g_saSym->Abs<AtomicModelStore *>(addr::ms_atomicModelStore);
	ms_atomicModelStore->AddItem();

	// CAtomicModelInfo::Init
	model->Init();

	// CModelInfo::ms_modelInfoPtrs
	auto ms_modelInfoPtrs = g_saSym->Abs<sa::CBaseModelInfo **>(addr::CModelInfo_ms_modelInfoPtrs);
	ms_modelInfoPtrs[id] = model;
	return model;
}

static const char *texdb_format_ext(sa::TextureDatabaseFormat fmt)
{
	switch (fmt)
	{
	case sa::DF_UNC: return "unc";
	case sa::DF_DXT: return "dxt";
	case sa::DF_360: return "360";
	case sa::DF_PS3: return "ps3";
	case sa::DF_PVR: return "pvr";
	case sa::DF_ETC: return "etc";
	default: return nullptr;
	}
}

static bool texdb_format_exists_in_modloader(const char *dbName, sa::TextureDatabaseFormat fmt)
{
	const char *ext = texdb_format_ext(fmt);
	if (!ext || !dbName || !*dbName || !ModLoader::isEnabled()) return false;
	char rel[0xff] = {0};
	char fixed[0x1ff] = {0};
	snprintf(rel, sizeof(rel), "texdb/%s/%s.%s.toc", dbName, dbName, ext);
	return ModLoader::resolve(rel, fixed, sizeof(fixed));
}

static bool texdb_format_exists_in_base(const char *dbName, sa::TextureDatabaseFormat fmt)
{
	const char *ext = texdb_format_ext(fmt);
	if (!ext || !dbName || !*dbName) return false;
	char rel[0xff] = {0};
	char fixed[0x1ff] = {0};
	snprintf(rel, sizeof(rel), "texdb/%s/%s.%s.toc", dbName, dbName, ext);
	return resolve_case_insensitive(Client::gameDir(), rel, fixed, sizeof(fixed));
}

DECL_HOOK(sa::TextureDatabaseRuntime *, TextureDatabaseRuntime_Load, const char *withName, bool fullyLoad, sa::TextureDatabaseFormat forcedFormat)
{
	sa::TextureDatabaseFormat selected = forcedFormat;
	if (withName && *withName)
	{
		static const sa::TextureDatabaseFormat kPriority[] = {
			sa::DF_ETC, sa::DF_PVR, sa::DF_DXT, sa::DF_UNC
		};

		// Prefer any format supplied by an active ModLoader pack (e.g. .unc/.dxt HUD packs)
		// before falling back to official base files/, so modded texdb packs are never shadowed.
		bool matchedMod = false;
		if (ModLoader::isEnabled())
		{
			if (selected != sa::DF_Default && texdb_format_exists_in_modloader(withName, selected))
			{
				matchedMod = true;
			}
			else
			{
				for (auto candidate : kPriority)
				{
					if (texdb_format_exists_in_modloader(withName, candidate))
					{
						selected = candidate;
						matchedMod = true;
						break;
					}
				}
			}
		}

		if (!matchedMod && (selected == sa::DF_Default || !texdb_format_exists_in_base(withName, selected)))
		{
			for (auto candidate : kPriority)
			{
				if (texdb_format_exists_in_base(withName, candidate))
				{
					selected = candidate;
					break;
				}
			}
		}
		spdlog::info("TextureDatabaseRuntime::Load({}, fully={}, req={}, sel={}, mod={})",
			withName, fullyLoad, (int)forcedFormat, (int)selected, matchedMod);
	}
	return TextureDatabaseRuntime_Load(withName, fullyLoad, selected);
}

static RwTexture *findTextureInDatabase(const char *dbName, const char *texture_name)
{
	auto database_handle = sa::TextureDatabaseRuntime::GetDatabase(dbName);
	if (!database_handle)
	{
		return nullptr;
	}

	auto registered = sa::TextureDatabaseRuntime::registered();
	bool alreadyRegistered = false;
	if (registered.dataPtr && registered.numEntries > 0)
	{
		for (int index = 0; index < registered.numEntries; ++index)
		{
			if (registered.dataPtr[index] == database_handle)
			{
				alreadyRegistered = true;
				break;
			}
		}
	}

	if (alreadyRegistered)
	{
		return nullptr;
	}

	sa::TextureDatabaseRuntime::Register(database_handle);
	RwTexture *texture = sa::TextureDatabaseRuntime::GetTexture(texture_name);
	sa::TextureDatabaseRuntime::Unregister(database_handle);
	return texture;
}

DECL_HOOK(RwTexture *, CTxdStore_TxdStoreFindCB, const char *texture_name)
{
	if (!texture_name || !*texture_name)
	{
		return nullptr;
	}

	if (ModLoader::isEnabled())
	{
		for (const auto &extraDb : ModLoader::extraTexDbs())
		{
			if (RwTexture *tex = findTextureInDatabase(extraDb.c_str(), texture_name))
			{
				return tex;
			}
		}
	}

	static const char *kBaseTexDbs[] = {"gta_int", "gta3", "samp", "menu", "mobile", "samp_extra", "cutscene"};
	for (const char *db : kBaseTexDbs)
	{
		if (RwTexture *tex = findTextureInDatabase(db, texture_name))
		{
			return tex;
		}
	}

	RwTexDictionary *current = eng::fn::RwTexDictionaryGetCurrent<RwTexDictionary *>();
	while (current)
	{
		RwTexture *texture = eng::fn::RwTexDictionaryFindNamedTexture<RwTexture *>( current, texture_name);
		if (texture)
		{
			return texture;
		}

		current = sa::CTxdStore::GetTxdParent(current);
	}

	return nullptr;
}

// Guard against AML plugins or game code calling TextureDatabaseRuntime::Register(nullptr)
// when an optional texdb fails to load; a null slot in registered[] crashes GetTexture() at +0x110.
DECL_HOOK(void, TextureDatabaseRuntime_Register, sa::TextureDatabaseRuntime *db)
{
	if (!db)
	{
		return;
	}
	TextureDatabaseRuntime_Register(db);
}

DECL_HOOK(int, CGame_InitialiseRenderWare)
{
	spdlog::info("Initializing samp texture database...");

	int result = CGame_InitialiseRenderWare();
	sa::TextureDatabaseRuntime::Load("samp", false, sa::DF_ETC);
	sa::TextureDatabaseRuntime::Load("samp_extra", false, sa::DF_ETC);
	if (ModLoader::isEnabled())
	{
		for (const auto &db : ModLoader::extraTexDbs())
		{
			spdlog::info("ModLoader: loading custom texture database '{}'", db);
			sa::TextureDatabaseRuntime::Load(db.c_str(), false, sa::DF_Default);
		}
	}
	Client::initializeUI();
	return result;
}

struct CStreamingFile
{
	unsigned char m_name[40];
	bool m_bRegister;
	uint8_t m_padding[3];
	int32_t m_lsn;
};
VALIDATE_SIZE(CStreamingFile, 0x30);

CStreamingFile *CStreaming_ms_files = nullptr;
DECL_HOOK(void, CStreaming_InitImageList)
{
	spdlog::info("Initializing image list...");

	CStreaming_ms_files = eng::as<CStreamingFile *>(eng::Sym::CStreaming_ms_files);

	if (CStreaming_ms_files)
	{
		for (int i = 0; i < 8; i++)
		{
			CStreaming_ms_files[i].m_name[0] = '\0';
			CStreaming_ms_files[i].m_lsn = 0;
		}

		// CStreaming::LoadCdDirectory iterates ms_files[0..7] in order and SKIPS
		// any modelId already registered (FIRST-WINS). Mounting ModLoader's virtual
		// models.img at slot 0 lets loose .dff/.ifp/.col/.ipl files override base
		// gta3.img non-destructively (uses 1 of 2 spare slots: 5 + 2 lazy = 7 <= 8).
		if (ModLoader::isEnabled() && ModLoader::hasVirtualModelsImg())
		{
			spdlog::info("ModLoader: registering virtual IMG '{}'", ModLoader::virtualModelsImgRelPath());
			eng::fn::CStreaming_AddImageToList( ModLoader::virtualModelsImgRelPath(), true);
		}

		eng::fn::CStreaming_AddImageToList( "TEXDB\\SAMPCOL.IMG", true);
		eng::fn::CStreaming_AddImageToList( "TEXDB\\GTA3.IMG", true);
		eng::fn::CStreaming_AddImageToList( "TEXDB\\GTA_INT.IMG", true);
		eng::fn::CStreaming_AddImageToList( "TEXDB\\SAMP.IMG", true);
		// PLAYER.IMG is registered lazily by the clothes system and CUTSCENE.IMG by
		// gta.dat's IMG line; registering them here too overflows the 8-slot ms_files
		// table and hangs CdStream during level load.
	}
	else
	{
		spdlog::error("Failed to get symbol for CStreaming_ms_files!");
	}
}

// WarDrum's CModelInfoAccelerator caches a sequential model-ID array in models/MINFO.BIN.
// Whenever ModLoader mounts virtual models.img or swaps gta3.img, the IMG entry sequence shifts
// by +N and stale MINFO.BIN causes GetModelInfoFast() to miss every subsequent .dff!
// Resolving directly via CModelInfo::GetModelInfo() avoids desync AND the 20550-entry cap (~3 ms).
DECL_HOOK(void, CModelInfoAccelerator_GetEntry, void *self, void **outInfo, int32_t *outId, char *name)
{
	using FnGetModelInfo = void *(*)(const char *, int32_t *);
	static FnGetModelInfo s_fnGetModelInfo = eng::as<FnGetModelInfo>(eng::Sym::CModelInfo_GetModelInfo);
	if (s_fnGetModelInfo && outInfo)
	{
		*outInfo = s_fnGetModelInfo(name, outId);
		return;
	}
	CModelInfoAccelerator_GetEntry(self, outInfo, outId, name);
}

DECL_HOOK(void, CStreaming_Init2)
{
	CStreaming_Init2();

	// CStreaming::ms_memoryAvailable (GetSymbol returns 0 on failure)
	if (auto* memAvailable = eng::as<uint32_t *>(eng::Sym::CStreaming_ms_memoryAvailable))
	{
		*memAvailable *= 3;
	}
}

DECL_HOOK(void, CTimer_StartUserPause)
{
	if (pUI)
	{
		pUI->setVisible(false);
	}

	if (g_java && pNetGame)
	{
		g_java->setPauseState(true);
	}

	if (auto* userPause = eng::as<bool *>(eng::Sym::CTimer_m_UserPause))
	{
		*userPause = true;
	}
}

DECL_HOOK(void, CTimer_EndUserPause)
{
	if (pUI)
	{
		pUI->setVisible(true);
	}

	if (g_java && pNetGame)
	{
		g_java->setPauseState(false);
	}

	if (auto* userPause = eng::as<bool *>(eng::Sym::CTimer_m_UserPause))
	{
		*userPause = false;
	}
}

unsigned int *msCollected;
unsigned int curMS;

static std::atomic<int> s_pendingScreenshotMode{-1};

void Hooks::requestScreenshot(int modeOverride)
{
	int mode = (modeOverride == 0 || modeOverride == 1) ? modeOverride : Settings::screenshotMode();
	s_pendingScreenshotMode.store(mode);
}

static void WritePngChunk(FILE* fp, const char type[4], const uint8_t* data, uint32_t len)
{
	uint8_t lenBE[4] = {
		(uint8_t)((len >> 24) & 0xFF),
		(uint8_t)((len >> 16) & 0xFF),
		(uint8_t)((len >> 8) & 0xFF),
		(uint8_t)(len & 0xFF)
	};
	fwrite(lenBE, 1, 4, fp);
	fwrite(type, 1, 4, fp);
	if (len > 0 && data) {
		fwrite(data, 1, len, fp);
	}
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
	if (len > 0 && data) {
		crc = crc32(crc, reinterpret_cast<const Bytef*>(data), len);
	}
	uint8_t crcBE[4] = {
		(uint8_t)((crc >> 24) & 0xFF),
		(uint8_t)((crc >> 16) & 0xFF),
		(uint8_t)((crc >> 8) & 0xFF),
		(uint8_t)(crc & 0xFF)
	};
	fwrite(crcBE, 1, 4, fp);
}

static void CaptureCurrentFramebufferToPng(int mode)
{
	if (!Client::gameDir()) return;

	int w = sa::RsGlobal ? sa::RsGlobal->screenWidth : 0;
	int h = sa::RsGlobal ? sa::RsGlobal->screenHeight : 0;
	if (w <= 0 || h <= 0) {
		GLint vp[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_VIEWPORT, vp);
		w = vp[2];
		h = vp[3];
	}
	if (w <= 0 || h <= 0) return;

	std::vector<uint8_t> pixels((size_t)w * (size_t)h * 4);

	// GTA:SA Android v2.10 runs OpenGL ES on a dedicated GraphicsThread via
	// WarDrum's RenderQueue. Calling raw glReadPixels() on GameMain has no
	// bound EGL context and silently leaves pixels zeroed (pure black PNG).
	// emu_glReadPixels() flushes pending draw batches, enqueues
	// RQ_Command_rqReadPixels on GraphicsThread, and blocks on
	// RenderQueue::Flush() until pixels are populated.
	using EmuGlReadPixelsFn = void(*)(int, int, int, int, uint32_t, uint32_t, void*);
	static EmuGlReadPixelsFn s_emuReadPixels = nullptr;
	if (!s_emuReadPixels && g_saSym) {
		s_emuReadPixels = eng::as<EmuGlReadPixelsFn>(eng::Sym::emu_glReadPixels);
	}
	if (s_emuReadPixels) {
		s_emuReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	} else {
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	}

	char dir1[512], dir2[512];
	snprintf(dir1, sizeof(dir1), "%sSAMP", Client::gameDir());
	snprintf(dir2, sizeof(dir2), "%sSAMP/screens", Client::gameDir());
	mkdir(dir1, 0775);
	mkdir(dir2, 0775);

	time_t now = time(nullptr);
	struct tm tmNow{};
	localtime_r(&now, &tmNow);
	static int s_shotSeq = 0;
	char filename[128];
	snprintf(filename, sizeof(filename), "samp_%04d%02d%02d_%02d%02d%02d_%02d_%s.png",
		tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
		tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
		(++s_shotSeq) % 100,
		mode == 1 ? "game" : "hud");

	std::string fullPath = std::string(dir2) + "/" + filename;
	std::string fileLabel(filename);

	if (pUI && pUI->chat()) {
		pUI->chat()->addInfoMessage("-> Screenshot Taken (%s): %s",
			mode == 1 ? "Game Only" : "HUD+Chat", filename);
	}

	std::thread([pixels = std::move(pixels), w, h, fullPath, fileLabel]() {
		const size_t rowBytes = (size_t)w * 4;
		std::vector<uint8_t> raw((rowBytes + 1) * (size_t)h);
		for (int y = 0; y < h; ++y) {
			uint8_t* dst = &raw[(size_t)y * (rowBytes + 1)];
			const uint8_t* src = &pixels[(size_t)(h - 1 - y) * rowBytes];
			dst[0] = 0; // PNG filter type 0 (None)
			memcpy(dst + 1, src, rowBytes);
			for (int x = 0; x < w; ++x) {
				dst[1 + x * 4 + 3] = 0xFF; // force opaque alpha
			}
		}

		uLongf compBound = compressBound((uLong)raw.size());
		std::vector<uint8_t> compressed(compBound);
		if (compress2(compressed.data(), &compBound, raw.data(), (uLong)raw.size(), Z_BEST_SPEED) != Z_OK) {
			spdlog::error("Screenshot compress2 failed for {}", fullPath);
			return;
		}

		FILE* fp = fopen(fullPath.c_str(), "wb");
		if (!fp) {
			spdlog::error("Screenshot fopen failed for {}", fullPath);
			return;
		}
		static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
		fwrite(kPngSig, 1, 8, fp);

		uint8_t ihdr[13] = {
			(uint8_t)((w >> 24) & 0xFF), (uint8_t)((w >> 16) & 0xFF), (uint8_t)((w >> 8) & 0xFF), (uint8_t)(w & 0xFF),
			(uint8_t)((h >> 24) & 0xFF), (uint8_t)((h >> 16) & 0xFF), (uint8_t)((h >> 8) & 0xFF), (uint8_t)(h & 0xFF),
			8, // bit depth
			6, // color type RGBA
			0, 0, 0
		};
		WritePngChunk(fp, "IHDR", ihdr, 13);
		WritePngChunk(fp, "IDAT", compressed.data(), (uint32_t)compBound);
		WritePngChunk(fp, "IEND", nullptr, 0);
		fclose(fp);
		spdlog::info("Screenshot saved: {} ({}x{}, {} bytes)", fileLabel, w, h, (uint32_t)compBound);
	}).detach();
}

static std::atomic<bool> s_bPendingAdjustableHud{false};
static bool s_bWatchingAdjustableHudExit = false;
static std::atomic<bool> s_bAllowInitialWorldBoot{false};
static std::atomic<bool> s_bExitRequested{false};

void Hooks::openAdjustableHud()
{
	s_bPendingAdjustableHud.store(true);
}

void Hooks::allowInitialWorldBoot()
{
	s_bAllowInitialWorldBoot.store(true, std::memory_order_release);
}

// Unified graceful exit path (aligned with PC SA-MP GameHookDeferGameShutdown + /q):
// Prevents MainGameFunc from exiting its loop into DoGameRestart -> CGame::Shutdown ->
// CAudioEngine::Shutdown (OpenSLES/BASS HAL hang on emulators) -> OS_ApplicationEvent UAF.
void Hooks::requestGracefulExit(const char* reason)
{
	if (sa::RsGlobal) {
		sa::RsGlobal->quit = 0;
	}

	if (s_bExitRequested.exchange(true, std::memory_order_acq_rel)) {
		return;
	}

	spdlog::info("Requesting graceful game exit ({})", reason ? reason : "unknown");

	if (pNetGame && pNetGame->GetRakClient()) {
		pNetGame->GetRakClient()->Disconnect(100);
	}

	if (auto logger = spdlog::default_logger()) {
		logger->flush();
	}

	std::thread([]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		_exit(0);
	}).detach();

	if (g_java) {
		g_java->exitGame();
	}
	else {
		_exit(0);
	}
}

DECL_HOOK(void, MainMenuScreen_OnExit, void* _this)
{
	(void) _this;
	Hooks::requestGracefulExit("MainMenuScreen::OnExit");
}

DECL_HOOK(void, QuitApp)
{
	Hooks::requestGracefulExit("QuitApp");
}

DECL_HOOK(bool, DoGameRestart)
{
	// MainGameFunc calls DoGameRestart() both on initial world boot (m_bWantToRestart == 1,
	// RsGlobal->quit == 0 -> sets gGameState = 8 and returns true) and on exit
	// (RsGlobal->quit != 0 -> runs CGame::Shutdown + CAudioEngine::Shutdown and returns false).
	if (sa::RsGlobal && sa::RsGlobal->quit != 0) {
		Hooks::requestGracefulExit("DoGameRestart:quit");
		return false;
	}
	return DoGameRestart();
}

DECL_HOOK(void, MainMenuScreen_OnStartGame, void* _this)
{
	if (!Client::offlineMode()) {
		spdlog::info("Blocked singleplayer Start Game menu in online session");
		return;
	}
	MainMenuScreen_OnStartGame(_this);
}

DECL_HOOK(void, Menu_NewGame)
{
	if (!Client::offlineMode() && !s_bAllowInitialWorldBoot.exchange(false, std::memory_order_acq_rel)) {
		spdlog::info("Blocked singleplayer Menu_NewGame in online session");
		return;
	}
	Menu_NewGame();
}

DECL_HOOK(void, StartGameScreen_OnLoadGame, void* _this)
{
	if (!Client::offlineMode()) {
		return;
	}
	StartGameScreen_OnLoadGame(_this);
}

DECL_HOOK(void, StartGameScreen_OnDeleteGame, void* _this)
{
	if (!Client::offlineMode()) {
		return;
	}
	StartGameScreen_OnDeleteGame(_this);
}

// GTA:SA v2.10 runs the frontend/menu on a separate frame path
// (DoGameState -> RsEventHandler -> AppEventHandler -> FrontendIdle) that
// never reaches Idle()/Render2dStuff(). Client::process() therefore has to
// be driven from here as well or StartGame() can never fire and the client
// sits in the menu forever behind the Java loading overlay.
static bool s_clientProcessedThisFrame = false;
extern volatile pid_t g_gameThreadTid;

DECL_HOOK(void, FrontendIdle)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		g_gameThreadTid = gettid();
		spdlog::info("hook fired: FrontendIdle tid={}", (int) g_gameThreadTid);
	}
	FrontendIdle();

	if (sa::RsGlobal && sa::RsGlobal->quit != 0) {
		Hooks::requestGracefulExit("FrontendIdle:RsGlobal.quit");
		return;
	}

	if (s_bWatchingAdjustableHudExit) {
		auto* pAdjInst = eng::as<uintptr_t*>(eng::Sym::CAdjustableHUD_m_pInstance);
		if (pAdjInst && *pAdjInst == 0) {
			s_bWatchingAdjustableHudExit = false;
			eng::fn::Menu_SwitchOffToGame();
			EnsureTouchWidgetsCreated();
		}
	}

	Client::process(false);
}

DECL_HOOK(void, Render2dStuff)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		spdlog::info("hook fired: Render2dStuff tid={}", (int) gettid());
	}

	// Clean 3D Game-Only Screenshot: capture right before 2D GTA HUD, touch
	// widgets, and SA-MP ImGui Chat/UI are drawn over the 3D scene.
	if (s_pendingScreenshotMode.load() == 1) {
		s_pendingScreenshotMode.store(-1);
		CaptureCurrentFramebufferToPng(1);
	}

	Render2dStuff();

	s_clientProcessedThisFrame = true;
	Client::process(true);

	// touch-widget watchdog: DeleteAll (CGame::Shutdown / gamemode restart /
	// death) or AML plugins like libSAUtils64.so redirecting m_pWidgets can
	// leave stock or custom combat widgets missing — recreate whenever needed.
	{
		static int wdFrames = 0;
		if (++wdFrames >= 60) {
			wdFrames = 0;
			uintptr_t* widgets = GetTouchWidgets();
			if ((pNetGame || Client::offlineMode()) && pGame && GamePool_FindPlayerPed() && pGame->FindPlayerPed() && widgets) {
				// engine rewrites targeting widgets back onto the attack slot
				// on every RepositionAdjustableWidgets — keep our layout fix
				FixTouchWidgetLayout();
				if (!widgets[sa::WidgetIDs::WIDGET_ATTACK]) {
					spdlog::info("touch widgets wiped, re-creating");
					eng::fn::Menu_SwitchOffToGame();
					EnsureTouchWidgetsCreated();
				}
				else if (!widgets[sa::WidgetIDs::WIDGET_BUTTON_CROUCH]
					|| !widgets[sa::WidgetIDs::WIDGET_CAR_CHEATS]
					|| !widgets[sa::WidgetIDs::WIDGET_MISSION_SKIP]) {
					spdlog::info("custom touch widgets missing in live m_pWidgets, re-creating");
					EnsureTouchWidgetsCreated();
				}
			}
		}
	}

	// aim-button latch + block button + on-foot camera toggle: driven every
	// frame in UpdateTouchCombatVisuals() (render visibility) and
	// TickTouchCombatInput() (logic-frame tap/hold sampling).
	if ((pNetGame || Client::offlineMode()) && pGame && GamePool_FindPlayerPed() && pGame->FindPlayerPed()) {
		extern void UpdateTouchCombatVisuals();
		UpdateTouchCombatVisuals();
	}

	// temp widget diagnostic (only emit in debug log mode)
	if (Client::isLogLevelDebug())
	{
		static int diagFrames = -1;
		if (diagFrames >= 0 && diagFrames < 600) {
			++diagFrames;
		}
		else if (diagFrames == -1 && pNetGame && pGame && GamePool_FindPlayerPed() && pGame->FindPlayerPed()) {
			diagFrames = 0;
		}
		if (diagFrames == 120 || diagFrames == 600) {
			if (diagFrames == 600) diagFrames = -2;
			uintptr_t* widgets = GetTouchWidgets();
			int nonNull = 0;
			for (int i = 0; i < 0xBE; ++i) {
				if (widgets && widgets[i]) ++nonNull;
			}
			spdlog::info("WIDGET-DIAG nonnull={}/190 widgets=0x{:x}", nonNull, reinterpret_cast<uintptr_t>(widgets));
			// DrawAll gates (arm64 GOT slots only; CWidget offsets below are arm64-specific)
			if (!VER_x32) {
				for (int i = 0; i < 0xBE && widgets; ++i) {
					uintptr_t w = widgets[i];
					if (!w) continue;
					uint32_t flags = *reinterpret_cast<uint32_t*>(w + 140);
					uint32_t mapping = *reinterpret_cast<uint32_t*>(w + 8);
					float px0 = *reinterpret_cast<float*>(w + 16);
					float py0 = *reinterpret_cast<float*>(w + 20);
					float px1 = *reinterpret_cast<float*>(w + 24);
					float py1 = *reinterpret_cast<float*>(w + 28);
					spdlog::info("WIDGET-DIAG [{}] flags=0x{:08x} map=0x{:08x} pos=({:.0f},{:.0f})-({:.0f},{:.0f})", i, flags, mapping, px0, py0, px1, py1);
				}
				uint8_t* pPhoto = *g_saSym->Abs<uint8_t**>(addr::GOT_TakePhoto_arm64);
				int* pCurInst = *g_saSym->Abs<int**>(addr::GOT_CurInputInst_arm64);
				uintptr_t mobileMenu = *g_saSym->Abs<uintptr_t*>(addr::GOT_MobileMenu_arm64);
				uint32_t mm36 = mobileMenu ? *reinterpret_cast<uint32_t*>(mobileMenu + 36) : 0;
				uintptr_t mm48 = mobileMenu ? *reinterpret_cast<uintptr_t*>(mobileMenu + 48) : 0;
				spdlog::info("WIDGET-DIAG takePhoto={} curInst={} mobileMenu36={} mobileMenu48=0x{:x}",
					pPhoto ? *pPhoto : -1, pCurInst ? *pCurInst : -1, mm36, mm48);
				// aim-mode gates: targeting button (0x13) is only polled when
				// GetEnterTargeting reaches IsTouched/IsReleased(19) — needs
				// IsLockOnMode or (free-aim && InputType==2)
				uintptr_t ms = reinterpret_cast<uintptr_t>(eng::live::mobileSettings());
				spdlog::info("WIDGET-DIAG MobileSettings=0x{:x} aimModeField={} lockon={} freeaim={} inptype={}",
					ms, ms ? *reinterpret_cast<int*>(ms + 256) : -9,
					eng::fn::MobileSettings_IsLockOnMode(),
					eng::fn::MobileSettings_IsFreeAimMode(),
					eng::fn::CHID_GetInputType());
				for (int id : {0x1, 0x12, 0x13, 0x14, 0x20, 0xa5, 0xaa, 0x16, 0x1f, 0x15, 0x98, 0x99}) {
					uintptr_t w = widgets ? widgets[id] : 0;
					spdlog::info("WIDGET-DIAG id=0x{:x} ptr=0x{:x} en={} fl=0x{:03x}", id, w,
						w ? (int)*reinterpret_cast<uint8_t*>(w + 89) : -9,
						w ? *reinterpret_cast<uint32_t*>(w + 140) : 0u);
					if (w) {
						uintptr_t spr = *reinterpret_cast<uintptr_t*>(w + 16);
						spdlog::info("WIDGET-DIAG id=0x{:x} alpha={} cnt={:.1f} spr0=0x{:x} rect=({:.0f},{:.0f},{:.0f},{:.0f})",
							id, (int)*reinterpret_cast<uint8_t*>(w + 88),
							*reinterpret_cast<float*>(w + 136), spr,
							*reinterpret_cast<float*>(w + 44), *reinterpret_cast<float*>(w + 48),
							*reinterpret_cast<float*>(w + 52), *reinterpret_cast<float*>(w + 56));
					}
				}
			}
		}
	}

	if (Settings::displayfps() && msCollected)
	{
		// fps
		msCollected[curMS++ % 0x28] = eng::fn::OS_TimeMS();

		RGBA bgColor = {0, 0, 0, 0};
		RGBA color = {255, 255, 255, 255};

		CFont::SetBackgroundColor(&bgColor);
		CFont::SetScale(1.12f * 0.54, 1.12f);
		CFont::SetOrientation(1);
		CFont::SetJustify(0);
		CFont::SetWrapX(640.0);
		CFont::SetProportional(0);
		CFont::SetFontStyle(1);
		CFont::SetEdge(0);
		CFont::SetColor(&color);

		char szStr[64];
		const unsigned int spanMs = msCollected[(curMS - 1) % 0x28] - msCollected[curMS % 0x28];
		const float fps = (spanMs > 0) ? (39000.0f / static_cast<float>(spanMs)) : 0.0f;
		snprintf(szStr, sizeof(szStr), "FPS: %.2f", fps);
		CFont::PrintString((float)((float)sa::RsGlobal->screenWidth / 640.0) * 4.0, (float)((float)sa::RsGlobal->screenHeight / 448.0) * 200.0, szStr);

		// mem (throttled to ~1 Hz to avoid per-frame mallinfo2 / /proc/meminfo overhead)
		static uint32_t s_lastMemSampleMs = 0;
		static long long s_cachedAllocMB = 0;
		static long long s_cachedTotalMB = 0;
		const uint32_t nowMs = GetTickCount();
		if (s_lastMemSampleMs == 0 || (nowMs - s_lastMemSampleMs) >= 1000)
		{
			s_lastMemSampleMs = nowMs;
			struct mallinfo2 memInfo = mallinfo2();
			s_cachedAllocMB = static_cast<long long>(memInfo.uordblks / (1024 * 1024));
#if VER_x32
			FILE *meminfo = fopen("/proc/meminfo", "r");
			if (meminfo)
			{
				char line[256];
				while (fgets(line, sizeof(line), meminfo))
				{
					if (strncmp(line, "MemTotal:", 9) == 0)
					{
						s_cachedTotalMB = atoll(&line[9]) / 1024;
						break;
					}
				}
				fclose(meminfo);
			}
#else
			struct sysinfo info{};
			if (sysinfo(&info) == 0)
			{
				const unsigned long long unit = info.mem_unit ? info.mem_unit : 1ULL;
				s_cachedTotalMB = static_cast<long long>((static_cast<unsigned long long>(info.totalram) * unit) / (1024ULL * 1024ULL));
			}
#endif
		}

		CFont::SetBackgroundColor(&bgColor);
		CFont::SetScale(1.12f * 0.54, 1.12f);
		CFont::SetOrientation(1);
		CFont::SetJustify(0);
		CFont::SetWrapX(640.0);
		CFont::SetProportional(0);
		CFont::SetFontStyle(1);
		CFont::SetEdge(0);
		CFont::SetColor(&color);

		char szStrMem[64];
		snprintf(szStrMem, sizeof(szStrMem), "MEM: %lld / %lld MB", s_cachedAllocMB, s_cachedTotalMB);
		CFont::PrintString((float)((float)sa::RsGlobal->screenWidth / 640.0) * 4.0, (float)((float)sa::RsGlobal->screenHeight / 448.0) * 210.0, szStrMem);

		// arch
		CFont::SetBackgroundColor(&bgColor);
		CFont::SetScale(1.12f * 0.54, 1.12f);
		CFont::SetOrientation(1);
		CFont::SetJustify(0);
		CFont::SetWrapX(640.0);
		CFont::SetProportional(0);
		CFont::SetFontStyle(1);
		CFont::SetEdge(0);
		CFont::SetColor(&color);

		char szStrArch[64];
		snprintf(szStrArch, sizeof(szStrArch), "ARCH: %s", VER_x32 ? "x32" : "x64");
		CFont::PrintString((float)((float)sa::RsGlobal->screenWidth / 640.0) * 4.0, (float)((float)sa::RsGlobal->screenHeight / 448.0) * 220.0, szStrArch);
	}
}

static void SanitizeAllAlphaLinkLists();

static RwCamera* (*Idle_RwCameraEndUpdate_orig)(RwCamera* camera) = nullptr;
static RwCamera* Idle_RwCameraEndUpdate_hook(RwCamera* camera)
{
	if (pUI)
	{
		bool bCaptureWithHud = (s_pendingScreenshotMode.load() == 0);
		bool prevBtnPanel = false;
		if (bCaptureWithHud && pUI->buttonpanel()) {
			prevBtnPanel = pUI->buttonpanel()->visible();
			pUI->buttonpanel()->setVisible(false);
		}

		pUI->render();

		if (bCaptureWithHud) {
			s_pendingScreenshotMode.store(-1);
			CaptureCurrentFramebufferToPng(0);
			if (pUI->buttonpanel()) {
				pUI->buttonpanel()->setVisible(prevBtnPanel);
			}
		}
	}

	return Idle_RwCameraEndUpdate_orig ? Idle_RwCameraEndUpdate_orig(camera) : RwCameraEndUpdate(camera);
}

DECL_HOOK(uint32_t, Idle, void *param, bool firstFrame)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		g_gameThreadTid = gettid();
		spdlog::info("hook fired: Idle tid={}", (int) g_gameThreadTid);
	}
	SanitizeAllAlphaLinkLists();
	s_clientProcessedThisFrame = false;
	uint32_t result = Idle(param, firstFrame);
	if (sa::RsGlobal && sa::RsGlobal->quit != 0) {
		Hooks::requestGracefulExit("Idle:RsGlobal.quit");
		return result;
	}
	if (!s_clientProcessedThisFrame) {
		Client::process(false);
	}

	return result;
}

DECL_HOOK(void, CRadar_ClearBlip, int32_t nIndex)
{
	uintptr_t dwRetAddr = 0;
	GET_LR(dwRetAddr);

	if ((uint16_t)nIndex > 249)
	{
		spdlog::warn("Invalid blip ID ({}). Called from 0x{:X}", nIndex, dwRetAddr);
		return;
	}

	return CRadar_ClearBlip(nIndex);
}

DECL_HOOK(void, CPools_Initialise)
{
	spdlog::info("GTA pools initializing..");
	Patches::initBetterPoolsSize();
}

ThreadOptimizer g_threadOpt;
DECL_HOOK(void, AND_RunThread, void *a1)
{
	if (g_gameThreadTid == -1) {
		g_gameThreadTid = gettid();
	}
	g_threadOpt.pushThread(gettid());
	AND_RunThread(a1);
}

extern sa::CPlayerInfo *WORLD_PLAYERS;

DECL_HOOK(sa::CPlayerInfo *, GetPlayerInfoForThisPlayerPed, sa::CPed *ped)
{
	if (!WORLD_PLAYERS || !ped) return nullptr;

	for (int i = 0; i < PLAYER_PED_SLOTS; i++)
	{
		if (WORLD_PLAYERS[i].m_pPed == ped)
		{
			return &WORLD_PLAYERS[i];
		}
	}

	return nullptr;
}

DECL_HOOK(uintptr_t *, RQCommand_rqSetAlphaTest, uintptr_t *result)
{
	if (result) {
		*result += 8;
	}
	return result;
}

DECL_HOOK(int, GetInputType)
{
	return 0;
}

// temp: trace which touch widgets report held — punch-while-walking bug
static int s_touchLogBudget = 300;

extern sa::WidgetIDs GetWidgetTypeFromWidget(uintptr_t pWidget);

// dump the full finger-claim state + CHID path state for one IsTouched hit:
// distinguishes (a) CHID::Implements/IsPressed physical-device path from
// (b) m_apInitialTouch finger-ownership path. arm64 addresses only.
static void DumpTouchState(int widgetID, uintptr_t w)
{
	if (VER_x32) return;
	int mapping = w ? *reinterpret_cast<int*>(w + 8) : -9;
	int finger = w ? *reinterpret_cast<int*>(w + 132) : -9;
	int b84 = w ? (int)*reinterpret_cast<uint8_t*>(w + 84) : -9;
	int hold = w ? *reinterpret_cast<int*>(w + 136) : -9;
	uint32_t fl = w ? *reinterpret_cast<uint32_t*>(w + 140) : 0;
	int impl = -9, pressed = -9;
	if (mapping >= 0) {
		impl = eng::fn::CHID_Implements( mapping);
		pressed = eng::fn::CHID_IsPressed( mapping, (float*)nullptr);
	}
	int curInst = -9; uintptr_t devVtbl = 0;
	if (int** pp = g_saSym->Abs<int**>(addr::GOT_CurInputInst_arm64); pp && *pp) {
		curInst = **pp;
		if (uintptr_t* da = *g_saSym->Abs<uintptr_t**>(addr::GOT_InputDeviceArray_arm64); da && curInst >= 0 && curInst < 8) {
			uintptr_t d = da[curInst];
			devVtbl = d ? *reinterpret_cast<uintptr_t*>(d) : 0;
		}
	}
	spdlog::info("TDIAG w=0x{:x} id=0x{:x} map={} f={} b84={} hold={} fl=0x{:03x} | cur={} vtbl=0x{:x} impl={} press={}",
		w, widgetID, mapping, finger, b84, hold, fl, curInst, devVtbl, impl, pressed);
	uintptr_t* init = *g_saSym->Abs<uintptr_t**>(addr::GOT_TouchInitOwner_arm64);
	uintptr_t* rel = *g_saSym->Abs<uintptr_t**>(addr::GOT_TouchRelOwner_arm64);
	uint8_t* act = *g_saSym->Abs<uint8_t**>(addr::GOT_TouchActive_arm64);
	float* pos = *g_saSym->Abs<float**>(addr::GOT_TouchPos_arm64);
	float* down = *g_saSym->Abs<float**>(addr::GOT_TouchDownPos_arm64);
	for (int i = 0; i < 4; ++i) {
		uintptr_t ow = init ? init[i] : 0;
		uintptr_t rw = rel ? rel[i] : 0;
		int isDown = eng::fn::CTouchInterface_IsTouchDown( i);
		spdlog::info("  f{} down={} act={} owner=0x{:x}({}) rel=0x{:x}({}) pos=({:.0f},{:.0f}) downPos=({:.0f},{:.0f})",
			i, isDown, act ? (int)act[i] : -9,
			ow, (int)GetWidgetTypeFromWidget(ow), rw, (int)GetWidgetTypeFromWidget(rw),
			pos ? pos[i * 2] : -9.f, pos ? pos[i * 2 + 1] : -9.f,
			down ? down[i * 2] : -9.f, down ? down[i * 2 + 1] : -9.f);
	}
}

DECL_HOOK(int, TI_IsTouched, int widgetID, void* pVec, int i)
{
	int r = TI_IsTouched(widgetID, pVec, i);
	if (r && s_touchLogBudget > 0) {
		--s_touchLogBudget;
		uintptr_t* widgets = GetTouchWidgets();
		uintptr_t w = (widgets && widgetID >= 0 && widgetID < 0xBE) ? widgets[widgetID] : 0;
		spdlog::info("IsTouched id=0x{:x} r={} vec=({}, {})", widgetID, r,
			pVec ? *(float*)pVec : -1.f, pVec ? *((float*)pVec + 1) : -1.f);
		DumpTouchState(widgetID, w);
	}
	return r;
}
DECL_HOOK(int, TI_IsHeldDown, int widgetID, int i)
{
	int r = TI_IsHeldDown(widgetID, i);
	if (r && s_touchLogBudget > 0) {
		--s_touchLogBudget;
		spdlog::info("IsHeldDown id=0x{:x} r={}", widgetID, r);
	}
	return r;
}

// engine rewrites targeting widgets back onto the shared attack posIdx on
// every reposition — reapply our split layout immediately afterwards or the
// aim icon lands on the fire button and eats its taps
DECL_HOOK(void, TI_RepositionAdjustableWidgets)
{
	extern void FixTouchWidgetLayout();
	TI_RepositionAdjustableWidgets();
	FixTouchWidgetLayout();
}

// log every finger->widget claim; lr identifies the call site inside the
// dispatcher (0x36f610 clear / 0x36f6e4 claim / 0x36f794 steal, +4 = ret addr)
DECL_HOOK(void, W_SetInitialTouch, int finger, uintptr_t widget)
{
	if (s_touchLogBudget > 0) {
		--s_touchLogBudget;
		uintptr_t lr = 0;
		GET_LR(lr);
		spdlog::info("SetInitialTouch f={} w=0x{:x}(id={}) from=0x{:x}", finger, widget,
			(int)GetWidgetTypeFromWidget(widget), lr);
	}
	W_SetInitialTouch(finger, widget);
}

// No HID device may claim to natively provide a control: DrawAll hides a
// touch widget whenever Replaces(mapping) is true, and on this client the
// keyboard-class device claims every button mapping (emulator / physical
// keyboard present) which suppressed all on-screen buttons and the stick.
DECL_HOOK(int, CHID_Replaces, uint32_t mapping)
{
	return 0;
}

// No HID device may implement a touch-widget mapping either: CWidget::IsTouched
// / CWidget::Update treat Implements(mapping) && IsPressed(mapping) as "widget
// held", and the emulator's virtual input device reports pressed for attack
// (mapping 1) and the move axes (30/31) whenever a finger is down, causing
// punch-while-walking. With Implements==0 every widget falls back to the real
// finger-claim path. Consistent with Replaces/GetInputType being forced to 0.
DECL_HOOK(int, CHID_Implements, uint32_t mapping)
{
	return 0;
}

DECL_HOOK(void, CWidget, uintptr_t pWidget, const char *pszSprite, uintptr_t WidgetPos, unsigned int nFlags, uint32_t Mapping)
{
	spdlog::info("Widget: {}", pszSprite);
	CWidget(pWidget, pszSprite, WidgetPos, nFlags, Mapping);
}

extern void RunWithUnscaledJoystickSx(uintptr_t widget, void (*fn)(uintptr_t));

DECL_HOOK(void, CWidget_Update, uintptr_t pWidget)
{
	if (pNetGame)
	{
		switch (ProcessFixedWidget(pWidget))
		{
		case STATE_NONE:
			break;
		case STATE_DISABLED:
			return;
		}
	}
	RunWithUnscaledJoystickSx(pWidget, CWidget_Update);
}

std::atomic<bool> g_suppressWidgetSetEnabled{false};

DECL_HOOK(void, CWidget_SetEnabled, uintptr_t pWidget, bool bEnabled)
{
	if (g_suppressWidgetSetEnabled.load(std::memory_order_acquire))
	{
		return;
	}
	if (pNetGame)
	{
		switch (ProcessFixedWidget(pWidget))
		{
		case STATE_NONE:
			break;
		case STATE_DISABLED:
			bEnabled = false;
			break;
		}
	}
	CWidget_SetEnabled(pWidget, bEnabled);
}

DECL_HOOK(int, CWorld_FindPlayerSlotWithPedPointer, sa::CPed *pPlayersPed)
{
	if (!WORLD_PLAYERS || !pPlayersPed) return -1;

	for (int i = 0; i < PLAYER_PED_SLOTS; ++i)
	{
		if (WORLD_PLAYERS[i].m_pPed == pPlayersPed)
		{
			return i;
		}
	}
	return -1;
}

std::atomic<int> g_lastObjectModel{-1};

DECL_HOOK(int32_t, CObject_Render, sa::CObject *_this)
{
	if (_this)
	{
		g_lastObjectModel.store(_this->m_nModelIndex, std::memory_order_relaxed);
	}

	// object material (covers both CObjectPool objects and CPlayerPed attached objects in O(1))
	CObject *pObject = nullptr;
	if (_this && !IsEntityPlaceable(_this))
	{
		pObject = CObject::FindByGtaEntity(_this);
		if (pObject)
		{
			pObject->TryChangeToCustomObjectMaterial();
		}
	}

	int32_t result = CObject_Render(_this);
	g_lastObjectModel.store(-1, std::memory_order_relaxed);

	if (pObject)
	{
		pObject->ChangeToOriginalObjectMaterial();
	}

	return result;
}

DECL_HOOK(void, CRenderer_RenderEverythingBarRoads)
{
	CRenderer_RenderEverythingBarRoads();

	if (pNetGame)
	{
		CObjectPool *pObjectPool = pNetGame->GetObjectPool();
		if (pObjectPool)
		{
			RwMatrix matCam{};
			bool hasCam = false;
			if (pGame && pGame->GetCamera())
			{
				pGame->GetCamera()->GetMatrix(&matCam);
				hasCam = true;
			}
			float lodScale = 1.0f;
			if (auto *pLodScale = eng::as<float *>(eng::Sym::CRenderer_ms_lodDistScale))
			{
				if (std::isfinite(*pLodScale) && *pLodScale > 0.1f)
				{
					lodScale = *pLodScale;
				}
			}

			for (OBJECTID i = 0; i < MAX_OBJECTS; i++)
			{
				CObject *pObject = pObjectPool->GetAt(i);
				if (!pObject)
				{
					continue;
				}
				if (pObject->m_bForceRender)
				{
					pObject->Render();
					continue;
				}
				if (hasCam && pObject->m_fDrawDistance > 0.0f && pObject->m_entity
					&& pObject->m_entity->m_pRwObject && !IsEntityPlaceable(pObject->m_entity))
				{
					const sa::CVector &pos = pObject->m_entity->GetPosition();
					const float dx = pos.x - matCam.pos.x;
					const float dy = pos.y - matCam.pos.y;
					const float dz = pos.z - matCam.pos.z;
					const float distSq = dx * dx + dy * dy + dz * dz;
					const float maxDist = pObject->m_fDrawDistance;
					if (distSq > maxDist * maxDist)
					{
						continue;
					}

					float nativeDist = 300.0f;
					float boundRadius = 50.0f;
					if (sa::CBaseModelInfo *mi = GetModelInfoByID(pObject->GetModelIndex()))
					{
						if (mi->m_lodDistance > 0.0f)
						{
							const float scaled = mi->m_lodDistance * lodScale;
							if (scaled < nativeDist)
							{
								nativeDist = scaled;
							}
						}
						if (mi->m_pColModel && mi->m_pColModel->m_boundSphere.m_fRadius > boundRadius)
						{
							boundRadius = mi->m_pColModel->m_boundSphere.m_fRadius;
						}
					}

					// GTA's CRenderer::ScanWorld already renders normal objects within nativeDist (~300m cap);
					// manually render objects beyond that range up to the server-specified m_fDrawDistance.
					const float minFarDist = nativeDist * 0.9f;
					if (distSq > minFarDist * minFarDist)
					{
						const float fwd = dx * matCam.up.x + dy * matCam.up.y + dz * matCam.up.z;
						if (fwd > -boundRadius)
						{
							pObject->UpdateRwMatrixAndFrame();
							pObject->Render();
						}
					}
				}
			}
		}
	}
}

DECL_HOOK(void, CWorld_ProcessPedsAfterPreRender)
{
	CWorld_ProcessPedsAfterPreRender();

	if (pNetGame)
	{
		CPlayerPool *pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool)
		{
			pPlayerPool->ProcessAttachedObjects();
		}
	}
}

extern void BeginUVAnimFrame();
extern RpMaterial* DriveUVAnimMaterialCB(RpMaterial* material, void* data);
extern RwMatrix* FindAtomicUVAnimMatrix(RpAtomic* atomic, bool* outHasDualUV = nullptr);

static void SaveAndForceUVWrap(void*& savedAddr)
{
	savedAddr = (void*) (uintptr_t) rwTEXTUREADDRESSWRAP;
	eng::fn::RwRenderStateGet<RwBool>( rwRENDERSTATETEXTUREADDRESS, &savedAddr);
	if (savedAddr != (void*) (uintptr_t) rwTEXTUREADDRESSWRAP) {
		RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*) (uintptr_t) rwTEXTUREADDRESSWRAP);
	}
}

static void RestoreUVWrap(void* savedAddr)
{
	if (savedAddr != (void*) (uintptr_t) rwTEXTUREADDRESSWRAP) {
		RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, savedAddr);
	}
}

DECL_HOOK(RpMaterial*, MaterialUpdateUVAnimCB, RpMaterial* material, void* data)
{
	return DriveUVAnimMaterialCB(material, data);
}

DECL_HOOK(void, rpMatFXOpenGLAllInOneRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		return;
	}

	bool bHasDualUV = false;
	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/)
			? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object), &bHasDualUV)
			: nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		// _rwOpenGLSetRenderStateNoExtras applies the global TEXTUREADDRESS state
		// rather than RwTexture::filterAddressing; force WRAP so scrolling UVs tile.
		SaveAndForceUVWrap(savedAddr);
		// Stock _rpMatFXOpenGLAllInOneRenderCB only switches on effects 1..5 and
		// drops effect 6 (rpMATFXEFFECTDUALUVTRANSFORM) to default render.
		if (bHasDualUV) {
			eng::fn::emu_SetTextureMatrixEnabled( (float*) uvMat, (uint8_t) 1);
		}
	}
	rpMatFXOpenGLAllInOneRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		if (bHasDualUV) {
			eng::fn::emu_SetTextureMatrixEnabled( (float*) nullptr, (uint8_t) 0);
		}
		RestoreUVWrap(savedAddr);
	}
}

DECL_HOOK(int, CustomPipeRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		spdlog::warn("CustomPipeRenderCB: Prevent crash - resEntry or object is null");
		return 0;
	}

	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/) ? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object)) : nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		SaveAndForceUVWrap(savedAddr);
		eng::fn::emu_SetTextureMatrixEnabled( (float*) uvMat, (uint8_t) 1);
	}
	int ret = CustomPipeRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		eng::fn::emu_SetTextureMatrixEnabled( (float*) nullptr, (uint8_t) 0);
		RestoreUVWrap(savedAddr);
	}
	return ret;
}

DECL_HOOK(int, CustomBuildingPipeRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		return 0;
	}

	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/) ? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object)) : nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		SaveAndForceUVWrap(savedAddr);
		eng::fn::emu_SetTextureMatrixEnabled( (float*) uvMat, (uint8_t) 1);
	}
	int ret = CustomBuildingPipeRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		eng::fn::emu_SetTextureMatrixEnabled( (float*) nullptr, (uint8_t) 0);
		RestoreUVWrap(savedAddr);
	}
	return ret;
}

DECL_HOOK(int, rxOpenGLDefaultAllInOneRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		spdlog::warn("rxOpenGLDefaultAllInOneRenderCB: Prevent crash - resEntry or object is null");
		return 0;
	}

	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/) ? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object)) : nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		SaveAndForceUVWrap(savedAddr);
		eng::fn::emu_SetTextureMatrixEnabled( (float*) uvMat, (uint8_t) 1);
	}
	int ret = rxOpenGLDefaultAllInOneRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		eng::fn::emu_SetTextureMatrixEnabled( (float*) nullptr, (uint8_t) 0);
		RestoreUVWrap(savedAddr);
	}
	return ret;
}

DECL_HOOK(RwBool, _RwTextureDestroy, uintptr_t texture)
{
	if (!texture)
	{
		spdlog::warn("RwTextureDestroy: Prevent crash");
		return 0;
	}
	return _RwTextureDestroy(texture);
}

DECL_HOOK(void *, RwFrameAddChild, void *parent, void *child)
{
	if (!parent || !child)
	{
		spdlog::warn("RwFrameAddChild: Prevent crash");
		return nullptr;
	}
	return RwFrameAddChild(parent, child);
}

DECL_HOOK(uint8_t *, RLEDecompress, uint8_t *dest, unsigned int destSize, const uint8_t *src, unsigned int segSize, unsigned int escape)
{
	if (!dest || !src)
	{
		spdlog::warn("RLEDecompress: Prevent crash");
		return nullptr;
	}
	return RLEDecompress(dest, destSize, src, segSize, escape);
}

DECL_HOOK(int, RwResourcesFreeResEntry, RwResEntry *a1)
{
	if (!a1)
	{
		spdlog::warn("RwResourcesFreeResEntry: Prevent crash");
		return 0;
	}
	return RwResourcesFreeResEntry(a1);
}

static void SanitizeAlphaLinkList(void *listPtr, const char *name)
{
	if (!listPtr) return;
	// CLinkList<AlphaObjectInfo>: 4 sentinel CLinks (usedHead, usedTail, freeHead, freeTail).
	// On arm64: sizeof(CLink)=40 (item 24B, prev +24, next +32).
	// On arm32: sizeof(CLink)=20 (item 12B, prev +12, next +16).
	constexpr size_t kLinkSize = VER_x32 ? 20 : 40;
	constexpr size_t kPrevOff  = VER_x32 ? 12 : 24;
	constexpr size_t kNextOff  = VER_x32 ? 16 : 32;

	uint8_t *head = reinterpret_cast<uint8_t *>(listPtr);
	uint8_t *tail = head + kLinkSize;

	auto getNext = [](uint8_t *node) -> uint8_t * {
		return *reinterpret_cast<uint8_t **>(node + kNextOff);
	};
	auto getPrev = [](uint8_t *node) -> uint8_t * {
		return *reinterpret_cast<uint8_t **>(node + kPrevOff);
	};
	auto setNext = [](uint8_t *node, uint8_t *v) {
		*reinterpret_cast<uint8_t **>(node + kNextOff) = v;
	};
	auto setPrev = [](uint8_t *node, uint8_t *v) {
		*reinterpret_cast<uint8_t **>(node + kPrevOff) = v;
	};

	uint8_t *cur = getNext(head);
	bool corrupted = (cur == nullptr);
	int steps = 0;
	while (!corrupted && cur != tail)
	{
		if (++steps > 4096)
		{
			corrupted = true;
			break;
		}
		uint8_t *p = getPrev(cur);
		uint8_t *n = getNext(cur);
		if (!p || !n)
		{
			corrupted = true;
			break;
		}
		cur = n;
	}

	if (corrupted)
	{
		static int s_logBudget = 20;
		if (s_logBudget > 0)
		{
			--s_logBudget;
			spdlog::warn("CVisibilityPlugins::InitAlphaEntityList: repaired corrupted {}", name);
		}
		setNext(head, tail);
		setPrev(tail, head);
	}
}

static void SanitizeAllAlphaLinkLists()
{
	static void *s_alphaList = eng::as<void *>(eng::Sym::CVisibilityPlugins_m_alphaList);
	static void *s_boatList = eng::as<void *>(eng::Sym::CVisibilityPlugins_m_alphaBoatAtomicList);
	static void *s_entityList = eng::as<void *>(eng::Sym::CVisibilityPlugins_m_alphaEntityList);
	static void *s_underwaterList = eng::as<void *>(eng::Sym::CVisibilityPlugins_m_alphaUnderwaterEntityList);
	static void *s_lastList = eng::as<void *>(eng::Sym::CVisibilityPlugins_m_alphaReallyDrawLastList);

	SanitizeAlphaLinkList(s_alphaList, "m_alphaList");
	SanitizeAlphaLinkList(s_boatList, "m_alphaBoatAtomicList");
	SanitizeAlphaLinkList(s_entityList, "m_alphaEntityList");
	SanitizeAlphaLinkList(s_underwaterList, "m_alphaUnderwaterEntityList");
	SanitizeAlphaLinkList(s_lastList, "m_alphaReallyDrawLastList");
}

DECL_HOOK(int, rxOpenGLDefaultAtomicAllInOneNode, void *self, const void *params)
{
	if (params)
	{
		const auto *atomic = *reinterpret_cast<const uint8_t *const *>(params);
		if (atomic)
		{
			// Disassembly-verified RenderWare offsets in libGTASA.so 2.10:
			//   arm32: _rxOpenGLDefaultAtomicAllInOneNode @ 0x220eb0, InstanceVertexArray @ 0x221060
			//   arm64: _rxOpenGLDefaultAtomicAllInOneNode @ 0x2cb5e0, InstanceVertexArray @ 0x2cb820
			// rpworld.h's RpGeometry/RpAtomic struct layouts drift by 8 bytes on arm64, so use raw offsets.
			constexpr size_t kAtomicRepEntryOff  = VER_x32 ? 20  : 40;
			constexpr size_t kAtomicGeomOff      = VER_x32 ? 24  : 48;
			constexpr size_t kAtomicStartMtOff   = VER_x32 ? 80  : 124;
			constexpr size_t kAtomicEndMtOff     = VER_x32 ? 82  : 126;
			constexpr size_t kGeomFlagsOff       = VER_x32 ? 8   : 16;
			constexpr size_t kGeomNumVertsOff    = VER_x32 ? 20  : 28;
			constexpr size_t kGeomNumMtOff       = VER_x32 ? 24  : 32;
			constexpr size_t kGeomMeshOff        = VER_x32 ? 88  : 144;
			constexpr size_t kGeomRepEntryOff    = VER_x32 ? 92  : 152;
			constexpr size_t kGeomMorphTargetOff = VER_x32 ? 96  : 160;
			constexpr size_t kMeshNumMeshesOff   = 4;
			constexpr size_t kMeshSerialOff      = 6;
			constexpr size_t kResEntrySerialOff  = VER_x32 ? 24  : 48;
			constexpr size_t kMorphTargetSize    = VER_x32 ? 28  : 40;
			constexpr size_t kMorphVertsOff      = VER_x32 ? 20  : 24;
			constexpr uint32_t kRpGeometryNative = 0x01000000u;

			const auto *geom = *reinterpret_cast<const uint8_t *const *>(atomic + kAtomicGeomOff);
			if (geom)
			{
				const uint32_t geomFlags = *reinterpret_cast<const uint32_t *>(geom + kGeomFlagsOff);
				const int32_t numVertices = *reinterpret_cast<const int32_t *>(geom + kGeomNumVertsOff);
				const auto *mesh = *reinterpret_cast<const uint8_t *const *>(geom + kGeomMeshOff);
				// Mobile GTA:SA DFFs use rpGEOMETRYNATIVE (verts stored in native GL stream, morphTarget.verts is intentionally null)
				if ((geomFlags & kRpGeometryNative) == 0 &&
					numVertices >= 1 && mesh && *reinterpret_cast<const uint16_t *>(mesh + kMeshNumMeshesOff) != 0)
				{
					const int32_t numMorphTargets = *reinterpret_cast<const int32_t *>(geom + kGeomNumMtOff);
					const auto *repEntry = (numMorphTargets == 1)
						? *reinterpret_cast<const uint8_t *const *>(geom + kGeomRepEntryOff)
						: *reinterpret_cast<const uint8_t *const *>(atomic + kAtomicRepEntryOff);

					bool needsInstance = (repEntry == nullptr);
					if (!needsInstance)
					{
						const uint16_t entrySerial = *reinterpret_cast<const uint16_t *>(repEntry + kResEntrySerialOff);
						const uint16_t meshSerial = *reinterpret_cast<const uint16_t *>(mesh + kMeshSerialOff);
						if (entrySerial != meshSerial)
						{
							needsInstance = true;
						}
					}

					if (needsInstance)
					{
						const int16_t startMt = *reinterpret_cast<const int16_t *>(atomic + kAtomicStartMtOff);
						const int16_t endMt = *reinterpret_cast<const int16_t *>(atomic + kAtomicEndMtOff);
						const auto *morphTargets = *reinterpret_cast<const uint8_t *const *>(geom + kGeomMorphTargetOff);

						auto hasValidVerts = [&](int16_t mt) -> bool {
							if (!morphTargets || mt < 0 || mt >= numMorphTargets) return false;
							const void *verts = *reinterpret_cast<const void *const *>(
								morphTargets + static_cast<size_t>(mt) * kMorphTargetSize + kMorphVertsOff);
							return verts != nullptr;
						};

						if (!hasValidVerts(startMt) || (endMt != startMt && !hasValidVerts(endMt)))
						{
							static int s_skipLogBudget = 20;
							if (s_skipLogBudget > 0)
							{
								--s_skipLogBudget;
								spdlog::warn("rxOpenGLDefaultAtomicAllInOneNode: skip atomic with null verts & no repEntry (model={}, numMt={}, startMt={}, endMt={})",
									g_lastObjectModel.load(std::memory_order_relaxed), numMorphTargets, startMt, endMt);
							}
							return 1;
						}
					}
				}
			}
		}
	}
	return rxOpenGLDefaultAtomicAllInOneNode(self, params);
}

DECL_HOOK(int, TextureDatabaseRuntime_GetEntry, uintptr_t _this, const char *a2, bool *a3)
{
	if (!_this)
	{
		spdlog::warn("TextureDatabaseRuntime_GetEntry: Prevent crash");
		return -1;
	}
	return TextureDatabaseRuntime_GetEntry(_this, a2, a3);
}

DECL_HOOK(void, CGame_Process)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		spdlog::info("hook fired: CGame::Process tid={}", (int) gettid());
	}
	BeginUVAnimFrame();
	CGame_Process();

	Settings::applyDynamicCameraAndFreeCam();

	const bool hudHiddenByGame = (pGame && pGame->IsHudHidden());
	if (auto* pDrawHud = eng::as<bool*>(eng::Sym::CHud_m_Wants_To_Draw_Hud)) {
		*pDrawHud = Settings::showGtaHud() && !hudHiddenByGame;
	}
	if (auto* pHideRadar = eng::as<bool*>(eng::Sym::CHud_bScriptDontDisplayRadar)) {
		*pHideRadar = (!Settings::showRadar() || !Settings::showGtaHud() || hudHiddenByGame);
	}

	if (pNetGame && pGame && pGame->FindPlayerPed()) {
		if (Settings::openHudEditOnStart()) {
			static int s_hudEditStartDelay = 0;
			if (++s_hudEditStartDelay >= 45) {
				s_hudEditStartDelay = 0;
				Settings::setOpenHudEditOnStart(false);
				s_bPendingAdjustableHud.store(true);
			}
		}
		if (s_bPendingAdjustableHud.exchange(false)) {
			uintptr_t pMobileMenu = eng::addr(eng::Sym::gMobileMenu);
			if (pMobileMenu) {
				eng::fn::MobileMenu_InitForPause( pMobileMenu);
				eng::fn::MobileMenu_ProcessPending( pMobileMenu);
				eng::fn::CAdjustableHUD_Toggle();
				s_bWatchingAdjustableHudExit = true;
			}
		}
	}

	if (pNetGame)
	{
		CTextDrawPool *pTextDrawPool = pNetGame->GetTextDrawPool();
		if (pTextDrawPool)
		{
			pTextDrawPool->SnapshotProcess();
		}

		CObjectPool *pObjectPool = pNetGame->GetObjectPool();
		if (pObjectPool)
		{
			pObjectPool->Process();
			pObjectPool->ProcessMaterialText();
		}

		CVehiclePool *pVehiclePool = pNetGame->GetVehiclePool();
		if (pVehiclePool)
		{
			pVehiclePool->ProcessLicensePlates();
		}

		// JNI edit callbacks queue ops from the Java UI thread — apply them
		// here on the game thread where GTA/ScriptCommand access is safe
		EditObject::processPending();

		if (pGame && pGame->GetCamera())
		{
			pGame->GetCamera()->SetToOwner();
		}
	}
}

// T34: SetObjectNoCameraCollision — let GTA camera LOS & sphere checks ignore flagged SA-MP objects
DECL_HOOK(bool, CWorld_CameraToIgnoreThisObject, sa::CEntity *pEntity)
{
	if (pEntity)
	{
		if (CObject *pObj = CObject::FindByGtaEntity(pEntity); pObj && pObj->m_bNoCameraCollision)
		{
			return true;
		}
	}
	return CWorld_CameraToIgnoreThisObject(pEntity);
}

DECL_HOOK(int, CRadar_SetCoordBlip, uint32_t BlpType, sa::CVector vecPosition, uint32_t nColour, uint32_t DispFlag, char *pScriptName)
{
	if (pScriptName && !strncmp(pScriptName, "CODEWAY", 7))
	{
		// CWorld::FindGroundZForCoord — only snap user-placed pause-map target markers ("CODEWAY"),
		// never server-supplied coord blips (e.g. SetPlayerMapIcon style 1/3).
		vecPosition.z = eng::fn::CWorld_FindGroundZForCoord(vecPosition.x, vecPosition.y);

		if (Client::offlineMode() && pGame)
		{
			CPlayerPed *pLocal = pGame->FindPlayerPed();
			if (pLocal && pLocal->m_ped)
			{
				RwMatrix mat = pLocal->m_ped->GetMatrix().ToRwMatrix();
				mat.pos = sa::CVector(vecPosition.x, vecPosition.y, vecPosition.z + 5.0f);
				if (pLocal->IsInVehicle() && pLocal->m_ped->pVehicle)
				{
					pLocal->m_ped->pVehicle->SetMatrix((sa::CMatrix &)mat);
				}
				pLocal->m_ped->SetMatrix((sa::CMatrix &)mat);

				if (pUI && pUI->chat())
				{
					pUI->chat()->addInfoMessage("-> Teleported to: %.2f %.2f %.2f", vecPosition.x, vecPosition.y, vecPosition.z);
				}
			}
		}

		if (pNetGame && pNetGame->GetRakClient())
		{
			RakNet::BitStream bsSend;
			proto::Writer writer(bsSend);
			proto::MapMarkerReq req{};
			req.position = {vecPosition.x, vecPosition.y, vecPosition.z};
			req.serialize(writer, proto::ProtoVersion::V037);
			pNetGame->GetRakClient()->RPC(&RPC_MapMarker, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
		}
	}

	return CRadar_SetCoordBlip(BlpType, vecPosition, nColour, DispFlag, pScriptName);
}

DECL_HOOK(void, CRadar_DrawRadarGangOverlay, bool bFullMap)
{
	if (pNetGame)
	{
		CGangZonePool *pGangZonePool = pNetGame->GetGangZonePool();
		if (pGangZonePool)
		{
			pGangZonePool->Draw(bFullMap);
		}
	}
}

DECL_HOOK(uint32_t, CHudColours_GetIntColour, uintptr_t _this, uint32_t index)
{
	return TranslateColorCodeToRGBA(index);
}

static inline bool HasOccupiedVehicleDriver(const sa::CVehicle *v)
{
	return v && (v->pDriver != nullptr || (v->m_pTowingVehicle != nullptr && v->m_pTowingVehicle->pDriver != nullptr));
}

DECL_HOOK(uint32_t, CAutoMobile_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CAutoMobile_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CBike_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CBike_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CMonsterTruck_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CMonsterTruck_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CTrailer_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (_this && pCollided && pCollided->IsVehicle()
		&& _this->m_pTowingVehicle == reinterpret_cast<sa::CVehicle*>(pCollided))
	{
		// skip self-collision between trailer and its own tractor
		return 0;
	}

	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CTrailer_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CPhysical_ProcessEntityCollision, sa::CPhysical *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (pNetGame && pNetGame->m_netSet && pNetGame->m_netSet->disableVehicleCollisions
		&& _this && pCollided && _this->IsVehicle() && pCollided->IsVehicle()
		&& _this->m_nModelIndex >= 400 && _this->m_nModelIndex <= 611
		&& pCollided->m_nModelIndex >= 400 && pCollided->m_nModelIndex <= 611)
	{
		auto* pVehA = reinterpret_cast<sa::CVehicle*>(_this);
		auto* pVehB = reinterpret_cast<sa::CVehicle*>(pCollided);
		if (HasOccupiedVehicleDriver(pVehA) && HasOccupiedVehicleDriver(pVehB)) {
			return 0;
		}
	}

	return CPhysical_ProcessEntityCollision(_this, pCollided, a3);
}

// PC SA-MP parity (hooks.cpp:1024-1040, GameHookGuardPhysicalProcessCollision):
// SetGravityProcessing(false) sets bit 31 (0x80000000u) on CEntity::m_nFlags.
// Returning before CPhysical::ProcessCollision prevents m_bIsInSafePosition = 1
// when m_bUsesCollision == 0, allowing CWorld::Process -> ProcessShift -> ApplySpeed()
// to integrate velocity smoothly for GhostMode, vehicle-surfing, and entering peds.
DECL_HOOK(void, CPhysical_ProcessCollision, sa::CPhysical *_this)
{
	if (!_this || (_this->m_nFlags & 0x80000000u) != 0)
	{
		return;
	}
	CPhysical_ProcessCollision(_this);
}

// PC SA-MP parity (hooks.cpp:1500-1540): update local & remote surfing attachments
// at the end of CWorld::Process() (after vehicle/object physics step, before RenderScene).
DECL_HOOK(void, CWorld_ProcessAttachedEntities)
{
	CWorld_ProcessAttachedEntities();

	if (!pNetGame)
	{
		return;
	}

	CPlayerPool *pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool)
	{
		return;
	}

	if (CLocalPlayer *pLocal = pPlayerPool->GetLocalPlayer())
	{
		if (pLocal->m_bIsActive && pLocal->m_surfSync.bIsActive)
		{
			pLocal->UpdateSurfing();
		}
	}

	for (PLAYERID i = 0; i <= pPlayerPool->m_iLastPlayerID && i < MAX_PLAYERS; ++i)
	{
		if (!pPlayerPool->GetSlotState(i)) continue;
		CRemotePlayer *pRemote = pPlayerPool->GetAt(i);
		if (pRemote && pRemote->IsActive() && pRemote->IsSurfing())
		{
			pRemote->ProcessSurfing();
		}
	}
}

// PC SA-MP parity (hooks.cpp:1195-1207, GameHookOverrideVerticalLineCollisionFlags):
// Force checkObjects = true whenever buildings are queried so custom CreateObject floors
// act as solid ground across FindGroundZ, SetPlayerPosFindZ, ped placement, and shadows.
DECL_HOOK(bool, CWorld_ProcessVerticalLine, const sa::CVector *vecOrigin, float fDistance, sa::CColPoint *pColPoint,
	sa::CEntity **ppEntity, bool bBuildings, bool bVehicles, bool bPeds, bool bObjects, bool bDummies,
	bool bSeeThrough, void *pIgnoreCar)
{
	if (pNetGame && bBuildings)
	{
		bObjects = true;
	}
	return CWorld_ProcessVerticalLine(vecOrigin, fDistance, pColPoint, ppEntity,
		bBuildings, bVehicles, bPeds, bObjects, bDummies, bSeeThrough, pIgnoreCar);
}

// PC SA-MP parity (patches.cpp:183-185): disable 2-player co-op screen-edge tether clamping.
DECL_HOOK(bool, CGameLogic_IsPlayerAllowedToGoInThisDirection, sa::CPed *pPed, sa::CVector vecDir, float fDistance)
{
	if (!Client::offlineMode())
	{
		return true;
	}
	return CGameLogic_IsPlayerAllowedToGoInThisDirection(pPed, vecDir, fDistance);
}

// PC SA-MP parity (hooks.cpp:1275-1344, GameHookPreservePedTransform):
// Preserve invulnerable Actor position and zero XY velocity across CCivilianPed::ProcessControl.
DECL_HOOK(void, CCivilianPed_ProcessControl, sa::CPed *_this)
{
	if (!_this || !pNetGame)
	{
		CCivilianPed_ProcessControl(_this);
		return;
	}

	const bool bInvulnFlags = _this->physicalFlags.bBulletProof
		&& _this->physicalFlags.bExplosionProof
		&& _this->physicalFlags.bMeleeProof;

	CActor *pManagedActor = nullptr;
	if (CActorPool *pActorPool = pNetGame->GetActorPool())
	{
		PLAYERID actorId = pActorPool->FindIDFromGtaPtr(_this);
		if (actorId != INVALID_PLAYER_ID)
		{
			pManagedActor = pActorPool->GetAt(actorId);
		}
	}

	const bool bLockTransform = bInvulnFlags || (pManagedActor && pManagedActor->m_invulnerable);
	sa::CVector savedPos{};
	bool bHasSavedPos = false;

	if (bLockTransform)
	{
		_this->m_vecMoveSpeed.x = 0.0f;
		_this->m_vecMoveSpeed.y = 0.0f;
		if (_this->m_matrix)
		{
			savedPos = _this->m_matrix->m_pos;
			bHasSavedPos = true;
		}
	}

	CCivilianPed_ProcessControl(_this);

	if (bLockTransform)
	{
		if (bHasSavedPos && _this->m_matrix)
		{
			_this->m_matrix->m_pos = savedPos;
		}
		_this->m_vecMoveSpeed.x = 0.0f;
		_this->m_vecMoveSpeed.y = 0.0f;
	}
}

bool g_bCreatingSirenVehicle = false;

DECL_HOOK(bool, CVehicle_UsesSiren, sa::CVehicle *_this)
{
	if (g_bCreatingSirenVehicle) {
		return true;
	}
	if (CVehicle_UsesSiren(_this)) {
		return true;
	}
	if (_this && pNetGame && pNetGame->GetVehiclePool()) {
		VEHICLEID vid = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(_this);
		if (vid != INVALID_VEHICLE_ID) {
			CVehicle* pVeh = pNetGame->GetVehiclePool()->GetAt(vid);
			if (pVeh && pVeh->m_bSiren) {
				return true;
			}
		}
	}
	return false;
}

// sector coords are floor(pos/50 + 60); CPhysical::Add rejects any bound
// rect whose coords land outside [-120,119] (unsigned c+120 <= 240), so
// out-of-bounds entities never get sector records at all. Repeat-sector
// groups only hash coord & 0xF — remap out-of-range rect edges mod-16 so
// Add accepts the entity and lands its records in the same groups a
// scanner at its true position hashes to.
// Destination band is the dead zone (w0-112 → coords -112..-97,
// x in [-8600,-7850]): remapping into positive cells 0..15 placed the rect
// over real SF-map territory, and since rect-vs-rect tests use the
// REMAPPED rect, OOB entities ghost-collided with real entities sharing
// their hash group — the residual "sticky" feel. No real entity can ever
// reach x < -3000, so dead-zone rects can never overlap real ones.
// Same mechanism libMultiSA ships: it inline-hooks GetBoundRect at +0x53.
DECL_HOOK(sa::CRect, CPhysical_GetBoundRect, sa::CPhysical *_this)
{
	sa::CRect r = CPhysical_GetBoundRect(_this);
	auto fixAxis = [](float &lo, float &hi) {
		int c0 = (int) floorf(lo / 50.0f + 60.0f);
		int c1 = (int) floorf(hi / 50.0f + 60.0f);
		if (c0 >= -120 && c0 <= 120 && c1 >= -120 && c1 <= 120) return;
		if (c1 < c0) { int t = c0; c0 = c1; c1 = t; }
		int span = c1 - c0;
		if (span > 15) span = 15;
		int w0 = c0 & 0xF;
		lo = (w0 - 172) * 50.0f + 25.0f;
		hi = (w0 + span - 172) * 50.0f + 25.0f;
	};
	fixAxis(r.left, r.right);
	fixAxis(r.bottom, r.top);
	return r;
}

// set while a script/opcode-initiated attach or detach is running, so that
// the BreakTowLink hook can tell intentional breaks from GTA-spontaneous ones
bool g_bIntentionalTrailerDetach = false;

DECL_HOOK(void, CTrailer_BreakTowLink, sa::CVehicle *_this)
{
	if (!g_bIntentionalTrailerDetach && pNetGame && _this
			&& _this->m_pTowingVehicle && _this->fHealth > 0.0f)
	{
		CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
		if (pVehiclePool)
		{
			VEHICLEID tractorId = pVehiclePool->FindIDFromGtaPtr(_this->m_pTowingVehicle);
			CVehicle* pTractor = (tractorId != INVALID_VEHICLE_ID) ? pVehiclePool->GetAt(tractorId) : nullptr;
			if (pTractor && pTractor->VerifyInstance() && pTractor->m_pTrailer
					&& !pTractor->IsDriverLocalPlayer())
			{
				// server still owns this attachment: veto GTA-initiated break
				return;
			}
		}
	}

	CTrailer_BreakTowLink(_this);
}

// fix crosshair
float *m_f3rdPersonCHairMultX, *m_f3rdPersonCHairMultY;

DECL_HOOK(void, DrawCrosshair)
{
	if (Settings::crosshairStyle() != 0)
	{
		// Custom crosshair (1..3) drawn via ImGui in UI::drawList(), or Hidden (4)
		return;
	}
	if (!m_f3rdPersonCHairMultX || !m_f3rdPersonCHairMultY)
	{
		DrawCrosshair();
		return;
	}
	float save1 = *m_f3rdPersonCHairMultX;
	*m_f3rdPersonCHairMultX = 0.52f;

	float save2 = *m_f3rdPersonCHairMultY;
	*m_f3rdPersonCHairMultY = 0.435f;

	DrawCrosshair();

	*m_f3rdPersonCHairMultX = save1;
	*m_f3rdPersonCHairMultY = save2;
}

DECL_HOOK(float, CDraw_CalculateAspectRatio)
{
	float ar = CDraw_CalculateAspectRatio();
	int mode = Settings::aspectRatio();
	if (mode > 0)
	{
		switch (mode)
		{
		case 1: ar = 16.0f / 9.0f; break;
		case 2: ar = 20.0f / 9.0f; break;
		case 3: ar = 21.0f / 9.0f; break;
		case 4: ar = 16.0f / 10.0f; break;
		case 5: ar = 4.0f / 3.0f; break;
		default: break;
		}
		if (auto* pAr = eng::as<float*>(eng::Sym::CDraw_ms_fAspectRatio))
		{
			*pAr = ar;
		}
	}
	return ar;
}

void Hooks::installWidgetFixHooks()
{
	spdlog::info("Installing widget fix hooks...");

	GetTouchWidgets();

	// HOOK("_ZN7CWidgetC2EPKcRK14WidgetPositionj10HIDMapping", CWidget);
	REG_HOOK(Boot, CWidget_Update, CWidget_Update, "Touch widget layout & visibility fix");
	REG_HOOK(Boot, CWidget_SetEnabled, CWidget_SetEnabled, "Suppress widget disable during remote ped control");
}

void Hooks::installCrosshairFixHooks()
{
	spdlog::info("Installing crosshair fix hooks...");

	m_f3rdPersonCHairMultX = eng::as<float *>(eng::Sym::CCamera_m_f3rdPersonCHairMultX);
	m_f3rdPersonCHairMultY = eng::as<float *>(eng::Sym::CCamera_m_f3rdPersonCHairMultY);

	REG_HOOK(Boot, CHud_DrawCrossHairs, DrawCrosshair, "Custom crosshair position & visibility");
}

DECL_HOOK(uint16_t*, CText_Get, void* _this, const char* key)
{
	if (uint16_t* custom = CleoEngine::getCustomGxt(key))
	{
		return custom;
	}
	if (!_this || !key)
	{
		return nullptr;
	}
	return CText_Get(_this, key);
}

DECL_HOOK(int, CPlantMgr_AddLocTris, void* entry, const sa::CVector* camPos, int a3, int startTri, int endTri)
{
	if (!entry) return 0;
	auto* entity = *reinterpret_cast<sa::CEntity**>(entry);
	if (!entity || IsEntityPlaceable(entity)) return 0;

	auto* colModel = eng::fn::CEntity_GetColModel<sa::CColModel*>( entity);
	if (!colModel || !colModel->m_pColData) return 0;

	auto* colData = colModel->m_pColData;
	if (!colData->m_pVertices || !colData->m_pTriangles || colData->m_nNumTriangles == 0) {
		return 0;
	}
	if (startTri < 0 || endTri < startTri || endTri >= static_cast<int>(colData->m_nNumTriangles)) {
		return 0;
	}
	for (int i = startTri; i <= endTri; ++i) {
		const sa::CColTriangle& tri = colData->m_pTriangles[i];
		if (tri.vA < 0 || tri.vB < 0 || tri.vC < 0 ||
			tri.vA > 65535 || tri.vB > 65535 || tri.vC > 65535) {
			return 0;
		}
	}
	return CPlantMgr_AddLocTris(entry, camPos, a3, startTri, endTri);
}

DECL_HOOK(void*, CAnimManager_AddAnimation, void* pClump, int groupId, int animId)
{
	void* pRes = CAnimManager_AddAnimation(pClump, groupId, animId);
	if (pRes && pClump) {
		sa::CPed* pLocal = GamePool_FindPlayerPed();
		if (pLocal && pLocal->m_pRwObject == pClump) {
			RecordLocalPedQueuedAnim(groupId, animId, 4.0f);
		}
	}
	return pRes;
}

DECL_HOOK(void*, CAnimManager_BlendAnimation, void* pClump, int groupId, int animId, float blendDelta)
{
	void* pRes = CAnimManager_BlendAnimation(pClump, groupId, animId, blendDelta);
	if (pRes && pClump) {
		sa::CPed* pLocal = GamePool_FindPlayerPed();
		if (pLocal && pLocal->m_pRwObject == pClump) {
			RecordLocalPedQueuedAnim(groupId, animId, blendDelta);
		}
	}
	return pRes;
}

void Hooks::install()
{
	spdlog::info("Installing hooks...");

	msCollected = eng::as<unsigned int *>(eng::Sym::msCollected);
	if (auto* pCurMS = eng::as<unsigned int *>(eng::Sym::curMS))
	{
		curMS = *pCurMS;
	}

	REG_HOOK(Boot, OS_FileOpen, OS_FileOpen, "ModLoader & custom path file override");
	REG_HOOK(Boot, CStreaming_InitImageList, CStreaming_InitImageList, "Register virtual modloader/models.img");
	REG_HOOK(Boot, CModelInfoAccelerator_GetEntry, CModelInfoAccelerator_GetEntry, "Bypass stale MINFO.BIN for virtual models");
	REG_HOOK(Boot, TextureDatabaseRuntime_Register, TextureDatabaseRuntime_Register, "Null-safe texture DB register");
	REG_HOOK(Boot, CStreaming_Init2, CStreaming_Init2, "Expand streaming memory budget");
	REG_HOOK(Boot, CTimer_StartUserPause, CTimer_StartUserPause, "Pause state tracking");
	REG_HOOK(Boot, CTimer_EndUserPause, CTimer_EndUserPause, "Resume state tracking");
	REG_HOOK(Boot, CGame_InitialiseRenderWare, CGame_InitialiseRenderWare, "Load extra texdbs & custom fonts");
	REG_HOOK(Boot, CModelInfo_AddPedModel, CModelInfo_AddPedModel, "Ped model store override");
	REG_HOOK(Boot, CModelInfo_AddAtomicModel, CModelInfo_AddAtomicModel, "Atomic model store override");
	REG_HOOK(Boot, CPools_Initialise, CPools_Initialise, "Expanded entity pools");
	REG_HOOK(Boot, TextureDatabaseRuntime_Load, TextureDatabaseRuntime_Load, "ModLoader texdb load intercept");
	REG_HOOK(Boot, CTxdStore_TxdStoreFindCB, CTxdStore_TxdStoreFindCB, "Custom TXD lookup fallback");

	// SAMP hooks //

	// UI
	REG_HOOK(Boot, ANDRunThread, AND_RunThread, "Capture GameMain thread id");
	REG_HOOK(Boot, FrontendIdle, FrontendIdle, "Auto-start game & adjustable HUD exit");
	REG_HOOK(Boot, MainMenuScreen_OnExit, MainMenuScreen_OnExit, "Graceful process exit on pause-menu Quit Game");
	REG_HOOK(Boot, QuitApp, QuitApp, "Graceful process exit on QuitApp");
	REG_HOOK(Boot, DoGameRestart, DoGameRestart, "Prevent engine shutdown UAF/deadlock on exit or restart");
	REG_HOOK(Boot, MainMenuScreen_OnStartGame, MainMenuScreen_OnStartGame, "Block singleplayer Start Game menu in online session");
	REG_HOOK(Boot, Menu_NewGame, Menu_NewGame, "Block singleplayer New Game restart in online session");
	REG_HOOK(Boot, StartGameScreen_OnLoadGame, StartGameScreen_OnLoadGame, "Block singleplayer Load Game in online session");
	REG_HOOK(Boot, StartGameScreen_OnDeleteGame, StartGameScreen_OnDeleteGame, "Block singleplayer Delete Save in online session");
	REG_HOOK(Boot, Render2dStuff, Render2dStuff, "ImGui HUD & SA-MP 2D overlay render");
	REG_HOOK(Boot, Idle, Idle, "Main render frame & skipped-frame net pump");
	REG_HOOK_BL(Boot, addr::RwCameraEndUpdate_patch, "Idle.RwCameraEndUpdate", Idle_RwCameraEndUpdate_hook, &Idle_RwCameraEndUpdate_orig, "Render ImGui UI & HUD screenshot before RwCameraEndUpdate");
	REG_HOOK(Boot, AND_TouchEvent, AND_TouchEvent, "Touch routing (UI/CLEO/Monet/TextDraw)");
	REG_HOOK(Boot, CLoadingScreen_DisplayPCScreen, DisplayScreen, "Custom loading splash screen");

	// Game process
	REG_HOOK(Boot, CGame_Process, CGame_Process, "Per-frame SA-MP client loop");
	REG_HOOK(Boot, CDraw_CalculateAspectRatio, CDraw_CalculateAspectRatio, "Custom aspect ratio override");
	if (CleoEngine::isEnabled())
	{
		REG_HOOK(Boot, CText_Get, CText_Get, "CLEO custom GXT text lookup");
	}

	// click map rpc
	REG_HOOK(Boot, CRadar_SetCoordBlip, CRadar_SetCoordBlip, "Map click teleport/marker RPC");

	// gangzone
	REG_HOOK(Boot, CRadar_DrawRadarGangOverlay, CRadar_DrawRadarGangOverlay, "SA-MP gang zone radar overlay");

	// radar color
	REG_HOOK(Boot, CHudColours_GetIntColour, CHudColours_GetIntColour, "SA-MP radar blip RGBA colors");

	// local ped blended movement animation capture (PC parity)
	REG_HOOK(Boot, CAnimManager_AddAnimation, CAnimManager_AddAnimation, "Capture local ped AddAnimation for OnFootSync");
	REG_HOOK(Boot, CAnimManager_BlendAnimation, CAnimManager_BlendAnimation, "Capture local ped BlendAnimation for OnFootSync");

	// vehicle collisions & siren
	REG_HOOK(Boot, CAutomobile_ProcessEntityCollision, CAutoMobile_ProcessEntityCollision, "DisableRemoteVehicleCollisions (automobile)");
	REG_HOOK(Boot, CBike_ProcessEntityCollision, CBike_ProcessEntityCollision, "DisableRemoteVehicleCollisions (bike)");
	REG_HOOK(Boot, CMonsterTruck_ProcessEntityCollision, CMonsterTruck_ProcessEntityCollision, "DisableRemoteVehicleCollisions (monster)");
	REG_HOOK(Boot, CTrailer_ProcessEntityCollision, CTrailer_ProcessEntityCollision, "DisableRemoteVehicleCollisions (trailer)");
	REG_HOOK(Boot, CPhysical_ProcessEntityCollision, CPhysical_ProcessEntityCollision, "DisableRemoteVehicleCollisions (boats/other)");
	REG_HOOK(Boot, CPhysical_ProcessCollision, CPhysical_ProcessCollision, "Guard CPhysical::ProcessCollision for 0x80000000 gravity-disabled peds");
	REG_HOOK(Boot, CWorld_ProcessAttachedEntities, CWorld_ProcessAttachedEntities, "Pre-render local & remote surfing matrix sync");
	REG_HOOK(Boot, CWorld_ProcessVerticalLine, CWorld_ProcessVerticalLine, "Include dynamic CObject floors in vertical ground queries");
	REG_HOOK(Boot, CGameLogic_IsPlayerAllowedToGoInThisDirection, CGameLogic_IsPlayerAllowedToGoInThisDirection, "Disable 2P co-op screen tether clamping");
	REG_HOOK(Boot, CCivilianPed_ProcessControl, CCivilianPed_ProcessControl, "Preserve invulnerable Actor transform in ProcessControl");
	REG_HOOK(Boot, CTrailer_BreakTowLink, CTrailer_BreakTowLink, "Prevent spurious trailer detach");
	REG_HOOK(Boot, CVehicle_UsesSiren, CVehicle_UsesSiren, "Enable horn-siren on non-emergency vehicles with addSiren");

	// OOB entities: wrap bound rects so CPhysical::Add accepts them
	REG_HOOK(Boot, CPhysical_GetBoundRect, CPhysical_GetBoundRect, "OOB (|x|>3000) sector bound remapping");

	// obj material
	REG_HOOK(Boot, CObject_Render, CObject_Render, "Object custom material/text & UVAnim");
	REG_HOOK(Boot, CWorld_CameraToIgnoreThisObject, CWorld_CameraToIgnoreThisObject, "SetObjectNoCameraCollision camera ignore");

	// render objects 3000+- pos
	REG_HOOK(Boot, CRenderer_RenderEverythingBarRoads, CRenderer_RenderEverythingBarRoads, "Force-render OOB objects");

	// attched objects
	REG_HOOK(Boot, CWorld_ProcessPedsAfterPreRender, CWorld_ProcessPedsAfterPreRender, "Render player attached objects");

	// crash fixes
	REG_HOOK(Boot, CPlantMgr_ProcessEntryCollisionDataSections_AddLocTris, CPlantMgr_AddLocTris, "Guard bad col triangle indices");
	REG_HOOK(Boot, CRadar_ClearBlip, CRadar_ClearBlip, "Guard invalid radar blip clear");
	REG_HOOK(Boot, CWorld_FindPlayerSlotWithPedPointer, CWorld_FindPlayerSlotWithPedPointer, "Resolve remote player slot");
	REG_HOOK(Boot, MaterialUpdateUVAnimCB, MaterialUpdateUVAnimCB, "World/object UVAnim material update");
	REG_HOOK(Boot, rpMatFXOpenGLAllInOneRenderCB, rpMatFXOpenGLAllInOneRenderCB, "Null-safe MatFX render");
	REG_HOOK(Boot, CCustomBuildingDNPipeline_CustomPipeRenderCB, CustomPipeRenderCB, "Custom building DN pipeline render");
	REG_HOOK(Boot, CCustomBuildingPipeline_CustomPipeRenderCB, CustomBuildingPipeRenderCB, "Custom building pipeline render");
	REG_HOOK(Boot, rxOpenGLDefaultAllInOneRenderCB, rxOpenGLDefaultAllInOneRenderCB, "OpenGL default render wrap/guard");
	REG_HOOK(Boot, RwTextureDestroy, _RwTextureDestroy, "Guard RwTexture double-destroy");
	REG_HOOK(Boot, RwFrameAddChild, RwFrameAddChild, "Guard RwFrameAddChild null/cycle");
	REG_HOOK(Boot, RLEDecompress, RLEDecompress, "Bounds-safe RLEDecompress");
	REG_HOOK(Boot, RwResourcesFreeResEntry, RwResourcesFreeResEntry, "Guard RwResourcesFreeResEntry");
	REG_HOOK(Boot, rxOpenGLDefaultAtomicAllInOneNode, rxOpenGLDefaultAtomicAllInOneNode, "Guard OpenGL atomic node render");
	REG_HOOK(Boot, TextureDatabaseRuntime_GetEntry, TextureDatabaseRuntime_GetEntry, "Texture DB entry lookup guard");
	REG_HOOK(Boot, CPlayerPed_GetPlayerInfoForThisPlayerPed, GetPlayerInfoForThisPlayerPed, "Map ped to relocated WORLD_PLAYERS");

	if (!eglGetProcAddress("glAlphaFuncQCOM"))
	{
		// If "glAlphaFuncQCOM" is not available, try "glAlphaFunc"
		if (eglGetProcAddress("glAlphaFunc"))
		{
			// If "glAlphaFunc" is found, store the address in the global library
			*((void **)(g_saSym->Abs(addr::glAlphaFuncQCOM_slot))) = (void *)eglGetProcAddress("glAlphaFunc");
			patches::registerData("glAlphaFuncQCOM.fallback", "Bind glAlphaFunc into glAlphaFuncQCOM slot",
				patches::Scope::Boot, g_saSym->Abs<uintptr_t>(addr::glAlphaFuncQCOM_slot));
		}
		else
		{
			// If neither function is available, hook the fallback symbol
			REG_HOOK(Boot, RQ_Command_rqSetAlphaTest, RQCommand_rqSetAlphaTest, "Fallback alpha test hook when glAlphaFunc absent");
		}
	}

	// fix spin on keyboard/controller
	REG_HOOK(Boot, CHID_GetInputType, GetInputType, "Keyboard/controller input type override");
	REG_HOOK(Boot, CHID_Replaces, CHID_Replaces, "HID mapping replaces override");
	REG_HOOK(Boot, CHID_Implements, CHID_Implements, "HID mapping implements override");
	REG_HOOK(Boot, CTouchInterface_RepositionAdjustableWidgets, TI_RepositionAdjustableWidgets, "Preserve custom buttons on HUD adjust");

	Pickups::installHooks();
	RemoveBuilding::installHooks();

	installPadHooks();
	installWidgetFixHooks();
	installCrosshairFixHooks();
}
