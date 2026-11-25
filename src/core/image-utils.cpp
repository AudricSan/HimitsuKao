#include "image-utils.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace himitsukao {

std::string base64_encode(const unsigned char *data, size_t size)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((size + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < size; i += 3) {
		const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out += table[(v >> 18) & 63];
		out += table[(v >> 12) & 63];
		out += table[(v >> 6) & 63];
		out += table[v & 63];
	}
	if (i + 1 == size) {
		const unsigned v = data[i] << 16;
		out += table[(v >> 18) & 63];
		out += table[(v >> 12) & 63];
		out += "==";
	} else if (i + 2 == size) {
		const unsigned v = (data[i] << 16) | (data[i + 1] << 8);
		out += table[(v >> 18) & 63];
		out += table[(v >> 12) & 63];
		out += table[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

std::string face_thumbnail_b64(const cv::Mat &frame, const Rect &face, int max_side, float margin, int quality)
{
	if (frame.empty() || (frame.type() != CV_8UC4 && frame.type() != CV_8UC3))
		return std::string();
	const Rect r = pad_rect(face, margin);
	const int x0 = std::clamp(static_cast<int>(std::floor(r.x * frame.cols)), 0, frame.cols - 1);
	const int y0 = std::clamp(static_cast<int>(std::floor(r.y * frame.rows)), 0, frame.rows - 1);
	const int x1 = std::clamp(static_cast<int>(std::ceil((r.x + r.w) * frame.cols)), x0 + 1, frame.cols);
	const int y1 = std::clamp(static_cast<int>(std::ceil((r.y + r.h) * frame.rows)), y0 + 1, frame.rows);

	cv::Mat crop = frame(cv::Rect(x0, y0, x1 - x0, y1 - y0));
	cv::Mat bgr;
	if (crop.type() == CV_8UC4)
		cv::cvtColor(crop, bgr, cv::COLOR_BGRA2BGR);
	else
		bgr = crop;

	const double scale = static_cast<double>(max_side) / std::max(bgr.cols, bgr.rows);
	if (scale < 1.0 || bgr.cols < max_side / 2) {
		cv::Mat resized;
		cv::resize(bgr, resized, cv::Size(), scale, scale, scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
		bgr = resized;
	}

	std::vector<uchar> jpeg;
	if (!cv::imencode(".jpg", bgr, jpeg, {cv::IMWRITE_JPEG_QUALITY, quality}))
		return std::string();
	return base64_encode(jpeg.data(), jpeg.size());
}

} // namespace himitsukao
