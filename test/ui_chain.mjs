/*
 * Belt — chain UI harness.
 *
 * Measures, not just checks. host_module_get_param in a chain editor is
 * shimmed onto shadow_get_param: a BLOCKING round-trip to the shim, serviced
 * once per SPI frame (~23 ms) and abandoned after 100 ms. The channel serves
 * roughly 44 reads a second in total. Past that ceiling reads TIME OUT and
 * return null, and any code that folds null into a literal default then writes
 * that default back to the DSP on the next knob turn.
 *
 * That chain shipped three hardware bugs in the sibling module Work while its
 * 445-check engine suite stayed green — none of them were engine bugs. Chain
 * editors get no bulk-read path (shim_handle_param_bulk only serves overtake
 * DSPs), so the only fix here is to read less.
 *
 * Run via `make test`.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/ui_chain.js'), 'utf8');

const MoveKnob1 = 71, MoveShift = 49, MoveMainKnob = 14;
const MoveMainButton = 3, MoveCapture = 52;

const constants = {
    MoveKnob1, MoveCapture, MoveShift, MoveMainButton, MoveMainKnob,
    Black: 0, White: 120, LightGrey: 118, Red: 127, BrightRed: 1, Blue: 125,
    Green: 126, BrightGreen: 8, Cyan: 14, Purple: 22, YellowGreen: 30,
    OrangeRed: 2
};

const params = new Map([
    ['key', '0'], ['scale', '0'], ['retune', '50'], ['amount', '100'],
    ['harm_level', '50'], ['double_amt', '0'], ['formant', '50'],
    ['wet', '100'], ['humanize', '0'], ['flex', '0'], ['spread', '50'],
    ['monitor', '1'], ['hw_input', '0']
]);

let roundTrips = 0;
let readFailures = 0;
const announcements = [];
const writes = [];

const context = vm.createContext({
    console, Math, Number, JSON, String, Array, parseInt, parseFloat, isFinite, Date,
    clear_screen() {}, print() {}, fill_rect() {}, draw_rect() {},
    text_width(t) { return String(t).length * 6; },
    move_midi_internal_send() {},
    host_speaker_active() { return false; },
    host_line_in_connected() { return true; },
    host_swap_module() {},
    host_module_get_param(key) {
        roundTrips++;
        if (readFailures > 0) { readFailures--; return null; }
        return params.get(key) ?? '0';
    },
    host_module_set_param(key, value) {
        writes.push({ key, value: String(value) });
        params.set(key, String(value));
    }
});

function synthetic(exports) {
    return new vm.SyntheticModule(Object.keys(exports), function initialize() {
        for (const [name, value] of Object.entries(exports)) this.setExport(name, value);
    }, { context });
}

const modules = new Map([
    ['/data/UserData/schwung/shared/constants.mjs', synthetic(constants)],
    ['/data/UserData/schwung/shared/input_filter.mjs', synthetic({
        /* Transcribed from schwung's src/shared/input_filter.mjs — decodeDelta
         * returns the ACCUMULATED tick count, which is exactly why the knob
         * maths needs a cap. Do not simplify. */
        decodeDelta(v) {
            if (v === 0) return 0;
            if (v >= 1 && v <= 63) return v;
            if (v >= 65 && v <= 127) return -(128 - v);
            return 0;
        },
        setLED() {}, setButtonLED() {}
    })],
    ['/data/UserData/schwung/shared/menu_layout.mjs', synthetic({
        drawMenuHeader() {}, drawMenuFooter() {}
    })],
    ['/data/UserData/schwung/shared/screen_reader.mjs', synthetic({
        announce(t) { announcements.push(String(t)); },
        announceParameter(l, v) { announcements.push(`${l} ${v}`); },
        announceView(t) { announcements.push(String(t)); }
    })]
]);

const module_ = new vm.SourceTextModule(source, { context, identifier: 'ui_chain.js' });
await module_.link((specifier) => {
    const found = modules.get(specifier);
    assert(found, `unexpected import: ${specifier}`);
    return found;
});
await module_.evaluate();

/* The chain UI publishes its entry points on a namespace rather than globals. */
const ui = context.chain_ui ?? context;
const cc = (num, value) => ui.onMidiMessageInternal([0xb0, num, value]);
const settle = (n = 20) => { for (let i = 0; i < n; i++) ui.tick(); };

/* ------------------------------------------------------------------ tests */

ui.init();
settle(40);

/* The headline measurement, taken in the LOOPING state because that is when
 * the playhead chase runs. */
roundTrips = 0;
settle(44);                                    /* one second of ticks */
assert(roundTrips <= 35,
    `steady state costs ${roundTrips} blocking round-trips per second against a ` +
    'channel that serves about 44 — at that rate the editor starves itself and ' +
    'reads start timing out');

/* Turning a knob must not stall behind a refresh. */
roundTrips = 0;
cc(MoveKnob1, 1);
assert(roundTrips === 0,
    `a knob detent cost ${roundTrips} blocking round-trips on the input path`);

/* decodeDelta is accumulated: one brisk turn is a single event carrying 20 or
 * more. Key is a twelve-entry enum, so a raw delta pins it to an end stop and
 * every key in between is unreachable by a normal turn. */
params.set('key', '3');
ui.init();
settle(30);
writes.length = 0;
cc(MoveKnob1, 1);
let w = writes.find(x => x.key === 'key');
assert.equal(w?.value, '4', `one detent moved key to ${w?.value}, expected 4`);

params.set('key', '3');
ui.init();
settle(30);
writes.length = 0;
cc(MoveKnob1, 30);
w = writes.find(x => x.key === 'key');
assert(w && Number(w.value) > 3 && Number(w.value) < 11,
    `a fast spin drove key to ${w?.value}; it must move a chunk, not straight ` +
    'to the end stop');

/* A dead param channel must not rewrite the engine with defaults, and the next
 * knob turn must continue from the real value rather than a zeroed mirror. */
params.set('retune', '75');
ui.init();
settle(30);
readFailures = 600;
settle(60);
readFailures = 0;
assert.equal(params.get('retune'), '75',
    'a timed-out read must leave the DSP alone, not write a default back');

console.log('belt chain UI: param-channel and knob-response tests passed');
