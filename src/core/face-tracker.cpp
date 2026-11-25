#include "face-tracker.h"

#include <algorithm>
#include <tuple>

namespace himitsukao {

float rect_iou(const Rect &a, const Rect &b)
{
	const float x1 = std::max(a.x, b.x);
	const float y1 = std::max(a.y, b.y);
	const float x2 = std::min(a.x + a.w, b.x + b.w);
	const float y2 = std::min(a.y + a.h, b.y + b.h);
	const float iw = x2 - x1;
	const float ih = y2 - y1;
	if (iw <= 0.f || ih <= 0.f)
		return 0.f;
	const float inter = iw * ih;
	const float uni = a.w * a.h + b.w * b.h - inter;
	return uni > 0.f ? inter / uni : 0.f;
}

Rect pad_rect(const Rect &r, float padding)
{
	const float nw = r.w * padding;
	const float nh = r.h * padding;
	const float x1 = std::clamp(r.x + r.w * 0.5f - nw * 0.5f, 0.f, 1.f);
	const float y1 = std::clamp(r.y + r.h * 0.5f - nh * 0.5f, 0.f, 1.f);
	const float x2 = std::clamp(r.x + r.w * 0.5f + nw * 0.5f, 0.f, 1.f);
	const float y2 = std::clamp(r.y + r.h * 0.5f + nh * 0.5f, 0.f, 1.f);
	return Rect{x1, y1, x2 - x1, y2 - y1};
}

FaceTracker::FaceTracker(TrackerConfig config) : config_(config) {}

std::vector<int> FaceTracker::update(const std::vector<Rect> &detections, int64_t now_ms)
{
	std::vector<int> assigned(detections.size(), 0);
	std::vector<bool> det_used(detections.size(), false);
	std::vector<bool> track_used(tracks_.size(), false);

	// All candidate pairs above the threshold, best IoU first.
	std::vector<std::tuple<float, size_t, size_t>> pairs;
	for (size_t t = 0; t < tracks_.size(); ++t) {
		for (size_t d = 0; d < detections.size(); ++d) {
			const float iou = rect_iou(tracks_[t].rect, detections[d]);
			if (iou >= config_.iou_threshold)
				pairs.emplace_back(iou, t, d);
		}
	}
	std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) {
		if (std::get<0>(a) != std::get<0>(b))
			return std::get<0>(a) > std::get<0>(b);
		if (std::get<1>(a) != std::get<1>(b))
			return std::get<1>(a) < std::get<1>(b);
		return std::get<2>(a) < std::get<2>(b);
	});

	const float alpha = std::clamp(config_.ema_alpha, 0.f, 1.f);
	for (const auto &pair : pairs) {
		const size_t t = std::get<1>(pair);
		const size_t d = std::get<2>(pair);
		if (track_used[t] || det_used[d])
			continue;
		track_used[t] = true;
		det_used[d] = true;
		Track &tr = tracks_[t];
		const Rect &det = detections[d];
		tr.rect.x = alpha * det.x + (1.f - alpha) * tr.rect.x;
		tr.rect.y = alpha * det.y + (1.f - alpha) * tr.rect.y;
		tr.rect.w = alpha * det.w + (1.f - alpha) * tr.rect.w;
		tr.rect.h = alpha * det.h + (1.f - alpha) * tr.rect.h;
		tr.last_seen_ms = now_ms;
		tr.hits++;
		assigned[d] = tr.id;
	}

	for (size_t d = 0; d < detections.size(); ++d) {
		if (det_used[d])
			continue;
		Track tr;
		tr.id = next_id_++;
		tr.rect = detections[d];
		tr.last_seen_ms = now_ms;
		tr.hits = 1;
		assigned[d] = tr.id;
		tracks_.push_back(std::move(tr));
	}

	tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
				     [&](const Track &t) { return now_ms - t.last_seen_ms > config_.max_age_ms; }),
		      tracks_.end());
	return assigned;
}

std::vector<Rect> masks_from_tracks(const std::vector<Track> &tracks, int64_t now_ms, int64_t hold_ms, float padding,
				    bool include_known)
{
	std::vector<Rect> out;
	for (const Track &t : tracks) {
		if ((include_known || t.person_id.empty()) && now_ms - t.last_seen_ms <= hold_ms)
			out.push_back(pad_rect(t.rect, padding));
	}
	return out;
}

std::vector<Rect> FaceTracker::masks(int64_t now_ms, int64_t hold_ms, float padding) const
{
	return masks_from_tracks(tracks_, now_ms, hold_ms, padding);
}

void FaceTracker::clear()
{
	tracks_.clear();
}

} // namespace himitsukao
