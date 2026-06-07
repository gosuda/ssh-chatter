/**
 * @file host_aggregate.c
 * @desc File-level documentation for host.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// This file stitches the host implementation together from modular C sources.
// Each component lives in src/host/ and is included here so the compiler
// still sees a single translation unit, preserving the existing static helper
// relationships while keeping the source tree organized.

#include "ssh_chatter/translation_helpers.h"

#include "host/core.c"
#include "host/transport.c"
#include "host/security_moderation.c"
#include "host/eliza_storage.c"
#include "host/session_output.c"
#include "host/session_commands.c"
#include "host/bbs_games.c"
#include "host/json_api.c"
#include "host/runtime.c"
