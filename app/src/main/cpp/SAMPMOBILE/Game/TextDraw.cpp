#include "TextDraw.h"
#include "TextDrawLayout.h"
#include "Font.h"
#include "Game.h"
#include "RGBA.h"
#include "SnapShotHelper.h"
#include "Util.h"
#include "Artwork/ArtworkCore.h"
#include "Artwork/ArtworkManager.h"
#include "Engine/Symbols.h"
#include "../Client.h"
#include "../UI/UI.h"
#include "../UI/UISettings.h"
#include <encoding.h>
#include <RenderWare/rw.h>
#include <SA.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <vector>


extern RwTexture* TextDrawTexture[];

CTextDraw::CTextDraw(TEXT_DRAW_TRANSMIT* pTextDrawTransmit, const char* szText)
{
	memset(&m_TextDrawData, 0, sizeof(TEXT_DRAW_DATA));

	m_TextDrawData.fLetterWidth = pTextDrawTransmit->fLetterWidth;
	m_TextDrawData.fLetterHeight = pTextDrawTransmit->fLetterHeight;

	m_TextDrawData.dwLetterColor = pTextDrawTransmit->dwLetterColor;
	m_TextDrawData.byteUnk12 = 0;
	m_TextDrawData.byteCentered = pTextDrawTransmit->byteCenter;
	m_TextDrawData.byteBox = pTextDrawTransmit->byteBox;

	m_TextDrawData.fLineWidth = pTextDrawTransmit->fLineWidth;
	m_TextDrawData.fLineHeight = pTextDrawTransmit->fLineHeight;

	m_TextDrawData.dwBoxColor = pTextDrawTransmit->dwBoxColor;
	m_TextDrawData.byteProportional = pTextDrawTransmit->byteProportional;
	m_TextDrawData.dwBackgroundColor = pTextDrawTransmit->dwBackgroundColor;
	m_TextDrawData.byteShadow = pTextDrawTransmit->byteShadow;
	m_TextDrawData.byteOutline = pTextDrawTransmit->byteOutline;
	m_TextDrawData.byteAlignLeft = pTextDrawTransmit->byteLeft;
	m_TextDrawData.byteAlignRight = pTextDrawTransmit->byteRight;
	m_TextDrawData.dwStyle = pTextDrawTransmit->byteStyle;

	m_TextDrawData.fX = pTextDrawTransmit->fX;
	m_TextDrawData.fY = pTextDrawTransmit->fY;

	m_TextDrawData.dwParam1 = 0xFFFFFFFF;
	m_TextDrawData.dwParam2 = 0xFFFFFFFF;
	m_TextDrawData.byteSelectable = pTextDrawTransmit->byteSelectable;
	m_TextDrawData.wModelID = pTextDrawTransmit->wModelID;
	m_TextDrawData.vecRot.x = pTextDrawTransmit->vecRot.x;
	m_TextDrawData.vecRot.y = pTextDrawTransmit->vecRot.y;
	m_TextDrawData.vecRot.z = pTextDrawTransmit->vecRot.z;
	m_TextDrawData.fZoom = pTextDrawTransmit->fZoom;
	m_TextDrawData.wColor1 = pTextDrawTransmit->wColor1;
	m_TextDrawData.wColor2 = pTextDrawTransmit->wColor2;
	m_TextDrawData.bHasKeyCode = false;
	m_TextDrawData.iTextureSlot = -1;
	SetText(szText);

	if (m_TextDrawData.dwStyle == 4) {
		m_TextDrawData.iTextureSlot = GetFreeTextDrawTextureSlot();
		LoadTexture();
	}

	m_TextDrawData.bHasRectArea = false;
	m_rectArea.left = 0.0f;
	m_rectArea.right = 0.0f;
	m_rectArea.bottom = 0.0f;
	m_rectArea.top = 0.0f;
	m_bHovered = false;
	m_dwHoverColor = 0;
	m_bUsesCustomAsset = false;
	m_bPendingCustomAsset = false;
}

CTextDraw::~CTextDraw()
{
	DestroyTextDrawTexture(m_TextDrawData.iTextureSlot);
}

/**
 * @param szText
 */
void CTextDraw::SetText(const char* szText)
{
	memset(m_szText, 0, 800);
	strncpy(m_szText, szText, 800);
	m_szText[800] = 0;

	if (m_TextDrawData.dwStyle == 4 && m_TextDrawData.iTextureSlot != -1) {
		DestroyTextDrawTexture(m_TextDrawData.iTextureSlot);
		LoadTexture();
	}
}

void CTextDraw::LoadTexture()
{
	char txdname[64 + 1];
	memset(txdname, 0, sizeof(txdname));
	char texturename[64 + 1];
	memset(texturename, 0, sizeof(texturename));

	char* szTexture = strchr(m_szText, ':');
	if (szTexture == nullptr) return;

	if (strlen(m_szText) < 64 && strchr(m_szText, '\\') == nullptr && strchr(m_szText, '/') == nullptr) {
		strncpy(txdname, m_szText, (size_t) (szTexture - m_szText));
		strcpy(texturename, ++szTexture);

		char rawTextureName[64 + 1];
		strncpy(rawTextureName, texturename, sizeof(rawTextureName) - 1);
		rawTextureName[sizeof(rawTextureName) - 1] = '\0';

		spdlog::info("LOADTEXTURE: txdname: {}, texture: {}", txdname, texturename);

		if (!strcasecmp(txdname, "ld_shtr") && !strcasecmp(texturename, "ship")) {
			strcpy(texturename, "ship_ld_shtr");
		}
		else if (!strcasecmp(txdname, "ld_spac") && !strcasecmp(texturename, "shoot")) {
			strcpy(texturename, "shoot_LD_SPAC");
		}
		else if (!strcasecmp(txdname, "ld_spac") && !strcasecmp(texturename, "white")) {
			strcpy(texturename, "white_LD_SPAC");
		}
		else {
			static char* texture_samp[] = {
					"smokeii_3", "back2", "back3", "back4", "back5",
					"back6", "back7", "back8", "back8_right", "back8_top",
					"intro1", "intro2", "intro3", "intro4", "duality", "health", "thrustg",
					"ric1", "ric2", "ric3", "ric4", "ric5", "load0uk", "loadsc0", "loadsc1",
					"loadsc10", "loadsc11", "loadsc12", "loadsc13", "loadsc14", "loadsc2",
					"loadsc3", "loadsc4", "loadsc5", "loadsc6", "loadsc7", "loadsc8",
					"loadsc9", "eax", "nvidia", "title_pc_eu", "title_pc_us", "loadscuk",
					"tyre64a", "wheel_alloy64", "wheel_classic64", "wheel_lighttruck64",
					"wheel_lightvan64", "wheel_offroad64", "wheel_saloon64", "wheel_smallcar64",
					"wheel_sport64", "wheel_truck64", "whee_rim64", "outro"};

			for (auto& i : texture_samp) {
				if (!strcasecmp(i, texturename)) {
					char buf[64];
					sprintf(buf, "%s_%s", texturename, txdname);
					strcpy(texturename, buf);
				}
			}
		}

		if (m_TextDrawData.iTextureSlot != -1) {
			RwTexture* tex = nullptr;
			m_bUsesCustomAsset = false;
			m_bPendingCustomAsset = false;
			// 0.3.DL custom TXD sprite syntax: "mdl<modelId>:<texname>" (e.g. "mdl-2001:sign")
			if (strncasecmp(txdname, "mdl", 3) == 0 && txdname[3] != '\0') {
				char* endPtr = nullptr;
				const long parsedId = strtol(txdname + 3, &endPtr, 10);
				if (endPtr && *endPtr == '\0') {
					const int32_t customId = static_cast<int32_t>(parsedId);
					if (artwork::isCustomObjectId(customId) || artwork::isCustomSkinId(customId)) {
						m_bUsesCustomAsset = true;
						tex = artwork::ArtworkManager::instance().findTextureInCustomModel(customId, rawTextureName);
						m_bPendingCustomAsset = (tex == nullptr);
					}
				}
			}
			if (!tex && !m_bUsesCustomAsset) {
				tex = LoadTextureFromTxd(txdname, texturename);
				if (!tex && strcasecmp(texturename, rawTextureName) != 0) {
					tex = LoadTextureFromTxd(txdname, rawTextureName);
				}
			}
			TextDrawTexture[m_TextDrawData.iTextureSlot] = tex;
		}
	}
}

static RGBA DWORD2RGBA(uint32_t dwColor)
{
	RGBA tmp;

	tmp.R = dwColor & 0xFF;
	dwColor >>= 8;
	tmp.G = dwColor & 0xFF;
	dwColor >>= 8;
	tmp.B = dwColor & 0xFF;
	dwColor >>= 8;
	tmp.A = dwColor & 0xFF; /* dwColor >>= 8; */

	return tmp;
}

void CTextDraw::Draw()
{
	if (m_TextDrawData.iTextureSlot != -1 || m_TextDrawData.dwStyle == 4) {
		DrawTextured();
	}
	else if (NeedsUnicodeRender()) {
		DrawUnicode();
	}
	else {
		DrawDefault();
	}
}

bool CTextDraw::NeedsUnicodeRender()
{
	if (m_TextDrawData.dwStyle == 4 || m_TextDrawData.dwStyle == 5) return false;
	return StringHasNonAscii(m_szText);
}

void CTextDraw::ComputeRectArea()
{
	int iScreenWidth = sa::RsGlobal->screenWidth;
	int iScreenHeight = sa::RsGlobal->screenHeight;
	float fHorizHudScale = 1.0f / 640.0f;
	float fVertHudScale = 1.0f / 448.0f;

	float fLineWidth = iScreenWidth * m_TextDrawData.fLineWidth * fHorizHudScale;
	float fLineHeight = iScreenWidth * m_TextDrawData.fLineHeight * fHorizHudScale;

	float fUseY = iScreenHeight - ((448.0 - m_TextDrawData.fY) * (iScreenHeight * fVertHudScale));
	float fUseX = iScreenWidth - ((640.0 - m_TextDrawData.fX) * (iScreenWidth * fHorizHudScale));

	if (m_TextDrawData.byteAlignRight) {
		m_rectArea.left = fUseX - (fLineWidth - fUseX);
		m_rectArea.right = fUseX;
		m_rectArea.bottom = fUseY;
		m_rectArea.top = fUseY + fLineHeight;
	}
	else if (m_TextDrawData.byteCentered) {
		m_rectArea.left = fUseX - (fLineHeight * 0.5f);
		m_rectArea.right = m_rectArea.left + fLineHeight;
		m_rectArea.bottom = fUseY;
		m_rectArea.top = fUseY + fLineWidth;
	}
	else {
		m_rectArea.left = fUseX;
		m_rectArea.right = fLineWidth - fUseX + fUseX;
		m_rectArea.bottom = fUseY;
		m_rectArea.top = fUseY + fLineHeight;
	}

	m_TextDrawData.bHasRectArea = true;
}

static textdraw::FontReferenceMetrics ProbeFontReferenceMetrics(const ImFont* font)
{
	textdraw::FontReferenceMetrics ref{};
	if (!font) {
		return ref;
	}
	ref.fontSize = font->FontSize > 0.0f ? font->FontSize : 18.0f;
	ref.bakedExtraSpacingX = (font->ConfigData != nullptr) ? font->ConfigData->GlyphExtraSpacing.x : 0.0f;
	ref.cjkAdvancePx = ref.fontSize * 0.76f;
	ref.cjkInkHeightPx = ref.fontSize * 0.72f;
	ref.cjkInkTopPx = ref.fontSize * 0.14f;

	// Probe a full-box CJK glyph (fallback to 'M') to measure the loaded font's true
	// advance and ink box independent of its ascender/descender ratio (e.g. Microsoft YaHei
	// 1.32x vs Noto Sans SC 1.17x vs SimHei 1.00x).
	static constexpr ImWchar kProbeChars[] = {0x56FD, 0x4E2D, 0x4E00, 'M'};
	for (ImWchar ch : kProbeChars) {
		const ImFontGlyph* g = font->FindGlyphNoFallback(ch);
		if (!g) continue;
		const float natAdv = g->AdvanceX - ref.bakedExtraSpacingX;
		const float inkH = g->Y1 - g->Y0;
		if (natAdv > 1.0f && inkH > 1.0f) {
			ref.cjkAdvancePx = natAdv;
			ref.cjkInkHeightPx = inkH;
			ref.cjkInkTopPx = g->Y0;
			break;
		}
	}
	return ref;
}

namespace {

struct RwTextDrawBatch {
	std::vector<RwIm2DVertex> verts;
	std::vector<RwImVertexIndex> indices;
	RwReal nearZ = 0.0f;
	RwReal recipZ = 1.0f;

	void flush(RwRaster* raster)
	{
		if (indices.empty()) {
			return;
		}
		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, reinterpret_cast<void*>(raster));
		RwIm2DRenderIndexedPrimitive(
				rwPRIMTYPETRILIST,
				verts.data(),
				static_cast<RwInt32>(verts.size()),
				indices.data(),
				static_cast<RwInt32>(indices.size()));
		verts.clear();
		indices.clear();
	}

	void pushQuad(
			RwRaster* raster,
			float x0, float y0, float x1, float y1,
			float u0, float v0, float u1, float v1,
			uint32_t abgrColor)
	{
		if (verts.size() + 4 > 65532) {
			flush(raster);
		}
		const RwImVertexIndex base = static_cast<RwImVertexIndex>(verts.size());
		const size_t idx = verts.size();
		verts.resize(idx + 4);

		auto setVert = [&](RwIm2DVertex& vtx, float x, float y, float u, float v) {
			RwIm2DVertexSetScreenX(&vtx, x);
			RwIm2DVertexSetScreenY(&vtx, y);
			RwIm2DVertexSetScreenZ(&vtx, nearZ);
			RwIm2DVertexSetRecipCameraZ(&vtx, recipZ);
			vtx.emissiveColor = abgrColor;
			RwIm2DVertexSetU(&vtx, u, recipZ);
			RwIm2DVertexSetV(&vtx, v, recipZ);
		};

		setVert(verts[idx + 0], x0, y0, u0, v0);
		setVert(verts[idx + 1], x1, y0, u1, v0);
		setVert(verts[idx + 2], x1, y1, u1, v1);
		setVert(verts[idx + 3], x0, y1, u0, v1);

		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}
};

void AppendScaledLinePass(
		RwTextDrawBatch& batch,
		RwRaster* fontRaster,
		const ImFont* font,
		const textdraw::WrappedLine& line,
		const textdraw::LayoutInput& input,
		const textdraw::LayoutMetrics& metrics,
		float originX,
		float originY,
		uint8_t defaultAlpha,
		bool forceColor,
		uint32_t forcedColorAbgr)
{
	if (!font || line.glyphs.empty()) {
		return;
	}
	if (forceColor && ((forcedColorAbgr & 0xFF000000u) == 0u)) {
		return;
	}
	if (!forceColor && defaultAlpha == 0u) {
		return;
	}

	const float baseY = originY + metrics.glyphBaselineOffsetY;
	float curX = originX;
	for (const auto& span : line.glyphs) {
		const uint32_t cp = span.codepoint;
		if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r') {
			const ImWchar wcharCode = (cp <= 0xFFFFu) ? static_cast<ImWchar>(cp) : font->FallbackChar;
			if (const ImFontGlyph* g = font->FindGlyph(wcharCode)) {
				float xOff = 0.0f;
				if (cp < 0x80 && !input.proportional) {
					xOff = (metrics.unpropAsciiAdvanceX - span.naturalWidthX) * 0.5f;
				}
				const float gx = curX + xOff;
				const uint32_t col = forceColor
						? forcedColorAbgr
						: IM_COL32(
								(span.colorRgb >> 16) & 0xFFu,
								(span.colorRgb >> 8) & 0xFFu,
								span.colorRgb & 0xFFu,
								defaultAlpha);
				batch.pushQuad(
						fontRaster,
						gx + g->X0 * metrics.glyphScaleX,
						baseY + g->Y0 * metrics.glyphScaleY,
						gx + g->X1 * metrics.glyphScaleX,
						baseY + g->Y1 * metrics.glyphScaleY,
						g->U0, g->V0, g->U1, g->V1,
						col);
			}
		}
		curX += span.advanceX;
	}
}

} // namespace

void CTextDraw::DrawUnicode()
{
	// Keep selectable hit area up to date even on early return (matches DrawDefault).
	ComputeRectArea();

	if (m_szText[0] == '\0' || !pUI) return;

	ImFont* pFont = pUI->font();
	RwRaster* fontRaster = pUI->fontRaster();
	if (!pFont || !fontRaster) return;

	uint32_t dwLetterColor = m_bHovered ? m_dwHoverColor : m_TextDrawData.dwLetterColor;
	RGBA letterColor = DWORD2RGBA(dwLetterColor);

	char resetHex[9];
	snprintf(resetHex, sizeof(resetHex), "{%02X%02X%02X}", letterColor.R, letterColor.G, letterColor.B);

	std::string text = Encoding::cp2utf(ReplaceKey(m_szText));
	text = GxtToImGuiColors(text, resetHex);
	if (text.empty()) return;

	textdraw::LayoutInput input{};
	input.screenWidth = static_cast<float>(sa::RsGlobal->screenWidth);
	input.screenHeight = static_cast<float>(sa::RsGlobal->screenHeight);
	input.x = m_TextDrawData.fX;
	input.y = m_TextDrawData.fY;
	input.letterWidth = m_TextDrawData.fLetterWidth;
	input.letterHeight = m_TextDrawData.fLetterHeight;
	input.lineWidth = m_TextDrawData.fLineWidth;
	input.lineHeight = m_TextDrawData.fLineHeight;
	input.style = m_TextDrawData.dwStyle;
	input.proportional = (m_TextDrawData.byteProportional != 0);
	input.box = (m_TextDrawData.byteBox != 0);
	input.centered = (m_TextDrawData.byteCentered != 0);
	input.alignRight = (m_TextDrawData.byteAlignRight != 0);
	input.outline = m_TextDrawData.byteOutline;
	input.shadow = m_TextDrawData.byteShadow;

	static const ImFont* s_cachedFont = nullptr;
	static RwRaster* s_cachedRaster = nullptr;
	static textdraw::FontReferenceMetrics s_cachedRef{};
	if (s_cachedFont != pFont || s_cachedRaster != fontRaster) {
		s_cachedRef = ProbeFontReferenceMetrics(pFont);
		s_cachedFont = pFont;
		s_cachedRaster = fontRaster;
	}

	const uint32_t defaultRgb =
			(static_cast<uint32_t>(letterColor.R) << 16) |
			(static_cast<uint32_t>(letterColor.G) << 8) |
			static_cast<uint32_t>(letterColor.B);

	const float bakedExtraX = s_cachedRef.bakedExtraSpacingX;
	auto measureNaturalAdvance = [pFont, bakedExtraX](uint32_t cp) -> float {
		const ImWchar wcharCode = (cp <= 0xFFFFu) ? static_cast<ImWchar>(cp) : pFont->FallbackChar;
		const ImFontGlyph* g = pFont->FindGlyph(wcharCode);
		if (!g) {
			return std::max(1.0f, pFont->FallbackAdvanceX - bakedExtraX);
		}
		return std::max(1.0f, g->AdvanceX - bakedExtraX);
	};

	textdraw::LayoutResult layout = textdraw::layoutText(text, defaultRgb, input, s_cachedRef, measureNaturalAdvance);
	if (!layout.metrics.valid || layout.lines.empty()) {
		return;
	}

	// Draw directly through RenderWare 2D inside CTextDrawPool::Draw() so Unicode/Chinese
	// TextDraws obey strict 0..MAX_TEXT_DRAWS-1 Z-order alongside ASCII, Sprite, and
	// 3D ModelPreview TextDraws (eliminating layering & click-occlusion mismatches).
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*) 0);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*) 0);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*) 1);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*) rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*) rwBLENDINVSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*) 0);
	RwRenderStateSet(rwRENDERSTATECULLMODE, (void*) rwCULLMODECULLNONE);
	RwRenderStateSet(rwRENDERSTATEBORDERCOLOR, (void*) 0);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*) rwALPHATESTFUNCTIONGREATER);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*) 0);
	RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*) rwFILTERLINEAR);
	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*) rwTEXTUREADDRESSCLAMP);

	// Reused single-threaded batch storage (GameMain only) to avoid per-frame allocations.
	static RwTextDrawBatch s_batch;
	s_batch.verts.clear();
	s_batch.indices.clear();
	s_batch.nearZ = getNearScreenZ();
	s_batch.recipZ = getRecipNearClip();

	if (layout.hasBox) {
		RGBA boxColor = DWORD2RGBA(m_TextDrawData.dwBoxColor);
		if (boxColor.A > 0) {
			s_batch.pushQuad(
					nullptr,
					layout.boxRect.left, layout.boxRect.top,
					layout.boxRect.right, layout.boxRect.bottom,
					0.0f, 0.0f, 0.0f, 0.0f,
					m_TextDrawData.dwBoxColor);
			s_batch.flush(nullptr);
		}
	}

	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*) 2);

	RGBA dropColor = DWORD2RGBA(m_TextDrawData.dwBackgroundColor);
	const uint32_t dropColorAbgr = m_TextDrawData.dwBackgroundColor;

	float curY = layout.metrics.posY;
	for (const auto& line : layout.lines) {
		if (!line.glyphs.empty()) {
			const float lineX = textdraw::computeLineOriginX(input, layout.metrics, line.width);

			// Match GTA:SA CFont::SetEdge / SetDropShadowPosition: outline takes precedence
			// over shadow, uses dwBackgroundColor, and ignores inline color tags.
			if (input.outline > 0 && dropColor.A > 0) {
				const float ox = layout.metrics.dropOffsetX;
				const float oy = layout.metrics.dropOffsetY;
				static constexpr float kDirs[8][2] = {
						{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
						{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
				for (const auto& d : kDirs) {
					AppendScaledLinePass(s_batch, fontRaster, pFont, line, input, layout.metrics,
							lineX + d[0] * ox, curY + d[1] * oy, letterColor.A, true, dropColorAbgr);
				}
			}
			else if (input.shadow > 0 && dropColor.A > 0) {
				AppendScaledLinePass(s_batch, fontRaster, pFont, line, input, layout.metrics,
						lineX + layout.metrics.dropOffsetX, curY + layout.metrics.dropOffsetY,
						letterColor.A, true, dropColorAbgr);
			}

			AppendScaledLinePass(s_batch, fontRaster, pFont, line, input, layout.metrics,
					lineX, curY, letterColor.A, false, 0u);
		}
		curY += layout.metrics.lineHeight;
	}

	s_batch.flush(fontRaster);
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, (void*) 0);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*) 0);
}

void CTextDraw::DrawDefault()
{
	if (m_szText[0] == '\0') return;

	int iScreenWidth = sa::RsGlobal->screenWidth;
	int iScreenHeight = sa::RsGlobal->screenHeight;
	float fHorizHudScale = 1.0f / 640.0f;
	float fVertHudScale = 1.0f / 448.0f;

	float fScaleY = (float) iScreenHeight * fVertHudScale * m_TextDrawData.fLetterHeight * 0.5f;
	float fScaleX = (float) iScreenWidth * fHorizHudScale * m_TextDrawData.fLetterWidth;

	CFont::SetScale(fScaleX, fScaleY);

	if (m_bHovered) {
		//uint32_t color = __builtin_bswap32(m_dwHoverColor | (0x000000FF));
		RGBA color = DWORD2RGBA(m_dwHoverColor);
		CFont::SetColor(&color);
	}
	else {
		RGBA letterColor = DWORD2RGBA(m_TextDrawData.dwLetterColor);
		CFont::SetColor(&letterColor);
	}

	CFont::SetJustify(0);

	if (m_TextDrawData.byteAlignRight) {
		CFont::SetOrientation(2);
	}
	else if (m_TextDrawData.byteCentered) {
		CFont::SetOrientation(0);
	}
	else {
		CFont::SetOrientation(1);
	}

	float fLineWidth = iScreenWidth * m_TextDrawData.fLineWidth * fHorizHudScale;
	CFont::SetWrapX(fLineWidth);

	float fLineHeight = iScreenWidth * m_TextDrawData.fLineHeight * fHorizHudScale;
	CFont::SetCentreSize(fLineHeight);

	CFont::SetBackground(m_TextDrawData.byteBox, 0);

	RGBA boxColor = DWORD2RGBA(m_TextDrawData.dwBoxColor);
	CFont::SetBackgroundColor(&boxColor);

	CFont::SetProportional(m_TextDrawData.byteProportional);

	RGBA backgroundColor = DWORD2RGBA(m_TextDrawData.dwBackgroundColor);
	CFont::SetDropColor(&backgroundColor);

	if (m_TextDrawData.byteOutline) {
		CFont::SetEdge(m_TextDrawData.byteOutline);
	}
	else {
		CFont::SetDropShadowPosition(m_TextDrawData.byteShadow);
	}

	CFont::SetFontStyle(m_TextDrawData.dwStyle);

	float fUseY = iScreenHeight - ((448.0 - m_TextDrawData.fY) * (iScreenHeight * fVertHudScale));
	float fUseX = iScreenWidth - ((640.0 - m_TextDrawData.fX) * (iScreenWidth * fHorizHudScale));

	std::string text = ReplaceKey(m_szText);
	CFont::PrintString(fUseX, fUseY, text.c_str());

	CFont::SetEdge(0);

	ComputeRectArea();
}

void CTextDraw::DrawTextured()
{
	float scaleX = sa::RsGlobal->screenWidth * (1.0f / 640.0f);
	float scaleY = sa::RsGlobal->screenHeight * (1.0f / 448.0f);

	m_rectArea.left = m_TextDrawData.fX * scaleX;
	m_rectArea.bottom = m_TextDrawData.fY * scaleY;
	m_rectArea.right = (m_TextDrawData.fX + m_TextDrawData.fLineWidth) * scaleX;
	m_rectArea.top = (m_TextDrawData.fY + m_TextDrawData.fLineHeight) * scaleY;

	static float uv_reflected[8] = {
			0.0f, 1.0f,
			1.0f, 1.0f,
			0.0f, 0.0f,
			1.0f, 0.0f};

	static float uv_normal[8] = {
			0.0f, 0.0f,
			1.0f, 0.0f,
			0.0f, 1.0f,
			1.0f, 1.0f};

	m_TextDrawData.bHasRectArea = true;

	if (m_TextDrawData.iTextureSlot < 0 || m_TextDrawData.iTextureSlot >= 200) {
		return;
	}
	RwTexture* tex = TextDrawTexture[m_TextDrawData.iTextureSlot];
	if (!tex) {
		return;
	}

	if (m_bHovered) {
		DrawTextureUV(tex, &m_rectArea, m_dwHoverColor | 0xFF000000u, m_TextDrawData.dwStyle == 5 ? uv_reflected : uv_normal);
	}
	else {
		DrawTextureUV(tex, &m_rectArea, m_TextDrawData.dwLetterColor, m_TextDrawData.dwStyle == 5 ? uv_reflected : uv_normal);
	}
}

void CTextDraw::SnapshotProcess()
{
	if (m_TextDrawData.dwStyle != 5 || m_TextDrawData.iTextureSlot != -1) {
		return;
	}

	RwTexture* snapshot = nullptr;

	// 0.3.DL custom model preview (16-bit wire ID: negative objects arrive as uint16 two's complement)
	const int32_t wireModel = artwork::decodeWireModelId16(m_TextDrawData.wModelID);
	if (artwork::isCustomSkinId(wireModel)) {
		m_bUsesCustomAsset = true;
		const int32_t slot = artwork::ArtworkManager::instance().ensureCustomSkinLoaded(wireModel);
		const bool ready = (slot >= 0 && IsValidPedModel(static_cast<uint>(slot)));
		m_bPendingCustomAsset = !ready;
		const int pedModel = ready ? slot : artwork::ArtworkManager::instance().resolveFallbackSkinModel(wireModel);
		snapshot = pSnapShotHelper->CreatePedSnapShot(
				pedModel,
				m_TextDrawData.dwBackgroundColor,
				&m_TextDrawData.vecRot,
				m_TextDrawData.fZoom);
	}
	else if (artwork::isCustomObjectId(wireModel)) {
		m_bUsesCustomAsset = true;
		const int32_t slot = artwork::ArtworkManager::instance().ensureCustomObjectLoaded(wireModel);
		const bool ready = (slot >= 0 && IsValidModel(slot));
		m_bPendingCustomAsset = !ready;
		const int objModel = ready ? slot : artwork::ArtworkManager::instance().resolveFallbackObjectModel(wireModel);
		snapshot = pSnapShotHelper->CreateObjectSnapShot(
				objModel,
				m_TextDrawData.dwBackgroundColor,
				&m_TextDrawData.vecRot,
				m_TextDrawData.fZoom);
	}
	// PED MODEL
	else if (IsValidPedModel(m_TextDrawData.wModelID)) {
		if (!IsExistInfoForModel(m_TextDrawData.wModelID)) {
			m_TextDrawData.wModelID = 0;
		}

		snapshot = pSnapShotHelper->CreatePedSnapShot(
				m_TextDrawData.wModelID,
				m_TextDrawData.dwBackgroundColor,
				&m_TextDrawData.vecRot,
				m_TextDrawData.fZoom);
	}
		// VEHICLE MODEL
	else if (m_TextDrawData.wModelID >= 400 && m_TextDrawData.wModelID <= 611) {
		snapshot = pSnapShotHelper->CreateVehicleSnapShot(
				m_TextDrawData.wModelID,
				m_TextDrawData.dwBackgroundColor,
				&m_TextDrawData.vecRot,
				m_TextDrawData.fZoom,
				m_TextDrawData.wColor1,
				m_TextDrawData.wColor2
		);
	}
		// OBJECT MODEL
	else if ((m_TextDrawData.wModelID <= 320 || m_TextDrawData.wModelID > 373) && m_TextDrawData.wModelID < 615) {
		if (m_TextDrawData.wModelID >= 0) return;
		if (!IsValidModel(m_TextDrawData.wModelID)) {
			m_TextDrawData.wModelID = 18631;
		}

		snapshot = pSnapShotHelper->CreateObjectSnapShot(
				m_TextDrawData.wModelID,
				m_TextDrawData.dwBackgroundColor,
				&m_TextDrawData.vecRot,
				m_TextDrawData.fZoom
		);
	}
	else {
		if (!IsExistInfoForModel(m_TextDrawData.wModelID)) {
			m_TextDrawData.wModelID = 18631;
		}

		snapshot = pSnapShotHelper->CreateObjectSnapShot(
				m_TextDrawData.wModelID,
				m_TextDrawData.dwBackgroundColor,
				&m_TextDrawData.vecRot,
				m_TextDrawData.fZoom
		);
	}

	if (snapshot) {
		const int slot = GetFreeTextDrawTextureSlot();
		if (slot >= 0 && slot < 200) {
			m_TextDrawData.iTextureSlot = slot;
			TextDrawTexture[slot] = snapshot;
		}
		else {
			eng::fn::RwTextureDestroy(snapshot);
		}
	}
}

void CTextDraw::RefreshCustomAsset()
{
	if (!m_bUsesCustomAsset || !m_bPendingCustomAsset) {
		return;
	}
	if (m_TextDrawData.dwStyle == 4) {
		if (m_TextDrawData.iTextureSlot != -1) {
			DestroyTextDrawTexture(m_TextDrawData.iTextureSlot);
			m_TextDrawData.iTextureSlot = -1;
		}
		m_TextDrawData.iTextureSlot = GetFreeTextDrawTextureSlot();
		if (m_TextDrawData.iTextureSlot != -1) {
			LoadTexture();
		}
	} else if (m_TextDrawData.dwStyle == 5) {
		if (m_TextDrawData.iTextureSlot != -1) {
			DestroyTextDrawTexture(m_TextDrawData.iTextureSlot);
			m_TextDrawData.iTextureSlot = -1;
		}
		// Next SnapshotProcess() tick will re-render the now-loaded custom model.
	}
}

void CTextDraw::ReleaseCustomAssetBeforeUnload()
{
	if (!m_bUsesCustomAsset) {
		return;
	}
	if (m_TextDrawData.iTextureSlot != -1) {
		DestroyTextDrawTexture(m_TextDrawData.iTextureSlot);
		m_TextDrawData.iTextureSlot = -1;
	}
	m_bPendingCustomAsset = true;
}
