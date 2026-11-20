#!/usr/bin/env bash
# Publish the pacman repo in <repo-dir> to the rolling GitHub release that
# hosts it (tag ${KWE_REPO_TAG:-repo}, a prerelease never marked latest).
#
#   release-asset.sh <repo-dir> <new-file>...
#
# <new-file>: the rebuilt packages and their .sig files, uploaded as-is. The
# repo metadata (kineticwe.db/.files and their .tar.gz and .sig, the ABI
# manifest, the public key) is taken from <repo-dir>, as written by
# ci/repo.sh and ci/build-package.sh. Packages carried over from the published
# repo are already release assets and are not re-uploaded.
#
# Order matters: pacman reads the database first and then downloads the files
# it names, so the live database must never reference a missing file:
#   1. new packages + signatures are uploaded first,
#   2. then each metadata file is swapped in (upload as <name>.new, delete the
#      old <name>, rename <name>.new -> <name>); the gap is two API calls,
#   3. only then are old package builds pruned, never one the new database
#      still lists.
#
# Needs gh (github-cli), GH_TOKEN (contents: write), GITHUB_REPOSITORY and
# GITHUB_SHA. KEEP_BUILDS (default 4) builds of each package stay downloadable.
set -euo pipefail

die() { echo "release-asset: $*" >&2; exit 1; }

repo_dir="${1:?usage: release-asset.sh <repo-dir> <new-file>...}"
shift
[[ -d "$repo_dir" ]] || die "$repo_dir is not a directory"
: "${GH_TOKEN:?GH_TOKEN not set}" "${GITHUB_REPOSITORY:?}" "${GITHUB_SHA:?}"
export GH_REPO="$GITHUB_REPOSITORY"
tag="${KWE_REPO_TAG:-repo}"
keep="${KEEP_BUILDS:-4}"
server_url="${GITHUB_SERVER_URL:-https://github.com}"
KWE_REPO_URL="${KWE_REPO_URL:-$server_url/$GITHUB_REPOSITORY/releases/download/$tag}"
keyring_dir="$(cd "$(dirname "$0")/../arch/kineticwe-keyring" && pwd)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# 1. The release must exist before anything can be uploaded to it.
if ! gh release view "$tag" >/dev/null 2>&1; then
    echo ">> Creating release $tag"
    gh release create "$tag" --prerelease --latest=false --target "$GITHUB_SHA" \
        --title "KineticWE pacman repo" \
        --notes "KineticWE pacman repo. This release is updated automatically by CI; do not delete it."
fi
release_id=$(gh api "repos/$GH_REPO/releases/tags/$tag" --jq .id)
[[ -n "$release_id" ]] || die "cannot read the id of release $tag"

# id <TAB> name <TAB> created_at of every asset of the release.
list_assets() {
    gh api --paginate "repos/$GH_REPO/releases/$release_id/assets?per_page=100" \
        --jq '.[] | [.id,.name,.created_at] | @tsv'
}
# Asset id of the exact name in the listing on stdin (empty if absent). Reads
# all input (no early exit), so a piped list_assets never dies of SIGPIPE.
asset_id() { awk -F '\t' -v n="$1" '$2 == n && !f { print $1; f = 1 }'; }
delete_asset() { gh api -X DELETE "repos/$GH_REPO/releases/assets/$1" >/dev/null; }

# 2. GitHub silently renames ':' in asset names (build-package.sh already
# replaced it); a renamed file would not match the database's %FILENAME%.
for f in "$@"; do
    [[ -f "$f" ]] || die "no such file: $f"
    [[ "$(basename -- "$f")" != *:* ]] || die "asset name contains ':': $(basename -- "$f")"
done

uploaded=()
# 3. New packages and signatures. --clobber: a rebuild with an unchanged
# filename (e.g. the keyring) replaces the old asset.
if (($#)); then
    echo ">> Uploading $# new file(s)"
    gh release upload "$tag" "$@" --clobber
    for f in "$@"; do uploaded+=("$(basename -- "$f")"); done
fi

# 4. Metadata swap. kineticwe.db/.files are symlinks to the .tar.gz: upload
# real copies (cp -L). kineticwe-abi.txt matters too: abi-watch treats a 404
# as "no manifest" and triggers a full rebuild, so it must never be missing.
meta=(kineticwe.db.tar.gz kineticwe.db.tar.gz.sig kineticwe.db kineticwe.db.sig
    kineticwe.files.tar.gz kineticwe.files.tar.gz.sig kineticwe.files kineticwe.files.sig
    kineticwe-abi.txt kineticwe.gpg)
for name in "${meta[@]}"; do
    [[ -e "$repo_dir/$name" ]] || continue
    echo ">> Swapping in $name"
    cp -L -- "$repo_dir/$name" "$work/$name.new"
    stale=$(list_assets | asset_id "$name.new")
    [[ -z "$stale" ]] || delete_asset "$stale"
    gh release upload "$tag" "$work/$name.new"
    assets=$(list_assets)
    new_id=$(asset_id "$name.new" <<<"$assets")
    old_id=$(asset_id "$name" <<<"$assets")
    [[ -n "$new_id" ]] || die "uploaded $name.new but cannot find it"
    [[ -z "$old_id" ]] || delete_asset "$old_id"
    gh api -X PATCH "repos/$GH_REPO/releases/assets/$new_id" -f name="$name" >/dev/null
    uploaded+=("$name")
done

# 5. Every file must be served under exactly the name the database uses.
assets=$(list_assets)
for name in "${uploaded[@]}"; do
    [[ -n "$(asset_id "$name" <<<"$assets")" ]] ||
        die "GitHub renamed or dropped $name (not in the release assets)"
done

# 6. Prune: keep the newest $keep builds of each package (by upload time).
# pkgname = filename minus -<pkgver>-<pkgrel>-<arch>.pkg.tar.zst (neither
# pkgver nor pkgrel may contain '-'). Never delete a file the new database
# lists, whatever its age.
declare -A in_db=()
if [[ -f "$repo_dir/kineticwe.db.tar.gz" ]]; then
    while IFS= read -r f; do
        [[ -z "$f" ]] || in_db[$f]=1
    done < <(bsdtar -xOf "$repo_dir/kineticwe.db.tar.gz" '*/desc' |
        awk '$0 == "%FILENAME%" { getline; print }')
fi
declare -A seen=()
while IFS=$'\t' read -r id name created; do
    pkg=$(sed -E 's/-[^-]+-[^-]+-[^-]+\.pkg\.tar\.zst$//' <<<"$name")
    seen[$pkg]=$(( ${seen[$pkg]:-0} + 1 ))
    ((${seen[$pkg]} > keep)) || continue
    if [[ -n "${in_db[$name]:-}" ]]; then
        echo ">> Keeping $name (listed in the database)"
        continue
    fi
    echo ">> Pruning old build: $name ($created)"
    delete_asset "$id"
    sig_id=$(asset_id "$name.sig" <<<"$assets")
    [[ -z "$sig_id" ]] || delete_asset "$sig_id"
done < <(awk -F '\t' '$2 ~ /\.pkg\.tar\.zst$/' <<<"$assets" | sort -t $'\t' -k3,3r)

# 7. Install instructions as the release notes. Quoted heredoc: the backticks
# and $ stay literal; placeholders are filled in by sed.
fpr=$(cut -d ':' -f1 "$keyring_dir/kineticwe-trusted" | head -1)
issues_url="$server_url/$GITHUB_REPOSITORY/issues"
cat > "$work/notes.md" <<'MD'
# KineticWE pacman repo

This release is the KineticWE pacman repository for Arch Linux. It is updated
automatically by CI; do not delete it.

Packages are signed with key `@FPR@` (check it against the project README).
Download it, check the fingerprint, and trust it once:

```sh
curl -fLO @REPO_URL@/kineticwe.gpg
gpg --show-keys kineticwe.gpg          # must show @FPR@
sudo pacman-key --add kineticwe.gpg
sudo pacman-key --lsign-key @FPR@
```

If this download is unreachable, use a keyserver instead of the first three lines:
`sudo pacman-key --recv-keys @FPR@ --keyserver hkps://keys.openpgp.org`

Add this to `/etc/pacman.conf`:

```ini
[kineticwe]
Server = @REPO_URL@
```

Then: `sudo pacman -Syu kineticwe-keyring kineticwe` (pulls in
`xdg-desktop-portal-kwe` and `kineticwe-noctalia`).

Optional greeter: `sudo pacman -S kineticwe-greeter`, then run
`kineticwe-setup-greeter`.

If `pacman -Syu` reports `breaks dependency 'qt6-base=…'`, a rebuild for the
new Qt is on its way. Please wait at least 12 hours before opening an
[issue](@ISSUES_URL@), and don't force it with `-dd` or `--ignore`.
MD
sed -i -e "s|@FPR@|$fpr|g" -e "s|@REPO_URL@|$KWE_REPO_URL|g" \
    -e "s|@ISSUES_URL@|$issues_url|g" "$work/notes.md"
gh release edit "$tag" --notes-file "$work/notes.md" >/dev/null
echo ">> Published to $KWE_REPO_URL"
