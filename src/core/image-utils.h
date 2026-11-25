#pragma once

#include <string>

#include <opencv2/core.hpp>

#include "face-tracker.h"

namespace himitsukao {

std::string base64_encode(const unsigned char *data, size_t size);

// Crops `face` (normalized rect, grown by `margin`) out of a BGRA/BGR frame, scales it so its largest side is
// `max_side` pixels and returns it as a base64 JPEG (no "data:" prefix). Empty string on failure.
std::string face_thumbnail_b64(const cv::Mat &frame, const Rect &face, int max_side = 96, float margin = 1.25f,
			       int quality = 80);

} // namespace himitsukao
