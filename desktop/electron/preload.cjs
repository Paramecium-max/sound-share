const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('miplayBridge', {
  getSidecarConfig: () => ipcRenderer.invoke('sidecar:getConfig'),
  getSidecarStatus: () => ipcRenderer.invoke('sidecar:getStatus'),
  restartSidecar: () => ipcRenderer.invoke('sidecar:restart'),
  onSidecarStatus: (cb) => {
    const listener = (_ev, status) => cb(status);
    ipcRenderer.on('sidecar:status', listener);
    return () => ipcRenderer.removeListener('sidecar:status', listener);
  },
  openWavFile: () => ipcRenderer.invoke('dialog:openWav'),
  openLrcFile: () => ipcRenderer.invoke('dialog:openLrc'),
  dispatchMediaControl: (action, positionMs) => ipcRenderer.invoke('media:dispatchControl', action, positionMs),
  onSystemTrack: (cb) => {
    const listener = (_ev, track) => cb(track);
    ipcRenderer.on('media:systemTrack', listener);
    return () => ipcRenderer.removeListener('media:systemTrack', listener);
  },
  platform: process.platform,
});
