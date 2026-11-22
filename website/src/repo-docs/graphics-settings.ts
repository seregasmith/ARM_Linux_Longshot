import { publicGameText } from '../lib/public-copy.ts';

const escape = value => value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
const slug = value => value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

// The settings table of docs/user/graphics-and-controls.md: one line a menu row, its tab named on the tab's first line.
const TABLE = /^\| Tab \| Option \| Choices \| Default \|\r?\n\|[-| :]+\|\r?\n((?:\|[^\n]+\r?\n?)+)/m;

// The tabs a player sees, as the table names them; the Controls page's rows (TAB_PAGE) are listed with the Controls tab's,
// and the Motion page's (TAB_MOTION) on the table's Motion line. The Debug tab is for testing, not here.
const TAB_NAMES = { GRAPHICS: 'Graphics', AUDIO: 'Audio', GAMEPLAY: 'Gameplay', CONTROLS: 'Controls', PAGE: 'Controls', NFC: 'NFC' };
const OTHER_TABS = ['Audio', 'Gameplay', 'Controls', 'NFC'];
const ACTIONS = new Set(['Restart', 'Quit']);   // (rows that do a thing, with no choice to list)
export const RESTART_BADGE = 'After a restart';

// What a row's cells say about when it shows or what sets it, as small badges on its card. A row the menu hides on some
// PCs (row_shown) must match a `shown` rule, so a new condition in the menu fails the build until the table names it.
export const BADGES = [
  { test: /shown only on NVIDIA GPUs/i, label: 'NVIDIA GPUs only', shown: true },
  { test: /shown only on AMD GPUs/i, label: 'AMD GPUs only', shown: true },
  { test: /shown where DLSS runs/i, label: 'Where DLSS runs', shown: true },
  { test: /\bshown with NVIDIA DLSS\b/i, label: 'With NVIDIA DLSS', shown: true },
  { test: /\bshown while Frame generation is on\b/i, label: 'While Frame generation is on', shown: true },
  { test: /\bshown with a CRT filter on\b/i, label: 'With a CRT filter on', shown: true },
  { test: /\bshown with HUD layout set to Custom and no `HOS_WIDE_UI` override\b/i, label: 'Custom HUD layout without a HOS_WIDE_UI override', shown: true },
  { test: /while HDR is on/i, label: 'While HDR is on', shown: true },
  { test: /\bwith HDR set to Native\b/i, label: 'With HDR on Native', shown: true },
  { test: /\bwith User selected\b/i, label: 'With User selected', shown: true },
  { test: /only in builds with/i, label: 'Depends on GPU and build', shown: true },
  { test: /(?:quality )?preset (?:sets|turns) it/i, label: 'Set by quality presets', both: true },
  { test: /on the Controls page/i, label: 'On the Controls page' },
];

/// The C string literals of a field, joined ("a" "b" is "ab"), their escapes read; null when it has none
function cString(field) {
  const parts = [...field.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map(match => match[1].replace(/\\(.)/g, (_, c) => c === 'n' ? '\n' : c));
  return parts.length ? parts.join('') : null;
}

// Where the menu's sources begin after vk_menu.cpp in the text readMenuRows is given: the feature modules, gpu/vk/menu/*.cpp
export const MODULES_MARKER = '\n// ---- gpu/vk/menu modules ----\n';
const PAGE_TABS = { graphics: 'GRAPHICS', audio: 'AUDIO', gameplay: 'GAMEPLAY', controls: 'CONTROLS', controls_page: 'PAGE',
  nfc: 'NFC', debug: 'DEBUG', motion: 'MOTION', fps_details: 'FPS_DETAILS', mods: 'MODS' };

/// From an opening bracket at `at` to just past its match, strings and comments skipped
function closing(text, at) {
  const open = text[at], close = { '{': '}', '(': ')' }[open];
  let depth = 0;
  for (let i = at; i < text.length; i++) {
    const c = text[i];
    if (c === '/' && text[i + 1] === '/') { while (i < text.length && text[i] !== '\n') i++; continue; }
    if (c === '/' && text[i + 1] === '*') { i = text.indexOf('*/', i + 2) + 1; continue; }
    if (c === '"' || c === "'") { let j = i + 1; while (j < text.length && text[j] !== c) j += text[j] === '\\' ? 2 : 1; i = j; continue; }
    if (c === '{' || c === '(') depth++;
    else if (c === '}' || c === ')') { if (--depth === 0) { if (c !== close) break; return i + 1; } }
  }
  throw new Error('Unreadable menu module');
}

/// A designated initializer's fields ({.key = "x", .label = ...}) as {key: text}
function designated(init) {
  const fields = {};
  let depth = 0, field = '';
  const body = init.slice(1, -1);
  const flush = () => { const m = field.trim().match(/^\.(\w+)\s*=\s*([\s\S]*)$/); if (m) fields[m[1]] = m[2].trim(); field = ''; };
  for (let i = 0; i < body.length; i++) {
    const c = body[i];
    if (c === '/' && body[i + 1] === '/') { while (i < body.length && body[i] !== '\n') i++; continue; }
    if (c === '"') { let j = i + 1; while (j < body.length && body[j] !== '"') j += body[j] === '\\' ? 2 : 1; field += body.slice(i, j + 1); i = j; continue; }
    if ('{(['.includes(c)) depth++;
    if ('})]'.includes(c)) depth--;
    if (c === ',' && depth === 0) { flush(); continue; }
    field += c;
  }
  flush();
  return fields;
}

/// The feature modules' rows and headings (gpu/vk/menu/*.cpp, MENU_MODULE), read without evaluating C++: each chain from
/// menu::tab or menu::page, its .heading, .section and .row calls, a row's fields and the key its place names
/// (after/before). `section` is the chain's own section (.heading or .section before it), else null.
function readModuleRows(text) {
  const rows = [];
  const step = /menu::(tab|page)\("(\w+)"|\.heading\("([^"]+)",\s*"((?:[^"\\]|\\.)*)"|\.section\("([^"]+)"\)|\.row\(/g;
  let page = null, section = null, m;
  while ((m = step.exec(text))) {
    if (m[1]) { page = m[2]; section = null; continue; }
    if (m[3]) { section = m[3]; rows.push({ page, heading: true, key: m[3], name: cString(`"${m[4]}"`), section: m[3] }); continue; }
    if (m[5]) { section = m[5]; continue; }
    const open = text.indexOf('{', m.index);
    const initEnd = closing(text, open);
    const callEnd = closing(text, m.index + m[0].length - 1);
    const fields = designated(text.slice(open, initEnd));
    const place = text.slice(initEnd, callEnd).match(/menu::(?:after|before)\("([^"]+)"\)/)?.[1] || null;
    step.lastIndex = callEnd;
    rows.push({ page, heading: false, key: cString(fields.key || '') || '', name: cString(fields.label || '') || '',
      value: fields.value || 'nullptr', help: cString(fields.help || '') || '', restart: fields.restart === 'true',
      moduleShown: 'shown' in fields, kind: fields.kind, activate: fields.on_activate, text: fields.text || '',
      hint: fields.hint || '', section, place });
  }
  return rows;
}

/// The menu's rows: vk_menu.cpp's own table (`Row rows[]`: the core's section headings and its custom rows), then the
/// feature modules' (gpu/vk/menu/*.cpp, after MODULES_MARKER in `menu`). Each row has tab, name, value, help, restart,
/// heading, its section's id (`section`) and `shown`, false for a row the menu can hide (row_shown, or a module's shown).
/// The order is the core's, then the modules' as written: the menu's own order comes from the model, which the settings
/// table follows (tests/test_settings_table.py), so readGraphicsSettings orders by the table.
export function readMenuRows(menu) {
  const at = menu.indexOf(MODULES_MARKER);
  const core = readCoreRows(at < 0 ? menu : menu.slice(0, at));
  const sections = Object.fromEntries([...(menu.match(/kCoreSections\[\]\[2\]\s*=\s*\{([\s\S]*?)\};/)?.[1] || '')
    .matchAll(/\{"([^"]+)", "(\w+)"\}/g)].map(match => [match[1], match[2]]));
  let section = null, tab = null;
  for (const row of core) {
    if (row.tab !== tab) { tab = row.tab; section = null; }
    if (row.heading) section = sections[row.name] || row.name;
    row.section = section;
    row.key = row.heading ? section : '@' + row.name;
  }
  const modules = at < 0 ? [] : readModuleRows(menu.slice(at + MODULES_MARKER.length));
  for (const row of modules) {
    row.tab = PAGE_TABS[row.page] || row.page.toUpperCase();
    // (this registered selector stores an opaque display identity rather than an Options integer: its own pointer and texts)
    row.monitorSelector = row.tab === 'GRAPHICS' && row.name === 'Fullscreen monitor' && row.value === '&monitor_pick'
      && /\btx_fullscreen_monitor\(/.test(row.text) && /\bhn_fullscreen_monitor\b/.test(row.hint);
  }
  const rows = [...core, ...modules];
  // (a module row with no section of its own is in the section of the row or heading its place names)
  const byKey = new Map(rows.map(row => [row.key, row]));
  const sectionOf = (row, seen = new Set()) => {
    if (row.section || !row.place || seen.has(row)) return row.section;
    seen.add(row);
    const anchor = byKey.get(row.place);
    return anchor ? (anchor.heading ? anchor.key : sectionOf(anchor, seen)) : null;
  };
  for (const row of modules) row.section = sectionOf(row);
  const shown = menu.match(/bool row_shown\(int i\) \{([\s\S]*?)\n\}/)?.[1];
  if (!shown) throw new Error('Missing menu row_shown');
  const conditional = new Set([...shown.matchAll(/&opts\.\w+/g)].map(match => match[0]));
  for (const row of rows) row.shown = !row.moduleShown && !conditional.has(row.value) && row.shown !== false;
  if (!rows.some(row => !row.shown)) throw new Error('Unrecognized menu row_shown');
  return rows;
}

/// vk_menu.cpp's own row table (`Row rows[]`), read without evaluating C++: each `{TAB_..., ...}` entry split at
/// its top-level commas, strings and comments skipped. The fields are Row's: tab, name, value, lo, hi, step, text, help, hint,
/// restart, preset_exempt, cheat, heading.
function readCoreRows(menu) {
  const body = menu.match(/Row rows\[\]\s*=\s*\{([\s\S]*?)\n\};/)?.[1];
  if (!body) throw new Error('Missing menu row table');
  const rows = [];
  let depth = 0, field = '', fields = [];
  for (let i = 0; i < body.length; i++) {
    const c = body[i];
    if (c === '/' && body[i + 1] === '/') { while (i < body.length && body[i] !== '\n') i++; continue; }
    if (c === '/' && body[i + 1] === '*') {
      i = body.indexOf('*/', i + 2) + 1;
      if (i === 0) throw new Error('Unreadable menu row table');
      continue;
    }
    if (c === '"') {
      let j = i + 1;
      while (j < body.length && body[j] !== '"') j += body[j] === '\\' ? 2 : 1;
      if (depth > 0) field += body.slice(i, j + 1);
      i = j;
      continue;
    }
    if (c === '{') {
      if (depth++ === 0) { field = ''; fields = []; continue; }
    } else if (c === '}') {
      if (--depth === 0) {
        fields.push(field.trim());
        const tab = fields[0].match(/^TAB_(\w+)$/)?.[1];
        const name = cString(fields[1] || '');
        if (!tab || name === null) throw new Error(`Unrecognized menu row: ${fields.slice(0, 2).join(', ')}`);
        rows.push({ tab, name, value: fields[2], help: cString(fields[7] || '') || '', restart: fields[9] === 'true',
          heading: fields[12] === 'true', shown: true,
          // This registered selector stores an opaque display identity rather than an Options integer.
          monitorSelector: tab === 'GRAPHICS' && name === 'Fullscreen monitor' && fields[2] === '&monitor_pick'
            && fields[6] === 'tx_fullscreen_monitor' && fields[8] === 'hn_fullscreen_monitor' });
        continue;
      }
      if (depth < 0) throw new Error('Unreadable menu row table');
    } else if (c === ',' && depth === 1) {
      fields.push(field.trim());
      field = '';
      continue;
    }
    if (depth > 0) field += c;
  }
  if (depth !== 0 || !rows.length) throw new Error('Unreadable menu row table');
  // Text fields have no integer option pointer. Their maintained registration and help
  // identify them; keep rejecting arbitrary null-valued Graphics actions.
  const fieldTable = menu.match(/const FieldRow field_rows\[\]\s*=\s*\{([\s\S]*?)\};/)?.[1] || '';
  const textFields = new Set([...fieldTable.matchAll(/\{\s*"([^"]+)"\s*,\s*&\w+\s*,\s*\w+\s*\}/g)].map(match => match[1]));
  for (const row of rows) {
    row.textField = row.value === 'nullptr' && textFields.has(row.name);
    if (row.textField) row.restart = /Applies after (?:a )?restart\./.test(row.help);
  }
  return rows;
}

/// The menu's own help for a card: what the row is. Its restart sentence is the card's badge instead.
const summaryOf = help => help.replace(/\s*Applies after (?:a )?restart\.?/g, '').trim();

// Choices and defaults have one maintained source, the settings page's table; names, order, sections, help and restarts
// come from the menu's. Fail on drift between them instead of publishing a wrong or partial list. Escaped table pipes
// remain cell content.
export function readGraphicsSettings(page, menu) {
  const table = page.match(TABLE);
  if (!table) throw new Error('Missing the settings table');
  let tab = '';
  const settings = [];
  const otherTabs = new Map();
  for (const line of table[1].trim().split('\n')) {
    const cells = line.trim().slice(1, -1).split(/(?<!\\)\|/).map(cell => cell.trim().replaceAll('\\|', '|'));
    if (cells.length !== 4) throw new Error('Changed settings table structure');
    tab = cells[0] || tab;
    const [, name, choices, defaultValue] = cells;
    if (tab !== 'Graphics') {
      if (tab === 'Debug' || !name) continue;
      if (!otherTabs.has(tab)) otherTabs.set(tab, []);
      otherTabs.get(tab).push({ name, choices, defaultValue: defaultValue || '—', id: `setting-${slug(tab)}-${slug(name)}` });
      continue;
    }
    if (!name || !choices || !defaultValue) throw new Error('Incomplete graphics setting');
    settings.push({ name, choices, defaultValue });
  }
  if (!settings.length) throw new Error('No Graphics settings in the table');
  if ([...otherTabs.keys()].some(name => !Object.values(TAB_NAMES).includes(name))) throw new Error('The settings table has an unknown tab');

  const rows = readMenuRows(menu);
  const withSource = (setting, tabName) => {
    const row = rows.find(row => !row.heading && TAB_NAMES[row.tab] === tabName && row.name === setting.name);
    if (!row) throw new Error(`table/menu ${tabName} coverage mismatch at ${setting.name}`);
    // (the table says "after a restart" exactly where the menu's row takes one)
    if (/after a restart/i.test(setting.choices) !== row.restart) throw new Error(`table/menu restart mismatch at ${setting.name}`);
    const badges = BADGES.filter(badge => badge.test.test(badge.both ? `${setting.choices} ${setting.defaultValue}` : setting.choices));
    if (!row.shown && !badges.some(badge => badge.shown)) {
      throw new Error(`The table names no condition for ${setting.name}, which the menu shows only on some PCs`);
    }
    return { ...setting, summary: summaryOf(row.help), restart: row.restart, badges: badges.map(badge => badge.label) };
  };

  // The Graphics tab: the table's lines, in the menu's order (tests/test_settings_table.py holds the table to the menu as
  // built), under the menu's section titles. The rows before the first section are the Quality preset's group.
  const graphics = rows.filter(row => row.tab === 'GRAPHICS');
  for (const row of graphics) {
    if (row.heading && row.value !== undefined && row.value !== 'nullptr') throw new Error(`table/menu graphics coverage mismatch: unrecognized heading ${row.name}`);
  }
  const titles = new Map(graphics.filter(row => row.heading).map(row => [row.section, row.name]));
  const groups = [{ name: 'Quality preset', section: null, settings: [] }];
  for (const setting of settings) {
    const row = graphics.find(row => !row.heading && row.name === setting.name);
    if (!row) throw new Error(`table/menu graphics coverage mismatch at ${setting.name}`);
    const hudReset = row.key === 'reset_hud_placement' && row.name === 'Reset HUD placement' && row.value === 'nullptr' &&
      row.kind === 'menu::Kind::Action' && row.activate === 'MENU_HOOK(reset_hud_placement)';
    const reshadeAction = row.value === 'nullptr' && row.kind === 'menu::Kind::Action' &&
      ((row.key === 'reshade_compare' && row.name === 'Compare ReShade Preset' && row.activate === 'MENU_HOOK(start_comparison)') ||
       (row.key === 'reshade_rescan' && row.name === 'Rescan ReShade presets' && row.activate === 'MENU_HOOK(scan_presets)'));
    const reshadeFile = row.key === 'reshade_user_pick' && row.name === 'User preset file' && row.value === '&user_pick' &&
      row.text === 'MENU_HOOK(filename)';
    if (!row.value.startsWith('&opts.') && !row.textField && !row.monitorSelector && !hudReset && !reshadeAction && !reshadeFile)
      throw new Error(`table/menu graphics coverage mismatch at ${row.name}`);
    if (row.section !== groups.at(-1).section) {
      if (!titles.has(row.section) || groups.some(group => group.section === row.section)) throw new Error(`table/menu graphics order mismatch at ${row.name}`);
      groups.push({ name: titles.get(row.section), section: row.section, settings: [] });
    }
    groups.at(-1).settings.push({ ...withSource(setting, 'Graphics'), id: `setting-${slug(row.name)}` });
  }
  const listed = new Set(settings.map(setting => setting.name));
  const unlisted = graphics.find(row => !row.heading && !listed.has(row.name));
  if (unlisted) throw new Error(`table/menu graphics coverage mismatch at ${unlisted.name}`);
  if (groups.some(group => !group.settings.length) || groups.length - 1 !== titles.size) throw new Error('table/menu graphics coverage mismatch');

  // The other tabs: every row the menu has there, and nothing it has not
  for (const tabName of OTHER_TABS) {
    const listed = (otherTabs.get(tabName) || []).map(setting => setting.name).sort();
    const source = rows.filter(row => !row.heading && TAB_NAMES[row.tab] === tabName && !ACTIONS.has(row.name)).map(row => row.name).sort();
    if (listed.join('\n') !== source.join('\n')) throw new Error(`table/menu ${tabName} coverage mismatch`);
  }
  return [
    ...groups.map(({ section: _section, ...group }) => ({ ...group, tab: 'Graphics', id: `settings-${slug(group.name)}` })),
    ...[...otherTabs].map(([name, tabSettings]) => ({ name, tab: name, id: `settings-${slug(name)}`,
      settings: tabSettings.map(setting => withSource(setting, name)) })),
  ];
}

// (choices this short show on their row; longer ones fold away)
export const SHORT_CHOICES = 120;

/// The settings page with its table replaced by the cards: one table, read and replaced in place.
export async function formatGraphicsSettings(text, menu, processor) {
  const table = text.match(TABLE);
  if (!table) throw new Error('Missing the settings table');
  if (text.slice(table.index + table[0].length).match(TABLE)) throw new Error('Expected one settings table');
  const groups = readGraphicsSettings(text, menu);
  const count = groups.reduce((sum, group) => sum + group.settings.length, 0);
  const inline = async value => (await processor.render(publicGameText(value))).code.trim().replace(/^<p>([\s\S]*)<\/p>$/, '$1');
  const rendered = [];
  for (const group of groups) {
    const cards = [];
    for (const setting of group.settings) {
      const badges = [...(setting.restart ? [RESTART_BADGE] : []), ...setting.badges];
      const choices = await inline(setting.choices);
      const short = setting.choices.length <= SHORT_CHOICES;
      // (one row a setting: its name and badges, what it does with its choices, then its default)
      cards.push(`<article class="graphics-row" data-setting>
<div class="graphics-name"><h4 id="${setting.id}"><a href="#${setting.id}">${escape(publicGameText(setting.name))}</a></h4>
${badges.length ? `<ul class="graphics-badges" aria-label="Conditions">${badges.map(badge => `<li${badge === RESTART_BADGE ? ' class="graphics-badge-restart"' : ''}>${escape(badge)}</li>`).join('')}</ul>` : ''}</div>
<div class="graphics-about">${setting.summary ? `<p class="graphics-summary">${escape(publicGameText(setting.summary))}</p>` : ''}
${short ? `<p class="graphics-choices"><span>Choices</span> ${choices}</p>` : `<details><summary>Choices and requirements</summary><p>${choices}</p></details>`}</div>
<p class="graphics-default"><span>Default</span> ${await inline(setting.defaultValue)}</p>
</article>`);
    }
    rendered.push(`<section class="graphics-group" data-settings-group="${group.id}">
<p class="graphics-tab">${escape(group.tab.toUpperCase())} TAB</p>
<h3 id="${group.id}">${escape(group.name)}</h3>
<div class="graphics-head" aria-hidden="true"><span>Setting</span><span>What it does</span><span>Default</span></div>
<div class="graphics-list">${cards.join('\n')}</div>
</section>`);
  }
  // (a jump list: each tab, and the Graphics tab's sections, with how many settings each has)
  const tabs = [];
  for (const group of groups) {
    if (tabs.at(-1)?.name !== group.tab) tabs.push({ name: group.tab, groups: [] });
    tabs.at(-1).groups.push(group);
  }
  const jump = `<nav class="graphics-jump" aria-label="Jump to a tab or section">${tabs.map(tab => `<div>
<p>${escape(tab.name)} tab</p>
<ul>${tab.groups.map(group => `<li><a href="#${group.id}">${escape(tab.groups.length > 1 ? group.name : `${group.settings.length} settings`)}</a>${tab.groups.length > 1 ? ` <span>${group.settings.length}</span>` : ''}</li>`).join('')}</ul>
</div>`).join('\n')}</nav>`;
  return text.slice(0, table.index) + `<div class="graphics-settings" data-graphics-settings>
${jump}
<form class="graphics-filters" data-settings-filters hidden role="search" aria-label="Settings">
<label>Find a setting<input type="search" name="query" placeholder="Try DLSS, HDR, rumble or restart" aria-controls="graphics-results" /></label>
<label>Section<select name="group" aria-controls="graphics-results"><option value="">Everything</option>${groups.map(group => `<option value="${group.id}">${escape(group.tab === group.name ? group.name : `${group.tab}: ${group.name}`)}</option>`).join('')}</select></label>
<button type="reset">Clear filters</button>
</form>
<p class="graphics-count" data-settings-count role="status" aria-live="polite">${count} settings.</p>
<p data-settings-empty hidden>No settings match. Try another term or clear the filters.</p>
<div id="graphics-results">${rendered.join('\n')}</div>
</div>
` + text.slice(table.index + table[0].length);
}
