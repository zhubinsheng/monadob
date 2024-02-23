// Copyright 2019-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Direct mode on PLATFORM_DISPLAY_KHR code.
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 */

#include "util/u_misc.h"

#include "main/comp_window_direct.h"

/*
 *
 * Private structs
 *
 */

/*!
 * Probed display.
 */
struct vk_display
{
	VkDisplayPropertiesKHR display_properties;
	VkDisplayKHR display;
};

/*!
 * Direct mode "window" into a device, using PLATFORM_DISPLAY_KHR.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_vk_display
{
	struct comp_target_swapchain base;

	struct vk_display *displays;
	uint16_t display_count;
};

/*
 *
 * Forward declare functions
 *
 */

static void
comp_window_vk_display_destroy(struct comp_target *ct);

static bool
comp_window_vk_display_init(struct comp_target *ct);

static struct vk_display *
comp_window_vk_display_current_display(struct comp_window_vk_display *w);

static bool
comp_window_vk_display_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height);


/*
 *
 * Functions.
 *
 */

static inline struct vk_bundle *
get_vk(struct comp_target *ct)
{
	return &ct->c->base.vk;
}

static void
_flush(struct comp_target *ct)
{
	(void)ct;
}

static void
_update_window_title(struct comp_target *ct, const char *title)
{
	(void)ct;
	(void)title;
}

struct comp_target *
comp_window_vk_display_create(struct comp_compositor *c)
{
	struct comp_window_vk_display *w = U_TYPED_CALLOC(struct comp_window_vk_display);

	// The display timing code hasn't been tested on vk display and may be broken.
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	w->base.base.name = "VkDisplayKHR";
	w->base.display = VK_NULL_HANDLE;
	w->base.base.destroy = comp_window_vk_display_destroy;
	w->base.base.flush = _flush;
	w->base.base.init_pre_vulkan = comp_window_vk_display_init;
	w->base.base.init_post_vulkan = comp_window_vk_display_init_swapchain;
	w->base.base.set_title = _update_window_title;
	w->base.base.c = c;

	return &w->base.base;
}

static void
comp_window_vk_display_destroy(struct comp_target *ct)
{
	struct comp_window_vk_display *w_direct = (struct comp_window_vk_display *)ct;

	comp_target_swapchain_cleanup(&w_direct->base);

	for (uint32_t i = 0; i < w_direct->display_count; i++) {
		struct vk_display *d = &w_direct->displays[i];
		d->display = VK_NULL_HANDLE;
	}

	if (w_direct->displays != NULL) {
		free(w_direct->displays);
	}

	free(ct);
}

static bool
append_vk_display_entry(struct comp_window_vk_display *w, struct VkDisplayPropertiesKHR *disp)
{
	// Make the compositor use this size.
	comp_target_swapchain_override_extents(&w->base, disp->physicalResolution);

	// Create the entry.
	struct vk_display d = {
	    .display_properties = *disp,
	    .display = disp->display,
	};

	w->display_count += 1;

	U_ARRAY_REALLOC_OR_FREE(w->displays, struct vk_display, w->display_count);

	if (w->displays == NULL) {
		COMP_ERROR(w->base.base.c, "Unable to reallocate vk_display displays");

		// Reset the count.
		w->display_count = 0;
		return false;
	}

	w->displays[w->display_count - 1] = d;

	return true;
}

static void
print_found_displays(struct comp_compositor *c, struct VkDisplayPropertiesKHR *display_props, uint32_t display_count)
{
	COMP_ERROR(c, "== Found Displays ==");
	for (uint32_t i = 0; i < display_count; i++) {
		struct VkDisplayPropertiesKHR *p = &display_props[i];

		COMP_ERROR(c, "[%d] %s with resolution %dx%d, dims %dx%d", i, p->displayName,
		           p->physicalResolution.width, p->physicalResolution.height, p->physicalDimensions.width,
		           p->physicalDimensions.height);
	}
}

static bool
comp_window_vk_display_init(struct comp_target *ct)
{
	struct comp_window_vk_display *w_direct = (struct comp_window_vk_display *)ct;
	struct vk_bundle *vk = get_vk(ct);
	VkDisplayPropertiesKHR *display_props = NULL;
	uint32_t display_count = 0;
	VkResult ret;

	if (vk->instance == VK_NULL_HANDLE) {
		COMP_ERROR(ct->c, "Vulkan not initialized before vk display init!");
		return false;
	}

	// Get a list of attached displays.
	ret = vk_enumerate_physical_device_display_properties( //
	    vk,                                                //
	    vk->physical_device,                               //
	    &display_count,                                    //
	    &display_props);                                   //
	if (ret != VK_SUCCESS) {
		CVK_ERROR(ct->c, "vk_enumerate_physical_device_display_properties", "Failed to get display properties",
		          ret);
		return false;
	}

	if (display_count == 0) {
		COMP_ERROR(ct->c, "No Vulkan displays found.");
		return false;
	}

	if (ct->c->settings.vk_display > (int)display_count) {
		COMP_ERROR(ct->c, "Requested display %d, but only %d found.", ct->c->settings.vk_display,
		           display_count);
		print_found_displays(ct->c, display_props, display_count);
		free(display_props);
		return false;
	}

	append_vk_display_entry(w_direct, &display_props[ct->c->settings.vk_display]);

	struct vk_display *d = comp_window_vk_display_current_display(w_direct);
	if (!d) {
		COMP_ERROR(ct->c, "display not found!");
		print_found_displays(ct->c, display_props, display_count);
		free(display_props);
		return false;
	}

	free(display_props);

	return true;
}

static struct vk_display *
comp_window_vk_display_current_display(struct comp_window_vk_display *w)
{
	int index = w->base.base.c->settings.display;
	if (index == -1)
		index = 0;

	if (w->display_count <= (uint32_t)index)
		return NULL;

	return &w->displays[index];
}

static bool
init_swapchain(struct comp_target_swapchain *cts, VkDisplayKHR display, uint32_t width, uint32_t height)
{
	VkResult ret;

	ret = comp_window_direct_create_surface(cts, display, width, height);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(cts->base.c, "Failed to create surface! '%s'", vk_result_string(ret));
		return false;
	}

	return true;
}

static bool
comp_window_vk_display_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_vk_display *w_direct = (struct comp_window_vk_display *)ct;

	struct vk_display *d = comp_window_vk_display_current_display(w_direct);
	if (!d) {
		COMP_ERROR(ct->c, "display not found.");
		return false;
	}

	COMP_DEBUG(ct->c, "Will use display: %s", d->display_properties.displayName);

	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	cts->display = d->display;

	return init_swapchain(&w_direct->base, d->display, width, height);
}


/*
 *
 * Factory
 *
 */

static const char *instance_extensions[] = {
    VK_KHR_DISPLAY_EXTENSION_NAME,
};

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return false;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = comp_window_vk_display_create(c);
	if (ct == NULL) {
		return false;
	}

	*out_ct = ct;

	return true;
}

const struct comp_target_factory comp_target_factory_vk_display = {
    .name = "Vulkan Display Direct-Mode",
    .identifier = "vk_display",
    .requires_vulkan_for_create = true,
    .is_deferred = false,
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .detect = detect,
    .create_target = create_target,
};
