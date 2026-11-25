#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "face-database.h"
#include "identity-manager.h"
#include "runtime-state.h"

namespace himitsukao {

struct LiveFace {
	std::string instance_key;   // which filter instance sees this face
	std::string instance_label; // human readable name of that instance
	int track_id = 0;
	std::string person_id;   // empty = unknown (masked)
	std::string person_name; // empty = unknown
	std::string thumbnail_jpeg_b64;
	int64_t last_seen_ms = 0;
	Rect rect;
};

// Thread-safe facade over the people database and the live filter instances. Step 5 (HTTP server) wraps this
// as is. Pure C++: no OBS headers.
class HimitsukaoService {
public:
	HimitsukaoService() = default;
	HimitsukaoService(const HimitsukaoService &) = delete;
	HimitsukaoService &operator=(const HimitsukaoService &) = delete;

	FaceDatabase &database() { return db_; }
	EnrollmentBoard &board() { return board_; }

	// Global masking state (mode: not persisted; style and strength: persisted by whoever sets the listener).
	RuntimeState &runtime() { return runtime_; }
	void set_mode(Mode mode) { runtime_.set_mode(mode); }
	void set_style(Style style, int strength);
	// Called (on the calling thread) after every set_style().
	void set_style_listener(std::function<void()> listener);

	// Filters register their IdentityManager in create() and MUST unregister it before destroying it.
	std::string register_instance(const std::string &label, IdentityManager *manager);
	void unregister_instance(const std::string &key);

	// Faces currently on screen: tracks seen within `max_age_ms` before `now_ms`.
	std::vector<LiveFace> live_faces(int64_t now_ms, int64_t max_age_ms = 1000) const;

	std::vector<PersonInfo> known_people() const { return db_.list(); }

	// Starts enrolling the face of `track_id` seen by `instance_key`. Returns the request id, or an empty string if
	// the instance does not exist.
	std::string enroll(const std::string &instance_key, int track_id, const std::string &name, bool session);
	EnrollStatus enroll_status(const std::string &request_id) const { return board_.get(request_id); }

	// Removes a person (and saves the database) and re-masks them immediately on every live track.
	bool remove_person(const std::string &person_id);
	// Renames a person and saves the database (session people are never written).
	bool rename_person(const std::string &person_id, const std::string &name);

private:
	struct Instance {
		std::string label;
		IdentityManager *manager = nullptr;
	};

	FaceDatabase db_;
	EnrollmentBoard board_;
	RuntimeState runtime_;
	std::mutex listener_mutex_;
	std::function<void()> style_listener_;
	mutable std::mutex mutex_;
	std::map<std::string, Instance> instances_;
	uint64_t next_key_ = 1;
};

} // namespace himitsukao
