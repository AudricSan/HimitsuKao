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

std::string generate_pin()
{
	std::random_device rd;
	std::uniform_int_distribution<int> dist(0, 999999);
	std::string pin = std::to_string(dist(rd));
	return std::string(6 - pin.size(), '0') + pin;
}

PinAuthenticator::PinAuthenticator(std::string pin, int max_failures, int64_t lock_ms)
	: pin_(std::move(pin)), max_failures_(max_failures), lock_ms_(lock_ms)
{
}

void PinAuthenticator::set_pin(const std::string &pin)
{
	std::lock_guard<std::mutex> lock(mutex_);
	pin_ = pin;
}

std::string PinAuthenticator::pin() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return pin_;
}

int64_t PinAuthenticator::retry_after_ms(const std::string &ip, int64_t now_ms) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = entries_.find(ip);
	if (it == entries_.end() || it->second.locked_until_ms <= now_ms)
		return 0;
	return it->second.locked_until_ms - now_ms;
}

AuthResult PinAuthenticator::check(const std::string &ip, const std::string &supplied_pin, int64_t now_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);

	if (entries_.size() > 1000) { // bound memory: drop expired entries
		for (auto it = entries_.begin(); it != entries_.end();) {
			if (it->second.locked_until_ms <= now_ms)
				it = entries_.erase(it);
			else
				++it;
		}
	}

	Entry &e = entries_[ip];
	if (e.locked_until_ms > now_ms)
		return AuthResult::Locked;
	if (e.locked_until_ms != 0) { // lock just expired: fresh start
		e.locked_until_ms = 0;
		e.failures = 0;
	}

	if (supplied_pin.empty())
		return AuthResult::Denied;
	if (!pin_.empty() && constant_time_equals(supplied_pin, pin_)) {
		e.failures = 0;
		return AuthResult::Ok;
	}
	if (++e.failures >= max_failures_) {
		e.locked_until_ms = now_ms + lock_ms_; // the 5th wrong PIN is still a 401; the next calls get 429
		e.failures = 0;
	}
	return AuthResult::Denied;
}

} // namespace himitsukao
