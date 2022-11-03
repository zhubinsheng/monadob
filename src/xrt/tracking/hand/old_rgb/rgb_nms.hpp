// Copyright 2021-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Code to deal with bounding boxes for camera-based hand-tracking.
 * @author Moses Turner <moses@collabora.com>
 * @author Marcus Edel <marcus.edel@collabora.com>
 * @ingroup drv_ht
 */

#include "rgb_sync.hpp"
#include <math.h>

#include "util/u_box_iou.hpp"

using namespace xrt::auxiliary::util::box_iou;
struct NMSPalm
{
	Box bbox;
	struct xrt_vec2 keypoints[7];
	float confidence;
};



static NMSPalm
weightedAvgBoxes(const std::vector<NMSPalm> &detections)
{
	float weight = 0.0f; // or, sum_confidences.
	float cx = 0.0f;
	float cy = 0.0f;
	float size = 0.0f;
	NMSPalm out = {};

	for (const NMSPalm &detection : detections) {
		weight += detection.confidence;
		cx += detection.bbox.cx * detection.confidence;
		cy += detection.bbox.cy * detection.confidence;
		size += detection.bbox.w * .5 * detection.confidence;
		size += detection.bbox.h * .5 * detection.confidence;

		for (int i = 0; i < 7; i++) {
			out.keypoints[i].x += detection.keypoints[i].x * detection.confidence;
			out.keypoints[i].y += detection.keypoints[i].y * detection.confidence;
		}
	}
	cx /= weight;
	cy /= weight;
	size /= weight;
	for (int i = 0; i < 7; i++) {
		out.keypoints[i].x /= weight;
		out.keypoints[i].y /= weight;
	}


	float bare_confidence = weight / detections.size();

	// desmos \frac{1}{1+e^{-.5x}}-.5

	float steep = 0.2;
	float cent = 0.5;

	float exp = detections.size();

	float sigmoid_addendum = (1.0f / (1.0f + pow(M_E, (-steep * exp)))) - cent;

	float diff_bare_to_one = 1.0f - bare_confidence;

	out.confidence = bare_confidence + (sigmoid_addendum * diff_bare_to_one);

	// U_LOG_E("Bare %f num %f sig %f diff %f out %f", bare_confidence, exp, sigmoid_addendum, diff_bare_to_one,
	// out.confidence);

	out.bbox.cx = cx;
	out.bbox.cy = cy;
	out.bbox.w = size;
	out.bbox.h = size;
	return out;
}

std::vector<NMSPalm>
filterBoxesWeightedAvg(const std::vector<NMSPalm> &detections, float min_iou)
{
	std::vector<std::vector<NMSPalm>> overlaps;
	std::vector<NMSPalm> outs;

	// U_LOG_D("\n\nStarting filtering boxes. There are %zu boxes to look at.\n", detections.size());
	for (const NMSPalm &detection : detections) {
		// U_LOG_D("Starting looking at one detection\n");
		bool foundAHome = false;
		for (size_t i = 0; i < outs.size(); i++) {
			float iou = boxIOU(outs[i].bbox, detection.bbox);
			// U_LOG_D("IOU is %f\n", iou);
			// U_LOG_D("Outs box is %f %f %f %f", outs[i].bbox.cx, outs[i].bbox.cy, outs[i].bbox.w,
			// outs[i].bbox.h)
			if (iou > min_iou) {
				// This one intersects with the whole thing
				overlaps[i].push_back(detection);
				outs[i] = weightedAvgBoxes(overlaps[i]);
				foundAHome = true;
				break;
			}
		}
		if (!foundAHome) {
			// U_LOG_D("No home\n");
			overlaps.push_back({detection});
			outs.push_back({detection});
		} else {
			// U_LOG_D("Found a home!\n");
		}
	}
	// U_LOG_D("Sizeeeeeeeeeeeeeeeeeeeee is %zu\n", outs.size());
	return outs;
}
