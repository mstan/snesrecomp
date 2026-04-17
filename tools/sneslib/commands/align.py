"""Frame alignment — detect frame offset between recomp and oracle."""
from sneslib.client import DualClient
from sneslib.formatting import error


def run(args):
    dual = DualClient()

    try:
        rh = dual.recomp.get_history()
    except Exception:
        error('Recomp (port 4377) not running')
    try:
        oh = dual.oracle.get_history()
    except Exception:
        error('Oracle (port 4378) not running')

    print(f'Recomp: frames {rh["oldest"]}..{rh["newest"]} ({rh["count"]} recorded)')
    print(f'Oracle: frames {oh["oldest"]}..{oh["newest"]} ({oh["count"]} recorded)')

    if args.start is not None and args.end is not None:
        start, end = args.start, args.end
    else:
        start = max(rh['oldest'], oh['oldest'], 0)
        end = min(rh['newest'], oh['newest'])
        end = min(end, start + 2000)

    if start > end:
        print('No overlapping frames.')
        return

    print(f'Querying frames {start}..{end}...')
    r_frames = dual.recomp.get_frame_range(start, end)
    o_frames = dual.oracle.get_frame_range(start, end)
    print(f'Got {len(r_frames)} recomp, {len(o_frames)} oracle frames')

    r_trans = _extract_mode_transitions(r_frames)
    o_trans = _extract_mode_transitions(o_frames)

    print(f'\nRecomp game_mode transitions ({len(r_trans)}):')
    for f, m in r_trans[:30]:
        print(f'  frame {f}: mode={m}')
    print(f'\nOracle game_mode transitions ({len(o_trans)}):')
    for f, m in o_trans[:30]:
        print(f'  frame {f}: mode={m}')

    offset, matched, total = _find_alignment(r_trans, o_trans)

    print(f'\n{"=" * 50}')
    print(f'FRAME OFFSET: {offset:+d}')
    print(f'  oracle_frame = recomp_frame {offset:+d}')
    print(f'  Matched {matched}/{total} transitions (tolerance: +/-2 frames)')

    if offset == 0:
        print('  Frames are aligned.')
    else:
        print(f'  Example: recomp frame 100 ~ oracle frame {100 + offset}')

    if total > 0 and matched < total * 0.5:
        print('  WARNING: Low match rate. Games may have diverged significantly.')


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
