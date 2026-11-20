#!/usr/bin/env bash
# Build the KineticWE binary pacman packages (and, for release builds, sign
# and publish them to the GitHub Releases pacman repo).
#
#   build-package.sh
#
# Run as root inside an archlinux:base-devel container, with the repo checked
# out at $GITHUB_WORKSPACE (.github/workflows/arch-repo.yml does this).
#
# Modes:
#   KWE_PUBLISH unset ... PR / test build: builds ALL split packages, nothing
#                         is signed, carried over or published. The packages
#                         are left in $GITHUB_WORKSPACE/out/x86_64 for the
#                         workflow to upload as an artifact.
#   KWE_PUBLISH=1 ....... release build: race guard, signing (ci/sign.sh),
#                         carry-over of packages not rebuilt
#                         (ci/fetch-published.sh), repo database (ci/repo.sh),
#                         ABI manifest (ci/abi-manifest.sh) and upload to the
#                         rolling release (ci/release-asset.sh).
#
# Partial rebuilds: KWE_BUILD is a comma-separated subset of the four split
# packages (e.g. "kineticwe-noctalia,kineticwe-greeter"); unset/empty builds
# all four, and tag pushes (without KWE_REBUILD=1) always build all four. The
# PKGBUILD only builds/packages the listed ones. The repo database must list
# every package, so each package NOT rebuilt is carried over from the currently
# published repo (with its .sig) before the database is rebuilt. Race guard:
# if the Qt / KF6 versions the published build was made against
# (kineticwe-abi.txt "version" lines) differ from this container's, kineticwe
# and xdg-desktop-portal-kwe are added to the set. kineticwe-keyring is always
# rebuilt (cheap, arch=any).
#
# Other inputs: KWE_REBUILD=1 (ABI/manual rebuild, gets a pkgrel suffix even on
# a tag), KWE_REBUILD_REASON, KWE_REPO_URL (default: this repository's
# releases/download/$KWE_REPO_TAG), KWE_SIGNING_KEY / KWE_REQUIRE_SIGNING
# (ci/sign.sh), GH_TOKEN (ci/release-asset.sh), CCACHE_MAXSIZE.
set -euo pipefail

die() { echo "build-package: $*" >&2; exit 1; }

cd "${GITHUB_WORKSPACE:?GITHUB_WORKSPACE not set}"
publish=false
[[ "${KWE_PUBLISH:-}" != 1 ]] || publish=true
all="kineticwe xdg-desktop-portal-kwe kineticwe-noctalia kineticwe-greeter"
if $publish; then
    : "${GITHUB_SERVER_URL:?}" "${GITHUB_REPOSITORY:?}"
    KWE_REPO_URL="${KWE_REPO_URL:-$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/releases/download/${KWE_REPO_TAG:-repo}}"
fi

# Fresh keyring first (avoids the classic expired-key CI failure), then full
# system upgrade (Arch never supports partial upgrades). A cheap no-op if the
# workflow already did it. Fresh containers have no local secret key; without
# the --init the keyring post-install hook prints a scary (but harmless)
# signing error.
pacman-key --init 2>/dev/null || true
pacman -Sy --noconfirm archlinux-keyring
pacman -Su --noconfirm
tools=(pacman-contrib ccache git)
! $publish || tools+=(github-cli)
pacman -S --noconfirm --needed "${tools[@]}"

# No -debug split packages: 200+ MB each, useless in a binary repo.
echo 'OPTIONS+=( !debug )' >> /etc/makepkg.conf
# ccache between runs: recurring (ABI-refresh) builds drop to ~20 min. The
# GitHub cache is limited to paths inside the workspace (and 10 GB per repo),
# so keep ccache and pacman's CacheDir there.
echo 'BUILDENV+=( ccache )' >> /etc/makepkg.conf
export CCACHE_DIR="$GITHUB_WORKSPACE/.ccache"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
mkdir -p "$CCACHE_DIR"
chmod 777 "$CCACHE_DIR"
sed -i "s|^#CacheDir.*|CacheDir = $GITHUB_WORKSPACE/.pkg-cache/|" /etc/pacman.conf

# Build the spec's version and pinned commits even if the committed PKGBUILD
# lags behind lgl-kineticwe-copr/kineticwe.spec.
arch/sync-from-spec.sh PKGBUILD

# ABI refreshes between releases get a CI-only pkgrel suffix so pacman offers
# them as upgrades; the next source release (e.g. 16) sorts higher. Triggered
# rebuilds may run on a release tag, so KWE_REBUILD=1 counts too.
# YYYYMMDDHHMM: two rebuilds in a day stay ordered, and it sorts above the
# older .YYYYMMDD suffixes.
if [[ "${GITHUB_REF_TYPE:-}" != tag || "${KWE_REBUILD:-}" == 1 ]]; then
    echo "Rebuild reason: ${KWE_REBUILD_REASON:-manual}"
    sed -i "s/^pkgrel=.*/&.$(date +%Y%m%d%H%M)/" PKGBUILD
    grep '^pkgrel' PKGBUILD
fi

# Install every build dependency the PKGBUILD declares that exists in the Arch
# repos. Runtime depends live inside the package_*() functions, so only the
# top-level makedepends (which cover the whole build) show up here; anything
# not in the Arch repos is reported and skipped.
mapfile -t deps < <(bash -c 'source PKGBUILD; printf "%s\n" "${depends[@]}" "${makedepends[@]}"' |
    sed 's/[<>=].*$//' | sort -u)
todo=()
for d in "${deps[@]}"; do
    if pacman -Si "$d" &>/dev/null; then
        todo+=("$d")
    else
        echo "Skipping (not in Arch repos): $d"
    fi
done
pacman -S --noconfirm --needed "${todo[@]}"

# Build set (see top). Release builds: validate KWE_BUILD, then the race
# guard. This runs after the system upgrade and dependency install, so
# pacman -Q reports the versions this build will actually compile against.
# The published manifest's "version <pkg> <ver>" lines carry the upstream
# version (pkgrel dropped); compare against the same.
if $publish; then
    if [[ "${GITHUB_REF_TYPE:-}" == tag && "${KWE_REBUILD:-}" != 1 ]]; then
        KWE_BUILD=""   # release build: everything
    fi
    want=" "
    for p in $(tr ',' ' ' <<<"${KWE_BUILD:-}"); do
        case " $all " in
            *" $p "*) want+="$p " ;;
            *) die "KWE_BUILD: unknown package '$p' (valid: $all)" ;;
        esac
    done
    [[ "$want" != " " ]] || want=" $all "
    published_abi="$(mktemp)"
    if curl -sfL "$KWE_REPO_URL/kineticwe-abi.txt" -o "$published_abi"; then
        while read -r kind pkg ver _; do
            [[ "$kind" == version ]] || continue
            cur=$(pacman -Q "$pkg" 2>/dev/null | awk '{print $2}')
            cur="${cur%-*}"
            if [[ "$cur" != "$ver" ]]; then
                echo "Race guard: $pkg is ${cur:-not installed} here, published build used $ver"
                want+="kineticwe xdg-desktop-portal-kwe "
            fi
        done < "$published_abi"
    else
        # Also the very first run: no release yet, so nothing to carry over.
        echo "No published kineticwe-abi.txt; building every package"
        want=" $all "
    fi
    rm -f -- "$published_abi"
    # canonical order, no duplicates
    KWE_BUILD=""
    for p in $all; do
        case "$want" in *" $p "*) KWE_BUILD+="${KWE_BUILD:+,}$p" ;; esac
    done
    echo "Building: $KWE_BUILD (+ kineticwe-keyring)"
else
    KWE_BUILD=""
    echo "PR/test build (KWE_PUBLISH unset): building every package; nothing is signed or published"
fi

# makepkg refuses to run as root; runuser is in util-linux (base).
id builder &>/dev/null || useradd -m builder
# Build this exact CI checkout (not the PKGBUILD's pinned _commit): the
# PKGBUILD picks up KWE_LOCAL_TARBALL, whose single top-level dir must be
# kineticwe-$pkgver/.
git config --global --add safe.directory "$GITHUB_WORKSPACE"
pkgver=$(bash -c 'source PKGBUILD; echo "$pkgver"')
git archive --format=tar.gz --prefix="kineticwe-$pkgver/" -o /home/builder/kineticwe-src.tar.gz HEAD
cp PKGBUILD kineticwe.install kineticwe-greeter.install /home/builder/
chown -R builder:builder /home/builder
# KWE_ABI_PIN=1: pin the exact qt6-base version (see PKGBUILD _kwe_qt_pin).
# KWE_REPO_BUILD=1: kineticwe depends on kineticwe-keyring (built below).
# KWE_BUILD: only build/package the split packages in the build set.
runuser -u builder -- env KWE_ABI_PIN=1 KWE_REPO_BUILD=1 KWE_BUILD="$KWE_BUILD" \
    KWE_LOCAL_TARBALL=/home/builder/kineticwe-src.tar.gz \
    bash -c 'cd /home/builder && makepkg -f --nodeps --noconfirm'
# Repo keyring package (arch=any, no dependencies).
cp -r arch/kineticwe-keyring /home/builder/keyring
chown -R builder:builder /home/builder/keyring
runuser -u builder -- bash -c 'cd /home/builder/keyring && makepkg -f --nodeps --noconfirm'

out="$GITHUB_WORKSPACE/out/x86_64"
rm -rf -- "$out"
mkdir -p "$out"
# the rebuilt split packages (!debug above keeps -debug packages out)
cp /home/builder/*.pkg.tar.zst /home/builder/keyring/*.pkg.tar.zst "$out/"

# epoch=2 puts a ':' in every filename (kineticwe-2:6.7.80-16-...). GitHub
# renames ':' in release asset names, while pacman downloads exactly the
# %FILENAME% repo-add records, so rename BEFORE signing and repo-add: the
# database then names the file as it is actually served. pacman verifies the
# package by its signature/checksum, not by its filename.
for f in "$out"/*.pkg.tar.zst; do
    b="$(basename -- "$f")"
    [[ "$b" != *:* ]] || mv -- "$f" "$out/${b//:/.}"
done
rebuilt=("$out"/*.pkg.tar.zst)
((${#rebuilt[@]})) || die "makepkg produced no packages"

if ! $publish; then
    echo "Built packages (not published):"
    ls -l -- "${rebuilt[@]}"
    exit 0
fi

# A rebuilt file whose exact name is already published (always the keyring,
# whose version rarely changes; everything on a re-run of the same tag) is the
# same package version, but not the same bytes. Uploading it would replace the
# served file while the live database still carries the old checksum, so a
# user syncing in between would get "package is corrupted". Keep the served
# copy (and its .sig) instead: the new database then describes exactly the
# bytes GitHub serves, and nothing is overwritten. Exception: an unsigned
# published copy while this build signs (the switch to signing) is replaced
# once, or ci/repo.sh would reject it.
new=()
for f in "${rebuilt[@]}"; do
    b="$(basename -- "$f")"
    rm -f -- "$f.published" "$f.published.sig"
    if curl -sfL "$KWE_REPO_URL/$b" -o "$f.published"; then
        curl -sfL "$KWE_REPO_URL/$b.sig" -o "$f.published.sig" || rm -f -- "$f.published.sig"
        if [[ -n "${KWE_SIGNING_KEY:-}" && ! -f "$f.published.sig" ]]; then
            echo "Published $b is unsigned; replacing it with the signed rebuild"
            rm -f -- "$f.published"
            new+=("$f")
            continue
        fi
        echo "Already published, keeping the served copy: $b"
        mv -- "$f.published" "$f"
        if [[ -f "$f.published.sig" ]]; then
            mv -- "$f.published.sig" "$f.sig"
        fi
    else
        rm -f -- "$f.published"
        new+=("$f")
    fi
done
if ((${#new[@]} == 0)); then
    echo "Every built package is already published; nothing to do"
    exit 0
fi
rebuilt=("${new[@]}")

# Sign the rebuilt packages (unsigned without KWE_SIGNING_KEY, see
# ci/sign.sh); carried-over packages keep their published .sig. repo.sh then
# checks each signature and signs the database.
GNUPGHOME="$(mktemp -d)"
export GNUPGHOME
KWE_SIGN_FPR=$(ci/sign.sh arch/kineticwe-keyring "${rebuilt[@]}")

# Carry over the packages not in the build set from the published repo, so
# ci/repo.sh and ci/abi-manifest.sh see the whole repo. Empty when the race
# guard found no published manifest (first run): it forced every package.
carry=()
for p in $all; do
    case ",$KWE_BUILD," in *",$p,"*) ;; *) carry+=("$p") ;; esac
done
if ((${#carry[@]})); then
    ci/fetch-published.sh "$KWE_REPO_URL" "$out" "${carry[@]}"
fi
GPGKEY="$KWE_SIGN_FPR" ci/repo.sh "$out"
# Qt / KF6 versions and linked sonames of every package in the repo (rebuilt
# and carried over), read by abi-watch and the race guard.
ci/abi-manifest.sh "$out"/*.pkg.tar.zst > "$out/kineticwe-abi.txt"
cat "$out/kineticwe-abi.txt"
cp arch/kineticwe-keyring/kineticwe.gpg "$out/"

# Upload only what is new (rebuilt packages and their signatures); carried
# packages are already release assets. release-asset.sh swaps in the
# metadata (db, files, abi manifest, key) from $out itself.
upload=()
for f in "${rebuilt[@]}"; do
    upload+=("$f")
    [[ ! -f "$f.sig" ]] || upload+=("$f.sig")
done
ci/release-asset.sh "$out" "${upload[@]}"
