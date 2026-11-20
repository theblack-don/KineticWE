#!/usr/bin/env bash
# Download the newest PUBLISHED build of each named package (plus its detached
# .sig) from the pacman repo at <server> into <dest>.
#
#   fetch-published.sh <repo-server-url> <dest-dir> <pkgname>...
#
# Used by partial CI rebuilds (KWE_BUILD): every package that was NOT rebuilt
# is carried over from the currently published repo, so ci/repo.sh (which
# rebuilds the database from the files in one directory) and
# ci/abi-manifest.sh see the whole repo, not just the rebuilt packages.
# The db is parsed like ci/smoke.sh does (%NAME% / %VERSION% / %FILENAME% of
# each */desc, newest per name by vercmp).
#
# Exits non-zero if a package is missing from the repo database or its
# download fails. A missing .sig only warns (the repo may be published
# unsigned); when signing is active, ci/repo.sh rejects the unsigned package.
set -euo pipefail

die() { echo "fetch-published: $*" >&2; exit 1; }

server="${1:?usage: fetch-published.sh <repo-server-url> <dest-dir> <pkgname>...}"   # e.g. https://github.com/<owner>/<repo>/releases/download/repo
dest="${2:?usage: fetch-published.sh <repo-server-url> <dest-dir> <pkgname>...}"
shift 2
(($# > 0)) || die "no packages given"

mkdir -p "$dest"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo ">> Fetching repo database from $server"
curl -sfL "$server/kineticwe.db.tar.gz" -o "$work/db.tar.gz" ||
    die "cannot download $server/kineticwe.db.tar.gz"

# Map each db entry to its %NAME% / %VERSION% / %FILENAME%. %FILENAME% is used
# as-is for the download and the local file name.
field() { awk -v f="%$1%" '$0 == f { getline; print; exit }' <<<"$desc"; }
declare -A latest_file=() latest_ver=()
while IFS= read -r entry; do
    desc="$(bsdtar -xOf "$work/db.tar.gz" "$entry")"
    name="$(field NAME)"
    ver="$(field VERSION)"
    file="$(field FILENAME)"
    [[ -n "$name" && -n "$file" ]] || continue
    if [[ -z "${latest_ver[$name]:-}" ]] || (( $(vercmp "$ver" "${latest_ver[$name]}") > 0 )); then
        latest_ver[$name]="$ver"
        latest_file[$name]="$file"
    fi
done < <(bsdtar -tf "$work/db.tar.gz" | grep '/desc$')

for name in "$@"; do
    file="${latest_file[$name]:-}"
    [[ -n "$file" ]] || die "package $name missing from the repo database"
    echo ">> Carrying over published $name ${latest_ver[$name]}: $file"
    curl -sfL "$server/$file" -o "$dest/$file" ||
        die "download failed: $server/$file"
    if ! curl -sfL "$server/$file.sig" -o "$dest/$file.sig"; then
        rm -f -- "$dest/$file.sig"
        echo "fetch-published: WARNING: no signature for $file (unsigned repo?)" >&2
    fi
done
