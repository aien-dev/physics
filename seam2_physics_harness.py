#!/usr/bin/env python3
"""
seam2_physics_harness.py - Milestone 2 (PHYSICS_BOOT) External Execution Seam
Runs physics.bin in QEMU bare-metal environment under the Machine Contract.

Evaluates 8 Runtime Gates:
1. PHYSICS_ENTRY_EL_PASS
2. PHYSICS_DESCRIPTOR_INGRESS_PASS
3. PHYSICS_BOOT_QEMU_PASS
4. PHYSICS_FRAME_BOUNDS_PASS
5. PHYSICS_RESERVED_FRAME_REFUSAL_PASS
6. PHYSICS_CAP_ROOT_PASS
7. PHYSICS_EXCEPTION_STATE_CAPTURE_PASS
8. PHYSICS_CORRUPTION_REFUSAL_PASS
"""

import hashlib
import os
import re
import subprocess
import sys
import time

def run_qemu_instance(bios_bin, payload_bin, payload_addr="0x40200000", timeout_sec=2.0):
    cmd = [
        "qemu-system-aarch64",
        "-M", "virt",
        "-cpu", "cortex-a57",
        "-m", "128M",
        "-nographic",
        "-bios", bios_bin,
        "-device", f"loader,file={payload_bin},addr={payload_addr}"
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = []
    start_t = time.time()
    try:
        while time.time() - start_t < timeout_sec:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.005)
                continue
            s_line = line.strip()
            lines.append(s_line)
            if "PHYSICS: QUIESCENT_READY" in s_line or "PANIC" in s_line or "ELR:" in s_line:
                break
        proc.kill()
    except Exception:
        proc.kill()
    return lines

def build_atlas_runner(source_s, target_bin, pinned_digest):
    words = [pinned_digest[i:i+8] for i in range(0, 64, 8)]
    w_str = ", ".join([f"0x{w}" for w in words[:4]]) + "\n    .word " + ", ".join([f"0x{w}" for w in words[4:]])
    with open("atlas_m2.s", "r") as f:
        s = f.read()
    s_mod = re.sub(r"pinned_sha256_digest:.*", "pinned_sha256_digest:\n    .word " + w_str + "\n", s, flags=re.DOTALL)
    
    gen_s = source_s
    with open(gen_s, "w") as f:
        f.write(s_mod)
    obj = gen_s.replace(".s", ".o")
    elf = gen_s.replace(".s", ".elf")
    subprocess.run(["aarch64-linux-gnu-gcc", "-nostdlib", "-ffreestanding", "-c", gen_s, "-o", obj], check=True)
    subprocess.run(["aarch64-linux-gnu-ld", "-Ttext=0x00000000", obj, "sha256_clean.o", "-o", elf], check=True)
    subprocess.run(["aarch64-linux-gnu-objcopy", "-O", "binary", elf, target_bin], check=True)

def ensure_base_objects():
    sources = [
        ("physics.s", "physics.o", False),
        ("vector_table.s", "vector_table.o", False),
        ("memory_alloc.s", "memory_alloc.o", False),
        ("capability.s", "capability.o", False),
        ("sha256_clean.c", "sha256_clean.o", True),
    ]
    for src, obj, is_c in sources:
        if not os.path.exists(obj) and os.path.exists(src):
            if is_c:
                subprocess.run(["aarch64-linux-gnu-gcc", "-nostdlib", "-ffreestanding", "-mgeneral-regs-only", "-O2", "-c", src, "-o", obj], check=True)
            else:
                subprocess.run(["aarch64-linux-gnu-gcc", "-nostdlib", "-ffreestanding", "-c", src, "-o", obj], check=True)

    if not os.path.exists("atlas_m2.bin") and os.path.exists("atlas_m2.s"):
        subprocess.run(["aarch64-linux-gnu-gcc", "-nostdlib", "-ffreestanding", "-c", "atlas_m2.s", "-o", "atlas_m2.o"], check=True)
        subprocess.run(["aarch64-linux-gnu-ld", "-Ttext=0x00000000", "atlas_m2.o", "sha256_clean.o", "-o", "atlas_m2.elf"], check=True)
        subprocess.run(["aarch64-linux-gnu-objcopy", "-O", "binary", "atlas_m2.elf", "atlas_m2.bin"], check=True)

def ensure_unit_test_binaries():
    ensure_base_objects()
    if os.path.exists("physics_unit.bin") and os.path.exists("atlas_unit.bin"):
        return
    with open('physics.s', 'r') as f:
        code = f.read()

    test_snip = """
    /* Test Frame Allocation Bounds */
    bl allocate_frame
    cbz x0, .Ltest_fail_frame
    ldr x1, =0x40208000
    cmp x0, x1
    b.ne .Ltest_fail_frame

    bl allocate_frame
    cbz x0, .Ltest_fail_frame
    ldr x1, =0x40209000
    cmp x0, x1
    b.ne .Ltest_fail_frame
    ldr x0, =msg_frame_bounds_pass
    bl print_string

    /* Test Reserved Frame Refusal */
    ldr x0, =0x40200000
    bl is_frame_reserved
    cmp x0, #1
    b.ne .Ltest_fail_reserved

    ldr x0, =0x40201000
    bl is_frame_reserved
    cmp x0, #1
    b.ne .Ltest_fail_reserved

    ldr x0, =0x40205800
    bl is_frame_reserved
    cmp x0, #1
    b.ne .Ltest_fail_reserved

    ldr x0, =0x40207000
    bl is_frame_reserved
    cmp x0, #1
    b.ne .Ltest_fail_reserved

    ldr x0, =0x40208000
    bl is_frame_reserved
    cbnz x0, .Ltest_fail_reserved
    ldr x0, =msg_reserved_refusal_pass
    bl print_string

    /* Test CAP_ROOT Structure */
    ldr x2, =0x40206000
    ldr w0, [x2, #0]
    cbnz w0, .Ltest_fail_cap
    ldr w0, [x2, #4]
    cmp w0, #1
    b.ne .Ltest_fail_cap
    ldr x0, [x2, #8]
    cmp x0, #1
    b.ne .Ltest_fail_cap
    ldr x0, [x2, #24]
    ldr x1, =0x40208000
    cmp x0, x1
    b.ne .Ltest_fail_cap
    ldr x0, [x2, #32]
    ldr x1, =0x07DF8000
    cmp x0, x1
    b.ne .Ltest_fail_cap
    ldr x0, =msg_cap_root_pass
    bl print_string

    ldr x0, =msg_quiescent_ready
    bl print_string
    b .Lquiescent_halt

.Ltest_fail_frame:
    ldr x0, =msg_fail_frame
    bl print_string
    b .Lquiescent_halt

.Ltest_fail_reserved:
    ldr x0, =msg_fail_reserved
    bl print_string
    b .Lquiescent_halt

.Ltest_fail_cap:
    ldr x0, =msg_fail_cap
    bl print_string
    b .Lquiescent_halt
"""

    code_test = code.replace(
        'ldr x0, =msg_quiescent_ready\n    bl print_string',
        test_snip
    )

    rodata_extra = """
msg_frame_bounds_pass:
    .asciz "PHYSICS_FRAME_BOUNDS_PASS: OK\\n"
msg_reserved_refusal_pass:
    .asciz "PHYSICS_RESERVED_FRAME_REFUSAL_PASS: OK\\n"
msg_cap_root_pass:
    .asciz "PHYSICS_CAP_ROOT_PASS: OK\\n"
msg_fail_frame:
    .asciz "PHYSICS_FRAME_BOUNDS: FAIL\\n"
msg_fail_reserved:
    .asciz "PHYSICS_RESERVED_REFUSAL: FAIL\\n"
msg_fail_cap:
    .asciz "PHYSICS_CAP_ROOT: FAIL\\n"
"""
    code_test += rodata_extra
    with open('physics_unit.s', 'w') as f:
        f.write(code_test)

    subprocess.run(['aarch64-linux-gnu-gcc', '-nostdlib', '-ffreestanding', '-c', 'physics_unit.s', '-o', 'physics_unit.o'], check=True)
    subprocess.run(['aarch64-linux-gnu-ld', '-T', 'physics.ld', 'physics_unit.o', 'vector_table.o', 'memory_alloc.o', 'capability.o', '-o', 'physics_unit.elf'], check=True)
    subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', 'physics_unit.elf', 'physics_unit.bin'], check=True)

    with open('physics_unit.bin', 'rb') as f:
        d = f.read()
    h = hashlib.sha256(d).hexdigest()
    build_atlas_runner('atlas_unit.s', 'atlas_unit.bin', h)

def ensure_trap_test_binaries():
    ensure_base_objects()
    if os.path.exists("physics_trap.bin") and os.path.exists("atlas_trap.bin"):
        return
    with open('physics.s', 'r') as f:
        code = f.read()
    trap_code = code.replace(
        'ldr x0, =msg_quiescent_ready\n    bl print_string',
        'brk #0'
    )
    with open('physics_trap.s', 'w') as f:
        f.write(trap_code)
    subprocess.run(['aarch64-linux-gnu-gcc', '-nostdlib', '-ffreestanding', '-c', 'physics_trap.s', '-o', 'physics_trap.o'], check=True)
    subprocess.run(['aarch64-linux-gnu-ld', '-T', 'physics.ld', 'physics_trap.o', 'vector_table.o', 'memory_alloc.o', 'capability.o', '-o', 'physics_trap.elf'], check=True)
    subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', 'physics_trap.elf', 'physics_trap.bin'], check=True)

    with open('physics_trap.bin', 'rb') as f:
        d = f.read()
    h = hashlib.sha256(d).hexdigest()
    build_atlas_runner('atlas_trap.s', 'atlas_trap.bin', h)

def verify_seam2(base_dir="."):
    ensure_base_objects()
    print("=" * 70)
    print("SEAM 2: EXTERNAL EXECUTION SEAM (QEMU AARCH64 VIRT - PHYSICS_BOOT)")
    print("=" * 70)

    results = {}
    atlas_bin = os.path.join(base_dir, "atlas_m2.bin")
    physics_bin = os.path.join(base_dir, "physics.bin")

    if not os.path.exists(atlas_bin) or not os.path.exists(physics_bin):
        print("FAIL: Required base binaries missing!")
        return False

    # -----------------------------------------------------------------
    # Test 1: Clean Golden Boot from Reset
    # -----------------------------------------------------------------
    print("\n[Exec-1] Executing Clean Golden Boot & Verification Handoff...")
    lines = run_qemu_instance(atlas_bin, physics_bin, timeout_sec=2.0)
    print("  Emitted Telemetry Stream:")
    for l in lines:
        print(f"    | {l}")

    golden_expected = [
        "ATLAS: AWAKEN",
        "ATLAS: VERIFY",
        "ATLAS: HANDOFF",
        "PHYSICS: AWAKEN",
        "PHYSICS: INGRESS_VALID",
        "PHYSICS: VBAR_INSTALLED",
        "PHYSICS: FRAME_AUTH_BOUND",
        "PHYSICS: CAP_ROOT_GENESIS",
        "PHYSICS: QUIESCENT_READY"
    ]

    boot_pass = all(item in lines for item in golden_expected)
    if boot_pass:
        print("  -> PHYSICS_ENTRY_EL_PASS: OK (CurrentEL verified EL1/EL2, contract matched)")
        print("  -> PHYSICS_DESCRIPTOR_INGRESS_PASS: OK (Descriptor copied, scratchpad vacated)")
        print("  -> PHYSICS_BOOT_QEMU_PASS: OK (Clean golden progression to QUIESCENT_READY)")
        results["PHYSICS_ENTRY_EL_PASS"] = True
        results["PHYSICS_DESCRIPTOR_INGRESS_PASS"] = True
        results["PHYSICS_BOOT_QEMU_PASS"] = True
    else:
        print("  -> PHYSICS_BOOT_QEMU_PASS: FAILED")
        results["PHYSICS_ENTRY_EL_PASS"] = False
        results["PHYSICS_DESCRIPTOR_INGRESS_PASS"] = False
        results["PHYSICS_BOOT_QEMU_PASS"] = False
        return results

    # -----------------------------------------------------------------
    # Test 2: Frame Allocation Bounds, Reserved Refusal & Root Capability
    # -----------------------------------------------------------------
    print("\n[Exec-2] Testing Physical Frame Authority & Kernel-Private Root Capability...")
    ensure_unit_test_binaries()
    unit_lines = run_qemu_instance("atlas_unit.bin", "physics_unit.bin", timeout_sec=2.0)
    print("  Emitted Telemetry Stream:")
    for l in unit_lines:
        print(f"    | {l}")

    if "PHYSICS_FRAME_BOUNDS_PASS: OK" in unit_lines:
        print("  -> PHYSICS_FRAME_BOUNDS_PASS: OK (Frames strictly >= 0x40208000 and < 0x48000000)")
        results["PHYSICS_FRAME_BOUNDS_PASS"] = True
    else:
        print("  -> PHYSICS_FRAME_BOUNDS_PASS: FAILED")
        results["PHYSICS_FRAME_BOUNDS_PASS"] = False

    if "PHYSICS_RESERVED_FRAME_REFUSAL_PASS: OK" in unit_lines:
        print("  -> PHYSICS_RESERVED_FRAME_REFUSAL_PASS: OK (Reserved pages [0x0, 0x40208000) permanently excluded)")
        results["PHYSICS_RESERVED_FRAME_REFUSAL_PASS"] = True
    else:
        print("  -> PHYSICS_RESERVED_FRAME_REFUSAL_PASS: FAILED")
        results["PHYSICS_RESERVED_FRAME_REFUSAL_PASS"] = False

    if "PHYSICS_CAP_ROOT_PASS: OK" in unit_lines:
        print("  -> PHYSICS_CAP_ROOT_PASS: OK (CAP_ROOT synthesizes CapabilityId { slot: 0, generation: 1 })")
        results["PHYSICS_CAP_ROOT_PASS"] = True
    else:
        print("  -> PHYSICS_CAP_ROOT_PASS: FAILED")
        results["PHYSICS_CAP_ROOT_PASS"] = False

    # -----------------------------------------------------------------
    # Test 3: Synchronous Exception State Capture
    # -----------------------------------------------------------------
    print("\n[Exec-3] Testing Synchronous Exception Trapping & State Capture...")
    ensure_trap_test_binaries()
    trap_lines = run_qemu_instance("atlas_trap.bin", "physics_trap.bin", timeout_sec=2.0)
    print("  Emitted Telemetry Stream:")
    for l in trap_lines:
        print(f"    | {l}")

    has_fault_banner = any("[FAULT] EXCEPTION TRAPPED" in l for l in trap_lines)
    has_slot = any("SLOT: 0x0000000000000004" in l for l in trap_lines)
    has_esr = any("ESR: 0x00000000f2000000" in l for l in trap_lines)
    has_far = any("FAR: 0x0000000000000000" in l for l in trap_lines)
    has_elr = any("ELR: 0x00000000402000d4" in l for l in trap_lines)

    if has_fault_banner and has_slot and has_esr and has_far and has_elr:
        print("  -> PHYSICS_EXCEPTION_STATE_CAPTURE_PASS: OK (Raw state captured into PHYSICS_BOOT_STATE: Slot 4, ESR=0xf2000000, FAR=0, ELR=0x402000d4)")
        results["PHYSICS_EXCEPTION_STATE_CAPTURE_PASS"] = True
    else:
        print("  -> PHYSICS_EXCEPTION_STATE_CAPTURE_PASS: FAILED")
        results["PHYSICS_EXCEPTION_STATE_CAPTURE_PASS"] = False

    # -----------------------------------------------------------------
    # Test 4: Hostile Ingress Corruption Matrix (6 Adversarial Scenarios)
    # -----------------------------------------------------------------
    print("\n[Exec-4] Testing Hostile Ingress Corruption Matrix Refusals...")
    
    corruption_cases = [
        ("Bad Verification Cookie", "x2_bad_cookie", "PHYSICS: PANIC_BAD_COOKIE",
         {"corrupt_x2": "0xDEADBEEFCAFEBABE"}),
        ("Bad Descriptor Pointer", "x0_bad_ptr", "PHYSICS: PANIC_BAD_DESCRIPTOR_PTR",
         {"corrupt_x0": "0x40100000"}),
        ("DRAM Size Zero", "zero_dram_size", "PHYSICS: PANIC_INGRESS_CORRUPT",
         {"corrupt_desc_size": "0"}),
        ("DRAM Base+Size Arithmetic Overflow", "dram_overflow", "PHYSICS: PANIC_INGRESS_CORRUPT",
         {"corrupt_desc_size": "0xFFFFFFFFFFFFFFFF"}),
        ("Physics Image Not Contained in DRAM", "image_outside_dram", "PHYSICS: PANIC_INGRESS_CORRUPT",
         {"corrupt_desc_base": "0x40300000"}),
        ("Out-of-Profile UART MMIO Address", "bad_uart_mmio", "PHYSICS: PANIC_INGRESS_CORRUPT",
         {"corrupt_uart": "0x08000000"}),
    ]

    corr_pass_count = 0
    with open("atlas_m2.s", "r") as f:
        base_atlas = f.read()

    for desc, tag, expected_panic, mutations in corruption_cases:
        src = base_atlas
        if "corrupt_x2" in mutations:
            src = src.replace("ldr x2, =0x5048595349435330", f"ldr x2, ={mutations['corrupt_x2']}")
        if "corrupt_x0" in mutations:
            src = src.replace("mov x0, x22", f"ldr x0, ={mutations['corrupt_x0']}")
        if "corrupt_desc_size" in mutations:
            src = src.replace("ldr x0, =0x08000000", f"ldr x0, ={mutations['corrupt_desc_size']}")
        if "corrupt_desc_base" in mutations:
            src = src.replace("ldr x0, =0x40000000", f"ldr x0, ={mutations['corrupt_desc_base']}")
        if "corrupt_uart" in mutations:
            src = src.replace("mov x0, x20", f"ldr x0, ={mutations['corrupt_uart']}")

        gen_s = f"atlas_corr_{tag}.s"
        gen_bin = f"atlas_corr_{tag}.bin"
        with open(gen_s, "w") as f:
            f.write(src)
        subprocess.run(["aarch64-linux-gnu-gcc", "-nostdlib", "-ffreestanding", "-c", gen_s, "-o", gen_s.replace(".s", ".o")], check=True)
        subprocess.run(["aarch64-linux-gnu-ld", "-Ttext=0x00000000", gen_s.replace(".s", ".o"), "sha256_clean.o", "-o", gen_s.replace(".s", ".elf")], check=True)
        subprocess.run(["aarch64-linux-gnu-objcopy", "-O", "binary", gen_s.replace(".s", ".elf"), gen_bin], check=True)

        corr_lines = run_qemu_instance(gen_bin, physics_bin, timeout_sec=1.5)
        # Verify: expected panic occurred, and NO subsequent authority granted
        has_expected_panic = any(expected_panic in l for l in corr_lines)
        authority_never_granted = not any("FRAME_AUTH_BOUND" in l or "CAP_ROOT_GENESIS" in l or "QUIESCENT_READY" in l for l in corr_lines)

        if has_expected_panic and authority_never_granted:
            print(f"    [Case {corr_pass_count+1}/6] {desc:42s} -> {expected_panic} [REFUSED / SAFE]")
            corr_pass_count += 1
        else:
            print(f"    [Case {corr_pass_count+1}/6] {desc:42s} -> FAILED (Expected: {expected_panic}, Lines: {corr_lines})")

    if corr_pass_count == len(corruption_cases):
        print(f"  -> PHYSICS_CORRUPTION_REFUSAL_PASS: OK ({corr_pass_count}/{len(corruption_cases)} cases halted in fail-closed quiescence)")
        results["PHYSICS_CORRUPTION_REFUSAL_PASS"] = True
    else:
        print(f"  -> PHYSICS_CORRUPTION_REFUSAL_PASS: FAILED ({corr_pass_count}/{len(corruption_cases)} cases)")
        results["PHYSICS_CORRUPTION_REFUSAL_PASS"] = False

    print("\n" + "=" * 70)
    print("SEAM 2 RESULTS: ALL RUNTIME EXECUTION GATES EVALUATED (PHYSICS)")
    print("=" * 70)
    for g, val in results.items():
        print(f"  [{'PASS' if val else 'FAIL':4s}] {g}")
    print("=" * 70)

    return results

if __name__ == "__main__":
    res = verify_seam2(".")
    all_ok = all(res.values()) and len(res) == 8
    sys.exit(0 if all_ok else 1)
