/**************************************************************************/
/*  texture_streaming.h                                                   */
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

#include "core/error/error_macros.h"
#include "core/io/image.h"
#include "core/object/object.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/rid_owner.h"
#include "core/templates/self_list.h"
#include "core/typedefs.h"
#include "core/variant/callable.h"
#include "servers/rendering/rendering_device.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

template <typename T>
class RID_IndexedOwner {
	static constexpr uint32_t INVALID_INDEX = 0xFFFFFFFF;

	struct Wrapper {
		T data;
		uint32_t index;
	};

	mutable RID_Owner<Wrapper, false> owner;
	LocalVector<RID> index_to_rid;

public:
	_FORCE_INLINE_ T *get_or_null(const RID &p_rid) {
		Wrapper *wrapper = owner.get_or_null(p_rid);
		return wrapper ? &wrapper->data : nullptr;
	}

	_FORCE_INLINE_ T *allocate(RID &p_rid) {
		uint32_t new_index = index_to_rid.size();
		Wrapper wrapper;
		wrapper.index = new_index;
		RID rid = owner.allocate_rid();
		owner.initialize_rid(rid, wrapper);
		index_to_rid.push_back(rid);
		Wrapper *wrapper_ptr = owner.get_or_null(rid);
		p_rid = rid;
		return wrapper_ptr ? &wrapper_ptr->data : nullptr;
	}

	_FORCE_INLINE_ T *get_or_null_index(uint32_t p_index) {
		if (p_index >= index_to_rid.size()) {
			return nullptr;
		}
		Wrapper *wrapper = owner.get_or_null(index_to_rid[p_index]);
		return wrapper ? &wrapper->data : nullptr;
	}

	_FORCE_INLINE_ RID get_rid(uint32_t p_index) const {
		if (p_index >= index_to_rid.size()) {
			return RID();
		}
		return index_to_rid[p_index];
	}

	_FORCE_INLINE_ uint32_t get_index(const RID &p_rid) const {
		Wrapper *wrapper = owner.get_or_null(p_rid);
		return wrapper ? wrapper->index : INVALID_INDEX;
	}

	_FORCE_INLINE_ void free(const RID &p_rid) {
		Wrapper *wrapper = owner.get_or_null(p_rid);
		if (wrapper) {
			uint32_t index = wrapper->index;
			uint32_t last_index = index_to_rid.size() - 1;

			if (index != last_index) {
				// Move last element to the freed slot
				RID last_rid = index_to_rid[last_index];
				index_to_rid[index] = last_rid;

				// Update the moved element's index
				Wrapper *last_wrapper = owner.get_or_null(last_rid);
				if (last_wrapper) {
					last_wrapper->index = index;
				}
			}

			index_to_rid.resize(last_index);
			owner.free(p_rid);
		}
	}

	_FORCE_INLINE_ uint32_t get_count() const {
		return index_to_rid.size();
	}
};

class TextureStreaming : public Object {
	GDCLASS(TextureStreaming, Object);

	static TextureStreaming *singleton;

protected:
	static void _bind_methods();

private:
	struct StreamingState {
		// Configuration
		RID texture;
		uint16_t width = 0;
		uint16_t height = 0;
		Image::Format format = Image::FORMAT_MAX;

		// Settings & Constraints
		Callable reload_callable;
		void *callback_data = nullptr;
		uint16_t min_resolution = 0;
		uint16_t max_resolution = 0;

		// Runtime state
		uint16_t feedback_resolution = 0; // The resolution requested by the material feedback.
		uint16_t request_resolution = 0; // The resolution after clamping to min/max.
		uint16_t fit_resolution = 0; // The resolution that fits in the budget or minimum.
		uint16_t current_resolution = 0; // The current resolution that has been set.
		std::atomic<uint16_t> last_resolution = 0; // The last resolution that was set.
		uint64_t frame = 0;
		uint64_t changed_tick_msec = 0;
		uint64_t requested_tick_msec = 0;
		// uint32_t size_in_bytes = 0;

		SelfList<StreamingState> reload_element;

		StreamingState() :
				reload_element(this) {}

		StreamingState(const StreamingState &p_other) :
				reload_element(this) {
			texture = p_other.texture;
			width = p_other.width;
			height = p_other.height;
			format = p_other.format;
			reload_callable = p_other.reload_callable;
			callback_data = p_other.callback_data;
			min_resolution = p_other.min_resolution;
			max_resolution = p_other.max_resolution;
			feedback_resolution = p_other.feedback_resolution;
			request_resolution = p_other.request_resolution;
			fit_resolution = p_other.fit_resolution;
			current_resolution = p_other.current_resolution;
			last_resolution = p_other.last_resolution.load();
			frame = p_other.frame;
			changed_tick_msec = p_other.changed_tick_msec;
			requested_tick_msec = p_other.requested_tick_msec;
			// size_in_bytes = p_other.size_in_bytes;
		}

		_FORCE_INLINE_ uint16_t get_min_resolution(uint32_t p_setting_texture_min_resolution) const {
			return min_resolution > 0 ? min_resolution : p_setting_texture_min_resolution;
		}
	};

	struct MaterialFeedbackBuffer {
		RID buffer; // RID for the material feedback buffer.
		uint32_t buffer_size = 0; // Size of the buffer in bytes.
		LocalVector<RID> rid_map; // Maps indices in the buffer to texture RIDs.
		uint64_t frame = 0;
		RID self;
		SelfList<MaterialFeedbackBuffer> list_element;

		PackedByteArray data;

		void cleanup();
		void clear();
		void resize();

		MaterialFeedbackBuffer();
		MaterialFeedbackBuffer(MaterialFeedbackBuffer const &p_other);
	};

	BinaryMutex material_mutex;

	struct TextureSizeCacheKey {
		Image::Format format;
		uint32_t resolution;

		TextureSizeCacheKey(Image::Format p_format, uint32_t p_resolution) :
				format(p_format), resolution(p_resolution) {}

		TextureSizeCacheKey(const TextureSizeCacheKey &other) :
				format(other.format), resolution(other.resolution) {}

		bool operator==(const TextureSizeCacheKey &other) const {
			return format == other.format && resolution == other.resolution;
		}
	};

	struct TextureSizeCacheKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const TextureSizeCacheKey &p_key) {
			uint32_t h = hash_murmur3_one_32(p_key.format);
			h = hash_murmur3_one_32(p_key.resolution, h);
			return hash_fmix32(h);
		}
	};

	AHashMap<TextureSizeCacheKey, uint64_t, TextureSizeCacheKeyHasher> size_cache;

	uint64_t _get_texture_size_bytes(Image::Format p_format, uint32_t p_resolution) {
		// TODO: Find out if caching is actually beneficial here.  Hashing format + resolution maybe more expensive than just calculating size.
		TextureSizeCacheKey key(p_format, p_resolution);
		AHashMap<TextureSizeCacheKey, uint64_t, TextureSizeCacheKeyHasher>::Iterator it = size_cache.find(key);

		if (!it) {
			uint64_t size = Image::get_image_data_size(p_resolution, p_resolution, p_format, true);
			size_cache.insert(key, size);
			return size;
		} else {
			return it->value;
		}
	}

	// Buffer pool management
	BinaryMutex buffer_pool_mutex;
	Vector<RID> buffer_pool;
	int buffer_count = 0;

	RID_Owner<MaterialFeedbackBuffer, true> feedback_buffer_owner;
	RID_Owner<StreamingState, true> streaming_info_owner;

	struct MaterialInfo3 {
		Vector<RID> textures;

		float smoothed_max = 0.0f;
		uint64_t last_update_tick_msec = 0;

		uint32_t update(uint32_t p_value, uint64_t p_current_tick_msec, float p_decay_per_msec = 0.0001f) {
			float value = log2(p_value);
			// Linear decay based on time
			if (last_update_tick_msec > 0) {
				uint64_t delta_msec = p_current_tick_msec - last_update_tick_msec;
				smoothed_max = MAX(0.0f, smoothed_max - (float(delta_msec) * p_decay_per_msec));
			} else {
				// fprintf(stderr, "WTF\n");
			}

			smoothed_max = MAX(float(value), smoothed_max);
			last_update_tick_msec = p_current_tick_msec;

			// fprintf(stderr, "%p: value=%u smoothed_max=%f\n", this, p_value, smoothed_max);

			return 1u << uint32_t(roundf(smoothed_max));
		}
	};
	RID_IndexedOwner<MaterialInfo3> material_info_owner;

	StreamingState *get_streaming_info(RID p_rid) { return streaming_info_owner.get_or_null(p_rid); }

	// Settings
	bool setting_streaming_is_enabled = true;
	bool setting_budget_enabled = true;
	uint32_t setting_budget_mb = 512;
	uint32_t setting_texture_max_resolution = 8192;
	uint32_t setting_texture_min_resolution = 32u;
	uint64_t setting_texture_change_wait_msec = 200;
	uint64_t setting_texture_change_idle_msec = 10000;
	void _settings_changed();

	// Feedback buffer processing
	BinaryMutex feedback_buffer_mutex;
	ConditionVariable feedback_buffer_condvar;
	SelfList<MaterialFeedbackBuffer>::List feedback_buffer_queue;
	Thread feedback_buffer_thread;
	bool feedback_buffer_thread_exit = false;
	uint64_t feedback_buffer_last_submit_ticks = 0;

	static void _feedback_buffer_thread_func(void *p_udata);
	void _feedback_buffer_thread_main();
	void _feedback_buffer_process(uint64_t ticks_msec);

	// Queue processing
	BinaryMutex texture_reload_mutex;
	ConditionVariable texture_reload_condvar;
	SelfList<StreamingState>::List texture_reload_queue;
	Thread texture_reload_thread;
	bool texture_reload_thread_exit = false;

	static void _texture_reload_thread_func(void *p_udata);
	void _texture_reload_thread_main();

	std::atomic<uint64_t> texture_streaming_total_memory = 0;

	// Comparator for sorting StreamingState objects by current_resolution in descending order.
	struct SortCustom {
		_FORCE_INLINE_ bool operator()(const StreamingState &p_a, const StreamingState &p_b) const {
			return p_a.current_resolution > p_b.current_resolution ? true : false;
		}
	};

	void feedback_handle_data(PackedByteArray array, RID p_buffer, uint64_t frame);

	RID m_current_feedback_buffer = RID();
	void feedback_frame_done_callback();

	RID feedback_buffer_get(uint32_t p_num_materials);
	void feedback_buffer_submit(RID p_buffer, uint64_t frame);

public:
	static TextureStreaming *get_singleton();

	// Feedback Buffer API
	uint32_t feedback_buffer_material_index(RID p_material);
	RID feedback_buffer_get_uniform_rid();
	void late_init();
	bool feedback_buffer_valid() const {
		return m_current_feedback_buffer.is_valid();
	}

	// Texture API
	RID texture_configure_streaming(RID p_texture, Image::Format format, int width, int height, int p_min_resolution = 0, int p_max_resolution = 0, Callable p_reload_callable = nullptr);
	void texture_update(RID p_rid, int width, int height, int p_min_resolution, int p_max_resolution);
	void texture_remove(RID p_rid);

	// Material API
	RID material_set_textures(RID p_feeback_rid, Vector<RID> &p_textures);
	// void material_remove(RID p_material);

	// Status API
	uint64_t get_memory_budget_bytes_used();

	void set_streaming_min_resolution(uint32_t p_resolution) {
		setting_texture_min_resolution = p_resolution;
	}
	uint32_t get_streaming_min_resolution() const {
		return setting_texture_min_resolution;
	}
	void set_streaming_max_resolution(uint32_t p_resolution) {
		setting_texture_max_resolution = p_resolution;
	}
	uint32_t get_streaming_max_resolution() const {
		return setting_texture_max_resolution;
	}
	void set_streaming_enabled(bool p_enabled) {
		setting_streaming_is_enabled = p_enabled;
	}
	bool is_streaming_enabled() const {
		return setting_streaming_is_enabled;
	}
	void set_budget_enabled(bool p_enabled) {
		setting_budget_enabled = p_enabled;
	}
	bool is_budget_enabled() const {
		return setting_budget_enabled;
	}
	void set_memory_budget_mb(float p_mb) {
		setting_budget_mb = uint32_t(p_mb);
	}
	float get_memory_budget_mb() const {
		return float(setting_budget_mb) / (1024.0f * 1024.0f);
	}

	TextureStreaming();
	virtual ~TextureStreaming();
};
