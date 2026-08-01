# feng 依赖管理机制

本文档定义第五阶段的依赖管理工作流: registry 选择、发行包随附包来源、全局缓存、`feng deps` 子命令行为、构建前依赖准备以及本地路径依赖的递归处理。

依赖值本身的语义归属仍以 [feng-package.md](./feng-package.md) 为准: `[dependencies]` 中的值要么是精确版本,要么是本地路径。

## 1. 精确版本包来源

精确版本依赖优先通过 registry 获取 `.fb` 分发包。registry 按以下优先级选择:

1. 当前项目 `feng.fm` 中 `[registry]` 节的 `url`
2. 全局配置 `~/.feng/config.fm` 中 `[registry]` 节的 `url`

编译器发行包可以按
[Feng 分发与安装方案](../engineering/feng-release-and-install.md#4-压缩包目录结构)
在顶层 `pkg/` 中随附精确版本 `.fb`。随附包路径固定为:

```text
<feng-install-root>/pkg/<name>-<version>.fb
```

`pkg/` 是所有精确版本包共用的备用安装来源,不针对具体包名增加特殊处理。依赖管理器
使用现有 Feng 可执行文件绝对路径定位安装根,按请求坐标组成固定文件名; 不扫描
`pkg/`,也不增加新的安装根环境变量。

当 cache 不可复用时,来源选择规则如下:

1. 若已配置 registry,先按 registry 的稳定路径查找包。
2. registry 明确不存在对应包时,回退到发行包 `pkg/`。
3. 若未配置 registry,直接查找发行包 `pkg/`。
4. registry 或 `pkg/` 中存在候选文件但候选文件非法、不可读取或无法完成安装时,
   报告对应来源错误,不继续使用更低优先级来源覆盖该错误。
5. 所有可用来源均未找到对应包时,报告 `<name>@<version>` 找不到。

全局配置示例:

```fm
[registry]
url: "https://packages.example.com/feng"
```

`url` 既可指向远程基地址,也可指向本地 registry 根目录。当前实现要求它最终能解析出 `packages/<name>-<version>.fb` 这一稳定路径。

## 2. 全局缓存

- 全局缓存根目录固定为 `~/.feng/cache`
- 每个精确版本包的缓存文件名固定为 `<name>-<version>.fb`
- `feng deps remove` 只修改当前项目 `feng.fm`,不删除全局缓存
- cache 文件存在、校验合法且未指定 `--force` 时,直接复用 cache,不访问 registry
  或 `pkg/`
- `feng deps install --force` 忽略可复用 cache,按 §1 的来源优先级重新安装并覆盖
  对应 cache 文件

从 registry 获取精确版本依赖时,来源路径固定为:

```bash
${registry}/packages/${name}-${version}.fb
```

从 registry 或 `pkg/` 获取的候选包必须在发布到 cache 前完成合法性校验,至少验证
该文件可作为 `.fb` 打开、内置 `feng.fm` 可解析为合法 bundle manifest,且包名与
版本和请求坐标完全一致。安装使用 cache 目录中的临时文件与原子替换; 失败不得留下
新的半成品 cache。

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

- 若值是精确版本,命令写入 `feng.fm` 后立即确保该包已安装到全局缓存
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
- 对所有精确版本依赖检查 cache; 若 cache 不可复用,按 §1 的来源优先级安装到
  `~/.feng/cache`
- 未配置 registry 时直接查找发行包 `pkg/`; 已配置 registry 时仅在 registry
  明确不存在对应包时回退 `pkg/`
- 候选包必须在原子发布到 cache 前按 §2 完成合法性校验; 校验失败时必须明确指出
  依赖坐标、候选来源及非法原因
- 从 cache 中读取已安装 `.fb` 的内置 `feng.fm`,按相同规则递归安装其中的精确
  版本传递依赖
- 对所有本地路径依赖做目标合法性校验,但不构建本地项目
- 若传递依赖中同一包名出现不同精确版本,立即报冲突错误

## 5. 构建前依赖准备

`feng build`、`feng check`、`feng run` 与 `feng pack` 在进入编译前都必须执行统一的依赖准备流程:

1. 对当前项目执行 `feng deps install`,确保精确版本依赖均已进入全局 cache
2. 递归解析本地路径依赖,必要时先构建本地 `target: "lib"` 项目并拿到对应 `.fb`
3. 展开完整依赖图,同时读取每个 `.fb` 内的 `feng.fm` 继续展开其传递依赖
4. 检测版本冲突与本地项目循环依赖
5. 将锁定后的依赖图展平为确定的 `.fb` 路径列表,传给编译器的 `--pkg`

构建工具传给编译器的是全局 cache 或本地依赖产物中的确定 `.fb` 路径,不是包名、
版本号或 registry 信息。发行包 `pkg/` 只参与安装来源选择,不得作为编译器直接扫描
或消费的包来源。

## 6. 诊断要求

第五阶段的依赖管理必须给出明确诊断,至少覆盖以下错误:

- 未配置 registry 且发行包 `pkg/` 未找到对应包时,诊断必须包含失败的依赖坐标
  `<name>@<version>` 和期望的随附包路径
- 已配置 registry 且 registry 与 `pkg/` 均未找到对应包时,诊断必须包含依赖坐标
  和已经检查的 registry
- `pkg/` 候选存在但非法时,诊断必须包含依赖坐标、候选文件路径和具体非法原因
- 本地路径不存在,或目标既不是 `.fb` 也不是有效项目
- 本地依赖校验失败时,诊断中必须同时包含依赖名、声明值以及具体失败原因; 若底层错误发生在目标 `.fb` 或目标项目 `feng.fm`,诊断中还应保留对应目标路径,便于定位
- 本地路径依赖键与目标包名不一致
- 本地项目依赖不是 `target: "lib"`
- 同一包名在依赖图中解析出多个精确版本
- 本地项目依赖图出现循环
- cache、随附包读取或 registry 下载失败
- 远程下载失败时,诊断中必须同时包含失败的依赖坐标 `<name>@<version>`、来源 registry 或下载 URL 以及底层失败原因
- 候选包不合法时,诊断中必须同时包含失败的依赖坐标 `<name>@<version>`、来源路径
  与具体非法原因
- 精确版本依赖安装诊断应以依赖坐标和失败原因作为主信息,不得把
  `~/.feng/cache` 等缓存目录路径作为用户可见诊断前缀
