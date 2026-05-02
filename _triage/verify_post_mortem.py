import json, sys
path = "F:/Projects/SuperMarioWorldRecomp/build/last_run_report.json"
with open(path, "r") as f:
    d = json.load(f)
print("valid json:", list(d.keys()))
print("threads count:", len(d["threads"]))
print("reason:", d["reason"])
print("px_tripwire armed:", d["px_tripwire"]["armed"])
print("scoped_tripwire armed:", d["scoped_tripwire"]["armed"])
print("recomp_stack top:", d["recomp_stack"]["top"])
