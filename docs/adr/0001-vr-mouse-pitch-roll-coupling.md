# ADR 0001 — Park the VR mouse-pitch → view-roll coupling

- **Status:** Accepted (parked)
- **Date:** 2026-08-06
- **Area:** VR / OpenXR port — render camera orientation (`CRenderDB::Process`, `r3BodyNoRoll`)

## Context

In the VR build the render camera is derived from the player's *body* orientation
(`pr3_eye.r3Rot`) with the tracked head pose composed on top. Trespasser's body
camera banks and pitches with the terrain and with mouse-look, so we level it before
handing it to the eyes: `r3BodyNoRoll` rebuilds the "up" axis so it lies in the
vertical plane through the look direction, stripping roll while keeping yaw and pitch.
It shapes a temporary copy of the render camera only — gameplay, physics and aiming
still use the true body orientation.

This works for the case that matters day-to-day:

- **Terrain slopes / banking:** with the mouse untouched, the horizon stays level on
  all surfaces. Confirmed in the headset.
- **Mouse yaw (horizontal):** swings the view correctly. Confirmed.

One case remains broken:

- **Mouse pitch (vertical):** moving the mouse up/down, which should tilt the view up
  and down, instead **rolls** the view.

We iterated on `r3BodyNoRoll` twice to fix this:

1. First version composed the body with a corrective `from-to(up → levelledUp)` delta.
   This is ill-conditioned near identity — for a clean pitch the two "up" vectors are
   equal in theory but differ by float noise, so the shortest-arc rotation picks a
   near-random axis and leaks roll into the pitch.
2. Second version (current) builds the de-banked rotation **directly** from the frame
   `{right, forward, up}` via `CMatrix3` → `CRotate3`, which has no near-zero
   subtraction and is a provable no-op for a clean yaw+pitch body.

The roll-on-pitch **persisted** through both versions. That strongly suggests the roll
is not introduced by `r3BodyNoRoll` itself but originates upstream — in how the engine
composes mouse-pitch into the body/head orientation for the stereo path (the same
family as the deferred "bug #3" skew/shear on pitch/roll). Root-causing it fully would
be a non-trivial dig into the camera composition, with uncertain payoff.

## Decision

**Park the mouse-pitch → view-roll coupling. Do not spend further effort on it now.**

Keep `r3BodyNoRoll` as-is (the matrix-based version): it delivers the confirmed,
valuable behaviour — a level horizon on slopes and correct yaw — and is the more
numerically robust of the two implementations regardless.

The justification for parking: **the mouse is not the shipping VR input method.** In
VR the player will use motion controllers, which is a later milestone on the roadmap.
Pitch will come from the controller / head, not the mouse, so the mouse-pitch path
never exercises this coupling in the intended experience.

## Consequences

- **Positive:** Slope leveling and yaw ship now; we stop burning time on an input path
  the product won't use. `r3BodyNoRoll` stays in its most robust form.
- **Negative / risk:** Anyone testing the VR build *with a mouse* will see the view roll
  when pitching. This is a known, accepted limitation — document it in test notes so it
  isn't re-reported as a fresh bug.
- **Revisit when:** controller input lands (roadmap). At that point, confirm pitch via
  controller/head is clean; if any roll-on-pitch remains, root-cause it in the engine's
  body/head orientation composition (shared with the deferred bug #3 skew/shear), not in
  `r3BodyNoRoll`.
