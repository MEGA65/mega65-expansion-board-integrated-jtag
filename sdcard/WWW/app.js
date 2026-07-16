(function () {
  'use strict';

  var filterText = '';
  var selectedIndex = -1;

  function ensureFilterBadge() {
    var badge = document.getElementById('filterBadge');
    if (!badge && document.body) {
      badge = document.createElement('div');
      badge.id = 'filterBadge';
      badge.className = 'filter-badge';
      badge.hidden = true;
      document.body.appendChild(badge);
    }
    return badge;
  }

  function ensureLaunchOverlay() {
    var overlay = document.getElementById('launchOverlay');
    if (!overlay && document.body) {
      overlay = document.createElement('div');
      overlay.id = 'launchOverlay';
      overlay.className = 'launch-overlay';
      overlay.setAttribute('aria-live', 'polite');
      overlay.innerHTML =
        '<div class="launch-panel">' +
          '<div class="launch-title">Warp Core</div>' +
          '<div class="launch-subtitle">Beaming core down via JTAG...</div>' +
          '<div class="launch-progress"><span id="launchProgress"></span></div>' +
        '</div>';
      document.body.appendChild(overlay);
    }
    return overlay;
  }

  function showLaunchOverlay() {
    var overlay = ensureLaunchOverlay();
    var bar = document.getElementById('launchProgress');
    if (!overlay || !bar) return;
    overlay.style.position = 'fixed';
    overlay.style.inset = '0';
    overlay.style.zIndex = '1000';
    overlay.style.display = 'flex';
    overlay.style.alignItems = 'center';
    overlay.style.justifyContent = 'center';
    overlay.style.background = 'radial-gradient(circle at 50% 44%, rgba(124, 8, 8, 0.38), rgba(2, 4, 8, 0.92) 58%), rgba(2, 4, 8, 0.88)';
    overlay.classList.add('active');
    bar.style.transition = 'none';
    bar.style.width = '0%';
    bar.offsetHeight;
    bar.style.transition = 'width 10s linear';
    bar.style.width = '100%';
  }

  function afterPaint(callback) {
    if (window.requestAnimationFrame) {
      requestAnimationFrame(function () {
        setTimeout(callback, 60);
      });
    } else {
      setTimeout(callback, 60);
    }
  }

  function launchCore(url) {
    showLaunchOverlay();
    afterPaint(function () {
      fetch(url, { method: 'GET', cache: 'no-store', headers: { 'Accept': 'text/plain' } })
        .then(function (response) {
          return response.text().then(function (text) {
            if (!response.ok) throw new Error(text || response.status);
            location.reload();
          });
        })
        .catch(function () {
          var overlay = document.getElementById('launchOverlay');
          if (overlay) overlay.classList.add('failed');
          setTimeout(function () {
            location.reload();
          }, 1200);
        });
    });
  }

  function deleteCore(url, name) {
    if (!confirm('Delete ' + name + '?')) return;
    fetch(url, { method: 'PUT', cache: 'no-store' })
      .then(function (response) {
        return response.text().then(function (text) {
          if (!response.ok) throw new Error(text || response.status);
          location.reload();
        });
      })
      .catch(function (error) {
        alert('Delete failed: ' + error.message);
      });
  }

  function launchTarget(link) {
    var resolver = document.createElement('a');
    resolver.href = link.href;
    return {
      path: resolver.pathname,
      request: resolver.pathname + resolver.search
    };
  }

  function launchCoreLink(link) {
    var target = launchTarget(link);
    if (target.path !== '/jtag') return true;
    launchCore(target.request);
    return false;
  }

  function entryRows() {
    var rows = document.querySelectorAll ? document.querySelectorAll('tbody tr.entry-row') : [];
    var out = [];
    for (var i = 0; i < rows.length; i++) {
      if (rows[i].querySelector && rows[i].querySelector('a.start')) out.push(rows[i]);
    }
    return out;
  }

  function rowSearchText(row) {
    var name = row.querySelector('.core-name');
    var meta = row.querySelector('.meta');
    return (
      ((name && name.textContent) || '') + ' ' +
      ((meta && meta.textContent) || '') + ' ' +
      (row.getAttribute('data-kind') || '')
    ).toLowerCase();
  }

  function visibleRows() {
    var rows = entryRows();
    var out = [];
    for (var i = 0; i < rows.length; i++) {
      if (rows[i].style.display !== 'none') out.push(rows[i]);
    }
    return out;
  }

  function clearSelection() {
    var rows = entryRows();
    for (var i = 0; i < rows.length; i++) rows[i].classList.remove('selected');
  }

  function setSelectedIndex(index) {
    var rows = visibleRows();
    clearSelection();
    if (!rows.length) {
      selectedIndex = -1;
      return;
    }
    selectedIndex = ((index % rows.length) + rows.length) % rows.length;
    rows[selectedIndex].classList.add('selected');
    if (rows[selectedIndex].scrollIntoView) rows[selectedIndex].scrollIntoView({ block: 'nearest' });
  }

  function updateFilterBadge(count) {
    var badge = ensureFilterBadge();
    if (!badge) return;
    if (!filterText) {
      badge.hidden = true;
      badge.textContent = '';
      return;
    }
    badge.hidden = false;
    badge.textContent = 'Filter: ' + filterText + ' (' + count + ')';
  }

  function applyFilter(selectFirst) {
    var needle = filterText.toLowerCase();
    var rows = entryRows();
    var current = visibleRows()[selectedIndex] || null;
    for (var i = 0; i < rows.length; i++) {
      rows[i].style.display = (!needle || rowSearchText(rows[i]).indexOf(needle) >= 0) ? '' : 'none';
    }
    var visible = visibleRows();
    updateFilterBadge(visible.length);
    if (!visible.length) {
      clearSelection();
      selectedIndex = -1;
    } else if (selectFirst) {
      setSelectedIndex(0);
    } else {
      var keep = visible.indexOf ? visible.indexOf(current) : -1;
      setSelectedIndex(keep >= 0 ? keep : 0);
    }
  }

  function activateSelected() {
    var rows = visibleRows();
    if (!rows.length) return;
    if (selectedIndex < 0 || selectedIndex >= rows.length) setSelectedIndex(0);
    rows = visibleRows();
    var link = rows[selectedIndex].querySelector('a.start');
    if (!link) return;
    var target = launchTarget(link);
    if (target.path === '/jtag') {
      launchCore(target.request);
    } else {
      window.location.href = link.href;
    }
  }

  function keyTargetIsTextInput(target) {
    if (!target || !target.tagName) return false;
    var tag = target.tagName.toLowerCase();
    return tag === 'input' || tag === 'textarea' || tag === 'select' || target.isContentEditable;
  }

  function keyHandler(event) {
    if (keyTargetIsTextInput(event.target) || event.ctrlKey || event.metaKey || event.altKey) return;
    if (event.key === 'ArrowDown') {
      setSelectedIndex(selectedIndex < 0 ? 0 : selectedIndex + 1);
      event.preventDefault();
    } else if (event.key === 'ArrowUp') {
      setSelectedIndex(selectedIndex < 0 ? visibleRows().length - 1 : selectedIndex - 1);
      event.preventDefault();
    } else if (event.key === 'Enter') {
      activateSelected();
      event.preventDefault();
    } else if (event.key === 'Escape') {
      filterText = '';
      applyFilter(false);
      event.preventDefault();
    } else if (event.key === 'Backspace') {
      if (filterText) {
        filterText = filterText.slice(0, -1);
        applyFilter(true);
        event.preventDefault();
      }
    } else if (event.key && event.key.length === 1) {
      filterText += event.key;
      applyFilter(true);
      event.preventDefault();
    }
  }

  function armKeyboardFocus() {
    if (!document.body || !document.body.focus) return;
    if (!document.body.hasAttribute('tabindex')) document.body.setAttribute('tabindex', '-1');
    try {
      document.body.focus({ preventScroll: true });
    } catch (e) {
      document.body.focus();
    }
  }

  window.launchCoreLink = launchCoreLink;
  window.deleteCore = deleteCore;
  document.addEventListener('keydown', keyHandler);
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', armKeyboardFocus);
  } else {
    armKeyboardFocus();
  }
})();
