/**************************************************************************/
/*  directional_shadow_fit.h                                              */
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

#pragma once

#include "core/math/geometry_3d.h"
#include "core/math/projection.h"

// Geometry for receiver-fitted orthographic directional shadows. Scene culling owns
// receiver/caster selection; this helper clips their bounds and stabilizes the map.
namespace DirectionalShadowFit {

// Intersect the camera with the fixed world-height slab, independent of scene occupancy.
inline Vector<Vector3> receiver_points(const Projection &p_projection, const Transform3D &p_camera, const Vector2 &p_heights) {
	Vector<Plane> planes = p_projection.get_projection_planes(p_camera);
	planes.push_back(Plane(Vector3(0, 1, 0), p_heights.y));
	planes.push_back(Plane(Vector3(0, -1, 0), -p_heights.x));
	return Geometry3D::compute_convex_mesh_points(planes.ptr(), planes.size());
}

// Sweep the camera/slab intersection toward the light. Supporting edge planes
// preserve its silhouette instead of replacing it with a light-space rectangle.
// Outward plane distances use world meters, including filter and user padding.
inline Vector<Plane> caster_query(const Projection &p_projection, const Transform3D &p_camera, const Vector<Vector3> &p_receivers, const Vector3 &p_lightward, const Vector2 &p_heights, real_t p_padding) {
	Vector<Plane> result;
	if (p_receivers.is_empty()) {
		return result;
	}
	const real_t epsilon = (real_t)0.0001;
	auto append_plane = [&](Plane p_plane) {
		p_plane.normalize();
		for (const Plane &existing : result) {
			if (existing.normal.dot(p_plane.normal) > (real_t)0.999999 && Math::abs(existing.d - p_plane.d) < epsilon) {
				return;
			}
		}
		result.push_back(p_plane);
	};
	Vector<Plane> receiver_planes = p_projection.get_projection_planes(p_camera);
	receiver_planes.push_back(Plane(Vector3(0, 1, 0), p_heights.y));
	receiver_planes.push_back(Plane(Vector3(0, -1, 0), -p_heights.x));
	for (Plane plane : receiver_planes) {
		if (plane.normal.dot(p_lightward) <= (real_t)0.000001) {
			plane.d += p_padding;
			append_plane(plane);
		}
	}
	// Every silhouette edge supports the projected receiver hull. Considering all
	// pairs avoids topology assumptions when camera near/far planes clip the slab.
	for (int a = 0; a < p_receivers.size(); a++) {
		for (int b = a + 1; b < p_receivers.size(); b++) {
			Vector3 normal = (p_receivers[b] - p_receivers[a]).cross(p_lightward);
			if (normal.length_squared() < epsilon * epsilon) {
				continue;
			}
			normal.normalize();
			Plane plane(normal, normal.dot(p_receivers[a]));
			real_t low = 0, high = 0;
			for (const Vector3 &point : p_receivers) {
				const real_t distance = plane.distance_to(point);
				low = MIN(low, distance);
				high = MAX(high, distance);
			}
			if (low < -epsilon && high > epsilon) {
				continue;
			}
			if (high > epsilon) {
				plane = -plane;
			}
			// Max support prevents floating-point tolerance from clipping a vertex.
			for (const Vector3 &point : p_receivers) {
				plane.d = MAX(plane.d, plane.normal.dot(point));
			}
			plane.d += p_padding;
			append_plane(plane);
		}
	}
	// The world envelope bounds lightward travel, including a horizontal sun.
	append_plane(Plane(Vector3(0, 1, 0), p_heights.y + p_padding));
	append_plane(Plane(Vector3(0, -1, 0), -p_heights.x + p_padding));
	return result;
}

// Project receiver vertices into the light's orthonormal basis, independent of its origin.
inline AABB light_bounds(const Vector<Vector3> &p_points, const Basis &p_light_basis) {
	const Basis world_to_light = p_light_basis.transposed();
	AABB bounds(world_to_light.xform(p_points[0]), Vector3());
	for (int i = 1; i < p_points.size(); i++) {
		bounds.expand_to(world_to_light.xform(p_points[i]));
	}
	return bounds;
}

// Extend every receiver ray to the world's lightward height boundary. Unlike
// resident caster bounds, this gives a stable depth span and stable depth bias.
// A horizontal light has no finite height intersection and uses scene caster bounds.
inline bool lightward_depth_limit(const AABB &p_receivers, const Basis &p_light_basis, const Vector2 &p_heights, real_t &r_depth) {
	const real_t vertical = p_light_basis.get_column(2).y;
	if (Math::abs(vertical) < (real_t)0.00001) {
		return false;
	}
	const real_t height = vertical > 0 ? p_heights.y : p_heights.x;
	r_depth = p_receivers.get_end().z;
	for (int corner = 0; corner < 4; corner++) {
		const real_t x = p_receivers.position.x + ((corner & 1) ? p_receivers.size.x : 0);
		const real_t y = p_receivers.position.y + ((corner & 2) ? p_receivers.size.y : 0);
		const real_t depth = (height - p_light_basis.get_column(0).y * x - p_light_basis.get_column(1).y * y) / vertical;
		r_depth = MAX(r_depth, depth);
	}
	return true;
}

// Preserve offscreen casters whose light-space footprint reaches a visible receiver.
inline bool caster_reaches_receivers(const AABB &p_caster, const AABB &p_receivers) {
	const Vector3 caster_end = p_caster.get_end();
	const Vector3 receiver_end = p_receivers.get_end();
	return caster_end.x >= p_receivers.position.x && p_caster.position.x <= receiver_end.x &&
			caster_end.y >= p_receivers.position.y && p_caster.position.y <= receiver_end.y &&
			caster_end.z >= p_receivers.position.z;
}

// Reserve six texels at either edge, then snap the center without changing the extent.
// The half-texel rounding displacement stays inside that guard, including fractional atlas sizes.
inline AABB stabilize(const AABB &p_bounds, int p_resolution) {
	AABB result = p_bounds;
	for (int axis = 0; axis < 2; axis++) {
		const real_t extent = MAX(p_bounds.size[axis], (real_t)0.001) * p_resolution / MAX(p_resolution - 12, 1);
		const real_t texel = extent / p_resolution;
		const real_t center = Math::snapped(p_bounds.position[axis] + p_bounds.size[axis] * 0.5, texel);
		result.position[axis] = center - extent * 0.5;
		result.size[axis] = extent;
	}
	return result;
}

} // namespace DirectionalShadowFit
