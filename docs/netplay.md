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
| `@netplay send V` | Send V to the other seat when paired. |
| `@netplay receive V R` | Consume one message and set V to its value and R to 1. When the inbox is empty, set only R to 0. |
| `@netplay close` | Leave the current room and discard pending messages. |

V and R are database variable IDs. Values travel in order as signed 32-bit
integers. The game chooses its own value ranges within its variable limits.
A connection ends for both seats when either seat leaves.
