#!/usr/bin/env python3
"""tests/bench/drvtest.py: the driver test bench (QEMU, Windows 98 SE).

usage: drvtest.py CASE [--sys FILE ...] [--keep] [--list]

A case names the emulated devices, the driver files to put on the test disk and
what the driver's debug log must (and must not) contain. The bench:
  1. copies the base image (a Windows 98 SE install, VM_BASE) copy-on-write,
  2. puts the driver files into C:\\KEXDRV,
  3. boots QEMU with the case's devices and -debugcon (build drivers with --debugcon),
  4. installs the driver through the "Add New Hardware" wizard ("Have disk",
     C:\\KEXDRV) as a user would,
  5. waits, then checks the debug log and saves a screenshot,
and prints PASS or FAIL with the reasons. Exit code 0 = pass.

Images and tools: VM_BASE (default /home/claude/vm/net-pci.img: Korean 98 SE, 2 GB,
MBR partition at sector 63), mtools, qemu-system-i386, socat-free (UNIX socket).
Copyright (C) 2026, KernelExEx contributors. GPLv2.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VM = Path(os.environ.get("VM_DIR", "/home/claude/vm"))
BASE = Path(os.environ.get("VM_BASE", str(VM / "net-pci.img")))
WORK = Path(os.environ.get("BENCH_DIR", "/tmp/bench"))
OFF = int(os.environ.get("VM_PART_OFFSET", "32256"))
# the user's Windows 98 CD: device installs copy Windows' own parts from it (WDM audio:
# KMixer, SysAudio, WDMAUD)
CD = os.environ.get("VM_CD", "/home/claude/w98/ko_win98_se_oem.iso")

# ---- the cases ---------------------------------------------------------------------------
CASES = {
    "hda": {
        "what": "kexhda.sys on QEMU's Intel HD Audio (ICH6) with a generic output codec",
        "qemu": ["-audiodev", "wav,id=snd0,path={work}/out.wav", "-device", "intel-hda",
                 "-device", "hda-output,audiodev=snd0"],
        "files": ["{sys:kexhda.sys}", str(ROOT / "drivers/audio/hda/kexhda.inf")],
        "expect": [r"kexhda: registers at", r"kexhda: CORB \d+ / RIRB \d+ entries",
                   r"kexhda: interrupt connected", r"kexhda: start result 0+\b"],
        "forbid": [r"start result 0*[1-9a-f]", r"\?\?"],
        "wait": 150,
        # playback check: off until it works in QEMU with the old driver too (after the
        # restart Sound Recorder finds no sound device and kexhda is not loaded again;
        # same before the KEDCL move). See docs/KEDCL.md.
        "play": False,
    },
    "em": {
        "what": "kexem.sys + kexbsd.sys (OpenBSD em) on QEMU's Intel 82540EM (e1000)",
        "nic": "user,model=e1000",
        "qemu": [],
        "files": ["{sys:kexem.sys}", "{sys:kexbsd.sys}", str(ROOT / "drivers/kexnet/kexem.inf")],
        "expect": [r"kexbsd: framework ready", r"em0: .*address [0-9a-f]{2}(:[0-9a-f]{2}){5}"],
        "forbid": [r"failed", r"panic"],
        "wait": 150,
        "play": False,
        # Windows finds the card at start-up and installs from its INF folder (as with
        # setup's DevicePath), then asks to restart: no wizard
        "install": "inf",
    },
    "em-wiz": {
        "what": "as em, installed through the wizard's Have Disk (no restart before the driver runs)",
        "nic": "user,model=e1000",
        "qemu": [],
        "files": ["{sys:kexem.sys}", "{sys:kexbsd.sys}", str(ROOT / "drivers/kexnet/kexem.inf")],
        "expect": [r"kexbsd: framework ready", r"em0: .*address [0-9a-f]{2}(:[0-9a-f]{2}){5}"],
        "forbid": [r"failed", r"panic"],
        "wait": 150,
        "play": False,
    },
    "ne2k-control": {
        "what": "control: an NE2000 PCI card with Windows' own driver (detect, install, restart)",
        "nic": "user,model=ne2k_pci",
        "qemu": [],
        "files": [],
        "expect": [],
        "forbid": [],
        "wait": 150,
        "play": False,
        "install": "inf",
    },
    "ac97": {
        "what": "kexac97.sys on QEMU's ICH AC'97",
        "qemu": ["-audiodev", "wav,id=snd0,path={work}/out.wav", "-device", "AC97,audiodev=snd0"],
        "files": ["{sys:kexac97.sys}", str(ROOT / "drivers/audio/ac97/kexac97.inf")],
        "expect": [r"kexac97"],
        "forbid": [],
        "wait": 150,
        "play": False,
    },
}


class Vm:
    def __init__(self, sock):
        self.sock = sock

    def cmd(self, line):
        for _ in range(20):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(str(self.sock))
                s.sendall((line + "\n").encode())
                time.sleep(0.2)
                s.close()
                return
            except OSError:
                time.sleep(0.5)

    def key(self, k, pause=0.3):
        self.cmd(f"sendkey {k}")
        time.sleep(pause)

    def type(self, text):
        m = {' ': 'spc', '.': 'dot', '\\': 'backslash', ':': 'shift-semicolon', '_': 'shift-minus', '-': 'minus',
             '/': 'slash', ',': 'comma'}
        for c in text:
            self.key(m.get(c, ('shift-' + c.lower()) if c.isupper() else c), 0.08)

    def eip(self):
        """the CPU's EIP (or None)"""
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(str(self.sock))
            s.sendall(b"info registers\n")
            time.sleep(0.5)
            out = s.recv(65536).decode("latin-1", "replace")
            s.close()
            m = re.search(r"EIP=([0-9a-f]{8})", out)
            return int(m.group(1), 16) if m else None
        except OSError:
            return None

    def shot(self, path):
        ppm = str(path) + ".ppm"
        self.cmd(f'screendump "{ppm}"')
        time.sleep(1)
        try:
            from PIL import Image
            Image.open(ppm).save(path)
            os.remove(ppm)
        except Exception:
            pass


def wav_peak(path):
    """largest absolute 16-bit sample in QEMU's WAV capture (0 if none)"""
    import struct
    try:
        d = path.read_bytes()
    except OSError:
        return 0
    i = d.find(b"data")
    if i < 0:
        return 0
    pcm = d[i + 8:]
    n = len(pcm) // 2
    return max((abs(v) for v in struct.unpack(f"<{n}h", pcm[:n * 2])), default=0)


def mtools(*args):
    subprocess.run(list(args), check=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", nargs="?")
    ap.add_argument("--sys", action="append", default=[], help="driver binary to test (default: build dirs)")
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list or not a.case:
        for k, v in CASES.items():
            print(f"{k:8} {v['what']}")
        return 0
    case = CASES[a.case]
    work = WORK / a.case
    work.mkdir(parents=True, exist_ok=True)
    img, log, sock = work / "disk.qcow2", work / "debug.log", work / "mon.sock"
    for f in (img, log, sock):
        f.unlink(missing_ok=True)
    # the driver files: --sys overrides, else the default build directories
    given = {Path(p).name.lower(): Path(p) for p in a.sys}
    files = []
    for f in case["files"]:
        m = re.fullmatch(r"\{sys:(.+)\}", f)
        if m:
            name = m.group(1)
            p = given.get(name.lower())
            if not p:
                cands = sorted(ROOT.parent.glob(f"build/*/{name}"), key=lambda q: q.stat().st_mtime)
                if not cands:
                    sys.exit(f"{name}: not built (pass --sys)")
                p = cands[-1]
            files.append(p)
        else:
            files.append(Path(f))
    # a writable copy of the base image with C:\KEXDRV
    raw = work / "disk.img"
    subprocess.run(["cp", "--sparse=always", str(BASE), str(raw)], check=True)
    mtools("mmd", "-i", f"{raw}@@{OFF}", "::/KEXDRV")
    for f in files:
        mtools("mcopy", "-o", "-i", f"{raw}@@{OFF}", str(f), "::/KEXDRV/")
        if case.get("install") == "inf":   # where Windows looks by itself (files beside the INF)
            mtools("mcopy", "-o", "-i", f"{raw}@@{OFF}", str(f), "::/WINDOWS/INF/")
    qemu = ["qemu-system-i386", "-M", "pc", "-cpu", "pentium2", "-m", "256", "-rtc", "base=localtime",
            "-drive", f"file={raw},format=raw,if=ide,index=0", "-boot", "c", "-vga", "cirrus",
            "-nic", case.get("nic", "user,model=pcnet"), "-display", "none", "-monitor", f"unix:{sock},server,nowait",
            # append: a power cycle starts QEMU again with the same log
            "-chardev", f"file,id=dbg,path={log},append=on", "-device", "isa-debugcon,chardev=dbg"] + (["-cdrom", CD] if CD and Path(CD).exists() else []) + \
        [x.format(work=work) for x in case["qemu"]]
    q = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    vm = Vm(sock)

    def first_start_hang():
        """Windows' ISA PnP delay loop with a frozen timer (VTD, C0053Bxx): the first start
        after a new device was installed sometimes stops there in QEMU (docs/NEXT.md).
        Do what a user would: switch off, start again (disk marked clean first)."""
        nonlocal q
        hits = sum(1 for _ in range(6) if 0xC0053B00 <= (vm.eip() or 0) < 0xC0053C00)
        if hits < 5:
            return False
        print("first-start hang (ISA PnP timer loop): power cycle")
        q.terminate()
        q.wait()
        subprocess.run([sys.executable, str(VM / "fatclean.py"), str(raw), str(OFF)], capture_output=True)
        sock.unlink(missing_ok=True)
        q = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        # the start-up menu after a failed start defaults to Safe mode: choose 1, Normal
        for _ in range(40):
            time.sleep(2)
            vm.cmd('screendump "/tmp/bt.ppm"')
            if subprocess.run([sys.executable, str(VM / "ismenu.py")]).returncode == 0:
                vm.key("1", 0.5)
                vm.key("ret", 1)
                break
        time.sleep(170)
        vm.key("ret", 30)   # log-on box, if any
        return True
    try:
        # Windows starts; the wizard comes up for the new device: Next, "list of drivers"?
        # As vm/hdatest.sh: Enter, Enter, then "Specify a location" with Have Disk.
        time.sleep(125)
        if case.get("install") == "inf":
            # Windows installs from our INF and asks to restart: yes; then the driver runs
            vm.shot(work / "found.png")
            vm.key("y", 200)
            first_start_hang()
            vm.key("ret", 1)
        else:
            vm.key("ret", 3)
            vm.key("ret", 3)
            vm.key("alt-f", 1)
            vm.key("alt-l", 1)
            vm.key("tab", 1)
            vm.type("C:\\KEXDRV")
            vm.key("ret", 8)
            vm.key("ret", 1)
        time.sleep(case["wait"])
        vm.shot(work / "end.png")
        if case.get("play"):
            # Finish; restart (Windows asks for it, or we ask: a new WDM audio device plays
            # only after one), then play a sound through Windows (Sound Recorder) into
            # QEMU's WAV file
            vm.key("ret", 10)
            vm.key("ctrl-esc", 3)
            vm.key("r", 6)
            vm.type("rundll32 shell32.dll,SHExitWindowsEx 2")
            vm.key("ret", 180)
            vm.shot(work / "restarted.png")
            vm.key("ret", 40)
            vm.key("ctrl-esc", 3)
            vm.key("r", 6)
            vm.type("sndrec32 /play /close C:\\WINDOWS\\MEDIA\\TADA.WAV")
            vm.key("ret", 40)
            vm.shot(work / "play.png")
    finally:
        if not a.keep:
            q.terminate()
            q.wait()
    text = log.read_text("latin-1") if log.exists() else ""
    fails = [f"missing: {e}" for e in case["expect"] if not re.search(e, text)]
    if case.get("play"):
        peak = wav_peak(work / "out.wav")
        print(f"audio captured: peak {peak} of 32767")
        if peak < 1000:
            fails.append("no sound in the captured audio")
    fails += [f"found: {f}" for f in case["forbid"] if re.search(f, text)]
    print(text[-3000:])
    print(f"screenshot: {work / 'end.png'}")
    print("PASS" if not fails else "FAIL: " + "; ".join(fails))
    raw.unlink(missing_ok=True)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
