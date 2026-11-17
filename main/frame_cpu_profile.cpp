/**************************************************************************/
/*  frame_cpu_profile.cpp                                               */
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

// Nested scopes pause their parents, so the rows partition complete main-thread frames.
// Worker/render-thread execution is excluded from this additive wall-time timeline.
#include "frame_cpu_profile.h"

#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/vector.h"

#include <atomic>
namespace {
std::atomic<bool> enabled = false;
bool recording = false;
uint64_t generation = 1, stamp = 0, start = 0, previous_end = 0, frame_elapsed = 0;
uint64_t total_usec = 0, peak_usec = 0, frames = 0;
Vector<String> stack;
Dictionary frame_rows, rows;
void charge(uint64_t p_now) {
	if (!stack.is_empty()) {
		Dictionary row = frame_rows[stack[stack.size() - 1]];
		row["usec"] = int64_t(row["usec"]) + int64_t(p_now - stamp);
	}
	stamp = p_now;
}
void add(const String &p_name, uint64_t p_usec, uint64_t p_calls) {
	if (!frame_rows.has(p_name)) {
		Dictionary row;
		row["usec"] = int64_t(0);
		row["calls"] = int64_t(0);
		frame_rows[p_name] = row;
	}
	Dictionary row = frame_rows[p_name];
	row["usec"] = int64_t(row["usec"]) + p_usec;
	row["calls"] = int64_t(row["calls"]) + p_calls;
}
} //namespace
void FrameCPUProfile::set_enabled(bool p_enabled) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	enabled = p_enabled;
	recording = false;
	generation++;
	previous_end = 0;
	stack.clear();
	frame_rows.clear();
	rows.clear();
	frames = 0;
	total_usec = 0;
	peak_usec = 0;
}
bool FrameCPUProfile::active() {
	return enabled.load(std::memory_order_relaxed) && Thread::is_main_thread() && recording;
}
uint64_t FrameCPUProfile::begin_frame() {
	if (!enabled) {
		return 0;
	}
	start = OS::get_singleton()->get_ticks_usec();
	stamp = start;
	frame_elapsed = previous_end ? start - previous_end : 0;
	frame_rows = Dictionary();
	add("OS events / between frames", frame_elapsed, 1);
	add("Unknown", 0, 1);
	stack.clear();
	stack.push_back("Unknown");
	recording = true;
	return generation;
}
void FrameCPUProfile::end_frame(uint64_t p_generation) {
	if (!p_generation || p_generation != generation || !recording) {
		return;
	}
	uint64_t now = OS::get_singleton()->get_ticks_usec();
	charge(now);
	frame_elapsed += now - start;
	previous_end = now;
	recording = false;
	stack.clear();
	frames++;
	total_usec += frame_elapsed;
	peak_usec = MAX(peak_usec, frame_elapsed);
	for (const Variant &name : frame_rows.keys()) {
		Dictionary sample = frame_rows[name];
		if (!rows.has(name)) {
			Dictionary row;
			row["usec"] = int64_t(0);
			row["calls"] = int64_t(0);
			row["peak"] = int64_t(0);
			rows[name] = row;
		}
		Dictionary row = rows[name];
		row["usec"] = int64_t(row["usec"]) + int64_t(sample["usec"]);
		row["calls"] = int64_t(row["calls"]) + int64_t(sample["calls"]);
		row["peak"] = MAX(int64_t(row["peak"]), int64_t(sample["usec"]));
	}
}
uint64_t FrameCPUProfile::push(const String &p_name) {
	if (!active()) {
		return 0;
	}
	charge(OS::get_singleton()->get_ticks_usec());
	add(p_name, 0, 1);
	stack.push_back(p_name);
	return generation;
}
void FrameCPUProfile::pop(uint64_t p_generation) {
	if (!p_generation || p_generation != generation || !recording) {
		return;
	}
	charge(OS::get_singleton()->get_ticks_usec());
	if (stack.size() > 1) {
		stack.resize(stack.size() - 1);
	}
}
Dictionary FrameCPUProfile::take() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), Dictionary());
	Dictionary result;
	result["frames"] = int64_t(frames);
	result["usec"] = int64_t(total_usec);
	result["peak"] = int64_t(peak_usec);
	result["rows"] = rows;
	frames = 0;
	total_usec = 0;
	peak_usec = 0;
	rows = Dictionary();
	return result;
}
