# renderer_vulkan on Windows: test checklist

Nothing of the Vulkan renderer or of the new frame limiter has run on
Windows yet (the CI only builds it). This checks startup, presentation
(resizes, minimize, fullscreen, `vid_restart`, quit), the same with the
submit thread (`r_vkSubmitThread 1`) and the frame limiter's Windows wait.
About 30 minutes. Sound can stay on.

## Setup

1. Build: the `out-windows-x64` artifact of the commit to test (GitHub,
   pstngh/openmohaa-vulkan, Actions, the "Builds" run of that commit;
   kept 3 days). Unzip it into a new folder: `openmohaa.exe`,
   `renderer_vulkan.dll`, `game.dll`, `cgame.dll`...
2. Game data: the folder of a Medal of Honor: Allied Assault install, the
   one that contains `main\Pak0.pk3` (below `C:\MOHAA`).
3. The game writes to `%APPDATA%\openmohaa\main` (paste that into the
   Explorer address bar). Put the two `.cfg` files below there.
4. Write down: Windows version, GPU, driver version, CPU, the display's
   resolution and refresh rate, and whether G-Sync/FreeSync is on.

`vktest_life.cfg`:

```
// startup, a map, vid_restart with mode changes
wait 2000
vkinfo
set thereisnomonkey 1
devmap dm/mohdm1
wait 6000
finishloadingscreen
wait 3000
vkinfo
vid_restart
wait 5000
set r_mode 4
vid_restart
wait 5000
set r_mode 3
vid_restart
wait 5000
vkinfo
```

`vktest_limiter.cfg`:

```
// 20 s at each com_maxfps in a map, a frame log each
set thereisnomonkey 1
devmap dm/mohdm1
wait 6000
finishloadingscreen
wait 3000
set com_maxfps 60
vkframelog clear
wait 20000
vkframelog lim_60.txt
set com_maxfps 144
vkframelog clear
wait 20000
vkframelog lim_144.txt
set com_maxfps 165
vkframelog clear
wait 20000
vkframelog lim_165.txt
set com_maxfps 250
vkframelog clear
wait 20000
vkframelog lim_250.txt
set com_maxfps 0
vkframelog clear
wait 20000
vkframelog lim_0.txt
vkinfo
quit
```

## Runs

Open a command prompt in the build folder. After each run, rename
`%APPDATA%\openmohaa\main\qconsole.log` to the run's name (the next run
overwrites it). The console opens with the key below Esc.

1. **Startup and vid_restart** (`life.log`):

   ```
   openmohaa.exe +set fs_basepath C:\MOHAA +set logfile 2 +set developer 1 +set r_fullscreen 0 +set r_mode 3 +set r_allowResize 1 +exec vktest_life.cfg
   ```

   When the script is done (the third `vkinfo` in the console), by hand:
   drag a corner of the window around for a few seconds; maximize it and
   restore it; minimize it for 5 s (taskbar) and bring it back; in the
   console `set r_fullscreen 1`, wait 3 s, `set r_fullscreen 0`, then
   `vkinfo` and `quit`. Then once more, everything the same, with
   `+set r_vkFallbacks 8` added (presents without fences) (`life_fb8.log`),
   and once more with `+set r_vkSubmitThread 1` added instead (the submit
   thread, `life_thread.log`). `r_vkSubmitThread` is archived: the next runs
   need `+set r_vkSubmitThread 0` (or the run's name says `thread`).
2. **Frame limiter** (`limiter.log` and the five `lim_*.txt` files):

   ```
   openmohaa.exe +set fs_basepath C:\MOHAA +set logfile 2 +set r_vkFrameLog 1 +set r_swapInterval 0 +set r_fullscreen 1 +set r_mode -2 +exec vktest_limiter.cfg
   ```

   (`r_mode -2`: the desktop's resolution.) Don't touch anything for the
   2 minutes it runs; it quits by itself. Then once more with
   `+set r_vkSubmitThread 1` added (`limiter_thread.log`; rename the five
   `lim_*.txt` files of the first run before, e.g. to `lim_*_off.txt`).
   Look at the Task Manager's CPU column for openmohaa meanwhile if you
   can: about a third of a core at 250 (the wait spins the last
   millisecond of every frame, more when the timer woke it up late), one
   core uncapped. A whole core at 60 to 165 means the waitable timer
   (Windows 10 1803 and later) isn't precise on that system and the wait
   spins more to stay on time.
3. Optional, with the LunarG Vulkan SDK installed: run 1 with
   `+set r_vkValidation 2` (`life_val.log`, slower).

## What to look for

- Near the top of each log: `Vulkan device: ...` and `Vulkan present
  fences yes, view swizzle yes, stride 0 bindings yes, Bresenham lines
  yes, line stipple yes` (`present fences no` is fine where the driver has
  no `VK_*_swapchain_maintenance1`; with `r_vkFallbacks 8` it must say no).
- No crash, hang or black window at resizes, minimize/restore,
  fullscreen, `vid_restart` and quit; quit takes under a second or two.
- `vkinfo: now ...` says `N present fences, 0 retired swapchains`
  (N about 2 to 4); no `WARNING: Vulkan: the last present of a swapchain
  didn't complete` and no `Vulkan validation` line.
- In each `lim_*.txt` the line `# frame time ms:` has a median of
  1000 / cap: 16.667 at 60, 6.944 at 144, 6.061 at 165, 4.000 at 250
  (uncapped: whatever the machine gives, above 1000 frames a second where it
  can), a p99.9 within about 0.1 ms of it, and a max of a few ms at
  most (a frame whose work ran long; the frames after it start one period
  after it, none shorter). With `fps 1` the HUD's counter must never read
  above `com_maxfps` (it counts the presents, which the limiter holds one
  period apart). The old limiter gave 6.000 at 144 and 165.
- With `r_vkSubmitThread 1`: the line `Vulkan present mode: ...` ends with
  `5 swapchain images, 2 frames in flight, submit thread (r_vkSubmitThread)`
  (3 images with vsync), and the same as above: no crash, hang or black
  window at resizes, minimize/restore, fullscreen, `vid_restart` and quit.

## Send back

All the `.log` files and the ten `lim_*.txt` files (zipped), the notes of
setup step 4, and for anything that looked wrong: what you did, what you
saw, a screenshot or phone video if it was visual. The `vkinfo` output is
in the logs.
