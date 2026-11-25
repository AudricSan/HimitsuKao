#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace himitsukao {

// Compares two strings in time that depends only on their lengths' maximum, not on where they differ.
bool constant_time_equals(const std::string &a, const std::string &b);

// Random 6-digit PIN (std::random_device).
std::string generate_pin();

enum class AuthResult {
	Ok,
	Denied, // wrong or missing PIN
	Locked, // too many wrong PINs from this address: rejected for a while (HTTP 429)
};

// PIN check with a per-IP lockout: `max_failures` wrong PINs lock that IP for `lock_ms`.
class PinAuthenticator {
public:
	explicit PinAuthenticator(std::string pin = {}, int max_failures = 5, int64_t lock_ms = 60000);

	void set_pin(const std::string &pin);
	std::string pin() const;

	// A missing PIN (empty) is Denied but does not count as a failure (the page has not asked yet).
	AuthResult check(const std::string &ip, const std::string &supplied_pin, int64_t now_ms);
	// Remaining lock time for this IP (0 if not locked).
	int64_t retry_after_ms(const std::string &ip, int64_t now_ms) const;

private:
	struct Entry {
		int failures = 0;
		int64_t locked_until_ms = 0;
	};
	mutable std::mutex mutex_;
	std::string pin_;
	int max_failures_;
	int64_t lock_ms_;
	std::map<std::string, Entry> entries_;
};

} // namespace himitsukao
