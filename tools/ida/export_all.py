"""Decompile every function in the open IDA database into decomp/, one .c
file per function, resumably.

Run inside IDA (the IDA MCP server's py_exec_file) repeatedly; each run skips
functions already on disk and stops after BUDGET seconds, because the server
aborts a call after 60 s. tools/ida/export_all_loop.py drives it until it
reports nothing left. Functions that fail to decompile get a stub recording
the error, so they are not retried. A function whose decompilation outlives
the server's timeout leaves its .pending marker behind; the next run turns
that into a stub instead of trying again.

Each run appends one JSON line per exported function to index.jsonl
(address, name, size, callers, callees, decompiled); when nothing is left the
lines are merged into index.json.

The output is the game's code: decomp/ is in .gitignore and must stay out of
the repository.
"""
import json
import os
import time

import ida_funcs
import ida_hexrays
import ida_lines
import ida_name
import idautils
import idc

OUT = r"F:\Re-Cherry\decomp"
BUDGET = 35

# Several IDA instances can be open at once (each on its own MCP port): never
# write another game's database into this folder.
if os.path.normcase(os.path.abspath(idc.get_idb_path())) != os.path.normcase(r"F:\Re-Cherry\assets\default.xex.i64"):
    # RuntimeError, not SystemExit: SystemExit is not caught by the MCP plugin
    # and leaves its server hung.
    raise RuntimeError(f"export_all.py: open database is {idc.get_idb_path()}, not Re-Cherry's")

os.makedirs(OUT, exist_ok=True)
start = time.time()
index_path = os.path.join(OUT, "index.jsonl")
done_now = failed_now = remaining = 0


def refs(ea):
    callers = sorted({f"0x{f.start_ea:08X}" for f in (ida_funcs.get_func(x) for x in idautils.CodeRefsTo(ea, 0)) if f})
    callees = set()
    for item in idautils.FuncItems(ea):
        for x in idautils.CodeRefsFrom(item, 0):
            f = ida_funcs.get_func(x)
            if f and f.start_ea != ea:
                callees.add(f"0x{f.start_ea:08X}")
    return callers, sorted(callees)


# Functions a timed-out run left pending go first, so their retry gets the
# whole call; a second timeout is final.
retry = [int(n[:8], 16) for n in os.listdir(OUT) if n.endswith(".c.pending")]
order = retry + [ea for ea in idautils.Functions() if ea not in set(retry)]

with open(index_path, "a", encoding="utf-8") as index:
    for ea in order:
        path = os.path.join(OUT, f"{ea:08X}.c")
        pending = path + ".pending"
        if os.path.exists(path):
            continue
        if time.time() - start > BUDGET:
            remaining += 1
            continue
        func = ida_funcs.get_func(ea)
        name = ida_name.get_name(ea) or f"sub_{ea:08X}"
        callers, callees = refs(ea)
        attempts = int(open(pending).read() or 0) if os.path.exists(pending) else 0
        if attempts >= 2:
            text, ok = "// decompilation failed: longer than the IDA MCP server's 60 s call limit", False
            os.remove(pending)
        else:
            with open(pending, "w") as f:
                f.write(str(attempts + 1))
            try:
                cfunc = ida_hexrays.decompile(ea)
                text = "\n".join(ida_lines.tag_remove(line.line) for line in cfunc.get_pseudocode())
                ok = True
            except ida_hexrays.DecompilationFailure as e:
                text, ok = f"// decompilation failed: {e}", False
            os.remove(pending)
        done_now += ok
        failed_now += not ok
        header = (f"// {name} @ 0x{ea:08X}, {func.end_ea - func.start_ea} bytes\n"
                  f"// callers: {', '.join(callers) if callers else '(none: indirect or entry)'}\n\n")
        with open(path, "w", encoding="utf-8") as f:
            f.write(header + text + "\n")
        index.write(json.dumps({"address": f"0x{ea:08X}", "name": name, "size": func.end_ea - func.start_ea,
                                "callers": callers, "callees": callees, "decompiled": ok}) + "\n")

if remaining == 0:
    entries = {}
    with open(index_path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                e = json.loads(line)
                entries[e["address"]] = e  # a later line for the same address wins
    with open(os.path.join(OUT, "index.json"), "w", encoding="utf-8") as f:
        json.dump(sorted(entries.values(), key=lambda e: e["address"]), f, indent=1)

print(json.dumps({"decompiled": done_now, "failed": failed_now, "remaining": remaining,
                  "seconds": round(time.time() - start, 1)}))
