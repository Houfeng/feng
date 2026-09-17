# Feng

[English](README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português](README.pt-BR.md) · [日本語](README.ja.md) · [Русский](README.ru.md) · [Deutsch](README.de.md) · **Français**

Feng est un langage de programmation compilé à typage statique, doté d’une syntaxe concise, de contrats explicites et d’une gestion automatique de la mémoire.

[Site web](https://feng-lang.com/index-fr.html) · [Manuel utilisateur](docs/manual/en/README.md) · [Versions](https://github.com/Houfeng/feng/releases)

## Fonctionnalités

- **Typage fort et statique** : inférence de types, génériques, fermetures et filtrage par motifs.
- **Contrats explicites** : déclarez des contrats avec `spec` ; utilisez `fit` pour adapter des types aux contrats ou ajouter des membres d’extension.
- **Gestion automatique de la mémoire** : le comptage automatique de références (ARC) et la collecte des cycles gèrent la durée de vie des objets gérés.
- **Interopérabilité avec C** : appelez des bibliothèques C via `extern func` et des annotations ABI.
- **Outils de développement** : compilation de projets, gestion des dépendances, services de langage, débogage et extensions pour VS Code et Zed.

## Installation

Les paquets officiels prennent en charge les Mac avec Apple Silicon et les systèmes GNU/Linux sur x86-64 et ARM64.

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

Après l’installation, ouvrez un nouveau terminal et exécutez `feng --version` pour vérifier l’installation. Consultez le [guide d’installation](docs/manual/en/getting-started/installation.md) pour choisir une version ou effectuer une installation manuelle.

## Démarrage rapide

Créez un projet dans un répertoire vide :

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

Remplacez le contenu du fichier généré `src/main.ff` par :

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

Exécutez le programme :

```bash
feng run
```

Le programme affiche `Hello, Feng!`. Utilisez `feng check` pour vérifier le projet et `feng build --release` pour compiler une version de publication. Poursuivez avec [Votre premier projet](docs/manual/en/getting-started/first-project.md).

## Documentation

- [Spécifications du langage et des outils](docs/specifications/README.md) (en chinois)
- [Guide de la bibliothèque standard](docs/manual/en/standard-library/README.md)
- Prise en charge des éditeurs : [VS Code](editors/feng-vscode/README.md), [Zed](editors/feng-zed/README.md) (en chinois)
- [Documentation d’ingénierie](docs/engineering/README.md) (en chinois)

## Compiler depuis les sources

Installez Clang, Make et Git LFS. Sous macOS, les Xcode Command Line Tools sont également nécessaires.

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

Le compilateur se trouve dans `build/bin/feng`. Exécutez `make test` pour lancer l’ensemble des tests de régression : `test/` vérifie le compilateur et l’environnement d’exécution, tandis que `fcts/` vérifie le comportement du langage.

## Licence

[MIT](LICENSE)
