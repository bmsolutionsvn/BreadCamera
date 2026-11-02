# Native video previews (BreadCamera + BreadReplay) — Design

Date: 2026-11-02. Status: draft for owner review.

## 1. Why

Every picture the UI shows today makes a round trip through the CPU and a codec:

- **BreadCamera** camera tiles (multiview, focused tile, calibration view): the engine forces
  the tick's readback for every watched camera (D5), `PreviewService` downscales on the CPU
  (swscale) and H.264-encodes one stream per tile (at most 4 encoders, D9 — tiles past the cap
  are blank with a warning). The page decodes each one with WebCodecs and draws it to a canvas.
- **BreadReplay** Preview and Output: render on D3D11, read back to the CPU, H.264-encode at
  960×540, send, decode in the page. The angle row sends raw BGRA (~2 MB per tile per update)
  over the WebSocket and the page draws it with `putImageData`.

Cost grows with every input: readbacks, CPU scaling, encoder sessions, socket traffic, decode,
canvas uploads. The 4-encoder cap already blanks tiles in BreadCamera. BreadReplay targets
esports, where 8+ angles is normal.

**Goal:** the engine presents every preview straight to the screen from its own D3D11 device.
No readback, no encode, no socket traffic, no decode, and no tile cap. The HTML UI keeps drawing
everything around and on top of the video: labels, tally, timecode, menus, dialogs.

## 2. Goals, non-goals, anti-goals

Goals
- Camera tiles (BreadCamera) show at camera rate (capped at 60 fps). Preview, Output and angle
  tiles (BreadReplay) show at its existing 33 ms preview-loop rate. No CPU readback or encode
  for previews in either app.
- No limit on the number of tiles other than GPU memory. BreadCamera's D9 cap no longer
  applies in native mode.
- HTML overlays (tags, timecodes, tally borders, menus, dialogs) draw on top of the video.
- Survives: shell restart and re-attach to a running engine, engine restart under the
  supervisor, window resize, maximize, minimize/restore, moving to a monitor with another DPI,
  and DPI changes.
- One shared implementation in BreadCamera's tree; BreadReplay builds it the same way it
  builds the other BreadCamera files.
- A fallback: when native video is unavailable (composition hosting fails, the engine's
  self-test fails, or the UI runs in a plain browser via `npm run dev`), the current H.264 /
  raw path keeps working unchanged.

Non-goals
- Remote or networked UI. The control API stays on 127.0.0.1.
- HDR or wide-gamut previews. Previews stay 8-bit sRGB. A float swap chain is a later option.
- Touch/pen input forwarding (WM_POINTER). Mouse, wheel and keyboard only. Add it when a touch
  panel is in use.
- File drag-and-drop into the page. Neither UI uses HTML drag events today. Composition hosting
  would need `ICoreWebView2CompositionController3` drag forwarding; add it when a UI starts
  using drag.
- Changing what the outputs (NDI, SRT, DeckLink, OMT, vcam, fullscreen) do. Only previews
  change.

Anti-goals (a failure even if it "works")
- Any engine tick or BreadReplay `previewLoop` step that blocks on presentation or vsync.
- Video drawn over a dialog, menu or context menu (the old "airspace" problem of child HWNDs).
- A shell crash or restart that stops an output, or that requires restarting the engine to
  get the pictures back.
- Blurry previews at 125–200 % display scaling.
- A broken title bar: drag, double-click maximize, Aero Snap, and the min/max/close buttons
  must work as they do today.
- Losing keyboard focus or hotkeys (BreadReplay is keyboard-driven).

## 3. Shape of the solution

```
 shell window (HWND, per-monitor-v2 DPI aware, background brush = page base colour)
 └─ DirectComposition target (topmost) ── root visual
      ├─ video visual "cam:A"   content = composition surface ← engine swap chain
      ├─ video visual "preview" content = composition surface ← engine swap chain
      ├─ …
      └─ WebView2 visual (composition hosting, transparent default background)  ← on top
```

1. **The shell hosts WebView2 in composition mode** (`CreateCoreWebView2CompositionController`)
   with a DirectComposition visual tree. The WebView visual is the top child of the root. Its
   default background is fully transparent.
2. **The engine owns the video.** For each preview the page wants, the engine creates a
   DirectComposition surface handle (`DCompositionCreateSurfaceHandle`) and a flip-model swap
   chain on it (`IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle`) on its single
   D3D11 device, and presents frames into it.
3. **The page decides what and where.** A `NativeVideo` box in React measures itself and tells:
   - the engine (over the existing control WebSocket) which surfaces it wants and their size in
     device pixels;
   - the shell (over `chrome.webview.postMessage`) where each one goes, with the handle value
     and engine pid that the engine returned.
4. **The shell places the video under the page.** It duplicates each handle out of the engine
   process (`DuplicateHandle`, source = the engine it supervises), wraps it with
   `IDCompositionDesktopDevice::CreateSurfaceFromHandle`, sets it as a video visual's content,
   offsets and clips the visual to the box, and commits. Per frame, the shell does nothing. DWM
   composes the engine's latest present under the page.
5. Where the page is transparent the video shows through. Where the page paints (labels,
   dialogs, menus) it covers the video. The window class background brush paints the base
   colour under everything, so transparent page areas without video look the same as today.

Why this and not the alternatives:

| Option | Verdict |
|---|---|
| Composition surface handle per preview, under a transparent WebView (chosen) | Zero copy, the shell does no per-frame work, overlays stay on top, and it survives shell restarts because the engine keeps the GPU work. |
| Engine shares a D3D11 texture (NT handle); the shell owns the swap chains and copies each frame | Proven cross-process mechanism, but the shell needs a D3D device, a render thread and per-frame signalling. **This is the plan B** if the §8 spike fails. |
| Child HWNDs (or engine-owned windows) over the WebView | Airspace: they cover menus and dialogs. Rejected. |
| WebView2 texture-stream / shared-buffer APIs | Experimental, or still a CPU copy into JS. Rejected. |
| Keep H.264 but optimise (shared memory instead of the socket) | Keeps encode and decode, which are the cost. Rejected as primary; kept as the fallback. |

## 4. Engine side (shared code in BreadCamera)

### 4.1 `bc::SurfacePresenter` — `src/gpu/surface_presenter.{h,cpp}` (in `bc_gpu`)

One preview's composition surface and swap chain.

- Creates the surface handle (`COMPOSITIONOBJECT_ALL_ACCESS`, default security) and a swap chain
  on it with `DXGI_FORMAT_R8G8B8A8_UNORM`, `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`, 2 buffers,
  `DXGI_ALPHA_MODE_IGNORE`, and size = the requested device-pixel size.
- `HANDLE handle() const` returns the engine-process handle value. The presenter owns it and
  closes it when destroyed.
- `void resize(w, h)`: stores the newest size. The next `present` calls `ResizeBuffers` once if
  the size changed. Sizes are clamped to 16..4096.
- `bool present(const GpuTexture& linear)`: under `GpuDevice::lock()`, letterboxes the linear
  RGBA16F picture into the buffer size (`Converter::fit`), converts it with
  `Converter::toBgra8(fit, w, h, /*swapRb=*/false)` (true RGBA order, so it matches the
  R8G8B8A8 back buffer and `CopyResource` is legal — the BGRA/RGBA trap that bit the angle row
  must not come back), copies it into the back buffer, then `Present(0, DXGI_PRESENT_DO_NOT_WAIT)`.
  `DXGI_ERROR_WAS_STILL_DRAWING` drops the frame and returns false. It never waits for vsync.
- Device removed or another present failure: the presenter marks itself broken and stops
  presenting. The owner (`SurfaceHub`) reports it through state (§4.3). It does not throw into
  the tick.
- **Self-test:** `static bool SurfacePresenter::available(GpuDevice&)` creates one 16×16
  presenter and presents once. The engines run it at start-up and publish the result.

### 4.2 `bc::SurfaceHub` — `src/engine/surface_hub.{h,cpp}` (compiled by path into BreadReplay, like `preview_hub.cpp`)

Which client wants which surface, at which size. It mirrors `PreviewHub`.

- `subscribe(client, [{id, w, h}])` replaces that client's whole set. It creates presenters for
  new ids, resizes kept ones, and destroys the ones no longer listed. It returns
  `[{id, handle, w, h}]`, with the handle as a decimal string of the engine-process value.
- `dropClient(client)` runs on WebSocket close and destroys that client's presenters.
- Surfaces are per (client, id), so a new shell's page simply gets new surfaces. Nothing has to
  persist across a shell restart, and a stale page's surfaces die with its socket.
- `wanted()` gives the tick loops the set of ids to present. `present(id, linear)` presents to
  every presenter for that id. Present runs on the thread that already renders the picture,
  which is cheap (two dispatches and a copy, never a wait).
- Threads: `subscribe`/`dropClient` run on the control-server thread, `present` on the tick or
  preview-loop thread. One hub mutex guards the map. `present` holds it while presenting, so a
  presenter is never destroyed mid-present. Lock order: hub mutex, then `GpuDevice::lock()`.
  Presenters are created and destroyed under the GPU lock too.
- Build: `bc_gpu` links `dcomp.lib` (and `dxgi.lib` if it does not already).

### 4.3 Control API (both engines)

- `subscribeSurfaces {surfaces: [{id, w, h}]}` → `{ok, pid, surfaces: [{id, handle, w, h}]}`.
  `w`/`h` are device pixels of the box. An empty list means nothing is visible.
- State `/engine/nativeVideo`: `true` when the self-test passed, otherwise the reason string.
- State `/engine/warnings/nativeVideo`: set while any presenter is broken (device removed and
  so on), cleared when none is.
- Ids: BreadCamera `cam:<cameraId>`. BreadReplay `preview`, `output`, `angle:<inputId>`.

### 4.4 BreadCamera wiring

- The frame tap presents `FrameTap.tex` (the camera's processed linear picture; the builder
  confirms it is linear RGBA16F) for every wanted `cam:` id.
- In native mode `setPreviewWanted` stops forcing the D5 readback for a camera that only has a
  surface subscription. The readback still runs when an output needs CPU pixels.
- `PreviewService`, `PreviewHub`, `subscribePreview` and `requestKeyframe` stay for the fallback.

### 4.5 BreadReplay wiring (`previewLoop`)

- Preview: the linear picture from `previewRenderer_->render(...)` is presented to `preview`
  subscribers. `toCpu` and the encoder run only when an H.264 subscriber exists.
- Output: `outputPlayer_->lastPicture()` is presented to `output`.
- Angles: `source_->preview(id, at)` stays a CPU BGRA image (decoded preview). It is uploaded
  with `Converter::uploadBgra8` → `toLinear(…, PixelLayout::BGRA8, true)` → presented to
  `angle:<id>`. Uploading the CPU image keeps this a small change. Decoding to the GPU is a
  later option. The raw `BRAP` socket message is sent only to clients without native video.
- The H.264 encoders (`previewEncoder_`, `outputEncoder_`) open lazily, only when a fallback
  client subscribes.
- The 33 ms loop stays. The angle row moves from every 3rd tick to every tick when native.

## 5. Shell side (shared code in BreadCamera)

### 5.1 `bc::CompositionHost` — `src/shell/composition_host.{h,cpp}` (in `bc_shell`; compiled by path into BreadReplay's shell)

Owns the DirectComposition tree and the WebView2 composition controller for one HWND. Both
shells replace their `CreateCoreWebView2Controller` call with it and fall back to the current
windowed controller when it fails.

- **Create:** `DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(IDCompositionDesktopDevice))`,
  `CreateTargetForHwnd(hwnd, TRUE)`, a root visual, a WebView visual added last (top).
  `ICoreWebView2Environment3::CreateCoreWebView2CompositionController`, `put_RootVisualTarget`.
  The `ICoreWebView2Controller` the shells already use is queried from it, so bounds,
  settings, the virtual host mapping and message handlers stay as they are.
  `ICoreWebView2Controller2::put_DefaultBackgroundColor({0,0,0,0})`.
- **Input:** forward `WM_MOUSEMOVE`, the button down/up/double-click messages (L/M/R/X),
  `WM_MOUSEWHEEL` and `WM_MOUSEHWHEEL` (wheel coordinates are screen coordinates; convert them)
  with `SendMouseInput`. Use `TrackMouseEvent` plus `COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE` on
  `WM_MOUSELEAVE`. `SetCapture` on button down and `ReleaseCapture` on the last button up, so
  timeline scrubs and slider drags keep working outside the window.
- **Cursor:** `add_CursorChanged`, then answer `WM_SETCURSOR` in the client area with
  `get_Cursor`.
- **Title bar:** keep `IsNonClientRegionSupportEnabled`. Answer `WM_NCHITTEST` for points inside
  the client area with `ICoreWebView2CompositionController4::GetNonClientRegionAtPoint`:
  `CAPTION` → `HTCAPTION`, otherwise `HTCLIENT`. The resize borders keep their current
  `DefWindowProc` handling.
- **Focus and keyboard:** `MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC)` on
  `WM_SETFOCUS` and on activation. Keyboard input then goes to WebView2 directly.
- **Accessibility:** answer `WM_GETOBJECT` (`UiaRootObjectId`) with
  `ICoreWebView2CompositionController2::get_AutomationProvider` via `UiaReturnRawElementProvider`.
- **Visibility:** on minimize, `put_IsVisible(FALSE)`, so the page's `visibilitychange` fires
  and it drops its surfaces. On restore, `put_IsVisible(TRUE)`.
- **Video layers:** `setLayers(json, DWORD enginePid, HANDLE engineProcess)` takes the page's
  message `{type:"videoLayers", pid, layers:[{id, handle, x, y, w, h}]}` (device pixels,
  relative to the client area):
  - Ignore the whole message if `pid` is not the engine the shell supervises right now.
  - New (id, handle): `DuplicateHandle(engineProcess, handle, self, …, DUPLICATE_SAME_ACCESS)`,
    `CreateSurfaceFromHandle`, a new visual inserted below the WebView visual, then close the
    duplicated handle (the surface object keeps the reference). On any failure, skip that layer
    and post `{type:"videoLayerFailed", id}` back to the page.
  - Every listed layer: `SetOffsetX/Y`, `SetClip` to `w×h`.
  - Ids not listed: remove the visual.
  - At most 64 layers. `Commit()` once per message.
- **Engine restart:** the old visuals stay (frozen or empty) until the page reconnects,
  resubscribes and sends layers with the new pid, which replace them all. A message whose pid
  is not the current engine's is ignored (the first bullet), so stale handles are never
  duplicated from the wrong process.
- **DPI:** on `WM_DPICHANGED`, set `ICoreWebView2Controller3::put_RasterizationScale(dpi / 96.0)`
  and update bounds. The builder checks that the page's `devicePixelRatio` then matches the
  monitor scale.
- Build: `bc_shell` (and BreadReplay's shell target) link `dcomp.lib` and
  `uiautomationcore.lib`.

### 5.2 Shell changes outside `CompositionHost`

- **Per-monitor-v2 DPI awareness:** `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
  first thing in `wWinMain`. The initial 1600×940 window size is scaled by `GetDpiForSystem()/96`.
  `WM_DPICHANGED` → `SetWindowPos` with the suggested rect. Today both shells are DPI-unaware
  and Windows bitmap-stretches them; this is required for sharp video and also sharpens the
  windowed fallback.
- `ProcessLauncher::adopt` opens the engine with `PROCESS_DUP_HANDLE` added. A launched engine's
  handle already has full access. `CompositionHost` gets the process handle from the launcher.
- The window class background brush becomes the page's base colour (BreadCamera and BreadReplay
  `--bm-bg`), not `BLACK_BRUSH`. With a topmost DirectComposition target, the window's own
  background shows wherever no visual paints.
- The page URL gets `&nativeVideo=1` when `CompositionHost` started. The page reads it at load.

## 6. Page side (each app's `ui/`)

### 6.1 `NativeVideo` component (one small copy per app; they are separate packages)

`<NativeVideo id="cam:A" fallback={<PreviewCanvas …/>} />`

- **Native mode** = the URL has `nativeVideo=1` **and** state `/engine/nativeVideo === true`.
  Otherwise it renders `fallback` and nothing else changes.
- In native mode it renders an empty, transparent, `aria-hidden` box. A shared registry
  (module-level) collects every mounted box. A `ResizeObserver` per box, window `resize`, and a
  `matchMedia('(resolution: …dppx)')` listener for DPI changes all mark the registry dirty. On
  the next animation frame it:
  1. measures every box (`getBoundingClientRect` × `devicePixelRatio`, rounded; zero-size or
     `display:none` boxes are left out);
  2. sends `subscribeSurfaces` with the full list (only when the set of ids or any size
     changed);
  3. on the reply, posts `{type:"videoLayers", pid, layers}` to the shell (also re-posted when
     only positions changed, for example on scroll or layout shifts, with no engine round trip).
- `visibilitychange` to hidden sends an empty subscription. Back to visible resends it.
- A WebSocket reconnect (engine restart) resends the subscription. A new pid flows through to
  the shell.
- `videoLayerFailed` from the shell, or a broken-presenter warning, makes that box show the
  same "no picture" placeholder the tile shows today (not the H.264 fallback per tile — mixed
  modes are not worth the code).

### 6.2 Styling rule: ancestors of a video box must be transparent

The video is under the page, so any ancestor of a box that paints a background covers it.

- `html.native-video` (class set at load in native mode) drops the opaque `html`/`body`/app-root
  backgrounds. The shell's window brush paints the same base colour underneath. In fallback
  mode the opaque backgrounds stay, and the windowed controller keeps an opaque default
  background colour.
- Containers between the root and each box (`.center`, `.mons`, `.mon`, `.angle-row`,
  `.angle`, multiview grid and tile, calibration view) must not paint a background behind the
  box. Where a container needs a colour around the video, it moves to a sibling, a border or a
  pseudo-element that does not overlap the box.
- Letterbox bars come from the presenter (it fills its whole buffer), so the `.mon` `#000`
  background is not needed in native mode.
- In dev builds, `NativeVideo` walks its ancestors once after mount and `console.warn`s for any
  with a non-transparent computed background. This is a cheap guard against a future CSS change
  silently hiding a preview.

### 6.3 Where it is used

- BreadCamera: `Multiview` tiles and the focused tile (`cam:<id>`), `CalibrationView` (`cam:<id>`,
  larger box). The colour-match view uses whichever component it renders today.
- BreadReplay: `Monitors` (`preview`, `output`), `AngleRow` (`angle:<inputId>`).

## 7. Failure and lifecycle behaviour

| Event | What happens |
|---|---|
| Shell closes or crashes | Engine keeps running and outputs are unaffected. The socket closes, `dropClient` destroys that client's presenters. |
| Shell restarts and re-attaches | New page, new subscription, new surfaces, new layers. The video is back in well under a second. |
| Engine crashes or restarts | Video visuals freeze on the last frame until the shell sees the new pid and drops them. The page reconnects, resubscribes and posts new layers. |
| Resize / maximize | Positions update on the next animation frame. The swap chain resizes on the next present. Until then, the clip hides overflow and the base colour fills any gap. |
| Minimize | `IsVisible(FALSE)` → page hidden → empty subscription → no presenting. Restore resubscribes. |
| DPI change / move to another monitor | `WM_DPICHANGED` resizes the window. `devicePixelRatio` changes and the page re-measures. |
| Composition controller or DirectComposition creation fails | Windowed controller, no `nativeVideo=1`, the current H.264 path. |
| Engine self-test fails (for example a WARP device that cannot create the swap chain) | `/engine/nativeVideo` = reason, and the page uses the fallback. |
| Device removed mid-show | The presenter goes broken, the warning shows, and the tile shows "no picture". Outputs follow their existing device-loss handling. |

## 8. Risk and the spike that gates the work

**Unverified:** Microsoft's docs do not say outright that a `DCompositionCreateSurfaceHandle`
handle can be duplicated into another process and bound there with `CreateSurfaceFromHandle`.
The API takes security attributes and inheritable handles, which points to cross-process use,
but nobody has proven it here.

**Spike (plan task 1, owner-visible):** a throwaway two-mode tool. `probe serve` creates a
surface plus swap chain and presents a moving colour bar, then prints `pid handle`.
`probe show <pid> <handle>` opens a window, duplicates the handle and binds it to a visual.
Run it on the RX 6600, and on the RTX 5070 when available. It passes when the bar moves in the
second process's window. If it fails, stop: the plan switches to plan B (§3 table: shared NT
texture, with the shell owning the swap chains) and the owner is told before more work starts.

## 9. Verification

- Unit (doctest, BreadCamera `bc_tests`): `SurfacePresenter` creates a non-null handle,
  presents a solid colour without error, and after `resize` + present the swap chain's
  `GetDesc1` reports the new size (hardware, or WARP when that is all there is; skip with the
  existing requirement skips when neither can create the swap chain). The pixels on screen are
  checked by the spike and the owner, not by unit tests: a flip-model back buffer cannot be
  read back after present. `SurfaceHub`:
  subscribe, replace, resize, drop client, and two clients on the same id.
- UI (vitest): `NativeVideo` renders the fallback without the flag, and in native mode sends one
  `subscribeSurfaces` per change and posts layers with the reply pid.
- Owner acceptance (both machines where possible), BreadCamera first, then BreadReplay:
  1. Previews show, are sharp at 100 % and 150 %, and move with window resize/maximize without
     lag you can see.
  2. 8 cameras in multiview, none blank (the D9 cap is gone). Task Manager CPU and GPU compared
     with the H.264 build on the same show.
  3. Menus, the custom right-click menus and dialogs draw over the video.
  4. Title bar drag, double-click maximize, Snap, min/max/close; BreadReplay hotkeys; timeline
     wheel and a scrub dragged outside the window.
  5. Kill the shell → outputs continue → relaunch → previews return. Kill the engine →
     supervisor restarts it → previews return.
  6. Minimize/restore. Drag the window to a monitor with another scale.
  7. Narrator reads the page (automation provider works).
  8. `npm run dev` in a browser still shows H.264 previews.

## 10. Order

1. Spike (§8). Gate.
2. BreadCamera: `SurfacePresenter` + `SurfaceHub` + control API + frame-tap wiring.
3. BreadCamera: `CompositionHost` + shell changes (DPI, launcher rights, brush, flag).
4. BreadCamera UI: `NativeVideo` + transparency pass. Owner acceptance for BreadCamera.
5. BreadReplay: engine wiring (`previewLoop`), shell adoption, UI adoption. This starts only
   after the parallel BreadReplay UX work (live-edge Preview, angle-tile colours, timeline
   labels, custom context menus in `ui/` and `src/engine/engine.cpp`) has merged. Owner
   acceptance for BreadReplay.

## 11. Calls made in this design (owner to check)

- Kept the H.264 / raw path as the fallback, also used for browser dev (`npm run dev`), instead
  of deleting it.
- The page relays the handle and pid from engine to shell. The shell trusts only its supervised
  engine pid and checks the object by binding it. No extra shell↔engine protocol.
- Surfaces are per client and die with the socket. Nothing persists across a shell restart, by
  design.
- Swap-chain buffers match the box's device-pixel size, and the engine letterboxes. The shell
  never scales.
- BreadReplay's Preview renderer stays at 960×540. Raising it to the show raster is a one-constant
  follow-up once native previews make it free.
- BreadCamera previews run at camera rate capped at 60 fps (today: 30). BreadReplay keeps its
  33 ms loop.
- 8-bit sRGB only. Mouse and keyboard only, no touch. No drag-and-drop forwarding.
- Whole-page fallback, not per tile.
