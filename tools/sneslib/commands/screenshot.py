"""Screenshot commands — snap, capture."""
import os
import sys
from sneslib.formatting import error


def run(args):
    cmd = args.command
    if not cmd:
        error('No screenshot subcommand. Try: snes.py screenshot --help')

    if cmd == 'snap':
        _snap(args)
    elif cmd == 'capture':
        _capture(args)
    else:
        error(f'Unknown screenshot command: {cmd}')


def _get_mode_and_port(args):
    mode = 'oracle' if args.target == 'oracle' else 'recomp'
    port = 4378 if mode == 'oracle' else 4377
    return mode, port


def _snap(args):
    try:
        from screenshot import find_smw_window, capture_window
    except ImportError:
        error('screenshot.py not found in current directory')

    mode, port = _get_mode_and_port(args)
    default_dir = f'screenshots/{mode}'
    os.makedirs(default_dir, exist_ok=True)
    filename = args.filename or f'{default_dir}/snap.png'

    os.makedirs(os.path.dirname(filename) or '.', exist_ok=True)
    hwnd = find_smw_window(port=port)
    if not hwnd:
        error(f'No SMW window found for {mode} (port {port})')
    img = capture_window(hwnd)
    if not img:
        error('Failed to capture window')
    img.save(filename)
    print(f'Saved {filename} ({mode})')


def _capture(args):
    import time
    try:
        from screenshot import find_smw_window, capture_window, get_frame
    except ImportError:
        error('screenshot.py not found in current directory')

    mode, port = _get_mode_and_port(args)
    out_dir = f'screenshots/{mode}'
    os.makedirs(out_dir, exist_ok=True)

    hwnd = find_smw_window(port=port)
    if not hwnd:
        error(f'No SMW window found for {mode} (port {port})')

    print(f'Capturing {args.count} screenshots from {mode} (port {port})...')
    for i in range(args.count):
        frame, func = get_frame(port)
        img = capture_window(hwnd)
        if img:
            path = f'{out_dir}/smw_screenshot_{i}.png'
            img.save(path)
            print(f'  [{i}] frame={frame} func={func} -> {path}')
        if i < args.count - 1:
            time.sleep(args.delay)
