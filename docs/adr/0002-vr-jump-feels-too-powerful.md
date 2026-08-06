# ADR 0002 — VR jump feels too powerful (parked)

- **Status:** Accepted (parked, watching)
- **Date:** 2026-08-06
- **Area:** VR controller input — jump (`Control.cpp` `CInputDeemone::Process`)

## Context

With jump mapped to a controller button (right primary / A), the player cleared a fence
early in the game that is not normally passable — jump feels more powerful in VR than it
should.

Importantly, **the jump itself was not modified.** The VR mapping only sets the same
`uCMD_JUMP` command bit that the keyboard's jump key sets (rising edge into `u4ButtonHit`,
held state into `u4ButtonState`), exactly mirroring `tinReadDefaultControls`. All jump force,
integration and physics are the game's own. So the extra "power" comes from somewhere other
than the input mapping. Candidate causes, none yet confirmed:

- **Frame-rate-dependent jump integration.** VR runs at ~90–120 fps versus the retail
  ~30 fps target. If any part of the jump impulse or the subsequent physics is integrated
  per-frame rather than per-second, the effective jump height/velocity can scale with frame
  rate. This is the leading suspect and would affect other physics too.
- **The command being held.** Like the keyboard, the mapping sets `u4ButtonState` while the
  button is held; if the jump logic responds to the held state as well as the edge, a held
  button could extend/repeat the impulse. (Edge detection for `u4ButtonHit` is correct — one
  rising edge per press — so this would be a game-side interpretation, not a double-fire.)
- **Simply the retail jump.** Trespasser's jump is famously floaty/strong; the fence may just
  be low. Needs comparison against a keyboard jump in the same spot.

## Decision

**Park it.** Do not change jump now. The player will keep an eye on it through a full
playthrough and decide afterwards whether it needs tuning.

## Consequences

- Jump is usable now (it clears the obstacle that was blocking progress), which is what
  matters for playing through.
- If tuning is wanted later, start by checking whether jump/physics integration is
  frame-rate-dependent (compare jump height at capped 30 fps vs uncapped VR), since that would
  be the cleanest explanation and would implicate other physics too. Only then consider
  scaling the VR jump command specifically.
- Revisit after the playthrough.
