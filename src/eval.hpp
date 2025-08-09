#pragma once
#include "external/chess/include/chess.hpp"

namespace eval {

// Evaluate from side-to-move perspective (centipawns)
int evaluate(const chess::Board& b);

} // namespace eval
