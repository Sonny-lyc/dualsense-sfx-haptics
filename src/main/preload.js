const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  // ============ DualSense Controller API ============
  getControllers: () => ipcRenderer.invoke('get-controllers'),
  
  sendVibration: (devicePath, leftMotor, rightMotor, duration) =>
    ipcRenderer.invoke('send-vibration', devicePath, leftMotor, rightMotor, duration),
  
  stopVibration: (devicePath) =>
    ipcRenderer.invoke('stop-vibration', devicePath),
  
  getControllerInfo: (devicePath) =>
    ipcRenderer.invoke('get-controller-info', devicePath),
  
  watchControllers: (callback) => {
    ipcRenderer.on('controllers-updated', (event, controllers) => {
      callback(controllers);
    });
    ipcRenderer.send('watch-controllers');
  },
  
  removeWatchControllers: () => {
    ipcRenderer.removeAllListeners('controllers-updated');
  },
});
