# Publishing the wish `.deb` to an APT repository

This document is the release runbook for the **`apt`-installable** wish
package. It covers the one-time infrastructure setup and the per-release
steps.

`apt` installs only from an **APT repository** — a directory of `.deb`s plus
a GPG-signed `Packages`/`Release` index served over HTTPS. A bare `.deb`
attached to a GitHub Release (which
[`release-deb.yml`](../.github/workflows/release-deb.yml) also produces) is
not one: users would have to download and `dpkg -i` it by hand and would get
no upgrades. This runbook is about the repo that makes `apt install wish`
work.

For building a `.deb` locally, see
[docs/building.md](building.md#building-a-deb-package). For the downloadable
binary zips, see [docs/publishing-binaries.md](publishing-binaries.md).

## How it works

The repo is a **flat** APT repository (no `dists/` tree — one URL, a
`pool/`, and the index files beside it) published to the repo's **GitHub
Pages** site (`gh-pages` branch), at:

```
https://binary-dice-games.github.io/wish/
  pool/wish_<version>_amd64.deb
  Packages / Packages.gz
  Release / Release.gpg / InRelease
  wish-archive-keyring.asc      <- public half of the signing key
  index.html                    <- the install instructions, for humans
```

On every `release: published`,
[`release-deb.yml`](../.github/workflows/release-deb.yml):

1. **`build-deb`** — builds the amd64 `.deb`
   ([`scripts/package_deb.py`](../scripts/package_deb.py)), attaches it to
   the release, and re-exports it as a workflow artifact.
2. **`publish-apt`** — checks out the current `gh-pages` content (to keep
   older versions in `pool/`), imports the signing key from the
   `APT_GPG_PRIVATE_KEY` secret, runs
   [`scripts/build_apt_repo.py`](../scripts/build_apt_repo.py) to drop the
   new `.deb` into `pool/` and regenerate + sign the index with
   `apt-ftparchive` and `gpg`, then force-pushes the result back to
   `gh-pages` as a single fresh commit.

`publish-apt` is a **no-op with a warning** until `APT_GPG_PRIVATE_KEY` is
set (see one-time setup), so merging this workflow doesn't break releases
before the infrastructure exists.

The package is **amd64 only** — `package_deb.py` builds on `ubuntu-latest`
and `dpkg --print-architecture` there is `amd64`. Adding arm64 later means a
second build on `ubuntu-24.04-arm` and passing both `.deb`s to
`build_apt_repo.py --add` (it already writes `Architectures: amd64` into
`Release`; widen that via `--architectures`).

## One-time setup

### 1. Create a signing key

On a trusted machine (not in CI):

```sh
gpg --quick-generate-key "wish package signing <opensource@binary-dice-games.com>" \
    default sign never
KEYID=$(gpg --list-keys --with-colons | awk -F: '/^fpr:/ {print $10; exit}')

gpg --armor --export-secret-keys "$KEYID" > wish-apt-private.asc   # -> secret
gpg --armor --export "$KEYID"             > wish-apt-public.asc     # published by CI
```

Generate it **without a passphrase** (simplest for CI), or set one and add
it as the `APT_GPG_PASSPHRASE` secret below. Keep `wish-apt-private.asc`
out of the repo; delete it once it's in GitHub secrets.

### 2. Add repository secrets

**Settings → Secrets and variables → Actions → New repository secret:**

| Secret | Value |
|---|---|
| `APT_GPG_PRIVATE_KEY` | full contents of `wish-apt-private.asc` |
| `APT_GPG_PASSPHRASE` | the key passphrase — **only if** you set one |

### 3. Enable GitHub Pages

**Settings → Pages → Build and deployment → Source: Deploy from a branch**,
branch **`gh-pages`**, folder **`/ (root)`**. The `gh-pages` branch doesn't
exist yet — the first `publish-apt` run creates it, after which Pages starts
serving. (Publishing a throwaway pre-release is the easiest way to bootstrap
it; delete the release + tag afterward, the repo content stays.)

The workflow pushes to `gh-pages` with the automatic `GITHUB_TOKEN`
(`permissions: contents: write`, already set) — no PAT or deploy key needed.

## Each release

Nothing extra. Follow the
[binary release steps](publishing-binaries.md#each-release) — tag `v<version>`,
publish the release. `build-deb` and `publish-apt` run automatically. When
`publish-apt` is green, `apt update` on any subscribed machine sees the new
version within a few minutes (GitHub Pages CDN propagation).

Re-running a failed `publish-apt` is safe: it rebuilds the whole index from
the `pool/` in `gh-pages` plus the artifact, so it's idempotent.

## Verify

On a clean amd64 Debian/Ubuntu machine (or container):

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://binary-dice-games.github.io/wish/wish-archive-keyring.asc \
  | sudo tee /etc/apt/keyrings/wish.asc > /dev/null

echo "deb [signed-by=/etc/apt/keyrings/wish.asc] https://binary-dice-games.github.io/wish ./" \
  | sudo tee /etc/apt/sources.list.d/wish.list

sudo apt update
sudo apt install wish
wish --version
```

`apt update` succeeding (no `NO_PUBKEY` / `not signed` error) confirms the
signature chain; `apt install` pulling `wish` confirms the index.

## Build / test the repo locally

`build_apt_repo.py` is the same code CI runs:

```sh
# needs apt-utils (apt-ftparchive) + gnupg
python3 scripts/package_deb.py --version 1.2.3
python3 scripts/build_apt_repo.py \
    --repo-dir /tmp/wish-apt --key-id "$KEYID" --add dist/*.deb
python3 -m http.server -d /tmp/wish-apt 8000 &
# deb [trusted=yes] http://localhost:8000 ./
```

`scripts/test_build_apt_repo.py` builds a throwaway key + fixture `.deb` and
asserts the signed index is internally consistent:

```sh
python3 scripts/test_build_apt_repo.py
```

(Skips itself when `apt-ftparchive` / `dpkg-deb` / `gpg` aren't installed.)

## Gotchas

- **`gh-pages` history is discarded each run.** `publish-apt` force-pushes a
  single fresh commit. That's deliberate (the branch is a generated
  artifact), but it means the `pool/` is only as complete as the last
  checkout — don't hand-delete old `.deb`s from `gh-pages` expecting to
  recover them from history.
- **Flat repo → no suite/component in `sources.list`.** The entry ends in
  `./`. Adding `main`/`stable` there makes `apt` look for a `dists/` tree
  that doesn't exist.
- **`Release` staleness.** No `Valid-Until:` is written, so the index never
  "expires" — fine for this cadence, but it also means a stale mirror won't
  be flagged by `apt`.
- **Key rotation.** Replacing the key changes
  `wish-archive-keyring.asc`; existing users must re-fetch it (their
  `apt update` will fail with a signature error until they do). Announce it
  in the release notes.
- **amd64 only.** An arm64 or i386 machine adding the repo gets
  `apt update` warnings about the missing architecture and can't install
  `wish`. See "How it works" for adding arm64.
