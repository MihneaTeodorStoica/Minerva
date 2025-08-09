import subprocess
import chess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / 'build' / 'minerva'
if not ENGINE.exists():
    ENGINE = ROOT / 'minerva'

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
                return line.split()[1]

    def quit(self):
        self._send('quit')
        self.proc.wait()

def play_self_game(plies=6):
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

if __name__ == '__main__':
    final_board = play_self_game()
    print('Final FEN:', final_board.fen())
