#!/usr/bin/env python3
"""Self-contained test for scripts/build_apt_repo.py.

Generates a throwaway GPG key in a temp GNUPGHOME and a minimal .deb with
`dpkg-deb`, runs ``refresh_repo()``, and asserts the signed flat-repo
metadata is produced and internally consistent (indexes reference the pooled
.deb, the gzip mirror matches, and both signatures verify).

Skips automatically when apt-ftparchive / dpkg-deb / gpg are unavailable
(e.g. on a non-Debian host).

    python3 scripts/test_build_apt_repo.py
"""

import gzip
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_apt_repo import refresh_repo  # noqa: E402

_TOOLS = ("apt-ftparchive", "dpkg-deb", "gpg")


def _make_deb(work_dir, name="wish", version="9.9.9", arch="amd64"):
    root = work_dir / "pkgroot"
    (root / "DEBIAN").mkdir(parents=True)
    (root / "usr/bin").mkdir(parents=True)
    (root / "usr/bin/wish-dummy").write_text("#!/bin/sh\n:\n")
    (root / "DEBIAN/control").write_text(
        f"Package: {name}\n"
        f"Version: {version}\n"
        f"Architecture: {arch}\n"
        f"Maintainer: Test <test@example.invalid>\n"
        f"Description: minimal fixture package for test_build_apt_repo\n")
    deb = work_dir / f"{name}_{version}_{arch}.deb"
    subprocess.run(["dpkg-deb", "--build", "--root-owner-group",
                    str(root), str(deb)],
                   check=True, capture_output=True)
    return deb


@unittest.skipUnless(all(shutil.which(t) for t in _TOOLS),
                     f"needs {', '.join(_TOOLS)} on PATH")
class BuildAptRepoTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

        self._prev_gnupghome = os.environ.get("GNUPGHOME")
        gnupg = self.tmp / "gnupg"
        gnupg.mkdir(mode=0o700)
        os.environ["GNUPGHOME"] = str(gnupg)
        subprocess.run(
            ["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase",
             "", "--quick-generate-key", "Wish APT Test <apt@test.invalid>",
             "default", "default", "never"],
            check=True, capture_output=True)
        colons = subprocess.run(
            ["gpg", "--list-secret-keys", "--with-colons"],
            check=True, capture_output=True, text=True).stdout
        self.key_id = next(ln.split(":")[9] for ln in colons.splitlines()
                           if ln.startswith("fpr:"))

    def tearDown(self):
        if self._prev_gnupghome is None:
            os.environ.pop("GNUPGHOME", None)
        else:
            os.environ["GNUPGHOME"] = self._prev_gnupghome
        self._tmp.cleanup()

    def test_refresh_repo_produces_consistent_signed_index(self):
        repo = self.tmp / "repo"
        deb = _make_deb(self.tmp)

        refresh_repo(repo, [deb], self.key_id, passphrase="")

        self.assertTrue((repo / "pool" / deb.name).is_file())

        packages = (repo / "Packages").read_text()
        self.assertIn("Package: wish", packages)
        self.assertIn(f"Filename: pool/{deb.name}", packages)

        # gzip mirror must match the plaintext index byte-for-byte.
        self.assertEqual(
            gzip.decompress((repo / "Packages.gz").read_bytes()).decode(),
            packages)

        release = (repo / "Release").read_text()
        self.assertIn("Origin: wish", release)
        self.assertIn("SHA256:", release)
        self.assertIn("Packages.gz", release)

        # Both signature forms must verify against the freshly built Release.
        for verify in (["--verify", str(repo / "Release.gpg"),
                        str(repo / "Release")],
                       ["--verify", str(repo / "InRelease")]):
            proc = subprocess.run(["gpg", "--batch", *verify],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_second_call_reindexes_without_dropping_existing_debs(self):
        repo = self.tmp / "repo"
        first = _make_deb(self.tmp / "a", version="1.0.0")
        refresh_repo(repo, [first], self.key_id, passphrase="")

        second = _make_deb(self.tmp / "b", version="2.0.0")
        refresh_repo(repo, [second], self.key_id, passphrase="")

        packages = (repo / "Packages").read_text()
        self.assertIn("Version: 1.0.0", packages)
        self.assertIn("Version: 2.0.0", packages)


if __name__ == "__main__":
    unittest.main()
