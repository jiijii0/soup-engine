#!/usr/bin/env python3
"""
romtool - read the camera firmware the way this project needs to.

Two images, one address space (defaults for PLATFORM=50D.109):

  rom/ROM1.BIN                    0xFF810000..             camera ROM
  vendor/magiclantern-<body>/     relocated ML image
    autoexec.bin                  (VA / file offset from platforms/*/patch.json)

Set PLATFORM=5DC.111 (or another platforms/ name) to retarget Magic Lantern.

Everything this project calls lives in one of those two, and every address
it hardcodes should be justified by something here rather than by a table
copied from somewhere else. That is not pedantry: a four-byte error in one
such table cost two camera trips, and an address taken from a NEWER
Magic Lantern tree than the binary we actually patch cost several more.

  romtool.py dis   0xFFB091D0 [bytes]   disassemble
  romtool.py str   0xFFB091D0 [n]       disassemble, resolving literals and
                                        printing any string they point at
  romtool.py xref  0xFFB091D0           every B/BL that targets it
  romtool.py word  0x0000FCA0           who loads this address as a literal
  romtool.py find  "some text"          locate a string and who references it
  romtool.py map                        every address the payload hardcodes,
                                        checked against the images

Reading outward from a known address -- which is what Phase 1 of the display
work needed and what the old commands could not do:

  romtool.py func    0x00980BBC         one function, start to end, annotated
  romtool.py calls   0x00986668         every call it makes, targets named
  romtool.py callers 0x00980AD0         every caller, resolved to its function
  romtool.py graph   0x00986668 [depth] the call graph below an address
  romtool.py reads   0x0000FCA0         every function that loads an address
                                        as a literal, named
  romtool.py fstart  0x00980BD0         the nearest prologue at or before
"""
import json, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLATFORM = os.environ.get("PLATFORM", "50D.109")
_plat = os.path.join(HERE, "ml", "platforms", PLATFORM, "patch.json")
_cfg = json.load(open(_plat, "r", encoding="utf-8")) if os.path.isfile(_plat) else {
    "vendor": "magiclantern-50D", "restart": "0x0004B000", "payload_off": "0x11C",
}
ROM  = os.path.join(HERE, "rom/ROM1.BIN")
ML   = os.path.join(HERE, "vendor", _cfg["vendor"], "autoexec.bin")
ROM_VA = 0xFF810000
ML_VA  = int(_cfg["restart"], 0) if isinstance(_cfg["restart"], str) else _cfg["restart"]
ML_OFF = int(_cfg["payload_off"], 0) if isinstance(_cfg["payload_off"], str) else _cfg["payload_off"]

_rom = open(ROM, "rb").read() if os.path.exists(ROM) else b""
_ml  = open(ML, "rb").read()  if os.path.exists(ML)  else b""


def where(va):
    """(image, file offset, name) for a virtual address, or None."""
    if _rom and ROM_VA <= va < ROM_VA + len(_rom):
        return _rom, va - ROM_VA, "ROM"
    if _ml and ML_VA <= va < ML_VA + (len(_ml) - ML_OFF):
        return _ml, va - ML_VA + ML_OFF, "ML"
    return None


def word(va):
    w = where(va)
    if not w or w[1] + 4 > len(w[0]):
        return None
    return struct.unpack_from("<I", w[0], w[1])[0]


def cstr(va, limit=120):
    w = where(va)
    if not w:
        return None
    img, off, _ = w
    end = img.find(b"\0", off, off + limit)
    if end < 0:
        return None
    try:
        t = img[off:end].decode("ascii")
    except UnicodeDecodeError:
        return None
    return t if t and all(32 <= ord(c) < 127 or c == "\t" for c in t) else None


def dis(va, n=0x60):
    w = where(va)
    if not w:
        return []
    img, off, _ = w
    tmp = "/tmp/_romtool.bin"
    open(tmp, "wb").write(img[off:off + n])
    out = subprocess.run(
        ["arm-none-eabi-objdump", "-D", "-b", "binary", "-m", "arm",
         "--adjust-vma=0x%X" % va, tmp],
        capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]{6,8}):\s+([0-9a-f]{8})\s+(.*)", line)
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2), 16),
                         m.group(3).rstrip()))
    return rows


def annotate(va, n=0x60):
    """Disassemble, and resolve every `ldr rX, [pc, #k]` to its literal."""
    for a, enc, text in dis(va, n):
        note = ""
        m = re.search(r"@ 0x([0-9a-f]+)", text)
        if m and "ldr" in text:
            lit = int(m.group(1), 16)
            v = word(lit)
            if v is not None:
                s = cstr(v)
                note = "   ; = 0x%08X" % v + ("  %r" % s if s else "")
        print("  %08X  %08X  %-34s%s" % (a, enc, text, note))


def branches_to(target, img, base):
    """Every B/BL in `img` whose target is `target`."""
    hits = []
    start = ML_OFF if img is _ml else 0
    for off in range(start, len(img) - 3, 4):
        enc = struct.unpack_from("<I", img, off)[0]
        if (enc & 0x0E000000) != 0x0A000000:
            continue
        d = enc & 0xFFFFFF
        if d & 0x800000:
            d -= 0x1000000
        va = base + (off - start)
        # Mask to 32 bits. ML's image sits at 0x00980000 and Canon's ROM at
        # 0xFF800000, which is 4.27 GB away -- far outside BL's +/-32 MB --
        # and the calls work anyway because the arithmetic wraps: the branch
        # at 0x00980B40 reaches memcpy at 0xFFB44664 with a NEGATIVE offset.
        # Without the mask every ML->ROM call was invisible, and `xref` on a
        # ROM address reported "ML never calls this" for every one of them.
        if ((va + 8 + d * 4) & 0xFFFFFFFF) == target:
            hits.append((va, "bl" if (enc & 0x01000000) else "b"))
    return hits


def loads_of(addr):
    """Every word in either image equal to `addr` -- i.e. literal pools."""
    hits = []
    for img, base, start, name in ((_rom, ROM_VA, 0, "ROM"),
                                   (_ml, ML_VA, ML_OFF, "ML")):
        if not img:
            continue
        pat = struct.pack("<I", addr)
        i = img.find(pat, start)
        while i >= 0:
            if i % 4 == 0:
                hits.append((base + (i - start), name))
            i = img.find(pat, i + 1)
    return hits


def cmd_map():
    """Every address the payload hardcodes, with a one-line sanity check."""
    import glob
    defs = {}
    for f in ("ml/payload/cam.h", "ml/payload/mlmap.h"):
        for line in open(os.path.join(HERE, f)):
            m = re.match(r"#define\s+((?:CAM|ML)_\w+)\s+(0x[0-9A-Fa-f]{6,8})", line)
            if m:
                defs[m.group(1)] = int(m.group(2), 16)
    print("%-28s %-12s %-5s %s" % ("symbol", "address", "image", "first word / note"))
    for name, va in sorted(defs.items(), key=lambda kv: kv[1]):
        w = where(va)
        if 0xC0000000 <= va < 0xD0000000:        # DIGIC registers
            print("%-28s 0x%08X  %-5s %s" % (name, va, "MMIO", "hardware register"))
            continue
        if not w:
            kind = "RAM" if va < 0x01000000 else "-"
            note = ("a Canon RAM variable, not code"
                    if kind == "RAM" else "NOT IN EITHER IMAGE")
            print("%-28s 0x%08X  %-5s %s" % (name, va, kind, note))
            continue
        v = word(va)
        hint = ""
        if va >= ROM_VA or va >= ML_VA:
            if v in (0xE1A0C00D,) or (v & 0xFFFF0000) == 0xE92D0000:
                hint = "ARM prologue"
            elif v == 0:
                hint = "zero"
        print("%-28s 0x%08X  %-5s %08X  %s" % (name, va, w[2], v, hint))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    c = sys.argv[1]
    if c == "map":
        cmd_map(); return 0
    if c == "tables":
        tables, names = cmd_tables()
        print("%d tables, %d exactly-named functions" % (len(tables), len(names)))
        for va, k in sorted(tables, key=lambda t: -t[1]):
            print("  table at 0x%08X, %d entries" % (va, k))
        for f in sorted(names):
            print("  0x%08X  %s" % (f, names[f]))
        return 0
    if c == "mlnames":
        for a2 in sorted(cmd_mlnames()):
            fn, fl, gap = cmd_mlnames()[a2]
            print("  0x%08X  %-30s %-28s (+%d)" % (a2, fn, fl or "", gap))
        return 0
    if c == "tasks":
        for nm, pri, stk, ent, site in cmd_tasks():
            print("  %-24s prio=%-7s stack=%-8s entry=%-12s created at 0x%08X"
                  % (nm, hex(pri) if pri is not None else "?",
                     hex(stk) if stk is not None else "?",
                     "0x%08X" % ent if ent else "?", site))
        return 0
    if c == "names":
        which = sys.argv[2] if len(sys.argv) > 2 else "ROM"
        f = cmd_names(which)
        for a in sorted(f):
            print("  0x%08X  %s" % (a, " | ".join(sorted(set(f[a]))[:3])))
        return 0
    if c == "find":
        needle = sys.argv[2].encode()
        for img, base, start, nm in ((_rom, ROM_VA, 0, "ROM"), (_ml, ML_VA, ML_OFF, "ML")):
            i = img.find(needle, start) if img else -1
            while i >= 0:
                va = base + (i - start)
                print("%s 0x%08X  %r" % (nm, va, cstr(va)))
                for la, lnm in loads_of(va):
                    print("    referenced by a literal at %s 0x%08X" % (lnm, la))
                i = img.find(needle, i + 1)
        return 0
    va = int(sys.argv[2], 16)
    n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
    if c == "func":
        cmd_func(va); return 0
    if c == "calls":
        cmd_calls(va); return 0
    if c == "callers":
        cmd_callers(va); return 0
    if c == "graph":
        cmd_graph(va, int(sys.argv[3]) if len(sys.argv) > 3 else 3); return 0
    if c == "reads":
        cmd_reads(va); return 0
    if c == "fstart":
        f = cmd_fstart(va)
        print("  0x%08X  %s" % (f, name_of(f)) if f else "  not found")
        return 0
    if c == "dis":
        for a, e, t in dis(va, n):
            print("  %08X  %08X  %s" % (a, e, t))
    elif c == "str":
        annotate(va, n)
    elif c == "xref":
        hits = []
        if _rom:
            hits += [(a, k, "ROM") for a, k in branches_to(va, _rom, ROM_VA)]
        if _ml:
            hits += [(a, k, "ML") for a, k in branches_to(va, _ml, ML_VA)]
        for a, k, nm in hits:
            print("  %s 0x%08X  %s" % (nm, a, k))
        print("  %d branch(es)" % len(hits))
        for la, nm in loads_of(va):
            print("  %s 0x%08X  literal (function pointer or table entry)" % (nm, la))
    elif c == "word":
        for la, nm in loads_of(va):
            print("  %s 0x%08X  loads 0x%08X" % (nm, la, va))
    else:
        print(__doc__); return 1
    return 0




# ---------------------------------------------------------------------------
# Naming the firmware from its own strings
#
# Canon shipped this ROM with its debug messages intact, and they are how it
# can be read at all. Three kinds matter:
#
#   "[DispCon] TurnOnDisplay (PUB)"   a function naming itself
#   "../Develop/Develop.c"            an assert naming its source file
#   "SomeTaskName"                    task and object names
#
# A string on its own is not worth much. A string plus the function that
# references it is: scan every literal pool for the string's address, walk
# back to the enclosing prologue, and the function has a name -- or at least
# a module. That is the whole technique, and it names a large part of an
# image that has no symbol table.
# ---------------------------------------------------------------------------

import collections

PROLOGUES = (0xE1A0C00D,)          # mov ip, sp -- the usual APCS frame setup


def _is_prologue(enc):
    if enc in PROLOGUES:
        return True
    # push {lr} alone, which gcc emits as `str lr, [sp, #-4]!`. This form was
    # missing, and it is what produced the bad attribution recorded in
    # docs/FIRMWARE.md 7c: bmp_init's prologue at 0x00980A74 is exactly this
    # encoding, the scan walked past it, and the name landed 636 bytes
    # earlier on dump_edmac. With it, bmp_init resolves to 0x00980A74 and the
    # gap drops from 636 to 40.
    if enc == 0xE52DE004:
        return True
    # push {..., lr} : cond=E, 1001 0010 1101, with bit 14 (lr) set
    return (enc & 0xFFFF0000) == 0xE92D0000 and (enc & 0x4000)


def word_index(img, base, start=0):
    """value -> [virtual addresses of every aligned word holding it]."""
    idx = collections.defaultdict(list)
    for off in range(start, len(img) - 3, 4):
        v = struct.unpack_from("<I", img, off)[0]
        if v:
            idx[v].append(base + (off - start))
    return idx


def strings_in(img, base, start=0, minlen=6):
    """(virtual address, text) for every NUL-terminated ASCII string."""
    out = []
    i = start
    n = len(img)
    while i < n:
        j = i
        while j < n and 0x20 <= img[j] < 0x7F:
            j += 1
        if j - i >= minlen and j < n and img[j] == 0:
            out.append((base + (i - start), img[i:j].decode("ascii")))
        i = j + 1
    return out


def function_starts(img, base, start=0):
    """Every plausible function entry, in order.

    Taking the greatest entry <= an address is how a stripped image gets
    carved into functions. It is a heuristic and it is wrong at the edges --
    a `push {..., lr}` inside a function looks like an entry -- but it is
    wrong in a bounded way: the answer is never a *later* function, so a
    name is attributed to the right code or to a fragment of it, never to
    the neighbour.
    """
    out = []
    for off in range(start, len(img) - 3, 4):
        if not _is_prologue(struct.unpack_from("<I", img, off)[0]):
            continue
        # A frame-pointer function starts `mov ip, sp` / `push {fp,ip,lr,pc}`.
        # Both words look like an entry, and taking the later one reports
        # every such function four bytes too high -- TurnOnDisplay came out
        # as 0xFFB091D4 when the camera calls 0xFFB091D0. Step back onto the
        # mov when it is there.
        if off - 4 >= start and \
           struct.unpack_from("<I", img, off - 4)[0] == 0xE1A0C00D:
            va = base + (off - 4 - start)
            if not out or out[-1] != va:
                out.append(va)
            continue
        out.append(base + (off - start))
    return out


def enclosing(starts, va):
    import bisect
    i = bisect.bisect_right(starts, va) - 1
    return starts[i] if i >= 0 else None


def function_start(img, base, va, limit=0x600, start=0):
    """Walk back from `va` to the nearest plausible function prologue."""
    off = va - base + start
    for back in range(0, limit, 4):
        p = off - back
        if p < start:
            break
        enc = struct.unpack_from("<I", img, p)[0]
        if _is_prologue(enc):
            return base + (p - start)
    return None



def cmd_names(which="ROM"):
    img, base, start = ((_rom, ROM_VA, 0) if which == "ROM"
                        else (_ml, ML_VA, ML_OFF))
    idx = word_index(img, base, start)
    funcs = collections.defaultdict(list)
    for sva, text in strings_in(img, base, start):
        for lva in idx.get(sva, ()):
            f = function_start(img, base, lva, start=start)
            if f is not None:
                funcs[f].append(text)
    return funcs


def _readable(va, maxlen=64, first_alpha=True):
    s = cstr(va, maxlen)
    if not s or len(s) < 2:
        return None
    if first_alpha and not (s[0].isalpha() or s[0] == "_"):
        return None
    return s


def _looks_like_code(va):
    if not where(va) or (va & 3):
        return False
    w = word(va)
    return w == 0xE1A0C00D or (w & 0xFFFF0000) == 0xE92D0000 \
        or (w & 0xFFFFF000) == 0xE52DE000


def cmd_tables():
    """Runs of {char *name, void *func} -- exact names, not inferred ones.

    Canon registers whole subsystems through tables like these, and each
    entry is a name the firmware's authors wrote next to the address it
    belongs to. Everything else in this file is a heuristic; this is not.
    """
    out = {}
    img, base = _rom, ROM_VA
    off, n = 0, len(img)

    def pair(o):
        a = struct.unpack_from("<I", img, o)[0]
        b = struct.unpack_from("<I", img, o + 4)[0]
        s = _readable(a)
        return (s, b) if (s and _looks_like_code(b)) else None

    tables = []
    while off + 8 <= n:
        if pair(off):
            start, k = off, 0
            while off + 8 <= n and pair(off):
                k += 1
                off += 8
            if k >= 4:
                tables.append((base + start, k))
        else:
            off += 4
    for va, k in tables:
        for i in range(k):
            s, f = pair(va - base + 8 * i)
            out.setdefault(f, s)
    return tables, out


def _imm_loads_before(site, span=0x90):
    """Registers set by ldr-from-literal or mov-immediate before `site`."""
    reg = {}
    for back in range(4, span, 4):
        va = site - back
        w = word(va)
        if w is None:
            break
        if (w & 0x0F7F0000) in (0x051F0000, 0x059F0000):      # ldr rX,[pc,#i]
            rd = (w >> 12) & 0xF
            imm = w & 0xFFF
            lit = va + 8 + imm if (w & 0x00800000) else va + 8 - imm
            reg.setdefault(rd, word(lit))
        elif (w & 0x0FEF0000) == 0x03A00000:                  # mov rX,#imm
            rd = (w >> 12) & 0xF
            v, rot = w & 0xFF, ((w >> 8) & 0xF) * 2
            reg.setdefault(rd, ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF)
    return reg


TASK_CREATE = 0xFFB18B40


ML_ASSERT = 0x009807D0


def cmd_mlnames(max_gap=0x100):
    """Name ML's functions from its own ASSERT(expr, file, line, func).

    ML passes the function name as the fourth argument, so the same trick
    that names Canon's ROM names ML's image -- with one caveat worth
    stating rather than hiding. Attribution takes the nearest prologue
    before the call site, and when the scan misses a prologue the name
    lands on the function before it. The distance says which: bmp_puts
    resolves 36 bytes from its assert and bmp_idle_copy 48, both matching
    addresses already known from the disassembly, while bmp_init resolves
    636 bytes away and is simply wrong.

    So the gap is reported, and anything past `max_gap` is dropped.
    """
    img, base, start = _ml, ML_VA, ML_OFF
    starts = function_starts(img, base, start)
    out = {}
    for site, _ in branches_to(ML_ASSERT, img, base):
        reg = _imm_loads_before(site, 0x60)
        fn = cstr(reg[3]) if reg.get(3) else None
        fl = cstr(reg[1]) if reg.get(1) else None
        if not fn:
            continue
        f = enclosing(starts, site)
        if f is None or site - f > max_gap:
            continue
        prev = out.get(f)
        if prev is None or site - f < prev[2]:
            out[f] = (fn, fl, site - f)
    return out


def cmd_tasks():
    """Every task the firmware creates, from the literals at each call site.

    task_create(name, priority, stack_bytes, entry, arg). The name and the
    entry are literals, so both can be recovered. This is how GuiMainTask's
    8 KB stack is known -- which is the stack the soup engine's button hook
    borrows, and the reason nothing on it may call into the filesystem.
    """
    found = []
    for site, _ in branches_to(TASK_CREATE, _rom, ROM_VA):
        reg = _imm_loads_before(site)
        nm = cstr(reg[0], 48) if reg.get(0) else None
        if nm:
            found.append((nm, reg.get(1), reg.get(2), reg.get(3), site))
    return sorted(found)




# ---------------------------------------------------------------------------
# Reading outward from an address
#
# Phase 1 of the display work needed three things the commands above could
# not do: the full extent of one function, every function that calls it, and
# the tree of what it calls. `xref` gave raw call sites, which is not the
# same as knowing WHICH function makes the call -- and that distinction is
# the whole of "which task does this run on".
#
# Function extent is a heuristic and says so: it stops at the first
# unconditional return (`pop {..,pc}`, `bx lr`, `b` to another function's
# entry) that is followed by either a literal pool or a prologue. Where that
# is wrong it is wrong by running long, never short, so a call it reports is
# always really there.
# ---------------------------------------------------------------------------

_names_cache = {}


def _all_names():
    """ML's self-asserted names, Canon's registration tables, and the
    debug-string attributions -- one lookup, built once."""
    if _names_cache:
        return _names_cache
    out = {}
    try:
        for va, (fn, fl, gap) in cmd_mlnames().items():
            out[va] = fn
    except Exception:
        pass
    try:
        _, tbl = cmd_tables()
        for va, nm in tbl.items():
            out.setdefault(va, nm)
    except Exception:
        pass
    # a handful established by hand in docs/FIRMWARE.md, where the
    # heuristics cannot reach (no assert, no string, no table entry)
    out.update(KNOWN_NAMES)
    _names_cache.update(out)
    return out


KNOWN_NAMES = {
    0x009807D0: "ASSERT",
    0x00980A40: "fontspec_font",
    0x00980A74: "bmp_init",
    0x00980AC0: "bmp_draw_to_idle",
    0x00980AD0: "bmp_idle_copy",
    0x00980BBC: "bmp_vram",
    0x00980E68: "bmp_puts",
    0x0098110C: "bmp_printf",
    0x00980518: "dump_with_buffer",
    0x00980820: "dump_edmac",
    0x009809C4: "_EngDrvOut",
    0x00989118: "EngDrvOut",
    0x00981BC0: "set_ml_palette",
    0x00981C50: "set_ml_palette_if_dirty",
    0x00984D38: "menu_entry_default_display",
    0x0098479C: "menu_draw_icon",
    0x00985A8C: "menus_display",
    0x00986668: "menu_redraw_do",
    0x009876B4: "run_in_separate_task",
    0x00989D70: "my_gui_task",
    0x0098A0F0: "handle_common_events_startup",
    0x00980440: "ml_init_task",
    0x00980A74: "bmp_init",
    0x00981390: "bmp_fill",
    0x0098091C: "AcquireRecursiveLock (bx lr -- no-op here)",
    0x00980920: "ReleaseRecursiveLock (bx lr -- no-op here)",
    0x00980938: "get_screen_layout (returns 0 here)",
    0x00992378: "bmp_on (bx lr -- no-op here)",
    0x009923CC: "bmp_zoom (bx lr -- no-op here)",
    0x00986C1C: "menu_redraw_task",
    0x00986F14: "menu_task",
    0x0098A028: "hijack_gui_main_task",
    0x0098A170: "handle_common_events_by_feature",
    0x0099B0D8: "handle_mlu_toggle",
    0x00988418: "fake_simple_button",
    0x009805B0: "ml_big_init_task",
    0x0099E9CC: "_update_vram_params",
    0x009937A8: "bvram_mirror_init",
    0xFF81800C: "gui_massive_event_loop",
    0xFFB16D84: "msg_queue_create",
    0xFFB4CB08: "malloc",
    0xFFB2A56C: "AllocateMemoryStrictly?",
    0xFFB07658: "TurnOnPowerPort",
    0xFFB09070: "WaitVSync",
    0xFFB088CC: "RequestNotifyBlank",
    0xFFB08E34: "EnableBitmapVBufferForPlayBackAndWaiting",
    0xFFB08BC8: "EnableImageVBufferForPlayBackAndWait",
    0xFFB08678: "RegisterTurnOnDisplayCBR",
    0xFFB09530: "TurnOffDisplay",
    0xFFB098E0: "ShowImage",
    0xFFB0995C: "HideImage",
    0xFFB09E40: "DispCon_take_semaphore",
    0xFFB09E64: "DispCon_give_semaphore",
    0xFFB0A528: "ConvertYUVPaletteToPBPalette",
    0xFFB0A888: "SetParameterToBitmapDisplayDevice",
    0xFFB0A95C: "SelectParameter",
    0xFFB0AD0C: "InitializeBitmapDisplayDevice",
    0xFFB0AFC0: "CreatePhysicalVram",
    0xFFB0B0E4: "DeletePhysicalVram",
    0xFFA279B0: "gui_destroy_bitmap_screen (the teardown)",
    0xFFB0B174: "DisplayPhysicalScreen",
    0xFFB0B240: "UndisplayPhysicalScreen",
    0xFFB0B2F0: "SetYUVPaletteToDisplayDevice",
    0xFFB0BD1C: "RefreshPhysicalVram (logs and returns)",
    0xFFA2521C: "gui_create_bitmap_screen (once per boot)",
    0xFFB091D0: "TurnOnDisplay",
    0xFFB097E8: "ShowBitmap",
    0xFFB09864: "HideBitmap",
    0xFFB09E84: "BmpDDev_take_semaphore",
    0xFFB09EA8: "BmpDDev_give_semaphore",
    0xFFB0AC74: "NotifyTurnOnDisplayToBitmapDisplayDevice",
    0xFFB187F4: "msleep",
    0xFFB18B40: "task_create",
    0xFFB44664: "memcpy",
    0xFFB1F578: "create_semaphore",
    0xFFB18430: "give_semaphore",
    0xFFB1828C: "take_semaphore",
    0xFFB170FC: "msg_queue_receive",
    0xFFB20698: "DebugMsg",
    0xFF8190D0: "GUI_Control",
}


def name_of(va):
    return _all_names().get(va, "")


def label(va):
    nm = name_of(va)
    return "0x%08X %s" % (va, nm) if nm else "0x%08X" % va


def _stub_entry(a):
    """Is `a` the entry of a one- or two-instruction veneer?

    ML's image is full of them: whole runs of `bx lr` and `mov r0,#0; bx lr`
    where a feature was compiled out for this body. They have no prologue,
    so walking back from one lands on whatever real function precedes the
    run -- and in this image that is `dump_edmac` at 0x00980820, the dead
    function this project's stub is written over. A call graph that says
    menu_redraw_do calls dump_edmac is worse than no call graph, and this is
    the same class of mistake (a name attributed to the function before the
    one meant) that docs/FIRMWARE.md 7c records costing two camera trips.

    The rule is deliberately narrow so it fires only on veneer tables: the
    word before `a` must be an unconditional return or a literal-pool word,
    and `a` itself must be a return or a `mov rX,#imm` followed by one.

    What it still gets wrong: a real leaf function with no prologue, which
    gcc emits when nothing needs saving and the exit is a tail call.
    `fake_simple_button` at 0x00988418 is one, and `fstart` puts it in the
    function before it. Those go in KNOWN_NAMES by hand. `callers` is
    unaffected -- it resolves branch targets, not enclosing functions.
    """
    prev = word(a - 4)
    if prev is None:
        return False
    # the veneer either follows another veneer's return, or sits just after
    # a literal pool -- which is where the first entry of a run lands
    if not _is_return(prev, a - 4) and _plausible_insn(prev):
        return False
    here = word(a)
    if here is None:
        return False
    if _is_return(here, a):
        return True
    nxt = word(a + 4)
    return (here & 0x0FEF0000) == 0x03A00000 and \
        nxt is not None and _is_return(nxt, a + 4)


def cmd_fstart(va):
    w = where(va)
    if not w:
        return None
    img, _, nm = w
    base, start = (ML_VA, ML_OFF) if nm == "ML" else (ROM_VA, 0)
    if word(va) is not None and _is_prologue(word(va)):
        return va
    # a veneer, or an address a word or two inside one
    for back in (0, 4):
        if _stub_entry(va - back):
            return va - back
    return function_start(img, base, va, start=start)


def _is_return(enc, va):
    """An unconditional return: pop {..,pc} / ldr pc,[sp],#4 / bx lr."""
    if enc == 0xE12FFF1E:                                   # bx lr
        return True
    if (enc & 0xFFFF0000) == 0xE8BD0000 and (enc & 0x8000):  # pop {..,pc}
        return True
    if enc == 0xE49DF004:                                   # ldr pc,[sp],#4
        return True
    return False


def func_extent(va, limit=0x800):
    """[start, end) of the function at `va`, by the rule in the comment."""
    end = va
    seen_return = False
    for off in range(0, limit, 4):
        a = va + off
        enc = word(a)
        if enc is None:
            break
        if _is_return(enc, a):
            seen_return = True
            end = a + 4
            continue
        if seen_return:
            # a literal pool entry, or the next function's prologue, ends it
            if _is_prologue(enc) or not _plausible_insn(enc):
                return va, end
            seen_return = False
        end = a + 4
    return va, end


def _plausible_insn(enc):
    """Does this word decode as an ARM instruction with a sane condition?

    A literal pool entry is usually an address (0x009Axxxx, 0xFFBxxxxx) or a
    small integer, and both come out with condition field 0 (EQ) or F, which
    real compiled code almost never uses at the start of a basic block.
    """
    cond = enc >> 28
    return cond != 0xF and cond != 0x0


def calls_in(va, limit=0x800):
    """Every B/BL out of the function at `va`, in order, with duplicates."""
    start, end = func_extent(va, limit)
    out = []
    for a in range(start, end, 4):
        enc = word(a)
        if enc is None or (enc & 0x0E000000) != 0x0A000000:
            continue
        d = enc & 0xFFFFFF
        if d & 0x800000:
            d -= 0x1000000
        t = (a + 8 + d * 4) & 0xFFFFFFFF      # wraps -- see branches_to()
        if start <= t < end:
            continue                      # an internal branch, not a call
        out.append((a, "bl" if (enc & 0x01000000) else "b", t))
    return out


def cmd_func(va, limit=0x800):
    f = cmd_fstart(va)
    if f is None:
        print("  not in either image")
        return
    if f != va:
        print("  (0x%08X is inside the function at %s)" % (va, label(f)))
    s, e = func_extent(f, limit)
    print("  %s   %d bytes" % (label(f), e - s))
    annotate(s, e - s)


def cmd_calls(va, limit=0x800):
    f = cmd_fstart(va) or va
    print("  %s calls:" % label(f))
    for site, kind, t in calls_in(f, limit):
        print("    0x%08X  %-2s  %s%s" % (site, kind, label(t),
              "   (tail call)" if kind == "b" else ""))


def cmd_callers(va):
    """Every call site that targets `va` exactly.

    `va` must be the function's ENTRY. For a frame-pointer function that is
    the `mov ip, sp`, not the `push {fp, ip, lr, pc}` four bytes later --
    and `romtool.py names` reports the push, because it attributes a string
    to the nearest prologue. Asking about the push finds nothing, because
    nobody branches there.

    That cost a wrong conclusion in docs/ML_DISPLAY.md 4.4:
    UndisplayPhysicalScreen was recorded as having ZERO callers in the
    whole ROM, and the real entry is four bytes lower with a caller at
    0xFFA279CC. So this warns rather than quietly answering the wrong
    question.
    """
    prev = word(va - 4)
    here = word(va)
    if prev == 0xE1A0C00D and here is not None and \
       (here & 0xFFFF0000) == 0xE92D0000:
        print("  NOTE: 0x%08X is the push of a frame-pointer function whose"
              % va)
        print("        entry is 0x%08X (`mov ip, sp`). Nothing branches to"
              % (va - 4))
        print("        a push -- you almost certainly want %s."
              % label(va - 4))
    hits = []
    if _rom:
        hits += [(a, k, "ROM") for a, k in branches_to(va, _rom, ROM_VA)]
    if _ml:
        hits += [(a, k, "ML") for a, k in branches_to(va, _ml, ML_VA)]
    print("  callers of %s:" % label(va))
    for a, k, nm in sorted(hits):
        f = cmd_fstart(a)
        print("    %s 0x%08X  %-2s  from %s  (+%d)"
              % (nm, a, k, label(f) if f else "?", a - f if f else -1))
    print("    %d call site(s)" % len(hits))
    lits = loads_of(va)
    for la, nm in lits:
        f = cmd_fstart(la)
        print("    %s 0x%08X  literal (function pointer or table entry)" % (nm, la))


def cmd_graph(va, depth=3, limit=0x800, _seen=None, _ind=0):
    if _seen is None:
        _seen = set()
    # An address we already know by name IS the function -- do not walk back
    # from it. bmp_draw_to_idle (0x00980AC0) and fake_simple_button
    # (0x00988418) are prologue-less leaves, and fstart puts both in the
    # function before them.
    f = va if name_of(va) else (cmd_fstart(va) or va)
    print("  " + "  " * _ind + label(f))
    if depth <= 0 or f in _seen:
        return
    _seen.add(f)
    done = set()
    for site, kind, t in calls_in(f, limit):
        if t in done:
            continue
        done.add(t)
        cmd_graph(t, depth - 1, limit, _seen, _ind + 1)


def cmd_reads(va):
    """Every function that loads `va` as a literal -- how a RAM global's
    readers and writers are found, since a global is never a branch target."""
    print("  functions whose literal pool holds 0x%08X:" % va)
    for la, nm in loads_of(va):
        f = cmd_fstart(la)
        print("    %s literal at 0x%08X  in %s  (+%d)"
              % (nm, la, label(f) if f else "?", la - f if f else -1))


if __name__ == "__main__":
    sys.exit(main())
