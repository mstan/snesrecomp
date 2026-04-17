"""Audit commands — query and update the function audit database."""
import json
import os
from sneslib.formatting import print_json, print_table, error

AUDIT_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))),
                          'audit_status.json')


def _load_db():
    if not os.path.exists(AUDIT_FILE):
        error(f'Audit database not found: {AUDIT_FILE}')
    with open(AUDIT_FILE, 'r') as f:
        return json.load(f)


def _save_db(db):
    with open(AUDIT_FILE, 'w') as f:
        json.dump(db, f, indent=2)


def run(args):
    cmd = args.command
    if not cmd:
        error('No audit subcommand. Try: snes.py audit --help')

    if cmd == 'status':
        _status(args)
    elif cmd == 'unaudited':
        _unaudited(args)
    elif cmd == 'set':
        _set(args)
    elif cmd == 'batch-set':
        _batch_set(args)
    elif cmd == 'summary':
        _summary(args)
    else:
        error(f'Unknown audit command: {cmd}')


def _status(args):
    db = _load_db()
    if args.func:
        # Search for function
        matches = {k: v for k, v in db.items() if args.func.lower() in k.lower()}
        if not matches:
            print(f'No functions matching "{args.func}"')
            return
        for name, info in sorted(matches.items()):
            print(f'{name}:')
            for k, v in info.items():
                print(f'  {k}: {v}')
    else:
        # Show overall status
        total = len(db)
        audited = sum(1 for v in db.values() if v.get('audited'))
        print(f'Total: {total}  Audited: {audited}  Remaining: {total - audited}')


def _unaudited(args):
    db = _load_db()
    unaudited = [(k, v) for k, v in db.items() if not v.get('audited')]
    if args.json:
        print_json([{'name': k, **v} for k, v in unaudited])
    else:
        print(f'{len(unaudited)} unaudited functions:')
        for name, info in sorted(unaudited, key=lambda x: x[1].get('first_frame', 99999)):
            bank = info.get('bank', '?')
            frame = info.get('first_frame', '?')
            print(f'  [{bank}] frame {frame}: {name}')


def _set(args):
    db = _load_db()
    if args.func not in db:
        # Try case-insensitive match
        matches = [k for k in db if k.lower() == args.func.lower()]
        if matches:
            args.func = matches[0]
        else:
            error(f'Function "{args.func}" not in audit database')
    db[args.func]['audited'] = True
    db[args.func]['status'] = args.status_val
    if args.notes:
        db[args.func]['notes'] = args.notes
    _save_db(db)
    print(f'Updated {args.func}: status={args.status_val}')


def _batch_set(args):
    db = _load_db()
    updated = 0
    for func in args.funcs:
        if func in db:
            db[func]['audited'] = True
            db[func]['status'] = args.status_val
            updated += 1
        else:
            print(f'  WARNING: {func} not in database')
    _save_db(db)
    print(f'Updated {updated}/{len(args.funcs)} functions to {args.status_val}')


def _summary(args):
    db = _load_db()
    # Group by bank and status
    by_bank = {}
    by_status = {}
    for name, info in db.items():
        bank = info.get('bank', '??')
        status = info.get('status', 'unaudited') if info.get('audited') else 'unaudited'
        by_bank.setdefault(bank, []).append(status)
        by_status.setdefault(status, 0)
        by_status[status] += 1

    print('By status:')
    for status, count in sorted(by_status.items(), key=lambda x: -x[1]):
        print(f'  {status:20s}: {count}')

    print('\nBy bank:')
    for bank in sorted(by_bank.keys()):
        statuses = by_bank[bank]
        total = len(statuses)
        ok = statuses.count('OK')
        broken = statuses.count('BROKEN')
        unaud = statuses.count('unaudited')
        print(f'  Bank {bank}: {total} total, {ok} OK, {broken} broken, {unaud} unaudited')
