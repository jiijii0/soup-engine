# EOS 50D.109 — pitfalls learned the hard way

The camera that works boots Magic Lantern with the Film page built in
(`tools/build-50d.py`, overlay in `integrations/magiclantern/50D.109/`).
It does not load `SE.BIN` and it does not take the GUI hook.

The notes below are from the heap-loaded `SE.BIN` attempt. Several were
hit more than once. Treat them as hard constraints unless you have a
new on-camera proof otherwise. Do not revive that path to "just try"
the hook again.

## Symptoms cheat sheet

| What you see | Likely cause |
| --- | --- |
| `ASSERT*.LOG`, dead buttons, ML menu broken | `user_mem_start` / `bss_end` carve starved ML malloc |
| Sensor-clean screen forever + CF LED stuck on | FIO or GUI hook touching the card / bad alias during clean |
| Boots, ML fine, **no new** `ML/SE.LOG` | Loader silent-failed (bad handle, magic, or stub delay) |
| `SE.LOG` has one early line then hang | Payload FIO (`se_note`) too early on the init path |
| `se_loaded=0`, Trash opens ML menu, `keys: 0` | Hook never called `se_key` (publish missing / wrong / cached stale) |
| Hook on → clean hang again | Hook path unsafe during clean (early publish or uncached stub read) |
| After runtime splice: ML menu ok-ish, no shutdown, `keys: 0` | Stub `blx` into heap `se_key` from GuiMainTask — do **not** call SE.BIN from the hook |

---

## 1. Do not raise `user_mem_start` / carve RAM for `SE.BIN`

**Mistake (repeated theme):** Linking SE at `0xCE000` and lifting `bss_end` /
`reserved` toward `0xFE000` so the blob “has a home.”

**Result:** DryOS / ML malloc pool collapsed (~29K). `menu_add_internal`
asserted every boot (`ASSERT*.LOG`). Buttons dead. Camera looked “half
alive.”

**Rule:** On 50D, keep stock `bss_end` / `reserved`. Load `SE.BIN` with
`AllocateMemory` after `ML_INIT_TASK` and relocate ABS32 at runtime.

---

## 2. FIO veneers must be the `ldr pc` entries, not the literal pool

**Mistake:** Off-by-4 on `FIO_ReadFile` / `FIO_CloseFile` (e.g. `0x9CC7C` /
`0x9CC1C` instead of `0x9CC78` / `0x9CC18`).

**Result:** Loader jumps into a constant word; fault / hang before any
`SE.LOG`.

**Rule:** Disassemble the veneer table; call the instruction, not the pool
word sitting next to it.

---

## 3. Never DMA `FIO_Read` into a cacheable buffer after BSS wipe

**Mistake:** Reading `SE.BIN` straight into a normal cached pointer (or into
a carved region that was just zeroed through the D-cache).

**Result:** Classic digic hang — sensor cleaning screen, CF access LED stuck
on. `sync_caches` *after* a hung Read never runs.

**Rule:** Destination = `buf | 0x40000000` (uncacheable alias). Prefer
`sync_caches` around the DMA. Same pattern ML uses with `fio_malloc` /
`UNCACHEABLE`.

---

## 4. `sync_caches` clobbers `r0`–`r3` — restore the FIO handle

**Mistake (subtle, cost a full debug cycle):**

```text
mov r5, handle
bl  sync_caches      @ destroys r0
orr r1, buf, #uncached
mov r2, #size
bl  FIO_ReadFile     @ r0 is garbage, not the handle
```

**Result:** Read fails quietly. Magic check fails. Stub returns. ML looks
perfect; `SE.LOG` never updates. Easy to blame “reloc” or “delay.”

**Rule:** `mov r0, r5` (or equivalent) **after** any call that may clobber
AAPCS scratch regs and **before** `FIO_ReadFile` / `Close`.

---

## 5. Do not “fix” clean-pass races with a long stub `msleep` before load

**Mistake:** Insert ~15s `msleep` in the loader hole before
`AllocateMemory` / Open / Read.

**Result:** Load never effectively happened (or failed oddly). Camera
behaved like stock ML; old `SE.LOG` stayed frozen. Looked like “hook
broken” or “reloc broken.”

**Rule:** Keep the stub load path minimal and proven. Defer **payload**
card I/O (first `se_note`, walks, screen) with `msleep` in `se_watch`, not
by parking the init-task loader for seconds.

---

## 6. No payload FIO on the init task during / right after load

**Mistake:** `se_note(...)` (full-file rewrite) from `se_start` immediately
after reloc — still on the init task, still near sensor clean.

**Result:** One log line made it (`se_start: reloc ok…` at 11:14) then
clean-screen hang + stuck LED. Proved load/reloc worked; early FIO did not.

**Rule:** `se_start` may `task_create` only. First card touch lives in
`se_watch` after a post-clean sleep.

---

## 7. Heap load needs a complete ABS32 reloc table (inclusive `__bss_end`)

**Mistake:** Building relocs only from `readelf` and/or scanning with
`base <= word < end` so the literal that holds `__bss_end` itself is
skipped.

**Result:** BSS zero loop wrong or skipped. Watcher dies before logging.
ML still fine.

**Rule:** Union ELF `R_ARM_ABS32` with a full image scan; upper bound
**inclusive** (`<= __bss_end`). Exclude header meta slots (`+0x08/+0x0C/+0x28/+0x34`).

---

## 8. Never publish `se_loaded` before reloc finishes

**Mistake:** Stub stores the heap load base into `se_loaded` and *then*
enters `se_pic_reloc`. GuiMainTask is already alive.

**Result:** Hook calls `load_base+0x44` → `se_key` on an **unrelocated**
image during sensor clean → hang + LED. Same class of failure as early FIO.

**Rule:** Leave `se_loaded == 0` until reloc + boot are done **and** the
post-clean wait in `se_watch` has elapsed. Only then publish the load base.

---

## 9. Do not uncached-read `se_loaded` from the GUI hook during clean

**Mistake:** Hook always does `orr r12, #0x40000000` before loading
`se_loaded`, including while it is still zero, on every early GUI event.

**Result:** Sensor-clean hang + stuck LED again — even with deferred
publish. A plain cached load of `0` (pass-through to ML) had previously
booted fine.

**Rule:** Hook reads `se_loaded` **cached**. After the payload writes the
base (post-sleep), call `sync_caches` so the hook’s cached view sees it.
Do not assume the uncached alias is safe for every early access to the
AUTOEXEC hole.

---

## 10. Re-enabling `gui_hook` is a regression risk — stage it

**Mistake (repeated):** Flip `gui_hook: true`, ship, get clean hang, bisect
under pressure, flip false, repeat without an isolated change.

**Update (staged attempt):** Even with cached `se_loaded` read, pass-through
when zero, and publish only after the watcher sleep, splicing
`gui_bl_site` → `se_hook` at patch time hung sensor clean + CF LED again.
So the unsafe window is not only “hook calls into SE too early” — **having
the bl retargeted for the whole boot** is enough to wedge this body during
clean. A future approach must leave the stock `bl handle_common_events_startup`
in place through clean, then install the splice at runtime after the
post-clean sleep (payload patches the instruction + `sync_caches`), or find
another input path that is not live during clean.

**Rule:**

1. Boot green with `gui_hook: false` and heap load + `SE.LOG`.
2. Publish `se_loaded` only after watcher sleep (prove `se_loaded≠0` in log
   with hook still false if useful).
3. Do **not** ship a boot-time GUI splice until a deferred/runtime splice is
   proven. Cached pass-through alone was not sufficient on camera.
4. Only then consume Trash / open the picker.

One behavioral change per card trip.

**Update (runtime splice on camera):** Every staged attempt failed on body:

- `se_loaded = load_base` + stub `blx` into heap `se_key` → GuiMainTask
  dead (`keys: 0`, no shutdown).
- Stub-local Trash eat / picker wake via `BGMT_MENU` → Canon menu + hang.
- Pass-through-only runtime splice after 15s sleep → missing clean screen
  + **Err 80** on capture.

**Recovery (current):** `gui_hook: false`, **do not plant** the hook hole,
**no** runtime `gui_bl` patch, **no** arming `se_loaded`. Stock `bl` and
an empty hook hole for the whole session. If the body is already wedged
(no clean UI, dead buttons, Err 80), boot **vendor** `AUTOEXEC.BIN` with
`SE.BIN` renamed aside until clean + buttons return, then reintroduce the
loader-only patch. GUI/picker input on 50D needs a path that is not this
`my_gui_task` splice.

---

## 11. Trash pass-through opens ML’s menu

**Mistake:** Toggle `se_scr_want` on Trash but `return 1`.

**Result:** Stock ML binding still opens the ML menu; picker never wins.

**Rule:** When Trash is the picker key, `return 0` after handling. Confirm
BGMT code via `keys` / `trash` counters in `SE.LOG` (`SE_BTN_TRASH` is
`0x09` on this port — verify on body if counters stay zero).

---

## 12. Hard-coded `SE_LOADED_ADDR` drifts when the stub shrinks

**Mistake:** `#define SE_LOADED_ADDR 0xB3B18` while `nm` says `se_loaded` is
now `0xB3B10` (layout changed). Writes hit `Lmagic` / wrong word.

**Rule:** After every stub edit, `arm-none-eabi-nm mlstub.elf | grep se_loaded`
and keep `cam.h` in lockstep — or stop hard-coding and patch the address
from the symbol at build time.

---

## 13. “No new log” is not always “SE dead”

**Mistakes:**

- Double waits (stub delay + watcher 15s) → log appears after ~30s.
- Reading a stale `SE.LOG` from an earlier session (compare timestamps to
  `SE.BIN`).
- Copy to `I:` failed (“device not ready”) while `card/` was updated —
  camera still ran yesterday’s binary.

**Rule:** Compare `LastWriteTime` of `AUTOEXEC.BIN` / `SE.BIN` / `SE.LOG`
on the **same** volume the camera boots from. Wait past the watcher sleep
before declaring failure.

---

## Known-good shape (recovery)

- Stock `bss_end` / `reserved` in `patch.json`.
- Stub: `ML_INIT` → `AllocateMemory(0x20000)` → Open → Read to
  `buf|0x40000000` with handle restored after any clobber → Close →
  `sync_caches` → magic via uncached → `blx buf+4`. **Do not** write
  `se_loaded` here.
- Payload: reloc → `se_start` only `task_create(watcher)` → watcher sleeps
  → then FIO / screen task. **No** runtime `gui_bl` patch, **no** arming
  `se_loaded` (that path: missing clean UI + Err 80 / Trash hangs).
- `gui_hook: false`; stock `bl`; **hook hole left empty** (planting it
  alone was enough to lose clean UI / buttons on camera).
- Do not retarget the Debug/Free Memory select slot at runtime. A build
  that did (`menu select armed` in `SE.LOG`, stub written over the spent
  loader) hung the body a few seconds later.
- Leave `se_auto_on` off. With it on, the watcher kept calling FindFirst
  on `DCIM` every few seconds. A session with only JPEGs (no develop
  line) still went bad after the next walk.

When in doubt, confirm clean screen + a normal shot before touching the
hook again. Vendor `AUTOEXEC` with `SE.BIN` aside is the hard recovery.
