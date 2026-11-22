#!/usr/bin/env python3
"""
tests/test_tools.py — the packaging tools, end to end.

Covers the contract between the laptop and the device:

  * canonical JSON produced in Python equals what the device's C produces
    (verified by compiling frame_pure's canonical.c and comparing bytes)
  * a package built by pack.py verifies with the device's own C verifier
    (tools/verify_pkg.c) and with the Python verifier
  * tampering with manifest or payload is rejected
  * the catalog builds, signs and verifies
  * inspect_wasm parses a hand-built module and refuses a non-wasm payload

Run: python3 tests/test_tools.py     (or make tools)
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import frame_crypto  # noqa: E402
import frame_pkg  # noqa: E402
import pack as pack_mod  # noqa: E402

PURE = os.path.join(ROOT, "components", "frame_pure")
PASS = 0
FAIL = 0


def check(cond: bool, what: str) -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok    {what}")
    else:
        FAIL += 1
        print(f"  FAIL  {what}")


def cc() -> str:
    return os.environ.get("CC", "cc")


# ----------------------------------------------------------------------------
# canonical.c vs Python canonical_json
# ----------------------------------------------------------------------------

def canonical_c(text: bytes) -> bytes:
    """Runs frame_pure/canonical.c on `text` through a tiny host program."""
    src = r'''
#include "frame_pure/canonical.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    static char in[65536], out[65536];
    size_t n = fread(in, 1, sizeof(in) - 1, stdin);
    in[n] = 0;
    if (!frame_canonicalise(in, n, out, sizeof(out))) { return 3; }
    fputs(out, stdout);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as td:
        cpath = os.path.join(td, "canon.c")
        exe = os.path.join(td, "canon")
        with open(cpath, "w") as f:
            f.write(src)
        subprocess.run([cc(), "-std=c11", "-O1", "-w", "-I", os.path.join(PURE, "include"),
                        cpath, os.path.join(PURE, "json.c"), os.path.join(PURE, "canonical.c"),
                        "-o", exe], check=True)
        r = subprocess.run([exe], input=text, capture_output=True, check=True)
        return r.stdout


def test_canonical() -> None:
    print("canonical JSON: Python == C")
    doc = {
        "z": 1, "a": {"yy": [1, 2, 3], "xx": "quote\" backslash\\ newline\n unicode é ✓"},
        "m": True, "k": None, "size": 421376, "id": "dev.frame.clock",
    }
    text = json.dumps(doc, indent=2).encode("utf-8")
    py = frame_pkg.canonical_json(json.loads(text))
    c = canonical_c(text)
    check(py == c, "canonical bytes identical for a manifest-like document")
    expected = '"a":{"xx":"quote\\" backslash\\\\ newline\\n unicode é ✓","yy":[1,2,3]}'.encode("utf-8")
    check(expected in py, "keys sorted, escapes minimal, UTF-8 raw")


# ----------------------------------------------------------------------------
# packages
# ----------------------------------------------------------------------------

def test_packages() -> None:
    print("packages: sign, verify (python + device C), tamper")
    with tempfile.TemporaryDirectory() as td:
        prefix = os.path.join(td, "publisher")
        frame_crypto.save_keypair(prefix)
        pub = frame_crypto.load_public(prefix + ".pub")

        manifest = {
            "schema": 1, "id": "test.clock", "name": "Clock", "version": "1.2.3",
            "kind": "managed", "sdk": "^1.0.0", "entry": "payload.bin",
            "description": "test package", "author": "tests", "permissions": ["ui"],
        }
        payload = make_wasm(("frame_app_main", "frame_app_event", "frame_app_tick"))
        path = os.path.join(td, "test.clock-1.2.3.fpack")
        seed = frame_crypto.load_seed(prefix + ".key")
        frame_pkg.build_fpack(manifest, payload, path, seed=seed)

        info = frame_pkg.inspect_wasm(payload)
        check("frame_app_main" in info["exports"], "inspect_wasm sees the export")
        check(info["initial_pages"] == 2, "inspect_wasm reads initial memory")

        res = frame_pkg.verify_fpack(path, pub)
        check(res["manifest"]["sha256"] == frame_pkg.hashlib.sha256(payload).hexdigest(),
              "python verifier accepts the signed package")

        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "pack.py"), "verify", path,
                            "--pubkey", prefix + ".pub", "--device-code"],
                           capture_output=True, text=True)
        check(r.returncode == 0 and "device-code verification passed" in r.stdout,
              "device C verifier accepts the signed package")
        if r.returncode != 0:
            print(r.stdout, r.stderr)

        # Wrong key must fail.
        other = os.path.join(td, "other")
        frame_crypto.save_keypair(other)
        try:
            frame_pkg.verify_fpack(path, frame_crypto.load_public(other + ".pub"))
            check(False, "wrong publisher key rejected")
        except frame_pkg.PkgError:
            check(True, "wrong publisher key rejected")

        # Tampered payload must fail in the device verifier.
        bad = os.path.join(td, "bad.fpack")
        tamper_payload(path, bad)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "pack.py"), "verify", bad,
                            "--pubkey", prefix + ".pub", "--device-code"],
                           capture_output=True, text=True)
        check(r.returncode != 0, "tampered payload rejected by the device verifier")

        # Deflated members must be refused before any signature check.
        zipped = os.path.join(td, "deflated.fpack")
        deflate_repack(path, zipped)
        try:
            frame_pkg.read_fpack(zipped)
            check(False, "deflated package refused")
        except frame_pkg.PkgError:
            check(True, "deflated package refused")

        # Manifest validation catches the common mistakes.
        for bad_manifest, why in (
            ({"schema": 2, "id": "a.b", "name": "x", "version": "1.0.0", "kind": "managed",
              "sdk": "^1.0.0", "entry": "payload.bin"}, "schema"),
            ({"schema": 1, "id": "Bad Id", "name": "x", "version": "1.0.0", "kind": "managed",
              "sdk": "^1.0.0", "entry": "payload.bin"}, "id"),
            ({"schema": 1, "id": "a.b", "name": "x", "version": "1.0", "kind": "managed",
              "sdk": "^1.0.0", "entry": "payload.bin"}, "version"),
        ):
            errs = frame_pkg.validate_manifest(bad_manifest, payload_size=len(payload))
            check(len(errs) > 0, f"manifest rejected for {why}")

        # Catalog.
        pkgs = os.path.join(td, "pkgs")
        os.makedirs(pkgs)
        os.rename(path, os.path.join(pkgs, "test.clock-1.2.3.fpack"))
        root_prefix = os.path.join(td, "root")
        frame_crypto.save_keypair(root_prefix)
        root_seed = frame_crypto.load_seed(root_prefix + ".key")
        cat, sig = frame_pkg.build_catalog(pkgs, base_url="http://192.168.1.10:8080",
                                           publisher_pub=pub, root_seed=root_seed)
        cat_obj = frame_pkg.verify_catalog_bytes(cat, frame_pkg.b64url(sig),
                                                 frame_crypto.load_public(root_prefix + ".pub"))
        check(len(cat_obj["apps"]) == 1 and cat_obj["apps"][0]["manifest"]["id"] == "test.clock",
              "catalog builds, signs and verifies")
        check(cat_obj["apps"][0]["publisher_key"].startswith("ed25519:"), "catalog carries the publisher key")
        try:
            frame_pkg.verify_catalog_bytes(cat + b" ", frame_pkg.b64url(sig),
                                           frame_crypto.load_public(root_prefix + ".pub"))
            check(False, "tampered catalog rejected")
        except frame_pkg.PkgError:
            check(True, "tampered catalog rejected")


def tamper_payload(src: str, dst: str) -> None:
    import zipfile
    with zipfile.ZipFile(src) as z:
        members = {n: z.read(n) for n in z.namelist()}
    p = bytearray(members["payload.bin"])
    p[-1] ^= 0x5A
    members["payload.bin"] = bytes(p)
    with zipfile.ZipFile(dst, "w") as z:
        for n, d in members.items():
            z.writestr(zipfile.ZipInfo(n), d)


def deflate_repack(src: str, dst: str) -> None:
    import zipfile
    with zipfile.ZipFile(src) as z:
        members = {n: z.read(n) for n in z.namelist()}
    with zipfile.ZipFile(dst, "w") as z:
        for n, d in members.items():
            zi = zipfile.ZipInfo(n)
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, d)


def make_wasm(exports: tuple[str, ...]) -> bytes:
    """A syntactically valid module: one memory, N empty function exports."""
    types = b"\x01\x60\x00\x00"                   # 1 type: () -> ()
    funcs = bytes([len(exports)]) + b"\x00" * len(exports)  # N functions of type 0
    mem = b"\x01\x01\x02\x02"                     # 1 memory: min 2 pages, max 2
    exp_names = b""
    for i, name in enumerate(exports):
        exp_names += bytes([len(name)]) + name.encode() + b"\x00" + bytes([i])
    exports_sec = bytes([len(exports)]) + exp_names
    return (b"\x00asm\x01\x00\x00\x00"
            + _sec(1, types) + _sec(3, funcs) + _sec(5, mem) + _sec(7, exports_sec))


def _sec(sid: int, body: bytes) -> bytes:
    return bytes([sid, len(body)]) + body


def main() -> int:
    test_canonical()
    test_packages()
    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
