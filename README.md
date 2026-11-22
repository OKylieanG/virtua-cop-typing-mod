# Virtua Cop Typing Mod

A mod for the 1996 Windows PC release of **Virtua Cop** (released in North America as *Virtua Squad*) that replaces the light gun with the keyboard. Every enemy on screen gets a word over its head. Type the word and the mod aims and fires for you through the game's own input, so the game's hit detection, scoring, combos and enemy reactions all work as normal.

The default word list is pro-audio and physics terms (*sidechain*, *headroom*, *inverse square law*, *quark*...). Use your own by editing `words.txt`.

> This repository contains only the mod's source code, tools and notes. **No game files are included.** You need your own copy of Virtua Cop / Virtua Squad for PC.

## How it plays

- **Locking on:** typing the first letter of an enemy's word locks onto that enemy, and the letters you've typed turn red. While locked, only that enemy's word counts, and no two enemies on screen share a first letter.
- **Firing:** finishing the word aims at the enemy and fires.
- **Wrong keys:** they count against your accuracy, but they never fire.
- **Esc:** cancels the current word and releases the lock.
- **Hostages:** they never get words.
- **Single letters:** anything shootable that isn't a person gets a single letter, and no two letters on screen are the same:
  - **Projectiles:** grenades, thrown knives and axes, and the boss's missiles. The letter follows the projectile, and typing it shoots it down.
  - **Pickups:** weapon and life pickups. Typing the letter collects it.
  - **Props:** lights, barrels and so on. Set `props=0` in `tdmod.ini` to turn these letters off.
- **Level word map:** Virtua Cop's levels are scripted, and every enemy appearance comes from a fixed spawn record in the game's data. `wordmap.txt` in the game folder lists those appearances in the order you first meet them, each with a maximum word length:
  - **Building the map:** the mod appends any appearance it hasn't seen before, with a default limit of 6 and a note of its wave and model. Play each stage once, then edit the limits.
  - **Each run's word list:** at startup the mod picks one word per appearance from `words.txt`, close to that appearance's limit. Appearances near each other in the map, which tend to be on screen together, get different first letters.
  - **Fallbacks:** an enemy that survives a hit gets a fresh word within the same limit. Appearances not yet in the map get a random word.
- **Tough enemies:** an enemy that survives a hit gets a longer word and takes more shots, and bosses get long phrases. Words are tiered by length:
  - up to 6 letters for most enemies,
  - 7–12 letters for some enemies,
  - longer words and phrases for tough enemies.
- **Reloading:** automatic.
- **Enemy speed:** Virtua Cop's timing assumes a light gun, giving you roughly 0.7 seconds to react, which isn't enough time to type a word. In typing mode the mod slows the characters only: their movement, aiming and firing run at 40% speed by default, while the camera, your gun and everything else stay at full speed. Slowed enemies are still drawn and can be shot on every frame. Press **Page Down** for slower enemies or **Page Up** for faster ones, in 10% steps from 20% to 100%. Your choice is saved to `tdmod.ini` in the game folder, which also has a `speed=` setting that slows the whole game if you want that instead.
- **F8:** switches between typing mode and normal mouse play. Use mouse mode for the stage-select screen, which needs aiming.
- **Enter:** acts as the trigger, for PRESS START and continue screens.
- **F7:** debug markers on everything the mod tracks, such as `T3` on a grenade or `P2150` on a prop, plus extra detail in `tdmod.log`.
- **HUD:** the bottom-right corner shows the current mode, your accuracy and your word count.

## Requirements

- Virtua Cop / Virtua Squad for Windows. This mod targets the executable `PPJDD.EXE` with SHA-256 `666884e8ddc2e08e80feccad1eaefe3f682c5220258fc9c0c509a996d93f0752`. Other versions will need the addresses adjusted. If the code at any hook site doesn't match, the mod skips that hook and notes it in `tdmod.log`, rather than patching the wrong place.
- Windows 10 or 11 with DirectPlay enabled: Control Panel → Programs → Turn Windows features on or off → Legacy Components → DirectPlay.
- [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw), so the game's 8-bit DirectDraw modes work on modern Windows.
- To build the mod: a MinGW-w64 GCC with `-m32` support plus binutils (`ld`, `dlltool`). The x86_64 toolchain bundled with Strawberry Perl works; no 32-bit C runtime is needed.

## Install

1. **Get the game running:**
   1. Install the game.
   2. Put cnc-ddraw's `ddraw.dll` and `ddraw.ini` in the game folder.
   3. Mount the game CD. The game checks the disc for `\common` and `\native\vcop`. If your image is a Nero `.nrg`, `tools/nrg2cue.py` converts it to `.iso` plus `.cue`/`.bin`.
2. **Build the mod** with `powershell -File build.ps1`, which produces `out\DPLAY.dll`.
3. **Copy the mod** `out\DPLAY.dll` and `words.txt` into the game folder.
   - The game loads `DPLAY.dll`, the old DirectPlay library, at startup, so the mod is the DLL itself, and its two DirectPlay functions are forwarded to Windows' `dplayx.dll`.
   - It also fixes the "DPLAY.dll not found" startup error on modern Windows.
   - Back up any existing `DPLAY.dll` first.
4. **Launch** `PPJDD.EXE`. The mod writes `tdmod.log` next to the game, listing which hooks were installed, every shot fired, and any reload problems.

To uninstall, delete `DPLAY.dll` from the game folder, or put back the one you backed up.

## How it works

The main hooks:

| Address | What it is | What the mod does |
|---|---|---|
| `0x40C580` | hit test for props (`player, object`) | projects the object's hit boxes to screen coordinates using the game's own matrix stack |
| `0x433740` | per-frame check for characters | projects each character's hit spheres, picks aim points (torso, then head), skips unarmed characters (hostages) |
| `0x40BD20` | per-frame object update | timers, word assignment, and the firing state machine |
| `0x434E82` | player update, just before the weapon handler | delivers reload requests |
| `0x4208BA` | just before the frame is shown | draws the words and HUD on the back buffer with GDI |

The table lists the main hooks. Others cover the other kinds of target (sphere-tested props, list items, pickups, the projectile pools), the spawn routine (for the word map), the character AI and animation (for the enemy slow-down), and the start of the character loop (to read the game's hit resolution).

**Firing:** the mod writes the crosshair position and the mouse-button state that the game reads its input from. It waits until the gun can fire (ammo, trigger cooldown, "shooting allowed" flag), then confirms the result two ways: the ammo count drops, and the game's hit test flags the target. For enemies it also reads the game's own decision on who kept the bullet. If a box or wall in front took it, the mod re-aims and fires again.

See [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) for the full set of addresses and data structures.

## Tools

The `tools/` folder has the helpers used to reverse-engineer the game. They're useful if you want to extend the mod or port it to another version.

| Tool | Purpose |
|---|---|
| `ida.py` | Calls the IDA Pro MCP plugin directly (decompile, cross-references) with no client time limit. |
| `hwbp.py` | Minimal debugger for 32-bit games, using hardware breakpoints. It attaches to the running game, logs every instruction that reads, writes or executes an address, then detaches. |
| `vmem.py` | Read-only memory snapshots, diffs and hex/word dumps of the running game. |
| `objs.py` | Dumps the live prop table. |
| `asmrange.py` | Prints an address range from an `objdump -d -M intel` listing. |
| `nrg2cue.py` | Converts Nero `.nrg` disc images to `.cue`/`.bin` plus a plain `.iso` of the data track. |
| `shot.ps1` | Screenshot of the game window's client area. |

## License

The mod source and tools are under the MIT license; see [LICENSE](LICENSE). *Virtua Cop* is a trademark of SEGA, and this project isn't affiliated with or endorsed by SEGA.
