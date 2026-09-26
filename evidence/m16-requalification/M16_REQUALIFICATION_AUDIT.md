# M16 Requalification Audit

## Original Qualification
- Commit: `physics@f72e2974793a288c5dd910180f4685deffa31bb7`
- Receipt: `evidence/m16-blackwell-native-path-receipt.json`
- Result on Independent Runtime Audit: **FAIL / SUPERSEDED**

## Defects Discovered

1. `sovereign_submit` linked and loaded `libcuda.so.1`.
2. CUDA APIs performed context/stream/memory initialization (`cuInit`, `cuDevicePrimaryCtxRetain`, `cuStreamCreate`, `cuMemHostAlloc`).
3. Old session-specific queue, USERD, and work-submit token state was reused (`ring = 0x200202000`, `userd_gpput = 0x20080108c`, `token = 0x40000003`).
4. Concurrent invocation failed due to colliding static virtual memory allocations.
5. Referenced raw evidence was not committed to Git (resided only in local filesystem run folders).
6. Original flush interpretation contained an unsupported/incorrect claim regarding method execution semantics.
7. GPGet interpretation was insufficiently established on physical Blackwell USERD.
8. Copy/compute object classes were mislabeled as GPFIFO channel classes where applicable.
9. Some physical-address claims lacked independent qualification (userspace virtual mappings were treated as verified physical addresses).

## Corrective Resolution
The corrective implementation (`nvrm/`, `m16/m16_native.c`, `m16/m16_native.h`, `m16/m16_requalify.c`, `m16/m16_concurrent.c`) completely replaces the invalid execution proof. All hardware resources (RM client, device, VA space, memory mappings, GPFIFO channel, work-submit token, doorbell MMIO) are created dynamically at runtime via direct ioctls on `/dev/nvidiactl`, `/dev/nvidia0`, and `/dev/nvidia-uvm` with zero userspace NVIDIA libraries.
