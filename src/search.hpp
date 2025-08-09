#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <chrono>
#include <optional>
#include "external/chess/include/chess.hpp"
#include "tt.hpp"
#include "move_order.hpp"

struct SearchLimits {
    int timeMs = 1000;
    int depth  = 0;     // 0=auto
    bool infinite = false;
};

struct SearchResult {
    chess::Move best = chess::Move::NO_MOVE;
    int bestScore = 0;
};

class Search {
public:
    explicit Search(size_t ttMB = 64);

    void setStopFlag(std::atomic<bool>* f) { stop_ = f; }
    void newGame();

    SearchResult go(const chess::Board& root, const SearchLimits& lim);

private:
    int  negamax(chess::Board& b, int depth, int alpha, int beta, int ply);
    int  qsearch(chess::Board& b, int alpha, int beta, int ply);
    void orderMoves(const chess::Board& b, chess::Movelist& ml, chess::Move ttMove, int ply);
    bool timeUp() const;

    std::vector<chess::Move> extractPV(const chess::Board& root);

private:
    TranspositionTable tt_;
    History history_;
    Killers killers_;
    std::atomic<bool>* stop_ = nullptr;

    using Clock = std::chrono::steady_clock;
    Clock::time_point t0_;
    SearchLimits lim_;
    uint64_t nodes_ = 0;
};
