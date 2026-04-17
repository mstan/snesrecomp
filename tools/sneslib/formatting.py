"""Output formatting helpers for the SNES debug CLI."""
import json
import sys


def print_json(obj):
    """Pretty-print a JSON object."""
    print(json.dumps(obj, indent=2))


def print_hex_diff(addr, recomp_hex, oracle_hex, label_r='Recomp', label_o='Oracle'):
    """Print a side-by-side hex diff."""
    r_bytes = recomp_hex.split() if recomp_hex else []
    o_bytes = oracle_hex.split() if oracle_hex else []
    max_len = max(len(r_bytes), len(o_bytes))

    diffs = 0
    for i in range(max_len):
        rv = r_bytes[i] if i < len(r_bytes) else '--'
        ov = o_bytes[i] if i < len(o_bytes) else '--'
        if rv != ov:
            diffs += 1
            marker = ' <-- DIFF'
        else:
            marker = ''
        print(f'  0x{addr + i:04X}: {label_r}={rv}  {label_o}={ov}{marker}')

    if diffs == 0:
        print(f'  MATCH ({max_len} bytes)')
    else:
        print(f'  {diffs} difference(s) in {max_len} bytes')
    return diffs


def print_frame_summary(frame_data):
    """Print a single frame record summary."""
    f = frame_data
    status = 'PASS' if f.get('pass') else 'FAIL'
    func = f.get('func', '?')
    mode = f.get('game_mode', f.get('mode', '?'))
    diffs = f.get('diff_count', f.get('diffs', 0))
    print(f"  Frame {f.get('frame', f.get('f', '?'))}: {status}  mode={mode}  diffs={diffs}  func={func}")


def print_connection_status(name, result, err):
    """Print connection status for a server."""
    if err:
        print(f'  {name}: NOT CONNECTED ({err})')
    else:
        frame = result.get('frame', '?')
        func = result.get('func', '?')
        print(f'  {name}: frame={frame}  func={func}')


def format_snap_bytes(snap_hex):
    """Parse snap hex string into labeled values."""
    if not snap_hex:
        return {}
    parts = snap_hex.split()
    labels = {
        21: 'game_mode', 22: 'gfx_00', 23: 'gfx_01', 24: 'gfx_02',
        25: 'gfx_03', 26: 'gfx_04', 27: 'gfx_05', 28: 'gfx_06', 29: 'gfx_07',
        32: 'scroll_x_lo', 33: 'scroll_y_lo', 34: 'scroll_x_hi', 35: 'scroll_y_hi',
        40: 'translevel', 41: 'overworld', 43: 'level_num',
        46: 'player_state', 47: 'player_pose',
        57: 'obj_number',
    }
    result = {}
    for idx, label in labels.items():
        if idx < len(parts):
            result[label] = parts[idx]
    return result


def print_table(headers, rows, col_widths=None):
    """Print a simple text table."""
    if not col_widths:
        col_widths = [max(len(str(h)), max((len(str(r[i])) for r in rows), default=0))
                      for i, h in enumerate(headers)]
    # Header
    hdr = '  '.join(str(h).ljust(w) for h, w in zip(headers, col_widths))
    print(hdr)
    print('-' * len(hdr))
    # Rows
    for row in rows:
        line = '  '.join(str(row[i]).ljust(w) for i, w in enumerate(col_widths))
        print(line)


def error(msg):
    """Print error and exit."""
    print(f'ERROR: {msg}', file=sys.stderr)
    sys.exit(1)
