"""Quick check: how many snippets does the existing fuzz cover, and how
many diverge?"""
import json, pathlib

FUZZ = pathlib.Path(__file__).resolve().parent.parent / 'fuzz' / 'results'
recomp = [json.loads(l) for l in (FUZZ / 'recomp_final.jsonl').read_text().splitlines()]
oracle = [json.loads(l) for l in (FUZZ / 'oracle_final.jsonl').read_text().splitlines()]
print(f"recomp results: {len(recomp)}")
print(f"oracle results: {len(oracle)}")
ora_by_id = {r['id']: r for r in oracle if 'id' in r}
mismatches = 0
checked = 0
for r in recomp:
    if 'id' not in r:
        continue
    o = ora_by_id.get(r['id'])
    if not o:
        continue
    checked += 1
    if r.get('A') != o.get('A') or r.get('X') != o.get('X') or r.get('Y') != o.get('Y'):
        mismatches += 1
print(f"checked {checked} snippets, {mismatches} mismatched on A/X/Y")
