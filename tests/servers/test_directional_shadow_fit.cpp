/**************************************************************************/
/*  test_directional_shadow_fit.cpp                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_directional_shadow_fit)

#include "servers/rendering/directional_shadow_fit.h"

// CPU-only coverage checks for receiver fitting. Rendering and streaming retain
// ownership of displaced instance bounds and residency, respectively.
namespace TestDirectionalShadowFit {

// Construct the production top-down camera without loading or rendering a world.
static Transform3D camera_transform() {
	return Transform3D(Basis(), Vector3(0, 51, 42)).looking_at(Vector3(), Vector3(0, 1, 0));
}

TEST_CASE("[DirectionalShadowFit] Empty camera depth does not dilute receiver resolution") {
	const Transform3D camera = camera_transform();
	const Vector2 heights(-2, 40);
	const Basis light = Basis::from_euler(Vector3(-0.9, 0.6, 0.0));
	for (real_t zoom : { 22.0f, 39.0f, 65.0f }) {
		Projection projection;
		projection.set_orthogonal(zoom, 1.6, 0.05, 180.0);
		const Vector<Vector3> points = DirectionalShadowFit::receiver_points(projection, camera, heights);
		REQUIRE(!points.is_empty());
		const AABB fit = DirectionalShadowFit::light_bounds(points, light);
		projection.set_orthogonal(zoom, 1.6, 0.05, 500.0);
		const AABB longer_fit = DirectionalShadowFit::light_bounds(DirectionalShadowFit::receiver_points(projection, camera, heights), light);
		CHECK(fit.position.is_equal_approx(longer_fit.position));
		CHECK(fit.size.is_equal_approx(longer_fit.size));
		for (int resolution : { 256, 2999, 4096, 4099, 8192 }) {
			const AABB stable = DirectionalShadowFit::stabilize(fit, resolution);
			for (const Vector3 &point : points) {
				const Vector3 local = light.transposed().xform(point);
				CHECK(local.x >= stable.position.x);
				CHECK(local.x <= stable.get_end().x);
				CHECK(local.y >= stable.position.y);
				CHECK(local.y <= stable.get_end().y);
			}
		}
	}
}

TEST_CASE("[DirectionalShadowFit] Lightward offscreen casters reach receivers without widening the map") {
	const Transform3D camera = camera_transform();
	Projection projection;
	projection.set_orthogonal(22.0, 1.6, 0.05, 180.0);
	const Vector<Plane> planes = projection.get_projection_planes(camera);
	for (real_t yaw : { 0.0f, 1.5f, 3.0f }) {
		const Basis light = Basis::from_euler(Vector3(-0.9, yaw, 0));
		const Vector3 lightward = light.get_column(2);
		const Vector3 receiver(0, 0, 0);
		const Vector3 caster = receiver + lightward * 100;
		bool outside_camera = false;
		for (const Plane &plane : planes) {
			outside_camera |= plane.distance_to(caster) > 0;
		}
		CHECK(outside_camera);
		const Vector3 local_receiver = light.transposed().xform(receiver);
		const AABB receivers(local_receiver - Vector3(2, 2, 2), Vector3(4, 4, 4));
		const AABB caster_bounds(light.transposed().xform(caster) - Vector3(1, 1, 1), Vector3(2, 2, 2));
		CHECK(DirectionalShadowFit::caster_reaches_receivers(caster_bounds, receivers));
		CHECK_FALSE(DirectionalShadowFit::caster_reaches_receivers(AABB(local_receiver - Vector3(1, 1, 20), Vector3(2, 2, 2)), receivers));
		CHECK_FALSE(DirectionalShadowFit::caster_reaches_receivers(AABB(local_receiver + Vector3(10, 0, 20), Vector3(2, 2, 2)), receivers));
	}
}


// Panning and scene residency changes cannot alter the footprint of a fixed camera/slab.
TEST_CASE("[DirectionalShadowFit] World-height fitting keeps map scale stable while panning") {
	Projection projection;
	projection.set_orthogonal(39, 1.6, 0.05, 180);
	const Basis light = Basis::from_euler(Vector3(-0.9, 0.6, 0));
	const Vector2 heights(-2, 45);
	Transform3D camera = camera_transform();
	const AABB baseline = DirectionalShadowFit::light_bounds(DirectionalShadowFit::receiver_points(projection, camera, heights), light);
	for (const Vector3 offset : { Vector3(0.13, 0, 0.27), Vector3(20, 0, -30), Vector3(-127, 0, 149) }) {
		Transform3D moved = camera;
		moved.origin += offset;
		const AABB fit = DirectionalShadowFit::light_bounds(DirectionalShadowFit::receiver_points(projection, moved, heights), light);
		real_t baseline_depth, moved_depth;
		REQUIRE(DirectionalShadowFit::lightward_depth_limit(baseline, light, heights, baseline_depth));
		REQUIRE(DirectionalShadowFit::lightward_depth_limit(fit, light, heights, moved_depth));
		CHECK(Math::is_equal_approx(baseline_depth - baseline.position.z, moved_depth - fit.position.z));
		CHECK(fit.size.is_equal_approx(baseline.size));
		CHECK(fit.position.is_equal_approx(baseline.position + light.transposed().xform(offset)));
	}
}

} // namespace TestDirectionalShadowFit
