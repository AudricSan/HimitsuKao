#include "server-config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "auth.h"
#include "file-util.h"

namespace himitsukao {

namespace fs = std::filesystem;
using json = nlohmann::json;

LoadResult ServerConfig::load(const std::string &path)
{
	*this = ServerConfig{};
	LoadResult result;
	const fs::path p = fs::u8path(path);
	std::error_code ec;
	if (!fs::exists(p, ec)) {
		code = generate_access_code();
		result.needs_save = true;
		result.message = "no server config yet, generated defaults";
		return result;
	}
	result.existed = true;
	try {
		std::ifstream in(p, std::ios::binary);
		if (!in)
			throw std::runtime_error("cannot open file");
		const json j = json::parse(in);
		ServerConfig c;
		c.enabled = j.value("enabled", false);
		c.port = j.value("port", c.port);
		c.code = j.value("code", std::string());
		parse_style(j.value("style", std::string("pixel")), c.style);
		c.strength = std::clamp(j.value("strength", c.strength), kMinStrength, kMaxStrength);
		if (c.port < 1024 || c.port > 65535)
			c.port = 8787;
		if (!is_valid_access_code(c.code)) {
			// Missing, or an old 6-digit PIN: replace it (the old "pin" key is simply dropped on the next save).
			c.code = generate_access_code();
			result.needs_save = true;
			result.message = "access code (re)generated";
		} else {
			result.message = "server config loaded";
		}
		if (j.contains("pin"))
			result.needs_save = true; // drop the legacy key
		*this = c;
	} catch (const std::exception &e) {
		*this = ServerConfig{};
		code = generate_access_code();
		result.ok = false;
		result.needs_save = true;
		fs::path bad = p;
		bad += ".bad";
		std::error_code cec;
		fs::copy_file(p, bad, fs::copy_options::overwrite_existing, cec);
		result.message = std::string("server config is corrupt (") + e.what() +
				 "), using defaults and a new access code; backup: " + bad.u8string();
	}
	return result;
}

bool ServerConfig::save(const std::string &path) const
{
	const json root = {{"enabled", enabled}, {"port", port},         {"code", code},
			   {"style", style_name(style)}, {"strength", strength}};
	const fs::path p = fs::u8path(path);
	fs::path tmp = p;
	tmp += ".tmp";
	try {
		std::error_code ec;
		if (p.has_parent_path())
			fs::create_directories(p.parent_path(), ec);
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;
			out << root.dump(2);
			out.flush();
			if (!out)
				return false;
		}
		restrict_to_owner(tmp);
		fs::rename(tmp, p);
		return true;
	} catch (const std::exception &) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
}

} // namespace himitsukao
