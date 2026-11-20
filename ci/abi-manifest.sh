#!/usr/bin/env bash
# Write the ABI manifest of a set of built packages to stdout. Run in the build
# container right after makepkg, while the exact build dependencies are still
# installed. ci/abi-watch.sh compares the published manifest with current Arch
# to decide whether the repo needs a rebuild.
#
# Format (one record per line, space separated, '#' lines are comments):
#   ref <git ref built>                      tag or branch the rebuild targets
#   commit <sha>
#   version <pkg> <upstream version>         Tier 1: private-API dependencies,
#                                            any version change needs a rebuild
#   soname <pkg> <full version> <path> <users>
#                                            Tier 2: every external library the
#                                            packages link; a rebuild is needed
#                                            only if <path> leaves <pkg>, and
#                                            only of <users>: the comma-separated,
#                                            sorted pkgnames (from .PKGINFO) whose
#                                            ELF files link it (v2; v1 manifests
#                                            have no <users> field)
#
# Tier 1 is qt6-base (QtCore/QtGui/QtPrintSupport private API and QPA in the
# compositor and portal) plus the KF6 frameworks whose private headers the
# compositor includes. Versions drop pkgrel, matching the PKGBUILD's Qt pin.
#
# Pass every package the repo publishes (rebuilt and carried over alike), so
# the manifest describes the whole repo. Packages without ELF files (e.g. the
# keyring) simply contribute nothing.
set -euo pipefail

(($# > 0)) || { echo "usage: abi-manifest.sh <pkg.tar.zst>..." >&2; exit 1; }

tier1=(qt6-base kwindowsystem kidletime)
# Toolchain libraries keep their sonames; never worth a rebuild.
skip_owners=(glibc gcc-libs libgcc libstdc++)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Each package into its own directory, so every soname can be attributed to
# the packages that link it.
roots=()
names=()
i=0
for pkg in "$@"; do
    name=$(bsdtar -xOf "$pkg" .PKGINFO | awk -F ' = ' '$1 == "pkgname" { print $2; exit }')
    [[ -n "$name" ]] || { echo "abi-manifest: no pkgname in $pkg" >&2; exit 1; }
    root="$work/root$((i++))"
    mkdir "$root"
    bsdtar -xf "$pkg" -C "$root" --exclude '.PKGINFO' --exclude '.BUILDINFO' \
        --exclude '.MTREE' --exclude '.INSTALL'
    roots+=("$root")
    names+=("$name")
done

# Libraries shipped by any of our own packages are not external.
declare -A own=()
while IFS= read -r f; do
    own[$(basename -- "$f")]=1
done < <(find "${roots[@]}" -name '*.so*' \( -type f -o -type l \))

# DT_NEEDED of every ELF file in the packages: soname -> " pkgname..." (may
# repeat; deduplicated on output).
declare -A needed=()
for i in "${!roots[@]}"; do
    while IFS= read -r f; do
        [[ "$(head -c4 -- "$f" | tr -d '\0')" == $'\x7fELF' ]] || continue
        while IFS= read -r so; do
            needed[$so]+=" ${names[i]}"
        done < <(readelf -d -- "$f" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
    done < <(find "${roots[i]}" -type f)
done

upstream_version() {
    local v
    v=$(pacman -Q "$1" | awk '{print $2}')
    printf '%s\n' "${v%-*}"
}

echo "# kineticwe ABI manifest v2, generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "ref ${GITHUB_REF_NAME:?GITHUB_REF_NAME not set}"
echo "commit ${GITHUB_SHA:?GITHUB_SHA not set}"
for pkg in "${tier1[@]}"; do
    echo "version $pkg $(upstream_version "$pkg")"
done

fail=0
for so in $(printf '%s\n' "${!needed[@]}" | sort); do
    [[ -z "${own[$so]:-}" ]] || continue
    path=/usr/lib/$so
    if [[ ! -e "$path" ]]; then
        path=$(ldconfig -p | awk -v s="$so" '$1 == s { print $NF; exit }')
    fi
    if [[ -z "$path" || ! -e "$path" ]]; then
        echo "abi-manifest: cannot resolve $so" >&2
        fail=1
        continue
    fi
    # Canonical directory (ldconfig may report /lib64 etc.), keeping the
    # soname symlink itself: Arch file lists contain the symlink path.
    path="$(readlink -f -- "$(dirname -- "$path")")/$so"
    if ! owner=$(pacman -Qoq -- "$path" 2>/dev/null); then
        echo "abi-manifest: no package owns $path ($so)" >&2
        fail=1
        continue
    fi
    if printf '%s\n' "${skip_owners[@]}" | grep -qxF -- "$owner"; then
        continue
    fi
    # shellcheck disable=SC2086  # word-split the space-separated pkgnames
    users=$(printf '%s\n' ${needed[$so]} | sort -u | paste -sd, -)
    echo "soname $owner $(pacman -Q "$owner" | awk '{print $2}') ${path#/} $users"
done
exit "$fail"
