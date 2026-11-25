#include "auth.h"

#include <algorithm>
#include <random>

namespace himitsukao {

bool constant_time_equals(const std::string &a, const std::string &b)
{
	const size_t n = std::max(a.size(), b.size());
	unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
	for (size_t i = 0; i < n; ++i) {
		const unsigned ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
		const unsigned cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
		diff |= ca ^ cb;
	}
	return diff == 0;
}

std::string generate_access_code()
{
	// 23 letters (no i, l, o) + 8 digits (no 0, 1) = 31 symbols: 10 characters ~ 49.5 bits.
	static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
	constexpr size_t n = sizeof(alphabet) - 1;
	std::random_device rd;
	std::uniform_int_distribution<size_t> dist(0, n - 1);
	std::string code;
	for (size_t i = 0; i < kAccessCodeLength; ++i)
		code += alphabet[dist(rd)];
	return code;
}

bool is_valid_access_code(const std::string &code)
{
	return code.size() >= kAccessCodeLength;
}

CodeAuthenticator::CodeAuthenticator(std::string code) : code_(std::move(code)) {}

CodeAuthenticator::CodeAuthenticator(std::string code, Limits limits) : code_(std::move(code)), limits_(limits) {}

void CodeAuthenticator::set_code(const std::string &code)
{
	std::lock_guard<std::mutex> lock(mutex_);
	code_ = code;
}

std::string CodeAuthenticator::code() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return code_;
}

int64_t CodeAuthenticator::retry_after_ms(const std::string &ip, int64_t now_ms) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t until = global_locked_until_ms_;
	const auto it = entries_.find(ip);
	if (it != entries_.end())
		until = std::max(until, it->second.locked_until_ms);
	return until > now_ms ? until - now_ms : 0;
}

bool CodeAuthenticator::take_global_lock_event()
{
	std::lock_guard<std::mutex> lock(mutex_);
	const bool e = global_event_;
	global_event_ = false;
	return e;
}

size_t CodeAuthenticator::tracked_ips() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return entries_.size();
}

// Drops only expired entries (not locked, and no failure for a whole global window): partial counts of recent
// attackers are kept. If the table is still full, the least recently active entries go first.
void CodeAuthenticator::prune_locked(int64_t now_ms)
{
	if (entries_.size() < limits_.max_entries)
		return;
	for (auto it = entries_.begin(); it != entries_.end();) {
		const Entry &e = it->second;
		if (e.locked_until_ms <= now_ms && now_ms - e.last_activity_ms > limits_.global_window_ms)
			it = entries_.erase(it);
		else
			++it;
	}
	while (entries_.size() >= limits_.max_entries) {
		auto oldest = entries_.begin();
		for (auto it = entries_.begin(); it != entries_.end(); ++it)
			if (it->second.last_activity_ms < oldest->second.last_activity_ms)
				oldest = it;
		entries_.erase(oldest);
	}
}

AuthResult CodeAuthenticator::check(const std::string &ip, const std::string &supplied_code, int64_t now_ms,
				    bool is_loopback)
{
	std::lock_guard<std::mutex> lock(mutex_);

	// Global lock: every remote client is rejected, even with the right code.
	if (!is_loopback && global_locked_until_ms_ > now_ms)
		return AuthResult::Locked;

	auto it = entries_.find(ip);
	if (it != entries_.end()) {
		Entry &e = it->second;
		if (e.locked_until_ms > now_ms)
			return AuthResult::Locked;
		if (e.locked_until_ms != 0) { // lock just expired: fresh start
			e.locked_until_ms = 0;
			e.failures = 0;
		}
	}

	if (supplied_code.empty())
		return AuthResult::Denied;
	if (!code_.empty() && constant_time_equals(supplied_code, code_)) {
		if (it != entries_.end())
			it->second.failures = 0;
		return AuthResult::Ok;
	}

	// A wrong code.
	if (it == entries_.end()) {
		prune_locked(now_ms);
		it = entries_.emplace(ip, Entry{}).first;
	}
	Entry &e = it->second;
	e.last_activity_ms = now_ms;
	if (++e.failures >= limits_.max_failures) {
		e.locked_until_ms = now_ms + limits_.lock_ms; // this attempt is still a plain "denied"; the next ones are locked
		e.failures = 0;
	}

	if (!is_loopback) {
		global_failures_.push_back(now_ms);
		while (!global_failures_.empty() && now_ms - global_failures_.front() > limits_.global_window_ms)
			global_failures_.pop_front();
		if (static_cast<int>(global_failures_.size()) > limits_.global_max_failures) {
			global_locked_until_ms_ = now_ms + limits_.global_lock_ms;
			global_failures_.clear();
			global_event_ = true;
		}
	}
	return AuthResult::Denied;
}

} // namespace himitsukao
