#include "UI/ObjectEditorCore.h"

#include <cmath>
#include <cstdio>

using namespace object_editor;

namespace {

int g_failures = 0;

#define EXPECT(cond) \
	do { \
		if (!(cond)) { \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures; \
		} \
	} while (0)

bool near(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) <= eps;
}

void testEulerWrappingAndRotateDelta()
{
	EXPECT(near(wrapEulerDeg(0.0f), 0.0f));
	EXPECT(near(wrapEulerDeg(360.0f), 0.0f));
	EXPECT(near(wrapEulerDeg(-90.0f), 270.0f));
	EXPECT(near(wrapEulerDeg(450.5f), 90.5f));
	EXPECT(near(wrapEulerDeg(-0.25f), 359.75f));

	Vec3 rot{358.0f, 10.0f, 90.0f};
	rot = applyRotateDelta(rot, EditAxis::X, 5.0f);
	rot = applyRotateDelta(rot, EditAxis::Y, -15.0f);
	EXPECT(near(rot.x, 3.0f));
	EXPECT(near(rot.y, 355.0f));
	EXPECT(near(rot.z, 90.0f));
}

void testLocalVsWorldMoveDelta()
{
	// Object rotated 90 degrees yaw: local +X points along world +Y, local +Y points along world -X
	Basis3 basis{
		{0.0f, 2.0f, 0.0f},  // unnormalized right (length 2 -> must normalize to (0, 1, 0))
		{-1.0f, 0.0f, 0.0f}, // up
		{0.0f, 0.0f, 1.0f},  // at
	};

	Vec3 p0{10.0f, 20.0f, 5.0f};
	const Vec3 worldMoved = applyMoveDelta(p0, EditAxis::X, 1.5f, false, basis);
	EXPECT(near(worldMoved.x, 11.5f) && near(worldMoved.y, 20.0f) && near(worldMoved.z, 5.0f));

	const Vec3 localMovedX = applyMoveDelta(p0, EditAxis::X, 1.5f, true, basis);
	EXPECT(near(localMovedX.x, 10.0f) && near(localMovedX.y, 21.5f) && near(localMovedX.z, 5.0f));

	const Vec3 localMovedY = applyMoveDelta(p0, EditAxis::Y, 2.0f, true, basis);
	EXPECT(near(localMovedY.x, 8.0f) && near(localMovedY.y, 20.0f) && near(localMovedY.z, 5.0f));
}

void testIndependentAndUniformScaleClamping()
{
	Vec3 s{1.0f, 1.0f, 1.0f};
	// Per-axis independent scaling (fixes legacy coupled X=Y=Z bug)
	s = applyScaleDelta(s, EditAxis::X, 0.5f, false);
	s = applyScaleDelta(s, EditAxis::Z, -0.3f, false);
	EXPECT(near(s.x, 1.5f) && near(s.y, 1.0f) && near(s.z, 0.7f));

	// Uniform scaling
	s = applyScaleDelta(s, EditAxis::Y, 0.2f, true);
	EXPECT(near(s.x, 1.7f) && near(s.y, 1.2f) && near(s.z, 0.9f));

	// Floor & ceiling clamps prevent zero/negative singular matrices or runaway scale
	const Vec3 floorClamped = applyScaleDelta(s, EditAxis::Z, -50.0f, false);
	EXPECT(near(floorClamped.z, 0.01f));
	EXPECT(near(floorClamped.x, 1.7f));

	const Vec3 ceilClamped = applyScaleDelta(s, EditAxis::X, 100.0f, false);
	EXPECT(near(ceilClamped.x, 20.0f));
}

void testGizmoHitTestAndPerspectiveDrag()
{
	std::array<GizmoScreenAxis, 3> axes{};
	axes[0] = {true, {500.0f, 500.0f}, {620.0f, 500.0f}, {440.0f, 500.0f}, 1.2f}; // +X right (120px = 1.2m)
	axes[1] = {true, {500.0f, 500.0f}, {500.0f, 600.0f}, {500.0f, 450.0f}, 1.2f}; // +Y down
	axes[2] = {true, {500.0f, 500.0f}, {500.0f, 380.0f}, {500.0f, 560.0f}, 1.2f}; // +Z up

	// Hit tip badges
	EXPECT(hitTestGizmoAxes(axes, 615.0f, 504.0f, 28.0f, 16.0f) == 0);
	EXPECT(hitTestGizmoAxes(axes, 502.0f, 595.0f, 28.0f, 16.0f) == 1);
	EXPECT(hitTestGizmoAxes(axes, 498.0f, 386.0f, 28.0f, 16.0f) == 2);

	// Hit shaft midway along +X
	EXPECT(hitTestGizmoAxes(axes, 560.0f, 506.0f, 28.0f, 16.0f) == 0);

	// Empty background misses cleanly so touch orbits camera
	EXPECT(hitTestGizmoAxes(axes, 220.0f, 220.0f, 28.0f, 16.0f) == -1);

	// 1:1 perspective drag at Normal gear: 60px along a 120px (1.2m) +X axis = +0.60m!
	const float moveNormal = computeGizmoDragDelta(EditMode::Move, axes[0], 60.0f, 25.0f, SpeedGear::Normal);
	EXPECT(near(moveNormal, 0.60f, 1e-3f));

	// Fine gear scales down to 0.15x (+0.09m), Fast gear scales up to 4.0x (+2.40m)
	const float moveFine = computeGizmoDragDelta(EditMode::Move, axes[0], 60.0f, 0.0f, SpeedGear::Fine);
	const float moveFast = computeGizmoDragDelta(EditMode::Move, axes[0], 60.0f, 0.0f, SpeedGear::Fast);
	EXPECT(near(moveFine, 0.09f, 1e-3f));
	EXPECT(near(moveFast, 2.40f, 1e-3f));

	// Dragging UP (-dy = -60px) along +Z (upward screen vector (0, -120)) yields positive +0.60m!
	const float moveUpZ = computeGizmoDragDelta(EditMode::Move, axes[2], 0.0f, -60.0f, SpeedGear::Normal);
	EXPECT(near(moveUpZ, 0.60f, 1e-3f));

	// Scrub track delta
	const float scrubDeg = computeScrubTrackDelta(EditMode::Rotate, 100.0f, 400.0f, SpeedGear::Normal);
	EXPECT(near(scrubDeg, 45.0f, 1e-3f));
}

} // namespace

int main()
{
	testEulerWrappingAndRotateDelta();
	testLocalVsWorldMoveDelta();
	testIndependentAndUniformScaleClamping();
	testGizmoHitTestAndPerspectiveDrag();

	if (g_failures > 0) {
		std::fprintf(stderr, "object_editor_core: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("object_editor_core: all tests passed!\n");
	return 0;
}
