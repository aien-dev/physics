#!/usr/bin/env python3
"""
seam2_physics_harness.py - Milestone 2 (PHYSICS_BOOT) External Execution Seam

Runs PHYSICS under QEMU virt (AArch64) and qualifies it by reading machine
state directly through the QEMU monitor (QMP `xp`), not by trusting UART
strings. UART lines are used only to know when a run has reached a terminal
state and to confirm refusal markers.

Every image is built from source for each run (m2_build); nothing cached.

Runtime gates:
  PHYSICS_ENTRY_EL_PASS                recorded CurrentEL == contract value;
                                       real EL2 entry is refused
  PHYSICS_DESCRIPTOR_INGRESS_PASS      PHYSICS-owned copy == contract descriptor;
                                       header/profile hostile cases refused
  PHYSICS_BOOT_QEMU_PASS               golden telemetry in order, allocator
                                       header exact, clean quiescence
  PHYSICS_FRAME_BOUNDS_PASS            full exhaustion: sequential, no duplicate,
                                       last < DRAM_END, then exhaustion; bitmap
                                       confined to PHYSICS_STATIC_DATA (canary)
  PHYSICS_RESERVED_FRAME_REFUSAL_PASS  every reserved address refused
  PHYSICS_CAP_ROOT_PASS                all 10 CAP_ROOT fields == contract; root
                                       cannot authorize outside
                                       [FREE_FRAME_BASE, DRAM_END)
  PHYSICS_EXCEPTION_STATE_CAPTURE_PASS trap frame read from memory: 31 GPR
                                       sentinels, SP, slot, EL, ESR, ELR, SPSR,
                                       FAR, FAR_VALID (BRK and data abort)
  PHYSICS_CORRUPTION_REFUSAL_PASS      full hostile ingress matrix refused before
                                       any authority is granted (memory-checked)

QEMU qualification only. It says nothing about DGX Spark hardware.
"""

import json
import os
import re
import shutil
import socket
import subprocess
import time

import m2_build

BUILD_DIR = os.path.join("build", "seam2")

BOOT_STATE = 0x40205800
ENTRY_RECORD = BOOT_STATE + 0x40
TRAP_FRAME = BOOT_STATE + 0x80
TEST_RECORD = BOOT_STATE + 0x200
CAP_TABLE = 0x40206000
STATIC_DATA = 0x40207000
BITMAP = STATIC_DATA + 0x20
FREE_FRAME_BASE = 0x40208000
DRAM_END = 0x48000000
TOTAL_FRAMES = (DRAM_END - FREE_FRAME_BASE) >> 12
BITMAP_BYTES = (TOTAL_FRAMES + 7) // 8
TRAP_MAGIC = 0x314D524650415254      # 'TRAPFRM1'
UNIT_MAGIC = 0x31305454494E5532      # '2UNITT01'
CANARY = 0xA5
SENTINEL_BASE = 0x53454E5400000000   # 'SENT' << 32
ALIGN_BASE = 0x40300000               # RAM (free frame region) for the alignment fault


def sentinel(n):
    return SENTINEL_BASE | n


# ---------------------------------------------------------------------------
# QEMU + QMP
# ---------------------------------------------------------------------------

class Qmp:
    def __init__(self, path, deadline):
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.02)
        self.f = self.sock.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            r = json.loads(line)
            if "return" in r:
                return r["return"]
            if "error" in r:
                raise RuntimeError(r["error"])

    def hmp(self, text):
        return self.cmd("human-monitor-command", **{"command-line": text})

    def read_u64(self, addr, count):
        out = self.hmp(f"xp /{count}gx {addr:#x}")
        vals = []
        for line in out.splitlines():
            if ":" in line:
                vals += [int(v, 16) for v in line.split(":", 1)[1].split()]
        assert len(vals) == count, out
        return vals

    def read_bytes(self, addr, count):
        out = self.hmp(f"xp /{count}bx {addr:#x}")
        vals = []
        for line in out.splitlines():
            if ":" in line:
                vals += [int(v, 16) for v in line.split(":", 1)[1].split()]
        assert len(vals) == count, (count, len(vals))
        return vals

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.sock.close()


TERMINAL = ("PHYSICS: QUIESCENT_READY", "PANIC", "ATLAS: REFUSE",
            "PHYSICS: TRAP_FRAME_CAPTURED", "PHYSICS: UNIT_TESTS_DONE")


def run_qemu(run_dir, atlas_bin, physics_bin, inspect, extra_loaders=(),
             machine="virt", timeout_sec=20.0):
    """Boot atlas_bin (as firmware) + physics_bin, wait for a terminal UART
    line, run inspect(qmp) while the machine is still up, then quit.
    Returns (uart_lines, inspect_result)."""
    os.makedirs(run_dir, exist_ok=True)
    serial = os.path.join(run_dir, "serial.txt")
    sock = os.path.join(run_dir, "qmp.sock")
    for p in (serial, sock):
        if os.path.exists(p):
            os.remove(p)
    cmd = ["qemu-system-aarch64", "-M", machine, "-cpu", "cortex-a57", "-m", "128M",
           "-accel", "tcg,thread=single", "-display", "none", "-monitor", "none",
           "-serial", f"file:{serial}", "-qmp", f"unix:{sock},server=on,wait=off",
           "-bios", atlas_bin,
           "-device", f"loader,file={physics_bin},addr=0x40200000,force-raw=on"]
    for path, addr in extra_loaders:
        cmd += ["-device", f"loader,file={path},addr={addr:#x},force-raw=on"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.time() + timeout_sec
    lines = []
    qmp = None
    try:
        qmp = Qmp(sock, deadline)
        while time.time() < deadline:
            with open(serial, errors="replace") as f:
                lines = [l.strip() for l in f.read().splitlines()]
            if any(t in l for l in lines for t in TERMINAL):
                break
            time.sleep(0.05)
        time.sleep(0.2)  # let the final telemetry drain
        with open(serial, errors="replace") as f:
            lines = [l.strip() for l in f.read().splitlines()]
        result = inspect(qmp) if inspect else None
    finally:
        if qmp:
            qmp.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return lines, result


# ---------------------------------------------------------------------------
# Test images
# ---------------------------------------------------------------------------

QUIESCENT_ANCHOR = "    ldr x0, =msg_quiescent_ready\n    bl print_string\n"


def physics_variant(name, snippet, rodata=""):
    """physics.s with the normal-boot QUIESCENT_READY emission replaced by a
    test snippet. Everything before it (ingress, EL check, VBAR, frame
    authority, CAP_ROOT) is the canonical code path."""
    with open("physics.s") as f:
        src = f.read()
    assert src.count(QUIESCENT_ANCHOR) == 1, "physics.s quiescent anchor must be unique"
    out_dir = os.path.join(BUILD_DIR, name)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"physics_{name}.s")
    with open(path, "w") as f:
        f.write(src.replace(QUIESCENT_ANCHOR, snippet) + rodata)
    elf, binary = m2_build.build_physics(out_dir, ".", entry_source=path)
    _, atlas = m2_build.build_atlas(out_dir, binary, ".")
    return elf, binary, atlas


def symbol(elf, name):
    out = subprocess.run(["aarch64-linux-gnu-nm", elf], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise KeyError(name)


RESERVED_PROBES = [  # (address, expected is_frame_reserved)
    (0x0, 1), (0x40000000, 1), (0x401FE000, 1), (0x40200000, 1), (0x40201000, 1),
    (0x40205800, 1), (0x40206000, 1), (0x40207000, 1), (0x40207FFF, 1),
    (0x48000000, 1), (0xFFFFFFFFFFFFF000, 1), (0x40208000, 0), (0x47FFF000, 0),
]

CAP_PROBES = [  # (address, size, expected validate_capability)
    (0x40208000, 0x1000, 1), (0x47FFF000, 0x1000, 1), (0x40208000, 0x7DF8000, 1),
    (0x40207000, 0x1000, 0), (0x47FFF000, 0x2000, 0), (0x48000000, 0x1000, 0),
    (0x40208000, 0xFFFFFFFFFFFFFFFF, 0), (0x40200000, 0x1000, 0), (0x0, 0x1000, 0),
    (0x40206000, 0x50, 0),
]


def unit_snippet():
    s = [
        "    /* ---- Seam 2 unit image: runs after CAP_ROOT genesis ---- */",
        f"    ldr x19, ={TEST_RECORD:#x}",
        "    ldr x20, =0x40208000          /* expected next frame */",
        "    mov x21, #0                   /* allocation count */",
        "    mov x22, #0                   /* last frame */",
        "    mov x23, #1                   /* order_ok */",
        ".Lunit_alloc_loop:",
        "    bl allocate_frame",
        "    cbz x0, .Lunit_alloc_done",
        "    cmp x0, x20",
        "    csel x23, xzr, x23, ne",
        "    mov x22, x0",
        "    add x20, x0, #0x1000",
        "    add x21, x21, #1",
        "    b .Lunit_alloc_loop",
        ".Lunit_alloc_done:",
        "    str x21, [x19, #0x00]",
        "    str x22, [x19, #0x08]",
        "    bl allocate_frame",
        "    str x0, [x19, #0x10]          /* after exhaustion: must be 0 */",
        "    str x23, [x19, #0x18]",
        "    mov x24, #0                   /* reserved-probe result bits */",
    ]
    for i, (addr, _) in enumerate(RESERVED_PROBES):
        s += [f"    ldr x0, ={addr:#x}", "    bl is_frame_reserved",
              "    and x0, x0, #1", f"    lsl x0, x0, #{i}", "    orr x24, x24, x0"]
    s += ["    str x24, [x19, #0x20]", "    mov x25, #0                   /* cap-probe result bits */"]
    for i, (addr, size, _) in enumerate(CAP_PROBES):
        s += [f"    ldr x0, ={CAP_TABLE:#x}", "    mov w1, #1",
              f"    ldr x2, ={addr:#x}", f"    ldr x3, ={size:#x}", "    bl validate_capability",
              "    and x0, x0, #1", f"    lsl x0, x0, #{i}", "    orr x25, x25, x0"]
    s += ["    str x25, [x19, #0x28]",
          f"    ldr x0, ={UNIT_MAGIC:#x}", "    str x0, [x19, #0x30]",
          "    ldr x0, =msg_unit_done", "    bl print_string", ""]
    return "\n".join(s) + "\n"


UNIT_RODATA = '\n    .section .rodata\nmsg_unit_done:\n    .asciz "PHYSICS: UNIT_TESTS_DONE\\n"\n'


def trap_snippet(kind):
    s = ["    /* ---- Seam 2 trap image: load sentinels, then fault ---- */"]
    if kind == "align":
        # QEMU 8.2 does not fault unaligned Device accesses with the MMU off,
        # so the test image turns on SCTLR_EL1.A (alignment checking).
        s += ["    mrs x0, sctlr_el1", "    orr x0, x0, #2", "    msr sctlr_el1, x0", "    isb"]
    s += ["    msr nzcv, xzr                 /* deterministic SPSR.NZCV */"]
    for n in range(31):
        s.append(f"    ldr x{n}, ={sentinel(n):#x}")
    if kind == "brk":
        s += ["    .global seam2_fault_site", "seam2_fault_site:", "    brk #0x4d32"]
    else:
        # Unaligned 64-bit load with SCTLR_EL1.A set: alignment Data Abort
        # with a valid FAR.
        s += [f"    ldr x28, ={ALIGN_BASE:#x}",
              "    .global seam2_fault_site", "seam2_fault_site:", "    ldr x0, [x28, #1]"]
    return "\n".join(s) + "\n"


# ---------------------------------------------------------------------------
# Hostile ingress matrix
# ---------------------------------------------------------------------------

PANIC_COOKIE = "PHYSICS: PANIC_BAD_COOKIE"
PANIC_PAYLOAD = "PHYSICS: PANIC_BAD_PAYLOAD_SIZE"
PANIC_PTR = "PHYSICS: PANIC_BAD_DESCRIPTOR_PTR"
PANIC_MAGIC = "PHYSICS: PANIC_BAD_DESC_MAGIC"
PANIC_VERSION = "PHYSICS: PANIC_BAD_DESC_VERSION"
PANIC_LENGTH = "PHYSICS: PANIC_BAD_DESC_LENGTH"
PANIC_RESERVED = "PHYSICS: PANIC_BAD_DESC_RESERVED"
PANIC_INGRESS = "PHYSICS: PANIC_INGRESS_CORRUPT"
PANIC_EL = "PHYSICS: PANIC_UNCONTRACTED_EL"

# (tag, description, defsyms, expected panic, descriptor-ingress case?)
HOSTILE_CASES = [
    ("bad_cookie", "bad verification cookie", {"ABI_X2": "0xDEADBEEFCAFEBABE"}, PANIC_COOKIE, False),
    ("bad_payload_size", "x1 payload size != 6144", {"ABI_X1": "4096"}, PANIC_PAYLOAD, False),
    ("bad_desc_ptr", "descriptor pointer moved", {"ABI_X0": "0x40100000"}, PANIC_PTR, False),
    ("bad_magic", "bad descriptor magic", {"DESC_MAGIC": "0x4D424453435F3032"}, PANIC_MAGIC, True),
    ("version_2", "unsupported descriptor version 2", {"DESC_VERSION": "2"}, PANIC_VERSION, True),
    ("version_0", "unsupported descriptor version 0", {"DESC_VERSION": "0"}, PANIC_VERSION, True),
    ("len_truncated", "truncated length 32", {"DESC_LENGTH": "32"}, PANIC_LENGTH, True),
    ("len_oversized", "oversized length 128", {"DESC_LENGTH": "128"}, PANIC_LENGTH, True),
    ("reserved_low", "reserved flags bit 0 set", {"DESC_FLAGS": "1"}, PANIC_RESERVED, True),
    ("reserved_high", "reserved flags bit 63 set", {"DESC_FLAGS": "0x8000000000000000"}, PANIC_RESERVED, True),
    ("ram_size_zero", "DRAM size zero", {"DESC_RAM_SIZE": "0"}, PANIC_INGRESS, True),
    ("ram_overflow", "DRAM base + size overflow", {"DESC_RAM_SIZE": "0xFFFFFFFFFFFFFFFF"}, PANIC_INGRESS, True),
    ("ram_oversized", "valid base + oversized size", {"DESC_RAM_SIZE": "0x09000000"}, PANIC_INGRESS, True),
    ("ram_undersized", "undersized RAM", {"DESC_RAM_SIZE": "0x07000000"}, PANIC_INGRESS, True),
    ("ram_base_lower", "RAM base shifted lower", {"DESC_RAM_BASE": "0x3FF00000"}, PANIC_INGRESS, True),
    ("ram_base_upper", "RAM base shifted upper", {"DESC_RAM_BASE": "0x40100000"}, PANIC_INGRESS, True),
    ("ram_beyond_profile", "valid-looking range beyond profile",
     {"DESC_RAM_SIZE": "0x100000000"}, PANIC_INGRESS, True),
    ("ram_excludes_physics", "DRAM range excluding PHYSICS", {"DESC_RAM_BASE": "0x40300000"}, PANIC_INGRESS, True),
    ("uart_out_of_profile", "UART outside machine profile", {"DESC_UART_BASE": "0x08000000"}, PANIC_INGRESS, True),
    ("uart_misaligned", "misaligned UART address", {"DESC_UART_BASE": "0x09000001"}, PANIC_INGRESS, True),
    ("physics_base_moved", "PHYSICS base moved", {"DESC_PHYSICS_BASE": "0x40300000"}, PANIC_INGRESS, True),
    ("physics_size_wrong", "PHYSICS size wrong", {"DESC_PHYSICS_SIZE": "4096"}, PANIC_INGRESS, True),
]


def no_authority_granted(q):
    """Memory proof that ingress refusal happened before any authority:
    entry record, allocator header and CAP_ROOT slot are still zero."""
    entry = q.read_u64(ENTRY_RECORD, 1)
    alloc = q.read_u64(STATIC_DATA, 4)
    cap = q.read_u64(CAP_TABLE, 10)
    return {"entry": entry, "alloc": alloc, "cap": cap,
            "clean": not any(alloc) and not any(cap)}


# ---------------------------------------------------------------------------
# Seam 2
# ---------------------------------------------------------------------------

def load_contract():
    with open("machine_contract.json") as f:
        return json.load(f)


def expected_descriptor(c):
    d = c["boot_descriptor_contract"]
    return [int(d["magic"], 16), d["version"] | (d["length"] << 32), d["flags_reserved"],
            int(d["ram_base"], 16), int(d["ram_size"], 16), int(d["uart_base"], 16),
            int(d["physics_base"], 16), d["physics_size"]]


def expected_cap_root(c):
    r = c["root_capability_spec"]
    digest = bytes.fromhex(r["atlas_provenance_digest"])
    words = [int.from_bytes(digest[i:i + 8], "little") for i in range(0, 32, 8)]
    return [r["slot"] | (r["generation"] << 32), int(r["principal_id"], 16),
            int(r["resource_type"], 16) | (int(r["allowed_ops"], 16) << 32),
            int(r["bound_base"], 16), int(r["bound_size"], 16),
            r["revocation_state"] | (r["attenuation_depth"] << 32)] + words


def check(results, gate, ok, detail):
    results[gate] = results.get(gate, True) and bool(ok)
    print(f"  [{'OK' if ok else 'FAIL'}] {gate}: {detail}")


def verify_seam2(base_dir="."):
    os.chdir(base_dir)
    shutil.rmtree(BUILD_DIR, ignore_errors=True)
    os.makedirs(BUILD_DIR)
    c = load_contract()
    results = {}
    print("=" * 70)
    print("SEAM 2: EXTERNAL EXECUTION SEAM (QEMU AARCH64 VIRT - PHYSICS_BOOT)")
    print("=" * 70)

    # Golden images: rebuild from source into build/seam2/golden and require
    # they are byte-identical to the canonical files under qualification.
    golden_dir = os.path.join(BUILD_DIR, "golden")
    _, g_phys = m2_build.build_physics(golden_dir, ".")
    _, g_atlas = m2_build.build_atlas(golden_dir, g_phys, ".")
    reproducible = (m2_build.sha256_file(g_phys) == m2_build.sha256_file("physics.bin") and
                    m2_build.sha256_file(g_atlas) == m2_build.sha256_file("atlas_m2.bin"))
    check(results, "PHYSICS_BOOT_QEMU_PASS", reproducible,
          "physics.bin and atlas_m2.bin rebuild byte-identically from source")

    # ---- Exec-1: golden boot ------------------------------------------------
    print("\n[Exec-1] Golden boot, machine state read over QMP")

    def inspect_golden(q):
        return {"desc": q.read_u64(BOOT_STATE, 8), "entry": q.read_u64(ENTRY_RECORD, 1)[0],
                "alloc": q.read_u64(STATIC_DATA, 4), "cap": q.read_u64(CAP_TABLE, 10),
                "bitmap": q.read_bytes(BITMAP, BITMAP_BYTES + 1),
                "trap_magic": q.read_u64(TRAP_FRAME + 0x138, 1)[0]}

    lines, st = run_qemu(os.path.join(BUILD_DIR, "run_golden"), "atlas_m2.bin", "physics.bin",
                         inspect_golden)
    for l in lines:
        print(f"    | {l}")
    golden = ["ATLAS: AWAKEN", "ATLAS: VERIFY", "ATLAS: HANDOFF", "PHYSICS: AWAKEN",
              "PHYSICS: INGRESS_VALID", "PHYSICS: ENTRY_EL1_VERIFIED", "PHYSICS: VBAR_INSTALLED",
              "PHYSICS: FRAME_AUTH_BOUND", "PHYSICS: CAP_ROOT_GENESIS", "PHYSICS: QUIESCENT_READY"]
    check(results, "PHYSICS_BOOT_QEMU_PASS", lines == golden,
          "telemetry is exactly the golden sequence, nothing else")
    check(results, "PHYSICS_BOOT_QEMU_PASS", st["trap_magic"] == 0, "no exception taken on the normal path")

    el_expect = int(c["architecture"]["expected_entry_el_value"], 16)
    check(results, "PHYSICS_ENTRY_EL_PASS", st["entry"] == el_expect,
          f"recorded CurrentEL {st['entry']:#x} == contract {el_expect:#x}")

    exp_desc = expected_descriptor(c)
    check(results, "PHYSICS_DESCRIPTOR_INGRESS_PASS", st["desc"] == exp_desc,
          "PHYSICS-owned descriptor copy == contract descriptor (all 8 words)")

    alloc_ok = st["alloc"] == [DRAM_END, TOTAL_FRAMES, 0, 0]
    check(results, "PHYSICS_BOOT_QEMU_PASS", alloc_ok,
          f"allocator header DRAM_END={st['alloc'][0]:#x} frames={st['alloc'][1]} "
          f"(expect {DRAM_END:#x}, {TOTAL_FRAMES})")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", alloc_ok and not any(st["bitmap"]),
          f"fresh bitmap: {BITMAP_BYTES} bytes zero inside PHYSICS_STATIC_DATA")

    exp_cap = expected_cap_root(c)
    check(results, "PHYSICS_CAP_ROOT_PASS", st["cap"] == exp_cap,
          "CAP_ROOT slot/generation/principal/resource_type/allowed_ops/bounds/"
          "revocation/attenuation/32-byte Atlas digest == contract")
    with open("atlas.sha256") as f:
        atlas_digest = f.read().split()[0]
    check(results, "PHYSICS_CAP_ROOT_PASS",
          atlas_digest == c["root_capability_spec"]["atlas_provenance_digest"],
          "contract provenance digest == atlas.sha256")
    cap_end = int(c["root_capability_spec"]["bound_base"], 16) + int(c["root_capability_spec"]["bound_size"], 16)
    check(results, "PHYSICS_CAP_ROOT_PASS",
          int(c["root_capability_spec"]["bound_base"], 16) == FREE_FRAME_BASE and cap_end == DRAM_END,
          "contract CAP_ROOT bounds == [FREE_FRAME_BASE, DRAM_END)")

    # ---- Exec-2: frame authority, reservation, capability probes -----------
    print("\n[Exec-2] In-kernel allocator exhaustion, reserved and capability probes")
    canary = os.path.join(BUILD_DIR, "canary.bin")
    # One canary byte after the bitmap (last byte of PHYSICS_STATIC_DATA) plus
    # the whole first free frame: bitmap writes must never reach them.
    canary_base = BITMAP + BITMAP_BYTES
    canary_len = FREE_FRAME_BASE + 0x1000 - canary_base
    with open(canary, "wb") as f:
        f.write(bytes([CANARY]) * canary_len)
    _, u_phys, u_atlas = physics_variant("unit", unit_snippet(), UNIT_RODATA)

    def inspect_unit(q):
        return {"rec": q.read_u64(TEST_RECORD, 7), "alloc": q.read_u64(STATIC_DATA, 4),
                "bitmap": q.read_bytes(BITMAP, BITMAP_BYTES),
                "canary": q.read_bytes(canary_base, canary_len)}

    lines, st = run_qemu(os.path.join(BUILD_DIR, "run_unit"), u_atlas, u_phys, inspect_unit,
                         extra_loaders=[(canary, canary_base)])
    for l in lines:
        print(f"    | {l}")
    count, last, after, order_ok, res_bits, cap_bits, magic = st["rec"]
    ran = magic == UNIT_MAGIC and "PHYSICS: UNIT_TESTS_DONE" in lines
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", ran, "unit image completed and wrote its record")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", count == TOTAL_FRAMES,
          f"{count} frames allocated before exhaustion (expect {TOTAL_FRAMES})")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", order_ok == 1,
          "every allocation == previous + 4 KiB from FREE_FRAME_BASE (deterministic, no duplicates)")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", last == DRAM_END - 0x1000 and last < DRAM_END,
          f"last frame {last:#x} < DRAM_END")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", after == 0, "next allocation returns FRAME_EXHAUSTED (0)")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", st["alloc"][2] == TOTAL_FRAMES,
          "allocator cursor == total_frames")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", all(b == 0xFF for b in st["bitmap"]),
          f"all {BITMAP_BYTES} bitmap bytes set, exactly one bit per granted frame")
    check(results, "PHYSICS_FRAME_BOUNDS_PASS", all(b == CANARY for b in st["canary"]),
          f"canary {canary_base:#x}..{FREE_FRAME_BASE + 0x1000:#x} untouched (bitmap stays in STATIC_DATA)")

    exp_res = sum(e << i for i, (_, e) in enumerate(RESERVED_PROBES))
    check(results, "PHYSICS_RESERVED_FRAME_REFUSAL_PASS", ran and res_bits == exp_res,
          f"{len(RESERVED_PROBES)} is_frame_reserved probes (bits {res_bits:#x}, expect {exp_res:#x})")
    exp_cap_bits = sum(e << i for i, (_, _, e) in enumerate(CAP_PROBES))
    check(results, "PHYSICS_CAP_ROOT_PASS", ran and cap_bits == exp_cap_bits,
          f"{len(CAP_PROBES)} validate_capability probes: CAP_ROOT grants only inside "
          f"[FREE_FRAME_BASE, DRAM_END) (bits {cap_bits:#x}, expect {exp_cap_bits:#x})")

    # ---- Exec-3: exception state capture ------------------------------------
    for kind in ("brk", "align"):
        print(f"\n[Exec-3/{kind}] Sentinel GPRs, then {'BRK #0x4d32' if kind == 'brk' else 'unaligned load (SCTLR_EL1.A=1)'}")
        t_elf, t_phys, t_atlas = physics_variant(f"trap_{kind}", trap_snippet(kind))
        site = symbol(t_elf, "seam2_fault_site")

        def inspect_trap(q):
            return q.read_u64(TRAP_FRAME, 40)

        lines, fr = run_qemu(os.path.join(BUILD_DIR, f"run_trap_{kind}"), t_atlas, t_phys, inspect_trap)
        for l in lines:
            print(f"    | {l}")
        gprs, sp = fr[0:31], fr[31]
        slot, el, esr, elr, spsr, far, far_valid, magic = fr[32:40]
        if kind == "brk":
            exp = {"esr": 0xF2004D32, "far": 0, "far_valid": 0}
        else:
            # EC 0x25 (Data Abort, same EL), IL=1, ISV=0, DFSC 0x21 alignment
            exp = {"far": ALIGN_BASE + 1, "far_valid": 1}
        gate = "PHYSICS_EXCEPTION_STATE_CAPTURE_PASS"
        check(results, gate, magic == TRAP_MAGIC and "PHYSICS: TRAP_FRAME_CAPTURED" in lines,
              "trap frame complete (TRAPFRM1 magic written last)")
        want = [sentinel(n) for n in range(31)]
        if kind == "align":
            want[28] = ALIGN_BASE
        bad = [n for n in range(31) if gprs[n] != want[n]]
        check(results, gate, not bad, f"x0..x30 sentinels preserved in frame (mismatch: {bad})")
        check(results, gate, sp == BOOT_STATE, f"interrupted SP {sp:#x} == PHYSICS stack top")
        check(results, gate, slot == 4, f"vector slot {slot} == 4 (current EL, SPx, synchronous)")
        check(results, gate, el == el_expect, f"CURRENT_EL {el:#x}")
        check(results, gate, elr == site, f"ELR {elr:#x} == fault site {site:#x}")
        check(results, gate, spsr == 0x3C5, f"SPSR {spsr:#x} == 0x3c5 (EL1h, DAIF masked)")
        if kind == "brk":
            check(results, gate, esr == exp["esr"], f"ESR {esr:#x} == {exp['esr']:#x} (BRK, imm 0x4d32)")
        else:
            ec, dfsc = esr >> 26, esr & 0x3F
            check(results, gate, ec == 0x25 and dfsc == 0x21 and not (esr >> 10) & 1,
                  f"ESR {esr:#x}: EC 0x25 data abort, DFSC 0x21 alignment, FnV 0")
        check(results, gate, far_valid == exp["far_valid"] and far == exp["far"],
              f"FAR {far:#x} FAR_VALID {far_valid} (expect {exp['far']:#x}, {exp['far_valid']})")

    # ---- Exec-4: hostile ingress matrix -------------------------------------
    print("\n[Exec-4] Hostile ingress matrix (memory-checked: no authority granted)")
    passed = 0
    total = len(HOSTILE_CASES) + 1
    desc_ok = True
    for tag, desc, defsyms, panic, is_desc in HOSTILE_CASES:
        out_dir = os.path.join(BUILD_DIR, f"hostile_{tag}")
        _, h_atlas = m2_build.build_atlas(out_dir, "physics.bin", ".", defsyms=defsyms)
        lines, st = run_qemu(os.path.join(out_dir, "run"), h_atlas, "physics.bin", no_authority_granted)
        ok = (panic in lines and st["clean"] and
              not any(m in l for l in lines for m in ("FRAME_AUTH_BOUND", "CAP_ROOT_GENESIS", "QUIESCENT_READY")))
        passed += ok
        desc_ok &= ok or not is_desc
        print(f"  [{'REFUSED' if ok else 'FAIL'}] {desc:40s} -> {panic.split(': ')[1]}"
              + ("" if ok else f"  lines={lines} state={st}"))

    # Real wrong-EL entry: virtualization=on starts the firmware (Atlas) at EL2,
    # so PHYSICS is entered at EL2, which the contract does not admit.
    lines, st = run_qemu(os.path.join(BUILD_DIR, "run_el2"), "atlas_m2.bin", "physics.bin",
                         no_authority_granted, machine="virt,virtualization=on")
    el2_ok = PANIC_EL in lines and st["clean"] and st["entry"] == [0]
    passed += el2_ok
    print(f"  [{'REFUSED' if el2_ok else 'FAIL'}] {'entry at EL2 (virtualization=on)':40s} -> "
          f"PANIC_UNCONTRACTED_EL" + ("" if el2_ok else f"  lines={lines} state={st}"))
    check(results, "PHYSICS_ENTRY_EL_PASS", el2_ok, "entry at EL2 refused before any authority")
    check(results, "PHYSICS_DESCRIPTOR_INGRESS_PASS", desc_ok,
          "every descriptor header/profile violation refused before bitmap init")
    check(results, "PHYSICS_CORRUPTION_REFUSAL_PASS", passed == total,
          f"{passed}/{total} hostile cases refused with the expected panic and zero authority state")

    order = ["PHYSICS_ENTRY_EL_PASS", "PHYSICS_DESCRIPTOR_INGRESS_PASS", "PHYSICS_BOOT_QEMU_PASS",
             "PHYSICS_FRAME_BOUNDS_PASS", "PHYSICS_RESERVED_FRAME_REFUSAL_PASS",
             "PHYSICS_CAP_ROOT_PASS", "PHYSICS_EXCEPTION_STATE_CAPTURE_PASS",
             "PHYSICS_CORRUPTION_REFUSAL_PASS"]
    return {g: results.get(g, False) for g in order}


if __name__ == "__main__":
    import sys
    r = verify_seam2(".")
    for g, ok in r.items():
        print(f"  [{'PASS' if ok else 'FAIL'}] {g}")
    sys.exit(0 if all(r.values()) else 1)
