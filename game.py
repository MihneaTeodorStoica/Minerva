# game.py — play against MinervaBot in a Pygame board
# ---------------------------------------------------
# Requirements (pip):  pygame, python‑chess, cairosvg
# Linux example:  pip install pygame python-chess cairosvg
#
# Directory layout expected:
#   Minerva/
#       minerva          # compiled C++ engine (see minerva.cpp)
#       game.py          # this file
#       assets/          # 12 SVG piece images from Lichess
#           wK.svg bK.svg wQ.svg bQ.svg ...
# ---------------------------------------------------

import io
import os
import subprocess
import sys
from pathlib import Path

import chess
import pygame  # type: ignore
from cairosvg import svg2png  # type: ignore

# ---------------------------------------------------------------------------
#  Configuration
# ---------------------------------------------------------------------------
ASSETS_DIR = Path(__file__).resolve().parent / "assets"
ENGINE_EXE = Path(__file__).resolve().parent / "minerva"
WINDOW_SIZE = 640
SQ_SIZE = WINDOW_SIZE // 8
FPS = 60

WHITE = (238, 238, 210)
BLACK = (118, 150, 86)
SELECT = (186, 202, 68)

# ---------------------------------------------------------------------------
#  Helper: load & rasterise SVGs to pygame Surfaces
# ---------------------------------------------------------------------------

def load_piece_images(size: int = 80):
    pieces = {}
    for color in ["w", "b"]:
        for piece in ["K", "Q", "R", "B", "N", "P"]:
            fname = f"{color}{piece}.svg"
            svg_path = ASSETS_DIR / fname
            if not svg_path.exists():
                raise FileNotFoundError(svg_path)
            png_bytes = svg2png(url=str(svg_path), output_width=size, output_height=size)
            image = pygame.image.load(io.BytesIO(png_bytes), fname).convert_alpha()
            pieces[color + piece] = image
    return pieces

# ---------------------------------------------------------------------------
#  Draw board & pieces
# ---------------------------------------------------------------------------

def draw_board(screen, board: chess.Board, images, selected_sq=None):
    for rank in range(8):
        for file in range(8):
            square_color = WHITE if (rank + file) % 2 == 0 else BLACK
            rect = pygame.Rect(file * SQ_SIZE, (7 - rank) * SQ_SIZE, SQ_SIZE, SQ_SIZE)
            screen.fill(square_color, rect)
            if selected_sq is not None and selected_sq == chess.square(file, rank):
                pygame.draw.rect(screen, SELECT, rect)

    for square in chess.SQUARES:
        piece = board.piece_at(square)
        if piece:
            img = images[("w" if piece.color == chess.WHITE else "b") + piece.symbol().upper()]
            x = chess.square_file(square) * SQ_SIZE
            y = (7 - chess.square_rank(square)) * SQ_SIZE
            screen.blit(img, (x, y))

# ---------------------------------------------------------------------------
#  Engine interface — synchronous call per move
# ---------------------------------------------------------------------------

def engine_move(board: chess.Board, think_ms: int = 250):
    fen = board.fen()
    try:
        res = subprocess.run([
            str(ENGINE_EXE), fen, str(think_ms)
        ], capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print("Engine error:\n", e.stderr)
        sys.exit(1)

    # Expect last token of stdout to be the UCI move
    tokens = res.stdout.strip().split()
    if not tokens:
        raise RuntimeError("Engine returned no output")
    uci_move = tokens[-1]
    return chess.Move.from_uci(uci_move)

# ---------------------------------------------------------------------------
#  Main game loop
# ---------------------------------------------------------------------------

def main():
    if not ENGINE_EXE.exists():
        print("Minerva engine not found – build minerva first (see minerva.cpp comments).")
        return

    pygame.init()
    screen = pygame.display.set_mode((WINDOW_SIZE, WINDOW_SIZE))
    pygame.display.set_caption("MinervaBot vs Human")
    clock = pygame.time.Clock()

    images = load_piece_images(SQ_SIZE)
    board = chess.Board()

    selected_sq = None
    running = True
    human_color = chess.WHITE  # play White; change to BLACK for opposite

    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.MOUSEBUTTONDOWN and board.turn == human_color and not board.is_game_over():
                x, y = event.pos
                file = x // SQ_SIZE
                rank = 7 - (y // SQ_SIZE)
                clicked_sq = chess.square(file, rank)

                if selected_sq is None:
                    # first click – must pick own piece
                    if board.piece_at(clicked_sq) and board.piece_at(clicked_sq).color == human_color:
                        selected_sq = clicked_sq
                else:
                    # second click → attempt move
                    move = chess.Move(selected_sq, clicked_sq)
                    if move in board.legal_moves:
                        board.push(move)
                        selected_sq = None
                    else:
                        # try promotion to queen by default
                        move = chess.Move(selected_sq, clicked_sq, promotion=chess.QUEEN)
                        if move in board.legal_moves:
                            board.push(move)
                        selected_sq = None

        # Engine move if it is engine's turn
        if not board.is_game_over() and board.turn != human_color:
            move = engine_move(board, think_ms=250)
            if move in board.legal_moves:
                board.push(move)
            else:
                print("Engine played illegal move:", move)
                running = False

        draw_board(screen, board, images, selected_sq)
        pygame.display.flip()
        clock.tick(FPS)

    print("Game over:", board.result())
    pygame.quit()


if __name__ == "__main__":
    main()
