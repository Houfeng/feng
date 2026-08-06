# Website 首页多语言开发方案

语言范围、文案和文档回退规则以
[`docs/engineering/feng-website.md`](feng-website.md#31-首页语言与文档回退) 为准。

## 修改内容

1. 新增 `website/index-pt-br.html`、`website/index-ja.html` 和
   `website/index-es.html`，基于英文首页翻译页面元信息、可见文案、无障碍文本及交互初始文本。
2. 修改全部首页的桌面端和移动端语言菜单，使其包含五种首页语言并正确标识当前语言。
3. 新增首页的“文档”链接直接指向 `/docs/en/`；简体中文首页继续指向
   `/docs/zh-CN/`，英文首页继续指向 `/docs/en/`。
4. 在 `website/app.js` 顶部增加以 `en`、`zh-CN`、`pt-BR`、`ja`、`es`
   为键的文案字典，包含复制结果和主题切换所需文案。按 `<html lang>` 取值，缺失时回退
   `en`，替换现有英文/非英文判断。
5. 修改 `website/styles.css`，让三个新增语言的 Hero 复用英文 Hero 的字号、间距和移动端规则；其他样式保持不变。
6. 不修改 `website/eleventy.config.js`、`docs/manual/` 和用户手册语言配置。

## 验证

- 五个首页之间能够双向切换，桌面端与移动端菜单一致。
- 新增首页的文案、复制反馈和主题提示使用对应语言。
- 新增首页的文档入口均进入英文文档，页面无横向溢出。
- 执行 `make test`，并按官网主规范完成人工 Review。
