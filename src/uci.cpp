#include "uci.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <vector>
#include <memory>

using namespace chess;

UciDriver::UciDriver() {
    searchers_.emplace_back(std::make_unique<Search>());
    searchers_[0]->setStopFlag(&stopFlag_);
}

UciDriver::~UciDriver() {
    stopFlag_.store(true);
    if (worker_.joinable()) worker_.join();
}

std::string UciDriver::move_to_uci(const Move& m) {
    std::string s;
    auto f = m.from(), t = m.to();
    s += char('a' + f.file()); s += char('1' + f.rank());
    s += char('a' + t.file()); s += char('1' + t.rank());
    if (m.typeOf()==Move::PROMOTION) {
        auto pt = m.promotionType();
        char c = 'q';
        if (pt==PieceType::KNIGHT) c='n';
        else if (pt==PieceType::BISHOP) c='b';
        else if (pt==PieceType::ROOK) c='r';
        s += c;
    }
    return s;
}

Move UciDriver::uci_to_move(const Board& b, const std::string& u) {
    if (u.size() < 4) return Move::NO_MOVE;
    Square from(u.substr(0,2));
    Square to(u.substr(2,2));
    bool hasPromo = (u.size() >= 5);
    PieceType promo = PieceType::QUEEN;
    if (hasPromo) {
        char pc = u[4];
        if (pc=='n'||pc=='N') promo = PieceType::KNIGHT;
        else if (pc=='b'||pc=='B') promo = PieceType::BISHOP;
        else if (pc=='r'||pc=='R') promo = PieceType::ROOK;
        else promo = PieceType::QUEEN;
    }

    Movelist ml; movegen::legalmoves(ml, b);
    for (const auto& m : ml) {
        if (m.from()==from && m.to()==to) {
            if (hasPromo) {
                if (m.typeOf()==Move::PROMOTION && m.promotionType()==promo) return m;
            } else {
                if (m.typeOf()!=Move::PROMOTION) return m;
            }
        }
    }
    return Move::NO_MOVE;
}

void UciDriver::cmd_position(const std::string& line) {
    // cancel any running search and join worker thread
    stopFlag_.store(true);
    if (worker_.joinable()) worker_.join();
    searching_.store(false);
    auto trim = [](std::string s){
        while(!s.empty() && s.front()==' ') s.erase(s.begin());
        while(!s.empty() && s.back()==' ')  s.pop_back();
        return s;
    };
    std::string rest = trim(line.substr(8));

    if (rest.rfind("startpos", 0) == 0) {
        board_ = Board(constants::STARTPOS, chess960_);
        size_t mvPos = rest.find("moves");
        if (mvPos != std::string::npos) {
            std::istringstream iss(rest.substr(mvPos + 5));
            std::string tok;
            while (iss >> tok) {
                Move m = uci_to_move(board_, tok);
                if (m != Move::NO_MOVE) board_.makeMove(m);
                else break;
            }
        }
        return;
    }

    size_t fenPos = rest.find("fen");
    if (fenPos != std::string::npos) {
        size_t movesPos = rest.find("moves", fenPos);
        std::string fenStr = (movesPos == std::string::npos)
            ? rest.substr(fenPos + 3)
            : rest.substr(fenPos + 3, movesPos - (fenPos + 3));
        fenStr = trim(fenStr);
        if (!fenStr.empty()) board_ = Board(fenStr, chess960_);
        if (movesPos != std::string::npos) {
            std::istringstream iss(rest.substr(movesPos + 5));
            std::string tok;
            while (iss >> tok) {
                Move m = uci_to_move(board_, tok);
                if (m != Move::NO_MOVE) board_.makeMove(m);
                else break;
            }
        }
        return;
    }

    // fallback
    board_ = Board(constants::STARTPOS, chess960_);
}

SearchLimits UciDriver::parseLimits(const std::string& line) const {
    SearchLimits lim{};
    lim.timeMs = 1000; lim.depth = 0; lim.infinite = false;

    std::istringstream ss(line);
    std::string tok; ss >> tok; // "go"
    int wtime=-1,btime=-1,winc=0,binc=0,movestogo=-1,movetime=-1,depth=-1;
    bool infinite=false;

    while (ss >> tok) {
        if (tok=="wtime") ss >> wtime;
        else if (tok=="btime") ss >> btime;
        else if (tok=="winc") ss >> winc;
        else if (tok=="binc") ss >> binc;
        else if (tok=="movestogo") ss >> movestogo;
        else if (tok=="movetime") ss >> movetime;
        else if (tok=="depth") ss >> depth;
        else if (tok=="infinite") infinite = true;
        else if (tok=="ponder"||tok=="nodes"||tok=="mate"||tok=="perft") {
            std::string dummy; ss >> dummy;
        }
    }

    if (infinite) { lim.infinite = true; lim.timeMs = 24*60*60*1000; return lim; }
    if (movetime > 0) { lim.timeMs = movetime; return lim; }
    if (depth > 0) { lim.depth = depth; lim.timeMs = 30*1000; return lim; }

    bool whiteToMove = (board_.sideToMove()==Color::WHITE);
    int myTime = whiteToMove ? wtime : btime;
    int myInc  = whiteToMove ? winc  : binc;

    if (myTime >= 0) {
        int mtg = (movestogo>0 ? movestogo : 30);
        int slice = myTime / std::max(1, mtg);
        int budget = slice + myInc/2;
        lim.timeMs = std::clamp(budget, 20, std::max(50, myTime-10));
    } else {
        lim.timeMs = 500;
    }
    return lim;
}

void UciDriver::cmd_go(const std::string& line) {
    // ensure previous search thread finished
    stopFlag_.store(true);
    if (worker_.joinable()) worker_.join();
    searching_.store(false);
    SearchLimits lim = parseLimits(line);
    stopFlag_.store(false);
    searching_.store(true);

    // Ensure we have enough persistent searchers
    if ((int)searchers_.size() < threads_) {
        while ((int)searchers_.size() < threads_) {
            searchers_.emplace_back(std::make_unique<Search>());
            searchers_.back()->setStopFlag(&stopFlag_);
        }
    } else if ((int)searchers_.size() > threads_) {
        searchers_.resize(threads_);
    }

    // Fire worker
    worker_ = std::thread([this, lim]() {
        std::vector<SearchResult> results(threads_);
        std::vector<std::thread> ths;
        for (int i = 0; i < threads_; ++i) {
            ths.emplace_back([&, i]() {
                results[i] = searchers_[i]->go(board_, lim);
            });
        }
        for (auto& t : ths) t.join();

        SearchResult best = results[0];
        for (int i = 1; i < threads_; ++i) {
            if (results[i].bestScore > best.bestScore)
                best = results[i];
        }

        Movelist legal; movegen::legalmoves(legal, board_);
        if (best.best == Move::NO_MOVE && !legal.empty()) {
            best.best = legal.front();
        }

        std::string bm = move_to_uci(best.best);
        if (bm.empty() && !legal.empty()) bm = move_to_uci(legal.front());
        if (bm.empty()) bm = "0000";
        std::cout << "bestmove " << bm << "\n" << std::flush;
        searching_.store(false);
    });
}

int UciDriver::loop() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "uci") {
            std::cout << "id name Minerva-Classic\n";
            std::cout << "id author Mihnea-Teodor Stoica\n";
            // Optionally: declare "Hash" here and wire to tt_.resize().
            std::cout << "uciok\n" << std::flush;
        } else if (line == "isready") {
            std::cout << "readyok\n" << std::flush;
        } else if (line == "ucinewgame") {
            for (auto& s : searchers_) s->newGame();
        } else if (line.rfind("setoption",0)==0) {
            std::istringstream ss(line);
            std::string token, name, value;
            ss >> token; // setoption
            ss >> token; // name
            while (ss >> token && token != "value") {
                if (!name.empty()) name += " ";
                name += token;
            }
            if (token == "value") ss >> value;
            if (name == "Threads") {
                int t = 1;
                try { t = std::stoi(value); } catch (...) { t = 1; }
                threads_ = std::max(1, t);
                if ((int)searchers_.size() < threads_) {
                    while ((int)searchers_.size() < threads_) {
                        searchers_.emplace_back(std::make_unique<Search>());
                        searchers_.back()->setStopFlag(&stopFlag_);
                    }
                } else if ((int)searchers_.size() > threads_) {
                    searchers_.resize(threads_);
                }
            }
            // For future: parse other setoption like "Hash".
        } else if (line.rfind("position",0)==0) {
            cmd_position(line);
        } else if (line.rfind("go",0)==0) {
            cmd_go(line);
        } else if (line == "stop") {
            stopFlag_.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (line == "quit") {
            stopFlag_.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            break;
        } else if (line=="d" || line=="print") {
            std::cout << "info string FEN " << board_.getFen() << "\n";
        }
    }
    return 0;
}
