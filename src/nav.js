(function () {
  const items = [
    ['/panel#general', 'Ogólne'],
    ['/panel#hotkeys', 'Skróty'],
    ['/panel#style', 'Styl OBS', 'obsonly'],
    ['/panel#platforms', 'Platformy'],
    ['/panel#audio', 'Audio'],
    ['/panel#overlay', 'Overlay'],
    ['/panel#logs', 'Logi'],
    ['/live-preview', 'Podgląd'],
    ['/edit-theme', 'Motyw'],
    ['/dock', 'Sterowanie']
  ];
  const nav = document.getElementById('appnav');
  if (!nav) return;
  const path = location.pathname.replace(/\/$/, '') || '/';
  const hash = location.hash || '#general';
  items.forEach(function (item) {
    const a = document.createElement('a');
    a.href = item[0];
    a.textContent = item[1];
    const target = item[0].split('#')[0];
    const itemHash = item[0].indexOf('#') >= 0 ? '#' + item[0].split('#')[1] : '';
    if (itemHash) {
      if (path === '/panel' && hash === itemHash) a.setAttribute('aria-current', 'true');
    } else if (path === target) {
      a.setAttribute('aria-current', 'true');
    }
    // Pages only the OBS plugin can act on stay out of the way elsewhere.
    if (item[2]) {
      a.dataset.only = item[2];
      a.hidden = true;
    }
    nav.appendChild(a);
  });
  if (nav.querySelector('[data-only="obsonly"]')) {
    fetch('/state').then(r => r.json()).then(st => {
      if (st.host !== 'obs') return;
      nav.querySelectorAll('[data-only="obsonly"]').forEach(a => { a.hidden = false; });
    }).catch(() => {});
  }
})();
