# Feng

[English](README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português](README.pt-BR.md) · **日本語** · [Русский](README.ru.md) · [Deutsch](README.de.md) · [Français](README.fr.md)

Feng は、簡潔な構文、明示的な契約、自動メモリ管理を備えた静的型付けのコンパイル型プログラミング言語です。名前は「鋭い」を意味する中国語の「锋」に由来します。

[公式サイト](https://feng-lang.com/index-ja.html) · [ユーザーマニュアル](docs/manual/en/README.md) · [リリース](https://github.com/Houfeng/feng/releases)

## 特徴

- **強い型付け・静的型付け**：型推論、ジェネリクス、クロージャ、パターンマッチングをサポートします。
- **明示的な契約**：`spec` で契約を宣言し、`fit` で型を契約に適合させたり、拡張メンバーを追加したりできます。
- **自動メモリ管理**：自動参照カウント（ARC）と循環参照の回収により、管理対象オブジェクトの寿命を管理します。
- **C との相互運用**：`extern func` と ABI アノテーションを使って C ライブラリを呼び出せます。
- **開発ツール**：プロジェクトのビルド、依存関係管理、言語サービス、デバッグ、VS Code と Zed 向けの拡張機能を提供します。

## インストール

公式パッケージは、Apple Silicon 搭載 Mac と、x86-64 および ARM64 の GNU/Linux ホストに対応しています。

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

インストール後、新しいターミナルを開き、`feng --version` を実行して確認してください。バージョンの選択や手動インストールについては、[インストールガイド](docs/manual/en/getting-started/installation.md)を参照してください。

## クイックスタート

空のディレクトリにプロジェクトを作成します：

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

生成された `src/main.ff` の内容を次のように置き換えます：

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

プログラムを実行します：

```bash
feng run
```

`Hello, Feng!` と出力されます。`feng check` でプロジェクトをチェックし、`feng build --release` でリリース版をビルドできます。続きは[最初のプロジェクト](docs/manual/en/getting-started/first-project.md)を参照してください。

## ドキュメント

- [言語とツールの仕様](docs/specifications/README.md)（中国語）
- [標準ライブラリガイド](docs/manual/en/standard-library/README.md)
- エディタ対応：[VS Code](editors/feng-vscode/README.md)、[Zed](editors/feng-zed/README.md)（中国語）
- [開発資料](docs/engineering/README.md)（中国語）

## ソースからビルド

Clang、Make、Git LFS を準備してください。macOS では Xcode Command Line Tools も必要です。

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

コンパイラは `build/bin/feng` に生成されます。`make test` で回帰テスト全体を実行できます。`test/` はコンパイラとランタイムを、`fcts/` は言語の動作を検証します。

## ライセンス

[MIT](LICENSE)
