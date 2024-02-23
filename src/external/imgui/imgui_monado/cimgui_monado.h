// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Custom imgui elements.
 * @author Christoph Haag <christoph.haag@collabora.com>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void igPlotTimings(const char *label,
                   float (*values_getter)(void *data, int idx), void *data,
                   int values_count, int values_offset,
                   const char *overlay_text, float scale_min, float scale_max,
                   ImVec2 frame_size, float reference_timing,
                   bool center_reference_timing, float range, const char *unit,
                   bool dynamic_rescale);

void igToggleButton(const char *str_id, bool *v);

void igImageBg(ImTextureID user_texture_id,
               const ImVec2 size,
               const ImVec2 uv0, const ImVec2 uv1,
               const ImVec4 tint_col, const ImVec4 border_col, const ImVec4 bg_col);

#ifdef __cplusplus
}
#endif
