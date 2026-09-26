# Hardware Access Audit: DGX Spark Accelerator Link (Milestone 15)

```text
Document ID:     AUDIT-PHYSICS-M15-NATIVE-ACCESS
Target Machine:  NVIDIA DGX Spark (P4242 / Grace Neoverse V2 + Blackwell GB10)
Host OS:         Linux 7.0.0-1019-nvidia aarch64 (Ubuntu 24.04 LTS)
Kernel Lockdown: none [integrity] confidentiality (EFI Secure Boot Active)
Strict Devmem:   CONFIG_STRICT_DEVMEM=y, CONFIG_IO_STRICT_DEVMEM=y
Audit Date:      2026-09-26T05:05:00Z
Auditor:         AIEN Physics Substrate / Epistemic Verification
```

---

## 1. System Hardware & Topology Observation

| Subsystem | Empirical Hardware Observation | Source / Sysfs Path |
| :--- | :--- | :--- |
| **GPU Device** | NVIDIA GB10 (PCI ID `10de:2e12`, rev a1) | `/sys/bus/pci/devices/000f:01:00.0/` |
| **PCI Topology** | Domain `000f`, Bus `01`, Device `00`, Function `0` | `lspci -s 000f:01:00.0` |
| **Active Driver** | `nvidia` (NVIDIA UNIX Open Kernel Module 580.173.02) | `/sys/bus/pci/devices/000f:01:00.0/driver` |
| **IOMMU Group** | Group `20` (Sole device: `000f:01:00.0`) | `/sys/kernel/iommu_groups/20/devices/` |
| **SMMUv3 Instance** | `arm-smmu-v3.1.auto` @ `0x13000000` | `/sys/class/iommu/smmu3.0x0000000013000000` |
| **Stream ID** | `0x0100` (256 decimal, ACPI IORT Node 29) | ACPI IORT / Kernel Boot Log |
| **BAR 0 Aperture** | `0x24000000 - 0x27ffffff` (64 MiB prefetchable MMIO) | `/proc/iomem`, `/sys/bus/pci/devices/000f:01:00.0/resource` |
| **Unified DRAM** | `[0x80000000, 0x2080000000)` (128 GiB coherent LPDDR5x) | DMI / `/proc/iomem` ("System RAM") |

---

## 2. Evaluation of Native Hardware-Access Mechanisms

In accordance with Rule 3 of the AIEN Epistemic Verification Doctrine, every prospective hardware access mechanism was audited empirically on the live system before code construction.

### Mechanism Matrix

| Mechanism | Available? | Requires Root? | Requires Driver Unbind? | Requires Kernel Module? | Risks Active GPU? | Vendor Runtime Dep? | Suitable for Permanent Lineage? | Suitable Only as External Oracle? |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1. Direct BAR Resource mmap** (`resource0`) | **NO** | YES | **YES** | NO | **YES** (Severe) | NO | NO (Host OS dependent) | NO (Kernel blocks mmap) |
| **2. VFIO Framework** (`/dev/vfio/20`) | **NO** | YES | **YES** | NO (`vfio_pci` loaded) | **YES** (Breaks telemetry) | NO | NO (Virtualization shim) | NO (Blocked by driver unbind) |
| **3. UIO Framework** (`/dev/uio*`) | **NO** | YES | **YES** | YES (`uio_pci_generic`) | **YES** | NO | NO | NO |
| **4. Generic IOMMU / iommufd** (`/dev/iommu`) | **PARTIAL** | YES | **YES** | NO (`iommufd` loaded) | **YES** | NO | NO | NO (Device unbound required) |
| **5. DMA-BUF / dma_heap** (`/dev/dma_heap/system`) | **YES** | YES | NO | NO | NO | NO | NO | **YES** (Allocates real DMA frames) |
| **6. HMM / SVA Interfaces** (`IOMMU_SVA`) | **YES** | NO | NO | NO | NO | NO | NO | **YES** (ATS host page tables) |
| **7. Existing Kernel GPU Interface** (`/dev/nvidia*`) | **YES** | NO | NO | NO (In-tree module) | NO | YES | NO | **YES** (Mediated kernel access) |
| **8. Vendor-Driver Oracle** (`libcuda.so.1`) | **YES** | NO | NO | NO | NO | YES | NO | **YES (NON-LINEAGE QUALIFICATION ORACLE)** |
| **9. Custom Kernel Mediator** (Out-of-tree `.ko`) | **NO** | YES | NO | YES | **YES** (Lockdown violation) | NO | NO | NO (Rejected: `-EKEYREJECTED`) |

---

## 3. Detailed Mechanism Findings

### 1. Direct BAR Resource mmap (`/sys/bus/pci/devices/000f:01:00.0/resource0`)
- **Finding**: Calling `mmap()` on `resource0` fails with `[Errno 1] Operation not permitted` (`EPERM`), even when invoked with `root` privileges.
- **Root Cause**: Linux kernel is compiled with `CONFIG_IO_STRICT_DEVMEM=y`. The in-tree driver `nvidia.ko` has claimed `24000000-27ffffff` via `pci_request_regions()`. The kernel function `iomem_is_exclusive()` returns `true`, triggering unconditional mmap refusal.
- **Safety Verdict**: Bypassing this requires unbinding `000f:01:00.0` from `nvidia.ko`. Doing so crashes the active `nvidia-smi` telemetry daemon and risks leaving the hardware in an unrecoverable state without rebooting. Strictly prohibited under Rule 3.

### 2. VFIO Framework (`/dev/vfio/vfio`)
- **Finding**: `/dev/vfio/vfio` is available and responds to `VFIO_GET_API_VERSION`. However, `/dev/vfio/20` does not exist because IOMMU group 20 is bound to `nvidia`, not `vfio-pci`.
- **Safety Verdict**: Binding `000f:01:00.0` to `vfio-pci` requires unbinding the active GPU. Unacceptable on this shared host.

### 3. Custom Kernel Module (`.ko`)
- **Finding**: `/sys/kernel/security/lockdown` reports `none [integrity] confidentiality`.
- **Root Cause**: Kernel module signature verification (`CONFIG_MODULE_SIG=y`) is enforced by EFI Secure Boot. Unsigned modules fail insertion with `init_module: Key was rejected by service` (`-EKEYREJECTED`).

### 4. DMA-BUF & DRM PRIME (`/dev/dma_heap/system`, `/dev/dri/renderD128`)
- **Finding**: Real physical coherent DMA buffer allocation was verified via `/dev/dma_heap/system` (`DMA_HEAP_IOC_ALLOC`), producing real DMA-BUF fds. DRM PRIME import (`DRM_IOCTL_PRIME_FD_TO_HANDLE`) successfully imports the DMA-BUF into `nvidia-drm` as GEM handle `1`.
- **Limitation**: `nvidia-drm` is a Kernel Mode Setting (KMS) driver; it does not implement raw compute command submission ioctls.

### 5. Vendor-Driver Qualification Oracle (`libcuda.so.1`)
- **Finding**: The system possesses `libcuda.so.1` (driver version 580.173.02), communicating with the in-tree kernel module `nvidia.ko` and `/dev/nvidia-uvm`.
- **Capabilities Verified**:
  1. `cuInit(0)` and `cuCtxCreate()` initialize device `000f:01:00.0` (`NVIDIA GB10`).
  2. `cuMemAlloc()` allocates genuine device-visible address / IOVA windows in the GPU translation domain.
  3. `cuMemcpyHtoD()` / `cuMemcpyDtoD()` submit legitimate copy and write work descriptors to the Blackwell hardware command queues.
  4. `cuCtxSynchronize()` / `cuStreamSynchronize()` perform independent hardware execution completion tracking.
  5. `cuMemFree()` revokes the device mapping.
  6. Subsequent device operations targeting the revoked IOVA return error `700` (`CUDA_ERROR_ILLEGAL_ADDRESS`), proving hardware MMU / SMMU access fault confinement.

---

## 4. Architectural Decision & Oracle Classification

In strict obedience to **Rule 4 (SEPARATE PERMANENT LINEAGE FROM QUALIFICATION ORACLES)**:
1. **The Sovereign Substrate (`physics_accel.c`, `physics_accel.h`)**:
   - Implements the sovereign authority model: capability attenuation, 192-byte `EffectReceipt` ledger, unkeyed rolling SHA-256 digest chain, IOVA bounds verification, and fail-closed state machines.
   - Preserves **Zero Foreign Toolchain Invariant**: 0 inline assembly (`__asm__`), 0 Python, 0 LLVM, 0 system commands.
2. **The External Qualification Observer (`native/`, `tools/m15tool.c`)**:
   - Designated as a **`NON-LINEAGE QUALIFICATION ORACLE`**.
   - Dynamically interfaces with the hardware via the existing kernel driver to provide independent, empirical proof of device execution, hardware-caused state changes, and post-revocation faulting.
   - It is an observer to prove the physical reality of the machine; it is NOT part of Omega's permanent realization path.
