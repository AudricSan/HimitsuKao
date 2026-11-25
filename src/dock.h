#pragma once

// The "HimitsuKao" dock: obs-browser's panel API showing the management page inside OBS.
// Compiled for real only when HIMITSUKAO_DOCK is defined (CMake option HIMITSUKAO_ENABLE_DOCK, needs Qt and the
// frontend API); otherwise every function is a no-op.

constexpr bool kDockAvailable =
#ifdef HIMITSUKAO_DOCK
	true;
#else
	false;
#endif

void dock_init();     // registers for the frontend events; the dock itself is created when OBS finished loading
void dock_shutdown(); // removes the dock
// Re-points the dock at the server's current address and access code. Safe to call from any thread.
void dock_refresh_url();
