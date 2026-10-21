# RE3Controls 1.9.4 — performance pass and later changes

The performance changes below (1.9.1) only reduce work on hot paths and keep the 1.9 behaviour (item 6 has one edge
case). The fixes and behaviour changes made afterwards, up to 1.9.4, are listed separately under "Later changes".

## What was expensive

- `Readable()` called `VirtualQuery` (kernel) **hundreds of times per frame**
- `Mode=MeleeCamera` still ran the full free-orbit camera path every frame
- Aim-walk hooks (`HookBlend` / `HookMotionUpdate`) scanned layers on **every** model in the game
- Several checks ran expensive work before cheap gates

## Changes

1. **`Readable()` result cache** (`common.h`)  
   Caches recent `VirtualQuery` results so repeated probes of the same pages skip the kernel call. Only readable
   regions are cached, and entries expire after 32 ms: the game frees and commits memory all the time (chapter
   restarts, level loads), so an older answer can be wrong. The table is behind a spin lock because split screen
   updates the players on separate threads. The first version kept 16 regions with no expiry and no lock: a stale
   "readable" meant reading freed memory (a possible crash), and a cached "not readable" stayed that way after the
   region was committed.

2. **MeleeCamera fast path** (`camera.cpp` `HookQfpsUpdate`)  
   When the orbit is idle (not in melee, not returning, not blending), only runs the game camera and a light yaw/pitch track. Skips pose mirror, `FollowPlatform`, `Orbit`, blends.

3. **`IsCamCharacter`**  
   Matches against known camera slots first; avoids probing random addresses.

4. **Aim-walk hooks** (`aimwalk.cpp`)  
   Early-out when no leg layer is active / fading / remote.

5. **Melee action-camera hook**  
   Checks “motion started this frame” before heavier character tests.

6. **`HookPlayerPadUpdate`**  
   Skips inventory/raw-hold patches in MeleeCamera; only runs `AimWalkRemoteTick` when `Sync=1`. That tick times out
   partner copies whose packets stopped, so if `Sync` is turned off in the ini while a copy is still walking, this
   hook no longer times it out.

7. **`HookCtrlUpdate`**  
   Skips `OurCamera` / noAimRig work in MeleeCamera mode.

## Later changes (behaviour, not performance)

- **Per-player tables** (`game.cpp` `g_states`, `crosshair.cpp` `g_aim`): 8 entries instead of 4. Entries of pad
  objects and characters that are gone are never released, so the table filled up over restarts; a full table then
  reused entry 0 for every new lookup and the two local players took it from each other every frame (no frame time,
  no run toggle, no steering; the reticle of player 1 flickering). A full table now reuses the entry unused for
  longest.
- **Cover** (low and wall cover, `ch+0x2D8C & 0xF00`): the free camera is off, so the game's own camera and sticks
  apply, as with `FreeCamera=0`; the shoulder swap is reset there. The `CoverTurn` setting and the code behind it
  (`lowCover`) were removed: the game turns the crouch with the right stick itself now.
- **Crosshair=1 in wall cover**: `DirectHorizontal` also applies there, so the aim no longer waits for the stick to
  get past the arm's aim zone.
- **Laser hidden completely** (`crosshair.cpp`): four more colour entries of the laser object (`+0x250`, `+0x260`,
  `+0x280`, `+0x2B0`) are hidden with the three that were (no 2 cm beam at the barrel tip). In dark places the beam
  is drawn from the barrel line (`ch+0x2710` start, `ch+0x2720` end, copied to the laser object at `+0x210/+0x220`),
  so that line is kept in its direction and made half a millimetre long while the crosshair is on.
- **Grenade launcher with Crosshair=1** (item `0x10C`): it leaves along the barrel, not toward the shot point like
  bullets, so the shots landed up and to the left of the centred crosshair. Its crosshair is drawn where the barrel
  line ends (the world point of the type-3 crosshair is moved there for the draw only).
- **Defaults and `RE3Controls.ini`**: the ini the mod writes when it is missing is now the one with the short
  comments, and the defaults in `config.h` / `config.cpp` match it: `RunButton=RB`, `Shoulder=60`,
  `AimDistance=100`, `MeleeCamera=1`, `GrabCamera=1`, `CrosshairSize=0.5`, `NoColorFilter=1`, `[Keyboard]
  Run=Toggle`. An existing ini is never touched; a key missing from it takes these defaults.
- **Log**: only the startup log is left (version, ini path and settings of the first load, hook and patch lines).
  The temporary diagnostics and the lines written while playing were removed; reloading the ini does not log the
  settings again.
- **Docs**: `README.md` and `docs/` describe the new defaults, cover behaviour and version.

## Build

```sh
bash build.sh
```

Output: `build/xinput1_3.dll` — drop it next to the game exe (same as the stock mod).

Version string: **1.9.4**
