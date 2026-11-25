#include "face-detector.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace himitsukao {

namespace {
std::vector<uchar> read_file(const std::string &path)
{
	// u8path so that non-ASCII install locations work on Windows.
	std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
	if (!in)
		throw std::runtime_error("cannot open model file: " + path);
	return std::vector<uchar>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

FaceDetector::FaceDetector(const std::string &model_path, float score_threshold, float nms_threshold)
{
	const std::vector<uchar> model = read_file(model_path);
	if (model.empty())
		throw std::runtime_error("model file is empty: " + model_path);
	detector_ = cv::FaceDetectorYN::create("onnx", model, std::vector<uchar>(), input_size_, score_threshold,
					       nms_threshold, 5000);
	if (!detector_)
		throw std::runtime_error("cannot create FaceDetectorYN");
}

void FaceDetector::set_score_threshold(float t)
{
	detector_->setScoreThreshold(t);
}

void FaceDetector::set_nms_threshold(float t)
{
	detector_->setNMSThreshold(t);
}

std::vector<Face> FaceDetector::detect(const cv::Mat &image)
{
	std::vector<Face> faces;
	if (image.empty() || image.cols < 8 || image.rows < 8)
		return faces;

	cv::Mat bgr;
	if (image.type() == CV_8UC4)
		cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
	else if (image.type() == CV_8UC3)
		bgr = image;
	else
		throw std::invalid_argument("FaceDetector::detect expects CV_8UC3 or CV_8UC4");

	if (bgr.size() != input_size_) {
		input_size_ = bgr.size();
		detector_->setInputSize(input_size_);
	}

	cv::Mat out;
	detector_->detect(bgr, out);
	if (out.empty() || out.cols < 15)
		return faces;

	const float iw = static_cast<float>(bgr.cols);
	const float ih = static_cast<float>(bgr.rows);
	for (int i = 0; i < out.rows; ++i) {
		const float *r = out.ptr<float>(i);
		const float x1 = std::clamp(r[0] / iw, 0.f, 1.f);
		const float y1 = std::clamp(r[1] / ih, 0.f, 1.f);
		const float x2 = std::clamp((r[0] + r[2]) / iw, 0.f, 1.f);
		const float y2 = std::clamp((r[1] + r[3]) / ih, 0.f, 1.f);
		if (x2 <= x1 || y2 <= y1)
			continue;
		Face f;
		f.rect = Rect{x1, y1, x2 - x1, y2 - y1};
		for (int k = 0; k < 5; ++k) {
			f.landmarks_px[k] = {r[4 + 2 * k], r[5 + 2 * k]};
			f.landmarks[k] = {r[4 + 2 * k] / iw, r[5 + 2 * k] / ih};
		}
		f.score = r[14];
		f.raw = out.row(i).clone();
		faces.push_back(std::move(f));
	}
	return faces;
}

} // namespace himitsukao
