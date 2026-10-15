// Built-in show templates. Sources are deterministic generators until the
// drivers open device inputs.
#pragma once

#include "engine/graph.h"

namespace pm::shows {

// A small livestream: talent mics, remote guests, playback; grouped into
// Talent and Remotes buses, a Program bus with a limiter, and a mix-minus
// bus for remote guests. Program -> outputs 1-2, mix-minus -> outputs 3-4.
GraphDesc livestream();

// Nothing but a Program bus (with a limiter) on outputs 1-2: add channels
// as needed.
GraphDesc blank();

// One strip per device input ("In 1", ...), spread across a Program bus
// (with a limiter) on outputs 1-2.
GraphDesc inputs(int count);

// Brings any show (template or saved session) to the console model:
// one mono Input node per device input channel ("In 1", ...; they always
// exist, so every input is always recorded, used or not), and input trim
// on the source -> channel edges (sessions before 0.0.2 kept it on the
// source node's fader).
void normalize(GraphDesc& g, int deviceInputs);

inline constexpr NodeId kProgram = 3000, kMixMinus = 3001;

}  // namespace pm::shows
