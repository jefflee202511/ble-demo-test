// ffi_loader.js
// Builds the koffi declarations from native/ble_ffi.json, so that nothing
// about the DLL interface is typed by hand in main.js.
//
//   const bridge = loadBridge(koffi, dllPath, jsonPath)
//   const cb = bridge.callback('ble_log_cb', (text) => { ... })   // JS function -> C function pointer
//   bridge.call.ble_init(cb, ...)                                 // call a DLL function
//   bridge.unregisterAll()                                        // at exit, after the DLL has stopped
'use strict'
const fs = require('fs')

function loadBridge (koffi, dllPath, jsonPath) {
  const map = JSON.parse(fs.readFileSync(jsonPath, 'utf8'))
  const types = map.types || {}
  const callbackDefs = map.callbacks || {}
  const functionDefs = map.functions || {}

  // JSON type name -> C type text used in a koffi prototype
  function cType (name, where) {
    if (Object.prototype.hasOwnProperty.call(callbackDefs, name)) return name + ' *' // pointer to callback
    if (Object.prototype.hasOwnProperty.call(types, name)) return types[name]
    throw new Error(`ble_ffi.json: unknown type '${name}' in ${where}`)
  }

  // { ret: "int", args: ["str"] } -> "int ble_connect(const char* a0)"
  function prototype (name, def) {
    const params = def.args.map((arg, i) => `${cType(arg, name)} a${i}`)
    return `${cType(def.ret, name)} ${name}(${params.join(', ')})`
  }

  const lib = koffi.load(dllPath)

  // 1. Callback shapes (DLL -> JS). Must be declared before the functions that take them.
  const protos = {}
  for (const [name, def] of Object.entries(callbackDefs)) {
    protos[name] = koffi.proto(prototype(name, def))
  }

  // 2. Functions (JS -> DLL), each wrapped with an argument check
  const registered = [] // callback pointers created by callback()

  function checkArg (fnName, index, typeName, value) {
    let ok = true
    if (Object.prototype.hasOwnProperty.call(callbackDefs, typeName)) ok = registered.includes(value)
    else if (typeName === 'str') ok = typeof value === 'string'
    else if (typeName === 'int' || typeName === 'u32') ok = Number.isInteger(value)
    else if (typeName === 'bytes') ok = Buffer.isBuffer(value) || value instanceof Uint8Array
    if (!ok) throw new TypeError(`${fnName}: argument ${index + 1} must be '${typeName}'`)
  }

  const call = {}
  for (const [name, def] of Object.entries(functionDefs)) {
    const native = lib.func(prototype(name, def))
    call[name] = (...args) => {
      if (args.length !== def.args.length) {
        throw new TypeError(`${name}: expected ${def.args.length} argument(s), got ${args.length}`)
      }
      def.args.forEach((typeName, i) => checkArg(name, i, typeName, args[i]))
      return native(...args)
    }
  }

  // 3. Turn a JS function into a C function pointer of the given callback type
  function callback (typeName, jsFunction) {
    if (!protos[typeName]) throw new Error(`ble_ffi.json: unknown callback '${typeName}'`)
    const pointer = koffi.register(jsFunction, koffi.pointer(protos[typeName]))
    registered.push(pointer)
    return pointer
  }

  // Release all callback pointers. Call only after the DLL can no longer call them.
  function unregisterAll () {
    while (registered.length > 0) koffi.unregister(registered.pop())
  }

  return { version: map.version, call, callback, unregisterAll }
}

module.exports = { loadBridge }
