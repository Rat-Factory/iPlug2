const { test } = require('node:test');
const assert = require('node:assert/strict');
const { loadBundleAndProcessor, makeControllerPair } = require('./helpers.cjs');

const env = loadBundleAndProcessor();

test('getParameters and setParameter round-trip through the DSP with quantization', async () => {
  const { controller, dsp } = makeControllerPair(env);
  const info = await controller.request('getParameters');
  assert.equal(info.params[0].normalizedValue, 0.5);
  const result = await controller.request('setParameter', { paramIdx: 0, normalized: 0.61 });
  assert.equal(result.changed[0].normalizedValue, 0.5); // quantized to quarters
  assert.equal(dsp.echo.length, 1);
  await controller.request('setParameter', { paramIdx: 0, value: 100 });
  assert.equal(dsp.normalized, 1);
  await controller.request('setParameter', { paramIdx: 0, text: '25' });
  assert.equal(dsp.normalized, 0.5);
});

test('invalid parameter requests are rejected with codes and nothing is applied', async () => {
  const { controller, dsp } = makeControllerPair(env);
  await assert.rejects(controller.request('setParameter', { paramIdx: 9, normalized: 0.5 }), (e) => e.code === 'invalid' && /Invalid parameter index/.test(e.message));
  await assert.rejects(controller.request('setParameter', { paramIdx: 0, normalized: 1.5 }), /between 0 and 1/);
  await assert.rejects(controller.request('setParameter', { paramIdx: 0, normalized: 0.5, value: 3 }), /exactly one/);
  await assert.rejects(controller.request('setParameter', { paramIdx: 0 }), /exactly one/);
  await assert.rejects(controller.request('bogus'), (e) => e.code === 'unknown-op');
  assert.equal(dsp.echo.length, 0);
});

test('batch setParameters is all-or-nothing', async () => {
  const { controller, dsp } = makeControllerPair(env);
  await assert.rejects(controller.request('setParameters', { values: [{ paramIdx: 0, normalized: 1 }, { paramIdx: 4, normalized: 0 }] }), /Invalid parameter index/);
  assert.equal(dsp.echo.length, 0);
  const result = await controller.request('setParameters', { values: [{ paramIdx: 0, normalized: 0.25 }] });
  assert.equal(result.changed.length, 1);
  assert.equal(dsp.normalized, 0.25);
  await assert.rejects(controller.request('setParameters', { values: [] }), /non-empty/);
});

test('resetParameter, presets and state snapshot/restore', async () => {
  const { controller, dsp } = makeControllerPair(env);
  await controller.request('setParameter', { paramIdx: 0, normalized: 1 });
  const state = await controller.request('getState');
  assert.equal(state.format, 'iplug-state-base64');
  await controller.request('resetParameter', {});
  assert.equal(dsp.normalized, 0.5);
  const restored = await controller.request('setState', { base64: state.base64 });
  assert.equal(restored.params[0].normalizedValue, 1);
  await assert.rejects(controller.request('setState', { base64: '' }), /non-empty/);
  const presets = await controller.request('getPresets');
  assert.deepEqual(presets.presets.map((p) => p.name), ['Init', 'Loud']);
  await controller.request('restorePreset', { presetIdx: 0 });
  assert.equal(dsp.normalized, 0.5);
  await assert.rejects(controller.request('restorePreset', { presetIdx: 5 }), /Invalid preset index/);
});

test('missing DSP bindings report not-supported', async () => {
  const { controller } = makeControllerPair(env, { serializeState: undefined, paramToNormalized: undefined });
  await assert.rejects(controller.request('getState'), (e) => e.code === 'not-supported');
  await assert.rejects(controller.request('setParameter', { paramIdx: 0, value: 3 }), (e) => e.code === 'not-supported');
});

test('requests time out, abort, and are rejected on destroy', async () => {
  const { controller } = makeControllerPair(env);
  controller.workletNode.port.postMessage = () => {}; // processor never answers
  await assert.rejects(controller.request('getParameters', {}, { timeoutMs: 10 }), (e) => e.code === 'timeout');

  const abort = new AbortController();
  const pending = controller.request('getParameters', {}, { signal: abort.signal, timeoutMs: 1000 });
  abort.abort();
  await assert.rejects(pending, (e) => e.code === 'aborted');

  const later = controller.request('getParameters', {}, { timeoutMs: 1000 });
  controller.destroy();
  await assert.rejects(later, (e) => e.code === 'destroyed');
  await assert.rejects(controller.request('getParameters'), (e) => e.code === 'not-ready');
  assert.equal(controller._requests.size, 0);
});

test('a suspended AudioContext shortens the timeout and explains itself', async () => {
  const { controller } = makeControllerPair(env);
  controller.audioContext.state = 'suspended';
  controller.workletNode.port.postMessage = () => {};
  const started = Date.now();
  await assert.rejects(controller.request('getParameters'), /not running/);
  assert.ok(Date.now() - started < 4000);
});
