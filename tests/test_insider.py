#!/usr/bin/env python3
# Dulcamara - insider builds (src/insider_check.c, src/insider.c and their hooks in src/update.c), built and run on the
# host.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds and runs, under the address and undefined-behaviour sanitizers (CC names the compiler, default clang):

* tests/insider_check.c with src/insider_check.c: the code's normalisation, the request's JSON, every kind of answer
  (update, none, not_qualified, invalid_code, expired_code, other HTTP statuses, malformed and cut-short JSON, a
  download address on another host, a bad SHA-256 or size, an older version), the update check's first step with a
  request of the test's own (expired code, no network and HTTP 5xx go to GitHub; an update is offered), the
  installer's size and SHA-256 checks on a tampered file, and np_log (src/util.c) keeping a download token out of the
  log;
* tests/insider_net_check.c with src/net.c and the host's libcurl, against a server on 127.0.0.1: the request as the
  service gets it, and an expired code, a 503, an answer that isn't JSON, a server that never answers and one that
  isn't there all going to GitHub, the last two within the request's limit;
* tests/update_insider_check.c with src/update.c itself (its threads on tests/host-sdl, the little of SDL2 it uses)
  and INSIDER_URL that server: the insider
  build offered as a test build, a stale .part of another build refused by the SHA-256 check and the try after it
  starting clean, a tampered download refused and the next try taking the right one, a download of the wrong size
  refused, a download the service refuses (403: the member's code removed) not installed, the offer withdrawn once
  Test builds is off, and the code and the token nowhere in the log.

The test service answers as docs/API.md has it: 415 for a request that isn't application/json and 403 for one with a
web page's Origin or Sec-Fetch-Site, which the app never sends.

Also checks that INSIDER_URL is empty unless the build gives one, and that make INSIDER_URL=... reaches the compiler
as a C string through tools/build.sh."""

import hashlib
import http.server
import io
import json
import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-D_DEFAULT_SOURCE"]
PURE = ["src/insider_check.c", "src/update_release.c", "examples/self-update/self_update_sha256.c", "src/util.c"]
MINIZ = ["miniz", "miniz_tdef", "miniz_tinfl", "miniz_zip"]
CODE = "ABCDEFGH"
TOKEN = "eyJvIjoiYnVpbGRzL2R1bGNhbWFyYSJ9.c2lnbmF0dXJlLW9mLXRoZS10b2tlbg-_x"
HINT = "Insider builds: get a code in the nightly Discord, discord.gg/pSqgRU3YP9"


def np_version():
    return re.search(r'#define\s+NP_VERSION\s+"([^"]+)"', (ROOT / "src/nightshade.h").read_text()).group(1)


def next_version(version):
    major, minor, patch = (int(p) for p in version.split("."))
    return f"{major}.{minor}.{patch + 1}"


def the_zip():
    # A real ZIP with no PPSA99010 folder in it: an install that gets as far as unpacking it writes nothing.
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("README.txt", "not an app\n" * 4000)
        archive.writestr("notes/0.txt", bytes(range(256)) * 300)
    return data.getvalue()


ZIP = the_zip()
TAMPERED = ZIP[:len(ZIP) // 3] + bytes([ZIP[len(ZIP) // 3] ^ 0xFF]) + ZIP[len(ZIP) // 3 + 1:]
JUNK = bytes((i * 31 + 7) % 256 for i in range(len(ZIP) // 2))  # half of "another build"
NEWER = next_version(np_version())


def compiler():
    cc = os.environ.get("CC", "clang")
    if not shutil.which(cc):
        raise unittest.SkipTest(f"needs {cc}")
    return cc


class InsiderCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = compiler()
        cls.directory = tempfile.TemporaryDirectory()
        cls.executable = Path(cls.directory.name) / "insider-check"
        subprocess.run([cc, *FLAGS, "-Wpedantic", "-I" + str(ROOT / "src"), str(ROOT / "tests/insider_check.c"),
                        *[str(ROOT / s) for s in PURE], "-o", str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_the_code_the_request_and_the_answers(self):
        with tempfile.TemporaryDirectory() as scratch:
            result = subprocess.run([str(self.executable), scratch], capture_output=True, text=True, timeout=120,
                                    check=False)
        print(result.stdout.strip().splitlines()[-1])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)  # the log it read back, printed

    def test_the_request_is_json(self):
        for installed, code in (("0.2.13", CODE), ('1"2\\3', 'A"B'), ("tab\there\nline", "\x01\x1f"),
                                ("café 日本", "</script>")):
            result = subprocess.run([str(self.executable), "json", installed, code], capture_output=True, text=True,
                                    check=True)
            body = json.loads(result.stdout)
            self.assertEqual(body, {"app": "dulcamara", "installed_version": installed, "code": code})
        result = subprocess.run([str(self.executable), "json", "0.2.13", CODE], capture_output=True, text=True,
                                check=True)
        self.assertEqual(result.stdout.strip(),
                         '{"app":"dulcamara","installed_version":"0.2.13","code":"ABCDEFGH"}')


class Service(http.server.BaseHTTPRequestHandler):
    """nightly's insider service as docs/API.md has it, one kind of answer per path: <base>/v1/updates and
    <base>/v1/download/<token>."""
    protocol_version = "HTTP/1.1"
    mode = "good"           # /upd: good, tampered-once or wrong-size
    requests = []           # (path, headers, body) of each POST
    downloads = []          # (path, Range) of each GET
    port = 0

    def log_message(self, *arguments):
        pass

    def answer(self, code, body, content_type="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def update(self, base, size=None, url=None):
        return json.dumps({
            "status": "update", "app": "dulcamara",
            "build": {"version": NEWER, "tag": NEWER, "title": f"Dulcamara {NEWER} (insider build)",
                      "file": "PPSA99010.zip", "size": len(ZIP) if size is None else size,
                      "sha256": hashlib.sha256(ZIP).hexdigest(), "notes": "Fixes.\n\"url\": \"https://evil/\"",
                      "published_at": "2026-11-08T05:00:00Z", "public_after": "2026-11-15T05:00:00Z",
                      "url": url or f"http://127.0.0.1:{Service.port}{base}/v1/download/{TOKEN}",
                      "url_expires_at": "2026-11-08T06:00:00Z"}})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        Service.requests.append((self.path, dict(self.headers), body))
        base = self.path[:-len("/v1/updates")] if self.path.endswith("/v1/updates") else None
        # docs/API.md: JSON only, and never a web page's request
        if self.headers.get("Content-Type") != "application/json":
            self.answer(415, '{"status":"bad_request"}')
        elif self.headers.get("Origin") is not None or self.headers.get("Sec-Fetch-Site", "none") != "none":
            self.answer(403, '{"status":"forbidden"}')
        elif base in ("/good", "/upd"):
            self.answer(200, self.update(base, size=len(ZIP) + 1 if Service.mode == "wrong-size" else None))
        elif base == "/expired":
            self.answer(200, '{"status":"expired_code","hint":"' + HINT + '"}')
        elif base == "/notqualified":
            self.answer(200, '{"status":"not_qualified"}')
        elif base == "/none":
            self.answer(200, '{"status":"none"}')
        elif base == "/unavailable":
            self.answer(503, '{"status":"unavailable"}')
        elif base == "/garbage":
            self.answer(200, "<html>a captive portal</html>", "text/html")
        elif base == "/wronghost":
            self.answer(200, self.update(base, url=f"http://127.0.0.2:{Service.port}/wronghost/v1/download/x"))
        elif base == "/hang":
            time.sleep(20)  # nothing comes back: the client gives up first
            self.answer(200, '{"status":"none"}')
        else:
            self.answer(404, '{"status":"unknown_app"}')

    def do_GET(self):
        Service.downloads.append((self.path, self.headers.get("Range", "")))
        if not self.path.endswith("/v1/download/" + TOKEN) or Service.mode == "revoked":
            # an altered or expired token, or one whose member's code was removed since
            self.answer(403, '{"status":"expired_or_invalid"}')
            return
        data = ZIP
        if Service.mode == "tampered-once" and len([d for d in Service.downloads if "/v1/download/" in d[0]]) == 1:
            data = TAMPERED
        match = re.fullmatch(r"bytes=(\d+)-", self.headers.get("Range", ""))
        if match:
            start = int(match.group(1))
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
            self.send_header("Content-Length", str(len(data) - start))
            self.send_header("Content-Type", "application/zip")
            self.end_headers()
            self.wfile.write(data[start:])
        else:
            self.answer(200, data, "application/zip")


class InsiderNetworkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = compiler()
        cls.directory = tempfile.TemporaryDirectory()
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Service)
        cls.server.daemon_threads = True
        Service.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{Service.port}"
        cls.net_check = Path(cls.directory.name) / "insider-net-check"
        build = subprocess.run([cc, *FLAGS, "-DNP_HAVE_CURL", "-I" + str(ROOT / "src"),
                                str(ROOT / "tests/insider_net_check.c"), str(ROOT / "src/net.c"),
                                *[str(ROOT / s) for s in PURE], "-lcurl", "-o", str(cls.net_check)],
                               capture_output=True, text=True, check=False)
        if build.returncode != 0:
            cls.tearDownClass()
            if "curl/curl.h" in build.stderr:
                raise unittest.SkipTest("needs libcurl's headers (libcurl4-openssl-dev)")
            raise AssertionError(build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.directory.cleanup()

    def ask(self, base, seconds=12, code=CODE):
        result = subprocess.run([str(self.net_check), base, code, str(seconds)], capture_output=True, text=True,
                                timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        line = [l for l in result.stdout.splitlines() if l.startswith("next ")][-1].split()
        fields = dict(zip(line[0::2], line[1::2]))
        fields["out"] = result.stdout + result.stderr[-3000:]
        return fields

    def test_the_request_and_where_each_answer_goes(self):
        Service.requests.clear()
        got = self.ask(self.base + "/good", code="abcd-efgh")
        self.assertEqual((got["next"], got["status"], got["http"], got["version"]), ("offer", "update", "200", NEWER),
                         got["out"])
        path, headers, body = Service.requests[-1]
        self.assertEqual(path, "/good/v1/updates")
        self.assertEqual(json.loads(body), {"app": "dulcamara", "installed_version": np_version(), "code": CODE})
        self.assertEqual(headers.get("Content-Type"), "application/json")
        # no web page's headers: the service refuses a request with them (403)
        self.assertNotIn("Origin", headers)
        self.assertFalse([h for h in headers if h.lower().startswith("sec-fetch-")], headers)
        self.assertEqual(headers.get("User-Agent"), "Dulcamara/" + np_version())
        self.assertLessEqual(len(body), 1024)

        # Anything but an update: GitHub, exactly as before.
        for base, status, http in (("/expired", "expired_code", "200"), ("/notqualified", "not_qualified", "200"),
                                   ("/none", "none", "200"), ("/unavailable", "http_error", "503"),
                                   ("/garbage", "malformed", "200"), ("/wronghost", "bad_build", "200"),
                                   ("/nothing-here", "http_error", "404")):
            got = self.ask(self.base + base)
            self.assertEqual((got["next"], got["status"], got["http"]), ("github", status, http), got["out"])

        # Offline: nothing listens on the port.
        free = socket.socket()
        free.bind(("127.0.0.1", 0))
        closed = free.getsockname()[1]
        free.close()
        got = self.ask(f"http://127.0.0.1:{closed}")
        self.assertEqual((got["next"], got["status"], got["http"]), ("github", "offline", "-1"), got["out"])

        # A service that never answers holds the check up for the request's limit at most.
        got = self.ask(self.base + "/hang", seconds=2)
        self.assertEqual((got["next"], got["status"], got["http"]), ("github", "offline", "-1"), got["out"])
        self.assertLess(float(got["seconds"]), 6.0, got["out"])

        # No code: nothing is asked at all.
        before = len(Service.requests)
        got = self.ask(self.base + "/good", code="")
        self.assertEqual((got["next"], got["status"]), ("github", "off"), got["out"])
        self.assertEqual(len(Service.requests), before)


class UpdateWithInsiderBuildTest(unittest.TestCase):
    """src/update.c's own check and install, with INSIDER_URL the test's server."""

    @classmethod
    def setUpClass(cls):
        cc = compiler()
        cls.directory = tempfile.TemporaryDirectory()
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Service)
        cls.server.daemon_threads = True
        Service.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.insider_url = f"http://127.0.0.1:{Service.port}/upd"
        build = Path(cls.directory.name)
        objects = []
        for name in MINIZ:
            obj = build / f"{name}.o"
            subprocess.run([cc, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-w", "-DNDEBUG",
                            "-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES", "-c", str(ROOT / f"third_party/miniz/{name}.c"),
                            "-o", str(obj)], check=True)
            objects.append(str(obj))
        cls.executable = build / "update-insider-check"
        result = subprocess.run([cc, *FLAGS, "-DNP_HAVE_CURL", f'-DINSIDER_URL="{cls.insider_url}"',
                                 "-I" + str(ROOT / "tests/host-sdl"), "-I" + str(ROOT / "src"),
                                 str(ROOT / "tests/update_insider_check.c"), str(ROOT / "src/update.c"),
                                 str(ROOT / "src/insider.c"), str(ROOT / "src/net.c"), str(ROOT / "src/i18n.c"),
                                 *[str(ROOT / s) for s in PURE], *objects, "-lcurl", "-pthread",
                                 "-o", str(cls.executable)], capture_output=True, text=True, check=False)
        if result.returncode != 0:
            cls.tearDownClass()
            if "curl/curl.h" in result.stderr:
                raise unittest.SkipTest("needs libcurl's headers (libcurl4-openssl-dev)")
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.directory.cleanup()

    def run_update(self, mode, installs, stale_part=None):
        Service.mode = mode
        Service.requests.clear()
        Service.downloads.clear()
        data = Path(tempfile.mkdtemp(dir=self.directory.name))
        if stale_part is not None:
            (data / "cache").mkdir()
            (data / "cache" / f"update-{NEWER}.zip.part").write_bytes(stale_part)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        result = subprocess.run([str(self.executable), str(data), str(installs)], capture_output=True, text=True,
                                timeout=180, check=False, env=env)
        out = result.stdout + result.stderr[-4000:]
        self.assertEqual(result.returncode, 0, out)
        logs = data / "logs"
        log = "".join(p.read_text(errors="replace") for p in sorted(logs.glob("*.log")))
        lines = result.stdout.splitlines()
        return data, lines, log, out

    def offered(self, lines, out):
        self.assertIn(f"check available version {NEWER} size {len(ZIP)} test_build 1", lines, out)

    def private(self, data, log, out):
        # the code is in its file, and nowhere in the log; nor is the download address's token
        self.assertEqual((data / "insider-code").read_text(), CODE + "\n")
        self.assertNotIn(CODE, log)
        self.assertNotIn(TOKEN, log + out)
        self.assertNotIn("c2lnbmF0dXJl", log + out)

    def test_a_stale_part_of_another_build_is_refused_and_the_next_try_starts_clean(self):
        # A .part of the same version's name with another build's bytes: gone on from (a Range request), the
        # finished file fails its SHA-256, and the file and its .part are gone, so "Try again" downloads it whole.
        data, lines, log, out = self.run_update("good", 2, stale_part=JUNK)
        self.offered(lines, out)
        self.assertIn("install 1 failed retry 1 message The download doesn't match its SHA256SUMS", lines, out)
        self.assertEqual(lines[lines.index("install 1 failed retry 1 message The download doesn't match its "
                                           "SHA256SUMS") + 1], "cache", out)
        # past its checks: unpacking the test's ZIP, which holds no app, is what fails
        self.assertIn("install 2 failed retry 1 message Couldn't write the new files (is /data unlocked?)", lines, out)
        self.assertEqual(lines[-1], "cache", out)
        downloads = [d for d in Service.downloads if "/v1/download/" in d[0]]
        self.assertEqual([r for _, r in downloads], [f"bytes={len(JUNK)}-", ""], out)
        self.assertIn(f"going on from {len(JUNK)} bytes", log)  # net.c names the address there: without its token
        self.assertIn("/upd/v1/download/...", log)
        self.assertIn("update: insider status=update http=200", log)
        self.assertIn(f"update: downloading 127.0.0.1:{Service.port}, PPSA99010.zip", log)
        self.private(data, log, out)
        # what the service was sent: the code and the version, nothing else
        path, headers, body = Service.requests[0]
        self.assertEqual(path, "/upd/v1/updates")
        self.assertEqual(json.loads(body), {"app": "dulcamara", "installed_version": np_version(), "code": CODE})
        self.assertEqual(headers.get("User-Agent"), "Dulcamara/" + np_version())
        # the hint: Test builds on and no code; a code, and none
        self.assertIn("enabled 1 hint before 1", lines, out)
        self.assertIn("code 0 has 1 hint 0", lines, out)

    def test_a_tampered_download_is_refused(self):
        data, lines, log, out = self.run_update("tampered-once", 2)
        self.offered(lines, out)
        self.assertIn("install 1 failed retry 1 message The download doesn't match its SHA256SUMS", lines, out)
        self.assertIn("install 2 failed retry 1 message Couldn't write the new files (is /data unlocked?)", lines, out)
        self.assertEqual([l for l in lines if l.startswith("cache")], ["cache", "cache"], out)
        self.assertEqual(len([d for d in Service.downloads if "/v1/download/" in d[0]]), 2)
        self.private(data, log, out)

    def test_a_download_of_another_size_is_refused(self):
        data, lines, log, out = self.run_update("wrong-size", 1)
        self.assertIn(f"check available version {NEWER} size {len(ZIP) + 1} test_build 1", lines, out)
        self.assertIn("install 1 failed retry 1 message The download doesn't match its SHA256SUMS", lines, out)
        self.assertIn(f"the insider build isn't the {len(ZIP) + 1} bytes its answer gave", log)
        self.assertEqual(lines[-1], "cache", out)
        self.private(data, log, out)

    def test_a_download_the_service_refuses_is_not_installed(self):
        # The download address answers 403 once the member's code is removed (or it ran out): a failed download,
        # nothing unpacked, nothing kept, and trying again changes nothing.
        data, lines, log, out = self.run_update("revoked", 2)
        self.offered(lines, out)
        self.assertIn("install 1 failed retry 1 message The download failed: the server answered 403", lines, out)
        self.assertIn("install 2 failed retry 1 message The download failed: the server answered 403", lines, out)
        self.assertEqual([l for l in lines if l.startswith("cache")], ["cache", "cache"], out)
        self.assertNotIn("downloaded in", log)
        self.private(data, log, out)

    def test_the_offer_goes_once_test_builds_is_off(self):
        data, lines, log, out = self.run_update("good", 0)
        self.offered(lines, out)
        self.assertIn("after test builds off none", lines, out)
        self.assertEqual(Service.downloads, [])


class InsiderSourceTest(unittest.TestCase):
    def test_no_insider_builds_unless_the_build_gives_the_address(self):
        header = (ROOT / "src/insider.h").read_text(encoding="utf-8")
        self.assertRegex(header, r'#ifndef INSIDER_URL\n#define INSIDER_URL ""\n#endif')
        self.assertIn("return INSIDER_URL[0] != '\\0';", (ROOT / "src/insider.c").read_text(encoding="utf-8"))
        timeout = int(re.search(r"#define INSIDER_TIMEOUT_SECONDS (\d+)", header).group(1))
        self.assertLessEqual(timeout, 15)
        insider = (ROOT / "src/insider.c").read_text(encoding="utf-8")
        self.assertIn('net_request_within(url, json, "application/json", NP_NAME "/" NP_VERSION, '
                      'INSIDER_TIMEOUT_SECONDS', insider)

    def test_the_update_asks_the_service_first_and_checks_what_it_downloads(self):
        update = (ROOT / "src/update.c").read_text(encoding="utf-8")
        check = re.search(r"static int check_thread\(void \*test_builds_on\)\n\{.*?\n\}", update, re.S).group(0)
        hook = check.index("insider_first(test_builds, version, sizeof(version), zip_url, sizeof(zip_url), "
                           "zip_sha256, &zip_size)")
        self.assertLess(hook, check.index("net_request(UPDATE_LIST_URL"))
        self.assertLess(check.index("version[0] = zip_url[0] = zip_sha256[0] = '\\0';"), hook)
        self.assertRegex(check[hook:], r"^[^}]*\{\n\s+test_build = true;\n\s+SDL_AtomicSet\(&state, "
                                       r"UPDATE_AVAILABLE\);\n\s+return 0;")
        install = re.search(r"static int install_thread\(void \*unused\)\n\{.*?\n\}", update, re.S).group(0)
        # the size the answer gave, then the SHA-256, before anything is unpacked (tests/insider_check.c makes the
        # same two checks on a tampered file)
        self.assertIn("if (!insider_size_ok(zip_url, zip, zip_size) || !release_file_sha256(zip, got) || "
                      "strcmp(got, zip_sha256) != 0)", install)
        self.assertLess(install.index("release_file_sha256(zip, got)"), install.index("mz_zip_reader_init_file"))
        # the download address is logged through insider_loggable: never an insider token
        self.assertIn("insider_loggable(zip_url, shown, sizeof(shown))", install)
        for call in re.findall(r"np_log\((.*?)\);", update, re.S):
            self.assertNotRegex(call, r",\s*zip_url\s*[,)]", call)

    def test_the_code_is_never_logged(self):
        for name in ("src/insider.c", "src/ui/insider_prompt.c", "src/insider_check.c"):
            source = (ROOT / name).read_text(encoding="utf-8")
            for call in re.findall(r"np_log\((.*?)\);", source, re.S):
                arguments = re.sub(r'"(?:[^"\\]|\\.)*"', '""', call)  # what it logs, not the words of its format
                self.assertNotRegex(arguments, r"\b(code|typed|normal|text|json)\b", f"{name}: np_log({call})")

    def test_settings_asks_for_the_code_on_dulcamaras_keyboard(self):
        settings = (ROOT / "src/ui/settings.c").read_text(encoding="utf-8")
        system = re.search(r"static const Row SYSTEM\[\] = \{(.*?)\n\};", settings, re.S).group(1)
        rows = re.findall(r'\{"([^"]+)"', system)
        self.assertEqual(rows[rows.index("Test builds") + 1], "Insider code")
        self.assertRegex(system, r"K_ACTION, 0, NULL, 0, NULL, A_INSIDER_CODE, -1\}")
        # only in a build with the service
        self.assertIn("(t->rows[i].action != A_INSIDER_CODE || insider_enabled())", settings)
        # Test builds switched on with no code: asked for
        self.assertRegex(settings, r"if \(r->field == FIELD\(update_test_builds\) && \*b && insider_enabled\(\) && "
                                   r"!insider_has_code\(\)\)\n\s+insider_prompt_open\(\);")
        self.assertIn("case A_INSIDER_CODE: insider_prompt_open(); break;", settings)
        self.assertIn('(insider_has_code() ? tr("Set") : tr("Not set"))', settings)
        self.assertIn(f'tr("{HINT}")', settings)
        self.assertIn("insider_hint()", settings)
        self.assertIn("if (insider_prompt_input(pressed))", settings)
        self.assertIn("insider_prompt_draw();", settings)
        prompt = (ROOT / "src/ui/insider_prompt.c").read_text(encoding="utf-8")
        self.assertIn("kb_button(&kb, BUTTONS[i], &character)", prompt)  # Dulcamara's own keys (keyboard.h)
        self.assertIn("insider_set_code(typed)", prompt)
        self.assertNotRegex(prompt + settings, r"sceIme|ImeDialog")  # never the console's keyboard
        for text in ("Insider code", "Insider code saved.", "No insider code.", HINT):
            self.assertTrue(all(ord(c) < 0x80 for c in text))

    def test_make_gives_the_compiler_the_address_as_a_c_string(self):
        build = (ROOT / "tools/build.sh").read_text(encoding="utf-8")
        block = re.search(r"\ninsider_definitions=\(\)\n.*?\nfi\n", build, re.S).group(0)
        self.assertIn('args+=(${insider_definitions[@]+"${insider_definitions[@]}"})', build)
        self.assertIn("export INSIDER_URL", (ROOT / "Makefile").read_text(encoding="utf-8"))
        cc = compiler()
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / "probe.c"
            probe.write_text('#include "insider.h"\nconst char *insider_url = INSIDER_URL;\n')
            # as tools/ninja-build.sh runs a compile: each argument through printf %q, then sh -c
            script = ("set -euo pipefail\n" + block +
                      'printf -v command "%q " "$CC" -E -P -I"$SRC" '
                      '${insider_definitions[@]+"${insider_definitions[@]}"} "$PROBE"\nsh -c "$command"\n')
            for url, want in (("", '""'), ("https://nightly-insider.example.workers.dev",
                                           '"https://nightly-insider.example.workers.dev"'),
                              ("https://insider.example:8443/base", '"https://insider.example:8443/base"')):
                env = dict(os.environ, CC=cc, SRC=str(ROOT / "src"), PROBE=str(probe), INSIDER_URL=url)
                result = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"const char *insider_url = {want};", result.stdout)
            for url in ("http://nightly-insider.example.workers.dev", "https://nightly-insider.example.workers.dev/",
                        'https://x.example";int x;"', "https://x.example/a b", "https://x.example?y=1",
                        "https://x.example/$(id)"):
                env = dict(os.environ, CC=cc, SRC=str(ROOT / "src"), PROBE=str(probe), INSIDER_URL=url)
                result = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env, check=False)
                self.assertEqual(result.returncode, 2, url)
                self.assertIn("invalid INSIDER_URL", result.stderr)


if __name__ == "__main__":
    unittest.main()
