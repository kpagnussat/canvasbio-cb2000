# 0008. Fifteen counted touches, and no template update after a match

Status: accepted

## Context

How many touches an enrollment takes had been read wrong twice in this
project's history, as 8 and then as 14. The figure is in how the Windows
driver configures its engine on this sensor: 10 plus 5, that is 15 counted
samples. The engine's code also contains an adaptive update, which
replaces a template node with a better one after a successful match, and a
driver that implements it improves over time.

## Decision

Enrollment is 15 counted touches, fused into one template the way the engine
fuses them. While samples 6 through 9 are being collected, a touch that adds
too little new area is refused with a cycling "move your finger" hint, which
is the vendor behavior. The adaptive update is not implemented, because
nothing on this sensor writes a merged template back: verify never stores
one, and identify only stores when a learn flag is set, which it is not
here. The merge itself runs in the vendor engine and is discarded.

## Consequences

- Enrollment takes 15 to 18 touches in practice, counting refusals, which is
  more than users expect and is stated in the README.
- A template does not improve with use. What the user enrolls is what they
  get, which makes the placement advice in the README matter more.
- An earlier plan to implement the adaptive update, drafted from the engine's
  code, was dropped once the driver side was read: implementing it would have
  been a departure from the sensor's actual behavior, not an improvement.

## Evidence

`src/cb2000_engine_enroll.c`, `src/cb2000_engine_adapter.c`.
`docs/RESEARCH.md`, "What the Windows driver does" (Enrollment) and
"Corrections to earlier readings" (Enrollment count, Adaptive template).
