# MoHAA Reloaded: native Metal for Apple Silicon

**MoHAA Reloaded** brings [OpenMoHAA](https://github.com/openmoh/openmohaa) natively to modern hardware. This is its `metal` branch: a native macOS port for Apple Silicon Macs. It plays Medal of Honor: Allied Assault, Spearhead and Breakthrough with your retail game data, on existing MOHAA and OpenMoHAA servers.

| Branch | Renderer | Status |
|---|---|---|
| **`metal`** (this one) | Metal, macOS on Apple Silicon | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases) |
| [`vulkan`](https://github.com/pstngh/MoHAA-Reloaded/tree/vulkan) | Vulkan, Linux and Windows | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases) |

> [!IMPORTANT]
> **This is a fork, not the upstream project.** Problems with this port belong on [this repository's tracker](https://github.com/pstngh/MoHAA-Reloaded/issues) (say which branch: `metal` or `vulkan`), not upstream's tracker or Discord.

- **Metal only.** Hand-written Metal shaders, no OpenGL, no translation layers (no MoltenVK, ANGLE or similar).
- **Native macOS.** The window, keyboard and mouse are AppKit and GCMouse (raw mouse input), and the sound is the port's own mixer on Core Audio, playing MOHAA's sound as the PC original did. No SDL, no OpenAL.
- **Low input latency.** About 2 ms from reading the mouse to the screen, and smooth movement at any frame rate (`cl_movefps`, below).
- **Fast.** On the benchmark demo the CPU's work per frame is 0.205 ms against GL1's 0.89–0.95 (4.5× less), the 99th percentile 0.48 ms against ~2.3.
- **Same game online.** The network protocol, the server-side game code and the data formats are upstream's, except that backpedal and strafe move at the original game's speeds, and a server it hosts caps SMGs and rifles at the original's top speeds.
- **Leaning in the campaign.** Allied Assault's single-player campaign lets you lean, as multiplayer does (the original game didn't).

Needs an Apple Silicon Mac (M1 or later) with macOS 26 or later. Developed and measured on an M4 Mac mini with a 240 Hz display.

## Install

1. Download `openmohaa-metal-<version>-macos-arm64.zip` from [Releases](https://github.com/pstngh/MoHAA-Reloaded/releases) and unzip it.
2. **Clear the quarantine, once.** The app is signed ad hoc, not notarized by Apple, so macOS refuses to run the download until you do. In Terminal, type `xattr -dr com.apple.quarantine ` (with the trailing space), drag the unzipped folder onto the window and press Return:
   ```bash
   xattr -dr com.apple.quarantine ~/Downloads/openmohaa-metal-1.0.0-macos-arm64
   ```
   "Open Anyway" in System Settings isn't enough: macOS then runs a read-only copy that can't find your game data, and the game stops and shows this line for your folder.
3. Copy `openmohaa.app` and `mohlauncher.app` into your MOHAA folder, next to `main/` (`Pak0.pk3` to `Pak6.pk3`; `mainta/` and `maintt/` for the expansions). Upstream's [installation guide](docs/markdown/01-intro/01-installation.md#linux-and-macos-users) shows how to get the data from the GOG installer.
4. Start `mohlauncher.app`: [MoHAA Launcher](#mohaa-launcher). If the game folder is on an external drive or in Documents or Desktop, allow the access prompts (they come back after each update). `openmohaa.app` starts directly too: Spearhead and Breakthrough with `open openmohaa.app --args +set com_target_game 1` (or `2`) in Terminal, in the game folder.

Configs, saves, screenshots, demos and logs go into the game folder (`main/configs/omconfig.cfg`, `main/screenshots/`, …), as in the original game, or into `~/Library/Application Support/openmohaa/` if the game folder isn't writable. An older `main/configs/omconfig.cfg` in the game folder wins over the settings in Application Support: delete it to keep those. Started from MoHAA Launcher, everything goes into the game folder (below).

### MoHAA Launcher

`mohlauncher.app` starts the game for Allied Assault, Spearhead or Breakthrough, at its main menu (**Single Player**) or onto a server (**Connect**):

- **Servers:** the address, the server's password and the RCON password. The servers you join are kept in a list (**Bookmark** keeps one without joining), Forte FFA built into Allied Assault's.
- **Settings:** your name, the crosshair (with a preview), the resolution and the display mode (the fullscreen Space, exclusive or borderless fullscreen, a window). Only what you change in the launcher goes to the game, so what you change in the game stays.
- **Portable:** it writes only into the game folder. `mohlauncher.json` holds the servers and their passwords; the game is started with its home there (`fs_homepath`), so its configs, saves, screenshots, demos, logs and state files go there too. A config kept in Application Support is copied over once.
- It finds the game folder by its `main/Pak0.pk3`, beside it, and asks for it otherwise.

Its source is [`launcher/macos`](launcher/macos) (SwiftUI: `launcher/macos/bundle.sh` makes the app, `swift test` there checks its logic); `port/release.sh` puts it into the zip.

## Settings that matter here

The shortest input-to-screen time on the test Mac (240 Hz):

```
seta cl_movefps 100
seta com_maxfps 480
seta r_fullscreen 1
seta r_noborder 1
seta r_swapInterval 0
seta in_mouseRaw 1
seta m_filter 0
```

Change settings in the console (`seta name value`) or at launch (`--args +set name value`). Display settings take effect after `vid_restart`.

**Added by this port:**

| Setting | Default | What it does |
|---|---|---|
| `cl_movefps` | `100` | **Movement apart from the frame rate.** The commands the server runs come from their own clock, 85–125 a second, and the game renders at whatever `com_maxfps` sets. Every command is exactly 1000 / `cl_movefps` ms of movement (10 ms at 100, 8 at 125) at any frame rate. Your own movement and the world are drawn at each frame's moment, so the picture is smooth at any frame rate and a key shows on screen ~2 ms after the press (uncapped); shots are aimed where your crosshair was at the click. `0` is stock: one command per rendered frame. (The first 1.0.0 zip called it `cl_cmdRate`: change that line in your config.) |
| `cl_movesubgrid` | `1` | **Shots and jumps sooner.** With `cl_movefps` on, a fire click or a jump in the first half of a step gets a command of its own at the half step: at 125, a shot reaches the server in 1.7 ms on average instead of 3.6. `0` puts every command on the grid. |
| `in_mouseRaw` | `1` | `1`: raw mouse deltas through GCMouse, no macOS acceleration. `0`: AppKit's mouse events. |
| `r_borderlessWindow` | `0` | With `r_fullscreen 1` and `r_noborder 1`: `1` is a borderless window over the screen instead of a fullscreen Space (no slide animation when you switch apps). |
| `r_blackOutDisplays` | `1` | **Smooth fullscreen next to a busy screen.** While the game is fullscreen and in front, your other displays show plain black, as macOS's own fullscreen does when the displays share their Spaces. Otherwise anything moving on another screen (a browser, a video, a chat) makes macOS hold back the game's frames: at 480 fps next to a busy 4K, a fifth of them never showed. The black goes away the moment you Cmd+Tab out or minimize. `0` keeps the other displays visible. Configs saved by 1.0.0 builds from 2026-10-29 hold `0`: set it to `1`. |
| `cg_tauntsounds` | `1` | `0` mutes other players' taunts (the chat text stays, your own still play). |
| `r_depthClamp` | `2` | **No seeing through walls you lean into.** The game cuts away whatever is nearer the eye than 4 units, so leaning into a wall or a window opened it up; now its surface stays whole. `1` the map's own surfaces only, `2` also doors, props and models, `0` stock. Effects and smoke are still cut. A cheat setting, as on the `vulkan` branch: it stays `2` online. |
| `cl_nullbinds` | `0` | `1`: **the newest key wins** for forward and back, the two strafes and the two leans. Holding both no longer stops you: the key pressed last counts, and letting it go hands the move back to the one still held. Some communities count this as an unfair advantage (like Snap Tap keyboards): it's off unless you turn it on. |
| `cl_latencyStats` | `0` | `1` prints input-to-screen times while you play. |

**Upstream's, with new behavior or defaults:**

| Setting | Default | What it does here |
|---|---|---|
| `com_maxfps` | `125` (upstream `85`) | The render cap, exact now (240 is 240, not 250). With `cl_movefps` on, any value; `0` is uncapped. On a 240 Hz display **480** showed input ~1 ms sooner than 250. With `cl_movefps 0` it is also the command rate: stay at 250 or below. |
| `r_fullscreen`, `r_noborder` | `1`, `0` | `1` and `1`: the **fullscreen Space**, the cleanest mode (the other displays go black: macOS blanks them when the displays share their Spaces, `r_blackOutDisplays` covers them when each has its own). `1` and `0`: exclusive fullscreen at `r_mode`'s size. `0`: a window, which macOS composites (more latency). Every fullscreen mode goes direct to the display. |
| `r_swapInterval` | `0` | `0` shows each frame at once (it can tear). `1` is vsync: tear-free, about two refreshes later. |
| `m_filter` | `0` (upstream `1` on macOS) | `1` averages each frame's mouse movement with the frame before's: half of every movement shows a frame later. Configs saved by earlier builds hold `1`: set it to `0`. |
| `cl_ctrlbindings` | `0` | A setting of its own now: `1` turns on the Ctrl and Del key bindings (`ctrlbind`), `cl_altbindings 1` only the Alt and Ins ones (`altbind`). Upstream's `cl_altbindings 1` turned on both: if you use Ctrl bindings, set `cl_ctrlbindings 1` too. |
| `ui_crosshair` | `1` | The crosshair is drawn as boxes, by default the stock image's shape (white arms one pixel thick, four long, a gap of three; the image's dark pixel at each arm's inner end is left out): `cg_crosshair_length` (`4`), `cg_crosshair_gap` (`3`, from the center) and `cg_crosshair_thickness` (`1`) in HUD pixels (the screen's up to 1920×1080, scaled with the height above), and `cg_crosshair_color` (`RRGGBB`, `FFFFFF`), read when a map loads or on `vid_restart`. Centered to the pixel at any size, one draw a frame. `0` (the Options' Crosshair box) hides it. The teammate crosshair and `cg_crosshair`'s image are gone. |
| `com_busyWait` | `0` | `1` spins the CPU through the frame wait: ~0.05–0.1 ms sooner, for a full core. |
| `cl_maxpackets` | `125` (upstream `30`) | Every command in its own packet. |
| `s_speaker_type` | `0` | `1` is **headphone 3D** (Apple's HRTF): sounds behind, above or below you sound there. `0` is stereo, as the original pans. |
| `s_ambientvolume` | `1` | `0` turns the soundtrack off entirely: no song is opened or played. |
| `r_uselod` | `0` (upstream `1`) | `0`: soldiers and weapons at full detail at any distance. Not saved in configs: set it at launch or in `autoexec.cfg`. |
| `ui_console` | `1` (upstream `0`) | The console key works; the game still starts at the main menu. |
| `cl_playintro` | `0` (upstream `1`) | `1` plays the intro videos at startup. |

In exclusive fullscreen, anything animating on another display costs frames (about a fifth at 480 a second): keep other displays still, or use the Space.

## Online

- The server side is upstream's: this client plays on stock MOHAA and OpenMoHAA servers.
- Backpedal and strafe move at the original game's speeds (upstream OpenMoHAA's are 25% and 18% faster), in this client's prediction and on a server it hosts. Your movement then matches stock MOHAA servers and servers with the same fix ([openmohaa-server](https://github.com/pstngh/openmohaa-server)); on an upstream OpenMoHAA server, the server corrects you slightly while you backpedal or strafe, until upstream changes too.
- A player's top speed is truncated as MOHAA 1.11 does it, on a server this build hosts: at the default `sv_dmspeedmult 1.1` an SMG runs at 262 (upstream OpenMoHAA: 264) and a rifle at 257 (258); other weapons are unchanged. It isn't a setting. On other servers your speed is theirs.
- With `cl_movefps` on, a server receives one command per step, each exactly one step long: the steps a stock client at that rate sends on average. A shot or jump with `cl_movesubgrid` splits one step in two (4 + 4 ms at 125, as a stock client at 250 frames a second sends all the time).
- No GUID: this client sends no `cl_guid`, as the original game's clients don't. Servers that track players by GUID see you without one.
- No dedicated server is shipped (`-DBUILD_SERVER=ON` builds `omohaaded` for local tests). For servers, use upstream's builds.

## Building

You need macOS 26, Xcode with the macOS 26 SDK and its **Metal Toolchain** component (Xcode → Settings → Components, or `xcodebuild -downloadComponent MetalToolchain`), and CMake, Ninja and Bison 3:

```bash
brew install cmake ninja bison
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install"
```

```bash
cmake --build build
```

```bash
cmake --install build
```

The install signs `install/lib/openmohaa/openmohaa.app` ad hoc (the game libraries are inside it); don't change files inside the app afterwards. If CMake doesn't find Homebrew's Bison, add `-DBISON_EXECUTABLE="$(brew --prefix bison)/bin/bison"`; if the build says "Metal shader compiler not found", add `-DMETAL_TOOLCHAIN_BIN="$(dirname "$(xcrun -f metallib)")"`. CI builds exactly this on every push (`.github/workflows/macos-metal.yml`). The scripts in `port/` are the developer's benchmark, screenshot and latency tools for one machine.

## Looks and limits

- **Looks like GL1**, except on purpose: foliage drawn after walls (a few tie pixels), all of 32+ dynamic lights lighting the world (MOHAA lit nothing on arm64), doors and lifts lit by every dynamic light, models at full detail, walls and windows you lean into kept whole (`r_depthClamp`), the chat and game messages kept clear of the compass at every resolution, and `r_gamma` applied in the final pass instead of the display's gamma table.
- **Tested on one Mac.** Reports from other Macs and ProMotion displays are welcome.
- **Not notarized**, hence the quarantine step.
- **At low frame caps** (about 60 or less) with `cl_movefps` on, the game keeps a CPU core busy between frames to send each command on time.
- **Removed:** gamepads and joysticks, the `openmohaa://` link handler, the update checker; Windows, Linux and Intel Macs.

## More

- The full record of the port (every decision, measurement and session): [`docs/METAL_PORT_PLAN.md`](docs/METAL_PORT_PLAN.md).
- Upstream, its documentation and community: [openmoh/openmohaa](https://github.com/openmoh/openmohaa). Its guides in [`docs/markdown`](docs/markdown) apply here except for Windows, Linux, the launchers, the update checker, where configs go (above) and building.
- Thanks to the OpenMoHAA team for the engine (built on [ioquake3](https://github.com/ioquake/ioq3) and the F.A.K.K SDK), to JayRewd, whose ideas were measured and re-implemented here, and to notuern for the sub-grid.
- OpenMoHAA is GPL-2.0 ([`COPYING.txt`](COPYING.txt)). It is an independent project, not affiliated with or endorsed by Electronic Arts.
