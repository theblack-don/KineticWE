#!/usr/bin/env bash
# Detach-sign packages with the KineticWE repo signing subkey (CI only).
#
#   GNUPGHOME=<empty dir> sign.sh <keyring-dir> <pkg.tar.zst>...
#
# KWE_SIGNING_KEY holds the secret signing subkey(s), exported with
# "gpg --export-secret-subkeys <primary> | base64 -w0" so it fits a secret in
# the GitHub `release` environment (only tags/master can use it, so only those
# sign).
# The key must belong to the primary fingerprint in <keyring-dir>/kineticwe-trusted,
# and <keyring-dir>/kineticwe.gpg must be that same key, so CI can never sign
# with a key users were not told to trust.
#
# stdout: the signing subkey fingerprint, for repo-add (GPGKEY); empty when
# unsigned. Without KWE_SIGNING_KEY the packages stay unsigned, unless
# KWE_REQUIRE_SIGNING=1, which turns that into an error.
set -euo pipefail

die() { echo "sign: $*" >&2; exit 1; }

keyring_dir="${1:?usage: sign.sh <keyring-dir> <pkg.tar.zst>...}"
shift
(($# > 0)) || die "no packages given"

if [[ -z "${KWE_SIGNING_KEY:-}" ]]; then
    [[ "${KWE_REQUIRE_SIGNING:-}" != 1 ]] ||
        die "KWE_REQUIRE_SIGNING=1 but KWE_SIGNING_KEY is not available (unprotected ref?)"
    echo "sign: KWE_SIGNING_KEY not available; packages stay unsigned" >&2
    exit 0
fi

: "${GNUPGHOME:?GNUPGHOME must point to an empty, private directory}"
chmod 700 "$GNUPGHOME"

primary=$(cut -d: -f1 "$keyring_dir/kineticwe-trusted" | head -1)
[[ "$primary" =~ ^[0-9A-F]{40}$ ]] || die "no primary fingerprint in $keyring_dir/kineticwe-trusted"

# The published keyring must be the trusted primary key.
shipped=$(gpg --batch --with-colons --show-keys "$keyring_dir/kineticwe.gpg" |
    awk -F: '$1 == "pub" { getline; print $10 }')
grep -qxF -- "$primary" <<<"$shipped" || die "$keyring_dir/kineticwe.gpg does not contain $primary"

gpg --batch --quiet --import "$keyring_dir/kineticwe.gpg"
base64 -d <<<"$KWE_SIGNING_KEY" | gpg --batch --quiet --import ||
    die "KWE_SIGNING_KEY is not a valid base64 gpg export"
printf '%s:6:\n' "$primary" | gpg --batch --quiet --import-ownertrust

# Newest valid (not expired/revoked) signing subkey of the trusted primary
# whose secret part was imported.
subkey=$(gpg --batch --with-colons --list-secret-keys "$primary" 2>/dev/null | awk -F: '
    $1 == "ssb" { cand = ($2 !~ /[erd]/ && $12 ~ /s/ && $15 !~ /#/); next }
    $1 == "fpr" && cand { fpr = $10; cand = 0 }
    END { print fpr }') || subkey=""
[[ -n "$subkey" ]] || die "KWE_SIGNING_KEY has no usable signing subkey of $primary"
echo "sign: signing with subkey $subkey (primary $primary)" >&2

for pkg in "$@"; do
    gpg --batch --yes --no-armor --local-user "$subkey!" \
        --detach-sign --output "$pkg.sig" "$pkg"
    echo "sign: $(basename -- "$pkg").sig" >&2
done
echo "$subkey"
