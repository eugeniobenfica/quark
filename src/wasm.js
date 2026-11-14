// Quark's WebAssembly loader and runtime, written by `quark build --target wasm32` next to its module.
//   node prog.js [args...]                    runs prog.wasm (next to this file)
//   Quark.run(bytes, {args, write, host})     in a browser (or require('./prog.js').run): a promise of the exit status
// The module imports its runtime from here ("rt": printing, memory, texts, maps, files) and the host's functions
// ("host"). Memory is a bump arena that only grows: a program's memory is freed when it ends.
'use strict';
(function () {
const MODULE = '@WASM@';
const PANIC_INDEX = 1, PANIC_DIVIDE = 2, PANIC_FUEL = 3, PANIC_EMPTY = 5, PANIC_SIZE = 6, PANIC_SLICE_END = 7,
    PANIC_SLICE_START = 8, PANIC_STACK = 9, PANIC_TEXT_CUT = 10, PANIC_MEMORY = 11, PANIC_OVERFLOW = 12,
    PANIC_TOO_BIG = 13, PANIC_EMPTY_RANGE = 14, PANIC_TASK = 15;
const node = typeof process === 'object' && process.versions && process.versions.node;
const utf8 = new TextEncoder(), fromUtf8 = new TextDecoder();
const M64 = (1n << 64n) - 1n;

class Stop { // a panic (message) or exit(code): unwinds the program back to run()
    constructor(message, code) { this.message = message; this.code = code; }
}

// ---- numbers as C's printf writes them ----
const f64 = new DataView(new ArrayBuffer(8));
function fixed(v, d) { // %.*f, exactly (ties to even on the exact value)
    if (v !== v) return 'nan';
    if (v === Infinity || v === -Infinity) return v > 0 ? 'inf' : '-inf';
    f64.setFloat64(0, v);
    const bits = f64.getBigUint64(0), neg = bits >> 63n === 1n, e = Number((bits >> 52n) & 0x7FFn);
    let m = bits & ((1n << 52n) - 1n), x = e - 1075;
    if (e === 0) x = -1074; else m |= 1n << 52n;
    let q;
    if (x >= 0) q = (m << BigInt(x)) * 10n ** BigInt(d);
    else {
        const num = m * 10n ** BigInt(d), den = 1n << BigInt(-x);
        q = num / den;
        const r2 = (num % den) * 2n;
        if (r2 > den || (r2 === den && (q & 1n))) q++;
    }
    let s = q.toString();
    if (d > 0) {
        if (s.length <= d) s = '0'.repeat(d + 1 - s.length) + s;
        s = s.slice(0, s.length - d) + '.' + s.slice(s.length - d);
    }
    return (neg ? '-' : '') + s;
}
function general(v, p) { // %.*g
    if (v === 0) return Object.is(v, -0) ? '-0' : '0';
    const [mant, ex] = v.toExponential(p - 1).split('e');
    const x = parseInt(ex, 10);
    const trim = s => s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s;
    if (x < -4 || x >= p) return trim(mant) + 'e' + (x < 0 ? '-' : '+') + (Math.abs(x) < 10 ? '0' : '') + Math.abs(x);
    return trim(fixed(v, p - 1 - x));
}
function floatText(v) { // the shortest text that reads back as the same number, with its point
    if (v !== v) return 'nan';
    if (v === Infinity || v === -Infinity) return v > 0 ? 'inf' : '-inf';
    let s = '';
    for (let digits = 1; digits <= 17; digits++) {
        s = general(v, digits);
        if (Number(s) !== v) continue;
        const size = Math.abs(v);
        if (s.includes('e') && size >= 1e-4 && size < 1e16) {
            const decimals = digits - 1 - Math.floor(Math.log10(size));
            s = fixed(v, decimals > 0 ? decimals : 0);
        }
        break;
    }
    return /[.eEn]/.test(s) ? s : s + '.0';
}

// Files where there is no file system (a browser): in memory, for as long as the program runs; options.files
// ({path: text or bytes}) gives the ones it starts with.
function memoryFiles(start) {
    const files = new Map();
    for (const [k, v] of Object.entries(start || {})) files.set(k, typeof v === 'string' ? new TextEncoder().encode(v) : v);
    const missing = () => Object.assign(new Error('no such file'), {code: 'ENOENT'});
    return {
        readFileSync(p) { if (!files.has(p)) throw missing(); return files.get(p).slice(); },
        writeFileSync(p, bytes) { files.set(p, bytes.slice()); },
        statSync(p) { if (!files.has(p)) throw missing(); return {isFile: () => true}; },
        readdirSync(p) {
            const dir = p.replace(/\/+$/, '') + '/', names = new Set();
            for (const k of files.keys()) if (k.startsWith(dir)) names.add(k.slice(dir.length).split('/')[0]);
            if (!names.size) throw missing();
            return [...names];
        },
        readSync() { return 0; },
    };
}

const ERRNO = {ENOENT: 'No such file or directory', EACCES: 'Permission denied', EISDIR: 'Is a directory',
    ENOTDIR: 'Not a directory', EEXIST: 'File exists', EPERM: 'Operation not permitted', EBUSY: 'Device or resource busy'};

function instantiate(module, options) {
    options = options || {};
    const meta = JSON.parse(fromUtf8.decode(WebAssembly.Module.customSections(module, 'quark')[0]));
    const fs = options.fs || (node ? require('fs') : memoryFiles(options.files));
    const path = meta.path;
    let memory, table, u8, dv;
    let heap = meta.heap, lastText = 0;
    const growing = new Array(64).fill(null).map(() => [0, 0]);
    let growingNext = 0;
    const refresh = () => { u8 = new Uint8Array(memory.buffer); dv = new DataView(memory.buffer); };

    // ---- output: buffered, stdout before stderr ----
    const write = options.write || (node ? (fd, bytes) => { let at = 0; while (at < bytes.length) at += fs.writeSync(fd, bytes, at); }
        : (fd, bytes) => (fd === 2 ? console.error : console.log)(fromUtf8.decode(bytes)));
    let outFd = 1, pending = [], pendingLen = 0;
    function flush() {
        if (!pendingLen) return;
        const all = new Uint8Array(pendingLen);
        let at = 0;
        for (const p of pending) { all.set(p, at); at += p.length; }
        pending = []; pendingLen = 0;
        write(1, all);
    }
    function outBytes(bytes) {
        if (outFd === 2) { flush(); write(2, bytes); return; }
        pending.push(bytes); pendingLen += bytes.length;
        if (pendingLen > 1 << 16) flush();
    }
    const out = s => outBytes(utf8.encode(s));

    // ---- memory ----
    function alloc(count, each) {
        if (count < 0 || (count && each > 2 ** 40 / count)) panic(PANIC_SIZE, 0, count, 0);
        let bytes = (count * each + 15) & ~15;
        if (bytes === 0) bytes = 16;
        const p = heap;
        heap += bytes;
        if (heap > memory.buffer.byteLength) {
            const pages = Math.ceil((heap - memory.buffer.byteLength) / 65536);
            try { memory.grow(Math.max(pages, memory.buffer.byteLength >> 17)); } catch (e) {
                try { memory.grow(pages); } catch (e2) { throw new Stop(`${path}: the program used more memory than it is given (4096 MB)`, 1); }
            }
            refresh();
        }
        return p;
    }
    const N = Number, B = BigInt;
    const i64 = p => dv.getBigInt64(p, true);
    const num = p => dv.getUint32(p, true) + dv.getInt32(p + 4, true) * 4294967296;
    const put = (p, v) => dv.setBigInt64(p, typeof v === 'bigint' ? v : B(v), true);
    const bytesAt = (p, n) => u8.slice(p, p + n);
    const textAt = (p, n) => fromUtf8.decode(u8.subarray(p, p + n));
    function newText(bytes) { // [length][bytes]
        const p = alloc(bytes.length + 8, 1);
        put(p, bytes.length);
        u8.set(bytes, p + 8);
        lastText = p;
        return p;
    }
    function cBytes(bytes) { // bytes of their own (and a 0 after them)
        const p = alloc(bytes.length + 1, 1);
        u8.set(bytes, p);
        return p;
    }
    function optional(has, a, b) {
        const p = alloc(3, 8);
        put(p, has ? 1 : 0);
        put(p + 8, has ? a : 0);
        put(p + 16, has ? b : 0);
        return B(p);
    }
    function result(ok, a, b, message) { // [ok][message (bytes, length)][the value's two slots]
        const m = ok ? null : utf8.encode(message);
        const mp = ok ? 0 : cBytes(m);
        const p = alloc(5, 8);
        put(p, ok ? 1 : 0);
        if (!ok) { put(p + 8, mp); put(p + 16, m.length); }
        put(p + 24, a); put(p + 32, b);
        return B(p);
    }
    function textArray(pieces) { // [count][(bytes, length)...], pieces: [address, length]
        const p = alloc(1 + 2 * pieces.length, 8);
        put(p, pieces.length);
        pieces.forEach(([a, n], i) => { put(p + 8 + 16 * i, a); put(p + 16 + 16 * i, n); });
        return p;
    }
    const cannot = (what, name, e) => result(false, 0, 0, `cannot ${what} ${name}: ${ERRNO[e && e.code] || (e && e.message) || 'not available here'}`);

    // ---- panics: the message, and the stack of calls from the engine's trace ----
    const names = meta.fns, inlines = meta.inlines;
    const siteAt = new Map(meta.sites.map(s => [s[0], s]));
    function frames(stack) {
        const fr = [];
        for (const line of String(stack || '').split('\n')) {
            const m = /wasm-function\[(\d+)\]:0x([0-9a-fA-F]+)/.exec(line);
            const fn = m ? +m[1] - meta.imports : -1;
            if (fn >= 0 && fn < meta.fns.length) fr.push({fn, at: parseInt(m[2], 16)}); // not the table's adapters
        }
        return fr;
    }
    function trace(m, inl, stack) {
        const fr = frames(stack);
        const calls = [];
        for (let i = 1; i < fr.length && calls.length < 16; i++) {
            const s = siteAt.get(fr[i].at);
            if (!s) break;
            calls.push({line: s[1], caller: s[2], callee: s[3], inl: s[4]});
        }
        const depth = calls.length;
        if (!depth && inl <= 0) return m;
        const here = depth ? (calls[0].callee >= 0 ? names[calls[0].callee] : 'a function kept as a value')
            : fr.length && fr[0].fn >= 0 ? names[fr[0].fn] : names[meta.main];
        const known = k => k > 0 && k < inlines.length;
        const inlined = (k, fn) => {
            while (known(k)) {
                const c = inlines[k];
                m += `\n  called from ${c[0] > 0 ? inlines[c[0]][1] : fn} at line ${c[2]}`;
                k = c[0];
            }
        };
        m += `\n  in ${known(inl) ? inlines[inl][1] : here}`;
        inlined(inl, here);
        const same = (a, b) => a.caller === b.caller && a.line === b.line;
        for (let i = 0; i < depth; i++) {
            const s = calls[i], caller = names[s.caller];
            if (i >= 2 && same(s, calls[i - 1]) && same(s, calls[i - 2])) {
                while (i + 1 < depth && same(calls[i + 1], s)) i++;
                m += `\n  ... called from ${caller} at line ${s.line}, again and again`;
                continue;
            }
            m += `\n  called from ${known(s.inl) ? inlines[s.inl][1] : caller} at line ${s.line}`;
            inlined(s.inl, caller);
        }
        return m;
    }
    function panic(code, lineAndInl, a, b) {
        const all = B.asUintN(64, B(lineAndInl));
        const inl = N(all >> 32n), L = N(all & 0xFFFFFFFFn);
        a = B(a); b = B(b);
        const at = `${path}:${L}:`;
        const text = {
            [PANIC_INDEX]: `${at} index ${a} is outside the array (its length is ${b})`,
            [PANIC_DIVIDE]: `${at} division by zero`,
            [PANIC_FUEL]: `${at} the program ran too long (its budget ran out)`,
            [PANIC_EMPTY]: `${at} min, max, minBy, maxBy or mean of nothing (the list is empty)`,
            [PANIC_SIZE]: `${at} an array of ${a} elements cannot be made`,
            [PANIC_SLICE_END]: `${at} a slice to ${a} goes past the end (the length is ${b - 1n})`,
            [PANIC_SLICE_START]: `${at} a slice from ${a} starts after its end ${b - 1n}`,
            [PANIC_TEXT_CUT]: `${at} a slice at byte ${a - 1n} cuts a character in two (texts are UTF-8: chars(t) gives the characters)`,
            [PANIC_EMPTY_RANGE]: `${at} randomInt(${a}, ${b}) has no Int to give (the end is not included)`,
            [PANIC_TOO_BIG]: `${at} this Float is too big for an Int (or not a number)`,
            [PANIC_TASK]: `${at} there is no task ${a} (step goes on until it is done, then it is gone)`,
            [PANIC_OVERFLOW]: `${at} an Int went past its limits (overflow)`,
            [PANIC_MEMORY]: `${path}: the program used more memory than it is given (${a >> 20n} MB)`,
            [PANIC_STACK]: `${at} too many calls deep (the stack is full): does a function call itself without end?`,
        }[code] || `${at} stopped (${code})`;
        throw new Stop(trace(text, inl, new Error().stack), 1);
    }

    // ---- random numbers: xoshiro256**, as the native runtime ----
    const rng = [0n, 0n, 0n, 0n];
    let seeded = false;
    function seed(s) {
        let x = B.asUintN(64, B(s));
        for (let i = 0; i < 4; i++) {
            x = (x + 0x9E3779B97F4A7C15n) & M64;
            let z = x;
            z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & M64;
            z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & M64;
            rng[i] = z ^ (z >> 31n);
        }
        seeded = true;
    }
    function next() {
        if (!seeded) seed(B(Date.now()) ^ 0x5DEECE66Dn);
        let r = (rng[1] * 5n) & M64;
        r = (((r << 7n) | (r >> 57n)) & M64) * 9n & M64;
        const t = (rng[1] << 17n) & M64;
        rng[2] ^= rng[0]; rng[3] ^= rng[1]; rng[1] ^= rng[2]; rng[0] ^= rng[3];
        rng[2] ^= t;
        rng[3] = ((rng[3] << 45n) | (rng[3] >> 19n)) & M64;
        return r;
    }

    // ---- text helpers ----
    const isSpace = c => c === 32 || c === 9 || c === 10 || c === 13;
    function splitPieces(t, n, sep) { // [address, length] of the pieces (sharing t's bytes)
        const pieces = [], sn = sep.length;
        let i = 0;
        while (i <= n) {
            let start, end;
            if (sn === 0) {
                while (i < n && isSpace(u8[t + i])) i++;
                if (i >= n) break;
                start = i;
                while (i < n && !isSpace(u8[t + i])) i++;
                end = i;
            } else {
                start = i; end = n;
                outer: for (let k = i; k + sn <= n; k++) {
                    for (let j = 0; j < sn; j++) if (u8[t + k + j] !== sep[j]) continue outer;
                    end = k;
                    break;
                }
                i = end + sn;
                if (end === n) i = n + 1;
            }
            pieces.push([t + start, end - start]);
        }
        return pieces;
    }
    const same = (a, b, n) => { for (let i = 0; i < n; i++) if (u8[a + i] !== u8[b + i]) return false; return true; };
    function decodeChar(t, n, i) { // [code point, next index], as the native runtime (a stray byte is itself)
        const c = u8[t + i];
        let more = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0xF8 || (c >= 0x80 && c < 0xC0) || i + more >= n + (more ? 0 : 1)) more = 0;
        let v = more === 3 ? c & 7 : more === 2 ? c & 15 : more === 1 ? c & 31 : c;
        for (let k = 1; k <= more; k++) {
            if ((u8[t + i + k] & 0xC0) !== 0x80) { more = 0; v = c; break; }
            v = v << 6 | (u8[t + i + k] & 63);
        }
        return [v, i + 1 + more];
    }
    function encodeChar(c, o) {
        if (c < 0x80) o.push(c);
        else if (c < 0x800) o.push(0xC0 | c >> 6, 0x80 | (c & 63));
        else if (c < 0x10000) o.push(0xE0 | c >> 12, 0x80 | (c >> 6 & 63), 0x80 | (c & 63));
        else o.push(0xF0 | c >> 18, 0x80 | (c >> 12 & 63), 0x80 | (c >> 6 & 63), 0x80 | (c & 63));
    }
    function otherCase(c, upper) {
        const odd = c & 1;
        if (upper) {
            if (c >= 97 && c <= 122) return c - 32;
            if (c >= 0xE0 && c <= 0xFE && c !== 0xF7) return c - 32;
            if (c === 0xFF) return 0x178;
            if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && odd) return c - 1;
            if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !odd) return c - 1;
            if (c >= 0x3B1 && c <= 0x3C9 && c !== 0x3C2) return c - 32;
            if (c === 0x3C2) return 0x3A3;
            if (c === 0x3AC) return 0x386;
            if (c >= 0x3AD && c <= 0x3AF) return c - 37;
            if (c === 0x3CC) return 0x38C;
            if (c === 0x3CD || c === 0x3CE) return c - 63;
            if (c >= 0x430 && c <= 0x44F) return c - 32;
            if (c >= 0x450 && c <= 0x45F) return c - 80;
            if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) && odd) return c - 1;
            if (c >= 0x561 && c <= 0x586) return c - 48;
            if (((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) && odd) return c - 1;
            if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
            return c;
        }
        if (c >= 65 && c <= 90) return c + 32;
        if (c >= 0xC0 && c <= 0xDE && c !== 0xD7) return c + 32;
        if (c === 0x178) return 0xFF;
        if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && !odd) return c + 1;
        if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && odd) return c + 1;
        if (c >= 0x391 && c <= 0x3A9 && c !== 0x3A2) return c + 32;
        if (c === 0x386) return 0x3AC;
        if (c >= 0x388 && c <= 0x38A) return c + 37;
        if (c === 0x38C) return 0x3CC;
        if (c === 0x38E || c === 0x38F) return c + 63;
        if (c >= 0x410 && c <= 0x42F) return c + 32;
        if (c >= 0x400 && c <= 0x40F) return c + 80;
        if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) && !odd) return c + 1;
        if (c >= 0x531 && c <= 0x556) return c + 48;
        if (((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) && !odd) return c + 1;
        if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
        return c;
    }
    function changeCase(t, n, upper) {
        const o = [];
        for (let i = 0; i < n;) {
            const at = i;
            const [c, j] = decodeChar(t, n, i);
            i = j;
            if (c >= 0x80 && i - at === 1) { o.push(c); continue; }
            encodeChar(otherCase(c, upper), o);
        }
        return B(newText(Uint8Array.from(o)));
    }

    // ---- maps: entries in the order their keys came, and a table of where each key is (as the native one) ----
    // QkMap: count 0, used 8, cap 16, valueBytes 24, keyText 32, tableCap 40, entries 48, table 56.
    // An entry: [alive][key: 2 slots][value]. The hash is this runtime's own (nothing shows it).
    const entryBytes = m => 24 + num(m + 24);
    const entryAt = (m, i) => num(m + 48) + i * entryBytes(m);
    function keyHash(m, k0, k1) {
        const kt = num(m + 32);
        let h;
        if (kt === 1) {
            const s = N(k0), n = N(k1);
            h = 0x811C9DC5;
            for (let i = 0; i < n; i++) h = Math.imul(h ^ u8[s + i], 0x01000193);
        } else {
            const a = N(B.asUintN(32, k0)), b = N(B.asUintN(32, k0 >> 32n)), c = kt === 2 ? N(B.asUintN(32, k1)) : 0;
            h = Math.imul(a ^ 0x9E3779B9, 0x85EBCA6B) ^ Math.imul(b + 0x632BE59B, 0xC2B2AE35) ^ Math.imul(c + 0x27D4EB2F, 0x165667B1);
        }
        h ^= h >>> 16; h = Math.imul(h, 0x7FEB352D); h ^= h >>> 15;
        return h >>> 0;
    }
    function keyEqual(m, e, k0, k1) {
        const kt = num(m + 32);
        if (kt === 0) return i64(e + 8) === k0;
        if (kt === 2) return i64(e + 8) === k0 && i64(e + 16) === k1;
        const n = N(k1);
        return num(e + 16) === n && same(num(e + 8), N(k0), n);
    }
    function mapFind(m, k0, k1) {
        const cap = num(m + 40);
        if (!cap) return -1;
        const table = num(m + 56), mask = cap - 1;
        for (let i = keyHash(m, k0, k1) & mask; ; i = (i + 1) & mask) {
            const at = dv.getInt32(table + 4 * i, true);
            if (!at) return -1;
            const e = entryAt(m, at - 1);
            if (num(e) && keyEqual(m, e, k0, k1)) return at - 1;
        }
    }
    function mapPlace(m, index) {
        const e = entryAt(m, index), table = num(m + 56), mask = num(m + 40) - 1;
        let i = keyHash(m, i64(e + 8), i64(e + 16)) & mask;
        while (dv.getInt32(table + 4 * i, true)) i = (i + 1) & mask;
        dv.setInt32(table + 4 * i, index + 1, true);
    }
    function mapRehash(m, cap) {
        put(m + 40, cap);
        put(m + 56, alloc(cap, 4));
        const used = num(m + 8);
        for (let i = 0; i < used; i++) if (num(entryAt(m, i))) mapPlace(m, i);
    }
    function mapNew(keyText, valueBytes) {
        const m = alloc(1, 64);
        put(m + 32, keyText);
        put(m + 24, valueBytes);
        return m;
    }

    // ---- standard input (Node): read a chunk at a time ----
    let inBuf = new Uint8Array(0), inAt = 0, inEnd = false;
    function inByte() {
        if (inAt >= inBuf.length) {
            if (inEnd) return -1;
            const chunk = new Uint8Array(1 << 16);
            let got = 0;
            try { got = fs.readSync(0, chunk, 0, chunk.length, null); } catch (e) { if (e.code === 'EAGAIN') return inByte(); got = 0; }
            if (!got) { inEnd = true; return -1; }
            inBuf = chunk.subarray(0, got);
            inAt = 0;
        }
        return inBuf[inAt++];
    }

    const env = options.env || (node ? process.env : {});
    const args = options.args || [];
    const rt = {
        panic,
        printInt: v => out(String(v)),
        printFloat: v => out(floatText(v)),
        printBool: v => out(v ? 'true' : 'false'),
        printText: (p, n) => outBytes(bytesAt(N(p), N(n))),
        printSep: () => out(' '),
        printEnd: () => out('\n'),
        printTo: s => { flush(); outFd = N(s) === 2 ? 2 : 1; },
        printHandle: (p, n, v) => { outBytes(bytesAt(N(p), N(n))); out('#' + v); },
        alloc: (count, each) => B(alloc(N(count), N(each))),
        grow(p, oldCount, newCount, each) {
            const q = alloc(N(newCount), N(each));
            u8.copyWithin(q, N(p), N(p) + N(oldCount) * N(each));
            return B(q);
        },
        copy(p, count, each) {
            const q = alloc(N(count), N(each));
            u8.copyWithin(q, N(p), N(p) + N(count) * N(each));
            return B(q);
        },
        push(p, len, each) { // the array's address with room for one more: in place when it has room
            p = N(p); len = N(len); each = N(each);
            const need = (len + 1) * each, cap = len * 2 > 8 ? len * 2 : 8;
            for (let i = 0; p && i < 64; i++)
                if (growing[i][0] === p) {
                    if (need <= growing[i][1]) return B(p);
                    const q = alloc(cap, each);
                    u8.copyWithin(q, p, p + len * each);
                    growing[i] = [q, cap * each];
                    return B(q);
                }
            const q = alloc(cap, each);
            if (len) u8.copyWithin(q, p, p + len * each);
            growing[growingNext] = [q, cap * each];
            growingNext = (growingNext + 1) % 64;
            return B(q);
        },
        arrayJoin(a, an, b, bn, each) {
            a = N(a); an = N(an); b = N(b); bn = N(bn); each = N(each);
            const p = alloc(an + bn, each);
            u8.copyWithin(p, a, a + an * each);
            u8.copyWithin(p + an * each, b, b + bn * each);
            return B(p);
        },
        now: () => (typeof performance === 'object' ? performance.now() : Date.now()) / 1000,
        time: () => Date.now() / 1000,
        sin: Math.sin, cos: Math.cos, tan: Math.tan, asin: Math.asin, acos: Math.acos, atan: Math.atan,
        atan2: Math.atan2, exp: Math.exp, log: Math.log, pow: Math.pow,
        fuelOut: line => panic(PANIC_FUEL, line, 0, 0),
        textJoin(a, la, b, lb) {
            a = N(a); la = N(la); b = N(b); lb = N(lb);
            const p = lastText;
            // s + t where s is the last text made and nothing came after it: t goes on its end, in place
            if (p && p + 8 === a && la > 0 && num(p) === la && p + ((la + 8 + 15) & ~15) === heap) {
                const tail = u8.slice(b, b + lb);
                const grownTo = p + ((la + lb + 8 + 15) & ~15);
                if (grownTo > heap) alloc(grownTo - heap, 1);
                u8.set(tail, p + 8 + la);
                put(p, la + lb);
                return B(p);
            }
            const both = new Uint8Array(la + lb);
            both.set(u8.subarray(a, a + la));
            both.set(u8.subarray(b, b + lb), la);
            return B(newText(both));
        },
        textCompare(a, la, b, lb) {
            a = N(a); la = N(la); b = N(b); lb = N(lb);
            const n = la < lb ? la : lb;
            for (let i = 0; i < n; i++) if (u8[a + i] !== u8[b + i]) return u8[a + i] < u8[b + i] ? -1n : 1n;
            return la < lb ? -1n : la > lb ? 1n : 0n;
        },
        textOfInt: v => B(newText(utf8.encode(String(v)))),
        textOfFloat: v => B(newText(utf8.encode(floatText(v)))),
        textFixed(v, d) {
            d = N(d);
            const s = fixed(v, d < 0 ? 0 : d > 40 ? 40 : d);
            return B(newText(utf8.encode(s.slice(0, 399))));
        },
        pad(t, n, width) {
            t = N(t); n = N(n); width = N(width);
            let chars = 0;
            for (let i = 0; i < n; i++) if ((u8[t + i] & 0xC0) !== 0x80) chars++;
            const pad = width > chars ? width - chars : 0;
            const o = new Uint8Array(n + pad).fill(32);
            o.set(u8.subarray(t, t + n), pad);
            return B(newText(o));
        },
        trim(t, n) {
            t = N(t); n = N(n);
            let a = 0, b = n;
            while (a < b && isSpace(u8[t + a])) a++;
            while (b > a && isSpace(u8[t + b - 1])) b--;
            return B(newText(u8.slice(t + a, t + b)));
        },
        split: (t, n, s, sn) => B(textArray(splitPieces(N(t), N(n), bytesAt(N(s), N(sn))))),
        words: (t, n) => B(textArray(splitPieces(N(t), N(n), new Uint8Array(0)))),
        lines(t, n) {
            t = N(t); n = N(n);
            if (n && u8[t + n - 1] === 10) n--;
            if (!n) return B(textArray([]));
            const pieces = splitPieces(t, n, Uint8Array.of(10));
            for (const piece of pieces) if (piece[1] && u8[piece[0] + piece[1] - 1] === 13) piece[1]--;
            return B(textArray(pieces));
        },
        chars(t, n) {
            t = N(t); n = N(n);
            const pieces = [];
            for (let i = 0; i < n;) {
                let j = i + 1;
                while (j < n && (u8[t + j] & 0xC0) === 0x80) j++;
                pieces.push([t + i, j - i]);
                i = j;
            }
            return B(textArray(pieces));
        },
        upper: (t, n) => changeCase(N(t), N(n), true),
        lower: (t, n) => changeCase(N(t), N(n), false),
        startsWith: (t, n, s, sn) => (N(sn) <= N(n) && same(N(t), N(s), N(sn))) ? 1n : 0n,
        endsWith: (t, n, s, sn) => (N(sn) <= N(n) && same(N(t) + N(n) - N(sn), N(s), N(sn))) ? 1n : 0n,
        find(t, n, s, sn) {
            t = N(t); n = N(n); s = N(s); sn = N(sn);
            for (let i = 0; i + sn <= n; i++) if (same(t + i, s, sn)) return B(i);
            return -1n;
        },
        replace(t, n, o, on, w, wn) {
            t = N(t); n = N(n); o = N(o); on = N(on); w = N(w); wn = N(wn);
            if (!on) return B(newText(u8.slice(t, t + n)));
            const parts = [];
            let i = 0, from = 0;
            while (i < n) {
                if (i + on <= n && same(t + i, o, on)) {
                    parts.push(u8.slice(t + from, t + i), u8.slice(w, w + wn));
                    i += on;
                    from = i;
                } else i++;
            }
            parts.push(u8.slice(t + from, t + n));
            const all = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
            let at = 0;
            for (const p of parts) { all.set(p, at); at += p.length; }
            return B(newText(all));
        },
        repeat(t, n, times) {
            t = N(t); n = N(n); times = N(times);
            if (times <= 0 || !n) return B(newText(new Uint8Array(0)));
            const one = u8.slice(t, t + n), all = new Uint8Array(n * times);
            for (let i = 0; i < times; i++) all.set(one, i * n);
            return B(newText(all));
        },
        join(items, count, s, sn) {
            items = N(items); count = N(count);
            const sep = bytesAt(N(s), N(sn)), parts = [];
            for (let i = 0; i < count; i++) parts.push(bytesAt(num(items + 16 * i), num(items + 16 * i + 8)));
            const total = parts.reduce((a, p) => a + p.length, 0) + (count > 1 ? sep.length * (count - 1) : 0);
            const all = new Uint8Array(total);
            let at = 0;
            parts.forEach((p, i) => { if (i) { all.set(sep, at); at += sep.length; } all.set(p, at); at += p.length; });
            const p = alloc(total + 8, 1);
            put(p, total);
            u8.set(all, p + 8);
            return B(p);
        },
        textCut(t, n, lo, hi) {
            t = N(t); n = N(n); lo = N(lo); hi = N(hi);
            if (lo > 0 && lo < n && (u8[t + lo] & 0xC0) === 0x80) return B(lo + 1);
            if (hi > 0 && hi < n && (u8[t + hi] & 0xC0) === 0x80) return B(hi + 1);
            return 0n;
        },
        parseInt(t, n) {
            const s = textAt(N(t), N(n));
            if (!/^[+-]?[0-9]+$/.test(s)) return optional(0, 0, 0);
            const v = B(s);
            return v >= -(1n << 63n) && v < (1n << 63n) ? optional(1, v, 0) : optional(0, 0, 0);
        },
        parseFloat(t, n) {
            const s = textAt(N(t), N(n));
            let v = NaN, ok = false;
            if (/^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$/.test(s)) { v = Number(s); ok = true; }
            else if (/^[+-]?(inf|infinity)$/i.test(s)) { v = s[0] === '-' ? -Infinity : Infinity; ok = true; }
            else if (/^[+-]?nan$/i.test(s)) { v = NaN; ok = true; }
            f64.setFloat64(0, v, true);
            return ok ? optional(1, f64.getBigInt64(0, true), 0) : optional(0, 0, 0);
        },
        fail(p, n, line) {
            const all = B.asUintN(64, line);
            const L = N(all & 0xFFFFFFFFn), inl = N(all >> 32n);
            const m = fromUtf8.decode(u8.subarray(N(p), N(p) + Math.min(N(n), 400)));
            throw new Stop(trace(`${path}:${L}: ${m}`, inl, new Error().stack), 1);
        },
        exit(code) { throw new Stop(null, N(B.asIntN(32, code))); },
        readFile(p, n) {
            const name = textAt(N(p), N(n));
            let data;
            try { data = fs.readFileSync(name); } catch (e) { return cannot('read', name, e); }
            return result(true, cBytes(data), data.length);
        },
        writeFile(p, n, t, tn) {
            const name = textAt(N(p), N(n));
            try { fs.writeFileSync(name, bytesAt(N(t), N(tn))); return 1n; } catch (e) { return 0n; }
        },
        fileExists(p, n) {
            try { return fs.statSync(textAt(N(p), N(n))).isFile() ? 1n : 0n; } catch (e) { return 0n; }
        },
        listDir(p, n) {
            const name = textAt(N(p), N(n));
            let list;
            try { list = fs.readdirSync(name); } catch (e) { return cannot('list', name, {code: 'ENOENT'}); }
            const byBytes = list.map(s => utf8.encode(s)).sort((a, b) => {
                for (let i = 0; i < a.length && i < b.length; i++) if (a[i] !== b[i]) return a[i] - b[i];
                return a.length - b.length;
            });
            const pieces = byBytes.map(b => [cBytes(b), b.length]);
            const arr = textArray(pieces);
            return result(true, arr + 8, pieces.length);
        },
        readLine() {
            flush();
            const o = [];
            let c, any = false;
            while ((c = inByte()) >= 0) {
                any = true;
                if (c === 10) break;
                o.push(c);
            }
            if (!any) return optional(0, 0, 0);
            if (o.length && o[o.length - 1] === 13) o.pop();
            return optional(1, cBytes(Uint8Array.from(o)), o.length);
        },
        args() {
            const pieces = args.map(a => { const b = utf8.encode(a); return [cBytes(b), b.length]; });
            return B(textArray(pieces));
        },
        env(p, n) {
            const v = env[textAt(N(p), N(n))];
            if (v === undefined) return optional(0, 0, 0);
            const b = utf8.encode(v);
            return optional(1, cBytes(b), b.length);
        },
        mapNew: (keyText, valueBytes) => B(mapNew(keyText, valueBytes)),
        mapSet(m, k0, k1) {
            m = N(m);
            const found = mapFind(m, k0, k1);
            if (found >= 0) return B(entryAt(m, found) + 24);
            const used = num(m + 8), each = entryBytes(m);
            if (used === num(m + 16)) {
                const cap = used ? used * 2 : 8;
                const entries = alloc(cap, each);
                if (used) u8.copyWithin(entries, num(m + 48), num(m + 48) + used * each);
                put(m + 48, entries);
                put(m + 16, cap);
            }
            const tableCap = num(m + 40);
            if ((used + 1) * 2 > tableCap) mapRehash(m, tableCap ? tableCap * 2 : 16);
            const e = entryAt(m, used);
            u8.fill(0, e, e + each);
            put(e, 1);
            put(e + 8, k0);
            put(e + 16, num(m + 32) ? k1 : 0n);
            mapPlace(m, used);
            put(m + 8, used + 1);
            put(m, num(m) + 1);
            return B(e + 24);
        },
        mapGet(m, k0, k1) {
            m = N(m);
            const at = mapFind(m, k0, k1);
            return at < 0 ? 0n : B(entryAt(m, at) + 24);
        },
        mapRemove(m, k0, k1) {
            m = N(m);
            const at = mapFind(m, k0, k1);
            if (at < 0) return;
            put(entryAt(m, at), 0);
            put(m, num(m) - 1);
        },
        mapCopy(m) {
            m = N(m);
            const c = mapNew(num(m + 32), num(m + 24)), count = num(m);
            if (!count) return B(c);
            const each = entryBytes(m);
            put(c + 16, count);
            put(c + 48, alloc(count, each));
            let used = 0;
            for (let i = 0, n = num(m + 8); i < n; i++) {
                const e = entryAt(m, i);
                if (num(e)) u8.copyWithin(entryAt(c, used++), e, e + each);
            }
            put(c + 8, used);
            put(c, used);
            let tableCap = 16;
            while (tableCap < used * 2) tableCap *= 2;
            mapRehash(c, tableCap);
            return B(c);
        },
        mapLen: m => i64(N(m)),
        mapUsed: m => i64(N(m) + 8),
        mapEntry(m, i) {
            const e = entryAt(N(m), N(i));
            return num(e) ? B(e + 8) : 0n;
        },
        mapKeys: m => mapList(N(m), true),
        mapValues: m => mapList(N(m), false),
        // loops: no collection here (the arena only grows); a frame the code may write its vars into
        loopStart: slots => B(alloc(6 + N(slots), 8)),
        loopCheck: () => 0n,
        loopCollect() {},
        parallelMap(fn, envSlot, input, n, inSlots, outSlots, line) { // one after the other: Wasm has one thread here
            input = N(input); n = N(n); inSlots = N(inSlots); outSlots = N(outSlots);
            if (inSlots > 63) panic(PANIC_SIZE, line, inSlots, 0);
            const f = table.get(N(fn));
            const o = alloc(n, outSlots * 8);
            for (let i = 0; i < n; i++) {
                const a = [];
                for (let k = 0; k < inSlots; k++) a.push(i64(input + 8 * (i * inSlots + k)));
                a.push(envSlot);
                while (a.length < f.length) a.push(0n); // the table's functions all take its widest call's slots
                const r = f(...a);
                const rs = Array.isArray(r) ? r : [r];
                for (let k = 0; k < outSlots; k++) put(o + 8 * (i * outSlots + k), rs[k]);
            }
            return B(o);
        },
        random: () => N(next() >> 11n) * (1 / 9007199254740992),
        randomInt(lo, hi, line) {
            if (hi <= lo) panic(PANIC_EMPTY_RANGE, line, lo, hi);
            const span = B.asUintN(64, hi - lo), limit = M64 - M64 % span;
            let r;
            do r = next(); while (r >= limit);
            return B.asIntN(64, B.asUintN(64, lo) + r % span);
        },
        seedRandom: s => seed(s),
    };
    function mapList(m, keys) {
        const count = num(m), each = keys ? (num(m + 32) ? 16 : 8) : num(m + 24);
        const p = alloc(1 + Math.ceil(count * each / 8), 8);
        put(p, count);
        let w = p + 8;
        for (let i = 0, n = num(m + 8); i < n; i++) {
            const e = entryAt(m, i);
            if (!num(e)) continue;
            u8.copyWithin(w, keys ? e + 8 : e + 24, (keys ? e + 8 : e + 24) + each);
            w += each;
        }
        return B(p);
    }

    const imports = {rt: {}, host: options.host || {}};
    for (const imp of WebAssembly.Module.imports(module))
        if (imp.module === 'rt') imports.rt[imp.name] = rt[imp.name];
    const instance = new WebAssembly.Instance(module, imports);
    memory = instance.exports.memory;
    table = instance.exports.table;
    refresh();

    // runs main: the exit status (a panic prints its message, as the native program does)
    function main() {
        const was = Error.stackTraceLimit;
        Error.stackTraceLimit = 64;
        let status = 0;
        try {
            instance.exports.main();
        } catch (e) {
            if (e instanceof Stop) {
                flush();
                if (e.message !== null) write(2, utf8.encode(e.message + '\n'));
                status = e.code;
            } else if (e instanceof RangeError && /call stack/i.test(e.message)) { // too deep
                const fr = frames(e.stack);
                const top = fr.length && fr[0].fn >= 0 ? fr[0].fn : meta.main;
                flush();
                write(2, utf8.encode(trace(`${path}:${meta.lines[top]}: too many calls deep (the stack is full): does a function call itself without end?`, 0, e.stack) + '\n'));
                status = 1;
            } else if (e instanceof WebAssembly.RuntimeError) {
                flush();
                write(2, utf8.encode(`${path}: ${e.message}\n`));
                status = 1;
            } else throw e;
        } finally {
            Error.stackTraceLimit = was;
        }
        flush();
        return status;
    }
    return {instance, main, rt, meta};
}

async function run(source, options) { // source: the module's bytes, a Response, or a WebAssembly.Module
    let module = source;
    if (!(source instanceof WebAssembly.Module)) {
        if (typeof Response === 'function' && source instanceof Response) source = await source.arrayBuffer();
        module = await WebAssembly.compile(source);
    }
    return instantiate(module, options).main();
}

function runSync(bytes, options) {
    return instantiate(new WebAssembly.Module(bytes), options).main();
}

const api = {run, runSync, instantiate, floatText};
if (typeof module === 'object' && module.exports) module.exports = api;
if (typeof globalThis === 'object') globalThis.Quark = api;
if (node && typeof require === 'function' && require.main === module) {
    const fs = require('fs'), pathMod = require('path');
    const bytes = fs.readFileSync(pathMod.join(__dirname, MODULE));
    process.exitCode = runSync(bytes, {args: process.argv.slice(2)});
}
})();
