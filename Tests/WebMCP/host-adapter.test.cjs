const { test } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { readFileSync } = require('node:fs');
const path = require('node:path');

/** Instantiate the footer class without running its constructor (no DOM), stubbing the fields the helpers touch */
function fixture() {
  const scope = vm.createContext({ window: {}, document: {}, navigator: {}, console, localStorage: undefined });
  vm.runInContext(readFileSync(path.join(__dirname, '../../IPlug/WEB/TemplateWasm/scripts/IPlugWasmHostControls.js'), 'utf8'), scope);
  const host = Object.create(scope.window.IPlugWasmHostControls.prototype);
  host.options = { pluginName: 'Test', isLiveEditAvailable: () => true };
  const attrs = () => ({ value: '', attributes: {}, classes: new Set(), setAttribute(k, v) { this.attributes[k] = v; }, classList: { toggle(c, on) { on ? this.classes?.add(c) : this.classes?.delete(c); } } });
  for (const key of ['startBtn', 'sourceSelect', 'gainSlider', 'waveformSelect', 'freqL', 'freqR', 'noiseTypeSelect', 'linkCheck', 'liveEditBtn']) host[key] = attrs();
  host.liveEditBtn.classList = { classes: new Set(), toggle(c, on) { on ? this.classes.add(c) : this.classes.delete(c); } };
  Object.assign(host, { audioStarted: false, freqLinked: true, audioBuffer: null, liveEditActive: false, audioContext: { state: 'running', sampleRate: 48000 } });
  host.sourceSelect.value = 'none'; host.gainSlider.value = '50'; host.freqL.value = '220'; host.freqR.value = '277'; host.noiseTypeSelect.value = 'white';
  host.startBtn.disabled = false;
  const calls = [];
  for (const key of ['stopCurrentSource', 'updateGain', 'updateSourceControls', 'savePreferences']) host[key] = () => calls.push(key);
  host.startCurrentSource = async () => calls.push('startCurrentSource');
  host.applyFrequency = (side, value) => { host[side === 'left' ? 'freqL' : 'freqR'].value = value; calls.push(`freq:${side}`); };
  return { host, calls };
}

test('getHostState snapshots the footer', () => {
  const { host } = fixture();
  const state = host.getHostState();
  assert.equal(state.plugin, 'Test');
  assert.equal(state.audioEnabled, false);
  assert.equal(state.audioContextState, 'running');
  assert.equal(state.sampleRate, 48000);
  assert.equal(state.gainPercent, 50);
  assert.equal(state.frequencyLeft, 220);
  assert.equal(state.frequenciesLinked, true);
  assert.equal(state.liveEditAvailable, true);
});

test('applySourceSettings updates the footer, unlinks explicit frequencies, and restarts the source only when audio runs', async () => {
  const { host, calls } = fixture();
  let state = await host.applySourceSettings({ source: 'tone', gainPercent: 12, frequencyLeft: 300 });
  assert.equal(state.source, 'tone');
  assert.equal(state.gainPercent, 12);
  assert.equal(state.frequencyLeft, 300);
  assert.equal(state.frequenciesLinked, false);
  assert.equal(host.linkCheck.checked, false);
  assert.deepEqual(calls, ['stopCurrentSource', 'freq:left', 'updateGain', 'updateSourceControls', 'savePreferences']);
  calls.length = 0;
  host.audioStarted = true;
  state = await host.applySourceSettings({ waveform: 'square', noiseType: 'pink' });
  assert.equal(state.waveform, 'square');
  assert.equal(state.noiseType, 'pink');
  assert.ok(calls.includes('startCurrentSource'));
});

test('setLiveEditActive mirrors the state on the button', () => {
  const { host } = fixture();
  host.setLiveEditActive(true);
  assert.equal(host.liveEditActive, true);
  assert.equal(host.liveEditBtn.attributes['aria-pressed'], 'true');
  assert.ok(host.liveEditBtn.classList.classes.has('active'));
  host.setLiveEditActive(false);
  assert.equal(host.liveEditBtn.attributes['aria-pressed'], 'false');
});
