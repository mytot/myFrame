#!/usr/bin/env python3
"""
frame_crypto.py — Ed25519 signing for the packaging tools (RFC 8032).

Pure Python, no third-party dependencies, so `pack.py` and `catalog.py` run on
any laptop. Signing is slow (~0.3 s per signature on a modern CPU) because it
uses Python big integers instead of optimised field arithmetic — that is fine
for a build tool, and the device-side verifier (components/frame_pure/ed25519.c)
is the same algorithm, host-tested against the same RFC vectors.

Key files:
    <name>.pub   32-byte public key, raw binary
    <name>.key   64 bytes: 32-byte seed || 32-byte public key (RFC 8032 format)

Never commit a .key file for anything but the dev store.
"""
from __future__ import annotations

import hashlib
import os
import secrets

# ----------------------------------------------------------------------------
# Curve constants
# ----------------------------------------------------------------------------

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _inv(x: int) -> int:
    return pow(x, P - 2, P)


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * _inv(D * y * y + 1)
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0:
        x = (x * SQRT_M1) % P
    if x % 2 != 0:
        x = P - x
    return x


BY = (4 * _inv(5)) % P
BX = _xrecover(BY)
BASE = (BX, BY, 1, (BX * BY) % P)
IDENTITY = (0, 1, 1, 0)


# ----------------------------------------------------------------------------
# Group operations (extended coordinates, a = -1)
# ----------------------------------------------------------------------------

def _edwards_add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = ((y1 - x1) * (y2 - x2)) % P
    b = ((y1 + x1) * (y2 + x2)) % P
    c = (t1 * 2 * D * t2) % P
    dd = (z1 * 2 * z2) % P
    e, f, g, h = b - a, dd - c, dd + c, b + a
    return ((e * f) % P, (g * h) % P, (f * g) % P, (e * h) % P)


def _scalarmult(p, e: int):
    if e == 0:
        return IDENTITY
    q = _scalarmult(p, e // 2)
    q = _edwards_add(q, q)
    return _edwards_add(q, p) if e & 1 else q


def _encodepoint(p) -> bytes:
    x, y, z, _t = p
    zi = _inv(z)
    x = (x * zi) % P
    y = (y * zi) % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decodepoint(s: bytes):
    if len(s) != 32:
        raise ValueError("public key must be 32 bytes")
    y = int.from_bytes(s, "little") & ((1 << 255) - 1)
    x = _xrecover(y)
    if x & 1 != (s[31] >> 7):
        x = P - x
    point = (x, y, 1, (x * y) % P)
    if not _on_curve(point):
        raise ValueError("public key is not a curve point")
    return point


def _on_curve(p) -> bool:
    x, y, z, t = p
    return (z % P != 0 and (x * y - z * t) % P == 0
            and (y * y - x * x - z * z - D * t * t) % P == 0)


def _hint(m: bytes) -> int:
    return int.from_bytes(hashlib.sha512(m).digest(), "little") % L


def _secret_expand(seed: bytes):
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = 2 ** 254 + sum(2 ** i * ((h[i // 8] >> (i % 8)) & 1) for i in range(3, 254))
    return a, h[32:]


# ----------------------------------------------------------------------------
# Public API
# ----------------------------------------------------------------------------

def public_from_seed(seed: bytes) -> bytes:
    a, _prefix = _secret_expand(seed)
    return _encodepoint(_scalarmult(BASE, a))


def sign(message: bytes, seed: bytes, public: bytes | None = None) -> bytes:
    """Detached Ed25519 signature (64 bytes: R || S)."""
    if public is None:
        public = public_from_seed(seed)
    a, prefix = _secret_expand(seed)
    r = _hint(prefix + message)
    big_r = _encodepoint(_scalarmult(BASE, r))
    s = (r + _hint(big_r + public + message) * a) % L
    return big_r + int.to_bytes(s, 32, "little")


def verify(message: bytes, signature: bytes, public: bytes) -> bool:
    if len(signature) != 64:
        return False
    try:
        a_point = _decodepoint(public)
    except ValueError:
        return False
    big_r = signature[:32]
    s = int.from_bytes(signature[32:], "little")
    if s >= L:
        return False
    h = _hint(big_r + public + message)
    sb = _scalarmult(BASE, s)
    ha = _scalarmult(a_point, h)
    neg_ha = ((P - ha[0]) % P, ha[1], ha[2], (P - ha[3]) % P)
    return _encodepoint(_edwards_add(sb, neg_ha)) == big_r


# ----------------------------------------------------------------------------
# Key files
# ----------------------------------------------------------------------------

def generate_keypair():
    seed = secrets.token_bytes(32)
    return seed, public_from_seed(seed)


def save_keypair(path_prefix: str):
    """Writes <prefix>.key (seed||pub) and <prefix>.pub. Refuses to overwrite."""
    key_path = path_prefix + ".key"
    pub_path = path_prefix + ".pub"
    if os.path.exists(key_path) or os.path.exists(pub_path):
        raise FileExistsError(f"refusing to overwrite {key_path} / {pub_path}")
    seed, public = generate_keypair()
    parent = os.path.dirname(os.path.abspath(key_path))
    os.makedirs(parent, exist_ok=True)
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(seed + public)
    with open(pub_path, "wb") as f:
        f.write(public)
    return seed, public


def load_public(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) == 64:
        data = data[32:]          # seed||pub form
    if len(data) != 32:
        raise ValueError(f"{path}: expected 32-byte public key, got {len(data)} bytes")
    return data


def load_seed(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != 64:
        raise ValueError(f"{path}: expected 64-byte key file (seed||pub), got {len(data)} bytes")
    seed, public = data[:32], data[32:]
    if public_from_seed(seed) != public:
        raise ValueError(f"{path}: public key does not match seed")
    return seed


# ----------------------------------------------------------------------------
# Self-test
# ----------------------------------------------------------------------------

def _cli(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Ed25519 key management for Frame packaging")
    sub = ap.add_subparsers(dest="cmd")
    g = sub.add_parser("gen", help="generate <prefix>.key and <prefix>.pub")
    g.add_argument("prefix")
    g.add_argument("--force", action="store_true", help="overwrite an existing key pair")
    v = sub.add_parser("pub", help="print the public key of a key file")
    v.add_argument("path")
    sub.add_parser("selftest", help="run the RFC 8032 self-test")
    args = ap.parse_args(argv)

    if args.cmd == "gen":
        if args.force:
            for ext in (".key", ".pub"):
                try:
                    os.remove(args.prefix + ext)
                except FileNotFoundError:
                    pass
        _seed, public = save_keypair(args.prefix)
        print(f"wrote {args.prefix}.key (secret) and {args.prefix}.pub")
        print(f"public key: {public.hex()}")
        return 0
    if args.cmd == "pub":
        print(load_public(args.path).hex())
        return 0
    if args.cmd == "selftest":
        _selftest()
        return 0
    ap.print_help()
    return 2


def _selftest():
    # RFC 8032 §7.1 test vector 1
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    pk = public_from_seed(seed)
    assert pk.hex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
    sig = sign(b"", seed, pk)
    assert sig.hex() == ("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
    assert verify(b"", sig, pk)
    assert not verify(b"x", sig, pk)
    print("frame_crypto: ok")


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1:
        sys.exit(_cli(sys.argv[1:]))
    _selftest()
