#!/usr/bin/env python3
"""The hands-off test loop on a real Switch: close what runs, open the homebrew
menu's network loader, send an NRO with its options and files, and keep its
log, all from the PC.

It drives the console through sys-botbase (atmosphere/contents/
430000000000000B on the SD card; it starts at boot), which presses buttons and
takes screenshots over Wi-Fi on port 6000. Without it, `run` asks for the
network loader to be opened by hand (hold R while starting a game, then Y).

  tools/switch-dev.py run build/sms.nro --path sms/sms-dkgl.nro \\
      --file build/dkgl-programs.pack --secs 90 SMS_SWITCH_RENDERER=deko3d
  tools/switch-dev.py run build/gx_selftest.nro --path sms-dkgl-test/gx_selftest.nro
  tools/switch-dev.py shot now.jpg      the screen, as it is
  tools/switch-dev.py close             close the running game
  tools/switch-dev.py title             what runs (nothing: the HOME menu)
  tools/switch-dev.py buttons HOME,W1000,A    any sys-botbase clickSeq

`run` keeps the log (the program's stdout and stderr, through nxlink) in
logs/switch/<time>.log unless --log names it. --file LOCAL[:NAME] files are
written beside the NRO before the game reads them (switch_main.cpp's
SMS_DEV_FETCH), so env.txt can be one of them. SWITCH_IP or --ip: the
console's address (192.168.50.49 by default).
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOT_PORT = 6000
FETCH_PORT = 28772
IMAGE = os.environ.get("SMS_SWITCH_IMAGE", "devkitpro/devkita64:latest")


class Bot:
    """sys-botbase: a line per command; queries answer with a line."""

    def __init__(self, ip, timeout=5):
        self.sock = socket.create_connection((ip, BOT_PORT), timeout=timeout)
        self.sock.settimeout(30)
        self.buf = b""

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r\n")

    def query(self, line):
        self.send(line)
        while b"\n" not in self.buf:
            data = self.sock.recv(1 << 16)
            if not data:
                raise ConnectionError("sys-botbase closed the connection")
            self.buf += data
        out, self.buf = self.buf.split(b"\n", 1)
        return out.decode(errors="replace").strip()

    def buttons(self, seq):
        # clickSeq returns at once and runs its sequence on its own thread;
        # wait out its W pauses and clicks here so the next command follows it.
        self.send("clickSeq " + seq)
        steps = seq.split(",")
        time.sleep(sum(int(s[1:]) for s in steps if s[:1] == "W") / 1000 + 0.06 * len(steps) + 0.2)

    def title(self):
        return self.query("getTitleID")

    def shot(self, out):
        data = bytes.fromhex(self.query("pixelPeek"))
        if not data:
            raise RuntimeError("pixelPeek returned nothing (capture refused)")
        with open(out, "wb") as f:
            f.write(data)
        return len(data)

    def close_program(self, timeout=20):
        if not self.title():
            return False
        # HOME, then X (close software) and A (Close) on its prompt
        self.buttons("HOME,W1500,X,W1200,A,W2500")
        end = time.time() + timeout
        while self.title():
            if time.time() > end:
                raise RuntimeError("the program did not close")
            time.sleep(1)
        return True

    def open_netloader(self, timeout=25):
        """From the HOME menu: the selected game with R held starts the homebrew
        menu in its place (Atmosphère's default override), and Y opens its
        network loader."""
        self.buttons("+R,W300,A,W3500,-R,W200")
        end = time.time() + timeout
        while not self.title():
            if time.time() > end:
                raise RuntimeError("nothing started from the HOME menu (is a game selected?)")
            time.sleep(1)
        time.sleep(2)  # hbmenu reads the SD card's NROs before it takes input
        self.buttons("Y,W1500")
        # no virtual controller left for the game to take as player 1
        self.send("detachController")


def serve_files(files, ready):
    """The files for SMS_DEV_FETCH, once, to the first connection."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", FETCH_PORT))
    srv.listen(1)
    srv.settimeout(300)
    ready.set()
    try:
        conn, peer = srv.accept()
    except socket.timeout:
        print("switch-dev: the game never asked for its files", file=sys.stderr)
        return
    with conn:
        conn.sendall(b"SMSF")
        for local, name in files:
            size = os.path.getsize(local)
            conn.sendall(struct.pack("<H", len(name.encode())) + name.encode() + struct.pack("<Q", size))
            with open(local, "rb") as f:
                while chunk := f.read(1 << 20):
                    conn.sendall(chunk)
            print(f"switch-dev: sent {name} ({size} bytes)", file=sys.stderr)
        conn.sendall(struct.pack("<H", 0))
        conn.settimeout(60)
        try:
            conn.recv(3)
        except OSError:
            pass
    srv.close()


def connect_bot(ip, required):
    try:
        return Bot(ip)
    except OSError as e:
        if required:
            sys.exit(f"switch-dev: no sys-botbase at {ip}:{BOT_PORT} ({e})")
        return None


def cmd_run(a):
    nro = os.path.abspath(a.nro)
    if not os.path.isfile(nro):
        sys.exit(f"switch-dev: {a.nro} does not exist")
    files = []
    for spec in a.file or []:
        local, _, name = spec.partition(":")
        if not os.path.isfile(local):
            sys.exit(f"switch-dev: {local} does not exist")
        files.append((os.path.abspath(local), name or os.path.basename(local)))
    path = a.path or "sms/" + os.path.basename(nro)
    log = a.log or os.path.join(HERE, "logs", "switch", time.strftime("%Y%m%d-%H%M%S") + ".log")
    os.makedirs(os.path.dirname(log), exist_ok=True)

    bot = connect_bot(a.ip, False)
    if bot:
        if bot.close_program():
            print("switch-dev: closed the running program", file=sys.stderr)
        bot.open_netloader()
    else:
        print("switch-dev: no sys-botbase: open the homebrew menu (hold R while starting a game), "
              "then press Y", file=sys.stderr)

    args = list(a.args)
    if files:
        ready = threading.Event()
        threading.Thread(target=serve_files, args=(files, ready), daemon=True).start()
        ready.wait()
        args.append(f"SMS_DEV_FETCH={FETCH_PORT}")

    name = "switch-dev-" + uuid.uuid4().hex[:8]
    cmd = ["docker", "run", "--rm", "--name", name, "--network", "host",
           "-v", f"{os.path.dirname(nro)}:/b:ro", IMAGE,
           "/opt/devkitpro/tools/bin/nxlink", "-s", "-a", a.ip, "-r", "60" if bot else "600",
           "-p", path, "/b/" + os.path.basename(nro)] + args
    print(f"switch-dev: sending {os.path.basename(nro)} to sdmc:/switch/{path}; log in {log}", file=sys.stderr)
    shots = None
    if a.shots and bot:
        shots = os.path.splitext(log)[0] + "-shots"
        os.makedirs(shots, exist_ok=True)
    start = time.time()
    next_shot = start + a.shots if shots else None
    rc = None
    with open(log, "wb") as out:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        os.set_blocking(p.stdout.fileno(), False)
        try:
            while True:
                chunk = p.stdout.read()
                if chunk:
                    out.write(chunk)
                    out.flush()
                    if not a.quiet:
                        sys.stdout.buffer.write(chunk)
                        sys.stdout.flush()
                rc = p.poll()
                if rc is not None:
                    break
                now = time.time()
                if a.secs and now - start > a.secs:
                    print(f"\nswitch-dev: {a.secs} s are up", file=sys.stderr)
                    break
                if next_shot and now >= next_shot:
                    next_shot += a.shots
                    try:
                        bot.shot(os.path.join(shots, f"{int(now - start):04d}s.jpg"))
                    except Exception as e:  # a refused capture must not end the run
                        print(f"switch-dev: screenshot failed: {e}", file=sys.stderr)
                time.sleep(0.1)
        finally:
            if p.poll() is None:
                subprocess.run(["docker", "kill", name], capture_output=True)
                p.wait()
    if bot and not a.keep:
        try:
            if bot.close_program():
                print("switch-dev: closed it", file=sys.stderr)
        except Exception as e:
            print(f"switch-dev: could not close it: {e}", file=sys.stderr)
    print(f"switch-dev: log {log}" + (f", screenshots {shots}" if shots else ""), file=sys.stderr)
    return 0 if rc in (None, 0) else rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("SWITCH_IP", "192.168.50.49"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="close, open the network loader, send an NRO and keep its log")
    r.add_argument("nro")
    r.add_argument("args", nargs="*", help="NAME=value options, as env.txt lines")
    r.add_argument("--path", help="where on the SD card, under sdmc:/switch/ (default sms/<name>)")
    r.add_argument("--file", action="append", help="LOCAL[:NAME] written beside the NRO first")
    r.add_argument("--secs", type=float, default=0, help="stop after this long (default: when it exits)")
    r.add_argument("--shots", type=float, default=0, help="a screenshot every this many seconds")
    r.add_argument("--log", help="log file (default logs/switch/<time>.log)")
    r.add_argument("--keep", action="store_true", help="leave it running afterwards")
    r.add_argument("--quiet", action="store_true", help="only keep the log, do not print it")
    s = sub.add_parser("shot", help="a screenshot (JPEG)")
    s.add_argument("out", nargs="?", default="switch-shot.jpg")
    b = sub.add_parser("buttons", help="a sys-botbase clickSeq, e.g. HOME,W1000,A")
    b.add_argument("seq")
    sub.add_parser("close", help="close the running program")
    sub.add_parser("title", help="the running program's title ID (empty: the HOME menu)")
    a = ap.parse_args()

    if a.cmd == "run":
        return cmd_run(a)
    bot = connect_bot(a.ip, True)
    if a.cmd == "shot":
        print(f"{a.out}: {bot.shot(a.out)} bytes")
    elif a.cmd == "buttons":
        bot.buttons(a.seq)
        bot.send("detachController")
    elif a.cmd == "close":
        print("closed" if bot.close_program() else "nothing was running")
    elif a.cmd == "title":
        print(bot.title() or "(HOME menu)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
