#!/usr/bin/env python3
"""FFmpeg's own tests on fluxOS: fetch the FATE samples tests/ffmpeg/fate.txt names
(once, into a cache) and boot tests/ffmpeg-guest with ffmpeg, ffprobe, the samples and
FATE's references in its initrd (tools/test-kernel-qemu.py does the booting)."""
import argparse
import pathlib
import subprocess
import sys
import urllib.request

FATE_SUITE = "https://fate-suite.ffmpeg.org/"

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "initrd", "guest", "ffmpeg", "ffprobe"):
    parser.add_argument(name)
parser.add_argument("--list", default="tests/ffmpeg/fate.txt")
parser.add_argument("--refs", default="tests/ffmpeg/ref")
parser.add_argument("--cache", default="build/tests/fate-suite")
parser.add_argument("harness", nargs=argparse.REMAINDER, help="more options for test-kernel-qemu.py")
args = parser.parse_args()

# What the tests read: SAMPLES/ paths in the arguments, and the reference sound of
# the oneoff and stddev comparisons (the last word of the comparison).
samples = {"mov/mov-3elist.mov"}            # what ffmpeg-guest.c has ffprobe describe
for line in pathlib.Path(args.list).read_text().splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    name, cmp, ffargs = (field.strip() for field in line.split("|", 2))
    samples.update(word[len("SAMPLES/"):] for word in ffargs.split() if word.startswith("SAMPLES/"))
    if cmp.split()[0] in ("oneoff", "stddev"):
        samples.add(cmp.split()[-1])

cache = pathlib.Path(args.cache)
for sample in sorted(samples):
    path = cache / sample
    if path.exists():
        continue
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"host: fetching {FATE_SUITE}{sample}", flush=True)
    part = path.with_name(path.name + ".part")
    with urllib.request.urlopen(FATE_SUITE + sample) as response, part.open("wb") as out:
        out.write(response.read())
    part.rename(path)

add = [f"{args.ffmpeg}:/usr/bin/ffmpeg", f"{args.ffprobe}:/usr/bin/ffprobe", f"{args.list}:/test/fate.txt"]
add += [f"{cache / s}:/test/fate-suite/{s}" for s in sorted(samples)]
add += [f"{ref}:/test/ref/{ref.name}" for ref in sorted(pathlib.Path(args.refs).iterdir())]
command = [sys.executable, str(pathlib.Path(__file__).with_name("test-kernel-qemu.py")),
           args.qemu, args.kernel, args.initrd, args.guest]
for a in add:
    command += ["--add", a]
command += [a for a in args.harness if a != "--"]
sys.exit(subprocess.call(command))
