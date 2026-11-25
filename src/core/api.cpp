#include "api.h"

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

namespace himitsukao {

using json = nlohmann::json;

namespace {

ApiResponse json_response(int status, const json &j)
{
	ApiResponse r;
	r.status = status;
	r.body = j.dump();
	r.no_store = true;
	return r;
}

ApiResponse error_response(int status, const std::string &message)
{
	return json_response(status, json{{"error", message}});
}

std::string thumbnail_uri(const std::string &b64)
{
	return b64.empty() ? std::string() : "data:image/jpeg;base64," + b64;
}

const char *enroll_state_name(EnrollState s)
{
	switch (s) {
	case EnrollState::Pending:
		return "pending";
	case EnrollState::Done:
		return "done";
	case EnrollState::Failed:
		return "failed";
	case EnrollState::Unknown:
		break;
	}
	return "unknown";
}

std::vector<std::string> split_path(const std::string &path)
{
	std::vector<std::string> parts;
	size_t i = 0;
	while (i < path.size()) {
		if (path[i] == '/') {
			++i;
			continue;
		}
		const size_t j = path.find('/', i);
		parts.push_back(path.substr(i, j == std::string::npos ? std::string::npos : j - i));
		if (j == std::string::npos)
			break;
		i = j;
	}
	return parts;
}

bool parse_body(const std::string &body, json &out)
{
	out = json::parse(body, nullptr, false);
	return out.is_object();
}

} // namespace

bool normalize_person_name(const std::string &raw, std::string &out)
{
	size_t b = 0, e = raw.size();
	while (b < e && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\n' || raw[b] == '\r'))
		++b;
	while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t' || raw[e - 1] == '\n' || raw[e - 1] == '\r'))
		--e;
	const std::string s = raw.substr(b, e - b);
	size_t chars = 0;
	for (unsigned char c : s) {
		if (c < 0x20 || c == 0x7f)
			return false;
		if ((c & 0xC0) != 0x80) // count UTF-8 code points, not continuation bytes
			++chars;
	}
	if (chars < 1 || chars > 40)
		return false;
	out = s;
	return true;
}

ApiHandler::ApiHandler(HimitsukaoService &service, std::string index_html, std::string code, Clock clock, LogFn log)
	: service_(service), index_html_(std::move(index_html)), auth_(std::move(code)), log_(std::move(log)), clock_(std::move(clock))
{
}

ApiResponse ApiHandler::handle(const ApiRequest &request)
{
	// Trusted networks only, and only under an IP-literal / localhost Host (DNS rebinding): checked before anything
	// else, the page included.
	if (!is_allowed_client_ip(request.remote_ip))
		return error_response(403, "forbidden");
	if (!is_acceptable_host_header(request.host))
		return error_response(421, "misdirected request");

	if (request.path == "/" || request.path == "/index.html") {
		if (request.method != "GET")
			return error_response(405, "method not allowed");
		ApiResponse r;
		r.content_type = "text/html; charset=utf-8";
		r.body = index_html_;
		r.no_store = true;
		return r;
	}
	if (request.path.compare(0, 5, "/api/") != 0)
		return error_response(404, "not found");

	const int64_t now = clock_();
	const AuthResult auth = auth_.check(request.remote_ip, request.code, now, is_loopback_ip(request.remote_ip));
	if (log_ && auth_.take_global_lock_event())
		log_("too many failed authentication attempts: rejecting all remote clients for 10 minutes");
	switch (auth) {
	case AuthResult::Ok:
		break;
	case AuthResult::Locked: {
		ApiResponse r = error_response(429, "too many wrong access codes, try again later");
		r.retry_after_s = static_cast<int>((auth_.retry_after_ms(request.remote_ip, now) + 999) / 1000);
		return r;
	}
	case AuthResult::Denied:
		return error_response(401, "access code required");
	}

	if (request.body.size() > kMaxBodyBytes)
		return error_response(413, "body too large");
	return handle_api(request);
}

ApiResponse ApiHandler::handle_api(const ApiRequest &req)
{
	const std::vector<std::string> p = split_path(req.path); // "api", ...
	const std::string &m = req.method;
	RuntimeState &rt = service_.runtime();
	json body;

	if (p.size() == 2 && p[1] == "state") {
		if (m != "GET")
			return error_response(405, "method not allowed");
		return json_response(200, json{{"mode", mode_name(rt.mode())},
					       {"style", style_name(rt.style())},
					       {"strength", rt.strength()},
					       {"version", rt.version()}});
	}

	if (p.size() == 2 && p[1] == "live") {
		if (m != "GET")
			return error_response(405, "method not allowed");
		const int64_t now = clock_();
		std::vector<LiveFace> faces = service_.live_faces(now, kLiveWindowMs);
		std::stable_sort(faces.begin(), faces.end(), [](const LiveFace &a, const LiveFace &b) {
			return a.person_id.empty() && !b.person_id.empty(); // unknown faces first
		});
		json arr = json::array();
		for (const auto &f : faces) {
			arr.push_back({{"instance", f.instance_key},
				       {"label", f.instance_label},
				       {"track_id", f.track_id},
				       {"person_id", f.person_id},
				       {"name", f.person_name},
				       {"thumbnail", thumbnail_uri(f.thumbnail_jpeg_b64)},
				       {"last_seen_ms_ago", std::max<int64_t>(0, now - f.last_seen_ms)}});
		}
		return json_response(200, arr);
	}

	if (p.size() >= 2 && p[1] == "known") {
		if (p.size() == 2 && m == "GET") {
			json arr = json::array();
			for (const auto &person : service_.known_people()) {
				arr.push_back({{"id", person.id},
					       {"name", person.name},
					       {"session", person.session},
					       {"thumbnail", thumbnail_uri(person.thumbnail_jpeg_b64)},
					       {"created", person.created}});
			}
			return json_response(200, arr);
		}
		if (p.size() == 2 && m == "POST") {
			if (!parse_body(req.body, body))
				return error_response(400, "invalid JSON");
			std::string name;
			if (!body.contains("name") || !body["name"].is_string() ||
			    !normalize_person_name(body["name"].get<std::string>(), name))
				return error_response(400, "name must be 1 to 40 characters");
			if (!body.contains("instance") || !body["instance"].is_string() || !body.contains("track_id") ||
			    !body["track_id"].is_number_integer())
				return error_response(400, "instance and track_id are required");
			const bool session = body.contains("session") && body["session"].is_boolean() &&
					     body["session"].get<bool>();
			EnrollError enroll_error = EnrollError::None;
			const std::string id = service_.enroll(body["instance"].get<std::string>(),
							       body["track_id"].get<int>(), name, session, &enroll_error);
			if (enroll_error == EnrollError::Busy) {
				ApiResponse busy = error_response(429, "too many enrollments in progress");
				busy.retry_after_s = 2;
				return busy;
			}
			if (id.empty())
				return error_response(404, "unknown camera/filter instance");
			return json_response(202, json{{"request_id", id}});
		}
		if (p.size() == 3 && m == "DELETE") {
			if (!service_.remove_person(p[2]))
				return error_response(404, "unknown person");
			return json_response(200, json{{"ok", true}});
		}
		if (p.size() == 3 && m == "PATCH") {
			std::string name;
			if (!parse_body(req.body, body) || !body.contains("name") || !body["name"].is_string() ||
			    !normalize_person_name(body["name"].get<std::string>(), name))
				return error_response(400, "name must be 1 to 40 characters");
			if (!service_.rename_person(p[2], name))
				return error_response(404, "unknown person");
			return json_response(200, json{{"ok", true}, {"name", name}});
		}
		return error_response(405, "method not allowed");
	}

	if (p.size() == 3 && p[1] == "enroll") {
		if (m != "GET")
			return error_response(405, "method not allowed");
		const EnrollStatus st = service_.enroll_status(p[2]);
		if (st.state == EnrollState::Unknown)
			return error_response(404, "unknown request");
		return json_response(200, json{{"state", enroll_state_name(st.state)},
					       {"person_id", st.person_id},
					       {"error", st.error}});
	}

	if (p.size() == 2 && p[1] == "mode") {
		if (m != "POST")
			return error_response(405, "method not allowed");
		Mode mode;
		if (!parse_body(req.body, body) || !body.contains("mode") || !body["mode"].is_string() ||
		    !parse_mode(body["mode"].get<std::string>(), mode))
			return error_response(400, "mode must be normal, faces_all, frame_all or off");
		service_.set_mode(mode);
		return json_response(200, json{{"mode", mode_name(rt.mode())}});
	}

	if (p.size() == 2 && p[1] == "style") {
		if (m != "POST")
			return error_response(405, "method not allowed");
		Style style;
		if (!parse_body(req.body, body) || !body.contains("style") || !body["style"].is_string() ||
		    !parse_style(body["style"].get<std::string>(), style))
			return error_response(400, "style must be pixel, gaussian or solid");
		int strength = rt.strength();
		if (body.contains("strength")) {
			if (!body["strength"].is_number_integer())
				return error_response(400, "strength must be an integer");
			strength = body["strength"].get<int>();
			if (strength < kMinStrength || strength > kMaxStrength)
				return error_response(400, "strength must be between 1 and 100");
		}
		service_.set_style(style, strength);
		return json_response(200, json{{"style", style_name(rt.style())}, {"strength", rt.strength()}});
	}

	return error_response(404, "not found");
}

} // namespace himitsukao
