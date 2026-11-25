#include "himitsukao-service.h"

namespace himitsukao {

std::string HimitsukaoService::register_instance(const std::string &label, IdentityManager *manager)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string key = "f" + std::to_string(next_key_++);
	instances_[key] = Instance{label, manager};
	return key;
}

void HimitsukaoService::unregister_instance(const std::string &key)
{
	std::lock_guard<std::mutex> lock(mutex_);
	instances_.erase(key);
}

std::vector<LiveFace> HimitsukaoService::live_faces(int64_t now_ms, int64_t max_age_ms) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<LiveFace> out;
	for (const auto &kv : instances_) {
		for (const Track &t : kv.second.manager->snapshot()) {
			if (now_ms - t.last_seen_ms > max_age_ms)
				continue;
			LiveFace f;
			f.instance_key = kv.first;
			f.instance_label = kv.second.label;
			f.track_id = t.id;
			f.person_id = t.person_id;
			f.person_name = t.person_name;
			f.thumbnail_jpeg_b64 = t.thumbnail_jpeg_b64;
			f.last_seen_ms = t.last_seen_ms;
			f.rect = t.rect;
			out.push_back(std::move(f));
		}
	}
	return out;
}

std::string HimitsukaoService::enroll(const std::string &instance_key, int track_id, const std::string &name,
				      bool session)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = instances_.find(instance_key);
	if (it == instances_.end())
		return {};
	const std::string request_id = board_.new_request();
	it->second.manager->request_enroll(request_id, track_id, name, session);
	return request_id;
}

bool HimitsukaoService::remove_person(const std::string &person_id)
{
	const bool removed = db_.remove(person_id);
	if (!removed)
		return false;
	db_.save();
	// After the database removal, so nothing can match this person again.
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &kv : instances_)
		kv.second.manager->clear_person(person_id);
	return true;
}

void HimitsukaoService::set_style(Style style, int strength)
{
	runtime_.set_style(style, strength);
	std::function<void()> listener;
	{
		std::lock_guard<std::mutex> lock(listener_mutex_);
		listener = style_listener_;
	}
	if (listener)
		listener();
}

void HimitsukaoService::set_style_listener(std::function<void()> listener)
{
	std::lock_guard<std::mutex> lock(listener_mutex_);
	style_listener_ = std::move(listener);
}

bool HimitsukaoService::rename_person(const std::string &person_id, const std::string &name)
{
	if (!db_.rename(person_id, name))
		return false;
	db_.save();
	return true;
}

} // namespace himitsukao
