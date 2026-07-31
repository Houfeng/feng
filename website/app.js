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
      label.textContent = copied ? '已复制' : '请重试';
      if (status) {
        status.textContent = copied ? '安装命令已复制' : '复制失败，请重试';
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
setupCodeTabs();
