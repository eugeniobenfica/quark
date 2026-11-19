"""Copies a folder into the Linux VM (tools/vm/vm.py, QK_ARCH picks which) and runs a command there in it.

  python tools/vm/push.py DIR /root/where "command"     the folder's files into /root/where, then the command in it
"""
import http.server
import io
import os
import sys
import tarfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vm  # noqa: E402


def push(folder, where, cmd):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w") as t:
        t.add(folder, arcname=".")
    body = data.getvalue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", vm.HTTP), Handler)
    threading.Thread(target=server.handle_request, daemon=True).start()
    vm.run("stty -echo; rm -rf %s && mkdir -p %s && cd %s && wget -q -O - http://10.0.2.2:%d/ | tar -x && echo copied"
           % (where, where, where, vm.HTTP), 600)
    server.server_close()
    return vm.run("cd %s && %s" % (where, cmd), 7200)


if __name__ == "__main__":
    sys.exit(push(sys.argv[1], sys.argv[2], sys.argv[3]))
