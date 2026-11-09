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
| `@netplay state V` | Read the connection state into V. Values are 0 idle, 1 connecting, 2 waiting, 3 first seat paired, 4 second seat paired, and -1 disconnected. |
| `@netplay seat V` | Read the local seat as 1 or 2 when paired, or 0 outside a pair. |
| `@netplay send V` | Send V to the other seat when paired. |
| `@netplay receive V R` | Consume one message and set V to its value and R to 1. When the inbox is empty, set only R to 0. |
| `@netplay close` | Leave the current room and discard pending messages. |

V and R are database variable IDs. Values travel in order as signed 32-bit
integers. The game chooses its own value ranges within its variable limits.
A connection ends for both seats when either seat leaves.

## Synchronized map events

Both games enter the same match map, finish setup, and run
`@netplay start ID SEED`. The two database variables hold a fresh positive
match number and the same random seed. The player begins frame zero after
both seats announce the match.

| Comment | Operation during the synchronized map |
| --- | --- |
| `@netplay active V` | Set V to 1 during synchronized simulation and 0 in regular play. |
| `@netplay input A B` | Read the input bitmasks for seats A and B into the two variables, in the same order on either machine. Left, right, up, down, decision, cancel, and shift use bits 0 through 6. |
| `@netplay frame V` | Read the zero-based simulation frame. |
| `@netplay known V` | Read whether both inputs and their history are known. A call on a predicted frame requests replay once the inputs arrive, even when the prediction matches. |
| `@netplay present V` | Display the resource picture whose ID is stored in V from the confirmed frame stream. The setting belongs to the picture slot. |
| `@netplay live V` | Restore immediate drawing for the picture slot whose ID is stored in V. |
| `@netplay stop` | End the synchronized map after this frame is confirmed. The next map tick resumes regular event processing at the confirmed state. |

Parallel map and common events advance on the synchronized clock. Use the
synchronized input variables for rules. The snapshot covers map events,
switches, variables, seeded randomness, and resource pictures. Transitions,
foreground events, interactive messages, save I/O, and mutable images belong
outside the synchronized map. Play BGM, fade BGM and play sound event commands
are published once from confirmed frames in simulation order. A picture can
be tracked before it appears. Reserve the immediate track for effects whose
speculative positions and lifetimes the game can safely expose.
