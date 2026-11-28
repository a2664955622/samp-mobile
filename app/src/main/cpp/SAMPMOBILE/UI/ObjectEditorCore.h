#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace object_editor {

enum class EditMode : uint8_t {
	Move = 0,
	Rotate = 1,
	Scale = 2,
};

enum class EditAxis : uint8_t {
	X = 0,
	Y = 1,
	Z = 2,
};

enum class SpeedGear : uint8_t {
	Fine = 0,
	Normal = 1,
	Fast = 2,
};

struct Vec2 {
	float x = 0.0f;
	float y = 0.0f;
};

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Basis3 {
	Vec3 right{1.0f, 0.0f, 0.0f};
	Vec3 up{0.0f, 1.0f, 0.0f};
	Vec3 at{0.0f, 0.0f, 1.0f};
};

struct Rect {
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;

	constexpr bool contains(float px, float py, float pad = 0.0f) const
	{
		return w > 0.0f && h > 0.0f
			&& px >= (x - pad) && px <= (x + w + pad)
			&& py >= (y - pad) && py <= (y + h + pad);
	}
};

struct GizmoScreenAxis {
	bool valid = false;
	Vec2 origin{};
	Vec2 posTip{};
	Vec2 negTip{};
	float worldLen = 1.0f;
};

constexpr float speedMultiplier(SpeedGear gear)
{
	switch (gear) {
		case SpeedGear::Fine:   return 0.15f;
		case SpeedGear::Normal: return 1.00f;
		case SpeedGear::Fast:   return 4.00f;
	}
	return 1.0f;
}

constexpr float stepAmount(EditMode mode, SpeedGear gear)
{
	switch (mode) {
		case EditMode::Move:
			switch (gear) {
				case SpeedGear::Fine:   return 0.002f;
				case SpeedGear::Normal: return 0.010f;
				case SpeedGear::Fast:   return 0.050f;
			}
			return 0.010f;
		case EditMode::Rotate:
			switch (gear) {
				case SpeedGear::Fine:   return 0.25f;
				case SpeedGear::Normal: return 1.00f;
				case SpeedGear::Fast:   return 5.00f;
			}
			return 1.0f;
		case EditMode::Scale:
			switch (gear) {
				case SpeedGear::Fine:   return 0.005f;
				case SpeedGear::Normal: return 0.020f;
				case SpeedGear::Fast:   return 0.100f;
			}
			return 0.020f;
	}
	return 0.010f;
}

inline float wrapEulerDeg(float deg)
{
	if (!std::isfinite(deg)) return 0.0f;
	float wrapped = std::fmod(deg, 360.0f);
	if (wrapped < 0.0f) wrapped += 360.0f;
	if (wrapped >= 359.9995f || std::fabs(wrapped) < 1e-5f) wrapped = 0.0f;
	return wrapped;
}

inline Vec3 normalizeSafe(Vec3 v, Vec3 fallback)
{
	const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
	if (!std::isfinite(lenSq) || lenSq < 1e-8f) {
		return fallback;
	}
	const float inv = 1.0f / std::sqrt(lenSq);
	return {v.x * inv, v.y * inv, v.z * inv};
}

inline Basis3 normalizedBasis(const Basis3& b)
{
	return {
		normalizeSafe(b.right, {1.0f, 0.0f, 0.0f}),
		normalizeSafe(b.up,    {0.0f, 1.0f, 0.0f}),
		normalizeSafe(b.at,    {0.0f, 0.0f, 1.0f}),
	};
}

inline float computeGizmoWorldLength(float camDistMeters)
{
	if (!std::isfinite(camDistMeters)) return 1.0f;
	const float clampedDist = std::clamp(camDistMeters, 0.6f, 60.0f);
	return std::clamp(clampedDist * 0.22f, 0.18f, 12.0f);
}

inline float pointToSegmentDistSq(Vec2 p, Vec2 a, Vec2 b)
{
	const float abx = b.x - a.x;
	const float aby = b.y - a.y;
	const float lenSq = abx * abx + aby * aby;
	if (lenSq < 1e-6f) {
		const float dx = p.x - a.x;
		const float dy = p.y - a.y;
		return dx * dx + dy * dy;
	}
	const float t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / lenSq, 0.0f, 1.0f);
	const float projX = a.x + t * abx;
	const float projY = a.y + t * aby;
	const float dx = p.x - projX;
	const float dy = p.y - projY;
	return dx * dx + dy * dy;
}

// Hit-tests the 3 projected gizmo axes. Returns 0 (X), 1 (Y), 2 (Z), or -1 (miss).
inline int hitTestGizmoAxes(const std::array<GizmoScreenAxis, 3>& axes,
	float px, float py, float badgeHitRadius, float shaftHitRadius)
{
	int bestAxis = -1;
	float bestDistSq = badgeHitRadius * badgeHitRadius;

	// Priority 1: circular axis tip handles (+X / +Y / +Z)
	for (int i = 0; i < 3; ++i) {
		if (!axes[i].valid) continue;
		const float dx = px - axes[i].posTip.x;
		const float dy = py - axes[i].posTip.y;
		const float d2 = dx * dx + dy * dy;
		if (d2 <= bestDistSq) {
			bestDistSq = d2;
			bestAxis = i;
		}
	}
	if (bestAxis >= 0) {
		return bestAxis;
	}

	// Priority 2: positive axis shafts (excluding crowded center origin)
	bestDistSq = shaftHitRadius * shaftHitRadius;
	for (int i = 0; i < 3; ++i) {
		if (!axes[i].valid) continue;
		const float odx = px - axes[i].origin.x;
		const float ody = py - axes[i].origin.y;
		if ((odx * odx + ody * ody) < 100.0f) continue;
		const float d2 = pointToSegmentDistSq({px, py}, axes[i].origin, axes[i].posTip);
		if (d2 <= bestDistSq) {
			bestDistSq = d2;
			bestAxis = i;
		}
	}
	return bestAxis;
}

// Converts a 2D finger drag delta (dxPx, dyPx) on a grabbed 3D gizmo handle into
// world/bone meters (Move), degrees (Rotate), or scale units (Scale).
inline float computeGizmoDragDelta(EditMode mode, const GizmoScreenAxis& axis,
	float dxPx, float dyPx, SpeedGear gear)
{
	const float mult = speedMultiplier(gear);
	const float vx = axis.posTip.x - axis.origin.x;
	const float vy = axis.posTip.y - axis.origin.y;
	const float scrLen = std::hypot(vx, vy);

	switch (mode) {
		case EditMode::Move: {
			const float wLen = std::max(0.05f, axis.worldLen);
			if (axis.valid && scrLen >= 14.0f) {
				const float projPx = (dxPx * vx + dyPx * vy) / scrLen;
				return (projPx / scrLen) * wLen * mult;
			}
			return (dxPx - dyPx) * 0.003f * wLen * mult;
		}
		case EditMode::Rotate:
			// Camera-flip-immune screen swipe: right/up = +deg, left/down = -deg
			return (dxPx - dyPx * 0.35f) * 0.45f * mult;
		case EditMode::Scale: {
			if (axis.valid && scrLen >= 14.0f) {
				const float projPx = (dxPx * vx + dyPx * vy) / scrLen;
				return projPx * 0.0045f * mult;
			}
			return (dxPx - dyPx) * 0.0045f * mult;
		}
	}
	return 0.0f;
}

inline float computeScrubTrackDelta(EditMode mode, float dxPx, float trackWidthPx, SpeedGear gear)
{
	const float safeW = std::max(120.0f, trackWidthPx);
	const float norm = dxPx / safeW;
	const float mult = speedMultiplier(gear);
	switch (mode) {
		case EditMode::Move:   return norm * 1.20f * mult;
		case EditMode::Rotate: return norm * 180.0f * mult;
		case EditMode::Scale:  return norm * 1.00f * mult;
	}
	return 0.0f;
}

inline Vec3 applyMoveDelta(Vec3 pos, EditAxis axis, float delta, bool localSpace, const Basis3& objBasis)
{
	if (!std::isfinite(delta) || delta == 0.0f) return pos;
	if (!localSpace) {
		switch (axis) {
			case EditAxis::X: pos.x += delta; break;
			case EditAxis::Y: pos.y += delta; break;
			case EditAxis::Z: pos.z += delta; break;
		}
		return pos;
	}
	const Basis3 b = normalizedBasis(objBasis);
	Vec3 dir{1.0f, 0.0f, 0.0f};
	switch (axis) {
		case EditAxis::X: dir = b.right; break;
		case EditAxis::Y: dir = b.up;    break;
		case EditAxis::Z: dir = b.at;    break;
	}
	pos.x += dir.x * delta;
	pos.y += dir.y * delta;
	pos.z += dir.z * delta;
	return pos;
}

inline Vec3 applyRotateDelta(Vec3 eulerDeg, EditAxis axis, float deltaDeg)
{
	if (!std::isfinite(deltaDeg) || deltaDeg == 0.0f) return eulerDeg;
	switch (axis) {
		case EditAxis::X: eulerDeg.x = wrapEulerDeg(eulerDeg.x + deltaDeg); break;
		case EditAxis::Y: eulerDeg.y = wrapEulerDeg(eulerDeg.y + deltaDeg); break;
		case EditAxis::Z: eulerDeg.z = wrapEulerDeg(eulerDeg.z + deltaDeg); break;
	}
	return eulerDeg;
}

inline Vec3 applyScaleDelta(Vec3 scale, EditAxis axis, float delta, bool uniform)
{
	if (!std::isfinite(delta) || delta == 0.0f) return scale;
	constexpr float kMinScale = 0.01f;
	constexpr float kMaxScale = 20.0f;
	if (uniform) {
		scale.x = std::clamp(scale.x + delta, kMinScale, kMaxScale);
		scale.y = std::clamp(scale.y + delta, kMinScale, kMaxScale);
		scale.z = std::clamp(scale.z + delta, kMinScale, kMaxScale);
		return scale;
	}
	switch (axis) {
		case EditAxis::X: scale.x = std::clamp(scale.x + delta, kMinScale, kMaxScale); break;
		case EditAxis::Y: scale.y = std::clamp(scale.y + delta, kMinScale, kMaxScale); break;
		case EditAxis::Z: scale.z = std::clamp(scale.z + delta, kMinScale, kMaxScale); break;
	}
	return scale;
}

} // namespace object_editor
