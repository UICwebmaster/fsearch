# Guide d'installation — FSearch (Linux)

FSearch est un utilitaire de recherche de fichiers rapide (GTK 3), inspiré d'Everything sous Windows.

## Installation en une commande (ce fork)

Les paquets officiels (PPA, COPR, etc.) installent la version **amont** de Christian Boxdörfer, pas ce fork (miniatures, glisser-déposer, etc.). Pour **votre** version, compilez depuis ce dépôt — le script fait tout (dépendances, clone, compilation, installation).

Sur la machine Linux, en utilisateur normal (pas root) :

```bash
curl -fsSL https://raw.githubusercontent.com/UICwebmaster/fsearch/everything/install-linux.sh | bash
```

Sans droits d’écriture sur `/usr/local` (installation dans `~/.local`) :

```bash
curl -fsSL https://raw.githubusercontent.com/UICwebmaster/fsearch/everything/install-linux.sh | env PREFIX="$HOME/.local" bash
```

Ensuite, vérifiez que `~/.local/bin` est dans votre `PATH`, puis lancez `fsearch`.

Si vous avez déjà cloné le dépôt :

```bash
cd linux_find_everything   # racine du clone
./install-linux.sh
```

Compilation seule, sans installation système : `SKIP_INSTALL=1 ./install-linux.sh` puis `./build/src/fsearch`.

## Pourquoi c’est « compliqué » ?

Sous Linux, il n’y a pas d’installateur `.exe` universel : chaque distribution gère les paquets différemment. Un **fork** personnalisé n’a en plus **aucun paquet prêt** sur les dépôts publics — d’où la compilation. Ce script automatise ce que ferait un développeur à la main.

## Choix rapide

| Méthode | Pour qui ? |
|---------|------------|
| Paquets officiels (PPA, COPR, pacman, …) | **Recommandé** — installation simple, mises à jour |
| [Flatpak](https://flathub.org/apps/details/io.github.cboxdoerfer.FSearch) | Toute distribution, installation isolée |
| Compilation depuis les sources | Développeurs ou version personnalisée (ce dépôt) |

Préférez les paquets **Stable** sauf si vous testez volontairement une version de développement.

## Paquets par distribution

### Ubuntu

```bash
sudo add-apt-repository ppa:christian-boxdoerfer/fsearch-stable
sudo apt update
sudo apt install fsearch
```

Version quotidienne (développement) : [PPA Daily](https://launchpad.net/~christian-boxdoerfer/+archive/ubuntu/fsearch-daily).

### Debian

Paquets binaires : [OpenBuildService — Debian](https://software.opensuse.org/download.html?project=home%3Acboxdoerfer&package=fsearch#manualDebian).

### Fedora / RHEL / CentOS

```bash
sudo dnf copr enable cboxdoerfer/fsearch
sudo dnf install fsearch
```

Voir aussi : [COPR Stable](https://copr.fedorainfracloud.org/coprs/cboxdoerfer/fsearch/).

### Arch Linux

```bash
sudo pacman -S fsearch
```

Version git (AUR) : [fsearch-git](https://aur.archlinux.org/packages/fsearch-git/).

### openSUSE

[OpenBuildService — openSUSE](https://software.opensuse.org/download.html?project=home%3Acboxdoerfer&package=fsearch#manualopenSUSE).

### Flatpak

```bash
flatpak install flathub io.github.cboxdoerfer.FSearch
flatpak run io.github.cboxdoerfer.FSearch
```

Limitations possibles en Flatpak : [wiki Flatpak](https://github.com/cboxdoerfer/fsearch/wiki/Flatpak-version-limitations).

## Compilation depuis les sources

### Prérequis d'exécution

- GTK 3.24, GLib 2.60
- PCRE2 (libpcre2) 10.21+
- ICU 4.4+
- glibc 2.19+ ou musl 1.1.15+

### Dépendances de compilation

**Debian / Ubuntu**

```bash
sudo apt update
sudo apt install -y build-essential meson appstream libtool itstool pkg-config \
  intltool libicu-dev libpcre2-dev libglib2.0-dev libgtk-3-dev libxml2-utils
```

**Fedora**

```bash
sudo dnf install -y gcc meson appstream libtool gettext-devel pkg-config \
  libicu-devel pcre2-devel glib2-devel gtk3-devel libxml2
```

**Arch**

```bash
sudo pacman -S --needed base-devel meson appstream libtool gettext pkg-config \
  icu pcre2 glib2 gtk3 libxml2 itstool
```

### Build et installation

Depuis la racine du dépôt :

```bash
meson setup build
ninja -C build
```

Test (optionnel) :

```bash
ninja -C build test
```

Installation système :

```bash
sudo ninja -C build install
```

Lancement sans installation :

```bash
./build/src/fsearch
```

Des options Meson avancées et d'autres distributions sont décrites dans la [wiki officielle — Build instructions](https://github.com/cboxdoerfer/fsearch/wiki/Build-instructions).

## Premier lancement

1. Au démarrage, FSearch construit un **index** de vos fichiers (comportement proche d'Everything).
2. Dans **Préférences → Base de données**, définissez les dossiers à indexer et les exclusions.
3. La recherche est **instantanée** une fois l'index à jour ; la syntaxe avancée est documentée dans la [Search syntax](https://github.com/cboxdoerfer/fsearch/wiki/Search-syntax).

## Dépannage

- **Pas de paquet pour ma distro** : essayez Flatpak ou la compilation.
- **Échec de `meson setup`** : vérifiez `libpcre2-dev` (pas seulement `libpcre3-dev` sur les systèmes récents).
- **Bugs / demandes** : [issues GitHub](https://github.com/cboxdoerfer/fsearch/issues) (projet amont) ou ce fork selon votre remonte.

## Voir aussi

- [README.md](README.md) — fonctionnalités et tableau des téléchargements
- [CONTRIBUTING.md](CONTRIBUTING.md) — contribution et release