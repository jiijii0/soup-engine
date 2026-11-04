#!/bin/sh
# Run the real payload in the simulator under every combination of the two
# ways Canon's reads misbehave, and check the result against values computed
# on the desktop. Both faults were found this way rather than on the camera.
set -e
CARD=out/simcard
SAMPLE="$1"
EXPECT_HASH="$2"
# Derived from the directory the card is built from, not written out as a
# number here. It was 11, and adding two stocks failed four passes with
# FAIL(stocks=13) -- a test that has to be hand-edited to keep agreeing
# with the thing it checks will eventually be edited to agree with a bug.
EXPECT_STOCKS=$(ls stocks/*.fs | wc -l | tr -d ' ')
fail=0

# The reference stock is soup800.fs, deliberately: it is the only one
# that turns on halation, so the simulated camera exercises the delay line,
# the per-hue stage, the split tone and the grain on every pass. It used to
# be cine.fs, which has since been retired -- and a reference stock that
# only exercises curves proves the least of any stock on the card.
#
# The byte-exact reference. The look checksum covers the pixels going *into*
# the encoder, so only comparing the finished files catches a fault in the
# encoder or in how the result reaches the card -- which is exactly where the
# camera's banding turned out to be.
./out/previewcheck "$SAMPLE" stocks/soup800.fs 2 out/simref.jpg >/dev/null

for short in 1 0; do
  for zero in 1 0; do
    rm -rf "$CARD"
    mkdir -p "$CARD/ML/FILM" "$CARD/DCIM/100EOS5D"
    cp stocks/*.fs "$CARD/ML/FILM/"
    ls stocks/*.fs | xargs -n1 basename > "$CARD/ML/FILM/INDEX.TXT"
    cp "$SAMPLE" "$CARD/DCIM/100EOS5D/IMG_0001.CR2"
    # Name the stock rather than relying on it being first in the index.
    # The payload starts on index 0, so this used to agree with the
    # reference only because cine.fs sorted first; retiring it silently
    # pointed the camera at acros.fs while the desktop still computed
    # soup800, and all four passes failed on a hash that was correct for
    # the stock each side was actually using.
    printf 'STOCK SOUP 800\n' > "$CARD/ML/SE.CFG"

    SE_SIM_SHORT_READS=$short SE_SIM_ZERO_READS=$zero \
      ./out/camsim "$CARD" 1 >out/simrun.txt 2>&1 && simrc=0 || simrc=$?

    # The payload alternates SE.LOG and SE.LO2 so a power cut can never take
    # both; whichever is longer is the live one.
    log="$CARD/ML/SE.LOG"
    if [ -f "$CARD/ML/SE.LO2" ] && \
       [ "$(wc -c < "$CARD/ML/SE.LO2")" -gt "$(wc -c < "$CARD/ML/SE.LOG" 2>/dev/null || echo 0)" ]; then
      log="$CARD/ML/SE.LO2"
    fi
    stocks=$(grep -oE 'stocks found: [0-9]+' "$log" 2>/dev/null | grep -oE '[0-9]+$' || echo 0)
    rc=$(grep -oE 'rc=-?[0-9]+' "$log" 2>/dev/null | head -1 || echo none)
    # From camsim, which reads the payload's own variable, not from the log.
    # se_note drops any line it cannot take the lock for, so a dropped
    # "look hash" line read as a wrong hash and failed a byte-perfect
    # develop about one run in eight.
    hash=$(grep -oE 'look hash [0-9A-F]+' out/simrun.txt 2>/dev/null | head -1 | sed 's/look hash //' || echo none)
    jpg=$(ls "$CARD"/DCIM/100EOS5D/*.FLM.JPG 2>/dev/null | head -1)
    size=$([ -n "$jpg" ] && wc -c < "$jpg" | tr -d ' ' || echo 0)
    bad=$(grep -c 'unparsable' "$log" 2>/dev/null || true)

    # What the screen did, from the payload's own log: what matters is what
    # the payload believes happened. camsim exits 3 if the screen painted
    # before anyone asked, never painted once opened, or kept painting
    # after it was closed.
    drew=$(grep -oE 'screen: blits=[0-9]+' "$log" 2>/dev/null | grep -oE '[0-9]+$' | tail -1 || echo 0)

    status=ok
    if [ -n "$jpg" ]; then
      cmp -s "$jpg" out/simref.jpg || status="FAIL(bytes differ from reference)"
    fi
    [ "${drew:-0}" -gt 0 ] || status="FAIL(the screen never painted)"
    # Nothing of ours goes near Canon's imaging buffers. camsim maps its
    # stand-ins PROT_NONE and points the state object straight at them, so
    # a payload that follows those pointers dies with SIGSEGV and camsim's
    # exit status carries it -- which the check above already covers. What
    # is asserted here is that the capture test actually ran, because a
    # silently skipped capture would pass that check for free.
    grep -q "no display hardware touched during the capture" out/simrun.txt \
                            || status="FAIL(the capture check did not run)"
    # No file should be left behind by a probe that no longer exists.
    probe=$(ls "$CARD"/ML/DB??.BIN "$CARD"/ML/S??_0.BIN 2>/dev/null | wc -l | tr -d ' ')
    [ "$probe" = 0 ] || status="FAIL($probe imaging-probe files written)"
    # the screen opens and closes on its own button, and says so
    # From camsim's own behavioural checks, not from the log. The screen task
    # no longer logs on the open or close path at all: se_note rewrites the
    # whole 16 KB log file, and doing that between the button and the panel
    # cost 3.46 s on the camera. camsim proves open and close by watching the
    # blit counter, which is the thing that actually matters.
    grep -q "opened: [1-9][0-9]* frames painted" out/simrun.txt \
                            || status="FAIL(the screen never opened)"
    grep -q "ok   closed, and the painting stopped" out/simrun.txt \
                            || status="FAIL(the screen never closed)"
    [ "$stocks" = "$EXPECT_STOCKS" ] || status="FAIL(stocks=$stocks)"
    [ "$rc" = "rc=0" ] || status="FAIL($rc)"
    [ "$hash" = "$EXPECT_HASH" ] || status="FAIL(hash)"
    [ "${bad:-0}" = "0" ] || status="FAIL($bad unparsable)"
    [ "$size" -gt 100000 ] || status="FAIL(jpeg $size bytes)"
    # Last, deliberately. This used to be checked first and every later
    # check overwrote it, so a camsim that exited 3 was reported as
    # FAIL(hash) -- one root cause wearing three different names.
    [ "$simrc" = 0 ] || status="FAIL(camsim exit $simrc -- see out/simrun.txt)"
    [ "$status" = ok ] || fail=1

    printf "  short_reads=%s zero_reads=%s  stocks=%-3s %-5s jpeg=%-7s rows=%-3s %s\n" \
           "$short" "$zero" "$stocks" "$rc" "$size" "$drew" "$status"
  done
done

# --------------------------------------------------------------------------
# The other source: the sensor data rather than Canon's preview.
#
# No byte-exact reference here. What is checked instead is the pair of
# bracketing checksums the payload logs -- every sample handed to the spool,
# and four rows read back out of it -- against the same two numbers computed
# straight from the raw by tools/hashcheck. That splits the only question
# this path has ever failed on: a wrong decode hash means the streaming
# lossless-JPEG decode is wrong, a right decode with a wrong readback means
# the spool is, and both right means anything left is downstream in the
# develop, which the host suite already checks byte for byte.
if [ $fail -eq 0 ]; then
  rm -rf "$CARD"
  mkdir -p "$CARD/ML/FILM" "$CARD/DCIM/100EOS5D"
  cp stocks/*.fs "$CARD/ML/FILM/"
  ls stocks/*.fs | xargs -n1 basename > "$CARD/ML/FILM/INDEX.TXT"
  cp "$SAMPLE" "$CARD/DCIM/100EOS5D/IMG_0001.CR2"
  # the same card as the runs above, so the only thing that varies is the
  # source
  echo "develop from the sensor data" > "$CARD/ML/SE.RAW"

  ./out/hashcheck "$SAMPLE" > out/rawref.txt
  want_d=$(grep -oE 'decode hash L=[0-9A-F]+ R=[0-9A-F]+' out/rawref.txt)
  want_r=$(grep -oE 'readback hash [0-9A-F]+' out/rawref.txt)

  ./out/camsim "$CARD" 1 >out/simraw.txt 2>&1 && rawrc=0 || rawrc=$?
  rlog="$CARD/ML/SE.LOG"
  if [ -f "$CARD/ML/SE.LO2" ] && \
     [ "$(wc -c < "$CARD/ML/SE.LO2")" -gt "$(wc -c < "$CARD/ML/SE.LOG" 2>/dev/null || echo 0)" ]; then
    rlog="$CARD/ML/SE.LO2"
  fi
  # From camsim, which reads the payload's own words, not from the log --
  # se_note drops any line it cannot take the lock for, so grepping these
  # out of ML/SE.LOG made the suite's most valuable assertion its flakiest.
  got_d=$(grep -oE 'decode L=[0-9A-F]+ R=[0-9A-F]+' out/simraw.txt 2>/dev/null \
          | sed 's/^decode /decode hash /' || echo none)
  got_r=$(grep -oE 'readback [0-9A-F]+' out/simraw.txt 2>/dev/null \
          | sed 's/^readback /readback hash /' || echo none)
  rawjpg=$(ls "$CARD"/DCIM/100EOS5D/*.FLM.JPG 2>/dev/null | head -1)
  rawsize=$([ -n "$rawjpg" ] && wc -c < "$rawjpg" | tr -d ' ' || echo 0)
  rawstat=ok
  [ "$rawrc" = 0 ]        || rawstat="FAIL(camsim exit $rawrc -- see out/simraw.txt)"
  grep -q 'from the sensor data' "$rlog" 2>/dev/null \
                          || rawstat="FAIL(SE.RAW was not picked up)"
  # Same reasoning: ask camsim, which reads the payload's own count of which
  # develop path ran, instead of grepping for a per-frame log line se_note is
  # allowed to drop. And it asserts what RAN, not what was logged.
  grep -qE '^\[sim\]   developed: [1-9][0-9]* from raw' out/simraw.txt \
                          || rawstat="FAIL(developed from the preview anyway)"
  [ "$got_d" = "$want_d" ] || rawstat="FAIL(decode hash: $got_d, wanted $want_d)"
  [ "$got_r" = "$want_r" ] || rawstat="FAIL(readback hash: $got_r, wanted $want_r)"
  [ "${rawsize:-0}" -gt 100000 ] || rawstat="FAIL(raw jpeg $rawsize bytes)"
  # the spool must not be left on the card: 50 MB of it
  [ -f "$CARD/ML/FE0.TMP" ] && rawstat="FAIL(spool left behind)"
  [ -f "$CARD/ML/FE1.TMP" ] && rawstat="FAIL(spool left behind)"
  printf "  from the sensor data: jpeg=%-8s %s\n" "$rawsize" "$rawstat"
  [ "$rawstat" = ok ] || fail=1
fi

# ---------------------------------------------------------------- pass 6
# Power-off, in a run of its own.
#
# Shutting the payload down is irreversible, so it cannot share a run with
# anything that checks a develop finished -- bolted onto the end of every
# run it made the raw pass intermittently produce no JPEG. A check that can
# break the run it lives in is a second source of failures, not a check.
rm -rf "$CARD"
mkdir -p "$CARD/ML/FILM" "$CARD/DCIM/100EOS5D"
cp stocks/*.fs "$CARD/ML/FILM/"
ls stocks/*.fs | xargs -n1 basename > "$CARD/ML/FILM/INDEX.TXT"
cp "$SAMPLE" "$CARD/DCIM/100EOS5D/IMG_0001.CR2"
# the same card as the runs above, so the only thing that varies is that
# this one gets switched off at the end
SE_SIM_POWEROFF=1 ./out/camsim "$CARD" 1 >out/simoff.txt 2>&1 && offrc=0 || offrc=$?
offstat=ok
[ "$offrc" = 0 ] || offstat="FAIL(camsim exit $offrc -- see out/simoff.txt)"
grep -q "ok   nothing drawn after the teardown began" out/simoff.txt                         || offstat="FAIL(drew after Canon began tearing the screen down)"
grep -q "ok   no card access after shutdown was requested" out/simoff.txt                         || offstat="FAIL(touched the card after shutdown)"
printf "  power off, screen open:               %s\n" "$offstat"
[ "$offstat" = ok ] || fail=1

# ---------------------------------------------------------------- pass 7
# The remembered look, across a power cycle.
#
# ML/SE.CFG is written when a button changes something and read at startup.
# A setting that does not survive being switched off is not a setting, so
# this writes one by hand and checks the payload comes up wearing it --
# including that the stock is matched BY NAME, which is what makes it
# survive .fs files being added to or removed from the card.
rm -rf "$CARD"
mkdir -p "$CARD/ML/FILM" "$CARD/DCIM/100EOS5D"
cp stocks/*.fs "$CARD/ML/FILM/"
ls stocks/*.fs | xargs -n1 basename > "$CARD/ML/FILM/INDEX.TXT"
cp "$SAMPLE" "$CARD/DCIM/100EOS5D/IMG_0001.CR2"
printf 'STOCK VELVIA\nSOURCE RAW\nSIZE HALF\n' > "$CARD/ML/SE.CFG"
./out/camsim "$CARD" 1 >out/simcfg.txt 2>&1 && cfgrc=0 || cfgrc=$?
cfgstat=ok
[ "$cfgrc" = 0 ] || cfgstat="FAIL(camsim exit $cfgrc -- see out/simcfg.txt)"
grep -q '^\[sim\]   active stock: VELVIA   source: RAW$' out/simcfg.txt \
                        || cfgstat="FAIL($(grep -o 'active stock:.*' out/simcfg.txt | head -1))"
printf "  remembered look restored:             %s\n" "$cfgstat"
[ "$cfgstat" = ok ] || fail=1

if [ $fail -eq 0 ]; then
  echo "  all read behaviours handled"
  # The screen findings are the same every run, so print them once, from the
  # last one, rather than four times.
  sed -n '/--- ML.s code ---/,$p' out/simrun.txt \
    | grep -vE 'one char per|^\[sim\]   y=|^\[sim\] reads=' | sed 's/^\[sim\]/ /'
else
  echo "  FAILURES"; exit 1
fi
