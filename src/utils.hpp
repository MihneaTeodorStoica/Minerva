#pragma once
#include <cstdint>
#include <algorithm>

namespace utils {

constexpr int INF  = 30000;
constexpr int MATE = 32000;
constexpr int MATE_IN_MAX = 10000; // distance windowing

inline int mate_score(int plies_to_mate) { return MATE - plies_to_mate; }
inline bool is_mate_score(int s) { return s > MATE - MATE_IN_MAX || s < -MATE + MATE_IN_MAX; }

// Convert score for TT storage/restore to keep mate distance consistent
inline int to_tt(int score, int ply) {
    if (score > MATE - MATE_IN_MAX) return score + ply;
    if (score < -MATE + MATE_IN_MAX) return score - ply;
    return score;
}
inline int from_tt(int score, int ply) {
    if (score > MATE - MATE_IN_MAX) return score - ply;
    if (score < -MATE + MATE_IN_MAX) return score + ply;
    return score;
}

// Integer “round”
inline int iround(double v) {
    return (int)(v + (v >= 0.0 ? 0.5 : -0.5));
}

} // namespace utils
