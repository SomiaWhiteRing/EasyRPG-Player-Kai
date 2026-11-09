# Game-owned netplay transport

Place the declaration in the project root next to RPG_RT.ldb.

```ini
[Netplay]
Enabled=1
```

The game uses regular RPG Maker comment commands for room controls. Initialize
the availability variable to zero in a regular event command before querying
it so players that ignore these comments follow the offline event path.

| Comment | Operation |
| --- | --- |
| `@netplay available V` | Set variable V to 1 when the Web host supplies a relay, or 0 otherwise. |
| `@netplay open V` | Open the room numbered by V. Valid room numbers range from 1 to 999999. |
| `@netplay create V` | Open a new room numbered by V. An occupied room is rejected. |
| `@netplay join V` | Join an existing room numbered by V. An absent or full room is rejected. |
| `@netplay rooms T` | Read the latest room directory into Maniac string T. Each row has a numeric `id` and a `players` array with `name` and `avatar` fields. |
| `@netplay identity T` | Read the local directory profile as a JSON object with `name` and `avatar` fields. |
| `@netplay stop_directory` | Stop receiving directory updates when leaving the online menu. |
| `@netplay fade_transfer OUT IN [DELAY]` | Set the fade durations and an optional black-screen delay before the next map's fade-in, in frames. A screen already erased starts with the delay and reveal. |
| `@netplay state V` | Read the connection state into V. Values are 0 idle, 1 connecting, 2 waiting, 3 first seat paired, 4 second seat paired, and -1 disconnected. |
| `@netplay seat V` | Read the local seat as 1 or 2 when paired, or 0 outside a pair. |
| `@netplay send V` | Send V to the other seat when paired. |
| `@netplay receive V R` | Consume one message and set V to its value and R to 1. When the inbox is empty, set only R to 0. |
| `@netplay close` | Leave the current room and discard pending messages. |

V and R are database variable IDs. Values travel in order as signed 32-bit
integers. The game chooses its own value ranges within its variable limits.
The remaining seat returns to waiting when its peer leaves. A room closes
when its last seat leaves.

## Game-owned presentation

With netplay enabled, a Maniac JavaScript event can register
`rm2k3.setNetplayOverlay(draw)`. The callback receives the canvas, its width,
its height, and the current monotonic time in milliseconds. Pass null when
leaving the online flow. Keep event and simulation updates in event commands.

| Canvas method | Operation |
| --- | --- |
| `fill(x, y, width, height, rgba)` | Blend a rectangle using a packed `0xRRGGBBAA` color. |
| `textWidth(value)` | Measure the default font in pixels. |
| `text(value, x, y, rgba, scale)` | Draw with the default font at scale 1 or 2. |
| `picture(file, sx, sy, sw, sh, x, y, w, h, opacity, flip)` | Draw a project picture using a source rectangle and a destination rectangle. Use zero source dimensions for the whole image and 1 to flip horizontally. |
| `capture()` and `restore()` | Save the current frame for reuse as the overlay backdrop. |
| `avatar(path, x, y, w, h)` | Draw an avatar supplied by the Web room directory once it has loaded. |
| `peerLatency(probe, reset)` | Read the latest peer round-trip time in milliseconds. Pass true to probe or reset. A missing sample returns -1. |

`rm2k3.netplayTime()` reads the same monotonic clock during event processing.
`rm2k3.netplayButtons(repeat)` returns input edges or, when passed true, the
engine's repeated buttons. Down, left, right, up, decision, cancel, and shift
use bits 1 through 7. An Escape edge uses bit 8 and the digit buttons use bits
10 through 19. `rm2k3.netplayFade(frames, show)` starts a fade with an explicit
duration. Omit both arguments to read its state as 0 visible, 1 transitioning,
or 2 erased.

## Synchronized map events

Both games enter the same match map, finish setup, and run
`@netplay start ID SEED`. The two database variables hold a fresh positive
match number and the same random seed. The player begins frame zero after
both seats announce the match.

`@netplay seed V` seeds the game RNG before entering the synchronized map.
Run `@netplay prepare MAP FADE [SEED]` before a map transfer to use the synchronized
map's update path during its local opening sequence. MAP is the destination
map variable. The optional positive FADE duration reveals the map after its
first update. Otherwise the project controls the reveal. The opening's
tracked pictures remain visible while the first confirmed frames collect.
An optional SEED variable applies on the destination map before the first
opening update.
When both seats run the same seeded setup, `@netplay start ID SEED continue_rng`
continues the random sequence produced by that setup. The two-argument start
seeds the RNG at frame zero.

An optional variable after the RNG mode sets the confirmed presentation
buffer in frames. For example, `@netplay start ID SEED continue_rng BUFFER`
lets a game choose how many confirmed frames to collect before playback
starts or resumes. The default is one frame.

| Comment | Operation during the synchronized map |
| --- | --- |
| `@netplay active V` | Set V to 1 during synchronized simulation and 0 in regular play. |
| `@netplay input A B` | Read the input bitmasks for seats A and B into the two variables, in the same order on either machine. Left, right, up, down, decision, cancel, and shift use bits 0 through 6. A confirmed event signal uses bit 7. |
| `@netplay frame V` | Read the zero-based simulation frame. |
| `@netplay known V` | Read whether both inputs and their history are known. A call on a predicted frame requests replay once the inputs arrive, even when the prediction matches. |
| `@netplay signal V` | If V is nonzero, put a one-frame pulse in the local seat's next outgoing input after this frame is confirmed. |
| `@netplay present V` | Display the resource or baked text picture whose ID is stored in V from the confirmed frame stream. The setting belongs to the picture slot. |
| `@netplay live V` | Restore immediate drawing for the picture slot whose ID is stored in V. |
| `@netplay birth V` | Start a new presentation lifetime for the picture slot whose ID is stored in V. |
| `@netplay retire V` | Hide the current lifetime of picture V from the immediate view when this frame is presented. A later birth in the same slot can draw immediately. |
| `@netplay track A B FLAGS [X [Y [U [V]]]]` | Set the view of an inclusive picture slot range stored in A and B. Add 1 for confirmed drawing or 8 to use the confirmed picture only when the immediate picture is invisible. Add 16 to capture confirmed poses while drawing immediately so later switches can use the already buffered poses. Add 32 to retain the view with the captured pose across slot reuse. Add 64 to also retain the view and use a confirmed birth only when no immediate picture is visible and that birth has not already been previewed. Add 2 for horizontal reflection or 4 to reflect the position only. X and Y give offsets before reflection. U and V add offsets when using the fallback. |
| `@netplay sample V COUNT` | Attach a range of variables to the current frame for presentation. |
| `@netplay shown V D COUNT` | Copy the most recently presented samples starting at V into variables starting at D. Missing samples read as zero. |
| `@netplay upcoming V D R U` | Find the first queued confirmed frame where sample V is nonzero. Write its playback distance to D and sample R from that frame to U. Both outputs are zero when no frame matches. |
| `@netplay drawn P X Y` | Read the last drawn coordinates of the picture slot stored in P, after offsets and before reflection. Missing coordinates read as zero. |
| `@netplay stop` | End the synchronized map after this frame is confirmed. The next map tick resumes regular event processing at the confirmed state. |

Parallel map and common events advance on the synchronized clock. Use the
synchronized input variables for rules. The snapshot covers map events,
switches, variables, seeded randomness, and resource pictures. Transitions,
foreground events, interactive messages, save I/O, and mutable images belong
outside the synchronized map. Play BGM, fade BGM and play sound event commands
are published once from confirmed frames in simulation order. A picture can
be tracked before it appears. Reserve the immediate track for effects whose
speculative positions and lifetimes the game can safely expose.
Use displayed samples and coordinates only for presentation decisions.

During a synchronized map tick, `rm2k3.netplayContinuePicture(id)` captures
the slot's final pose for an immediate drawing when the native picture is
absent. `netplayContinuePicture(id, x, y, angle)` updates its position and
rotation, and `netplayContinuePicture(id, null)` removes it. The copy follows
rollback snapshots and stops drawing when its birth is retired on the
confirmed presentation timeline or replaced by a newer picture.

`rm2k3.setNetplayProjection(callback)` runs a drawing-only callback after
prediction and replay, once per synchronized map tick. It receives the
zero-based seat and whether the match is stopping. The callback can read
`netplaySample(variable, shown)`, `netplayUpcoming(variable, related)` and
`netplayPosition(picture, source)`. Position sources 0, 1 and 2 read the
immediate, confirmed and last drawn coordinates respectively.
`netplayProject(first, last, source, x, y, shownX, shownY)` selects a drawing
source for an already tracked slot range and adds offsets. Source 0 keeps
the track's current policy, 1 selects immediate drawing and 2 selects
confirmed drawing. The last two arguments are optional extra offsets for
the confirmed drawing. These overrides do not change simulation or capture.
