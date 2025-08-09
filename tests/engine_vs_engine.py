import subprocess
import chess
import chess.pgn
from pathlib import Path
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / 'build' / 'minerva'
if not ENGINE.exists():
    ENGINE = ROOT / 'minerva'
PGN_OUT = ROOT / 'tests' / 'selfplay.pgn'

class Engine:
    def __init__(self, path: Path):
        self.proc = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self._send('uci')
        self._read_until('uciok')
        self._send('isready')
        self._read_until('readyok')

    def _send(self, cmd: str):
        self.proc.stdin.write(cmd + '\n')
        self.proc.stdin.flush()

    def _read_until(self, token: str):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                break
            if token in line:
                return line

    def bestmove(self, moves, think_ms=50):
        self._send('position startpos moves ' + ' '.join(moves))
        self._send(f'go movetime {think_ms}')
        while True:
            line = self.proc.stdout.readline().strip()
            if line.startswith('bestmove'):
                move = line.split()[1]
                break
        self._send('isready')
        self._read_until('readyok')
        return move

    def quit(self):
        self._send('quit')
        self.proc.wait()

def play_self_game(max_plies=100):
    eng = Engine(ENGINE)
    board = chess.Board()
    moves = []
    for _ in range(plies):
        bm = eng.bestmove(moves, think_ms=50)
        assert bm != '0000'
        move = chess.Move.from_uci(bm)
        assert move in board.legal_moves
        board.push(move)
        moves.append(bm)
    eng.quit()
    return board

def write_pgn(board: chess.Board, path: Path = Path("selfplay.pgn")) -> Path:
    game = chess.pgn.Game.from_board(board)
    game.headers["Event"] = "Minerva Self-Play Test"
    game.headers["Site"] = "Local"
    game.headers["Date"] = datetime.now().strftime("%Y.%m.%d")
    game.headers["Round"] = "1"
    game.headers["White"] = "Minerva"
    game.headers["Black"] = "Minerva"
    game.headers["Result"] = board.result()  # "*" if unfinished

    with open(path, "w", encoding="utf-8") as f:
        exporter = chess.pgn.FileExporter(f)
        game.accept(exporter)
    return path

if __name__ == '__main__':
    final_board = play_self_game()
    out = write_pgn(final_board, Path("selfplay.pgn"))
    print('Final FEN:', final_board.fen())
    print('PGN written to:', out)
