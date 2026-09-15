const { test } = require('node:test');
const assert = require('node:assert'); // non-strict: bridge results are created in a vm realm
const { loadBridgeScript } = require('./helpers.cjs');

/** Minimal element stub supporting the attribute/query surface the adapter uses */
function el(tag, attrs = {}, { text = '', shadow = null, disabled = false, hidden = false } = {}) {
  const node = {
    tagName: tag.toUpperCase(), attrs, textContent: text, shadowRoot: shadow, clicks: 0, id: attrs.id || '',
    getAttribute: (k) => (k in attrs ? String(attrs[k]) : null),
    hasAttribute: (k) => k in attrs || (k === 'disabled' && disabled),
    matches: (sel) => sel === ':disabled' && disabled,
    closest: () => null,
    getClientRects: () => (hidden ? [] : [1]),
    getBoundingClientRect: () => ({ left: 1, top: 2, right: 3, bottom: 4 }),
    click() { this.clicks++; }
  };
  return node;
}

function root(children) {
  const all = [];
  const collect = (nodes) => nodes.forEach((n) => { all.push(n); if (n.children) collect(n.children); });
  collect(children);
  const matches = (node, selector) => {
    const attr = selector.match(/^\[(.+)\]$/)?.[1];
    return attr ? node.getAttribute(attr) !== null : selector === '*';
  };
  return { querySelectorAll: (selector) => all.filter((n) => matches(n, selector)) };
}

function makeWebView({ ready = true } = {}) {
  const shadowButton = el('button', { 'data-iplug-action': 'shadow-action' }, { text: 'In shadow' });
  const knob = el('knob-control', { 'param-id': '0', label: 'Gain' }, { shadow: root([shadowButton]) });
  const small = el('button-control', { 'data-iplug-action': 'size-small' }, { text: 'Small' });
  const off = el('button-control', { 'data-iplug-action': 'size-off' }, { text: 'Off', disabled: true });
  const ghost = el('button-control', { 'data-iplug-action': 'size-ghost' }, { text: 'Ghost', hidden: true });
  const dupA = el('button', { 'data-iplug-action': 'dup' }, { text: 'A' });
  const dupB = el('button', { 'data-iplug-action': 'dup' }, { text: 'B' });
  const junk = el('div', { 'param-id': 'x' });
  return { root: root([knob, small, off, ghost, dupA, dupB, junk]), isReady: () => ready, nodes: { knob, small, off, shadowButton } };
}

test('discovers parameter bindings (including open shadow roots) and DOM actions with disabled/hidden state', async () => {
  const env = loadBridgeScript();
  const wv = makeWebView();
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'webview', webview: wv, getController: () => null });
  const controls = await bridge.call('get_webview_controls');
  assert.equal(controls.coordinateSpace, 'viewport CSS pixels');
  assert.deepEqual(controls.parameters.map((p) => [p.paramIdx, p.label, p.tagName]), [[0, 'Gain', 'knob-control']]);
  const actions = Object.fromEntries(controls.actions.map((a) => [a.id, a]));
  assert.equal(actions['size-small'].disabled, false);
  assert.equal(actions['size-off'].disabled, true);
  assert.equal(actions['size-ghost'].disabled, true);
  assert.equal(actions['shadow-action'].source, 'dom');
  assert.equal(actions['size-small'].bounds.left, 1);
});

test('activates DOM actions by click and rejects unknown, duplicate, disabled and hidden targets', async () => {
  const env = loadBridgeScript();
  const wv = makeWebView();
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'webview', webview: wv, getController: () => null });
  const result = await bridge.call('activate_webview_action', { id: 'size-small' });
  assert.deepEqual(result, { id: 'size-small', source: 'dom', result: null });
  assert.equal(wv.nodes.small.clicks, 1);
  await bridge.call('activate_webview_action', { id: 'shadow-action' });
  assert.equal(wv.nodes.shadowButton.clicks, 1);
  await assert.rejects(bridge.call('activate_webview_action', { id: 'nothing' }), (e) => e.code === 'not-found');
  await assert.rejects(bridge.call('activate_webview_action', { id: 'dup' }), (e) => e.code === 'duplicate');
  await assert.rejects(bridge.call('activate_webview_action', { id: 'size-off' }), (e) => e.code === 'disabled');
  await assert.rejects(bridge.call('activate_webview_action', { id: 'size-ghost' }), (e) => e.code === 'disabled');
  await assert.rejects(bridge.call('activate_webview_action', { id: '' }), /at least 1/);
});

test('registered actions take precedence, validate input, receive the signal, and reset on reload', async () => {
  const env = loadBridgeScript();
  const wv = makeWebView();
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'webview', webview: wv, getController: () => null });
  const api = bridge.webviewAPI;
  const seen = [];
  api.registerAction({ id: 'size-small', label: 'Registered small', inputSchema: { type: 'object', properties: { amount: { type: 'number' } }, required: ['amount'] }, run: async (input, { signal }) => { seen.push([input, signal]); return { ok: true }; } });
  assert.throws(() => api.registerAction({ id: 'size-small', run: () => {} }), /already registered/);
  assert.throws(() => api.registerAction({ id: 'x' }), /run\(\)/);
  const controls = await bridge.call('get_webview_controls');
  const registered = controls.actions.filter((a) => a.source === 'registered');
  assert.deepEqual(registered.map((a) => a.label), ['Registered small']);
  assert.equal(controls.actions.filter((a) => a.id === 'size-small').length, 1);
  await assert.rejects(bridge.call('activate_webview_action', { id: 'size-small' }), /missing required property 'amount'/);
  const abort = new AbortController();
  const result = await bridge.call('activate_webview_action', { id: 'size-small', input: { amount: 3 } }, { signal: abort.signal });
  assert.deepEqual(result, { id: 'size-small', source: 'registered', result: { ok: true } });
  assert.equal(seen[0][1], abort.signal);
  assert.equal(wv.nodes.small.clicks, 0);
  api.reset();
  await bridge.call('activate_webview_action', { id: 'size-small' });
  assert.equal(wv.nodes.small.clicks, 1);
});

test('webview tools are gated until the document has loaded', async () => {
  const env = loadBridgeScript();
  const wv = makeWebView({ ready: false });
  const bridge = new env.IPlugWasmWebMCP({ pluginName: 'Test', backend: 'webview', webview: wv, getController: () => null });
  await assert.rejects(bridge.call('get_webview_controls'), (e) => e.code === 'ui-not-ready');
  const status = await bridge.call('get_status');
  assert.equal(status.capabilities.webviewActions, false);
  assert.equal(status.tools.some((t) => t.name.endsWith('_get_ui_tree')), false);
});
