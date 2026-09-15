// Shared fixtures for the WebMCP bridge tests. The scripts under test are plain
// browser scripts, so they are evaluated in a vm context with the minimum DOM
// surface they touch. No jsdom: every DOM object is a small hand-rolled stub.
const vm = require('node:vm');
const { readFileSync } = require('node:fs');
const path = require('node:path');

const SCRIPTS = path.join(__dirname, '../../IPlug/WEB/TemplateWasm/scripts');

const PLACEHOLDERS = {
  NAME_PLACEHOLDER_LC: 'test', NAME_PLACEHOLDER: 'Test', MAXNINPUTS_PLACEHOLDER: '2', MAXNOUTPUTS_PLACEHOLDER: '2',
  IS_INSTRUMENT_PLACEHOLDER: 'false', HOST_RESIZE_PLACEHOLDER: 'false', HAS_UI_PLACEHOLDER: 'true',
  DOES_MIDI_IN_PLACEHOLDER: 'false', DOES_MIDI_OUT_PLACEHOLDER: 'false'
};

function fillTemplate(name) {
  let source = readFileSync(path.join(SCRIPTS, name), 'utf8');
  for (const [key, value] of Object.entries(PLACEHOLDERS)) source = source.replaceAll(key, value);
  return source;
}

/** A recording stand-in for navigator.modelContext */
function makeModelContext() {
  const tools = new Map();
  return {
    tools,
    registerTool(tool, { signal } = {}) {
      if (tools.has(tool.name)) throw new Error(`Duplicate tool ${tool.name}`);
      tools.set(tool.name, tool);
      signal?.addEventListener('abort', () => tools.delete(tool.name));
      return Promise.resolve();
    },
    unregisterTool(name) { tools.delete(name); }
  };
}

class FakeEventTarget {
  constructor() { this.listeners = new Map(); }
  addEventListener(type, fn) { (this.listeners.get(type) || this.listeners.set(type, new Set()).get(type)).add(fn); }
  removeEventListener(type, fn) { this.listeners.get(type)?.delete(fn); }
  dispatchEvent(event) { this.listeners.get(event.type)?.forEach((fn) => fn(event)); return true; }
}

/** Evaluate IPlugWasmWebMCP.js in a fresh context. Returns the context globals. */
function loadBridgeScript({ modelContext = makeModelContext(), search = '' } = {}) {
  const window = new FakeEventTarget();
  Object.assign(window, { location: { search } });
  const navigator = modelContext ? { modelContext } : {};
  const scope = vm.createContext({
    window, navigator, document: { activeElement: null },
    console, AbortController, Event: class { constructor(type, init) { this.type = type; Object.assign(this, init); } },
    KeyboardEvent: class { constructor(type, init) { this.type = type; Object.assign(this, init); } },
    URLSearchParams, setTimeout, clearTimeout, Promise, JSON, Math, Date, Number, Object, Array, String, RegExp, Error, Map, Set, Symbol, Boolean,
    requestAnimationFrame: (fn) => setTimeout(fn, 0)
  });
  scope.globalThis = scope;
  vm.runInContext(readFileSync(path.join(SCRIPTS, 'IPlugWasmWebMCP.js'), 'utf8'), scope);
  return { scope, window, modelContext, IPlugWasmWebMCP: window.IPlugWasmWebMCP };
}

/** Load the bundle template (controller + element) and processor template into one context. */
function loadBundleAndProcessor({ modelContext } = {}) {
  const window = new FakeEventTarget();
  Object.assign(window, { location: { search: '' } });
  let ElementClass;
  const scope = vm.createContext({
    window, document: { modelContext, querySelector: () => null }, navigator: {},
    HTMLElement: class {}, customElements: { get: () => false, define: (_, cls) => { ElementClass = cls; } },
    console, AbortController, setTimeout, clearTimeout, URLSearchParams, Promise, JSON, Math, Number, Object, Array, Map, Set, Error, Uint8Array, Uint32Array, DataView, Atomics: { load: () => 0, store: () => {} },
    requestAnimationFrame: () => {},
    AudioWorkletProcessor: class {}, registerProcessor: (_, cls) => { scope.Processor = cls; },
    sampleRate: 48000
  });
  scope.globalThis = scope;
  vm.runInContext(fillTemplate('IPlugWasmBundle.js.template'), scope);
  vm.runInContext(fillTemplate('IPlugWasmProcessor.js.template'), scope);
  return { scope, window, ElementClass, IPlugWasmController: window.IPlugWasmController, Processor: scope.Processor };
}

/**
 * A fake DSP Module with one shaped parameter (value = normalized^2 * 100,
 * quantized to quarters) and base64 state, enough to exercise the processor's
 * request handling end to end.
 */
function makeFakeDSPModule() {
  let normalized = 0.5;
  const echo = [];
  const param = () => ({
    idx: 0, id: 0, name: 'Shaped', label: '%', group: '', type: 'double', min: 0, max: 100, default: 25, defaultNormalized: 0.5, step: 0.01,
    stepped: false, canAutomate: true, meta: false, value: normalized * normalized * 100, normalizedValue: normalized, display: `${(normalized * normalized * 100).toFixed(1)} %`
  });
  const set = (v) => { normalized = Math.round(v * 4) / 4; echo.push(normalized); return true; };
  return {
    echo,
    getNumParams: () => 1,
    onParam: (_, idx, v) => { normalized = v; },
    setParamNormalized: (_, idx, v) => set(v),
    setParamFromString: (_, idx, str) => { const value = parseFloat(str); if (!Number.isFinite(value)) return false; return set(Math.sqrt(value / 100)); },
    paramToNormalized: (_, idx, value) => Math.sqrt(value / 100),
    resetParam: () => set(0.5),
    getParamDisplay: () => param().display,
    getPluginInfoJSON: () => JSON.stringify({ bridgeVersion: 1, name: 'Test', params: [param()] }),
    serializeState: () => Buffer.from(JSON.stringify({ normalized })).toString('base64'),
    unserializeState: (_, b64) => { try { normalized = JSON.parse(Buffer.from(b64, 'base64').toString()).normalized; echo.push(normalized); return true; } catch (e) { return false; } },
    getNumPresets: () => 2,
    getPresetName: (_, i) => ['Init', 'Loud'][i],
    restorePreset: (_, i) => set(i ? 1 : 0.5),
    get normalized() { return normalized; }
  };
}

/** Wire a controller and a processor together over an in-memory port. */
function makeControllerPair({ IPlugWasmController, Processor }, moduleOverrides = {}) {
  const dsp = Object.assign(makeFakeDSPModule(), moduleOverrides);
  const controller = new IPlugWasmController('Test');
  const processor = Object.create(Processor.prototype);
  processor.instanceId = 1;
  processor.Module = dsp;
  const toController = [];
  processor.port = { postMessage: (data) => { toController.push(data); Promise.resolve().then(() => controller._onDSPMessage({ data })); } };
  controller.workletNode = { port: { postMessage: (data) => processor._onMessage({ data }) }, disconnect() {} };
  controller.audioContext = { state: 'running' };
  controller.isReady = true;
  return { controller, processor, dsp, toController };
}

module.exports = { SCRIPTS, fillTemplate, makeModelContext, FakeEventTarget, loadBridgeScript, loadBundleAndProcessor, makeFakeDSPModule, makeControllerPair };
