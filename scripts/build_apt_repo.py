#!/usr/bin/env python3
"""Assemble (or refresh) a signed *flat* APT repository from one or more .deb files.

`apt` installs only from an APT repository -- a directory of .debs plus a
GPG-signed `Packages`/`Release` index served over HTTPS. A bare .deb attached
to a GitHub Release is not one. This script takes an existing flat-repo
directory (possibly empty / not yet created), drops new .deb files into its
`pool/`, regenerates the `Packages`(.gz) and `Release` indexes with
`apt-ftparchive`, and signs `Release` into a detached `Release.gpg` plus an
inline `InRelease` with `gpg`.

"Flat" repo layout (everything served relative to one URL):

    <repo>/
      pool/wish_<version>_<arch>.deb
      Packages
      Packages.gz
      Release
      Release.gpg
      InRelease

Consumed by a one-line sources entry (note the trailing `./`, which is what
makes it "flat" -- no dists/ tree, suite or component):

    deb [signed-by=/etc/apt/keyrings/wish.asc] https://<host>/wish ./

Requires `apt-ftparchive` (Debian's `apt-utils`) and `gnupg` on `PATH`.
Linux only. See docs/publishing-apt.md for the GitHub Pages release runbook
and scripts/test_build_apt_repo.py for a self-contained test.

Usage:
    python3 scripts/build_apt_repo.py --repo-dir public --key-id <KEYID> \\
        --add dist/wish_1.2.3_amd64.deb
"""

import argparse
import gzip
import os
import shutil
import subprocess
from pathlib import Path


def _run(cmd, **kwargs):
    print("+ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def refresh_repo(repo_dir, debs, key_id, passphrase=None, *,
                 origin="wish", label="wish", suite="stable",
                 architectures="amd64"):
    """Copy ``debs`` into ``repo_dir/pool`` and (re)build the signed index.

    :param repo_dir:      flat-repo root; created if missing. Any .debs
                          already under ``pool/`` are retained and re-indexed.
    :param debs:          iterable of paths to .deb files to add.
    :param key_id:        GPG key id/fingerprint to sign ``Release`` with
                          (must be present in the current GNUPGHOME).
    :param passphrase:    key passphrase. ``None`` lets gpg prompt via the
                          agent (interactive only); pass ``""`` for a
                          passphrase-less CI key to force loopback and never
                          block on a pinentry.
    :raises FileNotFoundError: if a path in ``debs`` does not exist.
    :raises subprocess.CalledProcessError: if apt-ftparchive or gpg fails.
    """
    repo_dir = Path(repo_dir)
    pool = repo_dir / "pool"
    pool.mkdir(parents=True, exist_ok=True)

    for deb in debs:
        deb = Path(deb)
        if not deb.is_file():
            raise FileNotFoundError(deb)
        shutil.copy2(deb, pool / deb.name)

    # `Filename:` fields come out relative to repo_dir (e.g.
    # "pool/wish_1.2.3_amd64.deb"), which is exactly what a flat repo needs.
    packages = _run(["apt-ftparchive", "packages", "pool"],
                    cwd=repo_dir, capture_output=True, text=True).stdout
    (repo_dir / "Packages").write_text(packages)
    (repo_dir / "Packages.gz").write_bytes(gzip.compress(packages.encode()))

    release = _run(
        ["apt-ftparchive",
         "-o", f"APT::FTPArchive::Release::Origin={origin}",
         "-o", f"APT::FTPArchive::Release::Label={label}",
         "-o", f"APT::FTPArchive::Release::Suite={suite}",
         "-o", f"APT::FTPArchive::Release::Architectures={architectures}",
         "release", "."],
        cwd=repo_dir, capture_output=True, text=True).stdout
    (repo_dir / "Release").write_text(release)

    gpg = ["gpg", "--batch", "--yes", "--armor", "--local-user", key_id]
    if passphrase is not None:
        gpg += ["--pinentry-mode", "loopback", "--passphrase", passphrase]
    for stale in ("Release.gpg", "InRelease"):
        (repo_dir / stale).unlink(missing_ok=True)
    _run(gpg + ["--detach-sign", "--output", "Release.gpg", "Release"],
         cwd=repo_dir)
    _run(gpg + ["--clearsign", "--output", "InRelease", "Release"],
         cwd=repo_dir)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo-dir", required=True,
                        help="flat-repo root (created if missing)")
    parser.add_argument("--key-id", required=True,
                        help="GPG key id/fingerprint to sign Release with")
    parser.add_argument("--add", nargs="+", required=True, metavar="DEB",
                        help="one or more .deb files to add to the repo")
    parser.add_argument("--passphrase", default=os.environ.get("APT_GPG_PASSPHRASE"),
                        help="signing key passphrase (default: $APT_GPG_PASSPHRASE)")
    parser.add_argument("--architectures", default="amd64",
                        help="Architectures: field for Release (default: amd64)")
    args = parser.parse_args()

    refresh_repo(args.repo_dir, args.add, args.key_id,
                 passphrase=args.passphrase, architectures=args.architectures)
    print(f"\napt repo ready: {Path(args.repo_dir).resolve()}")


if __name__ == "__main__":
    main()
