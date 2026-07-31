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

setupCommandCopying();
setupNavigationMenu();
