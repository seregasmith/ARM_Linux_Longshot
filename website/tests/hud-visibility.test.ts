import test from 'node:test';
import assert from 'node:assert/strict';
import { readdir, readFile } from 'node:fs/promises';
import { MODULES_MARKER, readGraphicsSettings } from '../src/repo-docs/graphics-settings.ts';

const page = await readFile(new URL('../../docs/user/graphics-and-controls.md', import.meta.url), 'utf8');
const moduleDir = new URL('../../gpu/vk/menu/', import.meta.url);
const modules = await Promise.all((await readdir(moduleDir)).filter(name => name.endsWith('.cpp')).sort()
  .map(name => readFile(new URL(name, moduleDir), 'utf8')));
const menu = await readFile(new URL('../../gpu/vk/vk_menu.cpp', import.meta.url), 'utf8') + MODULES_MARKER + modules.join('\n');

test('custom HUD sliders and reset are published with their visibility condition', () => {
  const names = ['Hearts and magic horizontal', 'Hearts and magic vertical', 'Hearts and magic size',
    'Action and item buttons horizontal', 'Action and item buttons vertical', 'Action and item buttons size',
    'Minimap horizontal', 'Minimap vertical', 'Minimap size', 'Reset HUD placement'];
  const settings = readGraphicsSettings(page, menu).filter(group => group.tab === 'Graphics').flatMap(group => group.settings);
  assert.deepEqual(settings.filter(setting => setting.badges.includes('Custom HUD layout without a HOS_WIDE_UI override'))
    .map(setting => setting.name), names);
  const reset = settings.find(setting => setting.name === 'Reset HUD placement');
  assert.equal(reset?.defaultValue, '—');
  assert.equal(reset?.id, 'setting-reset-hud-placement');

  const undocumented = page.replace(/(\| \| Reset HUD placement \|[^\n]*?) \(shown with HUD layout set to Custom and no `HOS_WIDE_UI` override\)/, '$1');
  assert.notEqual(undocumented, page);
  assert.throws(() => readGraphicsSettings(undocumented, menu), /no condition for Reset HUD placement/);
  const unregistered = menu.replace('MENU_HOOK(reset_hud_placement)', 'MENU_HOOK(unknown_action)');
  assert.notEqual(unregistered, menu);
  assert.throws(() => readGraphicsSettings(page, unregistered), /coverage mismatch at Reset HUD placement/);
});
