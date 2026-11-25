#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "face-database.h"
#include "face-tracker.h"

namespace himitsukao {

enum class EnrollState { Unknown, Pending, Done, Failed };

struct EnrollStatus {
	EnrollState state = EnrollState::Unknown;
	std::string person_id; // set once the person exists (state Done)
	std::string error;     // set when Failed
};

// Thread-safe registry of enrollment requests, shared by every filter instance.
class EnrollmentBoard {
public:
	std::string new_request(); // returns a fresh request id, state Pending
	void set(const std::string &request_id, EnrollState state, const std::string &person_id = {},
		 const std::string &error = {});
	EnrollStatus get(const std::string &request_id) const;

	// Forgets finished (done/failed) requests older than `max_age`. Called automatically (5 min) by new_request().
	void prune(std::chrono::milliseconds max_age);
	size_t size() const;

private:
	struct Entry {
		EnrollStatus status;
		std::chrono::steady_clock::time_point finished;
	};
	mutable std::mutex mutex_;
	std::map<std::string, Entry> requests_;
	uint64_t counter_ = 0;
};

struct IdentityConfig {
	float threshold = 0.363f;          // cosine similarity needed to recognise someone
	int64_t recheck_ms = 1000;         // delay between two recognition attempts on an unknown track
	int64_t enroll_expiry_ms = 3000;   // an enrollment fails / stops if its track yields nothing for this long
	int64_t enroll_spacing_ms = 150;   // minimum delay between two embeddings of the same enrollment
	int enroll_extra_embeddings = 4;   // embeddings added after the first one
	int64_t thumbnail_refresh_ms = 1000;
};

// Per filter instance. process() runs on the worker thread; every other method may be called from any thread.
//
// Product rules implemented here:
//  - recognised (or enrolled) tracks keep their person_id until the track expires: no re-verification;
//  - enrolling sets person_id on the track at the first embedding (the face is un-masked immediately);
//  - removing a person clears person_id on every live track right away (clear_person).
class IdentityManager {
public:
	using EmbedFn = std::function<Embedding(size_t detection_index)>; // empty result = failure
	using ThumbFn = std::function<std::string(size_t detection_index)>;
	using LogFn = std::function<void(const std::string &)>;

	IdentityManager(FaceDatabase &db, EnrollmentBoard &board, IdentityConfig config = {}, LogFn log = nullptr);

	void set_config(const IdentityConfig &config);

	// Enrollments queued or in progress (callers cap this to avoid piling up requests).
	size_t pending_requests() const { return pending_.load(); }

	// Queues an enrollment (the request id comes from EnrollmentBoard::new_request()).
	void request_enroll(const std::string &request_id, int track_id, const std::string &name, bool session);

	// Immediately removes the identity from live tracks carrying `person_id`.
	void clear_person(const std::string &person_id);

	// Worker thread. `ids[i]` is the track id of detection i (FaceTracker::update result). Callbacks compute the
	// embedding / thumbnail of detection i lazily.
	void process(FaceTracker &tracker, const std::vector<int> &ids, int64_t now_ms, const EmbedFn &embed,
		     const ThumbFn &thumb);

	// Copy of the tracks as last published by process() (with later clear_person calls applied).
	std::vector<Track> snapshot() const;
	// Mask rectangles of the published tracks (known people skipped).
	std::vector<Rect> masks(int64_t now_ms, int64_t hold_ms, float padding, bool include_known = false) const;

private:
	struct Request {
		std::string request_id;
		int track_id = 0;
		std::string name;
		bool session = false;
		int64_t created_ms = -1;
		std::string person_id; // empty until the first embedding
		int extra = 0;
		int64_t last_embedding_ms = 0;
		int64_t last_progress_ms = 0;
	};

	bool handle_request(Request &req, FaceTracker &tracker, const std::vector<int> &ids, int64_t now_ms,
			    const IdentityConfig &cfg, const EmbedFn &embed, const ThumbFn &thumb); // true = finished
	void publish(FaceTracker &tracker);
	void log(const std::string &msg) const;

	FaceDatabase &db_;
	EnrollmentBoard &board_;

	mutable std::mutex state_mutex_; // config, incoming requests, pending clears
	IdentityConfig config_;
	std::vector<Request> incoming_;
	std::set<std::string> incoming_clears_;

	std::vector<Request> active_; // worker thread only
	std::atomic<size_t> pending_{0};

	mutable std::mutex snapshot_mutex_;
	std::vector<Track> snapshot_;

	LogFn log_;
};

} // namespace himitsukao
