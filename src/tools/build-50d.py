#!/usr/bin/env python3
"""
Build the EOS 50D Magic Lantern autoexec with the Soup Film menu.

The Magic Lantern tree is not part of this repo. This checks out the
pinned commit, copies integrations/magiclantern/50D.109 over it, and
builds platform/50D.109. The engine sources stay in src/engine and the
platform Makefile finds them from that checkout.

  python3 tools/build-50d.py
  python3 tools/build-50d.py --card I:\\

Do not use tools/patch-film.py for the 50D. That plants a GUI hook the
body does not survive. See ml/platforms/50D.109/PITFALLS.md.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
ROOT = os.path.dirname(SRC)
OVERLAY = os.path.join(ROOT, "integrations", "magiclantern", "50D.109")
ML = os.path.join(SRC, "vendor", "magiclantern")
PIN_FILE = os.path.join(OVERLAY, "SOUP-PIN.txt")
REMOTE = "https://github.com/reticulatedpines/magiclantern_simplified"


def die(msg):
    sys.stderr.write("build-50d: %s\n" % msg)
    raise SystemExit(1)


def run(cmd, cwd=None, env=None):
    sys.stderr.write("+ %s\n" % " ".join(cmd))
    r = subprocess.call(cmd, cwd=cwd, env=env)
    if r != 0:
        die("command failed (%d)" % r)


def pin_commit():
    if not os.path.isfile(PIN_FILE):
        die("missing %s" % PIN_FILE)
    for line in open(PIN_FILE, "r", encoding="utf-8"):
        line = line.strip()
        if line.lower().startswith("commit "):
            return line.split()[1]
    die("no Commit line in %s" % PIN_FILE)


def git(args, cwd):
    run(["git"] + args, cwd=cwd)


def ensure_tree(commit):
    if not os.path.isdir(os.path.join(ML, ".git")):
        os.makedirs(os.path.dirname(ML), exist_ok=True)
        run(["git", "clone", REMOTE, ML])
        git(["checkout", "--detach", commit], ML)
    elif not os.path.isfile(os.path.join(ML, "src", "film_menu.c")):
        git(["fetch", "--depth", "1", "origin", commit], ML)
        git(["checkout", "--detach", "FETCH_HEAD"], ML)


def copy_overlay():
    if not os.path.isdir(OVERLAY):
        die("missing overlay %s" % OVERLAY)
    for dirpath, _dirs, files in os.walk(OVERLAY):
        rel = os.path.relpath(dirpath, OVERLAY)
        for name in files:
            src = os.path.join(dirpath, name)
            dst = os.path.join(ML, rel, name) if rel != "." else os.path.join(ML, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)


def arm_binpath():
    env = os.environ.get("ARM_BINPATH")
    if env and os.path.isfile(os.path.join(env, "arm-none-eabi-gcc")):
        return env
    candidates = [
        r"C:\PROGRA~2\ARMGNU~1\14EFD8~1.2RE\bin",
        "/usr/bin",
    ]
    for c in candidates:
        if os.path.isfile(os.path.join(c, "arm-none-eabi-gcc")) or \
           os.path.isfile(os.path.join(c, "arm-none-eabi-gcc.exe")):
            return c
    die("set ARM_BINPATH to the directory that contains arm-none-eabi-gcc")


def build():
    plat = os.path.join(ML, "platform", "50D.109")
    env = os.environ.copy()
    arm = arm_binpath()
    extra = [arm]
    git_usr = r"C:\Program Files\Git\usr\bin"
    if os.path.isdir(git_usr):
        extra.append(git_usr)
    env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    run([
        "make",
        "ARM_BINPATH=" + arm.replace("\\", "/"),
        "PYTHON=" + sys.executable.replace("\\", "/"),
        "build/autoexec.bin",
        "build/50D_109.sym",
    ], cwd=plat, env=env)
    auto = os.path.join(plat, "build", "autoexec.bin")
    sym = os.path.join(plat, "build", "50D_109.sym")
    if not os.path.isfile(auto) or not os.path.isfile(sym):
        die("build did not produce autoexec.bin and 50D_109.sym")
    return auto, sym


def copy_card(card, auto, sym):
    root = card.rstrip("\\/")
    if not os.path.isdir(root):
        die("card not found: %s" % card)
    shutil.copy2(auto, os.path.join(root, "AUTOEXEC.BIN"))
    mod = os.path.join(root, "ML", "modules")
    os.makedirs(mod, exist_ok=True)
    shutil.copy2(sym, os.path.join(mod, "50D_109.sym"))
    film = os.path.join(root, "ML", "FILM")
    os.makedirs(film, exist_ok=True)
    stocks = os.path.join(SRC, "stocks")
    for name in os.listdir(stocks):
        if name.endswith(".fs") or name == "INDEX.TXT":
            shutil.copy2(os.path.join(stocks, name), os.path.join(film, name))
    sys.stderr.write("copied AUTOEXEC.BIN, 50D_109.sym, and ML/FILM to %s\n" % root)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--card", help="copy AUTOEXEC.BIN, the sym, and ML/FILM here")
    args = ap.parse_args()
    commit = pin_commit()
    ensure_tree(commit)
    copy_overlay()
    auto, sym = build()
    if args.card:
        copy_card(args.card, auto, sym)
    else:
        sys.stderr.write("built %s\nbuilt %s\n" % (auto, sym))


if __name__ == "__main__":
    main()
