#!/usr/bin/env bash
# X4 VR installer for Linux: installs the latest release, and updates it when run again.
# Usage and examples: ./linux/install.sh --help (the text is in usage() below).
# Player instructions: README.md, section Linux. What players run:
#   curl -fsSL https://raw.githubusercontent.com/ToffelsKater/X4_VR/main/linux/install.sh | bash
#
# Each version is unpacked into a folder of its own; `current` points at the one in use, and
# ~/.local/bin/x4vr and x4vr-run point into it, so the Steam launch option never changes:
#   ~/.local/opt/x4vr/v0.6.0/{bin,lib,share,version.txt}
#   ~/.local/opt/x4vr/current -> v0.6.0
#   ~/.local/bin/x4vr     -> ~/.local/opt/x4vr/current/bin/x4vr
#   ~/.local/bin/x4vr-run -> ~/.local/opt/x4vr/current/bin/x4vr-run
# Older versions stay until --clean; --use switches back to one (rollback).
#
# Everything runs from main(), called on the last line: a download cut off midway defines
# functions but runs nothing.
set -euo pipefail

repo=${X4VR_REPO:-ToffelsKater/X4_VR}   # where releases come from; another repository for testing
opt=$HOME/.local/opt/x4vr               # the installed versions
bin=$HOME/.local/bin                    # the commands players run
min_glibc=2.31                          # what the release binaries need: Steam's own minimum
tmp=                                    # the download folder, set by main()

usage() {
    cat <<'TEXT'
NAME
    install.sh - install, update and switch versions of the X4 VR mod for Linux

SYNOPSIS
    install.sh [VERSION]
    install.sh --list
    install.sh --use VERSION
    install.sh --clean
    install.sh -h | --help

DESCRIPTION
    Installs the X4 VR mod's prebuilt release into your home folder, and updates it
    when run again. Each version is kept in a folder of its own, so you can switch
    back to an earlier one. Run it as your own user, not as root or with sudo.

    VERSION chooses which release to install:
        (none)    the latest release; pre-releases are skipped
        <tag>     that release or pre-release, e.g. v0.5.1

    Installing a version that is already installed downloads it again and replaces
    it, which also repairs damaged files.

    This installs prebuilt binaries only. To build from source instead,
    clone the repository and follow the README, section Linux.

OPTIONS
    --list           show the installed versions; * marks the one in use
    --use VERSION    switch to an installed version, e.g. to go back after an
                     update; nothing is downloaded
    --clean          remove every installed version except the one in use
    -h, --help       show this help and exit

    Close X4 first: nothing is changed while it runs.

FILES
    ~/.local/opt/x4vr/<version>/   the installed versions
    ~/.local/opt/x4vr/current      the version in use
    ~/.local/bin/x4vr              the menu
    ~/.local/bin/x4vr-run          the Steam launch option: ~/.local/bin/x4vr-run %command%

ENVIRONMENT
    X4VR_REPO        the GitHub repository to install from (default ToffelsKater/X4_VR)

EXAMPLES
    install.sh               install or update to the latest release
    install.sh v0.5.1        install release v0.5.1 and use it
    install.sh --use v0.6.0  go back to v0.6.0, installed before
TEXT
}

die() { echo "x4vr install: $*" >&2; exit 1; }
say() { echo "x4vr install: $*"; }

# ---- checks ----------------------------------------------------------------------------------

no_root() {
    [ "$(id -u)" -ne 0 ] || die "run this as your own user, not as root or with sudo: it installs into your home folder"
}

# X4 loads the mod's files while it runs: nothing is changed then.
x4_closed() {
    if pgrep -x X4 >/dev/null 2>&1; then die "X4 is running: close it first, then run this again"; fi
}

# The release binaries need glibc 2.31, as Steam itself does since 2025.
glibc_ok() {
    local have
    have=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}') || true
    [ -n "$have" ] || { say "warning: couldn't tell this system's glibc version; going ahead"; return 0; }
    [ "$(printf '%s\n%s\n' "$have" "$min_glibc" | sort -V | head -1)" = "$min_glibc" ] ||
        die "this system has glibc $have, the release needs $min_glibc or newer (as Steam does); to build from source instead, see the README, section Linux"
}

# ---- releases --------------------------------------------------------------------------------

# The newest release's tag. GitHub's "latest" skips pre-releases.
latest_tag() {
    curl -fsSL "https://api.github.com/repos/$repo/releases/latest" |
        sed -n 's/^ *"tag_name": *"\([^"]*\)".*/\1/p'
}

# Downloads a release's Linux archive into $tmp and checks it against its checksum file. Prints
# the archive's path.
download() {
    local tag=$1
    local name="X4_VR-$tag-linux-x86_64.tar.gz"
    local url="https://github.com/$repo/releases/download/$tag"
    curl -fL --progress-bar -o "$tmp/$name" "$url/$name" ||
        die "release $tag has no Linux download ($url/$name)"
    curl -fsSL -o "$tmp/$name.sha256" "$url/$name.sha256" ||
        die "release $tag has no checksum file for its Linux download"
    (cd "$tmp" && sha256sum --quiet -c "$name.sha256") ||
        die "the download doesn't match its checksum (damaged or incomplete): run this again"
    echo "$tmp/$name"
}

# ---- installed versions ----------------------------------------------------------------------

# The version in use, or nothing.
current() { if [ -L "$opt/current" ]; then readlink "$opt/current"; fi; }

# The installed versions, one per line (folders in $opt, not the hidden ones or `current`).
installed() {
    local dir
    for dir in "$opt"/*/; do
        dir=${dir%/}
        [ -d "$dir" ] && [ ! -L "$dir" ] && [ -f "$dir/bin/x4vr" ] && echo "${dir##*/}"
    done
    return 0
}

# Unpacks an archive as version $2. Into a hidden folder next to the others first, renamed into
# place once complete: a rename in the same folder is a single step, so no half-unpacked version
# is ever used. An existing folder of that version is replaced.
unpack() {
    local archive=$1 tag=$2
    local partial="$opt/.$tag-partial" old="$opt/.$tag-old"
    rm -rf "$partial" "$old"
    mkdir -p "$partial"
    tar -xzf "$archive" -C "$partial" --strip-components=1 # the archive's top folder is x4vr/
    [ -x "$partial/bin/x4vr" ] && [ -x "$partial/bin/x4vr-run" ] || die "the download for $tag is missing files"
    if [ -e "$opt/$tag" ]; then mv "$opt/$tag" "$old"; fi
    mv "$partial" "$opt/$tag"
    rm -rf "$old"
}

# Makes version $1 the one in use: `current` switched in a single step (a new link renamed over
# the old one), then the commands in ~/.local/bin pointed at it.
use() {
    local tag=$1 name
    ln -sfn "$tag" "$opt/.current-new"
    mv -T "$opt/.current-new" "$opt/current"
    mkdir -p "$bin"
    for name in x4vr x4vr-run; do
        if [ -e "$bin/$name" ] && [ ! -L "$bin/$name" ]; then
            die "$bin/$name is from a build from source: uninstall that first (x4vr menu: Uninstall), then run this again"
        fi
        ln -sfn "$opt/current/bin/$name" "$bin/$name"
    done
}

# What to do next, after an install or a switch.
next_steps() {
    say "now using $(current)"
    case ":$PATH:" in
        *":$bin:"*) say "start the menu with: x4vr" ;;
        *) say "start the menu with: $bin/x4vr ($bin isn't on your PATH)" ;;
    esac
    if [ -e /etc/NIXOS ]; then say "on NixOS start it through Steam's environment: steam-run $bin/x4vr"; fi
    say "Steam launch option for X4 (set once, stays the same): $bin/x4vr-run %command%"
}

# ---- commands --------------------------------------------------------------------------------

list() {
    local tag in_use
    in_use=$(current)
    [ -n "$(installed)" ] || { say "nothing installed in $opt"; return 0; }
    installed | while read -r tag; do
        if [ "$tag" = "$in_use" ]; then echo "* $tag"; else echo "  $tag"; fi
    done
}

switch_to() {
    local tag=$1
    x4_closed
    installed | grep -qxF "$tag" || die "$tag isn't installed; installed are: $(installed | tr '\n' ' ')"
    use "$tag"
    next_steps
}

clean() {
    local tag in_use removed=0
    in_use=$(current)
    [ -n "$in_use" ] || die "no version is in use, so nothing is removed"
    while read -r tag; do
        [ "$tag" = "$in_use" ] && continue
        rm -rf "${opt:?}/$tag"
        say "removed $tag"
        removed=1
    done < <(installed)
    [ "$removed" = 1 ] || say "only $in_use is installed: nothing to remove"
}

install() {
    local want=${1:-} archive
    x4_closed
    glibc_ok
    if [ -z "$want" ]; then
        want=$(latest_tag) || die "no latest release of $repo found (none published yet, or GitHub not reachable)"
        [ -n "$want" ] || die "$repo has no release yet"
    fi
    say "downloading $want from $repo"
    tmp=$(mktemp -d)               # the download; removed when the script ends, however it ends
    trap 'rm -rf "$tmp"' EXIT
    archive=$(download "$want") || exit 1
    mkdir -p "$opt"
    unpack "$archive" "$want"
    use "$want"
    say "installed $want in $opt/$want"
    next_steps
}

main() {
    case "${1:-}" in -h|--help) usage; exit 0 ;; esac
    no_root
    case "${1:-}" in
        --list) [ $# -eq 1 ] || { usage >&2; exit 2; }; list ;;
        --use) [ $# -eq 2 ] || { usage >&2; exit 2; }; switch_to "$2" ;;
        --clean) [ $# -eq 1 ] || { usage >&2; exit 2; }; clean ;;
        -*) usage >&2; exit 2 ;;
        *) [ $# -le 1 ] || { usage >&2; exit 2; }; install "${1:-}" ;;
    esac
}

main "$@"
