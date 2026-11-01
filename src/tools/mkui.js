#!/usr/bin/env node
/*
 * mkui.js - the camera's sprite pack, built from the web sample.
 *
 * doc/menu-sample/index.html is where the film screen is designed. Its cans
 * and panels are SVG and CSS, which a camera with a 150 MIPS processor and a
 * 256-colour layer has no way to draw, so this renders them once, here, in a
 * real browser, and packs the pixels: the background, every can at the five
 * places it can stand, the panels, and the fonts as alpha glyphs. The module
 * reads that one file when the menu opens. ml/700d/ui.c is the other half.
 *
 *   npm i playwright-core
 *   CHROME=<path to a chromium> node tools/mkui.js [design.json]
 *
 * design.json is what the sample's Edit tab copies. Without one, the design
 * is the one in the sample. Output: ml/700d/data/SOUPUI.BIN, and that file is
 * what make card-700d puts on the card, so nothing here runs at build time.
 *
 * The palette is the whole trick. 249 of the 256 entries are free (0, 3 and
 * 20 are Canon's transparent ones, 255 is ClearScreen's, 4 is Magic Lantern's
 * zebra grid, and 1 and 2 stay white and black so that anything Magic Lantern
 * draws stays readable). Text takes fixed ramps; the background and the cans
 * get their own median cuts, since the background needs few colours and the
 * cans need many. Art is dithered with a fixed Bayer matrix so that neighbour
 * frames do not crawl, and panels are not dithered at all.
 */
const fs = require('fs'), path = require('path'), zlib = require('zlib');
const { chromium } = require('playwright-core');

const ROOT = path.resolve(__dirname, '..', '..');
const PAGE = 'file://' + encodeURI(path.join(ROOT, 'doc/menu-sample/index.html'));
const OUT  = path.join(ROOT, 'src/ml/700d/data/SOUPUI.BIN');
const W = 720, H = 480;

/* These numbers are ml/700d/ui.c's. A sprite's place in the table is its id. */
const SP = { HINT_LOAD:0, HINT_CLOSE:1, AUTO_OFF:2, AUTO_ON:3, Q_HALF:4, Q_FULL:5, Q_QUARTER:6,
             TAG0:7, TAG1:8, TAG2:9, PILL:10, PILLHL:13, PEND:16, PBUSY:19, PFULL:22,
             RING:25, STRIP:38, ICON:39, ROWHL:45, LISTHL:48, INCARD:51,
             HINT_HOME:52, HINT_DARK:53, HINT_PRINTS:54, HINT_LENS:55, HINT_SYS:56, CANS:57 };
const CUBE_BASE = 69;                   /* ml/700d/prints.c's SE_CUBE_BASE */
const SLOTS = [ {d:0,x:0,s:1,o:1}, {d:1,x:172,s:.76,o:.96}, {d:-1,x:172,s:.76,o:.96},
                {d:2,x:287,s:.56,o:.72}, {d:-2,x:287,s:.56,o:.72} ];
const RAMPS = { WHITE:'#f2f2f7', DIM:'#9d9ec0', AMBER:'#f4b45c', BODY:'#c6c7dc', DARK:'#241a0a' };
const RAMP_BG = { DARK:'#f4b45c' };     /* what a ramp fades into */
const FONTS = [ {css:'700 39px "Barlow Semi Condensed"', chars:'name'},
                {css:'700 18px "Barlow Semi Condensed"'},
                {css:'700 15px "Barlow Semi Condensed"'},
                {css:'700 12px "Barlow Semi Condensed"'},
                {css:'700 11px "Barlow Semi Condensed"'},
                {css:'700 10px "Barlow Semi Condensed"'},
                {css:'600 13.5px "Barlow"'},
                {css:'700 18px "Barlow"'},
                {css:'600 15px "Barlow"'},
                {css:'700 25px "Barlow Semi Condensed"'} ];

/* ------------------------------------------------------------- png in */
function png(buf) {
  let p = 8, w, h, ct, idat = [];
  while (p < buf.length) {
    const n = buf.readUInt32BE(p), t = buf.toString('latin1', p + 4, p + 8), d = buf.subarray(p + 8, p + 8 + n);
    if (t === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); ct = d[9]; if (d[8] !== 8 || d[12]) throw new Error('png format'); }
    if (t === 'IDAT') idat.push(d);
    p += 12 + n;
  }
  const bpp = ct === 6 ? 4 : 3, raw = zlib.inflateSync(Buffer.concat(idat)), st = w * bpp;
  const out = Buffer.alloc(w * h * 4), row = Buffer.alloc(st), prev = Buffer.alloc(st);
  for (let y = 0, r = 0; y < h; y++, r += st + 1) {
    const f = raw[r];
    for (let i = 0; i < st; i++) {
      const a = i >= bpp ? row[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = raw[r + 1 + i];
      let v;
      if (f === 0) v = x; else if (f === 1) v = x + a; else if (f === 2) v = x + b;
      else if (f === 3) v = x + ((a + b) >> 1);
      else { const q = a + b - c, pa = Math.abs(q - a), pb = Math.abs(q - b), pc = Math.abs(q - c);
             v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c); }
      row[i] = v & 255;
    }
    row.copy(prev);
    for (let x = 0; x < w; x++) {
      out[(y * w + x) * 4] = row[x * bpp]; out[(y * w + x) * 4 + 1] = row[x * bpp + 1]; out[(y * w + x) * 4 + 2] = row[x * bpp + 2];
      out[(y * w + x) * 4 + 3] = bpp === 4 ? row[x * 4 + 3] : 255;
    }
  }
  return { w, h, px: out };
}

/* -------------------------------------------------------------- colour */
const hex = s => [1, 3, 5].map(i => parseInt(s.slice(i, i + 2), 16));
/* Magic Lantern's rgb2yuv422_rec601: the bitmap layer is standard definition */
function yuv(r, g, b) {
  const c = (v, a, z) => Math.max(a, Math.min(z, Math.trunc(v)));
  const Y = c((306 * r + 601 * g + 116 * b) / 1024, 0, 255);
  const U = c((-172 * r - 337 * g + 509 * b) / 1024, -128, 127);
  const V = c((509 * r - 427 * g - 82 * b) / 1024, -128, 127);
  return ((Y & 255) << 16 | (U & 255) << 8 | (V & 255)) >>> 0;
}
/* median cut over a histogram of 5-bit colours */
function medianCut(hist, n) {
  const all = [...hist.entries()].map(([k, c]) => [k >> 10 & 31, k >> 5 & 31, k & 31, c]);
  const boxes = [all];
  const score = b => { let lo = [99,99,99], hi = [0,0,0], cnt = 0;
    for (const e of b) { for (let i = 0; i < 3; i++) { lo[i] = Math.min(lo[i], e[i]); hi[i] = Math.max(hi[i], e[i]); } cnt += e[3]; }
    const ax = [0,1,2].sort((p, q) => (hi[q]-lo[q]) - (hi[p]-lo[p]))[0];
    return { ax, sp: (hi[ax] - lo[ax]) * Math.sqrt(cnt) }; };
  while (boxes.length < n) {
    let best = -1, bs = 0, ax = 0;
    boxes.forEach((b, i) => { if (b.length < 2) return; const s = score(b); if (s.sp > bs) { bs = s.sp; best = i; ax = s.ax; } });
    if (best < 0) break;
    const b = boxes[best].sort((p, q) => p[ax] - q[ax]);
    const tot = b.reduce((s, e) => s + e[3], 0); let acc = 0, cut = 1;
    for (let i = 0; i < b.length - 1; i++) { acc += b[i][3]; if (acc * 2 >= tot) { cut = i + 1; break; } }
    boxes.splice(best, 1, b.slice(0, cut), b.slice(cut));
  }
  return boxes.map(b => { let r = 0, g = 0, bl = 0, c = 0;
    for (const e of b) { r += e[0] * e[3]; g += e[1] * e[3]; bl += e[2] * e[3]; c += e[3]; }
    const f = v => Math.round((v / c) * 255 / 31); return [f(r), f(g), f(bl)]; });
}
const hkey = (r, g, b) => (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
function histAdd(h, im, weight = 1) {
  for (let i = 0; i < im.w * im.h; i++) {
    if (im.px[i * 4 + 3] < 128) continue;
    const k = hkey(im.px[i * 4], im.px[i * 4 + 1], im.px[i * 4 + 2]);
    h.set(k, (h.get(k) || 0) + weight);
  }
}
const BAYER = [0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,
               3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21];

/* flatten a sprite over a colour, or over the background where it will sit */
function flatten(im, under, ox = 0, oy = 0, min = 24) {
  const px = Buffer.from(im.px);
  for (let y = 0; y < im.h; y++) for (let x = 0; x < im.w; x++) {
    const i = (y * im.w + x) * 4, a = px[i + 3] / 255;
    if (px[i + 3] < min) { px[i + 3] = 0; continue; }
    const ub = under.px ? under.px : null, j = ub ? ((oy + y) * under.w + ox + x) * 4 : 0;
    for (let c = 0; c < 3; c++) px[i + c] = Math.round(px[i + c] * a + (ub ? ub[j + c] : under[c]) * (1 - a));
    px[i + 3] = 255;
  }
  return { w: im.w, h: im.h, px };
}

(async () => {
  const design = process.argv[2] ? JSON.parse(fs.readFileSync(process.argv[2], 'utf8')) : null;
  const browser = await chromium.launch({ executablePath: process.env.CHROME });
  const pg = await browser.newPage({ viewport: { width: 1500, height: 900 }, deviceScaleFactor: 1 });
  await pg.goto(PAGE);
  await pg.waitForFunction(() => document.fonts.status === 'loaded');
  await pg.evaluate(async d => {
    if (d) { FILMS = d.films; THEME = Object.assign({}, THEME_DEFAULT, d.theme); applyTheme(); buildCans(); render(); }
    await Promise.all(['700 39px "Barlow Semi Condensed"', '600 13.5px "Barlow"', '700 12px "Barlow"'].map(f => document.fonts.load(f)));
    S.idleMax = 1e9;
    window.fit = () => {};
    const lcd = document.getElementById('lcd');
    document.querySelector('.bezel').style.width = '734px';
    document.getElementById('cam').style.maxWidth = '1000px';
    lcd.style.width = '720px';
    document.getElementById('stage').style.transform = 'none';
    press('info');
  }, design);
  await pg.addStyleTag({ content: `
    html,body,.wrap,.cam,.bezel,.lcd,#stage,#s-menu{background:transparent!important;box-shadow:none!important}
    .can svg{filter:none!important} *{transition:none!important;animation:none!important}
    .solo #stage *{visibility:hidden!important} .solo #stage .show,.solo #stage .show *{visibility:visible!important}` });
  await pg.waitForTimeout(150);
  const box = await pg.evaluate(() => { const r = document.getElementById('lcd').getBoundingClientRect(); return { x: r.x, y: r.y }; });
  if (box.x !== Math.round(box.x) || box.y !== Math.round(box.y)) throw new Error('the LCD is not on a whole pixel');
  const solo = sel => pg.evaluate(sel => { document.body.classList.add('solo');
    document.querySelectorAll('.show').forEach(e => e.classList.remove('show'));
    sel.forEach(s => document.querySelectorAll(s).forEach(e => e.classList.add('show'))); }, sel);
  const grab = async (x, y, w, h) => png(await pg.screenshot({ clip: { x: box.x + x, y: box.y + y, width: w, height: h }, omitBackground: true }));

  /* ---------------------------------------------------- the background */
  await solo(['.bg', '.bg *', '.floor', '.logo', '.logo *']);
  const bgFloor = await grab(0, 0, W, H);       /* the film screen's */
  await solo(['.bg', '.bg *', '.logo', '.logo *']);
  const bg = await grab(0, 0, W, H);            /* every other page's */

  /* the shapes of the panels, measured so nothing here is a magic number */
  const rect = sel => pg.evaluate(s => { const r = document.querySelector(s).getBoundingClientRect(), l = document.getElementById('lcd').getBoundingClientRect();
    return { x: Math.floor(r.x - l.x), y: Math.floor(r.y - l.y), w: Math.ceil(r.width) + 1, h: Math.ceil(r.height) + 1 }; }, sel);

  const hud = [];    /* {im, ox, oy} in table order */
  const hudAt = async (sel, under) => { const r = await rect(sel); return { im: flatten(await grab(r.x, r.y, r.w, r.h), under, r.x, r.y), ox: r.x, oy: r.y }; };

  /* hints: the keys are the ones the camera build has so far */
  const setHints = word => pg.evaluate(w => { document.getElementById('hints').innerHTML =
    hints([['◄►','Film'],['▲▼','Shelf'],['SET',w],['Q','Last shot'],['MENU','Home']]); }, word);
  await solo(['#hints', '#hints *']);
  await setHints('Load');  hud[SP.HINT_LOAD]  = await hudAt('#hints', bg);
  await setHints('Close'); hud[SP.HINT_CLOSE] = await hudAt('#hints', bg);

  await solo(['#tauto', '#tauto *']);
  await pg.evaluate(() => { S.auto = false; renderFilm(); }); hud[SP.AUTO_OFF] = await hudAt('#tauto', bg);
  await pg.evaluate(() => { S.auto = true;  renderFilm(); }); hud[SP.AUTO_ON]  = await hudAt('#tauto', bg);
  await solo(['#tsize', '#tsize *']);
  for (const [sz, id] of [[0, SP.Q_HALF], [1, SP.Q_FULL], [2, SP.Q_QUARTER]]) {
    await pg.evaluate(z => { S.size = z; renderFilm(); }, sz); hud[id] = await hudAt('#tsize', bg);
  }
  await pg.evaluate(() => { S.size = 0; renderFilm(); });

  /* a pill: left cap, one column, right cap */
  const threeSlice = async (html, id, under) => {
    await pg.evaluate(h => { const d = document.createElement('div'); d.id = 'probe';
      d.style.cssText = 'position:absolute;left:300px;top:100px;width:90px;display:flex;justify-content:stretch;z-index:99';
      d.innerHTML = h; d.firstChild.style.width = '90px'; d.firstChild.classList.add('show');
      document.getElementById('stage').appendChild(d); }, html);
    await solo(['#probe', '#probe *']);
    const r = await rect('#probe > *'), im = await grab(r.x, r.y, r.w - 1, r.h - 1), H2 = im.h, cap = Math.ceil(H2 / 2) + 1;
    const cut = (x0, w) => { const px = Buffer.alloc(w * H2 * 4);
      for (let y = 0; y < H2; y++) im.px.copy(px, y * w * 4, (y * im.w + x0) * 4, (y * im.w + x0 + w) * 4); return { w, h: H2, px }; };
    /* a low threshold: a pill's fill is only 7% white, and it is a fill */
    hud[id] = { im: flatten(cut(0, cap), under, 0, 0, 8), ox: 0, oy: 0 };
    hud[id + 1] = { im: flatten(cut(cap, 1), under, 0, 0, 8), ox: 0, oy: 0 };
    hud[id + 2] = { im: flatten(cut(im.w - cap, cap), under, 0, 0, 8), ox: 0, oy: 0 };
    await pg.evaluate(() => document.getElementById('probe').remove());
  };
  const midInfo = [31, 32, 62], midTop = [33, 34, 66];       /* the screen behind the two rows */
  await threeSlice('<div class="pill"></div>', SP.PILL, midInfo);
  await threeSlice('<div class="pill hl"></div>', SP.PILLHL, midInfo);
  await threeSlice('<div class="pend"></div>', SP.PEND, midTop);
  await threeSlice('<div class="pend busy"></div>', SP.PBUSY, midTop);
  await threeSlice('<div class="pend busy full"></div>', SP.PFULL, midTop);

  await threeSlice('<div class="row on" style="padding:0"></div>', SP.ROWHL, [32, 33, 64]);
  await threeSlice('<div class="li on" style="padding:0"></div>', SP.LISTHL, [30, 31, 60]);

  /* the ring, 12 steps: flattened on the pending pill's own colour */
  const pill = hud[SP.PBUSY + 1].im, mid = (pill.h >> 1) * 4, ringBack = [pill.px[mid], pill.px[mid + 1], pill.px[mid + 2]];
  await pg.evaluate(() => { const d = document.createElement('div'); d.id = 'probe';
    d.style.cssText = 'position:absolute;left:300px;top:100px;width:19px;height:19px;z-index:99';
    d.innerHTML = '<svg viewBox="0 0 20 20" style="width:19px;height:19px;transform:rotate(-90deg);display:block"><circle cx="10" cy="10" r="8" fill="none" stroke-width="3" stroke="rgba(255,255,255,.14)"/><circle id="pr" cx="10" cy="10" r="8" fill="none" stroke-width="3" stroke="#f4b45c" stroke-linecap="round" stroke-dasharray="0 51"/></svg>';
    document.getElementById('stage').appendChild(d); });
  await solo(['#probe', '#probe *']);
  for (let i = 0; i <= 12; i++) {
    await pg.evaluate(p => document.getElementById('pr').setAttribute('stroke-dasharray', `${p * 50.3} 51`), i / 12);
    hud[SP.RING + i] = { im: flatten(await grab(300, 100, 19, 19), ringBack), ox: 0, oy: 0 };
  }
  await pg.evaluate(() => document.getElementById('probe').remove());

  /* the film strip of the Loaded moment: one period, 17 pixels */
  await pg.evaluate(() => { document.getElementById('film').classList.add('loaded'); });
  await pg.addStyleTag({ content: '.film.loaded .strip{left:288px;width:34px!important;opacity:1!important;box-shadow:none!important}' });
  await solo(['#strip']);
  { const r = await rect('#strip'); hud[SP.STRIP] = { im: await grab(r.x, r.y, 17, 92), ox: r.x, oy: r.y }; }
  await pg.evaluate(() => { document.getElementById('film').classList.remove('loaded'); });

  /* ----------------------------------------------------------- the cans */
  const films = await pg.evaluate(() => FILMS);
  const canBack = [34, 35, 72];       /* the screen behind the cans */
  const can = async (i, sl) => {
    await pg.evaluate(({ i, sl }) => {
      document.querySelectorAll('.can').forEach((el, k) => {
        const on = k === i; el.classList.toggle('show', on); el.classList.remove('incam');
        el.style.opacity = 1; el.style.zIndex = on ? 50 : 1;
        el.style.transform = `translateX(0) scale(${sl.s})`;
        if (on) { el.querySelector('.lab').style.transform = `translateX(${sl.d * -13}px)`;
                  el.querySelector('.lead').style.opacity = Math.abs(sl.d) > 1 ? .4 : .85; }
      });
    }, { i, sl });
    const x = Math.floor(360 - 58 * sl.s), y = Math.floor(244 - 182 * sl.s), w = Math.ceil(150 * sl.s) + 3, h = 244 - y + 2;
    let im = await grab(x, y, w, h);
    /* the sample's fade, against the screen, not against nothing */
    for (let p = 0; p < w * h; p++) im.px[p * 4 + 3] = Math.round(im.px[p * 4 + 3] * sl.o);
    return { im, ox: x - 360, oy: y - 244 };
  };
  await solo(['.can', '.can *']);
  const cans = [];
  for (let i = 0; i < films.length; i++) for (const sl of SLOTS) {
    const c = await can(i, sl);
    cans.push({ im: flatten(c.im, canBack), ox: c.ox, oy: c.oy });
  }
  /* a film the pack does not know: its can has no name on it */
  const blank = { name: ' ', brand: 'SOUP', shelf: 'COLOUR', iso: '', process: '', style: 'solid', c1: '#3a3b4c', c2: '#8d8ea0', c3: '#f2f2f7', desc: '' };
  await pg.evaluate(f => { FILMS[0] = Object.assign({}, FILMS[0], f); document.querySelector('.can').innerHTML = canSVG(FILMS[0]) + '<span class="tag">IN CAMERA</span>'; }, blank);
  for (const sl of SLOTS) { const c = await can(0, sl); cans.push({ im: flatten(c.im, canBack), ox: c.ox, oy: c.oy }); }
  /* the tag, at the three sizes a can stands */
  await pg.addStyleTag({ content: '.solo #stage .show.tagon svg{visibility:hidden!important}' });
  for (const [k, sl] of [SLOTS[0], SLOTS[1], SLOTS[3]].entries()) {
    await pg.evaluate(({ sl }) => { const el = document.querySelector('.can'); el.classList.add('incam', 'tagon');
      el.style.transform = `translateX(0) scale(${sl.s})`; }, { sl });
    const w = Math.ceil(80 * sl.s), h = Math.ceil(20 * sl.s) + 2, x = 360 - (w >> 1), y = Math.floor(244 - 183 * sl.s) - 1;
    hud[SP.TAG0 + k] = { im: flatten(await grab(x, y, w, h), canBack), ox: x - 360, oy: y - 244 };
  }

  /* ---------------------------------------------------------- the pages */
  await pg.evaluate(() => { document.querySelector('.can').innerHTML = canSVG(FILMS_DEFAULT[0]) + '<span class="tag">IN CAMERA</span>'; document.getElementById('film').style.display = 'none'; go('home'); });
  await solo(['.row svg', '.row svg *']);
  for (let i = 0; i < 6; i++) {
    const r = await rect(`.row:nth-child(${i + 1}) svg`);
    hud[SP.ICON + i] = { im: flatten(await grab(r.x, r.y, r.w, r.h), [32, 33, 64], 0, 0, 40), ox: 0, oy: 0 };
  }
  await pg.evaluate(() => { const c = document.querySelector('.incard'); c.innerHTML = ''; c.classList.add('show'); });
  await solo(['.incard']);
  { const r = await rect('.incard'); hud[SP.INCARD] = { im: flatten(await grab(r.x, r.y, r.w, r.h), bg, r.x, r.y, 8), ox: r.x, oy: r.y }; }
  const pageHint = async (page, id, over) => {
    await pg.evaluate(({ page, over }) => { go(page); if (over) document.getElementById('pagehints').innerHTML = hints(over); }, { page, over });
    await solo(['#pagehints', '#pagehints *']);
    hud[id] = await hudAt('#pagehints', bg);
  };
  await pageHint('home', SP.HINT_HOME);
  await pageHint('darkroom', SP.HINT_DARK);
  await pageHint('prints', SP.HINT_PRINTS, [['◄►▲▼','Move'],['SET','Open'],['MENU','Back']]);
  await pageHint('lens', SP.HINT_LENS);
  await pageHint('system', SP.HINT_SYS);

  /* ---------------------------------------------------------- the fonts */
  const fonts = await pg.evaluate(async FONTS => {
    const out = [];
    for (const f of FONTS) {
      await document.fonts.load(f.css);
      const c = document.createElement('canvas'); c.width = 96; c.height = 72;
      const x = c.getContext('2d', { willReadFrequently: true });
      /* unhinted, as the page draws it: hinted stems at 15 px close a 6 into an 8 */
      x.font = f.css; x.textBaseline = 'alphabetic'; x.textRendering = 'geometricPrecision'; x.fontKerning = 'none';
      const m = x.measureText('Hg'), asc = Math.ceil(m.actualBoundingBoxAscent), desc = Math.ceil(m.actualBoundingBoxDescent);
      const glyphs = [];
      for (let code = 32; code < 128; code++) {
        const ch = code === 127 ? '·' : String.fromCharCode(code);
        const g = { w: 0, h: 0, ox: 0, oy: 0, adv: Math.round(x.measureText(ch).width * 16), a: [] };
        const SS = 4;   /* drawn large and averaged down: glyph edges with no pixel-grid luck in them */
        if (f.chars === 'name' && !/[A-Za-z0-9 ]/.test(ch)) { glyphs.push(g); continue; }
        x.clearRect(0, 0, 96, 72); x.fillStyle = '#fff';
        const big = document.createElement('canvas'); big.width = 96 * SS; big.height = 72 * SS;
        const bx = big.getContext('2d', { willReadFrequently: true });
        bx.font = f.css.replace(/([\d.]+)px/, (m, p) => (parseFloat(p) * SS) + 'px'); bx.textBaseline = 'alphabetic';
        bx.textRendering = 'geometricPrecision'; bx.fontKerning = 'none'; bx.fillStyle = '#fff';
        bx.fillText(ch, 16 * SS, 54 * SS);
        const bd = bx.getImageData(0, 0, 96 * SS, 72 * SS).data, d = new Uint8ClampedArray(96 * 72 * 4);
        for (let yy = 0; yy < 72; yy++) for (let xx = 0; xx < 96; xx++) { let s = 0;
          for (let v = 0; v < SS; v++) for (let u = 0; u < SS; u++) s += bd[((yy * SS + v) * 96 * SS + xx * SS + u) * 4 + 3];
          d[(yy * 96 + xx) * 4 + 3] = s / (SS * SS); }
        let x0 = 96, x1 = -1, y0 = 72, y1 = -1;
        for (let yy = 0; yy < 72; yy++) for (let xx = 0; xx < 96; xx++)
          if (d[(yy * 96 + xx) * 4 + 3] >= 18) { x0 = Math.min(x0, xx); x1 = Math.max(x1, xx); y0 = Math.min(y0, yy); y1 = Math.max(y1, yy); }
        if (x1 >= 0) {
          g.w = x1 - x0 + 1; g.h = y1 - y0 + 1; g.ox = x0 - 16; g.oy = y0 - 54;
          for (let yy = y0; yy <= y1; yy++) for (let xx = x0; xx <= x1; xx++)
            g.a.push(Math.round(d[(yy * 96 + xx) * 4 + 3] / 255 * 7));
        }
        glyphs.push(g);
      }
      out.push({ asc, desc, glyphs });
    }
    return out;
  }, FONTS);
  await browser.close();

  /* -------------------------------------------------------- the palette */
  const free = []; for (let i = 5; i < 255; i++) if (i !== 20) free.push(i);
  const pal = new Array(256).fill(null);                 /* [r,g,b]; null: Canon's */
  const ramps = {};
  for (const [name, c] of Object.entries(RAMPS)) {
    const to = hex(c), from = hex(RAMP_BG[name] || '#1e1f40');
    ramps[name] = [0];
    for (let l = 1; l <= 7; l++) { const i = free.shift(); ramps[name].push(i); pal[i] = to.map((v, k) => Math.round(from[k] + (v - from[k]) * l / 7)); }
  }
  const hudUsed = hud.filter(Boolean);
  const hA = new Map(); cans.forEach(c => histAdd(hA, c.im));
  const hC = new Map(); hudUsed.forEach(c => histAdd(hC, c.im, 4));
  /* The background is a gradient along one line of colour, too subtle for a
   * cut over a 5-bit histogram: it gets its own entries, evenly spaced along
   * the line, and is matched to them directly. The logo goes with the art. */
  const LOGO = (x, y) => y < 42 && x < 140;
  const seen = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { if (LOGO(x, y)) continue;
    const i = (y * W + x) * 4; seen.push([bg.px[i], bg.px[i + 1], bg.px[i + 2]]); }
  seen.sort((a, b) => (a[0] * 3 + a[1] * 6 + a[2]) - (b[0] * 3 + b[1] * 6 + b[2]));
  const nB = 28, nC = 28, nA = free.length - nB - nC, bgPal = [];
  for (let k = 0; k < nB; k++) {
    const part = seen.slice(Math.floor(k * seen.length / nB), Math.floor((k + 1) * seen.length / nB));
    bgPal.push([0, 1, 2].map(c => Math.round(part.reduce((s, p) => s + p[c], 0) / part.length)));
  }
  const bgIdx = bgPal.map(c => { const i = free.shift(); pal[i] = c; return i; });
  [...medianCut(hC, nC), ...medianCut(hA, nA)].forEach(c => { const i = free.shift(); if (i !== undefined) pal[i] = c; });

  /* nearest entry, from a 5-bit cube; the ramps are for text, not art */
  if (free.length && !bgIdx.includes(CUBE_BASE - 1)) throw new Error('the cube has moved');
  const artIdx = []; for (let i = 0; i < 256; i++) if (pal[i] && !bgIdx.includes(i) && !Object.values(ramps).some(r => r.includes(i))) artIdx.push(i);
  const lut = new Uint8Array(262144);
  for (let k = 0; k < 262144; k++) {
    const r = (k >> 12 & 63) * 255 / 63, g = (k >> 6 & 63) * 255 / 63, b = (k & 63) * 255 / 63; let best = 1e9, bi = 2;
    for (const i of artIdx) { const p = pal[i], e = 2 * (p[0] - r) ** 2 + 4 * (p[1] - g) ** 2 + 3 * (p[2] - b) ** 2; if (e < best) { best = e; bi = i; } }
    lut[k] = bi;
  }
  const quant = (im, amp, X0 = 0, Y0 = 0) => {
    const o = Buffer.alloc(im.w * im.h);
    for (let y = 0; y < im.h; y++) for (let x = 0; x < im.w; x++) {
      const i = (y * im.w + x) * 4;
      if (im.px[i + 3] < 128) { o[y * im.w + x] = 0; continue; }
      const t = amp ? (BAYER[((Y0 + y) & 7) * 8 + ((X0 + x) & 7)] / 63 - .5) * amp : 0;
      const c = v => Math.max(0, Math.min(255, v + t));
      o[y * im.w + x] = lut[(c(im.px[i]) >> 2) << 12 | (c(im.px[i + 1]) >> 2) << 6 | c(im.px[i + 2]) >> 2];
    }
    return o;
  };

  const quantBg = bg => {
    const o = Buffer.from(quant(bg, 0));
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      if (LOGO(x, y) && Math.max(...[0, 1, 2].map(c => bg.px[(y * W + x) * 4 + c])) > 110) continue;   /* the logo's ink */
      const i = (y * W + x) * 4, t = (BAYER[(y & 7) * 8 + (x & 7)] / 63 - .5) * 2.5; let best = 1e9, bi = 0;
      for (const n of bgIdx) { const p = pal[n], e = 3 * (p[0] - bg.px[i] - t) ** 2 + 6 * (p[1] - bg.px[i + 1] - t) ** 2 + (p[2] - bg.px[i + 2] - t) ** 2; if (e < best) { best = e; bi = n; } }
      o[y * W + x] = bi;
    }
    return o;
  };

  /* -------------------------------------------------------------- write */
  const parts = []; let off = 0;
  const put = b => { const at = off; parts.push(b); off += b.length; const pad = (4 - (off & 3)) & 3; if (pad) { parts.push(Buffer.alloc(pad)); off += pad; } return at; };
  const HEAD = 160;
  put(Buffer.alloc(HEAD));
  const palB = Buffer.alloc(1024);
  pal.forEach((p, i) => palB.writeUInt32LE(p ? yuv(...p) : 0xffffffff, i * 4));
  const palOff = put(palB);
  /* The photo palette: the ramps and the background stay where they are, and
   * everything else is a colour cube, so the module can turn any photo into
   * these entries by arithmetic. Sprites are mapped across by the table. */
  const pal2 = pal.map(p => p), kept = new Set(bgIdx); Object.values(ramps).forEach(r => r.forEach(i => { if (i) kept.add(i); }));
  for (let i = 5; i < 255; i++) if (i !== 20 && !kept.has(i)) pal2[i] = [0, 0, 0];
  for (let r = 0; r < 6; r++) for (let g = 0; g < 6; g++) for (let b = 0; b < 5; b++) pal2[CUBE_BASE + (r * 6 + g) * 5 + b] = [r * 51, g * 51, Math.round(b * 255 / 4)];
  const pal2B = Buffer.alloc(1024);
  pal2.forEach((p, i) => pal2B.writeUInt32LE(p ? yuv(...p) : 0xffffffff, i * 4));
  const pal2Off = put(pal2B);
  const mapB = Buffer.alloc(256);
  for (let i = 0; i < 256; i++) {
    if (!pal[i] || kept.has(i)) { mapB[i] = i; continue; }
    const c = pal[i], lv = [Math.round(c[0] / 51), Math.round(c[1] / 51), Math.round(c[2] * 4 / 255)];
    mapB[i] = CUBE_BASE + (lv[0] * 6 + lv[1]) * 5 + lv[2];
  }
  const mapOff = put(mapB);
  const bgOff = put(quantBg(bg));
  const floorOff = put(quantBg(bgFloor).subarray(244 * W, 288 * W));
  /* sprites: u16 w,h; s16 ox,oy; u32 offset */
  const spriteList = [...hud, ...cans];
  const pixels = spriteList.map((s, id) => put(quant(s.im, id >= SP.CANS ? 6 : 0, 0, 0)));
  const spT = Buffer.alloc(spriteList.length * 12);
  spriteList.forEach((s, i) => { const o = i * 12; spT.writeUInt16LE(s.im.w, o); spT.writeUInt16LE(s.im.h, o + 2); spT.writeInt16LE(s.ox, o + 4); spT.writeInt16LE(s.oy, o + 6); spT.writeUInt32LE(pixels[i], o + 8); });
  const spOff = put(spT);
  /* films: u32 hash; brand[12]; iso[6]; process[8]; u8 shelf; pad; u16 first can */
  const SHELVES = ['COLOUR', 'SLIDE', 'B&W', 'CINE'];
  const fnv = s => { let h = 2166136261; for (const c of Buffer.from(s, 'latin1')) { h ^= c; h = Math.imul(h, 16777619) >>> 0; } return h >>> 0; };
  const flT = Buffer.alloc(films.length * 32);
  films.forEach((f, i) => { const o = i * 32; flT.writeUInt32LE(fnv(f.name), o);
    flT.write(f.brand.slice(0, 11), o + 4, 'latin1'); flT.write(String(f.iso).slice(0, 5), o + 16, 'latin1'); flT.write(f.process.slice(0, 7), o + 22, 'latin1');
    flT[o + 30] = SHELVES.indexOf(f.shelf); flT[o + 31] = 0; });
  const flOff = put(flT);
  /* fonts: one table each; u8 w,h; s8 ox,oy; u16 adv (1/16 px); pad; u32 offset */
  const fontT = [];
  for (const f of fonts) {
    const gT = Buffer.alloc(96 * 12);
    f.glyphs.forEach((g, i) => { const at = g.w ? put(Buffer.from(g.a)) : 0, o = i * 12;
      gT[o] = g.w; gT[o + 1] = g.h; gT.writeInt8(g.ox, o + 2); gT.writeInt8(g.oy, o + 3); gT.writeUInt16LE(g.adv, o + 4); gT.writeUInt32LE(at, o + 8); });
    fontT.push(put(gT));
  }
  const head = parts[0];
  head.writeUInt32LE(0x31495553, 0);      /* "SUI1" */
  head.writeUInt32LE(off, 4); head.writeUInt32LE(palOff, 8); head.writeUInt32LE(bgOff, 12);
  head.writeUInt32LE(spOff, 16); head.writeUInt32LE(spriteList.length, 20);
  head.writeUInt32LE(flOff, 24); head.writeUInt32LE(films.length, 28);
  head.writeUInt32LE(SP.CANS, 32); head.writeUInt32LE(pal2Off, 36); head.writeUInt32LE(mapOff, 40); head.writeUInt32LE(floorOff, 44);
  fontT.forEach((o, i) => head.writeUInt32LE(o, 48 + i * 4));
  Object.keys(RAMPS).forEach((n, i) => ramps[n].forEach((v, l) => { head[88 + i * 8 + l] = v; }));
  SHELVES.forEach((s, i) => head.write(s, 128 + i * 8, 'latin1'));
  const file = Buffer.concat(parts);
  fs.mkdirSync(path.dirname(OUT), { recursive: true });
  fs.writeFileSync(OUT, file);
  const total = file.length;
  console.log(`SOUPUI.BIN: ${total} bytes, ${(total / 1024) | 0} KB: ${spriteList.length} sprites, ${films.length} films, ${free.length} palette entries left`);
  console.log('hints', hud[SP.HINT_LOAD].ox, hud[SP.HINT_LOAD].oy, 'auto', hud[SP.AUTO_ON].ox, hud[SP.AUTO_ON].oy, 'size', hud[SP.Q_HALF].ox, hud[SP.Q_HALF].oy, 'strip', hud[SP.STRIP].ox, hud[SP.STRIP].oy);
})().catch(e => { console.error(e); process.exit(1); });
