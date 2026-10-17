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

`run` keeps the log (the program's stdout and stderr) in logs/switch/<time>.log
unless --log names it, and --shots n window captures every n seconds as PNGs
beside it. nxlink only sends the NRO: the program then listens on port 28773
(switch/src/dev_link.cpp) and this connects to it, so nothing here has to
accept a connection from the console. --file LOCAL[:NAME] files are written
beside the NRO before the program reads them, so env.txt can be one of them.
SWITCH_IP or --ip: the console's address (192.168.50.49 by default).
"""
import argparse
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOT_PORT = 6000
LINK_PORT = 28773
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
        # clickSeq answers "done" once its sequence (W pauses too) has run
        steps = seq.split(",")
        waits = sum(int(s[1:]) for s in steps if s[:1] == "W") / 1000
        self.sock.settimeout(30 + waits + 0.2 * len(steps))
        try:
            reply = self.query("clickSeq " + seq)
        finally:
            self.sock.settimeout(30)
        if reply != "done":
            raise RuntimeError(f"clickSeq {seq}: {reply!r}")

    def title(self):
        """The running game's title ID; "" for none (the HOME menu, or an
        applet such as the Album's homebrew menu, which answer zeros)."""
        t = self.query("getTitleID")
        return "" if not t.strip("0") else t

    def shot(self, out):
        data = bytes.fromhex(self.query("pixelPeek"))
        if not data:
            raise RuntimeError("pixelPeek returned nothing (capture refused)")
        with open(out, "wb") as f:
            f.write(data)
        return len(data)

    def close_program(self, tries=3):
        if not self.title():
            return False
        for _ in range(tries):
            # HOME, then X (close software) and A (Close) on its prompt; the
            # first press after the virtual controller attaches can be lost
            self.buttons("HOME,W1500,X,W1200,A,W2500")
            end = time.time() + 8
            while self.title():
                if time.time() > end:
                    break
                time.sleep(1)
            else:
                return True
        raise RuntimeError("the program did not close")

    def open_netloader(self, tries=2):
        for attempt in range(tries):
            try:
                return self._open_netloader()
            except RuntimeError:
                if attempt == tries - 1:
                    raise
                # a picker or prompt left open: back to the HOME menu
                self.buttons("B,W800,B,W800")
                if self.title():
                    self.close_program()
                time.sleep(2)

    def _open_netloader(self, timeout=25):
        """From the HOME menu: the selected game with R held starts the homebrew
        menu in its place (Atmosphère's default override), and Y opens its
        network loader. R stays held through the user picker the game may ask
        for first, until the program starts."""
        # a plain A would let go of R: press and release it by hand
        self.buttons("+R,W200,+A,W150,-A,W1500")
        if not self.title():
            self.buttons("+A,W150,-A,W1000")  # the picker: its first user
        end = time.time() + timeout
        while not self.title():
            if time.time() > end:
                self.buttons("-R")
                raise RuntimeError("nothing started from the HOME menu (is a game selected?)")
            time.sleep(0.5)
        self.buttons("W2500,-R,W200")
        time.sleep(2)  # hbmenu reads the SD card's NROs before it takes input
        self.buttons("Y,W1500")
        # no virtual controller left for the game to take as player 1
        self.send("detachController")


def send_files(conn, files):
    """The files for dev_link.cpp, before it lets the program read any."""
    conn.sendall(b"SMSF")
    for local, name in files:
        size = os.path.getsize(local)
        conn.sendall(struct.pack("<H", len(name.encode())) + name.encode() + struct.pack("<Q", size))
        with open(local, "rb") as f:
            while chunk := f.read(1 << 20):
                conn.sendall(chunk)
        print(f"switch-dev: sent {name} ({size} bytes)", file=sys.stderr)
    conn.sendall(struct.pack("<H", 0))
    ok = b""
    while len(ok) < 3:
        data = conn.recv(3 - len(ok))
        if not data:
            break
        ok += data
    return ok == b"OK\n"


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        data = conn.recv(min(n - len(buf), 1 << 20))
        if not data:
            return None
        buf += data
    return bytes(buf)


def write_png(path, rgba, w, h):
    """GL's rows (bottom first, RGBA) as a PNG, top first, RGB."""
    stride = w * 4
    rows = []
    for y in range(h - 1, -1, -1):
        row = rgba[y * stride:(y + 1) * stride]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[0::4], row[1::4], row[2::4]
        rows.append(b"\0" + bytes(rgb))

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))


def take_shots(conn, folder):
    """Window captures (SMS_WINDOW_SHOTS) from the second connection."""
    while True:
        head = recv_exact(conn, 16)
        if not head or head[:4] != b"SHOT":
            return
        frame, w, h = struct.unpack("<III", head[4:])
        rgba = recv_exact(conn, w * h * 4)
        if rgba is None:
            return
        out = os.path.join(folder, f"frame{frame:06d}.png")
        write_png(out, rgba, w, h)
        print(f"switch-dev: capture {out}", file=sys.stderr)


def connect_link(ip, port, timeout):
    """The program listens once its loader has started it (dev_link.cpp)."""
    end = time.time() + timeout
    while True:
        try:
            return socket.create_connection((ip, port), timeout=5)
        except OSError:
            if time.time() > end:
                raise
            time.sleep(1)


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
    shots = None
    args = list(a.args) + [f"SMS_DEV_LISTEN={LINK_PORT}"]
    if a.shots:
        shots = os.path.splitext(log)[0] + "-shots"
        os.makedirs(shots, exist_ok=True)
        args.append(f"SMS_WINDOW_SHOTS=+{max(1, round(a.shots * 60))}")

    bot = connect_bot(a.ip, False)
    if bot:
        if bot.close_program():
            print("switch-dev: closed the running program", file=sys.stderr)
            time.sleep(2)  # the HOME menu takes input again
        bot.open_netloader()
    else:
        print("switch-dev: no sys-botbase: open the homebrew menu (hold R while starting a game), "
              "then press Y", file=sys.stderr)

    # nxlink only sends: the program's log comes back over its own link. It
    # tries the address once, so until the loader listens (opened by hand, or
    # hbmenu still starting) send again every few seconds.
    name = "switch-dev-" + uuid.uuid4().hex[:8]
    cmd = ["docker", "run", "--rm", "--name", name, "--network", "host",
           "-v", f"{os.path.dirname(nro)}:/b:ro", IMAGE,
           "/opt/devkitpro/tools/bin/nxlink", "-a", a.ip, "-r", "10",
           "-p", path, "/b/" + os.path.basename(nro)] + args
    print(f"switch-dev: sending {os.path.basename(nro)} to sdmc:/switch/{path}; log in {log}", file=sys.stderr)
    give_up = time.time() + (600 if not bot else 60)
    try:
        while True:
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 0:
                break
            if "Connection to" not in r.stdout + r.stderr or time.time() > give_up:
                sys.exit(f"switch-dev: nxlink failed:\n{r.stdout}{r.stderr}")
            time.sleep(3)
    except KeyboardInterrupt:
        subprocess.run(["docker", "kill", name], capture_output=True)
        raise

    rc = 0
    with open(log, "wb") as out:
        conn = connect_link(a.ip, LINK_PORT, 90)
        if not send_files(conn, files):
            print("switch-dev: the program did not take the files", file=sys.stderr)
        if shots:
            try:
                shot_conn = socket.create_connection((a.ip, LINK_PORT), timeout=5)
                shot_conn.settimeout(None)
                threading.Thread(target=take_shots, args=(shot_conn, shots), daemon=True).start()
            except OSError as e:
                print(f"switch-dev: no capture connection: {e}", file=sys.stderr)
        conn.settimeout(1)
        start = time.time()
        while True:
            try:
                chunk = conn.recv(1 << 16)
            except socket.timeout:
                chunk = None
            except OSError:
                break
            if chunk == b"":
                break  # the program ended
            if chunk:
                out.write(chunk)
                out.flush()
                if not a.quiet:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.flush()
            if a.secs and time.time() - start > a.secs:
                print(f"\nswitch-dev: {a.secs} s are up", file=sys.stderr)
                rc = None
                break
        conn.close()
    if bot and not a.keep:
        try:
            if bot.close_program():
                print("switch-dev: closed it", file=sys.stderr)
        except Exception as e:
            print(f"switch-dev: could not close it: {e}", file=sys.stderr)
    print(f"switch-dev: log {log}" + (f", captures {shots}" if shots else ""), file=sys.stderr)
    return 0 if rc in (None, 0) else rc


def main():
    # a stop from outside still runs the finally blocks that kill the container
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("SWITCH_IP", "192.168.50.49"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="close, open the network loader, send an NRO and keep its log")
    r.add_argument("nro")
    r.add_argument("args", nargs="*", help="NAME=value options, as env.txt lines")
    r.add_argument("--path", help="where on the SD card, under sdmc:/switch/ (default sms/<name>)")
    r.add_argument("--file", action="append", help="LOCAL[:NAME] written beside the NRO first")
    r.add_argument("--secs", type=float, default=0, help="stop after this long (default: when it exits)")
    r.add_argument("--shots", type=float, default=0, help="a window capture every this many seconds (at 60 fps)")
    r.add_argument("--log", help="log file (default logs/switch/<time>.log)")
    r.add_argument("--keep", action="store_true", help="leave it running afterwards")
    r.add_argument("--quiet", action="store_true", help="only keep the log, do not print it")
    s = sub.add_parser("shot", help="a screenshot (JPEG)")
    s.add_argument("out", nargs="?", default="switch-shot.jpg")
    b = sub.add_parser("buttons", help="a sys-botbase clickSeq, e.g. HOME,W1000,A")
    b.add_argument("seq")
    sub.add_parser("close", help="close the running program")
    sub.add_parser("title", help="the running program's title ID (empty: the HOME menu)")
    # NAME=value options may follow the flags as well as precede them
    a, extra = ap.parse_known_args()
    if extra and (a.cmd != "run" or any(x.startswith("-") or "=" not in x for x in extra)):
        ap.error("unrecognized arguments: " + " ".join(extra))
    if extra:
        a.args += extra

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
