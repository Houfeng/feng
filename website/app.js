/** Copies text through the legacy selection API when Clipboard API access is unavailable. */
function copyWithFallback(text) {
  const helper = document.createElement('textarea');
  helper.value = text;
  helper.setAttribute('readonly', '');
  helper.style.position = 'fixed';
  helper.style.opacity = '0';
  document.body.appendChild(helper);
  helper.select();

  let copied = false;
  try {
    copied = document.execCommand('copy');
  } catch (_) {
    copied = false;
  }

  helper.remove();
  return copied;
}

/** Copies an installation command with the modern API and falls back when needed. */
async function copyCommand(command) {
  if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
    try {
      await navigator.clipboard.writeText(command);
      return true;
    } catch (_) {
    }
  }

  return copyWithFallback(command);
}

/** Connects each installation command to its adjacent copy button. */
function setupCommandCopying() {
  const isEnglish = document.documentElement.lang.startsWith('en');
  const feedback = isEnglish
    ? {
        copied: 'Copied',
        retry: 'Try again',
        copiedStatus: 'Install command copied',
        retryStatus: 'Copy failed. Please try again',
      }
    : {
        copied: '已复制',
        retry: '请重试',
        copiedStatus: '安装命令已复制',
        retryStatus: '复制失败，请重试',
      };

  document.querySelectorAll('[data-command]').forEach((container) => {
    const button = container.querySelector('[data-copy-command]');
    const label = container.querySelector('[data-copy-label]');
    const status = container.querySelector('[data-copy-status]');
    const code = container.querySelector('code');

    if (!button || !label || !code) {
      return;
    }

    const originalLabel = label.textContent;
    let resetTimer;

    button.addEventListener('click', async () => {
      window.clearTimeout(resetTimer);
      if (status) {
        status.textContent = '';
      }
      const copied = await copyCommand(code.textContent.trim());
      label.textContent = copied ? feedback.copied : feedback.retry;
      if (status) {
        status.textContent = copied ? feedback.copiedStatus : feedback.retryStatus;
      }

      resetTimer = window.setTimeout(() => {
        label.textContent = originalLabel;
        if (status) {
          status.textContent = '';
        }
      }, 1400);
    });
  });
}

/** Closes the native small-window navigation after a destination is selected. */
function setupNavigationMenu() {
  const menu = document.querySelector('[data-site-menu]');

  if (!menu) {
    return;
  }

  menu.addEventListener('click', (event) => {
    if (event.target.closest('a')) {
      menu.removeAttribute('open');
    }
  });

  menu.addEventListener('toggle', () => {
    if (!menu.open) {
      menu.querySelectorAll('[data-language-menu]').forEach((languageMenu) => {
        languageMenu.removeAttribute('open');
      });
    }
  });
}

/** Closes one language menu and optionally returns focus to its trigger. */
function closeLanguageMenu(menu, shouldFocus) {
  if (!menu.open) {
    return;
  }

  menu.removeAttribute('open');
  if (shouldFocus) {
    menu.querySelector('[data-language-trigger]')?.focus();
  }
}

/** Enhances native language menus with mutual exclusion and dismissal behavior. */
function setupLanguageMenus() {
  const menus = Array.from(document.querySelectorAll('[data-language-menu]'));

  if (menus.length === 0) {
    return;
  }

  menus.forEach((menu) => {
    menu.addEventListener('toggle', () => {
      if (!menu.open) {
        return;
      }

      menus.forEach((candidate) => {
        if (candidate !== menu) {
          closeLanguageMenu(candidate, false);
        }
      });
    });
  });

  document.addEventListener('pointerdown', (event) => {
    menus.forEach((menu) => {
      if (!menu.contains(event.target)) {
        closeLanguageMenu(menu, false);
      }
    });
  });

  document.addEventListener('keydown', (event) => {
    if (event.key !== 'Escape') {
      return;
    }

    const openMenu = menus.find((menu) => menu.open);
    if (openMenu) {
      event.preventDefault();
      closeLanguageMenu(openMenu, true);
    }
  });

  const smallViewport = window.matchMedia('(max-width: 720px)');
  const handleViewportChange = () => {
    menus.forEach((menu) => closeLanguageMenu(menu, false));
  };

  if (typeof smallViewport.addEventListener === 'function') {
    smallViewport.addEventListener('change', handleViewportChange);
  } else {
    smallViewport.addListener(handleViewportChange);
  }
}

const THEME_STORAGE_KEY = 'feng-theme';
const VALID_THEMES = new Set(['light', 'dark']);

/** Resolves the active theme from document state and then the system preference. */
function getActiveTheme() {
  const documentTheme = document.documentElement.dataset.theme;
  if (VALID_THEMES.has(documentTheme)) {
    return documentTheme;
  }

  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

/** Synchronizes every visible theme control with the active theme. */
function syncThemeControls(theme) {
  const isEnglish = document.documentElement.lang.startsWith('en');

  document.querySelectorAll('[data-theme-option]').forEach((button) => {
    const option = button.dataset.themeOption;
    const optionLabel = isEnglish
      ? (option === 'dark' ? 'dark' : 'light')
      : (option === 'dark' ? '深色' : '浅色');
    const accessibleLabel = isEnglish ? `Use ${optionLabel} mode` : `使用${optionLabel}模式`;
    button.setAttribute('aria-pressed', String(option === theme));
    button.setAttribute('aria-label', accessibleLabel);
    button.setAttribute('title', accessibleLabel);
  });
}

/** Applies a theme, updates browser chrome, and optionally persists the choice. */
function applyTheme(theme, shouldPersist) {
  if (!VALID_THEMES.has(theme)) {
    return;
  }

  document.documentElement.dataset.theme = theme;
  const themeColor = document.querySelector('[data-theme-color]');
  if (themeColor) {
    themeColor.setAttribute('content', theme === 'dark' ? '#111311' : '#f7f8f5');
  }

  if (shouldPersist) {
    document.documentElement.dataset.themeSource = 'manual';
    try {
      window.localStorage.setItem(THEME_STORAGE_KEY, theme);
      document.documentElement.dataset.themeSource = 'saved';
    } catch (_) {
    }
  }

  syncThemeControls(theme);
}

/** Enables persistent theme switching and keeps system and cross-tab changes synchronized. */
function setupThemeSwitching() {
  const buttons = Array.from(document.querySelectorAll('[data-theme-option]'));

  if (buttons.length === 0) {
    return;
  }

  applyTheme(getActiveTheme(), false);

  buttons.forEach((button) => {
    button.addEventListener('click', () => {
      applyTheme(button.dataset.themeOption, true);
    });
  });

  const colorScheme = window.matchMedia('(prefers-color-scheme: dark)');
  const handleSystemThemeChange = (event) => {
    if (document.documentElement.dataset.themeSource === 'system') {
      applyTheme(event.matches ? 'dark' : 'light', false);
    }
  };

  if (typeof colorScheme.addEventListener === 'function') {
    colorScheme.addEventListener('change', handleSystemThemeChange);
  } else {
    colorScheme.addListener(handleSystemThemeChange);
  }

  window.addEventListener('storage', (event) => {
    if (event.key !== THEME_STORAGE_KEY) {
      return;
    }

    if (VALID_THEMES.has(event.newValue)) {
      document.documentElement.dataset.themeSource = 'saved';
      applyTheme(event.newValue, false);
      return;
    }

    document.documentElement.dataset.themeSource = 'system';
    applyTheme(colorScheme.matches ? 'dark' : 'light', false);
  });
}

/** Activates one example tab and synchronizes its ARIA state with the panels. */
function activateCodeTab(container, selectedTab, shouldFocus) {
  const tabs = Array.from(container.querySelectorAll('[data-code-tab]'));
  const panels = Array.from(container.querySelectorAll('[data-code-panel]'));
  const selectedPanelId = selectedTab.getAttribute('href').slice(1);

  tabs.forEach((tab) => {
    const isSelected = tab === selectedTab;
    tab.setAttribute('aria-selected', String(isSelected));
    tab.setAttribute('tabindex', isSelected ? '0' : '-1');
  });

  panels.forEach((panel) => {
    panel.hidden = panel.id !== selectedPanelId;
  });

  if (shouldFocus) {
    selectedTab.focus();
  }
}

/** Enhances example links into keyboard-accessible tabs while preserving a no-script fallback. */
function setupCodeTabs() {
  document.querySelectorAll('[data-code-tabs]').forEach((container) => {
    const tabList = container.querySelector('[data-code-tab-list]');
    const tabs = Array.from(container.querySelectorAll('[data-code-tab]'));
    const panels = Array.from(container.querySelectorAll('[data-code-panel]'));

    if (!tabList || tabs.length === 0 || panels.length === 0) {
      return;
    }

    tabList.setAttribute('role', 'tablist');
    tabs.forEach((tab) => {
      const panelId = tab.getAttribute('href').slice(1);
      tab.setAttribute('role', 'tab');
      tab.setAttribute('aria-controls', panelId);
    });
    panels.forEach((panel) => {
      const tab = tabs.find((candidate) => candidate.getAttribute('href') === `#${panel.id}`);
      panel.setAttribute('role', 'tabpanel');
      if (tab) {
        panel.setAttribute('aria-labelledby', tab.id);
      }
    });

    container.classList.add('code-tabs--enhanced');
    const initialTab = tabs.find((tab) => tab.hash === window.location.hash) || tabs[0];
    activateCodeTab(container, initialTab, false);

    tabs.forEach((tab, index) => {
      tab.addEventListener('click', (event) => {
        event.preventDefault();
        activateCodeTab(container, tab, false);
      });

      tab.addEventListener('keydown', (event) => {
        let nextIndex;
        if (event.key === 'ArrowRight') {
          nextIndex = (index + 1) % tabs.length;
        } else if (event.key === 'ArrowLeft') {
          nextIndex = (index - 1 + tabs.length) % tabs.length;
        } else if (event.key === 'Home') {
          nextIndex = 0;
        } else if (event.key === 'End') {
          nextIndex = tabs.length - 1;
        } else {
          return;
        }

        event.preventDefault();
        activateCodeTab(container, tabs[nextIndex], true);
      });
    });
  });
}

setupCommandCopying();
setupNavigationMenu();
setupLanguageMenus();
setupThemeSwitching();
setupCodeTabs();
