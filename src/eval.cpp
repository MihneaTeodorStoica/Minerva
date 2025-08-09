#include "eval.hpp"
#include "external/chess/include/chess.hpp"
#include <cstdint>

using namespace chess;

namespace {

// Piece values
constexpr int V_P = 100, V_N = 320, V_B = 330, V_R = 500, V_Q = 900;

// Simple PST (midgame) for white side; black mirrored by rank
// indices assume a1=0..h1=7, a2=8.., ... a8=56..63 (this matches chess.hpp)
constexpr int PST_P[64] = {
     0,  0,  0,  5,  5,  0,  0,  0,
    10, 10, 10, 15, 15, 10, 10, 10,
     2,  2,  6, 12, 12,  6,  2,  2,
     1,  1,  5, 11, 11,  5,  1,  1,
     0,  0,  2,  6,  6,  2,  0,  0,
     1, -1,  1,  1,  1,  1, -1,  1,
     1,  3,  3, -2, -2,  3,  3,  1,
     0,  0,  0,  0,  0,  0,  0,  0
};
constexpr int PST_N[64] = {
   -5, -2,  0,  0,  0,  0, -2, -5,
   -1,  2,  3,  3,  3,  3,  2, -1,
    0,  3,  5,  6,  6,  5,  3,  0,
    0,  3,  6,  7,  7,  6,  3,  0,
    0,  3,  6,  7,  7,  6,  3,  0,
    0,  3,  5,  6,  6,  5,  3,  0,
   -1,  2,  3,  3,  3,  3,  2, -1,
   -5, -2,  0,  0,  0,  0, -2, -5
};
constexpr int PST_B[64] = {
    -3, -2, -2, -2, -2, -2, -2, -3,
    -2,  1,  0,  0,  0,  0,  1, -2,
    -2,  2,  3,  2,  2,  3,  2, -2,
    -2,  1,  2,  3,  3,  2,  1, -2,
    -2,  1,  2,  3,  3,  2,  1, -2,
    -2,  2,  3,  2,  2,  3,  2, -2,
    -2,  1,  0,  0,  0,  0,  1, -2,
    -3, -2, -2, -2, -2, -2, -2, -3
};
constexpr int PST_R[64] = {
     0,  0,  0,  3,  3,  0,  0,  0,
     1,  2,  2,  3,  3,  2,  2,  1,
     0,  0,  1,  2,  2,  1,  0,  0,
     0,  0,  1,  2,  2,  1,  0,  0,
     0,  0,  1,  2,  2,  1,  0,  0,
     0,  0,  1,  2,  2,  1,  0,  0,
     1,  2,  2,  3,  3,  2,  2,  1,
     0,  0,  0,  3,  3,  0,  0,  0
};
constexpr int PST_Q[64] = {
    -2, -1, -1,  0,  0, -1, -1, -2,
    -1,  0,  0,  1,  1,  0,  0, -1,
    -1,  0,  2,  2,  2,  2,  0, -1,
     0,  1,  2,  2,  2,  2,  1,  0,
     0,  1,  2,  2,  2,  2,  1,  0,
    -1,  0,  2,  2,  2,  2,  0, -1,
    -1,  0,  0,  1,  1,  0,  0, -1,
    -2, -1, -1,  0,  0, -1, -1, -2
};
constexpr int PST_K_MG[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     30, 40, 20,  0,  0, 20, 40, 30
};
constexpr int PST_K_EG[64] = {
    -50,-30,-30,-30,-30,-30,-30,-50,
    -30,-10,  0,  0,  0,  0,-10,-30,
    -30,  0, 15, 15, 15, 15,  0,-30,
    -30,  0, 15, 25, 25, 15,  0,-30,
    -30,  0, 15, 25, 25, 15,  0,-30,
    -30,  0, 15, 15, 15, 15,  0,-30,
    -30,-10,  0,  0,  0,  0,-10,-30,
    -50,-30,-30,-30,-30,-30,-30,-50
};

inline int mirror_black(int idx) { return idx ^ 56; }

inline int pst(const PieceType& pt, int idx, bool white, bool endgamePhase) {
    int s = 0;
    int i = white ? idx : mirror_black(idx);
    switch ((int)pt.internal()) {
        case (int)PieceType::PAWN:   s = PST_P[i]; break;
        case (int)PieceType::KNIGHT: s = PST_N[i]; break;
        case (int)PieceType::BISHOP: s = PST_B[i]; break;
        case (int)PieceType::ROOK:   s = PST_R[i]; break;
        case (int)PieceType::QUEEN:  s = PST_Q[i]; break;
        case (int)PieceType::KING:   s = endgamePhase ? PST_K_EG[i] : PST_K_MG[i]; break;
        default: break;
    }
    return white ? s : -s;
}

inline int piece_val(PieceType pt) {
    switch ((int)pt.internal()) {
        case (int)PieceType::PAWN: return V_P;
        case (int)PieceType::KNIGHT: return V_N;
        case (int)PieceType::BISHOP: return V_B;
        case (int)PieceType::ROOK: return V_R;
        case (int)PieceType::QUEEN: return V_Q;
        default: return 0;
    }
}

} // namespace

namespace eval {

int evaluate(const Board& b) {
    // Material + PST + bishop pair + simple pawn structure; tapered by game phase.
    int material = 0;
    int mg = 0, eg = 0;

    int phase = 0; // 0..24
    // Phase weights: N,B:1, R:2, Q:4 (common approach)
    auto count_phase = [&](PieceType pt, int per) {
        int c = b.pieces(pt, Color::WHITE).count() + b.pieces(pt, Color::BLACK).count();
        phase += per * c;
    };
    count_phase(PieceType::KNIGHT, 1);
    count_phase(PieceType::BISHOP, 1);
    count_phase(PieceType::ROOK,   2);
    count_phase(PieceType::QUEEN,  4);
    if (phase > 24) phase = 24;

    // Tally material and PST
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b.at(Square(sq));
        if (p == Piece::NONE) continue;
        bool white = (p.color() == Color::WHITE);
        int sign = white ? 1 : -1;

        PieceType pt = p.type();
        material += sign * piece_val(pt);
        mg += pst(pt, sq, white, /*eg*/false);
        eg += pst(pt, sq, white, /*eg*/true);
    }

    // Bishop pair
    if (b.pieces(PieceType::BISHOP, Color::WHITE).count() >= 2) mg += 30, eg += 35;
    if (b.pieces(PieceType::BISHOP, Color::BLACK).count() >= 2) mg -= 30, eg -= 35;

    // Pawn structure (very light): doubled & isolated
    auto whiteP = b.pieces(PieceType::PAWN, Color::WHITE).getBits();
    auto blackP = b.pieces(PieceType::PAWN, Color::BLACK).getBits();
    auto fileMask = [](int f)->uint64_t{
        return 0x0101010101010101ULL << f;
    };
    int whiteDoubled=0, blackDoubled=0, whiteIso=0, blackIso=0;
    for (int f=0; f<8; ++f) {
        uint64_t wf = whiteP & fileMask(f);
        uint64_t bf = blackP & fileMask(f);
        if (__builtin_popcountll(wf) > 1) whiteDoubled += __builtin_popcountll(wf)-1;
        if (__builtin_popcountll(bf) > 1) blackDoubled += __builtin_popcountll(bf)-1;

        uint64_t left  = (f>0? fileMask(f-1):0);
        uint64_t right = (f<7? fileMask(f+1):0);
        if (wf && ((whiteP & (left|right))==0)) whiteIso += __builtin_popcountll(wf);
        if (bf && ((blackP & (left|right))==0)) blackIso += __builtin_popcountll(bf);
    }
    mg += -10*whiteDoubled + 10*blackDoubled;
    mg += -8*whiteIso     +  8*blackIso;
    eg += -8*whiteDoubled +  8*blackDoubled;
    eg += -6*whiteIso     +  6*blackIso;

    // Tempo (small)
    int tempo = (b.sideToMove() == Color::WHITE) ? 8 : -8;

    int mgScore = material + mg + tempo;
    int egScore = material + eg + tempo;

    // Tapered scoring
    int score = (mgScore * phase + egScore * (24 - phase)) / 24;

    // Return from side-to-move perspective
    return (b.sideToMove() == Color::WHITE) ? score : -score;
}

} // namespace eval
