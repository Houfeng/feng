import { readFile, readdir, rm } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  I18nPlugin,
  InputPathToUrlTransformPlugin,
} from "@11ty/eleventy";
import Shiki from "@shikijs/markdown-it";

const WEBSITE_DIRECTORY = path.dirname(fileURLToPath(import.meta.url));
const MANUAL_DIRECTORY = path.resolve(WEBSITE_DIRECTORY, "../docs/manual");
const DOCS_OUTPUT_DIRECTORY = path.join(WEBSITE_DIRECTORY, "docs");
const FENG_GRAMMAR_PATH = path.resolve(
  WEBSITE_DIRECTORY,
  "../editors/feng-vscode/syntaxes/feng.tmLanguage.json",
);
const SUPPORTED_LANGUAGES = ["en", "zh-CN"];

const FENG_GRAMMAR = {
  ...JSON.parse(await readFile(FENG_GRAMMAR_PATH, "utf8")),
  name: "feng",
};
const SYNTAX_HIGHLIGHT = await Shiki({
  langs: [FENG_GRAMMAR, "bash", "json"],
  theme: "github-dark-default",
});

const NAVIGATION = [
  {
    key: "getting-started",
    pages: [
      "getting-started/installation",
      "getting-started/quick-start",
      "getting-started/first-project",
    ],
  },
  {
    key: "language",
    pages: [
      "language/values-and-bindings",
      "language/types",
      "language/expressions",
      "language/functions",
      "language/control-flow",
      "language/pattern-matching",
      "language/user-defined-types",
      "language/contracts-and-fit",
      "language/generics",
      "language/error-handling",
      "language/modules-and-visibility",
    ],
  },
  {
    key: "projects",
    pages: [
      "projects/project-structure",
      "projects/build-and-run",
      "projects/dependencies",
      "projects/testing-and-debugging",
    ],
  },
  {
    key: "standard-library",
    pages: [
      "standard-library",
      "standard-library/collections",
      "standard-library/text",
      "standard-library/filesystem-and-io",
      "standard-library/time-and-math",
      "standard-library/platform-and-process",
    ],
  },
  {
    key: "tooling-and-interop",
    pages: [
      "tooling/cli",
      "tooling/editor",
      "tooling/formatter",
      "interop/c-interop",
      "feng-style",
    ],
  },
];

const INTERFACE_TEXT = {
  en: {
    siteName: "Feng Documentation",
    manual: "User Manual",
    home: "Home",
    install: "Install",
    docs: "Docs",
    extension: "Extension",
    openSource: "Open Source",
    language: "Language",
    current: "Current",
    colorMode: "Color mode",
    light: "Light",
    dark: "Dark",
    useLight: "Use light mode",
    useDark: "Use dark mode",
    menu: "Toggle navigation menu",
    skip: "Skip to main content",
    chapters: "Manual contents",
    onThisPage: "On this page",
    previous: "Previous",
    next: "Next",
    copy: "Copy",
    copied: "Copied",
    copyFailed: "Try again",
    sectionLabels: {
      "getting-started": "Getting Started",
      language: "Language Guide",
      projects: "Project Development",
      "standard-library": "Standard Library",
      "tooling-and-interop": "Tooling and Interoperability",
    },
  },
  "zh-CN": {
    siteName: "Feng 文档",
    manual: "用户手册",
    home: "首页",
    install: "安装",
    docs: "文档",
    extension: "插件",
    openSource: "开源",
    language: "语言",
    current: "当前",
    colorMode: "颜色模式",
    light: "浅色",
    dark: "深色",
    useLight: "使用浅色模式",
    useDark: "使用深色模式",
    menu: "切换导航菜单",
    skip: "跳到正文",
    chapters: "手册目录",
    onThisPage: "本页内容",
    previous: "上一篇",
    next: "下一篇",
    copy: "复制",
    copied: "已复制",
    copyFailed: "请重试",
    sectionLabels: {
      "getting-started": "入门",
      language: "语言指南",
      projects: "项目开发",
      "standard-library": "标准库",
      "tooling-and-interop": "工具与互操作",
    },
  },
};

/** Returns every Markdown file below a directory. */
async function findMarkdownFiles(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = await Promise.all(entries.map(async (entry) => {
    const entryPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      return findMarkdownFiles(entryPath);
    }
    return entry.name.endsWith(".md") ? [entryPath] : [];
  }));
  return files.flat();
}

/** Removes Markdown formatting used in a page's first-level heading. */
function normalizeTitle(value) {
  return value
    .replace(/`([^`]+)`/g, "$1")
    .replace(/\[([^\]]+)\]\([^\)]+\)/g, "$1")
    .trim();
}

/** Converts a manual source path to its language and stable page key. */
function getDocumentIdentity(inputPath) {
  const relativePath = path.relative(MANUAL_DIRECTORY, inputPath).replaceAll(path.sep, "/");
  const [language, ...pathParts] = relativePath.split("/");
  const markdownPath = pathParts.join("/").replace(/\.md$/, "");
  const pageKey = markdownPath === "README"
    ? ""
    : markdownPath.endsWith("/README")
      ? markdownPath.slice(0, -"/README".length)
      : markdownPath;
  return { language, pageKey };
}

/** Produces the public URL for a localized manual page. */
function getDocumentUrl(language, pageKey) {
  const suffix = pageKey ? `${pageKey}/` : "";
  return `/docs/${language}/${suffix}`;
}

/** Reads localized page titles from the manual's Markdown headings. */
async function loadDocumentMetadata() {
  const metadata = Object.fromEntries(SUPPORTED_LANGUAGES.map((language) => [language, {}]));
  const files = await findMarkdownFiles(MANUAL_DIRECTORY);

  await Promise.all(files.map(async (file) => {
    const { language, pageKey } = getDocumentIdentity(file);
    if (!SUPPORTED_LANGUAGES.includes(language)) {
      return;
    }

    const source = await readFile(file, "utf8");
    const heading = source.match(/^#\s+(.+)$/m);
    if (!heading) {
      throw new Error(`Manual page is missing a level-one heading: ${file}`);
    }
    metadata[language][pageKey] = {
      key: pageKey,
      title: normalizeTitle(heading[1]),
      url: getDocumentUrl(language, pageKey),
    };
  }));

  return metadata;
}

const DOCUMENTS = await loadDocumentMetadata();

/** Verifies that navigation and localized page sets stay complete. */
function validateDocuments() {
  const orderedKeys = ["", ...NAVIGATION.flatMap((section) => section.pages)];
  for (const language of SUPPORTED_LANGUAGES) {
    const documentKeys = Object.keys(DOCUMENTS[language]);
    for (const key of orderedKeys) {
      if (!DOCUMENTS[language][key]) {
        throw new Error(`Missing ${language} manual page for navigation key: ${key || "README"}`);
      }
    }
    for (const key of documentKeys) {
      if (!orderedKeys.includes(key)) {
        throw new Error(`Manual page is missing from navigation: ${language}/${key}`);
      }
    }
  }
}

validateDocuments();

/** Creates localized navigation data without duplicating document titles. */
function createNavigation(language) {
  const text = INTERFACE_TEXT[language];
  return {
    home: DOCUMENTS[language][""],
    sections: NAVIGATION.map((section) => ({
      key: section.key,
      label: text.sectionLabels[section.key],
      pages: section.pages.map((pageKey) => DOCUMENTS[language][pageKey]),
    })),
  };
}

/** Resolves previous and next pages using the shared manual order. */
function createSequence(language, pageKey) {
  const keys = ["", ...NAVIGATION.flatMap((section) => section.pages)];
  const index = keys.indexOf(pageKey);
  return {
    previous: index > 0 ? DOCUMENTS[language][keys[index - 1]] : null,
    next: index >= 0 && index < keys.length - 1 ? DOCUMENTS[language][keys[index + 1]] : null,
  };
}

/** Escapes text before inserting generated navigation into HTML. */
function escapeHtml(value) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

/** Produces stable, unique heading identifiers for page anchors. */
function createSlug(value, usedSlugs) {
  const base = value
    .toLocaleLowerCase()
    .normalize("NFKC")
    .replace(/<[^>]+>/g, "")
    .replace(/&[a-z]+;/gi, " ")
    .replace(/[^\p{Letter}\p{Number}]+/gu, "-")
    .replace(/^-|-$/g, "") || "section";
  let slug = base;
  let suffix = 2;
  while (usedSlugs.has(slug)) {
    slug = `${base}-${suffix}`;
    suffix += 1;
  }
  usedSlugs.add(slug);
  return slug;
}

/** Adds heading anchors and renders the page table of contents at build time. */
function renderTableOfContents(content) {
  const startMarker = "<!-- DOCS_CONTENT_START -->";
  const endMarker = "<!-- DOCS_CONTENT_END -->";
  const startIndex = content.indexOf(startMarker);
  const endIndex = content.indexOf(endMarker);
  if (startIndex < 0 || endIndex < 0 || endIndex <= startIndex) {
    return content;
  }

  const contentStart = startIndex + startMarker.length;
  const documentContent = content.slice(contentStart, endIndex);
  const usedSlugs = new Set();
  const headings = [];
  const renderedDocumentContent = documentContent.replace(/<h([23])>([\s\S]*?)<\/h\1>/g, (match, level, headingHtml) => {
    const plainText = headingHtml.replace(/<[^>]+>/g, "").replace(/&amp;/g, "&").trim();
    const slug = createSlug(plainText, usedSlugs);
    headings.push({ level: Number(level), title: plainText, slug });
    return `<h${level} id="${slug}"><a class="docs-heading-anchor" href="#${slug}">${headingHtml}</a></h${level}>`;
  });

  const tableOfContents = headings.length === 0
    ? ""
    : `<ol>${headings.map((heading) => (
      `<li class="docs-toc-level-${heading.level}"><a href="#${heading.slug}">${escapeHtml(heading.title)}</a></li>`
    )).join("")}</ol>`;

  const rendered = `${content.slice(0, contentStart)}${renderedDocumentContent}${content.slice(endIndex)}`;
  return rendered.replace("<!-- DOCS_TOC -->", tableOfContents);
}

export default function configureEleventy(eleventyConfig) {
  eleventyConfig.addPlugin(I18nPlugin, {
    defaultLanguage: "en",
    errorMode: "strict",
  });
  eleventyConfig.addPlugin(InputPathToUrlTransformPlugin);

  eleventyConfig.ignores.add("../docs/manual/README.md");
  eleventyConfig.addPassthroughCopy({ "docs-src/assets": "docs/assets" });
  eleventyConfig.addPassthroughCopy({ "docs-src/docs-index.html": "docs/index.html" });
  eleventyConfig.addWatchTarget("docs-src");
  eleventyConfig.addWatchTarget(FENG_GRAMMAR_PATH);
  eleventyConfig.amendLibrary("md", (markdownLibrary) => {
    markdownLibrary.use(SYNTAX_HIGHLIGHT);
  });

  eleventyConfig.on("eleventy.before", async () => {
    await rm(DOCS_OUTPUT_DIRECTORY, { recursive: true, force: true });
  });

  eleventyConfig.addGlobalData("layout", "docs.njk");
  eleventyConfig.addGlobalData("eleventyComputed", {
    lang: (data) => getDocumentIdentity(data.page.inputPath).language,
    docKey: (data) => getDocumentIdentity(data.page.inputPath).pageKey,
    title: (data) => {
      const identity = getDocumentIdentity(data.page.inputPath);
      return DOCUMENTS[identity.language][identity.pageKey].title;
    },
    permalink: (data) => {
      const identity = getDocumentIdentity(data.page.inputPath);
      return `${getDocumentUrl(identity.language, identity.pageKey)}index.html`;
    },
    alternateUrl: (data) => {
      const identity = getDocumentIdentity(data.page.inputPath);
      const alternateLanguage = identity.language === "en" ? "zh-CN" : "en";
      return getDocumentUrl(alternateLanguage, identity.pageKey);
    },
    interface: (data) => INTERFACE_TEXT[getDocumentIdentity(data.page.inputPath).language],
    docsNavigation: (data) => createNavigation(getDocumentIdentity(data.page.inputPath).language),
    sequence: (data) => {
      const identity = getDocumentIdentity(data.page.inputPath);
      return createSequence(identity.language, identity.pageKey);
    },
    homepageUrl: (data) => getDocumentIdentity(data.page.inputPath).language === "en" ? "/" : "/index-zh.html",
  });

  eleventyConfig.addTransform("docs-table-of-contents", function addTableOfContents(content) {
    if (this.page.outputPath?.endsWith(".html") && this.page.url?.startsWith("/docs/")) {
      return renderTableOfContents(content);
    }
    return content;
  });

  return {
    templateFormats: ["md"],
    markdownTemplateEngine: false,
    htmlTemplateEngine: "njk",
    dir: {
      input: "../docs/manual",
      includes: "../../website/docs-src/_includes",
      output: ".",
    },
  };
}
