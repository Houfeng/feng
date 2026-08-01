# 依赖管理

Feng 项目在 `feng.fm` 的 `[dependencies]` 中声明依赖。

## 精确版本依赖

```text
[dependencies]
std: "0.1.0"
logging: "1.2.3"
```

Feng 当前使用精确版本，不支持版本范围或通配符。添加依赖：

```bash
feng deps add logging 1.2.3
```

删除依赖：

```bash
feng deps remove logging
```

## 本地路径依赖

```text
[dependencies]
shared: "../shared"
```

路径必须以 `./`、`../` 或 `/` 开头，可以指向：

- `.fb` 文件。
- 包含 `feng.fm` 的目录。
- 具体的 `feng.fm` 文件。

依赖键必须与目标包的 `[package].name` 一致。本地项目依赖必须是 `target: "lib"`。

也可以通过命令添加：

```bash
feng deps add shared ../shared
```

## 安装依赖

```bash
feng deps install
feng deps install --force
```

已安装的精确版本包默认从 `~/.feng/cache` 复用。`--force` 会重新安装清单声明的全部依赖。

`build`、`check`、`run` 和 `pack` 都会先执行统一的依赖准备，因此通常不需要手动运行 `deps install`。

## Registry

项目可以覆盖 registry：

```text
[registry]
url: "https://packages.example.com/feng"
```

也可以在 `~/.feng/config.fm` 中配置全局 registry。远程包的稳定路径为：

```text
<registry>/packages/<name>-<version>.fb
```

未配置 registry，或 registry 明确不存在对应包时，工具会按规范检查发行安装中的随附 `pkg/`。

## 传递依赖与冲突

Feng 会递归读取 `.fb` 中的依赖清单并展平依赖图。同一包名解析到不同精确版本、本地项目依赖形成环、包名与路径目标不一致时都会停止构建并报告错误。

发布本地库时，`.fb` 内不会保留本地路径；`pack` 会把它们写回对应包的精确版本。
