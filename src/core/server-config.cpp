#include "server-config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "auth.h"

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
		pin = generate_pin();
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
		c.port = j.value("port", c.port);
		c.pin = j.value("pin", std::string());
		parse_style(j.value("style", std::string("pixel")), c.style);
		c.strength = std::clamp(j.value("strength", c.strength), kMinStrength, kMaxStrength);
		if (c.port < 1 || c.port > 65535)
			c.port = 8787;
		if (c.pin.empty())
			c.pin = generate_pin();
		*this = c;
		result.message = "server config loaded";
	} catch (const std::exception &e) {
		*this = ServerConfig{};
		pin = generate_pin();
		result.ok = false;
		fs::path bad = p;
		bad += ".bad";
		std::error_code cec;
		fs::copy_file(p, bad, fs::copy_options::overwrite_existing, cec);
		result.message = std::string("server config is corrupt (") + e.what() +
				 "), using defaults and a new PIN; backup: " + bad.u8string();
	}
	return result;
}

bool ServerConfig::save(const std::string &path) const
{
	const json root = {{"port", port}, {"pin", pin}, {"style", style_name(style)}, {"strength", strength}};
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
		fs::rename(tmp, p);
		return true;
	} catch (const std::exception &) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
}

} // namespace himitsukao
