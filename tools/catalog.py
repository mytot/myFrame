#!/usr/bin/env python3
"""
catalog.py — build and sign catalog.json + catalog.sig from a directory of
.fpack files, the way the device consumes them.

  python3 tools/catalog.py --pkgs dist --out store \\
                           --base-url http://192.168.1.10:8080 \\
                           --root keys/dev/store_root --publisher keys/dev/publisher

writes store/catalog.json and store/catalog.sig. Each entry carries the app's
manifest inline, the download URL, size, sha256, the publisher's public key and
a per-entry signature; catalog.sig is the store root key's signature over the
exact catalog.json bytes (what the device verifies before trusting anything).

  python3 tools/catalog.py --verify store/catalog.json keys/dev/store_root.pub

The dev store (tools/devstore.py) runs this automatically on start and after
every upload, so in day-to-day development you never invoke it directly.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_crypto  # noqa: E402
import frame_pkg  # noqa: E402

REPO_ROOT = os.path.dirname(HERE)
DEFAULT_PUBLISHER = os.path.join(REPO_ROOT, "keys", "dev", "publisher")
DEFAULT_ROOT = os.path.join(REPO_ROOT, "keys", "dev", "store_root")


def cmd_build(args) -> int:
    root_seed = frame_crypto.load_seed(args.root + ".key")
    publisher_pub = None
    if not args.no_publisher_key:
        if args.publisher.endswith(".pub"):
            publisher_pub = frame_pkg.load_pub(args.publisher)
        elif os.path.exists(args.publisher + ".pub"):
            publisher_pub = frame_pkg.load_pub(args.publisher + ".pub")
        else:
            seed = frame_crypto.load_seed(args.publisher + ".key")
            publisher_pub = frame_crypto.public_from_seed(seed)
    out = frame_pkg.write_catalog(args.pkgs, args.out, base_url=args.base_url,
                                  publisher_pub=publisher_pub, root_seed=root_seed)
    with open(out, "rb") as f:
        cat = json.loads(f.read().decode("utf-8"))
    print(f"catalog: {len(cat['apps'])} app(s) -> {out}")
    for e in cat["apps"]:
        m = e["manifest"]
        print(f"  {m['id']:<28} {m['version']:<8} {m['kind']:<8} {e['size']:>8} B  {e['download_url']}")
    return 0


def cmd_verify(args) -> int:
    with open(args.catalog, "rb") as f:
        cat_bytes = f.read()
    sig_path = args.sig or os.path.splitext(args.catalog)[0] + ".sig"
    with open(sig_path, "r", encoding="ascii") as f:
        sig_b64 = f.read()
    root_pub = frame_pkg.load_pub(args.pubkey)
    cat = frame_pkg.verify_catalog_bytes(cat_bytes, sig_b64, root_pub)
    print(f"OK  catalog signature valid for {len(cat['apps'])} app(s)")
    for e in cat["apps"]:
        m = e["manifest"]
        fpack = os.path.join(os.path.dirname(args.catalog), "pkgs", os.path.basename(e["download_url"]))
        if os.path.exists(fpack):
            pub_text = e.get("publisher_key", "")
            pub = None
            if pub_text:
                import base64
                t = pub_text.split(":", 1)[1] if ":" in pub_text else pub_text
                pub = base64.urlsafe_b64decode(t + "=" * (-len(t) % 4))
            else:
                pub = root_pub
            frame_pkg.verify_fpack(fpack, pub)
            print(f"    {m['id']} {m['version']}: package signature valid")
        else:
            print(f"    {m['id']} {m['version']}: package not found at {fpack} (skipped)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("--pkgs", required=True, help="directory containing .fpack files")
    b.add_argument("--out", required=True, help="output directory for catalog.json/.sig")
    b.add_argument("--base-url", required=True, help="public base URL; /pkgs/<name> is appended")
    b.add_argument("--root", default=DEFAULT_ROOT, help="store root key prefix")
    b.add_argument("--publisher", default=DEFAULT_PUBLISHER,
                   help="publisher key prefix or .pub (default keys/dev/publisher)")
    b.add_argument("--no-publisher-key", action="store_true",
                   help="omit publisher_key (packages must then be root-signed)")
    b.set_defaults(fn=cmd_build)

    v = sub.add_parser("verify")
    v.add_argument("catalog")
    v.add_argument("pubkey", help="store root *.pub")
    v.add_argument("--sig", help="catalog.sig path (default: next to catalog)")
    v.set_defaults(fn=cmd_verify)

    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (frame_pkg.PkgError, FileNotFoundError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
