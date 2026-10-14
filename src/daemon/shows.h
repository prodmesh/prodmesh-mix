// Built-in show templates. Sources are deterministic generators until the
// drivers open device inputs.
#pragma once

#include "engine/graph.h"

namespace pm::shows {

// A small livestream: talent mics, remote guests, playback; grouped into
// Talent and Remotes buses, a Program bus with a limiter, and a mix-minus
// bus for remote guests. Program -> outputs 1-2, mix-minus -> outputs 3-4.
GraphDesc livestream();

inline constexpr NodeId kProgram = 3000, kMixMinus = 3001;

}  // namespace pm::shows
