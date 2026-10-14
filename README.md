# RE3Controls v1.9 for Resident Evil 5: source code

RE3Controls is a Resident Evil 5 mod that brings in Resident Evil 3 (2020) style controls: a free over-the-shoulder
camera, camera-relative movement, walking while aiming, a centred crosshair, one-button reload on the move, quick
turn, full keyboard/mouse support and more. This package is the complete source of the `xinput1_3.dll` published on
Nexus Mods as **RE3Controls v1.9**.

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

The build is reproducible. Built with zig 0.17.0, `build/xinput1_3.dll` is byte-identical to the DLL in the Nexus
archive `RE5_RE3Controls_v1.9.zip`:

```
SHA-256  E104CC826D1EA70E1AF4464C85F3BD5D09621F70BDC529FD1F5C154A43EE7CA6   (217 088 bytes)
```

`bash build.sh dev` makes a developer build (`build/dev/xinput1_3.dll`) with test tooling; it is not what is
published (see below).

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

Antivirus heuristics may flag it because it is an unsigned DLL with the name of a system DLL that hooks and patches
the game's code in memory. That is how this kind of game mod works (the same as other XInput/DirectInput proxy mods).

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
| `src/version.rc` | Version resource (1.9.0.0) |
| `src/devserver.cpp`, `src/devinput.cpp`, `src/d3d9_hook.cpp` | Developer test tooling only (see above) |
| `docs/` | The user READMEs shipped in the release archive (`@SHA256@` is filled in when packaging) |
| `third_party/minhook/` | MinHook 1.3.4 |

## Licence

RE3Controls is released under the [MIT License](LICENSE): you may use, change and share this code, also in your own
mods, as long as the copyright notice stays with it. `third_party/minhook/` keeps its own BSD 2-clause licence
(`third_party/minhook/LICENSE.txt`).
