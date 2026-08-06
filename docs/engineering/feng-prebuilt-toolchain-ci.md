# Feng CI 预构建 Toolchain Release 实施方案

> 状态：分步实施中；预构建发布脚本及其 workflow 已实施，拉取端与现有 CI 接线尚未实施。

## 1. 目标

本方案将 GitHub Actions 使用的预构建 toolchain 输入从 Git LFS checkout
迁移为固定 GitHub Release assets，降低日常 CI 对 Git LFS 下载带宽额度的消耗。

本方案只改变 CI 取得预构建 toolchain 的方式。开发者继续通过 Git LFS 使用仓库内的
`toolchain/`，现有构建、测试、component、发行组装、签名、公证、安装验证和发布脚本
继续只读取既定文件系统布局，不感知 toolchain 来自 Git LFS 还是 GitHub Release。

发行包结构、host 平台、目标平台、LLVM 内容和每份发行包包含全部四套 Linux sysroot
的要求，以 [Feng 分发与安装方案](./feng-release-and-install.md) 为唯一主规范，本文不
重复定义。

## 2. 范围

### 2.1 本次范围

- 新增一个独立的预构建发布脚本，将仓库 `toolchain/` 发布为固定、不可变的
  GitHub Release assets。
- 新增一个独立的预构建拉取脚本，根据版本锁下载、校验并恢复 CI 所需的
  `toolchain/` 目录。
- 新增独立的预构建发布 workflow，由 `toolchain-prebuilt/` 前缀 tag 触发。
- 将现有 CI/Release workflow 的 checkout 改为不自动拉取 Git LFS，并在 checkout
  后调用预构建拉取脚本。
- 保持现有构建和发行脚本的接口、职责与产物不变。
- 增加两个脚本及 workflow 接线所需的专项测试，并完成全量回归。

### 2.2 不在本次范围

- 不改变开发者的 Git LFS 使用方式。
- 不修改最终发行包布局、工具链查找规则或交叉编译能力。
- 不修改 compiler、runtime、私有 ABI 或语言行为。
- 不将 toolchain 拆分到独立仓库。
- 不让现有发行脚本解析 prebuilt tag、下载 Release asset 或校验 Release 状态。
- 不在本阶段重写 Git 历史或清理 GitHub 远端 LFS 对象。

## 3. 设计原则

### 3.1 预构建恢复是 CI 依赖准备层

目标调用关系固定为：

```text
checkout（lfs: false）
  -> 调用预构建拉取脚本
  -> 恢复既定 toolchain/ 布局
  -> 调用现有 make / test / release 脚本
```

现有脚本只能通过既定路径消费 toolchain，不得增加 prebuilt tag、Release URL、asset
名称或下载工具等概念。未来若将预构建来源迁移到其他对象存储，只修改预构建发布与
拉取层，不修改构建和发行逻辑。

### 3.2 workflow 只负责触发和编排

workflow 负责：

- tag 触发条件；
- runner、container、权限和系统传输依赖；
- checkout 方式；
- 向脚本传递 GitHub 上下文；
- 调用预构建发布或拉取脚本；
- 调用现有构建、测试和发行入口。

workflow 不得自行执行 toolchain 归档、Release 状态处理、asset 命名、摘要计算、
下载重试、压缩包校验或目录安装。

### 3.3 固定版本、失败关闭

CI 必须使用版本锁中的固定 repository、固定 `toolchain-prebuilt/<version>` tag、固定 asset
名称和固定 SHA-256，不使用 `latest`、浮动 tag 或未校验下载。Release、asset、摘要、
目录结构或平台校验任一不匹配时立即失败，不得回退到 Git LFS 或其他隐式来源。

## 4. Prebuilt Release 契约

### 4.1 Tag

预构建 tag 固定使用：

```text
toolchain-prebuilt/<version>
```

示例：

```text
toolchain-prebuilt/llvm-22.1.8-r1
```

tag 必须指向包含待发布 `toolchain/` 完整内容的确定 commit。发布流程由 push tag
触发，并为同名 tag 创建 GitHub Release；不得先在 GitHub UI 创建同名 Release 后
再触发发布脚本。

已发布 tag 不得复用。内容发生任何变化时必须创建新 tag。

### 4.2 Assets

一个 prebuilt Release 固定包含：

```text
feng-llvm-macos-arm64.tar.gz
feng-llvm-linux-x64-gnu.tar.gz
feng-llvm-linux-arm64-gnu.tar.gz
feng-sysroot-linux-all.tar.gz
manifest.json
SHA256SUMS
```

三份 LLVM asset 分别对应一个 host 平台。sysroot asset 包含主规范定义的全部四套
Linux sysroot：

```text
linux-x64-gnu
linux-x64-musl
linux-arm64-gnu
linux-arm64-musl
```

sysroot 只作为一份公共 asset 发布，最终组装时放入每一份 host 发行包，以保留完整
交叉编译能力。每个 asset 必须小于 GitHub Release 的单文件大小上限。

归档必须保留普通文件权限、可执行位和符号链接，不得解引用符号链接或改变 toolchain
内部相对布局。每个归档只能包含一个规范顶层目录，不得包含绝对路径、`..` 路径、
设备文件或其他不安全成员。

### 4.3 Manifest 与版本锁

`manifest.json` 至少记录：

- prebuilt tag；
- 来源 commit SHA；
- LLVM 版本；
- 三个 host 平台；
- 四个 sysroot 平台；
- 每个 asset 的名称、字节数和 SHA-256。

仓库根目录新增普通 Git 文件：

```text
toolchain-prebuilt.lock
```

版本锁至少记录 Release repository、完整 tag、asset 名称和 SHA-256。发布脚本可以在
`build/` 下生成待审核的版本锁候选文件，但不得自动修改、暂存或提交仓库中的版本锁。

### 4.4 不可变性

仓库必须启用 GitHub immutable releases。发布脚本先创建 draft Release，全部 asset
上传并验证成功后再发布；发布后必须确认 Release 为 immutable。已存在同名 Release
或任一同名 asset 时必须失败，不得覆盖、替换或追加。

## 5. 预构建发布脚本

唯一入口：

```text
scripts/toolchain-prebuilt-publish.sh \
  --repository=<owner/repository> \
  --tag=<toolchain-prebuilt-tag>
```

脚本使用仓库根目录和 `toolchain/` 的规范位置，不依赖调用者当前工作目录。首版不接受
自定义 asset 名称、平台集合或目录布局，避免 workflow 与脚本共同定义发布契约。

脚本职责：

1. 校验参数只指定一次且 tag 符合 `toolchain-prebuilt/<version>`。
2. 校验 tag 与当前 checkout commit 的关系，拒绝发布其他 commit 的内容。
3. 确认 `toolchain/` 中不存在未物化的 Git LFS pointer。
4. 校验三套 host LLVM 和四套 Linux sysroot 的完整性、平台与必要工具。
5. 在仓库 `build/` 下创建本次调用专用 staging，不在 `/tmp` 或 `/private/tmp` 中
   生成并执行产物。
6. 生成四份安全、可复现的归档以及 `manifest.json`、`SHA256SUMS`。
7. 重新读取归档，校验成员、权限、符号链接、摘要和大小。
8. 确认目标 Release 不存在，创建 draft Release 并上传全部 assets。
9. 校验远端 assets 的名称、大小和摘要，发布 Release。
10. 确认 Release 为 immutable，输出版本锁候选文件路径。

任何准备、归档、校验、上传或发布失败都必须返回非零状态。脚本不得修改
`toolchain/`、版本锁、Git index、tag 或 commit。

## 6. 预构建拉取脚本

唯一入口：

```text
scripts/toolchain-prebuilt-fetch.sh \
  [--host-platform=<host-platform>]... \
  [--with-sysroots]
```

`--host-platform` 可重复，取值由主规范的平台矩阵约束。至少指定一个 host 平台或
指定 `--with-sysroots`。脚本固定读取仓库根目录的 `toolchain-prebuilt.lock`，不允许
workflow 覆盖 repository、tag、asset 名称或摘要。

脚本职责：

1. 严格解析并校验版本锁。
2. 根据参数计算唯一、确定的 asset 集合。
3. 下载固定 Release 的固定 assets，不查询或使用 latest Release。
4. 在仓库 `build/` 下创建本次调用专用的下载和解包 staging。
5. 校验下载文件大小和 SHA-256 后再解包。
6. 拒绝绝对路径、`..` 路径、越界符号链接、设备文件和非规范顶层目录。
7. 校验每套 LLVM 的 host 平台、必要工具、权限和布局。
8. 指定 `--with-sysroots` 时，校验全部四套 Linux sysroot，禁止只恢复当前 host
   架构的 sysroot。
9. 全部输入通过后，将所请求内容原子安装到对应 `toolchain/llvm/<host-platform>/`
   和 `toolchain/sysroot/`。
10. 清理 staging 并输出恢复的 prebuilt tag 与平台集合。

脚本只允许替换参数明确选择的规范子目录，不得删除整个仓库根目录、未选择的平台或
其他开发者文件。失败时不得留下部分恢复的最终目录；若替换已开始，必须恢复调用前
目录或保持完整的新目录。

## 7. Workflow 接线

### 7.1 预构建发布 workflow

新增独立 workflow，仅匹配 `toolchain-prebuilt/**` tag：

```yaml
on:
  push:
    tags:
      - "toolchain-prebuilt/**"
```

该 workflow 只包含一个发布 Job：

1. 以 `lfs: true` checkout tag 指向的 commit。
2. 准备 Git LFS、归档工具、GitHub CLI 和 CA 证书等传输依赖。
3. 以最小 `contents: write` 权限调用 `scripts/toolchain-prebuilt-publish.sh`。

创建 GitHub Release 及上传 assets 的逻辑不得展开在 YAML 中。创建 Release 不再创建
新 tag，因此不会递归触发该 workflow。

### 7.2 现有 CI/Release workflow

所有需要仓库 toolchain 的 Job 固定执行：

```text
checkout（lfs: false）
  -> 安装下载与解包依赖
  -> 调用 scripts/toolchain-prebuilt-fetch.sh
  -> 调用原有入口
```

目标参数如下：

| Job | `--host-platform` | `--with-sysroots` | 后续入口 |
|-----|-------------------|-------------------|----------|
| `component_macos` | `macos-arm64` | 否 | `make test`、`release_component.sh` |
| `component_linux` x64 | `linux-x64-gnu` | 是 | `make test`、`release_component.sh` |
| `component_linux` arm64 | `linux-arm64-gnu` | 是 | `make test`、`release_component.sh` |
| `bundled_packages` | `linux-x64-gnu` | 是 | `release_bundled_packages.sh` |
| `assemble` | 三个 host 平台 | 是 | `release_assemble.sh` |

`prepare`、`finalize_macos`、`verify_macos`、`verify_linux` 和 `publish` 不直接读取仓库
toolchain，因此不调用拉取脚本。

现有构建和发行脚本的命令行、输入输出、artifact 和依赖关系保持不变。YAML 不向这些
脚本传入 prebuilt tag 或 Release 信息。

## 8. 错误处理与安全要求

- tag、Release、asset 或版本锁缺失时失败。
- Release 可变、来源 commit 不匹配或存在额外/重复 asset 时失败。
- 网络失败允许有限重试，但摘要或内容校验失败不得重试为其他来源。
- 下载凭证不得写入日志、归档、版本锁或最终 toolchain。
- 公共仓库下载仍须使用固定 HTTPS 地址并校验证书。
- GitHub token 只授予对应 Job 所需的最小权限。
- 发布和拉取脚本中的所有函数均按项目要求添加职责注释；关键校验必须有必要的内部
  注释，不得把复杂 shell 逻辑复制到 workflow。
- 不增加 compiler 或 runtime 开销，不改变运行时行为。

## 9. 测试与验收

### 9.1 发布脚本专项测试

- 非 `toolchain-prebuilt/` tag 被拒绝。
- 缺少 LLVM、sysroot、必要工具或已物化文件时被拒绝。
- 归档保留可执行位与符号链接。
- 不安全归档成员被拒绝。
- manifest、SHA256SUMS、asset 大小和平台集合正确。
- Release 已存在、asset 重名、上传失败或 Release 非 immutable 时失败。
- 失败后不修改源码 toolchain、版本锁、Git index 或现有 Release。

外部 GitHub 状态通过可控替身验证，专项测试不得创建真实 Release。

### 9.2 拉取脚本专项测试

- 单个和多个 host 平台选择正确。
- `--with-sysroots` 恢复且只恢复一份完整的四平台 sysroot 集合。
- 未知平台、重复或冲突参数被拒绝。
- 缺失 asset、下载失败、大小不符和 SHA-256 不符时失败。
- 路径穿越、越界符号链接、设备文件和错误顶层目录被拒绝。
- 最终目录原子替换，失败时不留下部分内容。
- 不读取 latest Release，不回退到 Git LFS。

### 9.3 集成验收

1. 发布首个 `toolchain-prebuilt/<version>` Release，并确认 immutable 和全部摘要。
2. 在三个原生 host 分别从空的已物化 toolchain 状态执行拉取脚本。
3. 比较恢复内容与 tag 对应 Git LFS toolchain，确认目录、文件、权限、符号链接和
   SHA-256 一致。
4. 三个平台分别执行完整 `make test`。
5. 执行一次 `workflow_dispatch` 完整试发，验证三份发行包仍包含 host 对应 LLVM、
   五份 runtime 和全部四套 Linux sysroot。
6. 验证普通 push、pull request 和正式版本发布均不执行 `git lfs fetch`。

全量回归统一命名为 `make test`，必须按项目要求在 Codex 沙箱外执行。测试不得在
`/tmp` 或 `/private/tmp` 中生成并执行编译产物。

## 10. 迁移步骤

1. 更新本方案及主规范中 CI toolchain 输入来源的相关描述。
2. 实现并专项测试 `scripts/toolchain-prebuilt-publish.sh`。
3. 实现并专项测试 `scripts/toolchain-prebuilt-fetch.sh`。
4. 临时恢复 Git LFS 访问，或在持有完整 LFS 对象的受信开发环境准备首版 assets。
5. 创建首个 `toolchain-prebuilt/<version>` tag，由独立 workflow 发布 immutable Release。
6. 人工审核发布脚本生成的版本锁候选，并提交 `toolchain-prebuilt.lock`。
7. 修改现有 CI/Release workflow：checkout 使用 `lfs: false`，随后调用拉取脚本。
8. 完成三个 host 的全量回归和一次完整试发。
9. 确认一段稳定运行期内普通 CI、试发和正式发布均不再消耗 Git LFS 下载带宽。
10. 稳定后再决定是否进入 §11 的历史 LFS 清理阶段。

步骤 5 成功前不得切换现有 CI；步骤 8 完成前不得删除或重写现有 LFS 历史。

## 11. 后续历史 LFS 清理

历史清理不属于两个核心脚本，也不得作为自动 CI Job 运行。后续维护工具最多负责：

- 枚举历史 LFS OID 及大小；
- 标识当前版本仍引用的对象和历史独占对象；
- 生成保留、迁移和待清理清单；
- 在人工明确批准后执行 Git 历史重写；
- 验证重写后保留版本的 LFS 完整性。

GitHub 远端 LFS 对象不会因删除工作树文件、tag、commit 或本地历史而自动释放。完成
历史重写后仍须按 GitHub 支持流程清理远端对象。历史重写会改变 commit SHA，并影响
已有 clone、branch、tag、pull request 和外部链接，必须作为独立破坏性维护事项由
人工决策和执行。

## 12. 完成标准

- `toolchain-prebuilt/**` tag 可通过单一脚本发布固定、完整、不可变的 Release。
- CI 可通过单一脚本恢复所需 host LLVM 和完整 sysroot。
- workflow 中不存在归档、下载、摘要校验或目录安装实现。
- 现有构建、测试和发行脚本不包含 prebuilt 概念且接口不变。
- 三份最终发行包的结构和交叉编译能力与主规范一致。
- 普通 CI、试发和正式发布均不执行 Git LFS fetch。
- 三个平台完整 `make test` 和完整试发通过。
- 文档、实现和测试不存在重复定义或平台特判。
