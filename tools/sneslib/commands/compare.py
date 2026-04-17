"""Comparison commands — exhaustive cross-server state comparison.

Commands: full, timeseries, cpu, ppu, dma, apu, cgram, oam, vram, apu-ram, ram, scan, tilemap
"""
from sneslib.client import DualClient
from sneslib.formatting import error


def run(args):
    cmd = args.command
    if not cmd:
        error('No compare subcommand. Try: snes.py compare --help')

    dispatch = {
        'full': _full, 'timeseries': _timeseries,
        'cpu': _cpu, 'ppu': _ppu, 'dma': _dma, 'apu': _apu,
        'cgram': _cgram, 'oam': _oam, 'vram': _vram, 'apu-ram': _apu_ram,
        'ram': _ram, 'scan': _scan, 'tilemap': _tilemap,
    }
    fn = dispatch.get(cmd)
    if not fn:
        error(f'Unknown compare command: {cmd}')
    fn(args)


# ---- Helpers ----

def _diff_json(r, o, label, fields):
    """Compare JSON dicts field-by-field. Returns list of (field, r_val, o_val)."""
    diffs = []
    for f in fields:
        rv = r.get(f)
        ov = o.get(f)
        if rv != ov:
            diffs.append((f, rv, ov))
    return diffs


def _diff_hex_blob(r_hex, o_hex):
    """Compare two hex strings byte-by-byte. Returns (total, diff_count, first_diff_offset)."""
    r_len = len(r_hex) // 2
    o_len = len(o_hex) // 2
    total = max(r_len, o_len)
    diff_count = 0
    first_diff = -1
    for i in range(total):
        rb = r_hex[i*2:i*2+2] if i*2+2 <= len(r_hex) else '--'
        ob = o_hex[i*2:i*2+2] if i*2+2 <= len(o_hex) else '--'
        if rb != ob:
            diff_count += 1
            if first_diff < 0:
                first_diff = i
    return total, diff_count, first_diff


def _subsystem_line(name, diffs, total=None, detail=''):
    """Print one subsystem result line."""
    if diffs == 0:
        print(f'  {name:20s} MATCH')
    else:
        extra = f' ({detail})' if detail else ''
        count = f'{diffs} diffs' if total is None else f'{diffs}/{total} differ'
        print(f'  {name:20s} DIFF  {count}{extra}')


# ---- compare full ----

def _full(args):
    dual = DualClient()
    dual.auto_align()

    # Pause both
    try:
        dual.recomp.query('pause')
        dual.oracle.query('pause')
    except Exception:
        pass

    import time; time.sleep(0.3)

    r_frame = dual.recomp.query('frame')
    o_frame = dual.oracle.query('frame')
    r_f = r_frame.get('frame', '?')
    o_f = o_frame.get('frame', '?')

    print(f'Paused: R:{r_f} O:{o_f}')
    print(f'=== Exhaustive State Comparison ===')

    results = {}

    # CPU
    try:
        rc = dual.recomp.query('get_cpu_state')
        oc = dual.oracle.query('get_cpu_state')
        cpu_fields = ['a','x','y','sp','pc','dp','k','db','c','z','v','n','i','d','xf','mf','e']
        diffs = _diff_json(rc, oc, 'CPU', cpu_fields)
        detail = ', '.join(f'{f}: R={rv} O={ov}' for f, rv, ov in diffs[:3])
        _subsystem_line('CPU registers', len(diffs), detail=detail)
        results['cpu'] = len(diffs)
    except Exception as e:
        print(f'  CPU registers:      ERROR ({e})')

    # PPU
    try:
        rp = dual.recomp.query('get_ppu_state')
        op = dual.oracle.query('get_ppu_state')
        ppu_fields = ['inidisp','bgmode','mosaic','obsel','setini','hScroll','vScroll',
                       'screenEnabled','screenWindowed','cgadsub','cgwsel','fixedColor',
                       'vramPointer','vramIncrement','vramRemapMode','cgramPointer','evenFrame']
        diffs = _diff_json(rp, op, 'PPU', ppu_fields)
        detail = ', '.join(f'{f}: R={rv} O={ov}' for f, rv, ov in diffs[:3])
        _subsystem_line('PPU registers', len(diffs), detail=detail)
        results['ppu'] = len(diffs)
    except Exception as e:
        print(f'  PPU registers:      ERROR ({e})')

    # DMA
    try:
        rd = dual.recomp.query('get_dma_state')
        od = dual.oracle.query('get_dma_state')
        dma_diffs = 0
        for ch in range(8):
            rc_ch = rd['channels'][ch]
            oc_ch = od['channels'][ch]
            if rc_ch != oc_ch:
                dma_diffs += 1
        _subsystem_line('DMA channels', dma_diffs, total=8)
        results['dma'] = dma_diffs
    except Exception as e:
        print(f'  DMA channels:       ERROR ({e})')

    # APU
    try:
        ra = dual.recomp.query('get_apu_state')
        oa = dual.oracle.query('get_apu_state')
        apu_diffs = 0
        if ra.get('spc') != oa.get('spc'):
            apu_diffs += 1
        if ra.get('inPorts') != oa.get('inPorts'):
            apu_diffs += 1
        if ra.get('outPorts') != oa.get('outPorts'):
            apu_diffs += 1
        _subsystem_line('APU/SPC', apu_diffs)
        results['apu'] = apu_diffs
    except Exception as e:
        print(f'  APU/SPC:            ERROR ({e})')

    # CGRAM
    try:
        rc = dual.recomp.query('dump_cgram')
        oc = dual.oracle.query('dump_cgram')
        total, diffs, first = _diff_hex_blob(rc['hex'], oc['hex'])
        detail = f'first at byte {first}' if diffs > 0 else ''
        _subsystem_line('CGRAM (palette)', diffs, total=total//2, detail=detail)
        results['cgram'] = diffs
    except Exception as e:
        print(f'  CGRAM (palette):    ERROR ({e})')

    # OAM
    try:
        ro = dual.recomp.query('dump_oam')
        oo = dual.oracle.query('dump_oam')
        total, diffs, first = _diff_hex_blob(ro['hex'], oo['hex'])
        detail = f'first at byte {first}' if diffs > 0 else ''
        _subsystem_line('OAM (sprites)', diffs, total=total, detail=detail)
        results['oam'] = diffs
    except Exception as e:
        print(f'  OAM (sprites):      ERROR ({e})')

    # VRAM (full 64KB)
    try:
        rv = dual.recomp.query('dump_vram 0 65536')
        ov = dual.oracle.query('dump_vram 0 65536')
        total, diffs, first = _diff_hex_blob(rv['hex'], ov['hex'])
        detail = f'first at byte {first}' if diffs > 0 else ''
        _subsystem_line('VRAM', diffs, total=total, detail=detail)
        results['vram'] = diffs
    except Exception as e:
        print(f'  VRAM:               ERROR ({e})')

    # WRAM (first 8KB sample)
    try:
        rw = dual.recomp.query('dump_ram 0 8192')
        ow = dual.oracle.query('dump_ram 0 8192')
        total, diffs, first = _diff_hex_blob(rw['hex'], ow['hex'])
        detail = f'first at 0x{first:04X}' if diffs > 0 else ''
        _subsystem_line('WRAM (first 8KB)', diffs, total=total, detail=detail)
        results['wram'] = diffs
    except Exception as e:
        print(f'  WRAM:               ERROR ({e})')

    # APU RAM
    try:
        ra = dual.recomp.query('dump_apu_ram 0 65536')
        oa = dual.oracle.query('dump_apu_ram 0 65536')
        total, diffs, first = _diff_hex_blob(ra['hex'], oa['hex'])
        detail = f'first at 0x{first:04X}' if diffs > 0 else ''
        _subsystem_line('APU RAM', diffs, total=total, detail=detail)
        results['apu_ram'] = diffs
    except Exception as e:
        print(f'  APU RAM:            ERROR ({e})')

    # Resume
    try:
        dual.recomp.query('continue')
        dual.oracle.query('continue')
    except Exception:
        pass

    total_diffs = sum(v for v in results.values() if isinstance(v, int))
    print(f'\nTotal: {total_diffs} differences across {len(results)} subsystems')


# ---- compare timeseries ----

def _timeseries(args):
    dual = DualClient()
    offset = dual.auto_align()
    start, end = args.start, args.end

    print(f'=== Timeseries Comparison: frames {start}-{end} (offset: {offset:+d}) ===')

    # Track first divergence per subsystem
    first_div = {}

    for f in range(start, end + 1):
        o_f = f + offset
        try:
            r = dual.recomp.query(f'get_frame_extended {f}')
            o = dual.oracle.query(f'get_frame_extended {o_f}')
        except Exception:
            continue

        if 'error' in r or 'error' in o:
            continue

        # CPU
        if 'cpu' not in first_div:
            rc, oc = r.get('cpu', {}), o.get('cpu', {})
            for field in ['a','x','y','sp','pc','dp','k','db','flags','e']:
                if rc.get(field) != oc.get(field):
                    first_div['cpu'] = (f, field, rc.get(field), oc.get(field))
                    break

        # PPU
        if 'ppu' not in first_div:
            rp, op = r.get('ppu', {}), o.get('ppu', {})
            for field in ['inidisp','bgmode','mosaic','obsel','setini','screenEnabled',
                          'cgadsub','cgwsel','hScroll','vScroll','fixedColor','vramPointer']:
                if rp.get(field) != op.get(field):
                    first_div['ppu'] = (f, field, rp.get(field), op.get(field))
                    break

        # DMA
        if 'dma' not in first_div:
            rd, od = r.get('dma', []), o.get('dma', [])
            if rd != od:
                for ch in range(min(len(rd), len(od))):
                    if rd[ch] != od[ch]:
                        first_div['dma'] = (f, f'ch{ch}', rd[ch], od[ch])
                        break

        # CGRAM
        if 'cgram' not in first_div:
            rc, oc = r.get('cgram', ''), o.get('cgram', '')
            if rc != oc:
                _, _, first_byte = _diff_hex_blob(rc, oc)
                first_div['cgram'] = (f, f'byte {first_byte}', rc[first_byte*2:first_byte*2+4], oc[first_byte*2:first_byte*2+4])

        # OAM
        if 'oam' not in first_div:
            ro, oo = r.get('oam', ''), o.get('oam', '')
            if ro != oo:
                _, _, first_byte = _diff_hex_blob(ro, oo)
                first_div['oam'] = (f, f'byte {first_byte}', '', '')

        # Zero page
        if 'zeropage' not in first_div:
            rz, oz = r.get('zeropage', ''), o.get('zeropage', '')
            if rz != oz:
                _, _, first_byte = _diff_hex_blob(rz, oz)
                rb = rz[first_byte*2:first_byte*2+2]
                ob = oz[first_byte*2:first_byte*2+2]
                first_div['zeropage'] = (f, f'$00{first_byte:02X}', rb, ob)

        # Early exit if all found
        if len(first_div) >= 6:
            break

    # Print results
    print(f'{"Subsystem":20s}  {"First Divergence":18s}  {"Detail"}')
    print('-' * 70)
    subsystems = ['cpu', 'ppu', 'dma', 'cgram', 'oam', 'zeropage']
    for sub in subsystems:
        if sub in first_div:
            f, field, rv, ov = first_div[sub]
            print(f'{sub:20s}  frame {f:<12d}  {field}: R={rv} O={ov}')
        else:
            print(f'{sub:20s}  (no divergence)')


# ---- Individual subsystem compares ----

def _cpu(args):
    dual = DualClient()
    rc = dual.recomp.query('get_cpu_state')
    oc = dual.oracle.query('get_cpu_state')
    fields = ['a','x','y','sp','pc','dp','k','db','c','z','v','n','i','d','xf','mf','e','func']
    diffs = 0
    for f in fields:
        rv, ov = rc.get(f), oc.get(f)
        if rv != ov:
            diffs += 1
            print(f'  {f:6s}: R={rv}  O={ov}  <-- DIFF')
        else:
            print(f'  {f:6s}: {rv}')
    print(f'\n{"MATCH" if diffs == 0 else f"{diffs} difference(s)"}')


def _ppu(args):
    dual = DualClient()
    rp = dual.recomp.query('get_ppu_state')
    op = dual.oracle.query('get_ppu_state')
    fields = ['inidisp','bgmode','mosaic','obsel','setini','hScroll','vScroll',
              'screenEnabled','screenWindowed','cgadsub','cgwsel','fixedColor',
              'vramPointer','vramIncrement','vramRemapMode','cgramPointer',
              'window1left','window1right','window2left','window2right','evenFrame']
    diffs = 0
    for f in fields:
        rv, ov = rp.get(f), op.get(f)
        if rv != ov:
            diffs += 1
            print(f'  {f:18s}: R={rv}  O={ov}  <-- DIFF')
        else:
            print(f'  {f:18s}: {rv}')
    print(f'\n{"MATCH" if diffs == 0 else f"{diffs} difference(s)"}')


def _dma(args):
    dual = DualClient()
    rd = dual.recomp.query('get_dma_state')
    od = dual.oracle.query('get_dma_state')
    diffs = 0
    for ch in range(8):
        rc = rd['channels'][ch]
        oc = od['channels'][ch]
        if rc == oc:
            active = 'DMA' if rc['dmaActive'] else ('HDMA' if rc['hdmaActive'] else 'idle')
            print(f'  Ch{ch}: {active} MATCH')
        else:
            diffs += 1
            print(f'  Ch{ch}: DIFF')
            for k in rc:
                if rc[k] != oc.get(k):
                    print(f'    {k}: R={rc[k]} O={oc.get(k)}')
    print(f'\n{"MATCH" if diffs == 0 else f"{diffs} channel(s) differ"}')


def _apu(args):
    dual = DualClient()
    ra = dual.recomp.query('get_apu_state')
    oa = dual.oracle.query('get_apu_state')
    from sneslib.formatting import print_json
    diffs = 0
    # SPC regs
    rs, os = ra.get('spc', {}), oa.get('spc', {})
    for f in ['a','x','y','sp','pc','c','z','v','n','i','h','p','b']:
        rv, ov = rs.get(f), os.get(f)
        if rv != ov:
            diffs += 1
            print(f'  spc.{f}: R={rv} O={ov}  <-- DIFF')
    # Ports
    if ra.get('inPorts') != oa.get('inPorts'):
        diffs += 1
        print(f'  inPorts: R={ra["inPorts"]} O={oa["inPorts"]}')
    if ra.get('outPorts') != oa.get('outPorts'):
        diffs += 1
        print(f'  outPorts: R={ra["outPorts"]} O={oa["outPorts"]}')
    print(f'\n{"MATCH" if diffs == 0 else f"{diffs} difference(s)"}')


def _cgram(args):
    dual = DualClient()
    rc = dual.recomp.query('dump_cgram')
    oc = dual.oracle.query('dump_cgram')
    total, diffs, first = _diff_hex_blob(rc['hex'], oc['hex'])
    if diffs == 0:
        print(f'CGRAM MATCH ({total} bytes)')
    else:
        print(f'CGRAM: {diffs}/{total} bytes differ, first at offset {first}')
        # Show first 20 diffs
        shown = 0
        rh, oh = rc['hex'], oc['hex']
        for i in range(total):
            rb = rh[i*2:i*2+2]
            ob = oh[i*2:i*2+2]
            if rb != ob:
                print(f'  [{i:3d}] R={rb} O={ob}')
                shown += 1
                if shown >= 20:
                    print(f'  ... ({diffs - 20} more)')
                    break


def _oam(args):
    dual = DualClient()
    ro = dual.recomp.query('dump_oam')
    oo = dual.oracle.query('dump_oam')
    rh, oh = ro['hex'], oo['hex']
    total, diffs, first = _diff_hex_blob(rh, oh)
    if diffs == 0:
        print(f'OAM MATCH ({total} bytes)')
    else:
        # Parse as 4-byte sprite entries (128 sprites in main table)
        sprite_diffs = 0
        for s in range(128):
            off = s * 8  # 4 bytes = 8 hex chars
            rs = rh[off:off+8]
            os = oh[off:off+8]
            if rs != os:
                sprite_diffs += 1
                if sprite_diffs <= 20:
                    print(f'  Sprite {s:3d}: R={rs} O={os}')
        if sprite_diffs > 20:
            print(f'  ... ({sprite_diffs - 20} more)')
        print(f'{sprite_diffs} sprite(s) differ, {diffs} total byte diffs')


def _vram(args):
    dual = DualClient()
    addr = int(getattr(args, 'addr', '0') or '0', 16)
    length = getattr(args, 'len', 65536) or 65536
    rv = dual.recomp.query(f'dump_vram {addr:x} {length}')
    ov = dual.oracle.query(f'dump_vram {addr:x} {length}')
    total, diffs, first = _diff_hex_blob(rv['hex'], ov['hex'])
    if diffs == 0:
        print(f'VRAM MATCH ({total} bytes at 0x{addr:04X})')
    else:
        print(f'VRAM: {diffs}/{total} bytes differ at 0x{addr:04X}, first at offset {first}')


def _apu_ram(args):
    dual = DualClient()
    addr = int(getattr(args, 'addr', '0') or '0', 16)
    length = getattr(args, 'len', 65536) or 65536
    ra = dual.recomp.query(f'dump_apu_ram {addr:x} {length}')
    oa = dual.oracle.query(f'dump_apu_ram {addr:x} {length}')
    total, diffs, first = _diff_hex_blob(ra['hex'], oa['hex'])
    if diffs == 0:
        print(f'APU RAM MATCH ({total} bytes at 0x{addr:04X})')
    else:
        print(f'APU RAM: {diffs}/{total} bytes differ at 0x{addr:04X}, first at offset {first}')


# ---- Legacy commands (kept for compatibility) ----

def _ram(args):
    dual = DualClient()
    addr = int(args.addr, 16)
    cmd_str = f'read_ram {addr:x} {args.len}'
    rr, or_, re, oe = dual.both(cmd_str)
    if re:
        print(f'Recomp: NOT CONNECTED ({re})')
    if oe:
        print(f'Oracle: NOT CONNECTED ({oe})')
    if not rr or not or_:
        return
    r_bytes = rr.get('hex', '').split()
    o_bytes = or_.get('hex', '').split()
    diffs = 0
    for i in range(max(len(r_bytes), len(o_bytes))):
        rv = r_bytes[i] if i < len(r_bytes) else '--'
        ov = o_bytes[i] if i < len(o_bytes) else '--'
        if rv != ov:
            diffs += 1
            print(f'  0x{addr + i:04X}: R={rv}  O={ov}  <-- DIFF')
    if diffs == 0:
        print(f'MATCH ({len(r_bytes)} bytes at 0x{addr:04X})')
    else:
        print(f'{diffs} difference(s) in {max(len(r_bytes), len(o_bytes))} bytes at 0x{addr:04X}')


def _scan(args):
    dual = DualClient()
    start = int(args.start, 16)
    end = int(args.end, 16)
    length = end - start
    if length <= 0 or length > 0x10000:
        error(f'Invalid range: 0x{start:X}..0x{end:X}')
    cmd = f'dump_ram {start:x} {length}'
    rr, or_, re, oe = dual.both(cmd)
    if re or oe:
        if re: print(f'Recomp: NOT CONNECTED')
        if oe: print(f'Oracle: NOT CONNECTED')
        return
    r_hex = rr.get('hex', '')
    o_hex = or_.get('hex', '')
    r_b = [r_hex[i:i+2] for i in range(0, len(r_hex), 2)]
    o_b = [o_hex[i:i+2] for i in range(0, len(o_hex), 2)]
    for i in range(min(len(r_b), len(o_b))):
        if r_b[i] != o_b[i]:
            print(f'First diff at 0x{start + i:04X}: R={r_b[i]} O={o_b[i]}')
            return
    print(f'No differences in 0x{start:04X}..0x{end:04X}')


def _tilemap(args):
    dual = DualClient()
    cmd = 'dump_ram c800 6912'
    rr, or_, re, oe = dual.both(cmd)
    if re or oe:
        if re: print(f'Recomp: NOT CONNECTED')
        if oe: print(f'Oracle: NOT CONNECTED')
        return
    r_hex = rr.get('hex', '')
    o_hex = or_.get('hex', '')
    if r_hex == o_hex:
        print('Map16 tilemap MATCH')
    else:
        r_b = [r_hex[i:i+2] for i in range(0, len(r_hex), 2)]
        o_b = [o_hex[i:i+2] for i in range(0, len(o_hex), 2)]
        diffs = 0
        for i in range(min(len(r_b), len(o_b))):
            if r_b[i] != o_b[i]:
                diffs += 1
                screen = i // 512
                offset = i % 512
                if diffs <= 50:
                    print(f'  Screen {screen} offset 0x{offset:03X}: R={r_b[i]} O={o_b[i]}')
        if diffs > 50:
            print(f'  ... ({diffs - 50} more)')
        print(f'{diffs} tile difference(s)')
