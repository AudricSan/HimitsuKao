#pragma once

#include <string>

#include "face-database.h" // LoadResult
#include "runtime-state.h"

namespace himitsukao {

// Contents of server.json. The masking mode is deliberately NOT stored (OBS always starts in "normal").
struct ServerConfig {
	bool enabled = false; // the phone page server is opt-in: nothing listens until this is turned on
	int port = 8787;
	std::string code; // access code, generated on first run
	Style style = Style::Pixel;
	int strength = 50;

	// Missing file: defaults + a new code (result.needs_save is set).
	// Corrupt file: defaults + a new code, the original is copied to `<path>.bad`.
	// An old 6-digit "pin" (previous versions) is replaced by a new access code.
	LoadResult load(const std::string &path);
	bool save(const std::string &path) const; // atomic (temp file + rename), 0600 on POSIX
};

} // namespace himitsukao
