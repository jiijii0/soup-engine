#!/usr/bin/env python3
"""
Patch the soup engine into a Magic Lantern AUTOEXEC.BIN for the 5D Classic.

Platform constants (vendor path, hole addresses, payload base, checksum)
live in ml/platforms/<name>/patch.json. The stub sources sit beside that
file. Shared payload sources stay in ml/payload/.

  python3 tools/patch-film.py --platform 5DC.111

This is the Classic path only. The 50D does not survive the GUI hook;
build that body with tools/build-50d.py instead.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAYDIR = os.path.join(ROOT, "ml", "payload")
OUTDIR = os.path.join(ROOT, "ml", "build")
CARD = os.path.join(os.path.dirname(ROOT), "card")

# Shared across platforms: which entry stubs may switch stacks.
STACK_OWNERS = {"se_boot_watch": "__wstack_top",
                "se_boot_screen": "__mstack_top"}
HEADER_SLOTS = {0x04: "se_boot_start", 0x44: "se_boot_key"}


def die(msg):
    sys.stderr.write("patch-film: %s\n" % msg)
    raise SystemExit(1)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def put32(b, o, v):
    struct.pack_into("<I", b, o, v & 0xFFFFFFFF)


def parse_int(v):
    if isinstance(v, int):
        return v
    return int(v, 0)


def load_platform(name):
    platdir = os.path.join(ROOT, "ml", "platforms", name)
    path = os.path.join(platdir, "patch.json")
    if not os.path.isfile(path):
        die("no platform config at %s" % path)
    cfg = json.load(open(path, "r", encoding="utf-8"))
    cfg["_dir"] = platdir
    for key in ("stock_size", "restart", "payload_off", "payload_base",
                "gui_bl_site", "gui_handler", "init_literal", "ml_init_task"):
        if key in cfg:
            cfg[key] = parse_int(cfg[key])
    for key in ("loader_addr", "loader_limit", "hook_addr", "hook_limit",
                "stub_addr", "stub_limit",
                "bss_end_literal", "bss_end_old", "bss_end_new",
                "reserved_literal", "reserved_old", "reserved_new"):
        if key in cfg:
            cfg[key] = parse_int(cfg[key])
    return cfg


def va2off(cfg, va):
    off = va - cfg["restart"] + cfg["payload_off"]
    if not (cfg["payload_off"] <= off < cfg["stock_size"]):
        die("address 0x%08X is outside the relocated image" % va)
    return off


def bl(frm, to):
    """Encode an ARM bl from `frm` to `to`."""
    delta = (to - frm - 8) >> 2
    if not (-(1 << 23) <= delta < (1 << 23)):
        die("bl from 0x%08X to 0x%08X is out of range" % (frm, to))
    return 0xEB000000 | (delta & 0x00FFFFFF)


def nm_syms(elf):
    nm = subprocess.run(["arm-none-eabi-nm", elf],
                        capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in nm.splitlines():
        f = line.split()
        if len(f) == 3 and not f[2].startswith("*"):
            try:
                syms[f[2]] = int(f[0], 16)
            except ValueError:
                pass
    return syms


def build_stub_split(cfg):
    """Assemble mlstub.S with a two-section linker script (50D style)."""
    plat = cfg["_dir"]
    o = os.path.join(PAYDIR, "mlstub.o")
    e = os.path.join(PAYDIR, "mlstub.elf")
    loader = os.path.join(PAYDIR, "mlstub-loader.bin")
    hook = os.path.join(PAYDIR, "mlstub-hook.bin")
    ld = os.path.join(plat, "mlstub.ld")
    src = os.path.join(plat, "mlstub.S")
    if not os.path.isfile(ld):
        die("split stub needs %s" % ld)
    subprocess.run(["arm-none-eabi-as", "-mcpu=arm946e-s", "-o", o, src],
                   check=True)
    subprocess.run(["arm-none-eabi-ld", "-T", ld, "-o", e, o], check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".loader",
                    e, loader], check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".hook",
                    e, hook], check=True)
    return (bytearray(open(loader, "rb").read()),
            bytearray(open(hook, "rb").read()),
            nm_syms(e))


def build_stub_single(cfg):
    """Assemble mlstub.S as one contiguous blob at stub_addr (5D Classic)."""
    plat = cfg["_dir"]
    o = os.path.join(PAYDIR, "mlstub.o")
    e = os.path.join(PAYDIR, "mlstub.elf")
    b = os.path.join(PAYDIR, "mlstub.bin")
    src = os.path.join(plat, "mlstub.S")
    subprocess.run(["arm-none-eabi-as", "-mcpu=arm946e-s", "-o", o, src],
                   check=True)
    subprocess.run(["arm-none-eabi-ld", "-Ttext=%#x" % cfg["stub_addr"],
                    "-e", "se_loader", "-o", e, o], check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text",
                    e, b], check=True)
    return bytearray(open(b, "rb").read()), nm_syms(e)


def branch_target(cfg, img, off):
    """Where the ARM `b` at header offset `off` goes, or None."""
    w = u32(img, off)
    if (w & 0x0F000000) != 0x0A000000:
        return None
    delta = w & 0x00FFFFFF
    if delta & 0x00800000:
        delta -= 0x01000000
    return cfg["payload_base"] + off + 8 + delta * 4


def check_header(cfg, pay):
    base = cfg["payload_base"]
    for off, name in sorted(HEADER_SLOTS.items()):
        t = branch_target(cfg, pay, off)
        if t is None:
            die("the payload's header slot +0x%02X (%s) holds 0x%08X, which "
                "is not a branch" % (off, name, u32(pay, off)))
        if not (base <= t < base + len(pay)):
            die("the payload's header slot +0x%02X (%s) branches to 0x%08X, "
                "outside the payload" % (off, name, t))


def check_stacks():
    """Only task bodies may switch stacks, and no two may share one."""
    src = open(os.path.join(PAYDIR, "start.S")).read()
    label, seen = None, {}
    for line in src.splitlines():
        t = line.strip()
        if t.endswith(":") and not t.startswith("."):
            label = t[:-1]
        m = re.match(r"SE_ENTER\s+(\S+)", t)
        if m:
            if label is None:
                die("an SE_ENTER with no label above it in start.S")
            seen[label] = m.group(1)
    if seen != STACK_OWNERS:
        die("start.S switches stacks in %r; only task bodies may, and the "
            "expected set is %r. A non-task entry that switches to a stack "
            "a task owns corrupts that task's frames."
            % (seen, STACK_OWNERS))
    tops = list(seen.values())
    if len(set(tops)) != len(tops):
        die("two entry stubs share a stack: %r" % seen)


def fix_checksum_xor_bx_r3(img):
    """The boot stub XORs every word and resets unless the result is zero.
    The last word is the adjustment, same as build_tools/xor_chk.c."""
    if u32(img, len(img) - 8) != 0xE12FFF13:
        die("footer magic is not BX R3; refusing to checksum this autoexec")
    total = 0
    for off in range(0, len(img), 4):
        total ^= u32(img, off)
    put32(img, len(img) - 4, u32(img, len(img) - 4) ^ total)
    total = 0
    for off in range(0, len(img), 4):
        total ^= u32(img, off)
    if total != 0:
        die("checksum did not close")


def hole_is_zero(cfg, img, addr, n):
    off = va2off(cfg, addr)
    return all(b == 0 for b in img[off:off + n])


def verify_magic(cfg, img):
    tag = cfg.get("magic_tag", "")
    if tag == "gaonisoy":
        if bytes(img[4:12]) != b"gaonisoy":
            die("no 'gaonisoy' marker: this is not a Magic Lantern autoexec")
    elif tag:
        head = bytes(img[:256])
        if b"Magic Lantern" not in head or tag.encode("ascii") not in head:
            die("this is not the Magic Lantern %s autoexec this patch knows"
                % cfg["name"])


def apply_split(cfg, img):
    loader, hook, syms = build_stub_split(cfg)
    for want in ("se_loader", "se_hook", "se_boot", "se_loaded", "se_loader_end"):
        if want not in syms:
            die("stub is missing the symbol %s" % want)
    if syms["se_boot"] != cfg["loader_addr"]:
        die("loader linked at 0x%08X, expected 0x%08X"
            % (syms["se_boot"], cfg["loader_addr"]))
    if syms["se_hook"] != cfg["hook_addr"]:
        die("hook linked at 0x%08X, expected 0x%08X"
            % (syms["se_hook"], cfg["hook_addr"]))
    if syms["se_loader_end"] > cfg["loader_limit"]:
        die("loader reaches 0x%08X, past the hole at 0x%08X"
            % (syms["se_loader_end"], cfg["loader_limit"]))
    if cfg["hook_addr"] + len(hook) > cfg["hook_limit"]:
        die("hook is %d bytes and would pass 0x%08X"
            % (len(hook), cfg["hook_limit"]))

    if not hole_is_zero(cfg, img, cfg["loader_addr"], len(loader)):
        die("the loader hole at 0x%08X is not empty" % cfg["loader_addr"])
    if not hole_is_zero(cfg, img, cfg["hook_addr"], len(hook)):
        die("the hook hole at 0x%08X is not empty" % cfg["hook_addr"])

    have = u32(img, va2off(cfg, cfg["gui_bl_site"]))
    want = bl(cfg["gui_bl_site"], cfg["gui_handler"])
    if have != want:
        die("the instruction at 0x%08X is 0x%08X, expected 0x%08X "
            "(bl handle_common_events_startup)"
            % (cfg["gui_bl_site"], have, want))

    have_init = u32(img, va2off(cfg, cfg["init_literal"]))
    if have_init != cfg["ml_init_task"]:
        die("the init-task literal at 0x%08X is 0x%08X, expected 0x%08X"
            % (cfg["init_literal"], have_init, cfg["ml_init_task"]))
    if u32(img, va2off(cfg, cfg["bss_end_literal"])) != cfg["bss_end_old"]:
        die("bss-end literal is not 0x%08X" % cfg["bss_end_old"])
    if u32(img, va2off(cfg, cfg["reserved_literal"])) != cfg["reserved_old"]:
        die("reserved-size literal is not 0x%08X" % cfg["reserved_old"])

    img[va2off(cfg, cfg["loader_addr"]):
        va2off(cfg, cfg["loader_addr"]) + len(loader)] = loader
    img[va2off(cfg, cfg["hook_addr"]):
        va2off(cfg, cfg["hook_addr"]) + len(hook)] = hook
    put32(img, va2off(cfg, cfg["gui_bl_site"]),
          bl(cfg["gui_bl_site"], syms["se_hook"]))
    put32(img, va2off(cfg, cfg["init_literal"]), syms["se_boot"])
    put32(img, va2off(cfg, cfg["bss_end_literal"]), cfg["bss_end_new"])
    put32(img, va2off(cfg, cfg["reserved_literal"]), cfg["reserved_new"])

    return syms, loader, hook


def apply_single(cfg, img):
    stub, syms = build_stub_single(cfg)
    for want in ("se_loader", "se_hook", "se_boot", "se_loaded", "se_stub_end"):
        if want not in syms:
            die("stub is missing the symbol %s" % want)
    if syms["se_loader"] != cfg["stub_addr"]:
        die("stub linked at 0x%08X, expected 0x%08X"
            % (syms["se_loader"], cfg["stub_addr"]))
    end = cfg["stub_addr"] + len(stub)
    if end > cfg["stub_limit"]:
        die("stub is %d bytes and would reach 0x%08X, past the hole"
            % (len(stub), end))

    have = u32(img, va2off(cfg, cfg["gui_bl_site"]))
    want = bl(cfg["gui_bl_site"], cfg["gui_handler"])
    if have != want:
        die("the instruction at 0x%08X is 0x%08X, expected 0x%08X "
            "(bl handle_common_events_startup) -- refusing to patch a gui "
            "task this tool does not recognise"
            % (cfg["gui_bl_site"], have, want))

    have_init = u32(img, va2off(cfg, cfg["init_literal"]))
    if have_init != cfg["ml_init_task"]:
        die("the init-task literal at 0x%08X is 0x%08X, expected 0x%08X -- "
            "refusing to retarget a boot path this tool does not recognise"
            % (cfg["init_literal"], have_init, cfg["ml_init_task"]))

    img[va2off(cfg, cfg["stub_addr"]):
        va2off(cfg, cfg["stub_addr"]) + len(stub)] = stub
    put32(img, va2off(cfg, cfg["gui_bl_site"]),
          bl(cfg["gui_bl_site"], syms["se_hook"]))
    put32(img, va2off(cfg, cfg["init_literal"]), syms["se_boot"])
    return syms, stub, end


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--platform", default=os.environ.get("PLATFORM", "5DC.111"),
                    help="platform directory under ml/platforms/ (default: 5DC.111)")
    args = ap.parse_args()
    cfg = load_platform(args.platform)

    stock = os.path.join(ROOT, "vendor", cfg["vendor"], "autoexec.bin")
    if not os.path.exists(stock):
        die("Magic Lantern's autoexec.bin is missing.\n\n"
            "  Expected it at:\n    %s\n\n"
            "  It is not shipped here. Put the %s build from\n"
            "  %s at that path.\n"
            "  The build this was written against is\n"
            "  %s.\n"
            % (stock, cfg["name"], cfg.get("vendor_hint", "Magic Lantern"),
               cfg.get("vendor_build", "(see platform README)")))

    img = bytearray(open(stock, "rb").read())
    if len(img) != cfg["stock_size"]:
        die("stock AUTOEXEC.BIN is %d bytes, expected %d"
            % (len(img), cfg["stock_size"]))
    verify_magic(cfg, img)
    check_stacks()

    style = cfg.get("stub_style", "split")
    if style == "split":
        syms, loader, hook = apply_split(cfg, img)
    elif style == "single":
        syms, stub, end = apply_single(cfg, img)
    else:
        die("unknown stub_style %r in patch.json" % style)

    csum = cfg.get("checksum", "none")
    if csum == "xor_bx_r3":
        fix_checksum_xor_bx_r3(img)
    elif csum != "none":
        die("unknown checksum %r in patch.json" % csum)

    if len(img) != cfg["stock_size"]:
        die("internal error: the image changed size")

    os.makedirs(OUTDIR, exist_ok=True)
    out = os.path.join(OUTDIR, "AUTOEXEC.BIN")
    open(out, "wb").write(img)

    pay = open(os.path.join(PAYDIR, "soupeng.bin"), "rb").read()
    if len(pay) < 0x48 or u32(bytearray(pay), 0) != 0x4D4C4946:
        die("ml/payload/soupeng.bin has no 'FILM' header")
    check_header(cfg, bytearray(pay))
    se_out = os.path.join(OUTDIR, "SE.BIN")
    open(se_out, "wb").write(pay)

    if cfg.get("card_copy", True):
        os.makedirs(CARD, exist_ok=True)
        open(os.path.join(CARD, "AUTOEXEC.BIN"), "wb").write(img)
        open(os.path.join(CARD, "SE.BIN"), "wb").write(pay)

    print("platform  %s" % cfg["name"])
    print("wrote %s  (%d bytes, same length)" % (out, len(img)))
    print("wrote %s  (%d bytes)" % (se_out, len(pay)))
    if cfg.get("card_copy", True):
        print("copied both onto %s" % CARD)

    if style == "split":
        print("  loader    0x%08X .. 0x%08X  (%d bytes, %d spare)"
              % (cfg["loader_addr"], syms["se_loader_end"], len(loader),
                 cfg["loader_limit"] - syms["se_loader_end"]))
        print("  hook      0x%08X  (%d bytes)" % (cfg["hook_addr"], len(hook)))
        print("  gui hook  0x%08X -> 0x%08X (was 0x%08X)"
              % (cfg["gui_bl_site"], syms["se_hook"], cfg["gui_handler"]))
        print("  boot      init task 0x%08X -> 0x%08X, chains 0x%08X"
              % (cfg["init_literal"], syms["se_boot"], cfg["ml_init_task"]))
        if "bss_end_new" in cfg:
            print("  reserve   user_mem_start 0x%08X, %d bytes for SE.BIN"
                  % (cfg["bss_end_new"],
                     cfg["bss_end_new"] - cfg["payload_base"]))
    else:
        print("  stub      0x%08X .. 0x%08X  (%d bytes, %d spare)"
              % (cfg["stub_addr"], end, len(stub), cfg["stub_limit"] - end))
        print("  gui hook  0x%08X -> 0x%08X (was 0x%08X)"
              % (cfg["gui_bl_site"], syms["se_hook"], cfg["gui_handler"]))
        print("  boot      init task entry 0x%08X -> 0x%08X, chains 0x%08X"
              % (cfg["init_literal"], syms["se_boot"], cfg["ml_init_task"]))


if __name__ == "__main__":
    main()
