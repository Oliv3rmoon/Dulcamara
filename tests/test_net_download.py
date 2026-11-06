#!/usr/bin/env python3
# Dulcamara - the update's download (src/net.c's net_download), built and run on the host against a local server.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/net_download_check.c with src/net.c and the host's libcurl (CC names the compiler, default clang),
with a stall limit of 3 seconds in place of NET_STALL_SECONDS, and runs it against a server on 127.0.0.1:

* an update-sized download (75.8 MB, as 0.2.11's PPSA99010.zip) at 2.5 MB/s, about 29 seconds, completes: until 0.2.13
  net_download gave up on any download after 25 seconds all told, and the tester's update ended in curl 28 three times
  in a row ("The download failed") on a phone's hotspot;
* GitHub's redirect from the release's address to the file is followed;
* a download that stalls is given up after the stall limit, with the reason, and what arrived is kept;
* a dropped download goes on from where it stopped (a Range request) and the file comes out whole;
* a server that ignores the Range request sends the whole file again, which is taken from the start;
* a 404 is NET_NOT_FOUND, with the reason, and nothing is written.
"""

import hashlib
import http.server
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-D_POSIX_C_SOURCE=200809L"]
STALL_SECONDS = 3
UPDATE_SIZE = 75_775_079       # 0.2.11's PPSA99010.zip ...
SLOW_RATE = 2_621_440          # ... at 2.5 MiB a second: about 29 seconds, longer than the old 25-second limit
WHOLE = bytes((i * 7 + i // 251) % 256 for i in range(3 * 1024 * 1024))  # 3 MiB, no two halves alike


def pattern(size):
    block = bytes(range(256)) * 1024
    return (block * (size // len(block) + 1))[:size]


SLOW = pattern(UPDATE_SIZE)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    flaky_requests = []

    def log_message(self, *arguments):
        pass

    def headers_for(self, code, length, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(length))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()

    def range_start(self):
        match = re.fullmatch(r"bytes=(\d+)-", self.headers.get("Range", ""))
        return int(match.group(1)) if match else None

    def do_GET(self):
        if self.path == "/slow/PPSA99010.zip":
            self.headers_for(200, len(SLOW))
            chunk = SLOW_RATE // 10
            for at in range(0, len(SLOW), chunk):
                self.wfile.write(SLOW[at:at + chunk])
                time.sleep(0.1)
        elif self.path == "/releases/download/0.2.13/PPSA99010.zip":
            self.send_response(302)
            self.send_header("Location", "/assets/PPSA99010.zip")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/assets/PPSA99010.zip":
            self.headers_for(200, len(WHOLE))
            self.wfile.write(WHOLE)
        elif self.path == "/stall/PPSA99010.zip":
            self.headers_for(200, len(WHOLE))
            self.wfile.write(WHOLE[:262_144])
            self.wfile.flush()
            time.sleep(30)  # nothing more arrives; the client gives up first
        elif self.path == "/flaky/PPSA99010.zip":
            # The first request gets half and the connection drops; the next is answered from its Range.
            start = self.range_start()
            Handler.flaky_requests.append(self.headers.get("Range", ""))
            if start is None:
                self.headers_for(200, len(WHOLE))
                self.wfile.write(WHOLE[:len(WHOLE) // 2])
                self.wfile.flush()
                self.close_connection = True
                self.connection.shutdown(2)
            else:
                self.headers_for(206, len(WHOLE) - start,
                                 [("Content-Range", f"bytes {start}-{len(WHOLE) - 1}/{len(WHOLE)}")])
                self.wfile.write(WHOLE[start:])
        elif self.path == "/norange/PPSA99010.zip":
            self.headers_for(200, len(WHOLE))  # whatever the Range, the whole file
            try:
                self.wfile.write(WHOLE)
            except (ConnectionResetError, BrokenPipeError):
                pass  # a client going on from a .part hangs up on a whole file: it asks again from the start
        else:
            self.headers_for(404, 9)
            self.wfile.write(b"Not Found")


class NetDownloadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            raise unittest.SkipTest(f"needs {cc}")
        cls.directory = tempfile.TemporaryDirectory()
        cls.executable = Path(cls.directory.name) / "net-download-check"
        build = subprocess.run([cc, *FLAGS, "-DNP_HAVE_CURL", f"-DNET_STALL_SECONDS={STALL_SECONDS}",
                                "-I" + str(ROOT / "src"), str(ROOT / "tests/net_download_check.c"),
                                str(ROOT / "src/net.c"), "-lcurl", "-o", str(cls.executable)],
                               capture_output=True, text=True, check=False)
        if build.returncode != 0 and "curl/curl.h" in build.stderr:
            cls.directory.cleanup()
            raise unittest.SkipTest("needs libcurl's headers (libcurl4-openssl-dev)")
        if build.returncode != 0:
            cls.directory.cleanup()
            raise AssertionError(build.stderr)
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.directory.cleanup()

    def start(self, path, name):
        dest = Path(self.directory.name) / name
        process = subprocess.Popen([str(self.executable), self.base + path, str(dest)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        return process, dest

    @staticmethod
    def finish(process, timeout):
        out, err = process.communicate(timeout=timeout)
        line = [l for l in out.splitlines() if l.startswith("result ")]
        assert line, out + err
        words = line[-1].split(" ", 9)
        fields = {"result": int(words[1]), "seconds": float(words[3]), "progress": words[5],
                  "calls": int(words[7]), "why": words[9] if len(words) > 9 else "", "out": out + err[-3000:]}
        return process.returncode, fields

    def run_one(self, path, name, timeout=60):
        return self.finish(self.start(path, name)[0], timeout) + (Path(self.directory.name) / name,)

    def test_downloads(self):
        # The update-sized slow one runs while the others are checked: together they take its 29 seconds.
        slow, slow_dest = self.start("/slow/PPSA99010.zip", "slow.zip")

        code, fields, dest = self.run_one("/releases/download/0.2.13/PPSA99010.zip", "redirected.zip")
        self.assertEqual(code, 0, fields["out"])
        self.assertEqual(dest.read_bytes(), WHOLE)
        self.assertEqual(fields["progress"], f"{len(WHOLE)}/{len(WHOLE)}", fields["out"])

        code, fields, dest = self.run_one("/stall/PPSA99010.zip", "stalled.zip")
        self.assertEqual(fields["result"], 2, fields["out"])  # NET_UNAVAILABLE
        self.assertLess(fields["seconds"], STALL_SECONDS + 10, fields["out"])
        self.assertEqual(fields["why"], f"the connection stalled (under 1 KB/s for {STALL_SECONDS} seconds)",
                         fields["out"])
        self.assertIn("curl 28", fields["out"])
        self.assertFalse(dest.exists())
        # what arrived is kept for the next try
        self.assertEqual(Path(str(dest) + ".part").read_bytes(), WHOLE[:262_144])

        # A dropped connection: half is kept, and the next call asks for the rest and finishes the file.
        Handler.flaky_requests.clear()
        code, fields, dest = self.run_one("/flaky/PPSA99010.zip", "flaky.zip")
        self.assertEqual(fields["result"], 2, fields["out"])
        self.assertEqual(Path(str(dest) + ".part").stat().st_size, len(WHOLE) // 2)
        code, fields, dest = self.run_one("/flaky/PPSA99010.zip", "flaky.zip")
        self.assertEqual(code, 0, fields["out"])
        self.assertEqual(Handler.flaky_requests, ["", f"bytes={len(WHOLE) // 2}-"])
        self.assertIn(f"going on from {len(WHOLE) // 2} bytes", fields["out"])
        self.assertEqual(hashlib.sha256(dest.read_bytes()).hexdigest(), hashlib.sha256(WHOLE).hexdigest())
        self.assertEqual(fields["progress"], f"{len(WHOLE)}/{len(WHOLE)}", fields["out"])
        self.assertFalse(Path(str(dest) + ".part").exists())

        # A server that ignores Range: the .part is dropped and the file taken whole from the start.
        stale = Path(self.directory.name) / "norange.zip.part"
        stale.write_bytes(WHOLE[:1000])
        code, fields, dest = self.run_one("/norange/PPSA99010.zip", "norange.zip")
        self.assertEqual(code, 0, fields["out"])
        self.assertIn("from the start", fields["out"])
        self.assertEqual(dest.read_bytes(), WHOLE)

        code, fields, dest = self.run_one("/releases/download/0.2.13/missing.zip", "missing.zip")
        self.assertEqual(fields["result"], 1, fields["out"])  # NET_NOT_FOUND
        self.assertEqual(fields["why"], "the server answered 404", fields["out"])
        self.assertFalse(dest.exists())
        self.assertFalse(Path(str(dest) + ".part").exists())  # the error page is not kept

        code, fields = self.finish(slow, 180)
        print(f"update-sized download at {SLOW_RATE / 1048576:.1f} MiB/s: {fields['seconds']:.1f} s, "
              f"{fields['calls']} progress calls")
        self.assertEqual(code, 0, fields["out"])
        self.assertGreater(fields["seconds"], 25.0, fields["out"])
        self.assertEqual(slow_dest.stat().st_size, UPDATE_SIZE)
        self.assertEqual(hashlib.sha256(slow_dest.read_bytes()).hexdigest(), hashlib.sha256(SLOW).hexdigest())
        self.assertEqual(fields["progress"], f"{UPDATE_SIZE}/{UPDATE_SIZE}", fields["out"])

    def test_no_limit_on_the_whole_download(self):
        source = (ROOT / "src/net.c").read_text(encoding="utf-8")
        download = source[source.index("NetResult net_download("):source.index("int net_request(")]
        self.assertNotIn("CURLOPT_TIMEOUT", download)
        self.assertIn("CURLOPT_LOW_SPEED_TIME, (long)NET_STALL_SECONDS", download)
        self.assertIn("CURLOPT_RESUME_FROM_LARGE", download)


if __name__ == "__main__":
    unittest.main()
