# VS Code 插件图标设计

状态：隼形方案已采用并接入，验证完成。

## 文档与资产位置

按 [工程文档职责](README.md)，本文集中定义插件图标的资产来源、接入方式和验证要求。设计源文件保存在 `designs/`，插件实际打包的图标保存在 `editors/feng-vscode/icons/`，旧方案归档至 `designs/legacy/`。

## 插件展示图标

- 使用已确认的 [圆角安全留白版白隼黑底水墨 Logo](../../designs/feng-logo-ink-dark-draft-v2.png)，保留飞行姿态、纹理和安全留白。
- 单独制作带圆角和白色轮廓的插件展示图片，1024 × 1024 设计资源保存为 `designs/feng-vscode-icon-rounded.png`，以 256 × 256 PNG 输出至 `editors/feng-vscode/icons/feng-logo.png`。最终插件图片采用 32 px 圆角、2 px 白色描边，圆角外部透明。圆角和描边按固定几何参数导出，设计资源中的对应尺寸为 128 px、8 px；隼图层直接使用已确认原图，不增加其他内容，不改变隼形、纹理、比例和位置。
- `package.json` 顶层 `icon` 保持为 `icons/feng-logo.png`。此字段只引用一张图片，扩展列表与 Marketplace 共用该资源，不区分 light / dark；固定黑底用于保持白色主体的对比度。
- 展示图标使用 PNG；当前打包入口要求 `icon` 路径不带 `./` 前缀。

## 文件图标

采用 [隼形文件图标设计](../../designs/feng-file-icons-falcon-draft.md) 中的八个 SVG；轮廓、角标、主题颜色与透明背景统一按该文档维护。插件资源名去掉设计源文件的 `falcon`、`draft` 标记，SVG 标题去掉“草稿”，图形保持一致。

| 语言 ID | 文件后缀 | light 资源 | dark 资源 |
| --- | --- | --- | --- |
| `feng` | `.feng` / `.ff` | `icons/feng-ff-light.svg` | `icons/feng-ff-dark.svg` |
| `feng-manifest` | `.fm` | `icons/feng-fm-light.svg` | `icons/feng-fm-dark.svg` |
| `feng-bundle` | `.fb` | `icons/feng-fb-light.svg` | `icons/feng-fb-dark.svg` |
| `feng-symbol-table` | `.ft` | `icons/feng-ft-light.svg` | `icons/feng-ft-dark.svg` |

在 `contributes.languages[].icon` 中分别配置 `light` 与 `dark` 路径。两套资源独立打包，由 VS Code 按主题选择；当文件图标主题未提供对应文件或语言图标时，使用这些语言默认图标。

## 旧资源与设计源文件

- 保留 `designs/` 中已 Review 的 Logo、八个 SVG 和预览文件，继续作为设计来源。
- 旧插件 PNG、SVG 与四类纸张文件图标保存在 [legacy 目录](../../designs/legacy/)。插件曾使用且与现有归档不同的 `feng-logo-v1.png`、`feng-logo.svg` 分别归档为 `feng-vscode-icon-v1.png`、`feng-vscode-logo-v1.svg`。
- 插件 `icons/` 目录只保留当前引用的展示图标和八个文件图标，旧图标不再进入 VSIX。

## 验证要求

- 检查展示 PNG 尺寸、八个 SVG 的有效性，以及与设计源文件的图形一致性；核对旧资源归档内容。
- 在浅色和深色背景检查展示图标，以及文件图标的 16 / 20 / 24 / 32 px 显示效果。
- 图标元数据测试校验四类文件的后缀、各自的 light / dark 路径及资源存在性。
- 按 [VS Code 插件构建](feng-vscode-build.md) 执行插件测试和 VSIX 打包，并检查包内配置与九个图标资源。
- 在沙箱外执行全量 `make test`。

## 验证结果

圆角安全留白版接入验证（2026-10-09）：

- 插件 PNG 为 256 × 256，带透明外角、32 px 圆角和 2 px 白色描边；设计资源内部像素与已确认原图的缩放结果一致，仅边缘增加圆角和轮廓。
- 八个文件图标与 `package.json` 均与接入前逐字节一致；未修改现有测试。
- 插件八组 JavaScript 测试及 `0.1.20` VSIX 打包通过，包内九个图标与工作区逐字节一致。
- 沙箱外 `make test` 通过；ASan/UBSan 与普通构建两个阶段均为标准库 607/607、FCTS 1676/1676。

2026-10-09 验证结果：

- 展示 PNG 为 256 × 256；八个 SVG 的 XML 检查通过，逐像素渲染结果与对应设计稿一致，每对 light / dark 仅颜色不同。十个设计源文件保持不变，七个旧插件图标均有内容一致的归档。
- 浏览器使用插件实际资源完成明暗背景预览，成功加载九个独立资源、共 52 个图标实例；检查了展示图标的 48 / 128 px 和文件图标的 16 / 20 / 24 / 32 px 显示效果。
- 经人工批准更新 `test/icon.test.js`，分别校验 light / dark 路径与八个 SVG；插件全部八组 JavaScript 测试通过，包括真实调试冒烟测试。
- `npm run pack -- --out <产物路径>` 成功生成 `feng-language-0.1.19.vsix`。包内配置与工作区一致，九个图标逐字节一致，不含旧图标。
- 沙箱外 `make test` 通过，退出码为 0；ASan/UBSan 与普通构建两个阶段的标准库测试均为 607/607，FCTS 均为 1676/1676。
- `git diff --check` 及相关文档链接检查通过。
