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
    const code = container.querySelector('code');

    if (!button || !label || !code) {
      return;
    }

    button.addEventListener('click', async () => {
      const original = label.textContent;
      const copied = await copyCommand(code.textContent.trim());
      label.textContent = copied ? '已复制' : '请重试';

      window.setTimeout(() => {
        label.textContent = original;
      }, 1400);
    });
  });
}

setupCommandCopying();
