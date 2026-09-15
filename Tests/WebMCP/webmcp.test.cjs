const { test } = require('node:test');
const assert = require('node:assert'); // non-strict: bridge results are created in a vm realm
const { loadBridgeScript, makeModelContext } = require('./helpers.cjs');

/** A fake <iplug-test> element with a fake UI module implementing the _iplug_webmcp_* exports over an in-memory tree. */
function makeElement({ uiReady = true, liveEdit = true, bridge = true, preserve = true } = {}) {
  const strings = new Map();
  let nextPtr = 1;
  const intern = (s) => { const ptr = nextPtr++; strings.set(ptr, s); return ptr; };
  const tree = {
    width: 600, height: 400, drawScale: 1, screenScale: 2, liveEditAvailable: liveEdit, liveEditEnabled: false, inTextEntry: false,
    classNames: true, backgroundColor: '#eeeeeeff', nControls: 3,
    controls: [
      { idx: 0, tag: -1, className: 'IPanelControl', group: '', parentIdx: -1, tooltip: '', hidden: false, disabled: false, bounds: { l: 0, t: 0, r: 600, b: 400 }, targetBounds: { l: 0, t: 0, r: 600, b: 400 }, vals: [{ valIdx: 0, paramIdx: -1, value: 0 }], pattern: '#eeeeeeff' },
      { idx: 1, tag: 42, className: 'IVSliderControl', group: '', parentIdx: -1, tooltip: '', hidden: false, disabled: false, bounds: { l: 10, t: 10, r: 100, b: 100 }, targetBounds: { l: 10, t: 20, r: 100, b: 100 }, vals: [{ valIdx: 0, paramIdx: 0, value: 0.5, display: '50 %' }], label: 'Gain', style: { showLabel: true, roundness: 0, colors: { fg: '#111111ff', bg: '#00000000' } } },
      { idx: 2, tag: 7, className: 'ITextControl', group: '', parentIdx: -1, tooltip: '', hidden: false, disabled: false, bounds: { l: 200, t: 10, r: 300, b: 40 }, targetBounds: { l: 200, t: 10, r: 300, b: 40 }, vals: [{ valIdx: 0, paramIdx: -1, value: 0 }], text: 'Title' }
    ]
  };
  const calls = [];
  let lastError = '';
  const mod = {
    UTF8ToString: (ptr) => strings.get(ptr),
    ccall: (name, ret, types, args) => { if (types.length !== args.length) throw new Error(`ccall ${name}: ${types.length} types for ${args.length} args`); return mod[`_${name}`](...args); },
    _iplug_webmcp_get_ui_tree: () => intern(JSON.stringify(tree)),
    _iplug_webmcp_last_error: () => intern(lastError),
    _iplug_webmcp_live_edit_available: () => (liveEdit ? 1 : 0),
    _iplug_webmcp_set_control_value: (h, idx, valIdx, v) => { calls.push(['value', idx, valIdx, v]); tree.controls[idx].vals[valIdx].value = v; return 1; },
    _iplug_webmcp_set_control_default: (h, idx) => { calls.push(['default', idx]); tree.controls[idx].vals[0].value = 0.5; return 1; },
    _iplug_webmcp_set_control_hidden: (h, idx, f) => { tree.controls[idx].hidden = !!f; return 1; },
    _iplug_webmcp_set_control_disabled: (h, idx, f) => { tree.controls[idx].disabled = !!f; return 1; },
    _iplug_webmcp_set_control_text: (h, idx, str) => { const c = tree.controls[idx]; if (c.text !== undefined) c.text = str; else if (c.label !== undefined) c.label = str; else { lastError = 'Control has no text or label.'; return 0; } return 1; },
    _iplug_webmcp_set_control_prop: (h, idx, key, value) => {
      const c = tree.controls[idx]; calls.push(['prop', idx, key, value]);
      if (!c.style) { lastError = 'Control is not an IVectorBase control.'; return 0; }
      if (key.startsWith('color.')) c.style.colors[key.slice(6)] = value;
      else if (key === 'label') c.label = value;
      else c.style[key] = value === 'true' ? true : value === 'false' ? false : Number(value);
      return 1;
    },
    _iplug_webmcp_set_control_bounds: (h, idx, l, t, r, b, tl, tt, tr, tb, override) => {
      calls.push(['bounds', idx, override]);
      tree.controls[idx].bounds = { l, t, r, b };
      tree.controls[idx].targetBounds = override ? { l: tl, t: tt, r: tr, b: tb } : { l, t: t + 10, r, b };
      return 1;
    },
    _iplug_webmcp_set_background_color: (h, hex) => { tree.backgroundColor = hex.toLowerCase(); tree.controls[0].pattern = hex.toLowerCase(); return 1; },
    _iplug_webmcp_mouse: (h, type, x, y, dx, dy, buttons, button, s, c, a) => { calls.push(['mouse', type, x, y, dx, dy, buttons, button]); return 1; },
    _iplug_webmcp_wheel: (h, x, y, d) => { calls.push(['wheel', x, y, d]); return 1; },
    _iplug_webmcp_key: (h, vk, utf8, s, c, a, down) => { calls.push(['key', vk, utf8, down]); return 1; },
    _iplug_webmcp_flush_draw: () => { calls.push(['flush']); return 1; },
    _iplug_set_live_edit: (h, enabled) => { tree.liveEditEnabled = !!enabled; return 1; }
  };
  if (!bridge) delete mod._iplug_webmcp_get_ui_tree;
  if (!liveEdit) delete mod._iplug_set_live_edit;
  const canvas = {
    _iplugGraphics: 123, clientWidth: 600, clientHeight: 400, width: 1200, height: 800,
    toDataURL: () => (preserve ? 'data:image/png;base64,' + 'A'.repeat(200) : 'data:,'),
    getContext: () => ({ getContextAttributes: () => ({ preserveDrawingBuffer: preserve }) })
  };
  const element = {
    isUIReady: uiReady, module: mod, canvas, controller: null, shadowRoot: { activeElement: null },
    get graphicsHandle() { return canvas._iplugGraphics; },
    setLiveEditEnabled: (enabled) => mod._iplug_set_live_edit?.(123, enabled ? 1 : 0) === 1
  };
  return { element, mod, tree, calls, canvas };
}

function makeController() {
  const listeners = new Map();
  const requests = [];
  return {
    isReady: true, audioContext: { state: 'running' }, requests,
    on(event, fn) { listeners.set(event, fn); return () => listeners.delete(event); },
    off(event) { listeners.delete(event); },
    emit(event, data) { listeners.get(event)?.(data); },
    request(op, args, opts) { requests.push({ op, args, opts }); return Promise.resolve({ op, args, aborted: opts?.signal?.aborted }); }
  };
}

function makeHostControls() {
  const state = { audioStarted: false };
  return {
    audioStarted: false, startBtn: { disabled: false }, sourceSelect: { value: 'tone' }, audioBuffer: null, audioContext: { state: 'running' },
    liveEditActive: false, state,
    getHostState() { return { audioEnabled: this.audioStarted, source: this.sourceSelect.value }; },
    async startAudio() { this.audioStarted = true; },
    stopAudio() { this.audioStarted = false; },
    async applySourceSettings(settings) { Object.assign(this.sourceSelect, settings.source ? { value: settings.source } : {}); this.applied = settings; return this.getHostState(); },
    setLiveEditActive(active) { this.liveEditActive = active; }
  };
}

function makeBridge(options = {}, elementOptions = {}) {
  const env = loadBridgeScript();
  const fake = makeElement(elementOptions);
  const controller = options.controller === null ? null : (options.controller || makeController());
  const hostControls = options.hostControls === null ? null : makeHostControls();
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test Plug', backend: 'igraphics', element: fake.element, hostControls, getController: () => controller, ...options.bridge });
  return { env, bridge, controller, hostControls, ...fake };
}

test('registers every tool up front with a plugin-derived prefix and reports availability', async () => {
  const { env, bridge, tree } = makeBridge({ controller: null });
  assert.equal(bridge.prefix, 'iplug_test_plug');
  assert.ok(env.modelContext.tools.has('iplug_test_plug_get_status'));
  assert.ok(env.modelContext.tools.has('iplug_test_plug_set_parameter'));
  assert.ok(env.modelContext.tools.has('iplug_test_plug_set_layout_editing'));
  const status = await bridge.call('get_status');
  assert.equal(status.webmcp, 'native');
  assert.equal(status.dsp.controllerReady, false);
  assert.equal(status.ui.width, tree.width);
  assert.equal(status.capabilities.parameters, false);
  assert.equal(status.capabilities.pointer, true);
  assert.ok(status.hints.some((h) => /Start Audio/.test(h)));
  const byName = Object.fromEntries(status.tools.map((t) => [t.name.replace('iplug_test_plug_', ''), t.available]));
  assert.equal(byName.get_parameters, false);
  assert.equal(byName.get_ui_tree, true);
  assert.equal(byName.set_layout_editing, true);
});

test('gated tools fail with structured codes and hints before the DSP runs', async () => {
  const { bridge } = makeBridge({ controller: null });
  await assert.rejects(bridge.call('get_parameters'), (e) => e.code === 'dsp-not-ready' && /set_audio_enabled/.test(e.message));
  await assert.rejects(bridge.call('set_parameter', { paramIdx: 0, normalized: 2 }), (e) => e.code === 'invalid-input');
  await assert.rejects(bridge.call('set_parameter', { paramIdx: 0, normalized: 0.5, extra: 1 }), /unknown property/);
  await assert.rejects(bridge.call('nope'), (e) => e.code === 'unknown-tool');
});

test('ui tools fail with ui-not-ready / not-supported when the module is missing', async () => {
  const notReady = makeBridge({}, { uiReady: false });
  await assert.rejects(notReady.bridge.call('get_ui_tree'), (e) => e.code === 'ui-not-ready');
  const noBridge = makeBridge({}, { bridge: false });
  await assert.rejects(noBridge.bridge.call('get_ui_tree'), (e) => e.code === 'not-supported');
  const noLiveEdit = makeBridge({}, { liveEdit: false });
  await assert.rejects(noLiveEdit.bridge.call('set_layout_editing', { enabled: true }), (e) => e.code === 'live-edit-unavailable');
  await noLiveEdit.bridge.call('set_control_bounds', { tag: 42, bounds: { l: 20, t: 20, r: 120, b: 120 } }); // bounds do not need live edit
});

test('parameter and state tools forward to controller.request with the abort signal', async () => {
  const { bridge, controller } = makeBridge();
  const abort = new AbortController();
  await bridge.call('set_parameter', { paramIdx: 0, normalized: 0.25 }, { signal: abort.signal });
  await bridge.call('set_parameters', { values: [{ paramIdx: 0, text: '3 dB' }] });
  await bridge.call('reset_parameter', {});
  await bridge.call('get_state');
  await bridge.call('set_state', { base64: 'AAAA' });
  await bridge.call('restore_preset', { presetIdx: 1 });
  assert.deepEqual(controller.requests.map((r) => r.op), ['setParameter', 'setParameters', 'resetParameter', 'getState', 'setState', 'restorePreset']);
  assert.equal(controller.requests[0].opts.signal, abort.signal);
  await assert.rejects(bridge.call('set_state', { base64: '' }), /at least 1/);
});

test('control tools resolve tags to indices, run through the UI path and support undo', async () => {
  const { bridge, tree, calls } = makeBridge();
  const result = await bridge.call('set_control_value', { tag: 42, normalized: 0.9 });
  assert.equal(result.control.vals[0].value, 0.9);
  assert.equal(result.dspSynced, true);
  assert.deepEqual(calls.at(-1), ['value', 1, 0, 0.9]);
  await assert.rejects(bridge.call('set_control_value', { tag: 42, index: 1, normalized: 0.1 }), /exactly one/);
  await assert.rejects(bridge.call('set_control_value', { tag: 99, normalized: 0.1 }), (e) => e.code === 'not-found');

  await bridge.call('set_control_hidden', { tag: 7, hidden: true });
  assert.equal(tree.controls[2].hidden, true);
  await bridge.call('set_control_text', { tag: 7, text: 'Hello' });
  assert.equal(tree.controls[2].text, 'Hello');
  await bridge.call('set_control_text', { tag: 42, text: 'Vol' });
  assert.equal(tree.controls[1].label, 'Vol');
  await assert.rejects(bridge.call('set_control_text', { index: 0, text: 'x' }), /text or label/);

  let undo = await bridge.call('undo');
  assert.equal(undo.undone, 'set_control_text');
  assert.equal(tree.controls[1].label, 'Gain');
  undo = await bridge.call('undo');
  assert.equal(tree.controls[2].text, 'Title');
  undo = await bridge.call('undo');
  assert.equal(tree.controls[2].hidden, false);
  undo = await bridge.call('undo');
  assert.equal(tree.controls[1].vals[0].value, 0.5);
  await assert.rejects(bridge.call('undo'), /Nothing to undo/);
});

test('style, bounds and background edits are undoable and exported as a layout patch', async () => {
  const { bridge, tree, calls } = makeBridge();
  await assert.rejects(bridge.call('export_layout'), /No layout edits/);
  await bridge.call('set_control_style', { tag: 42, style: { colors: { fg: '#ff0000' }, roundness: 0.5, label: 'Level' } });
  assert.equal(tree.controls[1].style.colors.fg, '#ff0000');
  assert.equal(tree.controls[1].style.roundness, 0.5);
  assert.equal(tree.controls[1].label, 'Level');
  await assert.rejects(bridge.call('set_control_style', { tag: 7, style: { roundness: 1 } }), (e) => e.code === 'not-supported');
  await assert.rejects(bridge.call('set_control_style', { tag: 42, style: { colors: { fg: 'red' } } }), /invalid format|RRGGBB/);

  await bridge.call('set_control_bounds', { tag: 42, bounds: { l: 20, t: 20, r: 120, b: 120 } });
  assert.deepEqual(calls.at(-1), ['bounds', 1, 0]);
  assert.deepEqual(tree.controls[1].targetBounds, { l: 20, t: 30, r: 120, b: 120 }); // control-computed hit area kept
  await bridge.call('set_control_bounds', { index: 1, bounds: { l: 30, t: 30, r: 130, b: 130 }, targetBounds: { l: 30, t: 30, r: 130, b: 130 } });
  assert.deepEqual(calls.at(-1), ['bounds', 1, 1]);
  await assert.rejects(bridge.call('set_control_bounds', { tag: 42, bounds: { l: 0, t: 0, r: 700, b: 10 } }), /inside/);
  await assert.rejects(bridge.call('set_control_bounds', { tag: 42, bounds: { l: 50, t: 50, r: 40, b: 60 } }), /non-empty/);
  await bridge.call('set_background_color', { color: '#FF0000' });
  assert.equal(tree.backgroundColor, '#ff0000');

  const patch = await bridge.call('export_layout');
  assert.equal(patch.format, 'iplug-layout-patch');
  assert.equal(patch.version, 2);
  assert.equal(patch.background.before, '#eeeeeeff');
  assert.equal(patch.background.after, '#ff0000');
  assert.equal(patch.changes.length, 1);
  assert.equal(patch.changes[0].tag, 42);
  assert.equal(patch.changes[0].persistentIdentity, true);
  assert.deepEqual(patch.changes[0].before.bounds, { l: 10, t: 10, r: 100, b: 100 });
  assert.deepEqual(patch.changes[0].after.bounds, { l: 30, t: 30, r: 130, b: 130 });
  assert.equal(patch.changes[0].before.style.roundness, 0);

  await bridge.call('undo'); // background
  assert.equal(tree.backgroundColor, '#eeeeeeff');
  await bridge.call('undo'); // second bounds
  await bridge.call('undo'); // first bounds
  assert.deepEqual(tree.controls[1].bounds, { l: 10, t: 10, r: 100, b: 100 });
  await bridge.call('undo'); // style
  assert.equal(tree.controls[1].style.colors.fg, '#111111ff');
  assert.equal(tree.controls[1].label, 'Gain');
  assert.equal((await bridge.call('export_layout')).changes.length, 0);

  tree.width = 700;
  await bridge.call('set_control_hidden', { tag: 7, hidden: true });
  tree.width = 800;
  await assert.rejects(bridge.call('undo'), /resized/);
  await assert.rejects(bridge.call('export_layout'), /resized/);
});

test('pointer simulation sends logical coordinates with per-step deltas', async () => {
  const { bridge, calls } = makeBridge();
  await bridge.call('pointer', { action: 'click', x: 50, y: 60 });
  assert.deepEqual(calls.filter((c) => c[0] === 'mouse'), [['mouse', 0, 50, 60, 0, 0, 1, 0], ['mouse', 1, 50, 60, 0, 0, 0, 0]]);
  calls.length = 0;
  await bridge.call('pointer', { action: 'drag', x: 0, y: 0, x2: 100, y2: 50, steps: 2, button: 2 });
  const mouse = calls.filter((c) => c[0] === 'mouse');
  assert.deepEqual(mouse, [
    ['mouse', 0, 0, 0, 0, 0, 2, 2],
    ['mouse', 2, 50, 25, 50, 25, 2, 2],
    ['mouse', 2, 100, 50, 50, 25, 2, 2],
    ['mouse', 1, 100, 50, 0, 0, 0, 2]
  ]);
  await bridge.call('pointer', { action: 'wheel', x: 10, y: 10, deltaY: -3 });
  assert.deepEqual(calls.at(-1), ['wheel', 10, 10, -3]);
  await assert.rejects(bridge.call('pointer', { action: 'drag', x: 10, y: 10 }), /x2,y2/);
  await assert.rejects(bridge.call('pointer', { action: 'move', x: 1000, y: 10 }), /inside/);
});

test('keys map to virtual key codes and route to a focused DOM text field during text entry', async () => {
  const { bridge, calls, tree, element } = makeBridge();
  await bridge.call('key', { key: 'a' });
  assert.deepEqual(calls.filter((c) => c[0] === 'key'), [['key', 0x41, 'a', 1], ['key', 0x41, 'a', 0]]);
  calls.length = 0;
  await bridge.call('key', { key: 'ArrowUp', action: 'down' });
  assert.deepEqual(calls.filter((c) => c[0] === 'key'), [['key', 0x26, '', 1]]);
  calls.length = 0;
  const input = { tagName: 'INPUT', value: 'ab', dispatched: [], dispatchEvent(e) { this.dispatched.push(e.type); } };
  tree.inTextEntry = true;
  element.shadowRoot.activeElement = input;
  const result = await bridge.call('key', { key: 'c' });
  assert.equal(result.target, 'dom-text-entry');
  assert.equal(input.value, 'abc');
  assert.equal(calls.filter((c) => c[0] === 'key').length, 0);
  await bridge.call('key', { key: 'Enter' });
  assert.deepEqual(input.dispatched, ['input', 'keydown', 'keyup']);
});

test('screenshots flush drawing first and explain a missing drawing buffer', async () => {
  const ok = makeBridge();
  const shot = await ok.bridge.call('capture_screenshot');
  assert.ok(shot.dataUrl.startsWith('data:image/png'));
  assert.equal(shot.width, 1200);
  assert.equal(shot.logicalWidth, 600);
  assert.deepEqual(ok.calls.at(-1), ['flush']);
  const bad = makeBridge({}, { preserve: false });
  await assert.rejects(bad.bridge.call('capture_screenshot'), (e) => e.code === 'screenshot-unavailable' && /iplugWasmCapture/.test(e.message));
  const status = await bad.bridge.call('get_status');
  assert.equal(status.capabilities.screenshot, false);
});

test('screenshots can be cropped to a control with padding, clamped to the UI', async () => {
  const { bridge, env } = makeBridge();
  const draws = [];
  env.scope.document.createElement = (tag) => ({
    tag, width: 0, height: 0,
    getContext: () => ({ drawImage: (...args) => draws.push(args.slice(1)) }),
    toDataURL: () => 'data:image/png;base64,' + 'B'.repeat(200)
  });
  // slider tag 42: bounds l10 t10 r100 b100 on a 600x400 UI drawn at 1200x800 (2x backing)
  const shot = await bridge.call('capture_screenshot', { tag: 42, padding: 20 });
  assert.deepEqual(shot.region, { l: 0, t: 0, r: 120, b: 120 }); // clamped at 0
  assert.deepEqual(draws[0], [0, 0, 240, 240, 0, 0, 240, 240]);
  assert.equal(shot.width, 240);
  assert.deepEqual(shot.control, { tag: 42 });
  assert.equal(shot.padding, 20);
  const plain = await bridge.call('capture_screenshot', { index: 2 });
  assert.deepEqual(plain.region, { l: 200, t: 10, r: 300, b: 40 });
  assert.deepEqual(draws[1], [400, 20, 200, 60, 0, 0, 200, 60]);
  await assert.rejects(bridge.call('capture_screenshot', { tag: 42, index: 2 }), /exactly one/);
  await assert.rejects(bridge.call('capture_screenshot', { tag: 99 }), (e) => e.code === 'not-found');
  await assert.rejects(bridge.call('capture_screenshot', { padding: -1 }), /must be >= 0/);
  const full = await bridge.call('capture_screenshot');
  assert.equal(full.width, 1200);
  assert.equal(full.region, undefined);
});

test('live edit toggling syncs the footer and live edit events are buffered', async () => {
  const { bridge, env, tree, hostControls } = makeBridge();
  await bridge.call('set_layout_editing', { enabled: true });
  assert.equal(tree.liveEditEnabled, true);
  assert.equal(hostControls.liveEditActive, true);
  env.window.dispatchEvent({ type: 'message', source: env.window, data: { type: 'iplug:live-edit:control-changed', control: { tag: 42 } } });
  env.window.dispatchEvent({ type: 'message', source: {}, data: { type: 'iplug:live-edit:control-changed' } }); // foreign source ignored
  env.window.dispatchEvent({ type: 'message', source: env.window, data: { type: 'unrelated' } });
  const events = await bridge.call('get_events', { types: ['iplug:live-edit:control-changed'] });
  assert.equal(events.events.length, 1);
  assert.equal(events.events[0].control.tag, 42);
  const seq = events.lastEventSeq;
  assert.equal((await bridge.call('get_events', { since: seq })).events.length, 0);
});

test('host tools drive the footer helpers and report autoplay blocking without throwing', async () => {
  const { bridge, hostControls, controller } = makeBridge();
  hostControls.audioContext.state = 'suspended';
  let result = await bridge.call('set_audio_enabled', { enabled: true });
  assert.equal(hostControls.audioStarted, true);
  assert.equal(result.reason, 'autoplay-blocked');
  result = await bridge.call('set_audio_enabled', { enabled: true });
  assert.equal(result.changed, false);
  await bridge.call('set_audio_enabled', { enabled: false });
  assert.equal(hostControls.audioStarted, false);
  await assert.rejects(bridge.call('configure_source', { source: 'file' }), /No audio file/);
  await assert.rejects(bridge.call('configure_source', { gainPercent: 101 }), /<= 100/);
  await bridge.call('configure_source', { source: 'noise', gainPercent: 12 });
  assert.deepEqual(hostControls.applied, { source: 'noise', gainPercent: 12 });
  const state = await bridge.call('get_host_state');
  assert.equal(state.source, 'noise');
  controller.emit('paramChange', { paramIdx: 0, value: 0.3 });
  const events = await bridge.call('get_events', { types: ['iplug:param-changed'] });
  assert.equal(events.events.at(-1).value, 0.3);
});

test('mutations are serialized, disposal unregisters, and instances get distinct prefixes', async () => {
  const env = loadBridgeScript();
  const fake = makeElement();
  const order = [];
  const controller = makeController();
  controller.request = async (op) => { order.push(`start ${op}`); await new Promise((r) => setTimeout(r, 5)); order.push(`end ${op}`); return {}; };
  const a = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'igraphics', element: fake.element, getController: () => controller });
  const b = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'igraphics', element: fake.element, getController: () => controller });
  assert.equal(a.prefix, 'iplug_test');
  assert.equal(b.prefix, 'iplug_test_2');
  await Promise.all([a.call('set_state', { base64: 'x' }), a.call('reset_parameter', {})]);
  assert.deepEqual(order, ['start setState', 'end setState', 'start resetParameter', 'end resetParameter']);
  const total = env.modelContext.tools.size;
  b.dispose();
  assert.ok(env.modelContext.tools.size < total);
  assert.ok(![...env.modelContext.tools.keys()].some((n) => n.startsWith('iplug_test_2_')));
  await assert.rejects(b.call('get_status'), (e) => e.code === 'unknown-tool');
  const tool = env.modelContext.tools.get('iplug_test_get_status');
  a.dispose();
  await assert.rejects(tool.execute({}), (e) => e.code === 'disposed');
});

test('the dev shim is installed when no modelContext exists and can list and call tools', async () => {
  const env = loadBridgeScript({ modelContext: null });
  const fake = makeElement();
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'igraphics', element: fake.element, getController: () => null });
  const shim = env.window.__iplugWebMCPShim;
  assert.ok(shim.isShim);
  assert.ok(shim.list().some((t) => t.name === 'iplug_test_get_ui_tree'));
  const tree = await shim.call('iplug_test_get_ui_tree', {});
  assert.equal(tree.controls.length, 3);
  assert.equal(tree.coordinateSpace, 'IGraphics logical units');
  assert.equal((await shim.call('iplug_test_get_status')).webmcp, 'shim');
  await assert.rejects(shim.call('iplug_test_missing'), /Unknown tool/);
  bridge.dispose();
  assert.equal(shim.list().length, 0);
});

test('a forced shim (?iplugWebMCPShim=1) is used even when a native context exists', () => {
  const env = loadBridgeScript({ search: '?iplugWebMCPShim=1' });
  const fake = makeElement();
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'igraphics', element: fake.element });
  assert.equal(env.modelContext.tools.size, 0);
  assert.ok(env.window.__iplugWebMCPShim.list().length > 0);
  bridge.dispose();
});
