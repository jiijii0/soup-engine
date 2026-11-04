#!/usr/bin/env python3
"""
Patch the soup engine into the working 2012 Magic Lantern build for the 5D
Classic, following the pattern that is known to work on this body.

An earlier attempt grew AUTOEXEC.BIN by 36 KB, added a second relocation
loop to cstart, and chained a task into Magic Lantern's init. It left the
camera with no display at all: nothing runs before ML has replaced Canon's
GuiMainTask, so a fault anywhere in that chain takes the whole UI with it.

This does none of that. The rules it follows, taken from the ROM-dump patch
that did work (24 bytes, same file size):

  * the file stays exactly 221,184 bytes -- nothing is appended, so whether
    the bootloader reads past the original length never matters;
  * cstart and the boot path are not touched at all;
  * nothing of ours runs at boot. The payload is read off the card only when
    the menu item is chosen;
  * every patched pointer is safe before the payload exists. The stub's
    `se_loaded` flag lives inside ML's image and is set only after the
    payload's magic word has been checked, and until then the button hook
    passes every event straight to ML. A missing or corrupt ML/SE.BIN costs
    one menu item and nothing else.

What changes, and nothing more:

  1. dump_edmac (0x00980820..0x009808F0) is replaced by the loader and the
     button hook. That function is dead in the shipped binary: a scan of
     every branch and every literal in the relocated image finds nothing
     pointing into it from outside. Its literal pool ends at 0x0098090C and
     ML's live veneer table starts at 0x00980910, so the budget is 236 bytes.

     That boundary is now known to be load-bearing rather than merely
     plausible. The veneers immediately past it are on Magic Lantern's own
     drawing path, which the Phase 1 work decompiled (docs/ML_DISPLAY.md
     3.1, 7):

         0x0098091C  AcquireRecursiveLock   `bx lr` -- 16 call sites
         0x00980920  ReleaseRecursiveLock   `bx lr` -- 16 call sites
         0x00980938  get_screen_layout      `mov r0,#0; bx lr`

     menu_redraw_do calls all three. A stub that ran past 0x0098090C would
     not corrupt some unused table; it would corrupt the function ML's menu
     redraw calls on every pass.

  2. my_gui_task's call to handle_common_events_startup (the `bl` at
     0x00989DB4) is retargeted at the hook, which offers each button to the
     payload and tail-calls the original handler for anything it declines.

  3. The Debug menu's "Don't click me!" entry has its .priv pointed at the
     loader. Its .select is already run_in_separate_task, so it is left
     alone -- as is every other field, and every other entry. The entry's
     name and help strings are overwritten in place, so no pointer moves and
     no space is needed anywhere.

The payload itself is a separate file, ML/SE.BIN on the card.
"""

import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

STOCK   = os.path.join(ROOT, "vendor", "magiclantern-5Dc", "autoexec.bin")
PAYDIR  = os.path.join(ROOT, "ml", "payload")
OUTDIR  = os.path.join(ROOT, "ml", "build")

STOCK_SIZE  = 221184
RESTART     = 0x00980000
PAYLOAD_OFF = 0x1000

# --- where the stub goes -------------------------------------------------
STUB_ADDR   = 0x00980820      # dump_edmac
STUB_LIMIT  = 0x0098090C      # ML's veneer table starts at 0x00980910

# --- the gui hook --------------------------------------------------------
GUI_BL_SITE = 0x00989DB4      # bl handle_common_events_startup, in my_gui_task
GUI_HANDLER = 0x0098A0F0

# --- the boot chain ------------------------------------------------------
# ML creates its init task with the entry read from this literal. Point it
# at our boot stub and the engine starts at power-on; the stub chains ML's
# own init afterwards so the camera still comes up normally.
INIT_LITERAL = 0x00980774
ML_INIT_TASK = 0x00980440

# Magic Lantern's menu is not touched at all any more.
#
# There used to be four more sites here: a Debug entry's .priv rewritten to
# the loader, its name and help text replaced in place, and a display
# callback written at runtime into every entry of every tab. All of that
# existed so the soup engine could be reached from, and drawn inside, ML's
# menu. It is reached at boot now and draws straight onto the panel, so
# three edits do what seven did and ML's menu is left exactly as shipped.


def die(msg):
    sys.stderr.write("patch-film: %s\n" % msg)
    raise SystemExit(1)


def va2off(va):
    off = va - RESTART + PAYLOAD_OFF
    if not (PAYLOAD_OFF <= off < 0x35000):
        die("address 0x%08X is outside the relocated image" % va)
    return off


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def put32(b, o, v):
    struct.pack_into("<I", b, o, v & 0xFFFFFFFF)


def bl(frm, to):
    """Encode an ARM bl from `frm` to `to`."""
    delta = (to - frm - 8) >> 2
    if not (-(1 << 23) <= delta < (1 << 23)):
        die("bl from 0x%08X to 0x%08X is out of range" % (frm, to))
    return 0xEB000000 | (delta & 0x00FFFFFF)


def build_stub():
    """Assemble mlstub.S at its final address and return (bytes, symbols)."""
    o = os.path.join(PAYDIR, "mlstub.o")
    e = os.path.join(PAYDIR, "mlstub.elf")
    b = os.path.join(PAYDIR, "mlstub.bin")
    subprocess.run(["arm-none-eabi-as", "-mcpu=arm946e-s", "-o", o,
                    os.path.join(PAYDIR, "mlstub.S")], check=True)
    subprocess.run(["arm-none-eabi-ld", "-Ttext=%#x" % STUB_ADDR,
                    "-e", "se_loader", "-o", e, o], check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text",
                    e, b], check=True)
    nm = subprocess.run(["arm-none-eabi-nm", e],
                        capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in nm.splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    return bytearray(open(b, "rb").read()), syms


# --- the payload's own header ---------------------------------------------
#
# The payload tells Magic Lantern where to call it by writing a word from its
# own header table into a menu entry at runtime, and the word it writes is a
# constant in cam.h (SE_PAY_DISPLAY) while the table itself is laid out in
# start.S. Those two have to agree, and nothing else would notice if they
# stopped: the display slot would hold a plausible address in the middle of
# the payload's code, ML would branch into it, and the camera would need its
# battery pulled to find out.
#
# So check it here. Every slot in the table that is reached from outside must
# be an ARM `b` to somewhere inside the payload.
PAYLOAD_BASE  = 0x009D0000
# The slots something outside the payload branches to.
#
# +0x04 is the one that matters: mlstub's loader ends with `add r0, r0, #4 ;
# blx r0`, so that word IS the entry point. It was zeroed once, on the
# grounds that it "was the menu select target", and the camera came up with
# Err and no working buttons because the init task ran three nops and then
# executed a string pointer. This check existed and was deleted in the same
# commit. It is back, and it is the reason this list is not trimmed to
# whatever happens to look used.
HEADER_SLOTS  = {0x04: "se_boot_start", 0x44: "se_boot_key"}


def branch_target(img, off):
    """Where the ARM `b` at header offset `off` goes, or None."""
    w = u32(img, off)
    if (w & 0x0F000000) != 0x0A000000:
        return None
    delta = w & 0x00FFFFFF
    if delta & 0x00800000:
        delta -= 0x01000000
    return PAYLOAD_BASE + off + 8 + delta * 4


def check_header(pay):
    for off, name in sorted(HEADER_SLOTS.items()):
        t = branch_target(pay, off)
        if t is None:
            die("the payload's header slot +0x%02X (%s) holds 0x%08X, which "
                "is not a branch" % (off, name, u32(pay, off)))
        if not (PAYLOAD_BASE <= t < PAYLOAD_BASE + len(pay)):
            die("the payload's header slot +0x%02X (%s) branches to 0x%08X, "
                "outside the payload" % (off, name, t))


# Which entry stubs are allowed to switch to one of the payload's own
# stacks, and which stack each must use.
#
# The rule is "task bodies only, one stack each", and it is checked rather
# than remembered because forgetting it does not fail loudly. se_boot_start
# once switched to __mstack_top and then created the screen task, which
# runs on __mstack_top: the new task's pushes landed on the return address
# the old context was about to use, the init task jumped into nothing, and
# the camera shot photographs while answering no button at all. Nothing in
# the build or the simulator noticed.
STACK_OWNERS = {"se_boot_watch": "__wstack_top",
                "se_boot_screen": "__mstack_top"}


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


def main():
    if not os.path.exists(STOCK):
        die("Magic Lantern's autoexec.bin is missing.\n\n"
            "  Expected it at:\n    %s\n\n"
            "  It is not shipped here. What this tool produces IS that\n"
            "  binary with three instructions changed, so you need Magic\n"
            "  Lantern's own 5D Classic build to start from. Download it\n"
            "  from https://magiclantern.fm and put its autoexec.bin at the\n"
            "  path above.\n\n"
            "  You do not need this to USE the soup engine -- card/SE.BIN\n"
            "  and card/AUTOEXEC.BIN are already built. See BUILDING.md."
            % STOCK)
    img = bytearray(open(STOCK, "rb").read())
    if len(img) != STOCK_SIZE:
        die("stock AUTOEXEC.BIN is %d bytes, expected %d" % (len(img), STOCK_SIZE))
    if bytes(img[4:12]) != b"gaonisoy":
        die("no 'gaonisoy' marker: this is not a Magic Lantern autoexec")

    check_stacks()

    stub, syms = build_stub()
    for want in ("se_loader", "se_hook", "se_boot", "se_loaded", "se_stub_end"):
        if want not in syms:
            die("stub is missing the symbol %s" % want)
    if syms["se_loader"] != STUB_ADDR:
        die("stub linked at 0x%08X, expected 0x%08X" % (syms["se_loader"], STUB_ADDR))

    end = STUB_ADDR + len(stub)
    if end > STUB_LIMIT:
        die("stub is %d bytes and would reach 0x%08X, past the %d available "
            "before ML's veneer table at 0x00980910"
            % (len(stub), end, STUB_LIMIT - STUB_ADDR))

    # --- verify the gui hook site still holds the call we expect ---------
    have = u32(img, va2off(GUI_BL_SITE))
    want = bl(GUI_BL_SITE, GUI_HANDLER)
    if have != want:
        die("the instruction at 0x%08X is 0x%08X, expected 0x%08X "
            "(bl handle_common_events_startup) -- refusing to patch a gui "
            "task this tool does not recognise" % (GUI_BL_SITE, have, want))

    # --- verify the init-task literal still holds ML's own entry ---------
    have_init = u32(img, va2off(INIT_LITERAL))
    if have_init != ML_INIT_TASK:
        die("the init-task literal at 0x%08X is 0x%08X, expected 0x%08X -- "
            "refusing to retarget a boot path this tool does not recognise"
            % (INIT_LITERAL, have_init, ML_INIT_TASK))

    # ------------------------------------------------------------ apply
    img[va2off(STUB_ADDR):va2off(STUB_ADDR) + len(stub)] = stub
    put32(img, va2off(GUI_BL_SITE), bl(GUI_BL_SITE, syms["se_hook"]))
    put32(img, va2off(INIT_LITERAL), syms["se_boot"])

    if len(img) != STOCK_SIZE:
        die("internal error: the image changed size")

    os.makedirs(OUTDIR, exist_ok=True)
    out = os.path.join(OUTDIR, "AUTOEXEC.BIN")
    open(out, "wb").write(img)

    # the payload travels separately
    pay = open(os.path.join(PAYDIR, "soupeng.bin"), "rb").read()
    if len(pay) < 0x48 or u32(bytearray(pay), 0) != 0x4D4C4946:
        die("ml/payload/soupeng.bin has no 'FILM' header")
    check_header(bytearray(pay))
    open(os.path.join(OUTDIR, "SE.BIN"), "wb").write(pay)

    print("wrote %s  (%d bytes, unchanged)" % (out, len(img)))
    print("wrote %s  (%d bytes)" % (os.path.join(OUTDIR, "SE.BIN"), len(pay)))
    print()
    print("  stub      0x%08X .. 0x%08X  (%d bytes, %d spare)"
          % (STUB_ADDR, end, len(stub), STUB_LIMIT - end))
    print("  gui hook  0x%08X -> 0x%08X (was 0x%08X)"
          % (GUI_BL_SITE, syms["se_hook"], GUI_HANDLER))
    print("  screen    ours: TRASH opens it. We ASK Canon for the panel -- a")
    print("            fake MENU press, the way ML's menu_open does at")
    print("            0x0098728C -- and never touch TurnOnDisplay or")
    print("            ShowBitmap. The palette is reasserted before every")
    print("            frame, because Canon restores its own on every")
    print("            panel-on. ML's menu is not patched and never opens.")
    print("  boot      init task entry 0x%08X -> 0x%08X, chains 0x%08X"
          % (INIT_LITERAL, syms["se_boot"], ML_INIT_TASK))
    print("            the engine starts at power-on; a missing SE.BIN still boots")
    print("  ML's code untouched; no instruction of Magic Lantern's is rewritten")


if __name__ == "__main__":
    main()
