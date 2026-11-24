#pragma once

#include "core/application_identity.h"

// The shell's library identity: ~/.config/noctalia-kwe, noctalia-kwe.log, NOCTALIA_* environment variables.
inline ApplicationIdentity shellApplicationIdentity() { return {.name = "noctalia-kwe", .envPrefix = "NOCTALIA"}; }
