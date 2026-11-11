#!/usr/bin/env python3
"""The Media Player in a real desktop: copy an installed disk image (fluxdisk.img) to a scratch
file, put the freshly built binaries, a sample and a startup script on it (mtools), boot it in
QEMU with a sound card recording to a WAV file, let the player run a few seconds, and keep
screenshots (QEMU's screendump) and the recording for a look. The disk image given is only read.

  tools/test-mediaplayer-qemu.py DISK.img SAMPLE OUTDIR [--seconds N] [--kvm]
"""
import argparse
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("disk")
parser.add_argument("sample")
parser.add_argument("outdir")
parser.add_argument("--qemu", default="qemu-system-x86_64")
parser.add_argument("--rootfs", default="build/rootfs")
parser.add_argument("--seconds", type=int, default=25, help="how long after boot to let it play")
parser.add_argument("--shots", type=int, default=3)
parser.add_argument("--keys", default="", help="monitor sendkey names, space separated, pressed halfway")
args = parser.parse_args()

out = pathlib.Path(args.outdir)
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="flux-mp-") as tmp:
    disk = pathlib.Path(tmp) / "disk.img"
    shutil.copyfile(args.disk, disk)
    target = f"{disk}@@1M"
    sample = pathlib.Path(args.sample)
    script = pathlib.Path(tmp) / "terminal"
    # the desktop starts "terminal -m" when it comes up: here that starts the player
    script.write_text(f"#!/bin/sh\nexec /bin/mediaplayer /home/{sample.name}\n")
    for src, dst in [(f"{args.rootfs}/bin/mediaplayer", "::/bin/mediaplayer"),
                     (f"{args.rootfs}/bin/desktop", "::/bin/desktop"),
                     (f"{args.rootfs}/bin/files", "::/bin/files"),
                     (str(script), "::/bin/terminal"),
                     (str(sample), f"::/home/{sample.name}")]:
        subprocess.run(["mcopy", "-o", "-i", target, src, dst], check=True)
    wav = out / "sound.wav"
    sock = pathlib.Path(tmp) / "mon.sock"
    cmd = [args.qemu, "-enable-kvm", "-cpu", "core2duo", "-smp", "2", "-m", "1024",
           "-drive", f"file={disk},format=raw,if=ide,index=0", "-boot", "c", "-display", "none",
           "-vga", "std", "-monitor", f"unix:{sock},server,nowait", "-serial", "file:" + str(out / "serial.log"),
           "-audiodev", f"wav,id=snd,path={wav}", "-device", "sb16,audiodev=snd", "-nic", "none"]
    qemu = subprocess.Popen(cmd)
    try:
        for _ in range(100):
            if sock.exists():
                break
            time.sleep(0.1)
        mon = socket.socket(socket.AF_UNIX)
        mon.connect(str(sock))
        mon.settimeout(5)

        def command(text):
            mon.sendall((text + "\n").encode())
            time.sleep(0.4)
            try:
                mon.recv(65536)
            except socket.timeout:
                pass

        step = args.seconds / (args.shots + 1)
        for i in range(args.shots):
            time.sleep(step)
            command(f"screendump {out}/shot{i}.ppm")
            if args.keys and i == args.shots // 2:
                for key in args.keys.split():
                    command(f"sendkey {key}")
        command("quit")
        qemu.wait(timeout=20)
    finally:
        if qemu.poll() is None:
            qemu.kill()
print("screenshots and sound.wav in", out)
