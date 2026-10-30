"""Offline regression tests for the standalone host utility."""

import contextlib
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.parse
import urllib.request
import urllib.response
import zipfile
from email.message import Message
from pathlib import Path
from unittest.mock import patch

import m65j


CATALOGUE = "https://example.invalid/catalogue.html"
FILE_ID = "12345678-1234-1234-1234-123456789abc"
ARCHIVE_URL = "https://example.invalid/cores.zip"


def cor_payload(board):
    data = bytearray(256)
    data[:16] = b"MEGA65BITSTREAM0"
    data[0x10:0x1C] = b"Example Core"
    data[0x30:0x34] = b"v1.2"
    data[0x70] = board
    return bytes(data)


def zip_bytes(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    return buffer.getvalue()


class FilehostAuthTests(unittest.TestCase):
    def setUp(self):
        test = self

        class FilehostServer(urllib.request.HTTPSHandler):
            def https_open(self, request):
                test.assertEqual(urllib.parse.urlsplit(request.full_url).hostname, "files.mega65.org")
                headers = Message()
                if request.get_header("X-requested-with") != "XMLHttpRequest":
                    result = {"state": "NOTOK", "message": "Invalid request"}
                elif request.full_url.endswith("/login.php"):
                    fields = urllib.parse.parse_qs(request.data.decode())
                    test.assertEqual(fields, {
                        "logindata[username]": ["test@example.invalid"],
                        "logindata[password]": ["test-password&=+"],
                    })
                    headers["Set-Cookie"] = "PHPSESSID=test-session; Path=/; Secure; HttpOnly"
                    result = test.login_result
                else:
                    test.assertEqual(request.full_url, "https://files.mega65.org/php/readfiledetail.php")
                    test.assertEqual(request.get_header("Cookie"), "PHPSESSID=test-session")
                    test.assertEqual(urllib.parse.parse_qs(request.data.decode()), {"fileid": [FILE_ID]})
                    result = [{"state": "OK", "location": ARCHIVE_URL, "filename": "cores.zip"}]
                response = urllib.response.addinfourl(
                    io.BytesIO(json.dumps(result).encode()), headers, request.full_url, 200,
                )
                response.msg = "OK"
                return response

        self.login_result = {"state": "OK"}
        build_opener = urllib.request.build_opener
        transport = FilehostServer()
        opener = build_opener(transport, urllib.request.ProxyHandler({}))
        patches = [
            patch("urllib.request.build_opener", side_effect=lambda *handlers: build_opener(
                transport, urllib.request.ProxyHandler({}), *handlers,
            )),
            patch("urllib.request.urlopen", side_effect=opener.open),
            patch.object(m65j, "parse_filehost_config", return_value=({
                "filehost_user": "test@example.invalid", "filehost_password": "test-password&=+",
            }, Path("test.config"))),
        ]
        for patcher in patches:
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_configured_login_and_file_details_follow_filehost_protocol(self):
        with contextlib.redirect_stderr(io.StringIO()):
            cookie = m65j.load_cookie(m65j.argparse.Namespace(cookie=None, cookie_file=None))
        self.assertEqual(cookie, "PHPSESSID=test-session")
        candidates, error = m65j.filehost_detail_candidates(FILE_ID, cookie)
        self.assertIsNone(error)
        self.assertEqual([candidate.url for candidate in candidates], [ARCHIVE_URL])

    def test_rejected_login_does_not_return_the_anonymous_session(self):
        self.login_result = {"state": "NOTOK", "message": "Invalid credentials"}
        diagnostics = io.StringIO()
        with contextlib.redirect_stderr(diagnostics):
            cookie = m65j.load_cookie(m65j.argparse.Namespace(cookie=None, cookie_file=None))
        self.assertIsNone(cookie)
        self.assertIn("Invalid credentials", diagnostics.getvalue())
        self.assertNotIn("test-password", diagnostics.getvalue())

    def test_session_cookie_is_scoped_to_filehost_and_not_forwarded_on_redirects(self):
        for url, expected in (
            ("https://files.mega65.org/download.zip", "PHPSESSID=test-session"),
            ("https://www.files.mega65.org/download.zip", "PHPSESSID=test-session"),
            (CATALOGUE, None),
            ("https://files.mega65.org.example.invalid/download.zip", None),
        ):
            for request in (m65j.request(url, "PHPSESSID=test-session"),
                            m65j.request_with_data(url, {}, "PHPSESSID=test-session")):
                with self.subTest(url=url, method=request.get_method()):
                    self.assertEqual(request.get_header("Cookie"), expected)
                    redirected = urllib.request.HTTPRedirectHandler().redirect_request(
                        request, None, 302, "Found", {}, ARCHIVE_URL,
                    )
                    self.assertIsNone(redirected.get_header("Cookie"))


class MirrorTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="m65j-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "mirror"
        self.output.mkdir()
        (self.output / "WWW").mkdir()
        (self.output / "WWW" / "index.html").write_text("Web UI")
        (self.output / "mega65-jtag.cfg.example").write_text("# Example config\n")
        self.extra = self.root / "bonus.bit"
        self.extra.write_bytes(b"extra bitstream")
        self.firmware = self.root / "firmware.uf2"
        self.firmware.write_bytes(b"firmware payload")
        self.theme = self.root / "default.m65jtheme"
        self.theme.write_bytes(b"theme payload")
        self.resources = {
            CATALOGUE: f'<h2>Example Core Stable</h2><a href="https://files.mega65.org/?id={FILE_ID}">R3/R6</a>'.encode(),
            "https://files.mega65.org/php/readfiledetail.php": json.dumps([
                {"state": "OK", "location": ARCHIVE_URL, "filename": "cores.zip"},
            ]).encode(),
            ARCHIVE_URL: zip_bytes({
                "nested.zip": zip_bytes({
                    "core-r3.cor": cor_payload(3),
                    "core-r6.cor": cor_payload(6),
                }),
                "README.txt": b"Not a core",
            }),
        }
        self.requests = []
        self.start_patch("urllib.request.urlopen", side_effect=self.urlopen)
        self.start_patch("read_client_config", return_value={})
        self.start_patch("parse_filehost_config", return_value=({}, None))

    def start_patch(self, name, **kwargs):
        patcher = patch.object(m65j, name, **kwargs) if "." not in name else patch(name, **kwargs)
        value = patcher.start()
        self.addCleanup(patcher.stop)
        return value

    def urlopen(self, request, **kwargs):
        url = request.full_url
        self.requests.append(url)
        # An unexpected URL fails the test instead of reaching the network.
        response = io.BytesIO(self.resources[url])
        response.headers = {}
        response.geturl = lambda: url
        return response

    def run_cli(self, args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return m65j.main([*args, "--cache", str(self.root / "cache"), "--quiet"])

    def mirror(self, *options):
        return self.run_cli([
            "mirror", "stable", str(self.output), CATALOGUE,
            "--board", "all", "--extra-core", str(self.extra),
            "--firmware", str(self.firmware), "--theme", str(self.theme),
            *options,
        ])

    def assert_manifest(self, board):
        path = self.output / f"stable-r{board}.sha256"
        rows = m65j.read_hash_manifest(path)
        self.assertEqual([row[0] for row in rows], ["core", "core", "firmware", "theme"])
        for _kind, payload_hash, transfer_hash, rel in rows:
            transfer = (self.output / rel).read_bytes()
            self.assertEqual(transfer_hash, hashlib.sha256(transfer).hexdigest())
            self.assertEqual(payload_hash, hashlib.sha256(m65j.strip_signature_trailer(transfer)).hexdigest())
        return rows

    def test_mirror_builds_board_specific_manifests_and_sd_bundles(self):
        self.assertEqual(self.mirror(), 0)
        for board in (3, 6):
            self.assertEqual((self.output / f"example-core-r{board}.cor").read_bytes(), cor_payload(board))
            self.assert_manifest(board)
            with zipfile.ZipFile(self.output / f"sdcard-files-r{board}-cores.zip") as archive:
                self.assertEqual(set(archive.namelist()), {
                    "mega65-jtag.cfg", "WWW/index.html", "THEMES/default.m65jtheme",
                    f"example-core-r{board}.cor", f"bonus-r{board}.bit", f"stable-r{board}.sha256",
                })
        with zipfile.ZipFile(self.output / "sdcard-files.zip") as archive:
            self.assertEqual(set(archive.namelist()), {
                "mega65-jtag.cfg", "WWW/index.html", "THEMES/default.m65jtheme",
            })
        self.assertIn("Example Core", (self.output / "index.html").read_text())
        # Reusing the output directory must preserve unchanged core payloads.
        self.assertEqual(self.mirror(), 0)
        self.assert_manifest(6)

    def test_latest_keeps_legacy_options_and_preserved_filenames(self):
        self.assertEqual(self.run_cli([
            "latest", "--board", "r6", "--output", str(self.output),
            "--channel", "nightly", "--source-url", CATALOGUE,
            "--preserve-filenames", "--hash-file", "custom.sha256", "--detail-workers", "1",
        ]), 0)
        self.assertEqual((self.output / "core-r6.cor").read_bytes(), cor_payload(6))
        self.assertFalse((self.output / "core-r3.cor").exists())
        self.assertEqual(m65j.read_hash_manifest(self.output / "custom.sha256")[0][3], "core-r6.cor")

    def test_mirror_dry_run_uses_numeric_board_and_default_channel(self):
        self.assertEqual(self.run_cli([
            "mirror", str(self.output), "--source-url", CATALOGUE,
            "--board", "6", "--dry-run", "--limit", "1",
        ]), 0)
        self.assertEqual(self.requests, [CATALOGUE])
        self.assertFalse((self.root / "cache").exists())

    def test_all_boards_rejects_a_single_hash_filename_before_downloading(self):
        with self.assertRaises(SystemExit) as error:
            self.mirror("--hash-file", "custom.sha256")
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(self.requests, [])

    def test_populate_uploads_core_files_then_manifests(self):
        uploads = self.start_patch("put_file")
        self.assertEqual(self.run_cli([
            "populate", "stable", CATALOGUE, "--board", "all",
            "--staging", str(self.output), "--device", "http://board.invalid",
            "--firmware", str(self.firmware), "--theme", str(self.theme), "--no-bless",
        ]), 0)
        names = [Path(call.args[0]).name for call in uploads.call_args_list]
        self.assertEqual(names, [
            "example-core-r3.cor", "example-core-r6.cor", "stable-r3.sha256", "stable-r6.sha256",
        ])
        for call in uploads.call_args_list:
            self.assertEqual(call.args[1], (self.output / Path(call.args[0]).name).read_bytes())

    def signing_key(self):
        if not shutil.which("openssl"):
            self.skipTest("OpenSSL is required for signing tests")
        key = self.root / "key.pem"
        subprocess.run([
            "openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key),
        ], check=True, capture_output=True)
        return key

    def test_signed_mirror_verifies_cores_artifacts_and_manifests(self):
        key = self.signing_key()
        self.assertEqual(self.mirror("--bless", "--key", str(key)), 0)
        trusted = [("test", m65j.raw_public_key_from_private_key(key))]
        for board in (3, 6):
            rows = self.assert_manifest(board)
            for rel in [row[3] for row in rows] + [f"stable-r{board}.sha256"]:
                path = self.output / rel
                status, detail = m65j.check_signed_bytes(path.read_bytes(), path, trusted)
                self.assertEqual(status, "blessed", detail)
        # The BOOTSEL download remains plain, while the OTA package is signed.
        self.assertEqual((self.output / "mega65-pico-jtag.uf2").read_bytes(), self.firmware.read_bytes())

    def test_copied_script_can_build_and_sign_a_mirror_on_its_own(self):
        key = self.signing_key()
        script = self.root / "standalone.py"
        shutil.copy2(m65j.__file__, script)
        catalogue = self.root / "empty.html"
        catalogue.write_text("<html>No remote cores</html>")
        result = subprocess.run([
            sys.executable, str(script), "mirror", "stable", str(self.output), catalogue.as_uri(),
            "--board", "6", "--extra-core", str(self.extra), "--bless", "--key", str(key),
            "--cookie", "offline", "--cache", str(self.root / "cache"), "--quiet",
        ], cwd=self.root, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path = self.output / "bonus-r6.bit"
        trusted = [("test", m65j.raw_public_key_from_private_key(key))]
        self.assertEqual(m65j.check_signed_bytes(path.read_bytes(), path, trusted)[0], "blessed")


if __name__ == "__main__":
    unittest.main()
