# 隼形文件图标草稿

状态：待 Review，尚未接入编辑器。

## 设计约定

- 以 [水墨 Logo 草稿](feng-logo-ink-light-draft.png) 的飞行姿态为基础，将隼形整理为透明背景、单色实心的 SVG。
- 四类文件共用同一主轮廓：短颈、钩喙、对角展开的双翼和短扇尾；省略水墨纹理、眼睛和细碎羽毛。
- 源码使用主标；清单、编译包、符号表分别在右下留白处加入列表、方盒、网格角标，位置不遮挡主标。
- 四个 SVG 均为 64 × 64 画布，使用深色填充。预览页在深色背景上显示它们的白色版本，便于比较；编辑器的明暗主题接入另行确认。
- 本轮仅新增设计草稿与预览，不替换现有图标，不修改编辑器配置。

## 文件

| 类型 | 草稿 |
| --- | --- |
| `.ff` / `.feng` 源码 | [feng-ff-falcon-draft.svg](feng-ff-falcon-draft.svg) |
| `.fm` 项目清单 | [feng-fm-falcon-draft.svg](feng-fm-falcon-draft.svg) |
| `.fb` 编译包 | [feng-fb-falcon-draft.svg](feng-fb-falcon-draft.svg) |
| `.ft` 符号表 | [feng-ft-falcon-draft.svg](feng-ft-falcon-draft.svg) |

## Review

打开 [对照预览](feng-file-icons-falcon-draft.html)，检查四类图标的整体一致性、16 / 20 / 24 / 32 px 下的辨识度，以及浅色、深色文件列表中的显示效果。

也可直接查看 [浏览器渲染预览图](feng-file-icons-falcon-draft-preview.png)。四个 SVG 已通过 XML 语法检查，预览页的 48 个图标实例均成功加载。已检查两种背景下的实际尺寸显示；方盒角标在 16 px 下的细节最弱，是本轮 Review 的重点之一。

本轮草稿不替代 [当前已采用的插件展示图标方案](../docs/engineering/feng-vscode-icon-design.md)。
