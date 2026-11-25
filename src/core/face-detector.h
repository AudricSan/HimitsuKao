#pragma once

#include <array>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/objdetect/face.hpp>

#include "face-tracker.h"

namespace himitsukao {

struct Face {
	Rect rect; // normalized 0..1
	float score = 0.f;
	// Landmarks: right eye, left eye, nose tip, right mouth corner, left mouth corner.
	std::array<std::array<float, 2>, 5> landmarks{};    // normalized 0..1
	std::array<std::array<float, 2>, 5> landmarks_px{}; // pixels of the analysed image
	cv::Mat raw; // 1x15 YuNet row (pixels), as FaceRecognizerSF::alignCrop expects
};

// Thin wrapper around cv::FaceDetectorYN (YuNet). Not thread-safe.
class FaceDetector {
public:
	// Throws std::runtime_error / cv::Exception if the model cannot be loaded.
	explicit FaceDetector(const std::string &model_path, float score_threshold = 0.5f, float nms_threshold = 0.3f);

	void set_score_threshold(float t);
	void set_nms_threshold(float t);

	// `image` must be CV_8UC4 (BGRA) or CV_8UC3 (BGR).
	std::vector<Face> detect(const cv::Mat &image);

private:
	cv::Ptr<cv::FaceDetectorYN> detector_;
	cv::Size input_size_{320, 320};
};

} // namespace himitsukao
