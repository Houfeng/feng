# Feng

[English](README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português](README.pt-BR.md) · [日本語](README.ja.md) · [Русский](README.ru.md) · **Deutsch** · [Français](README.fr.md)

Feng ist eine statisch typisierte, kompilierte Programmiersprache mit prägnanter Syntax, expliziten Verträgen und automatischer Speicherverwaltung.

[Website](https://feng-lang.com/index-de.html) · [Benutzerhandbuch](docs/manual/en/README.md) · [Releases](https://github.com/Houfeng/feng/releases)

## Funktionen

- **Starke, statische Typisierung**: Typinferenz, Generics, Closures und Musterabgleich.
- **Explizite Verträge**: Verträge werden mit `spec` deklariert. Mit `fit` lassen sich Typen an Verträge anpassen oder um zusätzliche Member erweitern.
- **Automatische Speicherverwaltung**: Automatische Referenzzählung (ARC) und die Bereinigung von Referenzzyklen verwalten die Lebensdauer verwalteter Objekte.
- **C-Interoperabilität**: C-Bibliotheken lassen sich über `extern func` und ABI-Annotationen aufrufen.
- **Entwicklungswerkzeuge**: Projekt-Builds, Abhängigkeitsverwaltung, Sprachdienste, Debugging und Erweiterungen für VS Code und Zed.

## Installation

Offizielle Pakete unterstützen Macs mit Apple Silicon sowie GNU/Linux-Systeme auf x86-64 und ARM64.

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

Öffne nach der Installation ein neues Terminal und prüfe die Installation mit `feng --version`. Hinweise zur Versionsauswahl und manuellen Installation findest du in der [Installationsanleitung](docs/manual/en/getting-started/installation.md).

## Schnellstart

Erstelle ein Projekt in einem leeren Verzeichnis:

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

Ersetze den Inhalt der erzeugten Datei `src/main.ff` durch:

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

Führe das Programm aus:

```bash
feng run
```

Die Ausgabe lautet `Hello, Feng!`. Mit `feng check` prüfst du das Projekt; mit `feng build --release` erstellst du einen Release-Build. Weiter geht es mit [Dein erstes Projekt](docs/manual/en/getting-started/first-project.md).

## Dokumentation

- [Sprach- und Werkzeugspezifikationen](docs/specifications/README.md) (Chinesisch)
- [Leitfaden zur Standardbibliothek](docs/manual/en/standard-library/README.md)
- Editor-Unterstützung: [VS Code](editors/feng-vscode/README.md), [Zed](editors/feng-zed/README.md) (Chinesisch)
- [Entwicklungsdokumentation](docs/engineering/README.md) (Chinesisch)

## Aus dem Quellcode bauen

Installiere Clang, Make und Git LFS. Unter macOS werden zusätzlich die Xcode Command Line Tools benötigt.

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

Der Compiler befindet sich unter `build/bin/feng`. Führe `make test` für die vollständige Regressionsprüfung aus: `test/` prüft Compiler und Laufzeitumgebung, `fcts/` das Sprachverhalten.

## Lizenz

[MIT](LICENSE)
