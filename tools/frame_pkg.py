#!/usr/bin/env python3
"""
frame_pkg.py — the package format in one place.

Shared by tools/pack.py (build + sign), tools/catalog.py (catalog from
packages), tools/devstore.py (serve) and tools/build_app.py (managed apps).
Nothing here imports third-party modules, so the tools run on a bare laptop.

.fpack: a ZIP (STORED members only — the device has no inflate) holding

    manifest.json     the contract (stored in canonical form: that is what is signed)
    payload.bin       native: ESP-IDF app image; managed: .wasm module
    signature.bin     Ed25519 over canonical(manifest) || payload
    icon.png          optional, 128x128
    assets/*          optional, flat, extracted to /apps/<id>/assets/

The byte layout that gets signed is exactly what components/frame_store/install.c
recomputes on the device: frame_canonicalise(manifest.json) || payload bytes.
"""
from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import re
import struct
import sys
import zipfile
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import frame_crypto  # noqa: E402  (pure-Python Ed25519)

REPO_ROOT = os.path.dirname(HERE)
DEV_PUBLISHER = os.path.join(REPO_ROOT, "keys", "dev", "publisher")
DEV_STORE_ROOT = os.path.join(REPO_ROOT, "keys", "dev", "store_root")

ID_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+)+$")
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")
KINDS = ("native", "managed")
MANIFEST_MAX_BYTES = 8192
CATALOG_MAX_BYTES = 256 * 1024


class PkgError(Exception):
    pass


# ----------------------------------------------------------------------------
# Canonical JSON — must match components/frame_pure/canonical.c byte for byte
# ----------------------------------------------------------------------------

def canonical_json(obj: Any) -> bytes:
    """Sorted keys, no whitespace, raw UTF-8, only the escapes JSON requires."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")


def canonical_manifest_bytes(manifest_text: bytes) -> bytes:
    return canonical_json(json.loads(manifest_text.decode("utf-8")))


# ----------------------------------------------------------------------------
# Manifest validation — mirrors frame_manifest_parse() + the device's checks
# ----------------------------------------------------------------------------

def validate_manifest(m: dict, *, payload_size: int | None = None) -> list[str]:
    """Returns a list of problems; empty means the device will accept the manifest."""
    errs: list[str] = []
    if m.get("schema") != 1:
        errs.append("schema must be 1")
    for key, limit in (("id", 64), ("name", 48), ("version", 24), ("sdk", 32),
                       ("entry", 48), ("description", 192), ("author", 48),
                       ("homepage", 96), ("release_notes", 192)):
        v = m.get(key)
        if key in ("id", "name", "version", "sdk", "entry") and not isinstance(v, str):
            errs.append(f"{key} is required")
            continue
        if isinstance(v, str) and len(v.encode("utf-8")) >= limit:
            errs.append(f"{key} longer than {limit - 1} bytes")
    if isinstance(m.get("id"), str) and not ID_RE.match(m["id"]):
        errs.append("id must be reverse-DNS, lowercase: letters, digits, dots, underscores")
    if isinstance(m.get("version"), str) and not SEMVER_RE.match(m["version"]):
        errs.append("version must be strict semver MAJOR.MINOR.PATCH")
    if m.get("kind") not in KINDS:
        errs.append("kind must be 'native' or 'managed'")
    perms = m.get("permissions")
    if perms is not None:
        if not isinstance(perms, list) or len(perms) > 8 or not all(isinstance(p, str) for p in perms):
            errs.append("permissions must be a list of at most 8 strings")
        else:
            for p in perms:
                if len(p) >= 24:
                    errs.append(f"permission '{p}' longer than 23 characters")
    req = m.get("requires")
    if req is not None:
        if not isinstance(req, dict):
            errs.append("requires must be an object")
        else:
            chips = req.get("chip", [])
            if not isinstance(chips, list) or len(chips) > 4 or not all(isinstance(c, str) and len(c) < 16 for c in chips):
                errs.append("requires.chip must be a list of at most 4 strings")
    if payload_size is not None and m.get("size") not in (None, payload_size):
        errs.append(f"size {m.get('size')} does not match payload {payload_size}")
    return errs


def finalise_manifest(m: dict, payload: bytes) -> dict:
    """Fills in size/sha256 from the payload and validates the result."""
    out = dict(m)
    out["size"] = len(payload)
    out["sha256"] = hashlib.sha256(payload).hexdigest()
    errs = validate_manifest(out, payload_size=len(payload))
    if errs:
        raise PkgError("invalid manifest:\n  " + "\n  ".join(errs))
    return out


# ----------------------------------------------------------------------------
# Keys
# ----------------------------------------------------------------------------

def b64url(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).decode("ascii").rstrip("=")


def load_pub(path: str) -> bytes:
    """Accepts a 32-byte raw *.pub file or a base64url text file."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) == 32:
        return raw
    text = raw.decode("ascii", "strict").strip()
    if text.startswith("ed25519:"):
        text = text[len("ed25519:"):]
    text += "=" * (-len(text) % 4)
    key = base64.urlsafe_b64decode(text)
    if len(key) != 32:
        raise PkgError(f"{path}: expected a 32-byte Ed25519 public key")
    return key


def resolve_signer(publisher: str | None, root: str | None, unsigned: bool):
    """
    Decides which key signs. Rules, in order:
      --unsigned           -> (None, None)  (only for CONFIG_FRAME_STORE_ALLOW_UNSIGNED devices)
      --publisher <prefix> -> that publisher key signs; its pub goes in the catalog
      default              -> keys/dev/publisher if present, else keys/dev/store_root
    Sideloaded packages (no catalog) must verify against the *store root* key,
    so a package meant for `POST /api/apps/install` must be signed with --root.
    """
    if unsigned:
        return None, None
    if publisher:
        seed = frame_crypto.load_seed(publisher + ".key")
        pub = frame_crypto.public_from_seed(seed)
        return seed, pub
    if root:
        seed = frame_crypto.load_seed(root + ".key")
        return seed, frame_crypto.public_from_seed(seed)
    for cand in (DEV_PUBLISHER, DEV_STORE_ROOT):
        if os.path.exists(cand + ".key"):
            seed = frame_crypto.load_seed(cand + ".key")
            return seed, frame_crypto.public_from_seed(seed)
    raise PkgError("no signing key: pass --publisher keys/<name> or --unsigned")


# ----------------------------------------------------------------------------
# Package
# ----------------------------------------------------------------------------

def sign_package(seed: bytes, canonical: bytes, payload: bytes) -> bytes:
    return frame_crypto.sign(canonical + payload, seed)


def build_fpack(manifest: dict, payload: bytes, out_path: str, *, seed: bytes | None,
                icon: bytes | None = None, assets: dict[str, bytes] | None = None) -> dict:
    """Writes a signed .fpack and returns the manifest that went into it."""
    m = finalise_manifest(manifest, payload)
    canonical = canonical_json(m)
    if len(canonical) >= MANIFEST_MAX_BYTES:
        raise PkgError(f"canonical manifest is {len(canonical)} bytes; the device allows "
                       f"{MANIFEST_MAX_BYTES - 1}")
    signature = b"" if seed is None else sign_package(seed, canonical, payload)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    tmp = out_path + ".tmp"
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr(zipfile.ZipInfo("manifest.json"), canonical)
        zi = zipfile.ZipInfo("payload.bin")
        zi.compress_type = zipfile.ZIP_STORED
        z.writestr(zi, payload)
        if seed is not None:
            z.writestr(zipfile.ZipInfo("signature.bin"), signature)
        if icon:
            z.writestr(zipfile.ZipInfo("icon.png"), icon)
        for name, data in sorted((assets or {}).items()):
            if "/" in name or ".." in name or name.startswith("."):
                raise PkgError(f"asset name must be flat and plain: {name}")
            z.writestr(zipfile.ZipInfo("assets/" + name), data)
    os.replace(tmp, out_path)
    return m


def read_fpack(path: str) -> tuple[dict, bytes, bytes, dict[str, bytes]]:
    """Returns (manifest, payload, signature or b'', assets). Rejects deflate."""
    with zipfile.ZipFile(path, "r") as z:
        for info in z.infolist():
            if info.compress_type != zipfile.ZIP_STORED:
                raise PkgError(f"{info.filename} is compressed; the device only reads STORED members "
                               "(build with tools/pack.py)")
        names = set(z.namelist())
        if "manifest.json" not in names or "payload.bin" not in names:
            raise PkgError("package must contain manifest.json and payload.bin")
        mtext = z.read("manifest.json")
        if len(mtext) >= MANIFEST_MAX_BYTES:
            raise PkgError("manifest.json too large")
        m = json.loads(mtext.decode("utf-8"))
        payload = z.read("payload.bin")
        sig = z.read("signature.bin") if "signature.bin" in names else b""
        assets = {n[len("assets/"):]: z.read(n) for n in names if n.startswith("assets/")}
    return m, payload, sig, assets


def verify_fpack(path: str, pub: bytes, *, strict: bool = True) -> dict:
    """
    Verifies a package the way the device does: canonical manifest, payload
    digest, Ed25519 over canonical || payload. Raises PkgError on failure.
    Returns {"manifest": m, "payload_sha256": hex, "signature_b64": ...}.
    """
    m, payload, sig, _ = read_fpack(path)
    errs = validate_manifest(m, payload_size=len(payload))
    if strict and errs:
        raise PkgError("manifest rejected:\n  " + "\n  ".join(errs))
    if m.get("sha256") and m["sha256"] != hashlib.sha256(payload).hexdigest():
        raise PkgError("payload sha256 does not match manifest")
    if not sig:
        raise PkgError("package is unsigned (signature.bin missing)")
    canonical = canonical_manifest_bytes(canonical_json(m))
    if not frame_crypto.verify(canonical + payload, sig, pub):
        raise PkgError("signature does not verify against the given public key")
    return {"manifest": m, "payload_sha256": m["sha256"], "signature_b64": b64url(sig)}


# ----------------------------------------------------------------------------
# WASM inspection — enough of the binary format to refuse a module the device
# would refuse, before it ever reaches the device.
# ----------------------------------------------------------------------------

def inspect_wasm(data: bytes) -> dict:
    if len(data) < 8 or data[:4] != b"\0asm":
        raise PkgError("payload is not a WebAssembly module")
    if struct.unpack("<I", data[4:8])[0] != 1:
        raise PkgError("unsupported wasm version")

    pos = 8
    exports: list[str] = []
    imports: list[tuple[str, str]] = []
    initial_pages = max_pages = None
    while pos < len(data):
        section_id = data[pos]
        pos += 1
        size, pos = _leb_u(data, pos)
        end = pos + size
        if section_id == 2:  # imports
            n, p = _leb_u(data, pos)
            for _ in range(n):
                mod_len, p = _leb_u(data, p)
                mod = data[p:p + mod_len].decode("utf-8", "replace")
                p += mod_len
                name_len, p = _leb_u(data, p)
                name = data[p:p + name_len].decode("utf-8", "replace")
                p += name_len
                kind = data[p]
                p += 1
                if kind == 0x00:  # func: type index
                    _, p = _leb_u(data, p)
                elif kind == 0x02:  # memory
                    flags = data[p]
                    p += 1
                    _, p = _leb_u(data, p)
                    if flags & 1:
                        _, p = _leb_u(data, p)
                imports.append((mod, name))
        elif section_id == 5:  # memory
            count, p = _leb_u(data, pos)
            if count:
                flags = data[p]
                p += 1
                initial_pages, p = _leb_u(data, p)
                if flags & 1:
                    max_pages, p = _leb_u(data, p)
        elif section_id == 7:  # exports
            n, p = _leb_u(data, pos)
            for _ in range(n):
                name_len, p = _leb_u(data, p)
                exports.append(data[p:p + name_len].decode("utf-8", "replace"))
                p += name_len
                p += 2  # kind + index
        pos = end

    return {
        "exports": exports,
        "imports": imports,
        "initial_pages": initial_pages,
        "max_pages": max_pages,
        "initial_bytes": None if initial_pages is None else initial_pages * 65536,
        "max_bytes": None if max_pages is None else max_pages * 65536,
    }


def _leb_u(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise PkgError("truncated wasm section")
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7


# ----------------------------------------------------------------------------
# Catalog
# ----------------------------------------------------------------------------

def catalog_entry(fpack_path: str, download_url: str, *, publisher_pub: bytes | None,
                  root_seed: bytes | None, include_signature: bool = True) -> dict:
    m, payload, sig, _ = read_fpack(fpack_path)
    entry = {
        "manifest": m,
        "download_url": download_url,
        "size": len(payload),
        "sha256": m.get("sha256") or hashlib.sha256(payload).hexdigest(),
    }
    if publisher_pub is not None:
        entry["publisher_key"] = "ed25519:" + b64url(publisher_pub)
        entry["publisher_alg"] = "ed25519"
    if include_signature and root_seed is not None:
        # Per-entry signature over the canonical entry without itself. v1 of the
        # device verifies the package and the catalog signature; this field is
        # what a later client will check to spot a tampered catalog entry.
        body = canonical_json({k: v for k, v in entry.items()})
        entry["signature"] = b64url(frame_crypto.sign(body, root_seed))
    return entry


def build_catalog(pkgs_dir: str, *, base_url: str, publisher_pub: bytes | None,
                  root_seed: bytes, include_signature: bool = True) -> tuple[bytes, bytes]:
    entries = []
    for name in sorted(os.listdir(pkgs_dir)):
        if not name.endswith(".fpack"):
            continue
        entries.append(catalog_entry(os.path.join(pkgs_dir, name), f"{base_url.rstrip('/')}/pkgs/{name}",
                                     publisher_pub=publisher_pub, root_seed=root_seed,
                                     include_signature=include_signature))
    if not entries:
        raise PkgError(f"no .fpack files in {pkgs_dir}")
    cat = {
        "schema": 1,
        "generated_at": _utc_now_iso(),
        "apps": entries,
    }
    cat_bytes = canonical_json(cat)
    if len(cat_bytes) > CATALOG_MAX_BYTES:
        raise PkgError(f"catalog is {len(cat_bytes)} bytes; the device caps it at {CATALOG_MAX_BYTES}")
    sig = frame_crypto.sign(cat_bytes, root_seed)
    return cat_bytes, sig


def _utc_now_iso() -> str:
    import datetime
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def write_catalog(pkgs_dir: str, out_dir: str, *, base_url: str, publisher_pub: bytes | None,
                  root_seed: bytes) -> str:
    cat_bytes, sig = build_catalog(pkgs_dir, base_url=base_url, publisher_pub=publisher_pub,
                                   root_seed=root_seed)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "catalog.json"), "wb") as f:
        f.write(cat_bytes)
    with open(os.path.join(out_dir, "catalog.sig"), "wb") as f:
        f.write(b64url(sig).encode("ascii"))
    return os.path.join(out_dir, "catalog.json")


def verify_catalog_bytes(cat: bytes, sig_b64: str, root_pub: bytes) -> dict:
    sig_text = sig_b64.strip()
    sig_text += "=" * (-len(sig_text) % 4)
    sig = base64.urlsafe_b64decode(sig_text)
    if not frame_crypto.verify(cat, sig, root_pub):
        raise PkgError("catalog signature does not verify against the store root key")
    cat_obj = json.loads(cat.decode("utf-8"))
    if cat_obj.get("schema") != 1:
        raise PkgError("unsupported catalog schema")
    return cat_obj


if __name__ == "__main__":
    print(__doc__)
