/* ==========================================================================
 *  AetherOS -- Electron main process
 *
 *  Owns one window and nothing else.  The window is just a frame around a
 *  <canvas>; the operating system itself is the WebAssembly module the window
 *  loads.  There is no Node integration in the renderer: the OS is untrusted
 *  code as far as the shell is concerned, exactly as it would be on real
 *  hardware.
 * ======================================================================== */
'use strict';

const { app, BrowserWindow, Menu, screen, shell } = require('electron');
const path = require('path');
const fs = require('fs');

const INDEX = path.join(__dirname, '..', 'app', 'index.html');
const WASM = path.join(__dirname, '..', 'app', 'aether.wasm');

let win = null;

/**
 * Size the window so the emulated 1280x720 surface is visible in full without
 * being upscaled past 1:1.  If the display is smaller than that, shrink to fit
 * rather than opening a window that hangs off the edge -- the failure mode that
 * made the standalone Windows build appear to do nothing on small screens.
 */
function windowSize() {
  const { workAreaSize } = screen.getPrimaryDisplay();
  const chromeH = 26 + 24;               /* title bar + status strip          */
  const maxW = Math.max(640, workAreaSize.width - 80);
  const maxH = Math.max(360, workAreaSize.height - 80 - chromeH);
  const scale = Math.min(1, maxW / 1280, maxH / 720);
  return {
    width: Math.round(1280 * scale),
    height: Math.round(720 * scale) + chromeH,
  };
}

function createWindow() {
  const { width, height } = windowSize();

  win = new BrowserWindow({
    width,
    height,
    /* Never let the minimum size exceed the display, or the window would be
       forced larger than the screen -- the same off-screen failure the
       standalone Win32 build had. Also centre explicitly. */
    minWidth: Math.min(640, width),
    minHeight: Math.min(400, height),
    center: true,
    backgroundColor: '#05070c',
    title: 'AetherOS',
    show: false,
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      backgroundThrottling: false,   /* the OS must keep ticking when unfocused */
    },
  });

  win.once('ready-to-show', () => {
    win.show();
    win.focus();
  });

  /* A missing wasm image is the single most likely startup failure, so say so
     plainly instead of showing a blank window. */
  if (!fs.existsSync(WASM)) {
    win.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
      `<body style="background:#05070c;color:#c8d6e5;font:14px sans-serif;padding:40px">
         <h2 style="color:#7fd4ff">aether.wasm is missing</h2>
         <p>The Electron shell loaded, but the operating system image was not found at:</p>
         <pre style="color:#8fa3ba">${WASM.replace(/</g, '&lt;')}</pre>
         <p>Build it with <code style="color:#9fe0a0">npm run build:wasm</code>.</p>
       </body>`));
    return;
  }

  win.loadFile(INDEX);

  /* Never navigate away from the OS, and send real links to the user's browser. */
  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: 'deny' };
  });

  win.on('closed', () => { win = null; });
}

/* One window, one machine.  A minimal menu keeps the standard accelerators
   (reload, devtools, zoom, quit) available without a visible menu bar. */
function buildMenu() {
  const template = [
    { role: 'fileMenu' },
    { role: 'viewMenu' },
    { role: 'windowMenu' },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

app.whenReady().then(() => {
  buildMenu();
  createWindow();
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
