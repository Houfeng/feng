# 隼形文件图标草稿

状态：待 Review，尚未接入编辑器。

## 设计约定

- 以 [水墨 Logo 草稿](feng-logo-ink-light-draft.png) 的飞行姿态为基础，将隼形整理为透明背景、单色实心的 SVG。
- 四类文件共用同一主轮廓：短颈、钩喙、对角展开的双翼和短扇尾；省略水墨纹理、眼睛和细碎羽毛。
- 源码使用主标；清单、编译包、符号表分别在右下留白处加入列表、方盒、网格角标，位置不遮挡主标。
- 每类文件提供 `light`、`dark` 两个独立 SVG，均为 64 × 64 画布、透明背景。同类图标的两套资源只有颜色不同，轮廓和角标保持一致。
- 文件名统一为 `feng-{类型}-falcon-{主题}-draft.svg`，与水墨 Logo 草稿的主题命名一致。`light` 表示用于浅色主题，图形为深灰色 `#20242c`；`dark` 表示用于深色主题，图形为白色 `#ffffff`。
- 预览页直接引用对应主题的 SVG，不依赖 CSS 反色。编辑器接入另行确认。
- 本轮归档旧设计资产、整理两套文件图标草稿与预览；插件当前使用的资源及配置保持不变。

## 文件

| 类型 | 浅色主题 | 深色主题 |
| --- | --- | --- |
| `.ff` / `.feng` 源码 | [feng-ff-falcon-light-draft.svg](feng-ff-falcon-light-draft.svg) | [feng-ff-falcon-dark-draft.svg](feng-ff-falcon-dark-draft.svg) |
| `.fm` 项目清单 | [feng-fm-falcon-light-draft.svg](feng-fm-falcon-light-draft.svg) | [feng-fm-falcon-dark-draft.svg](feng-fm-falcon-dark-draft.svg) |
| `.fb` 编译包 | [feng-fb-falcon-light-draft.svg](feng-fb-falcon-light-draft.svg) | [feng-fb-falcon-dark-draft.svg](feng-fb-falcon-dark-draft.svg) |
| `.ft` 符号表 | [feng-ft-falcon-light-draft.svg](feng-ft-falcon-light-draft.svg) | [feng-ft-falcon-dark-draft.svg](feng-ft-falcon-dark-draft.svg) |

## 旧设计归档

原 `designs/` 下的非隼图标（F 字标、四类纸张文件图标、VS Code 展示图标及其原始图片）与对应预览页，统一移至 `designs/legacy/`。旧方案的资产说明及链接见 [VS Code 插件图标设计](../docs/engineering/feng-vscode-icon-design.md)。

## Review

打开 [对照预览](feng-file-icons-falcon-draft.html)，检查四类图标的整体一致性、16 / 20 / 24 / 32 px 下的辨识度，以及浅色、深色文件列表中的显示效果。

也可直接查看 [浏览器渲染预览图](feng-file-icons-falcon-draft-preview.png)。方盒角标在 16 px 下的细节最弱，是本轮 Review 的重点之一。

本轮已检查 8 个 SVG 的 XML 语法、透明背景和配对轮廓；每对图标仅颜色不同。浏览器预览成功加载 8 个独立资源、共 48 个图标实例，未使用 CSS 反色。归档后的旧预览及相关文档链接也已检查。

本轮草稿不替代 [当前已采用的插件展示图标方案](../docs/engineering/feng-vscode-icon-design.md)。
