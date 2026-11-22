# Level data and custom spawns

Virtua Cop's levels are data tables in `PPJDD.EXE`. The camera route is fixed. Each stop on the route is a *scene*, and each scene has a list of spawn records that say who appears, when, where and how they behave. The mod can replace any scene's list from `spawns.txt` in the game folder.

## spawns.txt

```
[scene 0 0 0]
like=004a3fc0 wave=1 x=-2.18 z=-87.40
like=004a3fc0 wave=1 x=-5.50 z=-87.40 steps=18:89,18:89,18:89,18:89,1:17,2:67
```

- **Blocks:** a `[scene stage area scene]` block replaces that scene's whole spawn list, at most 30 lines. Stages, areas and scenes count from 0.
- **Reloading:** the file is re-read every time a scene starts, so you can edit it while the game runs.
- **Reference files:**
  - `python tools/levels.py PPJDD.EXE > spawns_reference.txt` prints every original scene in this format.
  - `python tools/levels.py PPJDD.EXE scripts > scripts_reference.txt` prints every behaviour script as `steps=`, with where it's used.
- **Each line is one spawn.** `like=` copies an original record; the other keys override its fields:

| Key | Record offset | Meaning |
|---|---|---|
| `like=` | | address of an original record to start from (hex) |
| `type=` | +0 | 1 enemy, 0 hostage; boss bits 4/8/0x10/0x20 |
| `wave=` | +1 | the wave this spawn belongs to. A wave starts when everyone in the previous one has gone |
| `next=` | +2 | delay before the next wave once this one is cleared |
| `wait=` | +3 | how long the enemy waits before shooting, scaled by difficulty |
| `model=` | +4 | character model (table `0x4B3CC8`): 0–48 enemies, 49–52 bosses, 58–66 hostages |
| `drop=` | +5 | item dropped when shot (0 none) |
| `death=` | +8 | death style: 0 normal, 1 fall from a height, 2, 3 thrown back |
| `face=` | +9 | nonzero: keep turning to face the camera |
| `diff=` | +0xA | lowest difficulty this spawn appears on |
| `flags=` | +0xB | hex flag bits (meanings still unknown) |
| `x= y= z=` | +0xC | position, in world units |
| `angle=` | +0x18 | facing, 0–65535 for a full turn (16384 = a quarter) |
| `floor=` | +0x20 | ground height to land on after a jump or drop (float) |
| `script=` | +0x1C | address of an original behaviour script (hex) |
| `steps=` | | a new behaviour script, below |
| `react=` `weapon=` | | header for `steps=`; default is the copied script's |

## Models

Identified from a gallery in the game. The weapon a model throws comes from the model itself (the byte at +0xC of its `0x4B3CC8` entry). The script only decides whether and when the throw happens.

| Models | Look |
|---|---|
| 0–3 | brown clothes, red hair, gun |
| 4–6 | red hair, vest (4 throws knives) |
| 7 | red jumpsuit, throws grenades |
| 8–11 | Men in Black, gun |
| 12, 13 | thug with vest, gun |
| 14, 15 | Men in Black, gun |
| 16, 18, 20, 21 | red vest (18 throws knives) |
| 22–24, 26 | red vest (22 and 24 knives, 23 gun, 26 grenades) |
| 27 | white, gas mask |
| 28, 30, 31 | red suit (28 knives, 30 and 31 guns) |
| 32–35 | white hazmat suit, gas mask |
| 37–40 | SWAT |
| 41, 43–45 | green berets |
| 47, 48 | not yet seen |
| 49–52 | bosses (Kong, King, Fang, Boss) |
| 58–66 | hostages |

Models the levels never use (17, 19, 25, 29, 36, 42, 46) haven't been tried.

## Behaviour scripts

A script is two header words, then steps of four 16-bit words, ending with `0xFFFF`:

- **Header:**
  - The reaction table (c+0x553) chooses how the character reacts to hits (tables at `0x49D5BC`).
  - The weapon (c+0x571) is 0 for unarmed, or 1 or 2 for armed. Thrown weapons are spawned with it.
- **Steps:** `action:animation:param:turn`, written in `steps=` with commas between steps. A missing param or turn counts as 0.
  - The *action* picks the state handler (table `0x486480`).
  - The *animation* is played by that handler.
  - *Param* goes to c+0xD6.
  - *Turn* is added to the facing before the step starts.

Each step usually runs until its animation finishes, then the next step starts (`0x406740`).

| Action | Handler | What it does |
|---|---|---|
| 1 | `0x405480` | Appear and get ready. The step waits for the camera to stop and the shooting delay (`wait=`) to run out |
| 2 | `0x4055A0` | Attack loop. It repeats its animation and damages the player when the shot frame comes (`0x406C40`). Scripts usually end here |
| 6 | `0x405A40` | Ride the linked prop (a vehicle) |
| 7 | `0x405A80` | Play the animation once, then continue |
| 8 | `0x405AA0` | Start a run: frees the spawn slot so the wave can move on |
| 9 | `0x405AF0` | Ride the linked prop until it's destroyed |
| 10 | `0x405B40` | Wait for the shooting delay, then continue |
| 11 | `0x405B80` | Face the camera and wait for its wave's turn |
| 12 | `0x405BF0` | Hostage intro: face the camera, wait, continue |
| 13 | `0x405C70` | Throw (grenade, knife or axe) at animation frame 15, then leave |
| 14 | `0x405CE0` | Melee hit at frame 33 |
| 15 | `0x405D20` | Jump forward and down to `floor=` |
| 16 | `0x405DD0` | Drop straight down to `floor=` |
| 17 | `0x405E30` | Hop up and back down |
| 18 | `0x405F20` | Walk (animation 89 for enemies, 74 for hostages); `turn` steers |
| 19 | `0x405F40` | Run |
| 20 | `0x405F70` | End: the character leaves (despawns) when the script ends here |
| 22–29 | | Boss routines (Kong, King, Fang, Boss) |

Actions 0, 3–5, 21 and 23–25 are internal states: spawning, hit reactions, death and falling. Scripts don't use them.

Common animations for enemies: 12, 17, 87, 19, 9 (step out or appear), 69, 67, 65, 70, 66 (aiming and firing), 89 (walk), 88 and 90 (run, sidestep), 93 (crouched), 99, 101 (throw).

## The level tree

- **Tree:** `0x4AE020[stage]` → area table → scene table (8 bytes per scene: the record list and a scene value) → spawn records (0x24 bytes each, ending with a 0xFF type).
- **Scene load:** `0x4424D0` copies a scene's records into the spawn table at `0x5555A0`, 8 bytes per entry, with the count at `0x555692`. The mod hooks this.
- **Waves:** each frame, `0x404E10` spawns the next wave's entries through `0x404FF0` once the current wave is cleared.
- **Current position:** the stage, area and scene numbers are at `0x5554B8`, `0x5554BA` and `0x5554BC`.

Still to map: the camera route and when each scene starts, what the scene value means, the item numbers for `drop=`, and the flag bits.
