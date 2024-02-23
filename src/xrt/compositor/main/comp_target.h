// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Abstracted compositor rendering target.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"

#include "vk/vk_helpers.h"

#include "util/u_trace_marker.h"


#ifdef __cplusplus
extern "C" {
#endif


/*!
 * For marking timepoints on a frame's lifetime, not a async event.
 *
 * @ingroup comp_main
 */
enum comp_target_timing_point
{
	//! Woke up after sleeping in wait frame.
	COMP_TARGET_TIMING_POINT_WAKE_UP,

	//! Began CPU side work for GPU.
	COMP_TARGET_TIMING_POINT_BEGIN,

	//! Just before submitting work to the GPU.
	COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN,

	//! Just after submitting work to the GPU.
	COMP_TARGET_TIMING_POINT_SUBMIT_END,
};

/*!
 * If the target should use the display timing information.
 *
 * @ingroup comp_main
 */
enum comp_target_display_timing_usage
{
	COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING = 0,
	COMP_TARGET_USE_DISPLAY_IF_AVAILABLE = 1,
};

/*!
 * Image and view pair for @ref comp_target.
 *
 * @ingroup comp_main
 */
struct comp_target_image
{
	VkImage handle;
	VkImageView view;
};

/*!
 * Collection of semaphores needed for a target.
 *
 * @ingroup comp_main
 */
struct comp_target_semaphores
{
	/*!
	 * Optional semaphore the target should signal when present is complete.
	 */
	VkSemaphore present_complete;

	/*!
	 * Semaphore the renderer (consuming this target)
	 * should signal when rendering is complete.
	 */
	VkSemaphore render_complete;

	/*!
	 * If true, @ref render_complete is a timeline
	 * semaphore instead of a binary semaphore.
	 */
	bool render_complete_is_timeline;
};

/*!
 * @brief A compositor target: where the compositor renders to.
 *
 * A target is essentially a swapchain, but it is such a overloaded term so
 * we are differentiating swapchains that the compositor provides to clients and
 * swapchains that the compositor renders by naming the latter to target.
 *
 * For design purposes, when amending this interface, remember that targets may not necessarily be backed by a
 * swapchain in all cases, for instance with remote rendering.
 *
 * @ingroup comp_main
 */
struct comp_target
{
	//! Owning compositor.
	struct comp_compositor *c;

	//! Name of the backing system.
	const char *name;

	//! Current dimensions of the target.
	uint32_t width, height;

	//! The format that the renderpass targeting this target should use.
	VkFormat format;

	//! Number of images that this target has.
	uint32_t image_count;
	//! Array of images and image views for rendering.
	struct comp_target_image *images;

	//! Transformation of the current surface, required for pre-rotation
	VkSurfaceTransformFlagBitsKHR surface_transform;

	// Holds semaphore information.
	struct comp_target_semaphores semaphores;

	/*
	 *
	 * Vulkan functions.
	 *
	 */

	/*!
	 * Do any initialization that is required to happen before Vulkan has
	 * been loaded.
	 */
	bool (*init_pre_vulkan)(struct comp_target *ct);

	/*!
	 * Do any initialization that requires Vulkan to be loaded, you need to
	 * call @ref create_images after calling this function.
	 */
	bool (*init_post_vulkan)(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height);

	/*!
	 * Is this target ready for image creation?
	 *
	 * Call before calling @ref create_images
	 */
	bool (*check_ready)(struct comp_target *ct);

	/*!
	 * Create or recreate the image(s) of the target, for swapchain based
	 * targets this will (re)create the swapchain.
	 *
	 * @pre @ref check_ready returns true
	 */
	void (*create_images)(struct comp_target *ct,
	                      uint32_t preferred_width,
	                      uint32_t preferred_height,
	                      VkFormat preferred_color_format,
	                      VkColorSpaceKHR preferred_color_space,
	                      VkImageUsageFlags image_usage,
	                      VkPresentModeKHR present_mode);

	/*!
	 * Has this target successfully had images created?
	 *
	 * Call before calling @ref acquire - if false but @ref check_ready is
	 * true, you'll need to call @ref create_images.
	 */
	bool (*has_images)(struct comp_target *ct);

	/*!
	 * Acquire the next image for rendering.
	 *
	 * If @ref comp_target_semaphores::present_complete is not null,
	 * your use of this image should wait on it..
	 *
	 * @pre @ref has_images() returns true
	 */
	VkResult (*acquire)(struct comp_target *ct, uint32_t *out_index);

	/*!
	 * Present the image at index to the screen.
	 *
	 * @pre @ref acquire succeeded for the same @p semaphore and @p index you are passing
	 *
	 * @param ct self
	 * @param queue The Vulkan queue being used
	 * @param index The swapchain image index to present
	 * @param timeline_semaphore_value The value to await on @ref comp_target_semaphores::render_complete
	 *                                 if @ref comp_target_semaphores::render_complete_is_timeline is true.
	 * @param desired_present_time_ns The timestamp to present at, ideally.
	 * @param present_slop_ns TODO
	 */
	VkResult (*present)(struct comp_target *ct,
	                    VkQueue queue,
	                    uint32_t index,
	                    uint64_t timeline_semaphore_value,
	                    uint64_t desired_present_time_ns,
	                    uint64_t present_slop_ns);

	/*!
	 * Flush any WSI state before rendering.
	 */
	void (*flush)(struct comp_target *ct);


	/*
	 *
	 * Timing functions.
	 *
	 */

	/*!
	 * Predict when the next frame should be started and when it will be
	 * turned into photons by the hardware.
	 */
	void (*calc_frame_pacing)(struct comp_target *ct,
	                          int64_t *out_frame_id,
	                          uint64_t *out_wake_up_time_ns,
	                          uint64_t *out_desired_present_time_ns,
	                          uint64_t *out_present_slop_ns,
	                          uint64_t *out_predicted_display_time_ns);

	/*!
	 * The compositor tells the target a timing information about a single
	 * timing point on the frames lifecycle.
	 */
	void (*mark_timing_point)(struct comp_target *ct,
	                          enum comp_target_timing_point point,
	                          int64_t frame_id,
	                          uint64_t when_ns);

	/*!
	 * Update timing information for this target, this function should be
	 * lightweight and is called multiple times during a frame to make sure
	 * that we get the timing data as soon as possible.
	 */
	VkResult (*update_timings)(struct comp_target *ct);

	/*!
	 * Provide frame timing information about GPU start and stop time.
	 *
	 * Depend on when the information is delivered this can be called at any
	 * point of the following frames.
	 *
	 * @param[in] ct           The compositor target.
	 * @param[in] frame_id     The frame ID to record for.
	 * @param[in] gpu_start_ns When the GPU work startred.
	 * @param[in] gpu_end_ns   When the GPU work stopped.
	 * @param[in] when_ns      When the informatioon collected, nominally
	 *                         from @ref os_monotonic_get_ns.
	 *
	 * @see @ref frame-pacing.
	 */
	void (*info_gpu)(
	    struct comp_target *ct, int64_t frame_id, uint64_t gpu_start_ns, uint64_t gpu_end_ns, uint64_t when_ns);

	/*
	 *
	 * Misc functions.
	 *
	 */

	/*!
	 * If the target can show a title (like a window) set the title.
	 */
	void (*set_title)(struct comp_target *ct, const char *title);

	/*!
	 * Destroys this target.
	 */
	void (*destroy)(struct comp_target *ct);
};

/*!
 * @copydoc comp_target::init_pre_vulkan
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline bool
comp_target_init_pre_vulkan(struct comp_target *ct)
{
	COMP_TRACE_MARKER();

	return ct->init_pre_vulkan(ct);
}

/*!
 * @copydoc comp_target::init_post_vulkan
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline bool
comp_target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	COMP_TRACE_MARKER();

	return ct->init_post_vulkan(ct, preferred_width, preferred_height);
}

/*!
 * @copydoc comp_target::check_ready
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline bool
comp_target_check_ready(struct comp_target *ct)
{
	COMP_TRACE_MARKER();

	return ct->check_ready(ct);
}

/*!
 * @copydoc comp_target::create_images
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_create_images(struct comp_target *ct,
                          uint32_t preferred_width,
                          uint32_t preferred_height,
                          VkFormat preferred_color_format,
                          VkColorSpaceKHR preferred_color_space,
                          VkImageUsageFlags image_usage,
                          VkPresentModeKHR present_mode)
{
	COMP_TRACE_MARKER();

	ct->create_images(          //
	    ct,                     //
	    preferred_width,        //
	    preferred_height,       //
	    preferred_color_format, //
	    preferred_color_space,  //
	    image_usage,            //
	    present_mode);          //
}

/*!
 * @copydoc comp_target::has_images
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline bool
comp_target_has_images(struct comp_target *ct)
{
	COMP_TRACE_MARKER();

	return ct->has_images(ct);
}

/*!
 * @copydoc comp_target::acquire
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline VkResult
comp_target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	COMP_TRACE_MARKER();

	return ct->acquire(ct, out_index);
}

/*!
 * @copydoc comp_target::present
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline VkResult
comp_target_present(struct comp_target *ct,
                    VkQueue queue,
                    uint32_t index,
                    uint64_t timeline_semaphore_value,
                    uint64_t desired_present_time_ns,
                    uint64_t present_slop_ns)

{
	COMP_TRACE_MARKER();

	return ct->present(           //
	    ct,                       //
	    queue,                    //
	    index,                    //
	    timeline_semaphore_value, //
	    desired_present_time_ns,  //
	    present_slop_ns);         //
}

/*!
 * @copydoc comp_target::flush
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_flush(struct comp_target *ct)
{
	COMP_TRACE_MARKER();

	ct->flush(ct);
}

/*!
 * @copydoc comp_target::calc_frame_pacing
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_calc_frame_pacing(struct comp_target *ct,
                              int64_t *out_frame_id,
                              uint64_t *out_wake_up_time_ns,
                              uint64_t *out_desired_present_time_ns,
                              uint64_t *out_present_slop_ns,
                              uint64_t *out_predicted_display_time_ns)
{
	COMP_TRACE_MARKER();

	ct->calc_frame_pacing(              //
	    ct,                             //
	    out_frame_id,                   //
	    out_wake_up_time_ns,            //
	    out_desired_present_time_ns,    //
	    out_present_slop_ns,            //
	    out_predicted_display_time_ns); //
}

/*!
 * Quick helper for marking wake up.
 * @copydoc comp_target::mark_timing_point
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_mark_wake_up(struct comp_target *ct, int64_t frame_id, uint64_t when_woke_ns)
{
	COMP_TRACE_MARKER();

	ct->mark_timing_point(ct, COMP_TARGET_TIMING_POINT_WAKE_UP, frame_id, when_woke_ns);
}

/*!
 * Quick helper for marking begin.
 * @copydoc comp_target::mark_timing_point
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_mark_begin(struct comp_target *ct, int64_t frame_id, uint64_t when_began_ns)
{
	COMP_TRACE_MARKER();

	ct->mark_timing_point(ct, COMP_TARGET_TIMING_POINT_BEGIN, frame_id, when_began_ns);
}

/*!
 * Quick helper for marking submit began.
 * @copydoc comp_target::mark_timing_point
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_mark_submit_begin(struct comp_target *ct, int64_t frame_id, uint64_t when_submit_began_ns)
{
	COMP_TRACE_MARKER();

	ct->mark_timing_point(ct, COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_submit_began_ns);
}

/*!
 * Quick helper for marking submit end.
 * @copydoc comp_target::mark_timing_point
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_mark_submit_end(struct comp_target *ct, int64_t frame_id, uint64_t when_submit_end_ns)
{
	COMP_TRACE_MARKER();

	ct->mark_timing_point(ct, COMP_TARGET_TIMING_POINT_SUBMIT_END, frame_id, when_submit_end_ns);
}

/*!
 * @copydoc comp_target::update_timings
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline VkResult
comp_target_update_timings(struct comp_target *ct)
{
	COMP_TRACE_MARKER();

	return ct->update_timings(ct);
}

/*!
 * @copydoc comp_target::info_gpu
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_info_gpu(
    struct comp_target *ct, int64_t frame_id, uint64_t gpu_start_ns, uint64_t gpu_end_ns, uint64_t when_ns)
{
	COMP_TRACE_MARKER();

	ct->info_gpu(ct, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

/*!
 * @copydoc comp_target::set_title
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_set_title(struct comp_target *ct, const char *title)
{
	COMP_TRACE_MARKER();

	ct->set_title(ct, title);
}

/*!
 * @copydoc comp_target::destroy
 *
 * Helper for calling through the function pointer: does a null check and sets
 * ct_ptr to null if freed.
 *
 * @public @memberof comp_target
 * @ingroup comp_main
 */
static inline void
comp_target_destroy(struct comp_target **ct_ptr)
{
	struct comp_target *ct = *ct_ptr;
	if (ct == NULL) {
		return;
	}

	ct->destroy(ct);
	*ct_ptr = NULL;
}

/*!
 * A factory of targets.
 *
 * @ingroup comp_main
 */
struct comp_target_factory
{
	//! Pretty loggable name of target type.
	const char *name;

	//! Short all lowercase identifier for target type.
	const char *identifier;

	//! Does this factory require Vulkan to have been initialized.
	bool requires_vulkan_for_create;

	/*!
	 * Is this a deferred target that can have it's creation
	 * delayed even further then after Vulkan initialization.
	 */
	bool is_deferred;

	//! Required instance extensions.
	const char **required_instance_extensions;

	//! Required instance extension count.
	size_t required_instance_extension_count;

	/*!
	 * Checks if this target can be detected, is the preferred target or
	 * some other special consideration that this target should be used over
	 * all other targets.
	 *
	 * This is needed for NVIDIA direct mode which window must be created
	 * after vulkan has initialized.
	 */
	bool (*detect)(const struct comp_target_factory *ctf, struct comp_compositor *c);

	/*!
	 * Create a target from this factory, some targets requires Vulkan to
	 * have been initialised, see @ref requires_vulkan_for_create.
	 */
	bool (*create_target)(const struct comp_target_factory *ctf,
	                      struct comp_compositor *c,
	                      struct comp_target **out_ct);
};

/*!
 * @copydoc comp_target_factory::detect
 *
 * @public @memberof comp_target_factory
 * @ingroup comp_main
 */
static inline bool
comp_target_factory_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	COMP_TRACE_MARKER();

	return ctf->detect(ctf, c);
}

/*!
 * @copydoc comp_target_factory::create_target
 *
 * @public @memberof comp_target_factory
 * @ingroup comp_main
 */
static inline bool
comp_target_factory_create_target(const struct comp_target_factory *ctf,
                                  struct comp_compositor *c,
                                  struct comp_target **out_ct)
{
	COMP_TRACE_MARKER();

	return ctf->create_target(ctf, c, out_ct);
}


#ifdef __cplusplus
}
#endif
