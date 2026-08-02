/** Copies text using the Clipboard API with a selection-based fallback. */
async function copyText(text) {
  if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (_) {
    }
  }

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

/** Adds accessible copy actions to every generated block of code. */
function setupCodeCopying() {
  const isEnglish = document.documentElement.lang.startsWith('en');
  const labels = isEnglish
    ? { copy: 'Copy', copied: 'Copied', failed: 'Try again' }
    : { copy: '复制', copied: '已复制', failed: '请重试' };

  document.querySelectorAll('.docs-content pre').forEach((pre) => {
    const code = pre.querySelector('code');
    if (!code) {
      return;
    }

    const wrapper = document.createElement('div');
    wrapper.className = 'docs-code-block';
    pre.before(wrapper);
    wrapper.appendChild(pre);

    const button = document.createElement('button');
    button.className = 'docs-code-copy';
    button.type = 'button';
    button.textContent = labels.copy;
    button.setAttribute('aria-label', labels.copy);
    wrapper.appendChild(button);

    let resetTimer;
    button.addEventListener('click', async () => {
      window.clearTimeout(resetTimer);
      const copied = await copyText(code.textContent);
      button.textContent = copied ? labels.copied : labels.failed;
      button.dataset.state = copied ? 'copied' : 'failed';
      resetTimer = window.setTimeout(() => {
        button.textContent = labels.copy;
        delete button.dataset.state;
      }, 1400);
    });
  });
}

/** Keeps the current table-of-contents entry synchronized with document scrolling. */
function setupTableOfContents() {
  const links = Array.from(document.querySelectorAll('.docs-toc a[href^="#"]'));
  const headings = links
    .map((link) => document.getElementById(link.hash.slice(1)))
    .filter(Boolean);
  if (links.length === 0 || headings.length === 0 || !('IntersectionObserver' in window)) {
    return;
  }

  const linksById = new Map(links.map((link) => [link.hash.slice(1), link]));
  const visibleHeadings = new Set();

  /** Marks the first visible section as the current table-of-contents entry. */
  function updateCurrentHeading() {
    const currentHeading = headings.find((heading) => visibleHeadings.has(heading.id));
    links.forEach((link) => link.removeAttribute('aria-current'));
    if (currentHeading) {
      linksById.get(currentHeading.id)?.setAttribute('aria-current', 'location');
    }
  }

  const observer = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
      if (entry.isIntersecting) {
        visibleHeadings.add(entry.target.id);
      } else {
        visibleHeadings.delete(entry.target.id);
      }
    });
    updateCurrentHeading();
  }, {
    rootMargin: '-96px 0px -68% 0px',
    threshold: 0,
  });

  headings.forEach((heading) => observer.observe(heading));
}

/** Closes the compact manual navigation after a chapter is selected. */
function setupMobileManualNavigation() {
  document.querySelectorAll('.docs-mobile-navigation').forEach((navigation) => {
    navigation.addEventListener('click', (event) => {
      if (event.target.closest('a')) {
        navigation.removeAttribute('open');
      }
    });
  });
}

setupCodeCopying();
setupTableOfContents();
setupMobileManualNavigation();
