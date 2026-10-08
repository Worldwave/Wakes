#!/bin/sh
# Host harness build for routetest / uitest / miditest. Replicates firmware/CMakeLists.txt's
# include-path order (generated overrides, our replacements, the shim, the eurorack root)
# and its Plaits flags, so what the harness links is what the firmware compiles.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# Build products go outside the repo unless told otherwise.
SP=${SP1_HOST_BUILD_DIR:-${TMPDIR:-/tmp}/wakes-sp1-host}
mkdir -p "$SP"
ER=$ROOT/third_party/eurorack
SRC=$ROOT/firmware/src
FELDD=$ROOT/third_party/feldd
UAC=$ROOT/third_party/sp1-usb-audio/upstream/usb-audio
# #32: Plaits' block size, as CONFIG_SP1_PLAITS_BLOCK (default 24, the firmware's). The
# overrides and the cached archive live in a directory per size, so switching sizes can
# never link an archive built for the other one.
SP1_PLAITS_BLOCK=${SP1_PLAITS_BLOCK:-24}
export SP1_PLAITS_BLOCK
OVR=$SP/hostovr$SP1_PLAITS_BLOCK
GEN=$SP/hostgen
OBJ=$SP/hostobj$SP1_PLAITS_BLOCK

python3 "$HERE/mkovr.py" "$OVR" >/dev/null
mkdir -p "$GEN" "$OBJ"
python3 "$ROOT/tools/gen_engines.py" "$ROOT/config/engines.csv" "$GEN/sp1_engines_gen.h" >/dev/null
# M6 (#50): the PRST field table -- also where a format change without a version bump fails.
python3 "$ROOT/tools/gen_prst.py" "$GEN/sp1_prst_gen.h" >/dev/null
# The MIDI script (M5a) as the firmware build generates it -- with ONE change: its `pickup`
# forced to `sum`, because miditest.cc checks the offset maths and the other suites were
# written against offsets. miditest_pickup.cc runs the shipped script as it is otherwise, once
# per other pickup (below); miditest_alt.cc uses a second script of its own.
# $1 = pickup, $2 = the ini to write. Fails if the script has no `pickup` line to replace.
pickup_ini() {
  sed -E 's/^pickup[[:space:]]*=.*/pickup = '"$1"'/' "$ROOT/config/midi.ini" > "$2"
  grep -Eq "^pickup = $1\$" "$2" || { echo "hostbuild: no pickup line in config/midi.ini" >&2; exit 1; }
}
pickup_ini sum "$GEN/midi-sum.ini"
# ...and its legato forced to `off`, Yarns' default, whatever the shipped script is set to for
# a hardware test: miditest.cc checks legato off; miditest_alt.cc checks legato on.
sed -i -E 's/^legato[[:space:]]*=.*/legato = off/' "$GEN/midi-sum.ini"
grep -Eq '^legato = off$' "$GEN/midi-sum.ini" || { echo "hostbuild: no legato line in config/midi.ini" >&2; exit 1; }
python3 "$ROOT/tools/gen_midi.py" "$GEN/midi-sum.ini" "$GEN/sp1_midi_gen.h" >/dev/null

INC="-I$OVR -I$SRC/plaits_ovr -I$SRC/plaits_shim -I$ER -I$SRC -I$GEN"
CXXFLAGS="-std=gnu++14 -O2 -funroll-loops -D_DEFAULT_SOURCE -DTEST -DCONFIG_SP1_PLAITS=1 \
-DCONFIG_SP1_MIDI=1 -DCONFIG_SP1_STRING_VOICES=2 -DCONFIG_SP1_PARTICLES=2 \
-DCONFIG_SP1_MODAL_MODES=12 -DCONFIG_SP1_PLAITS_BLOCK=$SP1_PLAITS_BLOCK \
-DCONFIG_SP1_TRIGGER_DELAY_SAMPLES=${SP1_TRIGGER_DELAY_SAMPLES:-24} \
-Wno-unused-variable -Wno-unused-parameter -Wno-unused-local-typedefs -Wno-sign-compare"
CFLAGS="-std=gnu11 -O2 -DCONFIG_SP1_PLAITS=1 -DCONFIG_SP1_MIDI=1 -DCONFIG_SP1_PLAITS_BLOCK=$SP1_PLAITS_BLOCK"

# The Plaits/Marbles source list, with an overridden or replaced .cc swapped in.
list_sources() {
  for f in $ER/plaits/dsp/voice.cc $ER/plaits/resources.cc \
           $ER/stmlib/dsp/atan.cc $ER/stmlib/dsp/units.cc $ER/stmlib/utils/random.cc \
           $ER/plaits/dsp/engine/*.cc $ER/plaits/dsp/engine2/*.cc \
           $ER/plaits/dsp/chords/*.cc $ER/plaits/dsp/fm/*.cc \
           $ER/plaits/dsp/physical_modelling/*.cc $ER/plaits/dsp/speech/*.cc \
           $ER/marbles/random/*.cc $ER/marbles/ramp/*.cc $ER/marbles/resources.cc; do
    rel=${f#$ER/}
    if [ -f "$SRC/plaits_ovr/$rel" ]; then echo "$SRC/plaits_ovr/$rel"
    elif [ -f "$OVR/$rel" ];        then echo "$OVR/$rel"
    else                                 echo "$f"
    fi
  done
}

# ⚠️ The Plaits/Marbles archive is CACHED, and it is rebuilt whenever any of its inputs --
# a vendored source, one of our replacements, or firmware/CMakeLists.txt itself -- is newer
# than the archive. Do not turn that check into an unconditional "if it exists, keep it":
# a stale archive is exactly how M4c's section 9 came to report twelve failures that were
# not there. SP1_HOST_REBUILD=1 forces it.
# (Our replacement HEADERS count too: voice.h shapes every Plaits object.)
LIB=$OBJ/libsp1dsp.a
stale=1
if [ -f "$LIB" ] && [ -z "$SP1_HOST_REBUILD" ]; then
  stale=0
  for f in $(list_sources) $(find "$SRC/plaits_ovr" -name '*.h') "$SRC/sp1_zero.cc" \
           "$ROOT/firmware/CMakeLists.txt" "$HERE/mkovr.py"; do
    [ "$f" -nt "$LIB" ] && { stale=1; break; }
  done
fi
if [ "$stale" = 1 ]; then
  rm -f "$LIB" $OBJ/dsp_*.o
  n=0
  for f in $(list_sources); do
    o=$OBJ/dsp_$n.o; n=$((n+1))
    g++ $CXXFLAGS $INC -c "$f" -o "$o"
  done
  # #36: the overridden FxEngine::Clear / DelayLine::Reset call plaits::sp1_zero. Same
  # flag as the firmware (firmware/CMakeLists.txt), so the host runs the same loops.
  g++ $CXXFLAGS -fno-tree-loop-distribute-patterns -fno-unroll-loops -c "$SRC/sp1_zero.cc" -o $OBJ/dsp_zero.o
  ar rcs "$LIB" $OBJ/dsp_*.o
fi

g++ $CXXFLAGS $INC -c $SRC/sp1_synth.cc   -o $OBJ/sp1_synth.o
g++ $CXXFLAGS $INC -c $SRC/sp1_marbles.cc -o $OBJ/sp1_marbles.o
g++ $CXXFLAGS $INC -c $SRC/sp1_midi.cc    -o $OBJ/sp1_midi.o
gcc $CFLAGS  $INC -c $SRC/sp1_plaits_ui.c  -o $OBJ/pui.o
gcc $CFLAGS  $INC -c $SRC/sp1_marbles_ui.c -o $OBJ/mui.o
gcc $CFLAGS  $INC -c $SRC/sp1_prst.c       -o $OBJ/prst.o

# Binaries land in the build dir, never in the repo.
for t in "$@"; do
  [ -f "$t" ] || t=$HERE/$t
  out=$SP/$(basename "${t%.*}")
  case $t in
    *uactest.c)
      # USB audio out (M5c): Ryan Gilmore's ring + regulator with Wakes' patch (word copies,
      # third_party/sp1-usb-audio/patches/uacring.patch) applied to a copy, STRICTLY, and
      # built the way the firmware builds it; sp1_uac_tuning.h force-included. EXTRA_UAC_FLAGS
      # is for the re-tuning sweep only (README).
      python3 "$ROOT/tools/apply_patch.py" "$UAC/uacring.c" \
          "$ROOT/third_party/sp1-usb-audio/patches/uacring.patch" "$SP/uacring.c"
      gcc -std=c99 -O2 -Wall -fno-tree-loop-distribute-patterns -fno-unroll-loops \
          -include "$SRC/sp1_uac_tuning.h" $EXTRA_UAC_FLAGS -I"$UAC" \
          "$t" "$SP/uacring.c" -o "$out" ;;
    *test_usb_rt_parse.c)
      # feldd's own host test of the packet validator Wakes uses (third_party/feldd).
      gcc -std=gnu11 -O2 "$t" "$FELDD/src/usb_rt_parse.c" -o "$out" ;;
    *miditest_alt.cc)
      # The MIDI suite's second script (midi-alt.ini: legato, portamento, bindings, omni):
      # its own generated header and its own objects for everything that reads it.
      ALT=$SP/hostgen_alt
      mkdir -p "$ALT"
      python3 "$ROOT/tools/gen_midi.py" "$HERE/midi-alt.ini" "$ALT/sp1_midi_gen.h" >/dev/null
      AINC="-I$ALT $INC"
      g++ $CXXFLAGS $AINC -c $SRC/sp1_midi.cc  -o $OBJ/sp1_midi_alt.o
      g++ $CXXFLAGS $AINC -c $SRC/sp1_synth.cc -o $OBJ/sp1_synth_alt.o
      gcc $CFLAGS  $AINC -c $SRC/sp1_plaits_ui.c  -o $OBJ/pui_alt.o
      gcc $CFLAGS  $AINC -c $SRC/sp1_marbles_ui.c -o $OBJ/mui_alt.o
      g++ $CXXFLAGS $AINC "$t" $OBJ/sp1_synth_alt.o $OBJ/sp1_marbles.o $OBJ/sp1_midi_alt.o \
          $OBJ/pui_alt.o $OBJ/mui_alt.o "$LIB" -lm -o "$out" ;;
    *miditest_pickup.cc)
      # The shipped script under pickup shared and takeover (Adara, M5a round 4): one
      # binary each, miditest_pickup_shared and miditest_pickup_takeover.
      for m in shared takeover; do
        PG=$SP/hostgen_$m
        mkdir -p "$PG"
        pickup_ini $m "$PG/midi.ini"
        python3 "$ROOT/tools/gen_midi.py" "$PG/midi.ini" "$PG/sp1_midi_gen.h" >/dev/null
        PINC="-I$PG $INC"
        g++ $CXXFLAGS $PINC -c $SRC/sp1_midi.cc  -o $OBJ/sp1_midi_$m.o
        g++ $CXXFLAGS $PINC -c $SRC/sp1_synth.cc -o $OBJ/sp1_synth_$m.o
        gcc $CFLAGS  $PINC -c $SRC/sp1_plaits_ui.c  -o $OBJ/pui_$m.o
        gcc $CFLAGS  $PINC -c $SRC/sp1_marbles_ui.c -o $OBJ/mui_$m.o
        g++ $CXXFLAGS $PINC "$t" $OBJ/sp1_synth_$m.o $OBJ/sp1_marbles.o $OBJ/sp1_midi_$m.o \
            $OBJ/pui_$m.o $OBJ/mui_$m.o "$LIB" -lm -o "${out}_$m"
        [ "$m" = takeover ] || echo "${out}_$m"
      done
      out=${out}_takeover ;;
    *.cc) g++ $CXXFLAGS $INC "$t" $OBJ/sp1_synth.o $OBJ/sp1_marbles.o $OBJ/sp1_midi.o \
              $OBJ/pui.o $OBJ/mui.o "$LIB" -lm -o "$out" ;;
    *.c)  gcc $CFLAGS  $INC "$t" $OBJ/pui.o $OBJ/mui.o $OBJ/prst.o $OBJ/sp1_midi.o -lm -o "$out" ;;
  esac
  echo "$out"
done
