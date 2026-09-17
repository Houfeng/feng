# Feng

[English](README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · **Português** · [日本語](README.ja.md) · [Русский](README.ru.md) · [Deutsch](README.de.md) · [Français](README.fr.md)

Feng é uma linguagem de programação compilada e estaticamente tipada, com sintaxe concisa, contratos explícitos e gerenciamento automático de memória. Seu nome vem do caractere chinês “锋”, que significa “afiado”.

[Site](https://feng-lang.com/index-pt-br.html) · [Manual do usuário](docs/manual/en/README.md) · [Versões](https://github.com/Houfeng/feng/releases)

## Recursos

- **Tipagem forte e estática**: Inferência de tipos, genéricos, closures e casamento de padrões.
- **Contratos explícitos**: Declare contratos com `spec`; use `fit` para adaptar tipos a contratos ou adicionar membros de extensão.
- **Gerenciamento automático de memória**: A contagem automática de referências (ARC) e a coleta de ciclos gerenciam o tempo de vida dos objetos gerenciados.
- **Interoperabilidade com C**: Chame bibliotecas C por meio de `extern func` e anotações ABI.
- **Ferramentas de desenvolvimento**: Compilação de projetos, gerenciamento de dependências, serviços de linguagem, depuração e extensões para VS Code e Zed.

## Instalação

Os pacotes oficiais oferecem suporte a Macs com Apple Silicon e a sistemas GNU/Linux nas arquiteturas x86-64 e ARM64.

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

Após a instalação, abra um novo terminal e execute `feng --version` para verificar a instalação. Consulte o [guia de instalação](docs/manual/en/getting-started/installation.md) para escolher uma versão ou instalar manualmente.

## Início rápido

Crie um projeto em um diretório vazio:

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

Substitua o conteúdo do arquivo gerado `src/main.ff` por:

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

Execute o programa:

```bash
feng run
```

A saída é `Hello, Feng!`. Use `feng check` para verificar o projeto e `feng build --release` para compilar uma versão de lançamento. Continue com [Seu primeiro projeto](docs/manual/en/getting-started/first-project.md).

## Documentação

- [Especificações da linguagem e das ferramentas](docs/specifications/README.md) (em chinês)
- [Guia da biblioteca padrão](docs/manual/en/standard-library/README.md)
- Suporte a editores: [VS Code](editors/feng-vscode/README.md), [Zed](editors/feng-zed/README.md) (em chinês)
- [Documentação de engenharia](docs/engineering/README.md) (em chinês)

## Compilar a partir do código-fonte

Instale Clang, Make e Git LFS. No macOS, as Xcode Command Line Tools também são necessárias.

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

O compilador fica em `build/bin/feng`. Execute `make test` para rodar todos os testes de regressão: `test/` verifica o compilador e o ambiente de execução, enquanto `fcts/` verifica o comportamento da linguagem.

## Licença

[MIT](LICENSE)
