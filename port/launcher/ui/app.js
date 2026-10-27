// Downhill Revived launcher front end. Files, network and processes are the Rust side's
// (src-tauri/src/main.rs); this file draws the state it reports.
'use strict';

const T = window.__TAURI__;
const invoke = T.core.invoke;
const listen = T.event.listen;
const appWindow = T.window.getCurrentWindow();
const $ = (id) => document.getElementById(id);

// The player defaults the game applies where dhd.ini doesn't say (port/hle/host.c, pc_settings.c).
const DEFAULTS = {
  scale: '2', window: 'borderless', size: 'monitor', widescreen: '1', DHD_ASPECT: 'wide', skip_movies: '1', vsync: '1',
  fps: '60', textures: 'on', font: 'sharp', vibration: '1', DHD_PRESENT_FILTER: 'linear',
  auto_update: 'ask', launcher: 'on', name: '', server: '', iso: '', unlock_all: '0',
};
const NAME_LETTERS = /[^A-Z0-9 -]/g;

// The settings pages: the rows of OPTIONS > PC SETTINGS plus the dhd.ini lines players change. A row
// with `when` is greyed out, with `offHint`, while that doesn't hold.
const TABS = [
  ['Graphics', [
    { key: 'scale', label: 'Resolution', hint: 'How sharp the 3D picture is drawn. 1x is the PS2\'s own.', type: 'select',
      options: [['1', '1x (PS2)'], ['2', '2x'], ['3', '3x'], ['4', '4x'], ['5', '5x'], ['6', '6x'], ['7', '7x'], ['8', '8x'], ['window', 'Match window']] },
    { key: 'DHD_ASPECT', label: 'Aspect', hint: 'Wide fits your monitor\'s shape; 16:9 is always 16:9', type: 'seg',
      options: [['4:3', '4:3'], ['wide', 'Wide'], ['16:9', '16:9'], ['stretch', 'Stretch'], ['pixels', 'Pixels']] },
    { key: 'DHD_PRESENT_FILTER', label: 'Scaling', type: 'seg', options: [['linear', 'Smooth'], ['point', 'Sharp']] },
    { key: 'fps', label: 'Frame rate', hint: 'Offline races. Online races always run at 60.', type: 'select',
      options: [['60', '60 fps'], ['120', '120 fps'], ['144', '144 fps'], ['165', '165 fps'], ['240', '240 fps'], ['monitor', 'Monitor']] },
    { key: 'vsync', label: 'V-Sync', hint: 'No tearing', type: 'switch', on: '1', off: '0' },
    { key: 'textures', label: 'Texture pack', hint: 'Uses the installed PCSX2 texture pack', type: 'switch', on: 'on', off: 'off',
      when: () => state.texturePack, offHint: 'No pack found. README.txt says where one goes.' },
    { key: 'font', label: 'Font', type: 'seg', options: [['sharp', 'Sharp'], ['original', 'Original']] },
  ]],
  ['Display', [
    { key: 'window', label: 'Display', type: 'seg', options: [['windowed', 'Window'], ['borderless', 'Borderless'], ['fullscreen', 'Fullscreen']] },
    { key: 'size', label: 'Window size', hint: 'The game window\'s size', type: 'select',
      when: () => get('window') === 'windowed', offHint: 'Only for Display: Window',
      options: [['monitor', 'Monitor size'], ['896x672', '896 × 672'], ['1280x960', '1280 × 960'], ['1440x1080', '1440 × 1080'],
                ['1280x720', '1280 × 720'], ['1920x1080', '1920 × 1080'], ['2560x1440', '2560 × 1440']] },
    { key: 'widescreen', label: 'Widescreen', hint: 'A wider view, with the HUD and menus kept in shape', type: 'switch', on: '1', off: '0' },
  ]],
  ['Game', [
    { key: 'iso', label: 'Disc image', type: 'disc' },
    { key: 'skip_movies', label: 'Skip intro movies', type: 'switch', on: '1', off: '0' },
    { key: 'vibration', label: 'Vibration', type: 'switch', on: '1', off: '0' },
    { key: 'unlock_all', label: 'Unlock All', hint: 'Every track, rider, bike and shop item; your save is not changed', type: 'switch', on: '1', off: '0' },
    { key: 'auto_update', label: 'Updates', hint: 'Online races need the newest version', type: 'seg',
      options: [['ask', 'Ask'], ['on', 'Automatic'], ['off', 'Off']] },
    { key: 'launcher', label: 'Open the launcher first', hint: 'Off: starting the game skips this launcher', type: 'switch', on: 'on', off: 'off' },
  ]],
  ['Online', [
    { key: 'name', label: 'Rider name', hint: 'Shown over your rider in online races. Up to 12 letters, digits, spaces or dashes.', type: 'name' },
    { key: 'server', label: 'Race server', hint: 'Leave empty for the official server', type: 'text', placeholder: 'wss://dhd.skuza.app' },
  ]],
];
const GAME_TAB = 2;

const state = {
  version: '', settings: {}, gameFound: true, running: false, tab: 0, update: null, check: 'idle', checkError: '',
  discProblem: null, texturePack: false,
  server: null,       // null: not asked yet; 'down'; or { races, riders, min_version }
  error: null,        // { text, fix: { label, icon, onclick } }: stays in the footer until dismissed
  confirmClose: false,
};
let flashTimer = 0;

function get(key) {
  if (key === 'size') {
    const w = state.settings.width, h = state.settings.height;
    return w && h && w !== 'monitor' ? `${w}x${h}` : 'monitor';
  }
  const v = state.settings[key.toLowerCase()];
  return v !== undefined && v !== '' ? v : DEFAULTS[key] ?? '';
}

async function write(key, value) {
  if (key === 'size') {
    const [w, h] = value === 'monitor' ? ['monitor', 'monitor'] : value.split('x');
    await invoke('save_setting', { key: 'width', value: w });
    await invoke('save_setting', { key: 'height', value: h });
    state.settings.width = w; state.settings.height = h;
  } else {
    await invoke('save_setting', { key, value });
    state.settings[key.toLowerCase()] = value;
  }
}

async function save(key, value, label) {
  try {
    await write(key, value);
    flash(`Saved. ${label}`);
  } catch (e) {
    fail(String(e));
  }
}

const needsDisc = () => !get('iso') || !!state.discProblem;

// ---------- pages ----------

function show(page) {
  for (const p of ['play', 'settings', 'notes']) $(`page-${p}`).hidden = p !== page;
  document.querySelectorAll('.nav button').forEach((b) => b.classList.toggle('on', b.dataset.page === page));
}

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null) e.append(k);
  return e;
}

function icon(id, cls = '') {
  const ns = 'http://www.w3.org/2000/svg';
  const s = document.createElementNS(ns, 'svg');
  s.setAttribute('class', `i ${cls}`.trim());
  const u = document.createElementNS(ns, 'use');
  u.setAttribute('href', `#${id}`);
  s.append(u);
  return s;
}

// ---------- play page ----------

const baseName = (p) => p.split(/[\\/]/).pop();

// The notes' sections as { title, text }: each bold line and the first sentence of its first
// bullet; notes without bold lines give their first bullets as titles.
function newsItems(notes) {
  const plain = (s) => s.replace(/:?\s*<https?:[^>\s]+>/g, '').replace(/\*\*|`/g, '').replace(/\[([^\]]+)\]\([^)]+\)/g, '$1').trim();
  const items = [];
  for (const line of notes.split('\n')) {
    const title = line.match(/^\*\*([^*]+)\*\*$/)?.[1];
    const bullet = line.match(/^\s*[-*]\s+(.*)/)?.[1];
    if (title) items.push({ title: plain(title), text: '' });
    else if (bullet && items.length && !items.at(-1).text) items.at(-1).text = plain(bullet).split(/(?<=[.!?])\s/)[0];
  }
  if (items.length) return items;
  return notes.split('\n').map((l) => l.match(/^\s*[-*]\s+(.*)/)?.[1]).filter(Boolean).map((b) => ({ title: plain(b), text: '' }));
}

function released(iso) {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const opts = { month: 'short', day: 'numeric' };
  if (d.getFullYear() !== new Date().getFullYear()) opts.year = 'numeric';
  return `Released ${d.toLocaleDateString('en-US', opts)}`;
}

function renderHome() {
  const first = needsDisc();
  $('home-ready').hidden = first;
  $('home-first').hidden = !first;
  if (first) {
    $('first-title').textContent = state.discProblem ? 'Choose your disc again' : 'One step to the start gate';
    const step = $('first-step');
    step.textContent = state.discProblem || 'Any folder; the file stays where it is';
    step.classList.toggle('bad', !!state.discProblem);
  } else {
    const r = state.update?.release;
    const notes = r?.notes ? playerNotes(r.notes) : '';
    const items = notes ? newsItems(notes).slice(0, 4) : [];
    if (items.length) {
      $('home-title').replaceChildren('New in ', el('b', {}, r.version));
      const date = released(r.published);
      $('home-meta').replaceChildren(...(date ? [date, el('span', { class: 'sep' }, '·')] : []),
        el('button', { class: 'link', onclick: () => show('notes') }, 'Read all patch notes'));
      $('news').replaceChildren(...items.map((i) => el('li', {}, el('b', {}, i.title), i.text)));
    } else {
      $('home-title').textContent = 'Ready to ride';
      $('home-meta').replaceChildren(`Version ${state.version}`);
    }
    $('news').hidden = !items.length;
  }
  const iso = get('iso');
  $('fact-disc').hidden = first || !iso;
  $('fact-disc').title = iso;
  $('fact-disc-name').textContent = baseName(iso);
  renderServer();
}

function renderServer() {
  const f = $('fact-server');
  const s = state.server;
  f.hidden = !s;
  if (!s) return;
  if (s === 'down') return f.replaceChildren(el('span', { class: 'dot' }), 'Race server not reachable');
  if (newer(s.min_version, state.version))
    return f.replaceChildren(el('span', { class: 'dot warn' }), `Online races need version ${s.min_version}`);
  const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;
  const open = s.races ? `${plural(s.races, 'public race', 'public races')}, ${plural(s.riders, 'rider', 'riders')}` : 'no public races right now';
  f.replaceChildren(el('span', { class: 'dot ok' }), `Race server up · ${open}`);
}

function newer(a, b) {
  const p = (v) => (v || '').split('.').map(Number);
  const [x, y] = [p(a), p(b)];
  for (let i = 0; i < 3; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0);
  return false;
}

// ---------- settings ----------

function control(row, off) {
  const value = get(row.key);
  const label = (text) => `${row.label}: ${text}`;
  const tab = off ? -1 : null;
  switch (row.type) {
    case 'seg':
      return el('div', { class: 'seg', role: 'radiogroup', 'aria-label': row.label },
        row.options.map(([v, t]) => el('button', {
          class: v === value ? 'on' : '', role: 'radio', 'aria-checked': String(v === value), tabindex: tab,
          onclick: () => save(row.key, v, label(t)).then(renderRows),
        }, t)));
    case 'select': {
      const s = el('select', { 'aria-label': row.label, disabled: off, onchange: (e) => {
        const opt = row.options.find(([v]) => v === e.target.value);
        save(row.key, e.target.value, label(opt[1]));
      } }, row.options.map(([v, t]) => el('option', { value: v, selected: v === value }, t)));
      return el('div', { class: 'select' }, s, icon('i-down'));
    }
    case 'switch': {
      const on = !off && value === row.on;
      return el('button', { class: 'sw', role: 'switch', 'aria-checked': String(on), 'aria-label': row.label, disabled: off,
        onclick: () => save(row.key, on ? row.off : row.on, label(on ? 'off' : 'on')).then(renderRows) });
    }
    case 'text':
      return el('input', { class: 'text-in', value, placeholder: row.placeholder, spellcheck: 'false', 'aria-label': row.label,
        onchange: (e) => save(row.key, e.target.value.trim(), label(e.target.value.trim() || 'official')) });
    case 'name':
      return el('input', { class: 'text-in upper', value, maxlength: '12', spellcheck: 'false', 'aria-label': row.label,
        oninput: (e) => { e.target.value = cleanName(e.target.value); },
        onchange: (e) => setName(e.target.value) });
    case 'disc':
      return el('button', { class: 'btn', onclick: chooseDisc }, icon('i-disc'), value ? 'Change' : 'Choose');
  }
}

// Redraws a part of the page and puts the focus back on the control in the same place, so a key or
// pad press on a choice or a tab keeps its place.
function keepFocus(box, draw) {
  const controls = () => [...box.querySelectorAll('button, select, input')];
  const at = controls().indexOf(document.activeElement);
  draw();
  if (at >= 0) controls()[at]?.focus();
}

function renderTabs() {
  keepFocus($('tabs'), () => $('tabs').replaceChildren(...TABS.map(([name], i) => el('button', {
    class: i === state.tab ? 'on' : '', role: 'tab', 'aria-selected': String(i === state.tab),
    onclick: () => { state.tab = i; renderTabs(); renderRows(); },
  }, name))));
}

function renderRows() {
  const rows = TABS[state.tab][1].map((row) => {
    const off = row.when ? !row.when() : false;
    let hint = off ? row.offHint : row.hint;
    let title = null;
    let bad = false;
    if (row.type === 'disc') {
      title = get('iso');
      hint = state.discProblem || (title ? shortPath(title) : 'Not chosen yet');
      bad = !!state.discProblem;
    }
    return el('div', { class: off ? 'row off' : 'row' },
      el('div', { class: 'l' }, row.label, hint ? el('small', { title, class: bad ? 'bad' : null }, hint) : null),
      control(row, off));
  });
  keepFocus($('rows'), () => $('rows').replaceChildren(...rows));
}

// Back to the game's player defaults; the disc image and the rider name stay.
async function resetTab() {
  const [name, rows] = TABS[state.tab];
  try {
    for (const row of rows) if (row.type !== 'disc' && row.type !== 'name') await write(row.key, DEFAULTS[row.key]);
    flash(`${name} is back to the defaults`);
  } catch (e) {
    fail(String(e));
  }
  renderRows();
}

// A long path keeps its drive and its file name: "D:\Games\…\Downhill Domination (USA).iso".
function shortPath(p) {
  return p.length <= 64 ? p : `${p.slice(0, 16)}…${p.slice(-46)}`;
}

async function chooseDisc() {
  try {
    const path = await invoke('pick_disc');
    if (!path) return;
    state.settings.iso = path;
    state.discProblem = null;
    state.error = null;
    renderAll();
    flash('Disc image checked and saved');
  } catch (e) {
    fail(String(e));
  }
}

// ---------- rider name ----------

function cleanName(s) { return s.toUpperCase().replace(NAME_LETTERS, '').slice(0, 12); }

function renderRider() {
  const name = get('name');
  $('rider').classList.toggle('unset', !name);
  $('rider-name').textContent = name || 'Your name';
  $('rider-hint').textContent = name ? 'Rider name' : 'Not set yet';
  $('rider-initial').textContent = name.trim()[0] || '?';
}

async function setName(raw) {
  const name = cleanName(raw).trim();
  if (!name) return;
  await save('name', name, `Rider name: ${name}`);
  renderRider();
}

function editName(on) {
  const input = $('rider-input');
  $('rider-name').hidden = on;
  input.hidden = !on;
  if (on) {
    $('rider-hint').textContent = 'Enter to save';
    input.value = get('name');
    input.focus();
    input.select();
  } else {
    renderRider();
  }
}

// ---------- patch notes ----------

function escapeHtml(s) { return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

function inline(s) {
  return escapeHtml(s.replace(/<(https?:[^>\s]+)>/g, '$1'))
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');
}

// The release notes for players: what's above "## Commits" (the list of commits is for the repo).
function playerNotes(text) {
  return text.replace(/\r/g, '').split(/^##\s+Commits\s*$/m)[0].replace(/^##\s+What's new\s*$/m, '').trim();
}

// A small markdown subset (headings, bold-only lines as headings, bullets, paragraphs): enough
// for GitHub release notes.
function markdown(text) {
  const out = [];
  let list = false;
  for (const raw of text.split('\n')) {
    const line = raw.trimEnd();
    const bullet = line.match(/^\s*[-*]\s+(.*)/);
    const heading = line.match(/^#{1,6}\s+(.*)/) || line.match(/^\*\*([^*]+)\*\*$/);
    if (!bullet && list) { out.push('</ul>'); list = false; }
    if (bullet) { if (!list) { out.push('<ul>'); list = true; } out.push(`<li>${inline(bullet[1])}</li>`); }
    else if (heading) out.push(`<h3>${inline(heading[1])}</h3>`);
    else if (line.trim()) out.push(`<p>${inline(line)}</p>`);
  }
  if (list) out.push('</ul>');
  return out.join('');
}

function renderNotes() {
  const r = state.update?.release;
  if (!r?.notes) return;
  $('notes-title').textContent = `What's new in ${r.version}`;
  $('notes-body').innerHTML = markdown(playerNotes(r.notes));
}

// ---------- footer ----------

function status(kind, iconId, ...parts) {
  const lead = iconId ? icon(iconId, kind) : el('span', { class: `dot ${kind}` });
  $('status').replaceChildren(lead, el('span', {}, ...parts));
}

// A confirmation fades back to the footer's state; an error stays (fail).
function flash(text) {
  clearTimeout(flashTimer);
  if (state.error || state.confirmClose) return;
  status('good', 'i-check', text);
  flashTimer = setTimeout(renderFooter, 3500);
}

function fail(text, fix = null) {
  state.error = { text, fix };
  renderFooter();
}

function setPrimary({ text, icon: iconId = null, disabled = false, onclick }) {
  const b = $('primary');
  b.disabled = disabled;
  b.replaceChildren(...(iconId ? [icon(iconId, iconId === 'i-play' ? 'fill' : '')] : []), el('span', {}, text));
  b.onclick = onclick;
}

// The button beside the primary one: a quiet text button, or a solid one for a fix.
function setSecondary(text, onclick, solid = false) {
  const b = $('secondary');
  b.hidden = !text;
  b.className = solid ? 'btn' : 'btn text';
  if (text) { b.replaceChildren(...(typeof text === 'string' ? [text] : text)); b.onclick = onclick; }
}

function renderFooter() {
  clearTimeout(flashTimer);
  const v = el('b', {}, state.version);
  const playNow = { text: 'Play', icon: 'i-play', onclick: play };
  setSecondary(null);
  $('danger').hidden = true;
  $('dismiss').hidden = true;
  if (state.running) {
    setPrimary({ text: 'Running', disabled: true });
    if (state.confirmClose) {
      status('warn', null, 'Close the game? Progress since your last save is lost.');
      setSecondary('Keep playing', () => { state.confirmClose = false; renderFooter(); });
      $('danger').hidden = false;
      $('danger').textContent = 'Close game';
      $('danger').onclick = () => { state.confirmClose = false; invoke('stop_game'); renderFooter(); };
      return;
    }
    status('ok', null, 'The game is running. The launcher comes back when it closes.');
    return setSecondary('Close game', () => { state.confirmClose = true; renderFooter(); });
  }
  if (!state.gameFound) {
    status('bad', 'i-alert', `${state.game} isn't in this folder. Unzip the whole release together.`);
    setSecondary([icon('i-folder'), 'Open folder'], () => invoke('open_folder').catch((e) => fail(String(e))), true);
    return setPrimary({ ...playNow, disabled: true });
  }
  if (needsDisc() && state.check !== 'installing') {
    if (state.discProblem) status('bad', 'i-alert', 'The disc image chosen before can\'t be used');
    else status('warn', null, 'No disc image yet');
    setPrimary({ text: 'Choose disc', icon: 'i-disc', onclick: chooseDisc });
  } else {
    switch (state.check) {
      case 'checking':
        status('busy', null, 'Looking for updates…');
        setPrimary(playNow);
        break;
      case 'off':
        status('', null, 'Version ', v, '. Update checks are off.');
        setPrimary(playNow);
        break;
      case 'unavailable':
        status('', null, 'Version ', v, '. No update package is published for this platform yet.');
        setPrimary(playNow);
        break;
      case 'failed':
        status('bad', 'i-alert', `Couldn't check for updates. You can still play ${state.version}.`);
        setSecondary([icon('i-retry'), 'Retry'], checkUpdate);
        setPrimary(playNow);
        break;
      case 'available': {
        const r = state.update.release;
        const size = r.size ? `, ${Math.round(r.size / 1048576)} MB` : '';
        status('warn', null, 'Version ', el('b', {}, r.version), ` is ready${size}`);
        setSecondary(`Play ${state.version}`, play);
        setPrimary({ text: 'Update', icon: 'i-download', onclick: installUpdate });
        break;
      }
      case 'installing':
        return; // drawn by the progress events
      case 'install-failed':
        status('bad', 'i-alert', `Update failed: ${state.checkError}`);
        setSecondary([icon('i-retry'), 'Try again'], installUpdate);
        setPrimary(playNow);
        break;
      default:
        status('ok', null, 'Up to date, version ', v);
        setPrimary(playNow);
    }
  }
  if (state.error) {
    const { text, fix } = state.error;
    status('bad', 'i-alert', text);
    setSecondary(fix && [icon(fix.icon), fix.label], fix?.onclick, true);
    $('dismiss').hidden = false;
  }
}

function renderProgress(done, total) {
  const pct = total ? Math.min(100, (done / total) * 100) : 0;
  const mb = (n) => (n / 1048576).toFixed(0);
  const bar = el('div', { class: 'bar' }, el('i', { style: `width:${pct}%` }));
  $('status').replaceChildren(el('div', {},
    'Downloading ', el('b', {}, state.update.release.version), total ? `, ${mb(done)} of ${mb(total)} MB` : `, ${mb(done)} MB`, bar));
}

function renderAll() {
  renderRider();
  renderHome();
  renderRows();
  renderFooter();
}

// ---------- actions ----------

async function checkUpdate() {
  if (get('auto_update') === 'off') { state.check = 'off'; return renderFooter(); }
  state.check = 'checking';
  renderFooter();
  try {
    state.update = await invoke('check_update');
    renderNotes();
    renderHome();
    state.check = !state.update.release ? 'unavailable' : state.update.newer ? 'available' : 'current';
    renderFooter();
    if (state.update.newer && get('auto_update') === 'on') installUpdate();
  } catch (e) {
    state.check = 'failed';
    state.checkError = String(e);
    renderFooter();
  }
}

async function checkServer() {
  try {
    state.server = await invoke('server_status');
  } catch {
    state.server = 'down';
  }
  renderServer();
}

async function installUpdate() {
  state.check = 'installing';
  state.error = null;
  setSecondary(null);
  $('dismiss').hidden = true;
  setPrimary({ text: 'Updating', icon: 'i-download', disabled: true });
  renderProgress(0, state.update.release.size);
  try {
    await invoke('install_update', { release: state.update.release }); // restarts the launcher
  } catch (e) {
    state.check = 'install-failed';
    state.checkError = String(e);
    renderFooter();
  }
}

async function play() {
  try {
    await invoke('play');
    state.running = true;
    state.error = null;
    renderFooter();
  } catch (e) {
    fail(String(e));
  }
}

// ---------- start ----------

async function start() {
  const snap = await invoke('snapshot');
  state.version = snap.version;
  state.settings = snap.settings;
  state.gameFound = snap.game_found;
  state.game = snap.game;
  state.running = snap.running;
  state.discProblem = snap.disc_problem;
  state.texturePack = snap.texture_pack;

  document.querySelectorAll('[data-page]').forEach((b) => b.addEventListener('click', () => show(b.dataset.page)));
  $('win-min').onclick = () => appWindow.minimize();
  $('win-close').onclick = () => appWindow.close();
  $('discord').onclick = () => invoke('open_link', { url: 'discord' });
  $('dismiss').onclick = () => { state.error = null; renderFooter(); };
  $('reset-tab').onclick = resetTab;
  $('fact-disc').onclick = () => { state.tab = GAME_TAB; renderTabs(); renderRows(); show('settings'); };
  $('notes-page').onclick = () => invoke('open_link', { url: state.update?.release?.page || 'https://github.com/skuza-oss/downhill-revived/releases' });

  // the whole rider card edits the name; a click outside the input while editing saves it
  const input = $('rider-input');
  const rider = $('rider');
  rider.tabIndex = 0;
  rider.setAttribute('role', 'button');
  rider.setAttribute('aria-label', 'Change rider name');
  $('rider-edit').tabIndex = -1;
  rider.addEventListener('mousedown', (e) => { if (e.target !== input && !input.hidden) e.preventDefault(); });
  rider.addEventListener('click', (e) => { if (e.target !== input) (input.hidden ? editName(true) : input.blur()); });
  rider.addEventListener('keydown', (e) => { if (e.target === rider && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); editName(true); } });
  input.addEventListener('input', () => { input.value = cleanName(input.value); });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') input.value = get('name');
    if (e.key === 'Enter' || e.key === 'Escape') input.blur();
  });
  input.addEventListener('blur', () => {
    if (cleanName(input.value).trim() && cleanName(input.value).trim() !== get('name')) setName(input.value);
    editName(false);
  });

  // the focus ring is for keys and pads only (style.css)
  addEventListener('keydown', () => document.documentElement.classList.add('kbd'), true);
  addEventListener('pointerdown', () => document.documentElement.classList.remove('kbd'), true);
  document.addEventListener('contextmenu', (e) => { if (!e.target.closest('input, .md')) e.preventDefault(); });

  await listen('update-progress', (e) => renderProgress(e.payload.done, e.payload.total));
  await listen('game-exited', (e) => {
    state.running = false;
    state.confirmClose = false;
    renderFooter();
    checkServer();
    if (e.payload.wants_update) checkUpdate().then(() => { if (state.check === 'available') installUpdate(); });
  });

  renderTabs();
  renderAll();
  $('primary').focus();
  checkUpdate();
  checkServer();
}

start();
