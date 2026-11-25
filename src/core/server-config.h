#pragma once

#include <string>

#include "face-database.h" // LoadResult
#include "runtime-state.h"

namespace himitsukao {

// Contents of server.json. The masking mode is deliberately NOT stored (OBS always starts in "normal").
struct ServerConfig {
	int port = 8787;
	std::string pin; // generated on first run
	Style style = Style::Pixel;
	int strength = 50;

	// Missing file: defaults + a new PIN (result.existed == false, caller should save()).
	// Corrupt file: defaults + a new PIN, the original is copied to `<path>.bad`.
	LoadResult load(const std::string &path);
	bool save(const std::string &path) const; // atomic (temp file + rename)
};

} // namespace himitsukao
