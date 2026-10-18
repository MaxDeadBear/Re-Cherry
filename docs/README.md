# Re-Cherry documentation

- [building.md](building.md) — SDK, codegen, presets, running.
- [renderer/README.md](renderer/README.md) — the native renderer that replaces the xenos plugin: how the guest D3D library is hooked, what it must never do, and the bring-up order.
- [renderer/debugging.md](renderer/debugging.md) — `--cherry_debug_hang_dump`, reading the hang and exception dumps, and the hangs found so far.
- [ui/option-menu.md](ui/option-menu.md) — the GRAPHICS page added to the game's Scaleform options screen: how the movie is patched as it loads, the `CherryGfx_*` ExternalInterface calls, and rebuilding the ActionScript with `tools/ui/build_actionscript.py`.
- [reverse-engineering/ue3-script.md](reverse-engineering/ue3-script.md) — the UE3 reflection layout `src/engine/ue3_objects.*` reads, `--cherry_debug_dump_script_classes`, where combos and abilities live, how `debug_unlock_all_combos` keeps them out of the save, and what `debug_unlock_all_moves` lends Chainsaw Dash and the Nick roulette.
- [reverse-engineering/ida-names.md](reverse-engineering/ida-names.md) — where the generated names in the IDA database come from (UE3 native tables, `GNatives`, `StaticClass`, vtables, FName globals) and how far to trust each kind.
- [reverse-engineering/guest-d3d.md](reverse-engineering/guest-d3d.md) — the statically linked XDK D3D library: address range, LTCG signatures, `D3DDevice` layout, present path and readback consumers.

Project layout:

| path | contents |
| --- | --- |
| `re_cherry_manifest.toml` | codegen manifest; lists every file under `config/` it includes |
| `config/functions.toml` | function boundaries the analyzer misses and names for hooked guest functions |
| `config/d3d.toml` | names for the guest D3D entry points the renderer hooks |
| `config/hooks/*.toml` | midasm hooks, one file per feature area (`display.toml`: the ultrawide back buffer) |
| `src/app/` | the `ReXApp` subclass: startup, paths, overlays |
| `src/core/` | shared plumbing; midasm hook declarations go in `src/core/hooks.h` |
| `src/engine/` | gameplay code: frame rate target, costumes, Xbox Live prompt, UE3 object access, the combo unlock |
| `src/ui/` | ImGui overlays and the options screen's GRAPHICS page (`actionscript/` holds its AS2 source and compiled header) |
| `src/gpu/` | the native renderer; `src/gpu/hooks/` replaces the guest D3D entry points |
| `generated/` | codegen output; only `rexglue.cmake` is tracked |
| `assets/` | the user's game dump and the IDA database; never tracked |
