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
    // Check cache first
    uint64_t key = b.hash();
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        auto it = eval_cache.find(key);
        if (it != eval_cache.end()) return it->second;
    }

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

    int mgScore = material + mg + tempo;
    int egScore = material + eg + tempo;

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
