#!/usr/bin/env bash
# Fail early, while there is still time to act, when the repo signing key is
# about to expire. Run by the daily smoke job; needs only gpg and the PUBLIC
# key shipped in the checkout (no secret).
#
#   key-expiry.sh <keyring-dir>
#
# Checks the trusted primary key (first fingerprint in
# <keyring-dir>/kineticwe-trusted) and its longest-lived usable signing
# subkey (the one ci/sign.sh would pick) in <keyring-dir>/kineticwe.gpg.
# Once a subkey expires CI cannot sign, and pacman rejects its signatures.
#
# Renewing (maintainer, on the machine holding the primary key):
#   gpg --quick-set-expire <primary> 2y <subkey-fpr>
#   gpg --export <primary> > arch/kineticwe-keyring/kineticwe.gpg
#   bump pkgrel in arch/kineticwe-keyring/PKGBUILD, update the
#   KWE_SIGNING_KEY secret (gpg --export-secret-subkeys <primary> | base64 -w0)
# Users get the extended key through kineticwe-keyring on their next -Syu.
set -euo pipefail

die() { echo "key-expiry: $*" >&2; exit 1; }

keyring_dir="${1:?usage: key-expiry.sh <keyring-dir>}"
primary=$(cut -d: -f1 "$keyring_dir/kineticwe-trusted" | head -1)
[[ "$primary" =~ ^[0-9A-F]{40}$ ]] || die "no primary fingerprint in $keyring_dir/kineticwe-trusted"

export GNUPGHOME="$(mktemp -d)"
trap 'rm -rf "$GNUPGHOME"' EXIT
chmod 700 "$GNUPGHOME"

# --with-colons: field 2 validity (r = revoked), field 7 expiry (epoch, empty
# = never), field 12 capabilities; the fpr line after a key carries field 10.
# Prints "<kind> <fpr> <expiry-epoch or 0>" for the primary and every
# non-revoked signing subkey.
mapfile -t keys < <(gpg --batch --with-colons --show-keys "$keyring_dir/kineticwe.gpg" 2>/dev/null | awk -F: -v want="$primary" '
    $1 == "pub" { kind = "pub"; ex = $7; keep = 1; next }
    $1 == "sub" { kind = "sub"; ex = $7; keep = ($2 !~ /r/ && $12 ~ /s/); next }
    $1 == "fpr" && kind != "" {
        if (kind == "pub") inkey = ($10 == want)
        else if (inkey && keep) print kind, $10, (ex == "" ? 0 : ex)
        if (kind == "pub" && inkey) print kind, $10, (ex == "" ? 0 : ex)
        kind = ""
    }')

now=$(date +%s)
days_left() { (( $1 == 0 )) && echo 99999 || echo $(( ($1 - now) / 86400 )); }

primary_days=""
subkey="" subkey_days=-99999
for k in "${keys[@]}"; do
    read -r kind fpr exp <<<"$k"
    d=$(days_left "$exp")
    if [[ "$kind" == pub ]]; then
        primary_days=$d
    elif (( d > subkey_days )); then
        subkey=$fpr subkey_days=$d
    fi
done
[[ -n "$primary_days" ]] || die "$keyring_dir/kineticwe.gpg does not contain $primary"
[[ -n "$subkey" ]] || die "no non-revoked signing subkey of $primary in $keyring_dir/kineticwe.gpg"

echo "key-expiry: primary $primary: $primary_days days left"
echo "key-expiry: signing subkey $subkey: $subkey_days days left"

# Fail KWE_KEY_WARN_DAYS (default 60) days ahead: enough time to renew even if
# the maintainer is away for a while, short enough that the daily failure mail
# stays meaningful instead of becoming noise people learn to ignore.
warn="${KWE_KEY_WARN_DAYS:-60}"
fail=0
check() {
    local what=$1 fpr=$2 days=$3
    if (( days < 0 )); then
        echo "FAIL: $what $fpr EXPIRED $(( -days )) days ago; CI cannot sign and pacman rejects its signatures. Renew it now (see the header of ci/key-expiry.sh)."
        fail=1
    elif (( days <= warn )); then
        echo "FAIL: $what $fpr expires in $days days (around $(date -u -d "+$days days" +%F)). Renew it (see the header of ci/key-expiry.sh)."
        fail=1
    fi
}
check "signing subkey" "$subkey" "$subkey_days"
check "primary key" "$primary" "$primary_days"
((fail == 0)) || exit 1
echo "key-expiry: OK (more than $warn days left on both keys)"
exit 0
