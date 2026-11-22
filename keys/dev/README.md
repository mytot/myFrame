# Development keys — NOT FOR RELEASE

These keys exist so the dev store works out of the box:

| File | Signs | Used by |
|------|-------|---------|
| `store_root.key` / `.pub` | the catalog (`catalog.sig`) | `tools/catalog.py`, embedded in the firmware via `tools/gen_store_key_header.py` |
| `publisher.key` / `.pub` | individual `.fpack` packages | `tools/pack.py` |

Anyone can read these files, so anything signed with them proves nothing
about provenance — only integrity. That is exactly what you want while
developing against `tools/devstore.py` on a laptop.

For a release:

1. Generate real keys somewhere safe:
   `python3 tools/frame_crypto.py --gen /secure/path/store_root`
2. Build with the release root key:
   `python3 tools/build.py --release --store-key /secure/path/store_root.pub`
   (the build refuses a key under `keys/dev/`.)
3. Sign packages with the release publisher key. Catalog entries carry the
   publisher public key, so the device never needs the publisher key up front.
