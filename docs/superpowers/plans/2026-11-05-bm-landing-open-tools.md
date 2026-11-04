# bm-landing: Show the Published Open Tools — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** bmsolutions.vn shows BM Solutions' published open-source tools: a new `/tools` page (vi + `/en/tools`), a teaser section on the home page, and a nav link.

**Why:** The owner published these repos on GitHub (all under `https://github.com/bmsolutionsvn/`) and wants the site to show them. Today the site shows only BreadStream, BreadCall and BreadOverlay.

**Tech Stack:** Astro (static), TypeScript, vitest. Repo `F:\Coding\BreadMedia\bm-landing`; its default branch is **`master`**.

## The tools (facts for the copy; do not claim anything beyond them)

| Tool | Repo | What it is (from its GitHub description) | Platform | Licence |
|---|---|---|---|---|
| BreadCamera | `BreadCamera` | Multi-camera GPU processing for live production: AI background removal without a green screen, background blur or replacement, colour-chart camera matching; NDI, OMT, SRT and SDI key+fill outputs | Windows | MIT |
| BreadReplay | `BreadReplay` | Instant replay: records every camera to disk on one shared timecode, cuts a replay of any moment on any angle at any speed, plays it out over NDI, OMT, SRT or SDI | Windows | MIT |
| OMT-HX | `OMT-HX` | Open Media Transport High Efficiency: GPU hardware-encoded HEVC/H.264 inside OMT, as NDI\|HX is for NDI | Windows | MIT |
| OMT-Bridge | `OMT-Bridge` | Takes any mix of OMT, OMT-HX and NDI sources and republishes them as any mix of OMT, OMT-HX and NDI outputs, converting between them, with audio, alpha and tally | Windows | MIT |
| OMT-HX for OBS | `omtplugin` | **Fork** of `openmediatransport/omtplugin` (the Open Media Transport plugin for OBS: send and receive OMT), with OMT-HX support added | OBS | MIT |
| Open Camera OMT-HX | `opencamera-omthx` | **Fork** of `ysdede/opencamera-omt` (Open Camera for Android with OMT streaming), with OMT-HX added | Android | (no licence chip) |
| OMT Community Tools | `omt-community-tools` | **Fork** of `MikanseiLaboratory/omt-community-tools`, with OMT-HX support added | (read its README) | (no licence chip) |

The three forks must say they are forks, and name and link the upstream project (its GitHub URL, added to `config.ts`) on the card, for example "Bản fork của … thêm OMT-HX" / "A fork of … with OMT-HX added". Never present a fork as BM Solutions' own product. Read each repo's README with `gh repo view bmsolutionsvn/<repo>` (read-only) when a line needs more than the table. Do not list BreadStudio, libomt, libomtnet or libvmx.

## Global Constraints

- Follow the repo's `new-page` skill (`.claude/skills/new-page/SKILL.md`) exactly: a shared `src/components/pages/ToolsPage.astro`, thin `src/pages/tools.astro` and `src/pages/en/tools.astro` wrappers, all copy in `src/i18n/vi.ts` (source of truth, Vietnamese) and the same key paths in `en.ts`. No `–` or `—` in any string.
- Every external URL goes in `src/config.ts` (one constant per repo URL, plus the org URL `https://github.com/bmsolutionsvn`). Never inline a URL.
- Reuse the site's existing classes and components (`card`, `grid-3`, `section`, `section-head`, `chip`, `link-more`, `btn`). No new design tokens, no new dependencies, no images that need fetching.
- Status chip on each tool: "MÃ NGUỒN MỞ" / "OPEN SOURCE". The licence and platform go in the card's mono `figure` line (for example `MIT · Windows`).
- Each card links to its GitHub repo ("Xem trên GitHub" / "View on GitHub"), opening in the same tab.
- Group the cards: **Production apps** (BreadCamera, BreadReplay) and **OMT-HX family** (OMT-HX, OMT-Bridge, OMT-HX for OBS, Open Camera OMT-HX, OMT Community Tools).
- Home page: a new section after the products grid, "Công cụ mã nguồn mở" / "Open-source tools", showing the two production apps and one OMT-HX card. Each links to `/tools`; under them goes a "Xem tất cả công cụ" / "See all tools" link. Leave the hero, products and final CTA unchanged.
- Nav: add "Công cụ" / "Tools" to `Header.astro` (desktop and mobile menus) and the footer, if the footer lists pages. Add `nav.tools` to both dictionaries.
- Update the tests that count pages (for example the sitemap `<url>` count in `tests/meta.test.mjs`) to the new true count. Do not weaken any other assertion.
- Pushing `master` deploys the live site: **never push**. The reviewer pushes after review.

## Review Focus

1. vi/en parity: the i18n parity test passes, and the en copy says the same as the vi copy.
2. Every claim on a card is in the table above or the repo's README.
3. `/tools` and `/en/tools` have a unique title, a description, a canonical URL and hreflang (meta test).
4. Mobile: at 375 px the cards stack and the nav menu shows the new link.

---

### Task 1: The `/tools` page, home section and nav

**Files:** Create `src/components/pages/ToolsPage.astro`, `src/pages/tools.astro`, `src/pages/en/tools.astro`. Modify `src/i18n/vi.ts`, `src/i18n/en.ts`, `src/config.ts`, `src/components/pages/HomePage.astro`, `src/components/Header.astro` (and `Footer.astro` if it lists pages), `tests/meta.test.mjs` (page count only).

- [ ] **Step 1: Failing tests:** add `/tools` and `/en/tools` to whatever list the meta test checks, and raise the sitemap count. Run `npx vitest run`: it fails, because there's no page yet.
- [ ] **Step 2: Implement** the page, home section, nav, config URLs and both dictionaries.
- [ ] **Step 3: Verify:** `npm run typecheck`, `npm run build`, `npx vitest run`, `npm run lint`, `npm run format:check`, all green.
- [ ] **Step 4: Check the built pages:** in `dist/`, `tools/index.html` and `en/tools/index.html` exist, each card's link is the right GitHub URL, and no string contains `–` or `—`.
- [ ] **Step 5: Commit on `bml-open-tools` and merge into `master`** (`feat(site): show the open-source tools`, `merge: open-source tools page`). Do not push.
