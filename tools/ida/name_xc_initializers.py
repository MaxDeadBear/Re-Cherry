"""Second half of name_leftovers.py: functions it called Lib_unref_ADDR whose
address sits in a .data/.rdata pointer table that IDA left as raw bytes,
mostly the CRT initializer tables (__xc_a .. __xc_z).

Each such table entry becomes a dword offset (so the reference shows up in
xrefs and the decompiler), and the function is renamed:
  StaticInit_<global>  when it takes no arguments and stores to a global in a
                       writable segment (the global it builds), like the
                       StaticInit_ names of earlier passes;
  Lib_callback_ADDR    otherwise (only referenced from data).
Renames are appended to the same log as name_leftovers.py.
"""
import json
import re
import struct

import ida_bytes
import ida_funcs
import ida_name
import ida_segment
import ida_xref
import idautils
import idc

LOG = r"F:\Re-Cherry\assets\rename_log_2026-10-16.json"
WRITABLE = {".data", "BINKDATA", "RADDATA", "BINKBSS"}
STORES = ("stw", "stb", "sth", "std", "stfs", "stfd", "stvx", "stvx128", "stwu", "stmw")

with open(LOG, encoding="utf-8") as fh:
    log = json.load(fh)

unref = {}
for f in idautils.Functions():
    n = ida_name.get_name(f)
    m = re.match(r"([A-Za-z0-9]+)_unref_([0-9A-F]{8})$", n)
    if m:
        unref[f] = m.group(1)

hits = {}
for sn in (".rdata", ".data"):
    sg = ida_segment.get_segm_by_name(sn)
    raw = ida_bytes.get_bytes(sg.start_ea, sg.end_ea - sg.start_ea) or b""
    for i in range(0, len(raw) - 3, 4):
        v = struct.unpack_from(">I", raw, i)[0]
        if v in unref:
            hits.setdefault(v, []).append(sg.start_ea + i)

made_offsets = 0
for v, locs in hits.items():
    for a in locs:
        flags = ida_bytes.get_flags(a)
        head = ida_bytes.get_item_head(a)
        if head != a and not ida_bytes.is_unknown(flags):
            continue  # inside a larger defined item: leave it
        if ida_bytes.is_unknown(flags) or ida_bytes.is_byte(flags):
            ida_bytes.del_items(a, 0, 4)
            ida_bytes.create_dword(a, 4)
        if ida_bytes.is_dword(ida_bytes.get_flags(a)):
            idc.op_plain_offset(a, 0, 0)
            ida_xref.add_dref(a, v, ida_xref.dr_O)
            made_offsets += 1


def written_global(f):
    """The global a no-argument initializer builds, or None."""
    regs = {}
    for h in idautils.FuncItems(f.start_ea):
        m = idc.print_insn_mnem(h)
        op0, op1 = idc.print_operand(h, 0), idc.print_operand(h, 1)
        drefs = list(idautils.DataRefsFrom(h))
        if m == "addi" and drefs:
            regs[op0] = drefs[0]
            continue
        if m in STORES or m.startswith("stvx"):
            if "@l(" in op1 and drefs:
                g = drefs[0]
            else:
                base = re.search(r"r(\d+)\)?$", idc.print_operand(h, 2) or op1)
                g = regs.get(f"r{base.group(1)}") if base else None
            if g and ida_segment.get_segm_name(ida_segment.getseg(g) or ida_segment.getseg(0)) in WRITABLE:
                return g
    return None


renamed = 0
for v in hits:
    f = ida_funcs.get_func(v)
    old = ida_name.get_name(v)
    g = written_global(f)
    if g is not None:
        gname = ida_name.get_name(g) or f"unk_{g:08X}"
        new = "StaticInit_" + re.sub(r"[^A-Za-z0-9_]", "_", gname)
    else:
        new = f"{unref[v]}_callback_{v:08X}"
    if ida_name.set_name(v, new, ida_name.SN_NOWARN | ida_name.SN_FORCE | ida_name.SN_NOCHECK):
        log.append({"ea": f"0x{v:08X}", "old": old, "new": ida_name.get_name(v)})
        renamed += 1

with open(LOG, "w", encoding="utf-8") as fh:
    json.dump(log, fh, indent=1)
print(json.dumps({"unref": len(unref), "in_tables": len(hits), "offsets": made_offsets, "renamed": renamed,
                  "still_unref": sum(1 for f in idautils.Functions() if "_unref_" in ida_name.get_name(f))}))
