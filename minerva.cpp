// MinervaBot — neural‑guided adversarial best‑first search prototype
// -----------------------------------------------------------------
// Build:
//     g++ -std=c++20 -O3 -march=native -DNDEBUG -Iexternal/chess/include -o minerva minerva.cpp
// Requirements:
//   • chess.hpp   — https://github.com/Disservin/chess-library (header‑only)
//   • A stronger evaluate() can later pipe Stockfish; here we keep a fast material eval.
//
// Usage:
//   ./minerva "fen_string" [time_ms]
//   (If no FEN given the program uses the standard initial position.)
// -----------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "external/chess/include/chess.hpp"

using namespace chess;

//---------------------------------------------------------------------
//  Evaluation helpers
//---------------------------------------------------------------------
//   materialRaw()  – always  (white_material ‑ black_material)
//   evaluate()     – signed from *side‑to‑move* perspective; this makes the
//                    negamax sign‑flip in the search loop correct.
//---------------------------------------------------------------------
static int materialRaw(const Board &b) {
    constexpr int V[6] = {100, 320, 330, 500, 900, 0}; // P N B R Q K
    int score = 0;
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b.at<Piece>(Square(sq));
        if (p == Piece::NONE) continue;
        int v = V[static_cast<int>(p.type())];
        score += (p.color() == Color::WHITE ? v : -v);
    }
    return score;
}

static inline int evaluate(const Board &b) {
    int s = materialRaw(b);
    return (b.sideToMove() == Color::WHITE) ? s : -s;
}

//---------------------------------------------------------------------
//  Minerva Search — adversarial best‑first with worst‑case aggregation
//---------------------------------------------------------------------
namespace minerva {

struct Node {
    Move  root;   // first ply move that leads to this node
    Board board;  // position at this node
    int   score;  // evaluation from the ROOT side’s perspective (higher = better)
};

struct MaxHeapCmp {
    bool operator()(const Node &a, const Node &b) const { return a.score < b.score; }
};

Move search(const Board &rootPos, int timeLimitMs = 100) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    std::priority_queue<Node, std::vector<Node>, MaxHeapCmp> pq;
    std::unordered_map<uint16_t, int> worstByRoot; // root‑move -> worst case score

    Movelist firstMoves;
    movegen::legalmoves(firstMoves, rootPos);
    if (firstMoves.empty()) return Move::NO_MOVE;

    // Seed frontier with every legal root move --------------------------------
    for (const auto &m : firstMoves) {
        Board b = rootPos;
        b.makeMove(m);
        int sc = -evaluate(b);                 // negate to convert to ROOT’s POV
        pq.push({m, b, sc});
        worstByRoot[m.move()] = sc;            // initial worst reply for that root
    }

    // Main loop ----------------------------------------------------------------
    while (!pq.empty() &&
           std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < timeLimitMs) {

        Node node = pq.top(); pq.pop();

        Movelist moves;
        movegen::legalmoves(moves, node.board);
        if (moves.empty()) continue;           // reached leaf / mate / stalemate

        for (const auto &mv : moves) {
            Board nb = node.board;
            nb.makeMove(mv);
            int sc = -evaluate(nb);            // flip perspective each ply (negamax)
            pq.push({node.root, nb, sc});

            int &agg = worstByRoot[node.root.move()];
            agg = std::min(agg, sc);           // keep the worst‑case score so far
        }
    }

    // Select the root move with the best worst‑case evaluation -----------------
    Move bestMove = Move::NO_MOVE;
    int  bestScore = std::numeric_limits<int>::min();
    for (const auto &[id, sc] : worstByRoot) {
        if (sc > bestScore) {
            bestScore = sc;
            bestMove  = Move(id);
        }
    }
    return bestMove == Move::NO_MOVE ? firstMoves.front() : bestMove;
}

} // namespace minerva

//---------------------------------------------------------------------
//  Driver / simple benchmark
//---------------------------------------------------------------------
int main(int argc, char **argv) {
    std::string fen = (argc >= 2) ? argv[1] : constants::STARTPOS;
    int timeMs      = (argc >= 3) ? std::stoi(argv[2]) : 500; // default 0.5 s

    Board board(fen);
    auto move = minerva::search(board, timeMs);

    std::cout << "Minerva suggestion after " << timeMs << " ms: "
              << uci::moveToUci(move) << std::endl;
    return 0;
}
