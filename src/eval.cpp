#include "eval.hpp"
#include "external/chess/include/chess.hpp"
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <utility>

using namespace chess;

namespace {

// Simple cache of evaluation results keyed by board hash
std::unordered_map<uint64_t, int> eval_cache;
std::mutex cache_mutex;

// Piece-square tables from PESTO (Rofchade) with midgame and endgame values
// Piece values (midgame and endgame)
constexpr int MG_VALUE[6] = {82, 337, 365, 477, 1025, 0};
constexpr int EG_VALUE[6] = {94, 281, 297, 512, 936, 0};

// Pawn
constexpr int MG_PAWN_TABLE[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
     98, 134,  61,  95,  68, 126,  34, -11,
     -6,   7,  26,  31,  65,  56,  25, -20,
    -14,  13,   6,  21,  23,  12,  17, -23,
    -27,  -2,  -5,  12,  17,   6,  10, -25,
    -26,  -4,  -4, -10,   3,   3,  33, -12,
    -35,  -1, -20, -23, -15,  24,  38, -22,
      0,   0,   0,   0,   0,   0,   0,   0,
};
constexpr int EG_PAWN_TABLE[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
    178, 173, 158, 134, 147, 132, 165, 187,
     94, 100,  85,  67,  56,  53,  82,  84,
     32,  24,  13,   5,  -2,   4,  17,  17,
     13,   9,  -3,  -7,  -7,  -8,   3,  -1,
      4,   7,  -6,   1,   0,  -5,  -1,  -8,
     13,   8,   8,  10,  13,   0,   2,  -7,
      0,   0,   0,   0,   0,   0,   0,   0,
};

// Knight
constexpr int MG_KNIGHT_TABLE[64] = {
    -167, -89, -34, -49,  61, -97, -15, -107,
     -73, -41,  72,  36,  23,  62,   7,  -17,
     -47,  60,  37,  65,  84, 129,  73,   44,
      -9,  17,  19,  53,  37,  69,  18,   22,
     -13,   4,  16,  13,  28,  19,  21,   -8,
     -23,  -9,  12,  10,  19,  17,  25,  -16,
     -29, -53, -12,  -3,  -1,  18, -14,  -19,
    -105, -21, -58, -33, -17, -28, -19,  -23,
};
constexpr int EG_KNIGHT_TABLE[64] = {
    -58, -38, -13, -28, -31, -27, -63, -99,
    -25,  -8, -25,  -2,  -9, -25, -24, -52,
    -24, -20,  10,   9,  -1,  -9, -19, -41,
    -17,   3,  22,  22,  22,  11,   8, -18,
    -18,  -6,  16,  25,  16,  17,   4, -18,
    -23,  -3,  -1,  15,  10,  -3, -20, -22,
    -42, -20, -10,  -5,  -2, -20, -23, -44,
    -29, -51, -23, -15, -22, -18, -50, -64,
};

// Bishop
constexpr int MG_BISHOP_TABLE[64] = {
    -29,   4, -82, -37, -25, -42,   7,  -8,
    -26,  16, -18, -13,  30,  59,  18, -47,
    -16,  37,  43,  40,  35,  50,  37,  -2,
     -4,   5,  19,  50,  37,  37,   7,  -2,
     -6,  13,  13,  26,  34,  12,  10,   4,
      0,  15,  15,  15,  14,  27,  18,  10,
      4,  15,  16,   0,   7,  21,  33,   1,
    -33,  -3, -14, -21, -13, -12, -39, -21,
};
constexpr int EG_BISHOP_TABLE[64] = {
    -14, -21, -11,  -8,  -7,  -9, -17, -24,
     -8,  -4,   7, -12,  -3, -13,  -4, -14,
      2,  -8,   0,  -1,  -2,   6,   0,   4,
     -3,   9,  12,   9,  14,  10,   3,   2,
     -6,   3,  13,  19,   7,  10,  -3,  -9,
    -12,  -3,   8,  10,  13,   3,  -7, -15,
    -14, -18,  -7,  -1,   4,  -9, -15, -27,
    -23,  -9, -23,  -5,  -9, -16,  -5, -17,
};

// Rook
constexpr int MG_ROOK_TABLE[64] = {
     32,  42,  32,  51,  63,   9,  31,  43,
     27,  32,  58,  62,  80,  67,  26,  44,
     -5,  19,  26,  36,  17,  45,  61,  16,
    -24, -11,   7,  26,  24,  35,  -8, -20,
    -36, -26, -12,  -1,   9,  -7,   6, -23,
    -45, -25, -16, -17,   3,   0,  -5, -33,
    -44, -16, -20,  -9,  -1,  11,  -6, -71,
    -19, -13,   1,  17,  16,   7, -37, -26,
};
constexpr int EG_ROOK_TABLE[64] = {
     13,  10,  18,  15,  12,  12,   8,   5,
     11,  13,  13,  11,  -3,   3,   8,   3,
      7,   7,   7,   5,   4,  -3,  -5,  -3,
      4,   3,  13,   1,   2,   1,  -1,   2,
      3,   5,   8,   4,  -5,  -6,  -8, -11,
     -4,   0,  -5,  -1,  -7, -12,  -8, -16,
     -6,  -6,   0,   2,  -9,  -9, -11,  -3,
     -9,   2,   3,  -1,  -5, -13,   4, -20,
};

// Queen
constexpr int MG_QUEEN_TABLE[64] = {
    -28,   0,  29,  12,  59,  44,  43,  45,
    -24, -39,  -5,   1, -16,  57,  28,  54,
    -13, -17,   7,   8,  29,  56,  47,  57,
    -27, -27, -16, -16,  -1,  17,  -2,   1,
     -9, -26,  -9, -10,  -2,  -4,   3,  -3,
    -14,   2, -11,  -2,  -5,   2,  14,   5,
    -35,  -8,  11,   2,   8,  15,  -3,   1,
     -1, -18,  -9,  10, -15, -25, -31, -50,
};
constexpr int EG_QUEEN_TABLE[64] = {
     -9,  22,  22,  27,  27,  19,  10,  20,
    -17,  20,  32,  41,  58,  25,  30,   0,
    -20,   6,   9,  49,  47,  35,  19,   9,
      3,  22,  24,  45,  57,  40,  57,  36,
    -18,  28,  19,  47,  31,  34,  39,  23,
    -16, -27,  15,   6,   9,  17,  10,   5,
    -22, -23, -30, -16, -16, -23, -36, -32,
    -33, -28, -22, -43,  -5, -32, -20, -41,
};

// King
constexpr int MG_KING_TABLE[64] = {
    -65,  23,  16, -15, -56, -34,   2,  13,
     29,  -1, -20,  -7,  -8,  -4, -38, -29,
     -9,  24,   2, -16, -20,   6,  22, -22,
    -17, -20, -12, -27, -30, -25, -14, -36,
    -49,  -1, -27, -39, -46, -44, -33, -51,
    -14, -14, -22, -46, -44, -30, -15, -27,
      1,   7,  -8, -64, -43, -16,   9,   8,
    -15,  36,  12, -54,   8, -28,  24,  14,
};
constexpr int EG_KING_TABLE[64] = {
    -74, -35, -18, -18, -11,  15,   4, -17,
    -12,  17,  14,  17,  17,  38,  23,  11,
     10,  17,  23,  15,  20,  45,  44,  13,
     -8,  22,  24,  27,  26,  33,  26,   3,
    -18,  -4,  21,  24,  27,  23,   9, -11,
    -19,  -3,  11,  21,  23,  16,   7,  -9,
    -27, -11,   4,  13,  14,   4,  -5, -17,
    -53, -34, -21, -11, -28, -14, -24, -43
};

inline int mirror(int idx) { return idx ^ 56; }

constexpr const int* MG_PST[6] = {
    MG_PAWN_TABLE,
    MG_KNIGHT_TABLE,
    MG_BISHOP_TABLE,
    MG_ROOK_TABLE,
    MG_QUEEN_TABLE,
    MG_KING_TABLE
};

constexpr const int* EG_PST[6] = {
    EG_PAWN_TABLE,
    EG_KNIGHT_TABLE,
    EG_BISHOP_TABLE,
    EG_ROOK_TABLE,
    EG_QUEEN_TABLE,
    EG_KING_TABLE
};

} // namespace

namespace eval {

int evaluate(const Board& b) {
    // Check cache first
    uint64_t key = b.hash();
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        auto it = eval_cache.find(key);
        if (it != eval_cache.end()) return it->second;
    }

    // Material + PST + bishop pair + simple pawn structure; tapered by game phase.
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

    // Tally material and PST using PESTO tables
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b.at(Square(sq));
        if (p == Piece::NONE) continue;
        bool white = (p.color() == Color::WHITE);
        int sign = white ? 1 : -1;

        int pt = static_cast<int>(p.type().internal());
        int idx = white ? sq : mirror(sq);
        mg += sign * (MG_VALUE[pt] + MG_PST[pt][idx]);
        eg += sign * (EG_VALUE[pt] + EG_PST[pt][idx]);
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

    // Passed pawns (bonus increases toward promotion)
    static const int PASS_MG[8] = {0, 5, 10, 20, 35, 60, 100, 0};
    static const int PASS_EG[8] = {0,10, 20, 40, 60,100,160, 0};

    auto passed_span_white = [&](int sq, int f){
        uint64_t ahead = (~0ULL) << (sq + 8);
        uint64_t mask = fileMask(f);
        if (f>0) mask |= fileMask(f-1);
        if (f<7) mask |= fileMask(f+1);
        return ahead & mask;
    };
    auto passed_span_black = [&](int sq, int f){
        uint64_t ahead = (1ULL << sq) - 1ULL;
        uint64_t mask = fileMask(f);
        if (f>0) mask |= fileMask(f-1);
        if (f<7) mask |= fileMask(f+1);
        return ahead & mask;
    };

    uint64_t wp = whiteP;
    while (wp) {
        int sq = __builtin_ctzll(wp);
        int f = sq & 7;
        int r = sq >> 3;
        if ((blackP & passed_span_white(sq,f)) == 0) {
            mg += PASS_MG[r];
            eg += PASS_EG[r];
        }
        wp &= wp - 1;
    }
    uint64_t bp = blackP;
    while (bp) {
        int sq = __builtin_ctzll(bp);
        int f = sq & 7;
        int r = 7 - (sq >> 3);
        if ((whiteP & passed_span_black(sq,f)) == 0) {
            mg -= PASS_MG[r];
            eg -= PASS_EG[r];
        }
        bp &= bp - 1;
    }

    // Knight on rim penalty ("A knight on the rim is dim")
    uint64_t wKnRim = b.pieces(PieceType::KNIGHT, Color::WHITE).getBits();
    while (wKnRim) {
        int sq = __builtin_ctzll(wKnRim);
        int f = sq & 7;
        int r = sq >> 3;
        if (f == 0 || f == 7 || r == 0 || r == 7) {
            mg -= 15;
            eg -= 10;
        }
        wKnRim &= wKnRim - 1;
    }
    uint64_t bKnRim = b.pieces(PieceType::KNIGHT, Color::BLACK).getBits();
    while (bKnRim) {
        int sq = __builtin_ctzll(bKnRim);
        int f = sq & 7;
        int r = sq >> 3;
        if (f == 0 || f == 7 || r == 0 || r == 7) {
            mg += 15;
            eg += 10;
        }
        bKnRim &= bKnRim - 1;
    }

    // Rook placement: bonus for rooks on open/semi-open files
    static const int ROOK_OPEN_MG = 15, ROOK_OPEN_EG = 10;
    static const int ROOK_SEMI_MG = 10, ROOK_SEMI_EG = 5;
    uint64_t wr = b.pieces(PieceType::ROOK, Color::WHITE).getBits();
    while (wr) {
        int sq = __builtin_ctzll(wr);
        int f = sq & 7;
        bool wpFile = whiteP & fileMask(f);
        bool bpFile = blackP & fileMask(f);
        if (!wpFile && !bpFile) {
            mg += ROOK_OPEN_MG;
            eg += ROOK_OPEN_EG;
        } else if (!wpFile && bpFile) {
            mg += ROOK_SEMI_MG;
            eg += ROOK_SEMI_EG;
        }
        wr &= wr - 1;
    }
    uint64_t br = b.pieces(PieceType::ROOK, Color::BLACK).getBits();
    while (br) {
        int sq = __builtin_ctzll(br);
        int f = sq & 7;
        bool bpFile = blackP & fileMask(f);
        bool wpFile = whiteP & fileMask(f);
        if (!bpFile && !wpFile) {
            mg -= ROOK_OPEN_MG;
            eg -= ROOK_OPEN_EG;
        } else if (!bpFile && wpFile) {
            mg -= ROOK_SEMI_MG;
            eg -= ROOK_SEMI_EG;
        }
        br &= br - 1;
    }

    // Connected rooks bonus
    constexpr int CONNECTED_R_MG = 10;
    constexpr int CONNECTED_R_EG = 10;
    auto connected_rooks = [&](Color c){
        uint64_t r = b.pieces(PieceType::ROOK, c).getBits();
        if (__builtin_popcountll(r) < 2) return;
        int sq1 = __builtin_ctzll(r);
        r &= r - 1;
        int sq2 = __builtin_ctzll(r);
        uint64_t occ = b.occ().getBits();
        if (attacks::rook(Square(sq1), occ).getBits() & (1ULL << sq2)) {
            if (c == Color::WHITE) {
                mg += CONNECTED_R_MG;
                eg += CONNECTED_R_EG;
            } else {
                mg -= CONNECTED_R_MG;
                eg -= CONNECTED_R_EG;
            }
        }
    };
    connected_rooks(Color::WHITE);
    connected_rooks(Color::BLACK);

    // King safety: penalize missing pawn shield
    auto king_shield = [&](Color c){
        Square ksq = b.kingSq(c);
        int file = ksq.file();
        int rank = ksq.rank();
        uint64_t pawns = b.pieces(PieceType::PAWN, c).getBits();
        int penMg = 0, penEg = 0;
        int forward = (c == Color::WHITE) ? 1 : -1;
        for (int df = -1; df <= 1; ++df) {
            int f = file + df;
            if (f < 0 || f > 7) {
                penMg += 15;
                penEg += 5;
                continue;
            }
            int r1 = rank + forward;
            int r2 = rank + 2 * forward;
            bool shield1 = false, shield2 = false;
            if (r1 >= 0 && r1 < 8) {
                int sq1 = r1 * 8 + f;
                shield1 = pawns & (1ULL << sq1);
            }
            if (!shield1 && r2 >= 0 && r2 < 8) {
                int sq2 = r2 * 8 + f;
                shield2 = pawns & (1ULL << sq2);
            }
            if (shield1) continue;
            if (shield2) {
                penMg += 8;
                penEg += 3;
            } else {
                penMg += 15;
                penEg += 5;
            }
        }
        return std::pair<int,int>{penMg, penEg};
    };
    auto [wksMg, wksEg] = king_shield(Color::WHITE);
    mg -= wksMg; eg -= wksEg;
    auto [bksMg, bksEg] = king_shield(Color::BLACK);
    mg += bksMg; eg += bksEg;

    // Mobility (very simple: count of attacked squares for minor/major pieces)
    auto wOcc = b.us(Color::WHITE).getBits();
    auto bOcc2 = b.us(Color::BLACK).getBits();
    int mobW = 0, mobB = 0;
    auto occ = b.occ();
    uint64_t wKn = b.pieces(PieceType::KNIGHT, Color::WHITE).getBits();
    while (wKn) { int sq = __builtin_ctzll(wKn); mobW += __builtin_popcountll((attacks::knight(Square(sq)).getBits()) & ~wOcc); wKn &= wKn-1; }
    uint64_t bKn = b.pieces(PieceType::KNIGHT, Color::BLACK).getBits();
    while (bKn) { int sq = __builtin_ctzll(bKn); mobB += __builtin_popcountll((attacks::knight(Square(sq)).getBits()) & ~bOcc2); bKn &= bKn-1; }
    uint64_t wBi = b.pieces(PieceType::BISHOP, Color::WHITE).getBits();
    while (wBi) { int sq = __builtin_ctzll(wBi); mobW += __builtin_popcountll((attacks::bishop(Square(sq), occ).getBits()) & ~wOcc); wBi &= wBi-1; }
    uint64_t bBi = b.pieces(PieceType::BISHOP, Color::BLACK).getBits();
    while (bBi) { int sq = __builtin_ctzll(bBi); mobB += __builtin_popcountll((attacks::bishop(Square(sq), occ).getBits()) & ~bOcc2); bBi &= bBi-1; }
    uint64_t wRk = b.pieces(PieceType::ROOK, Color::WHITE).getBits();
    while (wRk) { int sq = __builtin_ctzll(wRk); mobW += __builtin_popcountll((attacks::rook(Square(sq), occ).getBits()) & ~wOcc); wRk &= wRk-1; }
    uint64_t bRk = b.pieces(PieceType::ROOK, Color::BLACK).getBits();
    while (bRk) { int sq = __builtin_ctzll(bRk); mobB += __builtin_popcountll((attacks::rook(Square(sq), occ).getBits()) & ~bOcc2); bRk &= bRk-1; }
    uint64_t wQ = b.pieces(PieceType::QUEEN, Color::WHITE).getBits();
    while (wQ) { int sq = __builtin_ctzll(wQ); mobW += __builtin_popcountll((attacks::queen(Square(sq), occ).getBits()) & ~wOcc); wQ &= wQ-1; }
    uint64_t bQ = b.pieces(PieceType::QUEEN, Color::BLACK).getBits();
    while (bQ) { int sq = __builtin_ctzll(bQ); mobB += __builtin_popcountll((attacks::queen(Square(sq), occ).getBits()) & ~bOcc2); bQ &= bQ-1; }
    mg += 4 * (mobW - mobB);
    eg += 2 * (mobW - mobB);

    // Tempo (small)
    int tempo = (b.sideToMove() == Color::WHITE) ? 8 : -8;

    int mgScore = mg + tempo;
    int egScore = eg + tempo;

    // Tapered scoring
    int score = (mgScore * phase + egScore * (24 - phase)) / 24;

    // Cache and return from side-to-move perspective
    int finalScore = (b.sideToMove() == Color::WHITE) ? score : -score;
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        eval_cache[key] = finalScore;
    }
    return finalScore;
}

void clear_cache() {
    std::lock_guard<std::mutex> g(cache_mutex);
    eval_cache.clear();
}

} // namespace eval
