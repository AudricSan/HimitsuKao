#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace himitsukao {

// Axis-aligned rectangle in normalized image coordinates (0..1).
struct Rect {
	float x = 0.f;
	float y = 0.f;
	float w = 0.f;
	float h = 0.f;
};

float rect_iou(const Rect &a, const Rect &b);

// Grows a rectangle around its center by `padding` and clamps it to 0..1.
Rect pad_rect(const Rect &r, float padding);

struct Track {
	int id = 0;
	Rect rect;                // smoothed (EMA) rectangle
	int64_t last_seen_ms = 0; // timestamp of the last matching detection
	int hits = 0;             // number of detections matched to this track
	// Identity of the person, empty = unknown. Filled by later steps (face recognition);
	// known people will be skipped when masking.
	std::string person_id;
	std::string person_name;           // name of the recognised person, empty = unknown
	std::string thumbnail_jpeg_b64;    // small JPEG of the latest face crop (live view)
	int64_t thumbnail_ms = -1000000;   // when the thumbnail was last refreshed
	int64_t last_identity_check_ms = -1; // last recognition attempt (-1 = never)
};

// Same as FaceTracker::masks() (known people are skipped) but on a plain list of tracks (e.g. a snapshot published to another thread).
std::vector<Rect> masks_from_tracks(const std::vector<Track> &tracks, int64_t now_ms, int64_t hold_ms = 300,
				    float padding = 1.3f, bool include_known = false);

struct TrackerConfig {
	float iou_threshold = 0.3f; // minimum IoU to match a detection to a track
	float ema_alpha = 0.6f;     // weight of the new detection in the smoothed rectangle (1 = no smoothing)
	int64_t max_age_ms = 1000;  // tracks unseen for longer than this are dropped
};

// Greedy IoU tracker. Not thread-safe: use from one thread (or guard externally).
class FaceTracker {
public:
	explicit FaceTracker(TrackerConfig config = {});

	// Matches detections to tracks, creates new tracks, drops expired ones.
	// Returns, for each detection (same order), the id of the track it was assigned to.
	std::vector<int> update(const std::vector<Rect> &detections, int64_t now_ms);

	// Rectangles to mask now: tracks seen within hold_ms and not recognised (empty person_id), grown by
	// `padding` (centered, clamped to 0..1).
	std::vector<Rect> masks(int64_t now_ms, int64_t hold_ms = 300, float padding = 1.3f) const;

	const std::vector<Track> &tracks() const { return tracks_; }
	std::vector<Track> &tracks() { return tracks_; }
	void clear();

	const TrackerConfig &config() const { return config_; }
	void set_config(const TrackerConfig &config) { config_ = config; }

private:
	TrackerConfig config_;
	std::vector<Track> tracks_;
	int next_id_ = 1;
};

} // namespace himitsukao
