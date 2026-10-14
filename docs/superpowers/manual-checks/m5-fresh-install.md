# M5 fresh-install check (owner, spec §9 M5 "a clean install works on a fresh Windows 11 machine")

1. On a Windows 11 PC that never had BreadCamera or Visual Studio: run `BreadCamera-Setup-<v>.exe`.
2. The app starts: the dark custom title bar with menus, the empty Multiview ("No cameras yet"), the Inspector, and the GPU name in the status bar. Drag the window by its title bar; minimise, maximise and restore it.
3. Add an SRT listener camera named "Sân khấu trái" (Unicode); push a stream from OBS (`srt://<pc>:9000?mode=caller`) → the input chip turns green within 5 s, preview moving, the name shown unchanged.
4. Add an NDI output (if the NDI runtime is installed) and see it in vMix/OBS; add a Windows virtual camera output and see it in the Camera app with the camera's name.
5. Close the window → outputs keep running (tray icon). Tray → "Quit…" → the dialog's "Quit and stop all outputs" stops everything.
6. Uninstall from Settings → Apps; `%APPDATA%\BreadCamera\shows` is still there.
