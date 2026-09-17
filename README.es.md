# Feng

[English](README.md) · [简体中文](README.zh-CN.md) · **Español** · [Português](README.pt-BR.md) · [日本語](README.ja.md) · [Русский](README.ru.md) · [Deutsch](README.de.md) · [Français](README.fr.md)

Feng es un lenguaje de programación compilado y de tipado estático, con una sintaxis concisa, contratos explícitos y gestión automática de memoria.

[Sitio web](https://feng-lang.com/index-es.html) · [Manual de usuario](docs/manual/en/README.md) · [Versiones](https://github.com/Houfeng/feng/releases)

## Características

- **Tipado fuerte y estático**: Inferencia de tipos, genéricos, cierres y coincidencia de patrones.
- **Contratos explícitos**: Declara contratos con `spec`; usa `fit` para adaptar tipos a contratos o añadir miembros de extensión.
- **Gestión automática de memoria**: El conteo automático de referencias (ARC) y la recolección de ciclos gestionan la vida útil de los objetos administrados.
- **Interoperabilidad con C**: Llama a bibliotecas de C mediante `extern func` y anotaciones ABI.
- **Herramientas de desarrollo**: Compilación de proyectos, gestión de dependencias, servicios de lenguaje, depuración y extensiones para VS Code y Zed.

## Instalación

Los paquetes oficiales admiten Mac con Apple Silicon y sistemas GNU/Linux con arquitecturas x86-64 y ARM64.

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

Después de instalar, abre una nueva terminal y ejecuta `feng --version` para verificar la instalación. Consulta la [guía de instalación](docs/manual/en/getting-started/installation.md) para elegir una versión o realizar una instalación manual.

## Inicio rápido

Crea un proyecto en un directorio vacío:

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

Reemplaza el contenido del archivo generado `src/main.ff` por:

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

Ejecuta el programa:

```bash
feng run
```

La salida es `Hello, Feng!`. Usa `feng check` para comprobar el proyecto y `feng build --release` para compilar una versión de lanzamiento. Continúa con [Tu primer proyecto](docs/manual/en/getting-started/first-project.md).

## Documentación

- [Especificaciones del lenguaje y las herramientas](docs/specifications/README.md) (en chino)
- [Guía de la biblioteca estándar](docs/manual/en/standard-library/README.md)
- Soporte para editores: [VS Code](editors/feng-vscode/README.md), [Zed](editors/feng-zed/README.md) (en chino)
- [Documentación de ingeniería](docs/engineering/README.md) (en chino)

## Compilar desde el código fuente

Instala Clang, Make y Git LFS. En macOS también se necesitan las Xcode Command Line Tools.

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

El compilador se encuentra en `build/bin/feng`. Ejecuta `make test` para realizar todas las pruebas de regresión: `test/` verifica el compilador y el entorno de ejecución, mientras que `fcts/` verifica el comportamiento del lenguaje.

## Licencia

[MIT](LICENSE)
