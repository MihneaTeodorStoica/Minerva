#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include "external/chess/include/chess.hpp"
#include "utils.hpp"

struct TTEntry {
    uint64_t key = 0;
    uint16_t move = 0;        // packed
    int16_t  score = 0;       // from side-to-move
    int8_t   depth = -1;
    uint8_t  flag = 0;        // 0=EXACT,1=LOWER,2=UPPER
    uint8_t  gen  = 0;        // not used much
};

class TranspositionTable {
public:
    explicit TranspositionTable(size_t mb = 64) { resize(mb); }

    void resize(size_t mb) {
        size_t bytes = mb * 1024 * 1024;
        size_t n = std::max<size_t>(1, bytes / sizeof(TTEntry));
        table_.assign(n, {});
        mask_ = n - 1;
        gen_ = 0;
    }

    void new_generation() { gen_++; }

    const TTEntry* probe(uint64_t key) const {
        const TTEntry& e = table_[index(key)];
        if (e.key == key) return &e;
        return nullptr;
    }

    void store(uint64_t key, uint16_t move, int depth, int score, uint8_t flag) {
        TTEntry& e = table_[index(key)];
        if (e.key != key || depth >= e.depth) {
            e.key = key;
            e.move = move;
            e.depth = (int8_t)std::min(depth, 127);
            e.score = (int16_t)std::max(-::utils::MATE, std::min(::utils::MATE, score));
            e.flag = flag;
            e.gen = gen_;
        }
    }

private:
    size_t index(uint64_t key) const { return (size_t)key & mask_; }

    std::vector<TTEntry> table_;
    size_t mask_ = 0;
    uint8_t gen_ = 0;
};
