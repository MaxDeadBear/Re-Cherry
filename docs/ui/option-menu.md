# Options screen: the GRAPHICS page

The game's own options screen (pause menu or title, OPTION) gets two pages of graphics settings, in the game's style. Y or RB steps GAME, GRAPHICS 1/2, GRAPHICS 2/2; LB steps back. GRAPHICS changes apply the moment a value changes. OK keeps and saves both pages; CANCEL puts both back as they were when the screen opened. DEFAULT resets only the page on screen. X shows or hides the FPS counter (`cherry_fps_counter`, saved at once) on either page.

| row | choices | cvar |
| --- | --- | --- |
| RESOLUTION | AUTO, 144P, 360P, 720P, 1080P, 1440P, 2160P | `cherry_gfx_render_height` (AUTO is 0, follow the window) |
| ANTI-ALIASING | OFF, FXAA | `cherry_gfx_fxaa` |
| TEXTURE FILTERING | DEFAULT, 2X, 4X, 8X, 16X | `cherry_gfx_anisotropy` |
| ASPECT RATIO | ORIGINAL, STRETCH, 21:9 (RESTART), 32:9 (RESTART) | `cherry_gfx_stretch` and `cherry_gfx_ultrawide` together; the wide choices take effect at the next launch |
| DISPLAY MODE | WINDOWED, FULLSCREEN | `fullscreen` |
| V-SYNC | OFF, ON | `cherry_gfx_vsync` |
| FRAME RATE | 30 FPS, 60 FPS, UNCAPPED | `target_fps` (UNCAPPED is 0: every refresh with V-SYNC on, no limit with it off) |
| SHADOW QUALITY | DEFAULT, 256, 512, 1024 (RESTART) | `cherry_gfx_shadow_resolution` |
| FIELD OF VIEW | 90% to 130% | `cherry_gfx_fov`: both projection hooks in `src/engine/widescreen.cpp` scale the half-angle's tangent |
| BRIGHTNESS | -2 to +2 | `cherry_gfx_brightness`: the final blit raises the picture to 1 - 0.1 x step |
| SHARPENING | OFF, LOW, HIGH | `cherry_gfx_sharpen`: an unsharp mask over the four neighbours in the final blit |
| SKIP INTRO LOGOS | OFF, ON | `skip_startup_logos`: the five logo movies at boot are started and stopped at once |
| FPS COUNTER | OFF, ON | `cherry_fps_counter` |
| RENDERER | D3D12, VULKAN (RESTART) | `cherry_gfx_backend` (`auto` shows as D3D12) |

A value outside the list (set by hand in the config) shows as the first choice. OK writes the cvars on the UI thread with `SetFlagByName`, as the F4 settings overlay does, so change callbacks such as `fullscreen`'s run. It then saves the config.

## The movie

The screen is the Scaleform movie in `OptionMenu_kis_common.xxx`: a GFX 8 file with AS2 (`gfx.controls` components), 1280x720. Its controller is the sprite placed as `opn`, id 88 in this dump. That sprite's frame 3 `DoAction` defines everything:

- **Rows.** Each row is a label clip (`but_vib`, `but_camud`, `but_camrl`, `but_aim`, `but_bgm`, `but_se`, `but_sub`) and a value clip. Value clips are ON/OFF-style widgets with `SetChangeMode(bool)` and a `cel` child framed `on`/`off`, or 10-pip sliders (`bgm_vol`, `se_vol`, `nVolnum`).
- **Focus.** Navigation is gfx `FocusHandler` moving between invisible buttons. `hit_<row>` is the row; `hit_<row>_1` (right) and `hit_<row>_2` (left) bounce focus straight back to it. Their `focusIn` handlers (`Focus_*_Out`) do the left/right change.
- **Listeners.** Listeners are registered by name (`addEventListener(type, this, "MovFocus_Vibration")`), and gfx's `EventDispatcher` looks the name up on every dispatch, so reassigning the function on the timeline redirects it.
- **Cursor and A.** `mSelectCursor` is the focused row: 0 vib, 1 camud, 2 camrl, 8 aim, 3 bgm, 4 se, 9 sub, 5 DEFAULT, 6 OK, 7 CANCEL. `SelectButton_Push` acts on A.
- **Save and cancel.** `AllOptionSetting` calls the `ASHUS_Set*` UnrealScript functions; `AllOptionSetCancel` and `AllOptionSetDefault` restore the values.

Labels are text fields named `str` set to `$AshGame.OptionMenu.OPN_*` keys, which the translator localises, in the `$AshUIFont01` font from `gfxfontlib.swf`. Plain strings render in the same font.

## The patch

`src/ui/actionscript/option_menu_graphics.as` is compiled to AVM1 bytecode by `tools/ui/build_actionscript.py`, which needs JPEXS FFDec's `ffdec.jar` and Java. The output is `src/ui/actionscript/actionscript.h`, which is committed. `src/ui/option_menu.cpp` hooks two functions:

- **`FGFxFileOpener::OpenFile` (`0x82932070`, `src/ui/movie_patches.cpp`).** For a movie inside a package it returns a memory file (vtable `0x8218A6E8`: data `+8`, length `+12`) over the `USwfMovie`'s `RawData`. When a GFX movie has a `DefineSprite` and `DoAction` containing `ASHUS_SetOptionMenuVibMode`, it gets the compiled script as a new `DoAction` right after that one. The copy lives in guest memory, built once and reused, and the file is pointed at it. The game's package is never modified.
- **`FGFxExternalInterface::Callback` (`0x829339B0`).** This is Flash's `ExternalInterface.call`. It normally runs the UnrealScript function of that name with the arguments converted (`docs/reverse-engineering/ue3-script.md`). Calls named `CherryGfx_*` are answered natively instead:
  - `Get(row)` and `Default(row)` return the choice index.
  - `Set(row, index)` applies a value at once.
  - `Save` writes the config.
  - `ToggleFps` flips `cherry_fps_counter` and saves it.

  A result is a 16-byte `GFxValue` (`+4` type: 2 bool, 3 number, 5 wide string; `+8` value) passed to the movie view's vtable slot `+200` (`SetExternalInterfaceRetVal`).

The script copies `but_sub` and `sub_vol` with `duplicateMovieClip` for the seven GRAPHICS rows, which both graphics pages share: row r of page p is setting 7 x (p - 1) + r in `CherryGfx_*`. It places them over the original rows, interpolating between the aiming and subtitle rows for the two slider rows. Page switches flip `_visible`. The script also wraps the focus, push, setting, cancel, default and key-down functions so each checks the page. Copied clips rerun their own frame scripts, which reset `str.text`, so an `onEnterFrame` clip rewrites the GRAPHICS texts every frame.
