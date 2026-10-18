# Development rules

These apply to everyone working on the code, human or agent.

1. **Do not edit `rexglue` under any circumstances.**

   * Treat `rexglue` and `generated/` as read-only.
   * If changes are required, implement them outside of `rexglue` using hooks, wrappers, or other extension points.

2. **Do not add or use color test code.**

   * Never introduce temporary color validation, RGB test patterns, debug textures, or similar visual testing code.

3. **Always use IDA-Pro-MCP before making changes.**

   * The database is `assets/default.xex.i64`. Reference existing hooks, functions and symbols there before writing code.
   * Do not guess function behaviour or hook locations.

4. **Keep function declarations on a single line whenever practical.**

   * **Correct:** `void ApplyCostumeOverride(int dest, int src, const fs::path& cooked_dir, const fs::path& skin_mods_dir)`
   * Only wrap lines when absolutely necessary to satisfy language or tooling constraints.

5. **Keep comments to an absolute minimum.**

   * Only add comments when they provide necessary context that cannot be conveyed through code.
   * Comments must be a single sentence on a single line; no banners, section headers or TODO blocks.
   * Longer explanations belong in `docs/`.

6. **The renderer is native, not the xenos plugin.**

   * Every guest D3D entry point the renderer handles is named in `config/functions.toml` and replaced with `REX_HOOK(D3DDevice_X, D3DDevice_X_hook)` in `src/gpu/hooks/`, so the recompiled D3D body never runs.
   * Do not emulate the guest GPU: no ring buffer consumer, no interrupts, no fake read pointers. Replace the functions that wait instead.
   * Do not implement readback resolve. When game code reads a resolved render target on the CPU (camera, photos, exposure), hook that game function and give it the result natively.
   * See `docs/renderer/README.md`.

7. **Put code where the layout says it goes.**

   * `src/engine/` gameplay hooks, `src/ui/` overlays and HUD, `src/gpu/` the renderer, `src/app/` startup, `src/core/` shared plumbing.
   * A new midasm hook goes in the `config/hooks/<feature>.toml` for its area, with a comment saying where it sits in the guest function and why, and its declaration goes in `src/core/hooks.h`.
   * Every `REX_HOOK` / `REX_HOOK_RAW` override carries a `// GuestName(args)` comment and is defined exactly once.
   * Debug-only and investigation code lives in a `debug/` folder of its subsystem, not inline in the main path.

8. **Never rename a cvar.** Cvar names (`target_fps`, `costume_slot_NN`, `disable_xbox_live_prompt`, ...) are keys in users' saved settings.
