#!/usr/bin/env python3
"""Exercise real S3 wake, without touching an installed disk.

The kernel starts through the firmware and Limine, from a small CD made for
the run, and comes back from S3 through the firmware's waking vector:
real mode, kernel/acpi/wakeup.S, long mode again.
"""
import argparse
import json
import pathlib
import socket
import subprocess
import tarfile
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ('qemu', 'kernel', 'initrd', 'guest', 'mkiso', 'liminedir'):
    parser.add_argument(name)
parser.add_argument('--cpus', type=int, default=2, choices=range(1, 17))
parser.add_argument('--drop', action='append', default=[], metavar='PREFIX', help='leave initrd members under this path out')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='flux-suspend-') as directory:
    temp = pathlib.Path(directory)
    initrd = temp / 'initrd.tar'
    with tarfile.open(args.initrd) as src, tarfile.open(initrd, 'w', format=tarfile.USTAR_FORMAT) as dst:
        replaced = False
        for member in src:
            name = member.name.lstrip('./')
            if name == 'bin/init':
                dst.add(args.guest, arcname=member.name)
                replaced = True
            elif any(name == prefix or name.startswith(prefix.rstrip('/') + '/') for prefix in args.drop):
                continue
            else:
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        if not replaced:
            raise SystemExit('initrd has no /bin/init')
    # Limine, the kernel and the initrd on a CD (firmware/limine, tools/bootiso.sh)
    subprocess.run(['sh', str(pathlib.Path(__file__).resolve().parent / 'bootiso.sh'), str(temp / 'boot.iso'),
                    args.kernel, str(initrd), 'textmode automount=off', args.mkiso, args.liminedir],
                   check=True, stdout=subprocess.DEVNULL)
    initrd.unlink()
    log = temp / 'serial.log'
    command = [args.qemu, '-cpu', 'core2duo', '-smp', str(args.cpus),
               '-cdrom', str(temp / 'boot.iso'), '-m', '512', '-display', 'none',
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
