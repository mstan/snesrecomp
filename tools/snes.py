#!/usr/bin/env python3
"""SNES Recompilation Debug CLI.

Usage:
  python snes.py <group> <command> [args] [flags]

Groups:
  tcp        Direct TCP server commands (status, query, read-ram, frame, ...)
  compare    Cross-server comparison (ram, frames, snap, oam, tilemap, ...)
  timeline   Frame history timelines (modes, gfx, full)
  align      Auto-detect frame offset between recomp and oracle
  trace      Address/dispatch tracing (addr, dispatch, objects, step-diff, ...)
  profile    Function profiling (on, off, top, poll, functions, latches)
  audit      Function audit database (status, unaudited, set, summary)
  screenshot Screenshot capture (snap, capture)

Global flags:
  --recomp   Target recomp server (port 4377, default for most commands)
  --oracle   Target oracle server (port 4378)
  --both     Target both servers (default for compare)
  --json     Output as JSON
  --port N   Override port
  --verbose  Show connection details
"""
import argparse
import sys


def build_parser():
    parser = argparse.ArgumentParser(
        prog='snes',
        description='SNES Recompilation Debug CLI')

    # Global flags
    target = parser.add_mutually_exclusive_group()
    target.add_argument('--recomp', dest='target', action='store_const',
                        const='recomp', help='Target recomp (port 4377)')
    target.add_argument('--oracle', dest='target', action='store_const',
                        const='oracle', help='Target oracle (port 4378)')
    target.add_argument('--both', dest='target', action='store_const',
                        const='both', help='Target both servers')
    parser.set_defaults(target=None)  # Let commands set their own default
    parser.add_argument('--json', action='store_true', help='JSON output')
    parser.add_argument('--port', type=int, help='Override port')
    parser.add_argument('--verbose', action='store_true')

    sub = parser.add_subparsers(dest='group', help='Command group')

    # --- tcp ---
    tcp = sub.add_parser('tcp', help='Direct TCP commands')
    tcp_sub = tcp.add_subparsers(dest='command')

    tcp_sub.add_parser('status', help='Show both servers status')
    q = tcp_sub.add_parser('query', help='Send raw command')
    q.add_argument('cmd', nargs='+', help='Command and args')
    rr = tcp_sub.add_parser('read-ram', help='Read RAM')
    rr.add_argument('addr', help='Hex address')
    rr.add_argument('len', type=int, help='Length in bytes')
    dr = tcp_sub.add_parser('dump-ram', help='Large RAM dump')
    dr.add_argument('addr', help='Hex address')
    dr.add_argument('len', type=int, help='Length in bytes')
    fr = tcp_sub.add_parser('frame', help='Get frame from ring buffer')
    fr.add_argument('n', type=int, help='Frame number')
    frr = tcp_sub.add_parser('frame-range', help='Get frame range')
    frr.add_argument('start', type=int)
    frr.add_argument('end', type=int)
    tcp_sub.add_parser('history', help='Ring buffer bounds')
    tcp_sub.add_parser('pause', help='Pause execution')
    tcp_sub.add_parser('continue', help='Resume execution')
    st = tcp_sub.add_parser('step', help='Step N frames')
    st.add_argument('n', type=int, nargs='?', default=1)
    rt = tcp_sub.add_parser('run-to', help='Run to frame')
    rt.add_argument('frame', type=int)
    w = tcp_sub.add_parser('watch', help='Set watchpoint')
    w.add_argument('addr', help='Hex address')
    uw = tcp_sub.add_parser('unwatch', help='Remove watchpoint')
    uw.add_argument('addr', help='Hex address')
    ls = tcp_sub.add_parser('loadstate', help='Load save state')
    ls.add_argument('slot', type=int, choices=range(10))

    # --- compare ---
    cmp = sub.add_parser('compare', help='Cross-server comparison')
    cmp_sub = cmp.add_subparsers(dest='command')

    # Exhaustive comparison commands
    cmp_sub.add_parser('full', help='Exhaustive live comparison of ALL state')
    cts = cmp_sub.add_parser('timeseries', help='Per-frame ring buffer comparison')
    cts.add_argument('start', type=int)
    cts.add_argument('end', type=int)
    cmp_sub.add_parser('cpu', help='Compare CPU registers')
    cmp_sub.add_parser('ppu', help='Compare PPU registers')
    cmp_sub.add_parser('dma', help='Compare DMA channel state')
    cmp_sub.add_parser('apu', help='Compare APU/SPC state')
    cmp_sub.add_parser('cgram', help='Compare CGRAM (palette)')
    cmp_sub.add_parser('oam', help='Compare OAM (sprites, from PPU)')
    cv = cmp_sub.add_parser('vram', help='Compare VRAM')
    cv.add_argument('addr', nargs='?', default='0', help='Hex start address')
    cv.add_argument('len', type=int, nargs='?', default=65536, help='Length')
    car = cmp_sub.add_parser('apu-ram', help='Compare APU RAM')
    car.add_argument('addr', nargs='?', default='0', help='Hex start address')
    car.add_argument('len', type=int, nargs='?', default=65536, help='Length')
    # Legacy / utility comparison commands
    cr = cmp_sub.add_parser('ram', help='Compare WRAM range')
    cr.add_argument('addr', help='Hex start address')
    cr.add_argument('len', type=int, help='Length in bytes')
    cmp_sub.add_parser('tilemap', help='Compare Map16 tilemap')
    csc = cmp_sub.add_parser('scan', help='Scan RAM for first diff')
    csc.add_argument('start', help='Hex start address')
    csc.add_argument('end', help='Hex end address')

    # --- timeline ---
    tl = sub.add_parser('timeline', help='Frame history timelines')
    tl_sub = tl.add_subparsers(dest='command')
    for name in ('modes', 'gfx', 'full'):
        t = tl_sub.add_parser(name)
        t.add_argument('start', type=int, nargs='?')
        t.add_argument('end', type=int, nargs='?')

    # --- align ---
    al = sub.add_parser('align', help='Auto-detect frame offset')
    al.add_argument('start', type=int, nargs='?')
    al.add_argument('end', type=int, nargs='?')

    # --- trace ---
    tr = sub.add_parser('trace', help='Address/dispatch tracing')
    tr_sub = tr.add_subparsers(dest='command')
    ta = tr_sub.add_parser('addr', help='Start address trace')
    ta.add_argument('hex_addr', help='Hex address')
    tr_sub.add_parser('get', help='Get trace log')
    trg = tr_sub.add_parser('range', help='Start range trace (base + len bytes, up to 16)')
    trg.add_argument('base', help='Hex base address')
    trg.add_argument('len', type=lambda s: int(s, 0), help='Length in bytes (1..16)')
    tr_sub.add_parser('get-range', help='Get range trace log')
    td = tr_sub.add_parser('dispatch', help='Enable dispatch tracing for frame')
    td.add_argument('frame', type=int)
    tr_sub.add_parser('get-dispatch', help='Get dispatch trace')
    tr_sub.add_parser('objects', help='Query object state')
    tsd = tr_sub.add_parser('step-diff', help='Step and diff')
    tsd.add_argument('n', type=int, nargs='?', default=1)
    tsp = tr_sub.add_parser('step-ptr', help='Step and trace Map16 ptr')
    tsp.add_argument('n', type=int, nargs='?', default=1)

    # --- profile ---
    pr = sub.add_parser('profile', help='Function profiling')
    pr_sub = pr.add_subparsers(dest='command')
    pr_sub.add_parser('on', help='Enable profiling')
    pr_sub.add_parser('off', help='Disable profiling')
    pr_sub.add_parser('top', help='Show top functions')
    pp = pr_sub.add_parser('poll', help='Continuous polling')
    pp.add_argument('seconds', type=float, nargs='?', default=60.0)
    pr_sub.add_parser('functions', help='List all called functions')
    pr_sub.add_parser('latches', help='Show watchdog latches')

    # --- audit ---
    au = sub.add_parser('audit', help='Function audit database')
    au_sub = au.add_subparsers(dest='command')
    aus = au_sub.add_parser('status', help='Show function status')
    aus.add_argument('func', nargs='?', help='Function name')
    au_sub.add_parser('unaudited', help='List unaudited functions')
    auset = au_sub.add_parser('set', help='Update function status')
    auset.add_argument('func', help='Function name')
    auset.add_argument('status_val', help='Status (OK, BROKEN, etc.)')
    auset.add_argument('notes', nargs='?', default='', help='Notes')
    aubs = au_sub.add_parser('batch-set', help='Batch update status')
    aubs.add_argument('status_val', help='Status')
    aubs.add_argument('funcs', nargs='+', help='Function names')
    au_sub.add_parser('summary', help='Audit stats by bank/status')

    # --- screenshot ---
    ss = sub.add_parser('screenshot', help='Screenshot capture')
    ss_sub = ss.add_subparsers(dest='command')
    ssnap = ss_sub.add_parser('snap', help='Single screenshot')
    ssnap.add_argument('filename', nargs='?', help='Output filename')
    scap = ss_sub.add_parser('capture', help='Multiple screenshots')
    scap.add_argument('count', type=int, nargs='?', default=5)
    scap.add_argument('delay', type=float, nargs='?', default=3.0)

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    if not args.group:
        parser.print_help()
        return

    # Set default target per command group
    if args.target is None:
        if args.group in ('compare', 'align', 'timeline'):
            args.target = 'both'
        else:
            args.target = 'recomp'

    # Dispatch to command module
    try:
        if args.group == 'tcp':
            from sneslib.commands.tcp import run
        elif args.group == 'compare':
            from sneslib.commands.compare import run
        elif args.group == 'timeline':
            from sneslib.commands.timeline import run
        elif args.group == 'align':
            from sneslib.commands.align import run
        elif args.group == 'trace':
            from sneslib.commands.trace import run
        elif args.group == 'profile':
            from sneslib.commands.profile import run
        elif args.group == 'audit':
            from sneslib.commands.audit import run
        elif args.group == 'screenshot':
            from sneslib.commands.screenshot import run
        else:
            parser.print_help()
            return

        run(args)

    except ConnectionRefusedError:
        print(f'ERROR: Could not connect to server', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
