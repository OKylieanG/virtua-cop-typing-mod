# Reverse-engineering notes

These notes cover `PPJDD.EXE` from the Windows PC release, SHA-256 `666884e8ddc2e08e80feccad1eaefe3f682c5220258fc9c0c509a996d93f0752`. The image base is `0x400000`. All addresses below are virtual addresses in that image.

## Input

The PC version turns mouse input into an arcade-style light-gun record, and the gameplay code reads only that record.

| Address | Type | Meaning |
|---|---|---|
| `0x40A240` | function | window procedure. `WM_MOUSEMOVE` updates the crosshair; the mouse buttons set the button state below |
| `0x4866AC` | dword | crosshair position in window pixels, packed as `(y << 16) \| x` |
| `0x4866B0` | word | left button / trigger: 1 = held |
| `0x4866B4` | word | right button / reload: 1 = held. The reload button only works on a double press within 10 frames |
| `0x529004` / `0x529008` | word | screen width and height (640 and 480 in high-resolution mode) |
| `0x41927A` | code | converts the above into the gun record each frame |
| `0x5551C0 + n*0x18` | struct | arcade input record per player: `+2` buttons (active-low, `0x400` trigger, `0x100` reload), `+4`/`+6` press/release edges, `+8` gun X, `+A` gun Y |

The gun record uses arcade coordinates:
- `gunX = x * 320 / width + 16`
- `gunY = y * 240 / height`

Inverting those, the crosshair position for an aim point measured from the screen center is:
- `x = (aimX + 160) * width / 320`
- `y = (aimY + 120) * height / 240`

## Players

Player 1's struct is at `0x556220`; player 2's is at `0x5561D0`.

| Offset | Meaning |
|---|---|
| `+0` | player id (1 or 2) |
| `+2` | byte: set on frames where this player's shot went off; the hit tests check it |
| `+4` | per-frame state handler (e.g. `0x435220`) |
| `+0x0C` / `+0x0E` | buttons held / newly pressed |
| `+0x14` / `+0x16` | aim relative to the screen center: `gunX - 176`, `gunY - 120` |
| `+0x1C` / `+0x1E` | gun X/Y (arcade units) |
| `+0x28` | short: **ammo** |
| `+0x2A` | "empty, show RELOAD" counter |
| `+0x2C` | byte: **reload request**. Set by an offscreen shot or a reload double-tap; the weapon handler refills the gun and clears it |
| `+0x2E` | word: **trigger cooldown**. Presses are ignored while it's nonzero |
| `+0x34` / `+0x36` | trigger / reload button masks |
| `+0x3A` | index into the magazine-size table at `0x4B16C0` |
| `+0x3C` | weapon handler (pistol `0x435730`; others at `0x435760`, `0x435970`, `0x4359A0`) |

**Other player globals and code:**
- **`0x5554A6`** (byte): the game ignores the trigger entirely while this is 0.
- **Per-frame player update (`0x434E70`):**
  1. calls the state handler (`[+4]`),
  2. clears `+2`,
  3. calls the weapon handler (`[+0x3C]`).
- **Pistol code:**
  - fire: `0x435660`
  - reload: `0x4355A0`, which checks `+0x2C`

## Matrix stack and projection

| Address | Meaning |
|---|---|
| `0x549030` | pointer to the top of the matrix stack; the base is at `0x549050`; each entry is 0x30 bytes |
| `0x446EC0` | push (duplicates the top) |
| `0x446EA0` | pop |
| `0x446CD0` | `top = top * M` |
| `0x4472A0` | translate (x, y, z) |
| `0x446F20`, `0x447030` | rotations (16-bit angle) |
| `0x41AC60` | transforms a point by the top matrix (3×3 stored column-major, translation at `+0x24`) |
| `0x55C4D4` | float: focal length. A camera-space point projects to `(x*f/z, y*f/z)`, measured from the screen center in the same units as the player's `+0x14/+0x16` |

## Props (shootable objects)

- **Pool:** starts at `0x12C7890`, stride `0x4AC`. The object count is a byte at `0x12CE409`.
- **Update loop:** `0x40BD20` calls each object's callback (`obj+0x480`) every frame.
- **Hit test:** `int __cdecl hit_test(player, obj)` at `0x40C580`. It returns early unless `player+2` is set. For each hit box it:
  1. pushes the matrix stack,
  2. multiplies in the bone matrix `obj+0x29C + bone*0x30`,
  3. translates by the box offset and applies its two rotations,
  4. projects the quad x ∈ [−w, w], y ∈ [0, h],
  5. checks whether the aim point is inside the quad.

| Offset | Meaning |
|---|---|
| `+4` → `+0x10` | hit box array, 0x1C bytes each: `+0` bone, `+4..+0xC` offset, `+0x10` half-width (scaled by the double at `0x450118`), `+0x14` height, `+0x18`/`+0x1A` rotations |
| `+0x47C` | nonzero = not hittable |
| `+0x484` | bitmask of active bones |
| `+0x4A0` | hit box count |
| `+0x4A5` | player that hit it |
| `+0x4A6` | bone that was hit |

## Characters (enemies, hostages)

- **Pool:** starts at `0x556490`, stride `0x60C`.
- **Per-frame check:** `0x433740(character)` resets the hit flags, requires `+0x0B == 1` and `(+0x0C & 0x0C) == 0`, then, if a player fired that frame:
  1. runs the quick test `0x433970` (two spheres from the table at `0x4B13E0`),
  2. runs the detailed test `0x4339D0(player, character)` over all 21 spheres. Each sphere goes through the bone matrix and the circle test `0x4337D0(sphere, player)`.

| Offset | Meaning |
|---|---|
| `+0xD8` | model record. `model+4` → hit sphere array: 21 spheres, 0x14 bytes each (`+0` bone, `+4..+0xC` position, `+0x10` radius). `model+0x0C` = **weapon count; 0 means a hostage** |
| `+0x13C + bone*0x30` | bone matrices |
| `+0x548` / `+0x54C` | sphere index / bone that was hit |
| `+0x5C8` | per-sphere mask used by the hit-effect drawing |
| `+0x5D3` | hit flags: bit `id` = quick test hit, bit `id << 4` = detailed hit (`0x10` for player 1) |

A hit sphere that sits behind a prop, such as a crate, can still set the detailed-hit flag even though the prop takes the bullet. The mod confirms a kill by waiting for the character to drop out of the alive/targetable check.

## Timing

**Frame pacer (`0x41FA80`):** the game is frame-locked. Each call runs one logic step, then waits until 33 ms (`0x21`) have passed, giving a fixed 30 fps. Everything is counted in frames.

| Address | Meaning |
|---|---|
| `0x41FB64`, `0x41FB6D`, `0x41FB90`, `0x41FB9A`, `0x41FBAD` | `cmp`/`add eax, 0x21` in the pacer |
| `0x4095D3`, `0x40A410`, `0x42902A` | `add eax, 0x21` when the frame clock is reset after loading or a pause |
| `0x4866A4` | next frame deadline (`timeGetTime` units) |
| `0x677DD0` | word: frame counter, incremented once per frame |
| `0x4A3248` | frame-skip mode flag |

**Character loop (`0x405220`):** runs once per frame over the character pool. For each active character it calls, in order:
1. **AI step:** `0x406720` normally. This is the behavior script: movement and deciding to aim. Bosses (`+0x54E & 0x3C`) run `0x4066A0` instead, which calls `0x406720`, then `0x424CE0`, then for boss types 4/8/0x10/0x20 the boss shot pools (`0x439E70`) and their shoot-down test (`0x437DC0`).
2. **Update:** `0x433480`, which runs:
   1. **animation clock** (`0x432230`): `+0x44` keyframe += `1 + +0x559`; a requested animation in `+0x4A` becomes the current one in `+0x4E`,
   2. world matrix (`0x4331D0`),
   3. screen projection and culling (`0x433290`),
   4. level of detail plus pose/draw handlers (`0x4333C0`, table at `0x4B1430`),
   5. **animation events** (`0x433D70`): sounds and gunshots keyed on `+0x44`,
   6. hit effects (`0x433AF0`),
   7. hit check (`0x433740`).
3. **Hit reaction:** `0x4067F0`, which reads the `+0x5D3` hit flags.

**How enemies fire:** there's no separate attack countdown. A firing animation reaches its gunshot keyframe, and the animation-event handler (`0x422D00` area) plays sound `0xA01114FF` and calls `0x406C40(character, 1)` to shoot the player. The lock-on circle follows that animation. Skipping a character's AI, animation clock and animation events on some frames slows everything that character does, while its pose, drawing, hit check and hit reaction still run every frame.

**Sound IDs:** sounds are played by `0x41AEF0` with packed IDs `0xA011xxFF`, where `xx` indexes the table at `0x4B83C4` (8 bytes per entry: name pointer, id). Examples:
- `0x04` = reload
- `0x12`–`0x14` = enemy gunshots
- `0x22` = "don't shoot"
- `0x23` = "help"

## Hit resolution

When a shot hits several things, `0x444970` (end of the frame, after the object loop) keeps only the nearest:
- `0x4067F0` records the nearest hit character in `0x12D4F54` and its distance in `0x12D4F58`. The character loop clears both at its start (`0x405220`).
- The nearest hit prop is in `0x12CE970` (pool index, `0xFF` = none) and the nearest list item is in `0x12DB9F4`.
- If a prop is nearer than the character, `0x12D4F54` is cleared, unless the player's weapon index is 5, which shoots through.

## Projectiles

Thrown weapons and boss shots live in three pools of `0xAC`-byte records, separate from the props and the item list.

| Pool | Slots | What | Update / hit test |
|---|---|---|---|
| `0x5558D0` | 10 | thrown weapons: kind 3 = grenade/bomb, kind 4 = knife/axe | `0x439690` → `0x4396C0` per record; shoot-down test `0x439D30(player, proj)`, only while the thrower's `+0x599` is set |
| `0x4C2CB8` | 20 | boss shots | shootable while `+0x82 >= 20`; test `0x437C80`, sets `+0x82 = 0` and explodes |
| `0x4C7338` | 10 | boss shots | active while `+0x82 != 0`, shootable while `> 10`; sphere = origin, radius `+0x82 * 0.8` (`0x43A970`); resolved in `0x43A420` |

| Offset | Meaning |
|---|---|
| `+0` | active |
| `+8` | kind |
| `+9` | phase: 1 = bouncing on the ground (`0x439720`), 2 = in flight (`0x439870`), 3 = `0x439B40` |
| `+0x0A` | state (in flight: 2 = shot down) |
| `+0x0C` | world position (3 floats) |
| `+0x48` | world matrix, built by `0x439C70` |
| `+0x78` | hit spheres, 0x14 bytes each, list ends with a `0xFF` byte |
| `+0x7C` | thrower (character) |
| `+0x80` / `+0x82` | flight length / frames left (boss pools: life) |

Breakable props (callbacks `0x442110`–`0x4421B0`) aren't projectiles: each hit takes one point off `+0x49C`, the model swaps at 1, and at 0 `0x436790` spawns debris.

## Rendering

- **Surfaces:** the back buffer `IDirectDrawSurface*` is at `0x4866E0` and the primary at `0x4866DC`.
- **Frame present (`0x420600`–`0x4208DF`):** unlocks the back buffer, optionally draws GDI text through `GetDC` (vtable `+0x44`) and `ReleaseDC` (`+0x68`), then flips (`+0x2C`) at `0x4208CF`.
- **Window handle:** `0x12CF5B0`.

## Startup checks

- **CD check:** `GetDriveTypeA` on every drive, looking for `X:\common\` and `X:\native\vcop\`. The message is "Please insert VIRTUA SQUAD CD." The game plays music from the CD's audio tracks over MCI `cdaudio`.
- **DirectPlay:** `DPCTRL.DLL` imports ordinals 1 and 2 from `DPLAY.dll`, which are `DirectPlayCreate` and `DirectPlayEnumerate`.
