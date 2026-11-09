#!/usr/bin/env python3
"""The first native build (phase 6.5 of self-hosting-roadmap.md).

Boots the Linux-built kernel and system (with the native GCC on it) with
tests/build-guest.c as init and the source disk ("make srcdisk") as
/dev/hdb, which runs make, make check and make iso on fluxOS. Then, on the
host: e2fsck judges the source disk after the build's hours of writes, the
native build's ISO tree and the members of its system.tar and setup.tar are
compared with the Linux build's by name, the native fluxOS.iso is booted
as it is until the kernel starts init, and tools/test-install-qemu.py
installs from the native ISO tree onto a blank disk and starts that disk.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "initrd", "guest", "srcdisk", "isotree", "mkiso", "firmware", "install_guest"):
    parser.add_argument(name)
parser.add_argument("--cpus", type=int, default=2)
parser.add_argument("--memory", type=int, default=2048, help="guest RAM in MiB")
parser.add_argument("--timeout", type=int, default=6 * 3600, help="seconds for the native build")
parser.add_argument("--kvm", action="store_true")
parser.add_argument("--skip-build", action="store_true", help="the source disk already holds a native build: only check it")
args = parser.parse_args()
here = pathlib.Path(__file__).resolve().parent
sys.stdout.reconfigure(line_buffering=True)     # in order with the QEMU runs' output


def debugfs(image, request):
    result = subprocess.run(["debugfs", "-R", request, image], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise SystemExit(f"debugfs {request!r}: {result.stderr.decode(errors='replace')}")
    return result


def tree_names(root):
    root = pathlib.Path(root)
    return {str(p.relative_to(root)) for p in root.rglob("*")}


def tar_names(path):
    with tarfile.open(path) as tar:
        return {m.name.lstrip("./").rstrip("/") for m in tar if m.name.lstrip("./")}


def compare(what, linux, native):
    only_linux, only_native = sorted(linux - native), sorted(native - linux)
    for name in only_linux[:40]:
        print(f"  only in the Linux build:  {name}")
    for name in only_native[:40]:
        print(f"  only in the native build: {name}")
    if only_linux or only_native:
        print(f"native-build: {what}: {len(only_linux)} names only in the Linux build, "
              f"{len(only_native)} only in the native build")
        return False
    print(f"native-build: {what}: the same {len(linux)} names as the Linux build")
    return True


for tool in ("debugfs", "e2fsck"):
    if not shutil.which(tool):
        raise SystemExit(f"native-build: needs {tool} (e2fsprogs) to read the source disk back")

if not args.skip_build:
    t0 = time.monotonic()
    command = [sys.executable, str(here / "test-kernel-qemu.py"), args.qemu, args.kernel, args.initrd, args.guest,
               "--cpus", str(args.cpus), "--memory", str(args.memory), "--timeout", str(args.timeout),
               "--disk", args.srcdisk, "--success-marker", "NATIVE BUILD PASSED"] + (["--kvm"] if args.kvm else [])
    if subprocess.run(command).returncode:
        raise SystemExit("native-build: the build on fluxOS failed (see the console above)")
    print(f"native-build: make, make check and make iso ran on fluxOS in {(time.monotonic() - t0) / 60:.0f} minutes")

check = subprocess.run(["e2fsck", "-fn", args.srcdisk], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
if check.returncode:
    print(check.stdout.decode(errors="replace")[-4000:])
    raise SystemExit("native-build: e2fsck finds the source disk damaged after the build")
print("native-build: e2fsck finds the source disk clean after the build")

with tempfile.TemporaryDirectory(prefix="flux-native-build-") as directory:
    temp = pathlib.Path(directory)
    native_tree = temp / "iso"
    native_iso = temp / "fluxOS.iso"
    debugfs(args.srcdisk, f"rdump /build/iso {temp}")
    debugfs(args.srcdisk, f"dump /fluxOS.iso {native_iso}")
    if not (native_tree / "boot/fluxos.kernel").is_file() or not native_iso.stat().st_size:
        raise SystemExit("native-build: the source disk holds no native build/iso and fluxOS.iso")

    same = compare("the ISO's files", tree_names(args.isotree), tree_names(native_tree))
    for name in ("system.tar", "setup.tar"):
        same &= compare(name, tar_names(pathlib.Path(args.isotree) / "boot" / name), tar_names(native_tree / "boot" / name))
    if not same:
        raise SystemExit("native-build: the native build's files differ from the Linux build's")

    # the native ISO as it is, as a CD: GRUB from it, then the kernel up to init
    log = temp / "boot.log"
    with log.open("w") as output:
        proc = subprocess.Popen([args.qemu, "-cpu", "core2duo,-lm", "-smp", "2", "-m", "1G", "-display", "none",
                                 "-nic", "none", "-no-reboot", "-serial", f"file:{log}", "-boot", "d",
                                 "-cdrom", str(native_iso)], stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 180
        marker = "Scheduler started, launching /bin/init"
        while proc.poll() is None and time.monotonic() < deadline and marker not in log.read_text(errors="replace"):
            time.sleep(0.5)
        proc.kill()
        proc.wait()
    text = log.read_text(errors="replace")
    if marker not in text or "panic" in text.lower():
        print(text[-4000:])
        raise SystemExit("native-build: the native fluxOS.iso did not start (see the log above)")
    print("native-build: the native fluxOS.iso starts as a CD")

    # and installs: fluxinstall from the native CD's setup system onto a blank disk
    # (a system partition with room for the GCC it carries)
    if subprocess.run([sys.executable, str(here / "test-install-qemu.py"), args.qemu, args.mkiso, str(native_tree),
                       args.firmware, args.install_guest, "--disk-mb", "1024", "--system-mib", "512"]).returncode:
        raise SystemExit("native-build: installing from the native build failed")

print("NATIVE BUILD TEST PASSED")
