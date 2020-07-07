// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client side wrapper of instance.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_client
 */

#include "xrt/xrt_instance.h"
#include "xrt/xrt_gfx_fd.h"

#include "util/u_misc.h"
#include "util/u_var.h"

#include "ipc_protocol.h"
#include "ipc_client.h"
#include "ipc_client_generated.h"

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>


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

	struct xrt_tracking_origin *xtracks[8];
	size_t num_xtracks;

	struct xrt_device *xdevs[8];
	size_t num_xdevs;
};

static inline struct ipc_client_instance *
ipc_client_instance(struct xrt_instance *xinst)
{
	return (struct ipc_client_instance *)xinst;
}

static bool
ipc_connect(struct ipc_connection *ipc_c)
{
	struct sockaddr_un addr;
	int ret;

	ipc_c->print_spew = false;  // TODO: hardcoded - fetch from settings
	ipc_c->print_debug = false; // TODO: hardcoded - fetch from settings


	// create our IPC socket

	ret = socket(PF_UNIX, SOCK_STREAM, 0);
	if (ret < 0) {
		IPC_DEBUG(ipc_c, "Socket Create Error!");
		return false;
	}

	int socket = ret;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, IPC_MSG_SOCK_FILE);

	ret = connect(socket, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		IPC_DEBUG(ipc_c, "Socket Connect error!");
		close(socket);
		return false;
	}

	ipc_c->imc.socket_fd = socket;
	ipc_c->imc.print_debug = ipc_c->print_debug;

	return true;
}



/*
 *
 * Member functions.
 *
 */

static int
ipc_client_instance_select(struct xrt_instance *xinst,
                           struct xrt_device **xdevs,
                           size_t num_xdevs)
{
	struct ipc_client_instance *ii = ipc_client_instance(xinst);

	if (num_xdevs < 1) {
		return -1;
	}

	// @todo What about ownership?
	for (size_t i = 0; i < num_xdevs && i < ii->num_xdevs; i++) {
		xdevs[i] = ii->xdevs[i];
	}

	return 0;
}

static int
ipc_client_instance_create_fd_compositor(struct xrt_instance *xinst,
                                         struct xrt_device *xdev,
                                         bool flip_y,
                                         struct xrt_compositor_fd **out_xcfd)
{
	struct ipc_client_instance *ii = ipc_client_instance(xinst);
	struct xrt_compositor_fd *xcfd = NULL;

	int ret = ipc_client_compositor_create(&ii->ipc_c, xdev, flip_y, &xcfd);
	if (ret < 0 || xcfd == NULL) {
		return -1;
	}

	*out_xcfd = xcfd;

	return 0;
}

static int
ipc_client_instance_get_prober(struct xrt_instance *xinst,
                               struct xrt_prober **out_xp)
{
	*out_xp = NULL;

	return -1;
}

static void
ipc_client_instance_destroy(struct xrt_instance *xinst)
{
	struct ipc_client_instance *ii = ipc_client_instance(xinst);

	// service considers us to be connected until fd is closed
	ipc_message_channel_close(&ii->ipc_c.imc);

	for (size_t i = 0; i < ii->num_xtracks; i++) {
		u_var_remove_root(ii->xtracks[i]);
		free(ii->xtracks[i]);
		ii->xtracks[i] = NULL;
	}
	ii->num_xtracks = 0;

	os_mutex_destroy(&ii->ipc_c.mutex);

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
int
ipc_instance_create(struct xrt_instance_info *i_info,
                    struct xrt_instance **out_xinst)
{
	struct ipc_client_instance *ii =
	    U_TYPED_CALLOC(struct ipc_client_instance);
	ii->base.select = ipc_client_instance_select;
	ii->base.create_fd_compositor =
	    ipc_client_instance_create_fd_compositor;
	ii->base.get_prober = ipc_client_instance_get_prober;
	ii->base.destroy = ipc_client_instance_destroy;

	// FDs needs to be set to something negative.
	ii->ipc_c.imc.socket_fd = -1;
	ii->ipc_c.ism_fd = -1;

	if (!ipc_connect(&ii->ipc_c)) {
		IPC_ERROR(
		    &ii->ipc_c,
		    "Failed to connect to monado service process\n\n"
		    "###\n"
		    "#\n"
		    "# Please make sure that the service process is running\n"
		    "#\n"
		    "# It is called \"monado-service\"\n"
		    "# For builds it's located "
		    "\"build-dir/src/xrt/targets/service/monado-service\"\n"
		    "#\n"
		    "###\n");
		free(ii);
		return -1;
	}

	// get our xdev shm from the server and mmap it
	xrt_result_t r =
	    ipc_call_instance_get_shm_fd(&ii->ipc_c, &ii->ipc_c.ism_fd, 1);
	if (r != XRT_SUCCESS) {
		IPC_ERROR(&ii->ipc_c, "Failed to retrieve shm fd");
		free(ii);
		return -1;
	}

	struct ipc_app_state desc = {0};
	desc.info = *i_info;
	desc.pid = getpid(); // Extra info.

	r = ipc_call_system_set_client_info(&ii->ipc_c, &desc);
	if (r != XRT_SUCCESS) {
		IPC_ERROR(&ii->ipc_c, "Failed to set instance info");
		free(ii);
		return -1;
	}

	const int flags = MAP_SHARED;
	const int access = PROT_READ | PROT_WRITE;
	const size_t size = sizeof(struct ipc_shared_memory);

	ii->ipc_c.ism = mmap(NULL, size, access, flags, ii->ipc_c.ism_fd, 0);
	if (ii->ipc_c.ism == NULL) {
		IPC_ERROR(&ii->ipc_c, "Failed to mmap shm ");
		free(ii);
		return -1;
	}

	uint32_t count = 0;
	struct xrt_tracking_origin *xtrack = NULL;
	struct ipc_shared_memory *ism = ii->ipc_c.ism;

	// Query the server for how many tracking origins it has.
	count = 0;
	for (uint32_t i = 0; i < ism->num_itracks; i++) {
		xtrack = U_TYPED_CALLOC(struct xrt_tracking_origin);

		memcpy(xtrack->name, ism->itracks[i].name,
		       sizeof(xtrack->name));

		xtrack->type = ism->itracks[i].type;
		xtrack->offset = ism->itracks[i].offset;
		ii->xtracks[count++] = xtrack;

		u_var_add_root(xtrack, "Tracking origin", true);
		u_var_add_ro_text(xtrack, xtrack->name, "name");
		u_var_add_pose(xtrack, &xtrack->offset, "offset");
	}

	ii->num_xtracks = count;

	// Query the server for how many devices it has.
	count = 0;
	for (uint32_t i = 0; i < ism->num_idevs; i++) {
		struct ipc_shared_device *idev = &ism->idevs[i];
		xtrack = ii->xtracks[idev->tracking_origin_index];

		if (idev->name == XRT_DEVICE_GENERIC_HMD) {
			ii->xdevs[count++] =
			    ipc_client_hmd_create(&ii->ipc_c, xtrack, i);
		} else {
			ii->xdevs[count++] =
			    ipc_client_device_create(&ii->ipc_c, xtrack, i);
		}
	}

	ii->num_xdevs = count;

	*out_xinst = &ii->base;

	os_mutex_init(&ii->ipc_c.mutex);

	return 0;
}
