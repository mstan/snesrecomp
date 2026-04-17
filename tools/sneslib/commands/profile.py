"""Profile commands — on, off, top, poll, functions, latches."""
import time
from sneslib.client import get_client, DebugClient, RECOMP_PORT
from sneslib.formatting import print_json, error


def run(args):
    cmd = args.command
    if not cmd:
        error('No profile subcommand. Try: snes.py profile --help')

    # Profiling is recomp-only
    if args.target == 'oracle':
        error('Profiling is only available on the recomp server.')

    if cmd == 'on':
        _on(args)
    elif cmd == 'off':
        _off(args)
    elif cmd == 'top':
        _top(args)
    elif cmd == 'poll':
        _poll(args)
    elif cmd == 'functions':
        _functions(args)
    elif cmd == 'latches':
        _latches(args)
    else:
        error(f'Unknown profile command: {cmd}')


def _get_recomp(args):
    port = getattr(args, 'port', None) or RECOMP_PORT
    return DebugClient(port, name='Recomp')


def _on(args):
    client = _get_recomp(args)
    result = client.query('profile_on')
    print_json(result)


def _off(args):
    client = _get_recomp(args)
    result = client.query('profile_off')
    print_json(result)


def _top(args):
    client = _get_recomp(args)
    result = client.query('profile')
    if args.json:
        print_json(result)
    else:
        print(f'Frame {result.get("frame_num","?")} ({result.get("frame_ms",0):.1f}ms)')
        print(f'{result.get("funcs",0)} unique functions')
        for entry in result.get('top', []):
            print(f'  {entry["calls"]:6d}  {entry["name"]}')


def _poll(args):
    client = _get_recomp(args)
    # Enable profiling first
    client.query('profile_on')
    print(f'Polling profiler for {args.seconds}s (Ctrl+C to stop)...')
    end_time = time.time() + args.seconds
    while time.time() < end_time:
        try:
            result = client.query('profile')
            frame = result.get('frame_num', '?')
            ms = result.get('frame_ms', 0)
            top = result.get('top', [])
            top_str = ', '.join(f'{e["name"]}({e["calls"]})' for e in top[:5])
            print(f'  frame={frame} {ms:.1f}ms  top: {top_str}')
            time.sleep(1.0)
        except Exception as e:
            print(f'  Error: {e}')
            time.sleep(2.0)


def _functions(args):
    client = _get_recomp(args)
    result = client.query('get_functions')
    if args.json:
        print_json(result)
    else:
        funcs = result.get('functions', [])
        print(f'{len(funcs)} unique functions called (as of frame {result.get("frame","?")}):')
        for f in sorted(funcs):
            print(f'  {f}')


def _latches(args):
    client = _get_recomp(args)
    result = client.query('latches')
    if args.json:
        print_json(result)
    else:
        latches = result.get('latches', [])
        print(f'{result.get("count",0)} latched profiles:')
        for lp in latches:
            print(f'  frame {lp["frame"]}: {lp["ms"]:.0f}ms, {lp["funcs"]} funcs')
            for t in lp.get('top', []):
                print(f'    {t["c"]:6d}  {t["n"]}')
