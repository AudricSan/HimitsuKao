#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace himitsukao {

// Compares two strings in time that depends only on their lengths' maximum, not on where they differ.
bool constant_time_equals(const std::string &a, const std::string &b);

constexpr size_t kAccessCodeLength = 10;

// Random access code: kAccessCodeLength characters from an unambiguous lowercase alphabet (no 0/o/1/l/i),
// about 49 bits, drawn from std::random_device.
std::string generate_access_code();

// True if `code` looks like a code this version could have generated (long enough). Used to replace the
// 6-digit PINs of older versions.
bool is_valid_access_code(const std::string &code);

enum class AuthResult {
	Ok,
	Denied, // wrong or missing code
	Locked, // too many wrong codes (from this address, or from everywhere): rejected for a while (HTTP 429)
};

// Access code check with two failure budgets:
//  - per IP: `max_failures` wrong codes lock that IP for `lock_ms`;
//  - global: more than `global_max_failures` wrong codes from all (non-loopback) IPs within `global_window_ms`
//    reject every non-loopback client for `global_lock_ms`.
class CodeAuthenticator {
public:
	struct Limits {
		int max_failures = 5;
		int64_t lock_ms = 60000;
		int global_max_failures = 30;
		int64_t global_window_ms = 600000;
		int64_t global_lock_ms = 600000;
		size_t max_entries = 4096;
	};

	explicit CodeAuthenticator(std::string code = {});
	CodeAuthenticator(std::string code, Limits limits);

	void set_code(const std::string &code);
	std::string code() const;

	// `is_loopback`: loopback clients are exempt from the global lock and from the global failure count.
	// A missing code (empty) is Denied but is not counted as a failure (the page has not asked yet).
	AuthResult check(const std::string &ip, const std::string &supplied_code, int64_t now_ms,
			 bool is_loopback = false);
	// Remaining lock time for this IP (0 if not locked), including the global lock.
	int64_t retry_after_ms(const std::string &ip, int64_t now_ms) const;
	// True exactly once each time the global lock is triggered (so the caller logs one warning).
	bool take_global_lock_event();

	size_t tracked_ips() const; // for tests / diagnostics

private:
	struct Entry {
		int failures = 0;
		int64_t locked_until_ms = 0;
		int64_t last_activity_ms = 0;
	};
	void prune_locked(int64_t now_ms);

	mutable std::mutex mutex_;
	std::string code_;
	Limits limits_;
	std::map<std::string, Entry> entries_;
	std::deque<int64_t> global_failures_;
	int64_t global_locked_until_ms_ = 0;
	bool global_event_ = false;
};

} // namespace himitsukao
