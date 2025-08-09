#!/usr/bin/env python3
# game.py — Play against Minerva (UCI) in a Pygame board
# ------------------------------------------------------
# Requirements:  pygame, python-chess, cairosvg
#   pip install pygame python-chess cairosvg
#
# Expected layout:
#   Minerva/
#     build/minerva          (compiled C++ UCI engine)
#     assets/*.svg           (12 piece SVGs from Lichess)
#     game.py                (this file)
#
# Controls:
#   - Click a piece, then click destination.
#   - Promotion: a small overlay appears — click the desired piece.
#   - Press 'c' to swap sides (restart new game).
#   - Press 'n' to start a new game (same side).
#   - Press ESC / close window to quit.
# ------------------------------------------------------

import io
import os
import sys
import time
import queue
import threading
import subprocess
from pathlib import Path

import chess
import pygame  # type: ignore
from cairosvg import svg2png  # type: ignore

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parent
ASSETS_DIR = ROOT / "assets"
# Try build/minerva first, then fallback to ./minerva
ENGINE_EXE = (ROOT / "build" / "minerva")
if not ENGINE_EXE.exists():
    fallback = ROOT / "minerva"
    ENGINE_EXE = fallback if fallback.exists() else ENGINE_EXE

WINDOW_SIZE = 640
SQ_SIZE = WINDOW_SIZE // 8
FPS = 60

THINK_MS = 300        # engine movetime in milliseconds (tweak as you like)
HUMAN_PLAYS_WHITE = True  # toggle in-app with 'c'

COL_LIGHT = (238, 238, 210)
COL_DARK = (118, 150, 86)
COL_SELECT = (186, 202, 68)
COL_LAST = (246, 246, 105)
COL_HINT = (33, 33, 33, 120)

# ---------------------------------------------------------------------------
# UCI Engine wrapper (persistent)
# ---------------------------------------------------------------------------
class UCIEngine:
    def __init__(self, exe_path: Path):
        self.exe_path = str(exe_path)
        self.proc: subprocess.Popen | None = None
        self.q = queue.Queue()  # type: queue.Queue[str]
        self.reader_thread: threading.Thread | None = None
        self.lock = threading.Lock()
        self.running = False

    def start(self) -> None:
        if not os.path.exists(self.exe_path):
            raise FileNotFoundError(f"Engine not found: {self.exe_path}")
        self.proc = subprocess.Popen(
            [self.exe_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.running = True
        self.reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self.reader_thread.start()

        self._send("uci")
        self._wait_for("uciok", timeout=5.0)
        self._send("isready")
        self._wait_for("readyok", timeout=5.0)
        self._send("ucinewgame")

    def stop(self) -> None:
        with self.lock:
            if not self.proc:
                return
            try:
                self._send("quit")
            except Exception:
                pass
            try:
                self.proc.terminate()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=1.0)
            except Exception:
                pass
            self.running = False
            self.proc = None

    def _read_loop(self):
        assert self.proc and self.proc.stdout
        for line in self.proc.stdout:
            self.q.put(line.rstrip("\n"))
        self.running = False

    def _send(self, cmd: str) -> None:
        if not self.proc or not self.proc.stdin:
            raise RuntimeError("Engine not running")
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def _wait_for(self, token: str, timeout: float | None) -> None:
        t0 = time.time()
        while True:
            try:
                line = self.q.get(timeout=0.05)
            except queue.Empty:
                if timeout is not None and (time.time() - t0) > timeout:
                    raise TimeoutError(f"Timed out waiting for '{token}'")
                continue
            if token in line:
                return

    def new_game(self) -> None:
        self._send("ucinewgame")
        self._send("isready")
        self._wait_for("readyok", timeout=5.0)

    def set_position(self, board: chess.Board) -> None:
        # Use FEN position to fully describe state (castling, ep, etc.)
        fen = board.board_fen()
        active = "w" if board.turn == chess.WHITE else "b"
        castling = board.castling_xfen() or "-"
        ep = board.ep_square
        ep_str = chess.square_name(ep) if ep is not None else "-"
        halfmove = board.halfmove_clock
        fullmove = board.fullmove_number
        fen_full = f"{fen} {active} {castling} {ep_str} {halfmove} {fullmove}"
        self._send(f"position fen {fen_full}")

    def go(self, movetime_ms: int = THINK_MS) -> tuple[str, list[str]]:
        """Returns (bestmove, info_lines)"""
        self._send(f"go movetime {movetime_ms}")
        info = []
        best = None
        t0 = time.time()
        while True:
            try:
                line = self.q.get(timeout=0.05)
            except queue.Empty:
                # safety timeout (shouldn’t happen unless engine dies)
                if (time.time() - t0) > max(5.0, movetime_ms / 1000.0 + 2.0):
                    break
                continue
            if line.startswith("info "):
                info.append(line)
            elif line.startswith("bestmove"):
                parts = line.split()
                if len(parts) >= 2:
                    best = parts[1]
                break
        return best or "0000", info

# ---------------------------------------------------------------------------
# Assets: load & rasterize SVG to pygame surfaces
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
# Drawing helpers
# ---------------------------------------------------------------------------
def draw_board(screen, board: chess.Board, images, selected_sq=None, last_move=None, legal_targets=None):
    # squares
    for rank in range(8):
        for file in range(8):
            rect = pygame.Rect(file * SQ_SIZE, (7 - rank) * SQ_SIZE, SQ_SIZE, SQ_SIZE)
            base = COL_LIGHT if (rank + file) % 2 == 0 else COL_DARK
            screen.fill(base, rect)

    # last move highlight
    if last_move:
        a = last_move.from_square
        b = last_move.to_square
        for sq in (a, b):
            file = chess.square_file(sq)
            rank = chess.square_rank(sq)
            rect = pygame.Rect(file * SQ_SIZE, (7 - rank) * SQ_SIZE, SQ_SIZE, SQ_SIZE)
            pygame.draw.rect(screen, COL_LAST, rect, width=0)

    # selected square
    if selected_sq is not None:
        file = chess.square_file(selected_sq)
        rank = chess.square_rank(selected_sq)
        rect = pygame.Rect(file * SQ_SIZE, (7 - rank) * SQ_SIZE, SQ_SIZE, SQ_SIZE)
        pygame.draw.rect(screen, COL_SELECT, rect, width=0)

    # pieces
    for square in chess.SQUARES:
        piece = board.piece_at(square)
        if piece:
            key = ("w" if piece.color == chess.WHITE else "b") + piece.symbol().upper()
            img = images[key]
            x = chess.square_file(square) * SQ_SIZE
            y = (7 - chess.square_rank(square)) * SQ_SIZE
            screen.blit(img, (x, y))

    # legal move hints (dots)
    if legal_targets:
        surf = pygame.Surface((SQ_SIZE, SQ_SIZE), pygame.SRCALPHA)
        pygame.draw.circle(surf, COL_HINT, (SQ_SIZE // 2, SQ_SIZE // 2), SQ_SIZE // 8)
        for sq in legal_targets:
            file = chess.square_file(sq)
            rank = chess.square_rank(sq)
            screen.blit(surf, (file * SQ_SIZE, (7 - rank) * SQ_SIZE))

def draw_text(screen, text, pos, size=20, color=(20, 20, 20)):
    font = pygame.font.SysFont("arial", size)
    img = font.render(text, True, color)
    screen.blit(img, pos)

# ---------------------------------------------------------------------------
# Promotion overlay
# ---------------------------------------------------------------------------
def choose_promotion(screen, images, color_is_white: bool, at_square: int) -> chess.PieceType:
    """Modal: show 4 piece choices stacked on the promotion file; return selected piece type."""
    file = chess.square_file(at_square)
    # Top-left of column
    x = file * SQ_SIZE
    # Place the column starting from the side the pawn is promoting to
    top_rank = 0 if color_is_white else 4
    start_y = (7 - top_rank) * SQ_SIZE

    order = [chess.QUEEN, chess.ROOK, chess.BISHOP, chess.KNIGHT]
    options = []
    for i, pt in enumerate(order):
        key = ("w" if color_is_white else "b") + chess.Piece(pt, color_is_white).symbol().upper()
        rect = pygame.Rect(x, start_y + i * (SQ_SIZE // 1), SQ_SIZE, SQ_SIZE)
        pygame.draw.rect(screen, (245, 245, 245), rect)
        pygame.draw.rect(screen, (60, 60, 60), rect, width=2)
        screen.blit(images[key], rect.topleft)
        options.append((rect, pt))
    pygame.display.flip()

    while True:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                return chess.QUEEN
            elif event.type == pygame.MOUSEBUTTONDOWN:
                mx, my = event.pos
                for rect, pt in options:
                    if rect.collidepoint(mx, my):
                        return pt
            elif event.type == pygame.KEYDOWN:
                keymap = {
                    pygame.K_q: chess.QUEEN,
                    pygame.K_r: chess.ROOK,
                    pygame.K_b: chess.BISHOP,
                    pygame.K_n: chess.KNIGHT,
                }
                if event.key in keymap:
                    return keymap[event.key]

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    if not ASSETS_DIR.exists():
        print("Missing assets/ folder with SVG piece images.")
        return
    if not os.path.exists(ENGINE_EXE):
        print(f"Engine not found at '{ENGINE_EXE}'. Build it first.")
        return

    pygame.init()
    screen = pygame.display.set_mode((WINDOW_SIZE, WINDOW_SIZE))
    pygame.display.set_caption("Minerva (UCI) vs Human")
    clock = pygame.time.Clock()
    images = load_piece_images(SQ_SIZE)

    engine = UCIEngine(ENGINE_EXE)
    engine.start()

    def new_game(play_white: bool):
        b = chess.Board()
        return b, play_white, None  # board, human_color, last_move

    board, human_white, last_move = new_game(HUMAN_PLAYS_WHITE)
    selected_sq = None
    running = True
    engine_thinking = False

    def refresh():
        draw_board(screen, board, images, selected_sq, last_move, legal_targets=None if selected_sq is None else [m.to_square for m in board.legal_moves if m.from_square == selected_sq])
        pygame.display.flip()

    refresh()

    while running:
        # If engine to move and not already thinking: ask engine
        if not board.is_game_over() and ((board.turn == chess.WHITE) != human_white) and not engine_thinking:
            # Push current position
            engine.set_position(board)
            # Refresh right before thinking so user's last move is visible immediately
            refresh()
            engine_thinking = True

            # Run engine call in a thread so UI stays responsive
            bestmove_container = {"uci": None, "info": []}
            def think():
                bm, info = engine.go(THINK_MS)
                bestmove_container["uci"] = bm
                bestmove_container["info"] = info
            t = threading.Thread(target=think, daemon=True)
            t.start()

        # If engine was thinking, try to apply result if ready
        if engine_thinking:
            # Non-blocking: check if thread finished by joining with timeout 0
            if not t.is_alive():
                engine_thinking = False
                uci = bestmove_container["uci"]
                try:
                    move = chess.Move.from_uci(uci)
                except Exception:
                    move = None
                if move and move in board.legal_moves:
                    board.push(move)
                    last_move = move
                else:
                    print("Engine played illegal or empty move:", uci)
                    running = False

        # Handle UI/events
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    running = False
                elif event.key == pygame.K_n:  # new game (keep side)
                    board, human_white, last_move = new_game(human_white)
                    engine.new_game()
                    selected_sq = None
                elif event.key == pygame.K_c:  # swap sides (new game)
                    board, human_white, last_move = new_game(not human_white)
                    engine.new_game()
                    selected_sq = None

            elif event.type == pygame.MOUSEBUTTONDOWN:
                if board.is_game_over():
                    continue
                # Only allow human to act on their turn and if engine isn't thinking
                if ((board.turn == chess.WHITE) == human_white) and not engine_thinking:
                    x, y = event.pos
                    file = x // SQ_SIZE
                    rank = 7 - (y // SQ_SIZE)
                    clicked_sq = chess.square(file, rank)

                    if selected_sq is None:
                        piece = board.piece_at(clicked_sq)
                        if piece and piece.color == board.turn:
                            selected_sq = clicked_sq
                    else:
                        # Attempt move (handle promotion)
                        tentative = chess.Move(selected_sq, clicked_sq)
                        if tentative in board.legal_moves:
                            # Normal or capture
                            board.push(tentative)
                            last_move = tentative
                            selected_sq = None
                            refresh()  # ensure immediate screen update after human move
                        else:
                            # Maybe promotion
                            promo_needed = False
                            piece = board.piece_at(selected_sq)
                            if piece and piece.piece_type == chess.PAWN:
                                # White to rank 7->8 (rank == 7), Black to rank 2->1 (rank == 0)
                                if (piece.color == chess.WHITE and chess.square_rank(clicked_sq) == 7) or \
                                   (piece.color == chess.BLACK and chess.square_rank(clicked_sq) == 0):
                                    promo_needed = True

                            if promo_needed:
                                pt = choose_promotion(screen, images, board.turn == chess.WHITE, clicked_sq)
                                move = chess.Move(selected_sq, clicked_sq, promotion=pt)
                                if move in board.legal_moves:
                                    board.push(move)
                                    last_move = move
                                selected_sq = None
                                refresh()  # immediate update
                            else:
                                # Deselect if invalid
                                selected_sq = None

        # Draw
        draw_board(
            screen,
            board,
            images,
            selected_sq=selected_sq,
            last_move=last_move,
            legal_targets=None if selected_sq is None else [m.to_square for m in board.legal_moves if m.from_square == selected_sq],
        )
        # Status text
        status = []
        if board.is_game_over():
            status.append(f"Game over: {board.result()}")
        else:
            side = "White" if board.turn == chess.WHITE else "Black"
            status.append(f"Turn: {side}")
        status.append(f"You: {'White' if human_white else 'Black'}")
        draw_text(screen, " | ".join(status), (8, 8))
        pygame.display.flip()
        clock.tick(FPS)

    engine.stop()
    pygame.quit()

if __name__ == "__main__":
    main()
