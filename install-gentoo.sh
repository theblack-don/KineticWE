#!/usr/bin/env bash
set -euo pipefail

REPO_NAME="kineticwe-overlay"
REPO_URL="https://github.com/TwelfthFace/kineticwe-overlay.git"
PACKAGE="kde-plasma/kineticwe"

die() {
    echo -e "error: $*" >&2
    exit 1
}

info() {
    echo -e "==> $*"
}

has_eselect_repository_module() {
    command -v eselect >/dev/null 2>&1 || return 1

    eselect modules list 2>/dev/null \
        | awk '$1 == "repository" { found = 1 } END { exit(found ? 0 : 1) }'
}

repo_enabled() {
    eselect repository list -i 2>/dev/null \
        | awk -v repo="${REPO_NAME}" '$2 == repo { found = 1 } END { exit(found ? 0 : 1) }'
}

package_installed() {
    qlist -IC "${PACKAGE}" >/dev/null 2>&1
}

package_has_update() {
    emerge --pretend --update --quiet "${PACKAGE}" 2>/dev/null \
        | grep -q '^\[ebuild'
}

if [[ ${EUID} -ne 0 ]]; then
    die "please run as root: sudo ./install-gentoo.sh"
fi

command -v emerge >/dev/null 2>&1 || die "this script requires Gentoo/Portage"

if ! command -v git >/dev/null 2>&1; then
    info "Installing git..."
    emerge --ask=n dev-vcs/git
else
    info "git is already installed."
fi

if ! has_eselect_repository_module; then
    info "Installing eselect-repository..."
    emerge --ask=n app-eselect/eselect-repository
else
    info "eselect repository module is already installed."
fi

if ! has_eselect_repository_module; then
    die "eselect repository module is still unavailable after installing app-eselect/eselect-repository"
fi

if repo_enabled; then
    info "Repository ${REPO_NAME} is already enabled."
else
    info "Adding ${REPO_NAME}..."
    eselect repository add "${REPO_NAME}" git "${REPO_URL}"
fi

info "Syncing ${REPO_NAME}..."
emaint sync -r "${REPO_NAME}"

if ! command -v qlist >/dev/null 2>&1; then
    info "Installing gentoolkit..."
    emerge --ask=n app-portage/gentoolkit
else
    info "gentoolkit is already installed. \n"
fi

info "Note: ${PACKAGE} is currently provided as a 9999 live ebuild."
info "This means it builds the latest upstream Git version and may be unstable."
info "Versioned ebuilds will be added once KineticWE has stable releases.\n"

if package_installed; then
    info "${PACKAGE} is already installed."

    if package_has_update; then
        info "Updating ${PACKAGE}..."
        emerge --ask "${PACKAGE}"
    else
        info "${PACKAGE} is already up to date."
    fi
else
    info "Installing ${PACKAGE}..."
    emerge --ask "${PACKAGE}"
fi
