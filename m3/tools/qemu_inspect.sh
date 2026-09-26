#!/bin/sh
# m3/tools/qemu_inspect.sh -- Python-free QEMU state inspection for M3 gates.
#
# Boots qemu-system-aarch64 (-M virt -cpu cortex-a57 -m 128M, TCG single
# thread), waits for a UART marker in the serial file, stops the guest, dumps
# named guest-physical regions with QMP pmemsave, then quits. Fails closed:
# exit nonzero on timeout, fail-marker, QMP error, or any short/missing dump.
#
# Usage:
#   qemu_inspect.sh --out DIR --marker ERE [--fail-marker ERE] [--timeout SECS]
#                   [--bios FILE] [--loader FILE@ADDR]... [--region NAME:ADDR:LEN]...
#                   [--qemu-arg ARG]...
#   ADDR/LEN accept 0x hex or decimal. Region dumps land in DIR/NAME.bin.
#   Regions must lie inside guest RAM [0x40000000, +QEMU_MEM): pmemsave of
#   unmapped space returns zeros silently, so it is refused up front.
# Env overrides: QEMU (binary), QEMU_MACHINE, QEMU_CPU, QEMU_MEM.
# Outputs in DIR: serial.txt, qmp.log, qemu-cmd.txt, qemu-version.txt, NAME.bin.
# Exit codes: 0 ok, 1 usage/setup/QMP/dump failure, 2 marker timeout,
#             3 fail-marker seen, 4 QEMU exited before the marker.
set -eu

QEMU=${QEMU:-qemu-system-aarch64}
QEMU_MACHINE=${QEMU_MACHINE:-virt}
QEMU_CPU=${QEMU_CPU:-cortex-a57}
QEMU_MEM=${QEMU_MEM:-128M}

die() { echo "qemu_inspect: $*" >&2; exit "${RC:-1}"; }

OUT= MARKER= FAIL_MARKER= TIMEOUT=10 BIOS=
LOADERS= REGIONS= EXTRA=
nl='
'
while [ $# -gt 0 ]; do
  case $1 in
    --out) OUT=$2; shift 2 ;;
    --marker) MARKER=$2; shift 2 ;;
    --fail-marker) FAIL_MARKER=$2; shift 2 ;;
    --timeout) TIMEOUT=$2; shift 2 ;;
    --bios) BIOS=$2; shift 2 ;;
    --loader) LOADERS="$LOADERS$2$nl"; shift 2 ;;
    --region) REGIONS="$REGIONS$2$nl"; shift 2 ;;
    --qemu-arg) EXTRA="$EXTRA$2$nl"; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[ -n "$OUT" ] || die "--out required"
[ -n "$MARKER" ] || die "--marker required"
case $TIMEOUT in ''|*[!0-9]*) die "--timeout must be whole seconds" ;; esac
[ "$TIMEOUT" -ge 1 ] || die "--timeout must be >= 1"
command -v "$QEMU" >/dev/null || die "$QEMU not found"
command -v socat >/dev/null || die "socat not found"

to_dec() {  # strict unsigned number -> decimal; empty on error
  case $1 in
    0x*|0X*) h=${1#0?}; case $h in ''|*[!0-9a-fA-F]*) return 1 ;; esac ;;
    *) case $1 in ''|*[!0-9]*) return 1 ;; esac ;;
  esac
  printf '%d' "$1"
}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
SOCK=$OUT/qmp.sock
rm -f "$OUT/serial.txt" "$OUT/qmp.log" "$SOCK"

# Regions must lie wholly inside guest RAM: QEMU pmemsave of unmapped space
# silently returns zeros, which would fake "no effect" evidence.
RAM_BASE=$((0x40000000))
case $QEMU_MEM in
  *[!0-9]*[MG]|[MG]) die "QEMU_MEM must be NNM or NNG: $QEMU_MEM" ;;
  *M) RAM_SIZE=$(( ${QEMU_MEM%M} * 1048576 )) ;;
  *G) RAM_SIZE=$(( ${QEMU_MEM%G} * 1073741824 )) ;;
  *) die "QEMU_MEM must be NNM or NNG: $QEMU_MEM" ;;
esac
RAM_END=$((RAM_BASE + RAM_SIZE))

# Validate regions up front; remove stale dumps.
IFS_SAVE=$IFS
IFS=$nl
for r in $REGIONS; do
  IFS=: read -r name addr len <<EOF
$r
EOF
  case $name in ''|*[!A-Za-z0-9_.-]*) die "bad region name in '$r'" ;; esac
  to_dec "$addr" >/dev/null || die "bad region addr in '$r'"
  to_dec "$len" >/dev/null || die "bad region len in '$r'"
  a=$(to_dec "$addr"); n=$(to_dec "$len")
  [ "$n" -gt 0 ] || die "zero-length region '$r'"
  [ "$a" -ge "$RAM_BASE" ] && [ "$n" -le "$RAM_SIZE" ] && [ "$a" -le $((RAM_END - n)) ] \
    || die "region '$r' is not inside guest RAM [0x40000000, +$QEMU_MEM)"
  rm -f "$OUT/$name.bin"
done

# Build the QEMU argument list as positional parameters.
set -- "$QEMU" -M "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m "$QEMU_MEM" \
  -accel tcg,thread=single -display none -monitor none \
  -serial "file:$OUT/serial.txt" -qmp "unix:$SOCK,server=on,wait=off"
[ -z "$BIOS" ] || { [ -f "$BIOS" ] || die "bios not found: $BIOS"; set -- "$@" -bios "$BIOS"; }
for l in $LOADERS; do
  f=${l%@*}; a=${l##*@}
  [ "$f" != "$l" ] || die "loader must be FILE@ADDR: $l"
  [ -f "$f" ] || die "loader file not found: $f"
  to_dec "$a" >/dev/null || die "bad loader addr: $l"
  set -- "$@" -device "loader,file=$f,addr=$a,force-raw=on"
done
for e in $EXTRA; do set -- "$@" "$e"; done
IFS=$IFS_SAVE

"$QEMU" --version > "$OUT/qemu-version.txt"
{ for a in "$@"; do printf "'%s' " "$a"; done; echo; } > "$OUT/qemu-cmd.txt"

"$@" &
QPID=$!
cleanup() { kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; rm -f "$SOCK"; }
trap cleanup EXIT INT TERM

# Wait for the marker (bounded; 50 ms steps).
steps=$((TIMEOUT * 20)); i=0
while :; do
  if [ -n "$FAIL_MARKER" ] && grep -qE "$FAIL_MARKER" "$OUT/serial.txt" 2>/dev/null; then
    RC=3 die "fail marker /$FAIL_MARKER/ seen in serial output"
  fi
  grep -qE "$MARKER" "$OUT/serial.txt" 2>/dev/null && break
  kill -0 "$QPID" 2>/dev/null || RC=4 die "QEMU exited before marker /$MARKER/"
  i=$((i + 1))
  [ "$i" -le "$steps" ] || RC=2 die "timeout (${TIMEOUT}s) waiting for marker /$MARKER/"
  sleep 0.05
done
[ -S "$SOCK" ] || die "QMP socket missing: $SOCK"

# One QMP session: capabilities, stop, pmemsave each region, quit.
n=0
{
  printf '{"execute":"qmp_capabilities"}\n'
  printf '{"execute":"stop"}\n'
  IFS=$nl
  for r in $REGIONS; do
    IFS=: read -r name addr len <<EOF
$r
EOF
    printf '{"execute":"pmemsave","arguments":{"val":%s,"size":%s,"filename":"%s/%s.bin"}}\n' \
      "$(to_dec "$addr")" "$(to_dec "$len")" "$OUT" "$name"
  done
  sleep 0.5
  printf '{"execute":"quit"}\n'
} | socat -t 2 - "UNIX-CONNECT:$SOCK" > "$OUT/qmp.log" 2>"$OUT/socat.err" || true
IFS=$IFS_SAVE
wait "$QPID" 2>/dev/null || true
trap - EXIT INT TERM
rm -f "$SOCK"

grep -q '"error"' "$OUT/qmp.log" && die "QMP error (see $OUT/qmp.log)"
IFS=$nl
for r in $REGIONS; do
  IFS=: read -r name addr len <<EOF
$r
EOF
  f=$OUT/$name.bin
  [ -f "$f" ] || die "region dump missing: $f"
  got=$(wc -c < "$f")
  [ "$got" -eq "$(to_dec "$len")" ] || die "region $name: got $got bytes, want $(to_dec "$len")"
  n=$((n + 1))
done
IFS=$IFS_SAVE
echo "qemu_inspect: OK marker /$MARKER/, $n region(s) -> $OUT"
