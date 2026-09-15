/**
 * IPlugWasmWebMCP - agent bridge for iPlug2 Wasm host pages.
 *
 * Registers a set of WebMCP tools (navigator.modelContext) that let a browser
 * agent discover and automate plugin parameters, snapshot and restore plugin
 * state, and inspect or manipulate the UI: IGraphics controls (values,
 * visibility, text, style, bounds, pointer/keyboard input, screenshots) or
 * WebView DOM controls and explicitly registered actions.
 *
 * Every tool is also callable without WebMCP through bridge.call(name, input)
 * and, when the browser has no modelContext, through the dev shim installed at
 * window.__iplugWebMCPShim (list() / call()). Tools are all registered up
 * front so they are discoverable before audio starts; execution is gated on
 * readiness and reports structured error codes with hints.
 *
 * Shared, verbatim script (no build-time placeholders): copied next to
 * IPlugWasmHostControls.js by the CMake and Makefile distribution steps.
 */
(function () {
  'use strict';

  const BRIDGE_VERSION = 1;
  const EVENT_BUFFER_SIZE = 500;

  const getUrlParam = (name) => {
    try {
      return new URLSearchParams(window.location.search).get(name);
    } catch (e) {
      return null;
    }
  };

  const nextFrame = () => new Promise((resolve) => {
    if (typeof requestAnimationFrame === 'function') requestAnimationFrame(() => resolve());
    else setTimeout(resolve, 16);
  });

  class BridgeError extends Error {
    constructor(message, code = 'error', hint = '') {
      super(hint ? `${message} Hint: ${hint}` : message);
      this.name = 'BridgeError';
      this.code = code;
      this.hint = hint;
    }
  }

  //==========================================================================
  // Minimal JSON-schema validator (the subset the tool schemas use)
  //==========================================================================

  function validate(schema, value, path = 'input') {
    if (!schema) return;
    const fail = (msg) => { throw new BridgeError(`${path}: ${msg}`, 'invalid-input'); };
    const types = Array.isArray(schema.type) ? schema.type : (schema.type ? [schema.type] : []);

    if (types.length) {
      const ok = types.some((t) => {
        switch (t) {
          case 'object': return value !== null && typeof value === 'object' && !Array.isArray(value);
          case 'array': return Array.isArray(value);
          case 'string': return typeof value === 'string';
          case 'number': return typeof value === 'number' && Number.isFinite(value);
          case 'integer': return Number.isInteger(value);
          case 'boolean': return typeof value === 'boolean';
          case 'null': return value === null;
          default: return false;
        }
      });
      if (!ok) fail(`expected ${types.join(' or ')}`);
    }

    if (schema.enum && !schema.enum.includes(value)) fail(`expected one of ${schema.enum.join(', ')}`);
    if (typeof value === 'number') {
      if (schema.minimum !== undefined && value < schema.minimum) fail(`must be >= ${schema.minimum}`);
      if (schema.maximum !== undefined && value > schema.maximum) fail(`must be <= ${schema.maximum}`);
    }
    if (typeof value === 'string') {
      if (schema.minLength !== undefined && value.length < schema.minLength) fail(`must be at least ${schema.minLength} characters`);
      if (schema.pattern && !new RegExp(schema.pattern).test(value)) fail('has an invalid format');
    }
    if (Array.isArray(value)) {
      if (schema.minItems !== undefined && value.length < schema.minItems) fail(`needs at least ${schema.minItems} items`);
      if (schema.items) value.forEach((item, i) => validate(schema.items, item, `${path}[${i}]`));
    }
    if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
      const props = schema.properties || {};
      for (const key of schema.required || []) {
        if (value[key] === undefined) fail(`missing required property '${key}'`);
      }
      for (const [key, child] of Object.entries(value)) {
        if (props[key]) validate(props[key], child, `${path}.${key}`);
        else if (schema.additionalProperties === false) fail(`unknown property '${key}'`);
      }
    }
  }

  //==========================================================================
  // Dev shim: same contract as navigator.modelContext, drivable from DevTools
  //==========================================================================

  function installShim() {
    if (window.__iplugWebMCPShim) return window.__iplugWebMCPShim;

    const tools = new Map();
    const shim = {
      isShim: true,
      registerTool(tool, { signal } = {}) {
        tools.set(tool.name, tool);
        signal?.addEventListener('abort', () => {
          if (tools.get(tool.name) === tool) tools.delete(tool.name);
        }, { once: true });
      },
      unregisterTool(name) { tools.delete(name); },
      list() {
        return [...tools.values()].map((t) => ({
          name: t.name, description: t.description, inputSchema: t.inputSchema, annotations: t.annotations
        }));
      },
      call(name, input = {}, ctx = {}) {
        const tool = tools.get(name);
        if (!tool) return Promise.reject(new BridgeError(`Unknown tool '${name}'.`, 'unknown-tool', 'Use __iplugWebMCPShim.list() to see registered tools.'));
        return Promise.resolve().then(() => tool.execute(input, ctx));
      }
    };

    window.__iplugWebMCPShim = shim;
    return shim;
  }

  function resolveContext(override) {
    if (override) return override;
    const native = (typeof navigator !== 'undefined' && navigator.modelContext) || (typeof document !== 'undefined' && document.modelContext);
    if (native && typeof native.registerTool === 'function' && getUrlParam('iplugWebMCPShim') !== '1') return native;
    return installShim();
  }

  //==========================================================================
  // IGraphics adapter: wraps the _iplug_webmcp_* exports on the UI module
  //==========================================================================

  const KEY_CODES = {
    Backspace: 0x08, Tab: 0x09, Enter: 0x0D, Shift: 0x10, Control: 0x11, Escape: 0x1B, ' ': 0x20,
    PageUp: 0x21, PageDown: 0x22, End: 0x23, Home: 0x24,
    ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28, Delete: 0x2E
  };

  function keyToVK(key) {
    if (KEY_CODES[key] !== undefined) return KEY_CODES[key];
    if (/^F([1-9]|1[0-2])$/.test(key)) return 0x70 + Number(key.slice(1)) - 1;
    if (key.length === 1) {
      const upper = key.toUpperCase();
      if (upper >= '0' && upper <= '9') return 0x30 + upper.charCodeAt(0) - 48;
      if (upper >= 'A' && upper <= 'Z') return 0x41 + upper.charCodeAt(0) - 65;
    }
    return 0;
  }

  class IGraphicsAdapter {
    constructor(element) {
      this.element = element;
    }

    get module() { return this.element?.module || null; }
    get canvas() { return this.element?.canvas || null; }

    get handle() {
      const el = this.element;
      if (!el) return 0;
      if (el.graphicsHandle !== undefined) return el.graphicsHandle || 0;
      return el.canvas?._iplugGraphics || 0;
    }

    get ready() {
      const mod = this.module;
      return Boolean(this.element?.isUIReady && this.handle && mod && typeof mod._iplug_webmcp_get_ui_tree === 'function');
    }

    get bridgeCompiledIn() {
      return typeof this.module?._iplug_webmcp_get_ui_tree === 'function';
    }

    liveEditAvailable() {
      const mod = this.module;
      if (!mod || typeof mod._iplug_set_live_edit !== 'function') return false;
      if (typeof mod._iplug_webmcp_live_edit_available === 'function') return mod._iplug_webmcp_live_edit_available() === 1;
      return true;
    }

    lastError() {
      const mod = this.module;
      if (mod && typeof mod._iplug_webmcp_last_error === 'function') {
        return mod.UTF8ToString(mod._iplug_webmcp_last_error());
      }
      return '';
    }

    tree() {
      const mod = this.module;
      const json = mod.UTF8ToString(mod._iplug_webmcp_get_ui_tree(this.handle));
      const tree = JSON.parse(json);
      if (!tree) throw new BridgeError('IGraphics instance is not available.', 'ui-not-ready');
      return tree;
    }

    call(name, ...args) {
      const fn = this.module[`_${name}`];
      if (typeof fn !== 'function') throw new BridgeError(`This build does not export ${name}.`, 'not-supported');
      return fn(this.handle, ...args) === 1;
    }

    callWithStrings(name, argTypes, args) {
      const mod = this.module;
      if (typeof mod[`_${name}`] !== 'function') throw new BridgeError(`This build does not export ${name}.`, 'not-supported');
      return mod.ccall(name, 'number', ['number', ...argTypes], [this.handle, ...args]) === 1;
    }

    require(ok, message, code = 'igraphics-error') {
      if (!ok) {
        const detail = this.lastError();
        throw new BridgeError(detail ? `${message} ${detail}` : message, code);
      }
    }

    flushDraw() {
      const mod = this.module;
      if (typeof mod._iplug_webmcp_flush_draw === 'function') mod._iplug_webmcp_flush_draw(this.handle);
    }

    preserveDrawingBuffer() {
      try {
        const ctx = this.canvas?.getContext('webgl') || this.canvas?.getContext('experimental-webgl');
        return Boolean(ctx?.getContextAttributes?.().preserveDrawingBuffer);
      } catch (e) {
        return false;
      }
    }
  }

  //==========================================================================
  // WebView adapter: DOM discovery plus an explicit action registry
  //==========================================================================

  class WebViewAdapter {
    constructor({ root, isReady } = {}) {
      this.root = root;
      this.isReadyFn = typeof isReady === 'function' ? isReady : () => true;
      this.registry = new Map();
    }

    get ready() { return Boolean(this.root && this.isReadyFn()); }

    reset() { this.registry.clear(); }

    registerAction(action) {
      if (!action || typeof action.id !== 'string' || !action.id.trim()) throw new BridgeError('registerAction needs a non-empty id.', 'invalid-input');
      if (typeof action.run !== 'function') throw new BridgeError('registerAction needs a run() function.', 'invalid-input');
      if (this.registry.has(action.id)) throw new BridgeError(`Action '${action.id}' is already registered.`, 'duplicate');
      this.registry.set(action.id, {
        id: action.id, label: action.label || action.id, description: action.description || '',
        inputSchema: action.inputSchema || null, run: action.run
      });
      return () => this.unregisterAction(action.id);
    }

    unregisterAction(id) { this.registry.delete(id); }

    elements(selector) {
      const result = [];
      const visit = (node) => {
        if (!node || typeof node.querySelectorAll !== 'function') return;
        result.push(...node.querySelectorAll(selector));
        for (const el of node.querySelectorAll('*')) {
          if (el.shadowRoot) visit(el.shadowRoot);
        }
      };
      visit(this.root);
      return result;
    }

    static label(el) {
      return el.getAttribute?.('aria-label') || el.getAttribute?.('label') || (el.textContent || '').trim();
    }

    static disabled(el) {
      const rects = typeof el.getClientRects === 'function' ? el.getClientRects() : [1];
      const style = typeof getComputedStyle === 'function' && el instanceof Element ? getComputedStyle(el) : null;
      return Boolean(
        (typeof el.matches === 'function' && el.matches(':disabled')) ||
        el.hasAttribute?.('disabled') ||
        el.getAttribute?.('aria-disabled') === 'true' ||
        el.closest?.('[inert]') ||
        rects.length === 0 ||
        (style && style.visibility === 'hidden')
      );
    }

    static bounds(el) {
      const r = typeof el.getBoundingClientRect === 'function' ? el.getBoundingClientRect() : null;
      return r ? { left: r.left, top: r.top, right: r.right, bottom: r.bottom } : null;
    }

    discover() {
      const parameters = this.elements('[param-id]').map((el) => ({
        paramIdx: Number(el.getAttribute('param-id')),
        tagName: el.tagName?.toLowerCase(),
        id: el.getAttribute('data-iplug-id') || el.id || null,
        label: WebViewAdapter.label(el),
        bounds: WebViewAdapter.bounds(el)
      })).filter((p) => Number.isInteger(p.paramIdx) && p.paramIdx >= 0);

      const actions = [...this.registry.values()].map((a) => ({
        id: a.id, label: a.label, description: a.description, inputSchema: a.inputSchema, source: 'registered', disabled: false
      }));
      for (const el of this.elements('[data-iplug-action]')) {
        const id = el.getAttribute('data-iplug-action');
        if (this.registry.has(id)) continue;
        actions.push({ id, label: WebViewAdapter.label(el), source: 'dom', disabled: WebViewAdapter.disabled(el), bounds: WebViewAdapter.bounds(el) });
      }

      return { coordinateSpace: 'viewport CSS pixels', parameters, actions };
    }

    async activate(id, input, signal) {
      const registered = this.registry.get(id);
      if (registered) {
        if (registered.inputSchema) validate(registered.inputSchema, input || {}, 'input.input');
        const result = await registered.run(input || {}, { signal });
        return { id, source: 'registered', result: result === undefined ? null : result };
      }

      const matches = this.elements('[data-iplug-action]').filter((el) => el.getAttribute('data-iplug-action') === id);
      if (matches.length === 0) throw new BridgeError(`No action with id '${id}'.`, 'not-found', 'Use get_webview_controls to list actions.');
      if (matches.length > 1) throw new BridgeError(`Action id '${id}' matches ${matches.length} elements; ids must be unique.`, 'duplicate');
      if (WebViewAdapter.disabled(matches[0])) throw new BridgeError(`Action '${id}' is disabled or hidden.`, 'disabled');
      matches[0].click();
      return { id, source: 'dom', result: null };
    }
  }

  //==========================================================================
  // The bridge
  //==========================================================================

  const rectSchema = {
    type: 'object',
    properties: { l: { type: 'number' }, t: { type: 'number' }, r: { type: 'number' }, b: { type: 'number' } },
    required: ['l', 't', 'r', 'b'], additionalProperties: false
  };
  const modifiersSchema = {
    type: 'object',
    properties: { shift: { type: 'boolean' }, ctrl: { type: 'boolean' }, alt: { type: 'boolean' } },
    additionalProperties: false
  };
  const colorPattern = '^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$';
  const controlRef = { tag: { type: 'integer', minimum: 0 }, index: { type: 'integer', minimum: 0 } };
  const valueEntry = {
    type: 'object',
    properties: {
      paramIdx: { type: 'integer', minimum: 0 },
      normalized: { type: 'number', minimum: 0, maximum: 1 },
      value: { type: 'number' },
      text: { type: 'string' }
    },
    required: ['paramIdx'], additionalProperties: false
  };

  class IPlugWasmWebMCP {
    /**
     * @param {Object} options
     * @param {string} options.pluginName
     * @param {'igraphics'|'webview'} options.backend
     * @param {HTMLElement} [options.element] - the <iplug-*> web component (igraphics backend)
     * @param {() => IPlugWasmController|null} [options.getController]
     * @param {IPlugWasmHostControls} [options.hostControls] - enables the host/footer tools
     * @param {{root: Element, isReady: () => boolean}} [options.webview] - webview backend
     * @param {Object} [options.context] - modelContext override (tests)
     * @param {number} [options.requestTimeoutMs]
     * @param {boolean} [options.enabled] - false disables registration entirely
     * @param {boolean} [options.autoRegister] - default true
     */
    constructor(options = {}) {
      if (!options.pluginName) throw new Error('IPlugWasmWebMCP requires pluginName');
      this.options = options;
      this.pluginName = options.pluginName;
      this.backend = options.backend || (options.webview ? 'webview' : 'igraphics');
      this.element = options.element || null;
      this.hostControls = options.hostControls || null;
      this.getController = typeof options.getController === 'function' ? options.getController : () => (this.element?.controller || null);
      this.requestTimeoutMs = options.requestTimeoutMs;
      this.igraphics = this.backend === 'igraphics' ? new IGraphicsAdapter(this.element) : null;
      this.webview = this.backend === 'webview' ? new WebViewAdapter(options.webview || {}) : null;
      this.webviewAPI = this.webview ? {
        registerAction: (action) => this.webview.registerAction(action),
        unregisterAction: (id) => this.webview.unregisterAction(id),
        reset: () => this.webview.reset(),
        list: () => this.webview.discover()
      } : null;

      this.instance = window.__iPlugWebMCPInstances = (window.__iPlugWebMCPInstances || 0) + 1;
      const base = `iplug_${this.pluginName.toLowerCase().replace(/[^a-z0-9]/g, '_')}`;
      this.prefix = this.instance > 1 ? `${base}_${this.instance}` : base;

      this.context = null;
      this.tools = new Map();
      this._abort = null;
      this._disposed = false;
      this._queue = Promise.resolve();
      this._events = [];
      this._eventSeq = 0;
      this._undo = [];
      this._layoutBaseline = null;
      this._controllerHooked = null;
      this._unhookController = null;
      this._lastWebViewReady = null;
      this._onWindowMessage = (event) => {
        const data = event.data;
        if (event.source !== window || !data || typeof data.type !== 'string' || !data.type.startsWith('iplug:live-edit:')) return;
        this._pushEvent(data.type, data);
      };

      if (options.enabled !== false && options.autoRegister !== false) this.register();
    }

    //--- lifecycle ------------------------------------------------------------

    register() {
      if (this._abort || this._disposed) return this;
      this.context = resolveContext(this.options.context);
      this._abort = new AbortController();
      window.addEventListener('message', this._onWindowMessage);
      this._defineTools();
      this.refresh();
      return this;
    }

    /** Re-check readiness: hook a newly created controller, record readiness events. Cheap; call on uiready/audioready. */
    refresh() {
      const controller = this.getController();
      if (controller && controller !== this._controllerHooked && typeof controller.on === 'function') {
        if (this._unhookController) this._unhookController();
        this._controllerHooked = controller;
        const off = controller.on('paramChange', ({ paramIdx, value }) => this._pushEvent('iplug:param-changed', { paramIdx, value }));
        this._unhookController = typeof off === 'function' ? off : () => controller.off?.('paramChange');
        this._pushEvent('iplug:dsp-ready', { audioContextState: controller.audioContext?.state || 'unknown' });
      }
      if (this.webview) {
        const ready = this.webview.ready;
        if (ready && ready !== this._lastWebViewReady) this._pushEvent('iplug:webview-loaded', {});
        this._lastWebViewReady = ready;
      }
      return this;
    }

    dispose() {
      if (this._disposed) return;
      this._disposed = true;
      window.removeEventListener('message', this._onWindowMessage);
      if (this._unhookController) this._unhookController();
      this._abort?.abort();
      for (const name of this.tools.keys()) {
        try { this.context?.unregisterTool?.(name); } catch (e) { /* signal-aware contexts already removed it */ }
      }
      this.tools.clear();
    }

    /** Invoke a tool directly (name may omit the instance prefix). */
    call(name, input = {}, ctx = {}) {
      const full = this.tools.has(name) ? name : `${this.prefix}_${name}`;
      const tool = this.tools.get(full);
      if (!tool) return Promise.reject(new BridgeError(`Unknown tool '${name}'.`, 'unknown-tool'));
      return Promise.resolve().then(() => tool.execute(input, ctx));
    }

    //--- events -----------------------------------------------------------------

    _pushEvent(type, data) {
      const event = { seq: ++this._eventSeq, time: Date.now(), type, ...data };
      this._events.push(event);
      if (this._events.length > EVENT_BUFFER_SIZE) this._events.splice(0, this._events.length - EVENT_BUFFER_SIZE);
      return event;
    }

    //--- readiness --------------------------------------------------------------

    _gateUI() {
      if (!this.igraphics) throw new BridgeError('This host has no IGraphics UI.', 'not-supported');
      if (!this.element?.isUIReady) throw new BridgeError('The plugin UI has not finished loading.', 'ui-not-ready', 'Wait for the uiready event, then retry.');
      if (!this.igraphics.bridgeCompiledIn) throw new BridgeError('The UI module was built without the WebMCP bridge.', 'not-supported', 'Rebuild with IPLUG2_WASM_WEBMCP=ON (the default).');
      if (!this.igraphics.ready) throw new BridgeError('The IGraphics instance is not available yet.', 'ui-not-ready');
    }

    _gateDSP() {
      const controller = this.getController();
      if (!controller || !controller.isReady) {
        throw new BridgeError('The DSP is not running.', 'dsp-not-ready', 'Call set_audio_enabled({enabled:true}) or click Start Audio; the first start may need a user gesture.');
      }
      if (typeof controller.request !== 'function') throw new BridgeError('This controller build has no request() support.', 'not-supported');
      return controller;
    }

    _gateHost() {
      if (!this.hostControls) throw new BridgeError('This page has no host controls.', 'not-supported');
    }

    _gateLiveEdit() {
      this._gateUI();
      if (!this.igraphics.liveEditAvailable()) throw new BridgeError('Live edit is not compiled into this build.', 'live-edit-unavailable', 'Rebuild with -DIPLUG2_WASM_LIVE_EDIT=ON to use the visual editing overlay.');
    }

    _gateWebView() {
      if (!this.webview) throw new BridgeError('This host has no WebView UI.', 'not-supported');
      if (!this.webview.ready) throw new BridgeError('The WebView UI has not loaded.', 'ui-not-ready', 'Wait for the page to finish loading, then retry.');
    }

    _gateScreenshot() {
      this._gateUI();
      if (!this.igraphics.canvas || typeof this.igraphics.canvas.toDataURL !== 'function') throw new BridgeError('No canvas to capture.', 'screenshot-unavailable');
    }

    getStatus() {
      const controller = this.getController();
      const ui = { ready: this.element ? Boolean(this.element.isUIReady) : Boolean(this.webview?.ready), backend: this.backend };
      if (this.igraphics) {
        ui.bridgeCompiledIn = this.igraphics.bridgeCompiledIn;
        ui.liveEditAvailable = this.igraphics.liveEditAvailable();
        ui.preserveDrawingBuffer = this.igraphics.ready ? this.igraphics.preserveDrawingBuffer() : null;
        if (this.igraphics.ready) {
          try {
            const tree = this.igraphics.tree();
            Object.assign(ui, { width: tree.width, height: tree.height, drawScale: tree.drawScale, liveEditEnabled: tree.liveEditEnabled, nControls: tree.nControls });
          } catch (e) { ui.error = e.message; }
        }
      }
      if (this.webview) ui.webviewReady = this.webview.ready;

      const dsp = {
        controllerReady: Boolean(controller?.isReady),
        audioContextState: controller?.audioContext?.state || (this.hostControls?.audioContext?.state) || 'uninitialized',
        running: controller?.audioContext?.state === 'running'
      };

      const capabilities = {
        parameters: Boolean(controller?.isReady),
        state: Boolean(controller?.isReady),
        host: Boolean(this.hostControls),
        igraphics: Boolean(this.igraphics?.ready),
        layout: Boolean(this.igraphics?.ready),
        liveEdit: Boolean(this.igraphics?.liveEditAvailable()),
        pointer: Boolean(this.igraphics?.ready),
        screenshot: Boolean(this.igraphics?.ready && ui.preserveDrawingBuffer),
        webviewActions: Boolean(this.webview?.ready)
      };

      const hints = [];
      if (!dsp.controllerReady) hints.push('Parameter and state tools need the DSP: call set_audio_enabled({enabled:true}) or click Start Audio (a user gesture may be required by the browser).');
      if (this.igraphics && !ui.ready) hints.push('IGraphics tools become available after the UI module loads.');
      if (this.igraphics?.ready && !ui.preserveDrawingBuffer) hints.push('capture_screenshot needs preserveDrawingBuffer: add ?iplugWasmCapture=1 to the URL or set Module.iplugPreserveDrawingBuffer = true before the UI loads.');
      if (this.igraphics && !ui.liveEditAvailable) hints.push('set_layout_editing (visual overlay) needs a build with IPLUG2_WASM_LIVE_EDIT=ON; bounds/style edits work without it.');

      const tools = [...this.tools.values()].map((t) => ({ name: t.name, available: this._available(t.gate), requires: t.requires }));

      return {
        bridgeVersion: BRIDGE_VERSION, plugin: this.pluginName, instance: this.instance, prefix: this.prefix,
        webmcp: this.context?.isShim ? 'shim' : 'native',
        ui, dsp, capabilities, tools, hints, lastEventSeq: this._eventSeq
      };
    }

    _available(gate) {
      try { gate?.(); return true; } catch (e) { return false; }
    }

    //--- tool definition --------------------------------------------------------

    _add(name, def) {
      const fullName = `${this.prefix}_${name}`;
      const inputSchema = def.input || { type: 'object', properties: {}, additionalProperties: false };
      const tool = {
        name: fullName,
        shortName: name,
        description: def.description,
        inputSchema,
        annotations: { readOnlyHint: Boolean(def.readOnly) },
        gate: def.gate || null,
        requires: def.requires || [],
        execute: (input, ctx) => this._run(tool, def, input, ctx)
      };
      this.tools.set(fullName, tool);
      try {
        Promise.resolve(this.context.registerTool(tool, { signal: this._abort.signal })).catch((err) => {
          if (!this._abort.signal.aborted) console.warn('iPlug2 WebMCP: registration failed for', fullName, err);
        });
      } catch (err) {
        console.warn('iPlug2 WebMCP: registration failed for', fullName, err);
      }
    }

    _run(tool, def, input, ctx) {
      const execute = async () => {
        if (this._disposed) throw new BridgeError('The bridge has been disposed.', 'disposed');
        validate(tool.inputSchema, input ?? {});
        if (def.gate) def.gate();
        return def.run(input ?? {}, { signal: ctx?.signal });
      };
      if (def.readOnly) return execute();
      const result = this._queue.then(execute);
      this._queue = result.catch(() => {});
      return result;
    }

    _defineTools() {
      const N = this.pluginName;

      //--- core
      this._add('get_status', {
        description: `Readiness, capabilities and the list of ${N} tools with their availability. Call this first; it works before audio starts.`,
        readOnly: true, run: () => this.getStatus()
      });

      this._add('get_events', {
        description: 'Recent UI/DSP events (live edit changes, parameter changes, readiness). Poll with since=<lastEventSeq>.',
        readOnly: true,
        input: { type: 'object', properties: {
          since: { type: 'integer', minimum: 0 }, types: { type: 'array', items: { type: 'string' } }, clear: { type: 'boolean' }
        }, additionalProperties: false },
        run: ({ since = 0, types, clear }) => {
          const events = this._events.filter((e) => e.seq > since && (!types || types.includes(e.type)));
          if (clear) this._events = [];
          return { events, lastEventSeq: this._eventSeq };
        }
      });

      //--- host / footer
      if (this.hostControls) {
        this._add('get_host_state', {
          description: 'Read the host page state: audio running, sample rate, test signal source settings, live edit state.',
          readOnly: true, gate: () => this._gateHost(), requires: ['host'],
          run: () => this.hostControls.getHostState()
        });

        this._add('set_audio_enabled', {
          description: 'Start or stop audio (initializes the DSP worklet on first start). Browser autoplay rules may require a real click first; the result says so instead of throwing.',
          gate: () => this._gateHost(), requires: ['host'],
          input: { type: 'object', properties: { enabled: { type: 'boolean' } }, required: ['enabled'], additionalProperties: false },
          run: async ({ enabled }) => {
            const hc = this.hostControls;
            if (enabled === Boolean(hc.audioStarted)) return { ...hc.getHostState(), changed: false };
            if (enabled) {
              if (hc.startBtn?.disabled && !this.element?.isUIReady) throw new BridgeError('The plugin UI is not ready.', 'ui-not-ready');
              if (hc.sourceSelect?.value === 'audioin') throw new BridgeError('Microphone input must be started from the footer.', 'not-supported', 'Choose another source with configure_source first.');
              await hc.startAudio();
              this.refresh();
              const state = hc.audioContext?.state;
              if (state !== 'running') {
                // resume() is pending under autoplay restrictions
                this._pushEvent('iplug:audio-state', { state, blocked: true });
                return { ...hc.getHostState(), changed: true, reason: 'autoplay-blocked', hint: 'Click the Start Audio button once so the browser allows playback.' };
              }
              this._pushEvent('iplug:audio-state', { state });
              return { ...hc.getHostState(), changed: true };
            }
            hc.stopAudio();
            this._pushEvent('iplug:audio-state', { state: hc.audioContext?.state || 'suspended' });
            return { ...hc.getHostState(), changed: true };
          }
        });

        this._add('configure_source', {
          description: 'Configure the host test signal: source (none/tone/noise/file), gainPercent, waveform, frequencyLeft/Right (Hz), noiseType. Does not open files or request the microphone.',
          gate: () => this._gateHost(), requires: ['host'],
          input: { type: 'object', properties: {
            source: { type: 'string', enum: ['none', 'tone', 'noise', 'file'] },
            gainPercent: { type: 'number', minimum: 0, maximum: 100 },
            waveform: { type: 'string', enum: ['sine', 'square', 'triangle', 'sawtooth'] },
            frequencyLeft: { type: 'number', minimum: 20, maximum: 2000 },
            frequencyRight: { type: 'number', minimum: 20, maximum: 2000 },
            noiseType: { type: 'string', enum: ['white', 'pink'] }
          }, additionalProperties: false },
          run: async (input) => {
            const hc = this.hostControls;
            if (input.source === 'file' && !hc.audioBuffer) throw new BridgeError('No audio file is loaded.', 'not-supported', 'Load a file using the footer first.');
            if ((input.source || hc.sourceSelect?.value) === 'audioin') throw new BridgeError('Microphone input is configured from the footer.', 'not-supported');
            return hc.applySourceSettings(input);
          }
        });
      }

      //--- parameters (DSP path)
      const request = (op, args, ctx) => this._gateDSP().request(op, args, { signal: ctx?.signal, timeoutMs: this.requestTimeoutMs });

      this._add('get_parameters', {
        description: `Read all ${N} parameters from the DSP: index, name, label, group, type, range, step, value (native), normalizedValue (0-1), display string, enum display texts.`,
        readOnly: true, gate: () => this._gateDSP(), requires: ['dsp'],
        run: (input, ctx) => request('getParameters', {}, ctx)
      });

      this._add('set_parameter', {
        description: 'Set one parameter on the DSP with exactly one of: normalized (0-1), value (native units) or text (display string, e.g. "-6 dB"). Returns the quantized result; every attached UI follows automatically.',
        gate: () => this._gateDSP(), requires: ['dsp'],
        input: valueEntry,
        run: (input, ctx) => request('setParameter', input, ctx)
      });

      this._add('set_parameters', {
        description: 'Set several parameters at once (validated together; nothing is applied if any entry is invalid). Each entry takes paramIdx plus normalized, value or text.',
        gate: () => this._gateDSP(), requires: ['dsp'],
        input: { type: 'object', properties: { values: { type: 'array', minItems: 1, items: valueEntry } }, required: ['values'], additionalProperties: false },
        run: (input, ctx) => request('setParameters', input, ctx)
      });

      this._add('reset_parameter', {
        description: 'Reset one parameter (paramIdx) or every parameter (omit paramIdx) to its default.',
        gate: () => this._gateDSP(), requires: ['dsp'],
        input: { type: 'object', properties: { paramIdx: { type: 'integer', minimum: 0 } }, additionalProperties: false },
        run: (input, ctx) => request('resetParameter', input, ctx)
      });

      this._add('get_state', {
        description: `Snapshot the complete ${N} plugin state (SerializeState) as a base64 blob for later set_state.`,
        readOnly: true, gate: () => this._gateDSP(), requires: ['dsp'],
        run: (input, ctx) => request('getState', {}, ctx)
      });

      this._add('set_state', {
        description: 'Restore a plugin state blob from get_state. All parameters are pushed to the UI afterwards; returns the resulting parameters.',
        gate: () => this._gateDSP(), requires: ['dsp'],
        input: { type: 'object', properties: { base64: { type: 'string', minLength: 1 } }, required: ['base64'], additionalProperties: false },
        run: (input, ctx) => request('setState', input, ctx)
      });

      this._add('get_presets', {
        description: 'List the factory presets compiled into the plugin.',
        readOnly: true, gate: () => this._gateDSP(), requires: ['dsp'],
        run: (input, ctx) => request('getPresets', {}, ctx)
      });

      this._add('restore_preset', {
        description: 'Restore a factory preset by index (see get_presets).',
        gate: () => this._gateDSP(), requires: ['dsp'],
        input: { type: 'object', properties: { presetIdx: { type: 'integer', minimum: 0 } }, required: ['presetIdx'], additionalProperties: false },
        run: (input, ctx) => request('restorePreset', input, ctx)
      });

      //--- IGraphics
      if (this.igraphics) this._defineIGraphicsTools();

      //--- WebView
      if (this.webview) this._defineWebViewTools();
    }

    //--- IGraphics helpers --------------------------------------------------------

    _resolveControl(tree, { tag, index }) {
      if ((tag === undefined) === (index === undefined)) throw new BridgeError('Provide exactly one of tag or index.', 'invalid-input');
      const control = tag !== undefined ? tree.controls.find((c) => c.tag === tag) : tree.controls.find((c) => c.idx === index);
      if (!control) throw new BridgeError(tag !== undefined ? `No control with tag ${tag}.` : `No control at index ${index}.`, 'not-found', 'Use get_ui_tree to list controls (tags are stable, indices are not).');
      return control;
    }

    _controlKey(control) { return control.tag >= 0 ? { tag: control.tag } : { index: control.idx }; }

    _sizeKey(tree) { return `${tree.width}x${tree.height}`; }

    _pushUndo(label, tree, control, undo) {
      this._undo.push({ label, sizeKey: this._sizeKey(tree), key: this._controlKey(control), undo });
      if (this._undo.length > 100) this._undo.shift();
    }

    _captureBaseline(tree) {
      if (!this._layoutBaseline) this._layoutBaseline = tree;
    }

    _setProps(idx, props) {
      const ig = this.igraphics;
      for (const [key, value] of Object.entries(props)) {
        ig.require(ig.callWithStrings('iplug_webmcp_set_control_prop', ['number', 'string', 'string'], [idx, key, String(value)]), `Could not set ${key}.`);
      }
    }

    _styleToProps(style) {
      const props = {};
      for (const [key, value] of Object.entries(style || {})) {
        if (key === 'colors') {
          for (const [name, color] of Object.entries(value || {})) props[`color.${name}`] = color;
        } else {
          props[key] = value;
        }
      }
      return props;
    }

    _defineIGraphicsTools() {
      const ig = this.igraphics;
      const treeResult = () => {
        const tree = ig.tree();
        const canvas = ig.canvas;
        return { ...tree, coordinateSpace: 'IGraphics logical units', canvasCss: canvas ? { width: canvas.clientWidth, height: canvas.clientHeight } : null, lastEventSeq: this._eventSeq };
      };
      const withControl = (input) => {
        const tree = ig.tree();
        return { tree, control: this._resolveControl(tree, input) };
      };

      this._add('get_ui_tree', {
        description: `Describe the ${this.pluginName} IGraphics UI: size, scales, background colour and every control (index, stable tag, class, group, parameter bindings with values and display strings, hidden/disabled, draw and hit-test bounds in logical units, text/label/style where applicable).`,
        readOnly: true, gate: () => this._gateUI(), requires: ['ui'],
        run: () => treeResult()
      });

      this._add('set_control_value', {
        description: 'Set a control value (normalized 0-1) through the UI input path, so the control\'s own logic and action functions run and the change reaches the DSP as if the user made it. Identify the control by tag (preferred) or index.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { ...controlRef, valIdx: { type: 'integer', minimum: 0 }, normalized: { type: 'number', minimum: 0, maximum: 1 }, gesture: { type: 'boolean' } }, required: ['normalized'], additionalProperties: false },
        run: ({ valIdx = 0, normalized, gesture = true, ...ref }) => {
          const { tree, control } = withControl(ref);
          const before = control.vals?.[valIdx]?.value;
          ig.require(ig.call('iplug_webmcp_set_control_value', control.idx, valIdx, normalized, gesture ? 1 : 0), 'Could not set control value.');
          if (before !== undefined) this._pushUndo('set_control_value', tree, control, (idx) => ig.call('iplug_webmcp_set_control_value', idx, valIdx, before, 0));
          const after = this._resolveControl(ig.tree(), this._controlKey(control));
          return { control: after, dspSynced: Boolean(this.getController()?.isReady) };
        }
      });

      this._add('reset_control_value', {
        description: 'Reset a control value to its parameter default through the UI path.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { ...controlRef, valIdx: { type: 'integer', minimum: 0 } }, additionalProperties: false },
        run: ({ valIdx, ...ref }) => {
          const { control } = withControl(ref);
          ig.require(ig.call('iplug_webmcp_set_control_default', control.idx, valIdx === undefined ? -1 : valIdx), 'Could not reset control value.');
          return { control: this._resolveControl(ig.tree(), this._controlKey(control)) };
        }
      });

      const flagTool = (name, key, exportName) => this._add(name, {
        description: `Set the ${key} flag of a control (by tag or index). Undoable.`,
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { ...controlRef, [key]: { type: 'boolean' } }, required: [key], additionalProperties: false },
        run: (input) => {
          const { tree, control } = withControl(input);
          const before = control[key];
          ig.require(ig.call(exportName, control.idx, input[key] ? 1 : 0), `Could not set ${key}.`);
          this._pushUndo(name, tree, control, (idx) => ig.call(exportName, idx, before ? 1 : 0));
          return { control: this._resolveControl(ig.tree(), this._controlKey(control)) };
        }
      });
      flagTool('set_control_hidden', 'hidden', 'iplug_webmcp_set_control_hidden');
      flagTool('set_control_disabled', 'disabled', 'iplug_webmcp_set_control_disabled');

      this._add('set_control_text', {
        description: 'Set the string of an ITextControl or the label of a vector (IV*) control. Undoable.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { ...controlRef, text: { type: 'string' } }, required: ['text'], additionalProperties: false },
        run: ({ text, ...ref }) => {
          const { tree, control } = withControl(ref);
          const before = control.text !== undefined ? control.text : control.label;
          ig.require(ig.callWithStrings('iplug_webmcp_set_control_text', ['number', 'string'], [control.idx, text]), 'Could not set text.');
          if (before !== undefined) this._pushUndo('set_control_text', tree, control, (idx) => ig.callWithStrings('iplug_webmcp_set_control_text', ['number', 'string'], [idx, before]));
          return { control: this._resolveControl(ig.tree(), this._controlKey(control)) };
        }
      });

      this._add('set_control_style', {
        description: 'Change IVStyle properties of a vector control: colors {bg,fg,pr,fr,hl,sh,x1,x2,x3: "#RRGGBB[AA]"}, showLabel, showValue, drawFrame, drawShadows, emboss, roundness, frameThickness, shadowOffset, widgetFrac, angle, label. Undoable.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { ...controlRef, style: { type: 'object', properties: {
          colors: { type: 'object', additionalProperties: { type: 'string', pattern: colorPattern } },
          showLabel: { type: 'boolean' }, showValue: { type: 'boolean' }, drawFrame: { type: 'boolean' }, drawShadows: { type: 'boolean' }, emboss: { type: 'boolean' },
          roundness: { type: 'number', minimum: 0, maximum: 1 }, frameThickness: { type: 'number', minimum: 0 }, shadowOffset: { type: 'number' },
          widgetFrac: { type: 'number', minimum: 0, maximum: 1 }, angle: { type: 'number', minimum: 0, maximum: 360 }, label: { type: 'string' }
        }, additionalProperties: false } }, required: ['style'], additionalProperties: false },
        run: ({ style, ...ref }) => {
          const { tree, control } = withControl(ref);
          if (!control.style) throw new BridgeError('Control is not an IVectorBase control.', 'not-supported');
          for (const [name, color] of Object.entries(style.colors || {})) {
            if (!new RegExp(colorPattern).test(String(color))) throw new BridgeError(`colors.${name} must be #RRGGBB or #RRGGBBAA.`, 'invalid-input');
          }
          const props = this._styleToProps(style);
          const previous = {};
          for (const key of Object.keys(props)) {
            if (key.startsWith('color.')) previous[key] = control.style.colors?.[key.slice(6)];
            else if (key === 'label') previous[key] = control.label;
            else previous[key] = control.style[key];
          }
          this._captureBaseline(tree);
          this._setProps(control.idx, props);
          this._pushUndo('set_control_style', tree, control, (idx) => {
            this._setProps(idx, Object.fromEntries(Object.entries(previous).filter(([, v]) => v !== undefined)));
            return true;
          });
          return { control: this._resolveControl(ig.tree(), this._controlKey(control)) };
        }
      });

      this._add('set_control_bounds', {
        description: 'Move or resize a control in logical IGraphics units: bounds {l,t,r,b}; optional targetBounds overrides the hit-test rect, otherwise the control computes it (label-excluding areas are preserved). Undoable; export_layout diffs against the first edit.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { ...controlRef, bounds: rectSchema, targetBounds: rectSchema }, required: ['bounds'], additionalProperties: false },
        run: ({ bounds, targetBounds, ...ref }) => {
          const { tree, control } = withControl(ref);
          for (const rect of [bounds, targetBounds].filter(Boolean)) {
            if (rect.r <= rect.l || rect.b <= rect.t) throw new BridgeError('Bounds must be non-empty.', 'invalid-input');
            if (rect.l < 0 || rect.t < 0 || rect.r > tree.width || rect.b > tree.height) throw new BridgeError(`Bounds must lie inside the ${tree.width}x${tree.height} UI.`, 'invalid-input');
          }
          this._captureBaseline(tree);
          const t = targetBounds || bounds;
          ig.require(ig.call('iplug_webmcp_set_control_bounds', control.idx, bounds.l, bounds.t, bounds.r, bounds.b, t.l, t.t, t.r, t.b, targetBounds ? 1 : 0), 'Could not set bounds.');
          const { bounds: pb, targetBounds: pt } = control;
          this._pushUndo('set_control_bounds', tree, control, (idx) => ig.call('iplug_webmcp_set_control_bounds', idx, pb.l, pb.t, pb.r, pb.b, pt.l, pt.t, pt.r, pt.b, 1));
          return { control: this._resolveControl(ig.tree(), this._controlKey(control)) };
        }
      });

      this._add('set_background_color', {
        description: 'Set a solid background colour (#RRGGBB or #RRGGBBAA) on the IPanelControl background. Undoable.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: { color: { type: 'string', pattern: colorPattern } }, required: ['color'], additionalProperties: false },
        run: ({ color }) => {
          const tree = ig.tree();
          if (!tree.backgroundColor) throw new BridgeError('The background is not a solid IPanelControl.', 'not-supported');
          this._captureBaseline(tree);
          ig.require(ig.callWithStrings('iplug_webmcp_set_background_color', ['string'], [color]), 'Could not set background colour.');
          const before = tree.backgroundColor;
          this._undo.push({ label: 'set_background_color', sizeKey: this._sizeKey(tree), key: null, undo: () => ig.callWithStrings('iplug_webmcp_set_background_color', ['string'], [before]) });
          return { backgroundColor: ig.tree().backgroundColor };
        }
      });

      this._add('undo', {
        description: 'Undo the most recent tool-driven UI change (value, flag, text, style, bounds or background). Refuses if the UI was resized or the control can no longer be found.',
        gate: () => this._gateUI(), requires: ['ui'],
        run: () => {
          const entry = this._undo[this._undo.length - 1];
          if (!entry) throw new BridgeError('Nothing to undo.', 'not-found');
          const tree = ig.tree();
          if (this._sizeKey(tree) !== entry.sizeKey) throw new BridgeError('The UI was resized since that change; reload before undoing.', 'stale');
          let idx = -1;
          if (entry.key) idx = this._resolveControl(tree, entry.key).idx;
          const ok = entry.undo(idx);
          ig.require(ok !== false, `Could not undo ${entry.label}.`);
          this._undo.pop();
          return { undone: entry.label, remaining: this._undo.length, ...(entry.key ? { control: this._resolveControl(ig.tree(), entry.key) } : { backgroundColor: ig.tree().backgroundColor }) };
        }
      });

      this._add('export_layout', {
        description: 'Diff the current layout against the state before the first bounds/style/background edit: an iplug-layout-patch (version 2) a coding agent can apply to the C++ layout code. Untagged controls are identified by transient index.',
        readOnly: true, gate: () => this._gateUI(), requires: ['ui'],
        run: () => {
          const baseline = this._layoutBaseline;
          if (!baseline) throw new BridgeError('No layout edits have been made yet.', 'not-found');
          const current = ig.tree();
          if (this._sizeKey(current) !== this._sizeKey(baseline)) throw new BridgeError('The UI was resized since the baseline; reload before exporting.', 'stale');
          const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
          const changes = current.controls.flatMap((control) => {
            const before = baseline.controls.find((c) => control.tag >= 0 ? c.tag === control.tag : c.idx === control.idx);
            if (!before) return [];
            const geometryChanged = !same([before.bounds, before.targetBounds], [control.bounds, control.targetBounds]);
            const styleChanged = !same([before.style, before.label, before.text], [control.style, control.label, control.text]);
            if (!geometryChanged && !styleChanged) return [];
            return [{
              tag: control.tag, index: control.idx, persistentIdentity: control.tag >= 0, className: control.className,
              before: { bounds: before.bounds, targetBounds: before.targetBounds, ...(styleChanged ? { style: before.style, label: before.label, text: before.text } : {}) },
              after: { bounds: control.bounds, targetBounds: control.targetBounds, ...(styleChanged ? { style: control.style, label: control.label, text: control.text } : {}) }
            }];
          });
          return {
            format: 'iplug-layout-patch', version: 2, plugin: this.pluginName, coordinateSpace: 'IGraphics logical units',
            width: current.width, height: current.height,
            background: current.backgroundColor !== baseline.backgroundColor ? { target: 'panel-background', before: baseline.backgroundColor, after: current.backgroundColor } : null,
            changes
          };
        }
      });

      this._add('set_layout_editing', {
        description: 'Toggle the visual live edit overlay (grid, drag handles, delete) so a human or pointer simulation can rearrange controls; edits appear in get_events as iplug:live-edit:* events. Needs a build with IPLUG2_WASM_LIVE_EDIT=ON.',
        gate: () => this._gateLiveEdit(), requires: ['ui', 'live-edit'],
        input: { type: 'object', properties: { enabled: { type: 'boolean' } }, required: ['enabled'], additionalProperties: false },
        run: ({ enabled }) => {
          const before = ig.tree();
          this._captureBaseline(before);
          if (!this.element.setLiveEditEnabled(enabled)) throw new BridgeError('Live edit could not be toggled.', 'igraphics-error');
          this.hostControls?.setLiveEditActive?.(enabled);
          return { liveEditEnabled: ig.tree().liveEditEnabled };
        }
      });

      this._add('pointer', {
        description: 'Simulate pointer input on the IGraphics canvas in logical units. action: down | up | move | click | drag (x,y to x2,y2 over steps) | wheel (deltaY). button 0 = left, 2 = right. Exercises the controls\' real mouse handlers.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: {
          action: { type: 'string', enum: ['down', 'up', 'move', 'click', 'drag', 'wheel'] },
          x: { type: 'number' }, y: { type: 'number' }, x2: { type: 'number' }, y2: { type: 'number' },
          button: { type: 'integer', enum: [0, 2] }, steps: { type: 'integer', minimum: 1, maximum: 200 },
          deltaY: { type: 'number' }, modifiers: modifiersSchema
        }, required: ['action', 'x', 'y'], additionalProperties: false },
        run: async ({ action, x, y, x2, y2, button = 0, steps = 10, deltaY = 0, modifiers = {} }) => {
          const tree = ig.tree();
          const inside = (px, py) => px >= 0 && py >= 0 && px <= tree.width && py <= tree.height;
          if (!inside(x, y) || (action === 'drag' && (x2 === undefined || y2 === undefined || !inside(x2, y2)))) {
            throw new BridgeError(`Coordinates must lie inside the ${tree.width}x${tree.height} UI (drag also needs x2,y2).`, 'invalid-input');
          }
          const mods = [modifiers.shift ? 1 : 0, modifiers.ctrl ? 1 : 0, modifiers.alt ? 1 : 0];
          const buttonsMask = button === 2 ? 2 : 1;
          const mouse = (type, px, py, dx, dy, buttons) => ig.require(ig.call('iplug_webmcp_mouse', type, px, py, dx, dy, buttons, button, ...mods), 'Pointer event rejected.');

          switch (action) {
            case 'down': mouse(0, x, y, 0, 0, buttonsMask); break;
            case 'up': mouse(1, x, y, 0, 0, 0); break;
            case 'move': mouse(2, x, y, 0, 0, 0); break;
            case 'click': mouse(0, x, y, 0, 0, buttonsMask); await nextFrame(); mouse(1, x, y, 0, 0, 0); break;
            case 'wheel': ig.require(ig.call('iplug_webmcp_wheel', x, y, deltaY, ...mods), 'Wheel event rejected.'); break;
            case 'drag': {
              mouse(0, x, y, 0, 0, buttonsMask);
              let px = x, py = y;
              for (let i = 1; i <= steps; i++) {
                await nextFrame();
                const nx = x + (x2 - x) * i / steps;
                const ny = y + (y2 - y) * i / steps;
                mouse(2, nx, ny, nx - px, ny - py, buttonsMask);
                px = nx; py = ny;
              }
              await nextFrame();
              mouse(1, x2, y2, 0, 0, 0);
              break;
            }
          }
          await nextFrame();
          return { action, lastEventSeq: this._eventSeq, tree: treeResult() };
        }
      });

      this._add('key', {
        description: 'Send a key to the IGraphics UI at the last pointer position: key is a KeyboardEvent.key name ("a", "Enter", "ArrowUp", "F1"), action press | down | up. While a text entry is open, printable keys type into it.',
        gate: () => this._gateUI(), requires: ['ui'],
        input: { type: 'object', properties: {
          key: { type: 'string', minLength: 1 }, vk: { type: 'integer', minimum: 0 }, action: { type: 'string', enum: ['press', 'down', 'up'] }, modifiers: modifiersSchema
        }, required: ['key'], additionalProperties: false },
        run: async ({ key, vk, action = 'press', modifiers = {} }) => {
          const tree = ig.tree();
          const code = vk !== undefined ? vk : keyToVK(key);
          const utf8 = key.length === 1 ? key : '';
          const mods = [modifiers.shift ? 1 : 0, modifiers.ctrl ? 1 : 0, modifiers.alt ? 1 : 0];

          // A DOM text field (e.g. a platform text entry) that has focus receives printable keys directly.
          const active = this.element?.shadowRoot?.activeElement || document.activeElement;
          if (tree.inTextEntry && active && (active.tagName === 'INPUT' || active.tagName === 'TEXTAREA') && action !== 'up') {
            if (utf8) {
              active.value += utf8;
              active.dispatchEvent(new Event('input', { bubbles: true }));
            } else {
              active.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true }));
              active.dispatchEvent(new KeyboardEvent('keyup', { key, bubbles: true }));
            }
            return { handled: true, target: 'dom-text-entry' };
          }

          const send = (down) => ig.callWithStrings('iplug_webmcp_key', ['number', 'string', 'number', 'number', 'number', 'number'], [code, utf8, ...mods, down ? 1 : 0]);
          let handled = false;
          if (action !== 'up') handled = send(true) || handled;
          if (action !== 'down') { await nextFrame(); handled = send(false) || handled; }
          return { handled, vk: code };
        }
      });

      this._add('capture_screenshot', {
        description: 'Capture the IGraphics canvas as a PNG data URL (after drawing any pending changes). Optionally crop to one control (tag or index) with padding in logical units. Needs preserveDrawingBuffer (see get_status hints).',
        readOnly: true, gate: () => this._gateScreenshot(), requires: ['ui', 'screenshot'],
        input: { type: 'object', properties: { ...controlRef, padding: { type: 'number', minimum: 0 } }, additionalProperties: false },
        run: async ({ padding = 0, ...ref }) => {
          const tree = ig.tree();
          const canvas = ig.canvas;
          const cropTo = ref.tag !== undefined || ref.index !== undefined;
          const control = cropTo ? this._resolveControl(tree, ref) : null;
          ig.flushDraw();

          const noImage = () => new BridgeError('The canvas produced an empty image.', 'screenshot-unavailable', 'Enable preserveDrawingBuffer: add ?iplugWasmCapture=1 to the URL or set Module.iplugPreserveDrawingBuffer = true before the UI loads, then reload.');
          const result = { logicalWidth: tree.width, logicalHeight: tree.height, drawScale: tree.drawScale, screenScale: tree.screenScale };

          if (!control) {
            const dataUrl = canvas.toDataURL('image/png');
            if (!dataUrl || dataUrl === 'data:,' || dataUrl.length < 100) throw noImage();
            return { dataUrl, width: canvas.width, height: canvas.height, ...result };
          }

          // Crop: logical units -> canvas (backing) pixels, clamped to the UI
          const b = control.bounds;
          const region = {
            l: Math.max(0, b.l - padding), t: Math.max(0, b.t - padding),
            r: Math.min(tree.width, b.r + padding), b: Math.min(tree.height, b.b + padding)
          };
          const scaleX = canvas.width / tree.width;
          const scaleY = canvas.height / tree.height;
          const sx = Math.floor(region.l * scaleX), sy = Math.floor(region.t * scaleY);
          const sw = Math.max(1, Math.ceil(region.r * scaleX) - sx), sh = Math.max(1, Math.ceil(region.b * scaleY) - sy);
          const crop = document.createElement('canvas');
          crop.width = sw; crop.height = sh;
          const ctx = crop.getContext('2d');
          if (!ctx) throw new BridgeError('Could not create a 2D context for cropping.', 'screenshot-unavailable');
          ctx.drawImage(canvas, sx, sy, sw, sh, 0, 0, sw, sh);
          const dataUrl = crop.toDataURL('image/png');
          if (!dataUrl || dataUrl === 'data:,' || dataUrl.length < 100) throw noImage();
          return { dataUrl, width: sw, height: sh, ...result, control: this._controlKey(control), padding, region, coordinateSpace: 'IGraphics logical units' };
        }
      });
    }

    //--- WebView tools ------------------------------------------------------------

    _defineWebViewTools() {
      this._add('get_webview_controls', {
        description: `Discover ${this.pluginName} WebView UI controls: parameter-bound elements ([param-id]) and actions (registered via window.iPlugWebMCP.registerAction or annotated with data-iplug-action). Bounds are viewport CSS pixels.`,
        readOnly: true, gate: () => this._gateWebView(), requires: ['webview'],
        run: () => this.webview.discover()
      });

      this._add('activate_webview_action', {
        description: 'Activate a WebView action by id: registered actions run their async handler (with optional input); DOM actions receive a click. Rejects unknown, duplicate, disabled or hidden targets.',
        gate: () => this._gateWebView(), requires: ['webview'],
        input: { type: 'object', properties: { id: { type: 'string', minLength: 1 }, input: { type: 'object' } }, required: ['id'], additionalProperties: false },
        run: ({ id, input }, ctx) => this.webview.activate(id, input, ctx?.signal)
      });
    }
  }

  IPlugWasmWebMCP.BridgeError = BridgeError;
  IPlugWasmWebMCP.validate = validate;
  IPlugWasmWebMCP.installShim = installShim;
  IPlugWasmWebMCP.WebViewAdapter = WebViewAdapter;
  IPlugWasmWebMCP.IGraphicsAdapter = IGraphicsAdapter;
  IPlugWasmWebMCP.keyToVK = keyToVK;

  window.IPlugWasmWebMCP = IPlugWasmWebMCP;
})();
