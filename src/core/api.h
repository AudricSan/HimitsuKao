#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "auth.h"
#include "himitsukao-service.h"

namespace himitsukao {

struct ApiRequest {
	std::string method; // GET, POST, DELETE, PATCH
	std::string path;   // without query string
	std::string body;
	std::string pin;       // value of the X-HimitsuKao-Pin header ("" if absent)
	std::string remote_ip; // for the lockout
};

struct ApiResponse {
	int status = 200;
	std::string content_type = "application/json";
	std::string body;
	bool no_store = false;   // Cache-Control: no-store
	int retry_after_s = 0;   // Retry-After for 429
};

constexpr size_t kMaxBodyBytes = 4096;
constexpr int64_t kLiveWindowMs = 1500;

// The whole HTTP API, as plain functions of (request -> response): the socket layer stays thin and this part is
// unit-tested without sockets.
class ApiHandler {
public:
	using Clock = std::function<int64_t()>; // same time base as the tracker timestamps (milliseconds)

	ApiHandler(HimitsukaoService &service, std::string index_html, std::string pin, Clock clock);

	ApiResponse handle(const ApiRequest &request);

	void set_pin(const std::string &pin) { auth_.set_pin(pin); }
	PinAuthenticator &auth() { return auth_; }

private:
	ApiResponse handle_api(const ApiRequest &request);

	HimitsukaoService &service_;
	std::string index_html_;
	PinAuthenticator auth_;
	Clock clock_;
};

// "  Bob  " -> "Bob"; returns false if the trimmed name is not 1..40 characters or has control characters.
bool normalize_person_name(const std::string &raw, std::string &out);

} // namespace himitsukao
