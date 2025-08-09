# Minerva

Minerva is a small chess engine with a simple Pygame GUI.

## Features
- UCI compatible engine written in C++20
- PESTO-based evaluation with tapered midgame/endgame
- Optional GUI for human vs engine or engine self play

## Building
```
git submodule update --init --recursive
mkdir -p build
./build.sh
```
This produces `build/minerva`.

## Running the GUI
Install dependencies and launch:
```
pip install -r requirements.txt
python3 game.py
```

## License
Minerva is released under the MIT License. See [LICENSE](LICENSE) for details.
