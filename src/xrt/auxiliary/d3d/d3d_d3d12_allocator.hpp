// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Higher-level D3D12-backed image buffer allocation routine.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Fernando Velazquez Innella <finnella@magicleap.com>
 * @ingroup aux_d3d
 */

#pragma once

#include "xrt/xrt_compositor.h"

#include <Unknwn.h>
#include <d3d12.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <vector>


namespace xrt::auxiliary::d3d::d3d12 {

/**
 * Allocate images (ID3D12Resource) that have a corresponding native handle.
 *
 * @param device A D3D12 device to allocate with.
 * @param xsci Swapchain create info: note that the format is assumed to be a DXGI_FORMAT (conversion to typeless is
 * automatic)
 * @param image_count The number of images to create.
 * @param keyed_mutex Whether to create images with a shared "keyed mutex" as well
 * @param[out] out_images A vector that will be cleared and populated with the images.
 * @param[out] out_handles A vector that will be cleared and populated with the corresponding native handles.
 *
 * @return xrt_result_t, one of:
 * - @ref XRT_SUCCESS
 * - @ref XRT_ERROR_ALLOCATION
 * - @ref XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED
 * - @ref XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED
 * - @ref XRT_ERROR_D3D12
 */
xrt_result_t
allocateSharedImages(ID3D12Device &device,
                     const xrt_swapchain_create_info &xsci,
                     size_t image_count,
                     std::vector<wil::com_ptr<ID3D12Resource>> &out_images,
                     std::vector<wil::unique_handle> &out_handles);

}; // namespace xrt::auxiliary::d3d::d3d12
