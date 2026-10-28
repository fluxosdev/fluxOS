#!/usr/bin/env python3
"""Boot the kernel regressions with a temporary initrd and blank scratch disk."""
import argparse
import pathlib
import subprocess
import tarfile
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "initrd", "guest"):
    parser.add_argument(name)
parser.add_argument("--cpus", type=int, default=2, choices=range(1, 17))
parser.add_argument("--append", default="")
parser.add_argument("--memory", type=int, default=256, choices=range(64, 4097), help="guest RAM in MiB")
parser.add_argument("--success-marker", default="KERNEL REGRESSIONS PASSED")
args = parser.parse_args()
qemu, kernel, initrd, guest = args.qemu, args.kernel, args.initrd, args.guest
expected_cpus = args.cpus
for option in args.append.split():
    if option.startswith("maxcpus="):
        expected_cpus = min(expected_cpus, max(1, int(option.split("=", 1)[1])))
if "nosmp" in args.append.split():
    expected_cpus = 1
with tempfile.TemporaryDirectory(prefix="flux-kernel-test-") as directory:
    temp = pathlib.Path(directory)
    overlay = temp / "initrd.tar"
    replaced = False
    with tarfile.open(initrd, "r") as source, tarfile.open(overlay, "w", format=tarfile.USTAR_FORMAT) as target:
        for member in source:
            if member.name.lstrip("./") == "bin/init":
                target.add(guest, arcname=member.name)
                replaced = True
            else:
                target.addfile(member, source.extractfile(member) if member.isfile() else None)
    if not replaced:
        raise SystemExit("initrd has no /bin/init to replace")
    scratch = temp / "disk.img"
    with scratch.open("wb") as disk:
        disk.truncate(32 * 1024 * 1024)
    command = [qemu, "-cpu", "core2duo,-lm", "-smp", str(args.cpus),
               "-kernel", kernel, "-initrd", str(overlay), "-append", "automount=off " + args.append,
               "-m", f"{args.memory}M", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-nic", "none", "-drive", f"file={scratch},format=raw,if=ide,index=0"]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode(errors="replace"))
        raise SystemExit("QEMU kernel regressions timed out")
    print(output)
    if result.returncode or args.success_marker not in output:
        raise SystemExit("QEMU kernel regressions failed")
    if f"guest: {expected_cpus} CPUs online" not in output:
        raise SystemExit(f"QEMU expected {expected_cpus} active CPUs")
