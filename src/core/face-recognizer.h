#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/objdetect/face.hpp>

#include "face-database.h"
#include "face-detector.h"

namespace himitsukao {

// Wrapper around cv::FaceRecognizerSF (SFace). Not thread-safe.
class FaceRecognizer {
public:
	static constexpr float kDefaultThreshold = 0.363f; // OpenCV's recommended cosine threshold

	// Throws if the model cannot be loaded.
	explicit FaceRecognizer(const std::string &model_path, float threshold = kDefaultThreshold);

	float threshold() const { return threshold_; }
	void set_threshold(float t) { threshold_ = t; }

	// `frame` is the image the face was detected in (BGRA or BGR) and `face.raw` its YuNet row, in the
	// pixel space of that same frame. Returns an empty embedding if the face cannot be aligned.
	Embedding embed(const cv::Mat &frame, const Face &face);

	// Embeds an already aligned 112x112 BGR crop.
	Embedding embed_aligned(const cv::Mat &aligned_bgr);

private:
	cv::Ptr<cv::FaceRecognizerSF> recognizer_;
	float threshold_;
};

} // namespace himitsukao
