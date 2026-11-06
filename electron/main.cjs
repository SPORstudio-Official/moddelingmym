'use strict';

const { app, BrowserWindow, Menu } = require('electron');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

const appHtml = pathToFileURL(path.join(__dirname, '..', 'index.html')).href;
let mainWindow;

function createWindow() {
  mainWindow = new BrowserWindow({
    title: 'Wonderworks',
    width: 1280,
    height: 900,
    minWidth: 760,
    minHeight: 700,
    show: false,
    backgroundColor: '#f6f6f1',
    autoHideMenuBar: true,
    icon: path.join(__dirname, '..', 'assets', 'icon.ico'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true,
      webSecurity: true,
      devTools: !app.isPackaged
    }
  });

  Menu.setApplicationMenu(null);
  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  mainWindow.webContents.on('will-navigate', (event, url) => {
    if (url !== appHtml && !url.startsWith(`${appHtml}#`)) event.preventDefault();
  });
  mainWindow.once('ready-to-show', () => mainWindow.show());
  mainWindow.on('closed', () => { mainWindow = null; });
  mainWindow.loadFile(path.join(__dirname, '..', 'index.html'));
}

app.whenReady().then(() => {
  app.setAppUserModelId('com.sporstudio.wonderworks');
  createWindow();
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});

app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
app.on('web-contents-created', (_event, contents) => {
  contents.on('will-attach-webview', event => event.preventDefault());
});
