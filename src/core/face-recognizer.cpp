#include "face-recognizer.h"

#include <cmath>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace himitsukao {

namespace {
// cv::FaceRecognizerSF (4.10) only loads from a path, through narrow-char APIs. On Windows use the 8.3 short
// path so that installs under a non-ASCII user name still work.
std::string loadable_path(const std::string &utf8_path)
{
#ifdef _WIN32
	const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
	if (wlen > 0) {
		std::wstring wide(static_cast<size_t>(wlen), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wide.data(), wlen);
		wide.resize(static_cast<size_t>(wlen - 1));
		const DWORD slen = GetShortPathNameW(wide.c_str(), nullptr, 0);
		if (slen > 0) {
			std::wstring shortp(slen, L'\0');
			const DWORD got = GetShortPathNameW(wide.c_str(), shortp.data(), slen);
			shortp.resize(got);
			std::string out;
			bool ascii = true;
			for (wchar_t c : shortp) {
				if (c > 127)
					ascii = false;
				out.push_back(static_cast<char>(c));
			}
			if (ascii && !out.empty())
				return out;
		}
	}
#endif
	return utf8_path;
}
} // namespace

FaceRecognizer::FaceRecognizer(const std::string &model_path, float threshold) : threshold_(threshold)
{
	recognizer_ = cv::FaceRecognizerSF::create(loadable_path(model_path), "");
	if (!recognizer_)
		throw std::runtime_error("cannot create FaceRecognizerSF");
}

Embedding FaceRecognizer::embed_aligned(const cv::Mat &aligned_bgr)
{
	cv::Mat feature;
	recognizer_->feature(aligned_bgr, feature);
	if (feature.empty())
		return {};
	feature = feature.reshape(1, 1);
	cv::Mat f32;
	feature.convertTo(f32, CV_32F);
	Embedding e(f32.begin<float>(), f32.end<float>());
	return l2_normalize(std::move(e));
}

Embedding FaceRecognizer::embed(const cv::Mat &frame, const Face &face)
{
	if (frame.empty() || face.raw.empty())
		return {};
	cv::Mat bgr;
	if (frame.type() == CV_8UC4)
		cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
	else if (frame.type() == CV_8UC3)
		bgr = frame;
	else
		return {};
	cv::Mat aligned;
	recognizer_->alignCrop(bgr, face.raw, aligned);
	if (aligned.empty())
		return {};
	return embed_aligned(aligned);
}

} // namespace himitsukao
