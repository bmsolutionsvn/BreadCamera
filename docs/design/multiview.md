# Multiview — design brief

Approved 2026-10-08. Product record: `PRODUCT.md`. Visual system: the ecosystem
"Cyan Blueprint" in `../DESIGN.md` (token names below are its role names).
UX rules: `../bm-design/docs/UX-CONTRACT.md`. This brief adds no new identity;
it applies that system to one screen.

## 1. Job and audience

The operator on the production PC. Before the show (Operate mode) they check
that every camera arrived, has its effects on, and is sending. During the show
Multiview sits on a side monitor and is glanced at from across the desk.

## 2. Outcome

One look answers: is every camera's picture there, is every effect running, is
every output sending, and if not, what is wrong. Success: nothing wrong goes
unnoticed, and nothing on this screen can change a live output.

## 3. Direction: the multiviewer wall

Pictures fill the work area edge to edge, like a hardware broadcast
multiviewer. The signature is the **state strip** under each picture (a
broadcast under-monitor display): a hairline-bounded mono band that is
**always exactly one row** and reads left to right like the signal path:

```
[SRT] CAM 1 · Sân khấu trái       ◐ ◌ ◑  →  (NDI) (SRT) (SDI) +1   ↺ 20:14
 in    name (ellipsis)            effects     outputs              mark
```

- **Input chip** (left): a solid, filled 4px chip in its signal-state colour
  with a short code — `SRT`, `NDI`, `SDI`, `USB`, `AJA`.
- **Name**: one line, cut with an ellipsis; the full name is in the tooltip.
  The name never wraps and never pushes the outputs off the row.
- **Effects**: three small icons (Matte, Background/Blur, Color), no text.
- **→** then **output pills**: outlined pills (state-coloured dot and border,
  no fill) with short codes — `NDI`, `SRT`, `SDI`, `VCAM`, `FS`, `OMT`.
  More than fit: `+N`.
- In vs out is told apart at a glance by **filled chip on the left vs outlined
  pills after the arrow**. The colour of each still means its state.
- Every code and icon carries its full Vietnamese name in a tooltip and
  `aria-label` (e.g. `SRT` → "Đầu vào SRT — Đang phát").
- Long messages never go on the strip: the strip shows the short form (input
  chip reads `00:04` while holding a frame, `SLATE` once the slate is up) and
  the full sentence is in the tooltip and on Camera detail.

A state that changes keeps a mark on its strip until the operator opens that
camera, so faults that recovered on their own (e.g. "SRT dropped 3 s at 20:14")
are still seen.

## 4. Scope and boundaries

- One production-ready screen. A tile click opens Camera detail; nothing else
  on a tile is interactive.
- Top bar: show name, frame rate, GPU load, and a tick-overrun counter shown
  only when above 0. Primary action top right: **Thêm camera**.
- Anti-goals: controls on tiles; toasts for errors; automatic reordering; red
  tally borders (BreadCamera does not know what the switcher has on air, and red
  already means "Đang phát"); glow or gradient.

## 5. States and ranges

| Cameras | Grid |
|---|---|
| 0 | Empty state offering Thêm camera |
| 1 | One tile fills the work area |
| 2 | 2×1 |
| 3–4 | 2×2 (typical) |
| 5–6 | 3×2 |
| 7–8 | 4×2 |

- Input: Sẵn sàng · Đang kết nối · Đang phát · Tắt.
- Input lost: last frame held with a countdown on the strip ("Mất tín hiệu
  00:04"), then the slate.
- Stage bypassed: that effect badge turns to the error band.
- Output failing: that output's pill turns to the error band with its retry
  state; other pills are unaffected.
- Engine restarting: a block at the top of the work area; tiles freeze rather
  than blank.
- A frame with alpha shows over a dark checkerboard.
- More outputs than the strip holds: "+N".

## 6. Interaction and layout

Desktop only; tiles keep 16:9 at every grid size; strip text stays legible at
4×2. Arrow keys move between tiles, Enter opens one. Previews ~480×270 at 30 fps
per tile, streamed only while visible, and stopped while the window is hidden to
the tray.

## 7. Colour

The rule for this screen: **the chrome is colourless so the pictures and the
exceptions are the only colour.** Operators judge skin tone and colour match on
these pictures, so nothing tinted sits on or around them.

### Where each colour goes

| Element | Normal | Token(s) |
|---|---|---|
| Page behind the wall | Plate | `plate-black` |
| Rail | Rail, active item cyan wash | `rail-black`, `blueprint-cyan-wash` + `blueprint-cyan` |
| Top bar | Surface, hairline below; mono numbers in muted text | `surface`, `hairline`, `text-muted` |
| Thêm camera | The one cyan fill on the screen | `blueprint-cyan`, `ink-on-cyan` |
| Gaps between tiles | 2px of plate, no borders, so the pictures read as one wall | `plate-black` |
| Picture | Untouched video. No overlay, tint, dim or vignette in any state | — |
| Alpha checkerboard | 8px squares of two *neutral* greys (no blue cast), so a keyed edge reads true | `#141414` / `#1F1F1F` (BreadCamera-private, video only) |
| State strip | Surface, hairline top edge; name in paper white, mono labels in muted | `surface`, `hairline`, `paper-white`, `text-muted` |
| Strip on hover | One surface step up | `surface-hover` |
| Keyboard focus | 1px cyan border round the whole tile; the only cyan inside the wall | `blueprint-cyan` |
| Effect icons | Muted icons, no chip. Not cyan: an effect being on is normal, not an accent | `text-muted` |
| Input chip | **Filled**: solid state colour, dark ink text, 4px corners | signal band solid + `ink-on-cyan`-style dark ink (`#04191D`) |
| Output pills | **Outlined**: state-colour 1px border and dot, state-text label, no fill | signal band `*-text` |
| Offline (Tắt) | Input chip: `signal-offline` fill with paper-white text; output: grey outline | `signal-offline` |

### Exceptions (the only colour that should catch the eye)

| Situation | Treatment | Band |
|---|---|---|
| Output sending | Red pill, pulsing dot. **All Live dots share one clock**, so a healthy wall breathes in unison; any pill out of step or out of red is the exception | `signal-live` / `signal-live-text` |
| Input or output reconnecting | Amber pill | `signal-connecting` / `-text` |
| Input lost, holding last frame | Strip gets a Warning left border on its 8% wash; input chip turns amber and reads `00:04` counting down. Tooltip: "Mất tín hiệu · đang giữ khung hình cuối" | `system-warning`, `signal-connecting` |
| Slate showing (hold ran out) | Strip left border and wash in Danger; input chip reads `SLATE` | `system-danger` |
| Stage bypassed | That effect icon turns rose | `system-danger` |
| Output failed | That output: rose error chip (4px, not a pill) with retry text; never red | `system-danger` |
| Unseen past change | Small sky mark with the time on the strip's right ("↺ 20:14") until the camera is opened | `system-info` |
| Tick overruns > 0 | Counter in the top bar in Warning text | `system-warning` |
| Engine restarting | Warning block across the top of the work area; pills drop to Offline grey | `system-warning`, `signal-offline` |
| Engine restart failed | Same block in Danger, kept until resolved | `system-danger` |

### Checks

- Two Bands Rule holds: broadcast state is always a pill in the signal band;
  anything wrong with BreadCamera is a left-border block or 4px chip in the
  system band. Errors never use On-Air Red.
- Colour is never the only carrier: every chip and pill has its code, in vs
  out differ by fill and position, and error vs. state differ in shape — safe
  for red/green colour blindness.
- The filled input chip is the one place a signal colour is used as a fill;
  dark ink on the solid signal colours is ≥4.5:1 (red, amber and green solids
  all clear it with `#04191D`).
- The strip is one row at the 4×2 tile width (~440px): input chip ~44px, name
  takes the rest down to ~96px, three outputs ~150px, `+N` beyond that.
- Contrast: strip text and all `*-text` signal variants are ≥4.5:1 on
  `surface`; system colours on their 8% wash are ≥4.5:1.
- Under reduced motion the shared Live pulse becomes a static solid dot.

## 8. Open decisions (do not guess)

- The Vietnamese term for Show.
- Whether camera order can be changed, and where.
- Exact Vietnamese wording of the slate and engine-restart messages.
