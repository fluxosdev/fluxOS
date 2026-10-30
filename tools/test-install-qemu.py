#!/usr/bin/env python3
"""Install fluxOS from a CD made by mkiso onto a blank disk, then start it.

The CD is the build's (build/iso, with GRUB from firmware/grub), except
that its setup system's init is tests/install-guest.c, which runs
fluxinstall onto the disk, and its menu starts that at once, in text mode.
The disk is then started alone: GRUB's boot sector and core image that
fluxinstall wrote, its modules and menu on the FAT32 partition, the kernel
from there and the system unpacked onto it, as / .
"""
import argparse
import io
import pathlib
import shutil
import subprocess
import tarfile
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ('qemu', 'mkiso', 'isotree', 'firmware', 'guest'):
    parser.add_argument(name)
parser.add_argument('--disk-mb', type=int, default=512)
parser.add_argument('--data-dir', default='/src', help="fluxinstall -e: a second, ext2, partition for it ('' for none)")
parser.add_argument('--system-mib', type=int, default=200, help='fluxinstall -r, with --data-dir')
parser.add_argument('--timeout', type=int, default=600)
args = parser.parse_args()

QEMU = [args.qemu, '-cpu', 'core2duo,-lm', '-smp', '2', '-m', '1G', '-display', 'none', '-nic', 'none',
        '-no-reboot']


def run(command, log, until, timeout):
    """Run QEMU until it exits; True if a line of the log has `until`."""
    with log.open('w') as output:
        proc = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        while proc.poll() is None:
            if time.monotonic() > deadline or until in log.read_text(errors='replace'):
                time.sleep(1)
                proc.kill()
                break
            time.sleep(0.5)
        proc.wait()
    return until in log.read_text(errors='replace')


with tempfile.TemporaryDirectory(prefix='flux-install-test-') as directory:
    temp = pathlib.Path(directory)
    tree = temp / 'iso'
    shutil.copytree(args.isotree, tree)
    setup = tree / 'boot/setup.tar'
    patched = temp / 'setup.tar'
    with tarfile.open(setup) as src, tarfile.open(patched, 'w', format=tarfile.USTAR_FORMAT) as dst:
        for member in src:
            if member.name.lstrip('./') == 'bin/init':
                dst.add(args.guest, arcname=member.name)
            else:
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        if args.data_dir:
            extra = f'-e\n{args.data_dir}\n-r\n{args.system_mib}\n'.encode()
            info = tarfile.TarInfo('./etc/install-args')
            info.size = len(extra)
            dst.addfile(info, io.BytesIO(extra))
    shutil.move(patched, setup)
    (tree / 'boot/grub/grub.cfg').write_text(
        'set timeout=0\nmenuentry setup {\n multiboot /boot/fluxos.kernel bootfs=iso9660 nosplash\n'
        ' module /boot/setup.tar setup\n set gfxpayload=text\n boot\n}\n')
    iso = temp / 'install.iso'
    subprocess.run([args.mkiso, '-o', str(iso), '-b', 'boot/grub/i386-pc/eltorito.img',
                    '-G', str(pathlib.Path(args.firmware) / 'boot_hybrid.img'), str(tree)], check=True)
    disk = temp / 'disk.img'
    with disk.open('wb') as f:
        f.truncate(args.disk_mb << 20)

    log = temp / 'install.log'
    ok = run(QEMU + ['-serial', f'file:{log}', '-cdrom', str(iso), '-boot', 'd',
                     '-drive', f'file={disk},format=raw,if=ide,index=0'],
             log, 'INSTALL PASSED', args.timeout)
    text = log.read_text(errors='replace')
    if not ok:
        print(text[-4000:])
        raise SystemExit('install: fluxinstall did not finish (see the log above)')
    print('install: fluxinstall installed fluxOS from the mkiso CD')
    if args.data_dir:
        # the second partition, as Linux's e2fsck sees it
        table = disk.read_bytes()[446:478]
        start, count = (int.from_bytes(table[24:28], 'little'), int.from_bytes(table[28:32], 'little'))
        if table[20] != 0x83 or not count:
            raise SystemExit('install: no second (ext2) partition in the partition table')
        if shutil.which('e2fsck'):
            part = temp / 'part2.img'
            with disk.open('rb') as src, part.open('wb') as dst:
                src.seek(start * 512)
                left = count * 512
                while left:
                    chunk = src.read(min(left, 1 << 20))
                    dst.write(chunk)
                    left -= len(chunk)
            check = subprocess.run(['e2fsck', '-fn', str(part)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if check.returncode:
                print(check.stdout.decode(errors='replace'))
                raise SystemExit('install: e2fsck finds the ext2 partition damaged')
            print(f'install: the ext2 partition for {args.data_dir} ({count // 2048} MiB) is clean to e2fsck')

    log = temp / 'boot.log'
    # with a data partition, until init has mounted it (/etc/fstab)
    marker = f'init: mounted /dev/hda2 on {args.data_dir}' if args.data_dir else 'Scheduler started, launching /bin/init'
    ok = run(QEMU + ['-serial', f'file:{log}', '-boot', 'c',
                     '-drive', f'file={disk},format=raw,if=ide,index=0'],
             log, marker, 120)
    text = log.read_text(errors='replace')
    root = [l for l in text.splitlines() if 'Root filesystem: /dev/hda1' in l]
    if not ok or not root or 'panic' in text.lower():
        print(text[-4000:])
        raise SystemExit('install: the installed disk did not start from its own partition (see the log above)')
    print('install: the disk started from GRUB on its own:', root[0].split('] ', 1)[-1])
    if args.data_dir:
        print(f'install: init mounted the ext2 partition on {args.data_dir} (/etc/fstab)')
    print('INSTALL TEST PASSED')
