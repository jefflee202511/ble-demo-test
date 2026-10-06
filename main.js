const { app, BrowserWindow, ipcMain } = require('electron')
const path = require('path')
const fs = require('fs')

// ---- 로그: 창이 뜨기 전 메시지는 모아 뒀다가 전달 ----
let win = null
let pageReady = false
const pendingLogs = []
function sendLog (msg) {
  console.log('[BLE]', msg)
  if (win && !win.isDestroyed() && pageReady) win.webContents.send('ble:log', msg)
  else pendingLogs.push(msg)
}

// ---- FFI: native/build 의 공유 라이브러리 로드 ----
// 함수와 콜백의 모양은 native/ble_ffi.json 에서 읽어 자동으로 구성한다 (ffi_loader.js)
let ble = null       // 로드 성공 시 dll 함수 묶음
let bridge = null
let lastConfigJson = null // dll 이 마지막으로 알려 준 현재 설정 (json 문자열)
let loadError = null       // dll 로드 실패 시 그 이유
let loadedLibPath = null   // 실제로 로드한 dll 경로 (생성된 json 파일이 같은 폴더에 있음)
let koffi = null

function loadBle () {
  koffi = require('koffi')
  const { loadBridge } = require('./ffi_loader')

  const libName = { win32: 'ble_bridge.dll', darwin: 'libble_bridge.dylib' }[process.platform] || 'libble_bridge.so'
  const candidates = [
    path.join(__dirname, 'native', 'build', libName),
    path.join(__dirname, 'native', 'build', 'Release', libName),
    path.join(__dirname, 'native', 'build', 'Debug', libName)
  ]
  const libPath = candidates.find((p) => fs.existsSync(p))
  if (!libPath) throw new Error('라이브러리를 찾을 수 없습니다: ' + candidates[0])
  sendLog('라이브러리 로드: ' + libPath)

  loadedLibPath = libPath
  bridge = loadBridge(koffi, libPath, path.join(__dirname, 'native', 'ble_ffi.json'))
  sendLog('FFI 지도 버전: ' + bridge.version)

  // 네이티브 스레드에서 호출되는 콜백 (koffi 가 메인 스레드로 넘겨줌)
  const logCb = bridge.callback('ble_log_cb', (msg) => sendLog(msg))
  const rxCb = bridge.callback('ble_rx_cb', (data, len) => {
    const bytes = Buffer.from(koffi.decode(data, koffi.array('uint8_t', len)))
    const hex = Array.from(bytes, (b) => '0x' + b.toString(16).toUpperCase().padStart(2, '0')).join(' ')
    //const hex = bytes.toString('hex').replace(/(..)(?=.)/g, '$1 ')
	sendLog(`수신 ${len}바이트 → echo: ${hex}`)
  })
  const foundCb = bridge.callback('ble_found_cb', (name, address) => {
    if (win && !win.isDestroyed() && pageReady) win.webContents.send('ble:found', { name, address })
  })

  // 설정이 바뀔 때마다 dll 이 현재 설정 전체를 json 문자열로 알려 줌
  const configCb = bridge.callback('ble_config_cb', (json) => {
    lastConfigJson = json
    if (win && !win.isDestroyed() && pageReady) win.webContents.send('ble:config', json)
  })

  bridge.call.ble_init(logCb, rxCb, foundCb, configCb)
  ble = bridge.call
}

function createWindow () {
  pageReady = false
  win = new BrowserWindow({
    width: 800,
    height: 750,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js')
    }
  })
  win.webContents.on('did-finish-load', () => {
    pageReady = true
    while (pendingLogs.length) win.webContents.send('ble:log', pendingLogs.shift())
    if (lastConfigJson) win.webContents.send('ble:config', lastConfigJson)
  })
  win.loadFile('index.html')
}

app.whenReady().then(() => {
  ipcMain.handle('ble:scanStart', () => (ble ? ble.ble_scan_start() : -100))
  ipcMain.handle('ble:scanStop', () => (ble ? ble.ble_scan_stop() : -100))
  ipcMain.handle('ble:connect', (_e, address) => {
    if (!ble) return -100
    return ble.ble_connect(String(address))
  })
  ipcMain.handle('ble:disconnect', () => (ble ? ble.ble_disconnect() : -100))
  // dll 이 로드될 때 자기 폴더에 생성한 model_intf.json 의 경로와 내용
  ipcMain.handle('ble:interfaceFile', () => {
    if (!loadedLibPath) return { error: 'dll 이 로드되지 않았습니다. ' + (loadError || '') }
    const filePath = path.join(path.dirname(loadedLibPath), 'model_intf.json')
    try {
      return { path: filePath, content: fs.readFileSync(filePath, 'utf8') }
    } catch (err) {
      return { path: filePath, error: err.message }
    }
  })
  // 화면에서 바꾼 항목만 json 문자열로 만들어 dll 의 설정 매니저에 전달
  ipcMain.handle('ble:configSet', (_e, changes) => {
    if (!ble) return -100
    return ble.ble_config_set(JSON.stringify(changes))
  })
  // 서버 인증서 지문(SHA-256)이 dll 에 고정된 것과 일치하는지 확인
  ipcMain.handle('ble:pinCheck', (_e, fingerprint) => {
    if (!ble) return -100
    return ble.ble_pin_check(String(fingerprint))
  })
  ipcMain.handle('ble:send', (_e, text) => {
    if (!ble) return -100
    const buf = Buffer.from(String(text), 'utf8')
    return ble.ble_send(buf, buf.length)
  })

  // 창을 먼저 띄우고, BLE 로드 실패는 로그 창에 표시
  createWindow()
  try {
    loadBle()
  } catch (err) {
    loadError = String(err && err.message ? err.message : err)
    sendLog('BLE 라이브러리 로드 실패: ' + loadError)
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})

app.on('will-quit', () => {
  if (!ble) return
  ble.ble_dispose()        // dll 을 먼저 멈춘 뒤
  bridge.unregisterAll()   // 콜백 포인터를 해제
})
