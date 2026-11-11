#!/usr/bin/env python3
"""<video> and Media Source Extensions in Wave on a real desktop: copy an installed disk image
(fluxdisk.img) to a scratch file, put the freshly built Wave, media engine and desktop on it with
tests/wave-video/video.html and the streams it plays (tools/media-samples.py), have the desktop
start Wave on that page, and take screenshots. The page reports on the console, which goes to a
file on the disk; the console and the screenshots are checked: the page's report lines, and the
pixels of both videos changing between shots. The disk image given is only read.

  tools/test-wave-video-qemu.py DISK.img SAMPLES OUTDIR [--seconds N]
"""
import argparse
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("disk")
parser.add_argument("samples")
parser.add_argument("outdir")
parser.add_argument("--qemu", default="qemu-system-x86_64")
parser.add_argument("--rootfs", default="build/rootfs")
parser.add_argument("--page", default="tests/wave-video/video.html")
parser.add_argument("--seconds", type=int, default=40, help="how long after boot to let it run")
parser.add_argument("--sound", action="store_true", help="a Sound Blaster 16 recording to sound.wav")
parser.add_argument("--net", action="store_true", help="a network (for a page on the web: --url)")
parser.add_argument("--url", help="open this address instead of the test page (no checks then)")
parser.add_argument("--keys", default="", help="monitor sendkey names, space separated, pressed at --keys-at")
parser.add_argument("--keys-at", type=float, default=0.4, help="when to press them, as a fraction of --seconds")
parser.add_argument("--click", default="", help="X,Y: a mouse click there (screen pixels) at --keys-at")
args = parser.parse_args()


def read_ppm(path):
    data = path.read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:                   # P6, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    return int(parts[1]), int(parts[2]), data[pos + 1:]


out = pathlib.Path(args.outdir)
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="flux-wave-") as tmp:
    disk = pathlib.Path(tmp) / "disk.img"
    shutil.copyfile(args.disk, disk)
    part = pathlib.Path(tmp) / "part.img"
    offset = 1 << 20                        # the installer's one partition, ext2, at 1 MiB

    def take_part():
        with disk.open("rb") as src, part.open("wb") as dst:
            src.seek(offset)
            shutil.copyfileobj(src, dst, 1 << 22)

    def put_part():
        with part.open("rb") as src, disk.open("r+b") as dst:
            dst.seek(offset)
            shutil.copyfileobj(src, dst, 1 << 22)

    def debugfs(commands, write=False):
        cmds = pathlib.Path(tmp) / "debugfs.cmds"
        cmds.write_text("\n".join(commands) + "\n")
        subprocess.run(["debugfs"] + (["-w"] if write else []) + ["-f", str(cmds), str(part)],
                       check=True, capture_output=True)

    script = pathlib.Path(tmp) / "terminal"
    page = args.url or "file:///home/vtest/video.html"
    # the desktop starts "terminal -m" when it comes up: here that starts Wave
    script.write_text(f"#!/bin/sh\nexec /bin/wave {page} 2>/home/vtest/wave.log\n")
    script.chmod(0o755)
    files = [(f"{args.rootfs}/bin/{b}", f"/bin/{b}") for b in ("wave", "wavemedia", "desktop", "mediaplayer", "files")]
    files += [(str(script), "/bin/terminal"), (args.page, "/home/vtest/video.html")]
    files += [(f"{args.samples}/{s}", f"/home/vtest/{s}") for s in ("yt-18.mp4", "yt-133.mp4", "yt-140.mp4")]
    take_part()
    commands = ["mkdir /home/vtest", "sif /home/vtest mode 040777"]
    for src, dst in files:
        commands += [f"rm {dst}", f"write {pathlib.Path(src).resolve()} {dst}", f"sif {dst} mode 0100775"]
    debugfs(commands, write=True)
    put_part()
    sock = pathlib.Path(tmp) / "mon.sock"
    cmd = [args.qemu, "-enable-kvm", "-cpu", "core2duo", "-smp", "2", "-m", "1024",
           "-drive", f"file={disk},format=raw,if=ide,index=0", "-boot", "c", "-display", "none",
           "-vga", "std", "-monitor", f"unix:{sock},server,nowait", "-serial", "file:" + str(out / "serial.log"),
           "-nic", "user,model=e1000" if args.net else "none"]
    if args.sound:
        cmd += ["-audiodev", f"wav,id=snd,path={out / 'sound.wav'}", "-device", "sb16,audiodev=snd"]
    qemu = subprocess.Popen(cmd)
    shots = []
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

        # screenshots each few seconds once the page has had time to start
        start = time.time()
        times = [args.seconds * f for f in (0.45, 0.5, 0.55, 0.62, 0.8, 1.0)]
        events = sorted(times + ([args.seconds * args.keys_at] if args.keys or args.click else []))
        pressed = False
        for t in events:
            time.sleep(max(0, start + t - time.time()))
            if not pressed and (args.keys or args.click) and t == args.seconds * args.keys_at:
                pressed = True
                if args.click:          # QEMU's mouse is relative: from the far corner, then there
                    x, y = (int(v) for v in args.click.split(","))
                    command("mouse_move -2000 -2000")
                    command(f"mouse_move {x} {y}")
                    command("mouse_button 1")
                    command("mouse_button 0")
                for key in args.keys.split():
                    command(f"sendkey {key}")
                continue
            i = len(shots)
            shot = out / f"shot{i}.ppm"
            command(f"screendump {shot}")
            shots.append(shot)
        command("quit")
        qemu.wait(timeout=20)
    finally:
        if qemu.poll() is None:
            qemu.kill()
    take_part()
    (out / "wave.log").unlink(missing_ok=True)
    debugfs([f"dump /home/vtest/wave.log {out.resolve() / 'wave.log'}"])

log = (out / "wave.log").read_text(errors="replace") if (out / "wave.log").exists() else ""
report = [l.split("WAVETEST ", 1)[1] for l in log.splitlines() if "WAVETEST " in l]
print("\n".join("page: " + r for r in report))
print("screenshots and wave.log in", out)
if args.url:
    sys.exit(0)

failures = []


def need(cond, what):
    if not cond:
        failures.append(what)


def find(prefix):
    return next((r for r in report if r.startswith(prefix)), "")


need(find("types ") == "types probably,true,true", "the types the page asks about")
need(find("sourceopen"), "sourceopen")
appended = find("appended ")
need(appended.startswith("appended 1 0.00-18.9") or appended.startswith("appended 1 0.00-19.0"), "the buffered range after the appends")
need("readyState 4" in appended and "size 320x240" in appended, "readyState and size after the appends")
need(find("mse playing"), "the MSE video playing")
file_at = find("file at ")
need(file_at and float(file_at.split()[2]) >= 6.0 and "readyState 4" in file_at and "size 320x240" in file_at,
     "the file playing in time")
need(file_at and "loadedmetadata" in file_at and "playing" in file_at, "the file's events")
mse_at = find("mse at ")
need(mse_at and float(mse_at.split()[2]) >= 3.0, "the MSE video playing in time")
need(find("mse seeked to 15"), "the MSE video seeking")
need(find("mse ended"), "the MSE video ending after endOfStream()")

# both videos' pixels change between the screenshots while they play
if len(shots) >= 3 and all(s.exists() for s in shots[:3]):
    w, h, a = read_ppm(shots[0])
    _, _, b = read_ppm(shots[1])
    _, _, c = read_ppm(shots[2])
    changed = [0, 0]
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            i = (y * w + x) * 3
            if a[i:i + 3] != b[i:i + 3] or b[i:i + 3] != c[i:i + 3]:
                changed[0 if x < w // 2 else 1] += 1
    print(f"host: pixels changing between shots: left half {changed[0]}, right half {changed[1]}")
    need(sum(changed) > 500, "pictures changing on the screen")
else:
    failures.append("screenshots")
for f in failures:
    print("FAIL:", f)
if failures:
    sys.exit("Wave video regressions failed")
print("WAVE VIDEO REGRESSIONS PASSED")
