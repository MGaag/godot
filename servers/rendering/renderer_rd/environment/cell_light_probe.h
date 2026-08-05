/**************************************************************************/
/*  cell_light_probe.h                                                    */
/**************************************************************************/

#ifndef CELL_LIGHT_PROBE_H
#define CELL_LIGHT_PROBE_H

#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

namespace RendererRD {

// CPU-side model and scheduling primitives for cell-probe indirect lighting.
// RendererSceneRenderRD owns capture jobs, CellLightProbeConvolver converts each
// cubemap to SH9, and CellLightProbeGridRD publishes the resolved GPU lattice.
// None of these types know about CC rooms, doors, or ship modules.

// Stores diffuse irradiance in nine coefficients: three spherical-harmonic
// bands through second order. This compact representation is the persistent
// output of a transient cubemap capture.
struct CellLightSH9 {
	static constexpr uint32_t COEFFICIENT_COUNT = 9;

	Vector3 coefficients[COEFFICIENT_COUNT];

	void clear();
	void add_scaled(const CellLightSH9 &p_other, float p_scale);
	bool is_equal_approx(const CellLightSH9 &p_other) const;
	Vector3 evaluate_bent_cone(const Vector3 &p_direction, float p_visibility, float p_aperture_cosine) const;

	// Combines grid neighbors after edge and directional visibility have been
	// applied. Renormalization preserves a continuous field wherever the set of
	// compatible probes is unchanged and avoids darkening beside missing cells.
	static CellLightSH9 blend_visible(const CellLightSH9 *const *p_probes, const float *p_geometric_weights, const float *p_transmissibility, uint32_t p_count);
};

// Identifies a separately captured lighting state. Layer zero is reserved for
// the always-present environment and permanent lights; other IDs are supplied
// by the game for switchable circuits.
using CellLightLayerID = uint32_t;
static constexpr CellLightLayerID CELL_LIGHT_BASE_LAYER = 0;

struct CellLightCircuitState {
	// Circuits start dark until game code explicitly enables their stored bounce.
	bool enabled = false;
	// Unit energy preserves the captured contribution unless gameplay requests a
	// deliberate indirect-only multiplier.
	float energy = 1.0f;
};

// Owns the last complete result for one navigable cell. Captures write layer
// data independently, while resolve_layers() cheaply combines circuit states
// without recapturing the scene.
class CellLightProbe {
	Vector3i cell;
	HashMap<CellLightLayerID, CellLightSH9> layers;
	CellLightSH9 resolved;
	uint64_t capture_generation = 0;
	uint64_t resolve_generation = 0;

public:
	// Associates this record with a logical cell; it does not move GPU data.
	void set_cell(const Vector3i &p_cell);
	const Vector3i &get_cell() const;

	// Atomically replaces one completed layer and advances the capture generation.
	void commit_layer(CellLightLayerID p_layer, const CellLightSH9 &p_irradiance, uint64_t p_capture_generation);
	bool has_layer(CellLightLayerID p_layer) const;
	const CellLightSH9 *get_layer(CellLightLayerID p_layer) const;

	// Rebuilds the visible SH record from the base plus enabled circuit deltas.
	void resolve_layers(const HashMap<CellLightLayerID, CellLightCircuitState> &p_circuits);
	const CellLightSH9 &get_resolved() const;
	uint64_t get_capture_generation() const;
	uint64_t get_resolve_generation() const;
};

// A dirty request is intentionally independent from scene topology. Godot
// owns deterministic ordering and capture budgets; the game decides which
// cells become dirty by traversing its ship connectivity graph.
struct CellLightProbeUpdate {
	Vector3i cell;
	CellLightLayerID layer = CELL_LIGHT_BASE_LAYER;
	// Zero means the edited cell itself; larger values are progressively less
	// urgent according to the game-provided connectivity graph.
	uint32_t graph_distance = 0;
	// Zero means nearest the player's focus and therefore highest visual impact.
	uint32_t player_distance = 0;
	// Active layers sort ahead of hidden circuit layers at equal distances.
	bool active_layer = true;
	// FIFO order makes otherwise identical requests deterministic across runs.
	uint64_t insertion_order = 0;
};

class CellLightProbeScheduler {
	Vector<CellLightProbeUpdate> pending;
	uint64_t next_insertion_order = 0;

	static bool _is_higher_priority(const CellLightProbeUpdate &p_left, const CellLightProbeUpdate &p_right);

public:
	// Adds or upgrades a cell/layer request; duplicates never consume two captures.
	void queue(const Vector3i &p_cell, CellLightLayerID p_layer, uint32_t p_graph_distance, uint32_t p_player_distance, bool p_active_layer);
	// Removes the highest-impact request according to the documented stable order.
	bool pop_next(CellLightProbeUpdate &r_update);
	uint32_t get_pending_count() const;
	void clear();
};

} // namespace RendererRD

#endif // CELL_LIGHT_PROBE_H
