#include "eval.hpp"
#include "pst.hpp"
#include "external/chess/include/chess.hpp"
#include <cstdint>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

using namespace chess;

namespace {

// Simple cache of evaluation results keyed by board hash
std::unordered_map<uint64_t, int> eval_cache;
std::mutex cache_mutex;

// Piece-square tables and piece values reside in pst.cpp

inline int mirror(int idx) { return idx ^ 56; }

} // namespace

namespace eval {

int evaluate(const Board &b) {
  // Check cache first
  uint64_t key = b.hash();
  {
    std::lock_guard<std::mutex> g(cache_mutex);
    auto it = eval_cache.find(key);
    if (it != eval_cache.end())
      return it->second;
  }

  // Material + PST + bishop pair + simple pawn structure; tapered by game
  // phase.
  int op = 0, mg = 0, eg = 0;

  int phase = 0; // 0..24
  // Phase weights: N,B:1, R:2, Q:4 (common approach)
  auto count_phase = [&](PieceType pt, int per) {
    int c =
        b.pieces(pt, Color::WHITE).count() + b.pieces(pt, Color::BLACK).count();
    phase += per * c;
  };
  count_phase(PieceType::KNIGHT, 1);
  count_phase(PieceType::BISHOP, 1);
  count_phase(PieceType::ROOK, 2);
  count_phase(PieceType::QUEEN, 4);
  if (phase > 24)
    phase = 24;

  // Tally material and PST using PESTO tables
  for (int sq = 0; sq < 64; ++sq) {
    Piece p = b.at(Square(sq));
    if (p == Piece::NONE)
      continue;
    bool white = (p.color() == Color::WHITE);
    int sign = white ? 1 : -1;

    int pt = static_cast<int>(p.type().internal());
    int idx = white ? sq : mirror(sq);
    op += sign * (pst::OP_VALUE[pt] + pst::OP_PST[pt][idx]);
    mg += sign * (pst::MG_VALUE[pt] + pst::MG_PST[pt][idx]);
    eg += sign * (pst::EG_VALUE[pt] + pst::EG_PST[pt][idx]);
  }

  // Bishop pair
  if (b.pieces(PieceType::BISHOP, Color::WHITE).count() >= 2)
    op += 30, mg += 30, eg += 35;
  if (b.pieces(PieceType::BISHOP, Color::BLACK).count() >= 2)
    op -= 30, mg -= 30, eg -= 35;

  // Pawn structure (very light): doubled & isolated
  auto whiteP = b.pieces(PieceType::PAWN, Color::WHITE).getBits();
  auto blackP = b.pieces(PieceType::PAWN, Color::BLACK).getBits();
  auto fileMask = [](int f) -> uint64_t { return 0x0101010101010101ULL << f; };
  int whiteDoubled = 0, blackDoubled = 0, whiteIso = 0, blackIso = 0;
  for (int f = 0; f < 8; ++f) {
    uint64_t wf = whiteP & fileMask(f);
    uint64_t bf = blackP & fileMask(f);
    if (__builtin_popcountll(wf) > 1)
      whiteDoubled += __builtin_popcountll(wf) - 1;
    if (__builtin_popcountll(bf) > 1)
      blackDoubled += __builtin_popcountll(bf) - 1;

    uint64_t left = (f > 0 ? fileMask(f - 1) : 0);
    uint64_t right = (f < 7 ? fileMask(f + 1) : 0);
    if (wf && ((whiteP & (left | right)) == 0))
      whiteIso += __builtin_popcountll(wf);
    if (bf && ((blackP & (left | right)) == 0))
      blackIso += __builtin_popcountll(bf);
  }
  op += -12 * whiteDoubled + 12 * blackDoubled;
  op += -10 * whiteIso + 10 * blackIso;
  mg += -10 * whiteDoubled + 10 * blackDoubled;
  mg += -8 * whiteIso + 8 * blackIso;
  eg += -8 * whiteDoubled + 8 * blackDoubled;
  eg += -6 * whiteIso + 6 * blackIso;

  // Passed pawns (bonus increases toward promotion)
  static const int PASS_OP[8] = {0, 5, 10, 20, 35, 60, 100, 0};
  static const int PASS_MG[8] = {0, 5, 10, 20, 35, 60, 100, 0};
  static const int PASS_EG[8] = {0, 10, 20, 40, 60, 100, 160, 0};

  auto passed_span_white = [&](int sq, int f) {
    uint64_t ahead = (~0ULL) << (sq + 8);
    uint64_t mask = fileMask(f);
    if (f > 0)
      mask |= fileMask(f - 1);
    if (f < 7)
      mask |= fileMask(f + 1);
    return ahead & mask;
  };
  auto passed_span_black = [&](int sq, int f) {
    uint64_t ahead = (1ULL << sq) - 1ULL;
    uint64_t mask = fileMask(f);
    if (f > 0)
      mask |= fileMask(f - 1);
    if (f < 7)
      mask |= fileMask(f + 1);
    return ahead & mask;
  };

  uint64_t wp = whiteP;
  while (wp) {
    int sq = __builtin_ctzll(wp);
    int f = sq & 7;
    int r = sq >> 3;
    if ((blackP & passed_span_white(sq, f)) == 0) {
      op += PASS_OP[r];
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
    if ((whiteP & passed_span_black(sq, f)) == 0) {
      op -= PASS_OP[r];
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
      op -= 20;
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
      op += 20;
      mg += 15;
      eg += 10;
    }
    bKnRim &= bKnRim - 1;
  }

  // Rook placement: bonus for rooks on open/semi-open files
  static const int ROOK_OPEN_OP = 20, ROOK_OPEN_MG = 15, ROOK_OPEN_EG = 10;
  static const int ROOK_SEMI_OP = 12, ROOK_SEMI_MG = 10, ROOK_SEMI_EG = 5;
  uint64_t wr = b.pieces(PieceType::ROOK, Color::WHITE).getBits();
  while (wr) {
    int sq = __builtin_ctzll(wr);
    int f = sq & 7;
    bool wpFile = whiteP & fileMask(f);
    bool bpFile = blackP & fileMask(f);
    if (!wpFile && !bpFile) {
      op += ROOK_OPEN_OP;
      mg += ROOK_OPEN_MG;
      eg += ROOK_OPEN_EG;
    } else if (!wpFile && bpFile) {
      op += ROOK_SEMI_OP;
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
      op -= ROOK_OPEN_OP;
      mg -= ROOK_OPEN_MG;
      eg -= ROOK_OPEN_EG;
    } else if (!bpFile && wpFile) {
      op -= ROOK_SEMI_OP;
      mg -= ROOK_SEMI_MG;
      eg -= ROOK_SEMI_EG;
    }
    br &= br - 1;
  }

  // Connected rooks bonus
  constexpr int CONNECTED_R_OP = 12;
  constexpr int CONNECTED_R_MG = 10;
  constexpr int CONNECTED_R_EG = 10;
  auto connected_rooks = [&](Color c) {
    uint64_t r = b.pieces(PieceType::ROOK, c).getBits();
    if (__builtin_popcountll(r) < 2)
      return;
    int sq1 = __builtin_ctzll(r);
    r &= r - 1;
    int sq2 = __builtin_ctzll(r);
    uint64_t occ = b.occ().getBits();
    if (attacks::rook(Square(sq1), occ).getBits() & (1ULL << sq2)) {
      if (c == Color::WHITE) {
        op += CONNECTED_R_OP;
        mg += CONNECTED_R_MG;
        eg += CONNECTED_R_EG;
      } else {
        op -= CONNECTED_R_OP;
        mg -= CONNECTED_R_MG;
        eg -= CONNECTED_R_EG;
      }
    }
  };
  connected_rooks(Color::WHITE);
  connected_rooks(Color::BLACK);

  // King safety: penalize missing pawn shield
  auto king_shield = [&](Color c) {
    Square ksq = b.kingSq(c);
    int file = ksq.file();
    int rank = ksq.rank();
    uint64_t pawns = b.pieces(PieceType::PAWN, c).getBits();
    int penOp = 0, penMg = 0, penEg = 0;
    int forward = (c == Color::WHITE) ? 1 : -1;
    for (int df = -1; df <= 1; ++df) {
      int f = file + df;
      if (f < 0 || f > 7) {
        penOp += 20;
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
      if (shield1)
        continue;
      if (shield2) {
        penOp += 10;
        penMg += 8;
        penEg += 3;
      } else {
        penOp += 20;
        penMg += 15;
        penEg += 5;
      }
    }
    return std::tuple<int, int, int>{penOp, penMg, penEg};
  };
  auto [wksOp, wksMg, wksEg] = king_shield(Color::WHITE);
  op -= wksOp;
  mg -= wksMg;
  eg -= wksEg;
  auto [bksOp, bksMg, bksEg] = king_shield(Color::BLACK);
  op += bksOp;
  mg += bksMg;
  eg += bksEg;

  // Mobility (very simple: count of attacked squares for minor/major pieces)
  auto wOcc = b.us(Color::WHITE).getBits();
  auto bOcc2 = b.us(Color::BLACK).getBits();
  int mobW = 0, mobB = 0;
  auto occ = b.occ();
  uint64_t wKn = b.pieces(PieceType::KNIGHT, Color::WHITE).getBits();
  while (wKn) {
    int sq = __builtin_ctzll(wKn);
    mobW +=
        __builtin_popcountll((attacks::knight(Square(sq)).getBits()) & ~wOcc);
    wKn &= wKn - 1;
  }
  uint64_t bKn = b.pieces(PieceType::KNIGHT, Color::BLACK).getBits();
  while (bKn) {
    int sq = __builtin_ctzll(bKn);
    mobB +=
        __builtin_popcountll((attacks::knight(Square(sq)).getBits()) & ~bOcc2);
    bKn &= bKn - 1;
  }
  uint64_t wBi = b.pieces(PieceType::BISHOP, Color::WHITE).getBits();
  while (wBi) {
    int sq = __builtin_ctzll(wBi);
    mobW += __builtin_popcountll((attacks::bishop(Square(sq), occ).getBits()) &
                                 ~wOcc);
    wBi &= wBi - 1;
  }
  uint64_t bBi = b.pieces(PieceType::BISHOP, Color::BLACK).getBits();
  while (bBi) {
    int sq = __builtin_ctzll(bBi);
    mobB += __builtin_popcountll((attacks::bishop(Square(sq), occ).getBits()) &
                                 ~bOcc2);
    bBi &= bBi - 1;
  }
  uint64_t wRk = b.pieces(PieceType::ROOK, Color::WHITE).getBits();
  while (wRk) {
    int sq = __builtin_ctzll(wRk);
    mobW += __builtin_popcountll((attacks::rook(Square(sq), occ).getBits()) &
                                 ~wOcc);
    wRk &= wRk - 1;
  }
  uint64_t bRk = b.pieces(PieceType::ROOK, Color::BLACK).getBits();
  while (bRk) {
    int sq = __builtin_ctzll(bRk);
    mobB += __builtin_popcountll((attacks::rook(Square(sq), occ).getBits()) &
                                 ~bOcc2);
    bRk &= bRk - 1;
  }
  uint64_t wQ = b.pieces(PieceType::QUEEN, Color::WHITE).getBits();
  while (wQ) {
    int sq = __builtin_ctzll(wQ);
    mobW += __builtin_popcountll((attacks::queen(Square(sq), occ).getBits()) &
                                 ~wOcc);
    wQ &= wQ - 1;
  }
  uint64_t bQ = b.pieces(PieceType::QUEEN, Color::BLACK).getBits();
  while (bQ) {
    int sq = __builtin_ctzll(bQ);
    mobB += __builtin_popcountll((attacks::queen(Square(sq), occ).getBits()) &
                                 ~bOcc2);
    bQ &= bQ - 1;
  }
  op += 6 * (mobW - mobB);
  mg += 4 * (mobW - mobB);
  eg += 2 * (mobW - mobB);

  // Center control: bonus for attacking the four central squares
  const uint64_t CENTER =
      (1ULL << 27) | (1ULL << 28) | (1ULL << 35) | (1ULL << 36);
  auto count_center = [&](Color c) {
    int total = 0;
    uint64_t pieces;
    pieces = b.pieces(PieceType::PAWN, c).getBits();
    while (pieces) {
      int sq = __builtin_ctzll(pieces);
      total +=
          __builtin_popcountll(attacks::pawn(c, Square(sq)).getBits() & CENTER);
      pieces &= pieces - 1;
    }
    pieces = b.pieces(PieceType::KNIGHT, c).getBits();
    while (pieces) {
      int sq = __builtin_ctzll(pieces);
      total +=
          __builtin_popcountll(attacks::knight(Square(sq)).getBits() & CENTER);
      pieces &= pieces - 1;
    }
    pieces = b.pieces(PieceType::BISHOP, c).getBits();
    while (pieces) {
      int sq = __builtin_ctzll(pieces);
      total += __builtin_popcountll(attacks::bishop(Square(sq), occ).getBits() &
                                    CENTER);
      pieces &= pieces - 1;
    }
    pieces = b.pieces(PieceType::ROOK, c).getBits();
    while (pieces) {
      int sq = __builtin_ctzll(pieces);
      total += __builtin_popcountll(attacks::rook(Square(sq), occ).getBits() &
                                    CENTER);
      pieces &= pieces - 1;
    }
    pieces = b.pieces(PieceType::QUEEN, c).getBits();
    while (pieces) {
      int sq = __builtin_ctzll(pieces);
      total += __builtin_popcountll(attacks::queen(Square(sq), occ).getBits() &
                                    CENTER);
      pieces &= pieces - 1;
    }
    pieces = b.pieces(PieceType::KING, c).getBits();
    while (pieces) {
      int sq = __builtin_ctzll(pieces);
      total +=
          __builtin_popcountll(attacks::king(Square(sq)).getBits() & CENTER);
      pieces &= pieces - 1;
    }
    return total;
  };
  int centerW = count_center(Color::WHITE);
  int centerB = count_center(Color::BLACK);
  op += 6 * (centerW - centerB);
  mg += 4 * (centerW - centerB);
  eg += 2 * (centerW - centerB);

  // Tempo (small)
  int tempo = (b.sideToMove() == Color::WHITE) ? 8 : -8;

  int opScore = op + tempo;
  int mgScore = mg + tempo;
  int egScore = eg + tempo;

  // Three-phase tapered scoring
  int opWeight = phase * phase;
  int mgWeight = 2 * phase * (24 - phase);
  int egWeight = (24 - phase) * (24 - phase);
  int totalWeight = opWeight + mgWeight + egWeight;
  int score = (opScore * opWeight + mgScore * mgWeight + egScore * egWeight) /
              totalWeight;

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
