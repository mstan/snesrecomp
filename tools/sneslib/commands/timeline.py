"""Timeline commands — mode transitions, GFX changes, combined view."""
from sneslib.client import get_client, DualClient
from sneslib.formatting import error


def run(args):
    cmd = args.command
    if not cmd:
        error('No timeline subcommand. Try: snes.py timeline --help')

    if cmd == 'modes':
        _modes(args)
    elif cmd == 'gfx':
        _gfx(args)
    elif cmd == 'full':
        _full(args)
    else:
        error(f'Unknown timeline command: {cmd}')


def _get_frames(args):
    """Get frames from appropriate client(s)."""
    if args.target == 'both':
        dual = DualClient()
        if args.start is not None and args.end is not None:
            start, end = args.start, args.end
        else:
            start, end, rh, oh = dual.get_overlap_range()
            end = min(end, start + 2000)
        r_frames = dual.recomp.get_frame_range(start, end)
        o_frames = dual.oracle.get_frame_range(start, end)
        return r_frames, o_frames, start, end
    else:
        client, _ = get_client(args)
        if args.start is not None and args.end is not None:
            start, end = args.start, args.end
        else:
            h = client.get_history()
            start, end = h['oldest'], min(h['newest'], h['oldest'] + 2000)
        frames = client.get_frame_range(start, end)
        return frames, None, start, end


def _extract_transitions(frames, field):
    """Extract (frame, value) pairs where field changes."""
    transitions = []
    sorted_keys = sorted(frames.keys())
    prev = None
    for f in sorted_keys:
        val = frames[f].get(field, '?')
        if val != prev:
            transitions.append((f, val))
            prev = val
    return transitions


def _modes(args):
    r_frames, o_frames, start, end = _get_frames(args)

    if o_frames is not None:
        r_trans = _extract_transitions(r_frames, 'mode')
        o_trans = _extract_transitions(o_frames, 'mode')
        print(f'Recomp game_mode transitions ({len(r_trans)}):')
        for f, m in r_trans[:40]:
            print(f'  frame {f}: {m}')
        print(f'\nOracle game_mode transitions ({len(o_trans)}):')
        for f, m in o_trans[:40]:
            print(f'  frame {f}: {m}')
    else:
        trans = _extract_transitions(r_frames, 'mode')
        print(f'Game mode transitions ({len(trans)}):')
        for f, m in trans[:50]:
            print(f'  frame {f}: {m}')


def _gfx(args):
    r_frames, o_frames, start, end = _get_frames(args)

    if o_frames is not None:
        r_trans = _extract_transitions(r_frames, 'gfx')
        o_trans = _extract_transitions(o_frames, 'gfx')
        print(f'Recomp GFX transitions ({len(r_trans)}):')
        for f, g in r_trans[:40]:
            print(f'  frame {f}: {g}')
        print(f'\nOracle GFX transitions ({len(o_trans)}):')
        for f, g in o_trans[:40]:
            print(f'  frame {f}: {g}')
    else:
        trans = _extract_transitions(r_frames, 'gfx')
        print(f'GFX transitions ({len(trans)}):')
        for f, g in trans[:50]:
            print(f'  frame {f}: {g}')


def _full(args):
    r_frames, o_frames, start, end = _get_frames(args)

    def _print_combined(frames, label):
        mode_trans = _extract_transitions(frames, 'mode')
        gfx_trans = _extract_transitions(frames, 'gfx')
        # Merge by frame number
        events = [(f, 'mode', v) for f, v in mode_trans] + \
                 [(f, 'gfx', v) for f, v in gfx_trans]
        events.sort(key=lambda x: x[0])
        print(f'{label} timeline ({len(events)} events):')
        for f, kind, v in events[:60]:
            print(f'  frame {f}: {kind}={v}')

    if o_frames is not None:
        _print_combined(r_frames, 'Recomp')
        print()
        _print_combined(o_frames, 'Oracle')
    else:
        _print_combined(r_frames, args.target.capitalize())
