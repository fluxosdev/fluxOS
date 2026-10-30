#!/usr/bin/env python3
"""Boot the kernel regressions with a temporary initrd and blank scratch disk."""
import argparse
import array
import math
import pathlib
import shutil
import subprocess
import tarfile
import tempfile
import wave

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "initrd", "guest"):
    parser.add_argument(name)
parser.add_argument("--cpus", type=int, default=2, choices=range(1, 17))
parser.add_argument("--append", default="")
parser.add_argument("--memory", type=int, default=256, choices=range(64, 4097), help="guest RAM in MiB")
parser.add_argument("--success-marker", default="KERNEL REGRESSIONS PASSED")
parser.add_argument("--add", action="append", default=[], metavar="HOST:GUEST", help="a host file to add to the initrd at that guest path")
parser.add_argument("--drop", action="append", default=[], metavar="PREFIX", help="leave initrd members under this path out (the toolchain, for tests that do not need it in memory)")
parser.add_argument("--timeout", type=int, default=45)
parser.add_argument("--fsck-ext2", action="store_true", help="the guest leaves an ext2 volume on the scratch disk: have e2fsck check it")
parser.add_argument("--sound", nargs="?", const="hda", choices=("hda", "sb16", "usb"), help="give the guest a sound card (HD Audio by default), record what it plays, and check that the mixer test's two tones play at once (skipped if QEMU cannot record to a WAV file)")
args = parser.parse_args()


def read_left(path):
    """The left channel of a 16-bit WAV, and its rate."""
    with wave.open(str(path), "rb") as recording:
        rate, channels = recording.getframerate(), recording.getnchannels()
        return array.array("h", recording.readframes(recording.getnframes()))[::channels], rate


def tone_power(samples, rate, hz):
    """Goertzel power of one frequency in samples, per sample."""
    if not samples:
        return 0.0
    coeff = 2 * math.cos(2 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for x in samples:
        s1, s2 = x + coeff * s1 - s2, s1
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / len(samples)


def tones_overlap(samples, rate, tones, reference, window=0.1):
    """Seconds of the recording in which every tone sounds at once: within
    10 dB of its own loudest window, and far above a frequency nobody played."""
    step = max(1, int(rate * window))
    windows = [samples[i:i + step] for i in range(0, len(samples) - step + 1, step)]
    power = [{hz: tone_power(w, rate, hz) for hz in tones + (reference,)} for w in windows]
    peak = {hz: max((p[hz] for p in power), default=0.0) for hz in tones}
    both = sum(1 for p in power if all(p[hz] > 0.1 * peak[hz] and p[hz] > 100 * max(p[reference], 1.0)
                                        for hz in tones))
    return both * step / rate


def can_record(qemu):
    """Does this QEMU have the wav audiodev?"""
    probe = subprocess.run([qemu, "-audiodev", "help"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return "wav" in probe.stdout.decode(errors="replace").split()


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
            name = member.name.lstrip("./")
            if name == "bin/init":
                target.add(guest, arcname=member.name)
                replaced = True
            elif any(name == prefix or name.startswith(prefix.rstrip("/") + "/") for prefix in args.drop):
                continue
            else:
                target.addfile(member, source.extractfile(member) if member.isfile() else None)
        for extra in args.add:
            host, guest = extra.split(":", 1)
            target.add(host, arcname="./" + guest.lstrip("/"))
    if not replaced:
        raise SystemExit("initrd has no /bin/init to replace")
    scratch = temp / "disk.img"
    with scratch.open("wb") as disk:
        disk.truncate(32 * 1024 * 1024)
    command = [qemu, "-cpu", "core2duo,-lm", "-smp", str(args.cpus),
               "-kernel", kernel, "-initrd", str(overlay), "-append", "automount=off " + args.append,
               "-m", f"{args.memory}M", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-nic", "none", "-drive", f"file={scratch},format=raw,if=ide,index=0"]
    recording = temp / "sound.wav"
    if args.sound and not can_record(qemu):
        print("host: QEMU has no wav audiodev, so the sound check is skipped")
        args.sound = None
    if args.sound:
        command += ["-audiodev", f"wav,id=snd,path={recording}"] + {
            "hda": ["-device", "intel-hda", "-device", "hda-output,audiodev=snd"],
            "sb16": ["-device", "sb16,audiodev=snd"],
            "usb": ["-device", "qemu-xhci", "-device", "usb-audio,audiodev=snd"],
        }[args.sound]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.timeout)
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode(errors="replace"))
        raise SystemExit("QEMU kernel regressions timed out")
    print(output)
    if result.returncode or args.success_marker not in output:
        raise SystemExit("QEMU kernel regressions failed")
    if f"guest: {expected_cpus} CPUs online" not in output:
        raise SystemExit(f"QEMU expected {expected_cpus} active CPUs")
    if args.sound:
        # Both tones, well above a frequency neither program played, and at
        # the same time: each program plays one second, so played one after
        # the other they would hardly overlap.
        samples, rate = read_left(recording)
        power = {hz: tone_power(samples, rate, hz) for hz in (440, 660, 550)}
        print("sound: tone power " + ", ".join(f"{hz} Hz {p:.3g}" for hz, p in power.items()))
        if min(power[440], power[660]) < 100 * max(power[550], 1.0):
            raise SystemExit("QEMU sound: the recording does not hold both programs' tones")
        overlap = tones_overlap(samples, rate, (440, 660), 550)
        print(f"sound: both tones together for {overlap:.2f} s")
        if overlap < 0.5:
            raise SystemExit("QEMU sound: the two programs' tones did not play at once")
    if args.fsck_ext2:
        if shutil.which("e2fsck"):
            check = subprocess.run(["e2fsck", "-fn", str(scratch)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if check.returncode:
                print(check.stdout.decode(errors="replace"))
                raise SystemExit("e2fsck found the guest's ext2 volume damaged")
            print("host: e2fsck finds the ext2 volume the guest wrote clean")
        else:
            print("host: no e2fsck to check the guest's ext2 volume with")
