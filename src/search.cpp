#include "search.hpp"
#include "eval.hpp"
#include "utils.hpp"
#include <algorithm>
#include <iostream>
#include <chrono>

using namespace chess;

Search::Search(size_t ttMB) : tt_(ttMB) {
    history_.clear();
    killers_.clear();
}

void Search::newGame() {
    tt_.new_generation();
    history_.clear();
    killers_.clear();
    eval::clear_cache();
}

bool Search::timeUp() const {
    if (lim_.infinite) return stop_ && stop_->load(std::memory_order_relaxed);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count();
    if (stop_ && stop_->load(std::memory_order_relaxed)) return true;
    return elapsed >= lim_.timeMs;
}

void Search::orderMoves(const Board& b, Movelist& ml, Move ttMove, int ply) {
    auto scoreOf = [&](const Move& m){
        if (m == ttMove) return 30000000;
        if (b.isCapture(m)) return 20000000 + mvv_lva(b, m);
        if (killers_.is_killer(ply, m)) return 15000000;
        return 10000000 + history_.score(m);
    };
    std::sort(ml.begin(), ml.end(), [&](const Move& a, const Move& c){
        return scoreOf(a) > scoreOf(c);
    });
}

int Search::qsearch(Board& b, int alpha, int beta, int ply) {
    if ((nodes_++ & 0x3FF) == 0 && timeUp()) return eval::evaluate(b);

    // If side in check, extend like a normal node
    if (b.inCheck()) {
        Movelist evs; movegen::legalmoves(evs, b);
        if (evs.empty()) {
            // checkmate
            return -::utils::mate_score(ply);
        }
        int best = -::utils::INF;
        for (const auto& m : evs) {
            b.makeMove(m);
            int sc = -qsearch(b, -beta, -alpha, ply + 1);
            b.unmakeMove(m);
            if (sc > best) best = sc;
            if (best > alpha) alpha = best;
            if (alpha >= beta) break;
        }
        return best;
    }

    int stand = eval::evaluate(b);
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;

    Movelist ml; movegen::legalmoves(ml, b);
    // Keep only captures & promotions
    std::vector<Move> caps; caps.reserve(ml.size());
    for (const auto& m : ml) {
        if (b.isCapture(m) || m.typeOf() == Move::PROMOTION) caps.push_back(m);
    }
    if (caps.empty()) return stand;

    // MVV-LVA ordering
    std::sort(caps.begin(), caps.end(), [&](const Move& a, const Move& c){
        return mvv_lva(b, a) > mvv_lva(b, c);
    });

    int best = stand;
    for (const auto& m : caps) {
        // Futility-like pruning for obviously bad captures could be added here.
        b.makeMove(m);
        int sc = -qsearch(b, -beta, -alpha, ply + 1);
        b.unmakeMove(m);

        if (sc > best) best = sc;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

int Search::negamax(Board& b, int depth, int alpha, int beta, int ply) {
    if ((nodes_++ & 0x7FF) == 0 && timeUp()) return eval::evaluate(b);

    const int alphaOrig = alpha;

    // TT probe
    const auto key = b.hash();
    Move ttMove = Move::NO_MOVE;
    if (auto e = tt_.probe(key)) {
        ttMove = Move(e->move);
        if (e->depth >= depth) {
            int ttScore = ::utils::from_tt(e->score, ply);
            if (e->flag == 0 /*EXACT*/) return ttScore;
            else if (e->flag == 1 /*LOWER*/ && ttScore > alpha) alpha = ttScore;
            else if (e->flag == 2 /*UPPER*/ && ttScore < beta)  beta  = ttScore;
            if (alpha >= beta) return ttScore;
        }
    }

    if (depth <= 0) return qsearch(b, alpha, beta, ply);

    Movelist ml; movegen::legalmoves(ml, b);
    if (ml.empty()) {
        if (b.inCheck()) return -::utils::mate_score(ply);
        return 0; // stalemate
    }

    // Simple check extension
    bool inCheck = b.inCheck();
    if (inCheck) depth += 1;

    orderMoves(b, ml, ttMove, ply);

    int bestScore = -::utils::INF;
    Move bestMove = Move::NO_MOVE;
    int movesSearched = 0;

    for (const auto& m : ml) {
        b.makeMove(m);
        // Late-move reduction (super light)
        int subDepth = depth - 1;
        if (subDepth > 0 && movesSearched >= 4 && !b.isCapture(m) && m.typeOf()!=Move::PROMOTION) {
            subDepth -= 1;
        }
        int sc = -negamax(b, subDepth, -beta, -alpha, ply + 1);
        b.unmakeMove(m);

        movesSearched++;

        if (sc > bestScore) {
            bestScore = sc;
            bestMove = m;
        }
        if (sc > alpha) {
            alpha = sc;
            // history / killer updates for quiets
            if (!b.isCapture(m) && m.typeOf()!=Move::PROMOTION) {
                history_.bonus(m, std::min(2000, 100 + depth*depth));
                killers_.push(ply, m);
            }
        }
        if (alpha >= beta) {
            // history bonus on fail-high
            if (!b.isCapture(m) && m.typeOf()!=Move::PROMOTION) {
                history_.bonus(m, std::min(4000, 200 + depth*depth));
                killers_.push(ply, m);
            }
            break;
        }
    }

    // Store TT
    uint8_t flag = 0;
    if      (bestScore <= alphaOrig) flag = 2; // UPPER
    else if (bestScore >= beta)      flag = 1; // LOWER
    else                             flag = 0; // EXACT
    tt_.store(key, bestMove.move(), depth, ::utils::to_tt(bestScore, ply), flag);

    return bestScore;
}

SearchResult Search::go(const Board& root, const SearchLimits& lim) {
    lim_ = lim;
    nodes_ = 0;
    t0_ = Clock::now();

    SearchResult res{};
    Movelist rootMoves; movegen::legalmoves(rootMoves, root);
    if (rootMoves.empty()) {
        res.best = Move::NO_MOVE;
        res.bestScore = 0;
        return res;
    }

    int maxDepth = (lim.depth > 0 ? lim.depth : 64);
    Move best = rootMoves.front();
    int  bestScore = -::utils::INF;
    int  prevScore = 0;

    // Iterative deepening
    for (int d = 1; d <= maxDepth; ++d) {
        if (timeUp()) break;

        int alpha = -::utils::INF;
        int beta  = ::utils::INF;
        int score = 0;
        if (d > 1) {
            int window = 25;
            alpha = prevScore - window;
            beta  = prevScore + window;
            Board pos = root;
            score = negamax(pos, d, alpha, beta, 0);
            if (!timeUp() && (score <= alpha || score >= beta)) {
                alpha = -::utils::INF;
                beta  = ::utils::INF;
                pos = root;
                score = negamax(pos, d, alpha, beta, 0);
            }
        } else {
            Board pos = root;
            score = negamax(pos, d, alpha, beta, 0);
        }
        if (timeUp()) break;

        // Extract PV from TT
        auto pv = extractPV(root);
        if (!pv.empty()) best = pv.front();

        bestScore = score;
        prevScore = score;

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count();
        // UCI info
        std::cout << "info depth " << d
                  << " score cp " << bestScore
                  << " time " << ms
                  << " nodes " << nodes_
                  << " pv ";
        for (const auto& m : pv) {
            auto f = m.from(), t = m.to();
            std::string s;
            s += char('a'+f.file()); s += char('1'+f.rank());
            s += char('a'+t.file()); s += char('1'+t.rank());
            if (m.typeOf()==Move::PROMOTION) {
                auto pt = m.promotionType();
                char c = 'q';
                if (pt==PieceType::KNIGHT) c = 'n';
                else if (pt==PieceType::BISHOP) c = 'b';
                else if (pt==PieceType::ROOK) c = 'r';
                s += c;
            }
            std::cout << s << ' ';
        }
        std::cout << "\n" << std::flush;
    }

    res.best = best;
    res.bestScore = bestScore;
    return res;
}

std::vector<Move> Search::extractPV(const Board& root) {
    std::vector<Move> pv;
    Board b = root;
    for (int i=0; i<64; ++i) {
        auto e = tt_.probe(b.hash());
        if (!e || e->move == Move::NO_MOVE) break;
        Move m = Move(e->move);
        // validate m is legal in this position
        Movelist ml; movegen::legalmoves(ml, b);
        bool found = false;
        for (auto& lm : ml) if (lm.move()==m.move()) { found = true; break; }
        if (!found) break;
        pv.push_back(m);
        b.makeMove(m);
    }
    return pv;
}
