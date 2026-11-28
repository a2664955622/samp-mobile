// Host-side unit tests for Chinese TextDraw layout & metrics (Game/TextDrawLayout.h).
// Shares the exact layout implementation compiled into CTextDraw::DrawImGui() and verifies
// parity against GTA:SA CFont (libGTASA.so v2.10) + PC wm_sachs Chinese patch formulas.
#include "check.h"

#include "Game/TextDrawLayout.h"
#include "Game/TextDrawHitCore.h"

#include <string>

using namespace textdraw;

static FontReferenceMetrics makeYaHeiRef()
{
	// Matches dist/font.ttc Microsoft YaHei rasterized at atlasFontSize = 18.0f
	// with baked GlyphExtraSpacing.x = 2.0f stripped out.
	FontReferenceMetrics ref{};
	ref.fontSize = 18.0f;
	ref.bakedExtraSpacingX = 2.0f;
	ref.cjkAdvancePx = 13.64f;
	ref.cjkInkHeightPx = 12.0f;
	ref.cjkInkTopPx = 2.5f;
	return ref;
}

static FontReferenceMetrics makeSimHeiRef()
{
	// Different TTF vertical metrics ((asc-desc)/upem == 1.00 vs YaHei 1.32).
	FontReferenceMetrics ref{};
	ref.fontSize = 18.0f;
	ref.bakedExtraSpacingX = 2.0f;
	ref.cjkAdvancePx = 18.0f;
	ref.cjkInkHeightPx = 15.0f;
	ref.cjkInkTopPx = 1.5f;
	return ref;
}

static float mockYaHeiAdvance(uint32_t cp)
{
	if (cp >= 0x80) return 13.64f; // Fullwidth CJK
	if (cp == ' ') return 4.04f;   // 0.296 * 13.64
	if (cp >= 'A' && cp <= 'Z') return 9.60f;
	if (cp >= '0' && cp <= '9') return 8.00f;
	return 7.54f; // lowercase
}

static void testScaleAndLineHeightParity()
{
	LayoutInput in{};
	in.screenWidth = 1920.0f;
	in.screenHeight = 1080.0f;
	in.x = 320.0f;
	in.y = 90.0f;
	in.letterWidth = 0.35f;
	in.letterHeight = 1.3f;

	const FontReferenceMetrics yahei = makeYaHeiRef();
	const LayoutMetrics m = computeMetrics(in, yahei);
	CHECK(m.valid);

	// Exact CFont::SetScale inputs from PC SA-MP textdraw.cpp
	const float expectedScaleX = 1920.0f / 640.0f * 0.35f;          // 1.05
	const float expectedScaleY = 1080.0f / 448.0f * 1.3f * 0.5f;    // 1.5669643
	CHECK_NEAR(m.posX, 960.0f, 1e-3);
	CHECK_NEAR(m.posY, 1080.0f * (90.0f / 448.0f), 1e-3);
	CHECK_NEAR(m.scaleX, expectedScaleX, 1e-5);
	CHECK_NEAR(m.scaleY, expectedScaleY, 1e-5);

	// Exact CFont::GetHeight() / wm_sachs line step == ScaleY * 18.0f (NOT 30.0f!)
	CHECK_NEAR(m.lineHeight, expectedScaleY * 18.0f, 1e-4);

	// Fullwidth CJK advance == ScaleX * 32.0f (wm_sachs SBCLetterWidth = 32)
	float natW = 0.0f;
	const float cjkAdv = computeGlyphAdvance(0x4E2D, in, m, yahei.cjkAdvancePx, natW);
	CHECK_NEAR(cjkAdv, expectedScaleX * 32.0f, 1e-4);

	// Visual CJK ink height is font-metric invariant across YaHei and SimHei.
	const FontReferenceMetrics simhei = makeSimHeiRef();
	const LayoutMetrics mSim = computeMetrics(in, simhei);
	CHECK_NEAR(yahei.cjkInkHeightPx * m.glyphScaleY, simhei.cjkInkHeightPx * mSim.glyphScaleY, 1e-4);
	CHECK_NEAR(yahei.cjkInkHeightPx * m.glyphScaleY, expectedScaleY * kCjkVisualHeightFactor, 1e-4);
	CHECK_NEAR(m.glyphBaselineOffsetY + yahei.cjkInkTopPx * m.glyphScaleY,
			mSim.glyphBaselineOffsetY + simhei.cjkInkTopPx * mSim.glyphScaleY, 1e-4);
}

static void testIndependentLetterWidthAspectRatio()
{
	// Narrow tall TextDraw (0.18 x 1.5) vs wide short TextDraw (0.50 x 1.0)
	LayoutInput narrow{};
	narrow.screenWidth = 1920.0f;
	narrow.screenHeight = 1080.0f;
	narrow.letterWidth = 0.18f;
	narrow.letterHeight = 1.5f;

	LayoutInput wide = narrow;
	wide.letterWidth = 0.50f;
	wide.letterHeight = 1.0f;

	const auto ref = makeYaHeiRef();
	const auto mNarrow = computeMetrics(narrow, ref);
	const auto mWide = computeMetrics(wide, ref);

	// Changing fLetterWidth must scale horizontal glyph width proportionally while keeping lineHeight tied to fLetterHeight.
	CHECK_NEAR(mNarrow.glyphScaleX / mWide.glyphScaleX, 0.18f / 0.50f, 1e-5);
	CHECK_NEAR(mNarrow.lineHeight / mWide.lineHeight, 1.5f / 1.0f, 1e-5);
	CHECK(mNarrow.glyphScaleX < mNarrow.glyphScaleY);
	CHECK(mWide.glyphScaleX > mWide.glyphScaleY);
}

static void testCenteredBoxUsesNumLinesNotLineHeight()
{
	// Regression test: SA-MP centered boxed TextDraws pass box WIDTH in fLineHeight (TextDrawTextSize y),
	// e.g. TextDrawTextSize(td, 0.0, 260.0). Box height MUST be numLines * 18 * scaleY + 8, NOT 260!
	LayoutInput in{};
	in.screenWidth = 1920.0f;
	in.screenHeight = 1080.0f;
	in.x = 320.0f;
	in.y = 90.0f;
	in.letterWidth = 0.35f;
	in.letterHeight = 1.3f;
	in.lineWidth = 0.0f;
	in.lineHeight = 260.0f;
	in.centered = true;
	in.box = true;
	in.outline = 1;

	const std::string text = "{E2C063}欢迎来到 {FFFFFF}测试服\n{36682C}第二行内容";
	const LayoutResult res = layoutText(text, 0xFFFFFFu, in, makeYaHeiRef(), mockYaHeiAdvance);

	CHECK(res.metrics.valid);
	CHECK_EQ(res.lines.size(), 2);
	CHECK(res.hasBox);

	const float expectedHalfW = (260.0f * (1920.0f / 640.0f)) * 0.5f + 4.0f; // 390 + 4 = 394
	CHECK_NEAR(res.boxRect.left, 960.0f - expectedHalfW, 1e-3);
	CHECK_NEAR(res.boxRect.right, 960.0f + expectedHalfW, 1e-3);

	const float expectedBoxH = 2.0f * res.metrics.lineHeight + 8.0f;
	CHECK_NEAR(res.boxRect.bottom - res.boxRect.top, expectedBoxH, 1e-3);
	// Previously boxH was 260 * 3 = 780 px! Now it is ~64.4 px.
	CHECK(expectedBoxH < 80.0f);

	// Verify inline color carries cleanly and color tags have zero width.
	CHECK_EQ(res.lines[0].glyphs[0].colorRgb, 0xE2C063u);
	CHECK_EQ(res.lines[1].glyphs[0].colorRgb, 0x36682Cu);
}

static void testLeftAlignedAutoWrapAndColorCarry()
{
	// Left-aligned box from x=100 to lineWidth=200 (100 virtual px = 300 screen px on 1920w).
	// At letterWidth=0.25 -> scaleX=0.75 -> CJK char advance = 32 * 0.75 = 24 px.
	// floor(300 / 24) = 12 CJK chars per line. 25 CJK chars must wrap into 3 lines (12 + 12 + 1).
	LayoutInput in{};
	in.screenWidth = 1920.0f;
	in.screenHeight = 1080.0f;
	in.x = 100.0f;
	in.y = 120.0f;
	in.letterWidth = 0.25f;
	in.letterHeight = 1.0f;
	in.lineWidth = 200.0f;
	in.lineHeight = 1280.0f; // Default un-set Y should NEVER affect box height
	in.box = true;

	std::string text = "{FF2233}";
	for (int i = 0; i < 25; ++i) {
		text += "\xE4\xB8\xAD"; // UTF-8 '中'
	}

	const LayoutResult res = layoutText(text, 0xFFFFFFu, in, makeYaHeiRef(), mockYaHeiAdvance);
	CHECK_EQ(res.lines.size(), 3);
	CHECK_EQ(res.lines[0].glyphs.size(), 12);
	CHECK_EQ(res.lines[1].glyphs.size(), 12);
	CHECK_EQ(res.lines[2].glyphs.size(), 1);

	// Color carries across auto-wrapped lines.
	CHECK_EQ(res.lines[0].glyphs[0].colorRgb, 0xFF2233u);
	CHECK_EQ(res.lines[1].glyphs[0].colorRgb, 0xFF2233u);
	CHECK_EQ(res.lines[2].glyphs[0].colorRgb, 0xFF2233u);

	// Box spans [posX - 4, wrapX + 4] horizontally and 3 * lineHeight + 8 vertically.
	CHECK_NEAR(res.boxRect.left, 300.0f - 4.0f, 1e-3);
	CHECK_NEAR(res.boxRect.right, 600.0f + 4.0f, 1e-3);
	CHECK_NEAR(res.boxRect.bottom - res.boxRect.top, 3.0f * res.metrics.lineHeight + 8.0f, 1e-3);
}

static void testMixedAsciiWordWrapAndNewlines()
{
	LayoutInput in{};
	in.screenWidth = 640.0f;
	in.screenHeight = 448.0f;
	in.x = 100.0f;
	in.y = 50.0f;
	in.letterWidth = 0.25f; // scaleX=0.25, CJK=8px, ASCII upper=~5.63px, space=~2.37px
	in.letterHeight = 1.0f;
	in.lineWidth = 130.0f; // maxLineWidth = 30px
	in.box = false;

	// "中文 ABCDE 中文" -> line 1: "中文" (16px; adding " " + "ABCDE" = 16+2.37+28.15 > 30),
	// line 2: "ABCDE" (28.15px, kept whole as an ASCII word!), line 3: "中文" (16px).
	const std::string text = "\xE4\xB8\xAD\xE6\x96\x87 ABCDE \xE4\xB8\xAD\xE6\x96\x87";
	const LayoutResult res = layoutText(text, 0xFFFFFFu, in, makeYaHeiRef(), mockYaHeiAdvance);
	CHECK_EQ(res.lines.size(), 3);
	// Trailing space trimmed on line 0 and line 1.
	CHECK_EQ(res.lines[0].glyphs.size(), 2);
	CHECK_EQ(res.lines[1].glyphs.size(), 5);
	CHECK_EQ(res.lines[2].glyphs.size(), 2);

	// Explicit empty middle line and trailing newline match CFont::ProcessCurrentString.
	const LayoutResult nlRes = layoutText("\xE4\xB8\xAD\n\n\xE6\x96\x87\n", 0xFFFFFFu, in, makeYaHeiRef(), mockYaHeiAdvance);
	CHECK_EQ(nlRes.lines.size(), 3);
	CHECK_EQ(nlRes.lines[0].glyphs.size(), 1);
	CHECK_EQ(nlRes.lines[1].glyphs.size(), 0);
	CHECK_EQ(nlRes.lines[2].glyphs.size(), 1);
}

static void testNonProportionalAndOutlineSpacing()
{
	LayoutInput in{};
	in.screenWidth = 640.0f;
	in.screenHeight = 448.0f;
	in.x = 0.0f;
	in.y = 0.0f;
	in.letterWidth = 0.5f; // scaleX = 0.5
	in.letterHeight = 2.0f;
	in.style = 1;
	in.proportional = false;
	in.outline = 2;

	const auto ref = makeYaHeiRef();
	const auto m = computeMetrics(in, ref);
	float natW = 0.0f;

	// Non-proportional ASCII uses Font 1 UNPROP (20) + outline (2) = 22 * 0.5 = 11.0
	const float asciiAdv = computeGlyphAdvance('i', in, m, mockYaHeiAdvance('i'), natW);
	CHECK_NEAR(asciiAdv, (20.0f + 2.0f) * 0.5f, 1e-4);

	// CJK stays 32 + outline (2) = 34 * 0.5 = 17.0
	const float cjkAdv = computeGlyphAdvance(0x4E2D, in, m, mockYaHeiAdvance(0x4E2D), natW);
	CHECK_NEAR(cjkAdv, (32.0f + 2.0f) * 0.5f, 1e-4);

	// Outline offsets scale by screenW/640 and screenH/448
	CHECK_NEAR(m.dropOffsetX, 2.0f, 1e-4);
	CHECK_NEAR(m.dropOffsetY, 2.0f, 1e-4);
}

static void testMobileTouchHaloAndClosestRowDiscrimination()
{
	// Simulate Medieval RP kit.pwn tiny selectable rows on 1920x1080 (KIT_ROW=10 -> ~24px tall):
	// Row 10: y = [300..324], Row 11: y = [330..354] (6px gap), Isolated Close [X] id 12: [900..930] x [120..146]
	const HitBox boxes[] = {
		makeNormalizedHitBox(10, 400.0f, 300.0f, 760.0f, 324.0f),
		makeNormalizedHitBox(11, 400.0f, 330.0f, 760.0f, 354.0f),
		// Deliberately pass inverted coordinates (x0 > x1, y0 > y1) to verify normalization:
		makeNormalizedHitBox(12, 930.0f, 146.0f, 900.0f, 120.0f),
		// Zero-height dummy TextDraw should never steal halo clicks:
		makeNormalizedHitBox(13, 400.0f, 290.0f, 760.0f, 290.0f),
	};
	const TouchSelectRadii radii = computeTouchSelectRadii(1920.0f, 1080.0f);

	// 1. Exact hits always win
	CHECK_EQ(findBestSelectableHit(boxes, 4, 500.0f, 312.0f, radii.haloRadiusPx), 10);
	CHECK_EQ(findBestSelectableHit(boxes, 4, 500.0f, 340.0f, radii.haloRadiusPx), 11);
	CHECK_EQ(findBestSelectableHit(boxes, 4, 915.0f, 133.0f, radii.haloRadiusPx), 12);

	// 2. Narrow miss above Row 10 (y = 286, 14px above Row 10; dummy id 13 at y=290 is ignored!)
	CHECK_EQ(findBestSelectableHit(boxes, 4, 500.0f, 286.0f, radii.haloRadiusPx), 10);

	// 3. In the 6px gap between Row 10 (maxY=324) and Row 11 (minY=330), closest edge wins cleanly
	CHECK_EQ(findBestSelectableHit(boxes, 4, 500.0f, 325.5f, radii.haloRadiusPx), 10);
	CHECK_EQ(findBestSelectableHit(boxes, 4, 500.0f, 328.5f, radii.haloRadiusPx), 11);

	// 4. Far miss outside halo returns -1
	CHECK_EQ(findBestSelectableHit(boxes, 4, 500.0f, 240.0f, radii.haloRadiusPx), -1);
}

static void testTouchTrackerLiftOffRollSlideAndMultiTouch()
{
	const HitBox boxes[] = {
		makeNormalizedHitBox(20, 400.0f, 300.0f, 760.0f, 324.0f),
		makeNormalizedHitBox(21, 400.0f, 328.0f, 760.0f, 352.0f),
	};
	const TouchSelectRadii radii = computeTouchSelectRadii(1920.0f, 1080.0f);
	TouchSelectTracker tracker;

	// Case A: Press Row 20, thumb rolls 18px upward into empty margin on lift-off -> STILL clicks Row 20!
	auto downA = tracker.onTouch(2, 0, 520.0f, 310.0f, boxes, 2, radii);
	CHECK_EQ(downA.hoveredId, 20);
	CHECK_EQ(downA.clickedId, -1);

	auto upA = tracker.onTouch(1, 0, 522.0f, 282.0f, boxes, 2, radii);
	CHECK_EQ(upA.hoveredId, -1);
	CHECK_EQ(upA.clickedId, 20);

	// Case B: Stationary tap on Row 20 (y=318) whose lift-off rolls 11px down (y=329, 1px inside Row 21,
	// total finger movement < tapLockDragPx) -> keeps Row 20 locked instead of mis-clicking Row 21!
	auto downB = tracker.onTouch(2, 0, 520.0f, 318.0f, boxes, 2, radii);
	CHECK_EQ(downB.hoveredId, 20);
	auto upB = tracker.onTouch(1, 0, 520.0f, 329.0f, boxes, 2, radii);
	CHECK_EQ(upB.clickedId, 20);

	// Case C: Secondary finger (pointerId=1) touching empty screen never hijacks primary finger's hover/click
	CHECK_EQ(tracker.onTouch(2, 0, 520.0f, 312.0f, boxes, 2, radii).hoveredId, 20);
	CHECK_EQ(tracker.onTouch(2, 1, 100.0f, 100.0f, boxes, 2, radii).hoveredId, 20);
	CHECK_EQ(tracker.onTouch(1, 1, 100.0f, 100.0f, boxes, 2, radii).clickedId, -1);
	CHECK_EQ(tracker.onTouch(1, 0, 520.0f, 312.0f, boxes, 2, radii).clickedId, 20);

	// Case D: Intentional slide across menu (30px > tapLockDragPx) switches target to Row 21;
	// intentional drag far into empty space (> retainMaxDragPx) cancels cleanly!
	CHECK_EQ(tracker.onTouch(2, 0, 520.0f, 310.0f, boxes, 2, radii).hoveredId, 20);
	CHECK_EQ(tracker.onTouch(3, 0, 520.0f, 342.0f, boxes, 2, radii).hoveredId, 21);
	CHECK_EQ(tracker.onTouch(1, 0, 520.0f, 342.0f, boxes, 2, radii).clickedId, 21);

	CHECK_EQ(tracker.onTouch(2, 0, 520.0f, 310.0f, boxes, 2, radii).hoveredId, 20);
	CHECK_EQ(tracker.onTouch(3, 0, 520.0f, 200.0f, boxes, 2, radii).hoveredId, -1);
	CHECK_EQ(tracker.onTouch(1, 0, 520.0f, 200.0f, boxes, 2, radii).clickedId, -1);
}

int main()
{
	RUN(testScaleAndLineHeightParity);
	RUN(testIndependentLetterWidthAspectRatio);
	RUN(testCenteredBoxUsesNumLinesNotLineHeight);
	RUN(testLeftAlignedAutoWrapAndColorCarry);
	RUN(testMixedAsciiWordWrapAndNewlines);
	RUN(testNonProportionalAndOutlineSpacing);
	RUN(testMobileTouchHaloAndClosestRowDiscrimination);
	RUN(testTouchTrackerLiftOffRollSlideAndMultiTouch);
	return check::finish();
}
