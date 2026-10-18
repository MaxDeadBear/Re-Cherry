# Names in the IDA database

Most names in `assets/default.xex.i64` were generated from the game's own reflection data, not guessed. This page explains where each kind of name comes from and how far to trust it.

| name | source | trust |
| --- | --- | --- |
| `UClass::execFoo` | UE3 native tables: `{"UClassexecFoo", &UClass::execFoo}` pairs in `.data` (2162 functions) | exact |
| `Shared::execFoo`, `Shared::execFoo_etc` | the same tables, where identical-code folding merged several natives into one body; the function comment lists every native | exact, but one body serves several natives |
| `UObject::execLet`, `execJumpIfNot`, ... | the `GNatives` opcode table at `0x832E7168` (`GRegisterNative` 0x8227D318), named by opcode with the token numbers `tools/ue3script/disasm.py` uses; `0x60`-`0x6F` are `execExtendedNative_6N` | exact |
| `UClass::StaticClass`, `UClass::PrivateStaticClass` | every caller of `UClass::UClass` (0x82261F78); the class name is the `TEXT("UFoo")+1` argument | exact |
| `UClass::InternalConstructor`, `StaticConstructor`, `StaticInitializer` | the stack arguments of that `UClass::UClass` call, in that order | exact |
| `UClass::UClass`, `UClass::vftable` | the function `InternalConstructor` reaches that stores the vtable at `this+0` | high |
| `UClass::~UClass` | the first call of the class's slot 0 (`scalar_deleting_dtor`) | high |
| `UClass::vfNNN` | a virtual function in `UClass`'s vtable that differs from its parent's at that slot; `NNN` is the byte offset. The super class comes from `UClass::InitializePrivateStaticClass` (0x8225C170). | the owning class is exact; the method is unknown |
| `UClass::Method` on a virtual | the slot name carried down from the class that introduced the slot, from one of four sources: a native whose last call is that slot, a string naming the method in the base implementation (`Pure virtual not implemented (UProperty::GetCPPType)`), a base implementation that only raises the script event of that name (`AActor::PreBeginPlay`), or a slot read by hand (UObject slots 0, 3-10, 15, 16, 32, 37, 45, 50, 51, 62; about 60 slots of `AActor`, `UActorComponent`, `UPrimitiveComponent`, `UMaterialExpression`, `USequenceOp`, `UParticleModule`, `UAnimNode`, `UInterpTrack` and others) | high |
| `Class::Method` on non-virtual code | a string the function uses about itself: exactly `Class::Method`, `Class::Method: message`, or `... in Class::Method()` (PhysX, GFx and UE3 asserts) | high |
| `eventFoo` | a small thunk whose only `FindFunctionChecked` (0x822AB9D8) uses the FName global `NAME_Foo`, then calls `ProcessEvent` (UObject vtable `+0xF8`) | exact name, class unknown |
| `NAME_Foo` | FName globals, from the `FName::FName` calls that fill them at startup | exact |
| `SharedVftable_X`, `Shared::InternalConstructor_ADDR` | vtables and constructors that several classes share after folding; the comment lists the classes | exact |

| `EURCMacro_Foo::Execute`, `::DescribeCommand` | render commands: vtables whose slot 0 is `FRenderCommand::scalar_deleting_dtor` (0x82337A68) or that have a slot returning the command's name | exact |
| `GASArray::concat`, `GASMovieClip::gotoAndPlay`, ... | Scaleform ActionScript method tables (`{"name", function}` pairs); the class is the AS2 class those method lists belong to | high |
| `StaticInit_RegisterNative_X`, `StaticInit_NAME_Foo`, `StaticDtor_X` | the static initializers in 0x82E40000-0x82E80000, named after the native they register or the global they build | exact |
| `StaticInit_X` from the `__xc_a`..`__xc_z` tables | initializers that IDA never made functions because only the CRT initializer tables (raw pointers in `.data`/`.rdata`, now dword offsets) reach them; named after the global they store to | exact for the global; says nothing about its meaning when that global is `unk_ADDR` |
| `StaticInit_GRegisterNative_INDEX_NONE_ADDR` | `IMPLEMENT_FUNCTION(cls, INDEX_NONE, func)` reduced to `GRegisterNative`'s table guard (`if (!dword_833753C4) fill GNatives with execUndefined`) once its unused result was dropped; which native it was is not in the code | exact for what it does |
| `StaticInitShaderType_T`, `StaticDtor_ShaderType_T` | every caller of `FShaderType::FShaderType` (0x823052D0, 523 types); `T` is its first argument, the type name (`TShadowProjectionPixelShader<F16SampleHwPCF>` becomes `TShadowProjectionPixelShader_F16SampleHwPCF`), and the destructor is the one it passes to `atexit` | exact |
| `T::ConstructSerializedInstance`, `T::ModifyCompilationEnvironment`, `T::ConstructCompiledInstance`, `T::ShouldCache` | the callbacks of that registration: the eighth and last constructor arguments, then the two stores after the vtable (`FGlobalShaderType`/`FMaterialShaderType` `ConstructCompiledRef`, `ShouldCacheRef`). `Shared::ShouldCache_ADDR` and friends serve several types after folding; the comment lists them | exact |
| `Nop_ADDR` | a lone `blr` (IDA's `nullsub_N`); `Nop_829B43F8` is the shared empty function with about 5500 references | describes the code exactly |
| `GetField_0x128_ADDR`, `SetFloat_0x40_ADDR`, `ReturnConst_N_ADDR`, `VCallThunk_0xOFF_ADDR`, `j_X`, `X_adjN`, `X_wrapper_ADDR` | the instruction shape of a tiny function: field getter or setter, constant, vtable dispatch, tail jump, `this`-adjusting thunk, tail call with fixed arguments | describes the code exactly; says nothing about meaning |
| `vtbl_ADDR::vfNNN`, `NpScene::vfNNN` | entries of plain C++ vtables. The class is known only when one of its own entries was named from a string (PhysX, for example), otherwise the vtable address stands in for it. A function shared by several vtables takes the shortest one, usually the base. | location only |
| `Class::sub_ADDR` | called with the caller's own `this` from methods of `Class`; for UE3 classes, the lowest common ancestor of all the calling classes | member of that class, method unknown |
| `Class::util_ADDR` | every caller belongs to `Class` (or its subclasses), but not through `this` | used by that class, purpose unknown |
| `Caller_helperN_ADDR` | the only caller is `Caller`; `N` counts helper levels below it | location only |
| `File_sub_ADDR`, `Lib_sub_ADDR`, `Lib_callback_ADDR`, `Lib_unref_ADDR` | the last tier: the source file its asserts name, or the library its address range belongs to (`UE3`, `GFx`, `PhysX`, `D3DX`, `CRT`, `XAPI`, `XAudio2`, `XOnline`, `XDK`, `D3D`); `callback` means it is only referenced from data, `unref` that nothing references it (code the linker kept, mostly leftovers of identical-code folding) | location only |

Everything from `Class::sub_ADDR` down only says where the code lives. Treat it as a pointer for further reading, not as a description. These names keep the address, so `sub_ADDR` in `config/` and `src/` can still be found by searching for the address.

Function comments add two more things to unnamed functions: `src: File.cpp` for each source path the function's asserts reference, and `calls script eventFoo` for functions that raise script events.

Names from `config/functions.toml`, `config/d3d.toml` and the addresses in these docs were applied as they are written.

## Completing the database (2026-10-16)

The earlier passes left 259 functions as `sub_`/`nullsub_`, and about 46,700 instruction words of `.text` were code outside any function, mostly leaf functions that have no `.pdata` entry. `tools/ida/name_leftovers.py` creates functions in those gaps (47,534 functions became 51,227, counting the ones IDA's analysis found from the new code) and names every function that is still unnamed. `tools/ida/name_xc_initializers.py` and `name_gregisternative_none.py` then name the ones reached only through the initializer tables. The 49 8-byte "functions" that start with `0x82EA41BC` are not code: they are the `__C_specific_handler` and scope-table pointers that precede every function with an SEH scope, and are now dwords. The 55 `.pdata` entries that do not start a function are the `__finally`/`__except` funclets inside CRT and XAPI functions, which is correct as it is.

Every rename is in `assets/rename_log_2026-10-16.json` (address, old name, new name), and `assets/default.xex.before-naming-2026-10-16.i64` is the database from before the pass.

`tools/ida/export_all.py` (driven by `export_all_loop.py`) writes the Hex-Rays output of every function to `decomp/` with an `index.json` of names, sizes, callers and callees. That folder is the game's code and is ignored by git.
