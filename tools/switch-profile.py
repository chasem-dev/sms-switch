#!/usr/bin/env python3
"""Names the functions in a console profile (switch/src/profiler.cpp,
SMS_SWITCH_PROFILE=start,seconds): per thread, the call stacks it was found
in and how often.

  tools/switch-profile.py logs/switch/run.log [--elf build/port/sms.elf] [--top 30]

For each thread: where its time goes (the function it was in), and for the
samples that were waiting in the kernel, what it was waiting from (the first
caller outside the libraries' waiting functions). The ELF's symbols come from
the devkitpro/devkita64 image's nm (Docker)."""
import argparse, bisect, os, re, subprocess
from collections import Counter

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# frames that only wait: the caller of the first other one is the reason
WAITING = re.compile(r"^(svc|mutex|condvar|rmutex|semaphore|barrier|__libnx|__syscall|_lock|__lock|__retarget|"
                     r"pthread_|std::condition_variable|std::mutex|std::__|std::unique_lock|leventWait|ueventWait|"
                     r"waitSingle|waitMulti|waitObjects|threadWaitForExit|cnd_|mtx_)")


def symbols(elf):
    out = subprocess.run(["docker", "run", "--rm", "-v", f"{os.path.dirname(os.path.abspath(elf))}:/e:ro",
                          "devkitpro/devkita64:latest", "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm", "-C", "-n",
                          "--defined-only", "/e/" + os.path.basename(elf)], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]+) ([tTwW]) (.*)", line)
        if m:
            addrs.append(int(m.group(1), 16))
            names.append(m.group(3))
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--elf", default=os.path.join(HERE, "build", "port", "sms.elf"))
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()
    base = 0  # where the image runs (switch_main.cpp moves it below 4 GiB)
    labels, totals, stacks = {}, {}, {}
    for line in open(a.log, errors="replace"):
        m = re.search(r"image at 0x([0-9a-f]+)", line)
        if m:
            base = int(m.group(1), 16)
        m = re.search(r"profile thread (\d+) (.*): (\d+) samples", line)
        if m:
            labels[int(m.group(1))] = m.group(2)
            totals[int(m.group(1))] = int(m.group(3))
            continue
        m = re.search(r"profile (\d+) (\d+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)$", line.strip())
        if m:
            t = int(m.group(1))
            stacks.setdefault(t, []).append((int(m.group(2)), [int(m.group(i), 16) for i in range(3, 8)]))
    addrs, names = symbols(a.elf)

    def name(pc):
        if not pc:
            return ""
        pc = pc - base if pc >= base else pc
        i = bisect.bisect_right(addrs, pc) - 1
        return names[i] if i >= 0 else "?"

    def label(t):
        m = re.match(r"entry ([0-9a-f]+)", labels[t])
        return f"{labels[t]} ({name(int(m.group(1), 16))[:60]})" if m else labels[t]

    for t in sorted(totals, key=lambda t: -totals[t]):
        total = totals[t]
        flat, waits = Counter(), Counter()
        for n, st in stacks.get(t, []):
            frames = [name(pc) for pc in st if pc]
            flat[frames[0] if frames else "?"] += n
            if frames and WAITING.match(frames[0]):
                rest = [f for f in frames[1:] if not WAITING.match(f)]
                waits[" <- ".join(f[:70] for f in rest[:2]) or "?"] += n
        waited = sum(waits.values())
        print(f"== thread {t} {label(t)}: {total} samples, {100.0 * waited / total:.0f}% waiting")
        for nm, n in flat.most_common(a.top):
            print(f"  {100.0 * n / total:6.2f}%  {nm[:120]}")
        if waited:
            print("  waiting from:")
            for nm, n in waits.most_common(12):
                print(f"  {100.0 * n / total:6.2f}%  {nm}")
        print()


if __name__ == "__main__":
    main()
