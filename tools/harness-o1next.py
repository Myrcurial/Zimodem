#!/usr/bin/env python3
"""The WiFi lab fixture: what the board's network side needs to talk to.

    tools/harness-o1next.py build/o1next-s2/zimodem.ino.bin

Three listeners, one log, because they answer three different questions and the
board is the client of all of them:

  HTTP   :8080  the firmware image, for `update <url>` -- and a /hello.txt, for
                exercising the modem's own HTTP side
  telnet :2323  a BBS-shaped telnet server: IAC negotiation, a banner, and an echo.
                This is what `ATDT <host> 2323` from the modem connects to, and the
                counterparty the whole modem exists to talk to
  SSH    :2222  the system's sshd, started as this user with a key of its own.
                What #321's key auth, host verification and sessions are tested
                against

WHY THESE THREE, AND WHY IN ONE PROCESS

The modem's job is telnet and SSH; the OTA's job is HTTP. Testing any of them means
having the other end of a socket on the same network, and having three tools to
start (and three logs to watch) is how a test session spends its first ten minutes
doing bookkeeping. One command, one log, and every line carries the peer address.

If you only need the OTA step, `tools/ota-server-o1next.py` is the same HTTP
listener without the other two -- a quiet log is worth something when the thing you
are watching is a 1 MB transfer.

TWO THINGS THIS MACHINE IMPOSES, both measured rather than assumed

  - **The application firewall *asks*, and an unanswered question looks like a block.**
    The first time a new binary accepts an incoming connection, macOS asks the operator
    whether it may; until that is answered (and if it is denied) connections to the
    port *hang* -- loopback serves, the interface address times out, and nothing in any
    log says why.  Built-in signed binaries are exempt, so /usr/bin/python3 never
    prompts while a Homebrew python3 does.  Answer the prompt, or add the binary with
    `sudo socketfilterfw --add` and `--unblockapp`; `socketfilterfw --listapps` shows
    who has been allowed.  This harness prints the hint at startup, because a hang with
    no message is the worst shape a failure can have.
  - **SSH here is key auth only.** sshd started as a normal user cannot read the
    shadow file, so password authentication cannot work -- which is fine for #321,
    whose work is key auth, and is a real limit for testing upstream's
    password-only client. The harness makes its own host key, its own client key
    and its own authorized_keys in --ssh-dir, and touches nothing in ~/.ssh.

The board's side of all this, once it is on the network:

    wifi <ssid> <password>                        (or the modem's AT*SSID/AT*PASS)
    update http://192.168.2.1:8080/<image>        (the OTA, over WiFi)
    ATDT 192.168.2.1 2323                         (the modem dials the telnet server)
    ssh -p 2222 ...                               (#321, when it lands)
"""
import argparse
import fcntl
import hashlib
import os
import select
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HTTP_CHUNK = 64 * 1024
SIOCGIFADDR = 0xc0206921        # macOS/BSD; the buffer must be a whole ifreq (32)

IAC, WILL, WONT, DO, DONT = 0xFF, 0xFB, 0xFC, 0xFD, 0xFE
OPT_BINARY, OPT_ECHO, OPT_SGA = 0x00, 0x01, 0x03


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def local_ipv4s():
    """Every interface with an IPv4 address, as (name, address).  Not guessed: the
    address the board needs is the one on the interface it can reach, and on a
    machine that is both a router and a client there is more than one answer."""
    found = []
    for _, name in socket.if_nameindex():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            res = fcntl.ioctl(sock.fileno(), SIOCGIFADDR,
                              struct.pack("16s16x", name.encode()[:15]))
            found.append((name, socket.inet_ntoa(res[20:24])))
        except OSError:
            pass
        finally:
            sock.close()
    return found


def note(peer, fmt, *args):
    sys.stderr.write("%s  %-15s  " % (time.strftime("%H:%M:%S"), peer))
    sys.stderr.write(fmt % args if args else fmt)
    sys.stderr.write("\n")
    sys.stderr.flush()


# --------------------------------------------------------------------- HTTP --
class HttpHandler(BaseHTTPRequestHandler):
    """The image, and one known page.  Strict about everything else: a typo should
    be a 404 the board can report, not a small file written into a slot."""

    server_version = "o1next-harness"
    protocol_version = "HTTP/1.1"        # keep-alive; every answer has a length

    def log_message(self, fmt, *args):
        pass                             # this tool prints its own, with sizes in them

    def send_head(self, code, length, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.end_headers()

    def do_HEAD(self):
        self.handle_request(False)

    def do_GET(self):
        self.handle_request(True)

    def handle_request(self, body):
        peer = self.client_address[0]
        path = self.path.split("?", 1)[0]

        if path in ("/", "/index.html"):
            text = ("%s\n%d bytes\nsha256 %s\n\nGET /%s\ntelnet %s:%d\n"
                    % (self.server.image_name, self.server.image_size,
                       self.server.image_sha256, self.server.image_name,
                       self.server.http_host, self.server.telnet_port)).encode()
            self.send_head(200, len(text), "text/plain; charset=utf-8")
            if body:
                self.wfile.write(text)
            note(peer, "index")
            return

        if path == "/hello.txt":
            text = (b"hello from the o1next harness\r\n"
                    b"if you can read this, HTTP works end to end\r\n")
            self.send_head(200, len(text), "text/plain; charset=utf-8")
            if body:
                self.wfile.write(text)
            note(peer, "hello.txt (%d bytes)", len(text))
            return

        if path != "/" + self.server.image_name:
            self.send_head(404, 0, "text/plain; charset=utf-8")
            note(peer, "404 %s -- the image is at /%s", path, self.server.image_name)
            return

        if not body:
            self.send_head(200, self.server.image_size, "application/octet-stream")
            note(peer, "HEAD %s", self.server.image_name)
            return

        self.send_head(200, self.server.image_size, "application/octet-stream")
        began = time.time()
        sent = 0
        try:
            with open(self.server.image_path, "rb") as fh:
                while True:
                    block = fh.read(HTTP_CHUNK)
                    if not block:
                        break
                    self.wfile.write(block)
                    sent += len(block)
        except (BrokenPipeError, ConnectionResetError):
            note(peer, "client went away after %s in %.1f s",
                 human(sent), time.time() - began)
            return
        took = max(time.time() - began, 1e-6)
        note(peer, "sent %s in %.1f s (%s/s)", human(sent), took, human(sent / took))


class HttpServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ------------------------------------------------------------------- telnet --
class TelnetHandler(socketserver.BaseRequestHandler):
    """A BBS-shaped telnet server, which is what the modem dials.

    It negotiates the three options a BBS does -- WILL ECHO, WILL SGA, DO SGA -- and
    answers everything else with DONT, then sends a banner and echoes what arrives.
    That is enough to prove the whole path: text in at the modem's serial side,
    across the network, echoed back, and out the serial side again.

    `quit` or `bye` hangs up, which is how a session's end gets tested on purpose
    rather than by unplugging something.
    """

    def handle(self):
        peer = self.client_address[0]
        sock = self.request
        began = time.time()
        in_bytes = out_bytes = 0

        def send(payload):
            nonlocal out_bytes
            try:
                sock.sendall(payload)
                out_bytes += len(payload)
            except OSError:
                pass

        # the opening a BBS sends: we echo, we suppress go-ahead, you suppress too
        send(bytes([IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA, IAC, DO, OPT_SGA]))
        banner = ("\r\n\r\no1next harness telnet -- %s\r\n"
                  "type `quit` to hang up\r\n\r\n" % self.server.hostname)
        send(banner.encode())
        note(peer, "telnet session opened")

        line = bytearray()
        try:
            while True:
                ready, _, _ = select.select([sock], [], [], 1.0)
                if not ready:
                    if self.server.stopping:
                        break
                    continue
                data = sock.recv(512)
                if not data:
                    break
                in_bytes += len(data)

                out = bytearray()
                i = 0
                while i < len(data):
                    byte = data[i]
                    if byte == IAC and i + 2 < len(data):
                        cmd, opt = data[i + 1], data[i + 2]
                        if cmd in (DO, DONT):
                            # we will not do anything else, and saying so is polite
                            out += bytes([IAC, WONT, opt])
                        i += 3
                        continue
                    i += 1
                    if byte == 0x0D or byte == 0x0A:
                        if not line:
                            continue
                        text = line.decode("latin-1").strip()
                        line.clear()
                        if text.lower() in ("quit", "bye", "exit"):
                            send(b"\r\nbye\r\n")
                            raise StopIteration
                        reply = "\r\nyou said: %s\r\n" % text
                        send(reply.encode())
                        note(peer, "telnet line: %r", text)
                        continue
                    if byte in (0x7F, 0x08):
                        if line:
                            line.pop()
                            out += b"\b \b"
                        continue
                    if 0x20 <= byte <= 0x7E:
                        line.append(byte)
                        out += bytes([byte])
                if out:
                    send(bytes(out))
        except (StopIteration, OSError):
            pass
        finally:
            took = time.time() - began
            note(peer, "telnet session closed: %s in, %s out, %.1f s",
                 human(in_bytes), human(out_bytes), took)


class TelnetServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---------------------------------------------------------------------- SSH --
class SshListener:
    """The system's sshd, as this user, with a key of its own.

    No sudo and no new dependency: sshd runs happily on a high port as an ordinary
    user (measured), it needs a host key and an authorized_keys file, and both can
    live in --ssh-dir rather than in ~/.ssh.  The price is that password auth cannot
    work -- a non-root sshd cannot read the shadow file -- so this is a key-auth
    listener, which is what #321 is about anyway.

    The client key it generates is the one the *board* will use, and printing the
    exact ssh command line for it is the whole point of this class.
    """

    def __init__(self, directory, port, user, board_addr):
        self.directory = directory
        self.port = port
        self.user = user
        self.board_addr = board_addr
        self.process = None
        self.hostkey = os.path.join(directory, "hostkey")
        self.clientkey = os.path.join(directory, "clientkey")
        self.authorized = os.path.join(directory, "authorized_keys")
        self.log = os.path.join(directory, "sshd.log")

    def _keygen(self, path):
        if os.path.exists(path):
            return
        subprocess.run(["ssh-keygen", "-t", "ed25519", "-f", path, "-N", "", "-q"],
                       check=True)

    def start(self):
        os.makedirs(self.directory, exist_ok=True)
        self._keygen(self.hostkey)
        self._keygen(self.clientkey)
        with open(self.clientkey + ".pub", "rb") as src:
            public = src.read()
        with open(self.authorized, "wb") as dst:
            dst.write(public)

        sshd = "/usr/sbin/sshd"
        if not os.path.exists(sshd):
            note("-", "ssh: no %s on this machine -- the SSH listener is off", sshd)
            return False

        self.process = subprocess.Popen(
            [sshd, "-D", "-p", str(self.port), "-h", self.hostkey,
             "-o", "PidFile=" + os.path.join(self.directory, "sshd.pid"),
             "-o", "UsePAM=no", "-o", "PasswordAuthentication=no",
             "-o", "StrictModes=no", "-o", "AuthorizedKeysFile=" + self.authorized,
             "-E", self.log],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        if self.process.poll() is not None:
            note("-", "ssh: sshd exited at once -- see %s", self.log)
            return False
        note("-", "ssh: sshd up on port %d (key auth only)", self.port)
        return True

    def command(self):
        """What to run from the board's side, once #321 lands."""
        return ("ssh -i clientkey -p %d -o IdentitiesOnly=yes %s@%s"
                % (self.port, self.user, self.board_addr))

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()


# --------------------------------------------------------------------- main --
def main():
    parser = argparse.ArgumentParser(
        description="The WiFi lab fixture: HTTP, telnet and SSH for the board to talk to.")
    parser.add_argument("image", help="the application .bin (zimodem.ino.bin)")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--telnet-port", type=int, default=2323)
    parser.add_argument("--ssh-port", type=int, default=2222)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--ssh-dir", default=os.path.expanduser("~/.o1next-harness-ssh"),
                        help="where the harness keeps its own host key, client key and "
                             "authorized_keys (nothing in ~/.ssh is touched)")
    parser.add_argument("--no-http", action="store_true")
    parser.add_argument("--no-telnet", action="store_true")
    parser.add_argument("--no-ssh", action="store_true")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        sys.stderr.write("harness-o1next: no such file: %s\n" % args.image)
        return 1

    size = os.path.getsize(args.image)
    name = os.path.basename(args.image)
    hostname = socket.gethostname().split(".")[0]
    interfaces = [(n, a) for n, a in local_ipv4s() if not a.startswith("127.")]
    board_addr = interfaces[0][1] if interfaces else "127.0.0.1"

    note("-", "image    %s", os.path.abspath(args.image))
    note("-", "         %d bytes (%s), sha256 %s", size, human(size), sha256_of(args.image))

    http_server = telnet_server = ssh = None
    threads = []

    if not args.no_http:
        try:
            http_server = HttpServer((args.bind, args.http_port), HttpHandler)
        except OSError as exc:
            sys.stderr.write("harness-o1next: http: %s\n" % exc)
            return 1
        http_server.image_path = os.path.abspath(args.image)
        http_server.image_name = name
        http_server.image_size = size
        http_server.image_sha256 = sha256_of(args.image)
        http_server.telnet_port = args.telnet_port
        http_server.http_host = board_addr
        thread = threading.Thread(target=http_server.serve_forever, daemon=True)
        thread.start()
        threads.append(thread)

    if not args.no_telnet:
        try:
            telnet_server = TelnetServer((args.bind, args.telnet_port), TelnetHandler)
        except OSError as exc:
            sys.stderr.write("harness-o1next: telnet: %s\n" % exc)
            return 1
        telnet_server.hostname = hostname
        telnet_server.stopping = False
        thread = threading.Thread(target=telnet_server.serve_forever, daemon=True)
        thread.start()
        threads.append(thread)

    if not args.no_ssh:
        ssh = SshListener(args.ssh_dir, args.ssh_port, os.environ.get("USER", "user"),
                          board_addr)
        if not ssh.start():
            ssh = None

    # Everything the board's side needs, in one place, per interface -- because there
    # is more than one answer to "the address of this machine".
    for ifname, addr in interfaces:
        note("-", "  %-9s  http://%s:%d/%s", ifname, addr, args.http_port, name)
        note("-", "  %-9s  telnet %s %d", "", addr, args.telnet_port)
    if ssh is not None:
        note("-", "ssh key: %s", ssh.clientkey)
        note("-", "  %-9s  %s", "", ssh.command())
    note("-", "serving (ctrl-c to stop)")
    note("-", "if the board cannot reach this: look for a macOS prompt asking whether %s",
         os.path.basename(sys.executable))
    note("-", "  may accept incoming connections.  An unanswered one hangs the connection")
    note("-", "  with nothing in any log.  `socketfilterfw --listapps` shows the answer.")

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        sys.stderr.write("\n")
    finally:
        if telnet_server is not None:
            telnet_server.stopping = True
            telnet_server.shutdown()
            telnet_server.server_close()
        if http_server is not None:
            http_server.shutdown()
            http_server.server_close()
        if ssh is not None:
            ssh.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
