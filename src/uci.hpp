#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include "external/chess/include/chess.hpp"
#include "search.hpp"

class UciDriver {
public:
    UciDriver();

    int loop(); // returns exit code

private:
    void cmd_position(const std::string& line);
    void cmd_go(const std::string& line);
    SearchLimits parseLimits(const std::string& line) const;

    static std::string move_to_uci(const chess::Move& m);
    static chess::Move uci_to_move(const chess::Board& b, const std::string& u);

private:
    chess::Board board_{chess::constants::STARTPOS};
    bool chess960_ = false;

    std::vector<std::unique_ptr<Search>> searchers_;
    std::thread worker_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> searching_{false};
    int threads_ = 1;
};
