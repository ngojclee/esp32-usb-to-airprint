// The settings page (/setup) and the captive portal redirect of the setup
// WiFi. Hooked in front of the other web handlers.
#pragma once

#include <stdbool.h>

#include "airprint.h"

// true once a response was sent
bool setup_web_http(airprint_http_t *h, const char *method, const char *path);
