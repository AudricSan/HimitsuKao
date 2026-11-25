#pragma once

#include <filesystem>

namespace himitsukao {

// Files holding secrets or personal data (known people, access code) are private to the user on POSIX (0600).
// On Windows the per-user profile ACLs already apply, so this is a no-op there.
inline void restrict_to_owner(const std::filesystem::path &p)
{
#ifndef _WIN32
	std::error_code ec;
	std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
				     std::filesystem::perm_options::replace, ec);
#else
	(void)p;
#endif
}

} // namespace himitsukao
