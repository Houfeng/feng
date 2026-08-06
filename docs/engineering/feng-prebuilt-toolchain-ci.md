# Feng CI 预构建 Toolchain Release 实施方案

> 状态：分步实施中；预构建发布端已验证，拉取端与现有 CI 接线已实施，待 CI 验证。

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

- 新增一个独立的预构建发布脚本，将仓库 `toolchain/` 压缩并发布为一个
  GitHub Release asset。
- 新增一个独立的预构建拉取脚本，选择最新已发布的
  `toolchain-prebuilt/*` Release asset，校验并恢复 CI 所需的 `toolchain/`
  目录。
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

workflow 不得自行执行 toolchain 归档、asset 命名、下载、解压或目录安装。

### 3.3 最新预构建、失败关闭

CI 固定通过 GitHub matching refs API 枚举当前 repository 中
`toolchain-prebuilt/` 前缀的 tag，读取对应 Release 的 `publishedAt`，选择发布时间最新的
tag 并下载其 Release asset。该规则不枚举普通 Feng Release；“最新”只在
`toolchain-prebuilt/` 命名空间内计算，不得使用 GitHub 仓库级 `latest`。Release、
asset 或目录结构任一不匹配时立即失败，不得回退到 Git LFS 或其他隐式来源。

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

tag 必须指向包含待发布 `toolchain/` 完整内容的确定 commit。开发者可以在本地创建并
push tag，也可以在 GitHub 页面创建 tag 或同名 Release。tag push 统一触发 workflow；
workflow 不创建或修改 tag。

已发布 tag 不得复用。内容发生任何变化时必须创建新 tag。

### 4.2 Assets

一个 prebuilt Release 固定包含一个 asset：

```text
toolchain-prebuilt-<version>.tar.gz
```

归档包含完整的 `toolchain/` 目录，包括三套 host LLVM 和全部四套 Linux sysroot。
归档必须保留普通文件权限、可执行位、符号链接和 `toolchain/` 内部相对布局。

### 4.3 发布规则

发布脚本只生成归档，不创建 Release、tag 或其他远端对象。workflow 的发布步骤在同名
Release 已存在时，先将其设置为 prerelease，再上传归档；不存在时为触发它的现有 tag
创建 prerelease 并上传归档。Toolchain Release 统一使用 prerelease，不占用 Feng 的
仓库级 Latest。
已有同名 asset 时失败，不得覆盖。

## 5. 预构建发布脚本

唯一入口：

```text
scripts/toolchain-prebuilt-publish.sh
```

脚本从 GitHub Actions 提供的 `GITHUB_REF_NAME` 读取 tag，通过 `GITHUB_OUTPUT`
返回归档路径，使用仓库根目录和 `toolchain/` 的规范位置，不接受命令行参数，也不
依赖调用者当前工作目录。

脚本职责：

1. 校验 tag 上下文以及 `toolchain/` 目录存在。
2. 在仓库 `build/toolchain-prebuilt/` 下生成固定名称的单个归档。
3. 将完整 `toolchain/` 目录压缩到归档中。
4. 通过 GitHub Actions step output 返回归档路径。

任何准备或归档失败都必须返回非零状态。脚本不得访问 GitHub API，不得修改
`toolchain/`、Git index、tag 或 commit。

## 6. 预构建拉取脚本

唯一入口：

```text
scripts/toolchain-prebuilt-fetch.sh
```

脚本不接受命令行参数，从 GitHub Actions 提供的 `GITHUB_REPOSITORY`
读取 repository，并由 `GH_TOKEN` 向 GitHub API 进行只读访问。

脚本职责：

1. 通过 matching refs API 查询当前 repository 的 `toolchain-prebuilt/` 前缀 tag，
   读取各 tag 对应 Release 的 `publishedAt` 并选择最新一个。
2. 根据 tag 推导唯一 asset 名称并下载，不使用仓库级 latest Release。
3. 在仓库 `build/` 下创建本次调用专用的下载和解包 staging。
4. 解包前拒绝绝对路径、`..` 路径和非规范顶层目录。
5. 解包后拒绝越界符号链接、设备文件，并校验三套 host LLVM 和
   全部四套 Linux sysroot。
6. 全部输入通过后整体替换 checkout 中的 `toolchain/` 目录。
7. 清理 staging 并输出恢复的 prebuilt tag。

脚本不得删除仓库根目录或其他开发者文件。失败时不得留下部分恢复的最终目录；若
替换已开始，必须恢复调用前目录或保持完整的新目录。

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
2. 调用 `scripts/toolchain-prebuilt-publish.sh` 生成归档。
3. 以最小 `contents: write` 权限发布归档：同名 Release 已存在时通过
   `gh release edit --prerelease` 设置为 prerelease 后上传；不存在时通过
   `gh release create --prerelease` 为当前已有 tag 创建 prerelease 并上传。

workflow 和脚本均不得创建或移动 tag。

### 7.2 现有 CI/Release workflow

所有需要仓库 toolchain 的 Job 固定执行：

```text
checkout（lfs: false）
  -> 安装下载与解包依赖
  -> 调用 scripts/toolchain-prebuilt-fetch.sh
  -> 调用原有入口
```

`prepare`、`finalize_macos`、`verify_macos`、`verify_linux` 和 `publish` 不直接读取仓库
toolchain，因此不调用拉取脚本。

现有 `release.yml` 的 tag push 触发必须排除 `toolchain-prebuilt/**`；预构建 tag 只触发
本节定义的独立 workflow，不触发现有 Feng 构建与发行任务。

现有构建和发行脚本的命令行、输入输出、artifact 和依赖关系保持不变。YAML 不向这些
脚本传入 prebuilt tag 或 Release 信息。

## 8. 错误处理与安全要求

- Release 或 asset 缺失时失败。
- 内容校验失败时不得重试为其他来源。
- 下载凭证不得写入日志、归档或最终 toolchain。
- 公共仓库下载仍须使用固定 HTTPS 地址并校验证书。
- GitHub token 只授予对应 Job 所需的最小权限。
- 发布和拉取脚本中的所有函数均按项目要求添加职责注释，不得把 shell 逻辑复制到
  workflow。
- 不增加 compiler 或 runtime 开销，不改变运行时行为。

## 9. 测试与验收

### 9.1 发布脚本专项测试

- 非 `toolchain-prebuilt/` tag 被拒绝。
- 缺少 `toolchain/` 目录时失败。
- 归档包含完整 `toolchain/`，并保留可执行位与符号链接。
- 失败后不修改源码 toolchain、Git index、tag 或 commit。

workflow 发布步骤通过可控替身验证已有 Release 和只有 tag 两种路径，两种路径都必须
将 Toolchain Release 标记为 prerelease，且不得在专项测试中创建真实 Release。

### 9.2 拉取脚本专项测试

- 完整 `toolchain/` 目录被恢复。
- 三套 host LLVM 和全部四套 Linux sysroot 均存在。
- 没有符合前缀的 tag、Release 查询失败、缺失 asset 或下载失败时失败。
- 普通 Release 数量不影响 Toolchain tag 的发现。
- 路径穿越、越界符号链接、设备文件和错误顶层目录被拒绝。
- 最终目录原子替换，失败时不留下部分内容。
- 仅在 `toolchain-prebuilt/` 命名空间内选择最新 Release，不回退到 Git LFS。

### 9.3 集成验收

1. 发布首个 `toolchain-prebuilt/<version>` Release，并确认归档可下载和解压。
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
5. 在本地或 GitHub 页面创建首个 `toolchain-prebuilt/<version>` tag，由独立
   workflow 生成并发布预构建归档。
6. 修改现有 CI/Release workflow：checkout 使用 `lfs: false`，随后调用拉取脚本。
7. 完成三个 host 的全量回归和一次完整试发。
8. 确认一段稳定运行期内普通 CI、试发和正式发布均不再消耗 Git LFS 下载带宽。
9. 稳定后再决定是否进入 §11 的历史 LFS 清理阶段。

步骤 5 成功前不得切换现有 CI；步骤 7 完成前不得删除或重写现有 LFS 历史。

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

- `toolchain-prebuilt/**` tag 可触发独立 workflow，通过单一脚本生成包含完整
  `toolchain/` 的归档并发布到同名 Release。
- Toolchain Release 标记为 prerelease；拉取端仅在 `toolchain-prebuilt/` 命名空间内
  选择最新已发布版本。
- CI 可通过单一脚本恢复完整 `toolchain/`。
- workflow 中不存在归档、下载、摘要校验或目录安装实现。
- 现有构建、测试和发行脚本不包含 prebuilt 概念且接口不变。
- 三份最终发行包的结构和交叉编译能力与主规范一致。
- 普通 CI、试发和正式发布均不执行 Git LFS fetch。
- 三个平台完整 `make test` 和完整试发通过。
- 文档、实现和测试不存在重复定义或平台特判。
