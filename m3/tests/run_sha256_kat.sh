#!/bin/sh
# m3/tests/run_sha256_kat.sh -- end-to-end SHA-256 KAT for m3/sha256_be.s.
#
# 1. Builds everything (m3/build.sh).
# 2. Cross-checks the host tool (m3tool sha256) against coreutils sha256sum.
# 3. Boots the bare-metal KAT in QEMU (SCTLR_EL1.A on), dumps RES and PAT by
#    QMP pmemsave, and checks by STATE INSPECTION: every digest equals host
#    sha256sum of the same bytes, every refusal returns its code and leaves
#    its 0xA5-prefilled out buffer untouched.
# 4. Boots the fault-probe variant: a deliberate misaligned load must take a
#    synchronous alignment fault (ESR EC 0x25, DFSC 0x21) and never finish.
# Exits 0 only if every check passes. Evidence lands in m3/build/kat/.
set -eu

M3=$(cd "$(dirname "$0")/.." && pwd -P)
B=$M3/build
W=$B/kat
FAILS=0

"$M3/build.sh"
T=$B/m3tool
rm -rf "$W"; mkdir -p "$W/vec"
LOG=$W/results.txt; : > "$LOG"

ok()  { echo "PASS $*" | tee -a "$LOG"; }
bad() { echo "FAIL $*" | tee -a "$LOG"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2' want '$3'"; fi; }
hsha() { sha256sum < "$1" | cut -d' ' -f1; }
pattern() { LC_ALL=C awk -v n="$1" 'BEGIN { for (i = 0; i < n; i++) printf "%c", i % 256 }'; }

# ---- host vectors ---------------------------------------------------------
: > "$W/vec/0_empty"
printf 'abc' > "$W/vec/1_abc"
printf 'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq' > "$W/vec/2_nist56"
pattern 1000 > "$W/vec/3_pat1000"
pattern 65536 > "$W/vec/4_pat65536"
check "host vector nist56 length" "$(wc -c < "$W/vec/2_nist56" | tr -d ' ')" 56
check "host vector pat65536 length" "$(wc -c < "$W/vec/4_pat65536" | tr -d ' ')" 65536

# ---- m3tool sha256 vs sha256sum -------------------------------------------
for n in 55 56 63 64 65 119 120 200000; do pattern "$n" > "$W/vec/x_pat$n"; done
for f in "$W"/vec/* "$B/kat.bin" "$B/m3tool" "$M3/../sha256_clean.c"; do
  check "m3tool sha256 $(basename "$f") ($(wc -c < "$f" | tr -d ' ') B)" "$("$T" sha256 "$f")" "$(hsha "$f")"
done
check "known abc digest" "$(hsha "$W/vec/1_abc")" ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

# ---- QEMU: main KAT -------------------------------------------------------
RES=0x40280000
"$M3/tools/qemu_inspect.sh" --out "$W/run" --marker 'M3_KAT_DONE' --fail-marker 'M3_KAT_FAULT' \
  --timeout 20 --bios "$B/kat_stub.bin" --loader "$B/kat.bin@0x40200000" \
  --region "res:$RES:0x240" --region "pat:0x40290000:0x10000" \
  || { bad "qemu_inspect main KAT (rc=$?)"; echo "RESULT: FAIL ($FAILS)"; exit 1; }
R=$W/run/res.bin
check "RES magic" "$("$T" hexfield "$R" 0 8)" 0x534552544b41334d
check "guest PAT == host pattern" "$(hsha "$W/run/pat.bin")" "$(hsha "$W/vec/4_pat65536")"
sctlr=$("$T" hexfield "$R" 8 8)
check "SCTLR_EL1.A set" "$(( sctlr & 2 ))" 2
check "CurrentEL == EL1" "$("$T" hexfield "$R" 16 8)" 0x0000000000000004
i=0
for v in 0_empty 1_abc 2_nist56 3_pat1000 4_pat65536; do
  check "vector $i rc" "$("$T" hexfield "$R" $((0x20 + 4 * i)) 4)" 0x00000000
  check "vector $i $v digest == sha256sum" "$("$T" bytes "$R" $((0x80 + 32 * i)) 32)" "$(hsha "$W/vec/$v")"
  i=$((i + 1))
done
check "refusal len=SHA256_MAX_LEN+1 rc" "$("$T" hexfield "$R" $((0x20 + 4 * 5)) 4)" 0x00000001
check "refusal misaligned data rc"      "$("$T" hexfield "$R" $((0x20 + 4 * 6)) 4)" 0x00000002
check "refusal misaligned out rc"       "$("$T" hexfield "$R" $((0x20 + 4 * 7)) 4)" 0x00000003
check "refusal data+len wrap rc"        "$("$T" hexfield "$R" $((0x20 + 4 * 8)) 4)" 0x00000004
a5=$(printf 'a5%.0s' $(seq 1 $((0x200 - 0x140))))
if [ "$("$T" bytes "$R" $((0x140)) $((0x200 - 0x140)))" = "$a5" ]; then
  ok "refusal out buffers untouched (0x140..0x200: 192 B, all 0xA5)"
else bad "refusal out buffers modified (0x140..0x200 not all 0xA5)"; fi
check "no fault recorded" "$("$T" hexfield "$R" $((0x200)) 8)" 0x0000000000000000

# ---- QEMU: alignment fault probe -----------------------------------------
if "$M3/tools/qemu_inspect.sh" --out "$W/fault" --marker 'M3_KAT_FAULT' --fail-marker 'M3_KAT_DONE' \
     --timeout 20 --bios "$B/kat_stub.bin" --loader "$B/kat_fault.bin@0x40200000" \
     --region "res:$RES:0x240"; then
  F=$W/fault/res.bin
  check "fault probe: fault magic" "$("$T" hexfield "$F" $((0x200)) 8)" 0x21544c554146334d
  esr=$("$T" hexfield "$F" $((0x208)) 8)
  check "fault probe: ESR EC = 0x25 (data abort, same EL)" "$(( (esr >> 26) & 0x3f ))" 37
  check "fault probe: DFSC = 0x21 (alignment fault)" "$(( esr & 0x3f ))" 33
  check "fault probe: FAR = PAT+1" "$("$T" hexfield "$F" $((0x218)) 8)" 0x0000000040290001
  check "fault probe: vector = current EL SPx sync (0x200)" "$("$T" hexfield "$F" $((0x220)) 8)" 0x0000000000000200
  check "fault probe: run never completed (no RES magic)" "$("$T" hexfield "$F" 0 8)" 0x0000000000000000
else
  bad "qemu_inspect fault probe (rc=$?)"
fi

if [ "$FAILS" -eq 0 ]; then echo "RESULT: PASS" | tee -a "$LOG"; exit 0; fi
echo "RESULT: FAIL ($FAILS)" | tee -a "$LOG"; exit 1
