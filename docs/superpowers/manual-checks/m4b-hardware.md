# M4b hardware checks (owner)

## Virtual camera (Windows 11)
1. As administrator: `regsvr32 <build>\vcam\breadcamera-vcam.dll` → "succeeded".
2. Add an SRT camera, add output "Windows virtual camera" #1.
3. Open the Windows Camera app → pick "BreadCamera - <camera>" → live picture within 2 s.
4. Open it in OBS (Video Capture Device) at the same time as the Camera app → both show picture.
5. Stop the input → after 1 s the virtual camera shows the dark-grey slate, never black.

## DeckLink
1. Card with SDI in: add a DeckLink camera → live; change the source from 1080i50 to 1080p60 → picture returns without touching BreadCamera.
2. SDI out: add a DeckLink output → picture on a monitor.
3. Key + fill: on a card with an external keyer, a keyed output on a camera with Background mode = "Keyed (fill + key)" → fill on SDI A, key on SDI B; a switcher's DSK keys the person cleanly.

## AJA (experimental)
1. Input on channel 1 → live picture and audio meters in the switcher fed from BreadCamera.
2. Output on channel 1 → picture (fill only).