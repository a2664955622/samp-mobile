#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace textdraw {

struct HitBox {
	int id = -1;
	float minX = 0.0f;
	float minY = 0.0f;
	float maxX = 0.0f;
	float maxY = 0.0f;
};

struct TouchSelectRadii {
	float haloRadiusPx = 18.0f;     // Expand miss tolerance around small PC-sized hitboxes
	float tapLockDragPx = 15.0f;    // Keep pressed target locked during stationary tap thumb-roll
	float retainRectSlopPx = 28.0f; // Allow lift-off thumb roll slightly outside pressed rect
	float retainMaxDragPx = 44.0f;  // Clear/cancel if user intentionally drags far into empty space
};

struct TouchEvalResult {
	int hoveredId = -1;
	int clickedId = -1;
};

inline HitBox makeNormalizedHitBox(int id, float x0, float y0, float x1, float y1) noexcept
{
	HitBox b{};
	b.id = id;
	b.minX = std::min(x0, x1);
	b.maxX = std::max(x0, x1);
	b.minY = std::min(y0, y1);
	b.maxY = std::max(y0, y1);
	return b;
}

inline bool isNonEmptyHitBox(const HitBox& b) noexcept
{
	return (b.maxX - b.minX) > 0.5f && (b.maxY - b.minY) > 0.5f;
}

inline bool containsExact(const HitBox& b, float x, float y) noexcept
{
	return x >= b.minX && x <= b.maxX && y >= b.minY && y <= b.maxY;
}

inline float distanceSqToHitBox(const HitBox& b, float x, float y) noexcept
{
	const float dx = (x < b.minX) ? (b.minX - x) : ((x > b.maxX) ? (x - b.maxX) : 0.0f);
	const float dy = (y < b.minY) ? (b.minY - y) : ((y > b.maxY) ? (y - b.maxY) : 0.0f);
	return dx * dx + dy * dy;
}

inline TouchSelectRadii computeTouchSelectRadii(float screenW, float screenH) noexcept
{
	const float u = std::max(240.0f, std::min(screenW, screenH));
	TouchSelectRadii r{};
	r.haloRadiusPx     = std::clamp(u * 0.018f, 10.0f, 24.0f);
	r.tapLockDragPx    = std::clamp(u * 0.015f, 10.0f, 18.0f);
	r.retainRectSlopPx = std::clamp(u * 0.028f, 16.0f, 34.0f);
	r.retainMaxDragPx  = std::clamp(u * 0.042f, 24.0f, 52.0f);
	return r;
}

// Two-tier hit selection:
//  1) Exact hit inside [minX..maxX] x [minY..maxY] ALWAYS wins, choosing the last (top-most Z)
//     selectable TextDraw just like PC SA-MP.
//  2) When no exact rect contains (x, y), pick the closest selectable TextDraw within haloRadiusPx
//     (tie-breaking by top-most Z order). Makes tiny 10-unit PC rows effortless to tap on phones
//     without stealing clicks from adjacent rows.
inline int findBestSelectableHit(
	const HitBox* boxes,
	size_t count,
	float x,
	float y,
	float haloRadiusPx) noexcept
{
	if (!boxes || count == 0) {
		return -1;
	}

	int topExactId = -1;
	for (size_t i = 0; i < count; ++i) {
		if (!isNonEmptyHitBox(boxes[i])) {
			continue;
		}
		if (containsExact(boxes[i], x, y)) {
			topExactId = boxes[i].id;
		}
	}
	if (topExactId >= 0) {
		return topExactId;
	}

	if (haloRadiusPx <= 0.0f) {
		return -1;
	}

	const float maxDistSq = haloRadiusPx * haloRadiusPx;
	float bestDistSq = maxDistSq;
	int bestHaloId = -1;
	for (size_t i = 0; i < count; ++i) {
		if (!isNonEmptyHitBox(boxes[i])) {
			continue;
		}
		const float d2 = distanceSqToHitBox(boxes[i], x, y);
		if (d2 <= maxDistSq) {
			// Strictly closer wins; equal distance (within 0.25 sq px) prefers top-most Z order.
			if (bestHaloId < 0 || d2 + 0.25f < bestDistSq || std::fabs(d2 - bestDistSq) <= 0.25f) {
				if (bestHaloId < 0 || d2 < bestDistSq) {
					bestDistSq = d2;
				}
				bestHaloId = boxes[i].id;
			}
		}
	}
	return bestHaloId;
}

class TouchSelectTracker {
public:
	void reset() noexcept
	{
		m_tracking = false;
		m_pointerId = -1;
		m_pressedId = -1;
		m_lastHoverId = -1;
		m_downX = 0.0f;
		m_downY = 0.0f;
		m_maxDragSq = 0.0f;
	}

	// type: 2 = Down, 3 = Move, 1 = Up
	TouchEvalResult onTouch(
		int type,
		int pointerId,
		float x,
		float y,
		const HitBox* boxes,
		size_t count,
		const TouchSelectRadii& radii) noexcept
	{
		if (type == 2) { // Down
			if (!m_tracking || pointerId == m_pointerId) {
				m_tracking = true;
				m_pointerId = pointerId;
				m_downX = x;
				m_downY = y;
				m_maxDragSq = 0.0f;
				m_pressedId = findBestSelectableHit(boxes, count, x, y, radii.haloRadiusPx);
				m_lastHoverId = m_pressedId;
				return {m_lastHoverId, -1};
			}
			// Ignore secondary finger touches while primary finger is selecting
			m_lastHoverId = validateLiveId(boxes, count, m_lastHoverId);
			return {m_lastHoverId, -1};
		}

		if (type == 3) { // Move
			if (!m_tracking) {
				m_lastHoverId = findBestSelectableHit(boxes, count, x, y, radii.haloRadiusPx);
				return {m_lastHoverId, -1};
			}
			if (pointerId != m_pointerId) {
				m_lastHoverId = validateLiveId(boxes, count, m_lastHoverId);
				return {m_lastHoverId, -1};
			}

			const float dx = x - m_downX;
			const float dy = y - m_downY;
			const float dragSq = dx * dx + dy * dy;
			if (dragSq > m_maxDragSq) {
				m_maxDragSq = dragSq;
			}

			m_lastHoverId = resolveTrackedTarget(boxes, count, x, y, m_maxDragSq, radii);
			return {m_lastHoverId, -1};
		}

		if (type == 1) { // Up
			if (m_tracking && pointerId != m_pointerId) {
				m_lastHoverId = validateLiveId(boxes, count, m_lastHoverId);
				return {m_lastHoverId, -1};
			}

			int clicked = -1;
			if (m_tracking) {
				const float dx = x - m_downX;
				const float dy = y - m_downY;
				const float maxDragSq = std::max(m_maxDragSq, dx * dx + dy * dy);
				clicked = resolveTrackedTarget(boxes, count, x, y, maxDragSq, radii);
			}
			else {
				clicked = findBestSelectableHit(boxes, count, x, y, radii.haloRadiusPx);
			}

			reset();
			return {-1, clicked};
		}

		return {validateLiveId(boxes, count, m_lastHoverId), -1};
	}

private:
	static const HitBox* findBoxById(const HitBox* boxes, size_t count, int id) noexcept
	{
		if (!boxes || id < 0) {
			return nullptr;
		}
		for (size_t i = 0; i < count; ++i) {
			if (boxes[i].id == id && isNonEmptyHitBox(boxes[i])) {
				return &boxes[i];
			}
		}
		return nullptr;
	}

	static int validateLiveId(const HitBox* boxes, size_t count, int id) noexcept
	{
		return findBoxById(boxes, count, id) ? id : -1;
	}

	int resolveTrackedTarget(
		const HitBox* boxes,
		size_t count,
		float x,
		float y,
		float maxDragSq,
		const TouchSelectRadii& radii) const noexcept
	{
		const HitBox* pressedBox = findBoxById(boxes, count, m_pressedId);
		const float retainSlopSq = radii.retainRectSlopPx * radii.retainRectSlopPx;

		// 1) Stationary tap lock: if finger barely moved since Down (within tapLockDragPx) and is
		// still near the originally pressed button, keep m_pressedId locked so thumb lift-off roll
		// never jumps onto a tightly-stacked neighbor row!
		if (pressedBox && maxDragSq <= radii.tapLockDragPx * radii.tapLockDragPx) {
			if (distanceSqToHitBox(*pressedBox, x, y) <= retainSlopSq) {
				return m_pressedId;
			}
		}

		// 2) Active hit / slide-across-menu selection
		const int currentHit = findBestSelectableHit(boxes, count, x, y, radii.haloRadiusPx);
		if (currentHit >= 0) {
			return currentHit;
		}

		// 3) Lift-off / wobble retention when finger rolled slightly into empty margin around pressed button
		if (pressedBox && maxDragSq <= radii.retainMaxDragPx * radii.retainMaxDragPx) {
			if (distanceSqToHitBox(*pressedBox, x, y) <= retainSlopSq) {
				return m_pressedId;
			}
		}

		return -1;
	}

	bool m_tracking = false;
	int m_pointerId = -1;
	int m_pressedId = -1;
	int m_lastHoverId = -1;
	float m_downX = 0.0f;
	float m_downY = 0.0f;
	float m_maxDragSq = 0.0f;
};

} // namespace textdraw
