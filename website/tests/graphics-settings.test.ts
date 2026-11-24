import test from 'node:test';
import assert from 'node:assert/strict';
import { readdir, readFile } from 'node:fs/promises';
import { createMarkdownProcessor } from '@astrojs/markdown-remark';
import { readGraphicsSettings, formatGraphicsSettings, readMenuRows, MODULES_MARKER, SHORT_CHOICES } from '../src/repo-docs/graphics-settings.ts';
import { matchesSetting, installGraphicsSettings } from '../src/lib/graphics-settings.ts';

// The settings page holds the table of every menu row's choices and defaults; the menu's source holds the rest.
const page = await readFile(new URL('../../docs/user/graphics-and-controls.md', import.meta.url), 'utf8');
// (vk_menu.cpp, then its feature modules, gpu/vk/menu/*.cpp, as pages.ts gives them)
const moduleDir = new URL('../../gpu/vk/menu/', import.meta.url);
const modules = await Promise.all((await readdir(moduleDir)).filter(name => name.endsWith('.cpp')).sort()
  .map(name => readFile(new URL(name, moduleDir), 'utf8')));
const menu = await readFile(new URL('../../gpu/vk/vk_menu.cpp', import.meta.url), 'utf8') + MODULES_MARKER + modules.join('\n');
const processor = await createMarkdownProcessor();

test('frame-generation visibility has a badge and still requires its documented condition', () => {
  const setting = readGraphicsSettings(page, menu).flatMap(group => group.settings)
    .find(setting => setting.name === 'Frame generation by');
  assert(setting.badges.includes('While Frame generation is on'));
  const undocumented = page.replace('Shown while Frame generation is on.', 'Choose the interpolation provider.');
  assert.notEqual(undocumented, page);
  assert.throws(() => readGraphicsSettings(undocumented, menu), /no condition for Frame generation by/);
});

test('CRT-line visibility has a badge and still requires its documented condition', () => {
  const setting = readGraphicsSettings(page, menu).flatMap(group => group.settings)
    .find(setting => setting.name === 'CRT lines');
  assert(setting.badges.includes('With a CRT filter on'));
  const undocumented = page.replace('shown with a CRT filter on; ', '');
  assert.notEqual(undocumented, page);
  assert.throws(() => readGraphicsSettings(undocumented, menu), /no condition for CRT lines/);
});

test('Custom HUD rows require their visibility condition and the registered reset action', () => {
  const settings = readGraphicsSettings(page, menu).flatMap(group => group.settings);
  for (const name of ['Hearts and magic horizontal', 'Reset HUD placement']) {
    assert(settings.find(setting => setting.name === name).badges.includes('Custom HUD layout without a HOS_WIDE_UI override'));
  }
  assert.throws(() => readGraphicsSettings(page.replaceAll('shown with HUD layout set to Custom and no `HOS_WIDE_UI` override', 'Custom'), menu),
    /no condition for Hearts and magic horizontal/);
  assert.throws(() => readGraphicsSettings(page, menu.replace('MENU_HOOK(reset_hud_placement)', 'MENU_HOOK(other_action)')),
    /coverage mismatch at Reset HUD placement/);
});

test('the registered fullscreen monitor selector retains its identity-backed choices; unsupported rows fail', () => {
  const setting = readGraphicsSettings(page, menu).flatMap(group => group.settings)
    .find(setting => setting.name === 'Fullscreen monitor');
  assert.equal(setting.defaultValue, 'Auto (current monitor)');
  assert.equal(setting.restart, false);
  assert.match(setting.choices, /named connected screen/);
  assert.match(setting.summary, /F11 and Alt\+Enter/);
  assert.equal(setting.id, 'setting-fullscreen-monitor');
  // (gpu/vk/menu/display.cpp registers it: its own pointer, its text and its hint)
  for (const [from, to] of [['.value = &monitor_pick,', '.value = &unknown_pick,'],
    ['std::string(tx_fullscreen_monitor(v))', 'std::string(tx_unknown(v))'],
    ['.hint = MENU_HOOK(hn_fullscreen_monitor)', '.hint = MENU_HOOK(hn_unknown)']]) {
    assert(menu.includes(from), from);
    assert.throws(() => readGraphicsSettings(page, menu.replace(from, to)), /coverage mismatch at Fullscreen monitor/);
  }
  // A different Graphics row cannot borrow this selector's pointer or formatter registration.
  assert(menu.includes('.value = &opts.fullscreen,'));
  assert.throws(() => readGraphicsSettings(page, menu.replace('.value = &opts.fullscreen,', '.value = &monitor_pick,')),
    /coverage mismatch at Fullscreen/);
});

test('HDR Native condition accepts current prose but remains required', () => {
  for (const wording of ['with HDR set to Native', 'shown only with HDR set to Native']) {
    const text = page.replaceAll('with HDR set to Native)', `${wording})`);
    const setting = readGraphicsSettings(text, menu).flatMap(group => group.settings)
      .find(setting => setting.name === 'HDR tone mapper');
    assert(setting.badges.includes('With HDR on Native'));
  }
  assert.throws(() => readGraphicsSettings(page.replaceAll('with HDR set to Native)', 'alternative curve)'), menu),
    /no condition for HDR tone mapper/);
});

test('the Audio, Gameplay, Controls and NFC tabs follow the table, and Debug stays out', () => {
  const groups = readGraphicsSettings(page, menu);
  assert.deepEqual(groups.filter(group => group.tab !== 'Graphics').map(group => group.name), ['Audio', 'Gameplay', 'Controls', 'NFC']);
  assert(groups.find(group => group.name === 'Controls').settings.some(setting => setting.name === 'Vibration strength'));
  assert.equal(new Set(groups.flatMap(group => group.settings).map(setting => setting.id)).size, groups.flatMap(group => group.settings).length);
});

test('registered text fields retain their choices and restart requirements; unknown actions still fail', () => {
  const setting = readGraphicsSettings(page, menu).flatMap(group => group.settings)
    .find(setting => setting.name === 'Custom output size');
  assert.equal(setting.defaultValue, 'None');
  assert.equal(setting.restart, true);
  assert.match(setting.choices, /<width>x<height>/);
  const unregistered = menu.replace(/const FieldRow field_rows\[\][\s\S]*?;/, 'const FieldRow field_rows[] = {};');
  assert.notEqual(unregistered, menu);
  assert.throws(() => readGraphicsSettings(page, unregistered), /coverage mismatch at Custom output size/);
  const wrongRestart = page.replace(/(\| \| Custom output size \|[^\n]*)\(most changes apply after a restart\)/, '$1');
  assert.notEqual(wrongRestart, page);
  assert.throws(() => readGraphicsSettings(wrongRestart, menu), /restart mismatch at Custom output size/);
});

test('all Graphics rows follow the source sections and preserve the table\'s choices/defaults', () => {
  const groups = readGraphicsSettings(page, menu).filter(group => group.tab === 'Graphics');
  const settings = groups.flatMap(group => group.settings);
  const lines = page.slice(page.indexOf('| Graphics |'), page.indexOf('| Audio |')).trim().split('\n');
  assert.equal(settings.length, lines.length);
  settings.forEach((setting, i) => {
    const [, , name, choices, value] = lines[i].split('|').map(value => value.trim());
    assert.deepEqual([setting.name, setting.choices, setting.defaultValue], [name, choices, value]);
  });
  assert.equal(groups.find(group => group.name === 'HDR').settings[0].name, 'HDR');
  assert.equal(settings.find(setting => setting.name === 'Output resolution').restart, true);
  assert.equal(settings.find(setting => setting.name === 'Game frame rate cap').restart, false);
  assert(!settings.some(setting => setting.name === 'Volume'));
});

test('upstream value and section-name edits flow through without another settings list', () => {
  const groups = readGraphicsSettings(page.replace('| Docked; a quality preset sets it |', '| Handheld |'), menu.replace('"Resolution and upscaling", nullptr', '"Picture size", nullptr'));
  assert.equal(groups.find(group => group.name === 'Picture size').settings.find(setting => setting.name === 'Profile').defaultValue, 'Handheld');
});

test('missing tables, incomplete cells, added/removed/reordered rows stop publication', () => {
  assert.throws(() => readGraphicsSettings('', menu), /Missing the settings table/);
  assert.throws(() => readGraphicsSettings(page, ''), /Missing menu/);
  assert.throws(() => readGraphicsSettings(page.replace('| | Sharpness |', '| | Gone |'), menu), /mismatch/);
  assert.throws(() => readGraphicsSettings(page.replace(/^\| \| Sharpness .*\n/m, ''), menu), /mismatch/);
  assert.throws(() => readGraphicsSettings(page.replace('| 0%; a quality preset sets it |', '| |'), menu), /Incomplete/);
  assert.throws(() => readGraphicsSettings(page.replace('| | Sharpness |', '| | New cell | Sharpness |'), menu), /structure/);
  assert.throws(() => readGraphicsSettings(page, menu.replace('"Resolution and upscaling", nullptr', '"Resolution and upscaling", NULL')), /coverage mismatch/);
});

test('ReShade actions and user selector are accounted for', () => {
  const settings = readGraphicsSettings(page, menu).flatMap(group => group.settings);
  for (const name of ['Compare ReShade Preset', 'User preset file', 'Rescan ReShade presets'])
    assert(settings.some(setting => setting.name === name));
  assert.throws(() => readGraphicsSettings(page, menu.replace('MENU_HOOK(scan_presets)', 'MENU_HOOK(other_action)')), /coverage mismatch at Rescan ReShade presets/);
  assert.throws(() => readGraphicsSettings(page.replaceAll('with User selected', 'when selected'), menu), /no condition for User preset file/);
});

test('the registered Simulated turf page row takes its named help; another page row fails', () => {
  const setting = readGraphicsSettings(page, menu).flatMap(group => group.settings).find(setting => setting.name === 'Simulated turf');
  assert.match(setting.summary, /fluid simulation/);
  assert.equal(setting.id, 'setting-simulated-turf');
  // (gpu/vk/menu/sim_turf.cpp registers it: its key, a Page row with no option pointer, and the page it opens)
  for (const [from, to] of [['.opens = "sim_turf_settings"', '.opens = "other_page"'], ['.key = "@Simulated turf"', '.key = "@Other turf"']]) {
    assert(menu.includes(from), from);
    assert.throws(() => readGraphicsSettings(page, menu.replace(from, to)), /coverage mismatch at Simulated turf/);
  }
});

test('the menu table reader joins C strings, reads escapes and skips comments', () => {
  const rows = readMenuRows(`Row rows[] = {
    // {TAB_GRAPHICS, "Commented out", &opts.x},
    {TAB_GRAPHICS, "Odd, name", &opts.a, 0, 1, 1, tx_onoff, "Says \\"hi\\", "
     "then more. Applies after a restart.", nullptr, true, true},
    {TAB_GRAPHICS, "Heading", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
};
bool row_shown(int i) {
    return v != &opts.a;
}`);
  assert.deepEqual(rows.map(row => [row.name, row.restart, row.heading, row.shown]), [['Odd, name', true, false, false], ['Heading', false, true, true]]);
  assert.equal(rows[0].help, 'Says "hi", then more. Applies after a restart.');
  assert.throws(() => readMenuRows(menu.replace(/bool row_shown\(int i\) \{/, 'bool shown(int i) {')), /row_shown/);
});

test('cards take the menu\'s help, the source restart flag and the table\'s conditions', () => {
  const settings = readGraphicsSettings(page, menu).flatMap(group => group.settings);
  const find = name => settings.find(setting => setting.name === name);
  assert.equal(find('Profile').summary, 'Runs the game as the console docked or handheld.');
  assert(settings.every(setting => setting.summary), 'every card has the menu\'s description');
  assert(!settings.some(setting => /Applies after/.test(setting.summary)));
  assert.deepEqual(settings.filter(setting => setting.restart).map(setting => setting.name),
    ['Aspect ratio', 'Output resolution', 'Custom output size', 'Profile', 'GPU device', 'Speaker layout', 'Skip title screen', 'Game language', 'Button prompts']);
  assert.deepEqual(find('NVIDIA Reflex').badges, ['NVIDIA GPUs only']);
  assert.deepEqual(find('HDR tone mapper').badges, ['With HDR on Native']);
  assert(find('HDR contrast').badges.includes('While HDR is on'));
  assert(find('Sharpness').badges.includes('Set by quality presets'));
  assert(!settings.some(setting => setting.name === 'Restart' || setting.name === 'Descriptor reuse'));
});

test('restart, coverage and condition drift between the table and the menu stop publication', () => {
  // (the table says after a restart where the menu applies at once, or not where it takes one)
  assert.throws(() => readGraphicsSettings(page.replace('| | Fullscreen | Off, On', '| | Fullscreen | Off, On (after a restart)'), menu), /restart mismatch at Fullscreen/);
  const noSpeakerRestart = page.replace(/^(\| \| Speaker layout \|[^\n]*)after a restart/m, '$1without restarting');
  assert.notEqual(noSpeakerRestart, page, 'Speaker layout fixture must remove its restart annotation');
  assert.throws(() => readGraphicsSettings(noSpeakerRestart, menu), /restart mismatch at Speaker layout/);
  // (a row of another tab added to the menu, or one the table lists that the menu lacks)
  assert.throws(() => readGraphicsSettings(page, menu + '\nMENU_MODULE(balance) {\n    menu::tab("audio")\n        .row({.key = "balance", .label = "Balance", .help = "x", .value = &opts.volume});\n}\n'), /Audio coverage mismatch/);
  assert.throws(() => readGraphicsSettings(page.replace(/^\| \| Mute in background .*\n/m, ''), menu), /Audio coverage mismatch/);
  assert.throws(() => readGraphicsSettings(page.replace('| | Text size |', '| | Text sizes |'), menu), /Gameplay coverage mismatch/);
  // (a row the menu hides on some PCs, with no condition in the table)
  assert.throws(() => readGraphicsSettings(page.replace(' (shown only on AMD GPUs with `VK_AMD_anti_lag`)', ''), menu), /no condition for AMD Anti-Lag 2/);
  assert.throws(() => readGraphicsSettings(page, menu.replace('.label = "Field of view",', '.label = "Field of view",\n              .shown = [] { return false; },')), /no condition for Field of view/);
});

test('escaped pipes remain choices and do not become extra cells', () => {
  const groups = readGraphicsSettings(page.replace('0 to 100%, in steps of 10', 'Off \\| On'), menu);
  assert.equal(groups.flatMap(group => group.settings).find(setting => setting.name === 'Sharpness').choices, 'Off | On');
});

test('cards retain markup, stable anchors, conditions and a readable no-JS fallback', async () => {
  const result = await formatGraphicsSettings(page, menu, processor);
  assert.equal((result.match(/class="graphics-row"/g) || []).length, readGraphicsSettings(page, menu).flatMap(group => group.settings).length);
  assert(result.includes('id="setting-output-resolution"'));
  assert(result.includes('<kbd>F11</kbd>'));
  assert(result.includes('VK_NV_low_latency2'));
  assert(result.includes('only in builds with'));
  assert(result.includes('data-settings-filters hidden'));
  assert(result.includes('<li class="graphics-badge-restart">After a restart</li>'));
  assert(result.includes('<li>Custom HUD layout without a HOS_WIDE_UI override</li>'));
  assert(result.includes('<p class="graphics-summary">Runs the game as the console docked or handheld.</p>'));
  assert(result.includes('<a href="#settings-hdr">HDR</a>') && result.includes('<a href="#settings-controls">'));
  const groups = readGraphicsSettings(page, menu);
  for (const group of groups) assert(result.includes(`href="#${group.id}"`) && result.includes(`id="${group.id}"`));
  const long = groups.flatMap(group => group.settings).filter(setting => setting.choices.length > SHORT_CHOICES).length;
  assert.equal((result.match(/<details><summary>Choices and requirements<\/summary>/g) || []).length, long);
  assert(long > 0 && long < groups.flatMap(group => group.settings).length / 2);
  assert(!result.includes('| Tab | Option | Choices | Default |'), 'the table is replaced by the cards');
  assert(result.includes('Writing the controls in longshot_options.ini yourself'), 'the text after the table stays');
  for (const heading of page.match(/^## .+$/gm)) assert(result.includes(heading));
  await assert.rejects(formatGraphicsSettings('no table', menu, processor), /Missing the settings table/);
  const table = page.match(/^\| Tab \| Option[\s\S]*?\n\n/m)[0];
  await assert.rejects(formatGraphicsSettings(page + '\n' + table, menu, processor), /Expected one settings table/);
});

test('search accepts case, whitespace and terms in any order without regex interpretation', () => {
  assert(matchesSetting('NVIDIA DLSS Frame Generation', '  generation NVIDIA '));
  assert(matchesSetting('Minimum (4GB)', '(4gb)'));
  assert(matchesSetting('NVIDIA Reflex', ''));
  assert(!matchesSetting('NVIDIA Reflex', 'AMD'));
  assert(!matchesSetting('NVIDIA Reflex', '.*'));
});

// A small DOM fixture exercises user-action sequences, not browser layout/native
// focus behavior. Those still need the rendered browser review.
function catalogueFixture(unfoldedHdr = false) {
  const events = object => Object.assign(object, {
    listeners: {},
    addEventListener(type, fn) { (this.listeners[type] ??= []).push(fn); },
    emit(type, event = {}) { for (const fn of this.listeners[type] || []) fn(event); },
  });
  const card = (id, textContent, folded = true) => {
    const details = { open: false, setAttribute(name) { if (name === 'open') this.open = true; } };
    const result = { id, textContent, hidden: false, details: folded ? details : null, querySelector: () => folded ? details : null };
    result.heading = { closest: () => result, scrollIntoView() {} };
    return result;
  };
  const cards = [card('setting-upscaler', 'Upscaler NVIDIA DLSS'), card('setting-sharpness', 'Sharpness 0 to 100%'), card('setting-hdr', 'HDR Off Expanded Native', !unfoldedHdr)];
  const groups = [
    { dataset: { settingsGroup: 'picture' }, querySelectorAll: () => cards.slice(0, 2) },
    { dataset: { settingsGroup: 'hdr' }, querySelectorAll: () => cards.slice(2) },
  ];
  const fields = { query: { value: '' }, group: { value: '' } };
  const form = events({ hidden: true, elements: { namedItem: name => fields[name] }, reset() { this.emit('reset'); } });
  const count = {}, empty = {};
  const root = events({
    querySelector: selector => ({ form, '[data-settings-count]': count, '[data-settings-empty]': empty })[selector],
    querySelectorAll: selector => selector === '[data-setting]' ? cards : groups,
    contains: target => cards.some(card => card.heading === target),
  });
  const document = { querySelector: () => root, getElementById: id => cards.find(card => card.id === id)?.heading };
  const window = events({ location: new URL('https://example.test/Longshot/guides/graphics-and-controls/'), requestAnimationFrame: fn => fn() });
  installGraphicsSettings(document, window);
  const search = (query, group = '') => { fields.query.value = query; fields.group.value = group; form.emit('input'); };
  return { cards, groups, fields, form, count, empty, root, window, search };
}

test('search and sections combine; reset restores all cards and previous disclosures', () => {
  const f = catalogueFixture();
  assert.equal(f.form.hidden, false);
  f.cards[1].querySelector().open = true;
  f.search('NVIDIA');
  assert.deepEqual(f.cards.map(card => card.hidden), [false, true, true]);
  assert.equal(f.cards[0].querySelector().open, true);
  assert.equal(f.groups[1].hidden, true);
  assert.equal(f.count.textContent, '1 of 3 settings shown.');
  f.search('NVIDIA', 'hdr');
  assert.equal(f.empty.hidden, false);
  assert.equal(f.count.textContent, '0 of 3 settings shown.');
  f.form.reset();
  assert(f.cards.every(card => !card.hidden));
  assert.deepEqual(f.cards.map(card => card.querySelector().open), [false, true, false]);
});

test('deep links clear filters, reveal a card and reopen the same permalink', () => {
  const f = catalogueFixture();
  f.search('NVIDIA');
  f.window.location.hash = '#setting-hdr';
  f.window.emit('hashchange');
  assert(f.cards.every(card => !card.hidden));
  assert.equal(f.cards[2].querySelector().open, true);
  f.cards[2].querySelector().open = false;
  f.root.emit('click', { target: { closest: () => ({ href: f.window.location.href }) } });
  assert.equal(f.cards[2].querySelector().open, true);
});

test('printing exposes the whole catalogue then restores filters and disclosure state', () => {
  const f = catalogueFixture();
  f.cards[1].querySelector().open = true;
  f.search('NVIDIA');
  f.window.emit('beforeprint');
  assert(f.cards.every(card => !card.hidden && card.querySelector().open));
  f.window.emit('afterprint');
  assert.equal(f.fields.query.value, 'NVIDIA');
  assert.deepEqual(f.cards.map(card => card.hidden), [false, true, true]);
  f.form.reset();
  assert.deepEqual(f.cards.map(card => card.querySelector().open), [false, true, false]);
});

test('a card whose choices show without a disclosure filters, deep-links and prints like the others', () => {
  const f = catalogueFixture(true);
  f.search('HDR');
  assert.deepEqual(f.cards.map(card => card.hidden), [true, true, false]);
  f.window.location.hash = '#setting-hdr';
  f.window.emit('hashchange');
  assert(f.cards.every(card => !card.hidden));
  f.window.emit('beforeprint');
  assert(f.cards.slice(0, 2).every(card => card.querySelector().open));
  f.window.emit('afterprint');
  f.form.reset();
  assert.equal(f.count.textContent, '3 settings.');
});
