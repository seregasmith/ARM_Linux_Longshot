// The pages published from the repository's own documents: the site path, then the file it is made from.
// Only these are published; other notes stay on Forgejo until they are added here.
export const contentPages: Record<string, string> = {
  'install': 'docs/user/getting-started.md',
  'install/steam-deck': 'docs/user/steam-deck-playbook.md',
  'requirements': 'docs/user/requirements.md',
  'settings': 'docs/user/graphics-and-controls.md',
  'settings/advanced': 'docs/user/advanced.md',
  'saves': 'docs/user/updates-and-saves.md',
  'saves/transfer': 'docs/user/saves.md',
  'mods': 'docs/user/using-mods.md',
  'help': 'docs/user/common-issues.md',
  'help/report': 'docs/user/reporting-and-contributing.md',
  'help/performance': 'docs/user/performance-report.md',
  'privacy': 'docs/user/privacy.md',
  'contributing': 'CONTRIBUTING.md',
};

// The other way round, for rewriting a document's links to other published documents. docs/README.md is the docs
// folder's index: a link to it goes to the site's front page.
export const pageForSource = new Map<string, string>([
  ...Object.entries(contentPages).map(([page, source]): [string, string] => [source, page]),
  ['docs/README.md', ''],
]);
