# feng 依赖管理机制

本文档定义第五阶段的依赖管理工作流: registry 选择、全局缓存、`feng deps` 子命令行为、构建前依赖准备以及本地路径依赖的递归处理。

依赖值本身的语义归属仍以 [feng-package.md](./feng-package.md) 为准: `[dependencies]` 中的值要么是精确版本,要么是本地路径。

## 1. registry 选择

远程依赖通过 registry 拉取 `.fb` 分发包。registry 按以下优先级选择:

1. 当前项目 `feng.fm` 中 `[registry]` 节的 `url`
2. 全局配置 `~/.feng/config.fm` 中 `[registry]` 节的 `url`
3. 若以上两者都不存在,对任何远程依赖报错并提示配置 registry

全局配置示例:

```fm
[registry]
url: "https://packages.example.com/feng"
```

`url` 既可指向远程基地址,也可指向本地 registry 根目录。当前实现要求它最终能解析出 `packages/<name>-<version>.fb` 这一稳定路径。

## 2. 全局缓存

- 全局缓存根目录固定为 `~/.feng/cache`
- 每个远程包的缓存文件名固定为 `<name>-<version>.fb`
- `feng deps remove` 只修改当前项目 `feng.fm`,不删除全局缓存
- `feng deps install --force` 会重新拉取远程依赖并覆盖对应缓存文件

从 registry 拉取远程依赖时,目标路径固定为:

```bash
${registry}/packages/${name}-${version}.fb
```

## 3. 本地路径依赖

本地路径依赖用于 monorepo 或多项目协同开发。其识别与目标形式以 [feng-package.md](./feng-package.md) 为准,构建工具进一步遵循以下规则:

- 若目标是 `.fb`,直接将该 bundle 纳入依赖图,并继续读取其内置 `feng.fm` 展开传递依赖
- 若目标是目录或显式 `feng.fm`,构建工具读取目标项目清单并要求 `target: "lib"`
- 对目录或 `feng.fm` 形式的本地项目依赖,`feng build` / `check` / `run` / `pack` 在当前项目构建前都必须递归构建这些依赖,再使用生成的 `.fb`
- 本地项目依赖图按规范化后的 manifest 路径做循环检测; 一旦出现环,立即报错
- 发布为 `.fb` 时,本地路径依赖必须在包内 `feng.fm` 中写回为对应包的精确版本字符串,不得保留本地路径写法

`feng deps install` 不负责构建本地项目依赖,但仍需要验证本地路径是否可解析、名称是否一致、目标是否为有效 `.fb` 或 `target: "lib"` 项目。

## 4. feng deps 命令

`feng deps` 是操作当前项目 `[dependencies]` 的统一入口。

### 4.1 `feng deps add`

```text
feng deps add <pkg-name> <version-or-path> [<path>]
```

- `<pkg-name>`: 依赖包名
- `<version-or-path>`: 精确版本字符串,或以 `./`、`../`、`/` 开头的本地路径
- `[<path>]`: 项目目录或 `feng.fm` 路径; 省略时使用当前目录

行为:

- 若值是远程精确版本,命令写入 `feng.fm` 后立即确保该包已安装到全局缓存
- 若值是本地路径,命令写入 `feng.fm` 前必须校验路径可解析、目标包名匹配,但不触发构建
- 若依赖已存在,新值覆盖旧值

### 4.2 `feng deps remove`

```text
feng deps remove <pkg-name> [<path>]
```

行为:

- 从当前项目 `feng.fm` 的 `[dependencies]` 中移除该项
- 若项目中不存在该依赖,命令报错
- 不删除任何全局缓存文件

### 4.3 `feng deps install`

```text
feng deps install [<path>] [--force]
```

行为:

- 读取当前项目的直接依赖并递归展开完整依赖图
- 对所有远程依赖检查缓存; 若缺失或指定了 `--force`,则从 registry 拉取到 `~/.feng/cache`
- 对所有本地路径依赖做目标合法性校验,但不构建本地项目
- 若传递依赖中同一包名出现不同精确版本,立即报冲突错误

## 5. 构建前依赖准备

`feng build`、`feng check`、`feng run` 与 `feng pack` 在进入编译前都必须执行统一的依赖准备流程:

1. 对当前项目执行 `feng deps install`
2. 递归解析本地路径依赖,必要时先构建本地 `target: "lib"` 项目并拿到对应 `.fb`
3. 展开完整依赖图,同时读取每个 `.fb` 内的 `feng.fm` 继续展开其传递依赖
4. 检测版本冲突与本地项目循环依赖
5. 将锁定后的依赖图展平为确定的 `.fb` 路径列表,传给编译器的 `--pkg`

构建工具传给编译器的是路径,不是包名、版本号或 registry 信息。

## 6. 诊断要求

第五阶段的依赖管理必须给出明确诊断,至少覆盖以下错误:

- 缺少 registry 但遇到远程依赖
- 本地路径不存在,或目标既不是 `.fb` 也不是有效项目
- 本地路径依赖键与目标包名不一致
- 本地项目依赖不是 `target: "lib"`
- 同一包名在依赖图中解析出多个精确版本
- 本地项目依赖图出现循环
- 缓存或下载失败
