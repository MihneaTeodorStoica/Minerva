import sys
import types
import queue
import threading
import chess
from pathlib import Path

# Ensure project root on path and stub heavy dependencies for importing game
sys.path.append(str(Path(__file__).resolve().parents[1]))
sys.modules.setdefault("pygame", types.ModuleType("pygame"))
sys.modules.setdefault("cairosvg", types.SimpleNamespace(svg2png=lambda *a, **k: b""))

from game import UCIEngine


class DummyEngine(UCIEngine):
    def __init__(self):
        # minimal initialization without spawning a process
        self.exe_path = ""
        self.name = "dummy"
        self.proc = None
        self.q: "queue.Queue[str]" = queue.Queue()
        self.reader_thread = None
        self.lock = threading.Lock()
        self.running = True
        self.options = {}
        self.sent: list[str] = []

    def is_alive(self) -> bool:
        return True

    def drain(self) -> None:
        pass

    def _send(self, cmd: str) -> None:
        self.sent.append(cmd)

    def _wait_for(self, token: str, timeout: float | None) -> None:
        pass

    def sync(self) -> None:
        pass

    def restart(self) -> None:
        pass


def test_prepare_and_go_depth_sends_depth_command():
    eng = DummyEngine()
    board = chess.Board()
    eng.q.put("bestmove e2e4")
    best, _ = eng.prepare_and_go_depth(board, 3)
    assert best == "e2e4"
    assert "go depth 3" in eng.sent


def test_prepare_and_go_with_max_depth():
    eng = DummyEngine()
    board = chess.Board()
    eng.q.put("bestmove e2e4")
    best, _ = eng.prepare_and_go(board, 1000, max_depth=2)
    assert best == "e2e4"
    assert "go movetime 1000 depth 2" in eng.sent
