# myFrame site

Static GitHub Pages site — no build step.

- `index.html` — landing
- `flasher/` — Web Serial flasher (esp-web-tools, per-board)
- `docs/` — docs viewer (renders `../../docs/*.md` via marked.js)
- `assets/boards.json` — generated in CI from `devices/index.json`

## Enable Pages (one-time, owner)

Settings → Pages → **Build and deployment → Source: GitHub Actions**

Then `pages.yml` deploys on every push to `main`.

Local preview: `python -m http.server --directory site 8000`
