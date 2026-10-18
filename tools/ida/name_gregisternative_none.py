"""Static initializers that are only GRegisterNative's table guard:
if (!GNativesInitialized) { GNativesInitialized = 1; fill GNatives with
execUndefined; }. That is what IMPLEMENT_FUNCTION(cls, INDEX_NONE, func)
compiles to once the unused return value is dropped, so the code no longer
says which native it was. name_xc_initializers.py named them after the guard
(StaticInit_dword_833753C4_N); this renames them
StaticInit_GRegisterNative_INDEX_NONE_ADDR. Same log as the other passes."""
import json

import ida_name
import idautils
import idc

LOG = r"F:\Re-Cherry\assets\rename_log_2026-10-16.json"
GUARD, GNATIVES_SYM, UNDEFINED_SYM = "dword_833753C4", "GNatives", "UObject__execUndefined"

with open(LOG, encoding="utf-8") as fh:
    log = json.load(fh)
done, odd = 0, []
for f in idautils.Functions():
    n = ida_name.get_name(f)
    if not n.startswith("StaticInit_" + GUARD):
        continue
    text = [idc.GetDisasm(h) for h in idautils.FuncItems(f)]
    stores = [t for t in text if t.startswith(("stw ", "stwu", "stb ", "stfs", "stvx"))]
    ok = (len(text) <= 18 and any(GNATIVES_SYM in t for t in text) and any(UNDEFINED_SYM in t for t in text)
          and all(GUARD in t or t.startswith("stwu      r8, 4(r11)") or "4(r" in t for t in stores))
    if not ok:
        odd.append(f"0x{f:08X}")
        continue
    new = f"StaticInit_GRegisterNative_INDEX_NONE_{f:08X}"
    if ida_name.set_name(f, new, ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
        log.append({"ea": f"0x{f:08X}", "old": n, "new": new})
        done += 1
with open(LOG, "w", encoding="utf-8") as fh:
    json.dump(log, fh, indent=1)
print(json.dumps({"renamed": done, "odd": odd[:20], "odd_count": len(odd)}))
