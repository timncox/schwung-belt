/*
 * Belt — Signal Chain UI
 *
 * Loads when Belt's component editor is opened inside a slot's Signal
 * Chain (the Master FX editor uses auto-generated knob pages instead).
 *
 * Controls while this screen is open:
 *   Pads 68-71 (bottom row)  Harmony voices 1-4: tap = toggle on/off,
 *                            Shift+tap = cycle the voice's interval
 *   Pad 76 (above 68)        HARD — hard-tune punch (tap = latch,
 *                            hold = momentary): retune 0 / amount 100
 *   Pad 73                   Monitor (belt-in only): input on/off +
 *                            mic feedback guard
 *   Steps 1-12               chromatic strip C..B: in-scale notes dim,
 *                            detected note lights up (green = in tune,
 *                            orange = off pitch); press = set Key
 *   Steps 13-16              harmony voice indicators
 *   Knobs 1-8                Key, Scale, Retune, Amount, Harm Level,
 *                            Doubler, Formant, Wet
 *   Shift + Knobs            Humanize, Flex, Spread, Monitor
 *
 * Screen reader: pad actions, knob changes and note events are announced
 * via shared/screen_reader.mjs when the reader is enabled.
 */

import {
    MoveKnob1, MoveShift,
    Black, LightGrey, Red, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, YellowGreen, OrangeRed
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';


/* Pads (bottom-left corner of the 8x4 grid) */
const PAD_H1      = 68;
const PAD_H2      = 69;
const PAD_H3      = 70;
const PAD_H4      = 71;
const PAD_HARD    = 76;   /* hard-tune punch: tap = latch, hold = momentary */
const PAD_MONITOR = 73;   /* belt-in only: input monitoring + feedback guard */

/* Step buttons: 1-12 = chromatic strip, 13-16 = harmony indicators */
const STEP_FIRST = 16;

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const NOTE_SPEECH = ['C', 'C sharp', 'D', 'D sharp', 'E', 'F', 'F sharp',
                     'G', 'G sharp', 'A', 'A sharp', 'B'];

const SCALE_DEGREES = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
    [0, 2, 4, 5, 7, 9, 11],
    [0, 2, 3, 5, 7, 8, 10],
    [0, 2, 3, 5, 7, 8, 11],
    [0, 2, 3, 5, 7, 9, 10],
    [0, 2, 4, 5, 7, 9, 10],
    [0, 2, 4, 7, 9],
    [0, 3, 5, 7, 10],
    [0, 3, 5, 6, 7, 10]
];

const ITV_NAMES = ['Off', '-Oct', '-6th', '-5th', '-4th', '-3rd', 'Unis',
                   '+3rd', '+4th', '+5th', '+6th', '+Oct'];
const ITV_SPEECH = ['off', 'octave down', 'sixth down', 'fifth down',
                    'fourth down', 'third down', 'unison', 'third up',
                    'fourth up', 'fifth up', 'sixth up', 'octave up'];

/* Knobs 1-8 (CC 71-78). `name`/`opts` are the terse screen strings;
 * `speech`/`speechOpts` are what the screen reader says. */
const KNOBS = [
    { key: 'key',        name: 'Key',  opts: NOTE_NAMES, speech: 'Key',
      speechOpts: NOTE_SPEECH },
    { key: 'scale',      name: 'Scl',  opts: ['Chro', 'Maj', 'Min', 'HMin', 'Dor', 'Mixo', 'MajP', 'MinP', 'Blue'],
      speech: 'Scale',
      speechOpts: ['chromatic', 'major', 'minor', 'harmonic minor', 'dorian',
                   'mixolydian', 'major pentatonic', 'minor pentatonic', 'blues'] },
    { key: 'retune',     name: 'Ret',  min: 0, max: 100, step: 5,
      speech: 'Retune speed', unit: ' percent' },
    { key: 'amount',     name: 'Amt',  min: 0, max: 100, step: 5,
      speech: 'Correction amount', unit: ' percent' },
    { key: 'harm_level', name: 'Harm', min: 0, max: 100, step: 5,
      speech: 'Harmony level', unit: ' percent' },
    { key: 'double_amt', name: 'Dbl',  min: 0, max: 100, step: 5,
      speech: 'Doubler', unit: ' percent' },
    { key: 'formant',    name: 'Frmt', min: -100, max: 100, step: 5,
      speech: 'Formant', unit: '' },
    { key: 'wet',        name: 'Wet',  min: 0, max: 100, step: 5,
      speech: 'Wet', unit: ' percent' }
];

/* Knob page 2 — held-Shift layer. */
const KNOBS2 = [
    { key: 'humanize', name: 'Hum',  min: 0, max: 100, step: 5,
      speech: 'Humanize' },
    { key: 'flex',     name: 'Flex', min: 0, max: 100, step: 5,
      speech: 'Flex tune' },
    { key: 'spread',   name: 'Sprd', min: 0, max: 100, step: 5,
      speech: 'Harmony spread' },
    { key: 'monitor',  name: 'Mon',  opts: ['Mute', 'On'],
      speech: 'Monitoring', speechOpts: ['muted', 'on'] },
    null, null, null, null
];

let knobValues = [0, 1, 25, 100, 80, 0, 0, 100];
let knob2Values = [30, 0, 70, 1, 0, 0, 0, 0];
let harm = [0, 0, 0, 0];
/* interval each voice returns to when toggled back on */
let lastItv = [7, 9, 11, 1];

let detNote10 = -1;      /* detected midi note * 10, -1 = none */
let cents = 0;
let voiced = 0;
let tickCount = 0;
let needsRedraw = true;
let shiftHeld = false;

/* hard-tune punch */
const HARD_HOLD_MS = 350;
let hardOn = false;
let hardHeldAt = 0;

/* Feedback guard — belt-in only (hw_input=1: the DSP reads the mic/line
 * input directly). Mirrors the smack-in guard: speakers on + no line-in
 * cable -> mute the DSP output until overridden or a cable shows up. */
let hwInput = false;
let monitorOn = true;
let guardMuted = false;
let guardOverride = false;

function gp(key) {
    const v = host_module_get_param(key);
    return (v === null || v === undefined) ? null : String(v);
}

function fetchAll() {
    for (let i = 0; i < KNOBS.length; i++) {
        const v = gp(KNOBS[i].key);
        if (v !== null) knobValues[i] = parseFloat(v) || 0;
    }
    for (let i = 0; i < KNOBS2.length; i++) {
        if (!KNOBS2[i]) continue;
        const v = gp(KNOBS2[i].key);
        if (v !== null) knob2Values[i] = parseFloat(v) || 0;
    }
    for (let i = 0; i < 4; i++) {
        const v = gp(`harm${i + 1}`);
        if (v !== null) {
            harm[i] = parseInt(v) || 0;
            if (harm[i] > 0) lastItv[i] = harm[i];
        }
    }
    hwInput = gp('hw_input') === '1';
    monitorOn = (gp('monitor') || '1') !== '0';
    hardOn = gp('hard') === '1';
}

function pollStatus() {
    const s = gp('status');
    if (!s) return false;
    const parts = s.split(':');
    if (parts.length < 4) return false;
    const n = parseInt(parts[0]);
    const c = parseInt(parts[1]);
    const v = parseInt(parts[2]);
    const changed = (n !== detNote10) || (Math.abs(c - cents) > 4) || (v !== voiced);
    detNote10 = n; cents = c; voiced = v;
    return changed;
}

function feedbackRisk() {
    if (typeof host_speaker_active !== 'function') return false;
    if (typeof host_line_in_connected !== 'function') return false;
    return host_speaker_active() && !host_line_in_connected();
}

function setMonitor(on) {
    monitorOn = on;
    host_module_set_param('monitor', on ? '1' : '0');
    updateActionLEDs();
    needsRedraw = true;
}

function reconcileFeedbackGuard() {
    if (!hwInput) return;
    const risk = feedbackRisk();
    if (risk && monitorOn && !guardOverride) {
        guardMuted = true;
        setMonitor(false);
        announce('Feedback risk. Output muted. Monitor pad to override, or plug in headphones.');
    } else if (!risk && guardMuted) {
        guardMuted = false;
        guardOverride = false;
        setMonitor(true);
        announce('Monitoring restored');
    }
}

function knobDisplay(i, k, vals) {
    if (k.opts) {
        const idx = Math.max(0, Math.min(k.opts.length - 1, Math.round(vals[i])));
        return k.opts[idx];
    }
    return `${Math.round(vals[i])}`;
}

function knobSpeech(i, k, vals) {
    if (k.speechOpts) {
        const idx = Math.max(0, Math.min(k.speechOpts.length - 1, Math.round(vals[i])));
        return k.speechOpts[idx];
    }
    return `${Math.round(vals[i])}${k.unit || ''}`;
}

function adjustAny(i, delta, table, vals) {
    const k = table[i];
    if (!k) return;
    const max = k.opts ? k.opts.length - 1 : k.max;
    const min = k.opts ? 0 : k.min;
    const step = k.opts ? 1 : k.step;
    const v = Math.max(min, Math.min(max, vals[i] + delta * step));
    if (v === vals[i]) return;
    vals[i] = v;
    host_module_set_param(k.key, `${Math.round(v)}`);
    announceParameter(k.speech, knobSpeech(i, k, vals));
    if (k.key === 'monitor') {
        monitorOn = Math.round(v) !== 0;
        guardOverride = monitorOn && feedbackRisk();
        updateActionLEDs();
    }
    if (k.key === 'key' || k.key === 'scale') updateStepLEDs();
    needsRedraw = true;
}

/* ---- LEDs ---- */

function updateActionLEDs() {
    const hcol = [Blue, Purple, Cyan, YellowGreen];
    const pads = [PAD_H1, PAD_H2, PAD_H3, PAD_H4];
    for (let i = 0; i < 4; i++)
        setLED(pads[i], harm[i] > 0 ? hcol[i] : 0x10);
    setLED(PAD_HARD, hardOn ? BrightRed : Red);
    setLED(PAD_MONITOR, hwInput ? (monitorOn ? Green : BrightRed) : Black);
}

function updateStepLEDs() {
    const key = Math.round(knobValues[0]);
    const scale = Math.max(0, Math.min(8, Math.round(knobValues[1])));
    const deg = SCALE_DEGREES[scale];
    const detPc = detNote10 >= 0 && voiced
        ? ((Math.round(detNote10 / 10) % 12) + 12) % 12 : -1;
    for (let pc = 0; pc < 12; pc++) {
        let color = Black;
        const rel = ((pc - key) % 12 + 12) % 12;
        let inScale = false;
        for (let i = 0; i < deg.length; i++)
            if (deg[i] === rel) { inScale = true; break; }
        if (inScale) color = 0x10;               /* dim: allowed note */
        if (pc === key) color = LightGrey;       /* the key root */
        if (pc === detPc)
            color = Math.abs(cents) <= 25 ? BrightGreen : OrangeRed;
        setLED(STEP_FIRST + pc, color);
    }
    /* steps 13-16: harmony voice indicators */
    const hcol = [Blue, Purple, Cyan, YellowGreen];
    for (let i = 0; i < 4; i++) {
        let c = Black;
        if (harm[i] > 0) c = voiced ? hcol[i] : 0x10;
        setLED(STEP_FIRST + 12 + i, c);
    }
}

/* ---- Screen ---- */

function noteLabel() {
    if (detNote10 < 0 || !voiced) return '--';
    const n = Math.round(detNote10 / 10);
    const pc = ((n % 12) + 12) % 12;
    const oct = Math.floor(n / 12) - 1;
    return `${NOTE_NAMES[pc]}${oct}`;
}

function drawUI() {
    clear_screen();
    let title = 'BELT  ' + noteLabel();
    if (voiced) {
        const c = Math.round(cents);
        title += ` ${c >= 0 ? '+' : ''}${c}c`;
    }
    if (hardOn) title += ' HARD';
    drawHeader(title);

    /* two rows of four knob params (Shift = page 2) */
    for (let i = 0; i < 8; i++) {
        const col = i % 4, row = (i / 4) | 0;
        const x = 2 + col * 32;
        const y = 15 + row * 20;
        if (shiftHeld) {
            if (KNOBS2[i]) {
                print(x, y, KNOBS2[i].name, 1);
                print(x, y + 8, knobDisplay(i, KNOBS2[i], knob2Values), 1);
            }
        } else {
            print(x, y, KNOBS[i].name, 1);
            print(x, y + 8, knobDisplay(i, KNOBS[i], knobValues), 1);
        }
    }

    /* harmony summary line */
    {
        let hs = '';
        for (let i = 0; i < 4; i++)
            if (harm[i] > 0) hs += (hs ? ' ' : '') + ITV_NAMES[harm[i]];
        print(2, 55, hs ? `H: ${hs}` : 'H: off', 1);
    }

    /* footer budget is ~20 chars total */
    if (shiftHeld) drawFooter({ left: 'Knobs pg2', right: '' });
    else drawFooter({ left: 'Pads:voices  HARD', right: '' });
    needsRedraw = false;
}

/* ---- Actions ---- */

function toggleVoice(i) {
    if (harm[i] > 0) {
        lastItv[i] = harm[i];
        harm[i] = 0;
        announce(`Voice ${i + 1} off`);
    } else {
        harm[i] = lastItv[i];
        announce(`Voice ${i + 1} ${ITV_SPEECH[harm[i]]}`);
    }
    host_module_set_param(`harm${i + 1}`, `${harm[i]}`);
    updateActionLEDs();
    updateStepLEDs();
    needsRedraw = true;
}

function cycleVoice(i) {
    /* Shift+tap: next interval (1..11, skipping Off) */
    let v = harm[i] > 0 ? harm[i] : lastItv[i];
    v = v >= 11 ? 1 : v + 1;
    harm[i] = v;
    lastItv[i] = v;
    host_module_set_param(`harm${i + 1}`, `${v}`);
    announce(`Voice ${i + 1} ${ITV_SPEECH[v]}`);
    updateActionLEDs();
    updateStepLEDs();
    needsRedraw = true;
}

function setHard(on, speak) {
    if (on === hardOn) return;
    hardOn = on;
    host_module_set_param('hard', on ? '1' : '0');
    if (speak) announce(on ? 'Hard tune' : 'Hard tune off');
    updateActionLEDs();
    needsRedraw = true;
}

/* ---- Lifecycle ---- */

function init() {
    fetchAll();
    pollStatus();
    updateActionLEDs();
    updateStepLEDs();
    needsRedraw = true;
    announceView('Belt vocal processor');
    reconcileFeedbackGuard();
}

function tick() {
    tickCount++;

    /* jack state can change mid-session — re-check the guard ~2x/second */
    if (hwInput && tickCount % 15 === 0) reconcileFeedbackGuard();

    /* note display chase: ONE status poll per tick */
    if (pollStatus()) {
        updateStepLEDs();
        needsRedraw = true;
    }

    /* periodic full refresh — device knob edits and preset restores */
    if (tickCount % 12 === 0) {
        const oldKnobs = knobValues.join(',');
        const oldHarm = harm.join(',');
        const oldMon = monitorOn;
        const oldHard = hardOn;
        fetchAll();
        if (knobValues.join(',') !== oldKnobs) {
            updateStepLEDs();
            needsRedraw = true;
        }
        if (harm.join(',') !== oldHarm) {
            updateActionLEDs();
            updateStepLEDs();
            needsRedraw = true;
        }
        if (monitorOn !== oldMon) { updateActionLEDs(); needsRedraw = true; }
        if (hardOn !== oldHard) { updateActionLEDs(); needsRedraw = true; }
    }

    if (needsRedraw) drawUI();
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        if (d1 === MoveShift) {
            const was = shiftHeld;
            shiftHeld = d2 >= 64;
            if (was !== shiftHeld) needsRedraw = true;
            return;
        }
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const delta = decodeDelta(d2);
            if (delta === 0) return;
            const k = d1 - MoveKnob1;
            if (shiftHeld) adjustAny(k, delta, KNOBS2, knob2Values);
            else adjustAny(k, delta, KNOBS, knobValues);
            return;
        }
        return;
    }

    /* HARD pad release: a long hold was a momentary punch — snap back */
    if ((status === 0x80 || (status === 0x90 && d2 === 0)) && d1 === PAD_HARD) {
        if (hardHeldAt && Date.now() - hardHeldAt >= HARD_HOLD_MS)
            setHard(false, true);
        hardHeldAt = 0;
        return;
    }

    if (status === 0x90 && d2 > 0) {
        if (d1 === PAD_H1 || d1 === PAD_H2 || d1 === PAD_H3 || d1 === PAD_H4) {
            const i = d1 - PAD_H1;
            if (shiftHeld) cycleVoice(i);
            else toggleVoice(i);
            return;
        }
        if (d1 === PAD_HARD) {
            hardHeldAt = Date.now();
            setHard(!hardOn, true);
            return;
        }
        if (d1 === PAD_MONITOR) {
            if (!hwInput) return;   /* audio_fx build: pad is inert */
            if (monitorOn) {
                guardMuted = false;
                guardOverride = false;
                setMonitor(false);
                announce('Output muted');
            } else {
                guardMuted = false;
                if (feedbackRisk()) {
                    guardOverride = true;
                    announce('Monitoring on. Feedback risk!');
                } else {
                    announce('Monitoring on');
                }
                setMonitor(true);
            }
            return;
        }

        /* chromatic strip: press = set Key to that pitch class */
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + 12) {
            const pc = d1 - STEP_FIRST;
            knobValues[0] = pc;
            host_module_set_param('key', `${pc}`);
            announceParameter('Key', NOTE_SPEECH[pc]);
            updateStepLEDs();
            needsRedraw = true;
            return;
        }
        /* harmony indicator steps mirror the voice pads */
        if (d1 >= STEP_FIRST + 12 && d1 < STEP_FIRST + 16) {
            const i = d1 - STEP_FIRST - 12;
            if (shiftHeld) cycleVoice(i);
            else toggleVoice(i);
            return;
        }
    }
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
