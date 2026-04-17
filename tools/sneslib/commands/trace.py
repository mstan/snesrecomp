"""Trace commands — address tracing, dispatch tracing, step-diff, step-ptr."""
from sneslib.client import get_client, DualClient
from sneslib.formatting import print_json, error


def run(args):
    cmd = args.command
    if not cmd:
        error('No trace subcommand. Try: snes.py trace --help')

    if cmd == 'addr':
        _trace_addr(args)
    elif cmd == 'get':
        _get_trace(args)
    elif cmd == 'range':
        _trace_range(args)
    elif cmd == 'get-range':
        _get_trace_range(args)
    elif cmd == 'dispatch':
        _dispatch(args)
    elif cmd == 'get-dispatch':
        _get_dispatch(args)
    elif cmd == 'objects':
        _objects(args)
    elif cmd == 'step-diff':
        _step_diff(args)
    elif cmd == 'step-ptr':
        _step_ptr(args)
    else:
        error(f'Unknown trace command: {cmd}')


def _trace_addr(args):
    client, _ = get_client(args)
    result = client.query(f'trace_addr {args.hex_addr}')
    print_json(result)


def _get_trace(args):
    client, _ = get_client(args)
    result = client.query('get_trace')
    if args.json:
        print_json(result)
    else:
        print(f'Trace at 0x{result.get("addr", "?")}:')
        for entry in result.get('log', []):
            print(f'  frame {entry["f"]}: 0x{entry["old"]} -> 0x{entry["new"]}  [{entry["func"]}]')


def _trace_range(args):
    client, _ = get_client(args)
    result = client.query(f'trace_range {args.base} {args.len:x}')
    print_json(result)


def _get_trace_range(args):
    client, _ = get_client(args)
    result = client.query('get_trace_range')
    if args.json:
        print_json(result)
    else:
        base = result.get('base', '?')
        print(f'Range trace base=0x{base} len={result.get("len", "?")} entries={result.get("entries", 0)}:')
        for entry in result.get('log', []):
            off = entry.get('off', '?')
            stack = ' <- '.join(entry.get('stack', [])) if entry.get('stack') else ''
            print(f'  f{entry["f"]:>6}  +{off:<2}  0x{entry["old"]}->0x{entry["new"]}  [{entry["func"]}]  {stack}')


def _dispatch(args):
    client, _ = get_client(args)
    result = client.query(f'trace_dispatch {args.frame}')
    print_json(result)


def _get_dispatch(args):
    client, _ = get_client(args)
    result = client.query('get_dispatch_trace')
    if args.json:
        print_json(result)
    else:
        print(f'Dispatch trace for frame {result.get("frame", "?")} ({result.get("count", 0)} entries):')
        for e in result.get('entries', []):
            print(f'  obj={e["obj"]:3d}  func={e["func"]:40s}  ptr={e["ptr"]}  sub={e.get("sub","?")}')


def _objects(args):
    """Query sprite/object state from RAM."""
    client, _ = get_client(args)
    # Sprite table: 12 slots, status at 0x14C8, number at 0x009E
    status = client.query('read_ram 14c8 12')
    numbers = client.query('read_ram 9e 12')
    s_hex = status.get('hex', '').split()
    n_hex = numbers.get('hex', '').split()
    print('Sprite slots:')
    for i in range(min(12, len(s_hex))):
        st = s_hex[i] if i < len(s_hex) else '??'
        num = n_hex[i] if i < len(n_hex) else '??'
        if st != '00':
            print(f'  slot {i:2d}: status=0x{st}  number=0x{num}')


def _step_diff(args):
    """Step N frames, diff RAM after each."""
    dual = DualClient()
    for i in range(args.n):
        dual.both('step')
        import time
        time.sleep(0.2)
        # Read key state
        rr, or_, _, _ = dual.both('read_ram 100 16')
        if rr and or_:
            r_hex = rr.get('hex', '')
            o_hex = or_.get('hex', '')
            match = 'MATCH' if r_hex == o_hex else 'DIFF'
            rf = dual.recomp.query('frame')
            print(f'  Step {i+1}: frame={rf.get("frame","?")}  state={match}')


def _step_ptr(args):
    """Step N frames, trace Map16 pointer after each."""
    client, _ = get_client(args)
    for i in range(args.n):
        client.query('step')
        import time
        time.sleep(0.2)
        result = client.query('read_ptr map16')
        ptr = result.get('ptr_lo_map16_data', {})
        c_val = ptr.get('c_ptr', '?')
        dp_val = ptr.get('dp_bytes', '?')
        match = 'sync' if ptr.get('match') else 'DESYNC'
        f = client.query('frame')
        print(f'  Step {i+1}: frame={f.get("frame","?")}  ptr_c={c_val}  ptr_dp={dp_val}  {match}')
