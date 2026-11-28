#include "TouchHud.h"
#include "TouchHudTheme.h"

#include "../ImGuiRenderer.h"
#include "../../L10n.h"

#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace TouchHud {

namespace {

constexpr float kPi = 3.14159265358979323846f;

void drawCenteredLabel(ImDrawList* dl, ImFont* font, float fontSize,
	const ImVec2& center, const char* text, ImU32 color, ImU32 shadowCol)
{
	if (!dl || !font || !text || !text[0]) return;
	const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
	const ImVec2 pos(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f);
	dl->AddText(font, fontSize, ImVec2(pos.x + 1.2f, pos.y + 1.4f), shadowCol, text);
	dl->AddText(font, fontSize, pos, color, text);
}

// Corner keycap mini-badge (e.g., 'F', 'G', 'W', 'S') in bottom-right of medallion
void drawCornerBadge(ImDrawList* dl, ImFont* font, const ImVec2& center, float r,
	const char* keyTxt, ImU32 textCol, ImU32 shadowCol)
{
	if (!keyTxt || !keyTxt[0]) return;
	const ImVec2 pos(center.x, center.y + r * 0.62f);
	drawCenteredLabel(dl, font, std::max(11.0f, r * 0.36f), pos, keyTxt, textCol, shadowCol);
}

enum class MedallionStyle : uint8_t {
	Standard = 0,
	Fire,
	Gas,
	Brake,
	Custom,
};

// GTA:SA + Launcher unified obsidian glass medallion with specular top arc & dynamic theme accent
void drawMedallion(ImDrawList* dl, const touch_hud::ScreenCircle& c, float alpha,
	bool pressed, bool activeAccent, MedallionStyle style, const LauncherPalette& pal)
{
	const ImVec2 center(c.cx, c.cy);
	const float r = c.r;

	// Soft two-step outer drop shadow for legibility over bright sand & dark streets
	dl->AddCircleFilled(ImVec2(center.x, center.y + 2.4f), r + 2.2f, rgba(0, 0, 0, alpha * 0.28f), 36);
	dl->AddCircleFilled(ImVec2(center.x, center.y + 1.2f), r + 0.8f, rgba(0, 0, 0, alpha * 0.36f), 36);

	ImU32 fillCol = rgba(LauncherPalette::kGlassDark, alpha * 0.60f);
	ImU32 ringCol = rgba(LauncherPalette::kTextPrimary, alpha * 0.52f);
	float ringThick = 1.65f;

	if (style == MedallionStyle::Fire) {
		if (pressed) {
			fillCol = rgba(pal.accent, alpha * 0.90f);
			ringCol = rgba(pal.accentBright, alpha);
			ringThick = 2.8f;
		}
		else {
			fillCol = rgba(pal.accentDeep, alpha * 0.72f);
			ringCol = rgba(pal.accent, alpha * 0.88f);
			ringThick = 2.2f;
		}
	}
	else if (style == MedallionStyle::Gas) {
		if (pressed) {
			fillCol = rgba(LauncherPalette::kGood, alpha * 0.78f);
			ringCol = rgba(LauncherPalette::kTextPrimary, alpha * 0.96f);
			ringThick = 2.6f;
		}
		else {
			fillCol = rgba(LauncherPalette::kGlassDark, alpha * 0.64f);
			ringCol = rgba(LauncherPalette::kGood, alpha * 0.72f);
			ringThick = 1.9f;
		}
	}
	else if (style == MedallionStyle::Brake) {
		if (pressed) {
			fillCol = rgba(LauncherPalette::kDanger, alpha * 0.80f);
			ringCol = rgba(LauncherPalette::kTextPrimary, alpha * 0.96f);
			ringThick = 2.6f;
		}
		else {
			fillCol = rgba(LauncherPalette::kGlassDark, alpha * 0.64f);
			ringCol = rgba(LauncherPalette::kDanger, alpha * 0.72f);
			ringThick = 1.9f;
		}
	}
	else if (activeAccent) {
		fillCol = rgba(pal.accent, alpha * (pressed ? 0.92f : 0.80f));
		ringCol = rgba(pal.accentBright, alpha);
		ringThick = 2.6f;
	}
	else if (pressed) {
		fillCol = rgba(pal.accentDeep, alpha * 0.88f);
		ringCol = rgba(pal.accent, alpha);
		ringThick = 2.6f;
	}
	else if (style == MedallionStyle::Custom) {
		fillCol = rgba(LauncherPalette::kSurface, alpha * 0.68f);
		ringCol = rgba(pal.accent, alpha * 0.72f);
		ringThick = 1.85f;
	}

	// Main medallion body
	dl->AddCircleFilled(center, r, fillCol, 36);

	// Subtle inner dark vignette ring for crisp GTA HUD depth
	if (r > 18.0f) {
		dl->AddCircle(center, r * 0.88f, rgba(0, 0, 0, alpha * 0.24f), 32, 1.2f);
		// Top specular glass highlight arc
		dl->PathClear();
		dl->PathArcTo(center, r * 0.86f, -kPi * 0.78f, -kPi * 0.22f, 16);
		dl->PathStroke(rgba(255, 255, 255, alpha * (pressed ? 0.26f : 0.16f)), false, 1.4f);
	}

	// Crisp outer rim
	dl->AddCircle(center, r, ringCol, 36, ringThick);
}

// ---- GTA:SA-Inspired Iconic Vector Silhouettes ----
// Each icon is rendered with a subtle drop shadow pass (+1.2, +1.4) so it reads cleanly over any background.

void drawFistShape(ImDrawList* dl, const ImVec2& c, float r, ImU32 col, ImU32 cutout)
{
	// Classic GTA San Andreas clenched punching fist silhouette
	const float s = r * 0.56f;
	// Palm & wrist base
	dl->AddRectFilled(
		ImVec2(c.x - s * 0.52f, c.y - s * 0.08f),
		ImVec2(c.x + s * 0.62f, c.y + s * 0.62f), col, s * 0.22f);
	// Wrist cuff
	dl->AddRectFilled(
		ImVec2(c.x - s * 0.34f, c.y + s * 0.46f),
		ImVec2(c.x + s * 0.44f, c.y + s * 0.84f), col, s * 0.14f);
	// Four curled knuckles across top
	const float kw = s * 0.26f;
	const float kx0 = c.x - s * 0.50f;
	for (int i = 0; i < 4; ++i) {
		const float x0 = kx0 + i * (kw + s * 0.025f);
		const float topBoost = (i == 1 || i == 2) ? s * 0.06f : 0.0f;
		dl->AddRectFilled(
			ImVec2(x0, c.y - s * 0.68f - topBoost),
			ImVec2(x0 + kw, c.y + s * 0.04f), col, kw * 0.44f);
	}
	// Tucked thumb wrapping across lower-left knuckles
	ImVec2 thumb[4] = {
		{c.x - s * 0.68f, c.y + s * 0.06f},
		{c.x - s * 0.06f, c.y - s * 0.14f},
		{c.x + s * 0.04f, c.y + s * 0.18f},
		{c.x - s * 0.54f, c.y + s * 0.36f},
	};
	dl->AddConvexPolyFilled(thumb, 4, col);
	// Subtle crease separating thumb from palm/knuckles
	if (cutout != 0) {
		dl->AddLine(ImVec2(c.x - s * 0.44f, c.y + s * 0.02f), ImVec2(c.x + s * 0.54f, c.y - s * 0.06f), cutout, std::max(1.2f, r * 0.04f));
		dl->AddLine(ImVec2(c.x - s * 0.42f, c.y + s * 0.32f), ImVec2(c.x + s * 0.02f, c.y + s * 0.18f), cutout, std::max(1.2f, r * 0.038f));
	}
}

void drawFistGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	drawFistShape(dl, ImVec2(c.x + 1.3f, c.y + 1.5f), r, shadow, 0);
	drawFistShape(dl, c, r, fg, shadow);
}

void drawBulletShape(ImDrawList* dl, const ImVec2& c, float r, ImU32 col, ImU32 cutout)
{
	// Angled 45-deg tactical rifle cartridge (classic shooter fire icon)
	const float s = r * 0.58f;
	const float ang = -0.785398f; // -45 deg pointing up-right
	const float ca = std::cos(ang), sa = std::sin(ang);
	auto rot = [&](float lx, float ly) -> ImVec2 {
		return ImVec2(c.x + lx * ca - ly * sa, c.y + lx * sa + ly * ca);
	};
	// Main casing + ogive tip polygon
	ImVec2 body[7] = {
		rot(-s * 0.78f, -s * 0.26f),
		rot( s * 0.22f, -s * 0.26f),
		rot( s * 0.56f, -s * 0.18f),
		rot( s * 0.90f,  0.00f),
		rot( s * 0.56f,  s * 0.18f),
		rot( s * 0.22f,  s * 0.26f),
		rot(-s * 0.78f,  s * 0.26f),
	};
	dl->AddConvexPolyFilled(body, 7, col);
	// Base rim flange
	ImVec2 rim[4] = {
		rot(-s * 0.96f, -s * 0.30f),
		rot(-s * 0.84f, -s * 0.30f),
		rot(-s * 0.84f,  s * 0.30f),
		rot(-s * 0.96f,  s * 0.30f),
	};
	dl->AddConvexPolyFilled(rim, 4, col);
	// Subtle crimp line between bullet & casing
	if (cutout != 0) {
		dl->AddLine(rot(s * 0.18f, -s * 0.27f), rot(s * 0.18f, s * 0.27f), cutout, std::max(1.3f, r * 0.045f));
	}
}

void drawBulletGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool showDragArcs)
{
	drawBulletShape(dl, ImVec2(c.x + 1.3f, c.y + 1.5f), r, shadow, 0);
	drawBulletShape(dl, c, r, fg, shadow);

	if (showDragArcs) {
		// Subtle PUBG drag-aim micro-arcs at 4 corners
		const float ar = r * 0.74f;
		const float span = 0.22f;
		const float baseAngles[4] = {0.0f, kPi * 0.5f, kPi, kPi * 1.5f};
		for (float a : baseAngles) {
			dl->PathClear();
			dl->PathArcTo(c, ar, a - span, a + span, 6);
			dl->PathStroke(fg, false, std::max(1.4f, r * 0.042f));
		}
	}
}

// Dual staggered angled tactical bullets for Vehicle Fire (cohesive with on-foot fire icon)
void drawVehicleFireGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float subR = r * 0.78f;
	const float off = r * 0.21f;
	const ImVec2 p1(c.x - off, c.y - off * 0.85f);
	const ImVec2 p2(c.x + off, c.y + off * 0.85f);
	// Shadow pass
	drawBulletShape(dl, ImVec2(p1.x + 1.2f, p1.y + 1.4f), subR, shadow, 0);
	drawBulletShape(dl, ImVec2(p2.x + 1.2f, p2.y + 1.4f), subR, shadow, 0);
	// Foreground pass
	drawBulletShape(dl, p1, subR, fg, shadow);
	drawBulletShape(dl, p2, subR, fg, shadow);
}

void drawScopeGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		const float ir = r * 0.38f;
		const float o = r * 0.62f;
		const float gap = r * 0.18f;
		const float th = std::max(2.0f, r * 0.068f);
		dl->AddCircle(p, ir, col, 28, th);
		dl->AddCircleFilled(p, r * 0.085f, col, 14);
		dl->AddLine(ImVec2(p.x - o, p.y), ImVec2(p.x - gap, p.y), col, th);
		dl->AddLine(ImVec2(p.x + gap, p.y), ImVec2(p.x + o, p.y), col, th);
		dl->AddLine(ImVec2(p.x, p.y - o), ImVec2(p.x, p.y - gap), col, th);
		dl->AddLine(ImVec2(p.x, p.y + gap), ImVec2(p.x, p.y + o), col, th);
	};
	drawOne(ImVec2(c.x + 1.2f, c.y + 1.4f), shadow);
	drawOne(c, fg);
}

void drawShieldGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float w = r * 0.42f;
	const float h = r * 0.48f;
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		ImVec2 pts[6] = {
			{p.x,            p.y - h},
			{p.x + w,        p.y - h * 0.56f},
			{p.x + w * 0.84f, p.y + h * 0.28f},
			{p.x,            p.y + h},
			{p.x - w * 0.84f, p.y + h * 0.28f},
			{p.x - w,        p.y - h * 0.56f},
		};
		dl->AddConvexPolyFilled(pts, 6, col);
	};
	drawOne(ImVec2(c.x + 1.3f, c.y + 1.5f), shadow);
	drawOne(c, fg);
	// Inner vertical ridge line for solid GTA armor feel
	dl->AddLine(ImVec2(c.x, c.y - h * 0.62f), ImVec2(c.x, c.y + h * 0.62f), shadow, std::max(1.4f, r * 0.045f));
}

// Classic GTA:SA mobile running-man silhouette
void drawSprintManGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.58f;
	const float limb = std::max(2.2f, r * 0.105f);
	const float torso = std::max(2.6f, r * 0.135f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		// Head leaning forward
		dl->AddCircleFilled(ImVec2(p.x + s * 0.26f, p.y - s * 0.64f), s * 0.20f, col, 16);
		// Forward-pitched torso
		const ImVec2 neck(p.x + s * 0.12f, p.y - s * 0.36f);
		const ImVec2 hip(p.x - s * 0.14f, p.y + s * 0.14f);
		dl->AddLine(neck, hip, col, torso);
		// Front arm reaching forward
		const ImVec2 fElbow(p.x + s * 0.42f, p.y - s * 0.14f);
		const ImVec2 fHand(p.x + s * 0.68f, p.y - s * 0.28f);
		dl->AddLine(neck, fElbow, col, limb * 0.88f);
		dl->AddLine(fElbow, fHand, col, limb * 0.82f);
		// Back arm pumping behind
		const ImVec2 bElbow(p.x - s * 0.36f, p.y - s * 0.18f);
		const ImVec2 bHand(p.x - s * 0.56f, p.y + s * 0.04f);
		dl->AddLine(neck, bElbow, col, limb * 0.88f);
		dl->AddLine(bElbow, bHand, col, limb * 0.82f);
		// Front striding leg
		const ImVec2 fKnee(p.x + s * 0.30f, p.y + s * 0.32f);
		const ImVec2 fFoot(p.x + s * 0.16f, p.y + s * 0.78f);
		dl->AddLine(hip, fKnee, col, limb);
		dl->AddLine(fKnee, fFoot, col, limb * 0.92f);
		// Trailing back leg
		const ImVec2 bKnee(p.x - s * 0.46f, p.y + s * 0.48f);
		const ImVec2 bFoot(p.x - s * 0.74f, p.y + s * 0.66f);
		dl->AddLine(hip, bKnee, col, limb);
		dl->AddLine(bKnee, bFoot, col, limb * 0.92f);
		// Subtle speed dashes behind runner
		const float dashW = std::max(1.3f, r * 0.042f);
		dl->AddLine(ImVec2(p.x - s * 0.86f, p.y - s * 0.26f), ImVec2(p.x - s * 0.54f, p.y - s * 0.26f), col, dashW);
		dl->AddLine(ImVec2(p.x - s * 0.92f, p.y + s * 0.08f), ImVec2(p.x - s * 0.66f, p.y + s * 0.08f), col, dashW);
	};
	drawOne(ImVec2(c.x + 1.2f, c.y + 1.4f), shadow);
	drawOne(c, fg);
}

// Classic GTA:SA leaping/vaulting figure silhouette
void drawJumpFigureGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.56f;
	const float limb = std::max(2.1f, r * 0.10f);
	const float torso = std::max(2.6f, r * 0.13f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		// Head high
		dl->AddCircleFilled(ImVec2(p.x + s * 0.14f, p.y - s * 0.72f), s * 0.19f, col, 16);
		// Torso arching upward
		const ImVec2 neck(p.x + s * 0.06f, p.y - s * 0.44f);
		const ImVec2 hip(p.x - s * 0.12f, p.y + s * 0.04f);
		dl->AddLine(neck, hip, col, torso);
		// Raised front arm + back balance arm
		dl->AddLine(neck, ImVec2(p.x + s * 0.50f, p.y - s * 0.66f), col, limb * 0.88f);
		dl->AddLine(neck, ImVec2(p.x - s * 0.44f, p.y - s * 0.34f), col, limb * 0.85f);
		// Tucked front knee (vaulting over hurdle)
		const ImVec2 fKnee(p.x + s * 0.36f, p.y + s * 0.04f);
		const ImVec2 fFoot(p.x + s * 0.22f, p.y + s * 0.44f);
		dl->AddLine(hip, fKnee, col, limb);
		dl->AddLine(fKnee, fFoot, col, limb * 0.92f);
		// Trailing leg extended back
		const ImVec2 bKnee(p.x - s * 0.48f, p.y + s * 0.28f);
		const ImVec2 bFoot(p.x - s * 0.72f, p.y + s * 0.14f);
		dl->AddLine(hip, bKnee, col, limb);
		dl->AddLine(bKnee, bFoot, col, limb * 0.92f);
		// Ground / hurdle bar underneath
		dl->AddLine(ImVec2(p.x - s * 0.64f, p.y + s * 0.76f), ImVec2(p.x + s * 0.64f, p.y + s * 0.76f), col, std::max(1.8f, r * 0.058f));
	};
	drawOne(ImVec2(c.x + 1.2f, c.y + 1.4f), shadow);
	drawOne(c, fg);
}

// Classic GTA:SA crouching/kneeling figure silhouette
void drawCrouchFigureGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.56f;
	const float limb = std::max(2.1f, r * 0.105f);
	const float torso = std::max(2.6f, r * 0.135f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		// Low head
		dl->AddCircleFilled(ImVec2(p.x + s * 0.16f, p.y - s * 0.34f), s * 0.19f, col, 16);
		// Compact hunched torso
		const ImVec2 neck(p.x + s * 0.06f, p.y - s * 0.08f);
		const ImVec2 hip(p.x - s * 0.22f, p.y + s * 0.26f);
		dl->AddLine(neck, hip, col, torso);
		// Forward brace arm
		dl->AddLine(neck, ImVec2(p.x + s * 0.48f, p.y + s * 0.10f), col, limb * 0.85f);
		// Front bent leg planted
		const ImVec2 fKnee(p.x + s * 0.26f, p.y + s * 0.30f);
		const ImVec2 fFoot(p.x + s * 0.24f, p.y + s * 0.68f);
		dl->AddLine(hip, fKnee, col, limb);
		dl->AddLine(fKnee, fFoot, col, limb * 0.92f);
		// Back knee kneeling on ground
		const ImVec2 bKnee(p.x - s * 0.18f, p.y + s * 0.64f);
		const ImVec2 bFoot(p.x - s * 0.56f, p.y + s * 0.66f);
		dl->AddLine(hip, bKnee, col, limb);
		dl->AddLine(bKnee, bFoot, col, limb * 0.92f);
		// Subtle downward press chevron above head
		const float chW = s * 0.26f;
		ImVec2 ch[3] = {
			{p.x - chW, p.y - s * 0.78f},
			{p.x,       p.y - s * 0.58f},
			{p.x + chW, p.y - s * 0.78f},
		};
		dl->AddPolyline(ch, 3, col, false, std::max(1.6f, r * 0.052f));
	};
	drawOne(ImVec2(c.x + 1.2f, c.y + 1.4f), shadow);
	drawOne(c, fg);
}

// Iconic GTA side-profile car + enter/exit arrow silhouette
void drawCarGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool isExit)
{
	const float s = r * 0.58f;
	const ImVec2 shift(c.x, c.y - r * 0.12f);
	auto drawOne = [&](const ImVec2& p, ImU32 col, ImU32 winCol) {
		// Lower car body
		dl->AddRectFilled(
			ImVec2(p.x - s * 0.84f, p.y - s * 0.04f),
			ImVec2(p.x + s * 0.84f, p.y + s * 0.34f), col, s * 0.12f);
		// Cabin greenhouse
		ImVec2 cab[4] = {
			{p.x - s * 0.46f, p.y - s * 0.44f},
			{p.x + s * 0.32f, p.y - s * 0.44f},
			{p.x + s * 0.58f, p.y - s * 0.02f},
			{p.x - s * 0.62f, p.y - s * 0.02f},
		};
		dl->AddConvexPolyFilled(cab, 4, col);
		// Window cutout
		if (winCol != 0) {
			ImVec2 win[4] = {
				{p.x - s * 0.36f, p.y - s * 0.34f},
				{p.x + s * 0.24f, p.y - s * 0.34f},
				{p.x + s * 0.42f, p.y - s * 0.08f},
				{p.x - s * 0.46f, p.y - s * 0.08f},
			};
			dl->AddConvexPolyFilled(win, 4, winCol);
		}
		// Wheels
		dl->AddCircleFilled(ImVec2(p.x - s * 0.48f, p.y + s * 0.34f), s * 0.20f, col, 16);
		dl->AddCircleFilled(ImVec2(p.x + s * 0.48f, p.y + s * 0.34f), s * 0.20f, col, 16);
		if (winCol != 0) {
			dl->AddCircleFilled(ImVec2(p.x - s * 0.48f, p.y + s * 0.34f), s * 0.085f, winCol, 12);
			dl->AddCircleFilled(ImVec2(p.x + s * 0.48f, p.y + s * 0.34f), s * 0.085f, winCol, 12);
		}
		// Exit arrow pointing up-out above roof
		if (isExit) {
			const float ay = p.y - s * 0.66f;
			dl->AddLine(ImVec2(p.x - s * 0.14f, p.y - s * 0.30f), ImVec2(p.x + s * 0.28f, ay), col, std::max(1.8f, r * 0.06f));
			dl->AddTriangleFilled(
				ImVec2(p.x + s * 0.42f, ay - s * 0.14f),
				ImVec2(p.x + s * 0.10f, ay - s * 0.10f),
				ImVec2(p.x + s * 0.36f, ay + s * 0.16f), col);
		}
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow, 0);
	drawOne(shift, fg, shadow);
}

// Passenger seat + seated figure silhouette (for G key)
void drawPassengerSeatGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.54f;
	const ImVec2 shift(c.x, c.y - r * 0.10f);
	const float th = std::max(2.2f, r * 0.095f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		// Seat backrest + cushion (L-shape bucket seat)
		ImVec2 seat[3] = {
			{p.x - s * 0.54f, p.y - s * 0.52f},
			{p.x - s * 0.28f, p.y + s * 0.42f},
			{p.x + s * 0.52f, p.y + s * 0.42f},
		};
		dl->AddPolyline(seat, 3, col, false, th * 1.1f);
		// Seated passenger head + torso + legs
		dl->AddCircleFilled(ImVec2(p.x - s * 0.04f, p.y - s * 0.48f), s * 0.21f, col, 16);
		dl->AddLine(ImVec2(p.x - s * 0.06f, p.y - s * 0.20f), ImVec2(p.x + s * 0.04f, p.y + s * 0.18f), col, th * 1.15f);
		dl->AddLine(ImVec2(p.x + s * 0.04f, p.y + s * 0.18f), ImVec2(p.x + s * 0.56f, p.y + s * 0.18f), col, th);
		dl->AddLine(ImVec2(p.x + s * 0.56f, p.y + s * 0.18f), ImVec2(p.x + s * 0.62f, p.y + s * 0.54f), col, th * 0.92f);
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow);
	drawOne(shift, fg);
}

// Automotive accelerator (tall narrow) or brake (wide) pedal with lever arm & dark tread grooves
void drawPedalGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool isGas)
{
	const float s = r * 0.56f;
	const ImVec2 shift(c.x, c.y + r * 0.04f);
	const float armW = std::max(2.2f, r * 0.095f);
	auto drawOne = [&](const ImVec2& p, ImU32 col, ImU32 grooveCol) {
		if (isGas) {
			// Subtle firewall lever arm connecting top of accelerator pedal
			dl->AddLine(ImVec2(p.x + s * 0.06f, p.y - s * 0.68f), ImVec2(p.x + s * 0.22f, p.y - s * 1.00f), col, armW);
			// Tall sculpted throttle pedal
			ImVec2 ped[4] = {
				{p.x - s * 0.28f, p.y - s * 0.70f},
				{p.x + s * 0.34f, p.y - s * 0.70f},
				{p.x + s * 0.28f, p.y + s * 0.76f},
				{p.x - s * 0.34f, p.y + s * 0.76f},
			};
			dl->AddConvexPolyFilled(ped, 4, col);
			if (grooveCol != 0) {
				const float gw = std::max(1.3f, r * 0.044f);
				for (int i = -2; i <= 2; ++i) {
					const float gy = p.y + s * 0.04f + i * s * 0.25f;
					dl->AddLine(ImVec2(p.x - s * 0.19f, gy), ImVec2(p.x + s * 0.21f, gy), grooveCol, gw);
				}
			}
		}
		else {
			// Sturdy top brake pedal arm
			dl->AddLine(ImVec2(p.x, p.y - s * 0.42f), ImVec2(p.x, p.y - s * 0.88f), col, armW * 1.15f);
			// Wide sturdy brake pedal pad
			dl->AddRectFilled(
				ImVec2(p.x - s * 0.62f, p.y - s * 0.40f),
				ImVec2(p.x + s * 0.62f, p.y + s * 0.50f), col, s * 0.16f);
			if (grooveCol != 0) {
				const float gw = std::max(1.4f, r * 0.048f);
				for (int i = -1; i <= 1; ++i) {
					const float gx = p.x + i * s * 0.30f;
					dl->AddLine(ImVec2(gx, p.y - s * 0.24f), ImVec2(gx, p.y + s * 0.34f), grooveCol, gw);
				}
			}
		}
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow, 0);
	drawOne(shift, fg, shadow);
}

// Mechanical rally handbrake pull-lever + sweeping drift tire skid marks (no parking '(P)' icon)
void drawHandbrakeDriftGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.56f;
	const ImVec2 shift(c.x, c.y - r * 0.12f);
	const float th = std::max(1.9f, r * 0.072f);
	auto drawOne = [&](const ImVec2& p, ImU32 col, ImU32 cutout) {
		// Pivot console base at lower-left
		dl->AddCircleFilled(ImVec2(p.x - s * 0.42f, p.y + s * 0.28f), s * 0.20f, col, 16);
		if (cutout != 0) {
			dl->AddCircleFilled(ImVec2(p.x - s * 0.42f, p.y + s * 0.28f), s * 0.075f, cutout, 12);
		}
		// Steel shaft rising diagonally up-right
		dl->AddLine(
			ImVec2(p.x - s * 0.34f, p.y + s * 0.20f),
			ImVec2(p.x + s * 0.06f, p.y - s * 0.16f), col, th * 1.15f);
		// Thick contoured handbrake grip handle angled ~40 deg
		ImVec2 grip[4] = {
			{p.x - s * 0.06f, p.y - s * 0.18f},
			{p.x + s * 0.52f, p.y - s * 0.70f},
			{p.x + s * 0.68f, p.y - s * 0.52f},
			{p.x + s * 0.10f, p.y + s * 0.00f},
		};
		dl->AddConvexPolyFilled(grip, 4, col);
		// Thumb release button nub on tip
		dl->AddCircleFilled(ImVec2(p.x + s * 0.68f, p.y - s * 0.68f), s * 0.095f, col, 12);
		// Pull-up motion micro-arc above handle
		dl->PathClear();
		dl->PathArcTo(ImVec2(p.x - s * 0.36f, p.y + s * 0.24f), s * 0.78f, -1.52f, -1.04f, 8);
		dl->PathStroke(col, false, std::max(1.4f, r * 0.048f));
		// Dynamic twin curved drift skid marks sweeping underneath
		const ImVec2 s1[3] = {
			{p.x - s * 0.74f, p.y + s * 0.60f},
			{p.x + s * 0.02f, p.y + s * 0.48f},
			{p.x + s * 0.76f, p.y + s * 0.56f},
		};
		const ImVec2 s2[3] = {
			{p.x - s * 0.56f, p.y + s * 0.78f},
			{p.x + s * 0.12f, p.y + s * 0.66f},
			{p.x + s * 0.80f, p.y + s * 0.72f},
		};
		dl->AddPolyline(s1, 3, col, false, std::max(1.5f, r * 0.052f));
		dl->AddPolyline(s2, 3, col, false, std::max(1.5f, r * 0.052f));
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow, 0);
	drawOne(shift, fg, shadow);
}

// Classic 3-spoke automotive steering wheel with central horn pad (distinct from audio volume icons)
void drawHornGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float rimR = r * 0.50f;
	const float rimTh = std::max(2.6f, r * 0.115f);
	const float spokeTh = std::max(2.1f, r * 0.090f);
	const ImVec2 shift(c.x, c.y + r * 0.06f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		// Outer steering wheel rim
		dl->AddCircle(p, rimR, col, 32, rimTh);
		// Central horn boss pad
		dl->AddCircleFilled(p, rimR * 0.34f, col, 20);
		// Three classic automotive spokes (left, right, bottom)
		dl->AddLine(ImVec2(p.x - rimR * 0.94f, p.y - rimR * 0.08f), ImVec2(p.x + rimR * 0.94f, p.y - rimR * 0.08f), col, spokeTh);
		dl->AddLine(ImVec2(p.x, p.y + rimR * 0.22f), ImVec2(p.x, p.y + rimR * 0.94f), col, spokeTh);
		// Subtle twin honk ticks above top rim (not side volume waves)
		const float tickTh = std::max(1.6f, r * 0.056f);
		const float aL = -kPi * 0.62f;
		const float aR = -kPi * 0.38f;
		dl->AddLine(
			ImVec2(p.x + std::cos(aL) * (rimR * 1.24f), p.y + std::sin(aL) * (rimR * 1.24f)),
			ImVec2(p.x + std::cos(aL) * (rimR * 1.54f), p.y + std::sin(aL) * (rimR * 1.54f)), col, tickTh);
		dl->AddLine(
			ImVec2(p.x + std::cos(aR) * (rimR * 1.24f), p.y + std::sin(aR) * (rimR * 1.24f)),
			ImVec2(p.x + std::cos(aR) * (rimR * 1.54f), p.y + std::sin(aR) * (rimR * 1.54f)), col, tickTh);
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow);
	drawOne(shift, fg);
}

// Sleek air-to-air missile silhouette (for Hydra / Hunter VehFire)
void drawMissileGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.56f;
	const ImVec2 shift(c.x, c.y - r * 0.10f);
	const float ang = -0.785398f; // pointing up-right
	const float ca = std::cos(ang), sa = std::sin(ang);
	auto rot = [&](const ImVec2& p, float lx, float ly) -> ImVec2 {
		return ImVec2(p.x + lx * ca - ly * sa, p.y + lx * sa + ly * ca);
	};
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		ImVec2 body[5] = {
			rot(p, -s * 0.68f, -s * 0.16f),
			rot(p,  s * 0.42f, -s * 0.16f),
			rot(p,  s * 0.88f,  0.00f),
			rot(p,  s * 0.42f,  s * 0.16f),
			rot(p, -s * 0.68f,  s * 0.16f),
		};
		dl->AddConvexPolyFilled(body, 5, col);
		// Tail stabilizer fins
		dl->AddTriangleFilled(rot(p, -s * 0.25f, -s * 0.14f), rot(p, -s * 0.72f, -s * 0.48f), rot(p, -s * 0.68f, -s * 0.14f), col);
		dl->AddTriangleFilled(rot(p, -s * 0.25f,  s * 0.14f), rot(p, -s * 0.72f,  s * 0.48f), rot(p, -s * 0.68f,  s * 0.14f), col);
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow);
	drawOne(shift, fg);
}

// Radiant white-flare countermeasure starburst (for Hydra VehFlare)
void drawFlareGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.54f;
	const ImVec2 shift(c.x, c.y - r * 0.12f);
	const float th = std::max(1.8f, r * 0.068f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		dl->AddCircleFilled(p, s * 0.26f, col, 16);
		for (int i = 0; i < 8; ++i) {
			const float a = static_cast<float>(i) * (kPi * 0.25f);
			const float inner = s * 0.34f;
			const float outer = (i % 2 == 0) ? (s * 0.80f) : (s * 0.58f);
			dl->AddLine(
				ImVec2(p.x + std::cos(a) * inner, p.y + std::sin(a) * inner),
				ImVec2(p.x + std::cos(a) * outer, p.y + std::sin(a) * outer),
				col, (i % 2 == 0) ? th : th * 0.78f);
		}
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow);
	drawOne(shift, fg);
}

// Hydra Harrier VTOL/Jet vector thruster silhouette
void drawHydraThrusterGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool jetForward)
{
	const float s = r * 0.56f;
	const ImVec2 shift(c.x, c.y - r * 0.14f);
	const float th = std::max(1.8f, r * 0.065f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		// Sleek fighter fuselage side profile facing right
		ImVec2 jet[5] = {
			{p.x - s * 0.64f, p.y - s * 0.26f},
			{p.x + s * 0.22f, p.y - s * 0.16f},
			{p.x + s * 0.80f, p.y + s * 0.04f},
			{p.x + s * 0.16f, p.y + s * 0.14f},
			{p.x - s * 0.58f, p.y + s * 0.14f},
		};
		dl->AddConvexPolyFilled(jet, 5, col);
		// Tail fin
		dl->AddTriangleFilled(
			ImVec2(p.x - s * 0.62f, p.y - s * 0.18f),
			ImVec2(p.x - s * 0.44f, p.y - s * 0.56f),
			ImVec2(p.x - s * 0.22f, p.y - s * 0.18f), col);

		if (jetForward) {
			// Rear horizontal afterburner exhaust lines (forward jet mode)
			dl->AddLine(ImVec2(p.x - s * 0.96f, p.y - s * 0.08f), ImVec2(p.x - s * 0.66f, p.y - s * 0.08f), col, th);
			dl->AddLine(ImVec2(p.x - s * 0.88f, p.y + s * 0.08f), ImVec2(p.x - s * 0.66f, p.y + s * 0.08f), col, th);
		}
		else {
			// Twin downward VTOL hover thrust jets
			dl->AddLine(ImVec2(p.x - s * 0.26f, p.y + s * 0.20f), ImVec2(p.x - s * 0.26f, p.y + s * 0.58f), col, th);
			dl->AddLine(ImVec2(p.x + s * 0.14f, p.y + s * 0.20f), ImVec2(p.x + s * 0.14f, p.y + s * 0.58f), col, th);
		}
	};
	drawOne(ImVec2(shift.x + 1.2f, shift.y + 1.4f), shadow);
	drawOne(shift, fg);
}

// Movie / viewpoint camera silhouette (for CamZoom & VehCamToggle)
void drawCameraGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow)
{
	const float s = r * 0.54f;
	auto drawOne = [&](const ImVec2& p, ImU32 col, ImU32 cutout) {
		// Camera body
		dl->AddRectFilled(
			ImVec2(p.x - s * 0.66f, p.y - s * 0.24f),
			ImVec2(p.x + s * 0.24f, p.y + s * 0.44f), col, s * 0.14f);
		// Lens cone on right
		ImVec2 lens[4] = {
			{p.x + s * 0.22f, p.y - s * 0.08f},
			{p.x + s * 0.72f, p.y - s * 0.32f},
			{p.x + s * 0.72f, p.y + s * 0.38f},
			{p.x + s * 0.22f, p.y + s * 0.24f},
		};
		dl->AddConvexPolyFilled(lens, 4, col);
		// Twin film reels on top
		dl->AddCircleFilled(ImVec2(p.x - s * 0.42f, p.y - s * 0.42f), s * 0.20f, col, 14);
		dl->AddCircleFilled(ImVec2(p.x + s * 0.00f, p.y - s * 0.42f), s * 0.18f, col, 14);
		// Subtle viewfinder dot inside camera body
		if (cutout != 0) {
			dl->AddCircleFilled(ImVec2(p.x - s * 0.22f, p.y + s * 0.10f), s * 0.12f, cutout, 12);
		}
	};
	drawOne(ImVec2(c.x + 1.2f, c.y + 1.4f), shadow, 0);
	drawOne(c, fg, shadow);
}

// Stock GTA:SA mobile style top-down car silhouette + directional side-look arrow (zero Q/E text)
void drawSideLookGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool lookLeft)
{
	const float s = r * 0.56f;
	const float dir = lookLeft ? -1.0f : 1.0f;
	auto drawOne = [&](const ImVec2& p, ImU32 col, ImU32 cutout) {
		// Top-down car body shifted slightly away from the arrow direction
		const float carX = p.x - dir * s * 0.32f;
		const float carHalfW = s * 0.30f;
		const float carHalfH = s * 0.66f;
		dl->AddRectFilled(
			ImVec2(carX - carHalfW, p.y - carHalfH),
			ImVec2(carX + carHalfW, p.y + carHalfH), col, s * 0.16f);
		// Front windshield + rear window cutouts so the top-down car silhouette reads unmistakably
		if (cutout != 0) {
			dl->AddRectFilled(
				ImVec2(carX - carHalfW * 0.66f, p.y - carHalfH * 0.48f),
				ImVec2(carX + carHalfW * 0.66f, p.y - carHalfH * 0.12f), cutout, s * 0.05f);
			dl->AddRectFilled(
				ImVec2(carX - carHalfW * 0.60f, p.y + carHalfH * 0.22f),
				ImVec2(carX + carHalfW * 0.60f, p.y + carHalfH * 0.46f), cutout, s * 0.05f);
		}
		// Bold directional look arrow pointing outward from the side window
		const float shaftStart = carX + dir * (carHalfW + s * 0.14f);
		const float shaftEnd = p.x + dir * s * 0.54f;
		const float tipX = p.x + dir * s * 0.90f;
		const float shaftTh = std::max(2.0f, r * 0.088f);
		dl->AddLine(ImVec2(shaftStart, p.y), ImVec2(shaftEnd, p.y), col, shaftTh);
		dl->AddTriangleFilled(
			ImVec2(tipX, p.y),
			ImVec2(shaftEnd - dir * s * 0.04f, p.y - s * 0.36f),
			ImVec2(shaftEnd - dir * s * 0.04f, p.y + s * 0.36f), col);
	};
	drawOne(ImVec2(c.x + 1.2f, c.y + 1.4f), shadow, 0);
	drawOne(c, fg, shadow);
}

// Crisp double-chevron cycle arrows (for on-foot Weapon Prev / Next without raw <Q/E> text)
void drawDoubleChevronGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool left)
{
	const float s = r * 0.44f;
	const float th = std::max(2.2f, r * 0.095f);
	const float dir = left ? -1.0f : 1.0f;
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		for (int k = 0; k < 2; ++k) {
			const float ox = (k == 0 ? -0.34f : 0.34f) * s;
			ImVec2 pts[3] = {
				{p.x + ox - dir * s * 0.26f, p.y - s * 0.62f},
				{p.x + ox + dir * s * 0.32f, p.y},
				{p.x + ox - dir * s * 0.26f, p.y + s * 0.62f},
			};
			dl->AddPolyline(pts, 3, col, false, th);
		}
	};
	drawOne(ImVec2(c.x + 1.1f, c.y + 1.3f), shadow);
	drawOne(c, fg);
}

// Scope zoom magnifying lens with '-' or '+' (for Q/E while in 1st-person Sniper/Camera)
void drawScopeZoomGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool zoomIn)
{
	const float s = r * 0.50f;
	const float th = std::max(1.8f, r * 0.068f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		const ImVec2 lc(p.x - s * 0.14f, p.y - s * 0.14f);
		const float lr = s * 0.56f;
		dl->AddCircle(lc, lr, col, 22, th);
		dl->AddLine(ImVec2(lc.x + lr * 0.72f, lc.y + lr * 0.72f), ImVec2(p.x + s * 0.78f, p.y + s * 0.78f), col, th * 1.25f);
		dl->AddLine(ImVec2(lc.x - lr * 0.46f, lc.y), ImVec2(lc.x + lr * 0.46f, lc.y), col, th);
		if (zoomIn) {
			dl->AddLine(ImVec2(lc.x, lc.y - lr * 0.46f), ImVec2(lc.x, lc.y + lr * 0.46f), col, th);
		}
	};
	drawOne(ImVec2(c.x + 1.1f, c.y + 1.3f), shadow);
	drawOne(c, fg);
}

// Curved horizontal yaw/rudder rotation arrow (for Aircraft Q/E horizontal rotation)
void drawYawRotateGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, bool ccw)
{
	const float arcR = r * 0.44f;
	const ImVec2 shift(c.x, c.y - r * 0.12f);
	const float th = std::max(2.0f, r * 0.080f);
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		dl->PathClear();
		if (ccw) {
			// Counter-clockwise top-left sweep ending with arrowhead pointing left-down
			dl->PathArcTo(p, arcR, -0.35f, -kPi + 0.55f, 14);
			dl->PathStroke(col, false, th);
			const float a = -kPi + 0.55f;
			const ImVec2 tip(p.x + std::cos(a) * arcR, p.y + std::sin(a) * arcR);
			dl->AddTriangleFilled(
				ImVec2(tip.x - r * 0.16f, tip.y + r * 0.16f),
				ImVec2(tip.x - r * 0.16f, tip.y - r * 0.18f),
				ImVec2(tip.x + r * 0.18f, tip.y + r * 0.04f), col);
		}
		else {
			// Clockwise top-right sweep ending with arrowhead pointing right-down
			dl->PathArcTo(p, arcR, -kPi + 0.35f, -0.55f, 14);
			dl->PathStroke(col, false, th);
			const float a = -0.55f;
			const ImVec2 tip(p.x + std::cos(a) * arcR, p.y + std::sin(a) * arcR);
			dl->AddTriangleFilled(
				ImVec2(tip.x + r * 0.16f, tip.y + r * 0.16f),
				ImVec2(tip.x + r * 0.16f, tip.y - r * 0.18f),
				ImVec2(tip.x - r * 0.18f, tip.y + r * 0.04f), col);
		}
	};
	drawOne(ImVec2(shift.x + 1.1f, shift.y + 1.3f), shadow);
	drawOne(shift, fg);
}

// Bold beveled directional chevron (for Vehicle Steer Left/Right & Aircraft Pitch Up/Down)
void drawChevronArrowGlyph(ImDrawList* dl, const ImVec2& c, float r, ImU32 fg, ImU32 shadow, int dir)
{
	// dir: 0=left, 1=right, 2=up, 3=down
	const float s = r * 0.44f;
	auto drawOne = [&](const ImVec2& p, ImU32 col) {
		if (dir == 0) {
			ImVec2 pts[3] = {{p.x - s * 0.85f, p.y}, {p.x + s * 0.55f, p.y - s * 0.82f}, {p.x + s * 0.55f, p.y + s * 0.82f}};
			dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
		}
		else if (dir == 1) {
			ImVec2 pts[3] = {{p.x + s * 0.85f, p.y}, {p.x - s * 0.55f, p.y - s * 0.82f}, {p.x - s * 0.55f, p.y + s * 0.82f}};
			dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
		}
		else if (dir == 2) {
			ImVec2 pts[3] = {{p.x, p.y - s * 0.85f}, {p.x - s * 0.82f, p.y + s * 0.55f}, {p.x + s * 0.82f, p.y + s * 0.55f}};
			dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
		}
		else {
			ImVec2 pts[3] = {{p.x, p.y + s * 0.85f}, {p.x - s * 0.82f, p.y - s * 0.55f}, {p.x + s * 0.82f, p.y - s * 0.55f}};
			dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
		}
	};
	drawOne(ImVec2(c.x + 1.3f, c.y + 1.5f), shadow);
	drawOne(c, fg);
}

void drawJoystick(ImDrawList* dl, ImFont* font, const touch_hud::ScreenCircle& base,
	const touch_hud::TouchHudCore::VisualState& vis, float alpha, bool showSprintLock, const LauncherPalette& pal)
{
	const ImVec2 anchor = vis.joystickActive
		? ImVec2(vis.joystickAnchorX, vis.joystickAnchorY)
		: ImVec2(base.cx, base.cy);
	const ImVec2 knob = vis.joystickActive
		? ImVec2(vis.joystickKnobX, vis.joystickKnobY)
		: anchor;
	const float r = base.r;

	// Outer dark obsidian glass pad
	dl->AddCircleFilled(anchor, r, rgba(LauncherPalette::kGlassDark, alpha * (vis.joystickActive ? 0.48f : 0.32f)), 48);
	dl->AddCircle(anchor, r,
		vis.joystickActive ? rgba(pal.accent, alpha * 0.78f) : rgba(LauncherPalette::kTextPrimary, alpha * 0.38f),
		48, vis.joystickActive ? 2.2f : 1.7f);
	dl->AddCircle(anchor, r * 0.52f, rgba(255, 255, 255, alpha * 0.09f), 36, 1.0f);

	// 4 crisp directional cardinal triangles/ticks
	const float tickIn = r * 0.80f;
	const float tickOut = r * 0.94f;
	const ImU32 tickCol = vis.joystickActive ? rgba(pal.accentBright, alpha * 0.62f) : rgba(LauncherPalette::kTextPrimary, alpha * 0.36f);
	dl->AddLine(ImVec2(anchor.x, anchor.y - tickOut), ImVec2(anchor.x, anchor.y - tickIn), tickCol, 1.9f);
	dl->AddLine(ImVec2(anchor.x, anchor.y + tickIn),  ImVec2(anchor.x, anchor.y + tickOut), tickCol, 1.9f);
	dl->AddLine(ImVec2(anchor.x - tickOut, anchor.y), ImVec2(anchor.x - tickIn, anchor.y), tickCol, 1.9f);
	dl->AddLine(ImVec2(anchor.x + tickIn, anchor.y),  ImVec2(anchor.x + tickOut, anchor.y), tickCol, 1.9f);

	// PUBG auto-sprint lock badge above joystick with running-man silhouette
	if (showSprintLock && (vis.joystickActive || vis.sprintLocked)) {
		const ImVec2 lockPos(anchor.x, anchor.y - r * 1.32f);
		const float lr = r * 0.27f;
		const ImU32 lFill = vis.sprintLocked
			? rgba(pal.accent, alpha * 0.92f)
			: rgba(LauncherPalette::kGlassDark, alpha * 0.62f);
		const ImU32 lRing = vis.sprintLocked
			? rgba(pal.accentBright, alpha)
			: rgba(LauncherPalette::kTextPrimary, alpha * 0.54f);
		const ImU32 lIcon = vis.sprintLocked
			? rgba(pal.onAccent, alpha)
			: rgba(LauncherPalette::kTextPrimary, alpha * 0.92f);
		dl->AddCircleFilled(lockPos, lr, lFill, 24);
		dl->AddCircle(lockPos, lr, lRing, 24, vis.sprintLocked ? 2.2f : 1.5f);
		drawSprintManGlyph(dl, lockPos, lr, lIcon, rgba(0, 0, 0, alpha * 0.45f));
		if (vis.sprintLocked) {
			drawCenteredLabel(dl, font, std::max(11.0f, lr * 0.66f), ImVec2(lockPos.x, lockPos.y - lr - 9.0f),
				"LOCK", rgba(pal.accentBright, alpha), rgba(0, 0, 0, alpha * 0.78f));
		}
	}

	// Inner sculpted glass thumb puck
	const float knobR = r * 0.36f;
	dl->AddCircleFilled(ImVec2(knob.x, knob.y + 2.2f), knobR, rgba(0, 0, 0, alpha * 0.42f), 28);
	dl->AddCircleFilled(knob, knobR,
		vis.joystickActive ? rgba(LauncherPalette::kTextPrimary, alpha * 0.88f) : rgba(LauncherPalette::kTextPrimary, alpha * 0.56f), 28);
	dl->AddCircle(knob, knobR,
		vis.joystickActive ? rgba(pal.accent, alpha) : rgba(255, 255, 255, alpha * 0.82f), 28, vis.joystickActive ? 2.4f : 1.6f);
	dl->AddCircleFilled(knob, knobR * 0.34f,
		vis.joystickActive ? rgba(pal.accent, alpha * 0.85f) : rgba(LauncherPalette::kGlassDark, alpha * 0.25f), 16);
}

const char* rpItemLabel(touch_hud::RpStripItem it, bool expanded)
{
	switch (it) {
		case touch_hud::RpStripItem::ToggleCollapse:  return expanded ? "<<" : ">>";
		case touch_hud::RpStripItem::KeyY:            return "Y";
		case touch_hud::RpStripItem::KeyN:            return "N";
		case touch_hud::RpStripItem::KeyAlt:          return "ALT";
		case touch_hud::RpStripItem::KeyH:            return "H";
		case touch_hud::RpStripItem::KeyTab:          return "TAB";
		case touch_hud::RpStripItem::Key2:            return "2";
		case touch_hud::RpStripItem::NumDrawerToggle: return "Num";
		case touch_hud::RpStripItem::Num8:            return "8";
		case touch_hud::RpStripItem::Num4:            return "4";
		case touch_hud::RpStripItem::Num6:            return "6";
		case touch_hud::RpStripItem::Num2:            return "2";
		case touch_hud::RpStripItem::KeyEsc:          return "ESC";
		case touch_hud::RpStripItem::ScreenshotF8:    return "F8";
		case touch_hud::RpStripItem::OpenHudEditor:   return "HUD";
		default: return "";
	}
}

} // namespace

void renderTouchHudOverlay(ImGuiRenderer* renderer, const RenderSnapshot& snap)
{
	(void)renderer;
	ImDrawList* bgDl = ImGui::GetBackgroundDrawList();
	ImDrawList* fgDl = ImGui::GetForegroundDrawList();
	ImFont* font = ImGui::GetFont();
	if (!bgDl || !font) return;

	const LauncherPalette pal = LauncherPalette::current();

	// Build a temporary read-only TouchHudCore geometry view matching live screen + layout + caps
	touch_hud::TouchHudCore geom;
	geom.setScreenSize(snap.screenW, snap.screenH);
	geom.setLayout(snap.layout);
	geom.setCapabilities(snap.caps, 0);
	geom.setRpExpanded(snap.vis.rpExpanded);

	const float globalAlpha = snap.layout.prefs.globalOpacity;
	const ImU32 shadowCol = rgba(0, 0, 0, globalAlpha * 0.72f);

	// 1) Draw Standard Controls (automatically hidden when snap.caps.rpStripOnly is true)
	for (size_t i = 0; i < touch_hud::kControlCount; ++i) {
		const auto id = static_cast<touch_hud::ControlId>(i);
		if (id == touch_hud::ControlId::RpStrip) continue;
		if (!geom.isControlActive(id)) continue;

		const auto& cfg = snap.layout.ctrl(id);
		const float alpha = std::clamp(cfg.opacity * globalAlpha, 0.12f, 1.0f);
		const auto circle = geom.controlCircle(id);
		const bool pressed = (snap.vis.pressedControlMask & (1u << i)) != 0u;
		const ImU32 iconShadow = rgba(0, 0, 0, alpha * 0.68f);

		if (id == touch_hud::ControlId::Joystick || id == touch_hud::ControlId::VehJoystick) {
			drawJoystick(bgDl, font, circle, snap.vis, alpha,
				id == touch_hud::ControlId::Joystick && snap.layout.prefs.autoSprintLock, pal);
			continue;
		}

		MedallionStyle mStyle = MedallionStyle::Standard;
		if (id == touch_hud::ControlId::FireRight || id == touch_hud::ControlId::FireLeft || id == touch_hud::ControlId::VehFire) {
			mStyle = MedallionStyle::Fire;
		}
		else if (id == touch_hud::ControlId::VehAccel) {
			mStyle = MedallionStyle::Gas;
		}
		else if (id == touch_hud::ControlId::VehBrake) {
			mStyle = MedallionStyle::Brake;
		}

		const bool isActiveAccent =
			(id == touch_hud::ControlId::Aim && snap.caps.isAiming)
			|| (id == touch_hud::ControlId::Crouch && snap.caps.isCrouched)
			|| (id == touch_hud::ControlId::Sprint && snap.vis.sprintLocked)
			|| (id == touch_hud::ControlId::VehDriveBy && snap.caps.isInDriveBy)
			|| (id == touch_hud::ControlId::VehHorn && snap.caps.isHydra && snap.caps.hydraJetForward);

		drawMedallion(bgDl, circle, alpha, pressed, isActiveAccent, mStyle, pal);

		// Choose crisp high-contrast icon foreground color
		const bool solidAccentBg = (mStyle == MedallionStyle::Fire && pressed) || isActiveAccent;
		const ImU32 fg = solidAccentBg
			? rgba(pal.onAccent, alpha)
			: rgba(LauncherPalette::kTextPrimary, alpha);
		const ImU32 fgShadow = solidAccentBg ? rgba(0, 0, 0, alpha * 0.28f) : iconShadow;

		const ImVec2 center(circle.cx, circle.cy);
		const float fontSz = std::max(14.0f, circle.r * 0.56f);

		switch (id) {
			case touch_hud::ControlId::FireRight:
				if (snap.caps.hasFirearm) {
					drawBulletGlyph(bgDl, center, circle.r, fg, fgShadow, snap.layout.prefs.fireRightDragAim);
				}
				else {
					drawFistGlyph(bgDl, center, circle.r, fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::FireLeft:
				if (snap.caps.hasFirearm) {
					drawBulletGlyph(bgDl, center, circle.r, fg, fgShadow, false);
				}
				else {
					drawFistGlyph(bgDl, center, circle.r, fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::Aim:
				if (snap.caps.hasFirearm) {
					drawScopeGlyph(bgDl, center, circle.r, fg, fgShadow);
				}
				else {
					drawShieldGlyph(bgDl, center, circle.r, fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::Jump:
				drawJumpFigureGlyph(bgDl, center, circle.r, fg, fgShadow);
				break;
			case touch_hud::ControlId::Crouch:
				drawCrouchFigureGlyph(bgDl, center, circle.r, fg, fgShadow);
				break;
			case touch_hud::ControlId::Sprint:
				drawSprintManGlyph(bgDl, center, circle.r, fg, fgShadow);
				break;
			case touch_hud::ControlId::InteractF:
				if (snap.caps.nearVehicle) {
					drawCarGlyph(bgDl, center, circle.r, fg, fgShadow, false);
					drawCornerBadge(bgDl, font, center, circle.r, "F", rgba(pal.accentBright, alpha), iconShadow);
				}
				else {
					drawCenteredLabel(bgDl, font, fontSz * 1.15f, center, "F", fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::PassengerG:
				drawPassengerSeatGlyph(bgDl, center, circle.r, fg, fgShadow);
				drawCornerBadge(bgDl, font, center, circle.r, "G", rgba(pal.accentBright, alpha), iconShadow);
				break;
			case touch_hud::ControlId::WeaponPrevQ:
				if (snap.caps.isScoped1stPerson) {
					drawScopeZoomGlyph(bgDl, center, circle.r, fg, fgShadow, false);
				}
				else {
					drawDoubleChevronGlyph(bgDl, center, circle.r, fg, fgShadow, true);
				}
				break;
			case touch_hud::ControlId::WeaponNextE:
				if (snap.caps.isScoped1stPerson) {
					drawScopeZoomGlyph(bgDl, center, circle.r, fg, fgShadow, true);
				}
				else {
					drawDoubleChevronGlyph(bgDl, center, circle.r, fg, fgShadow, false);
				}
				break;
			case touch_hud::ControlId::VehLookLeftQ:
				if (snap.caps.context == touch_hud::HudContext::Aircraft) {
					drawYawRotateGlyph(bgDl, center, circle.r, fg, fgShadow, true);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("YAW L", "左旋"), rgba(pal.accentBright, alpha), iconShadow);
				}
				else {
					drawSideLookGlyph(bgDl, center, circle.r, fg, fgShadow, true);
				}
				break;
			case touch_hud::ControlId::VehLookRightE:
				if (snap.caps.context == touch_hud::HudContext::Aircraft) {
					drawYawRotateGlyph(bgDl, center, circle.r, fg, fgShadow, false);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("YAW R", "右旋"), rgba(pal.accentBright, alpha), iconShadow);
				}
				else {
					drawSideLookGlyph(bgDl, center, circle.r, fg, fgShadow, false);
				}
				break;
			case touch_hud::ControlId::CamZoom:
			case touch_hud::ControlId::VehCamToggle:
				drawCameraGlyph(bgDl, center, circle.r, fg, fgShadow);
				break;
			case touch_hud::ControlId::VehSteerLeft:
				drawChevronArrowGlyph(bgDl, center, circle.r, fg, fgShadow, 0);
				break;
			case touch_hud::ControlId::VehSteerRight:
				drawChevronArrowGlyph(bgDl, center, circle.r, fg, fgShadow, 1);
				break;
			case touch_hud::ControlId::VehPitchUp:
				drawChevronArrowGlyph(bgDl, center, circle.r, fg, fgShadow, 2);
				break;
			case touch_hud::ControlId::VehPitchDown:
				drawChevronArrowGlyph(bgDl, center, circle.r, fg, fgShadow, 3);
				break;
			case touch_hud::ControlId::VehAccel:
				drawPedalGlyph(bgDl, center, circle.r, fg, fgShadow, true);
				break;
			case touch_hud::ControlId::VehBrake:
				drawPedalGlyph(bgDl, center, circle.r, fg, fgShadow, false);
				break;
			case touch_hud::ControlId::VehHandbrake:
				if (snap.caps.isHydra) {
					drawScopeGlyph(bgDl, ImVec2(center.x, center.y - circle.r * 0.10f), circle.r * 0.88f, fg, fgShadow);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("LOCK", "锁定"), rgba(pal.accentBright, alpha), iconShadow);
				}
				else if (snap.caps.isHunter) {
					drawBulletGlyph(bgDl, ImVec2(center.x, center.y - circle.r * 0.10f), circle.r * 0.88f, fg, fgShadow, false);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("GUN", "机枪"), rgba(pal.accentBright, alpha), iconShadow);
				}
				else {
					drawHandbrakeDriftGlyph(bgDl, center, circle.r, fg, fgShadow);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("DRIFT", "手刹"), rgba(pal.accentBright, alpha), iconShadow);
				}
				break;
			case touch_hud::ControlId::VehFire:
				if (snap.caps.isInDriveBy) {
					drawBulletGlyph(bgDl, center, circle.r, fg, fgShadow, snap.layout.prefs.fireRightDragAim);
				}
				else if (snap.caps.isHydra || snap.caps.isHunter) {
					drawMissileGlyph(bgDl, center, circle.r, fg, fgShadow);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("MSL", "导弹"), fg, fgShadow);
				}
				else {
					drawVehicleFireGlyph(bgDl, center, circle.r, fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::VehFlare:
				drawFlareGlyph(bgDl, center, circle.r, fg, fgShadow);
				drawCornerBadge(bgDl, font, center, circle.r, L10N("FLARE", "干扰弹"), rgba(pal.accentBright, alpha), iconShadow);
				break;
			case touch_hud::ControlId::VehHorn:
				if (snap.caps.isHydra) {
					drawHydraThrusterGlyph(bgDl, center, circle.r, fg, fgShadow, snap.caps.hydraJetForward);
					drawCornerBadge(bgDl, font, center, circle.r,
						snap.caps.hydraJetForward ? L10N("JET", "前飞") : L10N("VTOL", "悬停"),
						snap.caps.hydraJetForward ? fg : rgba(pal.accentBright, alpha), fgShadow);
				}
				else {
					drawHornGlyph(bgDl, center, circle.r, fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::VehExitF:
				drawCarGlyph(bgDl, center, circle.r, fg, fgShadow, true);
				drawCornerBadge(bgDl, font, center, circle.r, "F", rgba(pal.accentBright, alpha), iconShadow);
				break;
			case touch_hud::ControlId::VehSubMission2:
				if (snap.caps.isAircraftWithGear) {
					drawCenteredLabel(bgDl, font, fontSz * 0.82f, ImVec2(center.x, center.y - circle.r * 0.14f), "2", fg, fgShadow);
					drawCornerBadge(bgDl, font, center, circle.r, L10N("GEAR", "起落架"), rgba(pal.accentBright, alpha), iconShadow);
				}
				else {
					drawCenteredLabel(bgDl, font, fontSz * 0.98f, center, "2", fg, fgShadow);
				}
				break;
			case touch_hud::ControlId::VehDriveBy:
				drawScopeGlyph(bgDl, center, circle.r, fg, fgShadow);
				break;
			default:
				break;
		}
	}

	// 2) Draw Custom Combo / Command Buttons
	for (size_t i = 0; i < touch_hud::kMaxCustomButtons; ++i) {
		if (!geom.isCustomButtonActive(i)) continue;
		const auto& b = snap.layout.customButtons[i];
		const auto circle = geom.customButtonCircle(i);
		const float alpha = std::clamp(b.opacity * globalAlpha, 0.14f, 1.0f);
		const bool pressed = (snap.vis.pressedCustomMask & (1u << i)) != 0u;
		drawMedallion(bgDl, circle, alpha, pressed, false, MedallionStyle::Custom, pal);
		const char* lbl = b.label[0] ? b.label : "KEY";
		drawCenteredLabel(bgDl, font, std::max(13.0f, circle.r * 0.44f), ImVec2(circle.cx, circle.cy),
			lbl, rgba(LauncherPalette::kTextPrimary, alpha), shadowCol);
	}

	// 3) Draw Collapsible Single-Row RP Action Strip (and Num 8/4/6/2 sub-drawer)
	// CRITICAL: When Scoreboard / SelectTextDraw / ServerMenu / ObjectSelect is active (rpStripOnly),
	// draw onto ImGui::GetForegroundDrawList() so TAB & ESC always stay visible ABOVE the modal!
	if (geom.isControlActive(touch_hud::ControlId::RpStrip)) {
		ImDrawList* stripDl = (snap.caps.rpStripOnly && fgDl) ? fgDl : bgDl;
		const auto& stripCfg = snap.layout.ctrl(touch_hud::ControlId::RpStrip);
		const float baseAlpha = snap.caps.rpStripOnly
			? std::max(0.88f, stripCfg.opacity * globalAlpha)
			: std::clamp(stripCfg.opacity * globalAlpha, 0.18f, 1.0f);

		std::array<touch_hud::RpStripLayoutItem, static_cast<size_t>(touch_hud::RpStripItem::Count)> items{};
		touch_hud::TouchHudCore stripGeom = geom;
		stripGeom.setNumDrawerOpen(snap.vis.numDrawerOpen);
		touch_hud::ScreenRect outerBounds{};
		stripGeom.computeRpStripItems(items, &outerBounds);

		// Unified Launcher-style floating dark glass dock behind main strip row
		if (outerBounds.w > 0.0f) {
			const float dockPad = 4.0f;
			const ImVec2 dMin(outerBounds.x - dockPad, outerBounds.y - dockPad);
			const ImVec2 dMax(outerBounds.x + outerBounds.w + dockPad, outerBounds.y + outerBounds.h + dockPad);
			stripDl->AddRectFilled(dMin, dMax, rgba(LauncherPalette::kBgDeep, baseAlpha * 0.72f), 11.0f);
			stripDl->AddRect(dMin, dMax, rgba(255, 255, 255, baseAlpha * 0.14f), 11.0f, ImDrawCornerFlags_All, 1.0f);
		}

		for (size_t i = 0; i < items.size(); ++i) {
			const auto& it = items[i];
			if (!it.visible) continue;

			const bool pressed = (snap.vis.pressedRpItemMask & (1u << i)) != 0u;
			const bool isAltLatched = (it.item == touch_hud::RpStripItem::KeyAlt && snap.vis.altLatched);
			const bool isNumActive  = (it.item == touch_hud::RpStripItem::NumDrawerToggle && snap.vis.numDrawerOpen);
			const bool isEditorBtn  = (it.item == touch_hud::RpStripItem::OpenHudEditor);
			const bool isModalDismissKey = snap.caps.rpStripOnly
				&& (it.item == touch_hud::RpStripItem::KeyTab || it.item == touch_hud::RpStripItem::KeyEsc);

			ImU32 bg = rgba(LauncherPalette::kSurface, baseAlpha * 0.86f);
			ImU32 bd = rgba(255, 255, 255, baseAlpha * 0.18f);
			ImU32 txt = rgba(LauncherPalette::kTextPrimary, baseAlpha);
			ImU32 txtShadow = rgba(0, 0, 0, baseAlpha * 0.70f);
			float bdThick = 1.2f;

			if (pressed || isAltLatched) {
				bg = rgba(pal.accent, baseAlpha * 0.94f);
				bd = rgba(pal.accentBright, baseAlpha);
				txt = rgba(pal.onAccent, baseAlpha);
				txtShadow = rgba(0, 0, 0, baseAlpha * 0.20f);
				bdThick = 2.0f;
			}
			else if (isNumActive || isModalDismissKey) {
				bg = rgba(pal.accentDeep, baseAlpha * 0.92f);
				bd = rgba(pal.accent, baseAlpha * 0.95f);
				txt = rgba(pal.accentBright, baseAlpha);
				bdThick = 1.7f;
			}
			else if (isEditorBtn) {
				bg = rgba(LauncherPalette::kSurface2, baseAlpha * 0.92f);
				bd = rgba(pal.accent, baseAlpha * 0.65f);
				txt = rgba(pal.accent, baseAlpha);
				bdThick = 1.4f;
			}

			const ImVec2 pMin(it.rect.x, it.rect.y);
			const ImVec2 pMax(it.rect.x + it.rect.w, it.rect.y + it.rect.h);
			const float round = std::min(8.0f, it.rect.h * 0.24f);
			stripDl->AddRectFilled(pMin, pMax, bg, round);
			// Subtle top glass sheen inside key tile
			if (!pressed && !isAltLatched) {
				stripDl->AddLine(
					ImVec2(pMin.x + round * 0.6f, pMin.y + 1.0f),
					ImVec2(pMax.x - round * 0.6f, pMin.y + 1.0f),
					rgba(255, 255, 255, baseAlpha * 0.12f), 1.0f);
			}
			stripDl->AddRect(pMin, pMax, bd, round, ImDrawCornerFlags_All, bdThick);

			const char* lbl = rpItemLabel(it.item, snap.vis.rpExpanded);
			drawCenteredLabel(stripDl, font, std::max(13.0f, it.rect.h * 0.44f),
				ImVec2((pMin.x + pMax.x) * 0.5f, (pMin.y + pMax.y) * 0.5f),
				lbl, txt, txtShadow);
		}
	}
}

} // namespace TouchHud
