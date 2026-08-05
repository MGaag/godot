/**************************************************************************/
/*  test_cell_light_probe.cpp                                             */
/**************************************************************************/

#include "tests/test_macros.h"

// Focused CPU contract tests for cell-light math and scheduling. Live CC tests
// cover RenderingDevice buffers and scene captures; these cases remain fast and
// isolate layer composition, deterministic priority, topology, and bent-cone SH.

TEST_FORCE_LINK(test_cell_light_probe)

#include "servers/rendering/renderer_rd/environment/cell_light_probe.h"

namespace TestCellLightProbe {

TEST_CASE("[RendererRD][CellLightProbe] Circuit layers resolve without recapture") {
	RendererRD::CellLightProbe probe;
	RendererRD::CellLightSH9 base;
	RendererRD::CellLightSH9 switched;
	base.clear();
	switched.clear();
	base.coefficients[0] = Vector3(1.0f, 2.0f, 3.0f);
	switched.coefficients[0] = Vector3(0.5f, 1.0f, 1.5f);

	probe.commit_layer(RendererRD::CELL_LIGHT_BASE_LAYER, base, 7);
	probe.commit_layer(42, switched, 8);

	HashMap<RendererRD::CellLightLayerID, RendererRD::CellLightCircuitState> circuits;
	circuits.insert(42, { false, 2.0f });
	probe.resolve_layers(circuits);
	CHECK(probe.get_resolved().coefficients[0].is_equal_approx(base.coefficients[0]));
	CHECK(probe.get_capture_generation() == 8);

	circuits[42].enabled = true;
	probe.resolve_layers(circuits);
	CHECK(probe.get_resolved().coefficients[0].is_equal_approx(Vector3(2.0f, 4.0f, 6.0f)));
	CHECK(probe.get_capture_generation() == 8);
	CHECK(probe.get_resolve_generation() == 2);
}

TEST_CASE("[RendererRD][CellLightProbe] Scheduler prioritizes visible impact deterministically") {
	RendererRD::CellLightProbeScheduler scheduler;
	scheduler.queue(Vector3i(5, 0, 0), 3, 0, 1, false);
	scheduler.queue(Vector3i(2, 0, 0), 1, 2, 2, true);
	scheduler.queue(Vector3i(1, 0, 0), 1, 0, 5, true);
	scheduler.queue(Vector3i(0, 0, 0), 1, 0, 5, true);

	// Re-queueing merges urgency rather than spending two captures on one layer.
	scheduler.queue(Vector3i(2, 0, 0), 1, 1, 1, true);
	CHECK(scheduler.get_pending_count() == 4);

	RendererRD::CellLightProbeUpdate update;
	REQUIRE(scheduler.pop_next(update));
	CHECK(update.cell == Vector3i(1, 0, 0));
	REQUIRE(scheduler.pop_next(update));
	CHECK(update.cell == Vector3i(0, 0, 0));
	REQUIRE(scheduler.pop_next(update));
	CHECK(update.cell == Vector3i(2, 0, 0));
	REQUIRE(scheduler.pop_next(update));
	CHECK(update.cell == Vector3i(5, 0, 0));
}

TEST_CASE("[RendererRD][CellLightProbe] Visible probe blending is continuous and rejects closed edges") {
	RendererRD::CellLightSH9 dark;
	RendererRD::CellLightSH9 bright;
	dark.clear();
	bright.clear();
	dark.coefficients[0] = Vector3(0.0f, 0.0f, 0.0f);
	bright.coefficients[0] = Vector3(4.0f, 2.0f, 1.0f);

	const RendererRD::CellLightSH9 *probes[] = { &dark, &bright };
	const float open_edges[] = { 1.0f, 1.0f };
	const float midpoint_weights[] = { 0.5f, 0.5f };
	RendererRD::CellLightSH9 midpoint = RendererRD::CellLightSH9::blend_visible(probes, midpoint_weights, open_edges, 2);
	CHECK(midpoint.coefficients[0].is_equal_approx(Vector3(2.0f, 1.0f, 0.5f)));

	const float near_boundary_weights[] = { 0.001f, 0.999f };
	RendererRD::CellLightSH9 near_boundary = RendererRD::CellLightSH9::blend_visible(probes, near_boundary_weights, open_edges, 2);
	CHECK(near_boundary.coefficients[0].is_equal_approx(Vector3(3.996f, 1.998f, 0.999f)));

	// Closing the bright cell's edge removes it and renormalizes the surviving
	// sample instead of introducing an artificial dark seam.
	const float closed_bright_edge[] = { 1.0f, 0.0f };
	RendererRD::CellLightSH9 blocked = RendererRD::CellLightSH9::blend_visible(probes, midpoint_weights, closed_bright_edge, 2);
	CHECK(blocked.coefficients[0].is_equal_approx(Vector3()));
}

TEST_CASE("[RendererRD][CellLightProbe] Bent-cone aperture windows directional SH bands") {
	RendererRD::CellLightSH9 irradiance;
	irradiance.clear();
	irradiance.coefficients[0] = Vector3(1.0f, 1.0f, 1.0f);
	irradiance.coefficients[3] = Vector3(1.0f, 0.0f, 0.0f);
	irradiance.coefficients[8] = Vector3(1.0f, 0.0f, 0.0f);

	const Vector3 narrow = irradiance.evaluate_bent_cone(Vector3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f);
	const Vector3 hemisphere = irradiance.evaluate_bent_cone(Vector3(1.0f, 0.0f, 0.0f), 1.0f, 0.0f);
	CHECK(narrow.x > hemisphere.x);
	CHECK(hemisphere.is_equal_approx(Vector3(0.5263965f, 0.282095f, 0.282095f)));
	CHECK(irradiance.evaluate_bent_cone(Vector3(1.0f, 0.0f, 0.0f), 0.0f, 1.0f).is_zero_approx());
}

} // namespace TestCellLightProbe
