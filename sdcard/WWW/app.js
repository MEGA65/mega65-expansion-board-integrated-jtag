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
    window.m65Busy = true;
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
    window.m65Busy = true;
    fetch(url, { method: 'PUT', cache: 'no-store' })
      .then(function (response) {
        return response.text().then(function (text) {
          if (!response.ok) throw new Error(text || response.status);
          location.reload();
        });
      })
      .catch(function (error) {
        window.m65Busy = false;
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
    var row = link;
    while (row && row.tagName && row.tagName.toLowerCase() !== 'tr') row = row.parentNode;
    if (row && row.getAttribute && row.getAttribute('data-kind') === 'PARTIAL') return false;
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
    if (rows[selectedIndex].getAttribute && rows[selectedIndex].getAttribute('data-kind') === 'PARTIAL') return;
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

  // -------------------------------------------------------------------------
  // File management: upload (button or drag and drop), new folder, delete folder
  // -------------------------------------------------------------------------

  // Must match has_core_ext()/has_manifest_ext() in the firmware
  var UPLOAD_EXTS = ['.bit', '.cor', '.m65j', '.sha256'];
  var RMDIR_MAX_ROUNDS = 5000;
  var fileOpBusy = false;
  var fileStatus = null;
  var fileProgress = null;

  function currentDir() {
    var m = /[?&]path=([^&#]*)/.exec(location.search);
    if (!m) return '';
    var p;
    try {
      p = decodeURIComponent(m[1].replace(/\+/g, ' '));
    } catch (e) {
      p = '';
    }
    return p.replace(/^\/+|\/+$/g, '');
  }

  function joinPath(dir, name) {
    return dir ? dir + '/' + name : name;
  }

  function encodePath(path) {
    var parts = path.split('/');
    for (var i = 0; i < parts.length; i++) parts[i] = encodeURIComponent(parts[i]);
    return parts.join('/');
  }

  function hasUploadExt(name) {
    var lower = name.toLowerCase();
    for (var i = 0; i < UPLOAD_EXTS.length; i++) {
      var ext = UPLOAD_EXTS[i];
      if (lower.length > ext.length && lower.slice(-ext.length) === ext) return true;
    }
    return false;
  }

  function badNameReason(name) {
    if (!name) return 'The name is empty.';
    if (name.length > 100) return 'The name is too long.';
    if (/[\/\\:*?"<>|\x00-\x1f]/.test(name)) return 'Names cannot contain / \\ : * ? " < > |';
    if (name.indexOf('..') >= 0) return 'Names cannot contain "..".';
    if (/[ .]$/.test(name) || /^ /.test(name)) return 'Names cannot start with a space or end with a space or dot.';
    return '';
  }

  // Server errors are "CODE Reason\nmessage\n"; show the message part
  function serverMessage(status, text) {
    if (status === 423 && /write grant/i.test(text || '')) {
      return 'The write grant is not active. Press the write-enable button on the board, then try again.';
    }
    var lines = (text || '').split('\n');
    return (lines[1] || lines[0] || ('HTTP ' + status)).trim();
  }

  function existingNames() {
    var rows = document.querySelectorAll ? document.querySelectorAll('tbody tr.entry-row[data-name]') : [];
    var names = {};
    for (var i = 0; i < rows.length; i++) {
      names[rows[i].getAttribute('data-name').toLowerCase()] = true;
    }
    return names;
  }

  function setStatus(text, isError) {
    if (!fileStatus) return;
    fileStatus.textContent = text || '';
    fileStatus.className = 'file-status' + (isError ? ' error' : '');
  }

  function setProgress(fraction) {
    if (!fileProgress) return;
    if (fraction === null) {
      fileProgress.hidden = true;
      return;
    }
    fileProgress.hidden = false;
    fileProgress.firstChild.style.width = Math.round(Math.max(0, Math.min(1, fraction)) * 100) + '%';
  }

  // window.m65Busy pauses the firmware's auto-reload watcher while one of our
  // own multi-step operations runs; we reload ourselves when it is done.
  function setBusy(busy) {
    fileOpBusy = busy;
    window.m65Busy = busy;
    var buttons = document.querySelectorAll ? document.querySelectorAll('.file-tools button') : [];
    for (var i = 0; i < buttons.length; i++) buttons[i].disabled = busy;
  }

  // Keep Enter/arrow keys driving the core list after a button was clicked
  function releaseFocus() {
    if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
    armKeyboardFocus();
  }

  function putRequest(url) {
    return fetch(url, { method: 'PUT', cache: 'no-store' }).then(function (response) {
      return response.text().then(function (text) {
        return { ok: response.ok, status: response.status, text: text };
      });
    });
  }

  function uploadOne(file, path, onProgress, done) {
    var xhr = new XMLHttpRequest();
    xhr.open('PUT', '/files/' + encodePath(path), true);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    if (xhr.upload) {
      xhr.upload.onprogress = function (e) {
        if (e.lengthComputable) onProgress(e.loaded / e.total);
      };
    }
    xhr.onload = function () {
      if (xhr.status >= 200 && xhr.status < 300) done(null);
      else done(serverMessage(xhr.status, xhr.responseText));
    };
    xhr.onerror = function () {
      done('the connection failed (the board may be busy or may have refused the file)');
    };
    xhr.send(file);
  }

  function uploadFiles(fileList) {
    if (fileOpBusy || !fileList || !fileList.length) return;
    var dir = currentDir();
    var names = existingNames();
    var queue = [];
    var skipped = [];
    var replacing = [];

    for (var i = 0; i < fileList.length; i++) {
      var f = fileList[i];
      var reason = badNameReason(f.name);
      if (!reason && !hasUploadExt(f.name)) reason = 'only ' + UPLOAD_EXTS.join(', ') + ' files can be uploaded';
      if (!reason && !f.size) reason = 'the file is empty (folders cannot be uploaded)';
      if (reason) {
        skipped.push(f.name + ': ' + reason);
        continue;
      }
      if (names[f.name.toLowerCase()]) replacing.push(f.name);
      queue.push(f);
    }

    if (replacing.length) {
      var replace = confirm(
        'These files already exist in this folder:\n\n  ' + replacing.join('\n  ') +
        '\n\nOK replaces them. Cancel skips them and uploads the rest.');
      if (!replace) {
        queue = queue.filter(function (file) { return !names[file.name.toLowerCase()]; });
      }
    }

    if (!queue.length) {
      if (skipped.length) alert('Nothing was uploaded.\n\n' + skipped.join('\n'));
      return;
    }

    var failed = [];
    var index = 0;
    setBusy(true);
    window.onbeforeunload = function () { return 'Files are still uploading.'; };

    function next() {
      if (index >= queue.length) {
        window.onbeforeunload = null;
        setProgress(null);
        var problems = skipped.concat(failed);
        if (problems.length) {
          alert((queue.length - failed.length) + ' of ' + queue.length + ' file(s) uploaded.\n\n' + problems.join('\n'));
        }
        location.reload();
        return;
      }
      var file = queue[index];
      var label = (index + 1) + ' of ' + queue.length + ': ' + file.name;
      setStatus('Uploading ' + label);
      setProgress(0);
      uploadOne(file, joinPath(dir, file.name), function (fraction) {
        setProgress(fraction);
        setStatus('Uploading ' + label + ' (' + Math.round(fraction * 100) + '%)');
      }, function (err) {
        if (err) failed.push(file.name + ': ' + err);
        index++;
        next();
      });
    }
    next();
  }

  function newFolder() {
    releaseFocus();
    if (fileOpBusy) return;
    var name = prompt('Name for the new folder:');
    if (name === null) return;
    name = name.replace(/^\s+|\s+$/g, '');
    var reason = badNameReason(name);
    if (reason) {
      alert('Cannot create that folder. ' + reason);
      return;
    }
    setBusy(true);
    setStatus('Creating folder ' + name + '...');
    putRequest('/mkdir?path=' + encodeURIComponent(joinPath(currentDir(), name)))
      .then(function (r) {
        if (!r.ok) throw new Error(serverMessage(r.status, r.text));
        location.reload();
      })
      .catch(function (error) {
        setBusy(false);
        setStatus('');
        alert('Could not create the folder: ' + error.message);
      });
  }

  // The firmware deletes a bounded number of entries per request, so keep
  // asking until it reports DONE
  function deleteFolderContents(url, name) {
    var rounds = 0;
    setBusy(true);
    window.onbeforeunload = function () { return 'A folder is still being deleted.'; };

    function finish(message) {
      window.onbeforeunload = null;
      if (message) alert(message);
      location.reload();
    }

    function step() {
      rounds++;
      setStatus('Deleting folder ' + name + ' and everything in it... (pass ' + rounds + ')');
      putRequest(url + '&recursive=1')
        .then(function (r) {
          if (!r.ok) throw new Error(serverMessage(r.status, r.text));
          if (/^OK RMDIR DONE/.test(r.text)) return finish('');
          if (rounds >= RMDIR_MAX_ROUNDS) throw new Error('gave up after ' + rounds + ' passes');
          step();
        })
        .catch(function (error) {
          finish('Deleting the folder stopped: ' + error.message +
                 '\n\nSome of its contents may already have been deleted.');
        });
    }
    step();
  }

  function deleteFolder(url, name) {
    releaseFocus();
    if (fileOpBusy) return;
    if (!confirm('Delete the folder "' + name + '"?')) return;
    setBusy(true);
    putRequest(url)
      .then(function (r) {
        if (r.ok) {
          location.reload();
          return;
        }
        setBusy(false);
        if (r.status !== 409) throw new Error(serverMessage(r.status, r.text));

        var typed = prompt(
          'WARNING: the folder "' + name + '" is NOT empty.\n\n' +
          'Deleting it will permanently delete EVERY file and folder inside it, ' +
          'including any cores. This cannot be undone.\n\n' +
          'To delete it and everything in it, type the folder name exactly:');
        if (typed === null) return;
        if (typed !== name) {
          alert('The name did not match. Nothing was deleted.');
          return;
        }
        deleteFolderContents(url, name);
      })
      .catch(function (error) {
        setBusy(false);
        alert('Could not delete the folder: ' + error.message);
      });
  }

  function buildFileTools() {
    var table = document.querySelector ? document.querySelector('main table') : null;
    if (!table || !table.parentNode) return;

    var tools = document.createElement('section');
    tools.className = 'file-tools';

    var input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.accept = UPLOAD_EXTS.join(',');
    input.hidden = true;
    input.addEventListener('change', function () {
      uploadFiles(input.files);
      input.value = '';
    });

    var upload = document.createElement('button');
    upload.type = 'button';
    upload.textContent = 'Upload files';
    upload.addEventListener('click', function () {
      releaseFocus();
      if (!fileOpBusy) input.click();
    });

    var mkdir = document.createElement('button');
    mkdir.type = 'button';
    mkdir.textContent = 'New folder';
    mkdir.addEventListener('click', newFolder);

    var hint = document.createElement('span');
    hint.className = 'hint';
    hint.textContent = 'or drop ' + UPLOAD_EXTS.join(' ') + ' files anywhere on this page';

    fileStatus = document.createElement('div');
    fileStatus.className = 'file-status';
    fileStatus.setAttribute('aria-live', 'polite');

    fileProgress = document.createElement('div');
    fileProgress.className = 'file-progress';
    fileProgress.hidden = true;
    fileProgress.appendChild(document.createElement('span'));

    tools.appendChild(upload);
    tools.appendChild(mkdir);
    tools.appendChild(hint);
    tools.appendChild(input);
    tools.appendChild(fileStatus);
    tools.appendChild(fileProgress);
    table.parentNode.insertBefore(tools, table);

    var overlay = document.createElement('div');
    overlay.className = 'drop-overlay';
    overlay.textContent = 'Drop files to upload to /' + currentDir();
    document.body.appendChild(overlay);

    var depth = 0;
    function isFileDrag(e) {
      var types = e.dataTransfer && e.dataTransfer.types;
      if (!types) return false;
      for (var i = 0; i < types.length; i++) if (types[i] === 'Files') return true;
      return false;
    }
    document.addEventListener('dragenter', function (e) {
      if (!isFileDrag(e)) return;
      e.preventDefault();
      depth++;
      if (!fileOpBusy) overlay.classList.add('active');
    });
    document.addEventListener('dragover', function (e) {
      if (!isFileDrag(e)) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = fileOpBusy ? 'none' : 'copy';
    });
    document.addEventListener('dragleave', function (e) {
      if (!isFileDrag(e)) return;
      depth = Math.max(0, depth - 1);
      if (!depth) overlay.classList.remove('active');
    });
    document.addEventListener('drop', function (e) {
      if (!isFileDrag(e)) return;
      e.preventDefault();
      depth = 0;
      overlay.classList.remove('active');
      uploadFiles(e.dataTransfer.files);
    });
  }

  window.launchCoreLink = launchCoreLink;
  window.deleteCore = deleteCore;
  window.deleteFolder = deleteFolder;
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', buildFileTools);
  } else {
    buildFileTools();
  }
  document.addEventListener('keydown', keyHandler);
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', armKeyboardFocus);
  } else {
    armKeyboardFocus();
  }
})();
