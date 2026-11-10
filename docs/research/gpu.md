# Modern GPUs for Windows 98: research notes

Status: research, November 2026. Nothing here is implemented. This document
answers two questions:

1. Can Windows 98 SE / ME use a modern GPU on a modern PC (for example a
   Ryzen system) **without a hypervisor**, possibly by using Linux drivers?
2. If a hypervisor is used instead, what exactly would that project need?
   (Part 2 is written so it can be handed to a separate project as is.)

## Summary

* No vendor will ever ship a Windows 9x driver for current GPUs. The last
  cards with 9x drivers are the GeForce 7 series (modified 82.69 driver) and
  the Radeon X850 (Catalyst 6.2). Anything newer gets at best a generic
  framebuffer.
* Porting Linux GPU drivers *into* the Windows 9x kernel is technically
  possible but is the wrong trade: FreeBSD needs a permanent team to keep
  its Linux-compatibility layer (LinuxKPI) and the DRM drivers up to date,
  on a far more capable kernel than Windows 9x.
* Emulating period graphics cards on bare metal, by trapping their I/O the
  way SBEMU does for Sound Blaster, works for slow devices (VGA, Sound
  Blaster) but not for a 3D card: one page fault per register write is far
  too slow.
* **The approach worth building is a split machine without a hypervisor:**
  Windows 98 runs natively on one CPU core (it can only use one anyway), and
  a Linux kernel runs at the same time on the remaining cores and owns the
  GPU and other modern devices. Windows 98 gets thin drivers that pass
  display, 3D, sound and input work to Linux through shared memory. This is
  the "multikernel" model; a Linux implementation that boots kernels on
  dedicated cores without a hypervisor was published in 2026 (mklinux), and
  older research systems (Popcorn Linux) showed the same on x86. The 3D path
  can reuse existing pieces: Mesa's virgl driver on the Windows side (Mesa
  already runs on Windows 9x in SoftGPU) and virglrenderer plus the normal
  AMD/Intel Mesa drivers on the Linux side.
* The same split gives Windows 98 modern USB, HD Audio, NVMe and network
  through Linux drivers, which also serves the gamepad and USB audio goals.
* With a hypervisor, the practical path already exists (QEMU/KVM with
  qemu-3dfx or SoftGPU). The remaining "accurate period cards with real
  firmware, rendered on the host GPU" project is well defined; see part 2.

## 1. Constraints of Windows 9x on modern hardware

| Constraint | Effect | Known workaround |
|---|---|---|
| No GPU drivers after GeForce 7 / Radeon X850 | No 2D or 3D acceleration on current cards | Generic VESA framebuffer drivers (VBEMP 9x, VMDisp9x) |
| UEFI-only firmware (no CSM) on recent boards | Windows 98 cannot boot at all; no VESA BIOS | CSMWrap: SeaBIOS CSM loaded as an EFI application, with a VESA BIOS built on the GOP framebuffer |
| TLB invalidation bug | Windows 98 SE / ME crash on Zen 2+ and Intel 11th gen+ | patcher9x |
| Single processor only | All other cores idle | (the split design below uses them) |
| 32-bit, no PAE | Practical RAM limit around 512 MB to 1 GB; device memory (BARs) must sit below 4 GB | PATCHMEM; "Above 4G decoding" and Resizable BAR off |
| Vintage ACPI / power management | Unstable on modern boards | Install without ACPI |
| Software 3D (Mesa llvmpipe) is single-core under 9x | A modern core is fast, but only one is usable | Split design: render on Linux cores or the GPU |

## 2. What already exists

| Project | What it does | Relevance |
|---|---|---|
| **CSMWrap** (FlyGoat) | Legacy BIOS on UEFI-only PCs: SeaBIOS CSM + SeaVGABIOS (VESA from GOP) as an EFI app | Makes bare-metal Windows 98 bootable on current boards at all |
| **VBEMP 9x**, **VMDisp9x** / boxv9x (Michal Necasek, JHRobotics) | Generic linear-framebuffer display drivers for 9x | Template for our display driver |
| **SoftGPU** (JHRobotics): Mesa9x, Wine9x, OpenGlide9x, VMHAL9x | OpenGL, DirectX and Glide for 9x on Mesa (llvmpipe, VMware SVGA3D, native DDI) | Proves Mesa runs on 9x; its stack is the Windows-side half of the split design |
| **qemu-3dfx** (KJ Liew) | Glide and OpenGL pass-through from a Win9x guest to the host GPU under QEMU/KVM | Proven API forwarding protocol and guest wrappers |
| **86Box / PCem** | Register-accurate emulation of period cards (Voodoo 1-3, S3, ATI, Matrox) with real ROMs; Voodoo is rendered in software with a JIT | Device models for the hypervisor project; no host-GPU rendering today |
| **patcher9x** | Fixes Windows 9x CPU bugs on new CPUs | Required in every scenario |
| **SBEMU / VSBHDA** | Sound Blaster emulation in DOS on HD Audio by trapping I/O | Shows trap-and-emulate works for slow devices; a VxD version is the natural 9x form |
| **FreeBSD drm-kmod** (LinuxKPI) | Linux amdgpu/i915/radeon drivers running in another kernel through a compatibility layer | Shows the cost of porting Linux DRM: continuous work by a dedicated team |
| **coLinux** | Linux kernel running cooperatively inside Windows 2000/XP as a driver | Shows Linux can share a machine with Windows without a hypervisor (time-sliced, one CPU) |
| **Popcorn Linux**, **mklinux** (Linux 7.0-mk2, 2026) | Several kernels on one x86 machine, each on its own cores, memory and devices, no hypervisor | The enabling technique for the split design |

## 3. Options without a hypervisor

### A. Generic framebuffer and software 3D (available now)

CSMWrap + patcher9x + a VESA/linear-framebuffer driver + SoftGPU's Mesa
llvmpipe. Gives modern resolutions and OpenGL/DirectX/Glide in software on
one core.

* Effort: integration and testing only.
* Limits: one core for all rendering; no hardware 3D; VESA mode list only.
* Recommendation: document it as the baseline ("Windows 98 on a Ryzen, today")
  and use it to test everything else.

### B. Port Linux GPU drivers into the Windows 9x kernel

A "LinuxKPI for VxD/WDM" layer in KernelExEx, then amdgpu or i915 on top.

* Needs from the host kernel: threads, workqueues, sleeping locks, DMA
  mapping, DMA fences, memory managers (TTM/GEM), firmware loading, MSI,
  timers, a large non-paged memory pool. Windows 9x has primitive or no
  equivalents for most of these.
* amdgpu on 32-bit x86 is not validated by AMD any more ("it should work in
  theory"). Its memory needs fight with Windows 98's 1 GB practical limit.
* User space would still need Mesa (radeonsi) ported to talk to it.
* FreeBSD, a far more capable kernel, keeps this working only with ongoing
  dedicated effort.
* Verdict: **not recommended.** Years of work, fragile, and always behind
  Linux.

### C. Emulate period cards on bare metal (trap-and-emulate)

The Windows 9x VMM already traps I/O ports for DOS boxes, and a VxD can trap
memory pages through page faults. A VxD could present a "Voodoo" or "VGA"
and draw with whatever real renderer exists.

* Works for low-rate devices: VGA/VESA for DOS programs, Sound Blaster
  (the VxD form of SBEMU), joysticks.
* Fails for 3D cards: a Voodoo receives millions of register writes per
  second; a page fault per write costs microseconds.
* For Windows games, replacing the API DLL (Glide, OpenGL) is faster and
  simpler than emulating the card.
* Verdict: **do it for Sound Blaster and VGA in DOS boxes; not for 3D.**

### D. Split machine: Windows 98 on one core, Linux on the rest (recommended)

```
 core 0                         cores 1..N
+--------------------------+   +------------------------------------------+
| Windows 98 SE (native)   |   | Linux (mklinux-style spawn kernel)       |
|  KernelExEx              |   |  amdgpu / i915 / nouveau + Mesa          |
|  kexdisp.drv  (2D)  -----+---+-> display server: scan-out of the 98     |
|  Mesa9x + virgl (3D) ----+---+-> virglrenderer (GL/Vulkan on the GPU)   |
|  Glide wrapper ----------+---+-> OpenGlide / host Glide                 |
|  kexaudio (mixer) -------+---+-> PipeWire/ALSA: HDA, USB audio, headsets|
|  kexinput (pads, HID) <--+---+-- evdev: xHCI USB, Bluetooth, gamepads   |
|  kexnet (NDIS) <---------+---+-> Linux network stack                    |
|  kexdisk (IOS port) <----+---+-> NVMe/SATA                              |
+--------------------------+   +------------------------------------------+
        shared memory rings + inter-processor interrupts (no hypervisor)
```

**How it works.** Both kernels run directly on the hardware. Neither traps the
other. They divide the machine: Windows 98 gets core 0, the low memory it can
use, and the legacy devices (PIC, PIT, RTC, keyboard controller). Linux gets
the other cores, the memory above, and the modern PCI devices. They talk
through shared-memory rings and inter-processor interrupts, which is how
mklinux and Popcorn connect their kernels.

**Boot order.** Two variants:

1. *Linux first (preferred).* UEFI boots Linux, which takes ACPI, the IOMMU,
   interrupt routing and power management, reserves a CPU and memory, and
   starts Windows 98 on the reserved core with a CSM environment (as CSMWrap
   builds it) describing only Windows' share of the machine. Linux owns the
   parts of the platform Windows 98 handles worst.
2. *Windows first.* CSMWrap boots Windows 98; a KernelExEx VxD starts Linux
   on the idle cores (INIT-SIPI, then a 64-bit trampoline into the Linux
   entry point with a memory map of the reserved region). Simpler to try, but
   Linux then works on a platform whose ACPI and interrupt state Windows 98
   has already touched.

**Graphics.**

* 2D: a Windows 9x display driver (based on VMDisp9x/boxv9x) draws into a
  framebuffer in shared memory; the Linux side shows it through KMS, scaled
  and at the monitor's native mode (page flip or GPU blit each frame). Any
  resolution, including widescreen, with no VESA BIOS involved.
* 3D: SoftGPU's Mesa9x built with Mesa's **virgl** Gallium driver. virgl
  produces a command stream that **virglrenderer** on Linux replays on the
  real GPU through its normal Mesa driver. This is the protocol used for
  virtual machines today; here the transport is a shared-memory ring instead
  of virtio. DirectX goes through Wine9x or SoftGPU's DDI on top of that
  Mesa; Glide through OpenGlide (either side). Fullscreen DOS VGA modes are
  emulated by the display driver or a VxD.
* Every GPU Linux supports works, from integrated Radeon/Intel graphics to
  current discrete cards, and new GPUs need no Windows-side work.

**What else Windows 98 gains.** The same mechanism carries USB 3 (xHCI),
modern HD Audio and USB headsets with a per-program mixer, Bluetooth and
current gamepads (with the Linux drivers named in the README decisions),
NVMe, and current network cards. The Discord presence companion could run on
the Linux side of the same PC.

**Hard parts and open questions.**

| Problem | Why it is hard | Direction |
|---|---|---|
| Hiding Linux-owned PCI devices from Windows 98 | Windows enumerates all devices through config space and may move or disable them | KernelExEx VxD filter on PCI enumeration; or Linux-first boot presents a filtered config space through the CSM/ACPI it builds |
| Interrupt routing | Windows 98 uses the legacy PIC on its core; Linux needs MSI on its cores | Modern devices use MSI aimed at Linux cores; the legacy PIC/IOAPIC pins stay with Windows |
| Timers and the TSC | Both kernels program time sources | Windows keeps the PIT/RTC; Linux uses the LAPIC timer and TSC only |
| Shared global state: SMM, microcode, power states, NMI | Hidden side effects between kernels | Disable C-states/P-state changes on Windows' core; Linux owns power |
| Memory split | Windows 98 must not see Linux memory | E820 map built for Windows; above-4 GB memory is invisible to it anyway |
| Keyboard and mouse | On modern PCs they are USB, which Linux owns | kexinput delivers them to Windows as PS/2-compatible devices |
| Crash isolation | No hypervisor, so a Windows 98 crash can corrupt Linux memory | Accept for a hobby system; IOMMU protects device DMA; optional watchdog reboot |
| mklinux maturity | Out-of-tree, new | Track upstream; the parts needed (spawn on a CPU set, IPI messaging) are small |

**Phased plan.**

1. *Baseline:* Windows 98 on a current AMD board with CSMWrap, patcher9x, a
   framebuffer driver and SoftGPU llvmpipe. Write it up, test it with
   KernelExEx.
2. *Proof of split:* Linux first, Windows 98 started on a reserved core with
   no shared devices; serial console on both. Success = both run for an hour.
3. *Display:* kexdisp writes a shared framebuffer; Linux shows it. Then input
   (kexinput).
4. *3D:* Mesa9x with virgl over the shared ring; virglrenderer on Linux.
   Measure with 3DMark 99/2001 and Quake III.
5. *Audio, USB, storage, network* in the same pattern.
6. *Packaging:* one boot image (Linux side) plus KernelExEx installer
   components (Windows side).

**Effort, roughly:** phase 1 is weeks; phase 2 is the main research risk
(1-3 months); phases 3-4 are several months; phase 5 is open-ended. The
Windows-side drivers belong in KernelExEx; the Linux side and the boot path
are a separate repository.

### E. Period cards with real firmware, without a hypervisor

Running a card's real BIOS and vendor driver needs every access to the
emulated card to be intercepted. Without a hypervisor that means option C's
page-fault trapping, which is too slow for 3D cards. Conclusion: with no
hypervisor, use API forwarding (option D) for speed, and keep register-level
emulation for low-rate devices. Accurate period cards are a hypervisor job
(part 2).

## Part 2. The hypervisor project (brief for a separate project)

### Goal

Run Windows 98 SE / ME on a modern Linux PC at native CPU speed (KVM) with
graphics that are either **fast** (API forwarding to the host GPU) or
**faithful** (emulated period cards with their real ROMs and drivers, with
rendering done on the host GPU), selectable per machine.

### What exists to build on

* QEMU with KVM runs Windows 98 SE at native speed once patcher9x is applied.
* qemu-3dfx forwards Glide and OpenGL to the host GPU; SoftGPU provides
  OpenGL/DirectX/Glide drivers for VMware SVGA, VirtualBox and QEMU.
* 86Box has accurate device models of Voodoo 1/2/3, Banshee, S3, ATI, Matrox
  and others, with real ROMs. Its Voodoo renderer is software (JIT plus
  threads). 86Box is an emulator, not a KVM front end.
* GPU passthrough (VFIO) of a real period PCI card works for some vendors
  (reported working for 3dfx, Matrox and NVIDIA; ATI problematic).

### Architecture

```
Linux host
  QEMU (KVM, i440FX/PIIX4 machine, no ACPI option, patched TLB behaviour)
   ├─ "fast" path:  qemu-3dfx devices + virtio/VMware SVGA + SoftGPU guest drivers
   ├─ "faithful" path: device models ported from 86Box (Voodoo, S3, ...) as QEMU
   │     PCI devices, real ROMs supplied by the user; rasteriser back end:
   │       - software (86Box code, threaded)  -> reference
   │       - Vulkan back end                  -> new work
   ├─ audio: SB16/AWE/OPL from 86Box/DOSBox code, AC97/HDA as today
   └─ input: host evdev -> emulated gameport/USB HID/XInput-friendly devices
```

### Work items

1. **Machine baseline.** QEMU/KVM configuration that installs and runs
   Windows 98 SE / ME unattended from a user-supplied CD, with patcher9x,
   correct timing (CPU speed, TSC), and a test harness (serial log,
   screenshot comparison).
2. **Fast path packaging.** qemu-3dfx and SoftGPU integrated and tested;
   automatic selection of renderers per game.
3. **Port 86Box device models to QEMU.** Start with Voodoo 1/2 (simplest, most
   valuable), then Voodoo 3/Banshee, S3 ViRGE, Matrox. 86Box is GPL-2.0-or-
   later and QEMU is GPL-2.0, so code can move; keep 86Box's structure so
   fixes can flow back.
4. **Vulkan rasteriser for Voodoo.** Translate the Voodoo pipeline (triangle
   setup, colour combine, texture combine, fog, alpha, depth, dithering) to
   Vulkan graphics or compute shaders, keyed by the register state. Verify
   pixel-for-pixel against 86Box's software renderer on a capture set, with
   tolerances only where documented.
5. **ROM and driver handling.** The user supplies ROMs and drivers; the
   project ships none.
6. **Input and audio** using host devices (gamepads via evdev, USB audio,
   per-VM volume).
7. **Front end.** Machine presets ("Pentium II + Voodoo 2", "fast modern"),
   save states, shader presets.

### Acceptance tests

* Windows 98 SE installs unattended in under 15 minutes on KVM.
* 3DMark 99 MAX and 3DMark 2001 complete on the fast path.
* With an emulated Voodoo 2 and the Vulkan back end: Quake II, Unreal and
  Need for Speed III (Glide) run at 60 fps at 800x600 on a mid-range host GPU,
  with output matching the 86Box software renderer.
* DOS Glide titles work in a Windows 98 DOS box and in pure DOS.

### Risks

* Voodoo's fixed-function details (dithering, LOD, fog tables) are where
  pixel-exactness gets hard; the software renderer stays as the reference.
* Register-level emulation still costs one VM exit per MMIO write; batching
  through the Voodoo command FIFO (as the hardware did) is essential.
* Windows 9x guests are sensitive to timing; KVM paravirtual features must be
  off.

## Sources

* SoftGPU and related projects: https://github.com/JHRobotics/softgpu ,
  https://github.com/JHRobotics/mesa9x , https://github.com/jhrobotics/vmdisp9x ,
  VOGONS thread https://www.vogons.org/viewtopic.php?p=1163950
* qemu-3dfx: https://github.com/kjliew/qemu-3dfx and its wiki
* CSMWrap: https://github.com/FlyGoat/csmwrap and its wiki
* patcher9x: https://github.com/JHRobotics/patcher9x
* Windows 9x GPU driver limits: VOGONS https://www.vogons.org/viewtopic.php?p=1239948 ,
  https://www.vogons.org/viewtopic.php?p=1239912
* VBEMP 9x: https://bearwindows.zcm.com.au/vbe9x.htm
* 86Box: https://86box.net/2025/12/21/86box-v5-3.html ; host GPU not used:
  https://gamingonlinux.com/2025/08/retro-x86-based-machine-emulator-86box-v5-0-brings-a-new-integrated-machine-manager
* FreeBSD DRM ports: https://www.freebsd.org/status/report-2025-04-2025-06/drm-drivers
* amdgpu on 32-bit: https://bugzilla.kernel.org/show_bug.cgi?id=211349
* SBEMU / VSBHDA: https://github.com/thp/sbemu , https://github.com/baron-von-riedesel/vsbhda
* coLinux: https://en.wikipedia.org/wiki/CoLinux
* Popcorn Linux: https://www.kernel.org/doc/ols/2014/ols2014-barbalace.pdf
* mklinux: https://lwn.net/SubscriberLink/1038847/18f41d9b28065fce/ ,
  https://phoronix.com/news/Linux-7.0-mk2-Multikernel
