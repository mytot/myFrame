#!/usr/bin/env python3
"""
build_app.py — compile a managed (WASM) app and pack it into a signed .fpack.

  python3 tools/build_app.py examples/wasm_hello/app.c \\
      --manifest examples/wasm_hello/manifest.json \\
      --out dist/example.clock-1.0.0.fpack

Needs a clang that can target wasm32 (any recent LLVM; on Debian/Ubuntu:
`apt install clang lld`). The module is built freestanding: no libc, no
allocator, so the whole app is a few KB and needs nothing from the device
except the frame_* imports in sdk/wasm/frame_app.h.

Flags are fixed on purpose — they define the ABI contract (exports, import
module "env", memory layout). Changing them means changing the device side.

  --unsigned         pack without a signature (dev devices only)
  --publisher KEY    sign with a publisher key prefix (default keys/dev/publisher)
  --root KEY         sign with the store root key (for raw sideloads)
  --serve DIR        also copy the .fpack into a dev-store directory
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_pkg  # noqa: E402

REPO_ROOT = os.path.dirname(HERE)
SDK_DIR = os.path.join(REPO_ROOT, "sdk", "wasm")

CLANG_CANDIDATES = ("clang", "clang-19", "clang-18", "clang-17", "clang-16", "clang-15")
INITIAL_MEMORY = 131072      # 2 pages: enough for a UI app's strings
MAX_MEMORY = 524288          # 8 pages = the device's default cap (512 KB)
EXPORTS = ("frame_app_main", "frame_app_event", "frame_app_tick")


def find_clang() -> str:
    for c in CLANG_CANDIDATES:
        path = shutil.which(c)
        if path:
            return path
    raise SystemExit("error: no clang found. Install LLVM (clang + lld) to build managed apps, "
                     "or use a release .fpack.")


def compile_wasm(sources: list[str], out: str, *, clang: str, optimize: str = "-O2",
                 extra_flags: list[str] | None = None) -> None:
    cmd = [
        clang, "--target=wasm32", optimize, "-nostdlib", "-ffreestanding",
        "-fno-builtin", "-Wall", "-Wno-unknown-attributes",
        "-I", SDK_DIR,
        "-Wl,--no-entry",
        *[f"-Wl,--export={e}" for e in EXPORTS],
        "-Wl,--import-undefined", "-Wl,--allow-undefined",
        f"-Wl,--initial-memory={INITIAL_MEMORY}", f"-Wl,--max-memory={MAX_MEMORY}",
        "-Wl,--stack-first", "-Wl,--strip-all",
        *(extra_flags or []),
        "-o", out, *sources,
    ]
    subprocess.run(cmd, check=True)


def build_and_pack(args) -> int:
    clang = args.clang or find_clang()
    with tempfile.TemporaryDirectory() as td:
        wasm = os.path.join(td, "app.wasm")
        compile_wasm(args.sources, wasm, clang=clang, optimize=args.opt,
                     extra_flags=args.flags.split() if args.flags else None)
        with open(wasm, "rb") as f:
            payload = f.read()

    info = frame_pkg.inspect_wasm(payload)
    missing = [e for e in EXPORTS if e not in info["exports"]]
    if "frame_app_main" in missing:
        raise frame_pkg.PkgError("module does not export frame_app_main")
    bad = [imp for imp in info["imports"] if imp[0] != "env"]
    if bad:
        raise frame_pkg.PkgError(f"imports must come from module \"env\", saw {bad}")
    if info["max_bytes"] is None or info["max_bytes"] > MAX_MEMORY:
        raise frame_pkg.PkgError(f"module max memory {info['max_bytes']} exceeds the {MAX_MEMORY} B cap")

    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    manifest["kind"] = "managed"
    manifest["entry"] = "payload.bin"

    seed, pub = frame_pkg.resolve_signer(args.publisher, args.root, args.unsigned)
    out = args.out or os.path.join("dist", f"{manifest['id']}-{manifest['version']}.fpack")
    frame_pkg.build_fpack(manifest, payload, out, seed=seed)
    print(f"{out}: {os.path.getsize(out)} bytes (module {len(payload)} B, "
          f"initial memory {info['initial_bytes']} B, exports {info['exports']})")
    print(f"imports from env: {len(info['imports'])}")
    if seed is None:
        print("UNSIGNED — installs only with CONFIG_FRAME_STORE_ALLOW_UNSIGNED=y")
    else:
        print(f"signed: publisher {frame_pkg.b64url(pub)}")
    if args.serve:
        os.makedirs(args.serve, exist_ok=True)
        shutil.copy2(out, os.path.join(args.serve, os.path.basename(out)))
        print(f"copied to {args.serve}/ (restart tools/devstore.py or POST /refresh to pick it up)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+", help=".c files (first must define frame_app_main)")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out")
    ap.add_argument("--clang", help="clang executable (default: search PATH)")
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("--flags", help="extra compiler flags, space separated")
    ap.add_argument("--unsigned", action="store_true")
    ap.add_argument("--publisher")
    ap.add_argument("--root")
    ap.add_argument("--serve", help="dev store pkgs directory to copy the .fpack into")
    args = ap.parse_args(argv)
    try:
        return build_and_pack(args)
    except frame_pkg.PkgError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f"error: {e.cmd[0]} failed with exit code {e.returncode}", file=sys.stderr)
        return e.returncode or 1


if __name__ == "__main__":
    sys.exit(main())
