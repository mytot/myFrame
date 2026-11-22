#!/usr/bin/env python3
"""
pack.py — build, sign and verify .fpack packages.

  python3 tools/pack.py --manifest examples/native_hello/manifest.json \\
                        --payload examples/native_hello/build/frame_native_hello.bin \\
                        --out dist/example.counter-1.0.0.fpack

  # managed app: compile the wasm first (tools/build_app.py), then pack it
  python3 tools/build_app.py examples/wasm_hello/app.c --out dist/example.clock-1.0.0.fpack

  # sign with a specific publisher key (default: keys/dev/publisher)
  python3 tools/pack.py ... --publisher keys/dev/publisher

  # sign with the store root key instead (needed for raw sideloads)
  python3 tools/pack.py ... --root keys/dev/store_root

  # unsigned — only installs on a device built with CONFIG_FRAME_STORE_ALLOW_UNSIGNED
  python3 tools/pack.py ... --unsigned

  # verify exactly the way the device does, using the firmware's own verifier
  python3 tools/pack.py --verify dist/example.clock-1.0.0.fpack --pubkey keys/dev/publisher.pub

--verify compiles components/frame_pure/*.c with the host compiler together
with tools/verify_pkg.c (a miniature of frame_store/install.c's verification
path) and runs it on the package. If that passes, the device will accept it.
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
PURE = os.path.join(REPO_ROOT, "components", "frame_pure")


def cmd_build(args) -> int:
    with open(args.payload, "rb") as f:
        payload = f.read()
    if args.kind == "managed" or args.kind is None:
        try:
            info = frame_pkg.inspect_wasm(payload)
            is_wasm = True
        except frame_pkg.PkgError:
            is_wasm = False
        if args.kind is None and is_wasm:
            args.kind = "managed"
    if args.kind == "managed":
        info = frame_pkg.inspect_wasm(payload)
        missing = [e for e in ("frame_app_main",) if e not in info["exports"]]
        if missing:
            raise frame_pkg.PkgError(f"module does not export {missing}; see sdk/wasm/frame_app.h")
        if info["max_bytes"] is None or info["max_bytes"] > 512 * 1024:
            print(f"warning: module max linear memory is {info['max_bytes']} bytes; "
                  f"the device caps it at 512 KB (CONFIG_FRAME_WASM_HEAP_MAX_KB)")
        print(f"module: {len(payload)} bytes, exports {info['exports']}, "
              f"{info['initial_bytes']} B initial memory")
    elif args.kind == "native":
        # An ESP-IDF app image starts with the magic 0xE9 and a segment header.
        if not payload or payload[0] != 0xE9:
            raise frame_pkg.PkgError("payload does not look like an ESP-IDF app image (magic 0xE9)")

    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    # --kind overrides the manifest; without it we keep what the manifest says,
    # falling back to auto-detection only when the manifest has no kind.
    if args.kind is not None:
        manifest["kind"] = args.kind
    elif "kind" not in manifest:
        # auto-detect: wasm payloads are managed, everything else native
        try:
            frame_pkg.inspect_wasm(payload)
            manifest["kind"] = "managed"
        except frame_pkg.PkgError:
            manifest["kind"] = "native"
    # args.kind now reflects the final value for the wasm/native checks below
    if args.kind is None:
        args.kind = manifest["kind"]

    icon = None
    if args.icon:
        with open(args.icon, "rb") as f:
            icon = f.read()

    assets = {}
    if args.assets and os.path.isdir(args.assets):
        for name in sorted(os.listdir(args.assets)):
            p = os.path.join(args.assets, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    assets[name] = f.read()

    seed, pub = frame_pkg.resolve_signer(args.publisher, args.root, args.unsigned)
    out = args.out or _default_name(manifest)
    m = frame_pkg.build_fpack(manifest, payload, out, seed=seed, icon=icon, assets=assets)
    size = os.path.getsize(out)
    print(f"built {out}  ({size} bytes, payload {len(payload)}, kind {m['kind']})")
    if seed is None:
        print("UNSIGNED — installs only on a device built with CONFIG_FRAME_STORE_ALLOW_UNSIGNED=y")
    else:
        print(f"signed with Ed25519 public key {frame_pkg.b64url(pub)}")
    return 0


def _default_name(m: dict) -> str:
    return os.path.join("dist", f"{m['id']}-{m['version']}.fpack")


def cmd_verify(args) -> int:
    if args.pubkey:
        pub = frame_pkg.load_pub(args.pubkey)
    elif args.device_code:
        pub = None   # the C verifier falls back to keys/dev/publisher.pub
    else:
        raise frame_pkg.PkgError("--verify needs --pubkey (or --device-code to use the dev key)")
    if pub is not None:
        result = frame_pkg.verify_fpack(args.fpack, pub, strict=not args.lenient)
        m = result["manifest"]
        print(f"OK  {args.fpack}")
        print(f"    id {m['id']}  version {m['version']}  kind {m['kind']}")
        print(f"    payload sha256 {result['payload_sha256']}")
        print(f"    signature {result['signature_b64'][:24]}...")
    if args.device_code:
        return verify_with_device_code(args.fpack, pub)
    return 0


def verify_with_device_code(fpack: str, pub: bytes | None) -> int:
    """Builds and runs tools/verify_pkg.c against components/frame_pure (host)."""
    cc = os.environ.get("CC", "cc")
    if shutil.which(cc) is None:
        raise frame_pkg.PkgError(f"no host C compiler ({cc}) for --device-code verification")
    srcs = [os.path.join(PURE, n) for n in ("json.c", "canonical.c", "manifest.c", "semver.c",
                                             "base64url.c", "sha256.c", "sha512.c", "ed25519.c",
                                             "zip.c", "slot_table.c")]
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "verify_pkg")
        cmd = [cc, "-std=c11", "-O1", "-w", "-I", os.path.join(PURE, "include"),
               os.path.join(HERE, "verify_pkg.c"), *srcs, "-o", exe, "-lm"]
        subprocess.run(cmd, check=True)
        keyfile = os.path.join(td, "pub.bin")
        with open(keyfile, "wb") as f:
            f.write(pub if pub is not None else frame_pkg.load_pub(
                os.path.join(REPO_ROOT, "keys", "dev", "publisher.pub")))
        r = subprocess.run([exe, fpack, keyfile], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            raise frame_pkg.PkgError("device-code verification FAILED")
        print("device-code verification passed (same C as components/frame_pure)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="build a signed .fpack")
    b.add_argument("--manifest", required=True)
    b.add_argument("--payload", required=True, help="ESP-IDF .bin or .wasm")
    b.add_argument("--out", help="default dist/<id>-<version>.fpack")
    b.add_argument("--kind", choices=frame_pkg.KINDS, help="default: managed if the payload is wasm")
    b.add_argument("--icon", help="icon.png, 128x128")
    b.add_argument("--assets", help="directory copied flat into assets/")
    b.add_argument("--publisher", help="publisher key prefix (signs with <prefix>.key)")
    b.add_argument("--root", help="store root key prefix (for raw sideloads)")
    b.add_argument("--unsigned", action="store_true")
    b.set_defaults(fn=cmd_build)

    v = sub.add_parser("verify", help="verify a .fpack")
    v.add_argument("fpack")
    v.add_argument("--pubkey", help="publisher *.pub (default keys/dev/publisher.pub)")
    v.add_argument("--device-code", action="store_true",
                   help="also run the firmware's C verifier (host build)")
    v.add_argument("--lenient", action="store_true", help="skip manifest field checks")
    v.set_defaults(fn=cmd_verify)

    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except frame_pkg.PkgError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except (FileNotFoundError, json.JSONDecodeError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
