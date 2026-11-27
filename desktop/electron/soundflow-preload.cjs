const { contextBridge, ipcRenderer } = require('electron');

const bridge = { platform: process.platform };
const methods = [
  'toggleReceiver', 'applyConfig', 'controlAction', 'setVolume', 'setAudioSyncOffset',
  'setRippleSensitivity', 'setVisualDemand', 'importLocalLrc', 'clearLyricCache',
  'senderRpcAsync', 'setSenderTabActive', 'pickSenderWav', 'pickSenderLrc',
  'openSenderLyricsStage', 'closeSenderLyricsStage', 'onJsReady', 'onUiState',
  'onRenderStats', 'fullscreenChanged', 'modalChanged', 'themeApplied',
  'requestDlnaVisualization', 'releasePortConflict', 'openBatterySettings',
  'openAutoStartSettings', 'runRootTask', 'fetchLogs', 'exportLogs',
  'openNotificationAccessSettings', 'setTrayEnabled', 'checkFirewall',
];

for (const method of methods) {
  bridge[method] = (...args) => { ipcRenderer.send('soundflow:request', method, args); };
}
bridge.onHostEvent = (callback) => {
  const listener = (_event, message) => callback(message);
  ipcRenderer.on('soundflow:event', listener);
  return () => ipcRenderer.removeListener('soundflow:event', listener);
};
contextBridge.exposeInMainWorld('SoundFlowBridge', bridge);
