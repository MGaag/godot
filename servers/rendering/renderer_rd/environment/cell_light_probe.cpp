/**************************************************************************/
/*  cell_light_probe.cpp                                                  */
/**************************************************************************/

#include "cell_light_probe.h"

namespace RendererRD {

void CellLightSH9::clear() {
	for (Vector3 &coefficient : coefficients) {
		coefficient = Vector3();
	}
}

void CellLightSH9::add_scaled(const CellLightSH9 &p_other, float p_scale) {
	for (uint32_t i = 0; i < COEFFICIENT_COUNT; i++) {
		coefficients[i] += p_other.coefficients[i] * p_scale;
	}
}

bool CellLightSH9::is_equal_approx(const CellLightSH9 &p_other) const {
	for (uint32_t i = 0; i < COEFFICIENT_COUNT; i++) {
		if (!coefficients[i].is_equal_approx(p_other.coefficients[i])) {
			return false;
		}
	}
	return true;
}

Vector3 CellLightSH9::evaluate_bent_cone(const Vector3 &p_direction, float p_visibility, float p_aperture_cosine) const {
	const Vector3 direction = p_direction.normalized();
	const float x = direction.x;
	const float y = direction.y;
	const float z = direction.z;

	// A hemisphere is the widest supported visibility cone. Its low-pass
	// window removes the second-order band; progressively narrower cones retain
	// more directional information from the probe.
	const float aperture_cosine = CLAMP(p_aperture_cosine, 0.0f, 1.0f);
	const float band_1_window = 0.5f * (1.0f + aperture_cosine);
	const float band_2_window = 0.5f * aperture_cosine * (1.0f + aperture_cosine);

	Vector3 result = coefficients[0] * 0.282095f;
	result += coefficients[1] * (0.488603f * y * band_1_window);
	result += coefficients[2] * (0.488603f * z * band_1_window);
	result += coefficients[3] * (0.488603f * x * band_1_window);
	result += coefficients[4] * (1.092548f * x * y * band_2_window);
	result += coefficients[5] * (1.092548f * y * z * band_2_window);
	result += coefficients[6] * (0.315392f * (3.0f * z * z - 1.0f) * band_2_window);
	result += coefficients[7] * (1.092548f * x * z * band_2_window);
	result += coefficients[8] * (0.546274f * (x * x - y * y) * band_2_window);

	return result * CLAMP(p_visibility, 0.0f, 1.0f);
}

CellLightSH9 CellLightSH9::blend_visible(const CellLightSH9 *const *p_probes, const float *p_geometric_weights, const float *p_transmissibility, uint32_t p_count) {
	CellLightSH9 result;
	result.clear();

	float total_weight = 0.0f;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_probes[i] == nullptr) {
			continue;
		}

		const float weight = MAX(p_geometric_weights[i], 0.0f) * CLAMP(p_transmissibility[i], 0.0f, 1.0f);
		if (weight <= 0.0f) {
			continue;
		}

		result.add_scaled(*p_probes[i], weight);
		total_weight += weight;
	}

	if (total_weight > 0.0f) {
		for (Vector3 &coefficient : result.coefficients) {
			coefficient /= total_weight;
		}
	}

	return result;
}

void CellLightProbe::set_cell(const Vector3i &p_cell) {
	cell = p_cell;
}

const Vector3i &CellLightProbe::get_cell() const {
	return cell;
}

void CellLightProbe::commit_layer(CellLightLayerID p_layer, const CellLightSH9 &p_irradiance, uint64_t p_capture_generation) {
	layers.insert(p_layer, p_irradiance);
	capture_generation = MAX(capture_generation, p_capture_generation);
}

bool CellLightProbe::has_layer(CellLightLayerID p_layer) const {
	return layers.has(p_layer);
}

const CellLightSH9 *CellLightProbe::get_layer(CellLightLayerID p_layer) const {
	return layers.getptr(p_layer);
}

void CellLightProbe::resolve_layers(const HashMap<CellLightLayerID, CellLightCircuitState> &p_circuits) {
	resolved.clear();

	if (const CellLightSH9 *base = layers.getptr(CELL_LIGHT_BASE_LAYER)) {
		resolved = *base;
	}

	for (const KeyValue<CellLightLayerID, CellLightCircuitState> &circuit : p_circuits) {
		if (!circuit.value.enabled || circuit.value.energy <= 0.0f) {
			continue;
		}

		if (const CellLightSH9 *contribution = layers.getptr(circuit.key)) {
			resolved.add_scaled(*contribution, circuit.value.energy);
		}
	}

	resolve_generation++;
}

const CellLightSH9 &CellLightProbe::get_resolved() const {
	return resolved;
}

uint64_t CellLightProbe::get_capture_generation() const {
	return capture_generation;
}

uint64_t CellLightProbe::get_resolve_generation() const {
	return resolve_generation;
}

bool CellLightProbeScheduler::_is_higher_priority(const CellLightProbeUpdate &p_left, const CellLightProbeUpdate &p_right) {
	if (p_left.active_layer != p_right.active_layer) {
		return p_left.active_layer;
	}
	if (p_left.graph_distance != p_right.graph_distance) {
		return p_left.graph_distance < p_right.graph_distance;
	}
	if (p_left.player_distance != p_right.player_distance) {
		return p_left.player_distance < p_right.player_distance;
	}
	return p_left.insertion_order < p_right.insertion_order;
}

void CellLightProbeScheduler::queue(const Vector3i &p_cell, CellLightLayerID p_layer, uint32_t p_graph_distance, uint32_t p_player_distance, bool p_active_layer) {
	for (CellLightProbeUpdate &update : pending) {
		if (update.cell == p_cell && update.layer == p_layer) {
			update.graph_distance = MIN(update.graph_distance, p_graph_distance);
			update.player_distance = MIN(update.player_distance, p_player_distance);
			update.active_layer = update.active_layer || p_active_layer;
			return;
		}
	}

	CellLightProbeUpdate update;
	update.cell = p_cell;
	update.layer = p_layer;
	update.graph_distance = p_graph_distance;
	update.player_distance = p_player_distance;
	update.active_layer = p_active_layer;
	update.insertion_order = next_insertion_order++;
	pending.push_back(update);
}

bool CellLightProbeScheduler::pop_next(CellLightProbeUpdate &r_update) {
	if (pending.is_empty()) {
		return false;
	}

	uint32_t best_index = 0;
	for (uint32_t i = 1; i < (uint32_t)pending.size(); i++) {
		if (_is_higher_priority(pending[i], pending[best_index])) {
			best_index = i;
		}
	}

	r_update = pending[best_index];
	pending.remove_at(best_index);
	return true;
}

uint32_t CellLightProbeScheduler::get_pending_count() const {
	return pending.size();
}

void CellLightProbeScheduler::clear() {
	pending.clear();
}

} // namespace RendererRD
