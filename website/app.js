const body = document.body;

function setCurrentYear() {
  const yearNode = document.querySelector('[data-year]');
  if (yearNode) {
    yearNode.textContent = String(new Date().getFullYear());
  }
}

function setupHeaderSurface() {
  const header = document.querySelector('.site-header');
  const hero = document.querySelector('.hero');

  if (!header || !hero) {
    return;
  }

  const syncSurface = () => {
    const heroTop = hero.getBoundingClientRect().top;
    const headerBottom = header.getBoundingClientRect().bottom;
    const surfaced = heroTop <= headerBottom;

    header.classList.toggle('is-surfaced', surfaced);
  };

  syncSurface();
  window.addEventListener('scroll', syncSurface, { passive: true });
  window.addEventListener('resize', syncSurface);
}

function setupMenu() {
  const toggle = document.querySelector('[data-menu-toggle]');
  const nav = document.querySelector('[data-nav]');

  if (!toggle || !nav) {
    return;
  }

  nav.dataset.open = 'false';
  toggle.addEventListener('click', () => {
    const next = nav.dataset.open !== 'true';
    nav.dataset.open = next ? 'true' : 'false';
    toggle.setAttribute('aria-expanded', next ? 'true' : 'false');
  });

  nav.querySelectorAll('a').forEach((link) => {
    link.addEventListener('click', () => {
      nav.dataset.open = 'false';
      toggle.setAttribute('aria-expanded', 'false');
    });
  });
}

function setupSampleTabs() {
  const tabs = Array.from(document.querySelectorAll('[data-sample-tab]'));
  const panels = Array.from(document.querySelectorAll('[data-sample-panel]'));
  const copyButton = document.querySelector('[data-copy-sample]');

  if (tabs.length === 0 || panels.length === 0) {
    return;
  }

  const activate = (target) => {
    tabs.forEach((tab) => {
      const selected = tab.dataset.sampleTab === target;
      tab.classList.toggle('is-active', selected);
      tab.setAttribute('aria-selected', selected ? 'true' : 'false');
    });

    panels.forEach((panel) => {
      const active = panel.dataset.samplePanel === target;
      panel.classList.toggle('is-active', active);
      panel.hidden = !active;
    });
  };

  tabs.forEach((tab) => {
    tab.addEventListener('click', () => activate(tab.dataset.sampleTab));
  });

  if (!copyButton) {
    return;
  }

  copyButton.addEventListener('click', async () => {
    const activePanel = panels.find((panel) => !panel.hidden);
    const codeNode = activePanel ? activePanel.querySelector('code') : null;
    if (!codeNode) {
      return;
    }

    const content = codeNode.textContent || '';
    let copied = false;

    if (navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
      try {
        await navigator.clipboard.writeText(content);
        copied = true;
      } catch (_) {
      }
    }

    if (!copied) {
      const helper = document.createElement('textarea');
      helper.value = content;
      helper.setAttribute('readonly', 'true');
      helper.style.position = 'absolute';
      helper.style.left = '-9999px';
      document.body.appendChild(helper);
      helper.select();
      try {
        copied = document.execCommand('copy');
      } catch (_) {
        copied = false;
      }
      document.body.removeChild(helper);
    }

    const original = copyButton.textContent;
    copyButton.textContent = copied ? 'Copied' : 'Copy failed';
    window.setTimeout(() => {
      copyButton.textContent = original;
    }, 1200);
  });
}

function setupReveal() {
  const sections = Array.from(document.querySelectorAll('[data-reveal]'));
  if (sections.length === 0) {
    return;
  }

  if (!('IntersectionObserver' in window)) {
    sections.forEach((section) => section.classList.add('is-visible'));
    return;
  }

  const observer = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
      if (!entry.isIntersecting) {
        return;
      }
      entry.target.classList.add('is-visible');
      observer.unobserve(entry.target);
    });
  }, {
    rootMargin: '0px 0px -10% 0px',
    threshold: 0.16
  });

  sections.forEach((section) => observer.observe(section));
}

body.dataset.jsReady = 'true';
setCurrentYear();
setupHeaderSurface();
setupMenu();
setupSampleTabs();
setupReveal();
