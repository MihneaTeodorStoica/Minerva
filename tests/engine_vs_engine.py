import subprocess
import chess
import chess.pgn

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
    moves: list[str] = []
    for _ in range(max_plies):
        bm = eng.bestmove(moves, think_ms=50)
        assert bm != '0000'
        move = chess.Move.from_uci(bm)
        assert move in board.legal_moves
        board.push(move)
        moves.append(bm)
        if board.is_game_over():
            break
    eng.quit()

    game = chess.pgn.Game.from_board(board)
    game.headers['Event'] = 'Selfplay'
    game.headers['White'] = 'Minerva'
    game.headers['Black'] = 'Minerva'
    game.headers['Result'] = board.result(claim_draw=True)
    with open(PGN_OUT, 'w', encoding='utf-8') as f:
        print(game, file=f, end='\n')
    return board

if __name__ == '__main__':
    final_board = play_self_game()
    print('Final FEN:', final_board.fen())
