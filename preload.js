const { contextBridge, ipcRenderer } = require('electron')

contextBridge.exposeInMainWorld('ble', {
  scanStart: () => ipcRenderer.invoke('ble:scanStart'),
  scanStop: () => ipcRenderer.invoke('ble:scanStop'),
  connect: (address) => ipcRenderer.invoke('ble:connect', address),
  disconnect: () => ipcRenderer.invoke('ble:disconnect'),
  interfaceFile: () => ipcRenderer.invoke('ble:interfaceFile'),
  setConfig: (changes) => ipcRenderer.invoke('ble:configSet', changes),
  onConfig: (fn) => ipcRenderer.on('ble:config', (_e, json) => fn(json)),
  pinCheck: (fingerprint) => ipcRenderer.invoke('ble:pinCheck', fingerprint),
  send: (text) => ipcRenderer.invoke('ble:send', text),
  onLog: (fn) => ipcRenderer.on('ble:log', (_e, msg) => fn(msg)),
  onFound: (fn) => ipcRenderer.on('ble:found', (_e, dev) => fn(dev))
})
