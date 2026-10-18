#!/usr/bin/env python3
"""Build res/shader_cache/shader_cache.cpp from dumped Xenos shader containers.

Runs the patched XenosRecomp (tools/xenosrecomp/cherry.patch) as ONE PROCESS PER
SHADER in `--blobs` mode, with retries, and merges the per-shader DXIL / smol-v
SPIR-V / meta files into one generated source.

Why not XenosRecomp's own directory batch? Driving DXC many times in one process
corrupts the heap (nondeterministic, worse with volume, survives serialisation;
see docs/renderer/shaders.md). One process per shader plus a retry loop absorbs it.

Input is a directory of `<XXH3_64 hex>.bin` containers, as the runtime dumps them
(default <repo>/shader_dump). `--image` additionally scans any blob for 0x102A11xx
containers with XenosRecomp's own test. The runtime key is
XXH3_64bits(container, virtualSize + physicalSize), which `--blobs` reports in its
.meta; it is checked against the dump's file name.

  python tools/shaders/build_shader_cache.py [--containers DIR] [--image FILE]
      [--out res/shader_cache/shader_cache.cpp] [--jobs N] [--retries N] [--no-cache]
"""

import argparse
import concurrent.futures
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_EXE = os.path.join(ROOT, "out", "xenosrecomp", "XenosRecomp", "XenosRecomp.exe")
DEFAULT_INCLUDE = os.path.join(ROOT, "thirdparty", "XenosRecomp", "XenosRecomp", "shader_common.h")
DEFAULT_CONTAINERS = os.path.join(ROOT, "shader_dump")
DEFAULT_OUT = os.path.join(ROOT, "res", "shader_cache", "shader_cache.cpp")
DEFAULT_CACHE = os.path.join(ROOT, "out", "shader_build_cache")

MAGIC_MASK = 0xFFFFFF00
MAGIC = 0x102A1100  # container v0x11: 0x102A1100 pixel, 0x102A1101 vertex
HEADER = 36         # nine dwords

try:
    import zstandard
except ImportError:  # stored verbatim; the runtime treats compressed == decompressed size as uncompressed
    zstandard = None


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def find_containers(data):
    """XenosRecomp's discovery test: magic, size in range, field1C == field20 == 0."""
    out = []
    i = 0
    n = len(data)
    while i + HEADER <= n:
        if (be32(data, i) & MAGIC_MASK) == MAGIC:
            total = be32(data, i + 4) + be32(data, i + 8)
            if 0 < total <= n - i and be32(data, i + 28) == 0 and be32(data, i + 32) == 0:
                out.append((i, total))
                i += total
                continue
        i += 4
    return out


def read_meta(path):
    with open(path, "r") as f:
        fields = f.read().split()
    h, dxil_size, spirv_size, spec = fields[:4]
    dims = fields[4] if len(fields) > 4 else "0"
    # One past the highest float4 constant the code reads; older metadata has none, so the host copies all 256.
    regs = fields[5] if len(fields) > 5 else "256"
    return {"hash": int(h, 16), "dxil_size": int(dxil_size), "spirv_size": int(spirv_size),
            "spec": int(spec), "dims": int(dims), "regs": int(regs)}


def cached_entry(cache_dir, key):
    meta = os.path.join(cache_dir, key + ".meta")
    dxil = os.path.join(cache_dir, key + ".dxil")
    spirv = os.path.join(cache_dir, key + ".spirv")
    if not (os.path.isfile(meta) and os.path.isfile(dxil) and os.path.isfile(spirv)):
        return None
    try:
        e = read_meta(meta)
    except (OSError, ValueError, IndexError):
        return None
    e.update(dxil=dxil, spirv=spirv)
    return e


def store(cache_dir, key, stem, meta_suffix):
    """Copy one shader's artefacts into the build cache (an empty .spirv for a DXIL salvage)."""
    dxil = os.path.join(cache_dir, key + ".dxil")
    spirv = os.path.join(cache_dir, key + ".spirv")
    shutil.copyfile(stem + ".dxil", dxil)
    if meta_suffix == ".meta":
        shutil.copyfile(stem + ".spirv", spirv)
    else:
        open(spirv, "wb").close()
    shutil.copyfile(stem + meta_suffix, os.path.join(cache_dir, key + ".meta"))  # last: marks the entry complete
    return dxil, spirv


def compile_one(idx, label, blob, work, exe, include, retries, cache_dir):
    """Returns (status, label, entry_or_error); status is ok, salvaged or failed."""
    key = hashlib.sha256(blob).hexdigest()
    if cache_dir:
        e = cached_entry(cache_dir, key)
        if e is not None:
            return ("salvaged" if e["spirv_size"] == 0 else "ok", label, e)

    stem = os.path.join(work, f"{idx:05d}")
    binp = stem + ".bin"
    with open(binp, "wb") as f:
        f.write(blob)

    outputs = [stem + s for s in (".meta", ".dxil.meta", ".dxil", ".spirv")]
    err = ""
    salvage = False
    for _ in range(max(1, retries)):
        for p in outputs:
            if os.path.exists(p):
                os.remove(p)
        proc = subprocess.run([exe, binp, stem, include, "--blobs"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if proc.returncode == 0 and os.path.isfile(stem + ".meta"):
            break
        err = proc.stderr.decode(errors="replace").strip() or f"exit code {proc.returncode:#x}"
        salvage = os.path.isfile(stem + ".dxil.meta") and os.path.isfile(stem + ".dxil")
    else:
        if not salvage:
            return ("failed", label, err[:2000])

    meta_suffix = ".meta" if os.path.isfile(stem + ".meta") else ".dxil.meta"
    e = read_meta(stem + meta_suffix)
    if cache_dir:
        dxil, spirv = store(cache_dir, key, stem, meta_suffix)
    else:
        dxil, spirv = stem + ".dxil", stem + ".spirv"
    e.update(dxil=dxil, spirv=spirv)
    if meta_suffix != ".meta":
        e["spirv_size"] = 0
        return ("salvaged", label, e, err[:2000])
    return ("ok", label, e)


def check_cache_stamp(cache_dir, exe, include):
    """The build cache is keyed by container content only, so it follows the translator and shader_common.h."""
    os.makedirs(cache_dir, exist_ok=True)
    stamp_file = os.path.join(cache_dir, "stamp.txt")
    stamp = "|".join(f"{os.path.getmtime(p)}:{os.path.getsize(p)}" for p in (exe, include))
    old = None
    if os.path.isfile(stamp_file):
        with open(stamp_file, "r") as f:
            old = f.read().strip()
    if old != stamp:
        if old is not None:
            print("translator or shader_common.h changed; invalidating the shader build cache")
        shutil.rmtree(cache_dir, ignore_errors=True)
        os.makedirs(cache_dir, exist_ok=True)
        with open(stamp_file, "w") as f:
            f.write(stamp)


def write_bytes(f, name, data):
    f.write(f"const uint8_t {name}[] = {{\n")
    if not data:
        f.write("0\n")
    for i in range(0, len(data), 64):
        f.write(",".join(map(str, data[i:i + 64])))
        f.write(",\n")
    f.write("};\n")


def compress(data):
    if zstandard is None or not data:
        return bytes(data)
    packed = zstandard.ZstdCompressor(level=19).compress(bytes(data))
    return packed if len(packed) < len(data) else bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=DEFAULT_EXE, help="patched XenosRecomp.exe")
    ap.add_argument("--include", default=DEFAULT_INCLUDE, help="shader_common.h")
    ap.add_argument("--containers", default=DEFAULT_CONTAINERS, help="directory of dumped <hash>.bin containers")
    ap.add_argument("--image", action="append", default=[], help="a blob to scan for containers (repeatable)")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--retries", type=int, default=8)
    ap.add_argument("--jobs", "-j", type=int, default=min(16, os.cpu_count() or 8))
    ap.add_argument("--cache-dir", default=DEFAULT_CACHE)
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory (HLSL of every shader)")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    include = os.path.abspath(args.include)
    for p in (exe, include):
        if not os.path.isfile(p):
            sys.exit(f"missing {p} (see docs/renderer/shaders.md)")

    cache_dir = None if args.no_cache else os.path.abspath(args.cache_dir)
    if cache_dir:
        check_cache_stamp(cache_dir, exe, include)

    sources = []  # (label, blob, expected hash or None)
    if args.containers and os.path.isdir(args.containers):
        names = sorted(n for n in os.listdir(args.containers) if n.lower().endswith(".bin"))
        for n in names:
            with open(os.path.join(args.containers, n), "rb") as f:
                data = f.read()
            found = find_containers(data)
            if not found:
                print(f"  skipping {n}: no 0x102A11xx container")
                continue
            try:
                expected = int(os.path.splitext(n)[0], 16)
            except ValueError:
                expected = None
            for k, (off, total) in enumerate(found):
                sources.append((n if k == 0 else f"{n}@0x{off:X}", data[off:off + total],
                                expected if (off == 0 and total == len(data)) else None))
        print(f"{len(names)} files in {args.containers}")
    elif args.containers and not args.image:
        sys.exit(f"no container directory {args.containers}")
    for image in args.image:
        with open(image, "rb") as f:
            data = f.read()
        found = find_containers(data)
        print(f"{len(found)} containers in {image}")
        sources.extend((f"{os.path.basename(image)}@0x{off:X}", data[off:off + total], None) for off, total in found)

    unique, seen = [], set()
    for label, blob, expected in sources:
        digest = hashlib.sha256(blob).digest()
        if digest not in seen:
            seen.add(digest)
            unique.append((label, blob, expected))
    sources = unique
    expected_hash = {label: expected for label, _, expected in sources}
    print(f"{len(sources)} unique containers, {args.jobs} jobs, {args.retries} attempts each")

    work = tempfile.mkdtemp(prefix="cherry_shaders_")
    entries, salvaged, failed = [], [], []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futures = [ex.submit(compile_one, idx, label, blob, work, exe, include, args.retries, cache_dir)
                       for idx, (label, blob, _) in enumerate(sources)]
            for done, fut in enumerate(concurrent.futures.as_completed(futures), 1):
                res = fut.result()
                if res[0] == "failed":
                    failed.append((res[1], res[2]))
                else:
                    entries.append(res[2])
                    if res[0] == "salvaged":
                        salvaged.append((res[1], res[3] if len(res) > 3 else "cached"))
                    exp = expected_hash.get(res[1])
                    if exp is not None and exp != res[2]["hash"]:
                        print(f"  WARNING {res[1]}: translator hash {res[2]['hash']:016X} differs from the file name")
                if done % 50 == 0 or done == len(futures):
                    print(f"  {done}/{len(futures)}")

        print(f"compiled {len(entries)}, failed {len(failed)}, salvaged dxil-only {len(salvaged)}")
        for label, err in salvaged:
            print(f"  SALVAGED (SPIR-V failed, DXIL kept) {label}: {err.splitlines()[0] if err else ''}")
        for label, err in failed:
            print(f"  FAILED {label}:\n    " + "\n    ".join(err.splitlines()[:6]))

        by_hash = {}
        for e in entries:
            by_hash.setdefault(e["hash"], e)
        unique_entries = [by_hash[h] for h in sorted(by_hash)]

        dxil_blob, spirv_blob, rows = bytearray(), bytearray(), []
        for e in unique_entries:
            d_off = len(dxil_blob)
            if e["dxil_size"]:
                with open(e["dxil"], "rb") as f:
                    dxil_blob += f.read()
            s_off = len(spirv_blob)
            if e["spirv_size"]:
                with open(e["spirv"], "rb") as f:
                    spirv_blob += f.read()
            rows.append((e["hash"], d_off, e["dxil_size"], s_off, e["spirv_size"], e["spec"], e["dims"], e["regs"]))

        dxil_c, spirv_c = compress(dxil_blob), compress(spirv_blob)

        out = os.path.abspath(args.out)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", newline="\n") as f:
            f.write('#include "shader_cache.h"\n\n')
            f.write("// Generated by tools/shaders/build_shader_cache.py from shader_dump/; do not edit.\n")
            f.write(f"// {len(rows)} shaders. Each blob is zstd-compressed, or stored verbatim when its compressed size"
                    " equals its decompressed size. SPIR-V is smol-v per entry.\n\n")
            f.write("ShaderCacheEntry g_shaderCacheEntries[] = {\n")
            for r in rows:
                f.write("\t{{ 0x{:016X}, {}, {}, {}, {}, {}, {}, {} }},\n".format(*r))
            if not rows:
                f.write("\t{},\n")
            f.write("};\n\n")
            write_bytes(f, "g_compressedDxilCache", dxil_c)
            f.write(f"const size_t g_dxilCacheCompressedSize = {len(dxil_c)};\n")
            f.write(f"const size_t g_dxilCacheDecompressedSize = {len(dxil_blob)};\n\n")
            write_bytes(f, "g_compressedSpirvCache", spirv_c)
            f.write(f"const size_t g_spirvCacheCompressedSize = {len(spirv_c)};\n")
            f.write(f"const size_t g_spirvCacheDecompressedSize = {len(spirv_blob)};\n\n")
            f.write(f"const size_t g_shaderCacheEntryCount = {len(rows)};\n")

        print(f"wrote {os.path.relpath(out, ROOT)}: {len(rows)} shaders, "
              f"dxil {len(dxil_blob)} -> {len(dxil_c)}, spirv {len(spirv_blob)} -> {len(spirv_c)}"
              f"{'' if zstandard else ' (zstandard not installed: stored verbatim)'}")
        return 1 if failed else 0
    finally:
        if args.keep:
            print(f"scratch kept at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
