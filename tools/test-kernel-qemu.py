#!/usr/bin/env python3
"""Boot the kernel regressions with a temporary initrd and blank scratch disk."""
import pathlib
import subprocess
import sys
import tarfile
import tempfile

qemu, kernel, initrd, guest = sys.argv[1:]
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
    command = [qemu, "-kernel", kernel, "-initrd", str(overlay), "-append", "automount=off",
               "-m", "256M", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-nic", "none", "-drive", f"file={scratch},format=raw,if=ide,index=0"]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode(errors="replace"))
        raise SystemExit("QEMU kernel regressions timed out")
    print(output)
    if result.returncode or "KERNEL REGRESSIONS PASSED" not in output:
        raise SystemExit("QEMU kernel regressions failed")
