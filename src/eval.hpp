#pragma once
#include "external/chess/include/chess.hpp"

namespace eval {

// Evaluate from side-to-move perspective (centipawns)
int evaluate(const chess::Board& b);

// Clear any cached evaluation data (called on new game)
void clear_cache();

} // namespace eval
