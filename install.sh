#!/bin/sh
# Builds watrc and installs it, with the standard library, so that `watrc`
# works from any directory.
#
#   ./install.sh                     # into ~/.local, and puts ~/.local/bin on your PATH
#   ./install.sh --prefix /usr/local # anywhere else (may need sudo)
#   ./install.sh --no-modify-path    # don't touch shell startup files
#   ./install.sh --uninstall         # remove it again (same --prefix as the install)
#
# The result: <prefix>/bin/watrc and <prefix>/lib/watrlang/std/. Needs a C
# compiler, make and GNU binutils (as, ld): on Debian or Ubuntu,
# `sudo apt install build-essential`.
set -eu

prefix="$HOME/.local"
modify_path=1
uninstall=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) [ $# -ge 2 ] || { echo "install.sh: --prefix needs a directory" >&2; exit 2; }
                  prefix=$2; shift ;;
        --prefix=*) prefix=${1#--prefix=} ;;
        --no-modify-path) modify_path=0 ;;
        --uninstall) uninstall=1 ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "install.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
    shift
done

cd "$(dirname "$0")"
case "$prefix" in /*) ;; *) prefix="$(pwd)/$prefix" ;; esac
bindir="$prefix/bin"
marker="# added by watrlang's install.sh"

# The shell startup file that sets PATH for new terminals.
rc_file() {
    case "$(basename "${SHELL:-sh}")" in
        zsh)  echo "${ZDOTDIR:-$HOME}/.zshrc" ;;
        bash) if [ "$(uname)" = Darwin ]; then echo "$HOME/.bash_profile"; else echo "$HOME/.bashrc"; fi ;;
        fish) echo "${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/watrlang.fish" ;;
        *)    echo "$HOME/.profile" ;;
    esac
}

if [ "$uninstall" = 1 ]; then
    make --no-print-directory uninstall PREFIX="$prefix"
    rc=$(rc_file)
    if [ -f "$rc" ] && grep -qF "$marker" "$rc"; then
        # Drop our two lines: the marker and the PATH line after it.
        tmp=$(mktemp)
        awk -v m="$marker" '$0 == m { skip = 2 } skip > 0 { skip--; next } { print }' "$rc" > "$tmp"
        cat "$tmp" > "$rc"
        rm -f "$tmp"
        echo "removed the PATH line from $rc"
    fi
    echo "watrlang uninstalled from $prefix"
    exit 0
fi

for tool in make as ld; do
    command -v "$tool" > /dev/null 2>&1 || {
        echo "install.sh: '$tool' is missing; install a C compiler, make and binutils first" >&2
        echo "            (Debian/Ubuntu: sudo apt install build-essential)" >&2
        exit 1
    }
done

echo "building watrc..."
make --no-print-directory
echo "installing into $prefix..."
make --no-print-directory install PREFIX="$prefix"

case ":$PATH:" in
    *":$bindir:"*)
        echo "done: $bindir is already on your PATH; try \`watrc --version\`"
        exit 0 ;;
esac

if [ "$modify_path" = 0 ]; then
    echo "done. $bindir isn't on your PATH; add it with:"
    echo "    export PATH=\"$bindir:\$PATH\""
    exit 0
fi

rc=$(rc_file)
if [ -f "$rc" ] && grep -qF "$marker" "$rc"; then
    echo "done ($rc already adds watrlang to PATH)"
else
    mkdir -p "$(dirname "$rc")"
    case "$rc" in
        *.fish) line="fish_add_path \"$bindir\"" ;;
        *)      line="export PATH=\"$bindir:\$PATH\"" ;;
    esac
    printf '\n%s\n%s\n' "$marker" "$line" >> "$rc"
    echo "done: added $bindir to PATH in $rc"
fi
echo "open a new terminal (or run: . \"$rc\") and try \`watrc --version\`"
