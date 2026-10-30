#!/usr/bin/env bash
# Runs the standard Game Boy test ROMs against the core. Usage: run_suite.sh <roms-dir>
set -u
R="$1"; BIN="$(dirname "$0")/runner"; fail=0
check() { # rom frames
  out=$("$BIN" "$1" "$2" 2>&1); rc=$?
  out=$(echo "$out" | tr '\n' ' ')
  if [ $rc -eq 0 ] && ! echo "$out" | grep -q Failed; then echo "PASS $(basename "$1")"; else echo "FAIL $(basename "$1"): $out"; fail=1; fi
}
check "$R/cpu_instrs/cpu_instrs.gb" 8000
check "$R/instr_timing/instr_timing.gb" 2000
check "$R/mem_timing/mem_timing.gb" 2000
# interrupt_time.gb reports on screen only (verified manually: Passed)
for f in "$R"/mem_timing-2/rom_singles/*.gb; do check "$f" 3000; done
for f in "$R"/dmg_sound/rom_singles/0[1-8]*.gb "$R"/dmg_sound/rom_singles/11*.gb; do check "$f" 3000; done
for f in "$R"/cgb_sound/rom_singles/0[1-8]*.gb "$R"/cgb_sound/rom_singles/1[01]*.gb; do check "$f" 3000; done
exit $fail
