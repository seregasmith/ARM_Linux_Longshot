# Longshot website preview

Astro/Starlight front page and player documentation for Longshot by Team MagicBean.
The game build is independent of this directory. Production deploys from main through
`.forgejo/workflows/website.yml`; pull-request previews require that workflow's review gate.

## Local preview

Use Node 22.23.4 or newer with the pinned lockfile. From `website/`:

```sh
npm ci
npm run build
npm run check
npx wrangler dev --local --ip 127.0.0.1 --port 4327
```

Open http://127.0.0.1:4327/. This serves both the static site and live download Worker.
`npm run dev` provides live editing; `npm run preview` serves static output only, so
neither supplies the Worker's download metadata. Project URLs and the canonical
`https://longshot.magicbean.dev` origin are defined in `src/site.ts`.

## Design and interaction

The front page has Guides, Help, Development, Team and About navigation, site search, light/dark
controls, and team credits. The main Grappling Hook Bean becomes the Glowing Fairy
Bean after three clicks, taps or keyboard button activations. Reload resets it.
A failed image load keeps the original artwork and allows a retry.
The hero artwork gently bobs over six seconds, matching the main MagicBean homepage.
Hovering or focusing the bean pauses it; reduced-motion preferences disable the sway.
The button and focus outline stay stationary, including after the fairy swap.

The homepage's bean punchline changes once per page load from ten short lines.
Session storage prevents an immediate repeat across reloads in the same tab when
available. Blocked storage still allows a random line; no JavaScript keeps the
original “Slightly emotional beans.” There is no timed rotation or animation.

At narrower widths splash pages use a Menu disclosure, including a no-JavaScript
fallback. Documentation pages use one Starlight navigation drawer, with Home and
Download launcher links alongside the guide links. With JavaScript, Escape, an outside click or choosing a link
closes it. Shared-memory GPUs have a separate checker choice; their memory remains
a caution rather than being compared to dedicated VRAM. GPU changes update the
automatic choice until the user chooses a memory type themselves; Reset restores it.

The team logo, Longshot icon and supplied bean illustrations are copied unchanged
from `../assets/branding/`; their provenance is recorded there. The upstream README's
own `../assets/longshot.png` is preserved separately. Astro generates responsive WebP
variants for the Longshot icons without changing their original files.
A panel below the hero offers a direct download link, compatibility guidance, the own-copy reminder and live launcher status;
four feature cards keep the next steps close. The progress board collapses secondary
filters on mobile and keeps the active filter summary visible when closed.

Body text and controls use self-hosted Atkinson Hyperlegible Next, weights 200–800,
upright and italic. Libertinus Serif Semibold remains for large headings; code stays
monospace. Libertinus is served as full-character WOFF2 files to reduce font downloads;
the original TTFs remain alongside them. `src/assets/fonts/libertinus/WOFF2-PROVENANCE.json`
records the encoder, source hashes and reproduction command. No encoder is required
for normal website builds. Font provenance and original notices are retained in
`src/assets/fonts/` and `public/fonts/`. No external font service or game-extracted
fonts are used.

## Team page

The team page (`/about/team/`) lists the people with the most commits in Longshot (Hookshot's history included),
collected from Forgejo when the site is built (`src/lib/contributors.ts`). There is nothing to add by hand: a merged
change puts you there at the next build.

## Content and source guards

`src/content-pages.ts` lists the pages made from the repository's own documents, each site path with the file it is
made from (`docs/user/*.md` and `CONTRIBUTING.md`); `src/repo-docs/` renders them. Edit those files, not the site: only
the listed pages are published, and their other relative links go to the Forgejo source tree.

The progress board and the front page's release and activity panels are collected from Forgejo when the site is
built; see `PROGRESS.md`.

Some pages get a presentation of their own, from their source text, keeping its anchors:

- **Install** (`docs/user/getting-started.md`): each section a card, with the launcher's channel picker.
- **Settings** (`docs/user/graphics-and-controls.md`): its table of every menu row (choices and defaults) becomes a
  card a setting, with the help, sections, order and restart flags from `gpu/vk/vk_menu.cpp`. A row missing, extra,
  out of order or without the condition the menu shows it under stops the build. Search and section filters enhance
  native disclosures; without JavaScript every card is there, and printing includes the whole catalogue.
- **Help** (`docs/user/common-issues.md`): a `##` group for each moment a problem shows up, each `###` problem ending in
  a line of report searches, then "Still stuck?". `format-common-issues.ts` adds presentation labels only; the old
  topic anchors open their cards.
- **Transfer a save** (`docs/user/saves.md`): wrapped for its screenshots.

## Validation and limits

Public pages use neutral game and console wording. Imported menu descriptions and activity text pass through
`src/lib/public-copy.ts` before publication; source records and links remain intact. `npm run check` rejects the
game-brand terms listed there (`GAME_BRANDING`) in generated page prose and the public progress feed. Players' issue
titles reach the feed as they are typed, so any listed term the wording rules don't cover becomes "in-game" rather
than blocking the deployment. Literal commands, required save
paths, technical product names and third-party attribution are preserved. Update public wording and its tests if
an upstream description introduces a new term; never rename a real file path or runtime setting to hide it.

`npm run check` verifies generated routes, links, anchors, edit destinations, artwork
bytes, search output, source guards and the website's test suite. These do not build
or launch the game.

Review rendered desktop/mobile layouts, keyboard access, both themes and live download
responses through the Worker preview. Record the tested browsers and screenshots in the
pull request; the build and unit tests do not establish visual or cross-browser correctness.

## Download links

`worker/index.ts` answers `/download` on longshot.magicbean.dev (everything else is the built site): permanent links to
the newest [Longshot Launcher](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher/releases), for READMEs, release
notes and chat. Forgejo's `/releases/latest` covers stable only; these cover each channel.

| Link | Goes to |
|---|---|
| `/download` | a page: the launcher for the visitor's system (by its User-Agent), from the best channel there is, and every channel |
| `/download/launcher/<os>` | that file of the best channel there is: stable, else nightly |
| `/download/launcher/<channel>` | the newest release page of `stable` or `nightly` (`rc`, the channel there was, goes to stable) |
| `/download/launcher/<channel>/<os>` | that file of the channel's newest release, such as `/download/launcher/stable/windows` |
| `/download/launcher/versions.json` | the available launcher versions and their exact file links, using the same channel rules and cached release list |

`<os>` is `windows` (`…-windows-x64-setup.exe`), `windows-portable` (`…-windows-x64-portable.zip`) or `linux`
(`…-x86_64.AppImage`). A link is a `302` to the file on git.magicbean.dev; a channel or file that isn't there is a `404`
page that says so and names the newest launcher there is. The channels follow the launcher's own rules
(`Core/ReleaseManifest.cs`, `scripts/release.py newest` in MagicBeanDev/Tooling), and the release list is kept 60 seconds.
`npx wrangler dev` (after `npm run build`) serves the site with the Worker; `tests/download.test.ts` checks the channel
rules.

The install page reads `versions.json` when opened and shows the selected channel's launcher version above its
download buttons. Once loaded, the buttons link to that exact release's files, so a later publication cannot change
the version being offered by an already-open page. Switching channels updates both the version and the links;
missing files are marked unavailable. Refresh the page to check for newer releases. If the lookup fails, the original
download routes and channel choices remain usable and the page suggests refreshing; without JavaScript it keeps the
original links and explains how to enable the version display. For this live behavior locally, build then run `npx wrangler dev`;
Astro's static preview alone does not serve the Worker endpoint.

The Worker fills the `/download/` Astro shell with release information, preserving the
shared header, navigation, search, themes and footer. If the shell cannot be loaded,
the standalone response remains available. Version metadata also supplies file formats
and sizes on the install page; desktop platform detection highlights a suggested button
without changing keyboard order. Unsupported/mobile user agents retain both platforms.
Launcher release notes and game/build release notes have separate labels.

Scrollable tables and code examples have named keyboard stops. With JavaScript, a ResizeObserver removes those stops when the content fits; table semantics and headers are preserved.
