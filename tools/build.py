#!/usr/bin/env python3
"""
build.py — the one supported way to build the shell firmware.

  python3 tools/build.py                       # debug build, default board
  python3 tools/build.py --board sunton_esp32_8048s070
  python3 tools/build.py --release --key /safe/place/store_root.pub
  python3 tools/build.py --flash               # build + flash + monitor

--release enforces the rules a release image must satisfy, so they cannot be
forgotten at the end:

  * the store root key does NOT come from keys/dev/
  * CONFIG_FRAME_STORE_ALLOW_UNSIGNED is off (a cached sdkconfig with it on is
    rejected, not silently fixed)
  * CONFIG_FRAME_HTTP_API_ENABLE is off (bench API, no authentication)
  * a clean build directory, so no stale objects from a debug build leak in

Everything else is a thin wrapper around idf.py: the board, sdkconfig layering,
partition table and generated headers are handled by CMake (see
tools/cmake/frame_project.cmake), not here.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
DEV_KEY_DIR = os.path.join(REPO_ROOT, "keys", "dev")
KNOWN_BOARDS = sorted(d for d in os.listdir(os.path.join(REPO_ROOT, "devices"))
                      if os.path.isdir(os.path.join(REPO_ROOT, "devices", d)))


def have(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def check_release(build_dir: str, key: str) -> list[str]:
    problems = []
    key_abs = os.path.abspath(key)
    if key_abs.startswith(os.path.abspath(DEV_KEY_DIR) + os.sep):
        problems.append(f"--key {key} is a development key; a release must pin a key kept out of the repo")
    cfg = os.path.join(build_dir, "config", "sdkconfig.h")
    sdkconfig = os.path.join(build_dir, "sdkconfig")
    if os.path.exists(sdkconfig):
        with open(sdkconfig, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "CONFIG_FRAME_STORE_ALLOW_UNSIGNED=y" in text:
            problems.append("sdkconfig has CONFIG_FRAME_STORE_ALLOW_UNSIGNED=y; "
                            "a release must verify signatures")
        if "CONFIG_FRAME_HTTP_API_ENABLE=y" in text:
            problems.append("sdkconfig has CONFIG_FRAME_HTTP_API_ENABLE=y; "
                            "the unauthenticated bench API must be off in a release")
    elif os.path.exists(cfg):
        with open(cfg, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "#define CONFIG_FRAME_STORE_ALLOW_UNSIGNED 1" in text:
            problems.append("sdkconfig has CONFIG_FRAME_STORE_ALLOW_UNSIGNED=1; "
                            "a release must verify signatures")
        if "#define CONFIG_FRAME_HTTP_API_ENABLE 1" in text:
            problems.append("sdkconfig has CONFIG_FRAME_HTTP_API_ENABLE=1; "
                            "the unauthenticated bench API must be off in a release")
    return problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="sunton_esp32_8048s070", choices=KNOWN_BOARDS)
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--key", help="store root public key (*.pub) to pin (default keys/dev/store_root.pub)")
    ap.add_argument("--build-dir", help="default build/ or build-release/")
    ap.add_argument("--flash", action="store_true", help="build, then flash and open the monitor")
    ap.add_argument("--port", help="serial port for --flash")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--size", action="store_true", help="print idf.py size after building")
    ap.add_argument("-D", dest="defines", action="append", default=[],
                    help="extra -D for CMake (repeatable)")
    args = ap.parse_args(argv)

    if not have("idf.py"):
        print("error: idf.py not on PATH. Run `. $IDF_PATH/export.sh` for ESP-IDF v6 first.",
              file=sys.stderr)
        return 1

    build_dir = args.build_dir or os.path.join(REPO_ROOT, "build-release" if args.release else "build")
    key = args.key or os.path.join(DEV_KEY_DIR, "store_root.pub")

    if args.release:
        problems = check_release(build_dir, key)
        if problems:
            print("release build refused:", file=sys.stderr)
            for p in problems:
                print(f"  - {p}", file=sys.stderr)
            return 2
        if args.clean or not os.path.isdir(build_dir):
            pass
        else:
            # Force a clean tree for releases unless the caller reuses a
            # directory that already passed the checks above.
            print(f"[build] reusing {build_dir} (passed release checks)")
    if args.clean and os.path.isdir(build_dir):
        shutil.rmtree(build_dir)
        print(f"[build] removed {build_dir}")

    cmd = ["idf.py", "-B", build_dir, f"-DBOARD={args.board}",
           f"-DFRAME_STORE_KEY_FILE={os.path.abspath(key)}"]
    if args.release:
        cmd.append("-DFRAME_RELEASE=1")
    if args.flash:
        cmd.append(f"-p{args.port}" if args.port else "")
        cmd = [c for c in cmd if c]
    for d in args.defines:
        cmd.append(f"-D{d}")
    cmd.append("build")
    print("[build] " + " ".join(cmd))
    rc = subprocess.call(cmd, cwd=REPO_ROOT)
    if rc != 0:
        return rc

    if args.size or args.release:
        subprocess.call(["idf.py", "-B", build_dir, "size"], cwd=REPO_ROOT)

    if args.release:
        problems = check_release(build_dir, key)
        if problems:
            print("release build produced a non-compliant configuration:", file=sys.stderr)
            for p in problems:
                print(f"  - {p}", file=sys.stderr)
            return 2
        print(f"[build] release image: {os.path.join(build_dir, 'myframe.bin')}")

    if args.flash:
        cmd = ["idf.py", "-B", build_dir, "-p", args.port, "flash", "monitor"] if args.port else \
              ["idf.py", "-B", build_dir, "flash", "monitor"]
        return subprocess.call(cmd, cwd=REPO_ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
