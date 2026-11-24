const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('miplayBridge', {
  getSidecarConfig: () => ipcRenderer.invoke('sidecar:getConfig'),
  openWavFile: () => ipcRenderer.invoke('dialog:openWav'),
  platform: process.platform,
});
