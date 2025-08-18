mkdir -p build
g++ -std=c++20 -O3 -march=native -DNDEBUG -pthread -Isrc src/main.cpp src/uci.cpp src/search.cpp src/eval.cpp src/pst.cpp -o build/minerva
