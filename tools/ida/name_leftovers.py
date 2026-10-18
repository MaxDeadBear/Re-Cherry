"""Name the functions earlier naming passes left as sub_/nullsub_, plus the
callbacks of every shader type registration.

Run inside IDA (the IDA MCP server's py_exec_file) until it prints
"done": each run stops after BUDGET seconds and keeps its progress in
builtins, so repeated calls continue where the last one stopped. Every rename
is appended to LOG (old and new name per address) so the pass can be undone.

Tiers, in order (see docs/reverse-engineering/ida-names.md):
  1. MANUAL: functions identified by reading them (CRT, libjpeg in D3DX).
  2. Shader types: every caller of FShaderType::FShaderType (0x823052D0)
     passes the type name, so the initializer, its atexit destructor and its
     ConstructSerializedInstance / ModifyCompilationEnvironment /
     ConstructCompiledInstance / ShouldCache callbacks are named after it.
     Callbacks shared by several types become Shared::Callback_ADDR with the
     type list in the function comment.
  3. GNatives registration initializers (inlined GRegisterNative).
  4. Instruction shape (GetField_, VCallThunk_, ReturnConst_, Nop_, j_, ...).
  5. StaticDtor_<global> for "r3 = &global; b destructor" thunks.
  6. Lib_sub_/Lib_callback_/Lib_unref_ADDR from the nearest library-tier
     neighbours ("unref": nothing references it; dead code kept by the linker).
"""
import builtins
import collections
import json
import os
import re
import time

import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_segment
import idautils
import idc

BUDGET = 40
LOG = r"F:\Re-Cherry\assets\rename_log_2026-10-16.json"
FSHADERTYPE_CTOR = 0x823052D0
GNATIVES = 0x832E7168

MANUAL = {
    0x82BCA1C8: "jpeg_start_pass_upsample",       # jdsample.c: next_row_out = max_v_samp_factor, rows_to_go = output_height
    0x82BCA318: "jpeg_fullsize_upsample",         # *output_data_ptr = input_data
    0x82BCA320: "jpeg_noop_upsample",             # *output_data_ptr = NULL
    0x82BCA408: "jpeg_h2v1_upsample",             # duplicates every sample horizontally
    0x82BCACE0: "jpeg_grayscale_convert",         # jdcolor.c: jcopy_sample_rows(input_buf[0], ...)
    0x82BCB9A8: "jpeg_start_pass_merged_upsample",  # jdmerge.c: spare_full = FALSE, rows_to_go = output_height
    0x82BE0040: "CRT_cropzeros_l",                # strips trailing zeros after the locale decimal point
    0x82BE0180: "CRT_cropzeros",                  # _cropzeros_l(buf, NULL)
    0x82BE00F0: "CRT_positive",                   # *d >= 0.0
    0x82BE0C90: "CRT_cfltcvt_l",                  # e/E, f, a/A, else g
    0x82BE0CE0: "CRT_cfltcvt",                    # _cfltcvt_l with locale NULL
    0x82BD63E0: "CRT_chkstk_r3",                  # r12 = -r3, falls into __chkstk
    0x82BD63E4: "CRT_chkstk",                     # probes the stack one 4 KB page at a time down to r1 + r12
    0x82217C58: "TruncFloatDifference",           # (int)(*a - *b)
    0x8257DCA8: "CompareFloat0x20ThenPtr0x48",    # sort predicate: float +0x20 ascending, then non-null +0x48 first
}

LIBS = ("UE3", "PhysX", "GFx", "XDK", "XAPI", "D3DX", "XAudio2", "D3D", "CRT", "XOnline")
LOCATION_ONLY = re.compile(r"^(sub_|nullsub_|(%s)_(sub|callback|unref)_[0-9A-F]{8}$)" % "|".join(LIBS))
UNNAMED = re.compile(r"^(sub_[0-9A-F]{8}|nullsub_\d+)$")

S = getattr(builtins, "_rc_name_state", None)
if S is None:
    S = builtins._rc_name_state = {"stage": "manual", "i": 0, "log": [], "shared": collections.defaultdict(set),
                                   "cb_owner": {}}
start = time.time()


def name(ea):
    return ida_name.get_name(ea) or ""


def rename(ea, new, comment=None):
    old = name(ea)
    if old == new:
        return
    if ida_name.set_name(ea, new, ida_name.SN_NOWARN | ida_name.SN_FORCE | ida_name.SN_NOCHECK):
        S["log"].append({"ea": f"0x{ea:08X}", "old": old, "new": name(ea)})
    if comment:
        f = ida_funcs.get_func(ea)
        if f:
            ida_funcs.set_func_cmt(f, comment, False)


def clean(s):
    s = re.sub(r"[^A-Za-z0-9_]+", "_", s.replace("::", "__")).strip("_")
    return s


def wstr(ea):
    out = []
    for i in range(256):
        c = ida_bytes.get_wide_word(ea + 2 * i)
        if c == 0:
            return "".join(out)
        if c > 0x7E:
            return None
        out.append(chr(c))
    return None


def over_budget():
    return time.time() - start > BUDGET


# ---------------------------------------------------------------- stages
def stage_manual():
    for ea, n in MANUAL.items():
        if ida_funcs.get_func(ea) and LOCATION_ONLY.match(name(ea)) or re.search(r"::sub_[0-9A-F]{8}$", name(ea)):
            rename(ea, n)
    S["shader_callers"] = sorted({ida_funcs.get_func(x.frm).start_ea for x in idautils.XrefsTo(FSHADERTYPE_CTOR)
                                  if ida_funcs.get_func(x.frm)})
    return True


ARG = r"\(int\)(?:&)?([A-Za-z_][\w:]*)"


def stage_shaders():
    callers = S["shader_callers"]
    while S["i"] < len(callers):
        if over_budget():
            return False
        ea = callers[S["i"]]
        S["i"] += 1
        try:
            text = str(ida_hexrays.decompile(ea))
        except Exception:  # noqa: BLE001
            continue
        m = re.search(r"UE3_sub_823052D0\(\s*(\w+),\s*\(int\)&(\w+),", text)
        if not m:
            continue
        type_name = wstr(idc.get_name_ea_simple(m.group(2)))
        if not type_name:
            continue
        t = clean(type_name.replace("<", "_").replace(">", ""))
        call = text[m.start():text.find(");", m.start())]
        args = re.findall(r"\(int\)([A-Za-z_][\w:]*)", call)
        stores = re.findall(r"\n  \w+ = \(int\)([A-Za-z_][\w:]*);", text)
        dtor = re.search(r"atexit\(\(void \(__cdecl \*\)\(\)\)(\w+)\)", text)
        cbs = {}
        if len(args) >= 2:
            cbs["ConstructSerializedInstance"] = args[0]
            cbs["ModifyCompilationEnvironment"] = args[-1]
        if len(stores) >= 2:
            cbs["ConstructCompiledInstance"] = stores[0]
            cbs["ShouldCache"] = stores[1]
        if LOCATION_ONLY.match(name(ea)) or re.match(r"StaticInit_dword_[0-9A-F]{8}$", name(ea)):
            rename(ea, f"StaticInitShaderType_{t}")
        if dtor:
            d = idc.get_name_ea_simple(dtor.group(1))
            if d != idc.BADADDR and ida_funcs.get_func(d) and LOCATION_ONLY.match(name(d)):
                rename(d, f"StaticDtor_ShaderType_{t}")
        for role, sym in cbs.items():
            cea = idc.get_name_ea_simple(sym)
            if cea == idc.BADADDR or not ida_funcs.get_func(cea):
                continue
            S["shared"][(cea, role)].add(type_name)
    return True


def stage_shader_callbacks():
    for (cea, role), types in sorted(S["shared"].items()):
        n = name(cea)
        if n.startswith("nullsub_"):
            continue  # the shared empty function: thousands of other references
        if not (LOCATION_ONLY.match(n) or re.match(r"StaticInit_dword_[0-9A-F]{8}_helper\d*_[0-9A-F]{8}$", n)):
            continue
        if len({r for (e, r) in S["shared"] if e == cea}) > 1:
            continue  # one body serving different roles: leave it to the shape tiers
        if len(types) == 1:
            rename(cea, f"{clean(next(iter(types)).replace('<', '_').replace('>', ''))}::{role}")
        else:
            rename(cea, f"Shared::{role}_{cea:08X}",
                   f"{role} of {len(types)} shader types: " + ", ".join(sorted(types)))
    return True


def insns(f):
    return [(h, idc.print_insn_mnem(h), idc.print_operand(h, 0), idc.print_operand(h, 1), idc.print_operand(h, 2))
            for h in idautils.FuncItems(f.start_ea)]


def gnatives_index(f):
    """For an inlined GRegisterNative(index, func): the last store into
    GNatives[index] and the stored function, or None."""
    ins = insns(f)
    if not any("GNatives" in op1 for _, m, _, op1, _ in ins):
        return None
    last = None
    for h, m, op0, op1, _ in ins:
        if m == "stw" and "GNatives" not in op1 and re.search(r"- 0x832E7168\)\(r\d+\)", op1):
            mm = re.match(r"\((\w+) - 0x832E7168\)", op1)
            if mm:
                tgt = idc.get_name_ea_simple(mm.group(1))
                last = (tgt - GNATIVES) // 4
    if last is None:
        return None
    for h, m, op0, op1, _ in ins:
        if m == "lwz" and op1.startswith("off_"):
            fn = ida_bytes.get_wide_dword(idc.get_name_ea_simple(op1.split("@")[0]))
            return last, fn
    return last, None


SHAPE_SKIP = ("li", "mr")  # argument setup allowed before a vcall


def shape_name(f):
    ins = insns(f)
    ea = f.start_ea
    ms = [m for _, m, *_ in ins]
    a = f"{ea:08X}"
    if ms == ["blr"]:
        return f"Nop_{a}"
    if len(ins) == 2 and ms[1] == "blr":
        h, m, op0, op1, _ = ins[0]
        if m == "li" and op0 == "r3":
            v = idc.get_operand_value(h, 1) & 0xFFFF
            v = v - 0x10000 if v & 0x8000 else v
            return f"ReturnConst_{v if v >= 0 else f'm{-v}'}_{a}"
        if op0 in ("r3", "f1") and m in ("lwz", "lfs", "lbz", "lhz") and op1.endswith("(r3)"):
            kind = {"lwz": "GetField", "lfs": "GetFloat", "lbz": "GetByte", "lhz": "GetWord"}[m]
            return f"{kind}_0x{idc.get_operand_value(h, 1) & 0xFFFF:X}_{a}"
        if m in ("stw", "stfs", "stb", "sth") and op1.endswith("(r3)") and op0 in ("r4", "f1"):
            kind = {"stw": "SetField", "stfs": "SetFloat", "stb": "SetByte", "sth": "SetWord"}[m]
            return f"{kind}_0x{idc.get_operand_value(h, 1) & 0xFFFF:X}_{a}"
        if m == "addi" and op0 == "r3" and op1 == "r3":
            return f"GetFieldAddr_0x{idc.get_operand_value(h, 2) & 0xFFFFFFFF:X}_{a}"
    if ms[-2:] == ["mtctr", "bctr"]:
        offs = []
        for h, m, op0, op1, _ in ins[:-2]:
            if m == "lwz" and re.match(r"(-?0x[0-9A-F]+|\d+)?\(r\d+\)$", op1):
                offs.append(idc.get_operand_value(h, 1) & 0xFFFF)
            elif m not in SHAPE_SKIP:
                offs = None
                break
        if offs and len(offs) >= 2:
            return "VCallThunk_" + "_".join(f"0x{o:X}" for o in offs) + f"_{a}"
    if ms == ["b"]:
        tgt = idc.get_operand_value(ins[0][0], 0)
        tn = name(tgt)
        if tn and not UNNAMED.match(tn):
            return f"j_{tn}"
    if ms == ["lis", "addi", "b"] and ins[1][2] == "r3":
        g = idc.get_operand_value(ins[1][0], 2)
        gname = name(g) or f"unk_{g:08X}"
        tn = name(idc.get_operand_value(ins[2][0], 0))
        if tn.startswith("StaticDtor_") or "Dtor" in tn or "dtor" in tn:
            return f"StaticDtor_{clean(gname)}"
    if ms == ["lis", "lis", "addi", "stw", "blr"] and "vftable" in ins[0][2]:
        g = idc.get_operand_value(ins[3][0], 1)
        return f"StaticDtor_{clean(name(g) or f'unk_{g:08X}')}"
    return None


def lib_name(ea):
    """Library of the nearest library-tier neighbours on each side."""
    found = []
    for step in (-1, 1):
        cur = ea
        for _ in range(60):
            f = ida_funcs.get_prev_func(cur) if step < 0 else ida_funcs.get_next_func(cur)
            if not f:
                break
            cur = f.start_ea
            m = re.match(r"(%s)_(sub|callback|unref)_[0-9A-F]{8}$" % "|".join(LIBS), name(cur))
            if m:
                found.append((abs(cur - ea), m.group(1)))
                break
    if not found:
        return "UE3"
    if len(found) == 2 and found[0][1] == found[1][1]:
        return found[0][1]
    return min(found)[1]


def stage_generic():
    funcs = S.setdefault("generic", [f for f in idautils.Functions() if UNNAMED.match(name(f))])
    while S["i"] < len(funcs):
        if over_budget():
            return False
        ea = funcs[S["i"]]
        S["i"] += 1
        if not UNNAMED.match(name(ea)):
            continue
        f = ida_funcs.get_func(ea)
        g = gnatives_index(f)
        if g:
            idx, fn = g
            fname = name(fn) if fn else ""
            if fname and not UNNAMED.match(fname) and not fname.startswith("Nop_"):
                rename(ea, f"StaticInit_RegisterNative_{clean(fname)}")
            else:
                rename(ea, f"StaticInit_RegisterNative_0x{idx:X}")
            continue
        n = shape_name(f)
        if n:
            rename(ea, n)
            continue
        refs = list(idautils.XrefsTo(ea))
        kind = "unref" if not refs else ("sub" if any(ida_funcs.get_func(r.frm) and idc.is_code(idc.get_full_flags(r.frm))
                                                      for r in refs) else "callback")
        rename(ea, f"{lib_name(ea)}_{kind}_{ea:08X}")
    return True


def stage_fixups():
    # Names that embedded a renamed nullsub_N/sub_ADDR (wrappers, adjustor thunks).
    renamed = {e["old"]: e["new"] for e in S["log"]}
    for f in idautils.Functions():
        n = name(f)
        m = re.match(r"^(nullsub_\d+|sub_[0-9A-F]{8})(_.+)$", n)
        if m and m.group(1) in renamed:
            rename(f, renamed[m.group(1)] + m.group(2))
    return True


STAGES = [("manual", stage_manual), ("shaders", stage_shaders), ("shader_callbacks", stage_shader_callbacks),
          ("generic", stage_generic), ("fixups", stage_fixups)]

order = [s for s, _ in STAGES]
while S["stage"] != "done":
    fn = dict(STAGES)[S["stage"]]
    if not fn():
        break
    nxt = order.index(S["stage"]) + 1
    S["stage"] = order[nxt] if nxt < len(order) else "done"
    S["i"] = 0

os.makedirs(os.path.dirname(LOG), exist_ok=True)
with open(LOG, "w", encoding="utf-8") as fh:
    json.dump(S["log"], fh, indent=1)
left = sum(1 for f in idautils.Functions() if UNNAMED.match(name(f)))
print(json.dumps({"stage": S["stage"], "i": S["i"], "renamed": len(S["log"]), "unnamed_left": left,
                  "seconds": round(time.time() - start, 1)}))
