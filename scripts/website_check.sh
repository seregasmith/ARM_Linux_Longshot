#!/usr/bin/env bash
# The website's build and checks (website/: npm ci, the Astro build, check-site.ts and its node tests), as CI runs them on
# every change: `mise run ci` and .forgejo/workflows/ci.yml's website job call this; CircleCI's `website` job runs the
# same three commands. No secrets: the build only reads public Forgejo data.
set -euo pipefail
cd "$(dirname "$0")/../website"
if [ ! -f package.json ]; then
  echo "website_check: no website/ here, nothing to check"
  exit 0
fi
npm ci --no-audit --no-fund
npm run build   # (check-site.ts reads dist/)
npm run check
