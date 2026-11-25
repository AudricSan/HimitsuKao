#pragma once

#include "core/himitsukao-service.h"

// Module-wide service: one people database shared by every filter instance (step 5 wraps it in HTTP).
void plugin_service_init();
void plugin_service_shutdown();
// nullptr before init / after shutdown.
himitsukao::HimitsukaoService *plugin_service();
