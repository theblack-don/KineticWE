#!/usr/bin/env bash
# Decide whether the published repo needs a rebuild, and of which packages,
# and trigger one.
# Cheap enough to run every few hours: needs only bash, curl, jq, bsdtar and git
# (the ubuntu-latest runner), run from a full-history checkout of the watched branch.
#
#   abi-watch.sh <manifest-url or local file> [--dry-run]
#
# Compares the manifest written by ci/abi-manifest.sh at the last build with
# the sources below, and collects the split packages that need a rebuild
# (kineticwe, xdg-desktop-portal-kwe, kineticwe-noctalia, kineticwe-greeter;
# kineticwe-keyring is always built and never listed):
#   * Code: the checked-out branch. Files changed since the built commit,
#     ignoring docs/ and *.md, map to packages by path:
#       shell/*                                   -> kineticwe-noctalia
#       portal/*                                  -> xdg-desktop-portal-kwe
#       kineticwe-greeter.install,
#       arch/kineticwe-setup-greeter.sh           -> kineticwe-greeter
#       PKGBUILD, lgl-kineticwe-copr/*, ci/*,
#       .github/workflows/arch-repo.yml,
#       arch/sync-from-spec.sh                    -> all
#       arch/kineticwe-keyring/*                  -> none (always built), but
#                                                    keyring-only changes -> all
#       anything else                             -> kineticwe
#     A built commit that is not in the branch history (e.g. published from a
#     master tag) -> all.
#   * ABI: the current Arch core/extra databases.
#     - Tier 1 ("version" lines): any upstream version change -> kineticwe and
#       xdg-desktop-portal-kwe (Qt private API / KF6 private headers have no
#       ABI guarantee).
#     - Tier 2 ("soname" lines): only for packages whose version changed, fetch
#       the package's file list; if the linked soname is gone -> the packages
#       in the line's <users> field (a v1 manifest without it -> all).
# A missing manifest (first run, or a repo published before this script)
# also triggers a full rebuild, which publishes one.
#
# Triggering is a workflow_dispatch of .github/workflows/arch-repo.yml
# (override the file name with KWE_WORKFLOW) and needs GH_TOKEN (actions:
# write) plus GitHub's predefined GITHUB_REPOSITORY, GITHUB_REF_NAME and
# GITHUB_API_URL. The rebuild runs on the watched branch (the schedule's ref,
# GITHUB_REF_NAME) with task=build and rebuild=true (KWE_REBUILD=1), so an ABI
# refresh also ships the branch tip. All reasons go into one dispatch, and the
# package set into kwe_build / KWE_BUILD (comma-separated, always listed
# explicitly, even when it is every package); packages not listed are carried
# over from the published repo.
# --dry-run prints the decision and never triggers.
# KWE_WATCH_FORCE=1 always requests a full rebuild (tests the trigger path).
set -euo pipefail

manifest_url="${1:?usage: abi-watch.sh <manifest-url> [--dry-run]}"
dry_run=false
[[ "${2:-}" != --dry-run ]] || dry_run=true
mirror="${ARCH_MIRROR:-https://geo.mirror.pkgbuild.com}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Canonical order, also the KWE_BUILD order.
all_pkgs=(kineticwe xdg-desktop-portal-kwe kineticwe-noctalia kineticwe-greeter)
reasons=()
declare -A build=()
need() {
    local p
    for p in "$@"; do build[$p]=1; done
}
need_all() { need "${all_pkgs[@]}"; }

ref="${GITHUB_REF_NAME:-$(git -C "$repo_root" rev-parse --abbrev-ref HEAD)}"

trigger() {
    local reason p build_list=
    # Defensive: a reason without packages must not end up as an empty
    # KWE_BUILD (which the build pipeline reads as "all" anyway).
    ((${#build[@]})) || need_all
    for p in "${all_pkgs[@]}"; do
        [[ -z "${build[$p]:-}" ]] || build_list+="${build_list:+,}$p"
    done
    reason=$(printf '%s; ' "${reasons[@]}")
    reason="${reason%; }; build: $build_list"
    echo ">> Rebuild needed: $reason"
    echo ">> Packages to rebuild: $build_list"
    if $dry_run; then
        echo ">> --dry-run: not triggering (ref $ref)"
        return 0
    fi
    : "${GH_TOKEN:?GH_TOKEN not set; cannot trigger the rebuild}"
    : "${GITHUB_REPOSITORY:?}" "${GITHUB_REF_NAME:?no ref to rebuild}"
    local api="${GITHUB_API_URL:-https://api.github.com}"
    local workflow="${KWE_WORKFLOW:-arch-repo.yml}" body
    # workflow_dispatch inputs are strings; the workflow accepts "true".
    body=$(jq -n --arg ref "$ref" --arg build "$build_list" --arg reason "${reason:0:1000}" \
        '{ref: $ref, inputs: {task: "build", rebuild: "true", kwe_build: $build, reason: $reason}}')
    echo ">> Dispatching $workflow (task=build, rebuild=true) on $ref"
    curl -sSf -X POST \
        -H "Authorization: Bearer $GH_TOKEN" \
        -H "Accept: application/vnd.github+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" \
        -d "$body" \
        "$api/repos/$GITHUB_REPOSITORY/actions/workflows/$workflow/dispatches" >/dev/null
    echo ">> Rebuild workflow dispatched"
}

echo ">> Fetching manifest $manifest_url"
if [[ -f "$manifest_url" ]]; then
    # Local file (testing): no HTTP status to inspect.
    cp -- "$manifest_url" "$work/manifest"
    code=200
else
    code=$(curl -sSL -o "$work/manifest" -w '%{http_code}' "$manifest_url") || code=000
fi
case "$code" in
    200) ;;
    404)
        reasons+=("no ABI manifest published yet")
        need_all
        trigger
        exit 0
        ;;
    *)
        echo "abi-watch: fetching the manifest failed (HTTP $code)" >&2
        exit 1
        ;;
esac

# Code: commits on the branch since the published build.
built=$(awk '$1 == "commit" { print $2; exit }' "$work/manifest")
head=$(git -C "$repo_root" rev-parse HEAD)
echo ">> Published build: ${built:-<none>}, $ref: $head"
if [[ -z "$built" ]]; then
    reasons+=("manifest records no commit")
    need_all
elif ! git -C "$repo_root" merge-base --is-ancestor "$built" HEAD 2>/dev/null; then
    reasons+=("published build ${built:0:10} is not from $ref")
    need_all
else
    mapfile -t changed < <(git -C "$repo_root" diff --name-only "$built" HEAD -- . \
        ':(exclude)docs/' ':(exclude)*.md')
    if ((${#changed[@]})); then
        count=$(git -C "$repo_root" rev-list --count "$built..HEAD")
        reasons+=("$ref: $count commits since ${built:0:10} (${#changed[@]} files changed)")
        keyring_only=true
        for f in "${changed[@]}"; do
            case "$f" in
                arch/kineticwe-keyring/*) continue ;; # always built
                shell/*) need kineticwe-noctalia ;;
                portal/*) need xdg-desktop-portal-kwe ;;
                kineticwe-greeter.install | arch/kineticwe-setup-greeter.sh)
                    need kineticwe-greeter ;;
                PKGBUILD | lgl-kineticwe-copr/* | .github/workflows/arch-repo.yml | ci/* | arch/sync-from-spec.sh)
                    need_all ;;
                *) need kineticwe ;;
            esac
            keyring_only=false
        done
        # Keyring-only changes: KWE_BUILD cannot say "none" (empty means all),
        # so rebuild everything. Conservative, and rare enough not to matter.
        ! $keyring_only || need_all
    fi
fi

# name -> version and name -> repo from the current sync databases.
declare -A cur_ver=() cur_repo=()
for repo in core extra; do
    echo ">> Fetching $repo.db"
    curl -sSfL -o "$work/$repo.db" "$mirror/$repo/os/x86_64/$repo.db"
    while read -r name ver; do
        cur_ver[$name]=$ver
        cur_repo[$name]=$repo
    done < <(bsdtar -xOf "$work/$repo.db" '*/desc' |
        awk '$0 == "%NAME%" { getline; n = $0 } $0 == "%VERSION%" { getline; print n, $0 }')
done

# Tier 1: upstream version (pkgrel dropped) must match exactly.
while read -r _ pkg built; do
    now=${cur_ver[$pkg]:-}
    if [[ -z "$now" ]]; then
        reasons+=("$pkg no longer in Arch")
    elif [[ "${now%-*}" != "$built" ]]; then
        reasons+=("$pkg $built -> ${now%-*}")
    else
        continue
    fi
    # Private API users: the compositor and the portal.
    need kineticwe xdg-desktop-portal-kwe
done < <(awk '$1 == "version"' "$work/manifest")

# Tier 2 users field -> packages to rebuild. Missing (v1 manifest) -> all;
# the keyring links nothing and is always built; unknown names -> all.
need_users() {
    local u
    local -a us
    if [[ -z "$1" ]]; then
        need_all
        return
    fi
    IFS=, read -ra us <<< "$1"
    for u in "${us[@]}"; do
        case " ${all_pkgs[*]} " in
            *" $u "*) need "$u" ;;
            *) [[ "$u" == kineticwe-keyring ]] || need_all ;;
        esac
    done
}

# Tier 2: the soname file must still be in its (changed) package.
declare -A files_checked=()
while read -r _ pkg built path users; do
    now=${cur_ver[$pkg]:-}
    if [[ -z "$now" ]]; then
        reasons+=("$pkg (provides ${path##*/}) no longer in Arch")
        need_users "$users"
        continue
    fi
    [[ "$now" != "$built" ]] || continue
    list="$work/files-$pkg.txt"
    if [[ -z "${files_checked[$pkg]:-}" ]]; then
        echo ">> $pkg changed ($built -> $now); checking its file list"
        curl -sSfL "https://archlinux.org/packages/${cur_repo[$pkg]}/x86_64/$pkg/files/json/" |
            jq -r '.files[]' > "$list"
        files_checked[$pkg]=1
    fi
    if ! grep -qxF -- "$path" "$list"; then
        reasons+=("${path##*/} gone from $pkg $now")
        need_users "$users"
    fi
done < <(awk '$1 == "soname"' "$work/manifest")

if [[ "${KWE_WATCH_FORCE:-}" == 1 ]]; then
    reasons+=("forced by KWE_WATCH_FORCE=1")
    need_all
fi

if ((${#reasons[@]} == 0)); then
    echo ">> Published packages match $ref and current Arch; no rebuild needed"
    exit 0
fi
trigger
