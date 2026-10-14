// The Tier 0 reference graph, shared by mix-bench and the tests so both
// measure the same thing. "Paths" count input strips and buses alike.
//
//   generator i -> strip i -> program bus      -> outputs 1-2
//                         \-> group bus g -> program
//                          \-> aux bus (even strips only) -> outputs 3-4
//
// Aux gets sends from even strips only, so an edit on an odd strip must
// leave outputs 3-4 bit-identical — the null test relies on that.
#pragma once

#include "engine/graph.h"

namespace pm::bench {

inline constexpr NodeId kGenBase = 1000, kStripBase = 2000;
inline constexpr NodeId kProgram = 3000, kAux = 3001, kGroupBase = 3002;
inline constexpr NodeId kOutProgram = 4000, kOutAux = 4001;

struct RefParams {
    int paths = 64;
    int heavyInserts = 4;           // strips carrying a CpuLoad insert
    float heavyOpsPerSample = 64;   // cost of each
    bool deviceInputs = false;      // Input nodes instead of generators
};

struct RefLayout {
    int strips = 0, groups = 0;
};

RefLayout layoutFor(int paths);
GraphDesc makeReferenceGraph(const RefParams& p);

// Channel-strip and bus chains of the reference load.
ProcessingDesc stripChain();
ProcessingDesc busChain(bool program);

}  // namespace pm::bench
