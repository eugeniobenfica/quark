"""Runs Quark's tests on Linux machines of other processors: an Alpine Linux VM in QEMU, driven over its serial
console. QK_ARCH picks the processor: aarch64 (the default: ARM64) or x86_64 (System V, as on Linux PCs).

  python tools/vm/vm.py boot        start QEMU, log in, bring up the network, install gcc (minutes)
  python tools/vm/vm.py test        copy the repository in with a quark for the VM (made here: quark build
                                    --target ...-linux --libc musl, by QUARK or build/quark.exe), build it with that
                                    one (build.sh --check: stage1, build/quark.exe, stage3 the same bytes), run
                                    tests/run.sh with build/quark.exe
  python tools/vm/vm.py run "cmd"   run a shell command in the VM, print its output, exit with its status
  python tools/vm/vm.py stop

Needs QEMU (MSYS2: pacman -S mingw-w64-ucrt-x86_64-qemu; Linux, macOS: the qemu package) and, in tools/vm/vm/,
the Alpine "virt" ISO of that processor (alpinelinux.org, Downloads) saved as alpine-aarch64.iso or
alpine-x86_64.iso. QEMU and EDK2_FW may point at the emulator and the ARM64 firmware elsewhere.
"""
import http.server
import io
import os
import re
import socket
import subprocess
import sys
import tarfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
VMDIR = os.path.join(HERE, "vm")
REPO = os.path.dirname(os.path.dirname(HERE))
ARCH = os.environ.get("QK_ARCH", "aarch64")
PORT = 4555 if ARCH == "aarch64" else 4556
HTTP = 8766 if ARCH == "aarch64" else 8767
QEMU = os.environ.get("QEMU", r"C:\msys64\ucrt64\bin\qemu-system-%s.exe" % ARCH)
FW = os.environ.get("EDK2_FW", r"C:\msys64\ucrt64\share\qemu\edk2-aarch64-code.fd")
ALPINE = "v3.24"
MEMORY = os.environ.get("QK_VM_MEMORY", "8192")  # MB: the compiler's --release build of itself takes about 4 GB


def connect(timeout=5.0):
    end = time.time() + timeout
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=2)
            s.settimeout(0.5)
            return s
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.5)


def read_until(s, pattern, timeout):
    buf = b""
    end = time.time() + timeout
    rx = re.compile(pattern.encode())
    while time.time() < end:
        try:
            chunk = s.recv(65536)
            if chunk:
                buf += chunk
                if rx.search(buf):
                    return buf
        except socket.timeout:
            pass
    raise TimeoutError("waiting for %r; got %r" % (pattern, buf[-800:]))


def boot():
    os.makedirs(VMDIR, exist_ok=True)
    log = open(os.path.join(VMDIR, "qemu-%s.log" % ARCH), "wb")
    iso = os.path.join(VMDIR, "alpine-%s.iso" % ARCH)
    if ARCH == "aarch64":
        machine = ["-M", "virt", "-cpu", "cortex-a72", "-bios", FW,
                   "-drive", "file=" + iso + ",if=none,id=cd,media=cdrom,readonly=on",
                   "-device", "virtio-scsi-pci", "-device", "scsi-cd,drive=cd"]
    else:
        machine = ["-M", "q35", "-cpu", "max", "-cdrom", iso, "-boot", "d"]
    args = [QEMU] + machine + ["-smp", "4", "-m", MEMORY,
            "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
            "-display", "none", "-monitor", "none",
            "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % PORT]
    flags = 0x00000008 | 0x00000200 if os.name == "nt" else 0  # detached, a process group of its own
    subprocess.Popen(args, stdout=log, stderr=log, stdin=subprocess.DEVNULL, creationflags=flags,
                     start_new_session=os.name != "nt")
    s = connect(30)
    if ARCH != "aarch64":  # ISOLINUX: the kernel's console on the serial line, not the screen
        read_until(s, r"boot:", 120)
        s.sendall(b"virt console=ttyS0,115200\n")
    read_until(s, r"login:", 900)
    s.sendall(b"root\n")
    read_until(s, r"# ", 60)
    s.sendall(("stty -echo; ip link set eth0 up && udhcpc -i eth0 -q -n >/dev/null 2>&1; "
               "echo https://dl-cdn.alpinelinux.org/alpine/%s/main > /etc/apk/repositories; "
               "echo https://dl-cdn.alpinelinux.org/alpine/%s/community >> /etc/apk/repositories; "
               "apk update -q && apk add -q gcc musl-dev bash diffutils coreutils binutils gdb >/dev/null; "
               "echo SETUP-$?\n" % (ALPINE, ALPINE)).encode())
    out = read_until(s, r"SETUP-\d+", 1800).decode(errors="replace")
    s.close()
    print("ready" if "SETUP-0" in out else out[-400:])


def run(cmd, timeout=3600):
    s = connect(10)
    s.sendall(b"\n")
    time.sleep(0.3)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    s.sendall(("( %s ) 2>&1; echo @@E$?ND@@\n" % cmd).encode())
    out = read_until(s, r"@@E(\d+)ND@@\r?\n", timeout).decode(errors="replace")
    s.close()
    m = list(re.finditer(r"@@E(\d+)ND@@", out))[-1]
    lines = out[:m.start()].replace("\r", "").split("\n")
    if lines and "@@E" in lines[0]:
        lines = lines[1:]  # the command, echoed
    print("\n".join(lines).strip("\n"))
    return int(m.group(1))


def test():
    """The repository into the VM (through a one-request HTTP server) with a quark for it, then the build and every
    test."""
    host = os.environ.get("QUARK", os.path.join(REPO, "build", "quark.exe"))
    boot = os.path.join(REPO, "build", "vm", "quark-%s" % ARCH)
    os.makedirs(os.path.dirname(boot), exist_ok=True)
    target = "arm64-linux" if ARCH == "aarch64" else "x86_64-linux"
    subprocess.check_call([host, "build", os.path.join(REPO, "compiler", "mainlinux.qk"), "--target", target,
                           "--libc", "musl", "-o", boot], stdout=subprocess.DEVNULL)
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w") as t:
        for part in ["build.sh", "include", "compiler", "tools/libquark.sh", "tests", "examples", "bench", "docs", "std"]:
            t.add(os.path.join(REPO, part), arcname=part)
        t.add(boot, arcname="quark-boot")
    body = data.getvalue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", HTTP), Handler)
    threading.Thread(target=server.handle_request, daemon=True).start()
    run("stty -echo; rm -rf /root/q && mkdir -p /root/q && cd /root/q && wget -q -O - http://10.0.2.2:%d/ | tar -x && echo copied" % HTTP, 300)
    server.server_close()
    return run("cd /root/q && chmod 755 quark-boot && QUARK=./quark-boot bash build.sh --check > /root/build.txt 2>&1; s=$?; "
               "grep -E 'error|warning|stage|quark:|fixed|libquark|Killed' "
               "/root/build.txt | head -30; [ $s = 0 ] && sha256sum build/quark.exe build/stage3/quark.exe build/libquark.a && "
               "bash tests/run.sh > /root/out.txt 2>&1; grep -E '^FAIL|passed' /root/out.txt | head -80", 4 * 3600)


def stop():
    if os.name == "nt":
        subprocess.call(["taskkill", "/F", "/IM", "qemu-system-%s.exe" % ARCH], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
    else:
        subprocess.call(["pkill", "-f", "qemu-system-" + ARCH])


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "boot":
        boot()
    elif what == "test":
        sys.exit(test())
    elif what == "run":
        sys.exit(run(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 3600))
    elif what == "stop":
        stop()
    else:
        print(__doc__)
