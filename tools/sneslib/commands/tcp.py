"""TCP direct commands — status, query, read-ram, frame, history, flow control."""
from sneslib.client import get_client, DualClient
from sneslib.formatting import print_json, print_connection_status, print_frame_summary, error


def run(args):
    cmd = args.command
    if not cmd:
        error('No tcp subcommand. Try: snes.py tcp --help')

    if cmd == 'status':
        _status(args)
    elif cmd == 'query':
        _query(args)
    elif cmd == 'read-ram':
        _read_ram(args)
    elif cmd == 'dump-ram':
        _dump_ram(args)
    elif cmd == 'frame':
        _frame(args)
    elif cmd == 'frame-range':
        _frame_range(args)
    elif cmd == 'history':
        _history(args)
    elif cmd in ('pause', 'continue'):
        _flow(args, cmd)
    elif cmd == 'step':
        _flow(args, f'step {args.n}')
    elif cmd == 'run-to':
        _flow(args, f'run_to_frame {args.frame}')
    elif cmd == 'watch':
        _flow(args, f'watch {args.addr}')
    elif cmd == 'unwatch':
        _flow(args, f'unwatch {args.addr}')
    elif cmd == 'loadstate':
        _flow(args, f'loadstate {args.slot}')
    else:
        error(f'Unknown tcp command: {cmd}')


def _status(args):
    dual = DualClient()
    print('Server Status:')
    for name, client in [('Recomp', dual.recomp), ('Oracle', dual.oracle)]:
        try:
            f = client.query('frame')
            h = client.get_history()
            print(f'  {name}: frame={f["frame"]}  func={f.get("func","?")}')
            print(f'    history: {h["count"]} frames, {h["oldest"]}..{h["newest"]}')
        except Exception as e:
            print(f'  {name}: NOT CONNECTED ({e})')


def _query(args):
    client, _ = get_client(args)
    cmd_str = ' '.join(args.cmd)
    try:
        result = client.query(cmd_str)
        print_json(result)
    except Exception:
        # Fall back to raw output
        raw = client.query_raw(cmd_str)
        print(raw)


def _read_ram(args):
    addr = int(args.addr, 16)
    length = args.len
    cmd_str = f'read_ram {addr:x} {length}'

    if args.target == 'both':
        dual = DualClient()
        rr, or_, re, oe = dual.both(cmd_str)
        print('Read RAM comparison:')
        if re:
            print(f'  Recomp: NOT CONNECTED ({re})')
        if oe:
            print(f'  Oracle: NOT CONNECTED ({oe})')
        if rr and or_:
            r_hex = rr.get('hex', '')
            o_hex = or_.get('hex', '')
            r_bytes = r_hex.split()
            o_bytes = o_hex.split()
            diffs = 0
            for i in range(max(len(r_bytes), len(o_bytes))):
                rv = r_bytes[i] if i < len(r_bytes) else '--'
                ov = o_bytes[i] if i < len(o_bytes) else '--'
                if rv != ov:
                    diffs += 1
                    print(f'  0x{addr + i:04X}: R={rv}  O={ov}  <-- DIFF')
            if diffs == 0:
                print(f'  MATCH ({len(r_bytes)} bytes)')
            else:
                print(f'  {diffs} difference(s)')
        elif rr:
            print(f'  Recomp: {rr.get("hex", "")}')
        elif or_:
            print(f'  Oracle: {or_.get("hex", "")}')
    else:
        client, _ = get_client(args)
        result = client.query(cmd_str)
        if args.json:
            print_json(result)
        else:
            print(f'{client.name} RAM 0x{addr:04X} ({length} bytes):')
            print(f'  {result.get("hex", "")}')


def _dump_ram(args):
    addr = int(args.addr, 16)
    client, _ = get_client(args)
    result = client.query(f'dump_ram {addr:x} {args.len}')
    if args.json:
        print_json(result)
    else:
        hex_str = result.get('hex', '')
        # Format as 16-byte rows
        bytes_list = [hex_str[i:i+2] for i in range(0, len(hex_str), 2)]
        for row_start in range(0, len(bytes_list), 16):
            row = bytes_list[row_start:row_start+16]
            row_addr = addr + row_start
            print(f'  {row_addr:04X}: {" ".join(row)}')


def _frame(args):
    client, _ = get_client(args)
    result = client.query(f'get_frame {args.n}')
    if args.json:
        print_json(result)
    else:
        print_frame_summary(result)
        if 'snap' in result:
            print(f'  snap: {result["snap"]}')
        if result.get('diffs'):
            for d in result['diffs']:
                print(f'    diff 0x{d["addr"]}: mine={d["mine"]} theirs={d["theirs"]}')


def _frame_range(args):
    client, _ = get_client(args)
    frames = client.get_frame_range(args.start, args.end)
    if args.json:
        print_json({'frames': list(frames.values())})
    else:
        print(f'{len(frames)} frames ({args.start}..{args.end}):')
        for f_num in sorted(frames.keys()):
            f = frames[f_num]
            status = 'PASS' if f.get('pass') else 'FAIL'
            mode = f.get('mode', '?')
            diffs = f.get('diffs', 0)
            sync = 'sync' if f.get('ptr_sync') else 'DESYNC'
            print(f'  {f_num}: {status}  mode={mode}  diffs={diffs}  {sync}')


def _history(args):
    client, _ = get_client(args)
    h = client.get_history()
    if args.json:
        print_json(h)
    else:
        print(f'Ring buffer: {h["count"]}/{h["capacity"]} frames, {h["oldest"]}..{h["newest"]}')


def _flow(args, cmd_str):
    if args.target == 'both':
        dual = DualClient()
        rr, or_, re, oe = dual.both(cmd_str)
        if rr:
            print(f'  Recomp: {rr}')
        elif re:
            print(f'  Recomp: NOT CONNECTED')
        if or_:
            print(f'  Oracle: {or_}')
        elif oe:
            print(f'  Oracle: NOT CONNECTED')
    else:
        client, _ = get_client(args)
        result = client.query(cmd_str)
        print_json(result)
