#define _GNU_SOURCE

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

/**
 * @file host.c
 * @desc File-level documentation for host.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// This file stitches the host implementation together from modular C sources.
// Each component lives in src/host_parts/ and is included here so the compiler
// still sees a single translation unit, preserving the existing static helper
// relationships while keeping the source tree organized.

#include "ssh_chatter/translation_helpers.h"

#include "host_parts/host_core.c"
#include "host_parts/host_transport.c"
#include "host_parts/host_security_and_moderation.c"
#include "host_parts/host_eliza_and_storage.c"
#include "host_parts/host_session_output.c"
#include "host_parts/host_session_commands.c"
#include "host_parts/host_bbs_and_games.c"
#include "host_parts/host_json_api.c"
#include "host_parts/host_runtime.c"

#pragma GCC diagnostic pop
