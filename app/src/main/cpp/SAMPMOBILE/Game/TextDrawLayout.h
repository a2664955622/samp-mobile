#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace textdraw {

// GTA:SA CFont + PC Chinese patch (wm_sachs) layout constants:
// - CFont::GetHeight / ComputeHeight (libGTASA.so 0x6cce5c, 0x6cd4a8):
//     ScaleY * 32.0f * 0.5f + ScaleY * 2.0f == ScaleY * 18.0f
// - CFont::PrintChar / wm_sachs::PrintCHSChar:
//     cell width = ScaleX * 32.0f, cell height = ScaleY * 20.0f
//     (visual CJK glyph ink fits cleanly at ~15.5 * ScaleY inside 18.0 * ScaleY line step)
// - CFont::PrintString (libGTASA.so 0x6cdf28..0x6cdfd0):
//     box padding = +-4.0f px, box height = numLines * lineHeight + 8.0f
// - CFont::RenderString shadow/edge offset (libGTASA.so 0x6cd8b4):
//     offsetX = (screenW / 640.0f) * amount, offsetY = (screenH / 448.0f) * amount
constexpr float kVirtualWidth = 640.0f;
constexpr float kVirtualHeight = 448.0f;
constexpr float kLineHeightFactor = 18.0f;
constexpr float kCjkAdvanceFactor = 32.0f;
constexpr float kCjkVisualHeightFactor = 15.5f;
constexpr float kCjkTopMarginFactor = 1.25f;
constexpr float kBoxPaddingPx = 4.0f;
constexpr float kUnpropWidthFont0 = 27.0f;
constexpr float kUnpropWidthFont1 = 20.0f;

struct LayoutInput {
	float screenWidth = 640.0f;
	float screenHeight = 448.0f;
	float x = 0.0f;
	float y = 0.0f;
	float letterWidth = 0.48f;
	float letterHeight = 1.12f;
	float lineWidth = 1280.0f;
	float lineHeight = 1280.0f;
	uint32_t style = 1;
	bool proportional = true;
	bool box = false;
	bool centered = false;
	bool alignRight = false;
	uint8_t outline = 0;
	uint8_t shadow = 0;
};

struct FontReferenceMetrics {
	float fontSize = 18.0f;
	float bakedExtraSpacingX = 0.0f;
	// Natural advance (without bakedExtraSpacingX) of a fullwidth CJK reference glyph.
	float cjkAdvancePx = 13.64f;
	// Visual ink vertical span (Y1 - Y0) and top bearing (Y0) of that CJK reference glyph.
	float cjkInkHeightPx = 12.0f;
	float cjkInkTopPx = 2.5f;
};

struct LayoutMetrics {
	float posX = 0.0f;
	float posY = 0.0f;
	float scaleX = 0.0f;
	float scaleY = 0.0f;
	float lineHeight = 0.0f;
	float glyphScaleX = 0.0f;
	float glyphScaleY = 0.0f;
	float glyphBaselineOffsetY = 0.0f;
	float edgeAdvanceX = 0.0f;
	float unpropAsciiAdvanceX = 0.0f;
	float wrapX = 0.0f;
	float centreSize = 0.0f;
	float maxLineWidth = 0.0f;
	float dropOffsetX = 0.0f;
	float dropOffsetY = 0.0f;
	bool valid = false;
};

struct RectF {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
};

struct GlyphSpan {
	uint32_t codepoint = 0;
	uint32_t colorRgb = 0xFFFFFFu;
	float advanceX = 0.0f;
	float naturalWidthX = 0.0f;
};

struct WrappedLine {
	std::vector<GlyphSpan> glyphs;
	float width = 0.0f;
};

struct LayoutResult {
	LayoutMetrics metrics;
	std::vector<WrappedLine> lines;
	RectF boxRect;
	bool hasBox = false;
};

inline LayoutMetrics computeMetrics(const LayoutInput& in, const FontReferenceMetrics& ref)
{
	LayoutMetrics m{};
	if (in.screenWidth <= 0.0f || in.screenHeight <= 0.0f ||
			in.letterWidth <= 0.0f || in.letterHeight <= 0.0f ||
			ref.fontSize <= 0.0f) {
		return m;
	}

	const float horizScale = in.screenWidth / kVirtualWidth;
	const float vertScale = in.screenHeight / kVirtualHeight;

	m.posX = in.screenWidth - (kVirtualWidth - in.x) * horizScale;
	m.posY = in.screenHeight - (kVirtualHeight - in.y) * vertScale;
	m.scaleX = horizScale * in.letterWidth;
	m.scaleY = vertScale * in.letterHeight * 0.5f;
	m.lineHeight = m.scaleY * kLineHeightFactor;

	const float safeCjkAdv = std::clamp(ref.cjkAdvancePx, ref.fontSize * 0.45f, ref.fontSize * 1.5f);
	const float safeCjkInkH = std::clamp(ref.cjkInkHeightPx, ref.fontSize * 0.40f, ref.fontSize * 1.15f);

	m.glyphScaleX = (kCjkAdvanceFactor * m.scaleX) / safeCjkAdv;
	m.glyphScaleY = (kCjkVisualHeightFactor * m.scaleY) / safeCjkInkH;
	m.glyphBaselineOffsetY = kCjkTopMarginFactor * m.scaleY - ref.cjkInkTopPx * m.glyphScaleY;

	m.edgeAdvanceX = static_cast<float>(in.outline) * m.scaleX;
	const float unpropRaw = (in.style == 0 || in.style == 2) ? kUnpropWidthFont0 : kUnpropWidthFont1;
	m.unpropAsciiAdvanceX = unpropRaw * m.scaleX;

	m.wrapX = horizScale * in.lineWidth;
	m.centreSize = horizScale * in.lineHeight;

	if (in.centered) {
		m.maxLineWidth = m.centreSize;
	}
	else if (in.alignRight) {
		// SA-MP never calls CFont::SetRightJustifyWrap, so RightJustifyWrap remains 0.0f.
		m.maxLineWidth = m.posX;
	}
	else {
		m.maxLineWidth = m.wrapX - m.posX;
	}

	const uint8_t dropAmount = in.outline > 0 ? in.outline : in.shadow;
	m.dropOffsetX = horizScale * static_cast<float>(dropAmount);
	m.dropOffsetY = vertScale * static_cast<float>(dropAmount);
	m.valid = true;
	return m;
}

inline bool parseInlineHexColor(const char* p, const char* end, uint32_t& outRgb)
{
	if (!p || (end - p) < 8 || p[0] != '{' || p[7] != '}') {
		return false;
	}
	uint32_t rgb = 0;
	for (int i = 1; i <= 6; ++i) {
		const unsigned char c = static_cast<unsigned char>(p[i]);
		uint32_t nibble = 0;
		if (c >= '0' && c <= '9') nibble = static_cast<uint32_t>(c - '0');
		else if (c >= 'a' && c <= 'f') nibble = static_cast<uint32_t>(10 + (c - 'a'));
		else if (c >= 'A' && c <= 'F') nibble = static_cast<uint32_t>(10 + (c - 'A'));
		else return false;
		rgb = (rgb << 4) | nibble;
	}
	outRgb = rgb;
	return true;
}

// Decodes one UTF-8 codepoint from [p, end), advancing p by at least 1 byte.
inline uint32_t decodeUtf8(const char*& p, const char* end)
{
	const unsigned char b0 = static_cast<unsigned char>(*p++);
	if (b0 < 0x80) {
		return b0;
	}
	if ((b0 & 0xE0) == 0xC0 && p < end) {
		const unsigned char b1 = static_cast<unsigned char>(p[0]);
		if ((b1 & 0xC0) == 0x80) {
			++p;
			return ((static_cast<uint32_t>(b0 & 0x1F) << 6) | static_cast<uint32_t>(b1 & 0x3F));
		}
	}
	else if ((b0 & 0xF0) == 0xE0 && (end - p) >= 2) {
		const unsigned char b1 = static_cast<unsigned char>(p[0]);
		const unsigned char b2 = static_cast<unsigned char>(p[1]);
		if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
			p += 2;
			return ((static_cast<uint32_t>(b0 & 0x0F) << 12) |
					(static_cast<uint32_t>(b1 & 0x3F) << 6) |
					static_cast<uint32_t>(b2 & 0x3F));
		}
	}
	else if ((b0 & 0xF8) == 0xF0 && (end - p) >= 3) {
		const unsigned char b1 = static_cast<unsigned char>(p[0]);
		const unsigned char b2 = static_cast<unsigned char>(p[1]);
		const unsigned char b3 = static_cast<unsigned char>(p[2]);
		if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
			p += 3;
			return ((static_cast<uint32_t>(b0 & 0x07) << 18) |
					(static_cast<uint32_t>(b1 & 0x3F) << 12) |
					(static_cast<uint32_t>(b2 & 0x3F) << 6) |
					static_cast<uint32_t>(b3 & 0x3F));
		}
	}
	return '?';
}

inline float computeGlyphAdvance(
		uint32_t cp,
		const LayoutInput& in,
		const LayoutMetrics& m,
		float naturalFontAdvancePx,
		float& outNaturalWidthX)
{
	if (cp == '\t') {
		outNaturalWidthX = kCjkAdvanceFactor * m.scaleX;
		return outNaturalWidthX + m.edgeAdvanceX;
	}
	outNaturalWidthX = naturalFontAdvancePx * m.glyphScaleX;
	if (cp < 0x80 && !in.proportional) {
		return m.unpropAsciiAdvanceX + m.edgeAdvanceX;
	}
	return outNaturalWidthX + m.edgeAdvanceX;
}

inline RectF computeBoxRect(const LayoutInput& in, const LayoutMetrics& m, size_t numLines)
{
	RectF r{};
	const float linesF = static_cast<float>(numLines > 0 ? numLines : 1);
	r.top = m.posY - kBoxPaddingPx;
	r.bottom = m.posY + linesF * m.lineHeight + kBoxPaddingPx;

	if (in.centered) {
		const float halfW = m.centreSize * 0.5f + kBoxPaddingPx;
		r.left = m.posX - halfW;
		r.right = m.posX + halfW;
	}
	else if (in.alignRight) {
		// Exact match to CFont::PrintString (libGTASA.so 0x6cdf58) with RightJustifyWrap == 0.
		r.left = -kBoxPaddingPx;
		r.right = m.posX + kBoxPaddingPx;
	}
	else {
		r.left = m.posX - kBoxPaddingPx;
		r.right = m.wrapX + kBoxPaddingPx;
	}
	return r;
}

inline float computeLineOriginX(const LayoutInput& in, const LayoutMetrics& m, float lineWidth)
{
	if (in.alignRight) {
		return m.posX - lineWidth;
	}
	if (in.centered) {
		return m.posX - lineWidth * 0.5f;
	}
	return m.posX;
}

// Lays out UTF-8 text (with inline {RRGGBB} colors and '\n' newlines) using
// GTA:SA CFont + wm_sachs wrapping semantics:
// - ASCII non-space runs wrap as words; non-ASCII (>= 0x80) codepoints break per character.
// - An empty line always accepts at least one token (no infinite wrap loops).
// - Trailing space at a wrap boundary is dropped so centered/right lines don't shift.
// - Inline {RRGGBB} colors carry across both explicit '\n' and auto-wrapped breaks.
// `measureNaturalAdvance` returns the unscaled ImFont glyph advance (with bakedExtraSpacingX
// already removed) for a Unicode codepoint.
template <typename MeasureFn>
inline LayoutResult layoutText(
		const std::string& utf8WithHexColors,
		uint32_t defaultRgb,
		const LayoutInput& in,
		const FontReferenceMetrics& ref,
		MeasureFn&& measureNaturalAdvance)
{
	LayoutResult result{};
	result.metrics = computeMetrics(in, ref);
	if (!result.metrics.valid || utf8WithHexColors.empty()) {
		return result;
	}

	const LayoutMetrics& m = result.metrics;
	const char* p = utf8WithHexColors.data();
	const char* end = p + utf8WithHexColors.size();
	uint32_t activeRgb = defaultRgb;

	WrappedLine curLine{};

	auto flushLine = [&]() {
		result.lines.push_back(std::move(curLine));
		curLine = WrappedLine{};
	};

	auto makeSpan = [&](uint32_t cp, uint32_t rgb) {
		GlyphSpan s{};
		s.codepoint = cp;
		s.colorRgb = rgb;
		const float nat = (cp == '\t') ? 0.0f : measureNaturalAdvance(cp);
		s.advanceX = computeGlyphAdvance(cp, in, m, nat, s.naturalWidthX);
		return s;
	};

	while (p < end) {
		uint32_t parsedRgb = 0;
		if (*p == '{' && parseInlineHexColor(p, end, parsedRgb)) {
			activeRgb = parsedRgb;
			p += 8;
			continue;
		}
		if (*p == '\r') {
			++p;
			continue;
		}
		if (*p == '\n') {
			++p;
			flushLine();
			continue;
		}
		if (*p == ' ') {
			++p;
			GlyphSpan sp = makeSpan(' ', activeRgb);
			if (!curLine.glyphs.empty() && curLine.width + sp.advanceX > m.maxLineWidth) {
				// Space triggers wrap: drop the breaking space (matches CFont::ProcessCurrentString).
				flushLine();
			}
			else {
				curLine.width += sp.advanceX;
				curLine.glyphs.push_back(sp);
			}
			continue;
		}

		// Read one wrappable token: either a single non-ASCII (>= 0x80) codepoint
		// or a contiguous run of printable ASCII non-space characters (allowing inline {RRGGBB}).
		std::vector<GlyphSpan> token;
		float tokenWidth = 0.0f;

		const char* probe = p;
		uint32_t firstCp = decodeUtf8(probe, end);
		if (firstCp >= 0x80 || firstCp == '\t') {
			p = probe;
			GlyphSpan s = makeSpan(firstCp, activeRgb);
			tokenWidth += s.advanceX;
			token.push_back(s);
		}
		else {
			while (p < end) {
				if (*p == '{' && parseInlineHexColor(p, end, parsedRgb)) {
					activeRgb = parsedRgb;
					p += 8;
					continue;
				}
				if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
					break;
				}
				const char* nextP = p;
				uint32_t cp = decodeUtf8(nextP, end);
				if (cp >= 0x80) {
					break;
				}
				p = nextP;
				GlyphSpan s = makeSpan(cp, activeRgb);
				tokenWidth += s.advanceX;
				token.push_back(s);
			}
		}

		if (!token.empty()) {
			if (!curLine.glyphs.empty() && curLine.width + tokenWidth > m.maxLineWidth) {
				// Trim trailing spaces from the finished line before wrapping.
				while (!curLine.glyphs.empty() && curLine.glyphs.back().codepoint == ' ') {
					curLine.width -= curLine.glyphs.back().advanceX;
					curLine.glyphs.pop_back();
				}
				if (curLine.width < 0.0f) curLine.width = 0.0f;
				flushLine();
			}
			for (const auto& g : token) {
				curLine.width += g.advanceX;
				curLine.glyphs.push_back(g);
			}
		}
	}

	// Emit the final line if it has glyphs, or if the entire string produced no lines yet
	// (e.g., color-only or whitespace-only input). A trailing '\n' does NOT add an extra blank line
	// beyond what was already flushed (matching CFont::ProcessCurrentString).
	if (!curLine.glyphs.empty() || result.lines.empty()) {
		flushLine();
	}

	if (in.box) {
		result.hasBox = true;
		result.boxRect = computeBoxRect(in, m, result.lines.size());
	}

	return result;
}

} // namespace textdraw
