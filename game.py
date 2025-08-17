#!/usr/bin/env python3
# game.py — Minerva GUI with two engines + auto-restart + robust UCI
# ------------------------------------------------------------------
# pip install pygame python-chess cairosvg
#
# Layout:
#   Minerva/
#     build/minerva   (or ./minerva)
#     assets/*.svg
#     game.py
# ------------------------------------------------------------------

from __future__ import annotations

import io
import os
import sys
import time
import math
import queue
import typing as T
import threading
import subprocess
from dataclasses import dataclass
from pathlib import Path
from datetime import datetime

import chess
import chess.pgn
import pygame  # type: ignore
from cairosvg import svg2png  # type: ignore

# -----------------------------------------------
# Config
# -----------------------------------------------
ROOT = Path(__file__).resolve().parent
ASSETS_DIR = ROOT / "assets"
GAMES_DIR = ROOT / "games"
ENGINE_EXE = ROOT / "build" / "minerva"
if not ENGINE_EXE.exists():
    alt = ROOT / "minerva"
    if alt.exists():
        ENGINE_EXE = alt

# Wider default window so side panel fits
START_WINDOW = (1000, 760)      # resizable
BASE_BOARD_MARGIN = 24
TARGET_FPS = 60
DEFAULT_THINK_MS = 300

# lichess-ish colors
COL_LIGHT = (240, 217, 181)
COL_DARK = (181, 136, 99)
COL_SELECT = (186, 202, 68)
COL_LAST = (246, 246, 105)
COL_CHECK = (209, 66, 66)
COL_TEXT = (22, 22, 22)
COL_HINT = (30, 30, 30, 110)
COL_PANEL_BG = (245, 245, 245)
COL_BTN = (230, 230, 230)
COL_BTN_HOVER = (200, 200, 200)
COL_ARROW = (50, 120, 230, 160)
COL_CIRCLE = (50, 120, 230, 160)

# -----------------------------------------------
# Utils
# -----------------------------------------------
def safe_mkdir(p: Path):
    p.mkdir(parents=True, exist_ok=True)

def now_stamp() -> str:
    return datetime.now().strftime("%Y-%m-%d_%H-%M-%S")

def clamp(v, a, b): return max(a, min(b, v))

# -----------------------------------------------
# UCI wrapper
# -----------------------------------------------
class UCIEngine:
    def __init__(self, exe_path: Path, name: str):
        self.exe_path = str(exe_path)
        self.name = name
        self.proc: subprocess.Popen[str] | None = None
        self.q: "queue.Queue[str]" = queue.Queue()
        self.reader_thread: threading.Thread | None = None
        self.lock = threading.Lock()
        self.running = False
        self.options: dict[str, T.Any] = {}

    # ---- process management
    def _spawn(self) -> None:
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

    def start(self) -> None:
        if not os.path.exists(self.exe_path):
            raise FileNotFoundError(f"Engine not found: {self.exe_path}")
        self._spawn()
        self._handshake()
        self._apply_options()

    def restart(self) -> None:
        self.stop()
        self._spawn()
        self._handshake()
        self._apply_options()

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

    def is_alive(self) -> bool:
        return bool(self.proc) and (self.proc.poll() is None)

    def _read_loop(self):
        assert self.proc and self.proc.stdout
        for line in self.proc.stdout:
            self.q.put(line.rstrip("\n"))
        self.running = False

    # ---- low level I/O
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
                    raise TimeoutError(f"[{self.name}] Timed out waiting for '{token}'")
                continue
            if token in line:
                return

    def drain(self) -> None:
        try:
            while True:
                self.q.get_nowait()
        except queue.Empty:
            pass

    def set_option(self, name: str, value: T.Any) -> None:
        self.options[name] = value
        if self.is_alive():
            self._send(f"setoption name {name} value {value}")
            self._send("isready")
            self._wait_for("readyok", 5.0)

    def _apply_options(self) -> None:
        for name, value in self.options.items():
            self._send(f"setoption name {name} value {value}")
        if self.options:
            self._send("isready")
            self._wait_for("readyok", 5.0)
            self.drain()

    # ---- handshake / new game
    def _handshake(self):
        self._send("uci")
        self._wait_for("uciok", 5.0)
        self._send("isready")
        self._wait_for("readyok", 5.0)

    def new_game(self):
        if not self.is_alive():
            self.restart()
            return
        self.drain()
        self._send("ucinewgame")
        self._send("isready")
        self._wait_for("readyok", 5.0)
        self.drain()

    def sync(self):
        if not self.is_alive():
            self.restart()
            return
        self.drain()
        self._send("isready")
        self._wait_for("readyok", 5.0)
        self.drain()

    # ---- one search (robust)
    def prepare_and_go(self, board: chess.Board, movetime_ms: int) -> tuple[str, list[str]]:
        """
        Robust per-move search:
          - auto-restart if the engine died
          - drain + isready before search
          - position fen …; go movetime …
          - deadline + stop
        """
        # ensure process
        if not self.is_alive():
            print(f"[{self.name}] engine not alive — restarting")
            self.restart()

        # clean channel + ready
        self.drain()
        self._send("isready")
        self._wait_for("readyok", 5.0)
        self.drain()

        # set position + go
        # Use ``shredder_fen()`` to omit the halfmove clock and fullmove
        # number from the FEN sent to the engine.  Some UCI engines only
        # parse the first four FEN fields and may respond with the bogus
        # move ``0000`` if the additional counters are present.
        fen = board.shredder_fen()
        self._send(f"position fen {fen}")
        self._send(f"go movetime {movetime_ms}")

        info: list[str] = []
        best: str | None = None
        deadline = time.time() + (movetime_ms / 1000.0) + 1.5
        sent_stop = False

        while True:
            if not self.is_alive():
                # crash mid-search → restart and report "(none)"
                print(f"[{self.name}] crashed during search — restarting")
                self.restart()
                break

            remaining = deadline - time.time()
            if remaining <= 0 and not sent_stop:
                try: self._send("stop")
                except Exception: pass
                sent_stop = True
                deadline = time.time() + 1.0
                remaining = deadline - time.time()

            try:
                line = self.q.get(timeout=max(0.01, min(0.2, remaining)))
            except queue.Empty:
                if time.time() > deadline:
                    break
                continue

            if line.startswith("info "):
                info.append(line)
            elif line.startswith("bestmove"):
                parts = line.split()
                if len(parts) >= 2:
                    best = parts[1]
                break

        best = best or "0000"
        self.sync()
        return best, info


def parse_score(info: list[str]) -> float | None:
    """Extract centipawn score from engine info lines (white perspective)."""
    for line in reversed(info):
        parts = line.split()
        if "score" in parts:
            idx = parts.index("score")
            if idx + 2 < len(parts):
                typ = parts[idx + 1]
                val = parts[idx + 2]
                try:
                    if typ == "cp":
                        return int(val) / 100.0
                    elif typ == "mate":
                        # Represent mate as large value
                        m = int(val)
                        return 1000.0 if m > 0 else -1000.0
                except ValueError:
                    pass
    return None

# -----------------------------------------------
# View / rendering helpers
# -----------------------------------------------
@dataclass
class View:
    w: int
    h: int
    margin: int
    flipped: bool = False

    def board_rect(self) -> pygame.Rect:
        size = min(self.w, self.h) - 2 * self.margin
        size = max(320, size)
        x = (self.w - size) // 2
        y = (self.h - size) // 2
        return pygame.Rect(x, y, size, size)

    def sq_size(self) -> int:
        return self.board_rect().width // 8

    def square_at(self, x: int, y: int) -> int | None:
        br = self.board_rect()
        if not br.collidepoint(x, y):
            return None
        s = self.sq_size()
        sx = (x - br.x) // s
        sy = (y - br.y) // s
        file = sx
        rank = 7 - sy
        if self.flipped:
            file = 7 - file
            rank = 7 - rank
        return chess.square(file, rank)

    def square_center(self, sq: int) -> tuple[int, int]:
        f = chess.square_file(sq)
        r = chess.square_rank(sq)
        if self.flipped:
            f = 7 - f
            r = 7 - r
        br = self.board_rect()
        s = self.sq_size()
        x = br.x + f * s + s // 2
        y = br.y + (7 - r) * s + s // 2
        return x, y

class PieceImages:
    def __init__(self, assets_dir: Path):
        self.assets_dir = assets_dir
        self.cache: dict[int, dict[str, pygame.Surface]] = {}

    def get(self, size: int) -> dict[str, pygame.Surface]:
        if size in self.cache:
            return self.cache[size]
        imgs: dict[str, pygame.Surface] = {}
        for color in ["w", "b"]:
            for piece in ["K", "Q", "R", "B", "N", "P"]:
                svg_path = self.assets_dir / f"{color}{piece}.svg"
                if not svg_path.exists():
                    raise FileNotFoundError(svg_path)
                png = svg2png(url=str(svg_path), output_width=size, output_height=size)
                img = pygame.image.load(io.BytesIO(png), svg_path.name).convert_alpha()
                imgs[color + piece] = img
        self.cache[size] = imgs
        return imgs

def draw_text(surface, txt, pos, size=18, color=COL_TEXT):
    font = pygame.font.SysFont("arial", size)
    img = font.render(txt, True, color)
    surface.blit(img, pos)

def draw_arrow_xy(surface, x1, y1, x2, y2, color_rgba, thickness=6):
    dx, dy = x2 - x1, y2 - y1
    dist = math.hypot(dx, dy)
    if dist < 1e-6: return
    ux, uy = dx / dist, dy / dist
    tail = 18
    head = 18
    x_tail = x1 + ux * tail
    y_tail = y1 + uy * tail
    x_head = x2 - ux * head
    y_head = y2 - uy * head
    col = pygame.Color(*color_rgba)
    pygame.draw.line(surface, col, (x_tail, y_tail), (x_head, y_head), thickness)
    perp = (-uy, ux)
    w = thickness * 2
    p1 = (x_head, y_head)
    p2 = (x_head - ux * head + perp[0] * w, y_head - uy * head + perp[1] * w)
    p3 = (x_head - ux * head - perp[0] * w, y_head - uy * head - perp[1] * w)
    s = pygame.Surface(surface.get_size(), pygame.SRCALPHA)
    pygame.draw.polygon(s, col, [p1, p2, p3])
    surface.blit(s, (0, 0))

def draw_arrow(surface, view: View, a: int, b: int, color_rgba, thickness=6):
    x1, y1 = view.square_center(a)
    x2, y2 = view.square_center(b)
    draw_arrow_xy(surface, x1, y1, x2, y2, color_rgba, thickness)

def draw_circle(surface, view: View, sq: int, color_rgba, thickness=4):
    x, y = view.square_center(sq)
    s = view.sq_size()
    radius = int(s * 0.36)
    pygame.draw.circle(surface, pygame.Color(*color_rgba), (x, y), radius, thickness)

def draw_button(surface, rect: pygame.Rect, label: str, hover: bool):
    pygame.draw.rect(surface, COL_BTN_HOVER if hover else COL_BTN, rect, border_radius=8)
    draw_text(surface, label, (rect.x + 12, rect.y + rect.height // 2 - 9), 18)

def draw_board(surface: pygame.Surface, view: View, board: chess.Board, images: dict[str, pygame.Surface],
               selected: int | None, last_move: chess.Move | None,
               legal_targets: list[int] | None,
               arrows: set[tuple[int, int]], circles: set[int]):
    br = view.board_rect()
    s = view.sq_size()
    # squares
    for r in range(8):
        for f in range(8):
            rr = r
            ff = f
            if view.flipped:
                rr = 7 - r
                ff = 7 - f
            color = COL_LIGHT if (rr + ff) % 2 == 0 else COL_DARK
            pygame.draw.rect(surface, color, pygame.Rect(br.x + f * s, br.y + r * s, s, s))
    # last move
    if last_move:
        for sq in (last_move.from_square, last_move.to_square):
            cx, cy = view.square_center(sq)
            pygame.draw.rect(surface, COL_LAST, pygame.Rect(cx - s // 2, cy - s // 2, s, s), 0)
    # check highlight
    if board.is_check():
        ksq = board.king(board.turn)
        if ksq is not None:
            cx, cy = view.square_center(ksq)
            pygame.draw.rect(surface, COL_CHECK, pygame.Rect(cx - s // 2, cy - s // 2, s, s), 0)
    # selected
    if selected is not None:
        cx, cy = view.square_center(selected)
        pygame.draw.rect(surface, COL_SELECT, pygame.Rect(cx - s // 2, cy - s // 2, s, s), 0)
    # pieces
    for sq in chess.SQUARES:
        p = board.piece_at(sq)
        if not p: continue
        key = ("w" if p.color == chess.WHITE else "b") + p.symbol().upper()
        img = images[key]
        x, y = view.square_center(sq)
        surface.blit(img, (x - s // 2, y - s // 2))
    # hints
    if legal_targets:
        dot = pygame.Surface((s, s), pygame.SRCALPHA)
        pygame.draw.circle(dot, COL_HINT, (s // 2, s // 2), s // 8)
        for sq in legal_targets:
            cx, cy = view.square_center(sq)
            surface.blit(dot, (cx - s // 2, cy - s // 2))
    # arrows
    for a, b in arrows:
        draw_arrow(surface, view, a, b, COL_ARROW, thickness=max(4, s // 10))
    # circles
    for c in circles:
        draw_circle(surface, view, c, COL_CIRCLE, thickness=max(3, s // 14))


def draw_side_panel(surface: pygame.Surface, view: View, images_cache: PieceImages,
                    history: list[str], cap_white: list[str], cap_black: list[str],
                    eval_cp: float | None):
    br = view.board_rect()
    x = br.right + 16
    y = br.top
    draw_text(surface, "Moves:", (x, y))
    y += 24
    lines: list[str] = []
    for i in range(0, len(history), 2):
        w = history[i]
        b = history[i + 1] if i + 1 < len(history) else ""
        lines.append(f"{i//2 + 1}. {w} {b}")
    for line in lines[-15:]:
        draw_text(surface, line, (x, y))
        y += 20

    small_size = max(24, view.sq_size() // 2)
    small_imgs = images_cache.get(small_size)

    y += 10
    draw_text(surface, "Captured by White:", (x, y))
    y += 24
    for i, sym in enumerate(cap_black):
        key = "b" + sym
        img = small_imgs.get(key)
        if img:
            surface.blit(img, (x + i * (small_size + 2), y))
    y += small_size + 8

    draw_text(surface, "Captured by Black:", (x, y))
    y += 24
    for i, sym in enumerate(cap_white):
        key = "w" + sym
        img = small_imgs.get(key)
        if img:
            surface.blit(img, (x + i * (small_size + 2), y))

    eval_y = br.bottom - 30
    if eval_cp is not None:
        draw_text(surface, f"Eval: {eval_cp:+.2f}", (x, eval_y))
    else:
        draw_text(surface, "Eval: --", (x, eval_y))

# -----------------------------------------------
# Promotion overlay
# -----------------------------------------------
def choose_promotion(surface, view: View, images, color_is_white: bool, at_square: int) -> chess.PieceType:
    s = view.sq_size()
    file = chess.square_file(at_square)
    if view.flipped: file = 7 - file
    x = view.board_rect().x + file * s
    top_rank = 0 if color_is_white else 4
    y = view.board_rect().y + top_rank * s
    order = [chess.QUEEN, chess.ROOK, chess.BISHOP, chess.KNIGHT]
    rects: list[tuple[pygame.Rect, chess.PieceType]] = []
    shade = pygame.Surface(surface.get_size(), pygame.SRCALPHA); shade.fill((0, 0, 0, 100))
    surface.blit(shade, (0, 0))
    for i, pt in enumerate(order):
        key = ("w" if color_is_white else "b") + chess.Piece(pt, color_is_white).symbol().upper()
        r = pygame.Rect(x, y + i * s, s, s)
        pygame.draw.rect(surface, (250, 250, 250), r)
        pygame.draw.rect(surface, (80, 80, 80), r, 2)
        surface.blit(images[key], r.topleft)
        rects.append((r, pt))
    pygame.display.flip()
    while True:
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT: return chess.QUEEN
            if ev.type == pygame.KEYDOWN:
                m = {pygame.K_q: chess.QUEEN, pygame.K_r: chess.ROOK, pygame.K_b: chess.BISHOP, pygame.K_n: chess.KNIGHT}
                if ev.key in m: return m[ev.key]
            if ev.type == pygame.MOUSEBUTTONDOWN:
                mx, my = ev.pos
                for r, pt in rects:
                    if r.collidepoint(mx, my): return pt

# -----------------------------------------------
# Engine move normalization
# -----------------------------------------------
def normalize_engine_move(board: chess.Board, uci: str) -> chess.Move | None:
    if not uci or len(uci) < 4: return None
    try:
        mv = chess.Move.from_uci(uci)
    except Exception:
        mv = None
    if mv and mv in board.legal_moves: return mv
    # Castling quirks from buggy engines
    if uci in ("e1h1", "e1a1", "e8h8", "e8a8"):
        fix = "e1g1" if uci == "e1h1" else "e1c1" if uci == "e1a1" else "e8g8" if uci == "e8h8" else "e8c8"
        try:
            mv2 = chess.Move.from_uci(fix)
            if mv2 in board.legal_moves: return mv2
        except Exception:
            pass
    return None

def first_legal(board: chess.Board) -> chess.Move | None:
    for m in board.legal_moves:
        return m
    return None

# -----------------------------------------------
# Game state / PGN
# -----------------------------------------------
@dataclass
class GameConfig:
    human_vs_engine: bool
    human_white: bool
    think_ms: int
    threads: int

@dataclass
class AnnoState:
    arrows: set[tuple[int, int]]
    circles: set[int]
    rmb_down_sq: int | None = None

def save_pgn(board: chess.Board, headers: dict[str, str], out_dir=GAMES_DIR) -> Path:
    safe_mkdir(out_dir)
    game = chess.pgn.Game.from_board(board)
    for k, v in headers.items(): game.headers[k] = v
    fname = f"{now_stamp()}_{headers.get('White','White')}vs{headers.get('Black','Black')}.pgn"
    path = out_dir / fname
    with open(path, "w", encoding="utf-8") as f:
        print(game, file=f, end="\n")
    print("PGN saved:", path)
    return path

# -----------------------------------------------
# Menu
# -----------------------------------------------
def menu(surface: pygame.Surface) -> GameConfig | None:
    running = True
    clock = pygame.time.Clock()
    human_vs_engine = True
    human_white = True
    think_ms = DEFAULT_THINK_MS
    threads = max(1, os.cpu_count() or 1)
    while running:
        surface.fill(COL_PANEL_BG)
        draw_text(surface, "Minerva — Friendly Match", (24, 24), 28)
        y = 90; bx = 24
        r1 = pygame.Rect(bx, y, 240, 38)
        r2 = pygame.Rect(bx + 260, y, 240, 38)
        pygame.draw.rect(surface, COL_BTN if human_vs_engine else COL_BTN_HOVER, r1, border_radius=8)
        pygame.draw.rect(surface, COL_BTN if not human_vs_engine else COL_BTN_HOVER, r2, border_radius=8)
        draw_text(surface, "Human vs Engine", (r1.x + 10, r1.y + 9))
        draw_text(surface, "Engine vs Engine", (r2.x + 10, r2.y + 9))
        y += 60
        r3 = pygame.Rect(bx, y, 160, 38)
        r4 = pygame.Rect(bx + 170, y, 160, 38)
        pygame.draw.rect(surface, COL_BTN if human_white else COL_BTN_HOVER, r3, border_radius=8)
        pygame.draw.rect(surface, COL_BTN if not human_white else COL_BTN_HOVER, r4, border_radius=8)
        draw_text(surface, "White", (r3.x + 10, r3.y + 9))
        draw_text(surface, "Black", (r4.x + 10, r4.y + 9))
        y += 60
        draw_text(surface, f"Engine movetime: {think_ms} ms  (Left/Right to adjust)", (bx, y), 20)
        y += 40
        draw_text(surface, f"Engine threads: {threads}  (Up/Down to adjust)", (bx, y), 20)
        y += 40
        rs = pygame.Rect(bx, y, 200, 44)
        draw_button(surface, rs, "Start", False)

        for ev in pygame.event.get():
            if ev.type == pygame.QUIT: return None
            if ev.type == pygame.KEYDOWN:
                if ev.key == pygame.K_ESCAPE: return None
                if ev.key == pygame.K_LEFT:  think_ms = int(max(50, think_ms - 50))
                if ev.key == pygame.K_RIGHT: think_ms = int(min(5000, think_ms + 50))
                if ev.key == pygame.K_UP:    threads = int(clamp(threads + 1, 1, 64))
                if ev.key == pygame.K_DOWN:  threads = int(clamp(threads - 1, 1, 64))
            if ev.type == pygame.MOUSEBUTTONDOWN and ev.button == 1:
                mx, my = ev.pos
                if r1.collidepoint(mx, my): human_vs_engine = True
                elif r2.collidepoint(mx, my): human_vs_engine = False
                elif r3.collidepoint(mx, my): human_white = True
                elif r4.collidepoint(mx, my): human_white = False
                elif rs.collidepoint(mx, my): return GameConfig(human_vs_engine, human_white, think_ms, threads)

        pygame.display.flip()
        clock.tick(TARGET_FPS)

# -----------------------------------------------
# Loops
# -----------------------------------------------
def loop_human(surface: pygame.Surface, eng: UCIEngine, human_white: bool, think_ms: int):
    images_cache = PieceImages(ASSETS_DIR)
    view = View(surface.get_width(), surface.get_height(), BASE_BOARD_MARGIN, not human_white)
    board = chess.Board()
    anno = AnnoState(set(), set())
    last_move: chess.Move | None = None
    selected: int | None = None
    premove: chess.Move | None = None

    move_history: list[str] = []
    captured_white: list[str] = []  # pieces captured from white
    captured_black: list[str] = []  # pieces captured from black
    last_eval: float | None = None

    headers = {
        "Event": "Minerva Friendly",
        "Site": "Local",
        "Date": datetime.now().strftime("%Y.%m.%d"),
        "Round": "-",
        "White": "Human" if human_white else "Minerva",
        "Black": "Minerva" if human_white else "Human",
        "Result": "*",
    }

    clock = pygame.time.Clock()

    def redraw():
        surface.fill((250, 250, 250))
        imgs = images_cache.get(view.sq_size())
        legal_targets = None
        if selected is not None:
            legal_targets = [m.to_square for m in board.legal_moves if m.from_square == selected]
        draw_board(surface, view, board, imgs, selected, last_move, legal_targets, anno.arrows, anno.circles)
        draw_side_panel(surface, view, images_cache, move_history, captured_white, captured_black, last_eval)
        draw_text(surface, f"{'White' if board.turn == chess.WHITE else 'Black'} to move | "
                           f"{'You' if (board.turn == chess.WHITE)==human_white else 'Engine'}",
                  (16, 12))
        pygame.display.flip()

    eng.new_game()
    redraw()

    think_thread: threading.Thread | None = None
    pending_move: dict[str, T.Any] = {"bm": None, "eval": None}

    def start_engine_think():
        def _run():
            bm, info = eng.prepare_and_go(board, think_ms)
            pending_move["bm"] = bm
            pending_move["eval"] = parse_score(info)
        t = threading.Thread(target=_run, daemon=True)
        t.start(); return t

    running = True
    while running:
        # engine to move?
        if not board.is_game_over() and ((board.turn == chess.WHITE) != human_white):
            if think_thread is None:
                pending_move["bm"] = None
                think_thread = start_engine_think()
        else:
            think_thread = None

        if think_thread and not think_thread.is_alive():
            bm = T.cast(str | None, pending_move["bm"])
            cp = T.cast(float | None, pending_move.get("eval"))
            pending_move["bm"] = None
            pending_move["eval"] = None
            think_thread = None

            if cp is not None:
                last_eval = cp if board.turn == chess.WHITE else -cp

            mv = normalize_engine_move(board, bm or "")
            ok = mv is not None and (mv in board.legal_moves)
            if ok:
                p = board.piece_at(mv.from_square)
                ok = bool(p and p.color == board.turn)
            if not ok:
                # retry once
                print("Engine returned illegal or stale move:", bm, "— hard reset + retry")
                eng.restart()
                bm2, _ = eng.prepare_and_go(board, think_ms)
                mv = normalize_engine_move(board, bm2 or "")
                ok = mv is not None and (mv in board.legal_moves)
                if ok:
                    p = board.piece_at(mv.from_square)
                    ok = bool(p and p.color == board.turn)
            if not ok:
                # fallback: make a first legal move (keep game flowing)
                mv = first_legal(board)

            if mv:
                san = board.san(mv)
                if board.is_capture(mv):
                    cap_sq = mv.to_square
                    if board.is_en_passant(mv):
                        cap_sq += -8 if board.turn == chess.WHITE else 8
                    cap_piece = board.piece_at(cap_sq)
                    if cap_piece:
                        if cap_piece.color == chess.WHITE:
                            captured_white.append(cap_piece.symbol().upper())
                        else:
                            captured_black.append(cap_piece.symbol().upper())
                board.push(mv)
                last_move = mv
                move_history.append(san)
                # premove auto-play if queued and now it's human's turn
                if premove and (premove in board.legal_moves):
                    san_p = board.san(premove)
                    if board.is_capture(premove):
                        cap_sq = premove.to_square
                        if board.is_en_passant(premove):
                            cap_sq += -8 if board.turn == chess.WHITE else 8
                        cap_piece = board.piece_at(cap_sq)
                        if cap_piece:
                            if cap_piece.color == chess.WHITE:
                                captured_white.append(cap_piece.symbol().upper())
                            else:
                                captured_black.append(cap_piece.symbol().upper())
                    board.push(premove)
                    last_move = premove
                    move_history.append(san_p)
                    premove = None
                redraw()
            else:
                print("Engine failed; no legal fallback.")

        for ev in pygame.event.get():
            if ev.type == pygame.QUIT: running = False
            elif ev.type == pygame.VIDEORESIZE:
                view.w, view.h = ev.w, ev.h; redraw()
            elif ev.type == pygame.KEYDOWN:
                if ev.key == pygame.K_ESCAPE: running = False
                elif ev.key == pygame.K_f: view.flipped = not view.flipped; redraw()
                elif ev.key == pygame.K_n:
                    board.reset()
                    last_move = None; selected = None; premove = None
                    move_history.clear(); captured_white.clear(); captured_black.clear(); last_eval = None
                    eng.new_game()
                    redraw()
            elif ev.type == pygame.MOUSEBUTTONDOWN:
                if ev.button == 1:  # LMB
                    sq = view.square_at(*ev.pos)
                    if sq is None or board.is_game_over(): continue
                    # human to move?
                    if ((board.turn == chess.WHITE) == human_white):
                        p = board.piece_at(sq)
                        if selected is None:
                            if p and p.color == board.turn:
                                selected = sq; redraw()
                        else:
                            tentative = chess.Move(selected, sq)
                            if tentative in board.legal_moves:
                                san = board.san(tentative)
                                if board.is_capture(tentative):
                                    cap_sq = tentative.to_square
                                    if board.is_en_passant(tentative):
                                        cap_sq += -8 if board.turn == chess.WHITE else 8
                                    cap_piece = board.piece_at(cap_sq)
                                    if cap_piece:
                                        if cap_piece.color == chess.WHITE:
                                            captured_white.append(cap_piece.symbol().upper())
                                        else:
                                            captured_black.append(cap_piece.symbol().upper())
                                board.push(tentative); last_move = tentative
                                move_history.append(san)
                                selected = None; premove = None; redraw()
                            else:
                                piece = board.piece_at(selected); need = False
                                if piece and piece.piece_type == chess.PAWN:
                                    if (piece.color == chess.WHITE and chess.square_rank(sq) == 7) or \
                                       (piece.color == chess.BLACK and chess.square_rank(sq) == 0):
                                        need = True
                                if need:
                                    imgs = images_cache.get(view.sq_size())
                                    pt = choose_promotion(surface, view, imgs, board.turn == chess.WHITE, sq)
                                    mv = chess.Move(selected, sq, promotion=pt)
                                    if mv in board.legal_moves:
                                        san = board.san(mv)
                                        if board.is_capture(mv):
                                            cap_sq = mv.to_square
                                            if board.is_en_passant(mv):
                                                cap_sq += -8 if board.turn == chess.WHITE else 8
                                            cap_piece = board.piece_at(cap_sq)
                                            if cap_piece:
                                                if cap_piece.color == chess.WHITE:
                                                    captured_white.append(cap_piece.symbol().upper())
                                                else:
                                                    captured_black.append(cap_piece.symbol().upper())
                                        board.push(mv); last_move = mv
                                        move_history.append(san)
                                    selected = None; premove = None; redraw()
                                else:
                                    selected = None; redraw()
                    else:
                        # set premove
                        if selected is None:
                            p = board.piece_at(sq)
                            if p and p.color == (chess.WHITE if human_white else chess.BLACK):
                                selected = sq; redraw()
                        else:
                            premove = chess.Move(selected, sq)
                            selected = None; redraw()
                elif ev.button == 3:  # RMB annotation start
                    sq = view.square_at(*ev.pos)
                    anno.rmb_down_sq = sq
            elif ev.type == pygame.MOUSEBUTTONUP:
                if ev.button == 3:
                    sq_up = view.square_at(*ev.pos)
                    if anno.rmb_down_sq is not None and sq_up is not None:
                        if anno.rmb_down_sq == sq_up:
                            if sq_up in anno.circles: anno.circles.remove(sq_up)
                            else: anno.circles.add(sq_up)
                        else:
                            a = (anno.rmb_down_sq, sq_up)
                            if a in anno.arrows: anno.arrows.remove(a)
                            else: anno.arrows.add(a)
                        redraw()
                    anno.rmb_down_sq = None

        # kick engine think if needed
        if not board.is_game_over() and ((board.turn == chess.WHITE) != human_white) and think_thread is None:
            pending_move["bm"] = None
            think_thread = start_engine_think()

        clock.tick(TARGET_FPS)

    # result
    res = "1-0" if board.is_checkmate() and board.turn == chess.BLACK else \
          "0-1" if board.is_checkmate() and board.turn == chess.WHITE else \
          "1/2-1/2" if board.is_stalemate() or board.can_claim_threefold_repetition() else "*"
    headers["Result"] = res
    save_pgn(board, headers)

def loop_engine_vs_engine(surface: pygame.Surface, eng: UCIEngine, think_ms: int):
    images_cache = PieceImages(ASSETS_DIR)
    view = View(surface.get_width(), surface.get_height(), BASE_BOARD_MARGIN, False)
    board = chess.Board()
    anno = AnnoState(set(), set())
    last_move: chess.Move | None = None

    move_history: list[str] = []
    captured_white: list[str] = []
    captured_black: list[str] = []
    last_eval: float | None = None

    headers = {
        "Event": "Minerva Friendly",
        "Site": "Local",
        "Date": datetime.now().strftime("%Y.%m.%d"),
        "Round": "-",
        "White": "Minerva",
        "Black": "Minerva",
        "Result": "*",
    }

    clock = pygame.time.Clock()

    def redraw():
        surface.fill((250, 250, 250))
        imgs = images_cache.get(view.sq_size())
        draw_board(surface, view, board, imgs, None, last_move, None, anno.arrows, anno.circles)
        draw_side_panel(surface, view, images_cache, move_history, captured_white, captured_black, last_eval)
        draw_text(surface, f"{'White' if board.turn == chess.WHITE else 'Black'} to move", (16, 12))
        pygame.display.flip()

    eng.new_game()
    redraw()

    running = True
    while running and not board.is_game_over():
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT: running = False
            elif ev.type == pygame.VIDEORESIZE:
                view.w, view.h = ev.w, ev.h; redraw()
            elif ev.type == pygame.KEYDOWN:
                if ev.key == pygame.K_ESCAPE: running = False
                elif ev.key == pygame.K_f: view.flipped = not view.flipped; redraw()

        bm, info = eng.prepare_and_go(board, think_ms)
        cp = parse_score(info)
        if cp is not None:
            last_eval = cp if board.turn == chess.WHITE else -cp
        mv = normalize_engine_move(board, bm)

        ok = mv is not None and (mv in board.legal_moves)
        if ok:
            p = board.piece_at(mv.from_square)
            ok = bool(p and p.color == board.turn)

        if not ok:
            print("Engine returned illegal or stale move:", bm, "— hard reset + retry")
            eng.restart()
            bm2, _ = eng.prepare_and_go(board, think_ms)
            mv = normalize_engine_move(board, bm2)
            ok = mv is not None and (mv in board.legal_moves)
            if ok:
                p = board.piece_at(mv.from_square)
                ok = bool(p and p.color == board.turn)

        if not ok:
            # final fallback: make a legal move to keep game going
            mv = first_legal(board)

        if mv:
            san = board.san(mv)
            if board.is_capture(mv):
                cap_sq = mv.to_square
                if board.is_en_passant(mv):
                    cap_sq += -8 if board.turn == chess.WHITE else 8
                cap_piece = board.piece_at(cap_sq)
                if cap_piece:
                    if cap_piece.color == chess.WHITE:
                        captured_white.append(cap_piece.symbol().upper())
                    else:
                        captured_black.append(cap_piece.symbol().upper())
            board.push(mv)
            last_move = mv
            move_history.append(san)
            redraw()
        else:
            print("Engine failed; no legal fallback. Stopping.")
            break

        clock.tick(TARGET_FPS)

    res = "1-0" if board.is_checkmate() and board.turn == chess.BLACK else \
          "0-1" if board.is_checkmate() and board.turn == chess.WHITE else \
          "1/2-1/2" if board.is_stalemate() or board.can_claim_threefold_repetition() else "*"
    headers["Result"] = res
    save_pgn(board, headers)

# -----------------------------------------------
# Main
# -----------------------------------------------
def main():
    if not ASSETS_DIR.exists():
        print("Missing assets/ with 12 SVGs.")
        return
    if not ENGINE_EXE.exists():
        print(f"Engine not found at '{ENGINE_EXE}'. Build it first.")
        return

    pygame.init()
    pygame.display.set_caption("Minerva GUI")
    surface = pygame.display.set_mode(START_WINDOW, pygame.RESIZABLE)
    cfg = menu(surface)
    if cfg is None:
        pygame.quit(); return

    eng = UCIEngine(ENGINE_EXE, "Minerva")
    eng.set_option("Threads", cfg.threads)
    eng.start()

    try:
        if cfg.human_vs_engine:
            loop_human(surface, eng, cfg.human_white, cfg.think_ms)
        else:
            loop_engine_vs_engine(surface, eng, cfg.think_ms)
    finally:
        eng.stop()
        pygame.quit()

if __name__ == "__main__":
    main()
