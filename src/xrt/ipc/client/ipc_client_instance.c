// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client side wrapper of instance.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_client
 */

#include "xrt/xrt_results.h"
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "xrt/xrt_instance.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_config_android.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_file.h"
#include "util/u_debug.h"
#include "util/u_git_tag.h"
#include "util/u_system_helpers.h"

#include "shared/ipc_protocol.h"
#include "shared/ipc_shmem.h"
#include "client/ipc_client.h"
#include "client/ipc_client_interface.h"
#include "client/ipc_client_connection.h"

#include "ipc_client_generated.h"


#include <stdio.h>
#if !defined(XRT_OS_WINDOWS)
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <limits.h>

#ifdef XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER
#include "android/android_ahardwarebuffer_allocator.h"
#endif

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#include "android/ipc_client_android.h"
#endif // XRT_OS_ANDROID

DEBUG_GET_ONCE_LOG_OPTION(ipc_log, "IPC_LOG", U_LOGGING_WARN)


/*
 *
 * Struct and helpers.
 *
 */

/*!
 * @implements xrt_instance
 */
struct ipc_client_instance
{
	//! @public Base
	struct xrt_instance base;

	struct ipc_connection ipc_c;

	struct xrt_tracking_origin *xtracks[XRT_SYSTEM_MAX_DEVICES];
	size_t xtrack_count;

	struct xrt_device *xdevs[XRT_SYSTEM_MAX_DEVICES];
	size_t xdev_count;
};

static inline struct ipc_client_instance *
ipc_client_instance(struct xrt_instance *xinst)
{
	return (struct ipc_client_instance *)xinst;
}

static xrt_result_t
create_system_compositor(struct ipc_client_instance *ii,
                         struct xrt_device *xdev,
                         struct xrt_system_compositor **out_xsysc)
{
	struct xrt_system_compositor *xsysc = NULL;
	struct xrt_image_native_allocator *xina = NULL;
	xrt_result_t xret;

#ifdef XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER
	// On Android, we allocate images natively on the client side.
	xina = android_ahardwarebuffer_allocator_create();
#endif // XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER

	xret = ipc_client_create_system_compositor(&ii->ipc_c, xina, xdev, &xsysc);
	IPC_CHK_WITH_GOTO(&ii->ipc_c, xret, "ipc_client_create_system_compositor", err_xina);

	// Paranoia.
	if (xsysc == NULL) {
		xret = XRT_ERROR_IPC_FAILURE;
		IPC_ERROR(&ii->ipc_c, "Variable xsysc NULL!");
		goto err_xina;
	}

	*out_xsysc = xsysc;

	return XRT_SUCCESS;

err_xina:
	xrt_images_destroy(&xina);
	return xret;
}


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
ipc_client_instance_create_system(struct xrt_instance *xinst,
                                  struct xrt_system **out_xsys,
                                  struct xrt_system_devices **out_xsysd,
                                  struct xrt_space_overseer **out_xso,
                                  struct xrt_system_compositor **out_xsysc)
{
	struct ipc_client_instance *ii = ipc_client_instance(xinst);
	xrt_result_t xret = XRT_SUCCESS;

	assert(out_xsys != NULL);
	assert(*out_xsys == NULL);
	assert(out_xsysd != NULL);
	assert(*out_xsysd == NULL);
	assert(out_xsysc == NULL || *out_xsysc == NULL);

	struct xrt_system_devices *xsysd = NULL;
	struct xrt_system_compositor *xsysc = NULL;

	// Allocate a helper xrt_system_devices struct.
	xsysd = ipc_client_system_devices_create(&ii->ipc_c);

	// Take the devices from this instance.
	for (uint32_t i = 0; i < ii->xdev_count; i++) {
		xsysd->xdevs[i] = ii->xdevs[i];
		ii->xdevs[i] = NULL;
	}
	xsysd->xdev_count = ii->xdev_count;
	ii->xdev_count = 0;

#define SET_ROLE(ROLE)                                                                                                 \
	do {                                                                                                           \
		int32_t index = ii->ipc_c.ism->roles.ROLE;                                                             \
		xsysd->static_roles.ROLE = index >= 0 ? xsysd->xdevs[index] : NULL;                                    \
	} while (false)

	SET_ROLE(head);
	SET_ROLE(eyes);
	SET_ROLE(hand_tracking.left);
	SET_ROLE(hand_tracking.right);

#undef SET_ROLE

	// Done here now.
	if (out_xsysc == NULL) {
		goto out;
	}

	if (xsysd->static_roles.head == NULL) {
		IPC_ERROR((&ii->ipc_c), "No head device found but asking for system compositor!");
		xret = XRT_ERROR_IPC_FAILURE;
		goto err_destroy;
	}

	xret = create_system_compositor(ii, xsysd->static_roles.head, &xsysc);
	if (xret != XRT_SUCCESS) {
		goto err_destroy;
	}

out:
	*out_xsys = ipc_client_system_create(&ii->ipc_c, xsysc);
	*out_xsysd = xsysd;
	*out_xso = ipc_client_space_overseer_create(&ii->ipc_c);

	if (xsysc != NULL) {
		assert(out_xsysc != NULL);
		*out_xsysc = xsysc;
	}

	return XRT_SUCCESS;

err_destroy:
	xrt_system_devices_destroy(&xsysd);

	return xret;
}

static xrt_result_t
ipc_client_instance_get_prober(struct xrt_instance *xinst, struct xrt_prober **out_xp)
{
	*out_xp = NULL;

	return XRT_ERROR_PROBER_NOT_SUPPORTED;
}

static void
ipc_client_instance_destroy(struct xrt_instance *xinst)
{
	struct ipc_client_instance *ii = ipc_client_instance(xinst);

	// service considers us to be connected until fd is closed
	ipc_client_connection_fini(&ii->ipc_c);

	for (size_t i = 0; i < ii->xtrack_count; i++) {
		u_var_remove_root(ii->xtracks[i]);
		free(ii->xtracks[i]);
		ii->xtracks[i] = NULL;
	}
	ii->xtrack_count = 0;

	ipc_shmem_destroy(&ii->ipc_c.ism_handle, (void **)&ii->ipc_c.ism, sizeof(struct ipc_shared_memory));

	free(ii);
}


/*
 *
 * Exported function(s).
 *
 */

/*!
 * Constructor for xrt_instance IPC client proxy.
 *
 * @public @memberof ipc_instance
 */
xrt_result_t
ipc_instance_create(struct xrt_instance_info *i_info, struct xrt_instance **out_xinst)
{
	struct ipc_client_instance *ii = U_TYPED_CALLOC(struct ipc_client_instance);
	ii->base.create_system = ipc_client_instance_create_system;
	ii->base.get_prober = ipc_client_instance_get_prober;
	ii->base.destroy = ipc_client_instance_destroy;

	xrt_result_t xret = ipc_client_connection_init(&ii->ipc_c, debug_get_log_option_ipc_log(), i_info);
	if (xret != XRT_SUCCESS) {
		free(ii);
		return xret;
	}

	uint32_t count = 0;
	struct xrt_tracking_origin *xtrack = NULL;
	struct ipc_shared_memory *ism = ii->ipc_c.ism;

	// Query the server for how many tracking origins it has.
	count = 0;
	for (uint32_t i = 0; i < ism->itrack_count; i++) {
		xtrack = U_TYPED_CALLOC(struct xrt_tracking_origin);

		memcpy(xtrack->name, ism->itracks[i].name, sizeof(xtrack->name));

		xtrack->type = ism->itracks[i].type;
		xtrack->offset = ism->itracks[i].offset;
		ii->xtracks[count++] = xtrack;

		u_var_add_root(xtrack, "Tracking origin", true);
		u_var_add_ro_text(xtrack, xtrack->name, "name");
		u_var_add_pose(xtrack, &xtrack->offset, "offset");
	}

	ii->xtrack_count = count;

	// Query the server for how many devices it has.
	count = 0;
	for (uint32_t i = 0; i < ism->isdev_count; i++) {
		struct ipc_shared_device *isdev = &ism->isdevs[i];
		xtrack = ii->xtracks[isdev->tracking_origin_index];

		if (isdev->name == XRT_DEVICE_GENERIC_HMD) {
			ii->xdevs[count++] = ipc_client_hmd_create(&ii->ipc_c, xtrack, i);
		} else {
			ii->xdevs[count++] = ipc_client_device_create(&ii->ipc_c, xtrack, i);
		}
	}

	ii->xdev_count = count;

	ii->base.startup_timestamp = ii->ipc_c.ism->startup_timestamp;

	*out_xinst = &ii->base;

	return XRT_SUCCESS;
}
