#!/usr/bin/env python3
"""Exercise real S3 wake through GRUB, without touching an installed disk.

QEMU's -kernel loader reloads kernel memory on reset/wakeup, destroying the
saved CPU context. Boot an ISO instead so that RAM survives S3.
"""
import argparse
import json
import pathlib
import shutil
import socket
import subprocess
import tarfile
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ('qemu', 'kernel', 'initrd', 'guest', 'mkrescue', 'grubdir'):
    parser.add_argument(name)
parser.add_argument('--cpus', type=int, default=2, choices=range(1, 17))
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='flux-suspend-') as directory:
    temp = pathlib.Path(directory)
    tree = temp / 'iso'
    (tree / 'boot/grub').mkdir(parents=True)
    shutil.copy(args.kernel, tree / 'kernel')
    with tarfile.open(args.initrd) as src, tarfile.open(tree / 'initrd', 'w', format=tarfile.USTAR_FORMAT) as dst:
        replaced = False
        for member in src:
            if member.name.lstrip('./') == 'bin/init':
                dst.add(args.guest, arcname=member.name)
                replaced = True
            else:
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        if not replaced:
            raise SystemExit('initrd has no /bin/init')
    (tree / 'boot/grub/grub.cfg').write_text(
        'set timeout=0\nmenuentry test {\n multiboot /kernel automount=off\n'
        ' module /initrd\n boot\n}\n')
    subprocess.run([args.mkrescue, '-d', args.grubdir, '-o', str(temp / 'boot.iso'), str(tree)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = temp / 'serial.log'
    command = [args.qemu, '-cpu', 'core2duo,-lm', '-smp', str(args.cpus),
               '-cdrom', str(temp / 'boot.iso'), '-m', '256', '-display', 'none',
               '-serial', 'stdio', '-qmp', f'unix:{temp}/qmp,server=on,wait=off',
               '-no-reboot', '-nic', 'none']
    cycles = 0
    error = None
    with log.open('w') as output:
        proc = subprocess.Popen(command, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 60
            while not (temp / 'qmp').exists():
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('QEMU monitor did not start')
                time.sleep(.05)
            with socket.socket(socket.AF_UNIX) as sock:
                sock.connect(str(temp / 'qmp'))
                sock.settimeout(30)
                with sock.makefile('rwb', buffering=0) as monitor:
                    def send(name):
                        monitor.write((json.dumps({'execute': name}) + '\n').encode())
                    json.loads(monitor.readline())
                    send('qmp_capabilities')
                    while time.monotonic() < deadline:
                        line = monitor.readline()
                        if not line:
                            break
                        msg = json.loads(line)
                        if 'error' in msg:
                            raise RuntimeError(str(msg['error']))
                        if msg.get('event') == 'SUSPEND':
                            cycles += 1
                            if cycles > 3:
                                raise RuntimeError('unexpected extra suspend')
                            send('system_wakeup')
                        if msg.get('event') == 'SHUTDOWN':
                            break
                    proc.wait(timeout=5)
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
            error = str(exc)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    text = log.read_text(errors='replace')
    print(text)
    if error or proc.returncode or cycles != 3 or 'SUSPEND REGRESSIONS PASSED' not in text:
        raise SystemExit(f'Suspend regression failed ({cycles} S3 cycles): {error or "guest failed"}')
