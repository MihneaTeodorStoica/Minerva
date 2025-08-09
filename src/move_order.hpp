#pragma once
#include "external/chess/include/chess.hpp"
#include <cstdint>

using namespace chess;

struct History {
    // Simple history heuristic: from->to (64x64)
    int16_t table[64][64] {};
    void clear() { for (auto &row : table) for (auto &x : row) x = 0; }
    void bonus(const Move& m, int v) {
        auto f = m.from().index(), t = m.to().index();
        int nv = table[f][t] + v;
        table[f][t] = (int16_t)std::max(-30000, std::min(30000, nv));
    }
    int score(const Move& m) const { return table[m.from().index()][m.to().index()]; }
};

struct Killers {
    // two killer moves per ply
    chess::Move m1[256] {}, m2[256] {};
    void clear() { for (int i=0;i<256;++i){ m1[i]=Move::NO_MOVE; m2[i]=Move::NO_MOVE; } }
    void push(int ply, chess::Move m) {
        if (m == m1[ply] || m == m2[ply]) return;
        m2[ply] = m1[ply]; m1[ply] = m;
    }
    bool is_killer(int ply, chess::Move m) const { return m == m1[ply] || m == m2[ply]; }
};

// MVV-LVA like score (bigger is better)
inline int mvv_lva(const chess::Board& b, chess::Move m) {
    static constexpr int V[7] = {100, 320, 330, 500, 900, 20000, 0}; // P N B R Q K NONE
    if (!b.isCapture(m)) return 0;
    auto to = m.to();
    int victimVal = 0;
    if (m.typeOf() == Move::ENPASSANT) {
        auto capSq = to.ep_square();
        auto p = b.at(capSq);
        victimVal = V[(int)p.type()];
    } else {
        auto p = b.at(to);
        victimVal = V[(int)p.type()];
    }
    auto attacker = b.at(m.from());
    int attackerVal = V[(int)attacker.type()];
    return 10000 + victimVal * 16 - attackerVal;
}
