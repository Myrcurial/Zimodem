#!/usr/bin/env python3
"""Serve an O1NEXT firmware image over HTTP, so the board can fetch it itself.

    tools/ota-server-o1next.py build/o1next-s2/zimodem.ino.bin

WHY THIS EXISTS, AND WHAT IS NOT DONE YET

`tools/update-o1next.py` sends the application image down the console, which works
today and needs no network (#334, item 2, console half).  This is the other half:
serving the same artifact so the *firmware* can pull it over WiFi, with nothing
plugged into the board at all.

The firmware half is not written yet.  What is here is the listener it will talk to,
and it is complete and testable on its own -- with curl, before any of it is trusted
to a board:

    tools/ota-server-o1next.py build/o1next-s2/zimodem.ino.bin &
    curl -sI http://127.0.0.1:8080/zimodem.ino.bin          # size and type
    curl -r 0-1023 http://127.0.0.1:8080/zimodem.ino.bin | wc -c   # a range

WHAT THE FIRMWARE WILL NEED FROM IT, and why each one is here

  - **Content-Length, always.**  `Update.begin(size)` wants the size before a byte
    is written, and it is what refuses an image that cannot fit in the slot.  The
    firmware reads it from here; a chunked response would have nothing to read.
  - **The whole file on one GET.**  No chunked encoding, no compression, no
    redirect: the firmware streams the response body straight into the slot.
  - **A 404 for anything else.**  A typo in a URL should be visible from the board's
    side as a refusal, not as a small file written into an application slot.
  - **Ranges, for us rather than for it.**  A single range is a few lines and makes
    a partial transfer reproducible by hand; a resuming client is not written yet.
  - **A log line per transfer.**  The evidence that it happened, and how fast.

It is stdlib only, on purpose: no dependencies to install to flash a modem.
"""
import argparse
import fcntl
import hashlib
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK = 64 * 1024
SIOCGIFADDR = 0xc0206921        # macOS/BSD; the buffer must be a whole ifreq (32)


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
    """Every interface with an IPv4 address, as (name, address), loopback included.

    Enumerated rather than guessed: the address the board has to be given is the one
    on the interface the board can reach, and on this machine that is not always the
    one a hostname resolves to -- there is no single answer to "what is my IP".
    """
    found = []
    for _, name in socket.if_nameindex():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            res = fcntl.ioctl(sock.fileno(), SIOCGIFADDR,
                              struct.pack("16s16x", name.encode()[:15]))
            found.append((name, socket.inet_ntoa(res[20:24])))
        except OSError:
            pass                        # no IPv4 on this interface, or not an ifreq
        finally:
            sock.close()
    return found


class Handler(BaseHTTPRequestHandler):
    server_version = "o1next-ota-server"
    protocol_version = "HTTP/1.1"       # keep-alive, which needs exact lengths: we have them

    # ------------------------------------------------------------------ plumbing
    def log_message(self, fmt, *args):
        pass                            # this tool prints its own, with sizes in them

    def note(self, fmt, *args):
        sys.stderr.write("%s  %-15s  " % (time.strftime("%H:%M:%S"), self.client_address[0]))
        sys.stderr.write(fmt % args if args else fmt)
        sys.stderr.write("\n")
        sys.stderr.flush()

    def send_head(self, code, length, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()

    # --------------------------------------------------------------------- paths
    def path_of(self):
        return self.path.split("?", 1)[0]

    def do_HEAD(self):
        self.handle_request(body=False)

    def do_GET(self):
        self.handle_request(body=True)

    def handle_request(self, body):
        path = self.path_of()

        if path in ("/", "/index.html"):
            text = ("%s\n%d bytes\nsha256 %s\n\nGET /%s\n"
                    % (self.server.image_name, self.server.image_size,
                       self.server.image_sha256, self.server.image_name)).encode()
            self.send_head(200, len(text), "text/plain; charset=utf-8")
            if body:
                self.wfile.write(text)
            self.note("index")
            return

        if path != "/" + self.server.image_name:
            self.send_head(404, 0, "text/plain; charset=utf-8")
            self.note("404 %s -- the image is at /%s", path, self.server.image_name)
            return

        self.serve_image(body)

    # -------------------------------------------------------------------- ranges
    def range_of(self):
        """(start, end) inclusive, or None for the whole file, or 'bad'."""
        header = self.headers.get("Range")
        if not header or not header.startswith("bytes=") or "," in header:
            return None                 # no range, or several: the whole file is a fine answer
        spec = header[len("bytes="):].strip()
        first, _, last = spec.partition("-")
        try:
            if first == "":             # a suffix: the last N bytes
                n = int(last)
                if n <= 0:
                    return "bad"
                start = max(0, self.server.image_size - n)
                return (start, self.server.image_size - 1)
            start = int(first)
            end = int(last) if last else self.server.image_size - 1
        except ValueError:
            return "bad"
        if start > end or start >= self.server.image_size:
            return "bad"
        return (start, min(end, self.server.image_size - 1))

    # ------------------------------------------------------------------- serving
    def serve_image(self, body):
        wanted = self.range_of()
        if wanted == "bad":
            self.send_head(416, 0, "text/plain; charset=utf-8",
                           {"Content-Range": "bytes */%d" % self.server.image_size})
            self.note("416 unsatisfiable range")
            return

        if wanted is None:
            start, end = 0, self.server.image_size - 1
            code, extra = 200, {}
        else:
            start, end = wanted
            code = 206
            extra = {"Content-Range": "bytes %d-%d/%d"
                     % (start, end, self.server.image_size)}

        length = end - start + 1
        self.send_head(code, length, "application/octet-stream", extra)
        if not body:
            self.note("HEAD %s (%d bytes)", self.server.image_name, length)
            return

        sent = 0
        began = time.time()
        try:
            with open(self.server.image_path, "rb") as fh:
                fh.seek(start)
                left = length
                while left > 0:
                    block = fh.read(min(CHUNK, left))
                    if not block:
                        break
                    self.wfile.write(block)
                    sent += len(block)
                    left -= len(block)
        except (BrokenPipeError, ConnectionResetError):
            took = time.time() - began
            self.note("client went away after %s of %s in %.1f s",
                      human(sent), human(length), took)
            return

        took = max(time.time() - began, 1e-6)
        self.note("%s %s in %.1f s (%s/s)%s",
                  "sent" if code == 200 else "sent range",
                  human(sent), took, human(sent / took),
                  "" if code == 200 else " [partial]")
        if code == 200 and sent == self.server.image_size:
            self.server.completed.set()


def main():
    parser = argparse.ArgumentParser(
        description="Serve a firmware image for the board to fetch over WiFi.")
    parser.add_argument("image", help="the application .bin (zimodem.ino.bin)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="0.0.0.0",
                        help="address to listen on (default: every interface)")
    parser.add_argument("--name", default=None,
                        help="the URL path to serve it at (default: the file's name)")
    parser.add_argument("--once", action="store_true",
                        help="exit once one complete image has been served")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        sys.stderr.write("ota-server-o1next: no such file: %s\n" % args.image)
        return 1

    size = os.path.getsize(args.image)
    name = args.name or os.path.basename(args.image)
    if "/" in name:
        sys.stderr.write("ota-server-o1next: --name is a path segment, not a path\n")
        return 1

    try:
        server = ThreadingHTTPServer((args.bind, args.port), Handler)
    except OSError as exc:
        sys.stderr.write("ota-server-o1next: cannot listen on %s:%d -- %s\n"
                         % (args.bind, args.port, exc))
        return 1

    server.image_path = os.path.abspath(args.image)
    server.image_name = name
    server.image_size = size
    server.image_sha256 = sha256_of(args.image)
    server.completed = threading.Event()

    sys.stderr.write("== image    %s\n" % server.image_path)
    sys.stderr.write("==          %d bytes (%s)\n" % (size, human(size)))
    sys.stderr.write("== sha256   %s\n" % server.image_sha256)
    sys.stderr.write("== listening on %s:%d\n" % (args.bind, args.port))

    # The URL is the one thing the board needs, and there is more than one answer to
    # "the address of this machine": print them all and say which is which.
    for ifname, addr in local_ipv4s():
        if addr.startswith("127."):
            continue
        sys.stderr.write("==   %-5s http://%s:%d/%s\n"
                         % (ifname, addr, args.port, name))
    sys.stderr.write("== give the board the address on the interface it can reach.\n")
    sys.stderr.flush()

    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    if args.once:
        # Wait for one whole image, then stop: the caller wants a result, not a daemon.
        while not server.completed.wait(0.5):
            pass
        time.sleep(0.2)                 # let the last response reach the socket
        sys.stderr.write("== one complete image served; stopping\n")
    else:
        sys.stderr.write("== serving (ctrl-c to stop)\n")
        sys.stderr.flush()
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            sys.stderr.write("\n")

    server.shutdown()
    server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
