# Feng

[English](README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português](README.pt-BR.md) · [日本語](README.ja.md) · **Русский** · [Deutsch](README.de.md) · [Français](README.fr.md)

Feng — компилируемый язык программирования со статической типизацией, лаконичным синтаксисом, явными контрактами и автоматическим управлением памятью.

[Сайт](https://feng-lang.com/index-ru.html) · [Руководство пользователя](docs/manual/en/README.md) · [Релизы](https://github.com/Houfeng/feng/releases)

## Возможности

- **Строгая статическая типизация**: Вывод типов, обобщения, замыкания и сопоставление с образцом.
- **Явные контракты**: Объявляйте контракты с помощью `spec`; используйте `fit`, чтобы адаптировать типы к контрактам или расширять их новыми членами.
- **Автоматическое управление памятью**: Автоматический подсчёт ссылок (ARC) и сборка циклов управляют временем жизни управляемых объектов.
- **Совместимость с C**: Вызывайте библиотеки C с помощью `extern func` и аннотаций ABI.
- **Инструменты разработки**: Сборка проектов, управление зависимостями, языковые сервисы, отладка и расширения для VS Code и Zed.

## Установка

Официальные пакеты поддерживают Mac с Apple Silicon и системы GNU/Linux на архитектурах x86-64 и ARM64.

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

После установки откройте новый терминал и выполните `feng --version` для проверки. Выбор версии и установка вручную описаны в [руководстве по установке](docs/manual/en/getting-started/installation.md).

## Быстрый старт

Создайте проект в пустом каталоге:

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

Замените содержимое созданного файла `src/main.ff` следующим кодом:

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

Запустите программу:

```bash
feng run
```

Программа выведет `Hello, Feng!`. Используйте `feng check` для проверки проекта и `feng build --release` для сборки релизной версии. Далее см. [Первый проект](docs/manual/en/getting-started/first-project.md).

## Документация

- [Спецификации языка и инструментов](docs/specifications/README.md) (на китайском)
- [Руководство по стандартной библиотеке](docs/manual/en/standard-library/README.md)
- Поддержка редакторов: [VS Code](editors/feng-vscode/README.md), [Zed](editors/feng-zed/README.md) (на китайском)
- [Инженерная документация](docs/engineering/README.md) (на китайском)

## Сборка из исходного кода

Установите Clang, Make и Git LFS. В macOS также необходимы Xcode Command Line Tools.

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

Компилятор будет доступен по пути `build/bin/feng`. Выполните `make test`, чтобы запустить весь набор регрессионных тестов: `test/` проверяет компилятор и среду выполнения, а `fcts/` — поведение языка.

## Лицензия

[MIT](LICENSE)
