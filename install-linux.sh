#!/usr/bin/env bash
# Installe ce fork FSearch (branche everything) depuis les sources.
# Usage typique sur une machine Linux :
#   curl -fsSL https://raw.githubusercontent.com/UICwebmaster/fsearch/everything/install-linux.sh | bash
#
# Variables utiles :
#   FSEARCH_REPO, FSEARCH_BRANCH, PREFIX, FSEARCH_SRC_DIR, SKIP_INSTALL=1

set -euo pipefail

FSEARCH_REPO="${FSEARCH_REPO:-https://github.com/UICwebmaster/fsearch.git}"
FSEARCH_BRANCH="${FSEARCH_BRANCH:-everything}"
PREFIX="${PREFIX:-/usr/local}"
SKIP_INSTALL="${SKIP_INSTALL:-0}"

say() { printf '\n%s\n' "$*"; }
die() { printf 'Erreur: %s\n' "$*" >&2; exit 1; }

need_root_priv_for_install() {
  [[ "$SKIP_INSTALL" == "1" ]] && return 1
  [[ "$PREFIX" == "$HOME/.local" ]] && return 1
  [[ "$PREFIX" == /usr/local* ]] || [[ "$PREFIX" == /usr ]]
}

if [[ "$(uname -s)" != "Linux" ]]; then
  die "Ce script fonctionne uniquement sous Linux (vous êtes sur $(uname -s))."
fi

if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
  die "Relancez sans sudo : le script appellera sudo seulement pour les paquets et l'installation."
fi

if ! command -v sudo >/dev/null 2>&1; then
  die "sudo est requis pour installer les dépendances et le programme."
fi

install_deps() {
  if [[ -r /etc/os-release ]]; then
    # shellcheck source=/dev/null
    . /etc/os-release
  else
    die "Impossible de détecter la distribution (/etc/os-release manquant)."
  fi

  say ">>> Installation des outils de compilation (${ID:-inconnu})..."

  case "${ID:-}" in
    ubuntu|debian|pop|linuxmint|elementary)
      sudo apt-get update -qq
      sudo apt-get install -y \
        build-essential meson ninja-build git appstream libtool itstool pkg-config \
        intltool libicu-dev libpcre2-dev libglib2.0-dev libgtk-3-dev libxml2-utils
      ;;
    fedora|rhel|centos|rocky|almalinux)
      sudo dnf install -y \
        gcc meson ninja-build git appstream libtool gettext-devel pkgconf-pkg-config \
        libicu-devel pcre2-devel glib2-devel gtk3-devel libxml2 itstool
      ;;
    arch|manjaro|endeavouros)
      sudo pacman -Sy --needed --noconfirm \
        base-devel meson ninja git appstream libtool gettext pkgconf icu pcre2 \
        glib2 gtk3 libxml2 itstool
      ;;
    opensuse*|suse)
      sudo zypper --non-interactive install -y \
        gcc meson ninja git appstream-tools libtool gettext-runtime pkg-config \
        libicu-devel libpcre2-devel glib2-devel gtk3-devel libxml2 itstool
      ;;
    *)
      die "Distribution « ${PRETTY_NAME:-$ID} » non gérée automatiquement. Consultez INSTALL.fr.md pour compiler à la main."
      ;;
  esac
}

resolve_source_dir() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

  if [[ -n "${FSEARCH_SRC_DIR:-}" ]]; then
    echo "$FSEARCH_SRC_DIR"
    return
  fi

  if [[ -f "$script_dir/meson.build" ]] && [[ -f "$script_dir/src/meson.build" ]]; then
    echo "$script_dir"
    return
  fi

  local clone_dir="${XDG_CACHE_HOME:-$HOME/.cache}/fsearch-fork-src"
  if [[ -d "$clone_dir/.git" ]]; then
    say ">>> Mise à jour du dépôt dans $clone_dir ..."
    git -C "$clone_dir" fetch origin "$FSEARCH_BRANCH"
    git -C "$clone_dir" checkout "$FSEARCH_BRANCH"
    git -C "$clone_dir" pull --ff-only origin "$FSEARCH_BRANCH" || true
  else
    say ">>> Clonage du fork ($FSEARCH_BRANCH) dans $clone_dir ..."
    rm -rf "$clone_dir"
    git clone --depth 1 --branch "$FSEARCH_BRANCH" "$FSEARCH_REPO" "$clone_dir"
  fi
  echo "$clone_dir"
}

build_and_install() {
  local src="$1"
  local build_dir="$src/build"

  say ">>> Configuration (prefix=$PREFIX) ..."
  cd "$src"
  meson setup "$build_dir" --prefix="$PREFIX" --buildtype=release -Dchannel=other

  say ">>> Compilation (peut prendre quelques minutes) ..."
  ninja -C "$build_dir"

  if [[ "$SKIP_INSTALL" == "1" ]]; then
    say ">>> SKIP_INSTALL=1 : binaire local : $build_dir/src/fsearch"
    return
  fi

  say ">>> Installation dans $PREFIX ..."
  if need_root_priv_for_install; then
    sudo ninja -C "$build_dir" install
  else
    ninja -C "$build_dir" install
  fi

  say ">>> Terminé."
  if command -v fsearch >/dev/null 2>&1; then
    printf 'Lancez : fsearch\n'
  else
    printf 'Si la commande n’est pas dans le PATH : %s/src/fsearch\n' "$build_dir"
    printf 'Ou ajoutez %s/bin à votre PATH.\n' "$PREFIX"
  fi
}

say "=== Installation FSearch (fork UICwebmaster, branche $FSEARCH_BRANCH) ==="
install_deps
SRC="$(resolve_source_dir)"
build_and_install "$SRC"