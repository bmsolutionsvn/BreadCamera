# C1 manual check — BreadCall guests as cameras (owner)

A person does this with **two real remote guests**; it is not self-certified (roadmap C1 row, design §9). The
automated acceptance (`tests/e2e_c1_test.cpp`) drives synthetic publishers against a local LiveKit; this checklist is
the human gate on real guests and the full stage pipeline.

## Setup

- A reachable BreadCall room and two guests joining it with the normal BreadCall web client.
- A hidden, subscribe-only receiver token for the room: `POST /api/rooms/{roomId}/receiver-token` (director/owner or
  admin). Mint **one token per BreadCamera camera** (each camera joins as its own hidden receiver).
- In BreadCamera, Add camera → **BreadCall guest**: the room URL (`wss://…`), the token, and the slot (1..8). One
  camera per enabled slot; give the camera effects (Matte, Background, Colour) as in any other camera.

## Steps

1. **All slots enabled → every guest is a camera.** Add a BreadCall guest camera for each occupied slot. Each guest
   appears as its own BreadCamera camera and is live; enable Matte + Background on one guest camera and see the effect
   applied to that guest's picture (and audio meters move for that camera).
2. **One slot enabled → only that guest.** Disable every guest camera but one. Only that guest's picture is live; the
   other guests do not appear.
3. **Slot 2 stays slot 2 across a guest change.** With slot 2's camera live, have the person in slot 2 leave and a
   **different** guest join the same slot. The slot-2 camera comes back live with the new person's picture — the
   camera, its effects and its tally do not change position, and no new camera is added.
4. **BreadCamera sends nothing back.** In the BreadCall room, BreadCamera's receiver does not appear as a normal
   participant (hidden) and publishes no camera or microphone track. Guests see no return picture or audio from
   BreadCamera, and cannot hear or see it.
5. **Record**: date, room, guests (who/what device), which steps passed, and anything that looked wrong (delay, a
   guest that never appeared, effects that did not apply).