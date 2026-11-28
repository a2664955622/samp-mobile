#pragma once

#include "../../Settings.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TouchHud {

struct Rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

// Exact match to LauncherThemeManager.hueToAccentColor (HSV -> RGB)
inline Rgb hsvToRgb(float hDeg, float s, float v)
{
	float h = std::fmod(hDeg, 360.0f);
	if (h < 0.0f) h += 360.0f;
	const float c = v * s;
	const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
	const float m = v - c;
	float rf = 0.0f, gf = 0.0f, bf = 0.0f;
	if (h < 60.0f)       { rf = c; gf = x; bf = 0.0f; }
	else if (h < 120.0f) { rf = x; gf = c; bf = 0.0f; }
	else if (h < 180.0f) { rf = 0.0f; gf = c; bf = x; }
	else if (h < 240.0f) { rf = 0.0f; gf = x; bf = c; }
	else if (h < 300.0f) { rf = x; gf = 0.0f; bf = c; }
	else                 { rf = c; gf = 0.0f; bf = x; }
	auto toByte = [](float f) -> uint8_t {
		return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(f * 255.0f)), 0, 255));
	};
	return {toByte(rf + m), toByte(gf + m), toByte(bf + m)};
}

inline ImU32 rgba(uint8_t r, uint8_t g, uint8_t b, float alpha)
{
	const int a = std::clamp(static_cast<int>(std::lround(alpha * 255.0f)), 0, 255);
	return IM_COL32(r, g, b, a);
}

inline ImU32 rgba(Rgb c, float alpha)
{
	return rgba(c.r, c.g, c.b, alpha);
}

// Unified palette matching app/src/main/res/values/colors.xml & LauncherThemeManager.java
struct LauncherPalette {
	// Static warm-obsidian Launcher surfaces & text
	static constexpr Rgb kBgDeep      = {  7,   6,   8}; // #070608 launcher_bg
	static constexpr Rgb kGlassDark   = { 13,  12,  16}; // #0D0C10 HUD medallion base
	static constexpr Rgb kSurface     = { 23,  22,  26}; // #17161A launcher_surface
	static constexpr Rgb kSurface2    = { 35,  34,  39}; // #232227 launcher_surface_2
	static constexpr Rgb kSurface3    = { 46,  45,  52}; // #2E2D34 elevated hover/pill
	static constexpr Rgb kTextPrimary = {244, 242, 238}; // #F4F2EE launcher_text
	static constexpr Rgb kTextDim     = {160, 156, 150}; // #A09C96 launcher_text_dim
	static constexpr Rgb kTextFaint   = {110, 106, 101}; // #6E6A65 ui_text_faint
	static constexpr Rgb kOnAccentDk  = { 21,  17,  10}; // #15110A launcher_on_accent
	static constexpr Rgb kGood        = { 76, 208, 128}; // #4CD080 ui_good
	static constexpr Rgb kWarn        = {255, 169,  77}; // #FFA94D ui_warn
	static constexpr Rgb kDanger      = {240,  85,  90}; // #F0555A launcher_danger

	// Dynamic user theme colors derived from launcher_theme_hue (default 38 = Classic GTA Gold)
	Rgb accent{};       // Main solid accent (HSV h, 0.84, 0.96)
	Rgb accentBright{}; // Rim / highlight glow (HSV h, 0.50, 1.00)
	Rgb accentDeep{};   // Deep tinted glass fill (HSV h, 0.70, 0.30)
	Rgb onAccent{};     // High-contrast text on solid accent (auto dark/light by luminance)

	static LauncherPalette current()
	{
		const float h = static_cast<float>(Settings::themeHue() % 360);
		LauncherPalette p{};
		p.accent       = hsvToRgb(h, 0.84f, 0.96f);
		p.accentBright = hsvToRgb(h, 0.48f, 1.00f);
		p.accentDeep   = hsvToRgb(h, 0.68f, 0.30f);
		const float lum = 0.299f * p.accent.r + 0.587f * p.accent.g + 0.114f * p.accent.b;
		p.onAccent = (lum >= 145.0f) ? kOnAccentDk : kTextPrimary;
		return p;
	}
};

} // namespace TouchHud
