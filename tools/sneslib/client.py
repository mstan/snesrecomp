"""TCP debug server client for SNES recompilation work.

Provides DebugClient (single server) and DualClient (recomp + oracle).
All TCP scripts should use these instead of raw sockets.
"""
import socket
import json
import time

RECOMP_PORT = 4377
ORACLE_PORT = 4378
DEFAULT_TIMEOUT = 5.0
RECV_BUFFER = 262144


class DebugClient:
    """Persistent connection to a single SNES debug server.

    Maintains a single TCP socket across commands. The server sends a
    {"connected":true,...} banner on accept; we consume it once and then
    reuse the socket for all subsequent queries.
    """

    def __init__(self, port, host='127.0.0.1', timeout=DEFAULT_TIMEOUT, name=''):
        self.port = port
        self.host = host
        self.timeout = timeout
        self.name = name or f'port:{port}'
        self._sock = None
        self._buf = b''  # leftover bytes from previous recv

    def _ensure_connected(self):
        """Connect if not already connected, consuming the banner."""
        if self._sock is not None:
            return
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        s.connect((self.host, self.port))
        self._sock = s
        self._buf = b''
        # Consume the {"connected":true,...}\n banner
        self._recv_line()

    def _recv_line(self):
        """Read bytes until a complete \\n-terminated line is available."""
        while b'\n' not in self._buf:
            chunk = self._sock.recv(RECV_BUFFER)
            if not chunk:
                # Server closed
                self.close()
                raise ConnectionError(f'{self.name}: connection closed')
            self._buf += chunk
        line, self._buf = self._buf.split(b'\n', 1)
        return line.decode().strip()

    def close(self):
        """Close the persistent connection."""
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
            self._buf = b''

    def query_raw(self, cmd):
        """Send command, return raw response string."""
        try:
            self._ensure_connected()
            self._sock.sendall((cmd + '\n').encode())
            return self._recv_line()
        except (ConnectionError, socket.timeout, OSError):
            # Connection lost — close and retry once
            self.close()
            self._ensure_connected()
            self._sock.sendall((cmd + '\n').encode())
            return self._recv_line()

    def query(self, cmd):
        """Send command, return parsed JSON dict."""
        raw = self.query_raw(cmd)
        if not raw:
            raise ConnectionError(f'{self.name}: empty response to "{cmd}"')
        return json.loads(raw)

    def is_alive(self):
        """Check if the server is responding."""
        try:
            r = self.query('ping')
            return r.get('ok', False)
        except (ConnectionRefusedError, ConnectionError, json.JSONDecodeError,
                socket.timeout, OSError):
            return False

    def get_frame_range(self, start, end):
        """Fetch frames with automatic chunking (server caps at 500)."""
        all_frames = {}
        chunk_size = 500
        s = start
        while s <= end:
            chunk_end = min(s + chunk_size - 1, end)
            resp = self.query(f'frame_range {s} {chunk_end}')
            for f in resp.get('frames', []):
                all_frames[f['f']] = f
            s = chunk_end + 1
        return all_frames

    def get_history(self):
        """Get ring buffer bounds."""
        return self.query('history')['history']


class DualClient:
    """Manages connections to both recomp and oracle debug servers."""

    def __init__(self, recomp_port=RECOMP_PORT, oracle_port=ORACLE_PORT,
                 timeout=DEFAULT_TIMEOUT):
        self.recomp = DebugClient(recomp_port, timeout=timeout, name='Recomp')
        self.oracle = DebugClient(oracle_port, timeout=timeout, name='Oracle')
        self._offset = None  # frame offset: oracle_frame = recomp_frame + offset

    @property
    def offset(self):
        return self._offset

    def auto_align(self, verbose=True):
        """Compute frame offset from game_mode transitions. Cache result."""
        if self._offset is not None:
            return self._offset

        start, end, rh, oh = self.get_overlap_range()
        end = min(end, start + 2000)
        if start > end:
            raise RuntimeError('No overlapping frames for alignment')

        r_frames = self.recomp.get_frame_range(start, end)
        o_frames = self.oracle.get_frame_range(start, end)

        r_trans = _extract_mode_transitions(r_frames)
        o_trans = _extract_mode_transitions(o_frames)

        offset, matched, total = _find_alignment(r_trans, o_trans)
        self._offset = offset

        if verbose:
            print(f'Aligning... offset = {offset:+d} (oracle = recomp {offset:+d}), '
                  f'validated on {matched}/{total} mode transitions')

        if total > 0 and matched < total * 0.5:
            print('  WARNING: Low match rate. Games may have diverged significantly.')

        return offset

    def aligned_frame(self, recomp_frame):
        """Return corresponding oracle frame number."""
        if self._offset is None:
            self.auto_align()
        return recomp_frame + self._offset

    def both(self, cmd):
        """Query both servers. Returns (recomp_result, oracle_result).
        Either may be None if that server is not connected."""
        r_result = o_result = None
        r_err = o_err = None
        try:
            r_result = self.recomp.query(cmd)
        except (ConnectionRefusedError, ConnectionError, socket.timeout, OSError) as e:
            r_err = str(e)
        try:
            o_result = self.oracle.query(cmd)
        except (ConnectionRefusedError, ConnectionError, socket.timeout, OSError) as e:
            o_err = str(e)
        return r_result, o_result, r_err, o_err

    def both_raw(self, cmd):
        """Query both servers, return raw strings."""
        r_raw = o_raw = None
        r_err = o_err = None
        try:
            r_raw = self.recomp.query_raw(cmd)
        except (ConnectionRefusedError, ConnectionError, socket.timeout, OSError) as e:
            r_err = str(e)
        try:
            o_raw = self.oracle.query_raw(cmd)
        except (ConnectionRefusedError, ConnectionError, socket.timeout, OSError) as e:
            o_err = str(e)
        return r_raw, o_raw, r_err, o_err

    def get_overlap_range(self):
        """Find the overlapping frame range in both ring buffers."""
        rh = self.recomp.get_history()
        oh = self.oracle.get_history()
        start = max(rh['oldest'], oh['oldest'], 0)
        end = min(rh['newest'], oh['newest'])
        return start, end, rh, oh

    def client_for(self, target):
        """Return the client for 'recomp' or 'oracle'."""
        if target == 'oracle':
            return self.oracle
        return self.recomp


def _extract_mode_transitions(frames):
    transitions = []
    prev_mode = None
    for f in sorted(frames.keys()):
        mode = frames[f].get('mode', '?')
        if mode != prev_mode:
            transitions.append((f, mode))
            prev_mode = mode
    return transitions


def _find_alignment(r_trans, o_trans, max_offset=30):
    if not r_trans or not o_trans:
        return 0, 0, 0
    best_offset = 0
    best_score = 0
    for offset in range(-max_offset, max_offset + 1):
        score = 0
        for r_frame, r_mode in r_trans:
            target = r_frame + offset
            for o_frame, o_mode in o_trans:
                if abs(o_frame - target) <= 2 and o_mode == r_mode:
                    score += 1
                    break
        if score > best_score:
            best_score = score
            best_offset = offset
    return best_offset, best_score, len(r_trans)


def get_client(args):
    """Create client(s) from parsed CLI args.
    Returns (client, is_dual) where client is DebugClient or DualClient."""
    target = getattr(args, 'target', 'recomp')
    port = getattr(args, 'port', None)

    if target == 'both':
        return DualClient(), True

    if port:
        return DebugClient(port, name=f'port:{port}'), False

    if target == 'oracle':
        return DebugClient(ORACLE_PORT, name='Oracle'), False

    return DebugClient(RECOMP_PORT, name='Recomp'), False
