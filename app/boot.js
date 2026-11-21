/* ==========================================================================
 *  AE -- shell bootstrap
 *
 *  Wires the page chrome (status strip, error overlay, fullscreen toggle) and
 *  hands control to AetherHost, which instantiates the kernel.
 * ======================================================================== */
'use strict';

(function () {
  const canvas = document.getElementById('screen');
  const statusEl = document.getElementById('status');
  const fpsEl = document.getElementById('fps');
  const overlay = document.getElementById('overlay');
  const errTitle = document.getElementById('err-title');
  const errBody = document.getElementById('err-body');
  const errHint = document.getElementById('err-hint');
  const errClose = document.getElementById('err-close');
  const fsBtn = document.getElementById('fullscreen');

  function showError(title, body, hint) {
    errTitle.textContent = title;
    errBody.textContent = body || '';
    errHint.textContent = hint || '';
    overlay.classList.remove('hidden');
    errClose.focus();
  }

  errClose.addEventListener('click', () => overlay.classList.add('hidden'));

  /* Fullscreen matters here: the OS paints to the whole surface and expects a
     generous canvas, and hiding the window chrome makes it feel like a machine
     rather than a web page. */
  function toggleFullscreen() {
    if (!document.fullscreenElement) {
      document.documentElement.requestFullscreen().catch(() => {});
    } else {
      document.exitFullscreen().catch(() => {});
    }
  }
  fsBtn.addEventListener('click', toggleFullscreen);

  document.addEventListener('keydown', (e) => {
    if (e.key === 'F11') { e.preventDefault(); toggleFullscreen(); }
    if (e.key === 'Escape' && document.fullscreenElement) {
      document.exitFullscreen().catch(() => {});
    }
  });

  AetherHost.start({
    canvas,
    status: statusEl,
    fps: fpsEl,
    showError,
  }).then((ok) => {
    if (ok) statusEl.textContent = 'running';
  });
})();
