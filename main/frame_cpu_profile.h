/**************************************************************************/
/*  frame_cpu_profile.h                                                 */
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

// Opt-in exclusive main-thread frame wall timings, including engine work and waits.
#pragma once
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
class FrameCPUProfile {
public:
	static void set_enabled(bool p_enabled);
	static bool active();
	static Dictionary take();
	static uint64_t begin_frame();
	static void end_frame(uint64_t p_generation);
	static uint64_t push(const String &p_name);
	static void pop(uint64_t p_generation);
	class Frame {
		uint64_t generation;

	public:
		Frame() : generation(begin_frame()) {}
		~Frame() { end_frame(generation); }
	};
	class Scope {
		uint64_t generation;

	public:
		Scope(const char *p_name) : generation(active() ? push(p_name) : 0) {}
		~Scope() { pop(generation); }
		void next(const char *p_name) {
			pop(generation);
			generation = active() ? push(p_name) : 0;
		}
	};
};
