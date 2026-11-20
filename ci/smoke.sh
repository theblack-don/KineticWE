#!/usr/bin/env bash
# Verify the packages published in the pacman repo still link against the
# CURRENT Arch repositories. Run inside a fully updated Arch container.
# The repo holds the four split packages built from one PKGBUILD; the newest
# build of each is downloaded and extracted into one combined root.
# Exits non-zero (and prints the broken binaries) if any package is missing
# from the repo or any shipped ELF has an unresolvable library.
set -euo pipefail

server="${1:?usage: smoke.sh <repo-server-url>}"   # e.g. https://github.com/<owner>/<repo>/releases/download/repo
# Trusted key comes from the checkout, not from the server being checked.
keyring_dir="$(cd "$(dirname "$0")/../arch/kineticwe-keyring" && pwd)"
work="$(mktemp -d)"
cd "$work"

pacman -S --noconfirm --needed libarchive curl >/dev/null

echo ">> Fetching repo database from $server"
curl -sfL "$server/kineticwe.db.tar.gz" -o db.tar.gz

pkgnames=(kineticwe xdg-desktop-portal-kwe kineticwe-noctalia kineticwe-greeter kineticwe-keyring)

# Map each db entry to its %NAME% / %VERSION% / %FILENAME%. %FILENAME% is used
# as-is for the download (epoch colons etc. are already encoded there).
field() { awk -v f="%$1%" '$0 == f { getline; print; exit }' <<<"$desc"; }
declare -A latest_file=() latest_ver=()
while IFS= read -r entry; do
    desc="$(bsdtar -xOf db.tar.gz "$entry")"
    name="$(field NAME)"
    ver="$(field VERSION)"
    file="$(field FILENAME)"
    [[ -n "$name" && -n "$file" ]] || continue
    if [[ -z "${latest_ver[$name]:-}" ]] || (( $(vercmp "$ver" "${latest_ver[$name]}") > 0 )); then
        latest_ver[$name]="$ver"
        latest_file[$name]="$file"
    fi
done < <(bsdtar -tf db.tar.gz | grep '/desc$')

mkdir root pkgs
for name in "${pkgnames[@]}"; do
    file="${latest_file[$name]:-}"
    if [[ -z "$file" ]]; then
        echo "FAIL: package $name missing from the repo database"
        exit 1
    fi
    echo ">> Latest published $name: $file"
    curl -sfL "$server/$file" -o "pkgs/$name.pkg.tar.zst"
    bsdtar -xf "pkgs/$name.pkg.tar.zst" -C root
done

# Signatures: once the repo is signed (kineticwe.db.sig published), the
# database and every package must verify against the keyring in the checkout.
# KWE_REQUIRE_SIGNING=1 also fails an unsigned repo.
export GNUPGHOME="$work/gnupg"
mkdir -m 700 "$GNUPGHOME"
if curl -sfL "$server/kineticwe.db.sig" -o db.tar.gz.sig; then
    echo ">> Verifying signatures"
    gpg --batch --quiet --import "$keyring_dir/kineticwe.gpg"
    gpg --batch --quiet --verify db.tar.gz.sig db.tar.gz 2>/dev/null ||
        { echo 'FAIL: bad repo database signature'; exit 1; }
    for name in "${pkgnames[@]}"; do
        if ! curl -sfL "$server/${latest_file[$name]}.sig" -o "pkgs/$name.pkg.tar.zst.sig"; then
            echo "FAIL: $name has no signature"
            exit 1
        fi
        gpg --batch --quiet --verify "pkgs/$name.pkg.tar.zst.sig" "pkgs/$name.pkg.tar.zst" 2>/dev/null ||
            { echo "FAIL: bad signature on $name"; exit 1; }
    done
    echo ">> Signatures OK"
elif [[ "${KWE_REQUIRE_SIGNING:-}" == 1 ]]; then
    echo 'FAIL: repo is unsigned but KWE_REQUIRE_SIGNING=1'
    exit 1
else
    echo ">> Repo is unsigned; skipping signature checks"
fi

# Assert the packaged portal backend is present and KDE-free.
echo ">> Checking portal backend packaging"
[[ -x root/usr/lib/libexec/xdg-desktop-portal-kwe ]] \
    || { echo 'FAIL: xdg-desktop-portal-kwe binary missing from /usr/lib/libexec'; exit 1; }
grep -q '^UseIn=KineticWE$' root/usr/share/xdg-desktop-portal/portals/kwe.portal \
    || { echo 'FAIL: kwe.portal missing or not keyed to KineticWE'; exit 1; }
[[ -f root/usr/share/xdg-desktop-portal/kineticwe-portals.conf ]] \
    || { echo 'FAIL: kineticwe-portals.conf missing'; exit 1; }
[[ -f root/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.kwe.service ]] \
    || { echo 'FAIL: portal D-Bus service file missing'; exit 1; }
# KDE-free check. Translations ship as xdg-desktop-portal-kwe.mo, so no
# KDE-named file should appear under the portal paths (locale dirs are still
# excluded to tolerate stray upstream catalogs); anything else is a
# packaging bug.
if find root/usr -path '*xdg-desktop-portal*' -name '*kde*' ! -path '*/locale/*' | grep -qv 'desktop.kwe'; then
    echo 'FAIL: KDE portal backend files shipped in package'
    find root/usr -path '*xdg-desktop-portal*' -name '*kde*'
    exit 1
fi
echo ">> Portal backend packaging OK"

# Install the dependencies of all four packages that exist in the Arch repos.
# Our own split packages are already extracted into root/ and are dropped
# from the list; anything else not in the Arch repos is reported and skipped.
mapfile -t deps < <(
    for name in "${pkgnames[@]}"; do
        bsdtar -xOf "pkgs/$name.pkg.tar.zst" .PKGINFO
    done | sed -n 's/^depend = //p' | sed 's/[<>=].*$//' | sort -u
)
todo=()
for d in "${deps[@]}"; do
    if printf '%s\n' "${pkgnames[@]}" | grep -qxF -- "$d"; then
        continue
    fi
    if pacman -Si "$d" &>/dev/null; then
        todo+=("$d")
    else
        echo ">> skipping (not in Arch repos): $d"
    fi
done
if ((${#todo[@]})); then
    pacman -S --noconfirm --needed "${todo[@]}"
fi

# Link-check every shipped ELF, preferring the package's own bundled libs.
export LD_LIBRARY_PATH="$work/root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fail=0
while IFS= read -r f; do
    out="$(ldd "$f" 2>/dev/null || true)"
    if grep -q 'not found' <<<"$out"; then
        echo "BROKEN: ${f#"$work"/root}"
        grep 'not found' <<<"$out" | sed 's/^/    /'
        fail=1
    fi
done < <(find root/usr -type f \( -name '*.so*' -o -perm -u+x \))

if ((fail)); then
    echo '>> SMOKE TEST FAILED: binary no longer links against current Arch.'
    echo '>> Trigger a rebuild: GitHub -> Actions -> Arch repo -> Run workflow (task: build).'
    exit 1
fi
echo '>> SMOKE TEST PASSED'
