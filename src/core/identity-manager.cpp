#include "identity-manager.h"

#include <algorithm>
#include <exception>

namespace himitsukao {

// ---------------------------------------------------------------- EnrollmentBoard

std::string EnrollmentBoard::new_request()
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string id = "enroll-" + std::to_string(++counter_);
	EnrollStatus st;
	st.state = EnrollState::Pending;
	requests_[id] = st;
	return id;
}

void EnrollmentBoard::set(const std::string &request_id, EnrollState state, const std::string &person_id,
			  const std::string &error)
{
	std::lock_guard<std::mutex> lock(mutex_);
	EnrollStatus &st = requests_[request_id];
	st.state = state;
	st.person_id = person_id;
	st.error = error;
}

EnrollStatus EnrollmentBoard::get(const std::string &request_id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = requests_.find(request_id);
	return it == requests_.end() ? EnrollStatus{} : it->second;
}

// ---------------------------------------------------------------- IdentityManager

namespace {
Track *find_track(FaceTracker &tracker, int id)
{
	for (auto &t : tracker.tracks()) {
		if (t.id == id)
			return &t;
	}
	return nullptr;
}

int detection_of(const std::vector<int> &ids, int track_id)
{
	for (size_t i = 0; i < ids.size(); ++i) {
		if (ids[i] == track_id)
			return static_cast<int>(i);
	}
	return -1;
}

void clear_identity(Track &t)
{
	t.person_id.clear();
	t.person_name.clear();
	t.last_identity_check_ms = -1;
}
} // namespace

IdentityManager::IdentityManager(FaceDatabase &db, EnrollmentBoard &board, IdentityConfig config, LogFn log)
	: db_(db), board_(board), config_(config), log_(std::move(log))
{
}

void IdentityManager::log(const std::string &msg) const
{
	if (log_)
		log_(msg);
}

void IdentityManager::set_config(const IdentityConfig &config)
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	config_ = config;
}

void IdentityManager::request_enroll(const std::string &request_id, int track_id, const std::string &name,
				     bool session)
{
	Request r;
	r.request_id = request_id;
	r.track_id = track_id;
	r.name = name;
	r.session = session;
	std::lock_guard<std::mutex> lock(state_mutex_);
	incoming_.push_back(std::move(r));
}

void IdentityManager::clear_person(const std::string &person_id)
{
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex_);
		for (auto &t : snapshot_) {
			if (t.person_id == person_id)
				clear_identity(t);
		}
	}
	std::lock_guard<std::mutex> lock(state_mutex_);
	incoming_clears_.insert(person_id);
}

std::vector<Track> IdentityManager::snapshot() const
{
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	return snapshot_;
}

std::vector<Rect> IdentityManager::masks(int64_t now_ms, int64_t hold_ms, float padding, bool include_known) const
{
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	return masks_from_tracks(snapshot_, now_ms, hold_ms, padding, include_known);
}

void IdentityManager::publish(FaceTracker &tracker)
{
	// Apply removals that arrived since the start of process(), so a stale identity is never republished.
	std::set<std::string> clears;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		clears.swap(incoming_clears_);
	}
	if (!clears.empty()) {
		for (auto &t : tracker.tracks()) {
			if (clears.count(t.person_id))
				clear_identity(t);
		}
	}
	std::lock_guard<std::mutex> lock(snapshot_mutex_);
	snapshot_ = tracker.tracks();
}

bool IdentityManager::handle_request(Request &req, FaceTracker &tracker, const std::vector<int> &ids,
				     int64_t now_ms, const IdentityConfig &cfg, const EmbedFn &embed,
				     const ThumbFn &thumb)
{
	if (req.created_ms < 0) {
		req.created_ms = now_ms;
		req.last_progress_ms = now_ms;
	}
	Track *track = find_track(tracker, req.track_id);
	const int di = track ? detection_of(ids, req.track_id) : -1;

	auto safe_embed = [&](int i) -> Embedding {
		try {
			return embed ? embed(static_cast<size_t>(i)) : Embedding{};
		} catch (const std::exception &e) {
			log(std::string("embedding failed: ") + e.what());
			return {};
		}
	};

	if (req.person_id.empty()) {
		if (!track) {
			board_.set(req.request_id, EnrollState::Failed, {}, "track disappeared");
			return true;
		}
		if (!track->person_id.empty()) {
			board_.set(req.request_id, EnrollState::Failed, {}, "face already recognised");
			return true;
		}
		if (di >= 0) {
			const Embedding emb = safe_embed(di);
			if (!emb.empty()) {
				std::string thumbnail;
				try {
					thumbnail = thumb ? thumb(static_cast<size_t>(di)) : std::string();
				} catch (const std::exception &) {
				}
				req.person_id = db_.add_person(req.name, req.session, emb, thumbnail);
				track->person_id = req.person_id;
				track->person_name = req.name;
				track->last_identity_check_ms = now_ms;
				if (!thumbnail.empty()) {
					track->thumbnail_jpeg_b64 = thumbnail;
					track->thumbnail_ms = now_ms;
				}
				req.last_embedding_ms = now_ms;
				req.last_progress_ms = now_ms;
				board_.set(req.request_id, EnrollState::Done, req.person_id);
				return false;
			}
		}
		if (now_ms - req.created_ms > cfg.enroll_expiry_ms) {
			board_.set(req.request_id, EnrollState::Failed, {}, "no usable face before timeout");
			return true;
		}
		return false;
	}

	// The person exists: gather a few more embeddings from later frames of the same track.
	bool finished = false;
	if (di >= 0 && req.extra < cfg.enroll_extra_embeddings && now_ms - req.last_embedding_ms >= cfg.enroll_spacing_ms) {
		const Embedding emb = safe_embed(di);
		if (!emb.empty()) {
			if (!db_.add_embedding(req.person_id, emb))
				finished = true; // person removed in the meantime
			req.extra++;
			req.last_embedding_ms = now_ms;
			req.last_progress_ms = now_ms;
		}
	}
	if (req.extra >= cfg.enroll_extra_embeddings || now_ms - req.last_progress_ms > cfg.enroll_expiry_ms)
		finished = true;
	if (finished && !req.session && !db_.save())
		log("could not save the known people database");
	return finished;
}

void IdentityManager::process(FaceTracker &tracker, const std::vector<int> &ids, int64_t now_ms,
			      const EmbedFn &embed, const ThumbFn &thumb)
{
	IdentityConfig cfg;
	std::set<std::string> clears;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		cfg = config_;
		clears.swap(incoming_clears_);
		for (auto &r : incoming_)
			active_.push_back(std::move(r));
		incoming_.clear();
	}
	if (!clears.empty()) {
		for (auto &t : tracker.tracks()) {
			if (clears.count(t.person_id))
				clear_identity(t);
		}
	}

	// 1. Enrollments
	active_.erase(std::remove_if(active_.begin(), active_.end(),
				     [&](Request &r) {
					     return handle_request(r, tracker, ids, now_ms, cfg, embed, thumb);
				     }),
		      active_.end());

	// 2. Recognition of unknown tracks (never re-verify a recognised one) and thumbnail refresh.
	for (size_t i = 0; i < ids.size(); ++i) {
		Track *t = find_track(tracker, ids[i]);
		if (!t)
			continue;
		const bool enrolling = std::any_of(active_.begin(), active_.end(),
						   [&](const Request &r) { return r.track_id == t->id; });
		if (t->person_id.empty() && !enrolling &&
		    (t->last_identity_check_ms < 0 || now_ms - t->last_identity_check_ms >= cfg.recheck_ms)) {
			t->last_identity_check_ms = now_ms;
			Embedding emb;
			try {
				emb = embed ? embed(i) : Embedding{};
			} catch (const std::exception &e) {
				log(std::string("embedding failed: ") + e.what());
			}
			if (!emb.empty()) {
				if (const auto m = db_.best_match(emb, cfg.threshold)) {
					t->person_id = m->id;
					t->person_name = m->name;
				}
			}
		}
		if (now_ms - t->thumbnail_ms >= cfg.thumbnail_refresh_ms) {
			std::string s;
			try {
				s = thumb ? thumb(i) : std::string();
			} catch (const std::exception &) {
			}
			if (!s.empty()) {
				t->thumbnail_jpeg_b64 = std::move(s);
				t->thumbnail_ms = now_ms;
			}
		}
	}

	publish(tracker);
}

} // namespace himitsukao
