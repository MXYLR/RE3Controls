# RE3Controls for Resident Evil 5

This is a modification of the mod created by palecvzhope and is based on version 1.9.
The original mod can be found here: https://www.nexusmods.com/residentevil5goldedition/mods/1141

RE3Controls is a Resident Evil 5 mod that brings in Resident Evil 3 (2020) style controls: a free over-the-shoulder
camera, camera-relative movement, walking while aiming, a centred crosshair, one-button reload on the move, quick
turn, full keyboard/mouse support and more. 

Target: **Resident Evil 5 Gold Edition, Steam, version 1.2.0** (`re5dx9.exe`, 32-bit). The mod uses fixed addresses of
that build and checks the code bytes at each address before it hooks or patches anything; on any other build it
leaves that part alone and writes a line to its log.

## Building

Requirements: [zig](https://ziglang.org/) 0.13 or newer (it brings clang, lld and the mingw headers) and a bash (Git
Bash is fine). No Visual Studio or Windows SDK is needed.

```sh
bash build.sh                          # release build -> build/xinput1_3.dll
ZIG=/path/to/zig.exe bash build.sh     # if zig is not on PATH
```

`bash build.sh dev` makes a developer build (`build/dev/xinput1_3.dll`) with test tooling. It is not the release
build (see below).

## How it works

- **Loading.** The game imports `xinput1_3.dll`. The mod is a proxy with that name placed in the game folder: it
  loads the system XInput (`%WINDIR%\System32\XInput1_4.dll`, or `XInput9_1_0.dll` from the same folder) and forwards
  all XInput calls to it (`src/xinput_proxy.cpp`, exports in `src/xinput1_3.def`).
- **Hooks.** Game functions are hooked with [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4
  (`third_party/minhook`, unmodified, BSD 2-clause licence in `third_party/minhook/LICENSE.txt`). A few single
  instructions or call targets in the game's own code are patched in place with `VirtualProtect` (for example the aim
  sway and recoil gates, the colour filter switch, the crosshair size). Every hook and patch checks the expected
  bytes first.
- **Files.** It reads `RE3Controls.ini` next to the DLL, writes a default one if it is missing, and writes
  `RE3Controls.log` next to the DLL. It touches no other files and no registry keys.
- **Network.** None of its own. In online co-op it sends small sync packets through the game's own Steam P2P
  channel (`src/net.cpp`) so the partner sees the camera-relative movement and the aim point.
- **Nothing else.** No downloads, no other processes, no code injection into other programs, no obfuscation, no
  packing. The release build is stripped of debug symbols only to keep it small.

## Release and developer code

`RE3C_DEV` (set only by `bash build.sh dev`) guards all test tooling: a local TCP inspection server on 127.0.0.1, a
shared-memory bridge with a virtual gamepad, keyboard and mouse, screenshots and an auto-skip of cutscenes, all for
automated testing on the author's machine. The release build compiles without `src/devserver.cpp` and
`src/d3d9_hook.cpp`, `src/devinput.cpp` compiles to nothing, and `DevMode()` is a compile-time `false`, so none of
that code is in the published DLL.

## Files

| Path | Contents |
|---|---|
| `src/main.cpp` | DLL entry, log, start-up (waits for the game's code, then installs the hooks) |
| `src/xinput_proxy.cpp`, `src/xinput1_3.def` | The XInput proxy (forwards to the system XInput) |
| `src/config.cpp`, `src/config.h` | `RE3Controls.ini`: default text with comments, parsing |
| `src/game.cpp` | The controls: gamepad layout, free-camera movement, run, reload, quick turn, prompts |
| `src/camera.cpp` | The free orbit camera, aim camera distance and field of view |
| `src/aimwalk.cpp` | Walking while aiming (a lower-body animation layer) |
| `src/crosshair.cpp` | Centred crosshair, shot convergence, aim speed |
| `src/kbinput.cpp` | Keyboard and mouse support |
| `src/inventory.cpp` | Walking with the inventory open |
| `src/melee.cpp` | Free camera during melee attacks and enemy grabs |
| `src/damage.cpp`, `src/balance.cpp` | Optional damage, fire rate, move speed, shotgun spread, colour filter |
| `src/net.cpp` | Co-op sync packets |
| `src/re5.h`, `src/common.h` | Game structure offsets and helpers |
| `src/version.rc` | Version resource |
| `src/devserver.cpp`, `src/devinput.cpp`, `src/d3d9_hook.cpp` | Developer test tooling only (see above) |
| `docs/` | The user READMEs shipped in the release archive |
| `third_party/minhook/` | MinHook 1.3.4 |

## Licence

RE3Controls is released under the [MIT License](LICENSE): you may use, change and share this code, also in your own
mods, as long as the copyright notice stays with it. `third_party/minhook/` keeps its own BSD 2-clause licence
(`third_party/minhook/LICENSE.txt`).

---

## Additions in this fork (MXYLR)

Beyond upstream v1.9 and notencio's v1.9.4 fixes, this build adds:

### Shoulder camera

| Input | Key |
|---|---|
| Keyboard | **B** (`[Controls] ShoulderKey=B`, any letter A-Z) |
| Gamepad | **RB** (**L3** when `RunButton=RB`) |

- The swap works **at any time**, aiming or not (`[Camera] ShoulderSwapAlways=1`).
  Set it to `0` for the old behaviour (swap only while aiming, RB acts as the
  knife outside the aim).
- Outside the aim the knife stays on the physical **LB**, so giving RB to the
  shoulder does not remove the knife.
- In a scope the key is the `ScopeWalk` zoom button instead.
- One side flag for everything (upstream semantics): the orbit camera and the
  game's own aim/look-at poses always flip together, so the character keeps the
  same side while walking and aiming. Per-character defaults were removed again:
  they kept disagreeing with the side the game itself uses.

### Plain-gun zoom

While aiming a gun (not a scope) the keyboard zooms:

| Key | Action |
|---|---|
| **F** | zoom in (one step) |
| **C** | zoom out |

`[Aim] ZoomKeys=1`, `ZoomFovStep=-14` (fov degrees per step), `ZoomMaxStep=2`.
The level resets when the aim ends, the sniper scope keeps its own zoom, and
gamepad players are unaffected.

### Earlier fixes kept in this fork

- The sniper scope no longer has its mouse input zeroed by the mod (it is drawn
  by the weapon camera, which the mod does not control).
- Scoped walking is on by default (`ScopeWalk=1`).
- The quick knife slash can be turned with the mouse / right stick.
- Aim pitch is inherited from the free camera, and the view no longer snaps back
  to the pre-aim pitch after aiming.
Note: `[Camera] ShoulderGameLeftKinds` (bit per character kind, **empty by
default**) tells the mod which side the GAME itself aims with. RE5's own aim puts
the character on the **right** for every character, so the list is empty unless a
character differs. The aim camera is mirrored only when the wanted side differs
from that default, so the view no longer changes side between walking and aiming.