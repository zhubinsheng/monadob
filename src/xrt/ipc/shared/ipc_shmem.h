// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared memory helpers
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_shared
 */

#pragma once

#include <xrt/xrt_handles.h>
#include <xrt/xrt_results.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @class xrt_shmem_handle_t
 * @brief Generic typedef for platform-specific shared memory handle.
 */
/*!
 * Create and map a shared memory region.
 *
 * @param[in] size Desired size of region
 * @param[in,out] out_handle Pointer to the handle to populate. Receives the
 * handle if this succeeds, or the invalid value if it fails.
 * @param[in,out] out_map Pointer to the pointer to populate with the mapping of
 * this shared memory region. On failure, contents are undefined.
 *
 * @public @memberof xrt_shmem_handle_t
 */
xrt_result_t
ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map);

/*!
 * Map a shared memory region.
 *
 * @param[in] handle Handle for region
 * @param[in] size Size of region
 * @param[in,out] out_map Pointer to the pointer to populate with the mapping of
 * this shared memory region.
 *
 * @public @memberof xrt_shmem_handle_t
 */
xrt_result_t
ipc_shmem_map(xrt_shmem_handle_t handle, size_t size, void **out_map);

/*!
 * Unmap a shared memory region.
 *
 * @param[in] map_ptr pointer to region
 * @param[in] size Size of region
 *
 * @public @memberof xrt_shmem_handle_t
 */
void
ipc_shmem_unmap(void **map_ptr, size_t size);

/*!
 * Destroy a handle to a shared memory region.
 *
 * This probably does not destroy the underlying region, if other references to
 * it (in this process or others) are still open.
 *
 * @param[in,out] handle_ptr Pointer to the handle to destroy - will be checked
 *                           for validity, destroyed, and cleared.
 * @param[in,out] map_ptr Pointer to the mapped memory to unmap - will be
 *                        checked for validity, destroyed, and cleared. It's
 *                        necessary unmap the region to destroy the shmem.
 * @param[in] size Size of the mapped region.
 *
 * @public @memberof xrt_shmem_handle_t
 */
void
ipc_shmem_destroy(xrt_shmem_handle_t *handle_ptr, void **map_ptr, size_t size);

#ifdef __cplusplus
}
#endif
