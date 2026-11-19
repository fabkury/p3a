// SPDX-License-Identifier: Apache-2.0
// Constants nobody changes: not the user (those are settings), not the build
// (that is Kconfig). One place, so that the firmware and its documentation
// cannot disagree. Add a limit here when two components need the same number.
#pragma once

// The largest artwork file the player loads and a source downloads. Files
// are read whole into PSRAM (32 MB on this board, shared with everything).
#define P3A_MAX_ARTWORK_BYTES (16 * 1024 * 1024)
