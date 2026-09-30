// w2-084 D2/D3: tests of the WebAssembly loading page's automatic reload (App/wasm/TaidaFlowApp.shell.html)
// with node, without a browser (Mango: no UI test). Uses only node's own modules.
//
//   node App/wasm/tests/shell-autoreload.test.js [--page <build>/TaidaFlowApp.html] [--template <copy of the template>]
//
//  1. JS syntax of every inline <script> of the template (placeholders filled like apply_wasm_shell.cmake)
//     and, with --page, of the configured page of a wasm build (which must carry the same auto-reload
//     script as the template).
//  2. The back-off functions of the page are the rules of TaidaFlowContent/components/LinkWatchdog.js
//     (w1-083, read here, not copied): same constants / keys, same results over a grid of inputs, and
//     the expected values 0 / 60 / 120 / 240 / 300 / 300 s.
//  3. TaidaFlowAutoReload.schedule() with a fake clock, fake timers and a fake sessionStorage: 10 s
//     countdown, reload exactly once at the deadline, storage written like LinkWatchdog (last, streak+1),
//     back-off after earlier reloads, storage blocked, cancel, throttled timers.
//  4. isFatalRuntimeError: wasm traps / Emscripten aborts yes, other script errors no.
//  5. The page's real init() script (the second inline <script>) run against a small fake DOM, a fake
//     qtLoad and the fake clock: crash after load (onExit), failed download, no WebAssembly, uncaught
//     RuntimeError, unhandled rejection, harmless errors, back-off, blocked storage, crash during the
//     w1-066 transition, normal run without any reload.
// Output: one PASS / FAIL line per test and "Totals: <n> passed, <m> failed"; exit code 0 = all passed.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const TF = path.resolve(__dirname, '..', '..', '..');
const args = process.argv.slice(2);
const argValue = (name) => (args.indexOf(name) >= 0 ? args[args.indexOf(name) + 1] : null);
// --template <file>: another copy of the template (negative controls); default the real one.
const TEMPLATE = argValue('--template') ? path.resolve(argValue('--template'))
                                         : path.join(TF, 'App', 'wasm', 'TaidaFlowApp.shell.html');
const WATCHDOG_JS = path.join(TF, 'TaidaFlowContent', 'components', 'LinkWatchdog.js');
const EPOCH0 = 1790780000000;                    // fake wall clock at monotonic 0

const pageArg = argValue('--page');

// ---- helpers ----------------------------------------------------------------------------------
function inlineScripts(html) {
    const result = [];
    const re = /<script([^>]*)>([\s\S]*?)<\/script>/g;
    let m;
    while ((m = re.exec(html)) !== null) {
        if (/\bsrc\s*=/.test(m[1]))
            continue;
        result.push(m[2]);
    }
    return result;
}
function configure(template) {
    const page = template.replace(/@APPEXPORTNAME@/g, 'TaidaFlowApp_entry')
                         .replace(/@APPNAME@/g, 'TaidaFlowApp')
                         .replace(/@PRELOAD@/g, '');
    const left = page.match(/@[A-Za-z_][A-Za-z0-9_]*@/);
    if (left)
        throw new Error('placeholder left: ' + left[0]);
    return page;
}
function autoReloadBlock(html) {
    const m = html.match(/\/\/ ==== taidaflow-auto-reload[\s\S]*?\/\/ ==== end of taidaflow-auto-reload/);
    return m ? m[0] : null;
}
const flush = async (n = 5) => { for (let i = 0; i < n; ++i) await new Promise((r) => setImmediate(r)); };

class FakeClock {
    constructor() { this.now = 1000; this.nextId = 1; this.timers = []; }
    epoch() { return EPOCH0 + this.now; }
    setTimeout(fn, ms) {
        const id = this.nextId++;
        this.timers.push({ id, due: this.now + Math.max(0, Number(ms) || 0), fn });
        return id;
    }
    clearTimeout(id) { this.timers = this.timers.filter((t) => t.id !== id); }
    advance(ms) {
        const end = this.now + ms;
        for (;;) {
            let next = null;
            for (const t of this.timers)
                if (t.due <= end && (!next || t.due < next.due || (t.due === next.due && t.id < next.id)))
                    next = t;
            if (!next)
                break;
            this.timers = this.timers.filter((t) => t !== next);
            this.now = next.due;
            next.fn();
        }
        this.now = end;
    }
}
class FakeStorage {
    constructor(init) { this.map = new Map(Object.entries(init || {})); this.throwOnGet = false; this.throwOnSet = false; }
    getItem(k) { if (this.throwOnGet) throw new Error('SecurityError'); return this.map.has(k) ? this.map.get(k) : null; }
    setItem(k, v) { if (this.throwOnSet) throw new Error('QuotaExceededError'); this.map.set(k, String(v)); }
}

// ---- sources ----------------------------------------------------------------------------------
const templateHtml = fs.readFileSync(TEMPLATE, 'utf8');
const configuredHtml = configure(templateHtml);
const scripts = inlineScripts(configuredHtml);
const AUTO_RELOAD_SCRIPT = scripts.find((s) => s.includes('var TaidaFlowAutoReload'));
const INIT_SCRIPT = scripts.find((s) => s.includes('async function init()'));

function loadAutoReload(extra) {
    const ctx = vm.createContext(Object.assign({}, extra || {}));
    vm.runInContext(AUTO_RELOAD_SCRIPT, ctx, { filename: 'TaidaFlowApp.shell.html#taidaflow-auto-reload' });
    return { ctx, AR: ctx.TaidaFlowAutoReload };
}
function loadWatchdog() {
    const code = fs.readFileSync(WATCHDOG_JS, 'utf8').replace(/^\s*\.pragma\s+library\s*$/m, '');
    const ctx = vm.createContext({});
    vm.runInContext(code, ctx, { filename: 'LinkWatchdog.js' });
    return ctx;
}
function makeEnv(clock, storage, reloads, countdown) {
    return {
        storage,
        epochNow: () => clock.epoch(),
        monotonicNow: () => clock.now,
        setTimeout: (fn, ms) => clock.setTimeout(fn, ms),
        clearTimeout: (id) => clock.clearTimeout(id),
        reload: () => reloads.push(clock.now),
        onCountdown: (s) => countdown.push(s)
    };
}

// ---- fake page (DOM subset used by init()) ----------------------------------------------------
function makeClassList() {
    const set = new Set();
    return { add: (c) => set.add(c), remove: (c) => set.delete(c), contains: (c) => set.has(c), _set: set };
}
function makeElement(id, hidden) {
    const listeners = {};
    return {
        id, hidden: !!hidden, textContent: '', classList: makeClassList(),
        style: { removeProperty(name) { delete this[name.replace(/-([a-z])/g, (_, c) => c.toUpperCase())]; } },
        addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
        removeEventListener(type, fn) { listeners[type] = (listeners[type] || []).filter((f) => f !== fn); },
        _listeners: listeners
    };
}
// options: storage (FakeStorage | 'blocked'), noWebAssembly, qtLoad(config, page) -> promise
async function startPage(options) {
    const clock = new FakeClock();
    const reloads = [];
    const warnings = [];
    const errors = [];
    const els = {
        '#tf-loader': makeElement('tf-loader'),
        '#screen': makeElement('screen'),
        '#tf-status': makeElement('tf-status'),
        '.tf-scene': makeElement('tf-scene'),
        '#tf-reload': makeElement('tf-reload', true)
    };
    els['#tf-status'].textContent = '系統載入中,首次開啟約需數秒…';
    const windowListeners = {};
    const storage = options.storage || new FakeStorage();
    const page = { clock, reloads, warnings, errors, els, windowListeners, storage, config: null, qtLoadCalls: 0 };
    const ctx = {
        document: { querySelector: (sel) => els[sel] || null },
        addEventListener: (type, fn) => { (windowListeners[type] = windowListeners[type] || []).push(fn); },
        setTimeout: (fn, ms) => clock.setTimeout(fn, ms),
        clearTimeout: (id) => clock.clearTimeout(id),
        requestAnimationFrame: (fn) => clock.setTimeout(() => fn(clock.now), 16),
        cancelAnimationFrame: (id) => clock.clearTimeout(id),
        performance: { now: () => clock.now },
        location: { reload: () => reloads.push(clock.now) },
        console: { log() {}, info() {}, warn: (...a) => warnings.push(a.join(' ')), error: (...a) => errors.push(a.join(' ')) },
        TaidaFlowApp_entry: function () {},
        qtLoad: (config) => { page.qtLoadCalls++; page.config = config; return options.qtLoad(config, page); }
    };
    ctx.window = ctx;
    const context = vm.createContext(ctx);
    // Date.now = fake wall clock (the page uses nothing else of Date).
    vm.runInContext('Date = (function (RealDate) { function D(...a) { return new RealDate(...a); } D.now = () => __epochNow(); return D; })(Date);', context);
    ctx.__epochNow = () => clock.epoch();
    if (storage === 'blocked')
        Object.defineProperty(ctx, 'sessionStorage', { get() { throw new Error('SecurityError: storage is blocked'); } });
    else
        ctx.sessionStorage = storage;
    if (options.noWebAssembly)
        vm.runInContext('delete globalThis.WebAssembly;', context);
    vm.runInContext(AUTO_RELOAD_SCRIPT, context, { filename: 'shell#auto-reload' });
    vm.runInContext(INIT_SCRIPT, context, { filename: 'shell#init' });
    page.context = context;
    page.fire = (type, event) => (windowListeners[type] || []).forEach((fn) => fn(event));
    page.runtimeError = (text) => vm.runInContext(`new WebAssembly.RuntimeError(${JSON.stringify(text)})`, context);
    page.fire('load', {});
    await flush();
    return page;
}
// qtLoad like Qt 6.8's: preRun, onLoaded; stays pending while the app runs.
const qtLoadRunning = (config) => {
    config.preRun.forEach((f) => f());
    config.qt.onLoaded();
    return new Promise(() => {});
};
const status = (page) => page.els['#tf-status'];
const reloadLine = (page) => page.els['#tf-reload'];
const loader = (page) => page.els['#tf-loader'];
function expectErrorShown(page, text) {
    assert.strictEqual(status(page).textContent, text);
    assert.ok(status(page).classList.contains('tf-error'), 'status is red (tf-error)');
    assert.ok(loader(page).classList.contains('tf-stopped'), 'animation stopped');
    assert.strictEqual(loader(page).hidden, false, 'loader shown again');
}
function expectReloadAfter(page, ms) {
    assert.strictEqual(reloadLine(page).hidden, false, 'countdown line shown');
    assert.strictEqual(reloadLine(page).textContent, `${Math.ceil(ms / 1000)} 秒後自動重新整理頁面…`);
    const before = page.reloads.length;
    page.clock.advance(ms - 1);
    assert.strictEqual(page.reloads.length, before, 'no reload before the deadline');
    assert.strictEqual(reloadLine(page).textContent, '1 秒後自動重新整理頁面…');
    page.clock.advance(1);
    assert.strictEqual(page.reloads.length, before + 1, 'reloaded at the deadline');
    assert.strictEqual(reloadLine(page).textContent, '正在重新整理頁面…');
    page.clock.advance(600000);
    assert.strictEqual(page.reloads.length, before + 1, 'only one reload');
}

// ---- tests ------------------------------------------------------------------------------------
const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('syntax: every inline script of the template', () => {
    assert.strictEqual(scripts.length, 2, 'two inline scripts (auto-reload, init)');
    assert.ok(AUTO_RELOAD_SCRIPT && INIT_SCRIPT);
    scripts.forEach((code, i) => new vm.Script(code, { filename: `template inline script ${i + 1}` }));
    assert.ok(templateHtml.includes('taidaflow-wasm-shell'), 'marker kept for apply_wasm_shell.cmake');
    assert.ok(!/@[A-Za-z_][A-Za-z0-9_]*@/.test(templateHtml.replace(/@(APPNAME|APPEXPORTNAME|PRELOAD)@/g, '')),
              'no other @WORD@ (configure_file @ONLY)');
});

test('syntax: configured page of the wasm build (--page)', () => {
    if (!pageArg) {
        console.log('  (skipped: no --page given)');
        return;
    }
    // configure_file may write CRLF line ends on Windows: compare the text with LF line ends.
    const lf = (text) => text.split('\r\n').join('\n');
    const built = lf(fs.readFileSync(path.resolve(pageArg), 'utf8'));
    assert.ok(built.includes('taidaflow-wasm-shell'), 'built page is the TaidaFlow page');
    assert.ok(!/@[A-Za-z_][A-Za-z0-9_]*@/.test(built), 'no placeholder left');
    const builtScripts = inlineScripts(built);
    assert.strictEqual(builtScripts.length, 2);
    builtScripts.forEach((code, i) => new vm.Script(code, { filename: `built inline script ${i + 1}` }));
    assert.strictEqual(autoReloadBlock(built), autoReloadBlock(lf(templateHtml)), 'same auto-reload script as the template');
    const m = built.match(/entryFunction: window\.([A-Za-z0-9_$]+),/);
    assert.ok(m, 'entryFunction configured');
    assert.ok(built.includes(`<script src="${'TaidaFlowApp'}.js"></script>`), 'loads TaidaFlowApp.js');
    console.log(`  built page ${pageArg}: entryFunction window.${m[1]}, auto-reload block identical`);
});

test('back-off: same constants and keys as LinkWatchdog.js', () => {
    const W = loadWatchdog();
    const { AR } = loadAutoReload();
    assert.strictEqual(AR.RELOAD_BACKOFF_BASE_MS, W.RELOAD_BACKOFF_BASE_MS);
    assert.strictEqual(AR.RELOAD_BACKOFF_MAX_MS, W.RELOAD_BACKOFF_MAX_MS);
    assert.strictEqual(AR.STORAGE_LAST_RELOAD_KEY, W.STORAGE_LAST_RELOAD_KEY);
    assert.strictEqual(AR.STORAGE_STREAK_KEY, W.STORAGE_STREAK_KEY);
    assert.strictEqual(AR.STORAGE_LAST_RELOAD_KEY, 'taidaflow.autoReload.lastEpochMs');
    assert.strictEqual(AR.STORAGE_STREAK_KEY, 'taidaflow.autoReload.streak');
    assert.strictEqual(AR.CRASH_RELOAD_AFTER_MS, 10000);
});

test('back-off: same results as LinkWatchdog.js over a grid of inputs', () => {
    const W = loadWatchdog();
    const { AR } = loadAutoReload();
    const streaks = [-1, 0, 0.5, 1, 1.5, 2, 3, 4, 5, 6, 7, 10, 19, 20, 21, 25, 100, NaN, undefined, null, '3', Infinity];
    let compared = 0;
    for (const s of streaks) {
        assert.ok(Object.is(AR.reloadGapMs(s), W.reloadGapMs(s)), `reloadGapMs(${s})`);
        ++compared;
    }
    const now = EPOCH0;
    const lasts = [0, -5, now, now - 1, now - 1000, now - 59999, now - 60000, now - 60001, now - 119999, now - 240000,
                   now - 300000, now - 3600000, now + 5000, now + 400000, NaN, undefined, null];
    for (const last of lasts)
        for (const s of streaks) {
            assert.ok(Object.is(AR.reloadWaitMs(now, last, s), W.reloadWaitMs(now, last, s)), `reloadWaitMs(now, ${last - now}, ${s})`);
            ++compared;
        }
    for (const text of [undefined, null, '', '0', '1', '12.5', '-3', 'abc', 'Infinity', 'NaN', ' 7 ', '1e3', String(now)]) {
        assert.ok(Object.is(AR.parseStoredNumber(text), W.parseStoredNumber(text)), `parseStoredNumber(${text})`);
        ++compared;
    }
    console.log(`  ${compared} results compared with LinkWatchdog.js`);
});

test('back-off: expected gaps 0 / 60 / 120 / 240 / 300 / 300 s and waits', () => {
    const { AR } = loadAutoReload();
    assert.deepStrictEqual([0, 1, 2, 3, 4, 5, 9].map((s) => AR.reloadGapMs(s)), [0, 60000, 120000, 240000, 300000, 300000, 300000]);
    assert.strictEqual(AR.reloadWaitMs(EPOCH0, EPOCH0 - 20000, 1), 40000);
    assert.strictEqual(AR.reloadWaitMs(EPOCH0, EPOCH0 + 999999, 1), 60000, 'clock set back: at most one gap');
    assert.strictEqual(AR.reloadWaitMs(EPOCH0, 0, 3), 0, 'no previous reload');
    const st = (last, streak) => ({ available: true, lastEpochMs: last, streak });
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(0, 0)), 10000, 'first: the 10 s countdown');
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0 - 5000, 1)), 55000);
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0 - 55000, 1)), 10000, 'never less than 10 s');
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0 - 20000, 2)), 100000);
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0, 3)), 240000);
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0, 4)), 300000);
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0, 12)), 300000, 'at most 5 min');
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, st(EPOCH0 - 600000, 12)), 10000, 'back-off over');
    assert.strictEqual(AR.reloadDelayMs(EPOCH0, { available: false, lastEpochMs: 0, streak: 0 }), 60000, 'no storage');
});

test('schedule: first crash -> 10 s countdown, one reload, storage like LinkWatchdog', () => {
    const { AR } = loadAutoReload();
    const clock = new FakeClock();
    const storage = new FakeStorage();
    const reloads = [];
    const countdown = [];
    const h = AR.schedule(makeEnv(clock, storage, reloads, countdown));
    assert.strictEqual(h.delayMs, 10000);
    assert.strictEqual(h.streak, 0);
    assert.strictEqual(h.storageAvailable, true);
    const t0 = clock.now;
    clock.advance(9999);
    assert.strictEqual(reloads.length, 0);
    clock.advance(1);
    assert.deepStrictEqual(reloads, [t0 + 10000]);
    assert.deepStrictEqual(countdown, [10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0]);
    assert.strictEqual(storage.getItem('taidaflow.autoReload.lastEpochMs'), String(EPOCH0 + t0 + 10000));
    assert.strictEqual(storage.getItem('taidaflow.autoReload.streak'), '1');
    clock.advance(3600000);
    assert.strictEqual(reloads.length, 1);
});

test('schedule: consecutive crashes follow the back-off (60 / 120 / 240 / 300 s)', () => {
    const { AR } = loadAutoReload();
    const storage = new FakeStorage();
    const clock = new FakeClock();
    // Simulated sequence of page loads that crash 5 s after each reload.
    const expected = [10000, 55000, 115000, 235000, 295000, 295000];
    const delays = [];
    for (let i = 0; i < expected.length; ++i) {
        clock.advance(5000);                 // the reloaded page runs 5 s, then crashes again
        const reloads = [];
        const h = AR.schedule(makeEnv(clock, storage, reloads, []));
        delays.push(h.delayMs);
        clock.advance(h.delayMs);
        assert.strictEqual(reloads.length, 1, `reload ${i + 1}`);
        assert.strictEqual(storage.getItem('taidaflow.autoReload.streak'), String(i + 1));
    }
    assert.deepStrictEqual(delays, expected);
    // LinkWatchdog sets the streak back to 0 after 60 s of healthy link -> 10 s again.
    storage.setItem('taidaflow.autoReload.streak', '0');
    assert.strictEqual(AR.schedule(makeEnv(clock, storage, [], [])).delayMs, 10000);
});

test('schedule: invalid stored values, storage blocked, setItem failing', () => {
    const { AR } = loadAutoReload();
    for (const [last, streak] of [['abc', '-3'], ['', ''], ['NaN', 'x']]) {
        const storage = new FakeStorage({ 'taidaflow.autoReload.lastEpochMs': last, 'taidaflow.autoReload.streak': streak });
        assert.strictEqual(AR.schedule(makeEnv(new FakeClock(), storage, [], [])).delayMs, 10000);
    }
    const blocked = new FakeStorage();
    blocked.throwOnGet = true;
    blocked.throwOnSet = true;
    const clock = new FakeClock();
    const reloads = [];
    const h = AR.schedule(makeEnv(clock, blocked, reloads, []));
    assert.strictEqual(h.storageAvailable, false);
    assert.strictEqual(h.delayMs, 60000);
    clock.advance(60000);
    assert.strictEqual(reloads.length, 1, 'reload even though storage throws');
    const nullStorage = AR.schedule(makeEnv(new FakeClock(), null, [], []));
    assert.strictEqual(nullStorage.delayMs, 60000);
    const setFails = new FakeStorage();
    setFails.throwOnSet = true;
    const c2 = new FakeClock();
    const r2 = [];
    AR.schedule(makeEnv(c2, setFails, r2, []));
    c2.advance(10000);
    assert.strictEqual(r2.length, 1);
});

test('schedule: cancel, and timers that fire late (throttled tab)', () => {
    const { AR } = loadAutoReload();
    const clock = new FakeClock();
    const reloads = [];
    const h = AR.schedule(makeEnv(clock, new FakeStorage(), reloads, []));
    clock.advance(4000);
    h.cancel();
    clock.advance(600000);
    assert.strictEqual(reloads.length, 0);
    assert.strictEqual(clock.timers.length, 0);
    // Throttled: the next timer only runs 30 s later -> reload at that first late tick, once.
    const c2 = new FakeClock();
    const r2 = [];
    const cd = [];
    const env = makeEnv(c2, new FakeStorage(), r2, cd);
    env.setTimeout = (fn, ms) => c2.setTimeout(fn, 30000);
    AR.schedule(env);
    c2.advance(30000);
    assert.strictEqual(r2.length, 1);
    assert.deepStrictEqual(cd, [10, 0]);
});

test('isFatalRuntimeError: wasm trap / abort yes, other errors no', () => {
    const { ctx, AR } = loadAutoReload();
    const trap = vm.runInContext('new WebAssembly.RuntimeError("unreachable")', ctx);
    assert.strictEqual(AR.isFatalRuntimeError(trap, 'Uncaught RuntimeError: unreachable'), true);
    assert.strictEqual(AR.isFatalRuntimeError(new WebAssembly.RuntimeError('memory access out of bounds'), ''), true);
    assert.strictEqual(AR.isFatalRuntimeError({ name: 'RuntimeError', message: 'x' }, ''), true);
    assert.strictEqual(AR.isFatalRuntimeError(null, 'Uncaught RuntimeError: memory access out of bounds'), true);
    assert.strictEqual(AR.isFatalRuntimeError(new Error('Aborted(native code called abort())'), ''), true);
    assert.strictEqual(AR.isFatalRuntimeError('Aborted(OOM)', ''), true);
    assert.strictEqual(AR.isFatalRuntimeError(new TypeError('x is undefined'), 'Uncaught TypeError: x is undefined'), false);
    assert.strictEqual(AR.isFatalRuntimeError(null, 'Script error.'), false);
    assert.strictEqual(AR.isFatalRuntimeError(new Error('Failed to fetch'), ''), false);
    assert.strictEqual(AR.isFatalRuntimeError(undefined, ''), false);
    assert.strictEqual(AR.isFatalRuntimeError('network', ''), false);
});

test('page: crash after the app was running (onExit) -> red message kept + reload after 10 s', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    assert.strictEqual(page.qtLoadCalls, 1);
    page.clock.advance(2000);                               // w1-066 transition done (guard 1.5 s)
    assert.strictEqual(loader(page).hidden, true, 'loader hidden after the transition');
    assert.ok(!loader(page).classList.contains('tf-leaving') && !page.els['#screen'].classList.contains('tf-entering'));
    assert.strictEqual(reloadLine(page).hidden, true, 'no countdown while running');
    page.config.qt.onExit({ text: 'Aborted(RuntimeError: unreachable)', crashed: true });
    expectErrorShown(page, '系統已結束,請重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
    assert.strictEqual(page.storage.getItem('taidaflow.autoReload.streak'), '1');
    // More errors afterwards do not schedule a second reload.
    page.fire('error', { error: page.runtimeError('unreachable'), message: 'Uncaught RuntimeError: unreachable' });
    page.clock.advance(600000);
    assert.strictEqual(page.reloads.length, 1);
});

test('page: onExit with an exit code keeps the existing message', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    page.clock.advance(2000);
    page.config.qt.onExit({ code: 3, crashed: false });
    expectErrorShown(page, '系統已結束(代碼 3),請重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
});

test('page: download failure before onLoaded -> load-failed message + reload after 10 s', async () => {
    const page = await startPage({
        qtLoad: (config) => {
            config.qt.onExit({ text: 'Could not fetch TaidaFlowApp.wasm', crashed: true });   // as qtLoad does
            return Promise.reject(new Error('Could not fetch TaidaFlowApp.wasm'));
        }
    });
    expectErrorShown(page, '無法載入系統,請確認網路連線後重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
});

test('page: qtLoad throws without onExit (TaidaFlowApp.js missing) -> load-failed + reload', async () => {
    const page = await startPage({ qtLoad: () => Promise.reject(new Error('config.qt.entryFunction is required, expected a function')) });
    expectErrorShown(page, '無法載入系統,請確認網路連線後重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
});

test('page: no WebAssembly support -> existing message + reload after 10 s (qtLoad not called)', async () => {
    const page = await startPage({ noWebAssembly: true, qtLoad: qtLoadRunning });
    assert.strictEqual(page.qtLoadCalls, 0);
    expectErrorShown(page, '此瀏覽器不支援本系統,請使用最新版 Chrome 或 Edge。');
    expectReloadAfter(page, 10000);
});

test('page: uncaught RuntimeError while running -> error message + reload after 10 s', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    page.clock.advance(5000);
    page.fire('error', { error: page.runtimeError('memory access out of bounds'),
                         message: 'Uncaught RuntimeError: memory access out of bounds' });
    expectErrorShown(page, '系統發生錯誤,已停止執行,請重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
});

test('page: unhandled rejection with an Emscripten abort -> reload after 10 s', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    page.clock.advance(5000);
    page.fire('unhandledrejection', { reason: page.runtimeError('Aborted(OOM)') });
    expectErrorShown(page, '系統發生錯誤,已停止執行,請重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
});

test('page: harmless script errors do nothing (no message, no reload in 10 min)', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    page.clock.advance(2000);
    page.fire('error', { error: new TypeError('x is undefined'), message: 'Uncaught TypeError: x is undefined' });
    page.fire('error', { error: null, message: 'Script error.' });
    page.fire('unhandledrejection', { reason: new Error('Failed to fetch') });
    page.clock.advance(600000);
    assert.strictEqual(page.reloads.length, 0);
    assert.strictEqual(loader(page).hidden, true);
    assert.strictEqual(reloadLine(page).hidden, true);
    assert.ok(!status(page).classList.contains('tf-error'));
});

test('page: normal run without errors never reloads (10 min)', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    page.clock.advance(600000);
    assert.strictEqual(page.reloads.length, 0);
    assert.strictEqual(loader(page).hidden, true);
    assert.strictEqual(status(page).textContent, '準備完成');
    assert.strictEqual(page.warnings.length, 0);
});

test('page: back-off from earlier reloads (streak 2, last reload 12 s before the crash -> 108 s)', async () => {
    const storage = new FakeStorage();
    const page = await startPage({
        storage,
        qtLoad: (config, p) => {
            storage.setItem('taidaflow.autoReload.lastEpochMs', String(p.clock.epoch() - 10000));
            storage.setItem('taidaflow.autoReload.streak', '2');
            return qtLoadRunning(config);
        }
    });
    page.clock.advance(2000);
    page.config.qt.onExit({ text: 'Aborted()', crashed: true });
    // last reload 12 s before the crash, gap 120 s -> 108 s
    expectReloadAfter(page, 108000);
    assert.strictEqual(storage.getItem('taidaflow.autoReload.streak'), '3');
    assert.ok(page.warnings.some((w) => w.includes('automatic reload in 108000 ms')));
});

test('page: sessionStorage blocked -> still reloads (after 60 s), no exception', async () => {
    const page = await startPage({ storage: 'blocked', qtLoad: qtLoadRunning });
    page.clock.advance(2000);
    page.config.qt.onExit({ text: 'Aborted()', crashed: true });
    expectErrorShown(page, '系統已結束,請重新整理頁面(F5)。');
    expectReloadAfter(page, 60000);
    assert.strictEqual(page.errors.length, 0);
});

test('page: crash during the w1-066 transition -> transition cancelled, loader back, reload', async () => {
    const page = await startPage({ qtLoad: qtLoadRunning });
    page.clock.advance(400);                                // 2 frames + 250 ms: transition running
    assert.ok(loader(page).classList.contains('tf-leaving'), 'transition started');
    page.config.qt.onExit({ text: 'Aborted()', crashed: true });
    assert.ok(!loader(page).classList.contains('tf-leaving') && !page.els['#screen'].classList.contains('tf-entering'));
    expectErrorShown(page, '系統已結束,請重新整理頁面(F5)。');
    expectReloadAfter(page, 10000);
    assert.strictEqual(loader(page).hidden, false, 'transition guard does not hide the error');
});

(async () => {
    let passed = 0;
    let failed = 0;
    console.log(`template: ${TEMPLATE}`);
    console.log(`LinkWatchdog.js: ${WATCHDOG_JS}`);
    console.log(`node ${process.version}`);
    for (const t of tests) {
        try {
            await t.fn();
            ++passed;
            console.log(`PASS   : ${t.name}`);
        } catch (e) {
            ++failed;
            console.log(`FAIL!  : ${t.name}\n    ${(e && e.stack || String(e)).split('\n').slice(0, 4).join('\n    ')}`);
        }
    }
    console.log(`Totals: ${passed} passed, ${failed} failed`);
    process.exit(failed ? 1 : 0);
})();
