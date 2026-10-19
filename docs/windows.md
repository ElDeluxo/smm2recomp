# SMM2 Recomp on Windows

The SMM2 Recomp (`recomp/`, see [recomp/README.md](../recomp/README.md)) is Super Mario Maker 2 v3.0.3 itself,
recompiled from **your own dump** into a native Windows program that draws with Vulkan. The download contains no
game files. Its setup screen takes your dump, checks it, and recompiles the game code from it on your PC.

> **Status: built, not yet run on Windows.** The Windows package (`smm2recomp.exe` with its setup screen, Python,
> the recompiler, zig and hactool) is cross-compiled and packaged on macOS, and the release workflow builds it the
> same way. Nobody has started it on a Windows PC yet; the owner will be the first to test it.
>
> The same setup screen and first-launch recompile take the macOS app from an empty data folder to the title screen,
> and the Vulkan renderer boots to the title screen on Linux arm64 (Mesa lavapipe). The Windows-only parts have never
> run: the Windows platform layer, the DLL loading and the link against the exe's import library.
>
> Expect problems. Please report them with `smm2recomp.log` (next to the exe).

## What you need

- **Windows and hardware:**
  - Windows 10 or 11, 64-bit.
  - A CPU with AVX2: Intel Haswell (2013) or newer, AMD Zen or Excavator or newer.
  - A graphics driver with **Vulkan 1.3**. Current NVIDIA, AMD and Intel drivers have it.
- **Your own dump of Super Mario Maker 2 with the 3.0.3 update**, either of:
  - the game's **NSP or XCI** and the **3.0.3 update's NSP** (NSZ / XCZ must be decompressed first), or
  - folders with the extracted files: the **3.0.3 ExeFS** (`main`, `sdk`, `rtld`, `main.npdm`) and the RomFS (the
    base game's and the update's, or an emulator's "Extract RomFS" of the updated game).
- **Your console's `prod.keys` and firmware** (the folder of its system `.nca` files, for example Ryujinx's
  `%APPDATA%\Ryujinx\bis\system\Contents\registered`). The game's fonts come from the firmware; it stops at the title
  screen without them.
- Optionally **Super Mario Maker (Wii U)**, decrypted (base game and update v208), for the SMM1 mods.
- **About 4 GB of free disk** during the first launch (6 GB more when it extracts an NSP / XCI). The generated C and
  the objects are deleted afterwards; the game code DLL stays (about 190 MB).

Nothing else: Python, the compiler and hactool come with the download.

## Get it

Download `SMM2Recomp-windows-x86_64.zip` from the [releases](https://github.com/smm2recomp/smm2recomp/releases) and
unzip it somewhere you can write to, such as Documents or the Desktop, not Program Files. The same folder holds your
saves, settings, the set-up game files and the log.

You can also build it yourself, from macOS or Linux (see [Build it yourself](#build-it-yourself-cross-compile-from-macos-or-linux)).

The zip contains:

- `smm2recomp.exe`, the game's runtime and its setup screen
- `SDL3.dll`, `libiconv-2.dll` and `libshaderc_shared.dll`. Vulkan itself comes with your graphics driver.
- `smm2recomp.dll.a`, the import library the game code DLL links against
- `python\`, a Python 3.12 of its own with the recompiler's packages (capstone, unicorn, lz4)
- `tools\`, the recompiler and the dump setup scripts; `gamelib-src\`, the headers the game code is compiled against
- `zig\`, the compiler; `hactool\`, for NSP / XCI and the firmware
- `README.txt`, `THIRD-PARTY.txt` and `licenses\`

## First launch: the setup screen

Start `smm2recomp.exe`. Until the game is set up it shows the setup screen instead:

1. **Your game.** Add your NSP / XCI files, or the folders with the extracted files ("Add NSP / XCI...", "Add
   folder...", or paste a path). Each shows what it is; a folder with the ExeFS shows its version.
2. **Your keys and firmware.** If Ryujinx, yuzu or a fork has them, they're filled in already; otherwise choose
   `prod.keys` and the firmware folder. They are read where they are and never copied. Key values are never shown or
   written to the logs.
3. **Super Mario Maker (Wii U), optional:** the folder with your decrypted SMM1.

Then press **Set up and play**. It:

1. checks the game files against the known 3.0.3 files (the build ids and SHA-256 of the ExeFS; an NSP / XCI is read
   with hactool and your keys, and extracted into `extracted\`), and lays them out in `extracted\` (links where
   Windows allows them, Developer Mode; else hard links on the same drive; copies as a last resort);
2. copies the console's shared fonts and a few system data archives from the firmware into `fonts\` and `sysdata\`;
3. recompiles the game code: the generator turns `extracted\exefs` into C, zig compiles it with one job per CPU
   thread, and links `extracted\smm2game.dll` against the exe. This takes from a few minutes to half an hour; on
   Linux the same build took 29 minutes in an 8-core VM, on an M4 Pro Mac about 20 minutes with 6 jobs;
4. starts the game.

Every step shows its progress, and a failure says what went wrong and what to do (the details are in `logs\` and
`smm2recomp.log`). Later launches start the game straight away. To change the files later: the overlay menu (F1 or
Esc) > Mods > Game files, or start `smm2recomp.exe --setup`.

The setup screen also checks GitHub for a newer release and says so, with a link to its page. Nothing is downloaded
or installed by itself; turn it off with "Check for updates".

## Play

| Keyboard | Switch control |
|---|---|
| WASD | left stick |
| arrows | d-pad |
| K / J / L / I | A / B / X / Y |
| Q / E | L / R (the title screen asks for L + R) |
| 1 / 3 | ZL / ZR |
| Enter / Backspace | Plus / Minus |

Controllers work through SDL. F1 or Esc opens the overlay menu, with its Controls, Video, Audio and Mods tabs.

Useful options, as environment variables:

- `LARECOMP_WINDOW=1280x720` for a window instead of fullscreen.
- `LARECOMP_GPU=<index>` to pick a graphics card. The log lists them.
- `SMM2_SEED=4` for the same title course on every start.
- `RECOMP_BUILD_JOBS=<n>` to recompile with fewer compiler jobs (a slower machine, or less memory: each takes up to
  about 1 GB).

## Build it yourself (cross-compile from macOS or Linux)

The release is built with zig (the compiler the package also ships) and MSYS2's UCRT64 libraries:

```sh
recomp/tools/fetch_zig.sh                         # build/zig: the compiler for this machine
recomp/tools/fetch_deps.sh windows-x86_64         # build/deps-windows-x86_64: MSYS2's SDL3, Vulkan, shaderc, libiconv (pinned)
make -C recomp TARGET=windows-x86_64 GAME=smm2 DEPS="$PWD/recomp/build/deps-windows-x86_64" package
```

`package` builds `build/smm2-windows-x86_64/smm2recomp.exe` (2.6 MB) and its import library (the eight runtime symbols
the game code calls: `runtime/platform/windows_exports.def`), fetches the Python, zig and hactool for Windows
(`tools/fetch_python.sh`, `tools/fetch_zig.sh`, `tools/build_hactool.sh`), checks the folder for anything from a game
dump (`scripts/check_package.py`) and writes `dist/SMM2Recomp-windows-x86_64.zip` (113 MB, 425 MB unpacked).

To also build the game code on the Mac from your own dump (for testing; never share it):
`make -C recomp TARGET=windows-x86_64 GAME=smm2 DEPS=... gen all` writes the x86-64 game code to `generated/smm2-c`
(FP/SIMD lowered to portable C, `--simd c`; linux-x86_64 shares it) and `build/smm2-windows-x86_64/smm2game.dll`
(186 MB, plus a 440 MB `.pdb`).

## Troubleshooting

- **"needs a CPU with AVX2"**: the game code is compiled for x86-64-v3, so older CPUs can't run it.
- **The setup screen says a file can't be used / the version is wrong:** it names the problem. The recomp needs
  exactly 3.0.3: add the update's NSP (or its extracted ExeFS) next to the base game.
- **"hactool failed" / no system titles in the firmware:** `prod.keys` must be your console's, dumped after the
  firmware the game and the firmware folder need (a current Lockpick_RCM).
- **"The game code didn't build"**: the screen names the failing step, and `smm2recomp.log` has the compiler's last
  line. Make sure about 4 GB are free. It starts over at the next launch.
- **The game stops at the title screen** with a crash in the log: the fonts are missing (choose the firmware again:
  overlay menu > Mods > Game files).
- **A black window or a Vulkan error at start**: update the graphics driver to one with Vulkan 1.3. On laptops with
  two graphics cards, set `LARECOMP_GPU=<index>`.
- **The first seconds of a new scene stutter**: shaders are translated and compiled the first time they're used, and
  cached in `cache\` afterwards.
- **Known difference from the console**, seen on Linux and so expected on Windows: in the 3D World title course, the
  Mario model is missing from the Vulkan renderer's frames. The Metal (macOS) renderer draws him, and everything else
  matches.

SMCraft (the earlier clean-room engine) and its Windows build `mmk-play.exe` are archived on the `smcraft-archive`
branch, with their instructions.
