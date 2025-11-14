/**************************************************************************/
/*  texture_streaming.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_method_pointer.h"
#include "core/os/os.h"
#include "core/templates/rid.h"
#include "core/typedefs.h"
#include "core/variant/callable.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering_server.h"
#include <cstdint>
#include <cstdio>

#include "texture_streaming.h"

// Helper macros for code outside of the rendering server, but that is
// called by the rendering server.
// #ifdef DEBUG_ENABLED
#define ERR_NOT_ON_RENDER_THREAD                                          \
	RenderingServer *rendering_server = RenderingServer::get_singleton(); \
	ERR_FAIL_NULL(rendering_server);                                      \
	ERR_FAIL_COND(!rendering_server->is_on_render_thread());
#define ERR_NOT_ON_RENDER_THREAD_V(m_ret)                                 \
	RenderingServer *rendering_server = RenderingServer::get_singleton(); \
	ERR_FAIL_NULL_V(rendering_server, m_ret);                             \
	ERR_FAIL_COND_V(!rendering_server->is_on_render_thread(), m_ret);
// #else
// #define ERR_NOT_ON_RENDER_THREAD
// #define ERR_NOT_ON_RENDER_THREAD_V(m_ret)
// #endif

TextureStreaming *TextureStreaming::singleton = nullptr;

void TextureStreaming::MaterialFeedbackBuffer::cleanup() {
	if (buffer.is_valid()) {
		RD::get_singleton()->free(buffer);
	}

	buffer = RID();
	buffer_size = 0;
	rid_map.clear();
}

void TextureStreaming::MaterialFeedbackBuffer::clear() {
	ERR_NOT_ON_RENDER_THREAD;

	frame = 0;
	rid_map.clear();

	RD::get_singleton()->buffer_clear(buffer, 0, buffer_size);
}

void TextureStreaming::MaterialFeedbackBuffer::resize() {
	cleanup();

	uint32_t material_info_count = TextureStreaming::get_singleton()->material_info_owner.get_count() * 4;

	buffer_size = nearest_power_of_2_templated(MAX(4096ull, material_info_count));

	buffer = RD::get_singleton()->storage_buffer_create(buffer_size * sizeof(uint32_t), Vector<uint8_t>(), 0, RD::BufferCreationBits::BUFFER_CREATION_AS_STORAGE_BIT);

	clear();
}

TextureStreaming::MaterialFeedbackBuffer::MaterialFeedbackBuffer() :
		list_element(this) {}

TextureStreaming::MaterialFeedbackBuffer::MaterialFeedbackBuffer(MaterialFeedbackBuffer const &p_other) :
		list_element(this) {
	buffer = p_other.buffer;
	buffer_size = p_other.buffer_size;
	rid_map = p_other.rid_map;
	self = p_other.self;
}

TextureStreaming *TextureStreaming::get_singleton() {
	return singleton;
}

uint64_t TextureStreaming::get_memory_budget_bytes_used() {
	return texture_streaming_total_memory.load();
}

void TextureStreaming::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_memory_budget_bytes_used"), &TextureStreaming::get_memory_budget_bytes_used);
}

RID TextureStreaming::texture_configure_streaming(RID p_texture, Image::Format format, int width, int height, int p_min_resolution, int p_max_resolution, Callable p_reload_callable) {
	ERR_NOT_ON_RENDER_THREAD_V(RID());
	ERR_FAIL_COND_V(p_reload_callable.is_null(), RID());

	RID rid = streaming_info_owner.allocate_rid();
	StreamingState state;
	state.texture = p_texture;
	state.format = format;
	state.width = width;
	state.height = height;
	state.min_resolution = CLAMP(p_min_resolution, 0, 8192);
	state.max_resolution = CLAMP(p_max_resolution, 0, 8192);
	state.reload_callable = p_reload_callable;
	streaming_info_owner.initialize_rid(rid, state);

	RendererRD::TextureStorage::get_singleton()->texture_2d_attach_streaming_state(p_texture, rid);

	return rid;
}

void TextureStreaming::texture_remove(RID p_rid) {
	ERR_NOT_ON_RENDER_THREAD;
	StreamingState *state = streaming_info_owner.get_or_null(p_rid);
	RendererRD::TextureStorage::get_singleton()->texture_2d_attach_streaming_state(state->texture, RID());
	streaming_info_owner.free(p_rid);
}

void TextureStreaming::texture_update(RID p_rid, int width, int height, int p_min_resolution, int p_max_resolution) {
	// THREAD SAFETY: This function can be called from any thread.
	StreamingState *state = streaming_info_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(state);

	if (state) {
		state->width = width;
		state->height = height;
		state->min_resolution = CLAMP(p_min_resolution, 0, 8192);
		state->max_resolution = CLAMP(p_max_resolution, 0, 8192);
	}
}

RID TextureStreaming::material_set_textures(RID p_feeback_rid, Vector<RID> &p_textures) {
	ERR_NOT_ON_RENDER_THREAD_V(RID());

	MaterialInfo3 *info = nullptr;
	if (p_feeback_rid.is_null()) {
		info = material_info_owner.allocate(p_feeback_rid);
	} else {
		info = material_info_owner.get_or_null(p_feeback_rid);
	}

	ERR_FAIL_NULL_V(info, RID());

	if (p_textures.is_empty()) {
		material_info_owner.free(p_feeback_rid);
		return RID();
	}

	info->textures = p_textures;

	return p_feeback_rid;
}

void TextureStreaming::_settings_changed() {
	setting_texture_change_idle_msec = GLOBAL_GET("rendering/textures/streaming/idle_time");
	setting_texture_change_wait_msec = 100ull; //GLOBAL_GET("rendering/textures/streaming/wait_time");
	setting_texture_min_resolution = 1 << int(GLOBAL_GET("rendering/textures/streaming/default_min_dimension"));
	setting_texture_max_resolution = 1 << int(GLOBAL_GET("rendering/textures/streaming/default_max_dimension"));
	setting_streaming_is_enabled = GLOBAL_GET("rendering/textures/streaming/enabled");
	setting_budget_enabled = GLOBAL_GET("rendering/textures/streaming/memory_budget_enabled");
	setting_budget_mb = GLOBAL_GET("rendering/textures/streaming/memory_budget_mb");
}

TextureStreaming::TextureStreaming() {
	singleton = this;

	GLOBAL_DEF_RST("rendering/textures/streaming/enabled", false);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/textures/streaming/initial_size", PROPERTY_HINT_ENUM, "1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192"), 13);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/textures/streaming/default_max_dimension", PROPERTY_HINT_ENUM, "1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192"), 13);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/textures/streaming/default_min_dimension", PROPERTY_HINT_ENUM, "1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192"), 5);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/textures/streaming/idle_time"), 10000);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/textures/streaming/wait_time"), 100);
	GLOBAL_DEF("rendering/textures/streaming/memory_budget_enabled", true);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/textures/streaming/memory_budget_mb"), 512);

	_settings_changed();
}

TextureStreaming::~TextureStreaming() {
	// Stop processing thread
	singleton = nullptr;

	{
		MutexLock lock(feedback_buffer_mutex);
		feedback_buffer_thread_exit = true;
		feedback_buffer_condvar.notify_one();
	}

	{
		MutexLock lock(texture_reload_mutex);
		texture_reload_thread_exit = true;
		texture_reload_condvar.notify_one();
	}

	feedback_buffer_thread.wait_to_finish();
	texture_reload_thread.wait_to_finish();

	// Clean up buffer pool
	{
		MutexLock lock(buffer_pool_mutex);
		LocalVector<RID> buffers = feedback_buffer_owner.get_owned_list();
		for (uint32_t i = 0; i < buffers.size(); i++) {
			MaterialFeedbackBuffer *mb = feedback_buffer_owner.get_or_null(buffers[i]);
			mb->cleanup();
			feedback_buffer_owner.free(buffers[i]);
		}
		buffer_pool.clear();
	}
}

void TextureStreaming::late_init() {
	ERR_NOT_ON_RENDER_THREAD;
	// MutexLock lock(buffer_pool_mutex);

	const bool streaming_enabled = GLOBAL_GET("rendering/textures/streaming/enabled");
	if (!streaming_enabled) {
		return;
	}

	const String rendering_method = OS::get_singleton()->get_current_rendering_method();
	if (rendering_method == "gl_compatibility") {
		WARN_PRINT("Texture streaming is not supported with the Compatibility renderer.");
		return;
	}

	const uint32_t num_materials = RendererRD::MaterialStorage::get_singleton()->num_materials();
	m_current_feedback_buffer = feedback_buffer_get(num_materials);

	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &TextureStreaming::_settings_changed));
	_settings_changed();

	RenderingServer::get_singleton()->connect("frame_post_draw", callable_mp(this, &TextureStreaming::feedback_frame_done_callback));

	feedback_buffer_thread_exit = false;
	feedback_buffer_thread.start(_feedback_buffer_thread_func, this);

	texture_reload_thread_exit = false;
	texture_reload_thread.start(_texture_reload_thread_func, this);
}

void TextureStreaming::feedback_frame_done_callback() {
	if (m_current_feedback_buffer.is_valid()) {
		const uint64_t frame = Engine::get_singleton()->get_frames_drawn();
		feedback_buffer_submit(m_current_feedback_buffer, frame);
	}

	const uint32_t num_materials = RendererRD::MaterialStorage::get_singleton()->num_materials();
	m_current_feedback_buffer = feedback_buffer_get(num_materials);
}

RID TextureStreaming::feedback_buffer_get_uniform_rid() {
	ERR_NOT_ON_RENDER_THREAD_V(RID());
	MaterialFeedbackBuffer *_buffer = feedback_buffer_owner.get_or_null(m_current_feedback_buffer);
	if (_buffer && _buffer->buffer.is_valid() && _buffer->buffer_size > 0) {
		return _buffer->buffer;
	}

	return RID();
}

RID TextureStreaming::feedback_buffer_get(uint32_t p_num_materials) {
	ERR_NOT_ON_RENDER_THREAD_V(RID());
	MutexLock lock(buffer_pool_mutex);

	RID buffer = RID();
	if (buffer_pool.size() > 0) {
		// Reuse a buffer from the pool
		buffer = buffer_pool[buffer_pool.size() - 1];
		buffer_pool.remove_at(buffer_pool.size() - 1);
	} else if (buffer_count < 4) {
		buffer = feedback_buffer_owner.allocate_rid();
		MaterialFeedbackBuffer materialFeedbackBuffer;
		materialFeedbackBuffer.buffer = RID();
		materialFeedbackBuffer.buffer_size = 0;
		materialFeedbackBuffer.rid_map.clear();
		materialFeedbackBuffer.self = buffer;

		feedback_buffer_owner.initialize_rid(buffer, materialFeedbackBuffer);
		buffer_count++;
	}

	if (buffer.is_valid()) {
		MaterialFeedbackBuffer *mb = feedback_buffer_owner.get_or_null(buffer);
		ERR_FAIL_NULL_V(mb, RID());
		mb->rid_map.reserve(p_num_materials);
		mb->resize();
		// mb->rid_map.resize(material_info_owner.get_rid_count());
	}

	return buffer;
}

uint32_t TextureStreaming::feedback_buffer_material_index(RID p_material) {
	ERR_NOT_ON_RENDER_THREAD_V(UINT32_MAX);
	ERR_FAIL_COND_V(p_material.is_null(), UINT32_MAX);

	MaterialInfo3 *info = material_info_owner.get_or_null(p_material);
	ERR_FAIL_NULL_V(info, UINT32_MAX);

	uint32_t index = material_info_owner.get_index(p_material);

	// Put the RID for the material into the feedback buffer's rid_map at the index of the material info.
	MaterialFeedbackBuffer *_buffer = feedback_buffer_owner.get_or_null(m_current_feedback_buffer);
	if (_buffer) {
		if (_buffer->rid_map.size() <= (index + 1)) {
			_buffer->rid_map.resize(index + 1);
		}

		_buffer->rid_map[index] = p_material;
	}

	return index;
}

void TextureStreaming::feedback_handle_data(PackedByteArray array, RID p_buffer, uint64_t frame) {
	if (TextureStreaming::get_singleton() == nullptr) {
		return;
	}
	ERR_FAIL_COND(!p_buffer.is_valid());
	MaterialFeedbackBuffer *mb = feedback_buffer_owner.get_or_null(p_buffer);
	if (mb == nullptr) {
		return;
	}

	mb->frame = frame;
	mb->data = array;

	{
		MutexLock lock(feedback_buffer_mutex);
		feedback_buffer_queue.add_last(&mb->list_element);
		feedback_buffer_condvar.notify_one();
	}
}

void TextureStreaming::feedback_buffer_submit(RID p_buffer, uint64_t frame) {
	MaterialFeedbackBuffer *mb = feedback_buffer_owner.get_or_null(p_buffer);
	RD::get_singleton()->buffer_get_data_async(mb->buffer, callable_mp(this, &TextureStreaming::feedback_handle_data).bind(p_buffer, frame));
}

void TextureStreaming::_feedback_buffer_thread_func(void *p_udata) {
	Thread::set_name("TextureStreaming");

	TextureStreaming *tss = static_cast<TextureStreaming *>(p_udata);

	tss->_feedback_buffer_thread_main();
}

void TextureStreaming::_feedback_buffer_thread_main() {
	while (true) {
		MaterialFeedbackBuffer *mb = nullptr;
		RID buffer_rid;

		{
			MutexLock lock(feedback_buffer_mutex);

			// Wait for work or exit signal
			while (feedback_buffer_queue.first() == nullptr && !feedback_buffer_thread_exit) {
				feedback_buffer_condvar.wait(lock);
			}

			if (feedback_buffer_thread_exit) {
				break;
			}

			// Get next buffer to process
			SelfList<MaterialFeedbackBuffer> *list_element = feedback_buffer_queue.first();
			mb = list_element->self();
			feedback_buffer_queue.remove(list_element);
		}
		uint64_t ticks_msec = OS::get_singleton()->get_ticks_msec();

		if (mb) {
			uint32_t *data_ptr = (uint32_t *)mb->data.ptrw();
			if (data_ptr) {
				// fprintf(stderr, "ticks:%lu\n", ticks_msec);
				// Process the buffer
				const uint32_t *p_data = (uint32_t *)data_ptr;
				for (uint32_t i = 0; i < mb->rid_map.size(); i++) {
					uint32_t requested_resolution = p_data[i];
					if (requested_resolution == 0) {
						// Don't request 0.  Let the natural decay method to reduce the resolution.
						continue;
					}

					RID material_rid = mb->rid_map[i];

					MutexLock lock(material_mutex);

					MaterialInfo3 *info = material_info_owner.get_or_null(material_rid);

					if (info) {
						const Vector<RID> &textures = info->textures;

						uint32_t new_resolution = info->update(requested_resolution, ticks_msec);

						requested_resolution = new_resolution;

						for (int j = 0; j < textures.size(); j++) {
							RID texture_rid = textures[j];
							StreamingState *state = streaming_info_owner.get_or_null(texture_rid);

							// If the texture has been deleted but the material hasn't updated the list of textures yet, skip it.
							if (!state) {
								continue;
							}

							//Just a safety clamp. This is really to just ensure we never get crazy values.
							uint32_t clamped_resolution = CLAMP(requested_resolution, 1u, 8192u);

							if (mb->frame != state->frame) {
								state->frame = mb->frame;
								// New frame, reset to minimum resolution so the biggest can be chosen.
								state->feedback_resolution = state->get_min_resolution(setting_texture_min_resolution);
							}

							if (clamped_resolution >= state->feedback_resolution) {
								state->requested_tick_msec = ticks_msec;
								state->feedback_resolution = clamped_resolution;
							}
						}
					}
				}
			}

			// Return buffer to the pool
			{
				MutexLock lock(buffer_pool_mutex);
				buffer_pool.push_back(mb->self);
			}
		}

		{
			_feedback_buffer_process(ticks_msec);
		}
	}
}

void TextureStreaming::_feedback_buffer_process(uint64_t p_ticks_msec) {
	const LocalVector<RID> buffers = streaming_info_owner.get_owned_list();
	uint32_t constrained_max = 1;
	{
		double totals[14] = {
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
		};
		// Pass 1: Calculate total requested memory.
		for (uint32_t i = 0; i < buffers.size(); i++) {
			StreamingState *state = streaming_info_owner.get_or_null(buffers[i]);
			uint16_t max_res = MIN(state->max_resolution > 0 ? state->max_resolution : setting_texture_max_resolution, MAX(state->width, state->height));
			uint16_t min_res = MIN(MIN(state->min_resolution > 0 ? state->min_resolution : setting_texture_min_resolution, MAX(state->width, state->height)), max_res);

			const uint64_t msecs_since_request = p_ticks_msec - state->requested_tick_msec;
			if (state->current_resolution > min_res && msecs_since_request > setting_texture_change_wait_msec) {
				// Reset the request time to avoid further downgrades.
				state->requested_tick_msec = p_ticks_msec;

				// Downgrade textures that haven't not been requested for a while.
				state->feedback_resolution = state->current_resolution >> 1u;
			}

			state->request_resolution = state->feedback_resolution;

			totals[13] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 8192u));
			totals[12] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 4096u));
			totals[11] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 2048u));
			totals[10] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 1024u));
			totals[9] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 512u));
			totals[8] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 256u));
			totals[7] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 128u));
			totals[6] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 64u));
			totals[5] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 32u));
			totals[4] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 16u));
			totals[3] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 8u));
			totals[2] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 4u));
			totals[1] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 2u));
			totals[0] += _get_texture_size_bytes(state->format, MIN(state->request_resolution, 1u));
		}

		// Perform weighted allocation
		const double real_budget = setting_budget_mb * 1024.0 * 1024.0;
		// Determine which max size fits in the budget.
		for (int size_index = 13; size_index >= 0; size_index--) {
			if (totals[size_index] <= real_budget) {
				constrained_max = 1 << size_index;
				break;
			}
		}
	}

	{
		// Pass 2: Fit the textures into the memory budget.
		uint64_t memory = 0;
		for (uint32_t i = 0; i < buffers.size(); i++) {
			StreamingState *state = streaming_info_owner.get_or_null(buffers[i]);

			if (!state) {
				continue;
			}
			uint16_t max_res = MIN(state->max_resolution > 0 ? state->max_resolution : setting_texture_max_resolution, MAX(state->width, state->height));
			uint16_t min_res = MIN(MIN(state->min_resolution > 0 ? state->min_resolution : setting_texture_min_resolution, MAX(state->width, state->height)), max_res);

			// If the budget is enabled, we need to fit the texture into the budget.
			if (setting_budget_enabled) {
				state->fit_resolution = CLAMP(state->request_resolution, min_res, constrained_max);
			} else {
				state->fit_resolution = state->request_resolution;
			}

			// Adjust current resolution towards fit resolution.
			{
				uint64_t ticks_msec = OS::get_singleton()->get_ticks_msec();
				uint64_t msecs_since_change = ticks_msec - state->changed_tick_msec;
				// fprintf(stderr, "ticks_msec=%lu changed_tick_msec=%lu msecs_since_change=%lu setting_texture_change_wait_msec=%lu\n", ticks_msec, state->changed_tick_msec, msecs_since_change, setting_texture_change_wait_msec);
				if (msecs_since_change >= setting_texture_change_wait_msec) {
					state->fit_resolution = CLAMP(state->fit_resolution, min_res, max_res);
					state->current_resolution = CLAMP(state->current_resolution, 1, 8192);

					// fprintf(stderr, "object=%p ticks=%lu msecs_since_change=%lu changed_tick_msec=%lu fit_resolution=%u current_resolution=%u max_resolution=%u min_resolution=%u\n", (void *)state, ticks_msec, msecs_since_change, state->changed_tick_msec, state->fit_resolution, state->current_resolution, max_res, min_res);

					if (state->fit_resolution > state->current_resolution && state->current_resolution < max_res) {
						state->current_resolution <<= 1u;
						state->changed_tick_msec = ticks_msec;
					}

					else if (state->fit_resolution < state->current_resolution && state->current_resolution > min_res) {
						state->current_resolution >>= 1u;
						state->changed_tick_msec = ticks_msec;
					}

				} else {
					// fprintf(stderr, "  -- waiting to change texture resolution... %lu < %lu\n", msecs_since_change, setting_texture_change_wait_msec);
				}
			}

			// Add to total memory used.
			memory += _get_texture_size_bytes(state->format, state->current_resolution);

			// If the resolution changed, queue a reload.
			if (state->current_resolution != state->last_resolution) {
				state->last_resolution = state->current_resolution;
				// state->changed_tick_msec = ticks_msec;
				{
					MutexLock lock(texture_reload_mutex);
					if (!state->reload_element.in_list()) {
						texture_reload_queue.add_last(&state->reload_element);
						// texture_reload_queue.sort_custom<SortCustom>();
						// texture_reload_condvar.notify_one();
					}
				}
			}
		}

		texture_reload_condvar.notify_one();
		texture_streaming_total_memory = memory;
	}
}

void TextureStreaming::_texture_reload_thread_func(void *p_udata) {
	Thread::set_name("TextureStreaming I/O");

	TextureStreaming *tss = static_cast<TextureStreaming *>(p_udata);

	tss->_texture_reload_thread_main();
}

void TextureStreaming::_texture_reload_thread_main() {
	while (true) {
		StreamingState *state = nullptr;

		{
			MutexLock lock(texture_reload_mutex);

			// Wait for work or exit signal
			while (texture_reload_queue.first() == nullptr && !texture_reload_thread_exit) {
				texture_reload_condvar.wait(lock);
			}

			if (texture_reload_thread_exit) {
				break;
			}

			// Get next buffer to process
			SelfList<StreamingState> *list_element = texture_reload_queue.first();
			state = list_element->self();
			list_element->remove_from_list();
		}

		state->reload_callable.call(state->last_resolution.load());
		// OS::get_singleton()->delay_usec(1000);
	}
}
