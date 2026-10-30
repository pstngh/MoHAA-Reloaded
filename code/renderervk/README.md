# renderervk — Vulkan renderer

`renderer_vulkan` started as a copy of upstream's GL1 renderer
(`code/renderergl1`) with every OpenGL call removed. All GPU work goes through
the interface in [`vk.h`](vk.h), which the `vk*.c` files implement with
Vulkan. Since 2026-10-23 it is the only renderer: upstream's GL1 and GL2
renderers and every other piece of OpenGL are gone from this branch (see
Implementation notes, No OpenGL). `cl_renderer` is `vulkan`; another name
loads `renderer_<name>` from the binary's directory (the tools' copies of
other builds) and falls back to `vulkan` when there is no such library, and
the `opengl1` of a config of an earlier build is reset to `vulkan`. On a
system without Vulkan (no loader, driver or device) it stops with an error
that says so.

Status (2026-10-14, branch `vulkan-renderer`): milestones 1 to 6 of the plan
below are done.

- the main menu matches GL1 pixel for pixel, except the animated ceiling fan
  (its frame depends on the wall clock), and so do the options pages, the
  weapon and team selection and the main menu of a game (`views/menus.txt`,
  but for a few pixels off by 1)
- with the deterministic setup described in [Testing](#testing), 3D views
  match GL1: world surfaces and lightmaps, static and animated models,
  alpha-tested and blended surfaces, decals, the view weapon, the sky, the
  sky portal and farplane fog, on `dm/mohdm1`, `dm/mohdm2`, `dm/mohdm4`,
  `dm/mohdm5`, `m2l1`, `m2l3` and `m4l1`, and in the milestone 6 parity
  pass on `dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`, `m1l1`, `m1l2a`, `m1l3a`,
  `m3l2`, `m3l3`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`, `m6l2a`, `m6l3a` and
  `obj/obj_team1` to `obj/obj_team4`: LOD terrain, the ocean and other
  water shaders, snow, night and fog maps, six more sky boxes (one with a
  cloud layer), sun flares, environment mapping, decals (91 views; the
  parity pass needed no renderer change). When both drivers rasterize with
  the same subpixel precision (a window wider than 4096 pixels) they match
  to the bit except for pixels off by 1 (usually under 0.3 % of the frame,
  up to 5 % when large polygons pass right next to the camera: the drivers
  clip against different guard bands; transient HUD text), a few isolated
  alpha-tested pixels, and what the game randomizes (snow and smoke
  particles, AI poses, swinging shutters). At 640x480, radeonsi uses a finer
  subpixel precision than RADV and 1 to 8 % of the pixels differ by 1
- fog and the user clip plane (portals and mirrors) are implemented
- the sun lens flare and its full-screen fade, the projected dynamic
  lights of `r_fastdlights 1`, cinematics, `TEXENV_ADD` (with a test
  shader: no shipped shader produces it) and `r_showtris`/`r_shownormals`
  match GL1 the same way (see [Testing](#testing) for the numbers); the
  dynamic lights of `r_fastdlights 0` are drawn per pixel, with a smoother
  falloff than GL1's texel-filtered dynamic lightmaps (an accepted
  difference, see GL1 quirks)
- the light flare depth test (`RB_TestFlare`, unreachable in OpenMoHAA, see
  [Flares](#flares)) reads the depth without waiting for the GPU; line
  stipple and GL's line rasterization rule come from
  `VK_KHR/EXT_line_rasterization`
- no validation or synchronization-validation message (`r_vkValidation 2`)
  during startup, the menu, a map load, `vid_restart` (also with a mode
  change and inside a map), window resizes (`r_allowResize 1`), `r_shadows 2`,
  `r_measureOverdraw 1`, `r_showImages 1`, texture mode changes, fogged maps,
  single-player maps, the sky portal, sun flares, dynamic lights, cinematics,
  debug drawing (with and without stipple) and shutdown; no validation
  message in any milestone 6 comparison run (`r_vkValidation 1`) and none
  with synchronization validation on `m3l2`, `m5l3`, `m6l2a`, `dm/mohdm7`,
  `obj_team3_deepbluesea` and `obj/obj_team4`; after the performance work,
  none either with synchronization validation for `mohdm1_dlight`, the
  main menu, `dm/mohdm1` with `r_ext_multisample 4` and `r_measureOverdraw
  1` (stencil readbacks every frame), live window resizes followed by
  their `vid_restart`, mode changes and a multisampling `vid_restart`;
  after the depth image change, none with synchronization validation for
  `mohdm1_dlight`, `m6l1a` and `r_measureOverdraw` turned on and off on
  `dm/mohdm1` (the depth image becomes readable, then not 300 frames
  later; overdraw counts equal to GL1's from the first frame), with 1 and 4
  samples; after the known risks' fixes (2026-10-18), none with
  synchronization validation through startup, map loads, `vid_restart`
  with mode changes, live resizes (a burst of 11 swapchains replaced),
  minimize and restore (also a simulated window without area, the Windows
  path), fullscreen toggles and shutdown, with present fences and with
  every fallback forced (`r_vkFallbacks 15`), and none in the 41 lists'
  runs of the build-against-build check (`r_vkValidation 1`); with the
  pipelines and skin data at load (2026-10-18), none with synchronization
  validation through startup, map loads and a map change, `vid_restart` in
  a map and with a mode change, a demo and shutdown; with the submit
  thread (2026-10-19), none with synchronization validation through
  startup, map loads, live resizes, minimize, fullscreen toggles, switching
  it on and off, mode changes and shutdown; with the redundant state
  skipped (2026-10-20), none with synchronization validation through map
  loads, a screenshot, `vid_restart`, a demo and 7 connects to a dedicated
  server (see Testing)
- performance (milestone 6, see [Performance](#performance)): on an idle
  machine Vulkan spends 13 to 28 % less main-thread time in the renderer
  than GL1 in the measured views at 1920x1080 (fence waits left out; after
  the second round, which took 6 to 24 % off: hidden symbol visibility,
  cheaper static model culling, cached sun traces, a faster draw surface
  sort), and in the `m6l1a` forest, where the GPU limits, its frames take 7
  % less time than GL1's at 3840x2160 and 5 % less at 1920x1080 (`m3l3`:
  13 % less at 1080p, CPU-bound; but 3 to 4 % more at 3840x2160 in
  `dm/mohdm3` and `m3l3`). Frames are drawn straight into the swapchain image (no copy
  at present, 8 to 9 % more frames per second where the GPU limits), the
  depth image is only readable while depth or stencil readbacks are wanted
  (a readable one has TC-compatible HTILE on RADV, which keeps fewer depth
  tiles compressed: 12 to 13 % more GPU time in that forest), textures
  share memory blocks, the pipeline cache is saved to disk, and with vsync
  the wait for the display happens after the present, like GL's swap.
  `r_ext_multisample` works (2 to 16 samples), 3 % slower than GL1's with 4
  samples (9 % before alpha was written with it)
- static geometry (2026-10-15, step 1 of a redesign of the back end): the
  world's faces, triangle surfaces and grids and the static models are
  uploaded once per map and drawn from device memory when their shader
  allows it, instead of being tessellated and copied every frame;
  consecutive static models with the same one-pass shader share a batch.
  The frames are the same to the bit as before in every view without
  random content; the renderer's CPU time goes down 15 to 20 % in the
  `obj_team2` yard and the `dm/mohdm3` overview (640x480 to 3840x2160, see
  Performance, Static geometry), with little change in the forests, whose
  static models mostly still need the CPU (steps 2 and 3, see Plan); the
  GPU time is unchanged
- dynamic lights per pixel (2026-10-16): the fragment shader adds the
  frame's lights to the lightmap instead of the CPU rebuilding dynamic
  lightmaps every frame; with 16 random lights the renderer's CPU time
  goes down 57 to 66 % in the `obj_team2` yard and the `dm/mohdm3`
  overview (both now at the 1000 fps limit at 1920x1080, from 559 and 487
  fps; GL1 479 and 447), with no measurable GPU cost at 3840x2160 (see
  Performance, Dynamic lights); views without dynamic lights are the same
  to the bit as before
- skeletal models on the GPU (2026-10-16, step 3, measured on new
  multiplayer views with 20 player models, see Testing): the vertex
  shader skins them and lights them (`rgbGen lightingSpherical`) with the
  CPU's floating point operations, so the frames are the same to the bit,
  and also collapses their vertexes at a lower level of detail; with a
  fix of the sun visibility trace cache, whose hash sent every entity on
  whole-unit coordinates to the same slot, the renderer's CPU time goes
  down 57 to 64 % in these views (1.00 to 1.41 ms to 0.39 to 0.56 ms at
  640x480; at 1920x1080 three of them now reach the 1000 fps limit, from
  652 to 838 fps, GL1 506 to 757, and the fourth runs at 772 fps instead of
  614, GL1 500), with the GPU time unchanged within 1 % at 3840x2160 (see
  Performance, Skeletal models); views without players don't change
- GPU time at 3840x2160 (2026-10-16 evening): where the GPU limits, the
  Vulkan renderer took 1 to 4 % more GPU time than GL1; the pipelines
  without blending now write alpha too (an RGB-only write to the BGRA
  swapchain image is a partial write), the depth image has no stencil
  plane when no stencil buffer is wanted, and RADV's clears to the fog
  color are drawn: 2.7 to 5.4 % less GPU time per frame in six of seven
  views (0.7 % in the seventh), faster than GL1 in all but the
  `dm/mohdm3` overview (0.6 % slower), with the same frames (see
  Performance, GPU time at 3840x2160). What is left is RADV not
  compressing textures
- frame spikes (2026-10-16 night): a frame log (`r_vkFrameLog`,
  `vkframelog`, off by default and free when off) records every frame's
  time, its parts and what the backend did, for a tester to send; demos
  of real matches played frame by frame compare both renderers on moving
  content. In two `obj` matches Vulkan's 99th and 99.9th percentiles are
  lower than GL1's at every size (1920x1080: 1.18 and 1.56 ms against
  1.92 and 2.65, 1.95 and 2.69 against 2.71 and 3.52), and the causes of
  the slow frames are ranked in Plan, Remaining (see Performance, Frame
  spikes)
- frame limiter (2026-10-17, an engine change outside this directory, for
  both renderers): `com_maxfps` is a deadline in microseconds that advances
  by one period instead of whole milliseconds since the last frame, so caps
  that don't divide 1000 are exact (144, 165 and 240 ran at 167, 167 and
  250 frames per second) and capped frames start evenly (the 99th
  percentile of the interval between frame starts 2 to 10 µs above the
  period, the old limiter's up to 83 µs above its whole milliseconds); the
  median time from a frame's input read to its present is the same or
  shorter at every cap with both renderers, the polled last millisecond of
  the wait costs 0.4 to 0.7 ms of a core per frame. Caps that divide 1000
  (125, 250, 500) start frames on the tick of the millisecond clock, as the
  old limiter did, so every frame there is the same whole milliseconds of
  game time (one usercmd): with the deadline in microseconds anywhere in
  the millisecond, up to half of the frames at 250 were 3 and 5 ms (see
  Performance, Frame limiter)
- present timing (2026-10-17, measurement only): when frames reach the
  display after their present, under KWin with Xwayland and as a native
  Wayland client, from the X11 completion events, Wayland's presentation
  feedback, the kernel's vblank timestamps and the framebuffer the primary
  plane scans out (`libinlat.so` with `INLAT_DISPLAY=1`,
  `framelog.py --display`). From input read to display at 1920x1080 on
  the 60 Hz display: fullscreen immediate (tearing) 1.5 ms uncapped and
  2.1 at 250, fullscreen mailbox (direct scanout, KWin bypassed) 4.6 to
  5.7 and 6.7 to 7.5, a window (composited) 6.6 to 7.3 and 8.6 to 8.9,
  FIFO 50.5 (three frames queued) and 71 in a window; Xwayland costs
  nothing measurable. The pacing changes that could lower it without
  adding lag, with their measured upside, are in Plan, Remaining (see
  Performance, Present timing)
- world culling (2026-10-17): starting from the Metal port hand-over's
  review of JayRewd's exact world culls, world faces wholly outside the
  view are left out (half of those GL1 draws in the `obj_team2` yard and
  `o2match`; not where dynamic lights reach), the BSP walk is replaced by the potentially visible leaves
  in its order, culled 8 at a time, the PVS is listed from a compact copy
  of the leaves (GL1's pass over every node took 0.23 ms at each cluster
  change in `obj/obj_team4`, 2 % of a match's frames) and culling a face
  reads only its `msurface_t`. The same leaves and surfaces as GL1's code
  (checked beside it, view by view, with a temporary build) and the same
  frames to the bit; the renderer's CPU time 2 to 10 % lower in the yard,
  4 to 5 % in `o2match`, 1 to 2 % in `o4match`, the same within the noise
  in the `dm/mohdm3` overview (see Performance, World culling)
- known risks before shipping (2026-10-18): presentation semaphores
  belong to their swapchain and go only once the presentation engine is
  done with them (present fences with `VK_KHR/EXT_swapchain_maintenance1`,
  otherwise the device wait that comes before), MoltenVK's portability
  subset has fallbacks for the view swizzle and the bindings of stride 0,
  lines without `VK_KHR/EXT_line_rasterization` were checked; every
  fallback can be forced (`r_vkFallbacks`) and gives the same frames, the
  input read to present time is unchanged, and Windows has a test
  checklist ([WINDOWS_TESTING.md](WINDOWS_TESTING.md); see Plan 15)
- pipelines and skin data at load (2026-10-18): the pipelines of a map's
  draws are created at the end of its load, worked out from the registered
  shaders as the stage iterators draw them, and a skeletal model's skin
  data (GPU skinning) when the model is registered, instead of at their
  first draw: with cold caches (the first play after an install) the 39
  and 49 pipelines that two matches created while they were played (60 and
  80 ms, up to 58 ms in one frame) now come at load (the map load 0.18 to
  0.20 s longer, 2 pipelines left in the match), the 99.9th percentile of
  the frame time goes from 2.08 to 1.56 ms and from 3.66 to 2.76 ms, the
  maximum from 34 and 58 ms to 21 and 29 ms (what registration in the
  match costs both renderers); after a driver update (the keys of what
  still came late are kept in `vkpipelines.keys`) and with warm caches
  none is created in the match and the load takes as long as before; the
  same frames, the same input to present time (see Performance, Pipelines
  and skin data at load)
- skeletal culling (2026-10-19): skeletal models wholly outside the view
  are no longer prepared and drawn (GL1 did, and the GPU clipped them): the
  sphere of the animation frame with a margin and the entity's axes,
  against the view's side planes, measured over 20 real matches and the
  41 view lists with a verification build that computed every posed
  vertex (no culled model had a vertex in view; JayRewd's rule, which this
  started from, hid splash drops in view and hasn't the margin for players
  aiming up or down). The renderer's CPU time goes down 6.5 and 8.5 % in
  `o2match` and `o4match` at 640x480 (-22 and -61 µs), 4 to 13 % in the
  soldier views (34 % in one that looks away from them), their 99th and
  99.9th percentiles too (`o4match` at 1920x1080: 1.631 and 2.134 ms to
  1.555 and 2.019), the input to present time a little; the same frames to
  the bit (see Performance, Skeletal culling)
- command rate (2026-10-19, an engine, client and cgame change, for both
  renderers): the client makes its usercmds, the steps of the player's
  movement, at a fixed rate (`cl_movefps`, `cl_cmdfps` until 2026-10-23,
  85 to 125 a second, default 100; `0` one per frame as stock since
  2026-10-24) instead of one per frame, the same steps at every frame
  rate, and the frame rate is
  no longer held at 1000 a second (`com_maxfps` above 1000 or 0); the view
  turns with every frame's mouse look. The server still receives `cl_movefps`
  steps. The client samples the current stock step with the keys held this
  frame, at `(milliseconds since the command + a sub-millisecond fraction) / interval`.
  The fraction is one monotonic read latched to `com_frameTime` (`Com_LatchRenderFraction`);
  it is not a `Pmove` input. From JayRewd's `cl_cmdfps` (omaha 1620f310) and his limiter
  change (b26d8a67), with commands on a fixed grid of server time (his steps
  depend on the frame rate), the look applied to the view only and no
  smoothing of the own position. Short steps get stuck on small bumps that
  longer ones get over: on `dm/mohdm2` 1 ms steps (1000 frames a second
  before) stopped 3 to 4 times as many walks as 8 to 12 ms, 4 ms (250) 1.6
  to 2 times as many, and no step got over every bump; why is in Performance,
  Command rate. The commands' steps are exact at 60 to 3300 frames a
  second, a walk makes the same moves to the bit at 144, 250, 1000 and
  uncapped, nothing is lost between commands, the input to display time
  at 250 and 1000 is the same as before; uncapped it is 0.32 to 0.41 ms
  lower where the GPU keeps up (640x480: 1800 to 2100 frames a second),
  higher where it limits (1920x1080 on this machine). Buttons and movement
  keys reach the server with the next command: 5 ms after the frame on
  average at the default, 2 at 250 frames a second before. The `fps`
  counter counts in microseconds
- submit thread (2026-10-19, `r_vkSubmitThread 1`, off by default): a
  thread submits and presents the frames and acquires the next frame's
  swapchain image right after each present, so the main thread no longer
  waits in `vkQueueSubmit`, `vkQueuePresentKHR` or an acquire that stalls
  (up to 1.8 ms in 15 to 67 of 15000 frames at 3840x2160 here, 1.3 ms
  every 341 frames on a tester's NVIDIA card); frames are drawn into the
  image the thread acquired, so they are not copied (JayRewd's version,
  from which this started, copied every frame: 23 to 30 % of the frame
  rate where the GPU limits), with 2 frames in flight as without it and 5
  swapchain images (3 with FIFO). Uncapped where the CPU limits, 3 to 21 %
  more frames a second and lower 99th and 99.9th percentiles in most
  views; where the GPU limits, the same frame rate and 1 ms less input to
  GPU done (the main thread waits for the GPU before the next input); with
  FIFO one frame less queued (33.8 against 50.5 ms to the display at 60
  Hz); at 250 frames a second the same frames and input to display within
  the noise, input to present 0.005 to 0.02 ms longer (the hand-off). The
  same frames to the bit (see Performance, Submit thread)
- redundant state and empty flushes (2026-10-20, from JayRewd's b26d8a67
  without its 2D batching): the setters of the viewport, the scissor, the
  matrices and the fog range return when the value doesn't change, and a
  flush of the render commands with none queued returns at once (each 2D
  call of the HUD flushes first: 120 of the 122 flushes of a match's frame
  were empty). The renderer's time per frame 4 to 7 % lower in two matches
  and the `dm/mohdm3` overview at 640x480 (-21 to -28 µs), 2 % in the
  `obj_team2` yard, the matches' 99th and 99.9th percentiles lower too, and
  at 1920x1080 their input to present 0.01 to 0.05 ms lower uncapped and
  at 250; the same frames to the bit. And a world loaded without `RE_BeginRegistration`
  (the client does that when it connects to another server from a game)
  started with the previous map's view cluster: when its first view was in
  a cluster of the same number, nothing of the world was drawn until the
  view left it (GL1 too); the view cluster is reset at load now (see
  Performance, Redundant state and empty flushes)
- effects shader (2026-10-20, step 2 of the redesign, from JayRewd's
  b26d8a67): the static batches whose shaders deform vertexes (the swaying
  trees, curtains and flags), compute texture coordinates (environment
  mapped windows and walls, scrolling and turbulent water) or the distance
  fade of the trees per vertex, and the static models lit by dynamic lights
  near them, are drawn from the static geometry by a vertex shader that
  computes what the CPU did, with the same float operations, instead of
  being tessellated and copied every frame. The renderer's time per frame
  11 % lower in the `m6l1a` forest at 640x480, 4.5 % in the `m3l3` path and
  the `dm/mohdm3` overview, 2 to 3 % in two matches, their slow frames and
  input to present a little lower, no GPU time measurable at 3840x2160; the
  same frames to the bit (see Performance, Effects shader)
- cgame caches (2026-10-20, a cgame change for both renderers, from
  JayRewd's b26d8a67): the ground trace of an entity's shadow is kept while
  its origin doesn't change, a temporary mark (the shadows of `cg_shadows`)
  made again within a server snapshot takes the fragments and verts it got
  before unless one is on a brush entity (a door moving under it within the
  snapshot shows its part from the next one), the active temporary models
  are counted when they are added and freed instead of every frame, and an
  unused conversion to Euler angles per sprite is gone. cgame's time per
  frame 2 to 4 µs lower in two matches (2 to 3.5 %); the same frames, also
  with shadows on, and the same results as before in 20 matches checked
  with a cgame that did both (see Performance, cgame caches)
- skinned passes (2026-10-20, from JayRewd's a5f5a54a): the generic
  iterator's skeletal model shaders whose stages are one texture or an
  environment map, lit by the sphere or with a constant color, are skinned
  in the vertex shader too (the Afrika Korps goggles, the tank commander,
  the Focke-Wulf's windows, bullet-hit debris, sniper lenses), the
  environment map's coordinates with the CPU's operations: the renderer's
  time 11 to 14 % lower where these models are drawn, nothing measurable in
  the two matches, the same frames to the bit (see Performance, Skinned
  passes)
- dynamic lights (2026-10-20, from JayRewd's b26d8a67): a scene keeps
  up to 64 lights (`r_maxDlights`, latched, default 4: the nearest the
  view; GL1 kept the first 32 added) with 64-bit masks, a draw lit by
  lights more than 32 apart in its list gets a list of them alone with
  their number (all 64 reach it), and brush models (doors, movers) get
  every light that reaches a lightmapped surface per pixel, as the world
  does, where GL1 lit them with light 0 only and projected the others
  (`ProjectDlightTexture`, no longer used with `r_fastdlights 0`). In real
  matches (never more than 4 lights a frame) the renderer's time is the
  same as before, the frames are the same to the bit where no brush model
  is lit by another light than its first and no scene has more than 4 (see
  Performance, Dynamic lights, 64 of them)
- terrain (2026-10-22, from JayRewd's b26d8a67): the LOD terrain is drawn
  from a copy of its vertex pool that each frame slot keeps in host
  visible memory, into which a frame writes the slots that changed since
  that copy was last written, instead of copying every terrain vertex twice
  every frame (no transfer, no barrier, no frame more in flight); and the
  tessellation's geomorph and merging go over the patches something can
  still happen to instead of every patch of the map, in the patches' order
  (JayRewd's order changed the tessellation; this keeps GL1's). The
  renderer's time per frame 7 to 8 % lower in views with much terrain at
  640x480 (`o3_beach` -57 µs), 3 % and 1.7 % in an `obj/obj_team1` and the
  `obj/obj_team4` match, the same in `o2match`; the same GPU time at
  3840x2160, the same frames to the bit (see Performance, Terrain)
- 2D batches (2026-10-22, from JayRewd's b26d8a67, `r_batch2D`):
  consecutive 2D pics, strings and triangles of one shader whose colors and
  texture coordinates come from each vertex alone are drawn as one surface,
  ended where anything else is about to use the renderer (his version ended
  it from every state setter of the backend, and joined any shader); and
  the boxes the UI's widgets draw with alpha 0, which change no pixel, are
  left out: in a match's frame 7.6 of 37 2D draws join a batch (the
  compass, the kill feed) and 37 of 40 boxes are left out. The renderer's
  time per frame 5 and 2.3 % lower in two matches at 640x480 (-16 and -14
  µs), their slow frames' percentiles too, 13 µs less GPU time a frame, the
  input to present time the same or lower; the same frames to the bit,
  merged and separate draws of one frame too (see Performance, 2D batches)
- light grid lookups (2026-10-22, from JayRewd's b26d8a67): the samples of
  the last 1024 light grid cells looked up, which every lookup decoded from
  4 run-length encoded columns, are kept, and so are the results of the
  last 1024 points by the point's exact bits (cgame lights every piece of
  debris and every smoke sprite with a lookup, and debris mostly lies
  still), in tables allocated with the world (his were dropped when the
  grid's address changed, which the next map's grid can reuse). 13 to 35 ns
  less per smoke or debris lookup, 8 to 12 µs less in the frames with 200
  or more of them, the 99th and 99.9th percentiles of cgame and the
  renderer 2 to 13 µs lower in three matches (0.3 to 1.6 %); the same
  results to the bit (see Performance, Light grid lookups)
- registration in the match (2026-10-22, a cgame and engine change, for
  both renderers): in multiplayer cgame registers at the load what a match
  loaded in the frame that first announced or used it: every player model
  of the game data with its first person model, the `cache` lines of the
  map's precache script (the server's own list; with `global/DMprecache.scr`
  the players' models and the weapons, then the map's items), the models of
  the map's entities and those its script names (the objectives' explosions
  and wrecks), the projectiles and turret view models these lead to, the
  health box a dying player drops, and the HUD's shaders and sprites
  (`cg_precache`); and the file system answers a lookup that a search
  directory can't satisfy from a listing of the directory, instead of
  correcting the path's case and failing an `fopen` (`fs_listDirs`). In 21
  real matches the 61 frames that loaded something (0.3 to 24 ms of it, up
  to 7 a match) and the 78 ms of the first frames after the loads are down
  to 7 frames, one of them a load (9.4 ms: named in a map script that this
  machine's mod pk3 replaced; it loads with the map since the mod's removal,
  2026-10-23); in the two
  measured matches the maximum frame time goes from 21.4 and 27.9 ms to 2.9
  and 5.4 ms uncapped (4.4 and 5.4 ms at 250 frames a second), the 99.9th
  percentile the same or lower, the share of registration in the slowest
  1 % of the frames from 18 to 27 % to 2 %; the map load 0.01 to 0.11 s
  shorter, 2 to 4 MB more texture memory in the same device memory (see
  Performance, Registration in the match)
- round changes (2026-10-23, an engine and cgame change, for both
  renderers): a round restart started the soundtrack again, whose mp3 the
  sound system scanned and decoded on the main thread, and changed 60 to 70
  configstrings, each copying the whole gamestate; a playing song decoded
  8 buffers in 8 frames in a row every few seconds. The soundtrack's mp3s
  are now read into memory when it loads and decoded ahead on a thread of
  the sound system, the same PCM (`s_streamThread`; since Plan 31 the Omaha
  mixer's worker decodes them, see the sound system below), the soundtrack
  file is kept (`s_soundtrackCache`), and a changed configstring goes after the
  others in the client's and cgame's gamestates (`cl_csAppend`,
  `cg_csAppend`). The round changes' frames take 1.0 to 1.4 ms less
  uncapped in two matches (`o4match` 3.1 to 3.8 ms to 1.9 to 2.4; 2.6 to
  3.0 ms less with a 3 minute song), their input to present 0.5 to 0.7 ms
  less at 250 frames a second, the 99.9th percentile the same or lower, and
  a match's first frames no longer decode the song's first buffers; the
  same frames, and the same PCM queued (see Performance, Round changes)
- busy moments (2026-10-23, the renderer and the engine's sound system):
  the slow frames of two matches profiled again; the terrain's
  tessellation leads in one of them. Its geomorph and merging leave out
  the patches where they would change nothing and a patch's indexes are
  kept until its triangles change (`r_terrainCache`; the same
  tessellation, checked over 21 matches), and each registered sound's
  OpenAL buffer (the Omaha mixer's sound since Plan 31) is made when the
  registration ends instead of in the frame that first
  plays it (`s_loadBuffers`: 131 to 146 first plays a match, 0.2 to 0.6
  ms each; 47.6 MB of buffers at the load). In `o4match` the 99th
  percentile goes from 1.52 to 1.37 ms uncapped and the mean 70 µs lower,
  input to present lower at every percentile (at 250 the 99.9th 2.22 to
  1.80 ms); `o2match` 0.03 to 0.06 ms lower; the slowest 0.1 % unchanged;
  the same frames (see Performance, Busy moments)
- the sound system (2026-10-23, Plan 31, an engine change for both
  renderers): JayRewd's pull request #2 replaces OpenAL with the Omaha mixer
  (miniaudio for the device, a mixer of its own on the device's thread, HRTF
  for headphones), which changes what the player hears: a gain per kind of
  sound (footsteps louder, weapons and explosions softer), overlapping and
  bursting sounds ducked, the music dipped under voices, sounds behind walls
  and closed doors muffled, a limiter; its tuning is kept as it came. Merged
  with Plans 29 and 30 redone for it, and improved without changing the
  mixed output: a walk of every alias at each sound's first play and the
  waits for its stream worker in the frame moved to the load or removed, the
  soundtrack's decoding off the main thread, device loss, a reused voice id,
  a `playmp3` crash, the HRTF set's lookup and the Windows package fixed;
  headphones mix at the HRTF set's 48 kHz, where its baked filters pass
  its own check, and filters designed from the measurements are made on a
  thread of their own, no longer on the mixer's;
  its obstruction traces (every playing world sound, every 100 ms) are left
  as they are. Played back faster than real time, as the timings are, the
  pull request costs `o2match` 0.06 ms of mean frame time and 0.2 to 0.3 ms
  at the 99th and 99.9th percentiles uncapped against OpenAL, mostly those
  traces, and Plan 31 takes 0.05 to 0.12 ms off the 99.9th percentiles;
  but the device plays the sounds in real time, so every sound stays
  playing 4 to 30 times longer in a match played back 4 to 30 times
  faster: at real speed (250 frames a second, two whole matches) the
  traces take 1.5 to 1.7 µs a frame on average and 0.05 to 0.14 ms in the
  slowest 0.1 % of frames, so the budget for them that Plan 31 added
  (`s_obstructionBudget`, off) was removed; input to present at 250
  within the runs' spread (see Implementation notes, Sound system, and
  Performance, Sound system)
- the surface list (2026-10-23, Plan 32, the renderer): JayRewd's pull
  request #3, which took 69 µs off the back end's surface list on its
  author's benchmark, merged and reworked: a stage drawn on the CPU takes
  the mesh's or the lightmap's texture coordinates from tess instead of a
  copy, a stage with one color for all its vertexes gets it as the
  constant color (its pipelines now built at the load: the pull request
  made 2 to 4 of them in a match's frames), and a skeletal surface's mesh
  comes from a table cleared at each map load (the pull request's was
  never cleared); its cache of the entities' lighting setup (which carried
  a setup from one draw list to the next and saved nothing measurable),
  its loop for the surfaces of one sort and its canary change were left
  out. 1.5 to 2.2 µs less a frame in the code it changes, within the
  runs' spread in the frame times; the same frames (see Performance,
  Surface list)
- defaults (2026-10-23, the engine and `renderer_vulkan`, as on the `metal`
  branch): `com_maxfps` 125 (upstream's 85), `cl_maxpackets` 125 (30: every
  command in its own packet), `m_filter` 0 on every platform (upstream's is
  1 on macOS; removed later that day, see below), `ui_console` 1 (0; the
  console opens at startup only in developer mode now), `cl_playintro` 0
  (1), and in `renderer_vulkan` `r_uselod` 0 (1: soldiers and weapons at
  full detail at any distance). `run.sh` and `perf.py` set `r_uselod 1`, so
  the numbers in this file (GL1's included) stay comparable; with the
  tools' settings the frames of `menus`, `hud`, `mp_mohdm1` and
  `dm/mohdm1` are the same as before but the Advanced menu's ticked
  Console box
- no OpenGL (2026-10-23, the user's request, as on the `metal` branch):
  upstream's GL1 and GL2 renderers, `qgl.h`, `sdl_glimp.c`, the GL build
  options, the WebGL (Emscripten) platform and the OpenGL code that was
  never built (vendored GL headers, a GameSpy sample, recastnavigation's
  demo, a map compiler's GL view) are deleted, the state helpers renamed
  (`R_State`, `R_Bind`, ...), and `cl_renderer` only loads
  `renderer_vulkan` (or a test copy); `tools/glcheck.sh` checks the source
  and the binaries (see Implementation notes, No OpenGL). The tools compare
  builds with each other (`compare.sh`, `cmplists.sh`); the GL1 comparisons
  in this file were made with the GL1 renderer of the builds up to
  53d8f9d7. The same frames as 53d8f9d7 (but in the lists that differ
  between two runs of one build)
- `cl_movefps` (2026-10-23): the command rate's cvar (`cl_cmdfps` until
  then) has the `metal` branch's name (85 to 125, default 100; see
  Performance, Command rate), and since 2026-10-24, as the user asked, its
  `0` as there: one command per frame, as a stock client, each step as long
  as its frame, and frames of a whole millisecond or more (at most 1000 a
  second, also above a cap of 1000 or uncapped)
- `m_filter` (2026-10-23): removed, as the user asked; the mouse movement
  of each frame is used as it comes (what `m_filter 0` did), and a config
  or the Controls menu's box that sets `m_filter 1` changes nothing
- the soundtrack off and taunts (2026-10-24, the user's, as on the `metal`
  branch): `s_ambientvolume 0` opens, decodes and mixes no song of the
  soundtrack (it only silenced them before), and `cg_tauntsounds 0` plays
  no taunt of another player (see Implementation notes, Sound system)
- `r_fastSync` (2026-10-24, Plan 34, the renderer): JayRewd's pull request
  #1 on MoHAA-Reloaded merged and reworked. `r_fastSync 1` (latched,
  archived, default 0) presents with mailbox instead of immediate when
  `r_swapInterval` is 0 (the default): no tearing, every refresh shows the
  newest finished frame; immediate where there is no mailbox. The pull
  request's test for a native Wayland window is gone: under SDL 2's
  default on a Wayland desktop, Xwayland, it left `r_fastSync 1` without
  effect, and mailbox works the same there, on X11 and on Windows.
  Fullscreen at the 60 Hz display it costs 4 to 6 ms of input to display
  (5.5 to 6.5 ms uncapped and 6.6 to 8.0 at 250, against tearing's 1.5 and
  1.9), and on RADV the acquire after the present waits up to 1 ms in 5 to
  17 % of the frames (never with immediate; the stall of immediate that
  the pull request's author saw on NVIDIA isn't there). The same frames,
  no validation message (see Performance, Present timing)
- no macOS (2026-10-24, the user's word: the `metal` branch is the macOS
  port, this branch was never built there): the macOS build (the `.app`
  bundle, code signing, the DMG, the AppleClang settings, Homebrew's Bison),
  `code/sys/sys_osx.m`, the macOS paths of the platform layer (`code/sys`,
  `code/sdl`) and the vendored macOS SDL are deleted. The portability
  tests of the shared engine and the libraries stay, and so does the
  instance's `VK_KHR_portability_enumeration` (generic Vulkan, MoltenVK's
  way in)
- SDL 3 (2026-10-24, Plan 35, the user's request: "a migration from the
  current SDL version to SDL 3.4.18 using the lowest latency APIs where
  applicable"): the window, input, events, threads and library loading
  use SDL 3 (3.2.0 or later; the downloads, CI and the vendored Windows
  headers and libraries are 3.4.18). On a Wayland desktop the game is now
  a native Wayland client (SDL 3's choice when the compositor has fifo-v1,
  as KWin does; SDL 2 ran it under Xwayland; `SDL_VIDEODRIVER=x11` still
  does). Relative mouse motion comes as floats and keeps its fractions
  (the same whole counts from a mouse and no motion lost, checked with
  injected motion), fullscreen takes the display mode closest
  to the video size as SDL 2's exclusive fullscreen did, and SDL 3 has no
  gamma ramps: `r_gamma` now brightens the textures on Linux (under
  Xwayland SDL 2's ramp had no effect: Xwayland ignores ramps), Windows
  sets the ramp itself as SDL 2 did. Fullscreen at 1920x1080 the input to
  display is 0.01 to 0.03 ms lower uncapped and the same at 250 within
  0.02 ms up to the 99th percentile; the event pump read the Wayland
  socket 6 µs later in each frame (SDL 3's D-Bus check and the flush of
  its frame callback came first), so input to present at 250 was 0.005 ms
  longer on the median while the time from that read to the present was
  the same (Plan 36 took that back, see the next item); without Xwayland
  the input reaches the game up to 0.04 ms sooner. The same frames as the
  build before, no validation message (see Performance, Present timing,
  and Implementation notes, SDL 3)
- SDL 3's event pump (2026-10-24, Plan 36, the user's request: "check if
  u can fix the SDL3 regression"): the frame's input read (the first
  `SDL_PollEvent`) no longer answers KWin's frame callback or polls
  joysticks. The frame limiter pumps SDL's events once when its polling of
  the last millisecond starts after a sleep, so SDL handles the callback
  (one per present, 1 ms after it) and sends its answer there, and
  `in_joystick 0` shuts SDL's joystick subsystem down (it polled udev and
  the devices in every pump). The frame's pump at 250 takes 3.1 µs on the
  median (SDL 3 before 14.1 to 14.6, SDL 2 7.8 to 8.2) and 8 to 10 µs at
  the 99th percentile (25 to 51, 14 to 24). Input to present at 250 is
  0.007 ms shorter than with SDL 2 on the median, 0.014 ms shorter than
  before, the same at the 99th and 99.9th percentiles within the noise;
  uncapped and input to display the same or lower. Mouse sums exact, the
  same frames, no validation message, no SDL patch (see Performance,
  Present timing, and Implementation notes, SDL 3)
- the internet server browser (2026-10-24, Plan 37, the engine's GameSpy
  code): JayRewd's pull request #3 on MoHAA-Reloaded merged and reworked.
  Servers are queried while the masters still send their lists, 32 at a
  time (two lists share them) instead of 4 to 15 from `rate`, a server
  that doesn't answer in 0.4 s is asked once more (1 s), and the status
  line counts the servers that answered and timed out. In real refreshes
  of the five public masters (about 220 servers listed, 132 answering)
  half the answers are in after 0.5 to 0.8 s and all after 1.3 to 2.3 s
  (before: 1.4 to 1.9 and 11 to 14 s), and the list is done after 5 s
  (before 32 to 36 s, the pull request 11 s). The masters' names are
  resolved on threads: name lookups on the main thread made frames of 36
  to 137 ms during a refresh before, and of up to 174 ms when one started
  with the pull request (all five at once); now a refresh has no frame
  over 5 ms but the first of a session (5 to 10 ms more: the process
  passes 64 open files, threads start). Fixed on the way: a master that
  can't be resolved or connected is skipped (it was tried again every
  frame and the refresh never ended), a master that accepts no
  connection is given up after 3 s instead of 10, "update server" on an
  idle list never ended, the LAN search never ended. CI runs the unit
  tests now (`cmake/tests/all.cmake` wasn't included)
- shots and jumps on the sub-grid (2026-10-24, Plan 38, the engine and
  cgame, as on the `metal` branch; the user's request): `cl_movesubgrid`
  (archived, default 1). With `cl_movefps` on, a fire click or a jump in
  the first half of a command's step gets a command of its own at the half
  step (5 ms after the one before at 100, 4 at 125), sent at once, and the
  commands after it stay on their grid: that one step is split in two.
  At 250 frames a second and `cl_movefps 100` a click reached a local
  server 2.6 ms after the press on average instead of 5.8 (95th
  percentile 6.6 instead of 11.4), a jump 2.9 instead of 4.7 (6.5 instead
  of 11.1); the server ran every command. A jump is early by the click's
  rule here (`metal` waits for the frame after the key's: SDL's key events
  carry the frame before's time). A shot or a jump at a random moment of a
  run into one of `dm/mohdm2`'s bumps stops it about as often as without
  (0 to 2 points more, 3 at one bump with 8 ms steps); where its two short
  steps meet the lip, up to 10 to 14 points more often. `0` keeps every
  command on the grid, `cl_movefps 0` runs none of it (see Performance,
  Command rate)
- the crosshair (2026-10-24, Plan 39, cgame and the renderer, as on the
  `metal` branch; the user's request): drawn as four boxes in one draw, by
  default the stock image's shape (`cg_crosshair_length` 4,
  `cg_crosshair_gap` 3, `cg_crosshair_thickness` 1, in HUD pixels, scaled
  above 1920x1080; `cg_crosshair_color` `FFFFFF`), read at a map load and
  on `vid_restart`; `ui_crosshair 0` hides it. The stock image, the
  teammate image of Spearhead and Breakthrough with the trace that chose
  between them in every frame, `cg_crosshair` and `cg_crosshair_friend`
  are gone. The frames are the build before's but for the stock image's
  four half-dark pixels at the arms' inner ends; the renderer's API is
  version 15 (`DrawBoxes`). See Implementation notes, Crosshair
- `cl_maxpackets` by the commands' own time (2026-10-24, Plan 40, the
  engine, as on the `metal` branch; found with Plan 38, the user's go):
  with `cl_movefps` on, a new command's packet waits only while its server
  time is less than 1000 / `cl_maxpackets` past the last packet's (a
  packet without a new command, and `cl_movefps 0`, count real time as
  before). Counted in real time, a command less than 8 ms (the default
  `cl_maxpackets 125`) after the packet before waited a frame: at 144
  frames a second, whose frames are 6.9 ms apart, half of all commands at
  the default `cl_movefps 100`, and at 250 with `cl_movefps 125` half to
  five sixths of them, after the time delta's corrections. Against a local
  server at 144 frames a second a forward press now reaches it 5.1 ms after
  the press on average instead of 10.0 (95th percentile 12.3 instead of
  17.5), a click 3.1 instead of 6.1; at 250 with `cl_movefps 125` forward
  3.7 instead of 5.5; no command waits any more; at 250 with
  `cl_movefps 100` nothing changes (see Performance, Command rate)
- the temporal path (2026-10-26, Plan 41, the renderer; the user's request:
  what modern ambient occlusion and anti-aliasing need first): with
  `r_temporal 1` (latched, off by default) the 3D views of a frame are
  drawn into an RGBA16F scene beside normals (from the draws; their readers
  derive the missing ones from the depth) and the motion vectors of what
  moves (moving entities, skeletal models' bones and morphs; their readers
  work out the camera's motion of the rest from the depth), with a depth
  pyramid of view distances (min and max) made when a reader needs it and
  the last frame's color and pyramid, then into the frame before the 2D
  drawing; `r_temporalJitter 1` jitters the projection (Halton 2, 3).
  Temporal anti-aliasing reads them since Plan 42 (below), ambient
  occlusion since Plan 44, with the helpers of
  `shaders/temporal_common.glsl`. With `r_temporal 0` the
  frames are the build before's to the bit. Cost on an NVIDIA GPU, GPU
  time a frame, in the user's `dm/mohdm6` view (a light one: 10700 frames
  a second): 0.043 to 0.121 ms at 2560x1440 (the first version 0.259),
  0.027 to 0.07 ms at 1920x1080 (0.143); the pyramid adds 0.04 ms at
  2560x1440 when it is made. See Implementation notes, Temporal path
- temporal anti-aliasing (2026-10-26, Plan 42, the renderer; the user's
  request: better anti-aliasing): with `r_temporal 1`, `r_taa 1` (archived,
  off by default, switched live) jitters the 3D views and blends each frame
  with the last frames' result, moved by the motion vectors, the history
  clipped to the colors around each pixel and dropped where the last frame
  didn't see the surface; `r_taaSharpen` (0.8) sharpens what is shown,
  `r_taaFeedback` (0.75) is the history's weight (the user's choice: fine
  at 60 frames a second, good at 125, excellent at 250). The 2D drawing is not
  touched. With `r_taa 0` and with `r_temporal 0` the frames are the build
  before's to the bit. This frame's color is made at each pixel's center
  from its 3 x 3 (the jitter doesn't show through, at any frame rate): in a
  still view 0.39 % of the pixels change between frames by more than 4 of
  255 (the jitter alone: 25.6 %; 1.25 % at the defaults). Cost on an NVIDIA
  RTX 4070 Ti, GPU time a frame, in the same `dm/mohdm6` view, after its
  optimization the same day (the user's request; the frames as before):
  the temporal path 0.043 to 0.123 ms at 2560x1440, with the
  anti-aliasing 0.344 ms (0.388 before); at 1920x1080 0.185 ms (0.227).
  The pass makes the level of the depth pyramid it reads itself, the
  filter's weights come from the CPU. The scene and the history stay
  RGBA16F, for HDR to come (8-bit and 10-bit formats were 0.055 ms less
  with a little more shimmer, and clip at 1). See Implementation notes,
  Temporal anti-aliasing
- HDR output (2026-10-26, Plan 43, the renderer; the user's request: SDR
  shown on an HDR display, the 3D view expanded toward its peak, the look
  set by cvars): `r_hdrOutput 1` (archived, latched: `vid_restart`, off
  by default) uses
  an HDR10 or scRGB swapchain when the window's surface offers one and SDL
  says the window is in HDR, on any Wayland compositor (the driver's WSI
  speaks `wp_color_manager_v1`) and on Windows alike, and changes mode
  when the window's HDR state changes; SDR otherwise, the console says
  why (`3` and `4` force HDR10 and scRGB, for tests; `2` is the HDR look
  in SDR, below). The frame is RGBA16F
  and goes to the swapchain through an output pass instead of the blit:
  the game's white at `r_hdrPaperWhite` nits (0: the system's SDR white,
  else 203): the brightness of the whole picture, the menus, the HUD and
  the 3D view, which the expansion starts from. With `r_temporal 1` the 3D view's bright parts are
  expanded toward `r_hdrPeak` (1000 nits; 0: what the system reports,
  often less than the display shows: 418 for the user's AW2725D, sold as
  a 1000-nit display), keeping their hue
  (`r_hdrExpand` 0.15, the user's choice after calibrating: a light lift
  of the bright parts, stacked effects above them toward the peak; `r_hdrExpandKnee` 0.1, the user's: the stock maps' sunlit
  surfaces are 0.15 to 0.45 in linear light, below the first version's
  0.6, which left all but the sky as they were; `r_hdrSkyBoost` 1,
  `r_hdrEffectsBoost` 1); `r_hdrBlack` (0) deepens or lifts the darkest
  shades only (the menus and the HUD too); screenshots and video frames
  are saved as SDR. Calibration as in other HDR games, in game and in
  the menus, without cheats: `r_hdrOutputTest 1` opens the black screen
  (a near-black box, a slider on `r_hdrBlack` until it is barely
  visible, Next), then the peak screen (a box on the largest signal, a
  slider on `r_hdrPeak` until the box is gone, Finish); with the HDR look
  (`r_hdrOutput 2`) the black screen, then the peak's with the white box
  scaled to SDR (SDR has no peak to find: a bright surface through the
  look's curve on SDR white, `r_hdrPeak` lifting it toward the white);
  it goes back to 0 at the end (2026-10-27, the user's request:
  sliders and buttons instead of typed cvars and values 1 to 3). With
  `r_hdrOutput 0` the frames are the build before's to the bit (with
  `r_temporal 1` and `r_saturation 1`). Cost on
  the RTX 4070 Ti, `dm/mohdm6`, 2560x1440, `r_temporal 1`: GPU time a
  frame 0.233 ms instead of 0.139, of which the output pass 0.034. The
  stock maps' lightmaps hold no range above 1 (their bytes stop at 127,
  254/255 after the overbright shift), so the range comes from the
  expansion and from stacked additive effects. See Implementation notes,
  HDR output
- the HDR look on SDR displays (2026-10-27, Plan 43, the renderer; the
  user's request: the same filter in SDR if a player wants it):
  `r_hdrOutput 2` (the user's choice: one cvar for the mode) with
  `r_temporal 1` expands the 3D view as HDR output would
  toward `r_hdrPeak` over `r_hdrPaperWhite` (1000 and 203 nits for 0) and
  puts it back into SDR with a soft shoulder from 0.5 (linear) to the
  expanded white, which stays white; `r_hdrBlack` sets the black level,
  the HUD is not touched. Bright mids get brighter, what is above white
  clips as in SDR (`dm/mohdm1`, the user's defaults: the sky's mean 170 to
  176 of 255, the other views up to 1). Off, the frames are the build
  before's to the bit
- the temporal path's images per reader (2026-10-27, Plan 45, the
  renderer; the user's request: each part of the path on only while
  something that reads it is): the anti-aliasing's history and the depth
  pyramid are made when `r_taa` is on (the pyramid also for
  `r_temporalShow`'s views that read it), the ambient occlusion's images
  when `r_ao` is, the last scene's color only for `r_temporalShow 5`, and
  freed when nothing reads them; the scene's color, motion vectors and
  normals stay (the HDR look reads the normals). At 2560x1440 about 270 MB
  with everything on, 44 MB with neither (`r_temporal 1` for the HDR look
  or `r_saturation`), 132 MB less with the anti-aliasing alone. A change of
  those cvars waits for the GPU once (a few milliseconds), the frames are
  the build before's to the bit. See Implementation notes, Temporal path
- `r_saturation` (2026-10-26, Plan 43, the renderer; the user's request):
  the 3D view's color saturation with `r_temporal 1`, in SDR and with HDR
  output (archived, default 1.1, the user's, in linear light about BT.709 luma, in
  the composite: the HUD keeps its colors; without `r_temporal` the HUD
  can't be told from the 3D view, so nothing). The frames of `r_temporal
  1` change with it (in `dm/mohdm6` at 1.125 67 to 95 % of the pixels, by
  up to 15); with `r_saturation 1` they are the build before's to the bit
- ambient occlusion (2026-10-27, Plan 44, the renderer; the user's
  request: AAA quality ground truth ambient occlusion): with
  `r_temporal 1`, `r_ao 1` (archived, off by default, switched live) adds
  GTAO (Jimenez et al. 2016, as Intel's XeGTAO does it): the contact
  shadows of corners, creases and what stands on a floor, from the
  horizons around each pixel in the depth, cosine weighted, with its own
  blending over frames (a noise that changes every frame, the history
  moved by the motion vectors, dropped where the depth says the surface
  wasn't there, clamped to what is around the pixel) and an edge-aware
  blur, so it is clean with and without `r_taa`. It darkens the opaque
  surfaces only: the primary view's render pass is broken after them and
  before the first blended surface, so smoke, muzzle flashes, glass and
  the lamps' glows in front of a corner keep their brightness; under fog
  it is applied to the surface's color before the fog, exactly. The
  normals are the depth's (`r_aoNormals 0`): the stock models smooth
  their normals across hard corners (the desks of `dm/mohdm6`), which
  then occluded themselves in black triangles. `r_aoQuality` 0 to 3
  (default 2: 3 directions of 3 samples a side), `r_aoRadius` (48 world
  units), `r_aoStrength` (1.5, a power), `r_aoFalloff`, `r_aoThickness`,
  `r_aoTemporal` (16 frames), `r_aoMultiBounce` (0.4); `r_temporalShow`
  9 and 10 show it. With `r_ao 0` (and `r_temporal 0`) the frames are
  the build before's to the bit. Cost on the RTX 4070 Ti, GPU time a
  frame in `dm/mohdm6` (the room at 448 -160 108), the temporal path
  0.162 ms: with it 0.96 ms at 2560x1440 (quality 0 to 3: 0.66, 0.76,
  0.96, 1.96), 0.53 at 1920x1080 (0.37, 0.42, 0.53, 1.09). The same day
  (the user's report: a wall right in front of the eye went black): the
  coarse levels of the view distances had never been made (0: a wall at
  the eye, which occluded what was nearer than the radius), and are now
  the mean of 1 / d at the texel's center; samples off the view are left
  out instead of clamped to its edge; the view distances are 32-bit
  again (in half floats a wall at a grazing angle went dark in stripes).
  Then (the user's reports: walking into a wall, the occlusion wobbled,
  its noise streaking with the movement keys) the blur's edges are worked
  out unquantized and the history is read from the 2 x 2 texels its
  weights are for (a gather's came back a texel off under a thousandth of
  a pixel's motion). Then a review and optimization (the user's request):
  the main pass's loops unrolled (slices and steps as specialization
  constants), the blend over frames and the blur in one pass: quality 2
  0.82 to 0.69 ms over the path at 2560x1440, 0.45 to 0.38 at 1920x1080,
  the frames the same but for one level in up to 4 % of the pixels.
  See Implementation notes, Ambient occlusion
- motion blur (2026-10-27, Plan 46, the renderer; the user's request: a
  velocity motion blur from the motion vectors and the depth): with
  `r_temporal 1`, `r_motionBlur 1` (archived, off by default, switched
  live) blurs the 3D view along each pixel's motion, the draws' motion
  vectors where something moves and the camera's from the depth elsewhere
  (McGuire et al. 2012's tiles and reconstruction filter, with Jimenez's
  2014 refinements: the depths compared softly, so what moves streaks over
  what is behind it and uncovers it, and nothing behind streaks over what
  is in front). The user's choices: the camera's motion blurs too
  (`r_motionBlurCamera`, 1; 0 only what moves); the blur is an exposure
  of a fixed time, the same at any frame rate (`r_motionBlurShutter` 0.5
  of a frame at `r_motionBlurFps` 60: 1/120 s; `r_motionBlurFps 0` the
  frame's own motion); the view weapon moves with the camera and blurs
  only by its own motion vectors, and nothing streaks over it.
  `r_motionBlurQuality` 0 to 2 (8, 12, 16 samples, default 1);
  `r_temporalShow` 11 and 12 show the blur. It runs after the
  anti-aliasing (whose history stays sharp) and before the sharpening's
  result is expanded for HDR; the HUD is not touched. With `r_motionBlur
  0`, and with it on in a still view, the frames are the build before's
  to the bit. Cost on the RTX 4070 Ti, GPU time a frame in `dm/mohdm6`
  at 2560x1440: 0.07 ms in a still view (its two passes), 0.14 turning
  (0.12 and 0.18 at quality 0 and 2), 0.21 turning with the
  anti-aliasing. See Implementation notes, Motion blur
- depth of field (2026-10-29, Plan 47, the renderer; the user's request:
  an implementation of depth of field): with `r_temporal 1`, `r_dof 1`
  (archived, off by default, switched live) blurs the 3D view by a thin
  lens' circle of confusion from the depth, focused on what is under the
  crosshair. The user's choices: autofocus at the crosshair, eased over
  `r_dofFocusSpeed` (0.25 s; `r_dofFocus` a fixed distance instead), a
  bokeh gather (Jimenez's 2014 one: the foreground blurs over what is in
  focus without a sharp outline, the background doesn't spread over what
  is in front of it), the Vulkan port only. `r_dofAperture` (0.5) is the
  far distance's blur in percent of the view's height when focused 128
  units away; a scope's zoom blurs more. The view weapon stays sharp.
  `r_dofQuality` 0 to 2 (16, 32, 48 samples a gather, default 1);
  `r_temporalShow 13` shows the blur. It runs after the anti-aliasing
  (whose history stays sharp) and before the motion blur. With `r_dof 0`
  the frames are the build before's to the bit, with and without the
  anti-aliasing, and with it on so is what is in focus. Cost on the RTX
  4070 Ti, GPU time a frame in `dm/mohdm6` at 2560x1440 with the
  anti-aliasing: 0.26 ms. See Implementation notes, Depth of field
- `renderer_vulkan.so` imports no GL symbol and no Vulkan symbol; it needs
  only SDL 3, libm, libstdc++, libgcc_s and libc. Vulkan is loaded at
  runtime through SDL
- GL1 was the visual reference until 2026-10-19; since then the look may
  change where that buys performance, and comparisons with the build before
  (until 2026-10-23 also with GL1) check for bugs and show what changed
  instead of requiring the same pixels

## Layout

| File | Role |
| --- | --- |
| `vk.h` | backend interface (the contract below) |
| `vk_local.h` | definitions shared by the backend files: `qvk*` function pointers, frame, texture and pipeline-key types |
| `vk_instance.c` | Vulkan loading through SDL, instance, validation messenger, device selection, swapchain, present fences and retired swapchains |
| `vk.c` | frames and command buffers, render targets, presentation, state recording, `vk_draw`, `vk_clear`, `vk_read_pixels`, the static geometry's buffer and the terrain pool's copies |
| `vk_image.c` | texture handles and storage, the memory blocks textures are placed in, samplers, descriptor sets, staging and ordering of uploads |
| `vk_pipeline.c` | pipeline cache keyed on the state that is baked into pipelines, pipeline creation, the `VkPipelineCache` file, the pipelines created before their first draw and the file of keys created after a load (`vkpipelines.keys`) |
| `vk_framelog.c` | the frame log (`r_vkFrameLog`, `vkframelog`): the per-frame exports timed, the backend's counters and GPU time of every frame, the dump (see Performance, Frame spikes) |
| `vk_submit.c` | the submit thread (`r_vkSubmitThread 1`): submits and presents the frames, acquires the swapchain image of the next frame after each present, copies a frame drawn offscreen (see Implementation notes, Submit thread) |
| `vk_output.c` | HDR output (`r_hdrOutput`): the window's HDR state (Vulkan surface formats, SDL), the swapchain's mode, paper white and peak, the 3D view's expansion parameters, the output pass (see Implementation notes, HDR output) |
| `vk_temporal.c` | the temporal path (`r_temporal 1`): the scene's images, render passes and pipeline layout, the depth pyramid, the composite into the frame (see Implementation notes, Temporal path) |
| `tr_temporal.c` | the temporal path's views and history: the scene begins and ends, the camera's and the entities' last frame, the skinned entities' last bones (see Implementation notes, Temporal path) |
| `tr_staticgeom.c` | the static geometry: world surfaces and static models uploaded at map load, and the back end batches drawn from it (see Implementation notes) |
| `tr_skin.c` | skeletal models skinned and lit on the GPU: the skin data of their surfaces (added when a model is registered), the frame data of each entity, the skinned batches (see Implementation notes) |
| `tr_pipelines.c` | the pipelines of a registration's shaders, worked out from their stages as the stage iterators draw them and created at its end (see Implementation notes, Pipelines) |
| `shaders/generic.vert`, `shaders/generic.frag` | the shaders of every pipeline; the vertex shader is built with and without `gl_ClipDistance`, the fragment stage is specialized per pipeline |
| `shaders/skinned.vert` | the vertex shader of skinned draws (skinning, `rgbGen lightingSpherical` and `tcGen environment`/`environmentmodel`), built with and without `gl_ClipDistance`; its outputs are `generic.vert`'s |
| `shaders/effects.vert` | the vertex shader of the static geometry's draws with effects (deforms, `tcGen` and `tcMod`, `distFade`, static models' dynamic lights; see Implementation notes, Effects shader), built with and without `gl_ClipDistance`; its outputs are `generic.vert`'s |
| `shaders/hiz_build.comp`, `shaders/taa_resolve.comp`, `shaders/temporal_composite.*`, `shaders/temporal_common.glsl` | the temporal path's passes after the scene (`hiz_build.comp` built three times: `PREPARE`, the default, `TAIL`; `taa_resolve.comp` the anti-aliasing) and what they and the passes to come that read its buffers share (included); `generic.*`, `skinned.vert` and `effects.vert` are also built with `TEMPORAL` defined for its scene |
| `shaders/output.frag` | HDR output's pass: the frame into the swapchain image, SDR, HDR10 (PQ) or scRGB, and the calibration screens' patterns; its vertex shader is `temporal_composite.vert` |
| `shaders/compile_shaders.sh` | regenerates `vk_shaders.c`, `vk_shaders_temporal.c` and `vk_shaders_hdr.c` with `glslc` (an argument, `main`, `temporal` or `hdr`, writes only that one) |
| `vk_shaders.c`, `vk_shaders_temporal.c`, `vk_shaders_hdr.c` | generated SPIR-V as C arrays (committed, so builds don't need `glslc`) |
| `tools/` | build-against-build screenshot comparison and performance scripts, view lists, the OpenGL check (see Testing) |
| `WINDOWS_TESTING.md` | a tester's checklist for Windows: commands, cvars, what to send back |
| `tr_immediate.c` | `glBegin`/`glEnd` emulation for debug and 2D drawing, emits indexed triangles or lines |
| `tr_backend.c` | renderer-side state tracking (`R_State`, `R_Bind`, `R_Cull`, `R_TexEnv`, matrices, vertex arrays, current color) |
| `tr_*.c`, `tr_*.cpp` | upstream's GL1 renderer with its GL calls replaced, otherwise unchanged but for the performance work (see Performance) and the static geometry hooks of the surface functions and stage iterators |
| `../sdl/sdl_vkimp.c` | SDL 3 window for Vulkan, video modes and fullscreen (`VKimp_*`) |
| `../sdl/sdl_gamma.c` | the hardware gamma ramp on Windows (`VKimp_SetGamma`; SDL 3 has none) |
| `../../cmake/renderer_vulkan.cmake` | build target `renderer_vulkan` (only `GetRefAPI` is exported: hidden symbol visibility) |

The state helpers had GL1's names until 2026-10-23 (`GL_State`, `GL_Bind`,
...); they are `R_State`, `R_Bind`, `R_Cull`, `R_TexEnv`, `R_SelectTexture`,
`R_EnableTexture`, `R_TextureMode`, `R_SetDefaultState` and `R_SetFogColor`
now, as on the `metal` branch (and `gl_filter_min`/`max` are
`tex_filter_min`/`max`). `glConfig` (a `glconfig_t`, whose layout cgame and
the UI share) and `glState` keep their names.

Shared files touched outside this directory: `CMakeLists.txt`
(`BUILD_RENDERER_VULKAN`, on by default, the only renderer option),
`cmake/libraries/{freetype,jpeg}.cmake` (the renderer's libraries),
`cmake/renderer_common.cmake`
and `cmake/client.cmake` (the renderer is always a library loaded at run
time), `code/renderercommon/tr_common.h` and `tr_types.h` (no `qgl.h` and
no GL declarations; `texnum` is `unsigned int`, same type as before), and
the CI workflows (`.github/workflows`: Linux x64 and Windows x64 only, the
renderer shipping as the `renderer_vulkan` library next to the executable;
artifacts kept 3 days; since 2026-10-23 the Windows job uploads only the
game's files, no symbols or installer; the unit tests build without the
renderer) and the Windows installer
(`misc/setup/windows/app/Package.wxs` lists `renderer_vulkan.dll`; CI no
longer builds it), and the client's `cl_renderer` (`vulkan`, the only
renderer: `code/client/cl_main.cpp`), and the removal of OpenGL (see
Implementation notes, No OpenGL), and `m_filter`'s removal
(`code/client/cl_input.cpp`, `cl_main.cpp`, `client.h`), and the soundtrack
off and taunts (`code/client/omaha/snd_omaha.cpp`, `code/cgame/cg_parsemsg.cpp`,
`cg_main.c`, `cg_local.h`), and the frame limiter
(`Com_Frame` and `Com_FrameWait` in `code/qcommon/common.c`,
`Sys_Microseconds` in `code/sys/sys_unix.c` and `code/sys/sys_win32.c`,
the timer slack set in `sys_unix.c`'s `Sys_PlatformInit`, `NET_SleepUs`
in `code/qcommon/net_ip.c`, their declarations in `code/qcommon/qcommon.h`:
`com_maxfps` caps in microseconds, those that divide 1000 on the
millisecond tick, see Performance, Frame limiter), and the command rate
(`code/client/cl_input.cpp`: `cl_movefps`, `CL_CreateNewCommands`,
`CL_ApplyLook`, `CL_GetViewUserCmd`, `CL_SendCmd`; `code/client/client.h`;
`code/client/cl_cgame.cpp` and `code/cgame/cg_public.h`: the cgame import
`GetViewUserCmd`; `code/cgame/cg_predict.c`; `code/cgame/cg_specialfx.cpp`;
`Com_Frame` and `Com_ModifyMsec` in `code/qcommon/common.c`: caps above
1000 and uncapped, frames of 0 ms, the `fps` counter in microseconds; see
Performance, Command rate), and cgame's caches (`code/cgame/cg_marks.c`:
temporary marks kept for the rest of a snapshot; `cg_modelanim.c` and
`cg_local.h`: shadow traces kept by origin; `cg_tempmodels.cpp` and
`cg_commands.cpp`/`.h`: the count of active temporary models; see
Performance, cgame caches), and the registration at load
(`code/cgame/cg_main.c`: `CG_PrecacheMatch`, `cg_precache`;
`code/cgame/cg_public.h` and `code/client/cl_cgame.cpp`: the cgame import
`CM_EntityString`; `code/qcommon/files.cpp`: `FS_DirMayHave`,
`fs_listDirs`; see Implementation notes, Registration at load), and the
round changes (`code/client/snd_codec_mp3.c`: memory streams and their
own dithering, `S_MP3_OpenMemoryStream`; `code/client/snd_codec.c`/`.h`:
`S_CodecLoad` opens the memory streams; `code/client/snd_dma_new.cpp`: the
cvars, `MUSIC_FlushCaches` at each load; `code/client/cl_cgame.cpp`:
`CL_ConfigstringModified`, `cl_csAppend`, the cgame import
`GetConfigString`; `code/cgame/cg_servercmds.c`: `CG_ConfigStringModified`,
`cg_csAppend`; see Implementation notes, Round changes), and the busy
moments (`code/client/snd_dma_new.cpp`: `S_EndRegistration`,
`s_loadBuffers`; see Implementation notes, Sound buffers at load), and the
sound system (JayRewd's pull request #2 and Plan 31: `code/client/omaha/`,
new, the Omaha sound host and mixer; `code/client/snd_host.cpp`/`.h`;
`code/client/snd_stream_memory.cpp`, new, the soundtrack's files in memory;
`code/client/snd_dma_new.cpp`, `snd_mem_new.cpp`, `snd_local_new.h`,
`snd_info.cpp`, `snd_codec_mp3.c`, `cl_uiview3d.cpp`, `snd_main.c`;
`code/client/snd_openal.c`, `snd_openal_new.cpp`/`.h`, `qal.c`/`.h` and the
OpenAL headers deleted; `code/qcommon/files.cpp`, `qcommon.h`:
`fs_shutdownHook`; `code/thirdparty/miniaudio`, `libmysofa`, `pffft` and
`OMAHA-SOUND-LICENSES.txt`, new; `assets/main/sound/prom/hrtf`, new: the
HRTF set; `cmake/client.cmake`, `cmake/libraries/{miniaudio,mysofa,pffft}.cmake`,
`openal.cmake` deleted; the CI workflows and the Windows installer without
OpenAL, with the HRTF set; see Implementation notes, Sound system), and the
crosshair (`code/cgame/cg_drawtools.cpp`: `CG_UpdateCrosshair`,
`CG_DrawCrosshair`; `cg_main.c` and `cg_local.h`: the cvars, its layout at
`CG_GetRendererConfig`; `code/cgame/cg_public.h` and
`code/client/cl_cgame.cpp`: the cgame import `R_DrawBoxes`;
`code/renderercommon/tr_public.h`: the export `DrawBoxes`,
`REF_API_VERSION` 15; see Implementation notes, Crosshair), and the
sub-grid (`code/client/cl_input.cpp`: `cl_movesubgrid`, `CL_SubGridStep`,
`CL_CreateNewCommands`, `CL_GetLiveUserCmd`, `CL_ReadyToSendPacket`;
`code/cgame/cg_predict.c` and `cg_public.h`: the open step as long as the
next command's; see Performance, Command rate).

## Build and run

Requirements: the Vulkan headers (1.4.341 on the dev machine; Ubuntu 22.04's
1.3.204 are too old), either Khronos' Vulkan-Headers installed as a CMake
package (`VulkanHeaders`, found with `-DVulkanHeaders_DIR=<prefix>/share/cmake/VulkanHeaders`
or `CMAKE_PREFIX_PATH`; the CI builds fetch and install the `v1.4.341`
tag) or, through CMake's `FindVulkan`, the system's headers plus the
loader development library, which `FindVulkan` needs to report Vulkan as
found even though the renderer doesn't link it (only `Vulkan::Headers` is
used). `glslc` is only needed to change shaders. `vulkan-validationlayers`
is needed for `r_vkValidation`.

```sh
cmake -S . -B build-vk -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-vk
cd build-vk/RelWithDebInfo
./openmohaa +set fs_basepath ~/Documents/moh \
    +set fs_homepath /tmp/moh-home +set r_fullscreen 0 +set r_mode 3
```

`renderer_vulkan` is the only renderer: `cl_renderer` is `vulkan`, and
another name loads `renderer_<name>` of the binary's directory (a copy of
another build's `renderer_vulkan.so`, as the tools use them; see Testing) or,
when there is none, falls back to `vulkan`; a name that starts with `opengl`
(a config of an earlier build) is reset to `vulkan`, also when an earlier
install left `renderer_opengl1` in the game folder
(`code/client/cl_main.cpp`).

Cvars of the backend:

| cvar | |
| --- | --- |
| `r_vkValidation` (latched, default 0) | 1 enables `VK_LAYER_KHRONOS_validation` and prints its warnings and errors to the console; 2 also enables synchronization validation (slow) |
| `r_swapInterval` (latched) | 0: immediate present mode (mailbox if unavailable), < 0: FIFO relaxed, > 0: FIFO; FIFO when the wanted mode is missing |
| `r_fastSync` (latched, archived, default 0) | 1: with `r_swapInterval 0`, mailbox instead of immediate (immediate if unavailable): no tearing, every refresh shows the newest finished frame, for the wait for the compositor's latch (Plan 34, see Performance, Present timing) |
| `r_ext_multisample` (latched) | 0 or 1: off; 2 to 16: samples (the largest the device supports up to that), see Implementation notes |
| `r_staticGeometry` (default 1) | 0 tessellates world surfaces and static models on the CPU every frame instead of drawing them from the static geometry (see Implementation notes) |
| `r_gpuSkinning` (default 1) | 0 skins and lights skeletal models on the CPU every frame instead of in the vertex shader (see Implementation notes, Skeletal models) |
| `r_skelCull` (default 1) | 0 prepares and draws skeletal models outside the view too, as GL1 does (see Implementation notes, Skeletal culling) |
| `r_maxDlights` (latched, archived, default 4) | 0 to 64: the dynamic lights a scene keeps, the nearest the view of those cgame added (GL1: the first 32 added to the frame); see Implementation notes, Dynamic lights |
| `r_staticEffects` (default 1) | 0 leaves the static batches whose shaders deform vertexes or compute texture coordinates or colors per vertex to the CPU (tess) instead of the effects shader (see Implementation notes, Effects shader) |
| `r_staticTerrain` (default 1) | 0 copies the LOD terrain's vertexes into tess every frame instead of drawing them from the terrain pool (see Implementation notes, Terrain) |
| `r_terrainCache` (default 7) | bits: 1 the LOD terrain's geomorph leaves out the patches whose vertexes need no new height, 2 its merging leaves out the patches where nothing changed since every merge in them failed, 4 a patch's indexes are kept until its triangles change; 0 walks everything every frame (the same tessellation either way, see Implementation notes, Terrain) |
| `r_batch2D` (default 3) | bits: 1 draws consecutive 2D pics, strings and triangles of one shader as one surface, 2 leaves out the boxes of `DrawBox` with alpha 0 (they change no pixel); 0 draws every 2D call as it comes (see Implementation notes, 2D batches) |
| `r_vkFrameLog` (latched, default 0) | the frame log: 1 records the last 131072 frames (larger values: that many), see `vkframelog` below and Performance, Frame spikes; 0 costs nothing |
| `r_vkSubmitThread` (latched, archived, default 0) | 1: a thread submits and presents the frames and acquires the swapchain images, so the main thread never waits in `vkQueueSubmit`, `vkAcquireNextImageKHR` or `vkQueuePresentKHR` (see Implementation notes, Submit thread, and Performance, Submit thread); the swapchain then has 5 images without vsync, 3 with FIFO. The startup line `Vulkan present mode: ..., N swapchain images, N frames in flight, submit thread` says what is used |
| `r_temporal` (latched, archived, default 0) | 1: the temporal path: the 3D views are drawn into an RGBA16F scene with motion vectors and normals, for temporal anti-aliasing (`r_taa`), ambient occlusion (`r_ao`), motion blur (`r_motionBlur`) and the HDR look; their own images (the depth pyramid, histories) are made only while they are on; off with multisampling (see Implementation notes, Temporal path) |
| `r_ao` (archived, default 0) | with `r_temporal 1`: ground truth ambient occlusion (GTAO) of the opaque surfaces, before what is blended over them (see Implementation notes, Ambient occlusion) |
| `r_aoQuality` (archived, default 2, 0 to 3) | with `r_ao 1`: directions and samples a pixel: 1 x 2, 2 x 2, 3 x 3, 9 x 3 a side (XeGTAO's low, medium, high and ultra; ultra blurs twice). GPU time over the temporal path at 2560x1440 0.43, 0.51, 0.69, 1.62 ms, at 1920x1080 0.23, 0.28, 0.38, 0.80 |
| `r_aoRadius` (archived, default 48, 4 to 256) | with `r_ao 1`: how far occluders reach, in world units |
| `r_aoStrength` (archived, default 1.5, 0 to 4) | with `r_ao 1`: the visibility to this power (1: as computed) |
| `r_aoFalloff` (archived, default 0.6, 0.05 to 1) | with `r_ao 1`: the share of the radius over which occluders fade out |
| `r_aoThickness` (archived, default 0.25, 0 to 4) | with `r_ao 1`: XeGTAO's thin occluder compensation: what is in front along the view fades out sooner (less dark halos behind thin objects) |
| `r_aoTemporal` (archived, default 16, 1 to 64) | with `r_ao 1`: the most frames blended (a quarter of it for what moves); 1: none, the noise stays put and the blur works alone |
| `r_aoNormals` (archived, default 0) | with `r_ao 1`: 0 the normals of the depth; 1 the draws' where they are within about 25 degrees of those (smoother curved models) |
| `r_aoMultiBounce` (archived, default 0.4, 0 to 0.9) | with `r_ao 1`: the albedo of Jimenez's multi-bounce fit: the light occluders bounce back lightens the occlusion; 0 none |
| `r_taa` (archived, default 0) | with `r_temporal 1`: temporal anti-aliasing; the projection is jittered with it whatever `r_temporalJitter` says (see Implementation notes, Temporal anti-aliasing) |
| `r_taaFeedback` (archived, default 0.75, 0.5 to 0.98) | with `r_taa 1`: the weight of the history in the blend; more is smoother and blurs more in motion, less is sharper and shimmers more (it is lowered by up to a quarter for pixels that move fast). Of the pixels of a still view, with `r_taaSharpen` 0.8, 1.25 % change between frames at 0.75, 2.3 % at 0.5; 0.39 % at 0.9 with sharpening 0.3 |
| `r_taaSharpen` (archived, default 0.8, 0 to 1) | with `r_taa 1`: sharpens the anti-aliased image when it is shown (the history stays unsharpened), within the colors of the pixel's 4 neighbours |
| `r_motionBlur` (archived, default 0) | with `r_temporal 1`: motion blur of the camera's motion and of what moves, from the motion vectors and the depth (see Implementation notes, Motion blur) |
| `r_motionBlurShutter` (archived, default 0.5, 0 to 2) | with `r_motionBlur 1`: the exposure, in frames at `r_motionBlurFps` (0.5 at 60: 1/120 s); the blur is the motion over that time |
| `r_motionBlurFps` (archived, default 60, 0 to 1000) | with `r_motionBlur 1`: the frame rate whose frames `r_motionBlurShutter` counts, so the blur is as long at any frame rate; 0: the frames as they come (`r_motionBlurShutter` of each frame's motion: shorter the higher the frame rate) |
| `r_motionBlurCamera` (archived, default 1, 0 to 1) | with `r_motionBlur 1`: the share of the camera's motion (turning, moving) in the blur; 0: only what moves |
| `r_motionBlurQuality` (archived, default 1, 0 to 2) | with `r_motionBlur 1`: 8, 12 or 16 samples a pixel where something moves |
| `r_dof` (archived, default 0) | with `r_temporal 1`: depth of field, focused on what is under the crosshair (or at `r_dofFocus`); the view weapon stays sharp (see Implementation notes, Depth of field) |
| `r_dofAperture` (archived, default 0.5, 0 to 5) | with `r_dof 1`: the lens' aperture, as the blur of the far distance when focused 128 units (about 3 m) away, in percent of the view's height; a scope's zoom blurs more (by its zoom, up to 4) |
| `r_dofFocus` (archived, default 0) | with `r_dof 1`: 0 focus on what is under the crosshair; more: a fixed focus distance in units (screenshots) |
| `r_dofFocusSpeed` (archived, default 0.25, 0 to 10) | with `r_dof 1`: the focus' time constant in seconds (game time); 0 at once. A cut (a teleport, a camera change) focuses at once |
| `r_dofQuality` (archived, default 1, 0 to 2) | with `r_dof 1`: 16, 32 or 48 samples a gather where something is blurred |
| `r_temporalJitter` (default 0) | with `r_temporal 1`: the 3D views' projection moves by a sub-pixel offset every frame (Halton 2, 3; `r_temporalJitterSamples`, default 8). For the anti-aliasing (`r_taa` turns it on itself): without it the image shakes |
| `r_temporalCutDist` (default 256) | with `r_temporal 1`: a camera or entity that moves farther in a frame has no motion (a cut, a teleport) |
| `r_temporalShow` (cheat, default 0) | with `r_temporal 1`, instead of the 3D view: 1 motion vectors (red right, green down, blue left or up, times `r_temporalShowScale`, default 20), 2 normals, 3 classes, 4 the depth pyramid's level `r_temporalShowMip` (min red, max green), 5 the last frame's color, 6 the view distances, 7 the last frame's (6 and 7 the nearest of level 0 of the pyramid: half the resolution; 1 and 2 work out what the draws left to the readers from it too), 8 the scene before the anti-aliasing's blend (with `r_taa 1`: still jittered, so each frame is aliased and moves by up to half a pixel; at a few hundred frames a second the eye averages it), 9 the ambient occlusion's factor (gamma-encoded, as it multiplies the scene), 10 one frame's ambient occlusion before the blending over frames and the blur, 11 the motion blur of each pixel (mode 1's colors, 1 the longest: half a streak of a tile's side), 12 the longest motion blur around each tile, 13 the depth of field's blur (red in front of the focus, blue behind it, 1 at 8 pixels, the scene in gray where in focus; the focus area's square green) |
| `r_vkFallbacks` (latched, default 0) | for tests: ignores what the device has and uses the fallback, bits added: 1 no `imageViewFormatSwizzle` (the alpha of RGB textures set in the shader), 2 no binding stride of 0 (those bindings read per instance, as on every portability subset device), 4 no `VK_KHR/EXT_line_rasterization`, 8 no `VK_KHR/EXT_swapchain_maintenance1` (no present fences); see Implementation notes. The startup line `Vulkan present fences ..., view swizzle ..., stride 0 bindings ..., Bresenham lines ..., line stipple ...` says what is used |

The console command `vkinfo` prints the backend's counters since its last
call: frames, render passes, submissions, mid-frame flushes, draws (those
from the static geometry, of them with the effects shader, from the terrain
pool, `terrainDraws`, skinned ones), the terrain pool's slots written
(`terrainSlots`, see Implementation notes, Terrain),
pipeline and descriptor set binds, bytes copied into vertex and staging
buffers, uploads ordered with draws, textures, pipelines and device memory
allocations created (with the microseconds spent creating them; of the
pipelines, those created before a draw needed them, `prebuiltPipelines`
and `prebuiltUs`, see Implementation notes, Pipelines), skin data added
(`skinUploads`, `skinBytes`), depth
images replaced to become readable or not (see Implementation notes,
Render target), swapchains created, bytes of frame vertex buffers created
because one was full, time spent waiting for frame fences (`waitUs`, with
the waits of mid-frame flushes, which `flushUs` also counts), in `vkQueueSubmit`, `vkAcquireNextImageKHR`
(before a present and right after one) and `vkQueuePresentKHR`, from the
start of a frame's recording to its present, and the GPU time of the frames
from timestamps (main command buffer, copy to the swapchain image, whole
frame), and with `r_vkSubmitThread 1` the submit thread's: the main
thread's waits for it, its frames, the time from a frame's hand-off to its
start, its submissions, acquires and presents with their times, and the
frames it copied (`thread*`); then the device memory objects, memory
blocks and pipelines that exist (with how many were created before their
first draw and how many of those a draw has used), whether the depth image
is readable, the present fences, retired swapchains, frames in flight and
swapchain images.
`tools/perf.py` reads it.

With `r_vkFrameLog 1` (latched: on the command line, or set it and
`vid_restart`), `vkframelog [file]` writes every frame recorded since the
last `vkframelog` (at most the last 131072) to `fs_homepath/main/<file>`
(default `vkframes.txt`) and prints the frame time percentiles and the
slowest frames with what happened in them; `vkframelog clear` drops them.
See Performance, Frame spikes, for what is in the file and how to send one.

The pipeline cache is saved as `vkpipelines.cache` in `fs_homepath/main` at
shutdown and `vid_restart` (when it grew) and given to the driver at
startup when the device and driver match. Next to it, `vkpipelines.keys`
lists the pipelines that were created after a load (a draw no shader of
the load gave, a shader registered during the match), at most 256: they are
created at the end of every load of the next runs, also after a driver
update, which makes `vkpipelines.cache` useless (see Implementation notes,
Pipelines). Deleting both files gives what a first play after an install
does.

Shaders: edit `shaders/*.vert|frag`, run `code/renderervk/shaders/compile_shaders.sh`
and commit the regenerated `vk_shaders.c`.

## Testing

Since 2026-10-23 there is no GL1 renderer to compare with (see Implementation
notes, No OpenGL): the tools compare builds of `renderer_vulkan` (and of the
engine and cgame) with each other, `compare.sh` two of them on one view
list, `cmplists.sh` any number on many lists. The GL1 numbers in this file
(the milestone tables, every "against GL1", `libglts.so`'s GPU times) were
measured with the GL1 renderer of the builds up to 53d8f9d7: check that
commit out to measure GL1 again.

Automated runs: put a cfg in `<fs_homepath>/main/` and pass `+exec name.cfg`.
Don't give it the name of a cfg the game runs itself: `default.cfg` runs
`menu.cfg` at startup, so a test script called `menu.cfg` runs then. Use
`devmap` so cheats work.

Do not use `s_initsound 0`: the game then crashes in
`ClientGameCommandManager::PlaySound` on the first footstep, with either
renderer (upstream bug). Mute with `s_volume 0`, `s_musicvolume 0`,
`s_ambientvolume 0` instead. `+set in_nograb 1` keeps the mouse free and
`+set in_mouse 0` keeps it from turning the view.

Screenshots land in `<fs_homepath>/main/screenshots/` as uncompressed 24-bit
TGA (18-byte header, BGR pixels, bottom row first).

### Deterministic 3D views

Two runs of the same map normally give different frames: the game time at the
screenshot depends on how long the loading and every frame took (swaying
foliage, animated and scrolling shaders, HUD), the player spawns at a random
point and the camera drifts. This script gives the same frame in every run
and with every build:

```
set thereisnomonkey 1
set fixedtime 16
devmap dm/mohdm5
wait 3000
finishloadingscreen
wait 1000
god
notarget
noclip
ui_hud 0
cg_drawviewmodel 0
tele 4896 -4640 172
face 0 180 0
wait 2000
pause
wait 300
screenshot f5_a
wait 100
centerprint " "
pause
wait 100
quit
```

Repeat the block from `tele` to the second `pause` for more views of the same
map; `tools/shots.py` writes such a script from a view list (see Comparing).
What each part is for:

- In OpenMoHAA `wait` counts milliseconds of game time
  (`Cbuf_Execute( msec )` in `Com_Frame`), not frames. `fixedtime 16` (a
  cheat cvar, settable before `devmap`) makes every frame advance the game by
  exactly 16 ms whatever the real frame time, so the script reaches each
  command after the same number of frames and at the same game time. `+set
  com_maxfps 60` keeps the real speed close to the game speed.
- `thereisnomonkey 1`: without it the game turns `cheats` off at the first
  cheat command (`Listener::ProcessEvent`), and `tele`, `noclip`, ... fail
  with "You must run the server with '+set cheats 1'".
- `tele x y z` and `face pitch yaw roll` (`fgame/player_util.cpp`) put the
  eye at an exact position; `noclip` keeps the player from falling or
  colliding, `god` and `notarget` keep the AI out of it. `coord` prints the
  current position and angles. Spawn points are in the entity lump of the
  BSP (`info_player_*`).
- `pause` before the screenshot: the renderer takes the screenshot at the
  end of the frame in which the command runs (see Frames and
  synchronisation; GL1 read a back buffer from two or three swaps earlier),
  so without the pause everything that moves (trees and bushes sway) can
  differ by a frame. While paused every frame is the same.
  `finishloadingscreen` unpauses too.
- The `screenshot` command centerprints "Wrote ..." in the view; the blank
  `centerprint` replaces it before the next screenshot.
- `ui_hud 0` and `cg_drawviewmodel 0` hide the HUD and the view weapon (set
  `cg_drawviewmodel 2` to test the weapon's depth range). Messages such as
  "Game Saved" and "An objective has been added!" go to the game message
  box, which `+set ui_gmbox 0` (on the command line: the box is created at
  startup) removes; `run.sh` sets it.

Still random between runs: what the game seeds with the wall clock
(`G_InitGame` seeds `rand()` with `Com_Milliseconds()`), such as the snow
particles of `dm/mohdm5` and `m6l1a`, AI movement in single-player maps and
the vehicles AI drives (the truck convoy of `m1l1`).

Single-player maps that start in a vehicle ignore `tele` while the player
is attached to it: `m1l1` (a truck of the convoy for about 90 s of game
time; `views/m1l1.txt` waits, run it with `TIMEOUT=300`), `m1l3a`
(halftrack gun, it starts driving about 15 s in), `m5l2a` (tank, the whole
level), `m3l1a` (landing craft for about two minutes; `views/m3l1a.txt`
waits, run it with `TIMEOUT=400`). `face` still turns the view.

Views must be taken from inside the world. From outside it (the void
around the map, above the sky brushes, inside a solid; the view cluster is
-1) part of the frame can be covered by no surface at all: it keeps what an
earlier frame drew into that buffer, an older swapchain image (the
offscreen render target before milestone 6's performance work; an older
back buffer with GL1), so two runs can differ there without meaning
anything. Two such views of the GL1 parity pass
looked like bugs: `obj_team2` from 1177 289 -120 and `dm/mohdm7` from
-1000 1000 800 above the sky, where the sky box (drawn from the sky polygons
seen from the view point) covers less than the frame. `shots.py` turns on
`r_showcluster` and `compare.sh` warns about shots whose cluster is -1.

GL1 now and then saved an all-black frame (about one shot in 40 in the
milestone 6 runs), which `compare.sh` warned about until GL1 was removed;
the Vulkan frames never did.

### Comparing

The scripts in `tools/` (Python 3 standard library and POSIX sh) automate it.
They read `MOH_BASEPATH` (game data, default `~/Documents/moh`),
`MOH_HOMEPATH` (throwaway homepath, default `/tmp/moh-vktest`), `MOH_BINDIR`
(default `build-vk/RelWithDebInfo`) and `MOH_OUT` (results, default
`$MOH_HOMEPATH/compare`).

```sh
cp build-vk/RelWithDebInfo/renderer_vulkan.so build-vk/RelWithDebInfo/renderer_base.so   # before the change
code/renderervk/tools/compare.sh code/renderervk/tools/views/mohdm5_fog.txt
WIDE=1 code/renderervk/tools/compare.sh code/renderervk/tools/views/mohdm5_fog.txt
code/renderervk/tools/cmplists.sh /tmp/cmp base=base,new=vulkan code/renderervk/tools/views/*.txt
code/renderervk/tools/glcheck.sh
```

- `views/*.txt`: view lists (`map <name>`, or `spmap <name>` for a single
  player game, `set <command>`, `shot <name> x y z pitch yaw roll [wait]`,
  `menu <name> [<menu> [wait]]`, `mask <name> x,y,w,h`), one per map or
  feature tested so far. `shot` waits 2 s of game time after moving the
  camera, or `wait` milliseconds (to catch a fade). `menu` takes a
  screenshot of a UI menu (`pushmenu <menu>`, then `popmenu`): over the
  main menu before the first `map`, over the paused view of the previous
  `shot` after it. `mask` leaves a rectangle of the 640x480 UI screen out
  of a view's statistics, scaled to the frame size (`tgadiff.py
  --mask-res`), for what the UI animates with the wall clock.
- `compare.sh`: writes the cfg (`shots.py`), runs it with two builds,
  `A` (the reference, default `base`: `renderer_base.so`, a copy of the
  build before) and `B` (default `vulkan`), with the engine binaries
  `A_BIN` and `B_BIN` of `MOH_BINDIR` (default `openmohaa`; a wrapper
  script `exec <dir>/openmohaa "$@"` runs a build with its own cgame.so,
  which is loaded from the binary's directory), through `run.sh`, then
  prints per channel the max and mean absolute difference and the number of
  differing pixels for every view (`tgadiff.py`), and writes PNG copies
  (`<view>_a.png`, `<view>_b.png`) and an amplified difference image
  (|a - b| x 8, equal pixels as a dark copy) to `$MOH_OUT`. `WIDE=1`
  compares at 4160x240. Until 2026-10-23 it compared GL1 with Vulkan.
  `VAL=2` runs with synchronization validation; validation messages are
  printed. A `views/<list>.shader` next to the list is installed as
  `main/scripts/aaa_<list>.shader` during the runs: the shader file listed
  first wins, so it replaces shaders of the game data (see
  `mohdm1_texenv`, `obj_team3_deepbluesea`). It warns about shots taken
  from outside the world (see Deterministic 3D views); `TIMEOUT`
  (seconds, default 180) is passed to `run.sh` for the lists that wait long
  (`m1l1`, `m3l1a`).
- `cmplists.sh <out> <variants> <list>...`: the same over many lists and
  any number of builds (`name=renderer[=r_vkFallbacks[=binary]]`, the first
  the reference), one `identical` or `DIFFERS` line per view and build in
  `<out>/summary.txt`; what the sessions used for the "against the build
  before" checks below.
- `glcheck.sh [bin dir]`: no OpenGL in the source and the binaries (see
  Implementation notes, No OpenGL).
- `hotspots.py`: zoomed side-by-side crops of the areas that differ most.
- `montage.py`: a grid of images, for example `a | b | diff` rows.
- `bspinfo.py`: worldspawn keys (fog, sun), spawn points, the number of LOD
  terrain patches and static models (the models whose path contains a
  keyword are listed with their origins), whether the `.dcl` decals load
  (keyword `decal` lists them) and the surfaces of the shaders whose name or
  script contains a keyword (`polygonoffset`, `alphafunc`, `nextbundle`,
  ...; terrain patches included), to pick views.
- `perf.py`: frame time and renderer time of views with several builds
  (copies of `renderer_vulkan.so`, `--renderers base,vulkan`), uncapped,
  in interleaved runs (see Performance), also of demos played
  frame by frame (`demo` lines, `views/perf_demo.txt`) and with every
  frame kept (`--framelog`), at frame rate caps (`--caps`), and of other
  engine builds (`--binaries`); `rtimer/` builds the
  `renderer_timed.so` wrapper it uses, `libstacks.so`, a stack sampler
  whose output `stacks.py` sums up, and `libinlat.so`, the time from each
  frame's input read to its present
  (see Performance, Frame limiter) and, with `INLAT_DISPLAY=1`, what the
  presentation engine, the compositor and the display did with it (see
  Performance, Present timing). With `r_vkSubmitThread 1` the present is
  made by the submit thread, when the main thread may have read the next
  frame's input already: the renderer then tells `libinlat.so` which frame
  each present shows (`inlat_frame_queued` on the main thread at the
  hand-off, `inlat_frame_presenting` on the thread; it looks them up once
  with `SDL_LoadFunction`, not on Windows), and the present gets its own
  frame's input read.
- `framelog.py`: frame time percentiles and the causes of the slow frames
  from the Vulkan renderer's frame log (`r_vkFrameLog`, `vkframelog`) or
  `renderer_timed.so`'s frame files, a `perf.py --framelog` directory as a
  whole (the renderers against the first one by name frame by frame, an
  SVG of the distributions,
  the input to present times of `libinlat.so`), the stack samples of
  the slow frames (see Performance, Frame spikes), and when each frame
  reached the display (`--display`, see Performance, Present timing).
- `soldiers.py`: writes a view list of a multiplayer scene, soldier models
  around the camera and muzzle flash lights (see Test content).
- `summary.py`: the worst view's numbers of several lists after
  `compare.sh` (`--wide` for the `WIDE=1` runs), in the form of the
  milestone 6 table below, with that table's row next to them
  (`--readme README.md`).

Changes that should not change the picture are checked against the
previous build: copy the old `renderer_vulkan.so` to `renderer_<name>.so`
in the binary directory and run the lists with `compare.sh` or
`cmplists.sh` (or one list with `run.sh <name> <list>` and `tgadiff.py`);
the frames of a deterministic view are then identical to the bit, run
after run, but for the lists that differ between two runs of the same
build (snow, smoke, AI and vehicles, some near-camera pixels; the checks
below name them). For an engine
change, copy the old `openmohaa` under another name (`MOH_BIN=<name>
run.sh vulkan <list>`, or the binary field of `cmplists.sh`); for a
cgame change, give the old build its own directory and a wrapper.

What the comparisons with GL1 found (radeonsi against RADV on the same
GPU, until 2026-10-23):

- At 640x480 (`r_mode 3`) 1 to 8 % of the pixels differ by 1 and a few
  hundred alpha-tested pixels (foliage edges) flip. radeonsi rasterizes
  viewports of up to 1024 pixels with 1/4096 pixel vertex precision, up to
  4096 with 1/1024, and 1/256 beyond (`si_state_viewport.c`); RADV always
  uses 1/256. Whole triangles then interpolate their texture coordinates very
  slightly differently, which bilinear filtering turns into ±1.
- With a window wider than 4096 pixels both drivers use 1/256:
  `+set r_mode -1 +set r_customwidth 4160 +set r_customheight 240`. The
  frames are then the same to the bit except pixels off by 1 on large
  polygons right next to the camera (the drivers clip against different
  guard bands; usually a few thousand pixels, up to 5 % of the frame when
  tree trunks, terrain or roofs pass the camera, as in `m61_slope` and
  `d6_roofs`) and on transient HUD text, a few isolated alpha-tested
  pixels, plus random particles and AI poses. One view differs much more:
  in `o1_sky` (`obj_team1`, looking up between hedges) the hedge polygons
  that pass nearly edge-on within a few units of the eye fill both ends of
  the 4160-pixel frame, and there their textures come out visibly shifted
  between the renderers (55 % of the pixels differ). The same view matches
  like the others at 640x480 and the middle of the wide frame matches, so
  this is attributed to the drivers clipping those polygons differently;
  it was not proven. That is the setting to check a change for exactness.
- `r_farplane_nofog 1` turns farplane fog off in both renderers.

Test content:

- `dm/mohdm1`: world, lightmaps, alpha-tested railings, trees and bushes
  (alpha-tested static models with vertex deforms), furniture, sky.
- Decals: a map's `.dcl` file only loads when its map time matches the
  BSP's. In the shipped data that is the case for `dm/mohdm4` (3 posters),
  `m2l3` (39 decals) and `obj/obj_team4`; the other maps print "BSP file is
  newer than DCL file" and have no decals with either renderer.
- Fog: `dm/mohdm2` (farplane 5000), `dm/mohdm4` (5500), `dm/mohdm5` (1800,
  dense), `m4l1` (3100), `m2l3` (set by its script). Sky portal: `m2l1`.
- Sun flare: `mohdm1_sunflare` (the "sun" lens flare of
  `global/lensflaredefs.txt`: a sprite at the sun and a full-screen white
  blend that fades over 600 ms once the sun leaves the view; the fade is
  caught at 160 and 400 ms). Every map with a `suncolor`/`sundirection` key
  and no `sunflarename none` has it (`dm/mohdm1`, `dm/mohdm3`, `dm/mohdm6`,
  `m2l2*`, `m3l2`, `m4l0`, `m5l*`, ...), but the flare only shows where the
  trace towards the sun hits a sky surface (not in the fog of `m2l3`).
- Dynamic lights: `mohdm1_dlight` spawns invisible entities
  (`spawn fx/dummy.tik targetname dl1`) and gives them a light (`script dl1
  origin "x y z"`, `script dl1 light r g b radius`); cgame adds them with
  `R_AddLightToScene` every frame, so they are as deterministic as the rest
  of the view. `script` (and `cam`) are refused when the game has more than
  one client slot, which is the case for `devmap` of a dm map: the list uses
  `spmap` (`spdevmap`). Run it again with `+set r_fastdlights 1` (latched)
  for the projected dlight pass. Its views show world faces, static models
  (the bushes, lit per vertex) and a small entity (the lamp);
  `mohdm1_dlight2` adds a brush model lit per pixel (the door `*15`; cgame
  adds only the lights near the view, so `dl2_door` and `dl2_doorproj`
  light it with light 0 alone, and `dl2_door3` with three lights, where
  GL1 lit brush models' lightmaps with light 0 only, see Implementation
  notes, Dynamic lights), curved patches and a soldier (`script_model`),
  `mohdm3_dlight` LOD terrain, and `mohdm1_dlight64` 40 lights in front of
  the door (run it with `+set r_maxDlights 64`; the default keeps the 4
  nearest the view): faces lit by lights more than 32 apart in the list,
  which get a list of their own. `cg_acidtrip N` adds N random dlights a
  frame (radius 128 to 639) for a quick look and for timing (`perf.py
  --exec`), not for comparisons.
- Multiplayer views with players (2026-10-16): `mp_mohdm1`, `mp_mohdm2`,
  `mp_mohdm7`, `mp_obj_team1` and `mp_obj_team4`, written by
  `tools/soldiers.py` (the command is in each list's first lines). Each
  spawns 20 `script_model`s with the player TIKIs of multiplayer
  (`models/player/*.tik`, allied and axis), 16 in a fan in front of the
  camera from 160 to 1060 units and 4 beside it, outside the view; they
  are dropped to the floor (`physics_on`, so that `droptofloor` traces
  against the world, then `physics_off`, and `physics_velocity "0 0 -1"`
  to cancel the upward velocity `physics_on` gives, which `physics_off`
  keeps: without it the soldiers rise by a unit a second), turned,
  animated with player animations (run, walk, idle, crouch) and most get
  a weapon in the right hand (`attachmodel models/weapons/<weapon>.tik
  tag_weapon_right`). Shots: `<tag>_front`; `<tag>_flash`, the same view
  with three dynamic lights next to three soldiers (muzzle flashes), 1.6 s
  of game time later; and `<tag>_side`, the view turned towards the
  soldiers beside the camera. The first shot comes more than 33 s after
  the map is loaded: the engine's skeletor keeps the time of a head's next
  blink in a `short` (`skeletor_c::m_timeNextBlink`), so heads blink at
  random during the first 32.8 s of `Sys_Milliseconds` and never after,
  and earlier shots differed between two runs of the same build in a few
  dozen pixels of one soldier's face. Now two runs of the same build give
  the same frames, but for 2 or 3 pixels of `pm7_front`.
  `views/perf_mp.txt` times them (`perf.py` takes the shots of a view
  list with `views <list> [shot ...]`). `mp_shadows` gives the same
  soldiers shadows (`rendereffects "+shadow"`, half of them
  `preciseshadow`) and takes them with `cg_shadows 1` (blobs) and 2 (feet),
  from the eye and from above: cgame's temporary marks (see Performance,
  cgame caches).
- Menus: `menus.txt` takes the main menu (fan masked), the options pages
  (options, controls, video, audio, advanced, multiplayer), the weapon
  selection, then in a game of `dm/mohdm1`, over a paused view, the weapon
  selection with the sign of a hovered weapon (`ui_weaponsign`), the main
  menu of a game (BACK TO GAME, QUIT; fan masked) and the team selection.
  Menu names are the first line of each `ui/*.urc` (`listmenus` lists the
  loaded ones). The panels and buttons of most of these menus (the
  weapon selection's wood panel, the controls, video and options pages,
  BACK TO GAME, QUIT, APPLY, DEFAULT, BACK) are in `Pak6EnUk.pk3`, the
  English localization pak; an install without it draws them, with
  either renderer, with the default image (`*default`: 16x16, grey 32
  with a white border, stretched and filtered: dark grey panels with
  soft white frames), and with `developer 1` the log shows `could not
  find 'textures/mohmenu/...'`. That was a tester's report on 2026-10-16
  (NVIDIA, Linux; GL1 the same); adding the pak fixed it. Running
  `menus.txt` with a basepath without that pak gives the same frames with
  both renderers.
- Cinematics: the game data has no RoQ file. `ffmpeg -loop 1 -i still.png
  -t 2 -r 30 -c:v roqvideo -f roq <homepath>/main/video/test.roq` makes one
  (the encoder is slow, about 10 frames a second); `cinematic test.roq 1`
  plays it full screen and holds the last frame, so a screenshot taken after
  the end is deterministic. Sizes must be powers of 2 (both renderers drop
  to the menu otherwise). RoQ refines a still image over several frames, so
  only the held last frame compares exactly.
- `TEXENV_ADD`: no shader of the game data produces it. `nextBundle add`
  is never used, and the collapse of a stage with an additive stage
  (`r_ext_texture_env_add 1`) needs `rgbGen identity` on both, which none of
  the three candidates has (`shaderlist` shows `MT(a)` for such stages; 12
  maps showed none). `mohdm1_texenv` replaces the ground shader with an
  opaque and an additive texture stage and a lightmap stage (one
  `TEXENV_ADD` stage plus the lightmap with `r_ext_texture_env_add 1`, three
  passes with 0; the two give the same ground within ±2) and a wall shader
  with `nextBundle add`. The pair to collapse must not contain the
  lightmap: see GL1 quirks.
- The effects shader (see Implementation notes): `fx_mohdm1` replaces three
  world shaders of `dm/mohdm1` (`fx_mohdm1.shader`, installed like
  `mohdm1_texenv`'s) with stages that use every deform, `tcGen` and `tcMod`
  it takes, a lightmap stage with a `tcMod`, `alphaGen distFade` on a
  constant color and `oneMinusDistFade` on the vertex colors, in
  deterministic views; the game's own shaders of that kind are in the
  forests, the water and windows of the other lists (counted with a
  temporary build, see Effects shader in the results below).
- The skinned passes (see Implementation notes, Skeletal models):
  `skin_passes` puts the models whose shaders the generic iterator skins on
  the GPU in `dm/mohdm1` (the Afrika Korps goggles, the tank commander's
  coat, hat and gloves, the bullet-hit debris scaled up, the sniper rifles
  with their lenses) and takes them again with two dynamic lights next to
  them (the sphere's lights in the vertex shader; the lenses, with an alpha
  of their own, back on the CPU).
- The HUD (2026-10-22): `hud` takes the in-game HUD, which every other list
  hides (`ui_hud 0`), in a team match of `dm/mohdm1` (`g_gametype 2` before
  the map): the compass, health, ammo and weapon bar, score and time left, a
  chat line and the scoreboard, for the 2D batches and `DrawBox`'s
  transparent boxes (see Implementation notes, 2D batches). The player's
  name is set (the default one ends in a random number) and the shots wait
  for the spawn and the weapon change. The compass turns towards its target
  with the UI's wall clock (`uid.time`), so it differs between runs:
  masked. What is drawn as one batch in a match, the compass's triangles and
  the kill feed's lines, was compared within one run instead (see the 2D
  batches in Comparing).
- Debris and smoke (2026-10-22): `smoke` puts emitters on the square of
  `dm/mohdm1` (a single-player game, so `spawn` and `script` work) that
  keep throwing pipes and beams, which bounce and come to rest, and
  volumetric smoke, in front of the camera: cgame lights every piece of
  debris with a light grid lookup every frame (`R_GetLightingForSmoke`),
  about 130 a frame in `sm_front`, more than the smokiest matches average.
  For timing (`perf.py`, see Performance, Light grid lookups); its content
  is random, the debris lands elsewhere in every run.
- A team match (2026-10-22): `team_obj_team2` takes `obj_team2`'s views in
  a team match (`g_gametype 2` before the map, like `hud`), where cgame
  registers at the load what a multiplayer match would load later (see
  Implementation notes, Registration at load); the other lists are
  single-player games, where it doesn't. A team match can't be paused, so
  the yard's swaying tree can land on another phase (`mpo2_yard`, once in
  ten runs).
- Debug drawing: `mohdm1_debug`. `r_showtris` and `r_shownormals` only draw
  with `developer 1` (`r_developer`), which is why they seemed to show
  nothing. The game's debug lines (`sv_showbboxes`, camera paths shown with
  `cam show`, AI paths) are drawn by `R_DrawDebugLines` from the front end,
  before the back end draws the view, which then covers them: they are
  invisible with both renderers. That is also the only user of line
  stipple (`DrawLineLoop` has no caller), so stipple was checked by forcing
  a pattern on `r_showtris` temporarily.
- Milestone 6 parity pass (one list per map, the comment at the top of each
  says what it covers):
  - sky boxes: `mohday1` (`mohdm3`, `mohdm6`, `obj_team2` at dusk),
    `africanight` with its scrolling additive cloud layer (`mohdm7`,
    `m1l1`, `m1l2a`), `mohnightfog` (`m1l3a`), `d-day2` (`m3l2`, `m3l3`,
    `obj_team3`), `m5l2` (`m5l1a`, `m5l2a`, `obj_team1`, `obj_team4`),
    `mohday2` (`m5l3`); the `m6` maps use `caulksky` under fog;
  - sun lens flares: `d3_sun`, `m32_sun`, `m53_sun`, `o2_sun` (and
    `m53_sunedge` with the sun just outside the view);
  - water: `deepbluesea_runup` and `deepbluesea_shoreline` (`obj_team3`),
    `deepbluesea` itself through `obj_team3_deepbluesea.shader` (its only
    map, `m3l1a`, shakes the camera), `subpen_water4` under `canal_sludge`
    (`m3l2`), `frozenriver` (`m6l2a`), `riverbed` (`obj_team4`);
  - snow and night: `snowfield1` with `tcMod parallax` (`m6l2a`), the
    snowy forest of `m6l1a` (2763 static models, fog 1500, random snow),
    the script fog of `m6l3a`, night maps `mohdm7`, `m1l1`, `m1l2a`,
    `m1l3a`, `m6l*`;
  - LOD terrain: `mohdm3`, `m1l1`, `m1l3a`, `m3l3`, `m5l1a`, `m6l1a`,
    `m6l2a`, `obj_team1`, `obj_team3`, `obj_team4`;
  - farplane fog: 1500 to 15000 (`m6l1a`, `m5l1a`, `obj_team1`, `m1l3a`,
    `obj_team4`, `m5l2a`, `m3l2`, `m5l3`);
  - environment mapping (`tcGen environment`): the windows of `mohdm3`,
    the wet pipes of `obj_team2`;
  - decals: the posters of `obj_team4`;
  - map-script effects: the burning debris of `m1l1`, the fog of `m6l3a`,
    the convoy, landing craft and halftrack rides (see Deterministic 3D
    views for what they mean for the views).
- No shipped map has a portal or mirror surface (`portal` shader keyword), so
  the user clip plane has no GL1 reference in game content. It was checked
  with a temporary plane forced on the main view: x_eye >= 0 cut every
  frame exactly at the middle column, the kept half was identical to the
  unclipped frame with the fragment shader fallback and identical except ±1
  on the triangles crossing the plane with `gl_ClipDistance`, and a plane at
  400 units removed the near geometry.
- Fallbacks (2026-10-18; RADV has every feature they replace, so they are
  forced with `r_vkFallbacks`, see Build and run): run a list with `+set
  r_vkFallbacks 3` (no view swizzle, no stride 0 bindings) against the
  default build; the frames must be the same to the bit. The game's RGB
  textures have alpha 255 in their data anyway (GL1 makes a texture RGB
  when every alpha is 255), so that alone doesn't show the shader's alpha
  at work: a temporary build that wrote alpha 0 into the uploads of RGB
  textures gave the same frames as before with the swizzle and with the
  shader setting it (`mohdm1`, `obj_team2`), and without either 0.3 to 100
  % of the pixels differed in every view (the sky boxes are RGB). `4`
  (lines without `VK_KHR/EXT_line_rasterization`) gives the same frames on
  RADV; on llvmpipe (`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`,
  a device with `strictLines`) it moves line pixels (see Fragment pipeline).
  `8` (no present fences) changes no pixel. The presentation paths were
  run with synchronization validation (`r_vkValidation 2`) and every
  combination of the bits: startup, map loads, `vid_restart` (with mode
  changes), live resizes with `r_allowResize 1` (`xdotool windowsize`, a
  burst of 11 sizes 50 ms apart: 11 swapchains replaced before the
  `vid_restart` the resize schedules), minimize and restore (Xwayland keeps
  the window's size: a temporary build reported no area for 60 frames out
  of every 200, the path of a minimized window on Windows, where the
  swapchain is destroyed and created again), fullscreen toggles (`set
  r_fullscreen 1`, `0`, with `in_nograb 0`; KWin under Xwayland) and quit:
  no message. The validation layer (1.4.341) doesn't check present fences
  against the destruction of swapchains and semaphores, though: a
  temporary build that left the fences out with the extension enabled gave
  no message either, so this rests on reading the specification (see
  Implementation notes, Presentation semaphores and swapchains), not on
  validation. Build against build: the 41 view lists (177 views,
  640x480, `r_vkValidation 1`) with this build and with `r_vkFallbacks 15`
  against the build before: 143 views the same to the bit with both; the
  other 34 are in the lists with random content (`m1l1`, `m1l2a`, `m1l3a`,
  `m2l1_skyportal`, `m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m5l3`,
  `m6l1a`, `mohdm5_fog`, `mohdm7`, `mp_mohdm7`: AI, vehicles, the landing
  craft's camera shake, snow, the known 1-pixel noise near the camera), and
  each of them also differs between two runs of the build before (a
  second pass with it twice); no validation message. GL1 against this
  build with `compare.sh` (`mohdm1`, `obj_team2`, `menus`,
  `mohdm1_debug`, `mohdm1_dlight`, at 640x480 and `WIDE=1`) gives the
  numbers of before. Windows: see [WINDOWS_TESTING.md](WINDOWS_TESTING.md),
  the checklist for a tester.

Results of the milestone 5 tests with `WIDE=1` (GL1 vs Vulkan, worst view
of each list; per channel max R/G/B and the share of differing pixels):

| test | max R/G/B | differing | nearly all |
| --- | --- | --- | --- |
| `mohdm1_sunflare` (6 views, fade included) | 132/129/101 | 0.375 % | ±1, 14 isolated foliage pixels |
| `mohdm1_dlight` | 61/42/23 | 0.024 % | ±1 |
| `mohdm1_dlight +set r_fastdlights 1` | 61/42/23 | 0.025 % | ±1 |
| `mohdm1_texenv` (`r_ext_texture_env_add` 1 and 0) | 66/54/20 | 0.024 % | ±1 |
| `mohdm1_debug` (`r_showtris 2`) | 252/253/253 | 0.015 % | a few nearly horizontal line segments |
| cinematics (held last frame, 256x256 and 512x256, 640x480) | 0/0/0 | 0 % | |
| main menu (fan masked, 640x480) | 0/0/0 | 0 % | |

Menus (`menus.txt`, 2026-10-16, 12 views): the 8 views over the main
menu give 0 differing pixels but 17 pixels off by 1 in `mn_video` at
640x480, and at 4160x240 198 in `mn_controls` and 14 in `mn_video`, all
off by 1. The 4 views in `dm/mohdm1` are the 3D view `m1_spawn` with the
menus over it: at 640x480 the differences of that view (5 to 7 % of the
pixels, nearly all ±1, 140 to 220 foliage pixels by more than 3), none
in the menus, and the main menu of the game, which covers the view, 0; at
4160x240 28 to 36 pixels, max 4 (0 for the main menu). The same numbers
at 640x480 with the game data without `Pak6EnUk.pk3`. No validation message, also with
synchronization validation (`VAL=2`).

At 640x480 the same lists show the usual 1 to 10 % of pixels off by 1 and a
few dozen to a few hundred isolated pixels off by more (alpha-tested foliage,
and broken `r_showtris` edges where the line and its polygon tie in depth),
all gone or nearly gone at 4160x240.

Results of the milestone 6 parity pass (sequential runs, worst view of each
list; per channel max and mean, share of differing pixels, and the number
of pixels off by more than 3 in the worst view for that count):

| list (views) | 640x480 max R/G/B | differing | > 3 | 4160x240 max R/G/B | differing | > 3 |
| --- | --- | --- | --- | --- | --- | --- |
| `mohdm3` (6) | 235/211/187 | 4.38 % | 199 | 63/58/25 | 0.245 % | 21 |
| `mohdm6` (6) | 116/141/185 | 1.62 % | 121 | 2/2/1 | 1.87 % | 0 |
| `mohdm7` (5) | 247/213/125 | 1.28 % | 112 | 28/25/23 | 0.114 % | 5 |
| `m1l1` (3) | 161/134/121 | 4.49 % | 6132 | 156/129/115 | 7.61 % | 41818 |
| `m1l2a` (5) | 66/57/70 | 1.24 % | 34 | 12/16/18 | 0.260 % | 1 |
| `m1l3a` (4) | 70/57/69 | 1.90 % | 39 | 138/94/92 | 0.243 % | 292 |
| `m3l2` (6) | 136/132/133 | 4.29 % | 144 | 161/157/155 | 1.08 % | 363 |
| `m3l3` (4) | 150/146/143 | 5.50 % | 125 | 157/153/152 | 0.303 % | 16 |
| `m5l1a` (6) | 136/126/116 | 4.53 % | 527 | 127/118/107 | 0.745 % | 3088 |
| `m5l2a` (2) | 112/107/110 | 3.05 % | 195 | 47/38/26 | 0.007 % | 19 |
| `m5l3` (6) | 165/155/128 | 3.34 % | 21 | 50/50/44 | 1.02 % | 32 |
| `m6l1a` (4) | 122/122/126 | 8.26 % | 625 | 126/126/126 | 4.92 % | 1030 |
| `m6l2a` (6) | 90/87/103 | 5.90 % | 281 | 30/27/28 | 3.09 % | 2 |
| `m6l3a` (3) | 53/58/91 | 1.48 % | 91 | 37/37/65 | 0.281 % | 885 |
| `obj_team1` (4) | 110/108/119 | 4.41 % | 171 | 118/118/119 | 54.7 % | 442501 |
| `obj_team2` (6) | 60/59/50 | 2.10 % | 48 | 214/189/204 | 2.56 % | 103 |
| `obj_team3` (6) | 50/56/63 | 3.79 % | 8 | 12/12/12 | 0.355 % | 48 |
| `obj_team3_deepbluesea` (3) | 49/56/63 | 3.55 % | 8 | 12/12/12 | 0.289 % | 48 |
| `obj_team4` (6) | 215/216/179 | 4.08 % | 310 | 83/83/88 | 0.782 % | 21 |

The mean differences stay below 0.15 per channel everywhere except `m1l1`
(up to 0.38: smoke) and the `obj_team1` WIDE row. Nearly every differing
pixel is off by 1 at both sizes; what is off by more was traced, view by
view, to:

- random game content: AI soldiers (`m3l2`, `m1l3a`, `m12_yard`), window
  shutters that swing (`m5l1a`: they are open in one run and closed in the
  other), snow particles (`m6l1a`, `m6l3a`), the smoke and fire light of
  `m1l1`;
- the WIDE clipping cases above (the larger off-by-1 areas of `m6l1a`,
  `m6l2a`, `obj_team2`, `mohdm6`, `m5l3`, and `o1_sky`, attributed to the
  drivers' clipping);
- at 640x480, alpha-tested foliage and one-pixel shifts of polygon edges
  (`o4_poster1`: a vertical edge one column over).

After the performance work (drawing into the swapchain image, screenshots
at the end of the frame, the memory sub-allocator, vertex reuse) every list
was run again at both sizes (`tools/summary.py` gives the rows): 13 lists
at 640x480 and 7 at 4160x240 give exactly the numbers of the table, and the
others differ where the game randomizes (the AI fire of `m1l3a`, which put
the red damage flash into one GL1 shot, its halftrack; the shutters of
`m5l1a`, the tank of `m5l2a`, the snow of `m6l1a` and `m6l3a`; the smoke of
`m1l1`, now 3.7 % and 1.1 % differing), or are better (`obj_team1` at
4160x240: 0.15 % instead of 54.7 %, the `o1_sky` clipping case did not
recur). The milestone 5 lists give their table's numbers too. The sharper
check is the previous Vulkan build: run on all 19 lists at 640x480 (and
`mohdm1_debug` at 4160x240), it gives the same frames to the bit in every
view without random content, with one exception: `d7_window` (`dm/mohdm7`)
has a two-pixel-high crack between world surfaces at the bottom of the
frame (`r_fastsky 1` shows the clear color there), which shows whatever
the buffer held: an earlier frame with GL1 and the old offscreen target,
black in a freshly acquired swapchain image (196 of the 202 pixels off by
more than 3 in that list). With `r_fastsky 1` the old and new frames are
identical there too. No validation message in any of these runs
(`r_vkValidation 1`).

The depth image that is only readable while readbacks are wanted (see
Implementation notes, Render target) was checked against the build before
it in the same way, on 29 lists at 640x480 (131 views: every list but
`m1l1` and `m3l1a`): 20 lists give the same frames to the bit in every
view (among them `mohdm1`, `mohdm3`, `obj_team1` to `obj_team4`, `m3l2`,
`m3l3`, `m5l3`, `m6l2a`, `m6l3a`, the decal, texture env, dynamic light,
sun flare and fog lists). The others differ in 2 to 3970 pixels of some
views: snowflakes (`m6l1a`, `mohdm5_fog`), the random content named above
(`m1l2a`, `m1l3a`, `m5l1a`, `m5l2a`), and off-by-1 noise on surfaces near
the camera in `m6l1a`, `mohdm7`, `m2l1_skyportal` and `m4l1_fog`. Running
the build before the change twice (two copies of the same `.so`) gives the
same kind of differences in the same views (`m61_slope` 2341 pixels,
`d7_arch` 798, `fm4_a`/`fm4_b` 4/2, `m5l1a` 7 to 297), so they vary from
run to run and are not the change (the comparison above found `mohdm7`
identical but for `d7_window`: these near-camera differences don't occur
in every run). No validation message (`r_vkValidation 1`). Against GL1
(`compare.sh`, `summary.py`): `m6l1a` 116/116/126, 8.26 %, 379 pixels off
by more than 3 at 640x480 and 125/125/125, 5.39 %, 272 at 4160x240;
`mohdm7` 247/213/125, 1.27 %, 202 and 48/36/35, 0.091 %, 56, where the
pixels off by more than 3 beyond the table's are the `d7_window` crack
(black in Vulkan, an older frame in GL1; at 4160x240 all 56 on one row).

The second round of the performance work (see Performance) was checked
against the build before it in the same way, on the 31 view lists at
640x480 plus `mohdm1_dlight` with `r_fastdlights 1` and `mohdm1_texenv`
with `r_ext_texture_env_add 1` (145 views): 118 views give the same frames
to the bit; the others are the random content (AI in `m1l2a` and `m1l3a`,
where AI fire put the red damage flash into one run's `m13_c`, the
shutters of `m5l1a`, the tank of `m5l2a`, snow in `m6l1a` and
`mohdm5_fog`) and the near-camera noise of `m2l1_skyportal`, `m4l1_fog`,
`m61_slope` and `mohdm7`, plus one pixel off by 1 in `m33_grass`
(`m3l3`), which two runs of the same old build also gave. With 4 samples
the `mohdm1` views are the same to the bit too. Against GL1
(`compare.sh`): `dm/mohdm3`, `m3l3` and `obj_team2` give the table's
numbers at 640x480, `m6l1a` 120/120/126, 8.28 %, 469 (snow); at 4160x240
`m6l1a`'s `m61_slope` differed in 5.8 % of the pixels, but two runs of the
same old build differ by 5.2 % there. No validation message with
synchronization validation (`r_vkValidation 2`) on `dm/mohdm1` (also with
`r_ext_multisample 4`, and with `r_stencilbits 8` and `r_measureOverdraw
1`), `m6l1a`, `mohdm1_dlight` and `m2l1_skyportal`; the main menu matches
GL1 (fan masked).

The static geometry (2026-10-15, see Implementation notes) was checked
against the build before it in the same way, on the 31 view lists at
640x480 plus `mohdm1_dlight` with `r_fastdlights 1` and `mohdm1_texenv`
with `r_ext_texture_env_add 1` (151 views): 119 views give the same
frames to the bit, among them every view of `dm/mohdm1` to `dm/mohdm4`,
`dm/mohdm6`, `m2l3`, `m3l2`, `m3l3`, `m6l2a`, `m6l3a`, `obj/obj_team1` to
`obj/obj_team4`, the decal, dynamic light (both modes), texture env, sun
flare, fog and debug drawing lists. The other 32 are the random content
and run-to-run noise named above: the smoke and convoy of `m1l1`, AI in
`m1l2a` and `m1l3a` (`m13_sky` caught the red damage flash of AI fire),
the camera shake of `m3l1a`, the shutters of `m5l1a`, the tank of
`m5l2a`, snow in `m6l1a` and `mohdm5_fog`, a particle in `m53_street`
(`m5l3`, which two runs of the old build show too), and the near-camera
noise of `m2l1_skyportal`, `m4l1_fog`, `m61_slope` and `d7_arch` (±1). Also
the same to the bit with `r_vertexLight 1` and with `r_lightmap 1` on
`dm/mohdm1` and `obj/obj_team2`. A bug found that way: a static model
whose surfaces draw nothing at their level of detail still joined a merged
batch and changed the modelview, so the next one-model batch was drawn
with the wrong matrix (29 pixels of `d3_top`); each model's batch now sets
its own matrix. Against GL1 (`compare.sh`, `summary.py`) every list gives
the numbers of the milestone 6 table at 640x480 and at 4160x240 except
where the game randomizes (`m1l1`, `m3l1a`, AI in `m1l3a` and `m3l2`, the
shutters of `m5l1a`, snow in `m6l1a` and `m6l3a`), after running again
the lists where GL1 saved a black frame (`m51_sky`, `d3_terrain` at
4160x240) or the previous shot's frame (`m53_river`); `obj_team1` at
4160x240 gives 0.15 % like the second round, `mohdm7` the `d7_window`
crack noted above. No validation message in any of these runs
(`r_vkValidation 1`), nor with synchronization validation
(`r_vkValidation 2`) on `dm/mohdm1`, `mohdm1_dlight` (both modes),
`m2l1_skyportal`, `m6l1a`, `obj/obj_team2` and a script that loads
`dm/mohdm1`, turns `r_staticGeometry` off and on, runs `vid_restart`,
then loads `dm/mohdm3` and `m6l1a`; the main menu matches GL1 (fan
masked).

The per-pixel dynamic lights (2026-10-15, see Implementation notes,
Dynamic lights), against GL1 (`compare.sh`, `summary.py`: worst view of
each list; per channel max, highest mean, share of differing pixels and the
pixels off by more than 3):

| list (views) | `r_fastdlights` | 640x480 max R/G/B | mean | differing | > 3 | 4160x240 max R/G/B | differing | > 3 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `mohdm1_dlight` (4) | 0 | 254/228/190 | 0.330 | 49.0 % | 2338 | 61/42/23 | 26.2 % | 1123 |
| | 1 | 254/228/190 | 0.074 | 8.65 % | 376 | 61/42/23 | 0.025 % | 3 |
| `mohdm1_dlight2` (4) | 0 | 228/218/165 | 0.518 | 48.2 % | 10316 | 21/15/7 | 44.4 % | 11354 |
| | 1 | 228/218/165 | 0.071 | 9.32 % | 162 | 2/2/0 | 3 pixels | 0 |
| `mohdm3_dlight` (2) | 0 | 220/193/140 | 0.147 | 20.3 % | 831 | 87/95/114 | 2.47 % | 210 |
| | 1 | 221/194/140 | 0.031 | 3.43 % | 84 | 87/95/114 | 0.31 % | 74 |

With `r_fastdlights 1` the frames are the previous build's to the bit (the
projected pass is unchanged) and GL1's at 4160x240 but for a few pixels;
the largest values at 640x480 and in `mohdm3` at 4160x240 are foliage
edges and the soldier's outline, which differ without dynamic lights too.
With `r_fastdlights 0` the differences are in the light pools (diff images
in the 2026-10-15 report): nearly all ±1 to ±3 where the falloff has no
texel steps any more, and bands a few pixels wide next to the lights where
GL1 normalized a saturated texel before filtering (up to 46 levels at
640x480 on the wall next to the light in `dl2_vault`, 21 at 4160x240), see
Differences from GL1. Against the previous Vulkan build (the same dynamic
lightmaps as GL1) `mohdm1_dlight` differs in 16 to 47 % of the pixels,
max 22 to 24, all in the light pools; views without dynamic lights are
the same to the bit (see below).

Against the previous build (`run.sh`, `tgadiff.py`, 640x480) on the 35
view lists plus `mohdm1_dlight`, `mohdm1_dlight2` and `mohdm3_dlight` with
`r_fastdlights 1` and `mohdm1_texenv` with `r_ext_texture_env_add 1` (163
views): 121 views give the same frames to the bit, all three lists with
`r_fastdlights 1` among them. Of the other 42, 10 are the views with
dynamic lights in `r_fastdlights 0`; the rest are the random content and
run-to-run noise named above: the smoke, fire and convoy of `m1l1`, the
camera shake of `m3l1a`, AI in `m1l2a` and `m1l3a`, the shutters of
`m5l1a`, the tank of `m5l2a`, snow in `m6l1a` and `mohdm5_fog`, and ±1
near the camera in `m2l1_skyportal`, `m33_grass`, `fm4_a`, `m61_slope`,
`d7_alley` and `d7_arch`; `m12_court`, `m12_lamp` and `m12_yard` (`m1l2a`,
±1 over a few hundred pixels) differ the same way between two runs of the
previous build. No validation message in any of these runs
(`r_vkValidation 1`), nor with synchronization validation (`r_vkValidation
2`) on the three dynamic light lists (both modes for `mohdm1_dlight`),
`mohdm1_dlight` with `r_ext_multisample 4`, `dm/mohdm1`, and a script that
loads `dm/mohdm1` with `cg_acidtrip 16`, turns `r_staticGeometry` and
`r_dlightBacks` off and on, runs `vid_restart` and loads `dm/mohdm3` and
`m3l3` with the same lights; the main menu matches GL1 (fan masked).

The skeletal models on the GPU (2026-10-16, see Implementation notes,
Skeletal models), against GL1 (`compare.sh`, `summary.py`): the 19
multiplayer and dm/obj lists of the earlier tables give their numbers
exactly at 640x480, and at 4160x240 too (`mohdm7`: 48/36/35, 0.057 %, 56,
the `d7_window` crack; `obj_team1` 0.149 % as in the second round); the
dynamic light lists those of their table. The new multiplayer lists (worst
view of each; the `_flash` views have the accepted per-pixel dynamic
light falloff, the others don't):

| list (views) | 640x480 max R/G/B | differing | > 3 | 4160x240 max R/G/B | differing | > 3 |
| --- | --- | --- | --- | --- | --- | --- |
| `mp_mohdm1` (3) | 253/228/189 | 16.0 % | 2514 | 25/18/9 | 27.4 % | 994 |
| `mp_mohdm2` (3) | 96/82/91 | 15.2 % | 2593 | 64/64/72 | 14.6 % | 2115 |
| `mp_mohdm7` (3) | 117/86/110 | 2.23 % | 102 | 2/1/1 | 0.054 % | 0 |
| `mp_obj_team1` (3) | 105/95/106 | 10.2 % | 1058 | 66/93/77 | 0.528 % | 179 |
| `mp_obj_team4` (3) | 93/91/103 | 22.7 % | 4066 | 54/44/31 | 17.0 % | 1004 |

Without dynamic lights (`_front`, `_side`) the 4160x240 frames differ
from GL1's in 2 to 243 pixels (nearly all by 1) where the soldiers
face the camera, and in 2324 to 8912 pixels (nearly all by 1) in three side
views that face a nearby wall (the clipping case of Comparing): the 20
skinned, lit soldiers of each view come out as GL1's.
Against the previous build (`run.sh`, `tgadiff.py`, 640x480) on the 41
view lists plus `mohdm1_dlight`, `mohdm1_dlight2` and `mohdm3_dlight` with
`r_fastdlights 1` and `mohdm1_texenv` with `r_ext_texture_env_add 1` (178
views): 142 views give the same frames to the bit, among them every view
of the multiplayer lists (but 1 pixel of `pm7_front`, which two runs of
one build show too), of `dm/mohdm1` to `dm/mohdm4`, `dm/mohdm6`,
`obj/obj_team1` to `obj/obj_team4`, the dynamic light lists (both modes),
`m2l3`, `m3l2`, `m5l3` but a particle, `m6l2a` and `m6l3a`. The other 36
are the random content and run-to-run noise named above: the smoke, fire
and convoy of `m1l1`, the camera shake of `m3l1a`, AI in `m1l3a`, the
shutters of `m5l1a`, the tank of `m5l2a`, snow in `m6l1a` and
`mohdm5_fog`, and the near-camera ±1 of `m2l1_skyportal`, `m33_grass`,
`fm4_a`/`fm4_b`, `m61_slope`, `d7_alley`, `d7_arch` and `m1l2a` (whose
`m12_*` views differ exactly the same way between two runs of the
previous build). The view weapon (`m2l3` with `cg_drawviewmodel 1`,
skinned and lit in the vertex shader) gives the same frames too, and so
does `mp_mohdm1` with `r_gpuSkinning 0` and 1. Coverage of these views (counted with
a temporary build, `mp_mohdm1` with the view weapon): 139 skinned
batches a frame and 2 left on the CPU (other shaders), surfaces at a
lower level of detail and at full detail, entities with head morphs,
meshes after the first (bone remap tables), light lists and constant
colors. No validation message in any of these runs (`r_vkValidation 1`),
nor with synchronization validation (`r_vkValidation 2`) on `mp_mohdm1`,
`mp_mohdm7`, `mohdm1_dlight2`, the view weapon test and a script that
spawns the soldiers of `mp_mohdm1`, turns `r_gpuSkinning` off and on,
runs `vid_restart` and loads `obj/obj_team4` with its soldiers (the skin
data cleared and filled again); the main menu matches GL1 (fan masked).

The GPU time changes of 2026-10-16 evening (alpha written by the
pipelines without blending, the depth image without a stencil plane when
`r_stencilbits` is 0, the fog color and `r_fastsky` clears drawn on
RADV; see Performance, GPU time at 3840x2160), against the build before
them (`run.sh`, `tgadiff.py`, 640x480) on the 41 view lists plus the
dynamic light lists with `r_fastdlights 1`, `mohdm1_texenv` with
`r_ext_texture_env_add 1`, `mohdm1` with `r_ext_multisample 4` and with
`r_stencilbits 8` (the stencil plane kept), `mohdm1` and `dm/mohdm3`
with `r_fastsky 1` (drawn clears) and `menus.txt` (208 views): 176 views
give the same frames to the bit, and the other 32 are the random content
and run-to-run noise named above (the smoke and convoy of `m1l1`, AI in
`m1l2a` and `m1l3a`, the camera shake of `m3l1a`, the shutters of
`m5l1a`, the tank of `m5l2a`, a particle in `m53_street`, snow in
`m6l1a` and `mohdm5_fog`, and ±1 near the camera in `s2_a`/`s2_b`,
`fm4_a`/`fm4_b`, `m61_slope`, `d7_alley` and `d7_arch`), among them the
`r_fastsky 1` views, whose color clears are all drawn now, and the fog
maps. No validation message in any of these runs (`r_vkValidation 1`),
nor with synchronization validation (`r_vkValidation 2`) on `mohdm1`
with `r_fastsky 1` (also with `r_ext_multisample 4`), `mohdm2_fog`,
`mohdm1_dlight`, `mohdm1` with `r_stencilbits 8` and `r_measureOverdraw
1` and with `r_measureOverdraw 1` alone (no stencil buffer: the warning,
like GL1), and a script that switches `r_stencilbits` 8 and 0,
`r_measureOverdraw` and `r_shadows 2`, and multisampling on and off with
`vid_restart` in `dm/mohdm1`, then loads `dm/mohdm2`. Against GL1
(`compare.sh`, `summary.py`), `dm/mohdm3`, `obj_team2`, `obj_team4`,
`mp_mohdm1` and `mp_obj_team4` give the numbers of the tables above at
640x480 and at 4160x240, `dm/mohdm1`, `mohdm2_fog` and `mohdm4_fog` the
same kind (at most 0.17 % of the pixels differ at 4160x240), and the
main menu matches GL1 (fan masked; `menus.txt` gives the numbers of its
paragraph above).

The world culling (2026-10-17, see Performance, World culling), against
the build before it (`run.sh`, `tgadiff.py`, 640x480) on the 14 `dm`, 5
`obj` and 5 multiplayer lists, `m2l1_skyportal`, `m2l3`, `m2l3_fog`,
`m3l3`, `m4l1_fog`, `m5l3`, `m6l1a` and `m6l2a`, the dynamic light lists
with `r_fastdlights 1`, `mohdm1_texenv` with `r_ext_texture_env_add 1`,
`obj_team2` and `dm/mohdm3` with `r_novis 1`, and `obj_team2` with
`r_nocull 1` and with `r_facePlaneCull 0` (170 views): 154 views give the
same frames to the bit, among them every view of `obj/obj_team1` to
`obj/obj_team4`, of the dynamic light lists in both modes, of the
multiplayer lists but 2 pixels of `pm7_front`, and of `m2l3`, `m3l3`,
`m6l2a`; the other 16 are the random content and run-to-run noise named
above (snow in `mohdm5_fog` and `m6l1a`, a particle in `m53_street`, and
near the camera `d7_alley`, `d7_arch`, `fm4_a`/`fm4_b`, `m61_slope`,
`pm7_front` and `s2_a`/`s2_b`, whose few larger pixels at the bottom edge
also differ between two runs of one build). Two views differed at first,
`dl_a` (`r_fastdlights 1`) and `dbg_normals` (see Performance, World
culling, change 1); they no longer cull faces there. Against GL1
(`compare.sh`, `summary.py`): `dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`,
`obj_team1` to `obj_team4`, `mp_mohdm1`, `mp_obj_team4` and
`mohdm1_dlight` give the numbers of the tables above at 640x480, and
`dm/mohdm3`, `dm/mohdm7`, `obj_team1`, `obj_team2`, `obj_team4`,
`mp_mohdm1` and `mp_obj_team4` at 4160x240 (`dm/mohdm7` 48/36/35,
0.070 %, 56: the `d7_window` crack; `obj_team1` 0.149 %); the main menu
matches GL1 (fan masked; `menus.txt` gives the numbers of its paragraph
above, 17 pixels off by 1 in `mn_video`, the views over `m1_spawn` that
view's differences, 5 to 7.4 %).
No validation message in these runs (`r_vkValidation 1`), nor with
synchronization validation (`r_vkValidation 2`) on `mohdm1_dlight` in
both modes, `m2l1_skyportal`, `obj_team2`, `mp_mohdm1` and a script that
loads `dm/mohdm1`, runs `vid_restart`, loads `obj/obj_team2`, turns
`r_novis`, `r_lockpvs` and `r_nocull` on and off, and loads `dm/mohdm3`;
`renderer_vulkan.so` imports no GL or Vulkan symbol and exports only
`GetRefAPI`.

Pipelines and skin data at load (2026-10-18, see Performance, Pipelines
and skin data at load), against the build before it (`run.sh`,
`tgadiff.py`, 640x480) on the 41 view lists (177 views): 146 give the
same frames to the bit, among them every view of the `dm`, `obj` and
multiplayer lists but the snow of `mohdm5_fog` and the near-camera ±1 of
`d7_alley` and `d7_arch`, of the menus, the dynamic light lists, the debug
drawing list, `m2l3`, `m3l2`, `m3l3`, `m5l3`, `m6l2a` and `m6l3a`. The
other 31 are in the 11 lists with random content or run-to-run noise named
above (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`, `m3l1a`, `m4l1_fog`,
`m5l1a`, `m5l2a`, `m6l1a`, `mohdm5_fog`, `mohdm7`): two runs of the build
before differ in 29 of the 43 views of those lists in the same way, and
the one view that differed only between the builds, `m13_sky`, differs by
a tracer the AI fired in one run (the rest of its frame is the same).
With `r_ext_multisample 4` (`mohdm1`, `mp_mohdm1`), `r_fastdlights 1` and
`r_dlightBacks 0` (the dynamic light lists), `r_vkFallbacks 3` (`mohdm1`,
`obj_team2`, `mp_mohdm1`: keys with the RGB bit of every unit, bindings
read per instance), `r_gpuSkinning 0` (`mp_mohdm1`, `mp_obj_team4`) and
`r_lightmap 1` (`mohdm1`: `TEXENV_REPLACE` with the RGB lightmaps) the 46
views are the same to the bit. No validation message in these runs
(`r_vkValidation 1`), nor with synchronization validation (`r_vkValidation
2`, and 1) in a script that starts the game, loads `obj/obj_team2`, runs
`vid_restart` in it, changes the mode to 800x600 with `vid_restart`,
loads `dm/mohdm1`, goes back to 640x480, disconnects, plays the `o2match`
demo and quits (the pipelines of each load built at its end, the unused
ones of the previous load destroyed, the keys file written at each
`vid_restart` and at shutdown and read again). Against GL1 (`compare.sh`,
`summary.py`): `obj_team2`, `mp_mohdm1`, `dm/mohdm3`, `obj_team4` and
`mp_obj_team4` give the numbers of the tables above at 640x480 and at
4160x240, `mohdm1_dlight` its known per-pixel light differences, and the
main menu matches GL1 (fan masked; `menus.txt`: 0 pixels in every menu
but the 17 of `mn_video`, the views over `m1_spawn` 5.2 to 7.4 %).
`renderer_vulkan.so` imports no GL or Vulkan symbol and exports only
`GetRefAPI`.

Skeletal culling (2026-10-19, see Performance, Skeletal culling), against
the verification build of that step (`run.sh`, `tgadiff.py`, 640x480, the
41 view lists, 177 views): that build draws every model as the build
before does and only adds the checks (the same frames as the build before
to the bit in `m3l3`, `mohdm1_debug`, `obj_team2` and `mp_mohdm1`); 144
views give the same frames to the bit, among them every view of the `dm`,
`obj` and multiplayer lists but the snow of `mohdm5_fog`, the near-camera
±1 of `d7_alley` and `d7_arch` and 2 pixels of `pm7_front`, of the
menus, the dynamic light, decal, fog and sun flare lists, `m2l3`, `m3l2`,
`m6l2a` and `m6l3a`. The others are in the lists with random content or
run-to-run noise named above (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`,
`m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m6l1a`, `mohdm5_fog`, `mohdm7`,
`pm7_front`, a particle in `m53_street`, one pixel of `m33_grass`, which
two runs of the build before also differ in), and `dbg_normals`
(`mohdm1_debug`, `r_shownormals`): it draws the normals of batches that
don't compute them, what earlier batches left in `tess.normal`, and a
culled model no longer leaves its own, so the cull is off with
`r_shownormals` (as world culling's is); with that the view is the same
to the bit. The final build against the build before on `m3l3`,
`mohdm1_debug`, `obj_team2` and `mp_mohdm1`: the same frames to the bit
(the four soldiers beside the camera of the `mp_*` front views are
culled). No validation message in these runs (`r_vkValidation 1`).

The submit thread (2026-10-19, see Performance, Submit thread), against
the build before (`run.sh`, `tgadiff.py`, 640x480, `r_vkValidation 1`):
with `r_vkSubmitThread 1` on the 41 view lists (177 views) 146 give the
same frames to the bit, among them every view of the `dm`, `obj` and
multiplayer lists but the snow of `mohdm5_fog` and the near-camera ±1 of
`d7_alley` and `d7_arch`, the menus, the dynamic light, decal, fog, sun
flare and debug drawing lists; the other 31 are in the lists with random
content or run-to-run noise named above (`m1l1`, `m1l2a`, `m1l3a`,
`m2l1_skyportal`, `m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m6l1a`,
`mohdm5_fog`, `mohdm7`, a particle in `m53_street`, 2 pixels of
`m33_grass`). With `r_vkSubmitThread 0` and 2 (every frame drawn
offscreen and copied by the thread) on 12 of the lists (`mohdm1`,
`mohdm2_fog`, `dm/mohdm3`, `m2l1_skyportal`, `obj_team2`, `obj_team4`,
`mp_mohdm1`, `mp_obj_team4`, `menus`, `mohdm1_dlight`, `mohdm1_debug`,
`mohdm1_texenv`; 60 views each) the same, but the bottom edge of
`s2_a`/`s2_b`. Screenshots read the frame's color target, the swapchain
image or the offscreen one: what reaches the screen through the thread's
copy was checked by capturing the window (`spectacle -b -n -a -e`, paused
`m1_spawn`): the same pixels with the copy and drawn directly, and as the
screenshot but for 14 pixels in the window's two rounded bottom corners
(KWin). On llvmpipe `mohdm1` and `mp_mohdm1` give the same frames with
`r_vkSubmitThread` 0, 1 and 2. No validation message in any of these
runs, nor with synchronization validation (`r_vkValidation 2`) through
startup, a map load, live resizes (a burst of 11 sizes 50 ms apart, the
frames copied with scaling until their `vid_restart`), minimize and
restore, fullscreen toggles, `r_vkSubmitThread` switched off and on (and 1
and 2) with `vid_restart`, a mode change and quit, with immediate, FIFO
and the copy, on RADV and llvmpipe; nor with a temporary build that
reported a window without area for 1 s of every 3 (the minimized path of
Windows: the swapchain destroyed and created again 6 times in 30 s, the
frames between submitted without presenting, a `vid_restart` in it) with
`r_vkSubmitThread` 0, 1 and 2. Against GL1 (`compare.sh` with the thread
on): `dm/mohdm1` and `menus.txt` give the numbers of before (the main menu
0 pixels with the fan masked, `mn_video` 17). `renderer_vulkan.so` imports
no GL or Vulkan symbol and exports only `GetRefAPI`; the build the timings
of FIFO, mailbox and the frame logs ran with differs from the final one in
`VK_CreateSwapchain` alone (a test override of the image count, removed),
the earlier batches' build in the frame count being a variable (for the
test with 3) and two such overrides.

Redundant state and empty flushes (2026-10-20, see Performance, Redundant
state and empty flushes), against the build before (`run.sh`,
`tgadiff.py`, 640x480, `r_vkValidation 1`) on 12 of the lists (`mohdm1`,
`mohdm2_fog`, `dm/mohdm3`, `m2l1_skyportal`, `obj_team2`, `obj_team4`,
`mp_mohdm1`, `mp_obj_team4`, `menus`, `mohdm1_dlight`, `mohdm1_debug`,
`mohdm1_texenv`; 60 views), `mohdm1_dlight` with `r_fastdlights 1` and
`mohdm1_texenv` with `r_ext_texture_env_add 1`: the same frames to the bit
but 14 and 21 pixels of `s2_a` and `s2_b` (`m2l1_skyportal`: a figure in
the distance, the bottom edge), where two runs of the build before differ
in 15 and 22. Each part on its own (the setters and the flush, separate
builds) on `mohdm1`, `obj_team2`, `mp_mohdm1`, `menus` and `mohdm1_debug`:
the same frames as the build before. No validation message in these runs,
nor with synchronization validation (`r_vkValidation 2`) in a script that
loads `dm/mohdm1`, takes a screenshot, runs `vid_restart`, loads
`obj/obj_team2`, plays the `o2match` demo and quits, nor in a session that
connects to a local dedicated server 7 times (each new world loaded without
a registration, see Performance). `renderer_vulkan.so` imports no GL or
Vulkan symbol and exports only `GetRefAPI`.

The effects shader (2026-10-20, see Performance, Effects shader), against
the build before (`run.sh`, `tgadiff.py`, 640x480, `r_vkValidation 1`): on
the 41 view lists (177 views) 144 give the same frames to the bit, among
them every view of the `dm`, `obj` and multiplayer lists but the snow of
`mohdm5_fog` and the near-camera ±1 of `d7_alley` and `d7_arch`, of the
menus, the dynamic light (static models lit by the lights near them in
`mohdm1_dlight2`), decal, fog, sun flare, texture env and debug drawing
lists, `m2l3`, `m3l2`, `m3l3`, `m6l2a` and `m6l3a`; the other 33 are in the
lists with random content or run-to-run noise named above (`m1l1`,
`m1l2a`, `m1l3a`, `m2l1_skyportal` with the 15 and 21 pixels of `s2_a` and
`s2_b`, `m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`). The
dynamic light lists with `r_fastdlights 1` and `mohdm1_texenv` with
`r_ext_texture_env_add 1` give the same frames. What these views draw with
the effects shader, counted with a temporary build (whose frames are the
final build's): 2.8 million draws in the 41 lists, with the deforms `wave`
and `flap t` (the trees of every forest, curtains, flags), `tcGen
environment` and `environmentmodel` (windows, wet walls), `tcMod`s of the
add and scale kinds (water), `distFade` on vertex colors (the trees) and
on constants, brush models, and static models lit by the lights near them
(3334 draws in `mohdm1_dlight2`, `mp_mohdm7` and `m1l1`). The rest (`move`,
`bulge`, `flap s`, a `wave` without frequency, `tcGen vector`, `rotate`,
`stretch`, `transform`, `turb`, `oneMinusDistFade`) only `fx_mohdm1` has
(see Test content); its 5 views give the same frames with the effects
shader, with `r_staticEffects 0` and with the build before, and the same as
the build before with `r_vkFallbacks` 1, 2 and 3 (no view swizzle, no
binding of stride 0; the view swizzle fallback itself changes one pixel of
`fx_b` by 1 in both builds), and `mohdm1`, `obj_team2`, `mp_mohdm1` and
`mohdm1_dlight2` the same with `r_vkFallbacks 3`. On llvmpipe
(`VK_DRIVER_FILES=.../lvp_icd.json`), whose float operations don't follow
`precise` the way RADV's do, the effects shader's `distFade` alpha differs
by 1 in one pixel of `fx_b` and of `fx_c` from llvmpipe's CPU path (`mohdm1`
the same), as GPU skinning does in two pixels of `pm1_front` and
`pm1_flash` there (`r_gpuSkinning` 0 against 1; the same to the bit on
RADV). No validation message in these runs, nor with synchronization
validation (`r_vkValidation 2`) on `fx_mohdm1`, `mohdm1_dlight2` and
`dm/mohdm3`. (Demos aren't a frame check: screenshots taken during a demo
played with `fixedtime 16` differ between two runs of the same build by up
to 62 % of their pixels.)

cgame's caches and the skinned passes (2026-10-20, see Performance, cgame
caches, and Performance, Skinned passes), against the build before
(`run.sh`, `tgadiff.py`, 640x480, `r_vkValidation 1`): on the 42 view lists
and the two new ones, `mp_shadows` and `skin_passes` (190 views), the final
renderer (a temporary copy that also counted what it skinned, whose frames
are the final renderer's) with the final cgame gives the same frames to the
bit in 158, among them every view of the `dm`, `obj`, multiplayer and new
lists but the snow of `mohdm5_fog` and the near-camera ±1 of `d7_alley`
and `d7_arch`; the others are in the lists with random content or run-to-run
noise named above (`m1l1`, `m1l2a`, `m1l3a`, where AI fire put the red
damage flash into the build before's `m13_sky`, `m2l1_skyportal`, `m3l1a`,
`m3l3`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`). An earlier pass with
a first version of the renderer's rule (without the stages' own alpha, see
Implementation notes, Skeletal models) on 186 views, the renderer alone and
with the cgame, gave the same. `mp_shadows` has the blob and feet shadows of
`cg_shadows` 1 and 2 on its soldiers, `skin_passes` every shader that the
generic iterator's skinned batches drew in these lists and in four real
matches (counted with that copy: the Afrika Korps goggles in every
multiplayer list, the windows of the Focke-Wulf in `dm/mohdm4` and
`obj/obj_team4`, a caged light bulb, the gloves of `m4l1_fog`, the
bullet-hit debris and the splash drops of `m3l1a`, the sniper lenses, the
tank commander's coat, hat and gloves in an `obj/obj_team1` match), with
and without dynamic lights next to them: its 4 views are the same with the
build before, the new renderer and the new renderer with `r_gpuSkinning 0`
(the CPU's skinning). In the same build, GPU against CPU skinning on
`mp_mohdm1`, `obj_team4`, `mohdm4_fog`, `mohdm1_dlight` and `m4l1_fog`: the
same frames but a pixel of `fm4_a` off by 1 (its known noise); with
`r_vkFallbacks 3` (`mp_mohdm1`, `obj_team4`) the same as the build before
with it; on llvmpipe the same as the build before (both 2 pixels of
`pm1_front` and `pm1_flash` off by 1 from llvmpipe's CPU skinning, as
before); GL1 with the new cgame against the cgame before (`mp_shadows`,
`mohdm1`): the same frames. A temporary cgame that also did everything the
caches skip and compared the results (the shadow trace, the mark's
fragments and verts, the count of temporary models) found no difference in
20 real matches played with `cg_shadows` 1 and 2 (0.26 million traces and
0.59 million marks taken from the caches, 1.2 million counts). No
validation message in these runs, nor with synchronization validation
(`r_vkValidation 2`) on `mp_mohdm1`, `obj_team4` and `mp_shadows` and in
3000 frames of the `o2match` demo with `cg_shadows 2`.
`renderer_vulkan.so` imports no GL or Vulkan symbol and exports only
`GetRefAPI`.

The 64 dynamic lights (2026-10-20, see Performance, Dynamic lights, 64 of
them), against the build before (`run.sh`, `tgadiff.py`, 640x480,
`r_vkValidation 1`): on the 45 view lists (195 views, with the new
`mohdm1_dlight64` and `dl2_door3`) 156 give the same frames to the bit,
among them every view of the `dm`, `obj` and multiplayer lists but the snow
of `mohdm5_fog`, the near-camera ±1 of `d7_alley` and `d7_arch` and 2
pixels of `pm7_front`, and every view of the dynamic light, skinned pass
and shadow lists with up to 4 lights (the static models lit by lights, the
soldiers by the muzzle flashes); the others are `dl2_door3` (the door lit
by its three lights, where the build before lit its lightmap with light 0
alone), the four views of `mohdm1_dlight64` (the build before kept the
first 32 of its 40 lights, and with 32 gave no world surface a light; the
new one at the default keeps the 4 nearest the view) and the lists with
random content or run-to-run noise named above (`m1l1`, `m1l2a`, `m1l3a`,
`m2l1_skyportal`, `m3l1a`, `m3l3`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m5l3`,
`m6l1a`). With `r_maxDlights 64` (the dynamic light lists, `mp_mohdm1`,
`skin_passes`) and with `r_fastdlights 1` (the dynamic light lists) the
same but `dl2_door3` and `mohdm1_dlight64` (with `r_fastdlights 1` the
door's three lights projected, where the build before projected light 0
alone); `mohdm1_texenv` with `r_ext_texture_env_add 1` the same. The
shader's count path checked against its mask path with temporary builds:
one that gave every draw with lights a list of its lights with their
number, one that gave every draw whose first light isn't the list's first
a list from its first light (the window), against the final build on
`mohdm1_dlight`, `mohdm1_dlight2`, `mohdm3_dlight`, `mp_mohdm1`,
`skin_passes` and `mohdm1_dlight64` with `r_maxDlights 64` (where the large
faces take the count path by themselves): the same frames to the bit, on
RADV and (the count path, `mohdm1_dlight2` and `mohdm1_dlight64`) on
llvmpipe. No validation message in any of these runs.

The terrain pool and walk (2026-10-22, see Performance, Terrain), against
the build before (`run.sh`, `tgadiff.py`, 640x480, `r_vkValidation 1`): on
the 45 view lists (195 views) 164 give the same frames to the bit, among
them every view of the `dm`, `obj` and multiplayer lists and of `m3l3` and
`m6l2a` but the snow of `mohdm5_fog`, the near-camera ±1 of
`d7_alley` and `d7_arch` and 2 pixels of `pm7_front`; the other 31 are in
the lists with random content or run-to-run noise named above (`m1l1`,
`m1l2a`, `m1l3a`, `m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m6l1a`). On the ten
lists with terrain (`mohdm2_fog`, `dm/mohdm3`, `mohdm3_dlight`,
`obj_team1`, `obj_team3`, `obj_team4`, `mp_obj_team1`, `mp_obj_team4`,
`m3l3`, `m6l2a`; 44 views) the new build with `r_staticTerrain 0` and a
build with the pool but GL1's walk over every patch give the base build's
frames too (but 2 and 1 pixels of `m33_grass`, its noise), and so do the
new build against the base build with `ter_cautiousframes 3` and with
`r_vkFallbacks 3`; `mohdm3_dlight` and the dynamic light lists with
`r_fastdlights 1` (terrain lit by the projected pass, from `tess`) and
with `r_maxDlights 64`, and `mohdm1_texenv` with `r_ext_texture_env_add
1`: the same. On llvmpipe the pool against `r_staticTerrain 0`
(`obj_team3`, `mohdm3_dlight`, `obj_team4`): the same. No validation
message in these runs, nor with synchronization validation (`r_vkValidation
2`) in a session that loads `obj/obj_team4`, runs `ter_restart` (the pool
made again), turns `r_staticTerrain` off and on, runs `vid_restart`, loads
`dm/mohdm1` (no terrain) and `obj/obj_team3`, plays the `o4match` demo and
quits, nor in one that connects 5 times in turn to two local dedicated
servers, `obj/obj_team4` and `dm/mohdm1` (each world loaded without a
registration, a map without terrain between two with it).
`renderer_vulkan.so` imports no GL or Vulkan symbol and exports only
`GetRefAPI`.

The 2D batches (2026-10-22, see Performance, 2D batches), against the build
before (`run.sh`, `tgadiff.py`, 640x480, `r_vkValidation 1`): on the 46 view
lists (198 views, with the new `hud`) 167 give the same frames to the bit,
among them every view of the `dm`, `obj` and multiplayer lists but the snow
of `mohdm5_fog`, the near-camera ±1 of `d7_alley` and `d7_arch` and 3 pixels
of `pm7_front`, the menus and the HUD (the compass masked, see Test
content); the other 31 are in the lists with random content or run-to-run
noise named above (`m1l1`, `m1l3a`, `m2l1_skyportal`, `m3l1a`, `m4l1_fog`,
`m5l1a`, `m5l2a`, `m5l3`, `m6l1a`). With `r_batch2D` 0, 1 and 2 (each part
alone, and both off) `menus`, `hud`, `mohdm1`, `obj_team2`, `mp_mohdm1`,
`mohdm2_fog` and `mohdm1_debug` (37 views each) give the build before's
frames too. The lists draw few 2D draws that join a batch: they hide the
HUD, and the compass, whose triangles join in every frame of a match, turns
with the wall clock. What joins was compared within one run instead, the
same frame with and without the batches: a single-player view of
`dm/mohdm1` with the HUD, paused (the compass settles: two shots with the
same setting are the same), taken with `r_batch2D` 3, 0, 1, 2 and 3 again:
the same to the bit, compass included; and the `o2match` and `o4match`
demos frozen (`cl_freezeDemo 1`) at 10 moments each, taken three times two
frames apart with `r_batch2D` 3, 0 and 3, then 3, 1 and 3 (a `wait 32` is a
frame: `Cbuf_Execute` takes a frame's 16 ms off a `wait` before it stops):
16 of the 40 triples are the same to the bit outside the compass, which keeps
turning in a frozen demo, among them frames with 1 to 6 kill feed lines of
one color; in the other 24 the two shots with the same setting differ as
well (lines fading, with the wall clock), and in none does the middle shot
differ where those two are the same. A temporary build that copied the
renderer's state when a batch opened (`glState`, the backend's recorded
state, `vk_dirty` and draw count, `backEnd` but its lighting spheres, the
shader's stage) and compared it at every draw that joined the batch and
before drawing it found no difference in 20 matches (667 thousand frames of the matches, 28.6 million checks, 7.6
joined draws a frame), nor any image replaced or uploaded into while a
batch was open; its first run on `menus` found one, which made
`RE_BeginFrame` end the batch: a pic drawn between frames when a menu is
pushed, its batch still open when the next frame began (`glState.finishCalled`
changed). On llvmpipe (`VK_DRIVER_FILES=.../lvp_icd.json`) the paused
frame's shots with `r_batch2D` 3, 0, 1 and 2 are the same to the bit too. No
validation message in these runs, nor with synchronization validation
(`r_vkValidation 2`) on `hud`, `menus` and the toggles of `o4match` frozen.
Against GL1 (`compare.sh`): `menus` gives the numbers of before (0 pixels
in every menu but the 17 of `mn_video`, the views over `m1_spawn` 5.2 to 7.4
%), `hud` the same HUD over a 3D view that differs where trees sway (a team
match can't be paused, and GL1's screenshot reads a frame from two or three
swaps earlier, see Deterministic 3D views). `renderer_vulkan.so` imports no
GL or Vulkan symbol and exports only `GetRefAPI`.

The light grid lookups (2026-10-22, see Performance, Light grid lookups),
against the build before (`run.sh`, `tgadiff.py`, 640x480, `r_vkValidation
1`): on the 46 view lists (198 views) 167 give the same frames to the bit,
the other 31 are in the lists with random content or run-to-run noise named
above (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`, `m3l1a`, `m4l1_fog`,
`m5l1a`, `m5l2a`, `m5l3`, `m6l1a`, `mohdm5_fog`, and ±1 in `d7_arch` of
`mohdm7`); the same 167 as the 2D batches gave. The new list `smoke` (debris and smoke emitters in
front of the camera on `dm/mohdm1`, for timing: see Performance, Light grid
lookups) has random content: its debris lands elsewhere in every run, and
two runs of one build differ in 99.6 % of the pixels. A verification build
that ran the old function next to every lookup and compared the results'
bits found no difference in 4 matches (`o2match`, `o4match` and two
`dm/mohdm6` matches with the most debris; 5.74 million lookups, 4.09
million of them answered by the table of points). No validation message in
these runs, nor with synchronization validation (`r_vkValidation 2`) in a
session that loads `obj/obj_team4`, runs `vid_restart`, loads `dm/mohdm6`
and `obj/obj_team4` again (the tables made again with each world) and
plays a `dm/mohdm6` match. `renderer_vulkan.so` imports no GL or Vulkan
symbol and exports only `GetRefAPI`.

The registration in the match (2026-10-22, see Performance, Registration in
the match; a cgame and engine change, the renderer's code the same), the
engine and cgame of the build before against the new ones (`run.sh`
through wrappers, `tgadiff.py`, 640x480, `r_vkValidation 1`): on the 47
view lists and 4 of them in a team match (222 views; `team_obj_team2` is
one of those, kept) 186 give the same frames to the bit, among them every
view of the team matches, where the registration at load runs (`hud`, the
views of `obj/obj_team1`, `obj/obj_team2`, `obj/obj_team4` and `dm/mohdm6`),
but `mpo2_yard` below (the other lists are single-player games, which only
the directory listings change); the other 36 are in the lists with random content or
run-to-run noise named above, 2 pixels of `pm7_front`, and `mpo2_yard`,
whose swaying tree landed on another phase in one of ten runs (the other
nine of both builds, and GL1's, the same). What a match registers was
traced over the 21 matches with a temporary engine build (see
Performance), and an OpenMoHAA dedicated server with 8 bots joining a team
match was played against (`omohaaded`, `obj/obj_team2`). No validation
message in these runs, nor with synchronization validation
(`r_vkValidation 2`) on `hud` and two team match lists and 3000 frames of
`o2match`. GL1 (`cl_renderer opengl1`) gives the same frames with the new
engine and cgame as with the old ones in the team match lists (but
`mpo2_yard`'s tree, which differs between two runs of the old ones too)
and plays `o2match` without a frame that registers. `renderer_vulkan.so`
is the same code (`.text`) as the build before.

The round changes (2026-10-23, see Performance, Round changes; the engine
and cgame, the renderer the same), the engine and cgame of the build before
against the new ones (`run.sh` through wrappers, `tgadiff.py`, 640x480,
`r_vkValidation 1`): on 18 view lists (`menus`, `hud`, `dm/mohdm1`,
`dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`, `obj/obj_team1` to `obj/obj_team4`,
three soldier lists, `team_obj_team2`, `fx_mohdm1`, `mohdm1_dlight`,
`smoke`, `m2l3`; 88 views) 84 give the same frames to the bit, the other 4
are `mohdm7`'s two views with their run-to-run noise (pixels off by 1) and
`smoke`'s, whose debris lands elsewhere in every run. GL1 gives the same
frames with the new engine and cgame as with the old ones in `hud`,
`obj_team4`, `mp_obj_team4` and `team_obj_team2` (18 views). No validation
message in these runs, nor with synchronization validation
(`r_vkValidation 2`) through 5000 frames of `o4match` (a round change).
With temporary test builds (not kept):

- The sound: every buffer a stream channel queued (hashed) and the PCM of
  the first streams, `s_streamThread 1` against the codec's file streams
  given a dithering sequence of their own, in real time at `com_maxfps 250`
  with the volume up and OpenAL Soft's wave backend as the device (a file
  instead of the speakers): `o4match` and `o2match` with test mp3s in the
  home path as their maps' songs (a 3 minute one for `obj/obj_team4`, a
  1.5 s one that loops for `obj/obj_team2`), and a local game script that
  switches soundtracks (`loadsoundtrack`), changes `s_ambientvolume` and
  `s_volume`, runs `snd_restart`, `restart` and a `devmap` (with a mono
  22 kHz mp3 as `dm/mohdm1`'s song): 274 buffers of the songs compared,
  none different, and no underrun (a song's source stopped for want of a
  queued buffer) with either. The recorded output of the script has the
  same songs at the same levels with the stream thread, the file streams
  and the file streams' shared dithering; against that sequence the
  stream thread's dithering differs by at most 2 in a sample (mean 0.46).
- ThreadSanitizer (the engine and cgame built with `-fsanitize=thread`):
  `o4match` with the 3 minute song (its round changes), `o2match` with the
  looping one and the game script (soundtrack switches, `snd_restart`, a
  map change): no report but one in RADV's disk cache thread (a barrier),
  in each run.
- The configstrings: a shadow of the client's gamestate changed the old way
  and compared with the new one, string by string, after every change, and
  cgame's copy compared with the client's after every change, over the 21
  matches played whole (at least 4240 changes in the client, 5200 in
  cgame): no difference; nor with the appends limited so that the old copy
  runs every few changes (both paths in turn, `o2match` and `o4match`).
  The 21 matches played with the stream thread and the 3 minute song as
  `obj/obj_team4`'s: no crash.
- `obj/obj_team1`'s flak wreck (see Plan, Remaining 1), one run of that
  match with the registration trace of Plan 28: the wreck and its
  explosion load with the map since `z_forteffa.pk3` left the game data.

The busy moments (2026-10-23, see Performance, Busy moments; the renderer
and the engine's sound system), the renderer of the build before against
the new one, both with the new engine (`run.sh` through `cmplists.sh`,
`tgadiff.py`, 640x480, `r_vkValidation 1`): on 47 view lists (all but
`m3l1a` and the performance lists; 203 views), with the sky's faces
culled then, 169 give the same frames to the bit; 33 others are in lists
whose frames differ between two runs of the build before too (run again
with it twice, with the sky part off and on: the snow of `dm/mohdm5` and
`m6l1a`, `dm/mohdm7`'s and `pm7_front`'s pixels, the vehicle starts of
`m1l1` and `m5l2a`, `m1l2a`, `m1l3a`, `m5l1a`, `m5l3`'s street (one
pixel), `m2l1_skyportal`, `m4l1_fog`, `m3l3`'s grass, `smoke`'s debris);
the last, `fx_mohdm1`'s view from above the map (`fx_e`), changed with the
sky's faces culled and is the same with that part off, which was then
left out. The final build against the build before on 13 lists
(`fx_mohdm1`, `obj/obj_team1` to `obj/obj_team4`, `dm/mohdm1`,
`dm/mohdm3`, two soldier lists, `team_obj_team2`, `m3l3`, `m6l2a`,
`hud`; 62 views): the same frames to the bit; `r_terrainCache` 0, 1, 2
and 4 on `m5l3`: its views the same but the street's random pixel. No
validation message in these runs. No synchronization
validation message (`r_vkValidation 2`) through 4000 frames of `o4match`;
GL1 plays `o2match` and `o4match` with the new engine. `renderer_vulkan.so`
imports no GL or Vulkan symbol. With temporary test builds (not kept):

- The terrain's walks: the verification build described in Performance,
  Busy moments, over the 21 matches (no walk left out that would have
  done something, every kept index list the same as the walk).
- The sounds' buffers: every buffer's samples (size, rate, a checksum)
  made at the load against those made on the first plays
  (`s_loadBuffers` 0), `o2match` and `o4match`: the same for the 118 and
  146 sounds both made; none made in the match with it on. A local game
  through four map loads with bots and a `snd_restart` (the freed sounds'
  buffers deleted at each load): no OpenAL error.

The sound system (2026-10-23, Plan 31, see Implementation notes, Sound
system; the engine only, `renderer_vulkan.so` and `cgame.so` the same code
as before). With temporary test builds (not kept) of the merged pull
request as it came (the merge commit, conflicts resolved) and of the final
one: the null playback device (no sound out of the speakers), the device
callback's output written to a file and every command the streams gave the
mixer (the queued PCM, seeks, stops) logged:

- The streams: session by session, the chunks queued, the stops and seeks
  and the PCM each stream plays from its start offset: `o2match` with a 3
  minute test song (24 s of it at `com_maxfps 250`, 524 chunks, 4.3 MB),
  `o4match`'s songs (started and freed at its round changes) and dialog
  lines: the same chunks, seeks and bytes as the merged pull request.
  `o2match` with this machine's 0.24 s songs, which loop: the same up to
  the first loop, after which the final build repeats the song from its
  loop start to the byte (12 loops) and the merged code lost 1318 samples
  at every loop (Plan 29's thread with the pull request's reads). The
  mixed output of the 3 minute song: no 5 ms window below -80 dB once the
  music starts.
- A local game script (`dm/mohdm1`): `loadsoundtrack` of two soundtracks
  with 3 minute songs, `s_ambientvolume` and `s_volume` changes,
  `snd_restart`, `restart`, a `devmap` to `obj/obj_team2`, `tmstart` and
  `tmstop`, `playmp3` and `stopmp3`: the music in the output all along but
  the loads, no error; `playmp3` crashed the merged code (a stack
  overflow, see Implementation notes), not the final one.
- Headphones (`s_speaker_type 1`, `fs_basepath` at the game data): the
  HRTF set read from the client's `main` folder (the merged code fell back
  to stereo); at 44.1 kHz the pull request's baked filters fail its own
  check (median -27.4 dB, max -8.0 dB) and every voice designs its filters
  from the measurements on the mixer thread, as it came; at 48 kHz, now
  the headphones' rate, the check passes (median -30.6 dB, max -15.6 dB)
  and the baked filters are used (0.61 s for a sound start against 0.88).
  `o2match` with headphones on the null device: the same levels as the
  merged code at `s_khz 48` (RMS within 0.5 %), no silence; with
  `s_hrtf_live_query 1` (filters from the measurements) the design thread
  runs and the output is as loud. A test program running the HRTF code of
  the merged commit and the final one (a voice still, turning 1.5° a
  callback, still again; 36 voices freed and taken again while their
  filters were being designed), the live path in both: the final one's
  output bit-identical to the merged code's from the second callback after
  a voice starts or stops turning (10 ms; until then it plays the baked
  filter, then the crossfade), one callback behind while it turns; the
  designs for freed voices dropped. ThreadSanitizer clean on the test
  program (the thread and the mixer at once) and on the game in live mode
  (the demo, the live script with its `snd_restart` and speaker changes)
  but RADV's own report.

The surface list (2026-10-23, Plan 32, see Performance, Surface list; the
renderer only), the merged pull request as it came and the final build
against the build before (`run.sh` through `cmplists.sh`, `tgadiff.py`,
640x480, `r_vkValidation 1`): on 47 view lists (all but `m3l1a` and the
performance lists; 203 views) the merged pull request gives the same
frames to the bit in 171 views, the final build in 172; the others are in
the lists whose frames differ between two runs of one build (see Busy
moments above: `smoke`'s debris differs in 99 % of its pixels from run to
run, `m1l1`'s `m11_flag` by up to 99.7 % with the convoy's lights and
dust). No validation message in these runs; no synchronization validation
message (`r_vkValidation 2`) through `hud`, `mp_mohdm1`, `mohdm1_dlight`,
`menus` and 3000 frames of `o4match`. GL1 against the final build on
`hud`, `mp_mohdm1`, `obj_team2` and `dm/mohdm1`: the differences of the
build before (the same frames; `hud`'s team match can't be paused, so its
swaying trees differ). `perf.py --cold` (nothing recorded): no pipeline
created in the measured frames of `o2match` and `o4match` (the merged pull
request: 2 and 4). `renderer_vulkan.so` imports no GL or Vulkan symbol.
With temporary test builds (not kept): the counts and the TSC cycles of
Performance, Surface list, and the pull request's lighting cache with a
count of the hits taken before a list's first setup.

No OpenGL, `cl_movefps` and no `m_filter` (2026-10-23, Plan 33; the engine,
cgame and the renderer's names) against the build before (53d8f9d7 in a
directory of its own, run through a wrapper; `cmplists.sh`, 640x480,
`r_vkValidation 1`): of the 206 views of the 48 lists with shots, 167 give
the same frames to the bit; the 39 others are in the lists whose frames
differ between two runs of one build (`m1l1`, `m1l2a`, `m2l1_skyportal`,
`m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`, `mohdm5_fog`,
`mohdm7`, `pm7_front`, `smoke`) but five views of `m3l2`, which one run of
the new build gave with their trees, water and a wheel off at the edges,
as if turned by a fraction of a pixel; run again (the build before, the
new build, the final one, the build before, the new build) `m3l2` gave the
same frames to the bit in all five runs. No validation message; no
synchronization validation message (`r_vkValidation 2`) through `menus`,
`hud`, `mp_mohdm1`, `mohdm1_dlight` and `dm/mohdm1`. `glcheck.sh`: clean
on the source and the build (on 53d8f9d7's build it reports
`renderer_opengl1.so` and its `SDL_GL_*` imports); the running game maps
no OpenGL, GLX or EGL library. `renderer_vulkan.so` of 53d8f9d7 still runs
under the new engine (`perf.py` through `renderer_timed.so`).

`r_fastSync` (2026-10-24, Plan 34; the renderer only) against the build
before (30a12f80; `cmplists.sh`, 640x480, `r_vkValidation 1`): the 26
views of `menus`, `hud`, `mp_mohdm1`, `mohdm1_dlight` and `dm/mohdm1` give
the same frames to the bit with `r_fastSync` 0 and 1 (mailbox in these
windowed runs under Xwayland) and in a second run of the build before. No
validation message, and no synchronization validation message
(`r_vkValidation 2`), through `r_fastSync` switched on, off and on again
with `vid_restart` (the last one fullscreen), under Xwayland and as a
native Wayland client, with `r_vkSubmitThread` 0 and 1 (5 swapchain
images with mailbox too). The present mode chosen (the `Vulkan present
mode` line): the build before, immediate always; the merged pull request,
mailbox only with `r_fastSync 1` and `SDL_VIDEODRIVER=wayland`; the final
build, mailbox with `r_fastSync 1` under both. `glcheck.sh` clean;
`renderer_vulkan.so` imports no GL or Vulkan symbol.

SDL 3 (2026-10-24, Plan 35; engine, renderer and tools) against the build
before (9d872203, SDL 2.32.10, in a bin dir of its own, run through a
wrapper `exec <its bin dir>/openmohaa "$@"`; `cmplists.sh`, 640x480,
`r_vkValidation 1`): the 26 views of `menus`, `hud`, `mp_mohdm1`,
`mohdm1_dlight` and `dm/mohdm1` give the same frames to the bit with SDL 3
as a native Wayland client and with `SDL_VIDEODRIVER=x11` (Xwayland). No
validation message at `r_vkValidation 2` in `menus`, `hud` and
`dm/mohdm1` under both, nor through `r_fullscreen` switched on and off
with `vid_restart` in between and `r_mode` changed (both drivers, and the
build before for comparison: the same swapchains). With the defaults the
renderer reports `GAMMA: software` (SDL 2: `hardware`, a ramp that
Xwayland ignored); at `r_gamma 1` the frames are the same either way.
`glcheck.sh` clean on the source (SDL 3's vendored headers are its
exception now) and the build; `renderer_vulkan.so` imports SDL 3, libm,
libstdc++, libgcc_s and libc only, and an installed build finds the
`libSDL3.so.0` beside it (`RUNPATH $ORIGIN`). The mouse and the latency
checks are in Implementation notes, SDL 3, and Performance, Present
timing.

SDL 3's event pump (2026-10-24, Plan 36; engine only) against the build
before (7c82547a, in the same bin dir through a wrapper): the same 26 views
give the same frames to the bit (`cmplists.sh`, 640x480, `r_vkValidation
1`, and the build before run twice for the noise: none). No validation
message at `r_vkValidation 2` in `menus` and `hud`, nor at 1 and 2 on
`dm/mohdm1` at `com_maxfps 250` through `r_fullscreen` switched on and off
with `vid_restart`, as a native Wayland client and under Xwayland.
`in_joystick 0` initializes the joystick subsystem for
`in_availableJoysticks` and shuts it down again, `in_restart` with 1 keeps
it up, back to 0 shuts it down (`developer 1`). `glcheck.sh` clean. The
mouse and the latency checks are in Performance, Present timing.

The server browser (2026-10-24, Plan 37) with engine builds that log the
browser's callbacks (a temporary patch) and `renderer_timed.so`'s frames,
at `com_maxfps 250`: `pushmenu joinnetgame` (the first refresh), 45 s,
`widgetcommand netgamelist refreshserverlist`, 45 s; 7c82547a, the merge of
the pull request and the final code interleaved, three runs each against
the five public masters. Fill and end per refresh, the frames meanwhile:

| build | answers: half, all | list done | the slowest frame of a refresh: first of a session, later |
| --- | --- | --- | --- |
| 7c82547a | 1.4 to 1.9 s, 10.7 to 14.4 s | 31.6 to 35.7 s | 54 to 137 ms, 5 to 36 ms |
| pull request #3 | 0.5 to 0.8 s, 1.1 to 3.1 s | 10.9 to 11.4 s | 84 to 96 ms, 5 to 171 ms |
| Plan 37 | 0.5 to 0.8 s, 1.3 to 2.3 s | 4.9 to 5.3 s (once 10.0) | 21 to 23 ms, under 5 ms |

The first frame of a session holds the menu's load (11.8 ms in every
build); the UI's own time in it was 0.9 ms before, 71 to 83 ms with the
pull request (five lookups) and 9 to 10 ms with Plan 37 (the process
passes 64 open files: the kernel grows its file table, 4 ms of one
`socket` call under `strace`; five threads start). The same servers answer
in every build (130 to 133 of 215 to 228 listed). A master name that
doesn't resolve (`com_master0_host master.invalid`) is skipped and one
given as an address is used (the same 132 answers, done after 5 s); a
cancel right after the start goes idle once the lookups end (71 ms) and
the next refresh is normal; the LAN search finds a local dedicated server
and ends (7c82547a stayed in "Searching LAN."). `test_lz77` and
`test_serverlist_scheduler` pass with GCC and with clang (Debug, the
Unit Tests job's setup).

Shots and jumps on the sub-grid and the crosshair (2026-10-24, Plans 38
and 39; engine, cgame and renderer) against the build before (4524cda1
from a `git archive` export, in a bin dir of its own through a wrapper;
`cmplists.sh`, 640x480, `r_vkValidation 1`): of the 53 views of `menus`,
`hud`, `mp_mohdm1`, `mp_obj_team1`, `mohdm1`, `mohdm1_dlight`,
`team_obj_team2`, `obj_team2`, `mohdm2_fog`, `m2l3` and `skin_passes`, 50
give the same frames to the bit, and the three of `hud` differ in the
stock crosshair image's half-dark pixel at each arm's inner end alone (4
pixels; 3 in `hud_scores`, whose fourth lies on black). The final build
(Plan 38's jump rule came after that pass) over `menus`, `hud`,
`mp_mohdm1`, `mohdm1` and `mohdm1_dlight` again: the same. No validation
message, and none at `r_vkValidation 2` in `menus`, `hud` and `dm/mohdm1`
nor through a `vid_restart` with new crosshair values. The crosshair in
magenta in a team match on `dm/mohdm1` (a temporary cfg, the player with a
rifle): at 640x480, 1920x1080, 1280x720 with length 5, gap 3 and
thickness 2, 1600x900 with 6, 4 and 3, 1600x900 with a gap and a length
past their maximums and thickness 0, 2560x1440 (scale 1.333) and
3840x2160 (scale 2), with `#00ff00`, with an invalid color (white) and
before and after a `vid_restart` that changed the values, every capture
has exactly the expected pixels, symmetric about the center square;
`ui_crosshair 0` draws none. `glcheck.sh` clean; `renderer_vulkan.so`
imports no GL or Vulkan symbol. Frame times (`perf.py`, `o2match` and
`o4match` at 1920x1080 fullscreen with `libinlat.so`, 5 runs each,
interleaved with the build before; the demos draw the HUD, the crosshair
where the recorder had it): at 250 frames a second the same, frame time
p99 4.043 and 4.038 ms against 4.043 and 4.039, p99.9 4.191 and 4.162
against 4.199 and 4.154, input to present p99 0.984 and 1.383 against
0.987 and 1.388 and p99.9 within the runs' spread (1.23 to 2.02 against
1.14 to 1.98 ms, 1.69 to 2.40 against 1.75 to 2.70); uncapped 1642 and
1008 frames a second against 1607 and 992 (p99 1.274 and 1.546 ms
against 1.306 and 1.592, p99.9 within the spread). cgame's own time a
frame (`renderer_timed.so`) is 0.2 to 0.3 µs lower in all four (4.4
against 4.6 µs in `o2match` at 250, 5.7 against 6.0 in `o4match`): the
stock path's trace and shader lookups of every frame.

`m3l1a` is not in the table: the beach artillery shakes and rolls the
camera at random, so the two renderers see slightly different views of the
open sea after the two minutes in the landing craft (GL1's horizon was
tilted in the run made, 97 % of the pixels differ); `obj_team3_deepbluesea`
covers its sea shader. No difference needed a change to the renderer:
besides the causes above, the two views that looked like bugs were taken
from outside the world, and GL1 sometimes saved a black or stale frame
(see Deterministic 3D views).

Other things learned:

- `com_maxfps` caps the menus too (this note used to say the opposite;
  measured on 2026-10-17 with `libinlat.so`: the main menu presented 167
  frames a second at `com_maxfps 144` with the old frame limiter, 144 with
  the new one).
- Commands appended while a script waits run after the whole script. The
  `vid_restart` that a window resize schedules is one of them, so a script
  with a long `wait` never sees it. The client also reads console commands
  from stdin (a FIFO works), which is a way to drive resize tests with
  `xdotool windowsize`; a `devmap` sent that way stops at the loading screen.
- Depending on timing, a map can stop at its loading screen with a CONTINUE
  button (with both renderers); `finishloadingscreen` dismisses it.
- The menu fan and 2D shader animations use the wall clock
  (`backEnd.refdef.time = ri.Milliseconds()` in `Set2DWindow`), so they
  differ between any two runs.
- The game archives cvars and autosaves into `fs_homepath`; delete
  `main/configs` and `main/save` between test runs to start from the same
  state.
- `r_anaglyphMode 1` is a fatal error in OpenMoHAA with either renderer
  (`RE_BeginFrame: Stereo is enabled, but stereoFrame was 0`).
- `PRINT_WARNING` and `PRINT_DEVELOPER` messages only show with
  `developer 1` in OpenMoHAA (`CL_RefPrintf`), so the backend prints
  validation messages and its own warnings with `PRINT_ALL`. `+set logfile 2`
  writes the console to `<fs_homepath>/main/qconsole.log`.
- AVI capture is compiled out of OpenMoHAA (`CL_OpenAVIForWriting` returns
  `qfalse`), and the `video` command only runs during demo playback, so
  `RB_TakeVideoFrameCmd` never runs.
- No `.gst` file (ghost texture) exists in the game data, so
  `R_UpdateGhostTextures` never has a texture to update.
- `r_clear 1` clears only the scissor rectangle the last 2D draw left (in
  both renderers: `glClear` honours the scissor), a strip of the screen, so
  it doesn't show which pixels a frame leaves undrawn. `r_fastsky 1` does:
  it clears every view to (0.5, 0.5, 1) and skips the sky.
  `bspinfo.py <map> --view x y z` tells whether a view is inside the world
  before running it.
- `AMD_DEBUG=vs,ps,nir` (with `MESA_SHADER_CACHE_DISABLE=true`) dumps what
  radeonsi compiles for GL1's fixed-function pipeline and `RADV_DEBUG=shaders`
  what RADV compiles for ours, which is how the arithmetic of the vertex
  transform and of fog were matched. With Mesa 26 radeonsi prints the ISA
  only with `asm` added (`AMD_DEBUG=vs,ps,asm`). `RADV_DEBUG=img` and
  `AMD_DEBUG=tex` print the layout of every image (swizzle mode, DCC,
  HTILE, TC-compatible HTILE), which is how the depth buffer's compression
  mode was found (see Performance).

### Performance

`tools/perf.py` measures views with several builds of the renderer (see its
docstring; until 2026-10-23 its default was GL1 against Vulkan, as in most
tables below):

```sh
code/renderervk/tools/rtimer/build.sh      # renderer_timed.so, libstacks.so and libinlat.so into build-vk
code/renderervk/tools/perf.py --list code/renderervk/tools/views/perf.txt \
    --sizes 640x480,1920x1080 --renderers base,vulkan --runs 3 --json out.json
code/renderervk/tools/perf.py m6l1a 6152 -6528 5500 0 135 0 --runs 1 --stacks out
code/renderervk/tools/stacks.py out_m6l1a_640x480_vulkan_1.txt --tree RB_RenderDrawSurfList --callers CM_BoxTrace
```

Each measurement starts a game, loads the map with `fixedtime 16` at 250
frames a second (the loading's waits count frames, see Plan 21), puts the
camera at the view, waits 2 s of game time and times 1000 frames with the
frame rate uncapped (`com_maxfps 0`, `r_swapInterval 0`, no validation).
A list line `views <view list> [shot ...]` takes shots of a view list,
with the commands that come before them in it (`views/perf_mp.txt`: the
soldiers of the multiplayer views, see Comparing).
The runs interleave the renderers (run i starts with renderer i), and the
summary gives medians with the lowest and highest. `--renderers` takes any
`renderer_<name>.so` of the binary directory: a copy of an older
`renderer_vulkan.so` under another name compares two builds in the same
batch, which is how every change below was measured. What it reports:

- frame time: wall clock time of the 1000 frames;
- renderer time, with `renderer_timed.so` (`tools/rtimer/rtimer.c`, built
  by `tools/rtimer/build.sh`; perf.py runs `cl_renderer timed` with
  `MOH_TIMED_RENDERER=<renderer>`): the main thread's time in the
  renderer's exports per frame, with a microsecond clock, for GL1 and
  Vulkan alike: `scene` (`RenderScene`, the front end), `2d` (`Set2DWindow`,
  `DrawStretchPic`, `DrawString`, ...: in OpenMoHAA they run the queued
  render commands, so this is where the 3D view's back end runs), `end`
  (`EndFrame`: the rest of the back end, swap or present), and `load`
  (time in the registration exports since startup);
- GPU busy (amdgpu's `gpu_busy_percent`) and the clock of the core running
  the main thread, sampled every 5 ms;
- for Vulkan, `vkinfo` after loading and around the measured frames: GPU
  time per frame from timestamps (and the copy to the swapchain image,
  when there is one), draws, render passes, vertex bytes, waits; pipelines,
  textures and allocations created while loading;
- how many CPUs other processes kept busy (from `/proc/stat` minus the
  game's CPU time); about one is the compositor showing a thousand frames
  a second, more is flagged. `--pause PID,...` stops other processes
  (SIGSTOP/SIGCONT) during each measurement, only for processes whose
  owner agreed.

`--stacks` preloads `libstacks.so` (`tools/rtimer/stacks.c`), which
interrupts the main thread every millisecond of wall clock time on average
and records its backtrace (the intervals are random between 0.5 and 1.5 ms:
at a fixed 1 ms the samples locked to the phase of frames capped at 1000
per second and always landed in the same part of the frame, which made
640x480 profiles show only the world traversal); `stacks.py` symbolizes
them (addr2line, with the library paths recorded in the file: keep a copy
of a `.so` that is rebuilt to read its old profiles) and prints self and
inclusive time per function, the call paths below one (`--focus`), its
callers (`--callers`) and the tree of what it calls (`--tree`). It needs
no privileges (perf is refused with the default
`kernel.perf_event_paranoid` of Ubuntu) and shows time spent waiting
(fences, the swap) where perf's CPU samples show nothing.

What makes simpler measurements misleading:

- `com_maxfps 0` meant at most 1000 frames per second until 2026-10-19:
  `Com_Frame` waited until a whole millisecond had passed since the last
  frame. On an idle machine most views reached that at 640x480 with both
  renderers, so the frame time showed no differences; the renderer time
  did. The tables below with 0.999 or 1.000 ms are from then. Since the
  command rate change (see Command rate) uncapped means uncapped, and
  `perf.py`'s uncapped frame times are the views' own.
- `com_speeds`' `rf` and `bk` are whole milliseconds, read right after the
  frame limiter's millisecond tick, and `bk` only counts the last batch of
  render commands of a frame; most of the back end runs before it, inside
  the UI's `Set2DWindow`. Both renderers were affected alike.
- The first numbers of this section (2026-10-14 morning) were taken while
  15 unrelated CPU-bound processes ran on the 16-core machine; they are
  replaced by the ones below. That run read a Vulkan front end of 1.15 ms
  against GL1's 0.84 ms in the `m6l1a` forest at 1080p: it was
  preemption and the `com_speeds` artifacts above; on the idle machine the
  front ends take 0.34 and 0.32 ms.

Results (2026-10-14 evening, idle machine: load average 0.4 to 1.8 with
the game itself, other processes 0.4 to 1.8 CPUs; AMD Radeon 780M, radeonsi
for GL1 and RADV, Mesa 26.0.8, windowed under Xwayland; medians of 3
interleaved runs, ms per frame). "before" is the renderer at the start of
the performance work (commit 41274ef3), "after" the current one; the GPU
columns are the main pass before and after the last change (the depth
image only readable while readbacks are wanted, see below):

| view | size | frame GL1 | frame before | frame after | renderer GL1 | renderer before | renderer after | GPU before last change | GPU after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest `6152 -6528 5500 0 135 0` | 640x480 | 1.043 | 1.048 | 1.024 | 0.874 | 0.881 | 0.865 | 0.45 | 0.44 |
| | 1920x1080 | 1.333 | 1.578 | 1.298 | 1.139 | 1.359 | 1.097 | 1.41 | 1.23 |
| | 3840x2160 | 4.398 | | 4.084 | 3.969 | | 3.760 | 4.64 | 4.05 |
| `obj/obj_team2` yard `1744 2832 68 0 315 0` | 640x480 | 0.999 | 0.999 | 0.999 | 0.543 | 0.549 | 0.533 | 0.22 | 0.22 |
| | 1920x1080 | 0.999 | 1.000 | 0.999 | 0.638 | 0.623 | 0.556 | 0.62 | 0.62 |
| `dm/mohdm3` overview `3000 -2400 1500 50 0 0` | 640x480 | 0.999 | 0.999 | 0.999 | 0.635 | 0.610 | 0.566 | 0.29 | 0.27 |
| | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.748 | 0.729 | 0.699 | 0.78 | 0.73 |
| `m3l3` forest path `-6800 3248 76 0 270 0` | 640x480 | 1.119 | 1.080 | 1.077 | 0.901 | 0.858 | 0.860 | 0.36 | 0.35 |
| | 1920x1080 | 1.306 | 1.273 | 1.213 | 1.006 | 0.987 | 0.936 | 1.02 | 0.93 |

(0.999 is the 1000 frames per second limit; GPU: from the start of the
frame's main command buffer to its end, timestamps; the 3840x2160 row is a
separate batch of 3 runs with GL1, the build before the last change and
the current one.)

The renderer time splits, at 1920x1080 after: `m6l1a` 0.343 front end,
0.674 2D calls with the 3D back end, 0.081 end of frame (GL1 0.319, 0.653,
0.162); `m3l3` 0.126, 0.737, 0.075 (GL1 0.129, 0.710, 0.165). The end of
frame is cheaper with Vulkan everywhere (no driver flush and swap), the
front end is the same code (equal within 3 %, except 7 to 8 % more in the
`m6l1a` forest and 7 % less in the `obj_team2` yard at 1080p, with the
same CPU clock: not explained; in the afternoon runs it was 8 % less in
the `dm/mohdm3` overview instead) and the back end is mostly the
renderer's own tessellation shared with GL1.

Profile of the `m6l1a` forest at 1920x1080, where Vulkan tied GL1 in the
first numbers and was 18 % slower on the idle machine (1.575 against 1.335
ms): Vulkan was GPU-bound. The GPU timestamps gave 1.545 ms of GPU work
per frame (1.41 main pass, 0.13 copy of the offscreen image to the
swapchain image), more than the frame's CPU work, and the CPU waited 0.22
ms per frame for the frame fence (inside `Set2DWindow`, where the back end
starts the frame). GL1 was CPU-bound. The stack samples (1 kHz, main
thread) put 0.33 to 0.35 ms per frame in the front end with both, 0.65 to
0.66 ms in the 3D back end (`RB_RenderDrawSurfList`) with both, of which
GL1's driver takes 0.27 ms on the main thread and Vulkan's `vk_draw` with
RADV 0.07 ms for 430 draws; the rest is the renderer's tessellation
(skeletal and static models, `ComputeColors`, terrain). The CPU clock was
the same (3.6 GHz). So the backend's own CPU cost was small and the plan
items that reduce it (vertex copies, render pass breaks) could not pay off
much; the GPU time could:

- the copy at present: `vkCmdCopyImage` costs the same as the blit; drawing
  into the swapchain image removed it (0.13 ms of GPU at 1080p, 0.47 ms at
  3840x2160): 1.575 to 1.451 ms per frame (635 to 689 fps), 5.15 to 4.69 ms
  at 3840x2160;
- the main pass: it took 6 to 7 % more GPU time than GL1's whole frame
  where both were GPU-bound (3840x2160: 4.63 ms against GL1's 4.39 ms
  frame; at 1080p with HiZ off, `RADV_DEBUG=nohiz`/`AMD_DEBUG=nohyperz`:
  2.08 against 1.94 ms), growing with the pixel count (0.23 ms at 320x240,
  0.41 at 640x480, 0.74 at 1280x720, 1.41 at 1080p, 4.63 at 3840x2160:
  about 0.25 ms plus 0.52 ms per megapixel). The cause was the depth
  buffer's compression mode; see below.

How the main pass was explained (2026-10-14 evening, same machine, idle;
`m6l1a` forest, medians of 2 or 3 runs):

- The shaders are not it. RADV's fragment shaders of the dominant
  pipelines (`MESA_SHADER_CACHE_DISABLE=true RADV_DEBUG=shaders`) are as
  short as radeonsi's fixed-function ones (`AMD_DEBUG=ps,vs,asm`: with Mesa
  26 nothing is printed without `asm`) or shorter. Both are ACO, wave64, in
  whole-quad mode for the derivatives and export FP16 color. The
  lightmapped fogged two-texture shader is 69 instructions; radeonsi's
  is a 19-instruction prolog (color interpolation), 80 in the main part
  (it divides the texture coordinates by q) and an 8-instruction epilog
  (its alpha test). The alpha-tested foliage shader demotes and leaves
  early like radeonsi's kill.
- Turning hardware features off in both drivers shows where the difference
  is (3840x2160, ms per frame, GL1 / Vulkan):

  | | GL1 | Vulkan |
  | --- | --- | --- |
  | as is | 4.40 | 4.69 |
  | primitive binning off (`AMD_DEBUG=nodpbb`, `RADV_DEBUG=nobinning`) | 4.86 | 5.85 |
  | DCC off (`nodcc`) | 4.80 | 5.85 |
  | binning and DCC off | 5.13 | 6.82 |
  | binning and HiZ off (`nohyperz`, `nohiz`) | 9.57 | 10.94 |

  Every feature that saves memory traffic hides part of the gap: Vulkan
  moved more data per pixel. (RADV's binning differs from radeonsi's too,
  128x128 bins where radeonsi uses 128x64 for depth-tested draws, see
  Plan, Remaining; with binning off in both the gap is larger, not
  smaller.)
- Not the color writes: writing alpha too (alpha 1, so the RGB write mask
  no longer makes partial writes) changed nothing, with binning off too
  (5.84 against 5.86 ms). Not the textures: RADV gives DCC only to color
  attachments larger than 512x512, radeonsi to textures too, but forcing
  it (`RADV_DEBUG=forcecompress`) saved only 0.02 to 0.03 ms. Not the
  memory: everything is in the 2 GB carve-out (`DEVICE_LOCAL`).
- By content (`r_drawstaticmodels 0`, `r_drawterrain 0`,
  `r_farplane_nofog 1`): the 2763 alpha-tested static models are about 80 %
  of the GPU time (0.81 ms left without them), the terrain 0.32 (GL1) and
  0.39 ms (Vulkan); without the farplane fog GL1 saves 0.44 ms and Vulkan
  0.09 ms (that includes the clear of the color buffer to the fog color,
  which RADV does as a comp-to-single DCC fast clear and which seems to
  cost GL1 more), so without it the gap was 15 %.
- The surfaces (`RADV_DEBUG=img`, `AMD_DEBUG=tex`): the color buffers are
  alike (64KB_R_X with DCC, BGRA8 against BGRX8), but GL1's depth buffer is
  Z24X8 with HTILE that isn't TC-compatible and ours was D32S8 with
  TC-compatible HTILE. RADV makes HTILE TC-compatible (the texture unit can
  read it, so copies and sampling need no decompression) for depth images
  with `SAMPLED`, `INPUT_ATTACHMENT` or `TRANSFER_SRC` usage, and ours had
  `TRANSFER_SRC` for depth and stencil readbacks. With TC-compatible HTILE,
  GFX9 and later keep an 8x8 depth tile compressed only while it holds at
  most 4 planes (`DECOMPRESS_ON_N_ZPLANES` 5 instead of 15,
  `ac_get_decompress_on_z_planes` in Mesa); the forest's small alpha-tested
  triangles put more planes than that in most tiles, which are then
  written and read uncompressed.

Without `TRANSFER_SRC` (the depth image is now only readable while
readbacks are wanted, see Implementation notes, Render target) the main
pass takes 12 to 13 % less GPU time and Vulkan is faster than GL1 where the
GPU limits:

Medians of 3 interleaved runs (`perf.py`, idle machine: load average 1.1
to 1.6 with the game, other processes 1.0 to 1.4 CPUs), ms per frame:

| `m6l1a` forest | GL1 | Vulkan before this change | after | GPU main pass before, after |
| --- | --- | --- | --- | --- |
| 1920x1080 | 1.370 (730 fps) | 1.455 (687 fps) | 1.328 (753 fps) | 1.412, 1.239 (-12 %) |
| 3840x2160 | 4.398 (227 fps) | 4.686 (213 fps) | 4.084 (245 fps) | 4.643, 4.046 (-13 %) |

At 1080p Vulkan is CPU-bound again (renderer time 1.115 ms against GL1's
1.152 and 1.236 before). In the other views of the results table the main
pass at 1080p went from 1.02 to 0.93 ms (`m3l3` path, -9 %), 0.78 to 0.73
(`dm/mohdm3` overview, -7 %) and stayed at 0.62 (`obj_team2` yard); their
frames are CPU-limited.

A depth format without the unused stencil plane (D32 instead of D32S8,
GL1 has no stencil either) saves 0.5 % more at 3840x2160 and nothing at
1080p after that; not done then, done since (see GPU time at 3840x2160).

Second round (2026-10-14 night): the renderer's CPU time. With the
depth image change the `m6l1a` forest is GPU-bound at 1080p, so CPU gains
there only turn into fence waits; the numbers below are the renderer time
minus `vkinfo`'s fence waits (`waitUs` per frame), at 1080p for the first
four changes and at 640x480 (no waits) for the last two, medians of 3
interleaved runs on the idle machine, each against the build before it, ms
of main-thread CPU time per frame in the `m6l1a` forest / `obj_team2` yard
/ `dm/mohdm3` overview / `m3l3` path. The stack samples put most of the
CPU time in code shared with GL1; every change keeps the frames
bit-identical (see Testing):

| change | why (stack samples) | ms per frame |
| --- | --- | --- |
| hidden symbol visibility: the library exports only `GetRefAPI` (`cmake/renderer_vulkan.cmake`) | with default visibility, calls between the renderer's own functions (1621 exported symbols) went through the PLT and could not be inlined, LTO or not (4034 PLT call sites in the library, 868 after; code 960 KB, 715 KB after, `size`) | -0.092 / -0.024 / -0.039 / -0.037 |
| static model culling without the model matrix: `R_AddStaticModelSurfaces` computed `R_RotateForStaticModel` (a 4x4 matrix product) for each of the 2763 static models of `m6l1a` every frame, though culling only needs the origin and axes; the matrix of the last one is still computed so `tr.ori` ends up the same | 9 % of the `m6l1a` frame | -0.060 / -0.016 / +0.006 / -0.004 |
| sun visibility traces kept by origin (`RB_Sphere_TraceSun`): the trace from an entity's lighting origin towards the sun only depends on the world, so its result is kept (1024 entries, keyed on the exact origin and sun direction, cleared at map load) for the entities that don't move | `CM_BoxTrace` for every lit entity and frame: 20 % of the `m3l3` frame, 6.5 % in `m6l1a`; most entities there move, the attached weapons share their owner's origin | -0.028 / -0.012 / -0.018 / -0.025 |
| radix sort of the draw surfaces: the four byte histograms counted in one pass, bytes that are the same in every key skipped (the same stable order) | `R_Radix` 9.7 % of the `obj_team2` frame | -0.030 / -0.012 / -0.007 / -0.014 |
| static model distance fade (`AGEN_DIST_FADE` in `ComputeColors`): the eye position in the model's axes computed once per batch instead of per vertex (the same operations) | 4.5 % of the `m6l1a` frame | -0.030 / -0.005 / -0.013 / -0.006 |
| static models culled from a compact array: their world-space sphere is computed at load with the same arithmetic, and the per-frame loop reads 24 bytes per model instead of the 1.3 KB `cStaticModelUnpacked_t` entries (one cache miss each) | `R_AddStaticModelSurfaces` 5.5 % of the `m6l1a` frame after the second change | -0.024 / -0.025 / -0.005 / -0.012 |

The result, against GL1 and the build before the second round (medians
of 3 interleaved runs, idle machine: load 1.0 to 1.4 with the game, other
processes 1.6 CPUs at most; frame time with the fps of the median, the
renderer's main-thread time without the fence waits, its front end, and
the main pass's GPU time; ms per frame):

| view | size | frame GL1 | frame before | frame after | renderer GL1 | before | after | front end GL1 / before / after | GPU before / after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest | 1920x1080 | 1.323 (756 fps) | 1.301 (769) | 1.259 (794) | 1.129 | 1.071 | 0.811 | 0.320 / 0.340 / 0.189 | 1.235 / 1.235 |
| | 3840x2160 | 4.386 (228) | 4.081 (245) | 4.089 (245) | 3.990 | 1.374 | 1.079 | 0.429 / 0.466 / 0.283 | 4.037 / 4.045 |
| `obj_team2` yard | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.618 | 0.546 | 0.482 | 0.143 / 0.137 / 0.109 | 0.622 / 0.622 |
| | 3840x2160 | 1.765 (567) | 1.763 (567) | 1.762 (568) | 1.608 | 0.704 | 0.652 | 0.172 / 0.163 / 0.137 | 1.742 / 1.740 |
| `dm/mohdm3` overview | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.744 | 0.682 | 0.644 | 0.162 / 0.151 / 0.142 | 0.728 / 0.726 |
| | 3840x2160 | 2.460 (407) | 2.558 (391) | 2.563 (390) | 2.324 | 0.888 | 0.876 | 0.222 / 0.201 / 0.196 | 2.527 / 2.532 |
| `m3l3` path | 1920x1080 | 1.294 (773) | 1.246 (803) | 1.122 (891) | 0.995 | 0.935 | 0.849 | 0.128 / 0.129 / 0.105 | 0.933 / 0.936 |
| | 3840x2160 | 2.992 (334) | 3.099 (323) | 3.090 (324) | 2.495 | 1.183 | 1.068 | 0.166 / 0.167 / 0.143 | 3.060 / 3.051 |

(GL1's renderer time includes its waits for the GPU, in its swap; 0.999
is the 1000 frames per second limit. Where the GPU limits, at 3840x2160
and in the `m6l1a` forest at 1080p, the CPU savings become fence waits.)

Tried and not kept (no gain beyond the ±0.01 ms noise, or a cost): a
prefetch of the next draw surfaces in `RB_RenderDrawSurfList` (the world
surface data is mostly cache misses in `obj_team2`), the per-frame vertex
buffer in host-cached memory instead of write-combined device-local memory,
the depth format without the stencil plane (0.5 % less GPU time at
3840x2160, nothing at 1080p or with multisampling) and writing alpha
without multisampling (1.2 % less GPU time at 3840x2160 in `dm/mohdm3` and
`m3l3`, but 1 pixel in about 10^5 rounds one step lower, see State). Both
were taken up again with the GPU time at 3840x2160 (below): alpha is now
written by the pipelines without blending only, which keeps the frames.

What the profile shows after it (640x480, `m6l1a`, % of the 1 ms frame):
`vk_draw` 10 % for 340 draws (vertex and index copies about 4, RADV's
command recording 2, the backend's own bookkeeping 3), the renderer's
tessellation (static and skeletal models, terrain, `ComputeColors`) about
20, the sun traces of moving entities 3, `vk_end_frame` 7.5 (RADV's
submission ioctl and the sync objects of the X11 present and acquire), the
front end 14. Keeping static geometry in GPU buffers would save part of
the copies (about 0.03 ms), not worth its complexity on its own (it was
done since as the first step of a redesign, see Static geometry below:
what it saves is mostly the tessellation, not the copies).

Multisampling: the gap to GL1 was the color write mask. The pipelines
wrote RGB only (no destination alpha), and on the multisampled
DCC-compressed color image such a partial write costs: with 4 samples the
`m6l1a` forest took 2.887 ms of GPU time per frame, 2.729 ms when the
pipelines that write all of RGB write alpha too (1 without blending,
unchanged with it, see State), and 2.651 ms without the resolve at all
(the resolve into the swapchain image costs 0.26 ms; `vkCmdResolveImage`
or resolving into the offscreen image cost the same). Frame times, medians
of 3: GL1 2.699 ms, before 2.934, after 2.774 ms (3 % slower than GL1
instead of 9 %), the same frames as before with multisampling (the
`mohdm1` views). The depth
format without stencil changed nothing there (2.889 against 2.884 ms).

Static geometry (2026-10-15, step 1 of the redesign, see Implementation
notes): the world's faces, triangle surfaces and grids and the static
models are drawn from device memory instead of being tessellated into
`tess` and copied every frame. What it covers (`vkinfo` per frame at
640x480, `m6l1a` / `obj_team2` / `dm/mohdm3` / `m3l3`): 107 / 93 / 221 / 25
of 322 / 201 / 408 / 262 draws come from the static geometry, and the
vertex bytes copied go from 584 / 819 / 690 / 503 KB to 529 / 256 / 252 /
436 KB. There are as many draws as before (a merged batch still draws each
of its static models); there are fewer batches, the stage iterator runs:
88 static models in 3 batches in the `m6l1a` forest, 118 in 35 in the
`dm/mohdm3` overview. In the forests most static models still take the CPU
path (deforms, `alphaGen distFade`, see Plan, Remaining), and the terrain
and skeletal models are not part of this step, so `m6l1a` and `m3l3`
change little. Stack samples (640x480): `RB_SurfaceFace` went from 8.7 % to
1.0 % of the `obj_team2` frame (4.1 to 1.0 % in `dm/mohdm3`), `vk_draw`
below the lightmapped iterator from 6.8 to 4.0 %.

Against GL1 and the build before (2026-10-15 evening, idle machine: load
1.0 to 1.8 with the game, other processes 1.6 CPUs at most, 2.2 in one GL1
run at 3840x2160; medians of 3 interleaved runs; frame time with the fps
of the median, the renderer's main-thread time without the fence waits
(GL1's includes its waits in the swap), the main pass's GPU time; ms per
frame):

| view | size | frame GL1 | frame before | frame after | renderer GL1 | before | after | GPU before / after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest | 640x480 | 1.049 (953 fps) | 1.000 | 1.000 | 0.881 | 0.648 | 0.640 | 0.447 / 0.440 |
| | 1920x1080 | 1.374 (728) | 1.255 (797) | 1.256 (796) | 1.153 | 0.827 | 0.824 | 1.229 / 1.228 |
| | 3840x2160 | 4.403 (227) | 4.091 (244) | 4.093 (244) | 3.994 | 1.139 | 1.118 | 4.043 / 4.044 |
| `obj_team2` yard | 640x480 | 1.000 | 1.000 | 1.000 | 0.562 | 0.487 | 0.405 | 0.216 / 0.207 |
| | 1920x1080 | 1.000 | 1.000 | 1.000 | 0.638 | 0.499 | 0.402 | 0.620 / 0.621 |
| | 3840x2160 | 1.766 (566) | 1.765 (567) | 1.755 (570) | 1.605 | 0.664 | 0.535 | 1.741 / 1.732 |
| `dm/mohdm3` overview | 640x480 | 1.000 | 1.000 | 1.000 | 0.627 | 0.546 | 0.443 | 0.270 / 0.283 |
| | 1920x1080 | 1.000 | 1.000 | 1.000 | 0.748 | 0.652 | 0.539 | 0.726 / 0.718 |
| | 3840x2160 | 2.469 (405) | 2.566 (390) | 2.556 (391) | 2.322 | 0.886 | 0.750 | 2.533 / 2.523 |
| `m3l3` path | 640x480 | 1.168 (856) | 1.007 (993) | 1.012 (988) | 0.929 | 0.763 | 0.769 | 0.339 / 0.361 |
| | 1920x1080 | 1.347 (742) | 1.132 (884) | 1.128 (887) | 1.018 | 0.863 | 0.847 | 0.930 / 0.929 |
| | 3840x2160 | 3.020 (331) | 3.094 (323) | 3.091 (323) | 2.472 | 1.092 | 1.119 | 3.053 / 3.050 |

(1.000 is the 1000 frames per second limit.) The renderer's CPU time goes
down 17 to 20 % in the `obj_team2` yard and 15 to 19 % in the `dm/mohdm3`
overview; in the forests the changes are within the noise (±0.03 ms). The
GPU time doesn't change: equal at 1920x1080 and 3840x2160, where it is
stable; at 640x480 the GPU is a fifth busy and its time varies by ±10 %
from run to run with either build (`m3l3`: 0.315 to 0.367 ms before, 0.361
to 0.366 after). Where the GPU limits (3840x2160, the forest at 1080p) the
frames take the same time as before.

Merged static model batches and grids, each measured against the same
build without it (640x480, 5 interleaved runs of 5000 frames, medians):
merging 0.643 against 0.657 ms in the `m6l1a` forest and 0.453 against
0.463 in the `dm/mohdm3` overview; grids 0.767 against 0.782 in the `m3l3`
path (2 % each; 3 runs of 1000 frames could not tell them from the noise).

Dynamic lights (2026-10-16, see Implementation notes, Dynamic lights):
`perf.py --exec 'cg_acidtrip N'` adds N random lights of radius 128 to
639 every frame (for timing only: they differ from run to run), on the
idle machine (load 0.5 to 2 with the game, other processes 1.7 CPUs at
most, but for bursts of up to 9.5 CPUs in a few 3840x2160 runs: the
medians below come from clean runs, and GL1's yard with 16 lights there
was measured again); medians of 3 interleaved runs, the renderer's
main-thread time without the fence waits, ms per frame. Each step against
the one before it, at 640x480 (no waits there):

| view | lights | GL1 | before | part 1: texel culling | part 2: per pixel | + box test of the texels | + lights culled per node (final) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 0 | 0.561 | 0.392 | | 0.398 | | |
| | 4 | 1.387 | 1.166 | 0.875 | 0.659 | 0.551 | 0.499 |
| | 16 | 2.209 | 2.008 | 1.952 | 1.234 | 0.833 | 0.684 |
| `dm/mohdm3` overview | 0 | 0.626 | 0.459 | | 0.441 | | |
| | 4 | 1.300 | 1.128 | 0.826 | 0.662 | 0.613 | 0.581 |
| | 16 | 2.072 | 1.927 | 1.559 | 1.031 | 0.836 | 0.796 |
| `m3l3` path | 0 | 0.912 | 0.763 | | 0.754 | | |
| | 4 | 1.046 | 0.887 | 0.867 | 0.787 | 0.788 | 0.764 |
| | 16 | 1.222 | 1.093 | 1.039 | 0.837 | 0.817 | 0.814 |

(GL1, before, part 1 and part 2 in one batch per light count; the box
test against part 2, the node culling against the box test, each in a
batch of its own. Without lights the last two do nothing.)

- Part 1 (exact: texels outside every light's reach skipped, faces whose
  texels no light reaches not allocated, see Testing) took 25 to 27 % off
  with 4 lights in the yard and the overview, 19 % in the overview with 16, but
  only 3 % in the yard with 16 lights of up to 639 units, where most
  texels are in reach; part 2 replaced it (its face test stayed).
- Part 2 removed the rest of the texel work, the uploads (94 KB per frame
  in `mohdm1_dlight`) and the lit surfaces' batches of their own: 41 to 43
  % less than before with 4 lights, 39 to 46 % with 16. The stack samples then put
  41 % of the yard's frame with 16 lights in `R_DlightReachesFace` and 11 %
  in the plane tests of `R_RealDlightFace`: every visible face tested every
  light, as GL1 did.
- The box of the texel centers, computed at load, rejects the faces far
  from a light before the exact test (the same masks): -16 % and -32 % in
  the yard with 4 and 16 lights, -9 and -19 % in the overview.
- The lights culled per BSP node (the same masks again): -9 % and -18 % in
  the yard, -5 % in the overview, -3 % in the `m3l3` path with 4.
- Dropped: letting the static models no light reaches keep their merged
  batches when lights are in view (`RB_StaticMerge`): no difference beyond
  the noise in these views (0.661 against 0.659, 0.659 against 0.662 ms...).

Against GL1 and the build before, at 1920x1080 and 3840x2160 (frame time
with the fps of the median, renderer time without fence waits, the main
pass's GPU time; ms per frame; final runs at 1080p and 4K with lights in
a batch with the build before, GL1 from the earlier batches):

| view | size | lights | frame GL1 | before | final | renderer GL1 | before | final | GPU before | final |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 1920x1080 | 0 | 0.999 | 0.999 | 0.999 | 0.625 | 0.391 | 0.401 | 0.617 | 0.620 |
| | | 4 | 1.454 (688 fps) | 1.191 (840) | 0.999 | 1.319 | 1.078 | 0.495 | 0.702 | 0.625 |
| | | 16 | 2.088 (479) | 1.788 (559) | 0.999 | 1.951 | 1.676 | 0.685 | 0.798 | 0.630 |
| | 3840x2160 | 0 | 1.843 (543) | 1.770 (565) | 1.753 (570) | 1.637 | 0.588 | 0.558 | 1.747 | 1.732 |
| | | 16 | 2.300 (435) | 1.959 (511) | 1.774 (564) | 2.128 | 1.809 | 0.771 | 1.753 | 1.751 |
| `dm/mohdm3` overview | 1920x1080 | 0 | 0.999 | 0.999 | 0.999 | 0.750 | 0.537 | 0.533 | 0.718 | 0.719 |
| | | 4 | 1.514 (661) | 1.291 (774) | 0.999 | 1.405 | 1.205 | 0.678 | 0.810 | 0.722 |
| | | 16 | 2.239 (447) | 2.054 (487) | 1.000 | 2.121 | 1.952 | 0.840 | 1.050 | 0.725 |
| | 3840x2160 | 0 | 2.469 (405) | 2.558 (391) | 2.569 (389) | 2.327 | 0.757 | 0.834 | 2.528 | 2.538 |
| | | 16 | 2.713 (369) | 2.645 (378) | 2.555 (391) | 2.552 | 2.192 | 1.028 | 2.568 | 2.524 |
| `m3l3` path | 1920x1080 | 0 | 1.367 (732) | 1.137 (879) | 1.169 (855) | 1.019 | 0.847 | 0.840 | 0.936 | 0.933 |
| | | 4 | 1.506 (664) | 1.220 (820) | 1.121 (892) | 1.162 | 0.941 | 0.859 | 0.935 | 0.931 |
| | | 16 | 1.616 (619) | 1.487 (672) | 1.198 (835) | 1.292 | 1.139 | 0.896 | 0.966 | 0.932 |

(0.999 is the 1000 frames per second limit. The CPU times of the
3840x2160 rows without lights come from the runs with bursts of load:
only their GPU times count. The no-light frame times of the `m3l3` path
include the game's own time, which varies more than the renderer's.)

The per-pixel lights cost no measurable GPU time: with 16 lights the main
pass takes 0.630 and 0.725 ms at 1080p against 0.620 and 0.719 without,
1.751 and 2.524 ms at 3840x2160 against 1.732 and 2.538. The dynamic
lightmaps cost 0.18 and 0.33 ms more at 1080p than no lights (the lit
surfaces in batches of their own, drawn from the CPU path, and the
uploads); the GPU time with lights now equals the GPU time without. Where
the CPU limited the frame, 16 lights now leave the yard and the overview at
the 1000 fps limit at 1080p (559 and 487 fps before, GL1 479 and 447), and
at 3840x2160 the yard runs at 564 fps against 511 (GL1 435).

What is left with 16 lights (stack samples, 640x480): the per-face tests
(`R_RealDlightFace`, its box and exact tests: about 19 % of the yard's
0.74 ms frame, the lights there being large and many faces in their
reach), and in the overview the per-vertex lighting of the static models
within twice a light's radius (`ComputeColors` and `RB_StaticMesh`, 13 %:
those models leave the static geometry), see Plan, Remaining. The
entities' lighting (`RB_Light_Real`) takes 0.3 %.

Skeletal models (2026-10-16, step 3, see Implementation notes, Skeletal
models). The views measured until then had few characters; in
multiplayer the players are most of the frame. On the multiplayer views
with 20 player models (`views/perf_mp.txt`: `pm1_front`, `po1_front`,
`po4_front` and `pm7_flash`, the last with three muzzle flash lights, see
Testing), the stack samples of the build before (640x480, % of the
frame; one run each) put the skinning (`RB_SkelMesh` and the functions
inlined into it) at 27 / 34 / 31 / 29 %, the per-vertex lighting
(`RB_Light_Real`) at 7 / 3 / 7 / 14 %, the copies of the skinned
vertexes into the frame's vertex buffer at 5 to 7 % (1.05 MB per frame
against 0.25 MB without the players), the sun visibility traces of the
entities (`RB_Sphere_TraceSun`, `CM_BoxTrace`) at 8 to 10 % and the
skeletons (`R_AddSkelSurfaces`, the engine's skeletor) at 6 to 9 %; the
renderer's time per frame was 1.0 to 1.4 ms there, against 0.40 to 0.47
in the `obj_team2` yard and the `dm/mohdm3` overview. Each change against
the build before it, renderer time (ms per frame, 640x480, where there
are no fence waits; medians of 3 interleaved runs, idle machine, other
processes 1.8 CPUs at most):

| change | `pm1_front` | `po1_front` | `po4_front` | `pm7_flash` | `obj_team2` yard | `dm/mohdm3` overview |
| --- | --- | --- | --- | --- | --- | --- |
| before | 1.336 | 1.042 | 1.208 | 1.421 | 0.398 | 0.474 |
| skinning and lighting in the vertex shader | 0.810 | 0.605 | 0.660 | 0.735 | 0.413 | 0.454 |
| sun traces kept for real (hash fix) | 0.642 | 0.476 | 0.548 | 0.596 | 0.401 | 0.453 |
| level of detail collapsed in the vertex shader | 0.650 → 0.571 | 0.471 → 0.397 | 0.554 → 0.467 | 0.597 → 0.511 | 0.402 → 0.385 | 0.450 → 0.448 |

(The first three rows in one batch, the last in a batch of its own with
the build of the row above.)

- The vertex shader does `RB_SkelMesh`'s and `RB_Light_Real`'s work with
  their floating point operations: 39 to 48 % less time in these views.
  The vertexes are no longer copied (497 against 1095 KB per frame in
  `pm1_front`); there are more draws (one per surface instead of one per
  batch: 556 against 497).
- The sun trace cache of the second round (`RB_Sphere_TraceSun`) hashed
  the bits of the origin with products by odd constants, which keep the
  low bits clear: an origin on whole or eighth units (map entities,
  network snapped positions) has many clear low mantissa bits, so every
  such entity went to the first of the 1024 slots and evicted the one
  before it, and all 36 traces of a `pm1_front` frame missed (counted
  with a temporary build). With the high bits mixed into the slot, the
  entities that don't move keep their trace: 17 to 21 % less. The
  soldiers of these views drift by tenths of a unit in the uncapped
  frames of `perf.py` (client interpolation), so about 15 of the 36 still
  miss there, as moving players do in a game: the trace of a moving
  entity is the engine's collision code (`CM_BoxTrace`, 5 to 8 % of these
  frames now), which only an approximation could avoid.
- The collapse of the vertexes at a lower level of detail (most of the
  surfaces of these views: `RB_SkelMesh` recomputed it every frame, 6 to 8
  % of the frame after the first change) is done by the vertex shader,
  which follows each vertex's collapse chain, and the draw takes a prefix
  of the surface's triangles found with a binary search (see
  Implementation notes, Skeletal models): 12 to 16 % less, and no indexes
  are copied any more.
- Tried and dropped: keeping the results of the visibility traces of
  `needs_trace` sphere lights (`RB_Sphere_AddLight`) by origin and light,
  like the sun's: no such light in these maps (no difference beyond the
  noise).

The result against GL1 and the build before (medians of 3 interleaved
runs, idle machine: others 2.0 CPUs at most, the one run at 2.0 being the
slowest of its median; frame time with the fps of the median, the
renderer's main-thread time without the fence waits (GL1's includes its
waits in the swap), the main pass's GPU time; ms per frame):

| view | size | frame GL1 | before | after | renderer GL1 | before | after | GPU before | after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `pm1_front` | 640x480 | | 1.480 (676 fps) | 0.999 | | 1.320 | 0.564 | 0.480 | 0.548 |
| | 1920x1080 | 2.001 (500) | 1.630 (614) | 1.295 (772) | 1.768 | 1.432 | 0.701 | 1.239 | 1.268 |
| | 3840x2160 | 4.050 (247) | 4.093 (244) | 4.096 (244) | 3.709 | 1.946 | 0.977 | 4.045 | 4.056 |
| `po1_front` | 640x480 | | 1.140 (877) | 0.999 | | 1.004 | 0.385 | 0.301 | 0.408 |
| | 1920x1080 | 1.321 (757) | 1.193 (838) | 0.999 | 1.135 | 1.035 | 0.409 | 0.809 | 0.835 |
| | 3840x2160 | 2.736 (365) | 2.771 (361) | 2.783 (359) | 2.459 | 1.254 | 0.570 | 2.743 | 2.754 |
| `po4_front` | 640x480 | | 1.364 (733) | 0.999 | | 1.188 | 0.470 | 0.272 | 0.383 |
| | 1920x1080 | 1.529 (654) | 1.426 (702) | 0.999 | 1.315 | 1.206 | 0.521 | 0.891 | 0.812 |
| | 3840x2160 | 2.526 (396) | 2.600 (385) | 2.610 (383) | 2.154 | 1.407 | 0.673 | 2.574 | 2.586 |
| `pm7_flash` | 640x480 | | 1.577 (634) | 0.999 | | 1.413 | 0.504 | 0.275 | 0.391 |
| | 1920x1080 | 1.976 (506) | 1.534 (652) | 0.999 | 1.795 | 1.372 | 0.548 | 0.929 | 0.662 |
| | 3840x2160 | 2.289 (437) | 2.004 (499) | 2.000 (500) | 2.058 | 1.547 | 0.640 | 1.967 | 1.982 |
| `obj_team2` yard | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.618 | 0.404 | 0.396 | 0.621 | 0.622 |
| | 3840x2160 | 1.765 (566) | 1.752 (571) | 1.750 (571) | 1.605 | 0.538 | 0.522 | 1.737 | 1.735 |
| `dm/mohdm3` overview | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.756 | 0.540 | 0.545 | 0.719 | 0.718 |
| | 3840x2160 | 2.468 (405) | 2.554 (392) | 2.551 (392) | 2.322 | 0.729 | 0.738 | 2.530 | 2.527 |

(0.999 is the 1000 frames per second limit.) Where the GPU limits
(3840x2160) the frames take the same time as before and the GPU time is
within 1 % (+0.3 to +0.8 % in the soldier views, the same in the
others): the skinning costs the GPU next to nothing. At 640x480 and
1920x1080 the GPU is a quarter to two thirds busy and its timestamps vary
with its clock from run to run (up and down by up to 30 % here: they
rose at 640x480 while the frames came faster, fell in `pm7_flash` at
1080p), which a fixed cost per frame wouldn't do: at 3840x2160 it would
show as much. At 1920x1080 `pm1_front` is now GPU-bound (93 % busy).

With four random dynamic lights (`cg_acidtrip 4`, 1920x1080, same
batches): `pm1_front` 2.685 ms (373 fps) with GL1, 1.731 (578) before,
1.293 (773) after, renderer 2.476 / 1.529 / 0.779; `po4_front` 1.805
(554), 1.455 (687), 0.999 (1000 fps limit), renderer 1.582 / 1.238 /
0.549. The lights now cost the entities nothing but the sphere setup:
their lighting is in the vertex shader.

What is left in these views (stack samples, 640x480): the skeletons in
the engine (`R_AddEntitySurfaces`, 7 to 8 %), the sun traces of the
entities that move (5 to 8 %), the skinned draws (`RB_SkinDraw`, 3 %:
`vk_draw` per surface), the index prefixes (`RB_SkinSurface`, 1.3 %),
the world and the static models (`RB_StaticMesh` 4 % in `pm1_front`),
cgame (about 20 % of the wall clock time, outside the renderer) and the
end of the frame.

GPU time at 3840x2160 (2026-10-16 evening). Where the GPU limits, the
Vulkan renderer's main pass took 1 to 4 % more GPU time than GL1's whole
frame (`dm/mohdm3` overview 2.53 ms against 2.47, `po4_front` 2.59
against 2.53, see the tables above), about 0.08 ms per frame, which
turning content off didn't change (see Plan, Remaining, before this
work). GL1 has no GPU timestamps, so `tools/rtimer/glts.c`
(`libglts.so`, preloaded: `LD_PRELOAD=<bindir>/libglts.so
GLTS_OUT=<file> perf.py ... --renderers opengl1,...`; removed with GL1 on
2026-10-23) wrote GL timestamp queries right after each swap, before and after the first
depth clear of a frame (the 3D view's) and before the swap, and a
temporary build of the Vulkan renderer wrote timestamps at the same
points (bottom of the pipe, like GL's). Per frame, ms (medians of 2 or 3
runs; the timestamps themselves cost about 0.02 ms):

| view | | before the clear | the clear | clear to end of frame | end to next frame (swap, present) | per frame |
| --- | --- | --- | --- | --- | --- | --- |
| `dm/mohdm3` overview | GL1 | 0.000 | 0.266 | 2.190 | 0.043 | 2.499 |
| | Vulkan before | 0.004 | 0.280 | 2.262 | 0.039 | 2.585 |
| `obj_team2` yard | GL1 | 0.000 | 0.002 | 1.750 | 0.028 | 1.780 |
| | Vulkan before | 0.004 | 0.010 | 1.722 | 0.027 | 1.762 |
| `obj_team2` yard, `r_fastsky 1` | GL1 | 0.000 | 0.264 | 1.503 | 0.034 | 1.801 |
| | Vulkan, alpha written | 0.004 | 0.279 | 1.560 | 0.028 | 1.871 |

So the start and the end of the frame (the swapchain image's layout
transitions, the submission, the present) cost what GL1's swap costs,
and the yard was already faster than GL1. The difference is in the
drawing, and it is largest in the views that clear the color buffer (to
the fog color in `dm/mohdm3`, where the view holds no sky, and with
`r_fastsky 1`); GL1 draws its sky more slowly than Vulkan (`r_fastsky 1`
takes 0.25 ms of drawing off GL1's yard and about 0.13 off Vulkan's),
which hid the rest there. What it was, each measured on its own against
the build before it (medians of 3 interleaved runs, ms per frame, frame
time):

- Partial color writes. GL1's window buffer is BGRX8 (no alpha), so its
  RGB writes are writes of whole pixels; the swapchain image is BGRA8 and
  the pipelines wrote RGB only, partial writes, which the DCC-compressed
  target has to merge with the alpha it holds. Writing alpha, as 1, in
  the pipelines without blending: 2.560 to 2.533 in the overview (-1.1 %),
  1.757 to 1.727 in the yard (-1.7 %), 1.952 to 1.872 in the yard with
  `r_fastsky 1` (-4.1 %). Alpha written by the blending pipelines too
  (unchanged, factors 0 and 1) saves nothing more (2.532, 1.724) and
  changes the rounding of a few blended pixels by 1 (6 in `d3_street`, 15
  in `o2_pipe2`, both directions: the 1 in 10^5 of the second round), so
  they keep the RGB mask; with only the pipelines without blending the
  frames are the same to the bit.
- The stencil plane. D32S8 has one; GL1's depth buffer (Z24X8) has none.
  Without a stencil buffer (`r_stencilbits` 0, the default on Linux) the
  depth image is D32, whose HTILE holds depth only and whose depth clear
  is a fill instead of a compute pass that keeps the stencil bits (the
  clear took 0.010 ms in the yard against GL1's 0.002): with alpha written,
  2.531 to 2.516 in the overview, 1.727 to 1.719 in the yard (-0.5 %).
- The color clear. To a color with components other than 0 and 1, both
  drivers clear with DCC comp-to-single, a compute pass that writes the
  color into every 256-byte block: 0.266 ms with GL1, 0.280 with Vulkan
  (with the depth clear). A quad drawn over the view (writing alpha, so
  whole pixels) takes 0.247 ms: with the two changes above, 2.517 to
  2.500 in the overview, 1.855 to 1.837 in the yard with `r_fastsky 1`,
  3.019 to 2.984 in the `m3l3` path (fog color too), the yard without a
  color clear unchanged (1.715, 1.716). Done on RADV only (other drivers
  clear any color without such a pass, untested here) and without
  multisampling.
- Not tried, from RADV's source: the load and store operations of the
  render pass (`DONT_CARE` for depth at the end of the frame, for stencil
  throughout). `radv_cmd_buffer.c` never reads `storeOp` and
  `radv_meta_clear.c` only looks for `loadOp` `CLEAR` (Mesa 26.0.8), and
  the measured views have one render pass per frame.

What is left is the textures. radeonsi gives DCC to textures, RADV only
to images that can be color attachments and are larger than 512x512
(`radv_image_use_fast_clear_for_image_early`): after loading
`dm/mohdm3`, 711 of GL1's 940 tiled textures have DCC (`AMD_DEBUG=tex`;
128x128 ones, the size of the lightmaps, among them) and none of
Vulkan's 533 (`RADV_DEBUG=img`), which are all 512x512 or smaller. With
`RADV_DEBUG=forcecompress` (DCC for every image) the final build takes
2.438 ms in the overview (GL1 2.473), 1.687 in the yard (1.773) and
2.984 in the `m3l3` path (3.022): faster than GL1 in every view, 0.03 to
0.06 ms less than without it. The renderer can't get that: its textures
are below RADV's threshold whatever their usage flags. It would be a
change to RADV (compressing sampled images that copies write, as
radeonsi does); not done.

The result against GL1 and the build before (idle machine; medians of 3
interleaved runs; frame time with the fps of the median and the main
pass's GPU time; ms per frame; the frames are the same to the bit, see
Testing):

| view | size | frame GL1 | before | after | GPU before | after |
| --- | --- | --- | --- | --- | --- | --- |
| `dm/mohdm3` overview | 3840x2160 | 2.483 (403 fps) | 2.566 (390) | 2.497 (400) | 2.535 | 2.466 |
| `obj_team2` yard | 3840x2160 | 1.775 (563) | 1.777 (563) | 1.721 (581) | 1.749 | 1.699 |
| `obj_team2` yard, `r_fastsky 1` | 3840x2160 | 1.779 (562) | 1.952 (512) | 1.848 (541) | 1.929 | 1.825 |
| `m3l3` path | 3840x2160 | 3.062 (327) | 3.129 (320) | 3.017 (331) | 3.090 | 2.980 |
| `pm1_front` | 3840x2160 | 4.062 (246) | 4.121 (243) | 3.943 (254) | 4.073 | 3.897 |
| `po1_front` | 3840x2160 | 2.761 (362) | 2.803 (357) | 2.727 (367) | 2.768 | 2.693 |
| `po4_front` | 3840x2160 | 2.551 (392) | 2.626 (381) | 2.491 (401) | 2.595 | 2.461 |
| `pm7_flash` | 3840x2160 | 2.455 (407) | 2.016 (496) | 2.003 (499) | 1.991 | 1.978 |
| `dm/mohdm3` overview | 1920x1080 | | 0.999 | 0.999 | 0.722 | 0.700 |
| `obj_team2` yard | 1920x1080 | | 0.999 | 0.999 | 0.620 | 0.622 |
| `m3l3` path | 1920x1080 | | 0.999 | 0.998 | 0.943 | 0.921 |
| `pm1_front` | 1920x1080 | | 1.299 (770) | 1.271 (787) | 1.271 | 1.241 |
| `po1_front` | 1920x1080 | | 0.999 | 0.999 | 0.834 | 0.823 |
| `po4_front` | 1920x1080 | | 0.999 | 0.999 | 0.812 | 0.792 |
| `pm7_flash` | 1920x1080 | | 0.999 | 0.999 | 0.663 | 0.661 |

(0.999 is the 1000 frames per second limit; others 2.3 CPUs at most, in
one run of the build before in the yard, its slowest. GL1 varies more
from run to run in the `m3l3` path, 3.005 to 3.119 ms, and in
`pm7_flash`, where it is CPU-bound with the three lights.) At 3840x2160
the GPU time per frame goes down 2.7 to 5.4 % (0.7 % in `pm7_flash`),
and Vulkan's frames are faster than GL1's in every view but the
`dm/mohdm3` overview (0.6 % slower) and the yard with `r_fastsky 1` (3.9
% slower: GL1's slow sky is gone there); at 1920x1080, where these views
are CPU-bound but `pm1_front`, the GPU time goes down or stays within
±0.3 %, and `pm1_front` runs at 787 fps instead of 770.

The plan items, each measured before and after with interleaved builds:

| item | measured |
| --- | --- |
| depth image readable only while depth or stencil reads are wanted (new) | main pass -12/-13 % in the `m6l1a` forest at 1080p/3840x2160 (7 % less frame time than GL1 at 3840x2160, 3 % less at 1080p), -9 % `m3l3`, -7 % `dm/mohdm3`, unchanged in `obj_team2`; the same frames as the build before it (see Testing); multisampling unchanged |
| drawing into the swapchain image (new) | no copy: 8 to 9 % more fps where the GPU limits (above), 0.02 ms less CPU at the end of each frame; screenshots unchanged |
| frame pacing (acquire right after present) | with vsync (60 Hz, `m6l1a`): start of a frame's recording to its present 16.3 ms with the image acquired at the frame's first render pass (the earlier code waited in the acquire before its copy, the same), 1.7 ms after; frame interval p99/sd 18.4/0.60 ms at 1080p, GL1 18.3/0.61, the copy-based code 18.6/0.69 |
| texture memory sub-allocator | per map load, `m6l1a`/`obj_team2`/`dm/mohdm3`/`m3l3`: 317/524/557/526 device allocations become 10/11/11/11, time in `vkAllocateMemory` 7.3/12.2/13.5/11.9 ms becomes 0.3 ms, creating texture storages 8.6/14.5/16.3/14.6 ms becomes 1.5/2.5/3.0/2.7 ms, renderer load time 339/688/616/667 becomes 327/673/595/666 ms; per frame: no change (submissions 41 to 51 µs per frame either way) |
| pipeline cache on disk | 15 pipelines from startup to the first `m6l1a` view: 17.5 to 19.4 ms with no file and `MESA_SHADER_CACHE_DISABLE=true`, 0.2 ms with the file; with RADV's own cache 0.5 ms without the file. No pipeline was created during the measured frames |
| vertex copies of multi-pass shaders | vertex bytes per frame 588/826/716/509 KB become 584/819/690/503 KB; no measurable time |
| fewer render pass breaks | nothing to remove: 1.00 render pass and 1 submission per frame in the 4 views, in `mohdm1_dlight` (94 KB of dynamic lightmaps uploaded per frame, all before the first draw that uses them) and in the `m2l1` sky portal |
| `r_ext_multisample` | 4 samples, `m6l1a` at 1080p: 2.94 ms per frame against GL1's 2.71 ms (4.23 ms while the depth/stencil buffer was resolved with every render pass; 2.94 against 2.70 again after the depth image change; 2.77 against 2.70 with alpha written, second round) |

Multisampling matches GL1's (`r_ext_multisample 4` with both): in the
`mohdm1` views 9.1 % of the pixels differ, nearly all by 1, against 7.3 %
without multisampling; each renderer changes the same ~4 % of the pixels
(edges, max 112) when it is turned on.

Frame spikes (2026-10-16 night): what makes the slowest frames (the
99th and 99.9th percentiles of the frame time, the maximum), measured
before changing anything. A tester (NVIDIA RTX 4070 Ti, Linux/Wayland)
judges by these percentiles; the medians and means of the tables above
don't show them, and fixed views (`perf.py`'s usual views) hide what only
happens when things move: effects, players, sounds, models and textures
loaded on first use, pipelines created on first use.

The frame log of the Vulkan renderer records every frame with what
happened in it. Off by default; `r_vkFrameLog` is latched, so:

```sh
./openmohaa +set r_vkFrameLog 1 ...        # or in the console: r_vkFrameLog 1; vid_restart
```

then, in the console, `vkframelog clear` before what should be measured
(a match, a demo) and `vkframelog name.txt` after it (or only
`vkframelog`, which writes `vkframes.txt`): the file is written to
`<fs_homepath>/main/` (the game's writable directory: the console's
`path` command lists it first) and holds at most the last 131072 frames (`r_vkFrameLog 500000` keeps
more; 128 bytes per frame). The console shows the percentiles and the
slowest frames; the file is what to send. Its header has the device,
driver, present mode, size and these percentiles; then one line per
frame, in microseconds:

| columns | |
| --- | --- |
| `frame start interval` | index, start (since the log started), frame time: from this `BeginFrame` to the next |
| `begin scene 2d end load` | main thread time in `BeginFrame`, `RenderScene` (the front end), the 2D calls (in OpenMoHAA the queued render commands run there: the 3D view's back end, and the fence wait at the start of the frame), `EndFrame` (submission, present, the acquire of the next image) and the registration calls (models, shaders, skins, fonts loaded during the frame) |
| `cgame engine` | the rest of the frame: `cgame` from `BeginFrame` to `EndFrame` outside of the renderer's calls (the UI and cgame building the scene), `engine` from `EndFrame`'s return to the next `BeginFrame` (sound mixing, the frame limiter's sleep, input and network events, the local server's game frame, the client's packets) |
| `present_at` | when `vkQueuePresentKHR` was called (differences: present to present) |
| `fence flush submit acquire acquire_ahead present` | waits for the frame fence and for mid-frame flushes, `vkQueueSubmit`, `vkAcquireNextImageKHR` before the present (normally 0) and right after it, `vkQueuePresentKHR` |
| `gpu gpu_start gpu_end` | GPU time of the main command buffer, and when it started and ended on the GPU (since the first timestamp; 0: none): `gpu_end` differences are the GPU's frame to frame time, `gpu_start` minus the previous `gpu_end` the GPU's idle time |
| `draws render_passes submits vertex_bytes staging_bytes` | per frame |
| `pipelines pipeline_us textures texture_us allocations allocation_us flushes vertex_growth depth_targets swapchains` | what was created in the frame and the time it took: pipelines, texture storages, device memory, mid-frame flushes, a frame vertex buffer replaced by a larger one (its size), the depth image replaced, a swapchain created |
| `skin_uploads skin_bytes` | skin data of skeletal models added in the frame (`vk_add_skin_data` calls and bytes; GPU skinning, see Implementation notes, Skeletal models) |
| `thread_wait thread_start thread_submit thread_acquire thread_present` | `r_vkSubmitThread 1`: the main thread's waits for the submit thread in the frame, and the thread's times for the frame (from its hand-off to the thread starting it, in `vkQueueSubmit`, `vkAcquireNextImageKHR`, the acquire of the next frame's image included, and `vkQueuePresentKHR`; `present_at` is then the thread's present). The columns `submit`, `acquire`, `acquire_ahead` and `present` are the main thread's |

`tools/framelog.py file` prints the distributions and the causes of the
slow frames (below). The log changes nothing of what the frames do or
draw (the same submissions, presents and acquires, in the same order) and
costs nothing when off: `GetRefAPI` returns the exports themselves then.
While it is on, its timing costs about 15 µs per frame on this machine
(two clock reads around each of the HUD's ~300 2D calls).

The same split of the frame for GL1: `renderer_timed.so` (`rtimer.c`)
writes every frame it measures to `$RTIMER_FRAMES` with the first columns
of the log, measured the same way, and `libglts.so` (`glts.c`) writes GL1's
GPU timestamps of every frame to `$GLTS_FRAMES`. `perf.py --framelog DIR`
keeps them for every run (and the Vulkan log for the renderers named by
`--vklog`), `framelog.py --batch DIR` compares them.

Engine time: the renderer only sees its own calls, so what isn't in them
is split by where it happens, `cgame` inside the frame and `engine`
between frames, without touching the engine. Within a frame below the
1000 frames per second limit, `engine` also holds the frame limiter's
sleep (`Com_Frame` waits for the next whole millisecond). For the details
of slow frames, `framelog.py --stacks samples.txt frames.rt.txt` puts the
stack samples of `libstacks.so` (`perf.py --stacks` in the same run;
both use `CLOCK_MONOTONIC`) into the frames of `renderer_timed.so`'s file
and lists the functions by their share of the slow frames' samples.

Moving content that both renderers can replay: demos. The client plays
`.dm_8` demos (`demo name`), and the original game's `.dm3` demos (protocol
8, the same format) through a `.dm_8` link. With `fixedtime 16` every
frame advances the demo by 16 ms whatever the frame took, so frame k shows
the same moment in every run and with both renderers (the same frames to
compare one by one), like a timedemo that interpolates between snapshots.
`views/perf_demo.txt` lists two matches recorded by original game clients
(files of the dev machine): `o2match` (`obj/obj_team2`, 256 s, the
recorder's own view, 52 kills) and `o4match` (`obj/obj_team4`, 246 s, 23
kills), 15000 frames each (240 s of the match) from the first frame after
loading. Bots (`sv_maxbots`, `addbot`) move and shoot but are random from
run to run (the game seeds `rand()` with the clock); they are a way to
record a demo, not to measure.

Results (2026-10-16 night, idle machine: others 1.6 CPUs at most but in
one GL1 run of the static views, 2.1, my analysis running beside it;
medians of 3 interleaved runs; frame time from `renderer_timed.so`, ms;
Vulkan without the frame log, whose runs gave the same within the run to
run spread). The demos, 15000 frames each, warm caches (the pipeline cache
file and Mesa's shader cache filled by earlier runs, as on a second
play):

| demo | size | renderer | mean (fps) | median | p99 (fps) | p99.9 (fps) | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 640x480 | GL1 | 1.050 (952) | 1.000 | 1.736 (576) | 2.258 (443) | 25.7 |
| | | Vulkan | 1.003 (997) | 1.000 | 1.039 (963) | 1.442 (693) | 20.9 |
| | 1920x1080 | GL1 | 1.075 (930) | 1.000 | 1.920 (521) | 2.646 (378) | 25.8 |
| | | Vulkan | 1.003 (997) | 1.000 | 1.175 (851) | 1.560 (641) | 21.3 |
| | 3840x2160 | GL1 | 1.416 (706) | 1.243 | 3.122 (320) | 3.810 (262) | 26.3 |
| | | Vulkan | 1.329 (753) | 1.130 | 3.068 (326) | 3.690 (271) | 23.3 |
| `o4match` | 640x480 | GL1 | 1.385 (722) | 1.348 | 2.336 (428) | 3.289 (304) | 31.1 |
| | | Vulkan | 1.037 (965) | 1.002 | 1.415 (707) | 1.821 (549) | 27.6 |
| | 1920x1080 | GL1 | 1.532 (653) | 1.513 | 2.706 (370) | 3.518 (284) | 31.6 |
| | | Vulkan | 1.168 (856) | 1.081 | 1.953 (512) | 2.693 (371) | 28.1 |
| | 3840x2160 | GL1 | 2.368 (422) | 2.273 | 3.999 (250) | 5.091 (196) | 32.3 |
| | | Vulkan | 2.241 (446) | 2.167 | 3.671 (272) | 4.950 (202) | 30.5 |

(1.000 is the 1000 frames per second limit of `Com_Frame`; the tester's
client has none, ours puts the median of the lighter frames there.) In the
matches Vulkan's 99th and 99.9th percentiles are lower than GL1's at every
size, by 0.6 to 0.9 ms at 640x480 and 1920x1080 (where GL1's CPU time per
frame limits) and 0.1 to 0.3 ms at 3840x2160 (where the GPU does), and so
is the maximum.

What makes the slow frames slow. The demo's frames are the same moments
in every run, so each frame's median over the 3 runs is what the content
costs, and a run's frame minus that median is noise of that run (the
system, the driver, the clock): `framelog.py --batch` splits them. The
noise is lower with Vulkan in every demo and size (99.9th percentile of a
frame's excess over its median: 0.13 against 0.65 ms at 640x480 in
`o2match`, 0.90 to 1.20 against 1.44 ms at 1920x1080 in `o4match`, 1.70 to
1.86 against 2.26 at 3840x2160). The content's slow frames (the medians
above their 99th percentile), split into parts that don't overlap, each
part's excess over its median summed (Vulkan's frame log; at 1920x1080 from
a run of the fixed log, see below):

| demo, size | front end (`scene`) | back end | cgame | registration (`load`) | fence wait | engine | pipelines |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` 1920x1080 | 19 % | 20 % | 21 % | 28 % | 6 % | 4 % | 1 % |
| `o4match` 1920x1080 | 38 % | 27 % | 15 % | 13 % | 0 | 5 % | 0 |
| `o2match` 3840x2160 | 10 % | 10 % | 10 % | 15 % | 53 % | 0 | 0 |
| `o4match` 3840x2160 | 19 % | 14 % | 10 % | 15 % | 37 % | 5 % | 0 |

(The logger of these batch runs timed nested registration calls twice;
fixed, the 1920x1080 rows come from the run with the stack samples below,
the 3840x2160 ones are as measured, their `load` share a little high.)

- The heavier moments of the match (more players, effects, sprites and
  terrain in view: the front end, the back end and cgame) make most of the
  99th percentile with both renderers; Vulkan's are smaller (GL1's front end
  and back end cost more per frame). At 3840x2160 it is the GPU: the fence
  wait of the frames whose drawing takes the GPU longer (the main pass
  takes 1.12 ms in the median `o2match` frame and 2.96 ms at its 99th
  percentile).
- Registration in the middle of the match makes the largest frames (21 to
  31 ms, the maximum of both renderers, GL1's larger). The stack samples
  of `libstacks.so`, put into the frames (`framelog.py --stacks`, one run
  per renderer at 1920x1080), show what: a configstring announces a model
  (a player's model or weapon, `CG_ProcessConfigString`,
  `RegisterServerModel`), cgame caches its resources (`CacheResource`)
  and its shaders load their images from the pk3s (`R_FindShader`,
  `LoadTGA`/`LoadJPG`, the zlib inflate, `R_MipMap`), 22 textures and 6.7
  MB in the worst frame of `o2match`; the file system's case correction
  (`FS_CorrectCase`, `opendir`/`readdir` of the directories) is part of
  it. Code shared by both renderers.
- The skin data of GPU skinning (Vulkan only): a skeletal model's first
  draw builds and uploads its skin data (`R_SkinAddSurface`,
  `R_SkinThresholds`, 429 KB for a player model), 0.6 ms more than GL1 in
  that frame (`o2match` frame 142, every run), once per model and map
  (since 2026-10-18 when the model is registered, see Pipelines and skin
  data at load).
- `engine` in `o4match`'s round changes (frames 4451, 7055, 11008, 13483:
  +1.2 ms, with +1.4 ms of cgame): a new soundtrack is loaded and decoded
  on the main thread (`MUSIC_NewSoundtrack`, `S_CodecLoad`, MP3 decoding,
  even with the music volume at 0) along with large configstrings (since
  2026-10-23 on a thread and in place, see Round changes).
- Pipelines: with warm caches the same ones are created in every run (a
  process creates its pipelines on first use, the caches make that cheap):
  19 or 38 in the first frames after the load and 11 or 20 in 10 or 11
  frames of the match, 0.6 to 1.5 ms in all.

Cold caches (the first play after an install or a driver update: no
`vkpipelines.cache`, `MESA_SHADER_CACHE_DISABLE=true` for both renderers,
`perf.py --cold`), 1920x1080, medians of 3:

| demo | renderer | mean (fps) | p99 (fps) | p99.9 (fps) | max | warm p99.9, max |
| --- | --- | --- | --- | --- | --- | --- |
| `o2match` | GL1 | 1.073 (932) | 1.941 (515) | 2.684 (373) | 25.3 | 2.646, 25.8 |
| | Vulkan | 1.007 (993) | 1.191 (840) | 2.088 (479) | 33.3 | 1.560, 21.3 |
| `o4match` | GL1 | 1.548 (646) | 2.737 (365) | 3.756 (266) | 31.8 | 3.518, 31.6 |
| | Vulkan | 1.157 (864) | 1.893 (528) | 3.516 (284) | 58.1 | 2.693, 28.1 |

Besides the 7 pipelines of the loading (11 ms), Vulkan creates 17 to 38
pipelines in the first frames after the load (25 to 52 ms: the 58 ms
frame of `o4match`) and 11 to 22 more during the match, in 10 to 12
frames, 1.6 to 2.4 ms each and up to 7 ms in one frame; the pipelines are
then 30 to 34 % of the content's slow frames, and Vulkan's 99.9th
percentile comes near GL1's. GL1's changes by 0.04 to 0.24 ms. (Since
2026-10-18 these pipelines are created at load: see Pipelines and skin data
at load below.)

Fixed views (no movement, 5000 frames, medians of 3; the GPU's frame to
frame time: Vulkan's `gpu_end` differences from the frame log, GL1's from
`libglts.so` in 2 runs of their own):

| view | size | renderer | median | p99 | p99.9 | max | GPU frame to frame p99, p99.9 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 1920x1080 | GL1 | 0.999 | 1.195 | 1.627 | 1.925 | |
| | | Vulkan | 0.999 | 1.117 | 1.183 | 1.221 | |
| | 3840x2160 | GL1 | 1.757 | 2.107 | 2.455 | 2.807 | 1.942 to 1.958, 2.306 to 2.703 |
| | | Vulkan | 1.701 | 2.500 | 2.885 | 3.579 | 1.862, 1.928 |
| `dm/mohdm3` overview | 1920x1080 | GL1 | 0.999 | 1.395 | 1.845 | 3.385 | |
| | | Vulkan | 0.999 | 1.069 | 1.146 | 1.269 | |
| | 3840x2160 | GL1 | 2.457 | 2.798 | 3.275 | 3.682 | 2.707, 2.881 to 3.407 |
| | | Vulkan | 2.476 | 3.019 | 3.484 | 3.667 | 2.701, 2.872 |
| `pm1_front` | 1920x1080 | GL1 | 1.848 | 2.876 | 3.345 | 3.778 | |
| | | Vulkan | 1.250 | 1.737 | 2.059 | 2.619 | |
| | 3840x2160 | GL1 | 4.020 | 4.690 | 5.333 | 5.897 | 5.325, 5.967 (1 run) |
| | | Vulkan | 3.931 | 5.039 | 5.628 | 6.126 | 4.192, 4.345 |

The build with the frame log and the build before it, logger off, give
the same frame times (0.002 ms at most between their medians, renderer
time within 0.013 ms); with the log on the renderer time is up to 0.02 ms
higher at 640x480 and 1920x1080 and the same within the noise at
3840x2160. Without `com_speeds 1` (perf.py no longer prints it when
`renderer_timed.so` measures), the spread of the 3840x2160 views is about
what the earlier runs with it gave (the overview's 99th percentile 3.02 to
3.20 ms against 3.24 then, GL1 2.80 against 2.86).

At 3840x2160, where the GPU limits, Vulkan's frame times spread more than
GL1's (99th percentile 0.2 to 0.4 ms higher) but the GPU finishes its
frames as evenly as GL1's or more (the GPU frame to frame times above):
what reaches the screen is as even, the spread is in when the main thread
starts its frames. Two causes, from the frame log:

- Where the wait is. Vulkan waits for the fence of frame N-2 at the start
  of frame N's back end, which in OpenMoHAA is the first UI widget's
  `set2D()` (`UIWidget::Display` runs the queued `RB_DrawBuffer`), before
  `View3D::Draw` runs cgame and `RenderScene`. Time those take beyond
  their median lengthens that frame and shortens the next frame's wait (a
  long interval, then a short one). GL1 waits in its swap, after the
  frame's work, which absorbs it in the same frame. The slow frames of the
  fixed views keep a fence wait near its median (1.6 ms in the overview)
  and add front end, back end and engine time above their medians.
- `vkAcquireNextImageKHR` right after the present waits 0.6 to 1 ms in 4 %
  of the yard's frames (215 of 4998; 12 in the overview), when the X
  server (Xwayland) has not released an image yet; the fence wait is that
  much shorter in those frames.

In the moving demos the GPU's load changes from frame to frame and
Vulkan's 99th percentile is still below GL1's at 3840x2160.

Checks of the log itself: against the build before it (`run.sh`,
`tgadiff.py`, 640x480), `dm/mohdm1`, `obj/obj_team2`, `mohdm1_dlight` and
`mp_mohdm1` (17 views) give the same frames to the bit with
`r_vkFrameLog` 0 and 1; no validation message in these runs
(`r_vkValidation 1`), nor with synchronization validation on
`mohdm1_dlight` with the log on, nor in a demo played with the log on
through `vkframelog`, `vid_restart` (the log starts again), `vkframelog`
twice (the second has no frames), `vkframelog clear` and `vkframelog`;
`renderer_vulkan.so` still imports no GL or Vulkan symbol and exports
only `GetRefAPI`.

Frame limiter (2026-10-17; an engine change, `Com_Frame` in
`code/qcommon/common.c`, shared by both renderers): `com_maxfps` was
applied in whole milliseconds. `Com_Frame` waited until
`Sys_Milliseconds` had passed the last frame's start by `1000 /
com_maxfps`, an integer, less a `bias` that took the previous frame's
lateness off, so caps that don't divide 1000 were wrong (144 and 165 ran
at 167 frames per second, 240 at 250, 334 to 499 at 500: players who cap
just below their display's refresh rate for VRR got more), and frames
started on the millisecond tick.

Now a cap is a deadline in microseconds (`Sys_Microseconds`:
`CLOCK_MONOTONIC`, `QueryPerformanceCounter` on Windows) that advances by
exactly one period (1000000 / cap, rounded up so that the rate never
exceeds the cap; a cap of 1000 or more had no deadline, only the
millisecond below, like uncapped, until the command rate change of
2026-10-19 (see Command rate: now 1000 waits for the millisecond, a cap
above it for its deadline alone, uncapped for nothing); a cap that divides 1000 has its deadline
in whole milliseconds since the change described at the end of this part,
Caps that divide 1000). A frame late by less than a period is
made up by the next one; after a later one the deadline starts again from
now (no burst of frames to catch up). As before, a frame also
waits until `Sys_Milliseconds` has passed the previous frame's start
(`com_frameTime`) by `minMsec`: 1 ms for a client, so that no frame lasts
0 ms of game time (`Com_ModifyMsec` would make it 1 ms and the game would
run ahead of real time) and uncapped still means at most 1000 frames per
second (both changed on 2026-10-19, see Command rate); a dedicated server's frame (`SV_FrameMsec`). The game time of a
frame alternates where the period isn't whole milliseconds (6 and 7 ms at
144). `com_timedemo`, `com_busyWait`, `com_maxfpsUnfocused`,
`com_maxfpsMinimized` and the local server's queued packets
(`SV_SendQueuedPackets`) work as before. The order of a frame is the same:
the wait, then the input (`IN_Frame`), the events, the server and client
frames; nothing waits after the input is read, and the renderer has no
part in it (no frame is held before its present).

The wait (`Com_FrameWait`) sleeps in `select` on the network sockets
(`NET_SleepUs`: packets are still read as they come) until 1 ms before the
deadline and polls the network for the rest, as the old code did for the
last millisecond before the tick. The millisecond (`Sys_Milliseconds`)
condition is waited for as before. Sleeps end late, by more when the core
had the time to reach a deep idle state; on this machine (Ryzen 7 PRO
8700GE, `amd-pstate-epp`, the menu idle governor; C1 1 µs exit latency, C2
18 µs, C3 350 µs with 700 µs target residency) a `select` on a UDP socket,
idle machine, 2000 sleeps each, woke up this late (µs):

| sleep asked | timer slack 50 µs (default): median, p99, p99.9, max | timer slack 1 µs |
| --- | --- | --- |
| 200 µs | 55, 80, 90, 251 | 6, 31, 38, 45 |
| 1 ms | 56, 174, 517, 579 | 7, 62, 429, 466 |
| 3 ms | 81, 667, 854, 907 | 35, 576, 741, 773 |
| 6 ms | 83, 625, 814, 850 | 45, 641, 798, 813 |

The timer slack is now 1 µs on Linux (`PR_SET_TIMERSLACK` in
`Sys_PlatformInit`; threads created after inherit it). The long sleeps'
late wake-ups (C3) are why the last millisecond is polled. Shorter sleeps
in that millisecond instead were tried, in a simulation (a loop with 1 ms
of work per frame, 10000 frames at 144 frames per second: sleeps of 100 µs
and polling the last 150 µs started the frames as late as polling at the
99th percentile, 4 µs, but 0.16 ms at the 99.9th against 0.02, for 2.8 %
of a core instead of 13 %) and in the game, where they cost latency:
the core clocks down when it sleeps more, and the frame's work, from its
input read to its present, takes longer. `o2match` (see below) at
1920x1080 with Vulkan, medians of 3 runs, in three batches (`libinlat.so`,
below; CPU: the game's, in CPUs):

| the wait's last millisecond | cap 144: input to present median, CPU, core clock | cap 240 |
| --- | --- | --- |
| old limiter (167 and 250 frames per second) | 0.685 to 0.696 ms, 0.20, 3191 to 3203 MHz | 0.616 to 0.631 ms, 0.27, 3271 to 3280 MHz |
| polled (kept) | 0.666 to 0.671 ms, 0.27, 3247 to 3267 MHz | 0.578 to 0.586 ms, 0.42, 3345 to 3354 MHz |
| sleeps of 100 µs, the last 150 µs polled | 0.713 to 0.723 ms, 0.16, 3185 to 3193 MHz | 0.619 to 0.631 ms, 0.24, 3211 to 3217 MHz |
| sleeps of 30 µs (no C2) | 0.702 ms, 0.18, 3227 MHz | 0.621 ms, 0.26, 3262 MHz |
| sleeps, the last 500 µs polled | 0.720 ms, 0.21, 3217 MHz | 0.619 ms, 0.31, 3270 MHz |
| sleeps, the last 300 µs polled | 0.729 ms, 0.18, 3198 MHz | 0.621 ms, 0.26, 3226 MHz |

Only polling the whole millisecond made the input to present time shorter
than the old limiter's (by 0.02 to 0.05 ms); the sleeps saved CPU but
made it up to 0.03 ms longer at 144. The polling costs no measurable GPU
time on this APU, whose CPU and GPU share a power budget: at 3840x2160 and cap 144 the
GPU's main pass took 2.17 to 2.20 ms per frame with it, 2.16 to 2.18 with
the old limiter and 2.00 to 2.23 with 30 µs sleeps (input to present
0.652, 0.676 and 0.686 ms). On Windows (untested: the CI builds it, this
machine can't run it) a sleep lasts whole timer ticks, 1 ms at best
(`Sys_PlatformInit` asks for 1 ms with `timeBeginPeriod`), so the wait
sleeps whole milliseconds while more than 2 ms remain and polls the rest;
`Sys_Microseconds` takes the whole seconds and the rest of
`QueryPerformanceCounter` apart (the counter times a million overflows
after a few days of uptime).

Results (2026-10-17, idle machine: other processes 1.5 CPUs at most, load
average under 2 with the game; 1920x1080 under Xwayland, the display at
59.96 Hz; medians of 3 interleaved runs of 10 s with each renderer and the
engine before ("old", left of the arrow) and after this change: `perf.py
--binaries openmohaa_before,openmohaa --caps 60,125,144,165,240,333,500,1000,0
--seconds 10 --preload libinlat.so --framelog DIR`, then `framelog.py
--batch DIR`). Frames per second: from the mean interval between input
reads, the frame starts the limiter sets; frame time: `renderer_timed.so`'s
`BeginFrame` to `BeginFrame`; frame starts: the interval between input
reads (`libinlat.so`); CPU: the game process in CPUs, its main thread in
parentheses. `o2match` (`views/perf_demo.txt` from its start, 600 frames at
60 to 10000 uncapped; the rows of 1000 and 0 come from a second batch,
since a cap of 1000 now counts as uncapped):

| `com_maxfps` | renderer | frames per second | frame time (ms): median, p99, p99.9, max, sd | frame starts (ms): p99, sd | input to present (ms): median, p99 | CPU (main thread) |
| --- | --- | --- | --- | --- | --- | --- |
| 60 | GL1 | 62.6 → 60.0 | 15.999, 16.089, 16.532, 16.53, 0.509 → 16.667, 16.731, 16.923, 16.92, 0.041 | 16.057, 0.480 → 16.671, 0.002 | 1.311, 2.821 → 1.277, 2.966 | 0.18 (0.12) → 0.20 (0.15) |
| 60 | Vulkan | 62.6 → 60.0 | 15.999, 16.169, 16.368, 16.37, 0.502 → 16.667, 16.743, 16.861, 16.86, 0.048 | 16.070, 0.481 → 16.675, 0.003 | 1.017, 2.069 → 1.008, 2.093 | 0.11 (0.10) → 0.14 (0.13) |
| 125 | GL1 | 125.1 → 125.0 | 8.000, 8.078, 8.356, 8.79, 0.193 → 8.000, 8.049, 8.151, 8.83, 0.055 | 8.056, 0.192 → 8.003, 0.063 | 0.899, 2.068 → 0.885, 2.097 | 0.27 (0.20) → 0.34 (0.26) |
| 125 | Vulkan | 125.1 → 125.0 | 8.000, 8.074, 8.247, 8.34, 0.198 → 8.000, 8.047, 8.169, 8.32, 0.181 | 8.047, 0.198 → 8.003, 0.074 | 0.718, 1.559 → 0.682, 1.463 | 0.16 (0.15) → 0.24 (0.22) |
| 144 | GL1 | 166.7 → 144.0 | 6.000, 6.072, 6.166, 9.00, 0.157 → 6.945, 6.996, 7.128, 8.98, 0.092 | 6.054, 0.157 → 6.950, 0.099 | 0.837, 1.917 → 0.843, 1.987 | 0.34 (0.24) → 0.38 (0.29) |
| 144 | Vulkan | 166.7 → 144.0 | 6.000, 6.070, 6.178, 8.35, 0.150 → 6.945, 6.996, 7.121, 8.43, 0.083 | 6.047, 0.157 → 6.951, 0.094 | 0.695, 1.317 → 0.658, 1.322 | 0.20 (0.19) → 0.27 (0.25) |
| 165 | GL1 | 166.7 → 165.1 | 6.000, 6.070, 6.157, 8.67, 0.143 → 6.061, 6.113, 6.293, 8.60, 0.153 | 6.053, 0.145 → 6.066, 0.110 | 0.852, 1.844 → 0.830, 1.849 | 0.35 (0.25) → 0.43 (0.33) |
| 165 | Vulkan | 166.7 → 165.0 | 6.000, 6.063, 6.201, 8.26, 0.147 → 6.061, 6.104, 6.229, 8.31, 0.096 | 6.048, 0.144 → 6.064, 0.106 | 0.684, 1.326 → 0.643, 1.232 | 0.20 (0.18) → 0.30 (0.29) |
| 240 | GL1 | 250.0 → 240.0 | 4.000, 4.066, 4.284, 8.93, 0.129 → 4.167, 4.205, 4.529, 8.87, 0.107 | 4.053, 0.137 → 4.170, 0.007 | 0.774, 1.650 → 0.744, 1.618 | 0.46 (0.33) → 0.58 (0.45) |
| 240 | Vulkan | 250.0 → 239.9 | 4.000, 4.045, 4.401, 8.16, 0.111 → 4.167, 4.203, 4.498, 8.28, 0.098 | 4.036, 0.121 → 4.170, 0.098 | 0.620, 1.112 → 0.571, 1.033 | 0.27 (0.25) → 0.42 (0.40) |
| 333 | GL1 | 333.2 → 332.9 | 3.000, 3.059, 3.259, 8.86, 0.115 → 3.004, 3.039, 3.271, 8.98, 0.107 | 3.051, 0.120 → 3.006, 0.019 | 0.748, 1.712 → 0.717, 1.653 | 0.59 (0.42) → 0.78 (0.62) |
| 333 | Vulkan | 333.3 → 332.9 | 3.000, 3.042, 3.243, 8.23, 0.103 → 3.004, 3.038, 3.254, 8.23, 0.096 | 3.036, 0.112 → 3.006, 0.010 | 0.594, 1.150 → 0.545, 1.094 | 0.36 (0.34) → 0.57 (0.55) |
| 500 | GL1 | 500.1 → 499.8 | 2.000, 2.060, 2.585, 8.65, 0.108 → 2.000, 2.048, 2.488, 8.61, 0.100 | 2.051, 0.037 → 2.002, 0.042 | 0.676, 1.600 → 0.655, 1.643 | 0.80 (0.57) → 1.09 (0.87) |
| 500 | Vulkan | 499.8 → 499.6 | 2.000, 2.034, 2.198, 8.20, 0.092 → 2.000, 2.031, 2.190, 8.25, 0.092 | 2.017, 0.101 → 2.001, 0.101 | 0.508, 0.994 → 0.490, 1.018 | 0.52 (0.49) → 0.81 (0.79) |
| 1000 | GL1 | 912.8 → 903.9 | 1.007, 1.967, 2.453, 11.27, 0.255 → 1.008, 1.987, 2.526, 11.47, 0.257 | 1.976, 0.243 → 1.999, 0.259 | 0.720, 1.738 → 0.724, 1.754 | 1.34 (0.92) → 1.33 (0.92) |
| 1000 | Vulkan | 997.3 → 996.7 | 1.000, 1.215, 1.634, 8.62, 0.120 → 1.000, 1.215, 1.567, 8.40, 0.116 | 1.223, 0.124 → 1.216, 0.090 | 0.514, 1.127 → 0.515, 1.140 | 1.04 (0.99) → 1.04 (1.00) |
| 0 | GL1 | 910.8 → 912.2 | 1.007, 1.973, 2.401, 11.38, 0.259 → 1.007, 1.913, 2.391, 11.24, 0.248 | 1.979, 0.263 → 1.927, 0.241 | 0.723, 1.750 → 0.708, 1.717 | 1.33 (0.92) → 1.33 (0.92) |
| 0 | Vulkan | 997.3 → 996.5 | 1.000, 1.215, 1.597, 8.59, 0.117 → 1.000, 1.184, 1.553, 8.49, 0.114 | 1.233, 0.122 → 1.181, 0.119 | 0.516, 1.145 → 0.504, 1.106 | 1.04 (0.99) → 1.04 (1.00) |

The `obj_team2` yard (a fixed view of `views/perf.txt`, the local server
running):

| `com_maxfps` | renderer | frames per second | frame time (ms): median, p99, p99.9, max, sd | frame starts (ms): p99, sd | input to present (ms): median, p99 | CPU (main thread) |
| --- | --- | --- | --- | --- | --- | --- |
| 60 | GL1 | 62.5 → 60.0 | 15.999, 16.224, 16.549, 16.55, 0.107 → 16.667, 16.893, 16.957, 16.96, 0.104 | 16.056, 0.013 → 16.671, 0.002 | 1.097, 2.128 → 1.114, 2.376 | 0.15 (0.12) → 0.18 (0.14) |
| 60 | Vulkan | 62.5 → 60.0 | 16.001, 16.257, 16.682, 16.68, 0.119 → 16.667, 16.889, 17.137, 17.14, 0.105 | 16.083, 0.052 → 16.677, 0.004 | 0.960, 1.894 → 0.961, 2.063 | 0.11 (0.10) → 0.13 (0.13) |
| 125 | GL1 | 125.0 → 125.0 | 8.000, 8.200, 8.402, 8.59, 0.102 → 8.000, 8.174, 8.227, 8.47, 0.091 | 8.061, 0.021 → 8.003, 0.001 | 0.877, 1.596 → 0.851, 1.533 | 0.25 (0.19) → 0.31 (0.25) |
| 125 | Vulkan | 125.0 → 125.0 | 8.000, 8.198, 8.271, 8.47, 0.096 → 8.000, 8.172, 8.217, 8.42, 0.088 | 8.051, 0.013 → 8.003, 0.001 | 0.743, 1.320 → 0.682, 1.280 | 0.15 (0.14) → 0.23 (0.22) |
| 144 | GL1 | 166.7 → 144.0 | 6.000, 6.184, 6.429, 6.48, 0.094 → 6.945, 7.115, 7.352, 7.39, 0.091 | 6.058, 0.022 → 6.949, 0.002 | 0.783, 1.382 → 0.821, 1.423 | 0.29 (0.22) → 0.34 (0.28) |
| 144 | Vulkan | 166.7 → 144.0 | 5.999, 6.186, 6.234, 6.47, 0.092 → 6.945, 7.107, 7.174, 7.32, 0.088 | 6.042, 0.009 → 6.949, 0.002 | 0.677, 1.207 → 0.649, 1.240 | 0.19 (0.17) → 0.26 (0.25) |
| 165 | GL1 | 166.7 → 165.0 | 6.000, 6.190, 6.400, 6.56, 0.097 → 6.060, 6.223, 6.410, 6.49, 0.087 | 6.065, 0.030 → 6.065, 0.002 | 0.791, 1.487 → 0.747, 1.334 | 0.29 (0.22) → 0.38 (0.31) |
| 165 | Vulkan | 166.7 → 165.0 | 6.000, 6.171, 6.398, 6.51, 0.092 → 6.061, 6.216, 6.446, 6.47, 0.087 | 6.036, 0.008 → 6.065, 0.002 | 0.673, 1.223 → 0.625, 1.146 | 0.19 (0.18) → 0.30 (0.28) |
| 240 | GL1 | 250.0 → 240.0 | 3.999, 4.133, 4.172, 4.23, 0.072 → 4.166, 4.296, 4.461, 4.71, 0.075 | 4.054, 0.019 → 4.170, 0.016 | 0.692, 1.110 → 0.644, 1.158 | 0.36 (0.26) → 0.51 (0.42) |
| 240 | Vulkan | 250.0 → 240.0 | 4.000, 4.119, 4.155, 4.26, 0.070 → 4.166, 4.302, 4.458, 4.61, 0.075 | 4.007, 0.005 → 4.171, 0.002 | 0.587, 0.992 → 0.555, 1.014 | 0.26 (0.25) → 0.41 (0.39) |
| 333 | GL1 | 333.3 → 332.9 | 3.000, 3.114, 3.147, 3.17, 0.065 → 3.004, 3.118, 3.169, 3.26, 0.066 | 3.041, 0.010 → 3.007, 0.012 | 0.634, 1.001 → 0.602, 1.023 | 0.44 (0.32) → 0.69 (0.57) |
| 333 | Vulkan | 333.3 → 332.9 | 3.000, 3.111, 3.144, 3.16, 0.065 → 3.004, 3.114, 3.146, 3.19, 0.063 | 3.003, 0.004 → 3.007, 0.002 | 0.556, 0.902 → 0.520, 0.864 | 0.34 (0.32) → 0.55 (0.53) |
| 500 | GL1 | 500.0 → 500.0 | 2.000, 2.098, 2.161, 2.73, 0.054 → 2.000, 2.096, 2.147, 2.30, 0.056 | 2.014, 0.015 → 2.002, 0.007 | 0.543, 0.890 → 0.533, 0.847 | 0.62 (0.46) → 0.97 (0.82) |
| 500 | Vulkan | 500.0 → 500.0 | 2.000, 2.091, 2.121, 2.14, 0.050 → 2.000, 2.094, 2.122, 2.14, 0.051 | 2.002, 0.003 → 2.002, 0.001 | 0.457, 0.721 → 0.447, 0.706 | 0.50 (0.48) → 0.78 (0.76) |
| 1000 | GL1 | 999.9 → 998.5 | 0.999, 1.189, 1.453, 2.00, 0.092 → 0.999, 1.201, 1.623, 2.89, 0.085 | 1.126, 0.038 → 1.164, 0.049 | 0.565, 0.877 → 0.563, 0.895 | 1.25 (0.94) → 1.24 (0.94) |
| 1000 | Vulkan | 1000.0 → 1000.0 | 0.999, 1.105, 1.186, 1.29, 0.047 → 0.998, 1.102, 1.201, 1.29, 0.049 | 1.001, 0.004 → 1.002, 0.006 | 0.431, 0.616 → 0.433, 0.665 | 1.03 (1.00) → 1.03 (1.00) |
| 0 | GL1 | 999.8 → 998.7 | 0.999, 1.180, 1.449, 1.71, 0.085 → 0.999, 1.184, 1.500, 1.96, 0.083 | 1.120, 0.037 → 1.116, 0.040 | 0.567, 0.878 → 0.559, 0.869 | 1.25 (0.94) → 1.25 (0.94) |
| 0 | Vulkan | 1000.0 → 1000.0 | 0.999, 1.098, 1.177, 1.35, 0.047 → 0.999, 1.097, 1.178, 1.56, 0.047 | 1.002, 0.007 → 1.001, 0.012 | 0.438, 0.652 → 0.432, 0.656 | 1.03 (1.00) → 1.03 (1.00) |

- The caps are exact: the mean interval between frame starts is the
  period to the microsecond (6944.997 µs at 144 for a period of 6945,
  16666.98 at 60), where the old limiter gave 62.5 frames per second at
  60, 166.7 at 144 and 165 and 250 at 240. Caps that divide 1000 (125,
  500) ran at their rate before too.
- Frame starts are even: the 99th percentile of the interval between them
  is 2 to 10 µs above the period at every cap up to 500 (the old limiter's
  frames started on the millisecond tick, 2 to 83 µs above its whole
  milliseconds), with a standard deviation of 0.001 to 0.016 ms in the
  yard. In the demo what is left are the frames that take longer than a
  period (models loaded in the middle of the match, 8 to 9 ms), followed
  by one short interval that makes up for it when they were late by less
  than a period.
- The frame time and the presents still vary with each frame's own work
  (the local server's frame, cgame, the renderer), relative to the period
  about as before: the limiter starts frames evenly and holds nothing
  after the input is read.
- Input to present: the median is the same or shorter at every cap with
  both renderers (in the yard Vulkan 0.649 against 0.677 ms at 144, 0.555
  against 0.587 at 240; GL1 at 144 in a batch of 5 runs 0.789 against
  0.788, at 60 1.103 against 1.105). At 60 and 144 the tail is longer, the
  90th percentile up to 0.16 ms and the 99th up to 0.25 (GL1 in the yard,
  5 runs: 1.638 against 1.450 ms at 144), all of it after `BeginFrame` (the
  renderer, cgame and the driver; from the input read to `BeginFrame` the
  same). It comes from the frame rate, not the wait: the new limiter at
  the frame rates the old one gave (caps 62 and 167: 62.0 and 166.97 frames
  per second, 4 runs) has the old tails within the run to run spread (99th
  percentile, GL1 at 167: 1.425 against 1.438 ms, Vulkan 1.229 against
  1.173, its runs 1.162 to 1.264 against 1.146 to 1.225; at 62: 2.346
  against 2.237 and 1.991 against 1.925, the runs 2.081 to 2.474 against
  2.135 to 2.337 and 1.871 to 2.028 against 1.756 to 1.960) and lower
  medians, and the same binary at 144 the longer GL1 tail (1.589). How a
  frame rate near a multiple of the display's (59.96 Hz here, frames shown
  by the compositor) lands against its refreshes is the likely cause; a VRR
  display, where caps are used most, wasn't available to test.
- CPU: the polled last millisecond costs CPU at every cap below 1000, 0.4
  to 0.7 ms of a core more per frame: Vulkan in the yard 0.11 to 0.13 CPUs at
  60, 0.19 to 0.26 at 144, 0.26 to 0.41 at 240, 0.34 to 0.55 at 333 and
  0.50 to 0.78 at 500; uncapped and at 1000, the same as before.
- Uncapped and at 1000 the frame rates are the old ones within the run to
  run spread (GL1 in the demo 903.9 and 912.2 frames per second with the
  new binary at 1000 and 0, the same code, the old one 912.8 and 910.8).
  The new limiter doesn't start a frame before `Sys_Milliseconds` has
  ticked since the last one, where the old `bias` let a frame that
  followed a late one start at once, with 0 ms of game time when the
  millisecond hadn't changed (`Com_ModifyMsec` made it 1 ms).
- The deterministic views (`run.sh`: `com_maxfps 60`, `fixedtime 16`) give
  the same frames to the bit with both binaries: `dm/mohdm1`,
  `obj/obj_team2`, `mohdm1_dlight`, `menus.txt` and `mp_mohdm1` with Vulkan
  and `dm/mohdm1` with GL1 (33 views; `MOH_BIN=openmohaa_before run.sh`).
  A dedicated server (`omohaaded`, `dm/mohdm1`) runs 20.00 and 40.00 frames
  a second at `sv_fps` 20 and 40 with both binaries, its intervals 49.82 to
  50.16 ms instead of 49.36 to 50.64.

Caps that divide 1000 (2026-10-17, a second change): the deadline in
microseconds started frames wherever in the millisecond it happened to be
(set at startup and again after every frame late by more than a period),
but the game time of a frame is whole milliseconds: `com_frameTime -
lastTime`, `com_frameTime` read from `Sys_Milliseconds` in `Com_EventLoop`
right after `IN_Frame`. From the wait's end to that read takes 7 to 14 µs
(median; 99th percentile 11 to 47 µs, a tail of up to 0.9 ms). When the
deadline sat that close before a tick, some reads fell in the next
millisecond and frames alternated 3 and 5 ms at a cap of 250 instead of
4. Every frame is a usercmd, a step of the player's movement (pmove:
`cmd.serverTime` minus the previous one). The old limiter started frames
on the tick. Measured with a temporary build (not committed) that logged
every frame's msec before `Com_ModifyMsec`, without `fixedtime`, 1920x1080,
Vulkan: a local game (`dm/mohdm1`, free for all with 8 bots, the view
turning) and the `o2match` demo, 5 runs of 60 s per cap and binary, every
run a new deadline phase. The frames that started on time but whose
`com_frameTime` fell in another millisecond than one period after the
previous frame's, and in parentheses the frames late by more than half a
millisecond and their catch-up (bursts of 6 to 8 ms frames, one per
binary in the demo at different moments: the machine, not the limiter):

| | frames per binary | old limiter | 413c50d1 | now |
| --- | --- | --- | --- | --- |
| local game, cap 125 | 37490 | 0 (0) | 0 (4) | 0 (0) |
| local game, cap 250 | 74990 | 0 (6) | 0 (18) | 0 (2) |
| local game, cap 500 | 149990 | 0 (9) | 36, in 4 of 5 runs (32) | 0 (6) |
| `o2match`, cap 125 | 37490 | 0 (0) | 4, in 1 run (28) | 0 (0) |
| `o2match`, cap 250 | 74990 | 0 (0) | 0 (0) | 0 (43) |
| `o2match`, cap 500 | 149990 | 0 (58) | 18, in 1 run (0) | 0 (0) |

The runs of 413c50d1 with such frames had their deadline 794 to 963 µs
after the tick. None of its ten runs at 250 drew a phase close enough;
with the deadline moved to a chosen phase (the same build, the local game,
cap 250, 20 s each, frames of 3 or 5 ms out of 5000):

| deadline after the tick (µs) | 500 | 900 | 950 | 970 | 980 | 984 | 986 | 988 | 990 | 992 | 994 | 997 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| frames of 3 or 5 ms | 0 | 18 | 18 | 4 | 26 | 130 | 242 | 554 | 2533 | 442 | 0 | 0 |

With the phase drawn by chance, about one start (or frame late by more
than a period) in a hundred got a phase where 3 to 51 % of the frames
were 3 and 5 ms, and about one in ten one with a pair every few seconds.
The old limiter also let a frame after a late one last 0 ms (`bias`; 7
such frames in its runs at 500).

Now a cap that divides 1000 (`Com_Frame`: 125, 200, 250, 500 and the
lower ones, also for `com_maxfpsUnfocused` and `com_maxfpsMinimized`)
keeps the deadline in whole milliseconds of `Sys_Milliseconds`: it
advances by 1000 / cap, the same rule starts it again after a frame late
by more than a period, and the frame starts when that clock ticks
(`minMsec` up to the deadline).
Caps that don't divide 1000 keep the deadline in microseconds; their 6 and
7 ms frames at 144 are what the cap asks for. The wait (`Com_FrameWait`)
still sleeps until 1 ms before the frame and polls the rest: it knows when
`Sys_Milliseconds` (`gettimeofday`) will tick from the last tick it saw
while polling, since `Sys_Microseconds` (`CLOCK_MONOTONIC`) runs at the
same rate and the tick is whole milliseconds later. If the millisecond
clock disagrees by more than 50 µs (the clock was set, a suspend), it
waits once like the old limiter, asleep for the whole milliseconds but one
and polling the last, and sees a tick again. Not on Windows, where
`timeGetTime` follows the timer interrupts: there it always waits the old
way. That old way alone, the first version measured, starts frames on the
tick too but polls 0 to 1 ms instead of 1 ms, and like the old limiter it
lengthened the input to present time by 0.02 to 0.05 ms at every cap with
both renderers (the core clocks down, see above).

Results (idle machine, other processes 1.4 CPUs at most; `perf.py
--binaries <old>,<413c50d1>,<the old way alone>,<this build> --caps
125,250,500 --seconds 10 --preload libinlat.so --framelog DIR`, 1920x1080,
medians of 3 interleaved runs; input to present and frame starts as in the
first results, in ms; CPU of the game process):

| view, cap | renderer | input to present, median: old, 413c50d1, old way alone, now | input to present, p99: 413c50d1, now | frame starts p99: old, 413c50d1, now | CPU: old, 413c50d1, old way alone, now |
| --- | --- | --- | --- | --- | --- |
| `o2match`, 125 | GL1 | 0.904, 0.862, 0.884, 0.883 | 2.076, 2.035 | 8.059, 8.003, 8.003 | 0.27, 0.33, 0.27, 0.33 |
| | Vulkan | 0.742, 0.680, 0.724, 0.670 | 1.425, 1.346 | 8.053, 8.003, 8.003 | 0.16, 0.24, 0.17, 0.24 |
| `o2match`, 250 | GL1 | 0.777, 0.726, 0.788, 0.751 | 1.447, 1.584 | 4.054, 4.002, 4.002 | 0.46, 0.59, 0.47, 0.60 |
| | Vulkan | 0.634, 0.575, 0.615, 0.589 | 1.161, 1.088 | 4.040, 4.002, 4.003 | 0.27, 0.44, 0.28, 0.44 |
| `o2match`, 500 | GL1 | 0.670, 0.666, 0.662, 0.657 | 1.599, 1.619 | 2.053, 2.002, 2.002 | 0.79, 1.09, 0.81, 1.09 |
| | Vulkan | 0.511, 0.488, 0.509, 0.497 | 0.963, 1.006 | 2.013, 2.001, 2.001 | 0.52, 0.81, 0.54, 0.82 |
| `obj_team2` yard, 125 | GL1 | 0.860, 0.829, 0.854, 0.831 | 1.723, 1.557 | 8.063, 8.003, 8.003 | 0.25, 0.31, 0.25, 0.30 |
| | Vulkan | 0.720, 0.692, 0.726, 0.682 | 1.390, 1.292 | 8.050, 8.003, 8.003 | 0.15, 0.24, 0.16, 0.24 |
| `obj_team2` yard, 250 | GL1 | 0.686, 0.647, 0.694, 0.643 | 1.116, 1.035 | 4.053, 4.002, 4.002 | 0.36, 0.54, 0.37, 0.53 |
| | Vulkan | 0.591, 0.555, 0.594, 0.547 | 0.950, 0.947 | 4.019, 4.002, 4.002 | 0.26, 0.43, 0.27, 0.42 |
| `obj_team2` yard, 500 | GL1 | 0.552, 0.522, 0.548, 0.537 | 0.867, 0.903 | 2.020, 2.002, 2.002 | 0.62, 0.97, 0.64, 0.98 |
| | Vulkan | 0.465, 0.454, 0.457, 0.435 | 0.723, 0.775 | 2.002, 2.002, 2.001 | 0.51, 0.78, 0.52, 0.78 |

- Every frame at a cap that divides 1000 is now 1000 / cap ms of game
  time but for the frames late by more than a period and their catch-up,
  as with the old limiter (the table above); the frames start on the tick
  plus 7 to 11 µs (median) to the read.
- Input to present: the medians are within 0.025 ms of 413c50d1's either
  way and shorter than the old limiter's everywhere (by 0.013 to 0.072
  ms); a second batch of 5 runs of `o2match` gave 0.880 against 0.891 ms
  (GL1, 125), 0.744 against 0.736 (GL1, 250), 0.694 against 0.683
  (Vulkan, 125) and 0.578 against 0.568 (Vulkan, 250), the runs
  overlapping. In `o2match` at 250 the medians were 0.008 to 0.025 ms
  higher in both batches, in the yard with Vulkan 0.008 to 0.019 lower.
  The 99th percentile: in the yard the same or lower; in `o2match` with
  Vulkan about the same (1.088 against 1.066 ms at 250, the
  median of 8 runs each, 1.042 to 1.526 against 1.008 to 1.225; 0.971 to
  1.044 against 0.955 to 0.972 at 500), with GL1 about 0.08 ms higher at
  250 in both batches (1.513 to 1.636 in 8 runs against 1.412 to 1.544),
  where the old limiter and the old way alone, which also start frames on
  the tick, gave 1.591 to 1.903 and 1.562 to 1.659. Not explained: the
  phase of the frames against the kernel's 1000 Hz timer tick is fixed
  when they start on the wall clock's millisecond and drawn by chance with
  a deadline in microseconds (not tested).
- Frame starts and CPU: those of 413c50d1 (the polled millisecond is
  kept).
- The usercmd's step also moves with the client's clock adjustment, which
  this doesn't change: on every snapshot `CL_AdjustTimeDelta` moves
  `cl.serverTimeDelta` by 1 ms, or -2 ms after extrapolated frames
  (Quake 3's code), so in the local game 6000 of the 74990 usercmds at 250
  are 5 or 2 ms with every binary (none in demos, which don't adjust).
- The deterministic views give the same frames to the bit with
  413c50d1 and with this build, at `run.sh`'s cap of 60 and at 50, which
  divides 1000: `dm/mohdm1`, `obj/obj_team2`, `mohdm1_dlight`,
  `menus.txt` and `mp_mohdm1` with Vulkan and `dm/mohdm1` with GL1 (66
  comparisons). A dedicated server runs 20.00 and 40.00 frames a second at
  `sv_fps` 20 and 40 as with 413c50d1 (its wait doesn't use the tick).

Present timing (2026-10-17): everything measured above ends at the
present call. What happens after it, until the frame is on the screen,
was measured on this machine's path: KWin 6.6.6 (Plasma Wayland session,
atomic modesetting, `allowTearing` on, its default), Xwayland 24.1.10,
RADV (Mesa 26.0.8), SDL 2.32.10, the display 1920x1080 on HDMI at 59.975
Hz (the mean interval of the kernel's vblank timestamps, 16.6735 ms), no
VRR. The game runs under Xwayland (SDL's default video driver, x11) and,
with `SDL_VIDEODRIVER=wayland`, as a native Wayland client: this build
works both ways. (SDL 2 then; since Plan 35 the game uses SDL 3, whose
default here is native Wayland, `SDL_VIDEODRIVER=x11` for Xwayland: see
SDL 3 below.) A 240 Hz display isn't available here, so these numbers
show the mechanisms (what the compositor costs, what each present mode
queues, latency against the cap), not a 240 Hz player's numbers.

What a client is told (`vulkaninfo`; read in the sources: Mesa's
`src/vulkan/wsi/wsi_common_x11.c`, `wsi_common_wayland.c`, `wsi_common.c`
and `src/loader/loader_wayland_helper.c` at `mesa-26.0.8`, Xwayland's
`hw/xwayland/xwayland-present.c` at `xwayland-24.1.10`, KWin's
`src/compositor.cpp`, `src/core/renderloop.cpp`,
`src/backends/drm/drm_commit_thread.cpp`, `drm_commit.cpp` and
`src/wayland/presentationtime.cpp` at `v6.6.6`):

- RADV has `VK_KHR_present_id` and `VK_KHR_present_wait` (and their `2`
  versions) on X11 (Xwayland) and Wayland surfaces; not
  `VK_EXT_present_timing` or `VK_GOOGLE_display_timing`.
- X11: `vkWaitForPresentKHR` returns once the frame's GPU work is done (a
  timeline semaphore of the WSI) and Xwayland's `PresentCompleteNotify`
  for it has arrived. Xwayland hands the game's buffer to KWin (mode
  "flip", in a window too) and completes an asynchronous present
  (immediate; mailbox, which Mesa sends to Xwayland as asynchronous too;
  FIFO relaxed) once a `wl_display.sync` round trip shows KWin has the
  commit, with the UST of the last frame callback; a FIFO present at the
  next frame callback, which KWin sends at the end of the composite that
  latched the frame, before its page flip. Neither is the time the frame
  is shown.
- Wayland: Mesa asks for presentation feedback (`wp_presentation`) for the
  presents that carry a present id; `vkWaitForPresentKHR` returns when it
  says "presented" or "discarded". KWin gives the kernel's page flip
  timestamp (flags hardware clock, hardware completion, vsync), never
  "zero copy" (direct scanout isn't reported), and for a tearing flip the
  timestamp of the vblank before it.
- So only a native Wayland client is told when its frame was shown, and
  not with tearing. What an unprivileged process can see besides: the
  kernel's vblank timestamps (`DRM_IOCTL_WAIT_VBLANK` on `/dev/dri/card1`
  needs no DRM master), which framebuffer the primary plane scans out
  (`GETPLANE`, `GETFB2`: KWin composites into AR30 buffers, the game's
  swapchain images are XR24, so one of them there is direct scanout, the
  compositor bypassed) and when that changes (KWin's commit; with tearing,
  the flip), and when the GPU finished each frame.
- Calling `vkWaitForPresentKHR` on a second thread while the game acquires
  and presents is invalid (the swapchain is externally synchronized for
  all three): a first version of the tool did, and with the Wayland WSI
  the heap got corrupted.

The tool: `libinlat.so` with `INLAT_DISPLAY=1` (nothing of it runs
otherwise; no renderer change) records, besides each frame's input read
and present call, the X11 completion events (`xcb_wait_for_special_event`
wrapped in Mesa's event thread), the Wayland presentation feedback (it
enables `VK_KHR_present_id`, numbers every present and wraps the
feedback's listener), when the GPU finished each submission (a timeline
semaphore of its own that every `vkQueueSubmit` also signals, waited for
by a thread), every vblank with the primary plane's framebuffer and its
format, the framebuffer changes (polled every 250 µs with
`INLAT_FBPOLL_US=250`, for the tearing variants), and whether the SDL
window is fullscreen. `INLAT_PRESENT_MODE=mailbox` forces mailbox (the
renderer asks for immediate with `r_swapInterval 0`, FIFO with `1`; since
Plan 34 `r_fastSync 1` asks for mailbox).
`framelog.py --display DIR` (`--table`, `--json`) works out when each
frame was shown, the start of the scanout that shows it or the tearing
flip:

- Wayland without tearing: the feedback's time, exact.
- X11 FIFO: the vblank after the completion (the frame callback).
- Tearing (Wayland: no vsync flag; X11: more frame callbacks than
  refreshes, or KWin's rule, a fullscreen window presenting with
  `PresentOptionAsyncMayTear`, which Mesa's immediate mode sets): each
  framebuffer change shows the newest frame ready before it (KWin
  composites as soon as a frame is ready and its commit thread flips at
  once).
- X11 asynchronous without tearing: KWin latches, when a composite starts,
  the newest commit whose buffer is ready, and sends frame callbacks at
  its end; a present completed with a UST older than a frame callback
  reached KWin before that composite. When the composite started isn't
  reported: taking its frame callback gives a lower bound, 1.5 ms before
  it an upper one ("to" in the table).

Checks: with direct scanout, the frames the primary plane showed (for the
best of the 24 ways to assign the 4 framebuffers to the swapchain images)
are the ones the rules put there at 95 to 100 % of the vblanks, for
Xwayland (mailbox: the lower bound; FIFO) and Wayland alike (78 % in one
Wayland FIFO run at 60 that was composited at 16 % of its vblanks); with
Wayland's tearing, the frames found are those KWin reported presented;
in a window, Wayland's exact times and Xwayland's lower bound agree within
0.35 ms uncapped and at 250 (at 59 the lower bound is 1.1 to 1.8 ms
under). The probes cost nothing measurable: the input to present medians
with them and without (`INLAT_DISPLAY=0`) within 0.025 ms.

Results (idle machine: other processes 1.9 CPUs at most; Vulkan,
1920x1080; `perf.py --list ... --binaries <variant wrappers> --caps
0,59,250 --seconds 10 --runs 3 --preload libinlat.so --framelog DIR` with
`INLAT_DISPLAY=1`, FIFO with `--frames 600`, cap 60 one run of 40 s: a cap
of 60 drifts against the 59.975 Hz display by one refresh in 40 s, so a
shorter run sees one phase; then `framelog.py --display DIR --table`). A
variant is a wrapper script in the binary directory, for example
`SDL_VIDEODRIVER=wayland INLAT_FBPOLL_US=250 exec ./openmohaa "$@" +set
r_fullscreen 1 +set in_nograb 0` (fullscreen needs `in_nograb 0`: with
`in_nograb 1`, perf.py's default, the first frame turns it off again).
Input to present: the median of the runs' medians; input to display: the
frames shown in all runs pooled (about 600 per 10 s run at 60 Hz, 2400 in
the 40 s run), ms; shown: the share of the presented frames that reached
the display; slack: the median time a finished frame waited for the
compositor's latch (its ready-to-display time above its run's 1st
percentile), the most a later frame start could take off:

| `com_maxfps` | variant | `o2match`: input to present, median | input to display: median, p90, p99, p99.9 | shown | slack | yard: input to present, median | input to display: median, p90, p99, p99.9 | slack |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | Xwayland, window, immediate | 0.514 | 7.29, 7.81, 8.22, 10.01 (to 8.41) | 6 % | 1.36 | 0.431 | 6.61, 7.48, 7.77, 7.86 (to 7.71) | 0.97 |
| 0 | Wayland, window, immediate | 0.516 | 7.33, 7.90, 8.89, 9.36 | 6 % | 1.35 | 0.430 | 6.94, 7.58, 7.78, 8.12 | 1.10 |
| 0 | Xwayland, fullscreen, mailbox | 0.516 | 4.63, 5.67, 6.21, 13.85 (to 6.33) | 6 % | 0.92 | 0.440 | 5.44, 6.73, 7.66, 7.76 (to 7.25) | 1.01 |
| 0 | Wayland, fullscreen, mailbox | 0.520 | 4.77, 5.71, 6.61, 12.32 | 6 % | 1.14 | 0.438 | 5.69, 7.06, 7.68, 7.90 | 0.99 |
| 0 | Xwayland, fullscreen, immediate | 0.532 | 1.57, 2.19, 3.09, 3.46 | 96 % | 0.40 | 0.465 | 1.52, 1.76, 2.01, 2.42 | 0.45 |
| 0 | Wayland, fullscreen, immediate | 0.541 | 1.56, 2.19, 3.12, 3.57 | 96 % | 0.39 | 0.473 | 1.52, 1.76, 2.01, 2.85 | 0.45 |
| 0 | Xwayland, window, FIFO | 1.008 | 70.97, 71.16, 71.79, 72.13 | 100 % | 2.41 | 0.941 | 71.00, 71.23, 72.11, 72.25 | 1.75 |
| 0 | Wayland, window, FIFO | 1.070 | 71.27, 72.23, 72.38, 72.44 | 100 % | 2.15 | 1.010 | 71.22, 72.09, 72.35, 72.43 | 1.54 |
| 0 | Xwayland, fullscreen, FIFO | 1.013 | 50.50, 50.53, 50.55, 50.56 | 100 % | 1.58 | 0.951 | 50.49, 50.52, 50.54, 50.56 | 1.41 |
| 0 | Wayland, fullscreen, FIFO | 1.011 | 50.49, 50.53, 50.55, 50.56 | 100 % | 1.63 | 0.947 | 50.49, 50.52, 50.55, 50.56 | 1.52 |
| 250 | Xwayland, window, immediate | 0.589 | 8.60, 10.34, 10.94, 11.17 (to 9.54) | 24 % | 2.25 | 0.561 | 8.59, 10.36, 10.90, 11.54 (to 9.57) | 2.35 |
| 250 | Wayland, window, immediate | 0.579 | 8.94, 10.61, 11.61, 12.03 | 24 % | 2.55 | 0.558 | 8.66, 10.43, 11.07, 11.82 | 2.52 |
| 250 | Xwayland, fullscreen, mailbox | 0.576 | 6.80, 8.50, 9.04, 10.11 (to 8.36) | 24 % | 2.66 | 0.551 | 7.49, 9.34, 10.74, 11.40 (to 9.24) | 2.48 |
| 250 | Wayland, fullscreen, mailbox | 0.575 | 6.74, 8.48, 9.38, 10.17 | 24 % | 2.60 | 0.562 | 7.45, 9.26, 10.52, 11.14 | 2.50 |
| 250 | Xwayland, fullscreen, immediate | 0.580 | 2.04, 2.53, 3.03, 3.42 | 100 % | 0.16 | 0.563 | 2.10, 2.35, 2.65, 2.90 | 0.17 |
| 250 | Wayland, fullscreen, immediate | 0.580 | 2.10, 2.42, 2.89, 3.36 | 100 % | 0.17 | 0.565 | 2.17, 2.42, 2.69, 3.17 | 0.16 |
| 59 | Xwayland, window, immediate | 0.996 | 11.95, 18.60, 21.07, 22.35 (to 16.02) | 100 % | 8.31 | 0.955 | 11.30, 18.09, 19.71, 20.68 (to 16.27) | 7.94 |
| 59 | Wayland, window, immediate | 1.023 | 13.08, 19.61, 21.34, 21.93 | 100 % | 8.15 | 0.964 | 13.13, 19.64, 21.14, 21.65 | 8.21 |
| 59 | Xwayland, fullscreen, mailbox | 0.977 | 11.30, 17.99, 20.00, 21.74 (to 15.27) | 100 % | 8.32 | 0.916 | 11.25, 17.97, 20.28, 21.75 (to 15.47) | 7.89 |
| 59 | Wayland, fullscreen, mailbox | 0.985 | 11.36, 17.97, 19.82, 20.88 | 100 % | 8.09 | 0.925 | 11.36, 18.01, 19.75, 21.43 | 7.99 |
| 59 | Xwayland, fullscreen, immediate | 0.991 | 2.83, 3.50, 4.40, 10.49 | 100 % | 0.19 | 0.923 | 2.67, 3.34, 4.13, 4.84 | 0.18 |
| 59 | Wayland, fullscreen, immediate | 0.984 | 2.80, 3.48, 4.40, 10.49 | 100 % | 0.21 | 0.929 | 2.69, 3.47, 4.26, 4.92 | 0.19 |
| 59 | Xwayland, window, FIFO | 0.993 | 14.41, 22.78, 39.92, 44.34 | 100 % | 9.85 | 0.929 | 12.53, 19.22, 22.43, 23.71 | 8.66 |
| 59 | Wayland, window, FIFO | 1.013 | 14.02, 21.36, 35.93, 38.13 | 100 % | 8.95 | 0.946 | 13.11, 19.72, 21.28, 21.65 | 8.12 |
| 59 | Xwayland, fullscreen, FIFO | 0.974 | 12.53, 20.45, 36.37, 40.79 | 100 % | 9.41 | 0.941 | 11.05, 17.75, 19.31, 20.55 | 8.17 |
| 59 | Wayland, fullscreen, FIFO | 0.991 | 12.91, 22.26, 39.04, 43.02 | 100 % | 9.46 | 0.939 | 11.27, 17.92, 19.53, 19.90 | 8.03 |
| 60 | Xwayland, window, immediate | 0.856 | 15.68, 21.84, 23.32, 23.61 (to 17.02) | 98 % | 8.47 | 0.958 | 15.29, 21.52, 23.02, 23.16 (to 16.01) | 7.53 |
| 60 | Wayland, window, immediate | 0.885 | 13.84, 21.09, 22.50, 22.69 | 100 % | 7.65 | 0.951 | 14.89, 22.13, 23.54, 23.70 | 7.45 |
| 60 | Xwayland, fullscreen, mailbox | 0.822 | 11.87, 19.02, 20.53, 20.77 (to 15.60) | 98 % | 7.72 | 0.921 | 12.87, 19.12, 20.53, 20.79 (to 15.35) | 8.58 |
| 60 | Wayland, fullscreen, mailbox | 0.837 | 11.97, 19.21, 20.64, 20.77 | 100 % | 7.47 | 0.905 | 13.65, 19.91, 21.54, 21.68 | 9.13 |
| 60 | Xwayland, fullscreen, immediate | 0.836 | 2.56, 3.21, 3.99, 4.54 | 100 % | 0.18 | 0.915 | 2.68, 3.38, 4.16, 4.61 | 0.17 |
| 60 | Wayland, fullscreen, immediate | 0.834 | 2.54, 3.18, 3.93, 4.62 | 100 % | 0.20 | 0.917 | 2.61, 3.32, 4.14, 4.93 | 0.17 |
| 60 | Xwayland, window, FIFO | 0.870 | 43.66, 49.94, 51.35, 51.48 | 100 % | 7.76 | 0.942 | 43.03, 49.32, 50.73, 50.88 | 7.84 |
| 60 | Wayland, window, FIFO | 0.873 | 51.58, 57.86, 59.28, 59.42 | 100 % | 8.00 | 0.947 | 30.70, 36.97, 38.39, 38.53 | 7.59 |
| 60 | Xwayland, fullscreen, FIFO | 0.832 | 44.20, 50.36, 50.53, 50.55 | 100 % | 7.92 | 0.907 | 41.11, 47.38, 48.80, 48.94 | 7.77 |
| 60 | Wayland, fullscreen, FIFO | 0.829 | 32.31, 38.60, 40.01, 40.15 | 100 % | 7.88 | 0.986 | 41.18, 47.52, 48.92, 49.06 | 7.53 |

What the compositor costs. From the frame's GPU work done to the display
(the `obj_team2` yard, uncapped, medians; first percentile in
parentheses): 5.4 to 5.8 ms (4.6) composited in a window, 4.2 to 4.5 ms
(2.6 to 3.5) with direct scanout, 0.5 ms (0.06) tearing; input to present
is 0.43 to 0.47 ms and the GPU is done 0.6 to 0.8 ms later. KWin's frame
callbacks, sent at the end of its composite, reach Xwayland 3.9 ms (in a
window) and 3.3 ms (direct scanout) before the vblank on the median (it
starts a composite its render time estimate, a safety margin and 1 ms
before the vblank), and the scanout starts 0.6 ms after the vblank
does; a frame waits for the latch about half a frame period (the slack
column). So composition itself (a window against direct scanout) costs
about 1 to 1.5 ms; the vsync latch at least 2.6 to 3.5 ms more than
tearing, plus the wait for it.

- Direct scanout: KWin scans the game's swapchain image out in fullscreen
  with mailbox and FIFO, under Xwayland and Wayland alike (a game buffer
  on the primary plane at every vblank); never in a window, and never with
  tearing: it then composites every frame into its own buffer and flips
  that at once (its AR30 buffers on the primary plane).
- Xwayland costs nothing measurable: fullscreen it gives the native
  Wayland numbers within 0.4 ms in every mode at 0, 59 and 250 (the single
  runs at 60 differ by up to 0.8 ms, and with FIFO by how full the queue
  got); its completion comes 0.1 ms after the present call.
- The lowest input to display is fullscreen immediate (`r_fullscreen 1`,
  `r_swapInterval 0`, the default), with tearing: 1.5 ms uncapped, 2.0 to
  2.2 at 250, 2.5 to 2.8 at 59 and 60 (the frame's own CPU and GPU time,
  and KWin's 0.45 ms). Without tearing the lowest is fullscreen mailbox
  (direct scanout), 4.6 to 5.7 ms uncapped, 6.7 to 7.5 at 250, 11.3 to
  13.7 at 59 and 60, rendering frames that are never shown (6 % are
  uncapped, 24 % at 250). A window adds 1.1 to 2.7 ms to that at the high
  caps. In a window or with mailbox, a higher cap shortens the wait for
  the latch (half a frame period on average): 250 is 1.3 to 2.2 ms worse
  than uncapped there, 0.5 to 0.6 ms with tearing.
- FIFO queues: uncapped, three frames wait (four swapchain images) and
  input to display is 50.5 ms fullscreen, 71 ms in a window (a refresh
  more: the composite). Capped at 59, just below the refresh, the queue
  stays empty, 11 to 14 ms on the median; but frames queued once drain
  by only 0.28 ms a frame (59 against 59.975 Hz): in `o2match` the first
  35 frames measured came 25 to 35 ms after their input (frames queued at
  the demo's start), the 99th percentile 36 to 40 ms (19 to 22 in the
  yard). A slow frame that the limiter makes up would queue one the same
  way. At 60, above the refresh, the queue fills: 31 to 52 ms within 40 s.
- At caps near the refresh (59, 60) every vsync mode shows 100 % of the
  frames but each waits a random part of a refresh for the latch (slack
  7.5 to 9.9 ms).

Pacing changes that could lower input to display without adding lag
(none implemented: the frame limiter is left as it is in this step):

1. FIFO: keep at most one frame queued. Wait, before the next input read
   (after the present, where the acquire of the next image already
   blocks, or in `Com_Frame`'s wait), until the previous frame's present
   has completed (`vkWaitForPresentKHR` on the
   main thread is valid; on X11 it returns at KWin's composite, early
   enough). Measured upside: FIFO uncapped 50.5 and 71 ms against 11 to 14
   ms at a cap below the refresh, so 38 to 59 ms; it also removes what
   stays queued at 59. Latency effect: lower, never higher (it waits
   before the input is read; nothing is held after it). Fewer swapchain
   images with FIFO (3, the minimum) should take one refresh off without
   it (not measured).
2. Vsync modes (mailbox, a window, FIFO): start the frame just in time, so
   that its image is ready right before the compositor's latch. Measured
   upside: the slack, 0.9 to 1.4 ms uncapped, 2.3 to 2.7 ms at 250, 7.5
   to 9.9 ms at 59 and 60 (with FIFO on top of the queue). Latency effect:
   lower by up to the slack when the latch is predicted right, a whole
   refresh higher for a frame that misses it. It needs the latch time,
   which KWin doesn't report (3.3 to 3.9 ms before the vblank here, and it
   adapts: its safety margin grows after a late commit); the vblank grid
   is known (the kernel's timestamps, or Wayland's feedback). An engine
   change (when `Com_Frame` starts a frame), not a renderer one.
3. With tearing nothing is left to pace: the slack is 0.16 to 0.45 ms, and
   input to display is the frame's own CPU and GPU time plus KWin's 0.45
   ms; only less CPU or GPU time per frame lowers it. For the user's setup
   (250 on a 240 Hz display, no VRR) fullscreen immediate is the lowest,
   with tearing; on that display vsync through KWin should cost about what
   it costs here at 250, 4.6 to 5.4 ms over tearing (KWin's margins are
   times, not refreshes, and the frames come as often), and a full FIFO
   queue 12.5 ms (not measured).

`r_fastSync` (2026-10-24, Plan 34): mailbox instead of immediate with
`r_swapInterval 0`, measured on the same path (`o2match`, fullscreen,
`perf.py --list <o2match> --sizes 1920x1080 --binaries <variant wrappers>
--caps 0,250 --seconds 10 --runs 3 --preload libinlat.so --framelog DIR`
with `INLAT_DISPLAY=1`, the variants `x_off`, `x_on`, `w_off`, `w_on`
appending `+set r_fullscreen 1 +set in_nograb 0` and, for `on`, `+set
r_fastSync 1`, `w` with `SDL_VIDEODRIVER=wayland`; the acquire times from
the frame log, `--vklog`, in runs of their own). Frame time: the medians of
the runs' percentiles, ms:

| `com_maxfps` | variant | input to present: median, p99 | input to display: median, p90, p99, p99.9 (pooled) | shown | frame time: p99, p99.9 | frames a second |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | Xwayland, `r_fastSync 0` (immediate, tearing) | 0.594, 1.309 | 1.51, 2.22, 3.00, 3.37 | 92 % | 1.354, 1.751 | 1441 |
| 0 | Xwayland, `r_fastSync 1` (mailbox) | 0.526, 1.139 | 5.54, 7.20, 8.41, 8.79 (to 7.20) | 4 % | 1.675, 2.097 | 1406 |
| 0 | Wayland, `r_fastSync 0` | 0.591, 1.319 | 1.48, 2.25, 3.08, 3.52 | 92 % | 1.372, 1.766 | 1415 |
| 0 | Wayland, `r_fastSync 1` | 0.539, 1.176 | 6.53, 8.06, 9.67, 16.48 | 4 % | 1.775, 2.175 | 1342 |
| 250 | Xwayland, `r_fastSync 0` | 0.483, 0.893 | 1.91, 2.37, 2.74, 3.38 | 99 % | 4.033, 4.165 | 250 |
| 250 | Xwayland, `r_fastSync 1` | 0.504, 0.911 | 6.56, 8.35, 9.13, 9.79 (to 8.28) | 24 % | 4.032, 4.193 | 250 |
| 250 | Wayland, `r_fastSync 0` | 0.482, 0.866 | 1.89, 2.29, 2.73, 3.04 | 100 % | 4.032, 4.155 | 250 |
| 250 | Wayland, `r_fastSync 1` | 0.498, 0.895 | 7.97, 10.16, 11.83, 25.49 | 24 % | 4.034, 4.198 | 250 |

- No tearing costs the latch: 4.0 to 5.0 ms more input to display
  uncapped and 4.6 to 6.1 ms at 250, at this 60 Hz display, about what
  the mailbox runs above showed (KWin's margin moves from run to run: the
  medians of single Xwayland runs differed by up to 1.5 ms; the Wayland
  p99.9 at 250 is one refresh of a 25 ms frame). A faster display waits
  less, but KWin's margins are times, not refreshes (see above).
- With mailbox the acquire after the present waits: KWin holds the image
  it shows, the one it took for the next refresh and the newest presented,
  so with 4 images the next acquire waits until KWin releases the image
  that the newest present replaced. More than 0.3 ms in 8 to 17 % of the
  frames uncapped (99th percentile 0.72 to 0.90 ms) and in 5 to 6 % at 250
  (1.0 to 1.1 ms; one Xwayland run at 250: 0.4 %); with immediate never
  (99.9th percentile 0.07 ms). Uncapped that is 2 to 5 % fewer frames a
  second and 0.3 to 0.4 ms more at the frame time's 99th percentile; at
  250 the limiter's wait takes it in.
- The stall the pull request was written for, immediate's acquire taking
  0.2 ms a frame in fullscreen on Wayland with NVIDIA (its author's
  report), doesn't happen with RADV: the acquire after an immediate
  present takes 0.03 ms on the median, 0.04 at the 99th percentile.
- A fifth swapchain image for mailbox (as the submit thread has) was tried
  and left out (2 x 3 runs, then 2 x 5 on native Wayland): the acquire
  never waited (99th percentile 0.05 to 0.12 ms), uncapped 14 % more
  frames a second (1359 to 1547) and the frame time's 99th percentile
  1.76 to 1.23 ms; at 250 the same input to display (8.15 and 8.16 ms on
  the median, 12.00 and 11.99 at the 99th percentile; Xwayland 7.96 to
  7.74), but on native Wayland 0.15 ms more on the median uncapped and,
  at a cap of 1000 with the same frame rate, 0.24 ms more (6.17 to 6.41;
  99th percentile 8.94 to 9.62). Not traced further: KWin scanned out the
  same 5 to 6 framebuffers, nothing imported again.

SDL 3 (2026-10-24, Plan 35): the build before (9d872203, SDL 2.32.10, its
default video driver x11, so Xwayland) against the SDL 3.4.18 build (its
default here, native Wayland), fullscreen immediate (tearing), the
defaults' path for a player, in `o2match` at 1920x1080 (`perf.py ...
--binaries b_x,n_w --caps 0,250 --seconds 10 --preload libinlat.so
--framelog DIR` with `INLAT_DISPLAY=1`; `b_x` a wrapper that runs the old
build from its own bin dir, `INLAT_FBPOLL_US=250 exec <old bin
dir>/openmohaa "$@" +set r_fullscreen 1 +set in_nograb 0`), three batches
on the idle machine, interleaved runs: 15 runs each uncapped, 25 at 250.
The medians of the runs' percentiles, ms (pooled over all frames within
0.02 of these, but at the 99.9th percentile at 250, where one run of each
build held a burst of 4 to 9 ms frames, the machine's; without those two
runs the pooled 99.9th percentiles are 1.19 and 1.20 ms input to present,
3.48 and 3.22 ms to display):

| `com_maxfps` | build | input to present: median, p90, p99, p99.9 | input to display: median, p90, p99, p99.9 |
| --- | --- | --- | --- |
| 0 | SDL 2, Xwayland | 0.586, 0.933, 1.297, 1.684 | 1.534, 2.233, 3.080, 3.401 |
| 0 | SDL 3, Wayland | 0.585, 0.930, 1.287, 1.695 | 1.510, 2.213, 3.048, 3.391 |
| 250 | SDL 2, Xwayland | 0.483, 0.658, 0.885, 1.132 | 1.905, 2.314, 2.727, 3.428 |
| 250 | SDL 3, Wayland | 0.488, 0.662, 0.878, 1.154 | 1.909, 2.325, 2.745, 3.060 |

- Uncapped, input to display is lower at every percentile (by 0.01 to
  0.03 ms; a bootstrap over the runs puts the median's difference at
  -0.028 to -0.017 ms), input to present the same within 0.01 ms.
- At 250 input to present is 0.005 ms longer on the median (95 % interval
  of the pooled difference +0.001 to +0.010 ms), +0.004, -0.007 and +0.022
  ms at the 90th, 99th and 99.9th percentiles (intervals around 0); input
  to display +0.004 to +0.018 ms up to the 99th percentile (intervals
  around 0) and 0.37 ms lower at the 99.9th. The 5 µs is where the input is read, not time
  added after it: libinlat counts from the first `SDL_PollEvent`, and SDL
  3's pump (timed per frame with a preload around `SDL_PollEvent` and the
  system calls on the display socket) spends 13.6 to 14.4 µs a frame
  against SDL 2's 7.7 to 8.4 µs, but 10.5 to 11.6 µs of it before it
  checks the Wayland socket (its D-Bus connection's check and the flush
  of the frame callback it asks for every frame; SDL 2 under Xwayland 4.5
  to 4.6 µs) and 3.9 µs after (SDL 2 3.7 to 3.8; 99th percentile 4.7 to
  5.1 against 5.7 to 6.3). The input is read 6 µs later, and the time
  from that read to the present is the same. SDL 3 under Xwayland pumps
  for 10.4 to 12.0 µs (its D-Bus check), SDL 2 as a native Wayland client
  for 12.9 to 14.1.
- Before the read: motion injected through uinput (`ydotoold`'s socket,
  bursts of 2000 moves at 1 and 8 kHz, in the main menu) reached the
  game's event loop 0.22 to 0.24 ms after it was sent on the median
  uncapped as a native Wayland client, 0.26 to 0.28 ms under Xwayland
  (the 99th percentile 0.39 to 0.44 against 0.43 to 0.46; part of it is
  the Wayland build's higher frame rate in the menu, 2600 against 1900 to
  2500 frames a second); at 250, where the wait for the next frame's read
  (0 to 4 ms) is most of it, 2.10 to 2.13 against 2.08 to 2.15 ms (8
  kHz).
- SDL 3's pump never took more than 87 µs in 10 runs at 250 (SDL 2's
  went to 3.6 ms once, in the run with the machine's burst).

SDL 3's event pump (2026-10-24, Plan 36): where the 13.6 to 14.4 µs of the
frame's first `SDL_PollEvent` went, timed inside a copy of SDL 3.4.18 with
timestamps in `SDL_PumpEventsInternal` and `Wayland_PumpEvents` (medians at
250, `o2match` fullscreen, native Wayland, from the pump's start): 0.6 µs
SDL's housekeeping, 3.1 to 3.4 µs its D-Bus connection's check
(`dbus_connection_read_write` with timeout 0 and a dispatch; SDL 2 had
none), 0.4 µs input method and decoration checks, 2.2 to 2.7 µs dispatching
what SDL's `wl_cursor_surface` thread had read from the socket (it waits
on the socket and reads every event as it comes), 3.3 to 3.5 µs
`wl_display_flush` sending SDL's answer to KWin's frame callback (damage
and a new callback; `strace -k`: one `sendmsg` from the pump and one from
the present per frame), 1.1 µs the poll of the socket (the read, nothing
left to read mostly), then 1.4 µs `SDL_UDEV_Poll` and 2.4 µs
`SDL_UpdateJoysticks` (the joystick subsystem was up with `in_joystick 0`)
and 0.4 µs the sentinel. KWin sends a frame callback for every tearing
flip, 0.82, 1.0 and 1.3 ms after the present (10th, 50th and 90th
percentiles of the cursor thread's reads); `kwin_wayland` runs with a
real-time priority here, and in some runs the pump's `sendmsg` cost 25 µs
in more than 10 % of the frames (the compositor woken onto this core). The
pump per frame (`pollcost.so`, the first `SDL_PollEvent`, two runs each),
µs, median, 90th and 99th percentile:

| build | the frame's pump at 250 |
| --- | --- |
| SDL 2, Xwayland | 7.8 to 8.2, 9.1 to 9.9, 14 to 24 |
| SDL 3 (Plan 35) | 14.1 to 14.6, 16 to 35, 25 to 51 |
| SDL 3, `in_joystick 0` shuts the subsystem down | 11.4 to 12.1, 14 to 38, 42 to 45 |
| the same and SDL's D-Bus check every 100 ms (a patch of SDL, not taken) | 8.9 to 9.4, 13, 18 to 35 |
| SDL 3, joystick and the limiter's pump (Plan 36) | 3.1 to 3.2, 3.5 to 3.6, 8 to 10 |
| the same and the D-Bus patch (not taken) | 1.95, 2.26, 7.4 to 7.9 |

Input to present and display with the same tools as the SDL 3 table above
(`perf.py ... --binaries b_x,n_w,jw_w`, interleaved, three batches: 45 runs
each of SDL 2 and Plan 36 at 250, 25 of Plan 35, 15 each uncapped; medians
of the runs' percentiles, ms):

| `com_maxfps` | build | input to present: median, p90, p99, p99.9 | input to display: median, p90, p99, p99.9 |
| --- | --- | --- | --- |
| 0 | SDL 2, Xwayland | 0.590, 0.940, 1.322, 1.718 | 1.517, 2.222, 3.039, 3.376 |
| 0 | SDL 3 (Plan 35) | 0.589, 0.947, 1.307, 1.723 | 1.509, 2.230, 3.056, 3.417 |
| 0 | SDL 3 (Plan 36) | 0.588, 0.943, 1.309, 1.697 | 1.510, 2.220, 3.054, 3.400 |
| 250 | SDL 2, Xwayland | 0.485, 0.660, 0.875, 1.121 | 1.915, 2.322, 2.720, 3.296 |
| 250 | SDL 3 (Plan 35) | 0.493, 0.667, 0.879, 1.136 | 1.909, 2.300, 2.709, 3.141 |
| 250 | SDL 3 (Plan 36) | 0.479, 0.651, 0.856, 1.127 | 1.893, 2.318, 2.736, 3.089 |

Pooled over all frames with a bootstrap over the runs, without the runs
that held a burst of the machine (a frame interval over 4.5 ms at 250: 8 of
SDL 2's 45 runs, 3 of Plan 36's), Plan 36 against SDL 2 at 250: input to
present -0.007 ms on the median (95 % interval -0.009 to -0.004), -0.009
at the 90th percentile (-0.015 to -0.005), -0.006 at the 99th (-0.024 to
+0.011), and at the 99.9th +0.110 with an interval from -0.025 to +0.344
(the pooled 99.9th percentile follows single runs: one of Plan 36's held
16 frames over 1.5 ms within 0.1 s, at no particular moment of the demo,
without a late frame); input to display within intervals around 0.
Against Plan 35 at 250: -0.014 ms on the median (-0.018 to -0.010), -0.015
at the 90th percentile (-0.021 to -0.009). Uncapped against SDL 2: input
to present -0.006 on the median (-0.011 to 0.000), to display -0.010
(-0.020 to -0.002), the other percentiles within intervals around 0.
Motion injected in the fullscreen main menu (10 bursts of 2000 moves at 1
and 8 kHz per build, at 250 and uncapped): every sum exact with Plan 35
and Plan 36, no fractions, the input to read the same (8 kHz at 250: 2.04
to 2.10 ms against 2.03 to 2.17 on the median, the wait for the next
frame; uncapped 0.24 ms both).

World culling (2026-10-17): the world's traversal (`R_RecursiveWorldNode`,
`R_AddWorldSurfaces`) led the CPU causes of `o4match`'s slow frames (Plan,
Remaining, item 3 before this step). The starting point was the review, in
the Metal port's hand-over, of the "exact world culls" of JayRewd's omaha
commit 1c2c8754 (a flat list of the potentially visible leaves, culls of
groups of 8 leaves, a bounding sphere per face, early rejects in the mark
fragments), read in the commit itself.

What the traversal did per view (a temporary counting build; all frames of
the demos):

| per view | `obj_team2` yard | `dm/mohdm3` overview | `o2match` | `o4match` |
| --- | --- | --- | --- | --- |
| nodes visited | 324 | 1937 | 218 | 2817 |
| leaves added | 80 | 562 | 20 | 1124 |
| leaf surfaces (marks) | 4899 | 5631 | 1742 | 5313 |
| faces reached | 3988 | 3160 | 1413 | 2471 |
| culled by their plane (GL1) | 685 | 1458 | 184 | 1109 |
| drawn by GL1 | 3246 | 1702 | 1219 | 1290 |
| of those, wholly outside the view | 1622 (50 %) | 269 (16 %) | 677 (56 %) | 259 (20 %) |

In the stack samples of `o4match` (1920x1080) the traversal took 10.9 to
11.6 % of the main thread's time: 5.5 to 5.7 % the walk itself (the nodes'
`visframe` read, the recursion, the leaves' surface lists), 2.8 to 3.2 %
`R_CullSurface` (reading each face's plane, a cache miss per face: every
face is an allocation of its own) and 1.3 to 1.4 % `BoxOnPlaneSide`.
Whenever the view's cluster changed, `R_MarkLeaves` also marked the leaves
of the new PVS and their ancestors in a pass over every node: 0.23 ms each
time in `obj/obj_team4` (a temporary timing build; 278 times in the 15000
frames of `o4match`, 2 % of the frames, so right at their 99th percentile).

JayRewd's culls as written, against GL1 (a script also checked the 66 BSP
files of the game data):

- The flat leaf list takes every node that `R_MarkLeaves` marked and that
  is a leaf, in the order of the nodes array. That includes the leaves of
  the brush models, which the BSP tree doesn't reach but which have a
  cluster, so `R_MarkLeaves` marks them when it is in the PVS (14 to 94
  such leaves in the `dm` and `obj` maps; no leaf the tree reaches lists a
  brush model's surface): their surfaces are in the models' own
  coordinates, and drawn as world surfaces they put copies near the
  origin, which the commit hides by skipping surfaces past the world
  model's ("Fixed in Omaha"). The array's order is the walk's in every
  shipped map, but each leaf is tested on its own box, which gives GL1's
  result only when the leaf's box lies within the boxes of all its
  ancestors: in 43 of the 66 maps some leaves stick out of an ancestor's
  box, by one unit (the stored integer bounds are rounded outwards; up to
  134 leaves in a map, 68 in `dm/mohdm3`, 5 in `obj/obj_team2`), and such
  a leaf can be culled by GL1 at an ancestor and kept on its own box, or
  the reverse.
- The groups of 8 consecutive leaves are exact by themselves: a box that
  contains the leaves' boxes is wholly behind (or in front of) a plane
  only if each of them is (`BoxOnPlaneSide` takes the farthest corners,
  and rounding keeps their order).
- The sphere per face is centred on the face's bounds with half their
  diagonal as radius, computed in floats (not certain to enclose every
  point), and culls a face whose sphere is behind a frustum plane, with no
  margin. It leaves out faces with deforms but not the sky (the sky's
  faces give the extents of the sky box, `RB_ClipSkyPolygons`) nor the
  sky portal's (they decide whether the sky portal's view is rendered).
- The early mark rejects (`R_MarkFragments`: cgame's bullet holes and
  feet shadows) are exact, since a fragment whose points are all less
  than 0.25 in front of a plane is dropped by `R_ChopPolyBehindPlane`
  (0.5) anyway, but they aren't the world's traversal: `R_MarkFragments`
  is 0.8 % of the `o4match` samples in all. Not taken.
- The commit's replay of the last walk when the view repeats exactly
  (frames that share a client millisecond) isn't in the review's list: it
  only helps above 1000 frames per second, or in fixed benchmark views.

What was done (see Implementation notes, World culling), each change
measured against the build before it:

1. Faces culled against the view: a face whose bounds lie wholly behind
   one of the frustum's side planes, by more than a unit, is left out (the
   GPU would clip all of it away). Only the planes its leaf isn't wholly
   in front of; not for faces whose shader deforms vertexes, the sky,
   portals and the sky portal's faces, nor in the leaves that dynamic
   lights reach, nor with `r_shownormals`, where a face outside the view
   still changes the frame: the lights of a batch are the union of its
   surfaces' (with `r_fastdlights 1` every light that reaches one face is
   projected onto all the batch's triangles, `ProjectDlightTexture`, and
   in this renderer's per-pixel lights the side a light must be on is
   tested for the whole batch when it has a lit face,
   `tess.lightmapDlightFaces`), and `r_shownormals` draws `tess.normal`,
   which faces write only when their shader needs normals, so the others
   show what earlier batches left there. The comparison with the build
   before found the `r_fastdlights 1` case in `dl_a` (`mohdm1_dlight`: a
   wall top lit by a light of an off-screen face of its batch, 730 pixels)
   and the `r_shownormals` one in `dbg_normals` (384 pixels of normals);
   the per-pixel case came from reading the code (it needs a lit face and
   a lit grid or terrain patch in one batch). With the conditions both
   views give the frames of the build before.
2. The BSP walk replaced by the potentially visible leaves listed in its
   order: the same leaves in the same order, each tested on its own box
   after the box of 8 consecutive ones, and the few that stick out of an
   ancestor's box along their path from the root; `BoxOnPlaneSide`'s
   arithmetic inline (the same products in the same order).
3. What culling a face reads copied into its `msurface_t` (64 bytes,
   aligned): its plane and its bounds, rounded outwards to whole units in
   shorts, so that the faces culled by their plane or by the view are not
   read at all; `srfSurfaceFace_t` keeps its size (with the bounds in it,
   change 1 had moved the dynamic light fields, which the front end
   writes for every drawn face, onto another cache line for more faces).
4. `R_MarkLeaves` lists the leaves of the PVS in the walk's order, with
   GL1's test, from a compact copy of each leaf's cluster and area (8
   bytes; the boxes in another array in the same order), and groups them:
   no pass over the nodes, no marks for the ancestors (nothing walks the
   nodes any more). In the same frames of `o4match`: 67 µs per cluster
   change on the median (mean 111, 90th percentile 227) against 238 for
   GL1's pass (mean 227, 90th percentile 359); it varies with the GPU's
   memory traffic.

Exactness. A temporary verification build ran GL1's code beside the new
one in every view: `R_MarkLeaves` and `R_RecursiveWorldNode` as in
`renderergl1` (with this renderer's culling of dynamic lights per node),
marking a private copy of the nodes' `visframe`, and `R_CullSurface` on
each face's own data. It compared the leaves added (in order, each with
the frustum planes left and its dynamic lights) and the faces culled by
their plane, and projected every face culled against the view with the
view's modelview and projection matrices (in double precision; zFar
doesn't change x, y and w) to check that all its points lie outside one
clip plane x = ±w or y = ±w by more than float32's rounding (1e-5 of |x|
+ |w|). Results:

| what was run (with the final build's verification copy) | lists | views | leaves added | faces culled by their plane | faces culled against the view | differences |
| --- | --- | --- | --- | --- | --- | --- |
| `dm` lists (`dm/mohdm1` to `dm/mohdm7`; fog, decals, dynamic lights, sun flare, debug drawing) | 14 | 11375 | 3.58 M | 7.31 M | 2.28 M | 0 |
| `obj` lists (`obj/obj_team1` to `obj/obj_team4`, the sea shader) | 5 | 4165 | 0.72 M | 1.37 M | 1.26 M | 0 |
| multiplayer lists (`mp_*`: 20 soldiers, muzzle flash lights) | 5 | 11240 | 3.18 M | 7.17 M | 1.71 M | 0 |
| single-player lists (the sky portal of `m2l1`, `m2l3` and its fog, `m3l3`, the fog of `m4l1`, `m5l3`, `m6l1a`, `m6l2a`) | 8 | 5586 | 1.57 M | 2.71 M | 0.84 M | 0 |
| variants: the dynamic light lists with `r_fastdlights 1`, `mohdm1_texenv` with `r_ext_texture_env_add 1`, `obj_team2` and `dm/mohdm3` with `r_novis 1`, `obj_team2` with `r_nocull 1` and with `r_facePlaneCull 0` | 8 | 7234 | 2.08 M | 4.04 M | 2.97 M | 0 |
| demos: `o2match`, `o4match`, an `obj/obj_team1` match (240 s) and a `dm/mohdm6` one (123 s), every frame | 4 | 52654 | 21.4 M | 36.1 M | 18.2 M | 0 |

(Differences: leaves that differ from GL1's walk in order, frustum planes
left or dynamic lights, faces whose plane cull differs from GL1's, faces
culled against the view with a point not outside the clip volume. The
nearest point of a culled face was 0.005 pixel beyond the frame's edge,
on a face whose points are all beyond it. The `r_novis` lists first
showed different leaves: the verification copy of GL1's `R_MarkLeaves`
had left out its re-mark when `r_showcluster` changes, which the lists
turn on; with it, none.)

Results (the final build; idle machine: other processes 1.6 CPUs at
most; the main thread's time in the renderer per frame, with its front end
(`scene`) and the 2D calls with the 3D back end; GPU time from timestamps;
medians of 5 interleaved runs for the fixed views and `o4match` at
1920x1080, of 3 for the other demo rows, 15000 frames each, whose
"content" percentiles are those of each frame's median over the runs, see
Frame spikes; ms):

| view | size | renderer before | after | scene before / after | 2D and back end before / after | GPU before / after | content p99 before / after | p99.9 before / after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 640x480 | 0.398 | 0.360 (-10 %) | 0.115 / 0.105 | 0.208 / 0.186 | 0.224 / 0.220 | | |
| | 1920x1080 | 0.402 | 0.393 (-2 %) | 0.113 / 0.113 | 0.217 / 0.209 | 0.623 / 0.622 | | |
| `dm/mohdm3` overview | 640x480 | 0.446 | 0.451 (+1 %) | 0.108 / 0.118 | 0.264 / 0.259 | 0.295 / 0.294 | | |
| | 1920x1080 | 0.524 | 0.529 (+1 %) | 0.133 / 0.138 | 0.321 / 0.319 | 0.700 / 0.702 | | |
| `o2match` | 640x480 | 0.359 | 0.344 (-4 %) | 0.067 / 0.063 | 0.221 / 0.211 | 0.277 / 0.278 | 1.037 / 1.035 | 1.446 / 1.381 |
| | 1920x1080 | 0.389 | 0.371 (-5 %) | 0.074 / 0.068 | 0.242 / 0.231 | 0.554 / 0.550 | 1.149 / 1.109 | 1.548 / 1.536 |
| `o4match` | 640x480 | 0.748 | 0.736 (-2 %) | 0.219 / 0.215 | 0.455 / 0.448 | 0.365 / 0.363 | 1.368 / 1.373 | 1.677 / 1.733 |
| | 1920x1080 | 0.887 | 0.876 (-1 %) | 0.289 / 0.282 | 0.523 / 0.517 | 0.749 / 0.745 | 1.671 / 1.648 | 2.105 / 2.018 |

(The frame times stay at the 1000 frames per second limit in the fixed
views and at 1.003 to 1.006 ms in `o2match`; `o4match`'s go from 1.051 to
1.045 and from 1.159 to 1.153.) The `dm/mohdm3` overview's 1 % is within
its run-to-run spread (0.440 to 0.471 before, 0.449 to 0.455 after at
640x480; earlier batches gave from -0.5 to +3 %). The slow frames of the
demos compared frame by frame (the 150 frames where the two builds'
medians add up highest; selecting by one build's frames alone favours the
other by regression to the mean): 0.046 ms shorter on average in
`o4match` at 1920x1080 (5 runs; 3 runs gave 0.070 longer, and
`o4match`'s content percentiles move by as much from one batch to the
next), 0.023 longer at 640x480, 0.051 and 0.004 shorter in `o2match`.
Between the build without the last condition of change 1 (no view
culling where dynamic lights reach) and the final one, in the same
batch: `o2match` 0.339 and 0.374 against 0.344 and 0.371, `o4match`
0.736 and 0.861 against 0.736 and 0.869; `o2match`, whose recorder fires
often, culls 9 % fewer faces against the view with it (614 per view
against 676).

Each change against the build before it (separate interleaved batches,
the same columns; renderer time, ms):

| view | size | before | 1. faces against the view | 2. leaf list | 3. face data in `msurface_t` | 4. PVS list | lit leaves kept |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 640x480 | 0.391 | 0.362 | 0.362 | 0.367 / 0.368 * | 0.361 / 0.356 | |
| | 1920x1080 | 0.398 | 0.390 | 0.380 | | 0.387 / 0.383 | |
| `dm/mohdm3` overview | 640x480 | 0.439 | 0.452 | 0.444 | 0.448 / 0.449 * | 0.443 / 0.454 | |
| | 1920x1080 | 0.534 | 0.535 | 0.531 | | 0.536 / 0.534 | |
| `o2match` | 640x480 | 0.362 | 0.349 | 0.342 | 0.347 | 0.342 / 0.337 | 0.339 / 0.344 |
| | 1920x1080 | 0.385 | 0.379 | 0.372 | 0.367 | 0.369 / 0.363 | 0.374 / 0.371 |
| `o4match` | 640x480 | 0.738 | 0.747 | 0.731 | 0.731 | 0.731 / 0.731 | 0.736 / 0.736 |
| | 1920x1080 | 0.866 | 0.877 | 0.876 | 0.852 | 0.855 / 0.848 | 0.861 / 0.869 |

(Changes 1 to 3 in one batch for each kind of view; * a batch of 3 runs
with the build before and after change 3; change 4 against the build with
changes 1 to 3 and the inline box test, which was worth 3 to 5 µs of the
front end in the overview and `o4match` on its own; the last column is
the condition of change 1 that keeps the faces where dynamic lights reach
and with `r_shownormals`, which changes nothing in the fixed views, which
have no dynamic lights.)

- Culling faces against the view saves back-end time (the static geometry
  had already made a world face cheap there, about 11 ns each in the
  yard) and a little GPU time at 640x480, and costs a test per face in the
  front end: it pays where many faces are outside the view (the yard and
  `o2match`, half the faces GL1 drew) and cost a little in the overview
  and `o4match` (16 and 20 %) until change 3 took the reads of the faces
  out of the culling. Static views keep the world's data in the caches,
  so change 3 shows in the demos only.
- The leaf list makes more box tests than the recursion where the PVS is
  large and the frustum culls most of it (`o4match`: `BoxOnPlaneSide` and
  the leaf tests 3.1 % of the samples against 1.3 %) and saves the node
  visits: about even, a little better with the box test inline. Groups
  of 4 or 16 leaves gave the same within the noise.
- What is left of the traversal in `o4match`: the leaves' surface lists
  (`R_AddWorldLeaf`, 3.6 % of the samples), the first read of each drawn
  face (now in `R_CheckDlightSurface`, which writes its dynamic light
  fields, 1.1 %; the back end reads the face anyway), the culling (1.4 %)
  and the box tests (3.1 %).

Present fences (2026-10-18, Plan 15; see Implementation notes,
Presentation semaphores and swapchains): what the known risks' fixes cost
on the main path is the present fence in `VkPresentInfoKHR` (Mesa adds it
to the submission it makes for every present anyway) and, after the
present and the acquire of the next image, one `vkGetFenceStatus` and one
`vkResetFences` for the next present's fence (two slots in use on RADV);
nothing between a frame's input read and its present, the acquire where it
was, no frame more in flight. The fallbacks are off on RADV. Measured on
the idle machine (other processes 0.7 to 1.9 CPUs, no Plex transcoding),
1920x1080 under Xwayland, the build before (`base`) and this one
(`new`) interleaved, 5 runs of 10 s each (`perf.py --list ... --binaries
openmohaa,openmohaa_fs --caps 0,250 --seconds 10 --runs 5 --preload
libinlat.so --framelog DIR`, `openmohaa_fs` a wrapper adding `+set
r_fullscreen 1 +set in_nograb 0`; fullscreen is immediate, with tearing).
Input read to present (`libinlat.so`), ms: the median of the runs'
medians (their range), the median of their 99th percentiles:

| view | `com_maxfps` | variant | base: median (runs), p99 | new: median (runs), p99 |
| --- | --- | --- | --- | --- |
| `obj_team2` yard | 0 | window | 0.410 (0.399 to 0.416), 0.631 | 0.411 (0.402 to 0.425), 0.618 |
| | 0 | fullscreen | 0.445 (0.438 to 0.462), 0.769 | 0.444 (0.433 to 0.457), 0.735 |
| | 250 | window | 0.540 (0.535 to 0.542), 0.929 | 0.529 (0.524 to 0.570), 0.944 |
| | 250 | fullscreen | 0.535 (0.530 to 0.549), 0.921 | 0.541 (0.526 to 0.545), 0.917 |
| `o2match` | 0 | window | 0.499 (0.489 to 0.517), 1.124 | 0.498 (0.484 to 0.506), 1.164 |
| | 0 | fullscreen | 0.524 (0.487 to 0.534), 1.299 | 0.533 (0.516 to 0.577), 1.326 |
| | 250 | window | 0.566 (0.555 to 0.576), 1.062 | 0.551 (0.547 to 0.565), 1.025 |
| | 250 | fullscreen | 0.568 (0.549 to 0.576), 1.046 | 0.562 (0.546 to 0.571), 1.026 |

The medians differ by -0.015 to +0.009 ms, inside the spread of the runs
either way: no change to measure. The build timed here had the stride 0
bindings follow `vertexAttributeAccessBeyondStride` on portability
devices; the final one differs from it only in `VK_CreateLogicalDevice`
(and the function it is inlined into): every other function is the same
code, instruction for instruction (`objdump -d`, addresses left out).

Frame time and the renderer's time uncapped (`perf.py --list ...
--sizes 1920x1080,3840x2160 --renderers base,final --runs 3`, medians of
3 interleaved runs with their range, ms; GPU: the main command buffer,
timestamps; end: `EndFrame`, which submits, presents, acquires the next
image and now chooses the next present's fence; other processes 0.9 to
2.0 CPUs):

| view | size | frame base | frame new | renderer base | renderer new | end base / new | GPU base / new |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 1920x1080 | 0.999 | 0.999 | 0.380 (0.372 to 0.403) | 0.394 (0.381 to 0.410) | 0.069 / 0.075 | 0.621 / 0.624 |
| | 3840x2160 | 1.724 (1.719 to 1.732) | 1.713 (1.710 to 1.714) | 1.567 | 1.569 | 0.125 / 0.122 | 1.708 / 1.697 |
| `o2match` | 1920x1080 | 1.005 | 1.005 | 0.376 (0.370 to 0.379) | 0.373 (0.373 to 0.376) | 0.073 / 0.076 | 0.551 / 0.552 |
| | 3840x2160 | 1.326 | 1.330 | 0.907 | 0.906 | 0.085 / 0.091 | 1.261 / 1.265 |
| `o4match` | 1920x1080 | 1.121 (1.121 to 1.135) | 1.139 (1.123 to 1.162) | 0.843 (0.843 to 0.856) | 0.860 (0.844 to 0.883) | 0.075 / 0.080 | 0.729 / 0.738 |
| | 3840x2160 | 2.240 | 2.236 | 1.959 | 1.963 | 0.102 / 0.105 | 2.203 / 2.200 |

The yard again with 5 runs of 5000 frames: renderer 0.355 (0.351 to
0.362) against 0.362 (0.360 to 0.367) at 640x480, 0.391 (0.387 to 0.394)
against 0.388 (0.380 to 0.395) at 1920x1080. In every case the present
call (`vkinfo`'s `presentUs`) takes 1.1 µs more, 11.0 to 13.4 µs against
12.1 to 14.6 (Mesa signals one fence more in the submission it makes
there), and `EndFrame` 3 to 6 µs more in all (the fence's status and its
reset after the present); the rest moves both ways within the noise.
That is the cost: about 4 µs a frame after the input's present, of which
1 µs inside the present call, after the moment `libinlat.so` takes as
the present.

Pipelines and skin data at load (2026-10-18, Plan 16; see Implementation
notes, Pipelines at load and Skeletal models). Three cases: the first play
after an install (cold caches: no `vkpipelines.cache`, no
`vkpipelines.keys`, `MESA_SHADER_CACHE_DISABLE=true`, `perf.py --cold`),
the first play after a driver update (the same with `vkpipelines.keys`
kept, `--keep-keys`), and every later play (warm caches). Idle machine
(other processes at most 1.1 CPUs, no Plex transcoding), 1920x1080 under
Xwayland, the build before (`base`) and this one (`new`) interleaved with
frame log copies of both (`--vklog`), 5 runs of the two match demos (3 for
the driver update), medians of the runs. Map load: `CL_InitCGame`'s time
(the map, cgame's registration, `EndRegistration`); the first frames after
it: the sum of the first 10, the slowest of the first 30; pipelines and
skin data added during the 15000 measured frames (`vkinfo`; the base build
added the skin data at first draws, which it doesn't count); ms:

| demo | caches | build | map load (s) | pipelines at load (ms) | first 10 frames | slowest of the first 30 | p99 | p99.9 | max | pipelines in the match (ms) | skin data in the match |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | cold | base | 1.33 | 7 (11) | 43.7 | 33.6 | 1.161 | 2.080 | 33.6 | 39 (60) | at first draws |
| | | new | 1.51 | 157 (179) | 20.8 | 10.5 | 1.167 | 1.556 | 21.2 | 2 (3.6) | 12 (214 KB), when registered |
| | driver update | new | 1.51 | 159 (183) | | | 1.157 | 1.544 | 21.0 | 0 | 12 (214 KB) |
| | warm | base | 1.33 | 7 (0.2) | 18.2 | 8.2 | 1.166 | 1.558 | 21.0 | 39 (0.9) | at first draws |
| | | new | 1.33 | 159 (1.0) | 16.5 | 7.0 | 1.150 | 1.554 | 21.2 | 0 | 12 (214 KB) |
| `o4match` | cold | base | 1.23 | 7 (14) | 80.2 | 58.4 | 1.832 | 3.658 | 58.4 | 49 (80) | at first draws |
| | | new | 1.43 | 177 (196) | 24.4 | 9.4 | 1.878 | 2.761 | 28.5 | 2 (3.6) | 7 (106 KB), when registered |
| | driver update | new | 1.44 | 179 (199) | | | 1.764 | 2.276 | 28.1 | 0 | 7 (106 KB) |
| | warm | base | 1.24 | 7 (0.2) | 24.9 | 8.5 | 1.821 | 2.452 | 28.0 | 49 (0.9) | at first draws |
| | | new | 1.24 | 179 (1.0) | 21.2 | 6.0 | 1.805 | 2.460 | 28.7 | 0 | 7 (106 KB) |

(The ranges over the runs: the cold maxima 33.2 to 34.5 and 58.1 to 59.2
ms before, 21.0 to 22.0 and 28.3 to 29.2 after; the warm 20.9 to 21.1 and
27.9 to 28.3 before, 20.9 to 21.7 and 28.0 to 31.1 after.)

- With cold caches the 39 and 49 pipelines that the matches created on
  first use (60 and 80 ms: 21 and 38 of them in the first 30 frames, 29
  and 53 ms, the others in 10 and 11 frames of the match, up to 7.6 ms in
  one) are created at load instead: 157 and 177 pipelines in 179 and 196
  ms, the map load 0.18 and 0.20 s longer (+14 and +16 %). The first
  frames after the load take 21 and 24 ms in all instead of 44 and 80, the
  slowest of them 10 and 9 ms instead of 34 and 58; the 99.9th percentile goes from 2.08 to 1.56
  ms and from 3.66 to 2.76 ms, the warm plays' values within the run to run
  spread (o4match's 99.9th percentile spreads more, 2.28 to 2.76 ms, from
  batch to batch); the maximum from 33.6 to 21.2 and from 58.4 to 28.5 ms,
  which is registration in the match (shared with GL1, see below and
  Remaining), the same as with warm caches. In `framelog.py --batch`'s
  causes of the slow frames (those above each run's 99th percentile),
  pipelines were 27 to 30 % of the excess in both matches and are 1.5 to
  2.4 % (the head icons).
- What the first play still creates in the match: 2 pipelines (3.6 ms), in
  the first frame, for the head icon sprites (`textures/hud/*_headicon`, a
  sprite model cgame registers when it first draws one; their alpha test
  and blend combination isn't any load shader's). After a driver update,
  and in every later play, they are recorded keys and built at load: 0.
- With warm caches the pipelines of the load come from the pipeline cache:
  152 and 172 more in 1 ms, the map load unchanged; the 0.8 to 0.9 ms they
  cost in the match is gone, and the first frames are shorter by 1.7 and
  3.7 ms (pipelines and the skin data of the first draws).
- The skin data is added when a model is registered: at load for the
  models of the gamestate (234 additions, 3.8 MB in `o2match`, where the
  base build added 1.6 MB over the whole match at first draws: the load
  now also adds the models that the match never draws), and during the
  match the 12 and 7 additions (214 and 106 KB) of the models announced
  then, all in frames that register (their frame log records have
  registration time): no first draw adds any. The skin data buffer is 32
  MB; 4.3 MB was the most a load used in the six demos below.
- The frame time otherwise: the same within the noise (the median of the
  runs' means 1.005 and 1.004 ms, 1.141 and 1.142 ms warm; renderer time
  0.377 and 0.380, 0.862 and 0.868 ms, ranges 0.375 to 0.384 and 0.371 to
  0.389, 0.839 to 0.899 and 0.860 to 0.894).

How complete the load's pipelines are, checked with a temporary build that
printed every pipeline a draw created and every shader registered after the
load, on six matches of four maps (`o2match`, `o4match`, a second match of
each, `obj/obj_team1`, `dm/mohdm6`), a fresh homepath for each: the base
build created 33 to 49 pipelines after the load, the new one creates none
at a draw; 2 come in the first frame for the head icons and 1 when the
menu opens at the end of a demo (`menu_button_trans`, which a real session
has created in the main menu); with the keys file of the six runs, none.
With `r_vkFallbacks 3` (keys with the RGB bit of every texture),
`r_dlightBacks 0`, `r_fastdlights 1`, `r_gpuSkinning 0` and
`r_ext_multisample 4` (the final build, `o2match` and `o4match`, no keys
file) the same: the 2 head icon pipelines after the load, none at a draw
(201 to 225 pipelines at load with the fallbacks, 172 to 198 with
`r_dlightBacks 0`, 146 to 166 without GPU skinning, which then adds no
skin data). The loads create 122 to 173 pipelines (fogged maps more: their fog isn't
known at load, so both), of which 31 to 50 (20 to 29 %) are used in the
match; unused ones are destroyed at the next registration. The first
version of the prediction missed the immediate mode clear to the fog color
and the UI boxes (now built, see Implementation notes) and built shaders
registered later with both fogs and static geometry variants (4 pipelines
for a head icon instead of 1). Memory on RADV: 178 more pipelines left the
game's resident memory 50 KB larger and its device memory (amdgpu's
`fdinfo`) unchanged: pipelines that differ only in fixed-function state
share their shaders.

The `obj_team2` yard (`views/perf.txt`, a fixed view; 5 runs each): with
cold caches the map load takes 1.07 s instead of 0.90 (157 pipelines at
load, 166 to 167 ms of them created before their draw), against the base
build's 20 pipelines by the time the view is set up (the loading screen's
7 and 13 in the first frames, 23 to 25 ms); none after it in either. Warm,
the map load is 0.90 and 0.90 to 0.91 s and the renderer's time per frame
the same: 0.359 and 0.358 ms at 640x480, 0.385 and 0.386 ms at 1920x1080.

Input read to present (`libinlat.so`, `perf.py --caps 0,250 --seconds 10
--runs 5 --preload libinlat.so --framelog`, 1920x1080 in a window, the
yard and `o2match`; ms, the median of the runs' medians with their range,
the median of their 99th percentiles):

| view | `com_maxfps` | base | new |
| --- | --- | --- | --- |
| `obj_team2` yard | 0 | 0.408 (0.406 to 0.416), 0.602 | 0.409 (0.405 to 0.411), 0.612 |
| | 250 | 0.530 (0.518 to 0.562), 0.917 | 0.530 (0.520 to 0.545), 0.910 |
| `o2match` | 0 | 0.498 (0.495 to 0.506), 1.141 | 0.508 (0.494 to 0.512), 1.110 |
| | 250 | 0.560 (0.557 to 0.564), 1.040 | 0.558 (0.557 to 0.570), 1.047 |

Nothing was added between a frame's input read and its present: the
pipelines are created at the end of the load (between frames), or for a
shader registered in a frame when it is registered, which is no later
than its first draw would have created them; the skin data when a model is
registered; per frame only the pipeline's used flag is stored at each
pipeline lookup that isn't the previous draw's pipeline; no frame more in
flight, the acquire where it was. The medians differ by -0.002 to +0.010
ms, inside the spread of the runs.

What is left of the slowest frames: registration in the match (Remaining,
item 1), shared with GL1, measured in the same demos with warm caches (the
frame logs of the 5 runs above; stack samples of `libstacks.so` in 2 more
runs per renderer, `perf.py --stacks`). `o2match` has 6 frames whose
registration (`load`) takes 0.3 ms or more, 39 ms of registration in all,
the slowest of them 21 ms with 19 ms of registration; `o4match` 3 frames,
26 ms, the slowest 28.6 ms with 24 ms of registration (GL1: 7 frames and
49 to 50 ms, the slowest 26 ms; 4 frames and 31 ms, the slowest 32 to 33
ms); 26 to 36 textures are created after the load (8 to 11 MB). Where the
main thread is in those frames' registration exports (134 samples of
Vulkan's 4 runs, 155 of GL1's, each attributed to the first of these
functions from its leaf up):

| | Vulkan | GL1 |
| --- | --- | --- |
| zlib decompressing the images from the pk3s (`inflate`, `unzReadCurrentFile`) | 48 % | 47 % |
| the file system's lookups (`FS_FOpenFileRead`, `FS_ReadFile`, the pk3 directories) | 17 % | 13 % |
| its case correction (`FS_CorrectCase`: `opendir`/`readdir` of each search directory that lacks the path) | 11 % | 10 % |
| mipmaps and scaling on the CPU (`R_MipMap`, `ResampleTexture`, `R_LightScaleTexture`; GL1's uploads) | 10 % | 21 % |
| TIKI files (the engine's skeletal models) | 7.5 % | 3 % |
| shader parsing, the sound | 2 % | 4 % |
| the skin data, now built when the model is registered (`R_SkinTiki`) | under 1 % | |
| TGA and JPEG decoding itself | about 1 % | 3 % |

The registration takes as long as before (0.3 to 0.7 ms more in all per
match: the skin data of the models announced in the match, 214 and 106 KB,
which their first draws added before), and the frames that used to add
skin data and create pipelines at a first draw don't any more.

Skeletal culling (2026-10-19, Plan 17; see Implementation notes, Skeletal
culling). GL1 prepares every skeletal model of the scene, in view or not:
its bones, morphs and surfaces, then in the back end its lighting (the sun
trace of a moving entity, the sphere's lights) and its draws, which the GPU
clips away. JayRewd's fork (b26d8a67) returns from `R_AddSkelSurfaces`
before the bones when the sphere of the model's animation frame, with its
radius plus 16 times the entity's scale, is outside the view, and reported
-22 and -61 µs per frame in `o2match` and `o4match`. The frame radius comes
from the animation files and isn't a bound of the mesh, so before taking it
a temporary verification build (not kept) measured how far the vertexes
really reach. It drew every model as before and, for every model the cull
could reject (not the player's own body, the view weapon, mirror views or
`lod_tool`), computed each vertex of each drawn surface as `RB_SkelMesh`
does (bones, weights, morphs; every vertex, so every level of detail,
whose vertexes are a subset) in world space with the entity's axes; it
recorded per TIKI how far the vertexes reach from the sphere's center, and
for a set of candidate margins checked each model the sphere would cull:
all its vertexes must lie outside one plane of the clip volume (x = ±w or y
= ±w, in double precision, by more than float32's rounding; the world
culling's check), else no triangle of it can be in view. It ran over 20
matches of real players played frame by frame (`fixedtime 16`, 1920x1080, up
to 40000 frames each: 5 of `obj/obj_team2` with `o2match`, 5 of
`obj/obj_team4` with `o4match`, 4 of `obj/obj_team1`, 4 of `dm/mohdm6`, one
of `dm/mohdm4` and of `obj/obj_team3`; a 21st switched to a map the game
data doesn't have) and over the 41 view lists (single-player AI, vehicles,
the landing craft, the convoy, the sky portal, the soldier views):

| content | models checked | culled by the final rule | with a vertex in view | closest culled model | radius alone: with a vertex in view | JayRewd's rule: with a vertex in view |
| --- | --- | --- | --- | --- | --- | --- |
| 20 matches | 11.68 M | 6.12 M (52.4 %) | 0 | 1.16 px outside the edge | 151 | 0 |
| 41 view lists | 1.22 M | 0.53 M (43.6 %) | 0 | 5.09 px | 0 | 23 |

What the radius misses, the largest reaches found (from the sphere's
center, against the frame's radius; TIKI units are world units divided by
the TIKI's `load_scale` times the entity's scale):

- players, while the bone controllers turn the torso and the head (aiming
  up or down, throwing a grenade): up to 1.20 times the radius, 35 TIKI
  units past it (`german_winter_1` in a `dm/mohdm6` match: 110.4 units
  for a radius of 92.1; `allied_airborne` 109.2, `american_ranger` 108.0);
  the bones alone reach 11 units past it, so the controllers, not the
  skin, make most of it; the morphs (faces) move vertexes by 0.5 units at
  most. Without the controllers 1.09 at most (the soldier views and the
  single-player AI 1.07);
- the stick grenade: its frame radius is 4.7 units, its vertexes reach
  14.0 (2.96 times);
- muzzle flashes are drawn with axes 2.4 times longer than unit vectors,
  splash drops (`fx/water_splash_drop.tik`) with axes up to 69 times longer
  and with a negative scale (mirrored): measured in world space their
  vertexes reach 2.3 and 44 times the radius, and a negative scale makes
  the radius negative, which turns the sphere test around (it culls what is
  inside). JayRewd's rule culled such drops in view in `m3l1a` (23 cases,
  the drops of the beach) and hasn't the margin for the players either:
  the worst one above is 2.3 units past his sphere (it was out of view
  then);
- vehicles, weapons, items, debris: within their radius.

The final rule: the radius (absolute) times 1.25 plus 32 TIKI units
(absolute), times how much the entity's axes can lengthen a vector, and
only the four side planes. With it the farthest vertex reaches 0.84 of the
sphere (the `german_winter_1` above: 110.4 of 131.8 units), the grenade
0.56; 1.25 + 32 culls 52.4 % of the matches' models against 54.6 % for the
radius alone and 53.2 % for JayRewd's rule (51.1 % for 1.5 + 48, 49.3 %
for 2 + 64): most culled models are far outside the view, so the margin
costs few culls. Models whose shaders deform vertexes are kept (0.2 % of
the culls: the bullet hole puffs).

Results (idle machine: other processes 1.0 CPU at most, no Plex
transcoding; the build before (`base`) and this one interleaved, medians
of 5 runs; the renderer's main-thread time per frame, its front end
(`scene`) and the 2D calls with the 3D back end, the GPU time of the main
command buffer and the draws per frame; the soldier views 1000 frames, the
demos 15000; ms):

| view | size | renderer base | new | scene base / new | 2D and back end base / new | GPU base / new | draws base / new |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `pm1_front` | 640x480 | 0.565 | 0.540 (-4 %) | 0.145 / 0.136 | 0.345 / 0.332 | 0.516 / 0.516 | 553 / 531 |
| `pm1_side` | 640x480 | 0.359 | 0.236 (-34 %) | 0.107 / 0.052 | 0.179 / 0.115 | 0.393 / 0.299 | 315 / 174 |
| `po1_front` | 640x480 | 0.381 | 0.351 (-8 %) | 0.119 / 0.103 | 0.187 / 0.175 | 0.422 / 0.413 | 343 / 299 |
| `po4_front` | 640x480 | 0.447 | 0.390 (-13 %) | 0.152 / 0.135 | 0.219 / 0.182 | 0.388 / 0.373 | 333 / 285 |
| `pm7_flash` | 640x480 | 0.494 | 0.457 (-8 %) | 0.147 / 0.129 | 0.274 / 0.248 | 0.383 / 0.358 | 386 / 334 |
| `o2match` | 640x480 | 0.341 | 0.319 (-6.5 %) | 0.061 / 0.057 | 0.206 / 0.190 | 0.273 / 0.264 | |
| `o4match` | 640x480 | 0.720 | 0.659 (-8.5 %) | 0.207 / 0.193 | 0.436 / 0.391 | 0.363 / 0.341 | |
| `pm1_front` | 1920x1080 | 1.085 | 1.071 | 0.168 / 0.163 | 0.836 / 0.825 | 1.225 / 1.222 | |
| `po4_front` | 1920x1080 | 0.482 | 0.428 (-11 %) | 0.171 / 0.153 | 0.239 / 0.200 | 0.780 / 0.774 | |
| `o2match` | 1920x1080 | 0.365 | 0.345 (-5.5 %) | 0.067 / 0.062 | 0.225 / 0.210 | 0.537 / 0.526 | |
| `o4match` | 1920x1080 | 0.833 | 0.779 (-6.5 %) | 0.265 / 0.247 | 0.493 / 0.452 | 0.721 / 0.703 | |

The frame times of the demos (medians of the 5 runs' percentiles, and
"content", each frame's median over the runs, see Frame spikes), ms:

| demo | size | mean base / new | p99 base / new | p99.9 base / new | content p99 base / new | content p99.9 base / new | max base / new |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 640x480 | 1.003 / 1.003 | 1.033 / 1.030 | 1.291 / 1.253 | 1.031 / 1.030 | 1.271 / 1.227 | 21.3 / 20.9 |
| `o4match` | 640x480 | 1.032 / 1.017 | 1.323 / 1.261 | 1.669 / 1.615 | 1.295 / 1.238 | 1.599 / 1.558 | 27.7 / 28.8 |
| `o2match` | 1920x1080 | 1.004 / 1.003 | 1.112 / 1.059 | 1.532 / 1.457 | 1.073 / 1.044 | 1.446 / 1.314 | 21.5 / 21.2 |
| `o4match` | 1920x1080 | 1.111 / 1.081 | 1.631 / 1.555 | 2.134 / 2.019 | 1.553 / 1.500 | 1.892 / 1.864 | 28.8 / 30.2 |

(1.000 is the 1000 frames per second limit, which most frames of
`o2match` reach; the maxima are registration in the middle of the match,
see Remaining: 27.6 to 30.8 ms from run to run in both builds.)

- What is saved per culled model: its bones (`R_GetFrame`, the engine's
  skeletor), morphs and surfaces in the front end, and in the back end its
  lighting sphere (with the sun trace of a model that moves) and one draw
  per surface (`vkinfo`: 553 to 531 draws in `pm1_front`, 315 to 174 in
  `pm1_side`, which looks away from the 16 soldiers of the front view). The
  GPU time goes down a little too (the culled triangles were set up and
  clipped). In the matches 6.3 and 10.1 models a view are culled (counted
  with a temporary timing build), 52 % of the skeletal models in all the
  matches above.
- The pose isn't skipped: the frame radius comes from it (`SetPose` takes
  the largest radius of the animation frames it blends), so culling before
  it would need a bound of every animation the model can play, or the
  engine giving the radius without posing. It costs the culled models 1.0
  and 1.9 µs a view in `o2match` and `o4match` (the temporary timing build:
  pose and radius of 6.1 and 8.5 models a view that cgame hadn't posed
  already for their tags), against 1.6 µs a model for the bones, morphs
  and surfaces that are left out in the front end alone.
- Input read to present (`libinlat.so`, `perf.py --caps 0,250 --seconds 10
  --preload libinlat.so`, 1920x1080 in a window, 5 runs; ms, base / new):
  `o2match` uncapped median 0.481 / 0.465, p99 1.067 / 1.031, p99.9 1.429
  / 1.369, at 250 0.534 / 0.523, 0.942 / 0.941, 1.324 / 1.323; `po4_front`
  uncapped 0.550 / 0.515, 0.810 / 0.741, 1.105 / 0.935, at 250 0.647 /
  0.589, 0.943 / 0.894, 1.114 / 1.110. Lower, never higher: the cull only
  removes work between the input read and the present; nothing else
  changes (no frame more in flight, the acquire where it was).

Command rate (2026-10-19, Plan 18; an engine, client and cgame change,
for both renderers). The client made one usercmd per frame, and a
usercmd is a step of the player's movement (pmove, run by the server and
predicted by cgame) as long as the frame: 1 ms at 1000 frames a second, 4
ms at 250 (and 2 and 5 ms when the clock drifts, `CL_AdjustTimeDelta`), 7
ms at 144 (4 to 8), 14 to 18 ms at 60. The frame rate was held at 1000 a
second (`Com_Frame` waited for a new millisecond of `Sys_Milliseconds`,
since `Com_ModifyMsec` counted a frame of 0 ms as 1 ms and the game would
have run ahead of real time). Players report that the short steps of high
frame rates get stuck on small bumps of `dm/mohdm2` that longer steps get
over; JayRewd's omaha commit 1620f310 (`cl_cmdfps`) separated the command
rate from the frame rate, and his b26d8a67 let the frame rate pass 1000.
What was taken, and how it differs:

- Usercmds at a fixed rate whatever the frame rate (`CL_CreateNewCommands`):
  `cl_movefps` (archived, 85 to 125 a second, default 100; named `cl_cmdfps`
  until 2026-10-23, when it took the `metal` branch's name, and so in the
  measurements below), a command every 1000 / `cl_movefps` whole
  milliseconds of server time (8 to 11 ms, 10 by
  default, the user's choice from the bump test below; 12 ms, 83 a second,
  is outside the range). JayRewd's waits until that much
  server time has passed since the last command and stamps it with the
  frame's time, so its step is the first multiple of the frame time at or
  above it (by its code about 14 ms at 144 frames a second, 8 at 250).
  Here each command is stamped one step after the one before, so every
  step has the same length at every frame rate, also when the clock
  drifts: a frame that passes no step makes no command, a frame longer
  than a step makes several with the same input (the weapon command, which
  cgame sends in three commands, is asked for each), up to 100 ms of them:
  below 10 frames a second, after a hitch or when the server time jumps,
  one command of all of it as before, so that a packet with the last
  packet's commands (`cl_packetdup`) stays within `MAX_PACKET_USERCMDS`.
  Until the first snapshot (`CA_PRIMED`) one per frame as before.
- The view turns every frame: `CL_ApplyLook` applies the mouse, the keys
  that turn and the joystick every frame, and cgame turns the view with
  this frame's angles through a new import, `GetViewUserCmd` (the newest
  command with this frame's view angles), after it has predicted the
  commands with the angles they were sent with (`CG_PredictPlayerState`,
  and `CG_InterpolatePlayerState` for noclip and spectators). No command
  changes once it is made; JayRewd's writes each frame's angles into the
  newest command, sent or not, and predicts it with them, so cgame's
  prediction of that command differs from the server's. What the mouse
  adds to the movement (strafe, `BUTTON_ANY`) waits for the next command.
  Buttons, taps and weapon commands are read only when a command is made,
  so none is lost between commands; a key held for part of a command's
  time moves it by that part, as before (JayRewd's counts a tap as the
  whole command and resamples the keys into the newest command for
  prediction, input the server never gets: not taken).
- No packet without a new command unless reliable commands wait
  (`CL_SendCmd`): a frame between commands sends nothing.
- cgame: in the frames between commands the snapshot can have run every
  command already, and its playerState lacks what the network doesn't
  carry, which cgame then took from the snapshot (`walking` false in a
  `nextSnap`: the view bob, the view model and the landing would flicker).
  It now takes `walking`, `groundPlane` and `groundTrace` from its last
  pmove of the same command and the rest from the snapshot (JayRewd's keeps
  the whole predicted state, the server's position included).
- The server still receives those `cl_movefps` steps. The client picture
  samples the current stock step: one `Pmove` of a full interval from the
  last sealed command, with the keys held this frame, then the origin and
  velocity at
  `(milliseconds since the command + the sub-millisecond fraction) / interval`.
  The fraction is one monotonic read latched to `com_frameTime`
  (`Com_LatchRenderFraction`). It is not a `Pmove` input. Not taken:
  JayRewd's display lerp of the own position between the last
  two commands (it shows the player up to a step behind). The own position
  used to move once per
  command, every 10 ms at the default (every second or third frame at 250
  frames a second, every tenth at 1000), and lags the world drawn at the
  frame's time by 0 to 10 ms, as at 100 frames a second before; the view
  turns every frame, the other players are interpolated every frame.
- What the fixed rate costs: a button (fire, use, jump) or a movement key
  reaches the server with the next command, 0 to 10 ms after the frame
  that read it at the default (5 on average), where one command per frame
  sent it within 4 ms at 250 frames a second (2 on average) and within 1 ms
  at 1000: 3 ms more on average than at 250. The view's turning is in
  every frame and goes to the server with the next command, as before
  with the frame's.
- The limiter (`Com_Frame`): at a timescale of 1, a client cap above 1000
  waits for its deadline in microseconds alone, an uncapped client
  (`com_maxfps 0`) for nothing, and `Com_ModifyMsec` no longer counts a
  frame of 0 ms as 1 ms (JayRewd's b26d8a67 and 1620f310; his also drop
  the millisecond at other timescales, where a frame of 0 ms still counts
  as 1 ms, so scaled time, a demo in slow motion for one, would run fast:
  they keep it here). Caps up to 1000 (the default is 85) and dedicated
  servers are as before. The `fps` counter takes the frame times in
  microseconds (`Sys_Microseconds`; whole milliseconds over 16 frames
  showed 1455 or 1600 at 1515 frames a second, 2667 or 3200 at 3000).
  The world is still drawn at whole milliseconds of game time (`cg.time`):
  a frame in the same millisecond as the one before shows the same moment
  of the world with the newest view. One division by the frame time in
  cgame (the water ripples) is guarded against 0.
- `cl_movefps 0` (2026-10-24, the user's, as on the `metal` branch): stock,
  one command per frame at the frame's server time (`CL_MoveInterval` 0 in
  `CL_CreateNewCommands`), so each step is as long as its frame (4 ms at
  250 frames a second); cgame predicts no open step (the newest command is
  this frame's; `GetLiveUserCmd` gives it with this frame's view), and
  `Com_Frame` keeps every frame a whole millisecond or longer
  (`CL_CommandPerFrame`): a cap above 1000 and uncapped run at 1000, as
  stock, so every command has a time of its own. A command log in
  `dm/mohdm1` (10 s, walking and turning, a temporary engine): at
  `com_maxfps` 250, 144, 1000, 2000 and 0, one command in every frame (250,
  144 and 1000 a second; steps of 4, 7 (4 to 8) and 1 ms, 1.5 % of the 1 ms
  steps 0 ms where the time delta's correction held the server time, as a
  stock client's); at the default 100 and `com_maxfps` 2000 as before (1986
  frames, 100 commands a second, every step 10 ms).

The bump. A temporary cgame command (not kept) walked the player's pmove
the way the server runs it (`Player::ClientMove`: no ground entity before
the move, the velocity clipped to the ground plane after it, a move
shorter than 0.005 units undone; cgame's prediction does none of these)
from every floor of a grid over `dm/mohdm2` in 8 directions for 1.5 s at
each step and recorded where each walk ended. Walks that stopped short
where another step from the same start went on (under half of the longest
of them), of 29,315 (a 96-unit grid; the 64-unit grid at 4, 8 and 12 ms
gave the same order):

| step (ms) | 1 | 4 | 8 | 9 | 10 | 11 | 12 | 16 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| walks stopped | 193 | 103 | 66 | 58 | 48 | 57 | 59 | 47 |
| of them only at this step | 81 | 6 | 2 | 1 | 0 | 3 | 4 | 6 |

Then in the game, the server and the client's real commands (a test build
that set the step, `fixedtime 16`), three bumps where the shorter steps
stopped, each walked into from 29 positions (64 to 256 units before it, 8
units to either side): the walks that got more than 32 units past it in
2.5 s:

| bump (where, direction) | 1 ms | 4 | 8 | 9 | 10 | 11 | 12 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C1 `-2624 331 38`, south: the edge where the dirt terrain meets the cobbles, at a house corner | 0 | 1 | 12 | 13 | 15 | 14 | 18 |
| C3 `-2817 -1485 18`, north | 27 | 21 | 25 | 25 | 27 | 27 | 26 |
| C7 `-1330 -3587 -6`, north-west | 19 | 17 | 17 | 18 | 20 | 19 | 24 |

Why, from pmove with debug prints at C1: the bump is a lip 1.5 units high
whose edge is a bevel of 70° about 0.55 units wide. When a move is
blocked, `PM_StepSlideMove` tries it again 18 units higher and traces back
down; when that trace lands on a surface too steep to walk on
(`MIN_WALK_NORMAL`: the bevel) the step is refused and the blocked move is
kept, whose slide along the bevel has taken nearly all the forward speed.
The raised move covers one command's distance from where the command
started, so the step is refused when the command started too close to the
lip for its distance to carry the box past the bevel, about the bevel's
width out of each step's length: at a running speed of 264 units a second,
0.55 out of 1.1 units at 4 ms, of 2.1 at 8 ms, of 3.2 at 12 ms. Once the
speed is gone a command from a standstill moves 0.17 units at 8 ms (0.38
at 12, 0.68 at 16): never past the bevel again, and the player stays there until
turning or jumping. So a shorter step stops more often at such an edge, a
longer one less, none never; other bumps stop longer steps more (C7 at 4
to 11 ms the same, the sweep's 16 ms as many as 10 ms). A fix would be in
pmove (the step-up refused on a short bevel), which the server runs, not
in the client.

Checks, with a test build (not kept) that logged every command, packet and
server move and cgame's view and prediction, in a local game of
`dm/mohdm2` (the player running in circles for 8.5 s with two button taps,
a weapon change and a use; `cl_cmdfps` 125, 8 ms, the default then, also in
the first latency batch below):

| `com_maxfps` | frames a second | usercmds a second | steps (ms) | usercmds per frame | packets without a new usercmd | server moves of 8 ms | before: steps (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 60 | 60 | 125.3 | 8 (1045 of 1045) | 1 to 3 | 0 | all but the first | 14 to 18 |
| 144 | 144 | 125.2 | 8 | 0 or 1 | 0 | all but the first | 4 to 8 (993 of 1216 7 ms) |
| 250 | 250 | 125.0 | 8 | 0 or 1 | 0 | all but the first | 2, 4, 5 (169 of 2116 not 4) |
| 1000 | 1000 | 125.0 | 8 | 0 or 1 | 0 | all but the first | 0, 1, 2 (231 of 8483 not 1; the server runs no 0 ms one) |
| 0 | 3272 | 125.1 | 8 | 0 or 1 | 0 | all but the first | as at 1000 |

- Nothing lost between commands: each attack tap is in one command (two at
  60 frames a second, whose frames make two commands), the use tap too, the
  weapon change in three commands, at every cap, as before.
- The view turns in the frames without a command: while turning with the
  keys, the view's yaw changed in 88 of 120 such frames at 144, 579 of 802
  at 250, 4108 of 5675 at 1000 (keys turn by whole milliseconds, so frames
  within one millisecond don't turn; the mouse turns the view in every
  frame that has its input).
- The same walk at 144, 250, 1000 frames a second and uncapped (`+forward`
  from a standstill, into C1 and from the Allied spawn): the server's moves
  are the same to the bit at every cap (311 and 374 moves of 8 ms).
- Prediction: the server's result for each predicted command, at every
  cap, before and after: the same errors (median 0.02 to 0.04 units, 90th
  percentile 0.05 to 0.12, maximum 0.5 to 3.4) in both builds, from what
  the server does that cgame doesn't (above).

Latency (the build before against this one, interleaved, 5 runs of 10 s,
1920x1080 fullscreen with tearing, `r_swapInterval 0`, `libinlat.so` with
`INLAT_DISPLAY=1` and `INLAT_FBPOLL_US=250`, `framelog.py --display`; ms,
medians of the runs (their range) and of their percentiles; uncapped is
1000 frames a second before and 1500 now):

| view | `com_maxfps` | build | input to present: median, p99 | input to GPU done | input to display: median (runs), p99, p99.9 |
| --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 250 | before | 0.501, 0.810 | 1.398 | 1.925 (1.848 to 2.062), 2.562, 2.845 |
| | | now | 0.499, 0.823 | 1.381 | 2.010 (1.937 to 2.081), 2.509, 2.761 |
| | 1000 | before | 0.427, 0.620 | 0.982 | 1.502 (1.407 to 1.593), 1.753, 1.900 |
| | | now | 0.430, 0.640 | 0.985 | 1.602 (1.473 to 1.637), 1.852, 2.112 |
| | 0 | before | 0.428, 0.610 | 0.981 | 1.542 (1.402 to 1.674), 1.820, 2.041 |
| | | now | 0.627, 0.837 | 1.466 | 1.777 (1.732 to 1.809), 2.083, 2.192 |
| `o2match` | 250 | before | 0.518, 0.956 | 1.305 | 1.901 (1.849 to 1.948), 2.671, 3.140 |
| | | now | 0.523, 0.964 | 1.315 | 2.046 (1.927 to 2.084), 2.827, 3.136 |
| | 1000 | before | 0.501, 1.216 | 1.065 | 1.497 (1.456 to 1.613), 3.126, 3.428 |
| | | now | 0.495, 1.201 | 1.051 | 1.570 (1.458 to 1.583), 3.151, 3.507 |
| | 0 | before | 0.502, 1.212 | 1.056 | 1.531 (1.507 to 1.593), 3.143, 3.469 |
| | | now | 0.563, 1.221 | 1.194 | 1.545 (1.495 to 1.547), 3.161, 3.554 |

(One run of this build at 250 in `o2match` didn't go fullscreen and is
left out.) At 250 and 1000 the input to present and to GPU done are the
same; input to display was 0.07 to 0.15 ms higher in all four cells of
this batch, inside the runs' range, so a second batch with the final build
(`cl_cmdfps` 100) and 8 runs each: `o2match` 1.466 (1.404 to 1.598)
against 1.498 (1.413 to 1.628) ms at 1000 and 1.978 against 1.965 at 250,
the yard 1.551 against 1.579 and 1.995 against 2.039: the same within the
spread, both ways.

Frame times and CPU (the same runs, `renderer_timed.so`): at 250 and 1000
the same as before (the yard 4.000 and 0.999 ms on the median, 99th
percentile 4.099 and 1.091 against 4.098 and 1.090; `o2match` 1.266
against 1.283 at 1000; the game 0.45 and 1.08 CPUs in both builds).
Uncapped, 1498 frames a second in the yard and 1516 in `o2match` (0.666
and 0.626 ms on the median, 99th percentile 0.975 and 1.284, 99.9th 1.276
and 1.726), the GPU 87 and 81 % busy, the game's main thread a whole core.
On this machine (Radeon 780M) uncapped at 1920x1080 is where the GPU
limits, and a frame then waits for the one before it on the GPU (input to
GPU done 1.47 against 0.98 ms in the yard): uncapped beyond what the GPU
can draw shows the newest input later, not earlier. Where the GPU keeps up
(640x480, fullscreen with tearing, 5 runs), uncapped is what the change
was for: 2083 frames a second in the yard and 1816 in `o2match` against
the 1000 before, input to display 0.936 (0.930 to 1.043) against 1.257
(1.163 to 1.335) ms and 1.062 (1.048 to 1.072) against 1.470 (1.316 to
1.504), p99 1.155 against 1.510 and 1.915 against 1.973, input to GPU
done 0.606 against 0.739 and 0.724 against 0.865; at a cap of 1000 the
same as before (1.216 against 1.247, 1.403 against 1.453). The game then
takes 1.12 CPUs (its main thread never waits).

View lists against the build before (`run.sh`, `tgadiff.py`, 640x480,
`r_vkValidation 1`, the 41 lists, 177 views, at `cl_cmdfps` 125: two
commands a frame under `fixedtime 16`): 143 views give the same frames to
the bit, among them every view of the `dm`, `obj` and multiplayer lists
but the snow of `mohdm5_fog` and the near-camera ±1 of `d7_alley` and
`d7_arch`, the menus, the dynamic light, decal, fog, sun flare and debug
drawing lists. The others are the random content and run-to-run noise
named in Testing (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`, `m3l1a`,
`m5l1a`, `m5l2a`, `m6l1a`, `mohdm5_fog`, a particle in `m53_street`, a
pixel of `m33_grass`) and the first view of two single-player lists,
`m32_house` (`m3l2`) and `m62_square` (`m6l2a`, 38 and 26 % of the pixels,
nearly all at edges): the eye is at the same place and the view is turned
by one unit of the 16-bit angle (0.0055°: the frames match shifted by 0.05
pixel; `viewpos` prints -59 against -60, the server's `coord` -60.00 in
both). `face` sets the view from the angles of the last command the server
got, and after the spawn that is another command now. No validation
message. With the final default (100 a second, 10 ms: one or
two commands a frame) on 9 of the lists again (`mohdm1`, `obj_team2`,
`mp_mohdm1`, `mp_obj_team4`, `menus`, `m3l2`, `m6l2a`, `mohdm1_dlight`,
`m2l3`, 48 views): the same, those two first views and the rest to the
bit. The final engine differs from the one these lists ran with in
`Com_Frame` alone (`objdump -d`, addresses left out: the timescale
condition and the `fps` counter) and in `cl_cmdfps`'s default.

Shots and jumps on the sub-grid (2026-10-24, Plan 38; the engine and
cgame; the `metal` branch's e38910e4, notuern's idea, the user's request:
"merging the subgrid thing from the metal port"): `cl_movesubgrid`
(archived, default 1). With `cl_movefps` on, a fire click (attack primary
or secondary) or a jump seen in the first half of a command's step takes
the next command to the half step (the newest command's time + half the
interval, rounded down: 5 ms at 100, 4 at 125), made by the first frame
there, and the commands after it stay on their grid: that step is split in
two (5 + 5 ms at 100, the steps of a stock client at 200 frames a second),
and the early command and the split's rest each go out at once in a packet
of their own (`CL_ReadyToSendPacket`: `cl_maxpackets` would hold a command
made half a step after the packet before). One early command a step; one
still to come when a frame is already at the grid's point is dropped, the
grid's command takes the event. Use and the movement keys stay on the grid
(on `metal` the movement keys needed two more rules and made four times as
many short steps; notuern: "Safe bet is shots and jumps only"). A click or
a jump is early when the frame before was still before the half point (it
came after that frame's input read). Unlike `metal`: here a key event
carries the time of the frame before's input read (`in_eventTime` of
`sdl_input.c`), so a command made by the frame that sees a jump already
has a share of the key (`CL_KeyState`) and a jump is early by the click's
rule; only one whose key event is in the frame's own millisecond (frames
under 1 ms apart), with the early command due in that frame, waits for the
grid. `metal`'s rule (early only when the frame that sees the jump is
before the half point) made 15 of 120 jumps early at 250 frames a second
here, whose frames between commands sit 4 to 7 and 8 to 11 ms past the
grid's point. cgame's open step takes the next command's time from
`GetLiveUserCmd` (the half or the rest while the sub-grid has one, whose
key shares the live command counts over that step), so the picture shows
the part gone of the shorter step. `cl_movesubgrid 0` is the grid as
before (the interval's step and count, no packet sent early), and
`cl_movefps 0` runs none of it.

Measured with a temporary engine (not kept): a dedicated server on
`dm/mohdm1` (a team match) logged every packet and every command its game
ran, with the time; the client, connected over UDP with
`cl_lanForcePackets 0` (so `cl_maxpackets` counts as on the internet),
pressed keys at random moments (fire, jump and forward in turn, 250 to 500
ms apart, held 60 to 160 ms, 180 presses a run, each queued as a key
event by the first frame's input read after its moment, as a real key)
and logged every command and packet; the runs interleaved, off, on, on,
off. From the press to the server running its command, ms, mean / p50 /
p95 / max (in brackets the presses whose command was on a short step):

| `com_maxfps`, `cl_movefps` | `cl_movesubgrid` | fire | jump | forward (on the grid in both) | short steps run |
| --- | --- | --- | --- | --- | --- |
| 250, 100 | 0 (2 runs) | 5.80 / 5.78 / 11.38 / 11.89 | 4.68 / 4.05 / 11.14 / 11.81 | 5.00 / 4.90 / 10.63 / 11.98 | 0 of 17,481 |
| | 1 (2 runs) | **2.59 / 2.42 / 6.55 / 7.40** (58 of 120) | **2.87 / 2.68 / 6.51 / 7.50** (58 of 120) | 5.59 / 5.54 / 11.21 / 11.99 | 232 of 17,527 (5 ms) |
| 250, 125 | 0 (2 runs) | 6.35 / 5.98 / 11.46 / 12.04 | 6.48 / 6.12 / 11.68 / 11.97 | 6.13 / 5.86 / 10.83 / 11.87 | 0 of 21,939 |
| | 1 (2 runs) | **2.53 / 2.23 / 5.97 / 8.22** (57 of 120) | **2.47 / 2.53 / 5.11 / 7.58** (67 of 120) | 4.83 / 5.04 / 8.65 / 11.44 | 248 of 22,184 (4 ms) |
| 1000, 100 | 0 | 5.51 / 6.28 / 9.72 / 11.34 | 5.71 / 5.77 / 9.64 / 10.36 | 5.12 / 5.49 / 9.27 / 10.86 | 0 of 8,755 |
| | 1 | **2.49 / 2.52 / 4.71 / 6.07** (27 of 60) | **2.75 / 2.79 / 4.90 / 5.06** (35 of 60) | 4.39 / 3.58 / 9.10 / 10.05 | 124 of 8,782 |
| 125, 100 | 0 | 4.93 / 4.15 / 12.72 / 15.50 | 5.92 / 5.66 / 13.77 / 15.17 | 5.44 / 4.42 / 12.40 / 13.44 | 0 of 8,825 |
| | 1 | 4.17 / 4.09 / 7.34 / 8.07 (11 of 60) | 4.17 / 4.19 / 7.88 / 8.02 (11 of 60) | 5.78 / 5.67 / 13.45 / 15.15 | 44 of 8,794 |

In every run the server ran every command the client made from the
first press on (in every build its game drops the commands of the first
second after the connection, while the client's clock settles), in steps
of the interval and its half only, and every early command and split's
rest went out at once. Forward, which stays on the grid, is the same
within the spread of 120 presses. At 125 frames a second the frames (8
ms) seldom fall in the first half of a 10 ms step: 11 of 60 presses
early. At `cl_movefps 125` the runs without the sub-grid held half of all
commands for a frame (made to sent: median 3.98 ms, 11,040 of 21,939 held
4 ms): `cl_maxpackets 125` counts real time, the time delta's corrections
bring the grid's commands 4 ms apart in real time now and then, and from
there every command waits a frame until a longer gap comes; with the
sub-grid's own packets 3,770 were held, so forward is faster there too.
At the default 100 (2 ms of margin) one command of 17,481 was held. Plan
40 changed that (below).

The cost is `metal`'s, measured again here with a temporary game (not
kept) that ran the server's own move (`Player::ClientMove`) with made-up
commands inside a player's think, its state put back after: `dm/mohdm2`'s
bumps C1, C3 and C7 (the table above), runs from 64 to 256 units before
each (every 2 units) and up to 12 units to either side (every 4),
forward and run toward the bump with a rifle; a run stopped when it got
no more than 32 units past the bump's point in 2.5 s. Each start ran
plain, with its step k split in two (a shot at that moment), and with a
jump pressed at step k (held 100 ms) on the grid and with that step split,
for every step k from the start to 6 steps past the lip (the step where
the plain run got within 3 units of where the plain runs that stop end):
850,645 runs, 150 million moves. Stopped, %, over every start and moment
alike (a shot or a jump at a random moment of the run), against the same
runs without the split; the extra points at each moment of the run
(mean, 95th percentile, the worst moment, in steps from the lip); the
runs whose outcome changed:

| bump, step | plain | shot, split | extra per moment | changed | jump: split, on the grid | extra per moment |
| --- | --- | --- | --- | --- | --- | --- |
| C1, 10 ms (5 + 5) | 54.9 | 54.9, 55.0 | -0.1, +0.6, +11.5 (+3) | 1.0 % | 54.8, 54.9 | -0.1, +1.4, +9.9 (+3) |
| C1, 8 ms (4 + 4) | 55.2 | 55.8, 55.7 | +0.3, +0.9, +12.7 (+4) | 0.9 % | 57.5, 57.4 | +0.1, +1.4, +13.8 (+4) |
| C3, 10 ms | 21.1 | 24.5, 22.9 | +1.9, +5.0, +11.4 (+1) | 10.3 % | 26.5, 26.0 | +0.5, +4.3, +7.0 (+1) |
| C3, 8 ms | 28.5 | 30.6, 29.3 | +1.2, +4.8, +9.2 (+1) | 13.2 % | 30.8, 30.7 | +0.2, +3.8, +5.9 (0) |
| C7, 10 ms | 0 | 0, 0 | 0 | 0 | 0, 0 | 0 |
| C7, 8 ms | 26.8 | 29.1, 26.0 | +3.3, +6.9, +10.5 (-63) | 18.2 % | 33.4, 34.1 | -0.8, +3.8, +7.4 (-64) |

As on `metal` (its sweep at 8 ms: C1 55.7 against 56.1, worst +15 at the
lip; C3 47.7 against 46.1; C7 29.9 against 35.4): a shot or a jump at a
random moment of a run stops it about as often as without (C1 the same,
C3 0.5 to 1.6 points more, C7 none at 10 ms and 3 points either way at 8
ms, where a small change of the path anywhere in the run changes one run
in five), and where its two short steps land on the lip the run stops up
to 10 to 14 points more often. No run of C7 stops with 10 ms steps, with
or without a split.


`cl_maxpackets` by the commands' own time (2026-10-24, Plan 40; the
engine; `metal`'s b5112fb9 has the same rule, the user's go after Plan 38
found the waits): with `cl_movefps` on, `CL_ReadyToSendPacket` holds a
packet with a new command only while that command's server time is less
than 1000 / `cl_maxpackets` past the server time of the last packet's
newest command, and sends at once when it is earlier (the grid started
again); a packet without a new command (reliable commands only) and
`cl_movefps 0` count real time as before. The grid's commands are a whole
step apart in server time, so at the defaults (`cl_maxpackets 125`, 8 ms;
`cl_movefps` 85 to 125, 8 to 11 ms) each goes out in the frame that makes
it. Counted in real time they didn't: a command made by a frame less than
8 ms after the packet before waited for the next frame, and the next
command then came less than 8 ms after that late packet, so the wait went
on until a longer gap came. At 144 frames a second the frames are 6.9 ms
apart, so with 10 ms steps every command whose frame came one frame after
the last command's waited; at 250 with 8 ms steps the time delta's
corrections now and then put two commands one frame (4 ms) apart. The
press test of Plan 38 (the same temporary builds, the build before and
this one interleaved), from the press to the server running its command,
ms, mean / p95:

| `com_maxfps`, `cl_movefps`, `cl_movesubgrid` | build | commands held for a frame | fire | jump | forward |
| --- | --- | --- | --- | --- | --- |
| 144, 100, 1 (2 runs) | before | 8,834 of 17,741 (7 ms) | 6.12 / 13.31 | 5.66 / 13.14 | 9.99 / 17.53 |
| | Plan 40 | 0 of 17,376 | **3.10 / 6.62** | **3.44 / 6.67** | **5.08 / 12.33** |
| 144, 100, 0 | before | 4,370 of 8,748 | 10.24 / 19.91 | 9.12 / 17.73 | 8.75 / 18.01 |
| | Plan 40 | 0 of 8,705 | **6.30 / 12.89** | **6.13 / 12.51** | **5.85 / 12.85** |
| 250, 125, 1 (2 runs) | before | 10,473 of 22,144 (4 ms) | 3.37 / 7.68 | 3.11 / 7.34 | 5.47 / 9.94 |
| | Plan 40 | 0 of 22,041 | **2.00 / 3.95** | **2.13 / 3.90** | **3.74 / 7.54** |
| 250, 125, 0 | before | 9,180 of 10,929 | 7.57 / 11.32 | 7.01 / 11.59 | 7.47 / 10.94 |
| | Plan 40 | 0 of 10,863 | **3.84 / 7.70** | **4.66 / 7.66** | **4.08 / 7.87** |
| 250, 100, 1 | Plan 40 | 0 of 8,908 | 2.96 / 7.54 | 3.42 / 7.59 | 5.24 / 10.11 |

The last row is Plan 38's 250 / 100 again within the spread of 60
presses (nothing was held there before either). Before, a held command
went out with the next one in a packet of two (4,967 of 12,780 packets at
144); now every packet has one new command, and the server ran every
command in every run.

Submit thread (2026-10-19, Plan 19; see Implementation notes, Submit
thread). JayRewd's commit a5f5a54a drew every frame offscreen and handed
it to a thread that acquired a swapchain image, copied the frame into it,
submitted and presented, with 8 frames in flight and at least 5 swapchain
images, and `r_vkSubmitThread 1` by default. A tester (RTX 4070 Ti,
2560x1440, fullscreen immediate, uncapped) measured 597 against 1074
frames a second at the 99.9th percentile, and the slow frames it removed
were an acquire of about 1.3 ms every 341 frames; on RADV it ran at 373
frames a second against 470 (the copy). Here the 8 frames in flight added
0.5 ms of input to GPU done at 3840x2160 even with the thread off, and when
the GPU limits each frame in flight is a frame of lag. Taken (the user's
decision): the thread and the 5 images, frames in flight cut back to 2 or
3 (2 measured better, below), off by default. What differs from his
version, each measured:

- The thread acquires the next frame's image right after its present (as
  `vk_end_frame` does without it), and a frame is drawn into that image
  when the thread has it by the frame's first render pass, so frames are
  not copied: in fullscreen 0.01 to 0.9 % of the frames uncapped (the
  acquire not done yet; 3.5 to 5 % in a window at 2500 frames a second)
  and none at 250 are drawn offscreen and copied. Always copying
  (`r_vkSubmitThread 2`, his way) costs 0.40 to 0.57 ms of GPU time per
  frame at 3840x2160: 23 to 30 % of the frame rate where the GPU limits,
  and 1.2 to 1.8 ms more input to display than without the thread (2.4 to
  3 ms more than drawing into the acquired image). His order (acquire,
  then the frame and the copy in one submission, then the present) also
  put the acquire between the hand-off and the present: input to present
  +0.04 ms at 640x480 in the first version measured here, now -0.01 to
  +0.02.
- 2 frames in flight, waited for at the end of the frame, before the next
  frame's input (see Implementation notes). With 3, uncapped frame rates
  where the CPU and GPU are about even were higher (the `obj_team2` yard at
  1920x1080: 1629 against 1432 frames a second), but uncapped every view
  gained 0.1 to 0.3 ms of input to display at 1920x1080, and 1.2 to 2.1
  ms of input to GPU done at 3840x2160, where the GPU limits (a frame
  more queued); at 250 the same.
- With FIFO the frame just handed over is waited for too (its job ends
  with the acquire, where the thread waits for the display), and the
  swapchain has 3 images, against the surface's minimum + 1 (4 here)
  without the thread.

Measured on the idle machine (other processes at most 1.4 CPUs, no Plex
transcoding), with the build before (`base`, b19079eb), this build with
`r_vkSubmitThread 0` (`off`) and with 1 (`thread`), interleaved, medians
of 5 runs of 10 s (10000 frames uncapped, 2500 at 250); input read to
present, GPU done and display from
`libinlat.so` with `INLAT_DISPLAY=1` (`framelog.py --display`: the median
of the runs' medians and of their 99th percentiles for input to present,
the runs pooled for the others); frame time percentiles from
`renderer_timed.so`; ms. Fullscreen immediate (tearing, `INLAT_FBPOLL_US
=250`), the user's mode, at 640x480 (CPU-bound, about 2000 frames a
second):

| view | `com_maxfps` | frames a second: base / off / thread | frame time p99, p99.9: base / off / thread | input to present, median: base / off / thread | input to display, median, p99, p99.9: base / off / thread |
| --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 1823 / 1796 / 2009 | 0.986, 1.367 / 0.996, 1.338 / 1.015, 1.281 | 0.463 / 0.472 / 0.479 | 1.06, 1.94, 2.48 / 1.08, 2.00, 2.58 / 1.07, 1.99, 2.50 |
| | 250 | 250 | 4.029, 4.307 / 4.028, 4.372 / 4.029, 4.308 | 0.507 / 0.506 / 0.512 | 1.76, 2.38, 3.06 / 1.65, 2.40, 3.14 / 1.64, 2.48, 3.10 |
| `o4match` | 0 | 1176 / 1160 / 1299 | 1.346, 1.779 / 1.432, 1.889 / 1.275, 1.647 | 0.797 / 0.803 / 0.783 | 1.72, 2.78, 3.83 / 1.74, 2.80, 3.82 / 1.69, 2.66, 3.58 |
| | 250 | 250 | 4.038, 4.647 / 4.035, 4.656 / 4.035, 4.677 | 0.801 / 0.812 / 0.817 | 1.96, 3.11, 4.16 / 1.95, 2.80, 3.68 / 2.01, 2.77, 3.51 |
| `obj_team2` yard | 0 | 1999 / 2004 / 2418 | 0.614, 0.987 / 0.673, 1.010 / 0.542, 0.884 | 0.428 / 0.427 / 0.414 | 0.97, 1.40, 1.78 / 0.96, 1.34, 1.73 / 0.87, 1.23, 1.49 |
| | 250 | 250 | 4.106, 4.157 / 4.105, 4.148 / 4.109, 4.140 | 0.503 / 0.515 / 0.524 | 1.58, 2.27, 2.78 / 1.63, 2.38, 3.16 / 1.62, 2.36, 2.86 |

At 1920x1080 (fullscreen immediate, where the yard's GPU is about as busy
as its CPU):

| view | `com_maxfps` | frames a second: base / off / thread | frame time p99, p99.9: base / off / thread | input to GPU done, median, p99: base / off / thread | input to display, median, p99, p99.9: base / off / thread |
| --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 1498 / 1516 / 1549 | 1.311, 1.715 / 1.278, 1.657 / 1.230, 1.557 | 1.18, 2.77 / 1.19, 2.77 / 1.20, 2.40 | 1.53, 3.15, 3.52 / 1.53, 3.14, 3.53 / 1.55, 2.78, 3.05 |
| | 250 | 250 | 4.027, 4.253 / 4.028, 4.278 / 4.028, 4.282 | 1.32, 2.07 / 1.29, 2.01 / 1.33, 2.13 | 1.98, 2.77, 3.17 / 1.92, 2.73, 3.15 / 1.98, 2.80, 3.15 |
| `o4match` | 0 | 947 / 943 / 995 | 1.739, 2.217 / 1.733, 2.219 / 1.648, 2.220 | 1.78, 2.63 / 1.78, 2.60 / 1.74, 2.61 | 2.20, 3.17, 3.80 / 2.20, 3.13, 3.79 / 2.11, 3.16, 3.75 |
| | 250 | 250 | 4.036, 4.593 / 4.036, 4.590 / 4.036, 4.565 | 1.85, 2.74 / 1.85, 2.82 / 1.86, 2.72 | 2.50, 3.50, 4.25 / 2.50, 3.56, 4.21 / 2.55, 3.43, 4.55 |
| `obj_team2` yard | 0 | 1520 / 1526 / 1432 | 0.954, 1.248 / 0.930, 1.194 / 0.890, 1.185 | 1.45, 1.67 / 1.46, 1.68 / 1.41, 1.52 | 1.73, 2.05, 2.16 / 1.75, 2.06, 2.15 / 1.71, 1.90, 1.99 |
| | 250 | 250 | 4.101, 4.130 / 4.104, 4.134 / 4.100, 4.125 | 1.36, 2.14 / 1.38, 1.77 / 1.39, 1.74 | 1.96, 2.69, (16.7) / 2.01, 2.61, 2.88 / 1.99, 2.50, 2.83 |

(The 16.7 ms of the base build's yard at 250 is one frame shown a refresh
late in one run.) At 3840x2160 in a window (the GPU limits uncapped; input
to display through the compositor, about 11 ms more), with `r_vkSubmitThread
2` (`copy`: every frame drawn offscreen and copied, JayRewd's way) and with
3 frames in flight (`3 frames`):

| view | `com_maxfps` | frames a second: base / off / thread / copy / 3 frames | frame time p99, p99.9: base / thread / copy | input to GPU done, median, p99: base / off / thread / copy / 3 frames | input to display, median: base / off / thread / copy / 3 frames |
| --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 729 / 729 / 728 / 512 / 729 | 3.103, 3.598 / 3.038, 3.450 / 3.822, 4.265 | 3.15, 8.07 / 3.15, 8.07 / 2.51, 6.07 / 3.63, 7.62 / 3.75, 9.07 | 13.4 / 13.2 / 12.2 / 14.6 / 15.1 |
| | 250 | 250 | 4.029, 4.341 / 4.030, 4.328 / 4.028, 4.620 | 2.60, 4.25 / 2.63, 4.27 / 2.60, 4.03 / 3.16, 4.57 / 2.54, 4.09 | 11.3 / 11.4 / 11.2 / 12.6 / 11.3 |
| `o4match` | 0 | 466 / 466 / 466 / 357 / 466 | 3.163, 4.492 / 2.961, 4.200 / 3.716, 4.760 | 5.20, 7.62 / 5.20, 7.62 / 4.21, 5.82 / 5.54, 7.36 / 6.31, 8.73 | 14.9 / 15.0 / 13.7 / 16.7 / 17.5 |
| | 250 | 250 | 4.043, 4.655 / 4.046, 4.762 / 4.091, 5.315 | 3.55, 4.83 / 3.57, 4.92 / 3.58, 4.75 / 3.78, 7.37 / 3.58, 4.81 | 13.9 / 13.0 / 13.1 / 14.4 / 13.1 |
| `obj_team2` yard | 0 | 590 / 590 / 590 / 438 / 590 | 2.144, 2.664 / 1.981, 2.471 / 2.603, 3.043 | 4.54, 4.76 / 4.53, 4.75 / 3.36, 3.56 / 4.54, 4.77 / 5.05, 5.29 | 12.4 / 12.3 / 11.2 / 14.1 / 13.2 |
| | 250 | 250 | 4.094, 4.114 / 4.098, 4.116 / 4.102, 4.148 | 3.27, 3.61 / 3.31, 3.71 / 3.29, 3.65 / 3.35, 4.14 / 3.31, 3.64 | 11.8 / 12.0 / 12.2 / 12.5 / 11.7 |

- Uncapped where the CPU limits (640x480, the 1920x1080 demos) the thread
  takes the submission, the present and the acquire off the main thread
  (`EndFrame` 0.074 to 0.009 ms): 3 to 21 % more frames a second, lower
  99th and 99.9th percentiles of the frame time in most views, and input
  to display the same or lower (the yard at 640x480: 0.97 to 0.87 ms on
  the median, 1.78 to 1.49 at the 99.9th percentile).
- Where the GPU limits (3840x2160, uncapped) the frame rate is the same,
  and input to GPU done is 1.0 to 1.2 ms lower on the median and 1.2 to 2
  ms at the 99th percentile: the main thread waits for the GPU at the end
  of the frame, before the next input, where without the thread it waits
  after the input, at the next frame's first command. Where the CPU and GPU
  are about even (the yard at 1920x1080) that wait costs frame rate (1432
  against 1520 frames a second: the engine's work between frames no
  longer overlaps the GPU's) for lower input to GPU done and display
  (p99 1.90 against 2.05 ms).
- At 250 (the user's cap) the frames are the same (the cap sets them);
  input to present is 0.005 to 0.02 ms longer on the median (the thread
  wakes up 10 to 12 µs after the hand-off when it slept a whole frame, 7
  µs uncapped), input to GPU done and to display the same within the run to
  run spread, both ways.
- CPU: the thread uses 0.05 to 0.2 of a core uncapped (the game 1.27
  against 1.13 CPUs in `o2match` at 640x480), nothing measurable at 250
  (0.45 and 0.53 CPUs with and without it).

FIFO (`r_swapInterval 1`, 640x480 fullscreen, the 60 Hz display, 600
frames, the yard and `o2match` at caps 0 and 250, 5 runs each): input to
display 33.8 ms on the median (99.9th percentile 33.9) against 50.5 (50.6)
for the build before and `off`: one frame less queued (3 images, the
surface's minimum, against 4). Input to present and to GPU done 0.04 to
0.07 ms longer (0.828 against 0.775 and 1.37 against 1.30 in `o2match`;
the thread sleeps a whole refresh and wakes up in 21 to 29 µs after the
hand-off), the frame times the same (16.67 ms, p99.9 17.6), no frame
copied.

Mailbox (`INLAT_PRESENT_MODE=mailbox`, 1920x1080 fullscreen, direct
scanout), 5 swapchain images against 3 with the thread: with 3 the thread
hadn't acquired an image by the first render pass in 15 % of the frames
uncapped (copied), and the tails were longer; with 5, `o2match` uncapped ran
at 1772 frames a second against 1483 without the thread (1710 with 3),
frame time p99 and p99.9 1.048 and 1.331 ms against 1.573 and 2.008, input
to display 5.18 ms on the median against 5.16 and 9.81 against 12.73 at
the 99.9th percentile; the yard 1870 against 1634 frames a second, p99 0.696
against 1.135; at 250 the same frame times and input to display (6.81
against 6.88 ms). Mailbox keeps 5 images.

Where the acquire waits went (`r_vkFrameLog`, the two demos uncapped, 3
runs of 15000 frames each): at 3840x2160 in a window without the thread
the main thread's acquire after the present waited more than 0.3 ms in 15
to 67 frames per run (up to 1.8 ms; Xwayland not releasing an image yet);
with the thread the main thread never waits for it (its longest wait for
the thread 0.14 ms), and the thread's own acquire waited more than 0.3 ms
in 0 to 10 frames per run (5 images). At 640x480 neither waits. The
frame log's `thread_*` columns give the thread's times per frame: the
hand-off to its start 7 µs on the median (99.9th percentile 34 to 41 µs),
`vkQueueSubmit` 20 to 26 µs, the acquire 26 to 30 µs, the present 10 to
12 µs.

The NVIDIA case, where the tester's acquire stalled 1.3 ms every 341
frames, wasn't measured here: input to display there, with and without the
thread, would need the tester's runs (`libinlat.so` with `INLAT_DISPLAY=1`
works on any Vulkan driver under X11 or Wayland).

Redundant state and empty flushes (2026-10-20, Plan 20). From JayRewd's
b26d8a67 (one of its steps: "leave viewport, scissor, projection,
modelview and fog range alone when the value is unchanged, and return
from an empty render-command flush"), as he wrote it but without the
hooks of his 2D batching, which isn't taken. The back end sets the
viewport, the scissor, the projection, the modelview and the fog range for
every batch and every 2D call (`Set2DWindow`, `RB_SetGL2D`, the entity
changes of `RB_RenderDrawSurfList`, each static model of a merged batch),
mostly to the values they have, and each set marked the dynamic state or
the vertex push constants to be set again by the next draw
(`vkCmdSetViewport`, the projection times modelview product,
`vkCmdPushConstants`); the setters now return when the value doesn't
change. And every 2D export (`DrawStretchPic`, `DrawString`, ...) first
flushes the render commands (`R_IssuePendingRenderCommands`), which with
none queued still ran the back end's command loop and its two reads of the
clock; it now returns. Counted per frame with a temporary build (skipped of
all calls; the fixed views of `perf.py` have no HUD):

| view | empty flushes | viewport | scissor | projection | modelview | fog range |
| --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 120 of 122 | 9 of 47 | 52 of 92 | 12 of 49 | 87 of 184 | 46 of 46 |
| `o4match` | 124 of 126 | 9 of 45 | 51 of 90 | 12 of 47 | 91 of 362 | 44 of 46 |
| `obj_team2` yard | 7 of 9 | 3 of 5 | 7 of 9 | 1 of 7 | 4 of 110 | 4 of 4 |
| `dm/mohdm3` overview | 7 of 9 | 3 of 5 | 7 of 9 | 1 of 7 | 19 of 380 | 4 of 6 |

Why the frames can't change: what a setter marks is applied by the next
draw, so a value equal to the one set before is applied already or will
be; every command buffer starts with everything marked (also the new one
after a mid-frame flush); and the one place that changes the state behind
the setters, the color clear drawn as a quad (`VK_ClearColor`), marks it
when it restores it. An empty flush did nothing but set `backEnd.pc.msec`,
which only `RE_EndFrame` reads, after a flush that is never empty (it
holds the swap). The frames are the same to the bit (see Testing).

Results (idle machine: other processes 0.7 CPUs at most, no Plex
transcoding; the build before (`base`, d00f1b1d), the setters alone
(`skip`), the flush alone (`flush`) and both (`new`) interleaved, medians
of 5 runs, 640x480 uncapped; the fixed views 5000 frames, the demos 15000;
ms; the frame time percentiles are the medians of the runs'):

| view | renderer base / skip / flush / new | 2D and back end base / new | frame time base / new (fps) | p99 base / new | p99.9 base / new |
| --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 0.358 / 0.350 / 0.358 / 0.352 | 0.189 / 0.182 | 0.436 / 0.432 (2292 / 2312) | 0.529 / 0.559 | 0.906 / 0.900 |
| `dm/mohdm3` overview | 0.452 / 0.437 / 0.447 / 0.431 (-4.6 %) | 0.275 / 0.255 | 0.513 / 0.495 (1949 / 2021) | 0.618 / 0.578 | 1.005 / 0.999 |
| `o2match` | 0.332 / 0.322 / 0.324 / 0.310 (-6.6 %) | 0.204 / 0.182 | 0.459 / 0.437 (2179 / 2286) | 0.911 / 0.877 | 1.285 / 1.171 |
| `o4match` | 0.684 / 0.657 / 0.674 / 0.656 (-4.1 %) | 0.411 / 0.384 | 0.856 / 0.824 (1168 / 1213) | 1.326 / 1.282 | 1.694 / 1.586 |

(The runs' ranges of the renderer time don't overlap between `base` and
`new` but in the yard: 0.354 to 0.362 against 0.349 to 0.359; the yard's
99th percentile spreads from run to run, 0.512 to 0.558 against 0.515 to
0.606, and the frame by frame medians over the runs give 0.481 against
0.480. JayRewd measured -12, -15 and 0 µs in the yard, `o2match` and
`o4match` with his renderer under this engine.) The two parts add up
roughly, the flush mostly in the demos, where the HUD makes the 2D calls;
the front end doesn't change (it calls none of these). In the demos'
99th and 99.9th percentiles the runs' ranges of `o4match` don't overlap
(1.307 to 1.332 against 1.241 to 1.295, 1.628 to 1.761 against 1.564 to
1.608); `o2match`'s 99.9th percentile spreads more (1.204 to 1.352
against 1.098 to 1.284).

At 1920x1080 in a window, uncapped and at the user's cap of 250 (`perf.py
--caps 0,250 --seconds 10 --preload libinlat.so --framelog`, 5 runs; input
read to present from `libinlat.so`, the median of the runs' medians with
their range, and of their percentiles; ms):

| demo | `com_maxfps` | renderer base / new | frames a second | frame time p99, p99.9: base / new | input to present, median (runs): base / new | p99, p99.9: base / new |
| --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 0.453 / 0.418 | 1605 / 1719 | 1.130, 1.492 / 1.107, 1.460 | 0.526 (0.522 to 0.535) / 0.485 (0.480 to 0.506) | 1.084, 1.448 / 1.055, 1.407 |
| | 250 | 0.451 / 0.430 | 250 | 4.029, 4.346 / 4.030, 4.306 | 0.543 (0.538 to 0.565) / 0.514 (0.509 to 0.538) | 0.951, 1.517 / 0.927, 1.372 |
| `o4match` | 0 | 0.817 / 0.761 | 986 / 1043 | 1.641, 2.119 / 1.581, 2.057 | 0.946 (0.928 to 0.955) / 0.897 (0.885 to 0.908) | 1.591, 2.104 / 1.518, 2.041 |
| | 250 | 0.748 / 0.727 | 250 | 4.037, 4.600 / 4.037, 4.627 | 0.878 (0.874 to 0.906) / 0.868 (0.861 to 0.872) | 1.461, 2.028 / 1.429, 2.059 |

The saved work lies between the frame's input read and its present, so
input to present goes down by about as much (0.01 to 0.05 ms on the
median); nothing else changes in the frame (no wait, no frame more in
flight, the acquire where it was).

The view cluster: `RE_BeginRegistration` resets `tr.viewCluster` (GL1's
"force markleafs to regenerate"), but the client doesn't always register
before loading a world. `CL_StartHunkUsers` calls `CL_BeginRegistration`
only when `cls.rendererRegistered` is false, which `CL_FlushMemory` sets
on a local map load, on a disconnect and when a gamestate arrives while
cgame runs; a `connect` to another server during a game goes through
`CL_Disconnect`, which stops cgame first, so the new gamestate arrives
with cgame stopped, nothing is flushed and cgame loads the new world
(`R_LoadWorldMap`) with the old map's last cluster in `tr.viewCluster`.
When the new map's first view is in a cluster of that number (and the
area mask is the same), `R_MarkLeaves` keeps what it has: in GL1 no node
of the new world is marked, here no leaf is listed, and no world surface
or sky is drawn until the view's cluster changes. A dedicated server
(`omohaaded`, `dm/mohdm1`, free for all) and a client that connected to
it 25 times in one session, spectating at a random spawn point, with a
temporary build that printed the cluster at each load: no
`RE_BeginRegistration` after the first; with the old code one connect of
24 found its old cluster again (269) and drew only the models, on black
(49 % of its pixels black, 1 to 14 % in the other shots); with
`RE_LoadWorldMap` resetting the cluster, as JayRewd's b26d8a67 does, every
first view lists its leaves.

Effects shader (2026-10-20, Plan 21; see Implementation notes, Effects
shader). The static batches whose shaders deform vertexes or compute
texture coordinates or colors per vertex went through `tess`, the CPU path
(`RB_DeformTessGeometry`, `ComputeTexCoords`, `ComputeColors`, the copy of
every vertex into the frame's vertex buffer); JayRewd's b26d8a67 moved
them to a vertex shader of their own. Draws per frame that take the
effects shader now (`vkinfo`'s `effectsDraws`, 640x480; of the draws from
the static geometry, with how many those were before): 130 of 225 in the
`m6l1a` forest (95 before), 63 of 85 in the `m3l3` path (22), 110 of 329 in
the `dm/mohdm3` overview (214), 54 of 138 in the `obj_team2` yard (84); the
vertex bytes copied per frame go from 328 to 137 KB in the forest, 134 to
98 KB in the path, 244 to 175 KB in the overview, 204 to 162 KB in the
yard. The draws are the same; the CPU work per batch is a record of 128
words instead of the vertex loops.

Results (idle machine: other processes 0.7 CPUs at most, no Plex
transcoding; the build before (`base`, 52b00a45) and this one interleaved,
medians of 5 runs with their range, 640x480 uncapped; the fixed views 5000
frames, the demos 15000; ms):

| view | renderer base | new | 2D and back end base / new | frame time base / new (fps) | p99 base / new | p99.9 base / new |
| --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest | 0.483 (0.471 to 0.491) | 0.430 (0.424 to 0.435), -11 % | 0.277 / 0.223 | 0.632 / 0.581 (1581 / 1722) | 0.904 / 0.828 | 1.173 / 1.086 |
| `m3l3` path | 0.312 (0.308 to 0.316) | 0.298 (0.296 to 0.301), -4.5 % | 0.177 / 0.168 | 0.504 / 0.482 (1986 / 2076) | 0.799 / 0.762 | 0.975 / 1.035 |
| `dm/mohdm3` overview | 0.436 (0.429 to 0.438) | 0.416 (0.412 to 0.420), -4.6 % | 0.256 / 0.240 | 0.500 / 0.481 (2000 / 2078) | 0.591 / 0.554 | 1.001 / 0.972 |
| `obj_team2` yard | 0.349 (0.345 to 0.354) | 0.343 (0.335 to 0.353), -1.7 % | 0.181 / 0.171 | 0.429 / 0.422 (2330 / 2370) | 0.525 / 0.514 | 0.908 / 0.903 |
| `o2match` | 0.313 (0.310 to 0.318) | 0.303 (0.302 to 0.312), -3.2 % | 0.183 / 0.176 | 0.441 / 0.431 (2270 / 2322) | 0.876 / 0.874 | 1.211 / 1.289 |
| `o4match` | 0.648 (0.644 to 0.658) | 0.634 (0.630 to 0.639), -2.2 % | 0.380 / 0.364 | 0.815 / 0.801 (1226 / 1248) | 1.260 / 1.269 | 1.628 / 1.615 |

(Each frame's median over the 5 runs, the demos' "content" of Frame spikes:
`o2match` p99 and p99.9 0.853 and 1.137 against 0.839 and 1.120 ms, the
raw 99.9th percentile above being one run's; `o4match` 1.231 and 1.537
against 1.221 and 1.551. JayRewd measured -2 to -17 µs in the matches,
-27 µs in the overview and -65 µs in the forest with his renderer under
this engine.) The front end doesn't change; the saved time is in the back
end (the 2D calls, where the 3D view's batches are drawn).

At 1920x1080 in a window, uncapped and at the user's cap of 250 (`perf.py
--caps 0,250 --seconds 10 --preload libinlat.so --framelog`, 5 runs; ms;
input read to present from `libinlat.so`, the medians of the runs'
percentiles):

| demo | `com_maxfps` | renderer base / new | frames a second | input to present, median: base / new | p99, p99.9: base / new |
| --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 0.417 / 0.408 | 1703 / 1739 | 0.489 / 0.473 | 1.078, 1.470 / 1.056, 1.414 |
| | 250 | 0.434 / 0.417 | 250 | 0.521 / 0.505 | 0.954, 1.345 / 0.897, 1.249 |
| `o4match` | 0 | 0.762 / 0.748 | 1044 / 1049 | 0.890 / 0.889 | 1.523, 2.045 / 1.522, 2.037 |
| | 250 | 0.716 / 0.695 | 250 | 0.853 / 0.841 | 1.451, 1.905 / 1.400, 2.021 |

The work taken off lies between the frame's input read and its present, so
input to present goes down by about as much; nothing else of the frame
changes (no wait, no frame more in flight, the acquire where it was).

The GPU: the vertexes the effects shader computes cost nothing measurable
where the GPU limits (3840x2160, 3 runs, GPU time of the main command
buffer from timestamps, ms): the `m6l1a` forest 3.982 / 3.977, the
`obj_team2` yard 1.678 / 1.677, the `dm/mohdm3` overview 2.447 / 2.447; the
frames 4.025 / 4.019, 1.694 / 1.694, 2.473 / 2.474 ms.


cgame caches (2026-10-20, Plan 22; cgame, both renderers). From JayRewd's
b26d8a67, as he wrote it but for the structure of the code:

- Shadow traces: `CG_EntityShadow` traces from each shadowed entity down to
  the ground every frame (`cg_shadows` 1 and 2), against the world only
  (brush model 0), so the trace is the same as long as the entity's origin
  is: each entity keeps its last trace and the origin it was made from
  (`cg.shadowTraces`), and an entity that hasn't moved takes it.
- Temporary marks (the blob and feet shadows, temporary decals): projected
  onto the world's brushes and terrain every frame, which don't move; a mark
  made again in the same server snapshot with the same shader, origin,
  direction, orientation and size takes the fragments it got before (16
  slots), and its verts when its colors are the same too. A mark with a
  fragment on a brush entity is built every frame; one without keeps its
  fragments for the rest of the snapshot (50 ms at the usual 20 snapshots a
  second), so a brush entity that moves under it within the snapshot, a
  door, only gets its part of the shadow from the next one. The leaf lookup
  that only kept marks use is left out for temporary ones.
- The count of active temporary models: `FreeSomeTempModels` walked the
  active list every frame to count them; `AllocateTempModel` and
  `FreeTempModel` keep the count (a loaded save counts once).
- A conversion of every sprite temporary model's axes to Euler angles whose
  result nothing read (`MatrixToEulerAngles` in `AddTempModels`).

`cg_shadows` is 0 by default, and then only the last two run. What the
caches skip was checked with a temporary cgame that did it anyway and
compared (see Testing): the same results in 20 matches. Results (idle
machine: other processes 0.4 to 0.6 CPUs, no Plex video transcoding; the
build before and the new cgame, each part alone and all four, interleaved,
the Vulkan renderer of the build before, `o2match` and `o4match` at 640x480
uncapped, 15000 frames; cgame's time per frame from `renderer_timed.so`'s
frame files, the part of the frame between `BeginFrame` and `EndFrame`
outside the renderer: the UI and cgame building the scene; µs, the median of
the runs' means with their range):

| demo | `cg_shadows` | before | count | Euler angles | shadow traces | temporary marks | all four (10 runs) | before (10 runs) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 100.1 (97.4-101.8) | 99.2 | 97.2 | | | 95.7 (94.8-98.8) | 97.8 (96.3-101.8) |
| | 1 | 98.9 (97.1-101.5) | | | 97.7 | 98.3 | 95.4 (93.2-98.8) | 98.9 (96.9-101.5) |
| `o4match` | 0 | 138.5 (135.5-142.4) | 138.9 | 139.3 | | | 135.1 (132.2-138.5) | 138.4 (135.5-142.4) |
| | 1 | 137.6 (136.4-138.8) | | | 139.1 | 136.9 | 133.9 (131.5-137.1) | 137.7 (135.8-140.1) |

(Each part on its own in the first batch of 5 runs; all four and the build
before in that batch and a second one, 10 runs each.) Each part alone is
within the run to run spread of 3 to 6 µs; all four take 2 to 4 µs off
cgame's time per frame (2 to 3.5 %), 4 to 9 µs off its 99th percentile, as
JayRewd measured (-3 to -5.5 µs). The renderer's time is the same (0.304
to 0.312 ms in `o2match`, 0.626 to 0.640 in `o4match`). In the 20 matches
of the check about a fifth of the shadow traces and a tenth (`cg_shadows`
1) to a sixth (2) of the temporary marks came from the caches (at 16 ms
frames; more at higher frame rates, which make more frames per snapshot). At 1920x1080 in
a window, uncapped and at 250, with the skinned passes below (5 runs, input
read to present from `libinlat.so`, ms, base / new): `o2match` uncapped
0.484 / 0.483 (p99 1.069 / 1.064), at 250 0.516 / 0.513 (0.980 / 0.957);
`o4match` uncapped 0.875 / 0.881 (1.537 / 1.538), at 250 0.849 / 0.851
(1.452 / 1.448): the same within the runs' ranges, which overlap. Nothing waits or is queued for longer; the work taken off lies
between the input read and the present.

Skinned passes (2026-10-20, Plan 23; see Implementation notes, Skeletal
models). Since step 3 the batches of the vertex-lit iterator are skinned
and lit in the vertex shader; the other skeletal model shaders were skinned
on the CPU. JayRewd's a5f5a54a also took the generic iterator's shaders
whose passes are one texture or an environment map with the sphere's
lighting or a constant color (`skinned.vert` computing the environment
map's coordinates with `inversesqrt`, which moved a pixel of `mp_mohdm1`):
taken, with `Q_rsqrt`'s operations instead (the same coordinates as the
CPU's, so the same frames), the skin data and the pipelines of these
shaders at load like the vertex-lit ones' (Pipelines and skin data at
load), `lightingGrid` stages lit by the light grid as `ComputeColors` does
(his took the sphere's lights for them when it had some), and stages with
an alpha of their own when the color is one constant (his took none:
`lightingGrid`, or `lightingSpherical` without lights). In the view lists
and four real matches (counted with a temporary copy) these are few shaders:
the Afrika Korps goggles, the tank commander's coat, hat and gloves, the
Focke-Wulf's windows, bullet-hit debris and splash drops, sniper lenses, a
caged light; what stays on the CPU among models is mostly effects with
vertex alphas, `distFade`, waves, `tcMod`s or deforms (smoke, muzzle
flashes, health packs, vehicle treads and wheels, fire). Results (idle
machine, other processes 0.4 to 0.6 CPUs but one run at 1.9; the build
before and this one interleaved, 640x480 uncapped, 5 runs, the fixed views
1000 frames, the demos 15000; the renderer's main-thread time per frame,
ms):

| view | renderer before | after | 2D and back end before / after | frame time before / after (fps) |
| --- | --- | --- | --- | --- |
| `sp_front` (`skin_passes`) | 0.431 (0.422-0.438) | 0.382 (0.380-0.385), -11 % | 0.295 / 0.244 | 0.520 / 0.473 (1922 / 2113) |
| `sp_lit` (with 2 dynamic lights) | 0.482 (0.474-0.490) | 0.414 (0.408-0.424), -14 % | 0.333 / 0.266 | 0.573 / 0.506 (1745 / 1975) |
| `pm1_front` | 0.541 (0.529-0.550) | 0.534 (0.530-0.538) | 0.318 / 0.317 | 0.680 / 0.673 |
| `po4_front` | 0.386 (0.380-0.398) | 0.391 (0.386-0.401) | 0.175 / 0.176 | 0.535 / 0.542 |
| `o2match` | 0.308 (0.303-0.310) | 0.307 (0.306-0.309) | 0.179 / 0.179 | 0.437 / 0.435 |
| `o4match` | 0.634 (0.629-0.642) | 0.635 (0.631-0.641) | 0.364 / 0.365 | 0.803 / 0.806 |

Where these models are drawn the CPU's skinning of their surfaces is gone
(`skin_passes`: 11 to 14 %); in the two matches they are too few to measure
(JayRewd measured -3 to -5 µs there). The GPU time at 3840x2160 is the
same (`sp_front` 3.168 / 3.163 ms, `sp_lit` 3.165 / 3.163, `pm1_front`
3.863 / 3.867). With cold caches (`perf.py --cold`, `o4match` and an
`obj/obj_team1` match) the load creates 16 pipelines more (the skinned
draws of these shaders, about 40 ms of a cold load), the match the same 2
as before (the head icons).

Dynamic lights, 64 of them (2026-10-20, Plan 24; see Implementation notes,
Dynamic lights). From JayRewd's b26d8a67 ("Scenes hold 64 dynamic lights
with 64-bit masks... Brush models light every lightmapped surface per pixel
with all their lights"), with the user's limit: `r_maxDlights` (latched,
default 4, 0 to 64), the lights a scene keeps, the nearest the view. In
real matches no frame has more than 4 lights (counted on 2026-10-19 over 10
matches, 220 thousand frames), so at the default nothing is left out there;
of the 45 view lists only `m3l1a`'s scenes have more (6 at most, the beach
landing; counted with a temporary build).
What changed, and how it differs from his:

- The masks over the scene's lights are 64 bits (`dlightMask_t`) in the
  surfaces, `tess` and the front end's tests. A face held two arrays of
  two 32-bit masks (`[SMP_FRAMES]`, for a second render thread this
  renderer never had: only element 0 was used); it holds one 64-bit mask
  each now, in the same 16 bytes at the same place, so the face's layout
  doesn't change (the spec's concern: a layout that grew the hot face
  data had cost more than the work it saved before).
- The shader's mask stays 32 bits (the push constants are full). His
  version copied the lights of a draw lit from both halves of the list
  into a list of their own and took the first 32 of them (with a warning);
  here a draw whose lights lie within 32 of the list's gets the list from
  the first of them, and one whose lights span more a list of them with
  their number (`VK_DLIGHT_COUNT`): every light reaches the draw.
- Brush models: every light that reaches a lightmapped surface, per pixel,
  as the world's surfaces get them (his the same), and no projected pass
  with `r_fastdlights 0` for their surfaces without a lightmap (his kept
  it): the projected pass's pipelines are only built with `r_fastdlights
  1` now (4 fewer pipelines at a load of `obj/obj_team2` and
  `dm/mohdm3`). With `r_fastdlights 1` they get all their lights
  projected (GL1, and his: light 0 only).
- The lights a scene keeps: the cgame's lights (up to 256 a frame) are
  sorted by distance to the view at `RE_RenderScene` and the
  `r_maxDlights` nearest kept in the order they were added (his kept the
  first 64 added, GL1 the first 32 added to the frame, and with exactly 32
  GL1's mask of all lights, `( 1 << 32 ) - 1`, was 0 on x86: no world
  surface got a light; his and this one fix that).
- Dead code gone: `tess.vertexDlightBits` (written for every vertex of the
  CPU path, read by nothing; 8 KB of `tess`) and `tess.dlightMap` (the
  sort key flag of brush model surfaces lit per pixel).
- The static models' lights (`useSpecialLighting`, up to 64 now): their
  list moved from each model's `cStaticModelUnpacked_t` (512 bytes for 32
  lights, 1 KB for 64) to one list in the back end
  (`backEnd.staticModelDlights`), since only the model being drawn reads
  it: the struct takes 256 bytes instead of 768. With the list in the model
  at 64 lights (1280 bytes), the front end of the `obj_team2` yard took 4
  to 6 µs longer in three batches (its walk over the visible models reads
  each model's struct); a test build with the list at 32 lights took as
  long as the build before.

Results (idle machine: other processes 1.9 CPUs at most; the build before
(`base`, 05f11b36) and this one (`new`) interleaved, medians of 5 runs,
640x480 uncapped, the fixed views 5000 frames, the demos 15000, the lights
of `cg_acidtrip N`; the renderer's main-thread time per frame and its front
end (`scene`), ms; the 16-light rows with `+set r_maxDlights 64`, the
others at the default):

| view | lights | renderer base / new | scene base / new |
| --- | --- | --- | --- |
| `obj_team2` yard | 0 | 0.346 / 0.345 | 0.101 / 0.103 |
| | 4 | 0.449 / 0.449 | 0.177 / 0.182 |
| | 16 | 0.640 / 0.634 | 0.336 / 0.344 |
| `dm/mohdm3` overview | 0 | 0.421 / 0.416 | 0.108 / 0.105 |
| | 4 | 0.527 / 0.524 | 0.176 / 0.176 |
| | 16 | 0.676 / 0.679 | 0.294 / 0.298 |
| `o2match` | the match's | 0.308 / 0.309 | 0.058 / 0.057 |
| `o4match` | the match's | 0.638 / 0.631 | 0.194 / 0.192 |

(The 16-light rows and a first batch at 1 and 2 lights, 0.378 / 0.381 and
0.404 / 0.408 ms in the yard, 0.478 / 0.471 and 0.498 / 0.489 in the
overview, come from the build with the static models' lists in the
models.) The demos' frame time percentiles: `o2match` p99 0.879 / 0.873,
p99.9 1.243 / 1.180 ms (each frame's median over the runs: 0.857 / 0.844,
1.167 / 1.124), `o4match` 1.257 / 1.244 and 1.641 / 1.590 (1.224 / 1.215,
1.549 / 1.563). Where the linker puts the renderer's large globals moves
these numbers by a few µs either way: `tess` is 8 KB smaller, which moved
`tr` and `backEnd` by 3 KB within their 4 KB pages, and a test build padded
back to the old placement took 0.011 ms longer in the overview (0.427 /
0.416) and about the same in the yard; the build before's numbers are
those of its own placement. More lights (the new build, 3 runs): 32
lights 0.840 and 0.843 ms, 64 lights 1.241 and 1.181 ms in the yard and the
overview, most of it the front end's tests of each face against each light
(0.539 and 0.921 ms in the yard). The GPU: with 16 lights at 3840x2160 the
yard's main pass takes 1.707 / 1.700 ms (3 runs), the shader's choice
between a mask and a count costs nothing measurable.

Input read to present at 1920x1080 in a window (`perf.py --caps 0,250
--seconds 10 --preload libinlat.so --framelog`, 5 runs; ms, the medians of
the runs' median, 99th and 99.9th percentiles, base / new): `o2match`
uncapped 0.477 / 0.472, 1.064 / 1.073, 1.404 / 1.491; at 250 0.512 /
0.510, 0.936 / 0.920, 1.417 / 1.286; `o4match` uncapped 0.889 / 0.866,
1.538 / 1.487, 2.027 / 2.068; at 250 0.842 / 0.837, 1.368 / 1.332, 1.947 /
2.263. The medians the same or lower; the 99.9th percentiles move both
ways inside the runs' ranges (`o4match` at 250, where a run's 99.9th
percentile is its third slowest frame of 2500: 1.83 to 3.09 ms before, 1.86
to 2.66 after, the 99th lower in every run). Nothing waits or is queued
for longer: the lights' work lies between the input read and the present,
as before.

Terrain (2026-10-22, Plan 25; see Implementation notes, Terrain). From
JayRewd's b26d8a67 (its terrain buffer: "Terrain draws from a device copy
of the vertex pool", and its walk: "Walk terrain geomorph and triangle
merge over patches drawn in the last ter_cautiousframes + 10 frames"). The
LOD terrain is tessellated again every frame where it is in view, its
vertexes kept in the slots of a pool. `RB_DrawTerrainTris` copied every
vertex of a drawn patch into `tess` (position, both texture coordinates, a
normal and a color) and the stage iterator copied them again into the
frame's vertex buffer, and the geomorph and merging passes went over every
patch of the map every frame (100 in `obj/obj_team2`, 177 in
`obj/obj_team1`, 211 in `obj/obj_team4`, 330 in `obj/obj_team3`), reading
two cache lines of each patch's 1.2 KB record. What differs from his:

- The pool: a copy per frame slot in host visible memory, written by its
  own frame (the slots that changed since that copy was last written): no
  transfer, staging copy or barrier. His single device local copy was
  brought up to date through the staging buffer and the upload command
  buffer every frame, with a barrier on each side (a frame's update waited
  for the vertex fetches of the frame before).
- The walk: his list put the patch drawn last first. Merging a triangle
  merges its base neighbour too, which can be in another patch, so the
  order of the walk decides which merges happen: his changed the
  tessellation (780 pixels of `o3_beach` in his evaluation). Here the
  list keeps the patches' order and holds every patch GL1's walk would do
  something to, so the tessellation is GL1's.

What it covers (`vkinfo` per frame at 640x480; vertex bytes copied into the
frame's vertex buffer before / after): the `o3_beach` view of the
`obj_team3` list 29 terrain draws, 404 / 208 KB; `o3_shore` 7 draws, 86 /
38 KB; the river of `obj_team4` (`o4_river`) 5 draws, 94 / 29 KB; the
`obj_team2` yard 5 draws, 162 / 105 KB; `o4match` 3.5 draws, 208 / 164 KB;
an `obj/obj_team1` match (`o1match`, 15000 frames of a match recorded by an
original client, like the two others) 6.1 draws, 140 / 97 KB; `o2match` 0.8
draws, 97 / 90 KB; `dm/mohdm3` (the overview and `d3_terrain`) one small
patch, 1 draw. The slots written into the frames' copies: 6 to 40 a frame
in these fixed views but `o3_beach` (177: its tessellation splits and
merges the same triangles every frame) and `o3_shore` (85), 34, 57 and 208
a frame in `o2match`, `o4match` and `o1match` (28 bytes each). The walk
visits 2 to 11 % of `obj/obj_team2`'s patches a frame in its matches, 10
to 13 % of `obj/obj_team4`'s, 19 to 22 % of `obj/obj_team1`'s.

Exactness. The frames are the same to the bit as the build before's (see
Testing). A temporary verification build checked, in every frame, that
each patch the walk leaves out is reset already and older than the
merging's window (then GL1's walk does nothing to it) and that the list is
in the patches' order, and for every terrain patch drawn from the pool,
each of its vertexes against the frame's copy (read back from it): over 20
real matches played frame by frame (`fixedtime 16`, 1920x1080; 5 of
`obj/obj_team2`, 5 of `obj/obj_team4`, 4 of `obj/obj_team1`, 4 of
`dm/mohdm6`, one of `dm/mohdm4` and of `obj/obj_team3`), 689 thousand
terrain views, 78 million patches left out and 641 million vertexes
checked: no difference, and every terrain patch was drawn from the pool
(none fell back to `tess`: no batch had a shader the pool doesn't take, no
slot changed after a draw of its frame). Another match with
`ter_cautiousframes` changed four times (4, 0, 2, 1) while it played (the
list filled again each time): the same.

Results (idle machine: other processes 1.3 CPUs at most; the build before
(`base`, 05928d6f), this one (`new`), this one with `r_staticTerrain 0`
(`walk`: the walk alone) and one that walks every patch (`pool`: the pool
alone), interleaved, medians of 5 runs with their range, 640x480 uncapped;
the fixed views 5000 frames, the matches 15000; the renderer's main-thread
time per frame, µs, and the median of the runs' 99th percentiles of it):

| view | base | new | walk | pool | p99 base / new |
| --- | --- | --- | --- | --- | --- |
| `o3_beach` | 761 (746-763) | 704 (691-721), -7.5 % | 755 | 716 | 818 / 765 |
| `o3_shore` | 219 (216-225) | 204 (199-211), -7 % | 218 | 207 | 270 / 250 |
| `o4_river` | 260 (255-264) | 240 (231-251), -7.6 % | 266 | 252 | 301 / 272 |
| `o4_street` | 234 (218-239) | 224 (217-229) | 229 | 220 | 274 / 259 |
| `obj_team2` yard | 351 (342-359) | 337 (329-360), -4 % | 356 | 353 | 418 / 407 |
| `d3_terrain` | 721 (717-737) | 717 (714-741) | 723 | 726 | 809 / 769 |
| `dm/mohdm3` overview | 419 (407-435) | 421 (412-433) | 429 | 423 | 464 / 484 |
| `o1match` | 447 (432-448) | 434 (428-436), -3 % | 446 | 434 | 647 / 630 |
| `o4match` | 630 (620-646) | 619 (611-624), -1.7 % | 637 | 619 | 983 / 971 |
| `o2match` | 308 (306-320) | 305 (301-312) | 308 | 313 | 607 / 596 |

The pool makes the gain, in the back end (the 2D calls, where the 3D view
is drawn: `o3_beach` 435 to 380 µs). The walk alone measures nothing beyond
the noise: in the stack samples of `o4match` (2 runs of each build, 12
thousand samples a run) the geomorph's walk over the patches went from 0.5
to 0.7 % of the frame to 0.1 to 0.2 %, while the geomorph's own work
(`R_UpdateVertMorphHeight`) grew from 1.7 to 1.8 % to 2.2 %: the back end
no longer reads the vertexes (`RB_DrawTerrainTris`, 5.5 to 5.8 % before,
3.0 to 3.3 % now, is the triangle lists), so the next frame's geomorph
finds fewer of them in the caches; the pool's updates take 0.2 to 0.3 %.
In the same frames of a match, compared frame by frame (each frame's median
over the runs), the front end (`scene`) takes 3 to 4 µs more on average
and the back end 6 to 20 µs less.

At 1920x1080 in a window, uncapped and at the user's cap of 250 (`perf.py
--caps 0,250 --seconds 10 --preload libinlat.so --framelog`, 5 runs; ms;
the renderer's time per frame, and each frame's median over the runs
("content", see Frame spikes) for its percentiles; input read to present
from `libinlat.so`, the median of the runs' medians and of their 99th
percentiles):

| demo | `com_maxfps` | renderer base / new | content p99, p99.9 base / new | input to present, median: base / new | p99 base / new |
| --- | --- | --- | --- | --- | --- |
| `o4match` | 0 | 0.745 / 0.728 | 1.191, 1.578 / 1.190, 1.514 | 0.865 / 0.852 | 1.451 / 1.458 |
| | 250 | 0.672 / 0.677 | 0.980, 1.056 / 0.985, 1.078 | 0.818 / 0.828 | 1.229 / 1.338 |
| `o2match` | 0 | 0.413 / 0.412 | 0.763, 0.881 / 0.767, 0.914 | 0.473 / 0.471 | 1.018 / 1.041 |
| | 250 | 0.426 / 0.417 | 0.672, 0.759 / 0.662, 0.831 | 0.517 / 0.501 | 0.907 / 0.904 |

The 99th percentile of `o4match`'s input to present at 250 (1.229 against
1.338 ms, the runs 1.21 to 1.29 against 1.27 to 1.36) was measured again,
10 runs of 20 s each (5000 frames a run) of both demos at 250: input to
present `o4match` 0.871 / 0.853 ms on the median (the runs 0.860 to 0.887 /
0.840 to 0.870), p99 1.360 / 1.348 (1.308 to 1.440 / 1.294 to 1.502), p99.9
1.921 / 1.967 (1.625 to 3.109 / 1.683 to 2.295); `o2match` 0.503 / 0.491,
p99 0.949 / 0.938, p99.9 1.347 / 1.377 (1.279 to 1.472 / 1.223 to 1.602);
the renderer's content percentiles `o4match` mean 0.723 / 0.705, p99 1.096
/ 1.043, p99.9 1.166 / 1.145, `o2match` mean 0.415 / 0.412, p99 0.686 /
0.681, p99.9 0.803 / 0.843 (its 5 slowest frames of 5000). The same or
lower but where a single run's slowest frames decide.

The GPU: the vertexes read from host visible memory instead of the frame's
vertex buffer (the same kind of memory) cost nothing measurable where the
GPU limits (3840x2160, 3 runs, GPU time of the main command buffer, ms):
`o3_beach` 1.467 / 1.464, `o4_river` 2.336 / 2.336, `o4match` 2.070 /
2.070; the frames 1.483 / 1.479, 2.364 / 2.364, 2.099 / 2.098 ms. Nothing
waits or is queued for longer: the pool's copy is written by its own frame
before its first terrain draw, after the wait for its frame slot that was
there already, and no frame more is in flight.

2D batches (2026-10-22, Plan 26; see Implementation notes, 2D batches).
JayRewd's b26d8a67 kept consecutive stretch pics and font glyphs in one
surface while the shader, the 2D color, the shader time and the entity
stayed the same, and ended it from every state setter of the backend,
`RB_BeginSurface` and `RB_ImmBegin`; under this engine, in his commit's
evaluation, that took about 2 µs (0.5 %) off the renderer's time in the
matches. What a match's HUD draws (a temporary build that logged the
renderer's exports in 4 frames of each match, and the verification build
below over 20 matches): every UI widget sets its window (`Set2DWindow`),
scissor, shader time and color, then draws one to six pics or strings; 35
to 44 `DrawBox` calls a frame draw the widgets' backgrounds and borders,
most of them with alpha 0 (every one in the traced frames: they change no
pixel, but each was a draw blended over its whole rectangle); and of 34 to
43 pics, strings and triangles a frame, those that can be one surface are
within a widget: the compass's quads (two `DrawTrianglePic` calls with one
shader each) and the kill feed's consecutive lines of one color. Over 667 thousand frames of the
20 matches, 7.6 of the 37.4 2D draws a frame joined a batch (5.4 to 11.8
per match) and 37.2 of the 40.2 boxes a frame were left out.

Results (idle machine: other processes at most 0.5 CPUs, no Plex
transcoding; the build before (`base`, c09e3f24) and this one with
`r_batch2D` 0 (`off`), 1 (`merge`), 2 (`boxes`) and 3 (`new`, the default)
interleaved, medians of 5 runs, 640x480 uncapped, 15000 frames; the
renderer's main-thread time per frame and its 2D calls with the back end,
the GPU time of the main command buffer, and each frame's median over the
runs ("content", see Frame spikes) for the percentiles; ms):

| demo | renderer base / off / merge / boxes / new | 2D and back end base / off / new | GPU base / new | content renderer mean, p99, p99.9: base / new | content frame time p99, p99.9: base / new |
| --- | --- | --- | --- | --- | --- |
| `o2match` | 0.313 / 0.303 / 0.305 / 0.301 / 0.297 | 0.183 / 0.177 / 0.168 | 0.233 / 0.222 | 0.311, 0.597, 0.714 / 0.293, 0.579, 0.697 | 0.832, 1.123 / 0.814, 1.095 |
| `o4match` | 0.619 / 0.622 / 0.617 / 0.612 / 0.605 | 0.352 / 0.353 / 0.337 | 0.322 / 0.311 | 0.617, 0.956, 1.152 / 0.599, 0.935, 1.132 | 1.183, 1.506 / 1.161, 1.493 |

(The runs' ranges of the renderer's time: `base` 0.307 to 0.316 and 0.613
to 0.638, `new` 0.289 to 0.301 and 0.594 to 0.607. `off` is this build with
both parts off, which draws as the build before did (the same frames, see
Testing); it differs from `base` by -10 and +3 µs, where the linker put the
code, so the parts are compared with `off`, the same library.) Against
`off` the boxes take 2 and 10 µs off, the batches 0 and 5 (about what
JayRewd's batching measured), both together 6 and 17 µs; against the build
before 16 and 14 µs (5 and 2.3 %), and 18 µs from each frame's median over
the runs, the slow frames' percentiles 17 to 21 µs lower. The GPU time goes
down by 0.011 ms: the boxes left out were blended over their rectangles.

At 1920x1080 in a window, uncapped and at the user's cap of 250 (`perf.py
--caps 0,250 --seconds 10 --preload libinlat.so --framelog`, 5 runs, other
processes at most 0.8 CPUs; ms; the renderer's time per frame, the GPU
time, input read to present from `libinlat.so`, the median of the runs'
medians with their range and of their 99th and 99.9th percentiles):

| demo | `com_maxfps` | renderer base / new | GPU base / new | frames a second | input to present, median (runs): base / new | p99, p99.9: base / new |
| --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | 0.419 / 0.400 | 0.439 / 0.434 | 1718 / 1777 | 0.480 (0.473 to 0.490) / 0.461 (0.455 to 0.466) | 1.049, 1.390 / 1.019, 1.370 |
| | 250 | 0.419 / 0.408 | | 250 | 0.502 (0.495 to 0.512) / 0.499 (0.490 to 0.505) | 0.982, 1.538 / 0.903, 1.346 |
| `o4match` | 0 | 0.727 / 0.718 | 0.648 / 0.642 | 1095 / 1098 | 0.843 (0.839 to 0.853) / 0.844 (0.837 to 0.850) | 1.455, 2.017 / 1.461, 1.930 |
| | 250 | 0.678 / 0.670 | | 250 | 0.811 (0.806 to 0.842) / 0.810 (0.801 to 0.816) | 1.393, 2.408 / 1.358, 2.130 |

(At 250 the GPU runs at lower clocks and its time spreads from run to run
by 0.1 ms.) The work taken off lies between the frame's input read and its
present (the 2D calls), so input to present goes down by about as much or
stays within the runs' spread; nothing waits or is queued for longer.

Where the GPU limits (3840x2160, 3 runs, ms): `o2match` 1.265 / 1.252 a
frame (791 / 799 frames a second), its GPU time 1.245 / 1.232; `o4match`
2.194 / 2.181 (456 / 459), GPU 2.167 / 2.154: the boxes' 13 µs of GPU time a
frame, in every run (the runs within 0.003 ms of each other). The final build
differs from the one timed and checked here in `RE_Shutdown` alone, which
forgets an open batch (`objdump -d` per function, addresses left out).

Light grid lookups (2026-10-22, Plan 27; see Implementation notes, Light
grid lookups). JayRewd's b26d8a67 kept the light grid cells' decoded
samples and the smoke's lookups; he measured 3.8 and 3.5 % off the 99th
percentile frame time in a 22-player fight in the middle of `dm/mohdm6`
(5 runs after a warm-up run, on his hardware and settings). Where the
lookups are here, from a counting build (every caller's calls and CPU
cycles per frame, and the two tables simulated) over 20 matches at 640x480
(14 `obj` and 6 `dm` maps, 40000 frames each, `fixedtime 16`): 13 to 66
lookups a frame on average, 126 to 482 in the 99th percentile frame, at
most 898; 81 to 125 ns each (the lighting spheres' 120 to 165); 1.4 to 6.1
µs a frame, 0.2 to 1.3 % of the frame time, 11 to 34 µs in the 99th
percentile frame (one `obj/obj_team3` match has 3 lookups a frame, 0.3 µs).
60 to 85 % of them are
cgame's `R_GetLightingForSmoke` (debris and smoke), most of the rest the
entities' lighting spheres. The smokiest are the `dm/mohdm6` matches
(`115335d7`: 50 smoke lookups a frame on average, 472 at the 99th
percentile, 599 at most). The tables would have found 92 to 96 % of the
cells and 71 to 84 % of the points looked up (JayRewd's point hash within
0.6 % of a good one).

Results (idle machine: other processes at most 1.3 CPUs, no Plex
transcoding; the build before (`base`, 6256ed41) and this one (`new`)
interleaved, medians of 5 runs, 640x480 uncapped; 15000 frames of
`o2match` and of the two smokiest `dm/mohdm6` matches, `d6a` (`164c7b5c`)
and `d6b` (`115335d7`), and 5000 of `sm_front` (`views/smoke.txt`, about
130 smoke lookups a frame); cgame's time per frame from `renderer_timed.so`'s
frame files (the part of the frame between `BeginFrame` and `EndFrame`
outside the exports it times; `R_GetLightingForSmoke`, which cgame calls
through the renderer's export table, isn't one of them, so it counts as
cgame), and each frame's median over the runs of cgame
and the renderer ("content", see Frame spikes) for the percentiles; ms,
cgame and content in µs):

| view | frame time base / new (runs) | renderer base / new | cgame base / new | content mean, p99, p99.9: base / new |
| --- | --- | --- | --- | --- |
| `o2match` | 0.424 (0.411 to 0.426) / 0.413 (0.409 to 0.430) | 0.298 / 0.290 | 97.3 / 94.7 | 391, 783, 971 / 386, 776, 959 |
| `d6a` | 0.432 (0.426 to 0.444) / 0.439 (0.426 to 0.440) | 0.310 / 0.317 | 95.4 / 94.1 | 404, 668, 817 / 406, 664, 804 |
| `d6b` | 0.492 (0.486 to 0.497) / 0.487 (0.478 to 0.497) | 0.353 / 0.350 | 108.9 / 108.7 | 461, 772, 883 / 455, 770, 876 |
| `sm_front` | 0.696 (0.692 to 0.719) / 0.691 (0.671 to 0.709) | 0.573 / 0.571 | 78.3 / 77.3 | |

The frame times are within the runs' ranges, of either sign: what the
tables take off is less than the difference that the code's placement
makes between two builds (every global of the library 32 bytes further
on here). Frame by frame (the runs aligned with `framelog.best_shift`,
each frame's smoke lookups from a run of the counting build under
`perf.py`) the difference grows with the frame's lookups: 13, 28 and 35 ns
less per smoke lookup in `o2match`, `d6a` and `d6b` (a least-squares
line), on top of -4.5, +2.5 and -3.5 µs in frames without any (the
placement); the frames with 200 or more lookups (615 of `d6a`'s 15000, 248
of `d6b`'s) 8 and 12 µs lower; the slowest 1 % of the frames 1.5 to 6 µs
(0.2 to 0.8 %); the content's 99th and 99.9th percentiles 2 to 13 µs (0.3
to 1.6 %). The cells save the walk of 4 columns, the points the whole
lookup, but the lookup itself was 80 to 125 ns of a frame of 400 to 700 µs,
and the lookups are 2 to 3 % of these matches' slowest frames: JayRewd's
3.5 % is not reached here.

At 1920x1080 in a window, uncapped and at the user's cap of 250 (`perf.py
--caps 0,250 --seconds 10 --preload libinlat.so --framelog`, 5 runs, other
processes at most 1.1 CPUs; ms; the renderer's time per frame, input read
to present from `libinlat.so`, the medians of the runs' median, 99th and
99.9th percentiles):

| demo | `com_maxfps` | renderer base / new | frames a second | input to present median, p99, p99.9: base / new |
| --- | --- | --- | --- | --- |
| `o2match` | 0 | 0.402 / 0.399 | 1777 / 1794 | 0.465, 1.005, 1.319 / 0.460, 0.996, 1.360 |
| | 250 | 0.403 / 0.405 | 250 | 0.492, 0.868, 1.329 / 0.491, 0.874, 1.290 |
| `d6a` | 0 | 0.400 / 0.397 | 1879 / 1864 | 0.478, 0.877, 1.095 / 0.479, 0.884, 1.076 |
| | 250 | 0.326 / 0.338 | 250 | 0.395, 0.687, 1.111 / 0.410, 0.703, 1.098 |
| `d6b` | 0 | 0.412 / 0.416 | 1765 / 1748 | 0.512, 0.926, 1.178 / 0.516, 0.930, 1.156 |
| | 250 | 0.443 / 0.449 | 250 | 0.564, 0.919, 1.413 / 0.564, 0.920, 1.412 |

Within the runs' spread, of either sign (at 250 the CPU runs at lower
clocks and the renderer's time spreads by 0.01 to 0.02 ms between runs;
the renderer's own lookups, the lighting spheres', are at most 2 µs a
frame, so its differences are the placement). Nothing waits or is queued
for longer; the work taken off lies between the input read and the
present.

Registration in the match (2026-10-22, Plan 28; see Implementation notes,
Registration at load; cgame and the engine, both renderers). The largest
frames of a match were those in which something the server announced, or
cgame drew for the first time, was loaded (Plan, Remaining 1 before this
step). What and how much, from a temporary engine build that timed every
registration cgame makes through its imports (models, server models,
effects, shaders, sounds, fonts: the outermost calls and the nested ones,
the frame and the frame's length; not kept), over the 21 demos of real
matches (20 matches of `obj/obj_team1` to `obj/obj_team4`, `dm/mohdm4` and
`dm/mohdm6` played whole, 1920x1080 uncapped, `fixedtime 16`; 5 load a
second map, one of which the game data hasn't), the build before (`base`)
and this one (`new`); calls of 50 µs or more, after the first frame of each
load and in it; ms:

| registered in the match | `base`: calls, ms, the largest (matches) | `new` |
| --- | --- | --- |
| the map's entity models (the objectives' explosions) | 45, 143.8, 17.6 (8) | none |
| players' models | 6, 44.7, 8.1 (5) | none |
| their first person models | 1, 3.4 (1) | none |
| items, turrets, vehicles, projectiles, effect models | 22, 38.6, 7.7 (14) | 4, 9.6, 7.4 (3) |
| cgame's shaders and sprites (decals, HUD) | 77, 22.1, 1.0 (20) | 8, 0.8, 0.4 (7) |
| sounds | 8, 1.8, 0.3 (6) | 6, 1.3, 0.3 (5) |
| effects spawned (cgame's init commands of each new effect, not a load) | 135, 17.2, 0.3 (11) | 135, 15.8, 0.3 (11) |
| in the first frame after each load | 96, 77.9, 17.3 | none |
| frames with 0.3 ms or more of it (after the first) | 61, 249 ms in all; 24.1, 18.2, 18.2, 18.1, ... | 7, 14.9 ms in all; 9.4, 2.0, 1.7, 0.7, 0.4, 0.4, 0.4 |

- The objectives' explosions: the exploder script's `exploderfire`
  entities carry `models/animate/fx_explosion_tank.tik` (`obj/obj_team2`;
  17.5 ms, its `textures/effects/bang.spr` alone 16.8: an animation of
  256x256 images) and `explosion_bridge.tik` (`obj/obj_team4`, 17.2 ms),
  and get the model when they go off: in every match of those maps where
  the objective blew up. `obj/obj_team4`'s precache script names its
  explosion, which the server caches but doesn't announce.
- Players' models: the servers of these demos (original game servers)
  announce the models of their precache list (`global/DMprecache.scr`) in
  the gamestate, and so does an OpenMoHAA dedicated server (checked with 8
  bots joining a team match, every model at the load), so these load with
  the map; what came in the match were the models the stock list lacks or
  misnames (`allied_sas`; `german_winter1` for `german_winter_1`): 7.0 to
  8.1 ms each, the player's own first person model 3.4 to 3.9 ms in the
  first frame.
- Items and the like: the explosive (2.1 ms), the health box of a dying
  player (1.0 ms), the P-47's fly-by (2.6 ms), the MG42's view model when a
  player mans it (1.7 ms), `obj/obj_team1`'s flak wreck and its explosion
  (7.7 and 3.4 ms), projectiles and explosions (0.2 to 0.3 ms).
- cgame's own: the `blastmark` decal of the first explosion (0.58 ms, in
  20 of the 21 matches), the crosshairs, team icons, radar, head icons
  and the lag icon (0.1 to 0.35 ms each, in the first frame or later).
- A model whose configstring comes and goes was freed with it and
  registered again (`models/emitters/adamspark.tik`: 32 times in 5
  matches, 0.13 ms each).

What is left with `new`: `obj/obj_team1`'s flak wreck and its explosion
(7.4 and 1.9 ms, one frame of 9.4 ms) in the match where the flak was
destroyed: their names are in the stock map script, which a mod pk3 on this
machine (`z_forteffa.pk3`) replaces for the client (with the stock script
installed in the home path both load with the map, and that match has no
frame of 0.3 ms of registration left; the mod was removed from this
machine's game data on 2026-10-23, and a traced run of that match confirms
it, see Plan, Remaining 1); the water sounds of the body (0.2 to
0.3 ms), a projectile the server's weapon file names and the client's
doesn't (0.15 ms); the frames of 1.7 and 2.0 ms are many crate debris
effects spawned at once (cgame's init commands, 0.1 ms each).

Results (idle machine: other processes at most 0.9 CPUs, no Plex
transcoding; `base` (8e48c063: its engine and cgame, the same renderer)
and `new` (`cg_precache 7`, `fs_listDirs 1`), uncapped also with each
part alone (`cp0`: `cg_precache 0`, only the directory listings; `ld0`:
`fs_listDirs 0`, only the registration at load), interleaved, 5 runs of
15000 frames of `o2match` and `o4match` at 1920x1080 in a window,
`perf.py --caps 0` and `--caps 250`, `--preload libinlat.so --framelog`;
the medians of the runs' percentiles, in parentheses the "content" ones
(each frame's median over the runs, see Frame spikes); ms):

| demo | `com_maxfps` | build | frame time p99.9 | max | p99 | mean | input to present p99.9 | max | p99 | median |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | `base` | 1.449 (1.312) | 21.36 | 1.048 (1.007) | 0.498 | 1.382 (1.348) | 20.25 | 0.978 | 0.418 |
| | | `new` | 1.327 (1.256) | 2.92 | 1.029 (0.994) | 0.492 | 1.265 (1.215) | 2.67 | 0.967 | 0.414 |
| | | `cp0` | 1.369 (1.264) | 18.80 | 1.034 (0.993) | 0.492 | 1.355 (1.270) | 17.68 | 0.969 | 0.414 |
| | | `ld0` | 1.329 (1.274) | 3.01 | 1.039 (1.014) | 0.493 | 1.270 (1.229) | 2.78 | 0.986 | 0.418 |
| | 250 | `base` | 4.235 (4.228) | 21.02 | 4.045 (4.041) | 4.001 | 1.509 (1.258) | 19.98 | 1.019 | 0.482 |
| | | `new` | 4.205 (4.196) | 4.45 | 4.045 (4.042) | 4.000 | 1.421 (1.193) | 2.65 | 1.008 | 0.474 |
| `o4match` | 0 | `base` | 1.930 (1.880) | 27.92 | 1.488 (1.435) | 0.926 | 1.880 (1.843) | 26.89 | 1.428 | 0.865 |
| | | `new` | 1.969 (1.854) | 5.35 | 1.492 (1.428) | 0.924 | 1.935 (1.756) | 5.21 | 1.428 | 0.863 |
| | | `cp0` | 1.963 (1.908) | 25.68 | 1.518 (1.460) | 0.940 | 1.886 (1.800) | 24.70 | 1.472 | 0.879 |
| | | `ld0` | 1.910 (1.780) | 5.53 | 1.479 (1.431) | 0.918 | 1.890 (1.774) | 5.33 | 1.411 | 0.862 |
| | 250 | `base` | 4.237 (4.195) | 28.12 | 4.041 (4.032) | 4.001 | 2.203 (1.674) | 27.08 | 1.606 | 0.923 |
| | | `new` | 4.190 (4.198) | 5.39 | 4.039 (4.031) | 4.000 | 2.301 (1.627) | 5.24 | 1.584 | 0.913 |

- The maximum was the registration (the bridge's and the tank's
  explosions, in `o2match` also two players' models of 7.5 ms): 21.4 and
  27.9 ms become 2.9 and 5.4 ms uncapped, 4.4 and 5.4 ms at 250 (where a
  frame lasts 4 ms). What is at the top now is the first frame after the
  load (0.9 ms of registration in `o4match`'s that doesn't come from
  cgame: the engine's UI, which draws the HUD) and `o4match`'s
  round changes (3.7 ms, the soundtrack decoded on the main thread, Remaining 1;
  since Plan 29 1.9 to 2.4 ms, see Round changes).
- The slowest 1 % of the frames: the registration was 26 to 27 % of their
  excess over the median frame in `o2match` and 18 to 19.5 % in `o4match`
  (`framelog.py`, each run), 2 % now; their average excess 0.98 to 0.70 ms
  and 0.99 to 0.78 ms. The 99.9th percentile is lower in `o2match`
  (1.449 to 1.327 ms uncapped, content 1.312 to 1.256; 4.235 to 4.205 at
  250) and the same within the runs' spread in `o4match` uncapped (one run
  of each build at 2.4 and 2.6; content 1.880 to 1.854), lower at 250
  (4.237 to 4.190); the 99th percentile and the mean the same within the
  spread (a match has 3 to 7 such frames in 15000).
- Input read to present: the median and the 99th percentile the same or
  lower; its maximum follows the frame's; the 99.9th percentile lower in
  `o2match`, in `o4match` 0.06 and 0.1 ms higher by the runs' medians (the
  runs 1.81 to 2.28 against 1.85 to 2.38 ms uncapped, 2.07 to 2.23 against
  2.16 to 2.36 at 250) where the content's goes from 1.843 to 1.756 and from
  1.674 to 1.627 ms: run to run noise. Nothing more happens between a
  frame's input read and its present; the load does more.
- Each part alone: the registration at load takes the maximum (`ld0`); the
  listings alone take 2.2 to 2.6 ms off it (`cp0`: the lookups of what is
  still loaded in the match).

The load (`CL_InitCGame`, the median of the runs; device memory, texture
memory and the game's resident memory from `vkinfo` and the process after
the load and at the end of the demo, one run each):

| demo | map load `base` / `new` (s) | `cp0`, `ld0` | texture memory used after the load, at the end (MB) | device memory for textures | resident |
| --- | --- | --- | --- | --- | --- |
| `o2match` | 1.320 / 1.280 | 1.220, 1.380 | 111.7, 123.7 / 123.8, 126.0 | 128 MB / 128 MB | 209.0 / 209.4 MB |
| `o4match` | 1.230 / 1.170 | 1.120, 1.280 | 126.3, 128.4 / 130.7, 132.7 | 128 MB, then 192 MB in the match / 192 MB at the load | 207.9 / 208.9 MB |

The registration at load costs 0.05 to 0.06 s of the load (`ld0`), the
listings take 0.10 to 0.11 s off it (`cp0`), together 0.04 and 0.06 s
less; over the 26 loads of the 21 demos (one run each, the first load and
the second of a demo that changes map) 0.01 to 0.11 s less. (`cp0` and
`ld0` were measured with this cgame before it also registered every
player model of the data: that added `german_elite_officer`, which no list
names, and its first person model; `new` is the final one.) The texture
memory after the load is 4 to 12 MB larger and 2 to 4 MB at the end of the
match (what the match didn't use: the other players' models, effects and
items), in the same device memory (in `o4match` the build before allocated
its third block of texture memory, 64 MB, in a frame of the match); 2 to 4
more pipelines at the load, the skin data 3.8 to 4.1 MB (of 32), the
resident memory 0.4 to 1.0 MB more. A load against an OpenMoHAA dedicated
server (`obj/obj_team2`, 8 bots joining from 20 s on): 1.324 to 1.235 s,
the same memory, no registration in a frame of the match (the build before:
one, a head icon).

Round changes (2026-10-23, Plan 29; see Implementation notes, Round
changes; the engine and cgame, both renderers; measured with OpenAL, which
Plan 31 replaced, see Sound system). After Plan 28 the slowest
frames of `o4match` were its round changes (frames 4451, 7055, 11008 and
13483, and frame 30: the server restarted once more right after the load),
3.2 to 3.9 ms against a median frame of 0.94 ms, with about 1.1 ms more
engine time and 1.0 ms more cgame time than a median frame. What they did,
from temporary builds that timed the sound system's calls and the
configstring changes in each frame (not kept), `o4match` at 1920x1080
uncapped, `fixedtime 16`, one run of each build, the 4 round changes (µs):

| in a round change's frame | the build before | this one |
| --- | --- | --- |
| the sound system's update (`S_Update`) | 1010 to 1113 | 98 to 142 |
| of which the soundtrack started (`MUSIC_PlaySong`): its mp3 opened (a scan of the whole file), its first read decoded | 552 to 662 | 54 to 94 (the head copied) |
| and the stream's update: the end of the 0.24 s file, opened and decoded again | 433 to 445 | 25 to 41 |
| the update with a 3 minute mp3 as the song (one run each) | 2669 to 2751 | 132 to 144 |
| the soundtrack file read (`MUSIC_NewSoundtrack`) | 41 to 71 | 7 to 9 |
| 64 configstring changes in the client (`CL_ConfigstringModified`) | 508 to 519 | 11 to 15 |
| cgame's copies of the gamestate (64) | 66 to 70 | 0 (one of 16 when the buffer was full) |
| cgame's 84 configstrings (`CG_ProcessConfigString`) | 142 to 185 | 114 to 127 |
| cgame's server commands (`CG_ExecuteNewServerCommands`, twice) | 822 to 882 | 234 to 254 |
| the frame | 3155 to 3885 | 1852 to 2444 |

This machine's game data has a 0.24 s mp3 (3.7 KB, the same file) for
every song of the soundtracks, so a song loops every 0.24 s; a 3 minute
mp3 in the home path (a generated test tone, 2.9 MB at 128 kbit/s) shows
what a full soundtrack costs. Besides the round changes, a playing song
queues 8 buffers in 8 frames in a row whenever half of its 16 have played:
in `o2match`, whose song plays from the load to its first round change,
three times (frames 3064 to 3071, 5879 to 5886, 8439 to 8446), 0.39 to
0.52 ms each with the 0.24 s file (a reopen and a decode each time), 0.85
to 0.92 ms a buffer with the 3 minute one (every 3 s while it plays); now
those frames copy PCM. What is left in a round change's frame: cgame's
server restart (`CG_ServerRestarted`, twice: the server info parsed again
and the HUD set, the marks cleared; 143 to 161 µs), the 84 configstrings'
processing, and the first view at the new spawn (the renderer's front end
and cgame's entities). Not done: the first frame after a load (now the
largest of `o4match`, 4.6 ms: cgame's first frame and 0.9 ms of the
engine UI's registrations) and the body's water sounds (part (b) of the
item, optional).

Results (idle machine: other processes at most 0.8 CPUs, no Plex
transcoding; `base` (912599f4: its engine and cgame, the same renderer) and
`new`, interleaved, 5 runs of 15000 frames of `o2match` and `o4match` at
1920x1080 in a window, `perf.py --caps 0` and `--caps 250`, `--preload
libinlat.so --framelog`, with this machine's game data and, uncapped, with
the 3 minute test mp3 in the home path as both maps' song (`mp3`); the
medians of the runs' percentiles, in parentheses the "content" ones (each
frame's median over the runs, see Frame spikes); ms):

| demo | `com_maxfps` | build | frame time p99.9 | max | p99 | mean | input to present p99.9 | max | p99 | median |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | `base` | 1.397 (1.239) | 2.92 (2.92) | 1.046 (1.003) | 0.495 | 1.298 | 2.63 | 0.978 | 0.414 |
| | | `new` | 1.375 (1.257) | 2.18 (1.90) | 1.037 (0.996) | 0.493 | 1.325 | 2.52 | 0.984 | 0.415 |
| | 250 | `base` | 4.208 (4.217) | 4.42 | 4.044 (4.040) | 4.000 | 1.439 | 2.66 | 1.000 | 0.474 |
| | | `new` | 4.212 (4.205) | 4.42 | 4.044 (4.041) | 3.999 | 1.337 | 2.22 | 0.974 | 0.471 |
| | 0, `mp3` | `base` | 1.502 (1.413) | 4.52 (4.52) | 1.042 (1.009) | 0.493 | 1.273 | 1.87 | 0.982 | 0.416 |
| | | `new` | 1.300 (1.246) | 1.95 (1.95) | 1.026 (0.993) | 0.493 | 1.262 | 2.55 | 0.971 | 0.419 |
| `o4match` | 0 | `base` | 1.912 (1.824) | 5.37 (5.37) | 1.459 (1.407) | 0.910 | 1.867 | 5.29 | 1.400 | 0.853 |
| | | `new` | 1.891 (1.794) | 4.74 (4.63) | 1.453 (1.404) | 0.919 | 1.899 | 5.47 | 1.414 | 0.866 |
| | 250 | `base` | 4.194 (4.195) | 5.37 | 4.041 (4.033) | 4.000 | 2.305 | 5.29 | 1.613 | 0.918 |
| | | `new` | 4.179 (4.184) | 4.66 | 4.040 (4.033) | 4.000 | 2.128 | 5.40 | 1.619 | 0.923 |
| | 0, `mp3` | `base` | 2.127 (1.982) | 6.99 (6.99) | 1.509 (1.431) | 0.920 | 1.995 | 5.28 | 1.454 | 0.858 |
| | | `new` | 1.891 (1.830) | 4.70 (4.70) | 1.462 (1.405) | 0.918 | 1.856 | 2.42 | 1.409 | 0.866 |

The round changes' frames (`o4match`: frames 30, 4451, 7055, 11008 and
13483; `o2match`: 10233 and 10961), the medians of the 5 runs; uncapped
the largest frame within 2 of each, at 250 (where every frame lasts 4 ms)
the input to present of the frame that runs the commands (ms):

| demo | | `base` | `new` |
| --- | --- | --- | --- |
| `o4match` | frame time, uncapped | 3.09 to 3.80 | 1.86 to 2.42 |
| | with `mp3` | 4.85 to 5.35 | 1.87 to 2.48 |
| | input to present at 250 | 1.73 to 2.58 | 1.08 to 1.92 |
| `o2match` | frame time, uncapped | 2.25, 2.64 | 1.09, 1.39 |
| | with `mp3` | 3.81, 4.16 | 1.18, 1.28 |
| | input to present at 250 | 1.24, 1.60 | 0.71, 0.94 |

- The round changes' frames are 1.0 to 1.4 ms shorter uncapped (2.6 to
  3.0 ms with the 3 minute song); at 250 they fit in their 4 ms either way
  and their input to present is 0.5 to 0.7 ms shorter: cgame's work comes
  before the present (0.4 ms of cgame left against 1.0 to 1.2), the
  sound's after it, where the frame limiter's wait absorbs it.
- The 99.9th percentile is lower or the same within the runs' spread in
  every row (a match has 2 to 5 round changes in 15000 frames), 0.2 ms
  lower with the 3 minute song; the round changes are gone from the top
  of the frame times: what is left at
  the top is the first frame after the load (`o4match` 4.5 to 4.8 ms; its
  0.9 ms of the engine UI's registrations, part (b), not done), and in
  `o2match` the first frames are 0.4 ms (0.9 ms with the 3 minute song)
  shorter each until the song has queued its 16 buffers. The 99th
  percentile, the mean and the medians the same within the runs' spread
  (the runs' medians 0.90 to 0.94 ms of both builds in `o4match`; the
  build with all four switches off has `new`'s 0.926, the 3 minute song's
  runs the mean the other way).
- Input to present: the 99.9th percentile lower at 250 (`o2match` 1.439 to
  1.337, `o4match` 2.305 to 2.128), uncapped the same within the spread
  (1.298 to 1.325, 1.867 to 1.899: one run of `new` at 9.7 ms in its first
  frame); its maximum is the first frame after the load in both builds
  (in the `mp3` rows of `base` it fell outside the matched frames in 4 of 5
  runs); the median within 0.013 ms (the runs 0.84 to 0.88 of both).
  Nothing happens between a frame's input read and its present that
  didn't before; less does at a round change.

Each part alone (one change switched off, 2 runs of `o4match` uncapped
each, the round changes' frames, ms): `new` 1.85 to 2.38; without the
stream thread (`s_streamThread 0`) 2.68 to 3.45 (its engine time back to
0.95 to 1.26 ms); without the client's appends (`cl_csAppend 0`) 2.20 to
2.81 (cgame 0.35 to 0.45 ms more); without cgame's (`cg_csAppend 0`) 2.09
to 2.56 (0.05 to 0.1 ms more); without the soundtrack cache
(`s_soundtrackCache 0`) 1.85 to 2.45 (the 40 to 70 µs within the runs'
spread); with all four off 3.21 to 3.88, `base`'s 3.14 to 3.79. GL1 with
the new engine and cgame (`cl_renderer opengl1`, one run of each, `o4match`
uncapped): the round changes' frames 3.25 to 4.09 ms to 2.22 to 2.98 ms.

Busy moments (2026-10-23, Plan 30; see Implementation notes, Terrain and
Sound buffers at load; the renderer and the engine's sound system). The busier moments of a match on the CPU (Remaining, slow
frames item 3) measured again after Plans 25 to 29: the frame log and the
stack samples of `libstacks.so` in the same runs (`perf.py --framelog
--vklog --stacks`, 4 runs of `o2match` and `o4match` at 1920x1080
uncapped, the build before), the "content" slow frames (each frame's
median over the runs above its 99th percentile, see Frame spikes) split
by the frame log's parts, and the samples of those frames put into
groups by the first of a list of functions on their stack, each group's
time in a slow frame against an average frame (ms; a sample is about 1
ms):

| `o4match` (slow frames 1.49 ms and up, 0.73 ms over an average frame) | excess | `o2match` (1.02 ms and up, 0.65 ms over) | excess |
| --- | --- | --- | --- |
| terrain tessellation and its indexes | 0.234 | cgame's events of the shots (tracers, impacts, sounds, effects; first plays of sounds 0.014) | 0.141 |
| world leaves (`R_AddWorldLeaves`, and `R_MarkLeaves` 0.020) | 0.112 | back end drawing | 0.108 |
| back end drawing (`RB_RenderDrawSurfList`, the driver) | 0.106 | terrain tessellation and its indexes | 0.085 |
| cgame's events (sounds, tracers, effects; first plays of sounds 0.014) | 0.085 | the fence wait (the GPU: 0.9 ms of main pass at its p99) | 0.069 |
| the 2D calls (the HUD) | 0.047 | world leaves | 0.065 |
| cgame's entities' animation and shadows | 0.019 | the 2D calls | 0.060 |
| the sky's clipping (`RB_ClipSkyPolygons`) | 0.011 | cgame's entities' animation and shadows | 0.048 |

By the frame log's parts the front end is 46 % of `o4match`'s slow
frames' excess (back end 23 %, cgame 21 %), cgame 33 % of `o2match`'s
(front end 29 %, back end 21 %, the fence wait 10 %). The terrain leads
in `o4match` and is third in `o2match`: a temporary counting build (`o4match`, one run) gave its
tessellation 120 µs in an average frame, 335 µs in the slow frames and
561 µs in the slowest 0.1 % (1082 splits and 4116 triangles allocated in
such a frame: a large turn of the view or a respawn splits the terrain in
front of the camera, which the old view merges back over the next
frames). Most of the walks' work changes nothing, though: each frame the
geomorph went over 1714 vertexes of the drawn patches to recompute 74,
the merging tried 2732 triangles to merge 47, and `RB_DrawTerrainTris`
walked 2706 triangles, most of them in lists that hadn't changed since
the frame before, to write 1034 triangles' indexes. Three parts in the
renderer, each with its own switch:

- `r_terrainCache` 1: the geomorph leaves out a patch whose vertexes all
  have their height for longer (the lowest `uiDistRecalc` of its last
  walk);
- `r_terrainCache` 2: the merging leaves out a patch where every merge
  failed when it was last walked and nothing they read changed since;
- `r_terrainCache` 4: a patch's indexes are kept until its list changes.

Their exactness: a temporary build that, wherever a walk
was left out, ran its tests without changing anything and counted what it
would have done, and compared every kept index list with the walk, over
the 21 matches played frame by frame (`fixedtime 16`, 1920x1080, up to
40000 frames each; one switches to a map not installed after its first
seconds): 5.0 million geomorph walks left out, not one vertex due for a
new height in them; 5.2 million merge walks left out, 431 million merge
tests in them, none that would have merged; 11.7 million kept index
lists, all the same as the walk. A part dropped: the free lists' back
links, written into the next free triangle at each allocation and read by
nothing (one cache miss each), measured nothing.

Each part alone (a temporary build that timed the three passes, the
indexes and the sky's clipping per frame, `CLOCK_MONOTONIC`; one run of
each setting, 1920x1080 uncapped; µs per frame: all frames / the slow
frames above the content's 99th percentile / its 99.9th):

| `o4match` | split | geomorph | merging | indexes | sky | total |
| --- | --- | --- | --- | --- | --- | --- |
| all off | 17 / 125 / 273 | 30 / 51 / 46 | 41 / 85 / 98 | 40 / 83 / 132 | 12.8 / 14.9 / 16.0 | 141 / 359 / 565 |
| geomorph (1) | 17 / 133 / 300 | 17 / 53 / 82 | 41 / 97 / 115 | 39 / 90 / 148 | | 127 / 387 / 661 |
| merging (2) | 17 / 135 / 317 | 29 / 55 / 57 | 21 / 59 / 92 | 40 / 85 / 125 | | 120 / 350 / 608 |
| indexes (4) | 20 / 138 / 323 | 28 / 49 / 49 | 41 / 88 / 104 | 10 / 39 / 106 | | 112 / 329 / 596 |
| all three | 21 / 141 / 296 | 15 / 37 / 56 | 21 / 61 / 94 | 10 / 39 / 97 | 12.8 / 14.9 / 15.9 | 80 / 292 / 559 |
| and the sky's faces culled | 20 / 145 / 319 | 13 / 34 / 53 | 20 / 57 / 92 | 10 / 36 / 91 | 11.3 / 13.9 / 14.5 | 74 / 286 / 569 |

(`o2match`, whose terrain is in view less: 19.3 / 123 / 182 µs with all
off, 14.9 / 115 / 205 with all on.) The terrain's time in an average
frame of `o4match` goes from 141 to 74 µs, in the slow frames from 359
to 286 µs; the slowest 0.1 % keep their 560 µs, which are the splits
(a pass these parts don't shorten; the splits' time even grows a little
when the other passes no longer bring the triangles into the caches).

Tried and left out: the sky's faces outside the view culled like the
other faces (a `viewCull` for the sky's faces behind a switch). They are
about 10 % of the sky's triangles in these matches (71 to 64 a frame in
`o4match`, 6 to 3 in `o2match`): 1.5 µs a frame. But the sky box is
drawn over the rectangle (per side of the box) that the sky's polygons
cover, and the faces outside the view widened it over directions in the
view too: a pixel that no face covers (a crack between faces) showed the
sky there and the clear color without them, and with no sky face in the
view the sky isn't drawn at all. In the build-against-build check only
`fx_mohdm1`'s view from above the map (`fx_e`) showed it: 806 pixels along
the walls' tops, the sky's color (230, 201, 143) before, black with the
faces culled (the same frame with the switch off, and the build before
the same twice). The queue's condition (the same pixels where no world
face covers) didn't hold, for 1.5 µs.

A fourth part in the engine, `s_loadBuffers`, for the first plays of
sounds (cgame's events, 0.014 ms of the slow frames in both matches, more
in single frames): a temporary engine build that
timed the creation of each sound's OpenAL buffer gave 131 first plays in
`o2match` and 146 in `o4match`, all in the match (none during the load),
11.9 and 12.7 ms in all, 0.38 and 0.60 ms the largest (a reload, an
explosion, the river's loop); with `s_loadBuffers` the registration makes
the 410 buffers of the map's sounds at its end (47.6 MB of samples in
OpenAL's memory, against the 8.6 to 10.2 MB the two matches' first plays
made; 0.02 to 0.03 s more map load) and the matches make none; the
samples given to OpenAL are the same for every sound (their size, rate
and a checksum compared, `s_loadBuffers` 1 against 0).

Results (idle machine: other processes at most 0.6 CPUs, no Plex
transcoding; `base` (7337a68b) and `new` (the three terrain parts and
`s_loadBuffers` on, and the sky's faces culled: that part, 1.5 µs a
frame, was left out after these runs, the code of the others the same),
interleaved, 5 runs of 15000 frames of `o2match` and `o4match`
at 1920x1080 in a window, `perf.py --caps 0` and `--caps 250`, `--preload
libinlat.so --framelog`; the medians of the runs' percentiles, in
parentheses the "content" ones (each frame's median over the runs, see
Frame spikes); ms):

| demo | `com_maxfps` | build | frame time p99.9 | max | p99 | mean | input to present p99.9 | max | p99 | median |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | `base` | 1.379 (1.252) | 1.99 (1.95) | 1.046 (0.995) | 0.496 | 1.334 | 2.65 | 0.995 | 0.418 |
| | | `new` | 1.315 (1.215) | 2.44 (2.44) | 0.999 (0.974) | 0.483 | 1.272 | 2.26 | 0.952 | 0.409 |
| | 250 | `base` | 4.219 (4.197) | 4.44 | 4.045 (4.041) | 3.999 | 1.448 | 2.69 | 1.011 | 0.476 |
| | | `new` | 4.187 (4.167) | 5.03 | 4.045 (4.042) | 3.999 | 1.476 | 2.61 | 0.969 | 0.468 |
| `o4match` | 0 | `base` | 1.987 (1.793) | 4.57 (4.57) | 1.518 (1.452) | 0.937 | 1.928 | 5.27 | 1.472 | 0.875 |
| | | `new` | 1.923 (1.786) | 4.25 (4.25) | 1.373 (1.326) | 0.867 | 1.838 | 2.57 | 1.332 | 0.810 |
| | 250 | `base` | 4.193 (4.194) | 4.74 | 4.042 (4.033) | 4.000 | 2.224 | 5.28 | 1.623 | 0.915 |
| | | `new` | 4.169 (4.146) | 4.37 | 4.038 (4.032) | 3.999 | 1.802 | 2.47 | 1.420 | 0.848 |

- `o4match`: the 99th percentile 0.145 ms lower uncapped (content 0.126),
  the mean 70 µs (the renderer's time 0.732 to 0.672 ms, 0.769 to 0.701
  at 250); frame by frame (each frame's median over the runs, the frames
  chosen by the sum of both builds', see Frame spikes) the slowest 1 % 87
  µs shorter (1.578 to 1.492 ms: the back end 39 µs, the front end 22,
  cgame 20 with the first plays of sounds), every frame 64 µs on average
  (the front end 33, the back end 26), the slowest 0.1 % the same (2.043
  to 2.055 ms: the splits). Input to present
  lower at every percentile, uncapped and at 250 (the runs' 99.9th
  percentiles 2.08 to 2.25 to 1.71 to 1.89 ms at 250, the medians 0.90 to
  0.92 to 0.83 to 0.86).
- `o2match`: the 99th and 99.9th percentiles 0.03 to 0.06 ms lower
  uncapped, the slowest 1 % of the frames 20 µs shorter, the mean 13 µs;
  input to present lower uncapped, the same within the runs' spread at
  250 (the runs' 99.9th percentiles 1.28 to 2.06 against 1.23 to 3.44 ms,
  the 3.44 a run with a burst of 6 to 7 ms frames of the machine; the 99th
  percentiles 0.96 to 1.05 against 0.94 to 1.00).
- The maximum of `o2match` uncapped (and `o4match`'s input to present at
  `base`) is the first frame of the window: the loading screen's last
  frame (a 1.6 to 2.2 ms 2D draw, no cgame) fell into the window in 3 of
  `new`'s 5 runs and none of `base`'s, a frame earlier; the match's first
  frame is 1.8 to 2.1 ms in both. The map load 0.02 to 0.03 s longer (the
  sounds' buffers).

Not done: cgame's events themselves (each of a frame's model commands is
built as an event by name, `new Event(name)`, its arguments copied as
strings, then dispatched: in the slow frames of `o2match` the event
lookups by name are about 15 µs, the rest the events' work: bullet traces,
impact marks, sounds) and the init commands of a new effect (0.1 ms each,
none in these two matches); the mark fragments' early rejects (JayRewd's,
exact, `R_MarkFragments` is 0.017 ms of `o2match`'s slow frames and the
rejects save part of its clipping); cgame's entities' animation (the
skeletons' bone transforms); the world leaves and the back end's draws,
which grow with what is in view.

`vk.h` documents each call. The points below are what OpenGL used to provide
implicitly and the Vulkan implementation reproduces.

Sound system (2026-10-23, Plan 31; see Implementation notes, Sound system;
the engine's client, both renderers). JayRewd's pull request #2 replaced
OpenAL with the Omaha mixer. Its main-thread work, from a temporary build
that timed the sound host's calls (`o2match` and `o4match` at 1920x1080
uncapped, `fixedtime 16`, the merged pull request, µs; a frame's sum over
the 15000 frames):

| in a frame | mean | p99 | p99.9 | max |
| --- | --- | --- | --- | --- |
| `o2match`: obstruction traces (109 thousand, 3 rays each) | 57 | 304 | 678 | 927 |
| the same with `s_obstructionBudget 2` (since removed; 17.6 thousand) | 11 | 79 | 156 | 363 |
| `o4match`: obstruction traces (101 thousand) | 31 | 136 | 189 | 319 |
| the rest of `S_Respatialize` (positions, loops) | 5 to 7 | | | |
| `S_Update` (the voices' updates, music, the mixer's returns) | 7 to 8 | 25 | 50 to 56 | 89 to 123 |

and in single frames: a walk of every global alias at a sound's first play
(`S_OMAHA_ClassifySfx`, 153 to 161 a match, 0.18 to 0.89 ms each) and at
every start of a streamed sound that isn't combat speech (0.27 to 0.52 ms
each, a player's taunt, a plane's fly-by), a pain line loaded at its first
play (0.09 to 0.52 ms), and the streams' waits for their worker (0.12 to
0.16 ms at a song's start, 0.07 to 0.10 ms at a detach) and, with the
merge's Plan 29 thread, decodes (0.3 to 0.55 ms at a start or a loop).
After Plan 31: the first plays' walk 3 to 9 µs, a song's start 3 to 41 µs,
no wait; the obstruction traces as they were (the pull request's rule), an
option for them (`s_obstructionBudget`, off: it changes when the muffling
follows a move), removed once timed at real speed (below).

These numbers overstate the obstruction traces by far (found 2026-10-23,
when the pull request's author asked; Plan 31 had them at a quarter in a
real match): the device plays the sounds in real time while a match is
played back faster (30 times at `fixedtime 16` uncapped, 4 times at 250
frames a second), so every sound stays playing that many times longer in
the match, and every playing world sound is traced every 100 ms of it. The
same matches whole at real speed (250 frames a second with `fixedtime 4`,
`perf.py --caps 250 --exec 'set fixedtime 4'` and a quarter of the frames
in the demo list: 60000 frames, 240 s; the trace build of the final code,
µs):

| at real speed, in a frame | mean | p99 | p99.9 | max |
| --- | --- | --- | --- | --- |
| `o2match`: obstruction traces (8477; 111 thousand at `fixedtime 16`) | 1.7 | 51 | 142 | 369 |
| `o4match`: obstruction traces (13197; 101 thousand) | 1.5 | 25 | 48 | 118 |
| `S_Update` | 7.0 to 7.7 | 31 to 32 | 70 to 76 | 824 to 1367 |

A trace's 3 rays take 5 to 7 µs (the median), up to 0.37 ms where they
cross much of the map. Played back faster, the many sounds left playing
made the slow frames: up to 26 traces a frame of 40 µs each in one part of
`o2match`. Spreading the second checks of the sounds started in the same
frame over the 100 ms (tried) took the slowest 0.1 % from 0.73 to 0.68 ms
there, so it was left out. `S_Update`'s slowest frame at real speed, 1.3
ms once in `o2match`, was spent in the update of a stream channel that
wasn't streaming (it returns at its first check): the trace times wall
time, so it was the main thread off its core for a moment, the frame on
time (3.98 ms); a replay with each stream update's state showed such idle
updates at 40 to 60 µs too. The others (0.4 to 0.8 ms) aren't broken
down.

Results (idle machine: other processes at most 0.7 CPUs, no Plex
transcoding; `base` (a5120199, OpenAL), `raw` (the merge commit: the pull
request with the conflicts resolved), `new` (Plan 31) and `budget` (`new`
with `s_obstructionBudget 2`, the option since removed), interleaved, 5
runs of 15000 frames of `o2match` and `o4match` at 1920x1080 in a window,
`perf.py --caps 0` and `--caps 250`, `--preload libinlat.so --framelog`,
the same renderer and cgame; the medians of the runs' percentiles of the frames from the third
on (the measured window of `new` started a frame earlier in every run, on
the first frame after the load, 4.7 ms of input to present that the other
builds' windows leave out); ms):

| demo | `com_maxfps` | build | frame time p99.9 | max | p99 | mean | input to present p99.9 | max | p99 | median |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | `base` | 1.305 | 1.66 | 1.003 | 0.479 | 1.253 | 1.64 | 0.946 | 0.406 |
| | | `raw` | 1.613 | 2.02 | 1.205 | 0.537 | 1.550 | 1.86 | 1.130 | 0.480 |
| | | `new` | 1.563 | 2.06 | 1.181 | 0.532 | 1.531 | 1.97 | 1.134 | 0.477 |
| | | `budget` | 1.296 | 1.64 | 1.010 | 0.484 | 1.272 | 1.60 | 0.965 | 0.419 |
| | 250 | `base` | 4.163 | 4.38 | 4.045 | 4.000 | 1.291 | 2.29 | 0.949 | 0.467 |
| | | `raw` | 4.225 | 5.47 | 4.044 | 4.000 | 1.378 | 2.24 | 1.018 | 0.477 |
| | | `new` | 4.195 | 4.51 | 4.043 | 4.000 | 1.271 | 2.17 | 1.013 | 0.471 |
| | | `budget` | 4.196 | 4.43 | 4.043 | 4.000 | 1.262 | 2.34 | 0.964 | 0.468 |
| `o4match` | 0 | `base` | 1.820 | 2.54 | 1.386 | 0.874 | 1.785 | 2.49 | 1.339 | 0.810 |
| | | `raw` | 1.896 | 3.02 | 1.452 | 0.894 | 1.861 | 2.56 | 1.415 | 0.838 |
| | | `new` | 1.771 | 2.53 | 1.389 | 0.889 | 1.701 | 2.48 | 1.351 | 0.834 |
| | | `budget` | 1.809 | 2.63 | 1.386 | 0.873 | 1.784 | 2.60 | 1.338 | 0.823 |
| | 250 | `base` | 4.144 | 4.34 | 4.038 | 4.000 | 1.712 | 2.27 | 1.353 | 0.848 |
| | | `raw` | 4.197 | 5.03 | 4.039 | 4.000 | 1.779 | 2.13 | 1.371 | 0.843 |
| | | `new` | 4.164 | 5.05 | 4.040 | 4.000 | 1.856 | 2.28 | 1.362 | 0.855 |
| | | `budget` | 4.147 | 4.29 | 4.038 | 4.000 | 1.800 | 2.39 | 1.386 | 0.842 |

- The pull request as it came costs `o2match` 0.06 ms of mean frame time
  and 0.2 to 0.3 ms at the 99th and 99.9th percentiles uncapped, input to
  present the same, against OpenAL; `o4match` 0.02 ms and 0.07 ms. Most of
  it is the obstruction traces (above), which Plan 31 leaves as they are:
  `new` takes 0.05 ms off `o2match`'s 99.9th percentile and 0.12 off
  `o4match`'s (the waits and walks), the rest of `raw`'s cost stays. Most
  of that is the playback's speed: at real speed the traces cost 1.5 to
  1.7 µs a frame (above).
- With `s_obstructionBudget 2` `o2match` is back at OpenAL's level (99.9th
  percentile 1.296 against 1.305, mean 0.484 against 0.479), and so is
  `o4match`; at real speed there is little to take off (the option is
  removed).
- At 250 frames a second every build fits its 4 ms; input to present
  within the runs' spread of OpenAL's (the 99.9th percentile of 15000
  frames is the 15th slowest; `o4match`'s 1.71 to 1.86 ms over the four
  builds), its median 0.47 and 0.84 to 0.86 ms in all.
- The mixer thread: the game takes 1.08 to 1.10 CPUs uncapped against
  OpenAL's 1.01 to 1.03 (its main thread 0.95 to 0.99 either way), at 250
  0.41 to 0.51 against 0.40 to 0.50.
- With a 3 minute test song as both maps' music (3 runs of `base` and
  `new`, uncapped): `o2match`'s song streams from the load on: the same
  picture (99.9th percentile 1.268 and 1.536 ms, mean 0.489 and 0.531);
  its decoding is on the worker and no longer shows (Plan 29's 0.85 to 0.92
  ms buffers with OpenAL).

Headphones (2026-10-23, after Plan 31; see Implementation notes, Sound
system): the mixer's callback timed (a temporary build, the null device's
5 ms callbacks, `o2match` with `s_speaker_type 1` played at about real
time, `com_maxfps 63` with `fixedtime 16`, 49 s, 9800 callbacks; the
medians of 3 interleaved runs, µs):

| build | rate, filters | mean | p99 | p99.9 |
| --- | --- | --- | --- | --- |
| merged (`35b5c93e`) | 44.1 kHz, designed in the callback (the default as it came) | 83.0 | 239 | 312 |
| final | 48 kHz, baked (the default now) | 82.2 | 190 | 239 |
| merged | 48 kHz, `s_hrtf_live_query 1`: designed in the callback | 88.4 | 248 | 309 |
| final | 48 kHz, `s_hrtf_live_query 1`: designed on the thread | 83.1 | 197 | 255 |

A filter's design takes 20 µs (18 at best, 48 kHz); the merged code made
every new voice's first in the callback it started in and up to 4 more a
callback for voices that turned. A sound start takes 0.61 s at 48 kHz
against 0.88. One run of the last row had a slow stretch (99.9th
percentile 0.57 ms, max 0.90), the others' max 0.29 and 0.34 ms. The
frame isn't touched: the mixer runs on the device's thread.

Surface list (2026-10-23, Plan 32; see Implementation notes, CPU stages
and skeletal meshes). JayRewd's pull request #3 ("cut beSurfs CPU ~69µs on
outdoor scenes": 2401 to 3059 frames a second in an outdoor demo of its
author's benchmark, other hardware) changes the back end's surface list
and the generic stage iterator in six places. Its author measured with
per-surface profiling timers (`com_prof`, not part of the pull request);
the largest gain, 36 µs, came from calling those timers less often. What
each part does in two matches (temporary counting builds, one run of
`o2match` and `o4match` each; per frame, over the 15000 measured):

| per frame | `o2match` | `o4match` |
| --- | --- | --- |
| lit surfaces (a new sort with a lit entity or static model) | 100.7 | 251.0 |
| of them the same entity, lit the same way, as the one before (the pull request's cache hits) | 6.1 | 11.4 |
| of the hits, where something not cached was set up (a static model's dynamic lights) | 2.0 | 4.2 |
| `RDF_HUD` lighting (`hudSphere`, computed for each surface) | 0 | 0 |
| skeletal surfaces (`RB_SkelMesh`, each a walk of its TIKI's meshes) | 26.4 | 40.2 |
| stages drawn on the CPU (not static, not skinned; 2D too) / their vertexes | 96.7 / 1525 | 125.0 / 1927 |
| of them with tess's texture coordinates (no copy) / vertexes | 65.9 / 1401 | 101.6 / 1801 |
| of them with one color (the constant, no array) / vertexes | 21.6 / 87 | 36.6 / 147 |
| entries in the mesh table at the end | 82 | 112 |

The parts:

- Texture coordinates from tess (kept, fixed): 1400 to 1800 vertexes'
  coordinates a frame no longer copied into `tess.svars.texcoords` (the
  upload copies them from tess instead). A stage whose `rgbGen` or
  `alphaGen` is `sCoord` or `tCoord` reads that copy, and the pull
  request gave such a stage whatever was copied before; the game's 14
  such stages all have a `tcMod`, which keeps the copy, but a map's or a
  mod's shader may not.
- One color as the constant (kept, predicted at the load): 22 to 37
  stages a frame, quads (4 vertexes) on average. The constant has another
  pipeline key, which the load (`tr_pipelines.c`) didn't build: with
  nothing recorded (`perf.py --cold`) the pull request created 2
  pipelines in `o2match`'s measured frames and 4 in `o4match`'s (none
  before; 0.1 ms in all here, the driver's disk cache had them, 1 to 2 ms
  each with cold driver caches) and recorded them for the next runs. The
  load now builds the constant's keys for those stages instead of the
  color array's (188 and 222 pipelines at the load, from 191 and 219) and
  no pipeline is made in the frames.
- The mesh table (kept, fixed): 26 to 40 lookups a frame instead of walks.
  The pull request's table was never cleared, so the pointers of models
  freed at a map change stayed in it (a new model at the same address
  would get the old one's mesh and skel), and after about ten maps (82 to
  112 entries each) its 1024 entries would be full and a new surface's
  lookup would probe all of them; the front end wrote every surface of
  every model into it in every frame. Now cleared with the skin data,
  filled at a surface's first lookup.
- The lighting setup cache (left out): 6 to 11 hits a frame, 2 to 4 of
  them where it saved a few distance checks (a static model's dynamic
  lights), the others where the entity's sphere was taken from its cache
  anyway. The sphere that is computed for every surface, `hudSphere`, is
  for `RDF_HUD` scenes, which only the inventory's item models are
  (`cl_invrender.cpp`; the view weapon is drawn in the main view). The
  cache kept its last setup in static variables, from one draw list to
  the next: a temporary build counting the hits taken before a list's
  first setup found 2 in `o2match`, 0 in `o4match` (a frame's first list
  then uses the lighting set up in the list before: a static model's
  dynamic lights, an entity's light grid value or another entity's sphere).
- One loop for the surfaces of one sort, and `RB_EndSurface`'s canary
  checks only near a full tess (left out): the same work as the list's own
  loop for those surfaces, and three loads of lines that every batch
  touches.

The code that changed, timed (a temporary build that added up the TSC
cycles of the stage iterator's CPU work, `RB_SkelMesh`'s mesh lookup and
`VK_UploadGeometry` in every frame; the build before and Plan 32, 3
interleaved runs each, medians; µs per frame):

| | `o2match` before | after | `o4match` before | after |
| --- | --- | --- | --- | --- |
| stages on the CPU (97 and 124 a frame): texture coordinates, colors | 5.85 | 4.95 | 12.47 | 10.90 |
| `RB_SkelMesh`'s mesh (26 and 40 a frame) | 0.50 | 0.35 | 0.92 | 0.53 |
| `VK_UploadGeometry` (103 and 153 a frame) | 8.75 | 8.32 | 13.01 | 12.75 |

1.5 and 2.2 µs a frame in all; the upload doesn't take longer for the
coordinates read with a stride of 16 (fewer color bytes to copy).

Results (idle machine: other processes at most 0.6 CPUs, no Plex
transcoding; `base` (4f2bdbf2), `raw` (the pull request merged, its
conflict resolved) and `new` (Plan 32), interleaved, 5 runs of 15000
frames of `o2match` and `o4match` at 1920x1080 in a window, `perf.py
--caps 0` and `--caps 250`, `--preload libinlat.so --framelog`; the
medians of the runs' percentiles from the third frame on, in parentheses
the content ones (each frame's median over the runs); ms):

| demo | `com_maxfps` | build | frame time p99.9 | max | p99 | mean | input to present p99.9 | max | p99 | median |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 0 | `base` | 1.561 (1.529) | 1.94 (1.82) | 1.179 (1.147) | 0.536 | 1.516 | 1.80 | 1.132 | 0.479 |
| | | `raw` | 1.534 (1.506) | 1.85 (1.82) | 1.184 (1.148) | 0.531 | 1.494 | 1.81 | 1.138 | 0.472 |
| | | `new` | 1.540 (1.536) | 1.92 (1.84) | 1.162 (1.153) | 0.531 | 1.524 | 1.85 | 1.125 | 0.473 |
| | 250 | `base` | 4.206 (4.192) | 4.82 (4.39) | 4.045 (4.041) | 4.000 | 1.304 | 2.35 | 1.015 | 0.473 |
| | | `raw` | 4.199 (4.194) | 5.00 (4.43) | 4.044 (4.042) | 4.000 | 1.279 | 2.19 | 1.021 | 0.478 |
| | | `new` | 4.205 (4.195) | 5.07 (4.44) | 4.045 (4.042) | 4.000 | 1.336 | 2.30 | 1.012 | 0.477 |
| `o4match` | 0 | `base` | 1.832 (1.761) | 2.58 (2.45) | 1.416 (1.366) | 0.891 | 1.809 | 2.54 | 1.371 | 0.830 |
| | | `raw` | 1.790 (1.734) | 2.54 (2.46) | 1.389 (1.345) | 0.895 | 1.754 | 2.54 | 1.355 | 0.835 |
| | | `new` | 1.897 (1.759) | 2.80 (2.47) | 1.396 (1.346) | 0.888 | 1.881 | 2.67 | 1.360 | 0.834 |
| | 250 | `base` | 4.157 (4.140) | 4.29 (4.26) | 4.041 (4.032) | 4.000 | 1.947 | 2.49 | 1.418 | 0.850 |
| | | `raw` | 4.135 (4.133) | 4.30 (4.28) | 4.038 (4.033) | 4.000 | 1.803 | 2.52 | 1.416 | 0.855 |
| | | `new` | 4.157 (4.137) | 4.29 (4.25) | 4.040 (4.032) | 4.000 | 1.805 | 2.46 | 1.383 | 0.846 |

The renderer's own time (`renderer_timed.so`, mean / 99th percentile):
`o2match` 0.354 / 0.721, 0.352 / 0.730, 0.353 / 0.727 ms uncapped and
0.365 / 0.726, 0.367 / 0.726, 0.366 / 0.734 at 250; `o4match` 0.674 /
1.115, 0.677 / 1.101, 0.672 / 1.121 and 0.701 / 1.187, 0.701 / 1.183,
0.697 / 1.163. Everything is within the runs' spread (one run's 99.9th
percentile of `o4match` uncapped ranges from 1.72 to 2.24 ms within a
build; the content ones, which leave most of that out, are 1.734 to
1.761): the 1.5 to 2 µs a frame of the parts are too small to show in
frame times on this machine.

### Coordinate conventions

- Projection matrices are OpenGL ones (column-major, clip z in [-w, w], y up).
  With `VK_EXT_depth_clip_control` (enabled when the device has it) the
  pipelines use GL's clip space depth (`negativeOneToOne`) and the projection
  is used as is, so clip space z, near/far clipping and depth values are the
  same as GL's; otherwise the backend remaps depth with `z' = (z + w) / 2`,
  pre-multiplied into the projection. Projection × modelview is computed on
  the CPU in the order of Mesa's `_ModelProjectMatrix` and pushed as one
  matrix, and the vertex shader transforms with the operations of Mesa's
  fixed-function vertex program (a multiply, then fused multiply-adds), so
  clip coordinates come from the same operations on the same values as GL1's
  on Mesa. y is flipped with
  a negative viewport height (core since Vulkan 1.1).
- Viewport, scissor and `vk_read_pixels` use window coordinates with the
  origin at the bottom-left: Vulkan `y = windowHeight - (y + height)`.
- `vk_set_depth_range(n, f)` maps to `VkViewport::minDepth/maxDepth`. The
  renderer uses (0, 1), (0, 0.3) for the weapon, (1, 1) for the sky and (0, 0)
  for debug lines.
- `vk_read_pixels` returns rows bottom row first, tightly packed. Depth is the
  window-space depth (the flare code converts it back with `2 * depth - 1`).
  Pixels outside the framebuffer read as 0.
- Texture data is uploaded top row first and t = 0 addresses the first row,
  same as Vulkan, so textures need no flip.
- Front faces are counter-clockwise in window space (GL default). Once y is
  flipped (negative viewport height or a flipped projection, they give the
  same framebuffer coordinates), `VK_FRONT_FACE_COUNTER_CLOCKWISE` matches GL.

### State (`vk_set_state`)

Blend factors: the usual mapping of `GLS_SRCBLEND_*` / `GLS_DSTBLEND_*`; no
blend bits means blending off. `GLS_DEPTHFUNC_EQUAL` selects EQUAL, otherwise
LESS_OR_EQUAL. `GLS_DEPTHTEST_DISABLE`, `GLS_DEPTHMASK_TRUE` and
`GLS_POLYMODE_LINE` (front and back) map directly. Like GL, there are no depth
writes while the depth test is disabled.

Alpha test (fragment discard), from the GL1 code:

| bits | keep fragment when |
| --- | --- |
| `GLS_ATEST_GT_0` | alpha > 0.05 |
| `GLS_ATEST_LT_80` | alpha < 0.5 |
| `GLS_ATEST_GE_80` | alpha >= 0.5 |
| `GLS_ATEST_LT_FOLIAGE1` / `GE_FOLIAGE1` | alpha < / >= `r_alpha_foliage1` (default 0.75) |
| `GLS_ATEST_LT_FOLIAGE2` / `GE_FOLIAGE2` | alpha < / >= `r_alpha_foliage2` (default 0.75) |

The test applies to the final fragment alpha (after texture env, before fog).

`GLS_CLAMP_EDGE`, `GLS_MULTITEXTURE` and `GLS_MULTITEXTURE_ENV` are never set
by the current shader parser. `GLS_COLOR_NOMASK` (shader keyword
`nocolorwrite`) is set but GL1 ignores it. Fog bits are handled by `R_State`
through `vk_set_fog*`.

The framebuffer has no destination alpha, like the GL1 visual on Linux (SDL
asks for no alpha bits): `DST_ALPHA` and `ONE_MINUS_DST_ALPHA` blend
factors are the constants 1 and 0 and `SRC_ALPHA_SATURATE` gives 0, as in
GL1, whatever the color target's alpha holds (a swapchain image's is
undefined until written; only a test shader, `textures/mohtest/brick`,
uses `GL_DST_ALPHA`). Pipelines that write all of RGB without blending
write alpha too, as 1, because a write mask without alpha is a partial
write, which costs DCC-compressed color targets: 1 to 2 % of the GPU time
at 3840x2160, 4 % in views that clear the color buffer first, 6 % in the
`m6l1a` forest with 4 samples (Radeon 780M, see Performance). Blending
pipelines write alpha (unchanged) only with multisampling: without it RADV
then rounds about 1 blended pixel in 10^5 one step lower than with the RGB
mask (and GL1), in both directions (15 pixels of `o2_pipe2`, the wet pipes
of `obj_team2`), and blending reads the target anyway.

### Fragment pipeline

1. Vertex color: per-vertex `colors` or the constant `color` of the geometry.
2. Texture unit 0 (if `st[0]` is set) combines with the vertex color using its
   texture env, unit 1 (if `st[1]` is set) combines with the result of unit 0.
   The formulas are next to `texEnv_t` in `vk.h`; `TEXENV_ADD` clamps the sum
   to 1 like GL. For `TEXFMT_RGB8/RGB5` (and `BC1_RGB`) textures alpha samples
   as 1 (`A = ONE` swizzle, or, on a portability subset device without
   `imageViewFormatSwizzle`, the shader sets it: see Implementation notes,
   Textures); with `TEXENV_REPLACE` GL then keeps the previous
   alpha instead of 1. `TEXENV_COMBINE4` is drawn as modulate (no shader
   produces it).
3. Alpha test.
4. Fog, if enabled: `f = clamp((end - d) / (end - start), 0, 1)` with
   `d = |z_eye|`, `rgb = mix(fogColor.rgb, rgb, f)`; alpha unchanged. The
   backend computes it like Mesa's fixed-function fog, which makes it match
   GL1 to the bit on Mesa: the vertex shader writes `|z_eye|` (row 2 of the
   modelview, same operations as the position) as the fog coordinate, the
   fragment shader computes `f = sat(fma(d, -1 / (end - start), end / (end -
   start)))` with both constants computed on the CPU in float, then
   `rgb = fma(rgb, f, fogColor * (1 - f))`. The fog color is clamped to
   [0, 1]. Fog on/off is part of the pipeline key; color and range are push
   constants.
5. User clip plane: discard where `dot(plane, eyePos) < 0`, eye position =
   modelview * vertex. The backend transforms the plane to object space
   (`plane * modelview`) and clips with `gl_ClipDistance`; on devices without
   `shaderClipDistance` the fragment shader discards on the interpolated
   distance instead (same pixels, except that triangles crossing the plane
   aren't split).
6. Blending, color mask, depth and stencil per the state.

Stencil ops are the clamping GL ones (`INCREMENT_AND_CLAMP`,
`DECREMENT_AND_CLAMP`). Stencil shadows (`r_shadows 2`) and
`r_measureOverdraw` need at least 4 stencil bits.

Polygon offset (`vk_set_polygon_offset`, from `r_offsetfactor` -1 /
`r_offsetunits` -2) maps to depth bias slope/constant factors, for filled
polygons only (`GL_POLYGON_OFFSET_FILL`).

Lines (line primitives and polygons in line mode) use GL's rasterization
rule for aliased lines, the diamond-exit rule, which is Vulkan's Bresenham
mode (`VK_KHR_line_rasterization` or `VK_EXT_line_rasterization` with
`bresenhamLines`; RADV's default line mode rasterizes the same pixels).
`vk_set_line_stipple(factor, pattern)` stipples them like `glLineStipple`
(factor clamped to [1, 256], 0 disables it; needs `stippledBresenhamLines`);
the stipple counter restarts at each segment of a line list, as GL does for
`GL_LINES`. Line width above 1 needs `wideLines`. All of this only affects
debug drawing and is ignored when unsupported: without the extension (or
`bresenhamLines`) lines have the device's default rule, the same pixels on
RADV (`strictLines` false, its lines follow the diamond-exit rule anyway;
`r_vkFallbacks 4` gives the same frames as before in `mohdm1_debug`), and on a
device with `strictLines` (rectangular lines: llvmpipe) a pixel here and
there on nearly diagonal lines moves by one (`r_showtris` 1 and 2 and
`r_shownormals` in `mohdm1_debug` on llvmpipe: 0.5 to 1.4 % of the pixels,
isolated pixels along the lines; the triangles under them are the same);
without `stippledBresenhamLines` lines aren't stippled (only the game's
debug lines are, and the view covers them, see Testing).

### Clears

`vk_clear` clears the current scissor rectangle. Like `glClear` it honours the
color mask, the depth write bit of the current state and the stencil write
mask. Depth clears to 1.0, stencil to 0. `color` may be NULL when
`CLEAR_COLOR` is not set. The backend uses `vkCmdClearAttachments`, which
ignores masks, and draws a quad for a color clear with a partial color mask,
and on RADV (without multisampling) for a color clear to a color with a
component other than 0 or 1, which RADV clears with comp-to-single, slower
than the quad at 3840x2160 (see Performance, GPU time at 3840x2160); a
partial stencil write mask clears all bits (the renderer never sets one).
Without a stencil buffer (`r_stencilbits` 0, see Implementation notes,
Render target) stencil clears do nothing and stencil reads give 0. The
clear color is first rounded to the nearest 8-bit value on the CPU: drivers
convert clear colors with float arithmetic that can round the other way near
half a step (RADV gives 179 for 0.7, GL1 the correctly rounded 178; it shows
in the fog-colored background of `m2l3`).

### Textures

- `vk_create_texture` gives the image a non-zero handle in `image->texnum`.
  Handles must never be reused: the renderer caches bound handles per unit
  (`glState.currenttextures`).
- Calling it again on an image replaces its storage (scratch images for
  cinematics, ghost textures); draws already recorded keep the old storage, so
  release old storage only once the GPU is done with it. Same for
  `vk_destroy_texture`.
- `vk_bind_texture` binds the image, not a particular storage: a draw uses the
  storage the image has when the draw is recorded.
- Uploads happen mid-frame (cinematics every frame, ghost textures; the
  dynamic lightmaps of `R_UploadDlights` before the dynamic lights were
  drawn per pixel): draws recorded before an upload must see the old
  contents.
- Sampler: wrap from `image->wrapClampModeX/Y` (`WRAP_REPEAT` repeats,
  `WRAP_CLAMP` clamps to the edge); textures with one mip level use linear
  min/mag filtering; mipmapped ones use the filter from
  `vk_set_texture_filter` (`r_textureMode`, default
  `GL_LINEAR_MIPMAP_LINEAR`) and anisotropy from
  `r_ext_texture_filter_anisotropic` / `r_ext_max_anisotropy`.
- `TEXFMT_RGBA4/RGB5` may be stored as 8-bit formats (they are).
- BC formats come from DDS files; the texture env and sampler rules are the
  same.

### Frames and synchronisation

- `vk_begin_frame` is called from `RB_DrawBuffer` at the start of each frame,
  but `RE_StretchRaw`, texture uploads and other front-end paths can run
  before the first command of a frame: the frame starts lazily on the first
  operation. In OpenMoHAA the back end mostly runs inside the 2D exports:
  `Set2DWindow`, `DrawStretchPic`, `DrawString`, ... call
  `R_IssuePendingRenderCommands` (which returns at once when no command is
  queued, as for most of them) and draw at once (or leave their surface
  open for the next 2D draws, see Implementation notes, 2D batches), and the
  UI calls `Set2DWindow` before and after the 3D view, so the 3D view's draw
  surfaces are executed there, not in `RE_EndFrame`.
- The color contents of the frame are undefined until it draws or clears
  them, like a GL back buffer (frames are drawn into the swapchain image,
  see Implementation notes). Screenshots, levelshots and resampled
  screenshots are therefore taken at the end of the frame, in
  `RB_SwapBuffers` (`RB_TakePendingScreenshots`), where GL1 read the back
  buffer when the command ran, before the frame was drawn (an older frame on
  Mesa); the "Wrote" centerprint of the `screenshot` command is run once
  the file is written, so it isn't in the picture. With `pause` (see
  Deterministic 3D views) both show the same frame.
- `vk_finish` is called at swap for frames with no 3D view (menus) and around
  cinematics, as `glFinish` was. It waits for the fence of the last
  submission; what the current frame recorded is submitted at
  `vk_end_frame`.
- `vk_read_pixels` must see everything recorded so far in the frame and the
  frame must continue afterwards (screenshots, AVI capture,
  `r_measureOverdraw`). It waits for the GPU, which is acceptable for these
  occasional reads. Depth and stencil read as 0 until the depth image is
  readable: without multisampling it only is from the start of the frame
  after a depth or stencil read, or after `vk_want_depth_reads` (called
  before the frame starts, it makes that frame's reads work;
  `RE_BeginFrame` calls it for `r_measureOverdraw`), until 300 frames
  after the last one (see Implementation notes, Render target).
- `vk_queue_depth_reads` records reads of single depth values in order with
  the draws without waiting; `vk_get_depth_read` returns a result once the
  frame that recorded it has completed. The backend collects the results
  when it reuses the frame slot, so a read made in frame N is available
  from frame N + 2 on (2 frames in flight), whatever the GPU timing, and
  stays available for 2 frames. Ids from before a `vid_restart` are lost,
  and the reads of a frame in which the depth image isn't readable get id
  0 (lost); they make it readable from the next frame.

### Flares

There are two flare systems in the renderer:

- The lens flares of `tr_sun_flare.cpp` (`R_DrawLensFlares`): the sun flare
  of maps with a sun (`sunflarename`, default "sun"), entities with
  `RF_LENSFLARE`/`RF_VIEWLENSFLARE` and dlights of type `lensflare` or
  `additive`. They test visibility with collision traces (`CM_BoxTrace`),
  not the depth buffer, are always on and draw through immediate mode; they
  match GL1 (`views/mohdm1_sunflare.txt`). `r_drawSun` is unused (the
  `RB_DrawSun` call is `#if 0` in both renderers).
- The Quake 3 light flares of `tr_flares.c` (`r_flares`, `r_flareSize`,
  `r_flareFade`): `RB_AddFlare` has no caller in OpenMoHAA. `MST_FLARE`
  surfaces load as `SF_FLARE`, whose tesselation function is empty in both
  renderers, no shipped map has one, and `RB_AddDlightFlares` is commented
  out. `RB_TestFlare` therefore never runs in the game.

GL1's `RB_TestFlare` read the depth under each flare with `glReadPixels`
(a GPU sync per flare and per frame) and reset `glState.finishCalled`, so
`glFinish` ran at every swap. The Vulkan version queues the reads of all the
flares of a view with one `vk_queue_depth_reads` call at the same point of
the frame, and tests each flare against the newest read that has completed,
with the eye depth and projection of the frame that made it: no flush, no
wait, no reset of `finishCalled`. Until a read completes, a flare keeps its
visibility; the fade (`r_flareFade`, `fadeTime`) is GL1's code. The effect is
that visibility changes reach the fade 2 frames later than in GL1: a flare
that appears starts fading in 2 frames late (32 ms with `fixedtime 16`, at
most 0.22 of intensity behind with `r_flareFade 7`), then both are equal. A
flare that faded out stays in the active list while it is added (its next
read is pending) instead of being dropped and re-added, which gives the
same result. This was checked with a temporary hack that adds a flare for
every dlight (`RB_AddDlightFlares`) in the old (synchronous) and new code:
frames are identical once the flares have faded in, and the logged
intensities of the new code follow the old ones exactly 2 frames later.
The first reads (and the first after 300 frames without any) are lost
while the depth image becomes readable (see Frames and synchronisation),
which delays the first test by one more frame. Like GL1, `RB_RenderFlare` doesn't set `tess.vertexColorValid`, so
`rgbGen vertex` skips the flare color and the quad draws with stale colors;
the test set it to see the fade.

### Capabilities (`vk_initialize`)

`vk_initialize` reports what the device supports; `R_ApplyExtensionCvars`
(`tr_init.c`) then turns features off from the same cvars the GL1 renderer
used, so shader collapsing and image loading follow the user's config:

| glConfig field | device value | turned off by |
| --- | --- | --- |
| `numTextureUnits` | 2 (shaders using `nextBundle` fail with fewer) | `r_ext_multitexture 0` |
| `textureEnvAddAvailable` | true | `r_ext_texture_env_add 0` (the Linux default, so additive second stages draw as two passes like GL1) |
| `textureCompression` | `TC_S3TC_ARB` if `textureCompressionBC` is supported | `r_ext_compressed_textures 0` (default); `.dds` files are only loaded when this is not `TC_NONE` |
| `textureFilterAnisotropic`, `maxAnisotropy` | `samplerAnisotropy`, `maxSamplerAnisotropy` | `r_ext_texture_filter_anisotropic 0` (default) |
| all of the above | | `r_allowExtensions 0` |

`vk_initialize` also fills `maxTextureSize` (`maxImageDimension2D`; uploads
are downscaled to it), the vendor string (from the PCI vendor id), the
renderer string (device name), the version string (Vulkan version, driver
name and info) and the extensions string (enabled device extensions).
`VKimp_Init` sets the requested `colorBits`/`depthBits`/`stencilBits` from
`r_colorbits`, `r_depthbits` and `r_stencilbits`; the backend replaces them
with what it picked: 24 color bits, the depth bits of the depth format
(`D24_UNORM_S8_UINT` when available, `D32_SFLOAT_S8_UINT` on AMD, which has no
D24), and 8 stencil bits only when `r_stencilbits` asked for a stencil buffer
(0 by default on Linux, like GL1, which disables `r_shadows 2` and
`r_measureOverdraw`); the depth format always has a stencil aspect.
`deviceSupportsGamma`, `vidWidth`, `vidHeight` and `isFullscreen` come from
`sdl_vkimp.c`. `r_stereoEnabled` is not supported. `r_ext_multisample`
selects the sample count (see Implementation notes).

## Implementation notes

- **No OpenGL** (2026-10-23, the user's request: "strip all of OpenGL from
  the code like we did for metal"). Deleted: upstream's GL1 and GL2
  renderers (`code/renderergl1`, `code/renderergl2`, `renderer_opengl1`
  was built and shipped as a fallback until then), `renderercommon/qgl.h`,
  `sdl/sdl_glimp.c`, `cmake/renderer_gl1.cmake` and `renderer_gl2.cmake`,
  the options `BUILD_RENDERER_GL1`, `BUILD_RENDERER_GL2` and
  `USE_RENDERER_DLOPEN` (the renderer is always a library loaded at run
  time: the tools load copies of other builds by name), the Emscripten
  platform (WebGL only) and its `code/web` page, the macOS OpenGL framework
  and the unused macOS CI workflow (a static GL1 build), `OPENGL_DRIVER_NAME`,
  the unused `GLimp_*` declarations and SMP stubs, and the OpenGL code
  that was never built: SDL2's vendored GL and GLES headers,
  recastnavigation's demo, the GameSpy `gt2action` sample, the map
  compiler's `gldraw.c`, and the tools' `libglts.so`. Renamed as on
  `metal`: the state helpers (`GL_State` -> `R_State`, `GL_Bind`,
  `GL_Cull`, `GL_TexEnv`, `GL_SelectTexture`, `GL_EnableTexture`,
  `GL_TextureMode`, `GL_SetDefaultState`, `GL_SetFogColor`),
  `gl_filter_min`/`max` -> `tex_filter_min`/`max`, `GLimp_SetGamma` ->
  `VKimp_SetGamma`, and the comments and messages that named OpenGL
  (the error without a Vulkan driver no longer suggests `cl_renderer
  opengl1`, and `CL_InitRef` resets that value to `vulkan`). The window layer no longer calls the refimport hooks
  `Sys_GLimpInit`/`Sys_GLimpSafeInit` (no-ops on Linux and Windows); they
  stay in `refimport_t` so that the layout, and with it renderer copies of
  older builds, still fit. `tools/glcheck.sh` runs the `metal` branch's
  grep (`qgl|opengl|SDL_GL_|GL_[A-Z0-9_]+|glimp`, any case) over `code/`,
  `cmake/`, `.github/`, `misc/` and `CMakeLists.txt`, Markdown files left
  out, and prints whatever is not one of its documented exceptions:
  SDL's own API headers (vendored: SDL 3's since 2026-10-24, without its
  GL, GLES and EGL headers), comments of miniaudio and a Qt guard
  of libjpeg, recastnavigation's own CI files, the shader script keywords
  of the retail `.shader` files (`blendFunc GL_ONE GL_ZERO`), the cvar
  values configs hold (`r_textureMode GL_LINEAR_MIPMAP_NEAREST`,
  `r_drawBuffer GL_BACK`), GLSL's built-ins (`gl_Position`,
  `gl_ClipDistance`, ...), false positives (`fontheader_sgl_t`,
  `GL_USEANGLES`, `EV_UILGL_*`) and the two hooks above; then it checks
  the binaries of a bin dir: no OpenGL renderer, no GL, GLX, EGL or GLES
  library needed and no `gl*`, `glX*`, `egl*`, `wgl*` or `SDL_GL_*` import
  (the build before: `renderer_opengl1.so` and its `SDL_GL_*` imports).
  SDL itself (SDL 3 since 2026-10-24), a library the game ships, still has
  its own OpenGL support, which loads OpenGL only for a GL window; the
  game asks for a Vulkan one.
  Frames: the same as 53d8f9d7 (but in the lists that differ between two
  runs of one build).
- **SDL 3** (2026-10-24, Plan 35; SDL 2.32 before). The code includes
  `<SDL3/SDL.h>` with the vendored headers (`code/thirdparty/SDL3-3.4.18`,
  without SDL's GL, GLES and EGL headers) and with an SDL 3 package alike;
  CMake takes the shared library (`SDL3::SDL3-shared`, 3.2.0 or later: the
  client and the renderer share one SDL), or on Windows with
  `USE_INTERNAL_SDL` the release's `SDL3.dll`, `SDL3.lib` and
  `libSDL3.dll.a` (`code/thirdparty/libs/win32`, `win64`). SDL 3 has no
  SDLmain: `SDL_main.h` in `sys_main.c` gives the client its `WinMain`.
  Installed Linux binaries look for libraries in their own folder first
  (`RUNPATH $ORIGIN`): the package ships `libSDL3.so.0` beside them, as
  Ubuntu 22.04 and 24.04 have no SDL 3. CI builds SDL 3.4.18 from its tag,
  the Linux job on an x86-64 runner with the X11 and Wayland development
  files and a check that both video drivers are in (the ARM runner's cross
  build had none: the `libSDL2-2.0.so.0` of the 1.0.0 download has only
  the offscreen and dummy drivers, and the game ran on the system's SDL 2),
  the unit tests without video. What changed, and what each lower-latency
  API of SDL 3 does here:
  - The window: `SDL_CreateWindowWithProperties` (the position kept across
    `vid_restart`), a fullscreen window takes the display mode closest to
    the video size and `r_displayRefresh` (`SDL_GetClosestFullscreenDisplayMode`,
    `SDL_SetWindowFullscreenMode`: SDL 2's exclusive fullscreen), also when
    `r_fullscreen` toggles, and `SDL_SyncWindow` waits for the window
    system after showing and toggling (SDL 2 did that inside). The modes
    listed come from `SDL_GetFullscreenDisplayModes`: under Wayland the
    output's own and SDL's emulated ones, fewer than Xwayland's list. The
    swapchain size is `SDL_GetWindowSizeInPixels`.
  - Native Wayland: SDL 3 picks it on a Wayland desktop whose compositor
    has fifo-v1 (KWin 6.6 has); the mouse's relative motion then comes
    straight from KWin's relative pointer (`dx_unaccel`), without
    Xwayland in between: injected motion reached the game up to 0.04 ms
    sooner on the median uncapped (0.22 to 0.24 against 0.26 to 0.28 ms,
    partly the higher frame rate; the same at 250, see Performance,
    Present timing). Kept: input to display isn't longer at any cap.
  - The event pump: SDL 3 reads the window system at the first
    `SDL_PollEvent` of a loop (a sentinel ends the loop), SDL 2 at every
    call. Kept: a read per call only catches what arrives during the loop,
    microseconds, and pays its system calls each time. Each pump also
    checks SDL's D-Bus connection and, under Wayland, dispatches what SDL's
    cursor thread read from the socket and flushes the requests that
    queues (SDL internals, before the socket is read; under Wayland mostly
    SDL's answer to KWin's frame callback, which wakes the compositor),
    and with the joystick subsystem up it polls udev and the joysticks
    after the read: 6 µs more per frame than SDL 2 at 250 (Plan 35). Since
    Plan 36 the frame limiter calls `IN_PumpEvents` (`SDL_PumpEvents`)
    once when its polling of the last `COM_POLL_US` starts after a sleep:
    KWin's frame callback has come by then (0.8 to 1.3 ms after the
    present), so the frame's own pump has only what came after, and
    `in_joystick 0` (the default) shuts the joystick subsystem down (its
    hotplug events were ignored with 0 before; `in_restart` starts it
    again). Uncapped, with a wait shorter than `COM_POLL_US` or with
    `com_busyWait 1` nothing sleeps, so nothing more is pumped. Left: the
    D-Bus check (1.2 µs at 250; no
    hint skips it, SDL connects at every `SDL_Init`; checking every 100 ms
    would need a patch of SDL). See Performance, Present timing.
  - Relative mouse motion: `SDL_SetWindowRelativeMouseMode` and
    `SDL_SetWindowMouseGrab`; unaccelerated, as before
    (`SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE` stays off): XInput 2 raw motion
    on X11, the relative pointer on Wayland, and on Windows raw input read
    by SDL 3's own time-critical thread (`GetRawInputBuffer`) as it
    arrives, so the events are queued before the pump (SDL 2 read the same
    messages in it). SDL 3 gives floats: whole counts go to the engine and
    the rest is carried to the next event (SDL 2 truncated every event
    under X11, so a touchpad lost its fractions; a mouse's counts are
    whole and the same). Injected motion (bursts of 2000 random moves at 1
    and 8 kHz through uinput, 10 per variant) reached the engine with the
    sums injected in every variant and no fractions; SDL 3 under Wayland
    got 1999 or 2000 events of a burst (at most one pair merged on the
    way), SDL 2 under Xwayland 1991 to 2000 (up to 10 merged at a burst's
    start).
  - Keys: SDL 3's key events carry the unshifted keycode, as SDL 2's
    (binds unchanged); letters of non-Latin layouts now give Latin keys
    (SDL 3's default). Windows' raw keyboard (`SDL_HINT_WINDOWS_RAW_KEYBOARD`)
    stays off: its events are queued like the window messages before the
    pump, so it reads nothing sooner here, and with text input on (the
    console and the UI) it changes how keys and text come.
  - Event timestamps (SDL 3's are nanoseconds): not used. The engine gives
    an event the time of the previous input read on purpose (a key counts
    as held from the earliest moment it could have been pressed,
    `CL_KeyState`); the real time would shorten a key's share of a command,
    a movement change and no latency. The frame limiter (`Com_FrameWait`,
    `Sys_Microseconds`) doesn't use SDL, so `SDL_GetTicksNS` and
    `SDL_DelayPrecise` don't apply; `VK_Microseconds` keeps
    `SDL_GetPerformanceCounter`.
  - The wheel: whole notches as SDL 2's `y`, the parts of a
    high-resolution wheel or a touchpad carried. Gamepads: the
    `SDL_Gamepad` functions, SDL 2's buttons (SDL 3's extra `MISC2` to
    `MISC6` aren't bound), `in_joystickNo` indexes `SDL_GetJoysticks`.
  - Gamma: SDL 3 has no gamma ramps. On Linux the renderer applies
    `r_gamma` to the textures (its software path; a change shows after
    `vid_restart` or a map load); under Xwayland SDL 2's ramp did nothing
    (Xwayland 24.1's `xwl_randr_crtc_set_gamma` returns without doing
    anything), so `r_gamma` above 1 brightens the game there now. On
    Windows `sdl_gamma.c` sets the ramp as SDL 2 did (`SetDeviceGammaRamp`
    on the window's monitor, the desktop's ramp saved and put back while
    unfocused and at the end; not run on Windows yet).
  - The submit thread's lock and conditions and the HRTF design thread's
    semaphore are SDL 3's (`SDL_Mutex`, `SDL_Condition`, `SDL_Semaphore`:
    the same pthread and POSIX primitives underneath). `sdl_snd.c` (SDL's
    audio, never built: the Omaha mixer uses miniaudio) is deleted, and so
    are `SDL_HasRDTSC` and `SDL_Has3DNow`, gone in SDL 3 (only `CF_ALTIVEC`
    is read).
  - `tools/rtimer/inlat.c` works with SDL 3 and SDL 2 builds: its
    `SDL_PollEvent` takes SDL 3's `bool` and SDL 2's `int`, its
    `SDL_CreateWindow` both signatures, and it wraps
    `SDL_CreateWindowWithProperties`.
- **Loading.** `SDL_Vulkan_GetVkGetInstanceProcAddr` gives
  `vkGetInstanceProcAddr`; every other function is loaded into a `qvk*`
  pointer (lists in `vk_local.h`), device functions through
  `vkGetDeviceProcAddr`. Vulkan 1.1 is required (negative viewport height).
  The device is the first discrete, then integrated, then virtual, then CPU
  device with a graphics queue that can present to the window. Enabled
  features, when supported: `fillModeNonSolid`, `wideLines`,
  `samplerAnisotropy`, `textureCompressionBC`, `shaderClipDistance` (user
  clip plane). Extensions when present: `VK_EXT_depth_clip_control` (GL clip
  space depth, see Coordinate conventions), `VK_KHR_line_rasterization` or
  else `VK_EXT_line_rasterization` (with `bresenhamLines`, and
  `stippledBresenhamLines` for line stipple; `vkCmdSetLineStipple*` is
  loaded under the enabled extension's name), `VK_KHR_portability_subset`
  (MoltenVK, with `VK_KHR_portability_enumeration` on the instance; its
  `imageViewFormatSwizzle` is enabled when it has it, with a fallback
  otherwise, and the bindings of stride 0, which it never allows, have one:
  see Textures and Draws), `VK_KHR_create_renderpass2` with `VK_KHR_depth_stencil_resolve`
  (multisampling), `VK_KHR_swapchain_maintenance1` or else
  `VK_EXT_swapchain_maintenance1` (`swapchainMaintenance1`: present fences,
  see Frames; on the instance `VK_KHR_get_surface_capabilities2` and
  `VK_KHR_surface_maintenance1` or `VK_EXT_surface_maintenance1`, which they
  need). `r_vkFallbacks` turns these off for tests (see Build and run).
- **Render target.** The color target of a frame is the swapchain image it
  presents (`VK_SelectTarget`, at the frame's first render pass, readback or
  present), when the swapchain has the size of the render target
  (`glConfig.vidWidth x vidHeight`) and images that can be color
  attachments and transfer sources: the color format is the swapchain's
  (`B8G8R8A8_UNORM` on RADV, `R8G8B8A8_UNORM` when the swapchain has neither
  of the two), each swapchain image has a view and a framebuffer, and
  readbacks swap red and blue for BGRA. The image comes from
  `UNDEFINED`, so its contents are undefined until the frame draws, like a
  GL back buffer (see Frames and synchronisation). Otherwise (a window
  resized before its `vid_restart`, no usable swapchain) the frame is drawn
  into an offscreen color image of that format, blitted (scaled) to the
  swapchain image at present; frames that drew nothing present nothing. The
  depth/stencil image is always offscreen. Its format is the first of
  `D24_UNORM_S8_UINT`, `D32_SFLOAT_S8_UINT` and `D16_UNORM_S8_UINT` the
  device supports, or without a stencil buffer (`r_stencilbits` 0, the
  default on Linux, where GL1's depth buffer has no stencil either) the same
  depth without the stencil plane (`X8_D24_UNORM_PACK32`, `D32_SFLOAT`,
  `D16_UNORM`; `D32_SFLOAT` on RADV): HTILE then holds depth only, 0.5 % less
  GPU time at 3840x2160 on a Radeon 780M, and the depth clear is a plain fill
  of HTILE instead of a compute pass that keeps its stencil bits. The
  stencil buffer is reported (`glConfig.stencilBits`) only when there is one,
  like GL1's. Without multisampling it can
  only be copied from (`TRANSFER_SRC` usage) while depth or stencil reads
  are wanted (`VK_UpdateDepthTarget`, at the start of a frame, replaces it
  with a readable image after a read or `vk_want_depth_reads`, and with a
  non-readable one 300 frames after the last; the new image starts cleared
  and the old one and its framebuffers go through the deferred destruction
  lists): RADV gives depth images that can be copied from or sampled
  TC-compatible HTILE, which keeps an 8x8 depth tile compressed only while
  it holds at most 4 planes (15 otherwise), and that cost 12 to 13 % of the
  GPU time in the `m6l1a` forest (see Performance). A frame's depth and
  stencil contents at its start are never needed (every 3D view clears
  depth first, 2D drawing doesn't test it), so the switch loses nothing.
  With multisampling the draws use the multisampled depth image, which is
  never copied from, and the single-sampled one is always readable. One
  render pass loads and stores
  both attachments; it ends for every readback and every upload that must
  be ordered with draws, and restarts on the next draw or clear. Drawing
  into the swapchain image saves the copy of every frame: 0.13 ms of GPU
  time at 1920x1080 on a Radeon 780M, 0.47 ms at 3840x2160 (see
  Performance).
- **Multisampling** (`r_ext_multisample` 2 to 16, latched). The draws go to
  multisampled color and depth/stencil images of the sample count asked for
  (the largest the device supports up to it, for color, depth and stencil
  attachments); every render pass resolves the color (average of the
  samples) into the frame's color target, so presenting and color readbacks
  work as without it, and the pipelines rasterize with that many samples
  (no sample shading, no alpha to coverage, like GL1's multisampled visual).
  The depth/stencil buffer is resolved (sample 0, `VK_KHR_depth_stencil_
  resolve`, render pass 2) into the single-sampled depth image only before
  a depth or stencil readback (`VK_ResolveDepth`, an empty render pass of
  its own): resolving it with every render pass cost 1.3 ms per frame at
  1920x1080 with 4 samples. Without the extensions multisampling is off
  (with a warning). Barriers on the depth image include the resolve's color
  attachment stage and access.
- **Frames.** Two frames in flight, with the submit thread too. Each
  frame slot has a command pool with three command buffers (upload, main,
  and present for frames drawn offscreen), a fence, a vertex/index buffer
  (4 MB, doubled when full), a
  32 MB staging buffer, a buffer for 256 depth reads and a query pool of 4
  GPU timestamps (`vkinfo`). A frame drawn into the swapchain image is
  submitted once, waiting for the image's acquire semaphore at the color
  attachment stage, signaling the image's present semaphore and the frame
  fence; the image goes to `PRESENT_SRC` at the end of the main command
  buffer. A frame drawn offscreen submits the rendering, then the present
  command buffer (blit and layout transitions) with the frame fence. The
  swapchain is recreated when the drawable size changes
  or on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`; with no area
  (minimized) the frame is submitted without presenting.
- **Presentation semaphores and swapchains** (`vk_instance.c`, present
  fences and retired swapchains). The semaphores that presents wait for
  belong to the swapchain, one per image, created with it: the semaphore of
  an image is only signaled again once the image has been acquired again
  and its acquire semaphore waited for, which guarantees that the image's
  previous present has completed, its semaphore wait included (Khronos'
  Vulkan Guide, Swapchain Semaphore Reuse). A swapchain that is replaced
  (resize, out of date), or destroyed (minimized, shutdown), is retired with
  its semaphores: nothing is presented to it any more, but the presentation
  engine may still hold them. With `swapchain_maintenance1` every present
  signals a fence (`VkSwapchainPresentFenceInfoKHR`) once the presentation
  engine is done with its wait semaphore; a present's semaphores can be
  destroyed once its fence has signaled, the swapchain once the fences of
  all its presents have, and those of one swapchain signal in present order,
  so a retired swapchain and its semaphores are destroyed once the fence of
  its last present has signaled: checked after each frame's present, waited
  for (at most 1 s each) at shutdown and `vid_restart`, and when 8 retired
  swapchains are pending (not seen). The fences come from up to 16 slots,
  taken in turn; the next present's is chosen and reset right after the
  present before it (`VK_PreparePresentFence`, a slot whose present is done,
  or a new one): nothing is waited for or reset between a frame's input
  and its present, which only adds the fence to its `VkPresentInfoKHR`. On
  RADV two slots are used: Mesa signals the fence in the submission it makes
  for the present anyway (`wsi_common_queue_present`), with the frame's GPU
  work. Without the extension a retired swapchain and its semaphores are
  destroyed when it is retired, which always follows a `vkDeviceWaitIdle`
  after its last present: per the Vulkan specification `vkQueueWaitIdle`
  waits for the queue operations of presents too (`vkQueuePresentKHR`
  "behaves as if it signals a fence payload" for it; the semaphore waits are
  such operations). Before, the 16 semaphores were created with the frames,
  reused by every swapchain and destroyed at shutdown before the swapchain,
  which relied on that wait for both. `vkinfo` prints the number of present
  fence slots and of retired swapchains still pending.
- **Frame pacing.** Right after presenting a frame drawn into the swapchain
  image, `vk_end_frame` acquires the image of the next frame
  (`VK_AcquireNextImage`, the image is "held" until a frame uses it). With
  a vsync present mode (`r_swapInterval` non-zero) `vkAcquireNextImageKHR`
  is where the CPU waits for the display, so the wait now happens before
  the game runs the next frame, as GL's swap waits after queuing the
  present, instead of between the next frame's game logic and its present:
  the time from the start of a frame's recording to its present went from
  16.3 to 1.7 ms at 60 Hz (one refresh less of input latency). Acquire
  semaphores come from a ring of 3 (frames in flight + 1), not from the
  frame slot, since an image is acquired before its frame starts; a
  semaphore is reused once the submission that waited for it has
  completed. A held image's semaphore is waited for by an empty submission
  before the swapchain is recreated or destroyed, and with
  `swapchain_maintenance1` the image is then released
  (`vkReleaseSwapchainImagesKHR`), so a retired swapchain can free it. The
  acquire stays right after the present, and present fences add no wait
  (see Presentation semaphores and swapchains).
- **Submit thread** (`vk_submit.c`, `r_vkSubmitThread 1`, latched; off by
  default, see Performance, Submit thread). The main thread records a frame
  as without it; at `vk_end_frame` it ends the frame's command buffers,
  resets its fence and hands it to a thread (an SDL thread woken through a
  condition variable), which submits and presents it and right after the
  present acquires the swapchain image of the next frame, as `vk_end_frame`
  does without it. At the next frame's first render pass `VK_SelectTarget`
  takes that image if the thread has it, without waiting
  (`VK_TakeSubmitThreadImage`), and the frame is drawn into it as without
  the thread: its first submission waits for the acquire semaphore and
  signals the image's present semaphore. When the thread hasn't acquired
  it yet (or the swapchain doesn't fit the render target), the frame is
  drawn into the offscreen color image, and the thread copies it into the
  image it acquired since (or acquires one), the copy after the frame's
  command buffers in the same `vkQueueSubmit`, only the copy waiting for
  the image. JayRewd's version (a5f5a54a, from which this started) drew
  every frame offscreen and acquired, copied and presented on the thread:
  the copy costs GPU time (0.03 ms at 640x480, 0.5 ms at 3840x2160 on the
  Radeon 780M) and the acquire came between the hand-off and the present.
  - The queue and the swapchain are externally synchronized: while the
    thread has frames, the main thread uses neither. It drains the thread
    (`VK_DrainSubmitThread`: waits until every frame handed over is done)
    to submit itself (mid-frame flushes, `vk_finish`, `vkinfo`) and before
    it replaces or destroys the swapchain, which happens at a frame's first
    render pass as without the thread (the window changed, or the thread
    found the swapchain out of date). The present fences and the retired
    swapchains are the thread's while it runs: it chooses the next
    present's fence after each present and destroys the retired swapchains
    whose presents are done; the main thread retires a swapchain only with
    the thread drained, and hands the image the thread acquired ahead back
    with its own (`VK_ReclaimSubmitThreadImage`, `VK_ReleaseHeldImage`).
  - Frames in flight: as many as without the thread (`VK_FRAMES`, 2): a
    frame slot is used again once the thread is done with its frame
    (`VK_JoinSubmitJob`) and the frame's fence has signaled. The main
    thread waits for that at the end of the frame, before the game reads
    the next frame's input, not at the next frame's first command as
    without the thread (after the input), and with FIFO it also waits for
    the frame it just handed over, whose job ends with the acquire of the
    next frame's image, where the thread waits for the display: the next
    frame reads its input once its image is acquired and is drawn into it,
    as without the thread. Without vsync that acquire doesn't wait and the
    thread works beside the next frame.
  - Acquire semaphores: the ring of `vk.numFrames` + 1, used by the thread
    only. The acquire for frame k (at its job, or after the present of
    frame k - 1) reuses the semaphore of an acquire for frame k - 3 or
    earlier (3 semaphores, 2 frames in flight), and the main thread waited
    for that frame's fence before it handed frame k - 1 over: the
    submission that waited for it has completed.
  - Swapchain images: 5 without vsync (immediate, mailbox), so an image is
    free whenever the thread acquires even while the compositor holds
    some, which adds no lag (every present is shown, or replaced, at once);
    3 with FIFO, the fewest that let it acquire while one image is shown
    and one waits for the display (FIFO keeps every presented image until
    the display takes it: more images would be more frames queued). At
    least the surface's minimum, at most its maximum.
  - The thread calls no `ri` function (the engine's aren't thread-safe):
    its times and the results of its calls stay with the frame's job until
    the main thread joins the frame, which raises the errors then;
    validation messages raised on the thread are kept (formatted without
    `va()`) and printed by the main thread then. `vkinfo` and the frame log
    report its times (`thread*`, `thread_*`).
  - With `r_vkSubmitThread 0` the frames go the way they went before (the
    same code: acquire after the present on the main thread, 2 frames in
    flight, the surface's minimum + 1 images).
- **Mid-frame flush.** `vk_read_pixels`, and uploads that don't fit in the
  staging buffer, submit what the frame recorded so far, wait for it and
  continue in the same command buffers (a new main-buffer "segment").
  Uploads larger than the staging buffer get a temporary buffer.
- **Depth reads** (`vk_queue_depth_reads`). One `vkCmdCopyImageToBuffer`
  with a 1x1 region per read copies the depth aspect into the frame slot's
  depth read buffer (4 bytes per read, the layout of the depth format's
  depth aspect), between the same barriers as `vk_read_pixels` and a
  transfer-to-host barrier on the buffer. An id is the frame serial (low 24
  bits) and the index of the read. When the slot is reused, after waiting
  for its fence, the values are converted to floats and kept with the
  serial of their frame (`VK_CollectDepthReads`); reads outside the
  framebuffer give 0.
- **Deferred destruction.** Released objects go to a list of the frame that
  may still use them and are destroyed when that frame slot's fence has been
  waited for (or after a mid-frame flush, when the GPU is idle).
- **Textures** (`vk_image.c`). `texnum` encodes a slot of a 4096-entry table
  plus a per-slot generation, so a handle is only reused after 2^20 images
  went through the same slot. Each storage is a `VkImage`, a view (with the
  `A = ONE` swizzle for RGB formats) and a descriptor set holding view and
  sampler. A portability subset device without `imageViewFormatSwizzle`
  (MoltenVK can lack it) gets identity views, and the pipelines of draws
  with an RGB texture have `VK_PIPE_TEX*_RGB` set with every texture env (not
  only `TEXENV_REPLACE`), whose fragment shader then sets the texel's alpha
  to 1: the same 1.0 (`r_vkFallbacks 1`, see Testing). Images are placed in 64 MB blocks of device memory, first fit
  in a sorted list of free ranges per block with merging on release (one
  block per memory type to start; an image larger than 16 MB gets memory of
  its own); a released range goes through the deferred destruction lists
  like the image, and an empty block is freed unless it is the last one of
  its memory type. A map load now makes 10 or 11 device memory allocations
  instead of 300 to 560 (one per texture), 7 to 13 ms less in
  `vkAllocateMemory`, and no driver limit on the number of allocations
  (`maxMemoryAllocationCount`, 4096 on some) is in sight. Samplers are
  cached by filter and wrap mode. When `r_textureMode` changes, mipmapped
  textures get new descriptor sets (sets used by recorded commands can't be
  updated). Uploads go to the upload command buffer, submitted before the
  main one, unless a draw recorded in the current segment of the main
  command buffer uses the texture. Then they are recorded in the main
  command buffer in order with the draws. New textures, level loading and
  cinematics take the first path, so they don't break
  the render pass. Between command buffers every texture is in
  `SHADER_READ_ONLY_OPTIMAL`.
- **Draws.** Each draw copies positions (repacked to 12 bytes), colors (RGBA8
  per vertex, or one float RGBA color read with a binding stride of 0),
  texture coordinates and 32-bit indexes into the frame's vertex buffer, in
  separate bindings. The passes of a multi-pass shader draw the same
  positions: the stage iterators set them with `RB_SetTessVertexArray`,
  which gives the draws a version (`drawGeometry_t::version`), and a draw
  with the same version, positions pointer, stride and vertex count as the
  previous one binds the copy made for it (the indexes too when they are
  the same array and count; the dlight pass has its own). The copy is valid
  until the frame's vertex buffer is reused from its start (new frame,
  mid-frame flush). A constant color, and the texture coordinates of a
  unit that isn't used (the positions, which the shader ignores), are read
  with a binding stride of 0. The portability subset allows no stride of 0:
  a stride must be at least `minVertexInputBindingStrideAlignment` (4 on
  MoltenVK; VUID-VkVertexInputBindingDescription-stride-04456), whatever
  `vertexAttributeAccessBeyondStride` says, so on such a device those
  bindings are read per instance (`VK_VERTEX_INPUT_RATE_INSTANCE`, the
  attribute's size as the stride, so no attribute reaches beyond it either):
  every draw has one instance, the first, so they read the same bytes
  (`r_vkFallbacks 2` forces it; the other strides are 4, 8 and 12 bytes, and
  a larger alignment is reported with a warning). A push constant for the
  color, the first idea, would need 16 bytes more than the 128 that are
  guaranteed and all used, and wouldn't cover the texture coordinates. In the measured views this saves 1 to 4 % of the
  vertex bytes (most surfaces are one multitextured pass). Draws from the
  static geometry (below) copy nothing but, when they need them, their
  indexes and a constant color, skinned draws (Skeletal models) nothing
  (but the indexes of a lower level of detail of the rare surfaces whose
  collapse map the vertex shader can't follow); vertex and index buffers are
  only bound again when they or their offsets change.
- **Static geometry** (`tr_staticgeom.c`, `vk_set_static_geometry`). At the
  end of a map load, `R_BuildStaticGeometry` puts into one device-local
  buffer (through the staging buffer and the upload command buffer, with a
  barrier to vertex input): positions, texture and lightmap coordinates
  and normals (for the effects shader, below) of
  the world's faces, triangle surfaces and grids (every vertex of a grid's
  subdivision), then of each TIKI that static models use (its surfaces in
  the order of `R_AddStaticModelSurfaces`, from `pStaticXyz`,
  `pStaticTexCoords` and `pStaticNormal` like `RB_StaticMesh`); vertex colors (the world's, the
  same with white RGB, the static models' `staticModelData`, so vertex v of
  a model has color `staticColor + v`); and indexes: those of the faces and
  triangle surfaces, grouped by shader in the order a traversal of the BSP
  tree first reaches the surfaces (what the front end adds in a frame is a
  subsequence), then those of the TIKI surfaces. The back end decides at
  the first surface of a batch whether it draws it from there
  (`RB_StaticBatch`): faces, triangle surfaces, grids and static models
  whose shader has no deform but those the effects shader makes, isn't the
  sky, with no dynamic light pass of
  `r_fastdlights 1` (`ProjectDlightTexture`) and no debug drawing, every stage's texture coordinates from `tcGen texture` or
  `lightmap` without `tcMod` or computed by the effects shader, and colors that `ComputeColors` would make a
  constant (evaluated on the CPU for one vertex, `RB_StaticStage`), the
  vertex colors as they are, with alpha 255, or white with the vertex alpha,
  or what the effects shader makes of them
  (`RB_StaticStageColor` follows `ComputeColors` case by case, including
  where it leaves colors of an earlier batch, which stays on the CPU). The
  stage iterators run as before; their arrays (`tess.xyz`,
  `tess.svars.colors`, ...) stand for the streams and `RB_DrawElements` draws
  those (`RB_StaticDraw`). Such a batch counts the vertexes and indexes it
  would have put into `tess`, so it is split at the same surfaces as on the
  CPU; a surface it can't take (one that needs a dynamic light pass, vertex
  alphas below 255 where a stage needs 255, another surface type) turns it
  into a CPU batch: its surfaces are added again (`RB_StaticBatchToCPU`).
  Grids and static models with a lower level of detail compute their
  indexes per frame (into `tess.indexes`); a batch whose surfaces are not
  one range of the index stream copies them there too, and the backend
  copies them once for all passes. Consecutive static models with the same
  one-pass shader whose colors don't depend on the model share one batch
  (`RB_StaticMerge`): one draw per model with its modelview, colors and
  indexes, one stage setup for all, the same triangles in the same order.
  `r_staticGeometry 0` draws everything through `tess` again.
- **Effects shader** (`shaders/effects.vert`, `tr_shade_calc.c`: STATIC
  GEOMETRY EFFECTS, `r_staticEffects`; from JayRewd's b26d8a67, see
  Performance, Effects shader). The static batches whose shader computes
  something per vertex went through `tess`: in the forests most static
  models (`alphaGen distFade` on the trees, `deformVertexes` on leaves,
  flags and curtains), the environment mapped windows and wet walls, the
  scrolling and turbulent water, the static models near a dynamic light
  (`useSpecialLighting`). They are drawn from the static geometry now, and
  `effects.vert` does what `RB_DeformTessGeometry`, `ComputeTexCoords` and
  `ComputeColors` did, with the same floating point operations in the same
  order (`precise`; what the C code computes in double precision exactly or
  as float-float sums and products, divisions and square roots with one
  correction step, like `skinned.vert`): the deforms `wave` (with a
  frequency or without), `move`, `bulge`, `flap s` and `flap t` (not
  `normal`, `autosprite`, `autosprite2`, `lightglow`, which stay on the
  CPU), `tcGen texture`, `lightmap`, `environment`, `environmentmodel` and
  `vector` (not `sunreflection`), the tcMods `turb`, `scroll`,
  `entityTranslate`, `scale`, `stretch`, `transform`, `rotate`, `offset`,
  `parallax`, `wavetrans`, `wavetrant` and `bulge` (which changes nothing;
  not `macro`, which reads the image size), and of the colors `distFade` and
  `oneMinusDistFade` (the alpha of the vertex colors or of the stage's
  constant) and the lighting of a static model by the dynamic lights near
  it (its vertex colors plus N·L with GL1's falloff). What doesn't depend on
  the vertex is computed on the CPU into a record of 128 words in the frame
  data of the skinned draws (`vk_alloc_skin_frame`; `RB_StaticFxRecord`):
  the deforms' parameters (`EvalWaveForm` where the wave is the same for
  every vertex, `shaderTime * frequency` where it isn't), each unit's source
  and up to 4 tcMods turned into adds, scales, 2x3 transforms and
  turbulence (as `RB_CalcScrollTexCoords`, `RB_CalcRotateTexCoords`, ...
  compute their parameters), the view origin and distFade's eye in the
  model's space, the light list (origin in the model's space, `7500 *
  radius` as a float-float, color) and the draw's constant color; the wave
  tables are in the skin data (20 KB, once per map). The draw's
  `firstInstance` is its record, and a draw that would write the same words
  as the last one in the frame takes it again. The pipelines
  (`VK_PIPE_EFFECTS`) read the positions, the texture and the lightmap
  coordinates, the normals (binding 4) and the vertex colors, all per vertex
  (no binding of stride 0, which a portability subset device would read per
  instance, from `firstInstance`: a draw with a constant color has it in its
  record). A batch with effects is one instance (no merging: the record is
  the model's), and is only drawn from the static geometry when the frame
  data has room for the records of all its draws, else on the CPU, so
  nothing is ever left out. Differences from JayRewd's version:
  a stage with a constant color and `distFade` (the pulsing objective
  highlights, `rgbGen wave` with `alphaGen distFade`) got the alpha of
  whatever vertex `tess` held from an earlier batch there, it gets the
  alpha per vertex here; his draws whose record didn't fit in the frame
  data were left out; the bindings of stride 0; the pipelines of the
  effects draws are created at load (see Pipelines at load); not taken from
  that commit: the indirect draws of instances and their `INSTANCED` vertex
  shader, the terrain buffer.
- **Terrain** (`tr_terrain.c`, `r_staticTerrain`, `vk_set_terrain_pool`,
  `vk_sync_terrain_pool`; from JayRewd's b26d8a67, see Performance,
  Terrain). The LOD terrain is tessellated again every frame where it is in
  view (GL1's `R_DoTriSplitting`, `R_DoGeomorphs`, `R_DoTriMerging`, which
  split and merge triangles and move vertexes up and down between their
  levels), its vertexes kept in the slots of a pool (`g_pVert`); a drawn
  patch's every vertex went into `tess` and from there into the frame's
  vertex buffer (`RB_DrawTerrainTris`). Every frame slot of the backend now
  has a copy of the pool's positions and texture coordinates in host
  visible memory (and white colors: terrain has no vertex colors, `tess`
  got 255), and a batch of terrain patches is drawn from it like a batch of
  the static geometry (`tess.staticTerrain`, `drawGeometry_t::staticTerrain`):
  its indexes are the slots, the only thing `RB_DrawTerrainTris` writes
  (counting the vertexes as `tess` would hold them, so the batches split at
  the same patches). Before a frame's first draw from its copy, the
  backend writes into it the slots that changed since that copy was last
  written: the terrain code marks a slot when it is allocated (a split
  fills it) and when a geomorph changes its height (`R_MarkVertChanged`, a
  bit per slot), and `vk_sync_terrain_pool` adds those to what each copy
  hasn't got yet and writes the current one's, in runs of consecutive
  slots. A copy is only written by its own frame, after that frame slot's
  fence: no transfer, no barrier, no frame more in flight. A slot that
  changes after a draw of the frame has read the copy (a second view's
  tessellation; not in the game's views) can't go there: the batches that
  would need it are drawn from `tess` until the next frame. Batches the
  effects shader would draw (deforms, `tcGen` other than `texture` and
  `lightmap`, `tcMod`: the pool has no normals) and those the static
  geometry wouldn't take (the dynamic light pass of `r_fastdlights 1`,
  colors computed per vertex) go through `tess` as before; the dynamic
  lights of `r_fastdlights 0` are added per pixel either way. JayRewd's
  version kept one device local copy, brought up to date through the
  staging buffer, the upload command buffer and two barriers every frame.
  The walk: GL1's `R_DoGeomorphs` and `R_DoTriMerging` went over every patch
  of the map every frame, but nothing happens to a patch that was reset
  already (`byDirty`: its vertexes at their lower level) and not drawn for
  the merging's window (`ter_cautiousframes` + 10 frames). They now go over
  a list of the other patches (`g_pLivePatches`) in the patches' order:
  merging a triangle merges its base neighbour too, which can be in another
  patch, so the order of the walk decides which merges happen (JayRewd's
  list, the patches last drawn first, changed the tessellation: 780 pixels
  of `o3_beach`). Every patch is in it after a load (a patch never drawn is
  reset once), a patch joins when it is drawn (`R_MarkTerrainPatch`, in its
  place) and leaves once it is reset and older than the window; when
  `ter_cautiousframes` changes, every patch is in it again. The same
  tessellation as GL1's walk, checked with a temporary build (see
  Performance, Terrain).
  What the walks leave out (`r_terrainCache`, bits 1, 2 and 4, default 7;
  see Performance, Busy moments): most of their work changes nothing,
  so a few values per patch, kept up to date whatever the bits, say where
  it can't. (1) `R_DoGeomorphs` gives a drawn patch's vertexes a new
  height when their `uiDistRecalc` is no more than `g_uiTerDist`; the
  lowest of them after its last walk (`g_pPatchMorphRecalc`, 0 once a
  vertex is added, and for every patch when `g_uiTerDist` wraps) skips the
  walk while it is above. (2) `R_DoTriMerging` tries every triangle of a
  live patch's merge list; a merge reads the triangle's split count, its
  left child's new vertex's height (in the same patch), its base
  neighbour's split count and both varnodes' flags, which only a split or a
  merge in the patch or in one of its four neighbours (a base neighbour
  across the patch's edge) or a vertex height changed by the geomorph in
  the patch can change. `g_pPatchMergeMode` holds the mode (cautious,
  aggressive) of the patch's last walk, set before the walk and cleared by
  any of those (`R_TerrainPatchChanged` from `R_SplitTri` and
  `R_ForceMerge`, `R_DoGeomorphs`, a patch joining the live list): while it
  equals the mode the patch is in now, every merge in it failed last time
  with what it reads unchanged, and the walk is left out. (3) The indexes
  `RB_DrawTerrainTris` writes for a patch (its drawn triangles' vertex
  slots in the order of its list) are kept per patch
  (`RB_TerrainPatchIndexes`, at most 128 triangles: two roots split down
  to `MAX_TERRAIN_LOD`) until a triangle is allocated, released, demoted
  or merged back into its list, and copied instead of walking the list.
  The same tessellation and indexes (checked with a temporary build over
  21 matches, see Performance, Busy moments).
- **World culling** (`tr_world.c`). GL1's marking of the PVS and walk of
  the world's BSP tree (`R_MarkLeaves`, `R_RecursiveWorldNode`) are
  replaced by a list of the potentially visible leaves that gives the same
  leaves in the same order (see Performance, World culling, for why and
  how it was checked). At map load `R_SetupWorldLeaves` lists the leaves
  the tree reaches in the walk's order (children[0] first; the brush
  models' leaves aren't reached) with their boxes, their cluster and area
  and whether the box lies within the boxes of all their ancestors
  (`worldWalkLeaf_t`). When the view's cluster or the area mask changes,
  `R_MarkLeaves` tests them as GL1 did (`R_GroupVisibleLeaves`) and keeps
  the visible ones in groups of up to 8 consecutive leaves that lie within
  their ancestors' boxes, with the box of their boxes, and the others in a
  group of their own. Per view, `R_AddWorldLeaves` tests a group's box
  against the frustum planes (`R_CullBoxToFrustum`: `BoxOnPlaneSide`'s
  arithmetic inline, the far plane only where GL1 culls with it), then
  each leaf's box against the planes the group isn't wholly in front of,
  or a leaf of its own against the boxes of its path from the root
  (`R_CullLeafAlongPath`), and `R_AddWorldLeaf` runs GL1's leaf body, the
  dynamic lights first culled to the leaf's bounds (`R_CullDlightsToNode`;
  its ancestors' bounds contain them). A face's `msurface_t` holds what
  culling it reads (`isFace`, the plane, `viewCull` and the bounds of its
  points rounded outwards to whole units, in shorts; 64 bytes, the world's
  array aligned to them; `ParseFace`): `R_CullWorldFace` makes GL1's plane
  cull on the copy, then `R_CullFaceToView` leaves out a face whose bounds
  lie wholly behind one of the side planes its leaf isn't wholly in front
  of, by more than a unit (not for faces whose shader deforms vertexes,
  the sky, portals and the sky portal's faces: `viewCull` 0; not in the
  leaves dynamic lights reach, nor with `r_shownormals`:
  `R_FaceViewPlanes`; the sky's faces outside the view were tried on
  2026-10-23 and left in, see Performance, Busy moments). Brush models
  keep GL1's culling. `RE_LoadWorldMap` resets the view cluster (`tr.viewCluster`), as `RE_BeginRegistration`
  does: the client loads a world without a registration when it connects
  to another server from a game, and `R_MarkLeaves`, finding the old map's
  last cluster again at the new map's first view, listed no leaf.
- **Dynamic lights** (`r_fastdlights 0`, the default; `tr_light.c`,
  `vk_add_dlights`, `vk_set_lightmap_dlights`). GL1 relit the lightmap of
  every surface a light reached, texel by texel (`R_RealDlightFace`,
  `R_RealDlightPatch`, `R_RealDlightTerrain`, `R_DlightSample`), into 15
  atlas textures uploaded every frame (`R_UploadDlights`), and drew the lit
  surfaces with them, in batches of their own (the atlas number was in the
  sort key) and with offset lightmap coordinates, so they left the static
  geometry. The fragment shader now adds the lights to the lightmap texel
  per pixel, with the light model of `R_DlightSample`: for each light,
  `color * 375 * (1 - d / radius)^2` truncated to a lightmap byte, times
  the overbright scale (`tr.overbrightMult / 255`), added to the filtered
  lightmap texel, the sum divided by its largest component when that is
  above 1 and truncated to a byte (GL1 did that per texel; see Differences
  from GL1). The front end only decides which lights a surface gets, with
  GL1's tests (the face's plane and `r_dlightBacks`, the bounds of patches
  and terrain) and a test that the light reaches a sample point of its
  lightmap texels (`R_DlightReachesFace`: the parallelogram `lmOrigin + s *
  lmVecs[0] + t * lmVecs[1]` of the texel centers, after a test of its
  bounding box computed at load, `lmBounds`; the bounds of patches and
  terrain), and keeps them as a mask in the surface's `dlightMap`. GL1 gave
  every visible surface every light to test (`R_RecursiveWorldNode` passes
  them all down), which with 16 lights cost most of the frame once the
  texels were gone (in the `obj_team2` yard 41 % in the parallelogram test
  and 11 % in the plane tests); now each node of the BSP tree has the
  bounds of what its subtree can light (`dlightBounds`: the texel boxes of
  its lightmapped faces and the bounds of its lightmapped patches, from
  `R_SetupDlightBounds` at load) and the recursion drops the lights whose
  reach misses them (`R_CullDlightsToNode`, `r_fastdlights 0` only: the
  projected pass reaches farther), which changes no surface's mask. The
  back end ORs the masks of a batch's surfaces (`tess.lightmapDlights`);
  where GL1 bound the dynamic lightmap (a stage's lightmap bundle), the
  stage iterators call `RB_LightmapDlights`, which stores the lights in the
  space of the current entity once per view for the world, once per run of
  batches for a brush model (`vk_add_dlights`), and sets the unit, the list
  and the mask for the draw. The backend keeps a 256 KB host-visible
  storage buffer per frame slot for the lists (8192 lights,
  `vkDlightGPU_t`: origin, 1 / radius, color in lightmap bytes, scale; set
  2 of the shaders, bound once per command buffer); the list's first light
  and the mask are push constants, the unit and the `r_dlightBacks 0` test
  pipeline key fields (specialization constants `DLIGHT_UNIT`,
  `DLIGHT_BACKS`). A scene has up to 64 lights (`MAX_SCENE_DLIGHTS`; GL1's
  `MAX_DLIGHTS` in `tr_types.h` stays 32 for the GL renderers), the
  `r_maxDlights` (default 4) nearest the view of those cgame added
  (`R_KeepNearestDlights` at `RE_RenderScene`, in the order they were
  added; GL1 kept the first 32 added to the frame), and the masks are 64
  bits (`dlightMask_t`). The push constants are full (128 bytes), so the
  shader's mask stays 32 bits: `vk_set_lightmap_dlights` gives a draw whose
  lights lie within 32 of the list's (always with 32 lights or fewer) the
  list from the first of them, and one whose lights span more a list of
  them alone (`VK_DlightSubList`, made once per frame for each list and
  mask, from a copy of the lists of more than 32 lights kept in host
  memory, since the buffer is write-combined), with their number instead
  of a mask (`VK_DLIGHT_COUNT` in the base; the shader adds them in the
  same order, so a draw lit by all 64 gets them all). The vertex shader passes the object space position on;
  pipelines without lights don't read it (RADV removes the unused varying
  when it links the stages). With `r_dlightBacks 0` the shader takes the
  plane of the triangle from the derivatives of the position (their cross
  product, oriented with `gl_FrontFacing`: a BSP face's front is the
  clockwise side) and leaves out the lights behind it, in batches with a
  lit face (GL1 tested each face's plane against the light's center;
  patches and terrain have no such test, and a batch that mixes faces and
  patches tests both). Lit world surfaces keep their batches (their sort
  keys have no dlight bits any more) and their static geometry. Brush
  models (doors, movers) get their lights as the world's surfaces do:
  `R_AddBrushModelSurfaces` passes the lights that reach the model
  (`R_DlightBmodel`) to the faces' and patches' tests, so every light that
  reaches a lightmapped surface is added per pixel. GL1 passed
  `needDlights` (1) as the lights to test, so only light 0 lit their
  lightmaps, the surfaces it didn't reach got `ProjectDlightTexture` from
  every light that reached the model, and the surfaces lit per pixel had
  their own sort key so that the projected pass left them out (with
  `r_fastdlights 1` only light 0 was projected on them). With
  `r_fastdlights 0` no surface gets the projected pass any more (its
  pipelines are only built with `r_fastdlights 1`), and the surfaces of
  brush models without a lightmap get no dynamic light, as the world's
  never did (85 such surfaces in the 66 maps of the game data, none opaque
  in a multiplayer map; no map has triangle surfaces). GL1 also gave
  dynamic lightmaps to lightmapped faces with `SURF_NODLIGHT`, which only
  stops the projected pass; so do the per-pixel lights.
  `r_fastdlights 1` (`ProjectDlightTexture`, a pass per light with
  per-vertex texture coordinates and colors) is unchanged, and so is the
  per-vertex lighting of entities (`RB_Light_Real`, at most
  `MAX_REAL_LIGHTS`, 32, lights an entity, the map's sphere lights first)
  and static models (`useSpecialLighting`: every light within twice its
  radius, up to 64; their list is in `backEnd.staticModelDlights` while
  the model is drawn, not in each `cStaticModelUnpacked_t`, where 64 would
  have taken 1 KB of each), see Performance.
- **Skeletal models** (`tr_skin.c`, `shaders/skinned.vert`, `vk_add_skin_data`,
  `vk_alloc_skin_frame`). `RB_SkelMesh` computed every vertex of a
  skeletal model's surfaces from its bones on the CPU (each weight's offset
  rotated by its bone, plus the bone's position, times the weight; morph
  offsets on the first weight; the normal rotated by the first weight's
  bone), and the vertex-lit iterator lit them per vertex
  (`RB_Light_Real`, `rgbGen lightingSpherical`), every frame. A batch of
  skeletal model surfaces whose shader takes
  `RB_StageIteratorVertexLitTextureUnfogged` (one stage, `rgbGen
  lightingSpherical` or `lightingGrid`, `tcGen texture` without `tcMod`,
  no deform: the characters, the weapons and most other skeletal models)
  is now drawn with `shaders/skinned.vert`, which does both with the same
  floating point operations in the same order (`precise`, so no fused
  multiply-add the C code doesn't have; the build has no FMA and no
  fast-math): positions, normals and colors are the CPU's to the bit, so
  the frames are too. Divisions (`fDot / fDistSquared`) get one correction
  step with the exact residual (`fma`), which makes the GPU's reciprocal
  product correctly rounded but for very rare operands, and the color
  bytes are turned into floats as `c / 255` correctly rounded (one
  correction step, exact for all 256 values), the value an `R8G8B8A8_UNORM`
  vertex color has. The vertexes of each surface go once into the skin
  data, a 32 MB device local buffer (`vk_add_skin_data`, through the
  staging buffer and the upload command buffer): 8-word vertex records
  (normal, texture coordinates, counts, where its morphs and weights
  start), the morphs and weights, the indexes of all the triangles, and
  the bone remap tables of the meshes after a TIKI's first (heads, hands,
  helmets: `TIKI_GetLocalChannel` of each bone's channel, as `RB_SkelMesh`
  maps them), when the TIKI is registered (`R_SkinRegisterModel` from
  `R_RegisterModelInternal`; cgame registers the models of the
  configstrings, the players' and their weapons, as server models), if one
  of its surfaces has a shader that can be skinned on the GPU (`R_SkinPasses`), and
  those registered before the map load at the end of the registration
  (`R_SkinRegisteredModels`): its first draw used to add it, 0.6 ms more
  than GL1 in that frame for a player model; the first draw still adds
  what that missed (a custom shader). Everything is forgotten at each map
  load (`R_ClearSkinData`, after `vk_finish`), which comes before the
  models the map's cgame registers. What changes every
  frame goes into the frame data, a 2 MB host-visible buffer per frame
  slot (`vk_alloc_skin_frame`), once per entity and frame: its bones (the
  `skelBoneCache_t` that `R_AddSkelSurfaces` filled) after the view origin
  in its space, its morph weights,
  and its sphere's lights (`reallightinfo_t`: point, directional, spot),
  or the constant color the other lighting functions give
  (`RB_CalcLightGridColor`, `RB_Light_Fullbright`, `RB_Light_Real` without
  lights), evaluated for one vertex. The skin data and the frame data are
  bindings 1 and 2 of set 2, next to the dynamic lights. A skinned batch
  counts the vertexes and indexes `RB_SkelMesh` would have added, so it is
  split at the same surfaces, and draws each surface in turn (one draw per
  surface instead of one per batch): `vertexOffset` is its first vertex
  record, `firstInstance` its draw record (bones, remap table, morphs,
  lights or color, scale, level of detail), and the indexes are those of
  the skin data. At a lower level of detail (`render_count` below the
  surface's vertex count) `RB_SkelMesh` collapsed each vertex from
  `render_count` on along its collapse chain and kept the triangles up to
  the first degenerate one; the vertex shader follows the chain itself
  (the target is the eighth word of the vertex record), and the draw takes
  the same prefix of the surface's triangles: a triangle that is
  degenerate at a level of detail is at every lower one (the chains only
  go down and stay together once they meet), so each has a threshold,
  computed at upload with a binary search, and the prefix is found with a
  binary search in their running maximum (`R_SkinThresholds`). Surfaces
  whose collapse map has a target that isn't below its vertex (the colt45:
  `RB_SkelMesh` then reads entries it hasn't filled) get their collapsed
  indexes from the CPU as `RB_SkelMesh` computes them, copied into the
  frame's vertex buffer. The pipelines of skinned
  draws have no vertex input (`VK_PIPE_SKINNED`); the fragment stage is
  the same. A batch of the generic iterator is skinned the same way when
  every stage of its shader is one texture with the mesh's texture
  coordinates or `tcGen environment`/`environmentmodel` without `tcMod`,
  colored by `rgbGen lightingSpherical`, `lightingGrid`, `identity`,
  `identityLighting`, `const` or `entity` with `alphaGen identity`, `skip`,
  `entity` or `const`, and the shader has no deform and no polygon offset
  (`R_SkinPasses`; from JayRewd's a5f5a54a, see Performance, Skinned
  passes; his rule left out an alpha of the stage's own with lighting,
  which takes the bullet-hit debris here: `lightingGrid` is one color per
  entity, and so is `lightingSpherical` where the sphere has no light; with
  lights the vertex shader's lighting gives alpha 1, and such a batch stays
  on the CPU): the generic iterator runs as before, and
  for each stage `RB_SkinStage` takes the place of `ComputeTexCoords` and
  `ComputeColors` (the sphere's lights, or the constant `ComputeColors`
  gives one vertex) and its draw is one draw per surface with draw records
  of its own (the GPU reads them once the frame is recorded), whose
  `firstVertex` carries the texture coordinates in its two high bits
  (`VK_SKIN_ENV_*`): the vertex shader computes the environment map's like
  `RB_CalcEnvironmentTexCoords` and `RB_CalcEnvironmentTexCoords2`, with
  `Q_rsqrt`'s operations, from the view origin in front of the bones.
  Stages with `rgbGen lightingSpherical` stay on the CPU where it leaves
  the colors of an earlier batch (`r_drawspherelights 0`, a sphere without
  a lighting function). Still skinned on the CPU: the other shaders
  (`alphaGen distFade`, `tcMod`s, deforms, two textures...), debug drawing (`r_showtris`,
  `r_shownormals`), `lod_tool`, stencil shadows, a TIKI whose mesh maps a
  bone to no channel or reads more morph weights than the entity has, and
  what doesn't fit in the skin or frame data. `r_gpuSkinning 0` skins
  everything on the CPU.
- **Skeletal culling** (`R_SkelOutsideView` in `tr_model.cpp`, `r_skelCull`;
  see Performance, Skeletal culling). GL1 prepared every skeletal model of
  the scene whether it was in view or not (`R_AddSkelSurfaces` let
  `CULL_OUT` through and `R_CullSkelModel` returns `CULL_IN`, "FIXME: not
  working properly"): its bones, morphs and surfaces, and in the back end
  its lighting (sun traces, the sphere's lights) and draws, which the GPU
  then clipped away. Now, once the skeleton is posed (the pose gives the
  radius of the animation frames), a model whose sphere lies wholly behind
  one of the view's four side planes returns before its bones: the sphere
  around the TIKI's origin with the frame's radius times 1.25 plus 32 units
  of the TIKI (`load_scale` times the entity's scale), both taken absolute
  (splash drops have a negative scale), times how much the entity's axes can
  lengthen a vector (the square root of the largest row sum of their
  absolute Gram matrix: 1 for a rotation; muzzle flashes and splash drops
  are drawn with scaled axes). The frame radius alone doesn't hold every
  vertex: the bone controllers turn the torso and the head past it (players
  reach up to 1.2 times it), and a stick grenade's vertexes reach three
  times its radius; the margin comes from a verification build that computed
  every posed vertex of every model the cull would reject (see Performance).
  Not culled: models drawn with a shader that deforms vertexes (any skin, or
  the custom shader), `RF_THIRD_PERSON`, `RF_FIRST_PERSON` and
  `RF_DEPTHHACK` models (the player's own body, the view weapon), mirror and
  portal views, `lod_tool`, `r_nocull`, and `r_shownormals` (it draws the
  normals of batches that don't compute them, what earlier batches left in
  `tess.normal`, which a culled model would change). The farplane's plane
  isn't used: a model beyond it is drawn in the fog color, which shows where
  the sky is behind it (`obj/obj_team4` has both). What a culled model
  leaves out isn't read by anything else: the bone and morph caches
  (`bonestart`, `morphstart`), its level of detail, skin frame data and
  lighting sphere are only used by its surfaces in the back end; cgame poses
  the skeleton itself for tags and shadows (`RE_TIKI_Orientation`), the lens
  flares of `RF_LENSFLARE` entities go through the entity list on their own.
  The one side effect left out is the skeletor's blink: its morph weights
  (`SKEL_GetMorphWeightFrame`) draw a `rand()` for a head whose blink is due
  (every frame once the blink timer, a `short`, has wrapped after 33 s), so
  the client's random sequence (seeded from the clock at every start anyway)
  moves differently.
- **2D batches** (`tr_draw.c`, `r_batch2D`; see Performance, 2D batches).
  The UI and cgame draw the HUD and the menus one call at a time
  (`DrawStretchPic`, `DrawString`, `DrawTilePic`, `DrawTrianglePic`, ...),
  each a surface of its own: a stage iteration and a `vk_draw`. A 2D draw
  whose shader is batchable (`R_2DShaderBatchable`: one stage with one
  texture, no deform, the generic stage iterator, texture coordinates from
  `tcGen texture`, `identity` or `vector` with `tcMod scroll`, `scale`,
  `stretch`, `transform` or `turb`, colors from the 2D color, constants,
  waves or, for pics and triangles, their vertex colors: what
  `ComputeTexCoords` and `ComputeColors` compute for each vertex from that
  vertex alone or from what stays the same while the batch is open) leaves
  its surface open in `tess` (`R_Begin2D`, `R_End2D`), and the next 2D draws
  with the same shader from the same kind of call (pics and triangles, or
  glyphs, which have no vertex colors) add their vertexes and indexes to it:
  the triangle strip of `RB_StreamEnd` from the draw's first vertex, each
  glyph's two triangles. It is drawn as one surface when anything else is
  about to use the renderer (`R_End2DBatch`). The same triangles in the same
  order with the same pipeline and state give the same pixels (blending
  follows the order of the primitives within a draw); single-pass shaders
  only, so the state of each joined draw is what the first one's stage
  iteration sets (`R_State`, `R_Cull` and `R_Bind` change nothing the
  second time). What draws the batch first: every path that runs the back
  end or records GPU work outside these draws goes through
  `R_IssuePendingRenderCommands` or `R_IssueRenderCommands` (`Set2DWindow`
  before every UI widget, the boxes, lines, cinematics and backgrounds, a
  model or skin loaded, the front end's debug drawing, the queued 3D view,
  the end of the frame), and the exports that change what the batch reads
  without them end it themselves: `Draw_SetColor` when the color changes
  (`rgbGen` and `alphaGen` of the 2D color read it when the batch is drawn),
  `Set2DInitialShaderTime` and `RE_SetRenderTime` (the shader time),
  `RE_Scissor`, `RE_RefreshShaderNoMip` (it replaces a shader's image),
  `RE_UploadCinematic` (an image's contents), the load of a font's shader
  (it sets its stage's colors) and `RE_BeginFrame` (the UI draws between
  frames when a menu is pushed). JayRewd's b26d8a67 kept the surface open in
  the same way but ended it from the backend's state setters (`vk_set_*`),
  `RB_BeginSurface` and `RB_ImmBegin`, and joined draws of any shader; here
  nothing in the back end knows about it, and a temporary build checked
  that nothing else ran in the renderer while a batch was open (see
  Performance, 2D batches). Within a widget, what joins in a match are the
  compass's triangles (each quad is two `DrawTrianglePic` calls) and the
  kill feed's lines of one color; every widget sets its own window
  (`Set2DWindow`), whose viewport and projection a batch can't span.
  `DrawBox` leaves out its draw when the 2D color's alpha is 0: blended with
  the source alpha, such a box leaves every pixel as it is, and the UI's
  widgets draw their backgrounds and borders that way, transparent ones
  included (37 of the 40 boxes a frame in matches). Its state changes are
  kept, so the draws after it see the same state; with `r_measureOverdraw`
  (its fragments counted in the stencil buffer) the box is drawn.
- **Crosshair** (cgame's `CG_UpdateCrosshair` and `CG_DrawCrosshair` in
  `cg_drawtools.cpp`, the renderer's `DrawBoxes` in `tr_draw.c`; 2026-10-24,
  Plan 39, as on the `metal` branch, its bccff6f9). `ui_crosshair 1` (the
  default) draws the crosshair as four untextured boxes in one draw, `0`
  hides it (the Options' Crosshair box). The stock path is gone: the image
  (`textures/hud/crosshair`, `cg_crosshair`), the teammate image
  (`cg_crosshair_friend`, which only Spearhead and Breakthrough have) with
  the trace along the view that chose between them in every frame of a
  team game, the two shader lookups of every frame, and the registration
  of both images at the load (`CG_PrecacheMatch`). `cg_crosshair_length`
  (4), `cg_crosshair_gap` (3, from the center) and
  `cg_crosshair_thickness` (1) are HUD pixels as the stock image was drawn
  (16x16: white arms one pixel thick and four long, a gap of three; its
  half-transparent black pixel at each arm's inner end is left out),
  scaled with the height above 1920x1080 (`uiHiResScale`, rounded to whole
  pixels), and `cg_crosshair_color` is `RRGGBB` (a leading `#` too;
  anything else is white). The layout is worked out when cgame takes the
  renderer's configuration (a map load, `vid_restart`), so a change of
  the cvars shows after one of those, and a frame only draws. The arms sit
  around a center square as wide as they are thick, the same whole number
  of pixels from it on every side (the gap less half the thickness, at
  least 1), so the crosshair is symmetric at any size. It shows where the
  stock one did (`STAT_CROSSHAIR`, not with `PMF_NO_HUD` or at the
  intermission; the stock code's zoom test drew nothing without
  `STAT_CROSSHAIR`) and leaves the 2D color white, as the stock image's
  stretch pic did. `DrawBoxes`, new at the end of `refexport_t` (with the
  cgame import `R_DrawBoxes`), draws the boxes as one immediate draw with
  `DrawBox`'s state and pipeline in the color given, converted as
  `Draw_SetColor` converts it (`tr.identityLightByte`), after
  `R_IssuePendingRenderCommands` (so an open 2D batch is drawn first, as
  the stock image's own surface came after it), and gives the current
  color back to the 2D color; the frame log and `renderer_timed.so` time
  it as a 2D call. `REF_API_VERSION` is 15 since: an engine refuses an
  older `renderer_vulkan.so` at startup ("Mismatched REF_API_VERSION",
  then "Couldn't initialize refresh") instead of calling a missing export,
  so comparisons with a build before this one run the old build from its
  own bin dir (a wrapper as the binary of a `cmplists.sh` variant or of
  `perf.py --binaries`), as for engine and cgame changes. Against
  `metal`'s: the color goes through the 2D color's conversion, the API
  version, the tools' wrappers and the load's registration are this
  branch's, the cgame code is the same.
- **Light grid lookups** (`tr_light.c`, `R_GetLightingGridValue`; see
  Performance, Light grid lookups). A lookup blends the 8 light grid
  samples around a point: those of the 4 columns around it, at its row and
  the row above. The grid stores each column run-length encoded (runs of
  one repeated sample and runs of different samples), so every lookup used
  to walk 4 columns from their start. `R_InitLightGridCaches` allocates two
  tables on the hunk once the grid is loaded (zeroed: empty; they go with
  the world): 1024 cells (`lightGridCell_t`, 16 bytes: the cell's index + 1
  as its key, and for each column the palette indexes the walk found at
  the two rows, or one index for both when one run covers them; a Fibonacci
  hash of the key picks the slot, and a cell decoded there replaces the
  one it held), left out on a grid of 2^32 cells or more, whose indexes
  don't fit the key; and 1024 points (`lightGridPoint_t`, 32 bytes: the
  point's 3 floats as bits, and its result), looked up first. The blend
  reads the samples of the same columns and adds them with the same
  weights in the same order as the walk did, so every result is the same to
  the bit (a verification build compared every lookup with the old
  function's: 5.74 million in 4 matches, none different). `floor` became a
  truncation corrected for negative values, the same integer for every
  value in the grid's range. The callers: cgame's `R_GetLightingForSmoke`
  for every debris model (`T_COLLISION` temporary models, every frame) and
  the volumetric smoke's sprites (at `vss_lighting_fps`), nearly all the
  lookups in a match; the entities' lighting spheres
  (`RB_GetEntityGridLighting`, `RB_Sphere_ResetPointColors`), decals and
  static models. Besides the world, a result depends on
  `tr.identityLightByte` alone (no sample lit), which is set once per start
  of the renderer (`R_SetColorMappings` reads latched cvars and the window
  state of its creation; `R_Init` clears `tr`), and every start loads the
  world again. JayRewd's version kept the samples' colors of 1024 cells (32
  bytes each, keyed by 10 bits of each coordinate) and the smoke's lookups
  by origin in `R_GetLightingForSmoke`, both dropped when the
  `lightGridData` pointer changed: `Hunk_Alloc` is `Z_TagMalloc` here, so
  the next map's grid can land at the last one's address and find its
  entries. His point hash
  (`x * 73856093 ^ y * 19349663 ^ z * 83492791`, the low 10 bits) keeps the
  low zero bits of whole coordinates, but debris doesn't rest at whole
  coordinates: its hit rate in matches is that of a good hash.
- **CPU stages and skeletal meshes** (`RB_IterateStagesGeneric`,
  `RB_SkelMesh`; JayRewd's pull request #3 and Plan 32, see Performance,
  Surface list). What the back end did again for each stage or surface
  with the same result:
  - A stage drawn on the CPU (not from the static geometry, not skinned)
    whose texture coordinates are tess's own (every bundle `tcGen texture`
    or `lightmap` without `tcMod`, and no `rgbGen` or `alphaGen` `sCoord`
    or `tCoord`, which read the copy: `RB_StageTexCoordsDirect`) gives
    the draw `tess.texCoords` (stride 16) instead of `ComputeTexCoords`
    copying them into `tess.svars.texcoords`. A stage with the same color
    at every vertex (`rgbGen identity` or `identityLighting`, `alphaGen
    identity`: `RB_StageConstantColor`) is drawn with it as the constant
    color (`ComputeColors`' byte over 255, the float the vertex input
    makes of the array's bytes) instead of an array `ComputeColors` fills
    and the draw copies, and so is the lightmapped multitexture iterator's
    white (GL1's `tess.constantColor255`). A constant color has another
    pipeline key (no `VK_PIPE_COLOR_ARRAY`); `tr_pipelines.c` predicts it
    for those stages in 2D and on the CPU (see Pipelines at load).
  - `RB_SkelMesh` finds the mesh of a surface in a table by TIKI and
    surface (a skel can be a mesh of more than one TIKI; 1024 entries,
    open addressing, filled up to 3/4, then the walk for the rest; 82 and
    112 entries in two matches) instead of walking the TIKI's meshes (an
    engine call each) for every surface drawn; the walk fills it at a
    surface's first draw. It is forgotten with the skin data
    (`R_ClearSkinData`: at each map load and at shutdown, after
    `TIKI_FreeAll` may have freed the models).
- **Push constants** (`vkPushConstants_t`, the guaranteed 128 bytes).
  Vertex stage: projection × modelview, row 2 of the modelview (fog
  distance) and the clip plane in object space (zero without a clip plane),
  pushed again when a matrix or the plane changes. Fragment stage: fog color
  and the two linear fog constants, pushed when they change, and the
  dynamic lights of the draw (first light of its list in the frame's
  dlight buffer and the mask of those it adds, see Dynamic lights), pushed
  when they change. Both ranges are pushed at the start of every command
  buffer. The back end sets the matrices, the viewport, the scissor and
  the fog range for every batch and 2D call, mostly to the values they
  have: `vk_set_projection`, `vk_set_modelview`, `vk_set_viewport`,
  `vk_set_scissor` and `vk_set_fog_range` return then, without marking
  anything to set or push again (see Performance, Redundant state and
  empty flushes).
- **Pipelines** (`vk_pipeline.c`). The key packs blend factors, depth
  test/write/func, polygon mode, cull mode, topology, RGB color mask, alpha
  function (and its reference value), polygon offset on/off, color array or
  constant color, texture env per unit (0 for an unused unit), "RGB texture
  with TEXENV_REPLACE" per unit (with any env without the view swizzle), the stencil function and ops, fog on/off
  clip plane on/off, line stipple on/off, the unit whose texture gets the
  dynamic lights (0 for none), `r_dlightBacks 0`'s test and whether the
  draw is skinned (no vertex input, `shaders/skinned.vert`). Viewport, scissor, depth bias,
  line width and stencil reference/masks are dynamic, and the stipple factor
  and pattern for stippled pipelines (set when such a pipeline is bound and
  they changed). Line pipelines and polygon-line pipelines chain a
  `VkPipelineRasterizationLineStateCreateInfo` with the Bresenham mode. The fragment shader is specialized for
  texture envs, the alpha test, fog, the clip plane discard fallback, the
  alpha written without blending (see State) and the dynamic lights;
  pipelines with a clip plane use the vertex shader built with
  `gl_ClipDistance`.
  Pipelines are cached for the life of the device, with a
  `VkPipelineCache` saved to `vkpipelines.cache` (fs_homepath) when the
  device is destroyed, if it grew, and loaded at startup when its header
  (vendor, device, pipeline cache UUID) matches; a driver that refuses the
  data gets an empty cache. With a cold driver shader cache
  (`MESA_SHADER_CACHE_DISABLE=true`) a pipeline takes about 1 to 1.5 ms to
  create, 18 ms for the 15 pipelines up to the first view of `m6l1a`, and
  0.2 ms with the file; RADV's own disk cache brings it to 0.5 ms without
  the file.
- **Pipelines at load** (`tr_pipelines.c`, `vk_prebuild_pipeline`; see
  Performance, Pipelines and skin data at load). A pipeline used to be
  created by the first draw that needed it, inside that frame: with cold
  caches 1 to 2 ms each, 33 to 49 of them in the first frames after a load
  and during a match. At the end of a registration (`RE_EndRegistration`,
  after cgame has registered the map's models, effects and HUD)
  `R_PrebuildPipelines` goes through every registered shader and works
  out the draws the stage iterators make with it, from its stages as they
  pass them to `R_State`, `R_Cull`, `R_TexEnv` and `R_Bind` (the
  generic iterator stage by stage, the lightmapped multitexture and
  vertex-lit ones, the sky box, the projected dynamic light pass), and
  `vk_prebuild_pipeline` creates their pipelines. It builds the key with
  `vk_draw`'s own `VK_BuildPipelineKey`, which now reads the state from a
  `vkState_t` it is given (`vk_draw` gives it the recorded state), from a
  state with those fields set and a geometry that says only whether it is
  skinned, has colors per vertex and which units have texture coordinates:
  the same key, so the pipeline the draw finds is the one it would have
  created (`VK_CreatePipeline` depends on the key and the device only). What
  only the frame decides, every value it can take: where the shader is
  drawn (2D, where depth test is off for a stage without a second texture
  and the cull is the shader's or the none `Set2DWindow` sets behind
  `R_Cull`'s back, which a draw keeps until `R_Cull` changes it; a 3D
  view on the CPU, with colors per vertex, or the constant color of the
  stages `RB_StageConstantColor` takes and of the lightmapped multitexture
  iterator (Plan 32; in 2D too); the static geometry, with a
  constant color when `RB_StaticStageColor` can give one; the static
  geometry with the effects shader, for a stage it can draw: the shader's
  deforms, a bundle's `tcGen` or `tcMod`, `distFade`, a lit static model's
  `rgbGen`; skinned), the
  fog (off, or the stage's `GLS_FOG_ENABLED`, since `R_State` keeps the
  previous fog when that bit doesn't change and `Set2DWindow` turns it
  off: both, the map's fog isn't known before its first frame), dynamic
  lights on a lightmap unit (and `r_dlightBacks 0`'s test when set), the
  images an animated bundle binds (RGB textures with `TEXENV_REPLACE`, or
  any env without the view swizzle, change the key), the cull left before
  the sky box, and the immediate mode draws of every frame (the color
  clear drawn as a quad, the UI's boxes). A shader registered after that
  (an effect or a sprite announced in the match, in
  `GeneratePermanentShader`) gets its pipelines in that frame, with the fog
  of the last 3D view and none from the static geometry, which only the
  load registers. Not built: mirror and portal views (cull swapped, clip
  plane), debug drawing, stencil shadows, `r_measureOverdraw`, other
  immediate mode draws. The keys created after a load (by a draw, or for
  a shader registered later) are written to `vkpipelines.keys` at
  shutdown when new ones came (keys only: a header with the layout
  version of the `VK_PIPE_*` fields, at most 256, the new ones first),
  and created at the end of every registration of the next runs (those
  this device can make: no stipple without the extension, no polygon line
  mode without `fillModeNonSolid`), so they are compiled at load after a
  driver update too. Bounded: at most `VK_MAX_PREBUILT` (1024) pipelines
  per registration, and at the start of each registration
  (`vk_begin_prebuild`) the pipelines created before their draw that no
  draw used are destroyed (no command buffer refers to them), so what
  exists is what the draws used plus one registration's: 130 to 190 after
  loading the measured maps, of which 30 to 50 are used. On RADV they cost
  next to nothing in memory (pipelines that differ only in fixed-function
  state share their shaders): 178 more pipelines left the game's resident
  memory 50 KB larger and its device memory (amdgpu's fdinfo) the same.
  `vkinfo` prints how many exist, were created before their draw and of
  those were used.
- **Registration at load** (cgame's `CG_PrecacheMatch` in `cg_main.c`,
  `cg_precache`; the engine's `FS_DirMayHave` in `files.cpp`,
  `fs_listDirs`; both renderers; see Performance, Registration in the
  match). A model announced by a configstring during a match
  (`CG_ProcessConfigString`, `RegisterServerModel`) was loaded in that
  frame: its TIKI, its shaders' images from the pk3s, its client init
  commands' caches. In multiplayer (`cgs.gametype` not single player),
  after the gamestate's configstrings, `CG_PrecacheMatch` registers at the
  load, with `R_RegisterModel` (a model of cgame's own, which
  `R_UnregisterServerModel` doesn't free when its configstring changes, so
  one that comes and goes is looked up again instead of loaded), what the
  match can announce later (the server announces the models of its own
  precache list in the gamestate, the original servers of the demos and an
  OpenMoHAA dedicated server alike, so for those this is a lookup by name;
  what it loads is what the server's list lacks):
  - bit 1: every player model of the game data, as the server's
    `InitModelList` and `IsPlayerModel` find them (`models/player/*.tik`
    starting with `allied_`, `american_`, `german_`, `IT_` or `SC_`, not
    the first person ones), each with its first person model (`_fps`: the
    one cgame registers for the player's own view); then the `cache` lines
    of the map's precache script (the server's
    name, `maps/<map>_precache.scr`, `Level::SetMap`) and of the scripts it
    executes (`exec`, without the `::label`; at most 8 deep), whatever
    `if` they are under (a superset: Spearhead's models in an `if` of
    `global/DMprecache.scr` aren't in this game's data and are left out by
    the existence check), registered as cgame's `cache` command does
    (`CacheResource`: sprites as shaders, `.wav` and `.mp3` as sounds) but
    models as above; a map without one gets `global/DMprecache.scr`, the
    stock list of the players' models, their first person models and the
    weapons;
  - bit 2: the `.tik` models of the map's entities (`CM_EntityString`, a
    new cgame import: the exploder's `exploderfire` entities carry the
    explosions of `obj/obj_team2` and `obj/obj_team4`, which get their
    model when they go off) and every `.tik` name in the text of the map's
    own script (`maps/<map>.scr`: `obj/obj_team1`'s flak wreck and its
    explosion), read as text rather than tokens;
  - bit 4: what cgame registers itself the first time it draws it (the
    `blastmark` decal, the team icons, the radar, the lag icon, the four
    head icon sprites; the two crosshair images until Plan 39 drew the
    crosshair as boxes) and the health box
    `Player::EventDMDeathDrop` drops.

  Each model (`CG_PrecacheModel`) gets the server's name for it
  (`models/` in front, `FS_CanonicalFilename`), is skipped when the file
  doesn't exist (the stock list names `german_winter1` and others the data
  hasn't, which would print an error), and brings, one level deep, the
  models its server init commands name (a weapon's projectile, a
  projectile's `explosionmodel`) and, for a `TurretGun`, the view model
  `TurretGun::CreateViewModel` makes from its name (`_viewmodel.tik`). The
  renderer then builds the skin data and the pipelines of all of it at
  `RE_EndRegistration` as for the rest of the load. The images stay
  registered from load to load (`R_FreeUnusedImages` frees those the next
  load doesn't register), so the next map loads only the TIKIs again (when
  no local server keeps them).

  `fs_listDirs` (not on Windows): a lookup in a search directory
  (`FS_FOpenFileReadDir`, the game's directory in the home path and the
  base path, before their pk3s) built the path with `FS_CorrectCase`,
  which, for a file that isn't there, `access()`es every component of the
  path and lists the deepest directory that exists for a name of another
  case, then failed in `Sys_FOpen` (a `stat` and an `fopen`): 17 to 23 µs
  each here, for every file that only a pk3 has, twice for an image (its
  `.jpg` is looked for before the `.tga`). `FS_DirMayHave` keeps a listing
  of each search directory's names (lower case, sorted), valid while the
  directory's device, inode and modification time stay the same (one
  `stat`, 1.2 to 1.5 µs), and answers "not there" when the path's first
  name isn't in it under any case: then neither the path nor anything
  `FS_CorrectCase` would make of it exists, the same answer. A listing of a
  directory changed less than 2 seconds before isn't kept (a change within
  the timestamps' granularity could keep the same time), nor one of more
  than 65536 names; a search directory whose own path doesn't exist as
  named goes the old way (`FS_CorrectCase` may find it under another case).
- **Round changes** (the engine's sound system and client, and cgame;
  both renderers; see Performance, Round changes). A round of an objective
  match ends with a server restart: the server sends 60 to 70 configstring
  changes at once (the soundtrack's set to `none` and back, models, the
  server info, each change twice in the same frame since
  `CG_ExecuteNewServerCommands` runs the commands once for the models and
  once for the rest), and cgame starts the soundtrack again (in the
  recorded matches the client's `CL_ServerRestarted`, which stops the music
  since OpenMoHAA's fix, comes a frame later and frees it again; in a local
  game the restart comes first and the song plays on; unchanged). Three
  parts, each with its own switch:
  - `s_streamThread` (`snd_stream_thread.cpp`, until Plan 31): a song of
    the soundtrack, an mp3 that a stream channel decoded on the main thread
    16384 samples at a time after scanning the whole file to open it, was
    read into memory when the soundtrack loaded and decoded ahead on a
    thread of the sound system. The Omaha mixer that replaced OpenAL (Plan
    31, see Sound system below) decodes every stream on its own worker:
    the soundtrack's files are still read into memory and scanned when it
    loads (`snd_stream_memory.cpp`, under `s_soundtrackCache`), and the
    thread is gone.
  - `s_soundtrackCache` (`MUSIC_SoundtrackText`): the soundtrack file's
    text (`music/<map>.mus`) is kept from its load to the next map load,
    instead of reading it from the pk3 again when the restart sets the
    same soundtrack (40 to 70 µs); since Plan 31 its songs' mp3 files too.
  - `cl_csAppend` (`CL_ConfigstringModified`) and `cg_csAppend`
    (`CG_ConfigStringModified`): a changed configstring went into the
    client's gamestate by copying the whole gamestate (53 KB) aside and
    every string back into it, about 8 µs, and cgame then copied the whole
    gamestate (`GetGameState`), about 1 µs. Now the new text goes after the
    others' (an empty one at offset 0, as before) and the old text stays
    unused until the buffer is full, when the copy runs as before and
    compacts it; cgame copies only the changed string the same way, read
    with a new cgame import, `GetConfigString` (`CL_ConfigString`), and the
    whole gamestate when it doesn't fit. The client changes only that one
    configstring for each `cs` command cgame runs, so both keep the same
    string for every index as before (checked with shadow copies below).
- **Sound buffers at load** (the engine's sound system, both renderers:
  `S_EndRegistration`, `s_loadBuffers`, default 1; see Performance, Busy
  moments). The registration reads every sound's samples into memory, but
  the sound system made its own copy (OpenAL's buffer until Plan 31, the
  Omaha mixer's sound since, decoded to its format) only when a channel
  first played the sound, in the frame of the match that fired the weapon:
  118 to 146 first plays a match, 0.2 to 0.6 ms each with OpenAL. When the
  registration ends, each registered sound's copy is now made from the
  same samples (`S_OMAHA_PreloadSfx`, `S_OMAHA_LoadSfxSound`, the code the
  first play ran; the streamed sounds and the mp3 sound effects, which are
  decoded from their file, still on their first play; since Plan 31 also
  each sound's category and its pain or death lines' 3D load, see Sound
  system); a sound registered later in the match gets it on its first play
  as before. The sounds whose slots the registration frees give theirs
  back (`S_OMAHA_FreeSfx`, after the channels that last played them let
  them go).
- **Sound system** (the engine's client, both renderers; JayRewd's pull
  request #2, merged 2026-10-23 with the changes below, Plan 31; see
  Performance, Sound system). OpenAL is gone: `code/client/omaha/` mixes
  every sound itself and miniaudio (`code/thirdparty/miniaudio`, 0.11.25,
  its decoders, engine and resource manager compiled out) plays the mix
  on the device's own thread, with PulseAudio, ALSA or JACK on Linux,
  WASAPI, DirectSound or WinMM on Windows and Core Audio on macOS, all
  opened at run time (no new link dependency). Layers: the sound host
  (`snd_host.cpp`, `omaha/snd_omaha.cpp`, the old OpenAL host's channel
  policy, music and streams, with the mix rules below) calls the mixer's
  voice interface (`omaha/snd_renderer.h`, `omaha/snd_render_omaha.cpp`):
  the game thread sends commands through a lock-free ring and each
  voice's latest parameters through a mailbox, the device's callback
  applies them, mixes (a windowed-sinc resampler per voice, the speaker
  or headphone panning, filters, a limiter) and hands back what it let go
  of through a return ring; no lock, allocation or print in the callback,
  and no call of the game thread waits for it. What changes what the
  player hears, as the pull request made it: a gain per category of
  sound (`OM_MIX_*`, `s_mix`: footsteps 1.35, the player's own fire 0.6
  and others' 0.65, foley 0.7, impacts 0.6, explosions 0.75, voices 0.9,
  vehicles and the world 0.8), sounds without an entity (cgame's shots'
  impacts and the like) quieter when the same kind already plays from
  the same direction (overlap ducking), others' fire and explosions
  quieter in a burst from one spot (pocket ducking, never below 0.35),
  the music dipped by 2 to 10 dB under voices, world sounds behind
  brushes and across closed area portals muffled (low-pass and gain,
  `s_obstruction`), a high-frequency ducker and a limiter on the master,
  and `s_volume` applied after the limiter. Headphones (`s_speaker_type
  1`) render other players' gunfire, foley, steps and voices through
  their own HRTF filters and everything else through a shared ambisonic
  bus, from the SADIE KU100 set in `main/sound/prom/hrtf` (12.5 MB, Apache
  2.0, read with libmysofa); 5.1 and quad layouts exist too. The pull
  request's tuning (the filters, levels, gains, distances, HRTF and update
  rates) is kept as it came, at the user's word (2026-10-23: it is finely
  tuned): every change below leaves the mixed output the same, and what
  would change it is a proposal (see Remaining). Changed against the pull
  request:
  - Streams (`omaha_channel_two_d_stream`, `snd_stream_async.cpp`): its
    update waited for the decode-ahead worker when a stream was about to
    run dry, which right after every start (one chunk queued) it was, and
    a stream's detach waited for the worker's decode in progress (every
    song start, seek and loop end): 0.1 to 0.16 ms each in the frame. Now
    the update queues what the worker has decoded and never waits (a song
    keeps 0.3 s queued; the PCM the voice plays is the same), a stream is
    released to the worker, which closes it once its read is done
    (`S_AsyncStream_Update`, every frame), a stream that `queue_stream`
    opened goes to the worker in its first update, after its start offset
    was read on without opening the file again, and the worker's 16 jobs
    are allocated once (an attach cost a 128 KB allocation, and a 17th
    stream decodes on the main thread as before). The starts, seeks and
    loop restarts take the pull request's paths (the same chunks queued in
    the same frames). `s_streamAsync 0` decodes all on the main thread.
  - The soundtrack's files (`snd_stream_memory.cpp`, `s_soundtrackCache`,
    replacing Plan 29's `snd_stream_thread.cpp` and `s_streamThread`): a
    song's mp3 is read into memory, scanned and its first 8192 samples
    decoded (the head) when the soundtrack loads, and kept with the
    soundtrack file's text until the next map load. A stream of it opens
    with no file system call or scan (0.7 to 0.9 ms for a 3 minute song),
    the reads of its start, start offset and loop restarts (on the main
    thread, as the pull request does) copy the head, and its own decoder
    decodes the rest on the worker. The same PCM as the file's own decode
    in any read sizes. Plan 29's thread decoded into reads of a fixed size,
    which the pull request's reads didn't match: the merged code lost 30 ms
    (1318 samples) at every restart of a looping song.
  - Sound starts: the category of a sound came from a walk of every
    global alias at its first play (0.2 to 0.9 ms in that frame), and
    every start of a streamed sound that isn't combat speech walked them
    again to find out (0.3 to 0.5 ms each time a player's taunt played).
    One walk of the alias list (at each registration and when the list
    changed) keeps, for each file, its aliases' categories and whether one
    is combat speech (`S_OMAHA_AliasFiles`, the same answers), and every
    registered sound is classified and its pain or death lines loaded for
    3D (`s_pain_hrtf`) when the registration ends (`S_OMAHA_PreloadSfx`,
    the state the pull request reaches at their first play); a sound
    registered later is looked up in the table.
  - The mixer: a device is lost when its callbacks stop for 0.5 s (the
    pull request reopened it on every stop notification, also those of a
    rerouting WASAPI restarts itself, and missed an unplugged device whose
    stop fails, which notifies nothing: silence until `snd_restart`);
    while the callbacks stop, the game thread applies the commands so ids
    keep coming back; a voice's handle takes no command after its stop or
    destroy (an id the mixer frees could be reused while an old command
    for it was still queued); the HRTF set is also read from the client's
    own `main` folder (where the build and the installs put it) when
    `fs_basepath` points at the game data (headphones fell back to stereo
    then); the HRTF data is freed when no device opens; `s_khz 48` mixes
    at 48 kHz (and loads the sounds without downsampling to 22 kHz); the
    capture path for AVI export, which nothing called, and unused HRTF
    functions are removed.
  - Headphones (the user's OK, 2026-10-23; they change the headphone sound
    by the baked filters' difference from the measured ones): they mix at
    48 kHz, the HRTF set's own rate, whatever `s_khz` says. At 44.1 kHz,
    the default, the set resampled made the pull request's baked filters
    (a 72 MB table of directions every 1.5°, built at the sound's start)
    fail its own check against the filters designed from the measurements
    (median -27.4 dB, max -8.0 dB from them, the limits -20 and -10), so
    every voice used designed ones, made on the mixer thread: a new
    voice's first in the callback it started in, about 20 µs each, and up
    to 4 more per callback for voices that turned more than 1°. At 48 kHz
    the check passes (median -30.6 dB, max -15.6 dB) and the baked ones
    are used: a lookup per voice and turn, and the sound start takes 0.61
    s instead of 0.88. Where designed filters are still used (a set whose
    baked filters fail the check, `s_hrtf_live_query 1`, or no memory for
    the table) a thread of their own makes them (`Hrtf_DesignThread`,
    woken by a semaphore the mixer posts; scratch of its own): a voice
    asks for one when it starts or has turned more than 1° and plays the
    baked filter (kept for that; none without a table) until the thread's
    is made, then crossfades to it over a block like every change of
    filter; one request per voice in flight, the reply for a voice that
    was freed and taken again in between dropped. The mixer no longer
    designs any.
  - The obstruction traces are the pull request's. Plan 31 had added
    `s_obstructionBudget` (off: at most N sounds traced again in a frame,
    an answer held while both ends moved 8 units or less, a new sound
    taking the answer of one from the same spot), timed with matches
    played back faster than real time; at real speed the traces cost 1.5
    to 1.7 µs a frame (see Performance, Sound system), and it was removed
    (2026-10-23, the user).
  - The soundtrack off and taunts (2026-10-24, the user's, as on the
    `metal` branch): `s_ambientvolume 0` plays no song of the soundtrack
    (the mood music and the `amb_stereo` ambiences): none is opened,
    decoded or mixed. `S_OMAHA_MUSIC_AmbientOff` stops the songs playing,
    fading or paused (their streams and voices freed) and drops the files
    read into memory at the load, a load reads none while it is 0, and
    `S_OMAHA_MUSIC_PlaySong` starts none; raised again, the mood's song
    starts where it stopped. Before, `0` only silenced the songs, which
    were still read, decoded and mixed. The music scripts trigger
    (`s_musicvolume`) isn't the soundtrack and plays on. `cg_tauntsounds`
    (cgame, archived, default 1): at 0 another player's taunt
    (`CGM_VOICE_CHAT`, `CGM6_VOICE_CHAT` on protocol 6 to 8 servers) isn't
    started, so its file isn't opened either; its chat line and radar icon
    stay, one's own taunts still play. Checked with a 60 s test song in
    `dm/mohdm4` (`strace`: the file opened only while the volume is above
    0, closed when it went to 0) and with a cgame that printed each taunt's
    decision (a match demo: another player's taunt skipped at 0, played at
    1; a local match: one's own taunts played at 0).
  - Fixes: `playmp3` crashed (GCC 15 devirtualized `chan->stop()` of an
    `omaha_channel *` pointing at the stream member `omaha.chan_mp3` to no
    target and dropped `S_OMAHA_LoadMP3`'s body, a recursion until the
    stack overflowed; the parameter is the stream type now), and the mp3
    codec's whole-file decode (`S_MP3_CodecLoad`) returned zone memory that
    every caller freed as temporary hunk memory (the other codecs' kind;
    the old OpenAL host did the same).
  - Build: the Windows package and installer carry the HRTF set and the
    licenses' notice (`OMAHA-SOUND-LICENSES.txt`; the pull request's
    package step copied only DLLs and executables), a macOS `.app` gets no
    data in `Contents/MacOS` (code signing), libmysofa's unused
    `minphase.c` is left out, the SADIE copyright line is in its README.
- **Counters** (`vkinfo`, `vkStats_t`; `staticDraws` counts the draws from
  the static geometry, `effectsDraws` those of them with the effects
  shader). Every `vkAllocateMemory` goes
  through `VK_AllocateMemory`, which counts it; GPU times come from
  timestamps at the start and end of the main command buffer and around the
  copy to the swapchain image (`TOP_OF_PIPE`/`BOTTOM_OF_PIPE`), read without
  waiting when the frame slot is reused; times on the CPU use
  `SDL_GetPerformanceCounter`.
- **Frame log** (`vk_framelog.c`, `r_vkFrameLog`). When the cvar is set as
  the renderer starts, `GetRefAPI` returns wrappers of the exports that run
  every frame (the ones `tools/rtimer/rtimer.c` wraps, timed the same way)
  instead of the exports; otherwise it returns them unchanged, and the only
  trace of the log is one test of `vk_frameLog` per frame in
  `VK_CollectTimestamps`. Each frame's record is filled at `EndFrame` from
  the difference of `vkStats_t` since its `BeginFrame` (so the fence wait
  at the start of the frame, in the first 2D call, and the acquire after
  the present are in it; what the backend does between frames, a map load
  with the pipelines created at its end, is no frame's, like the
  registration time between frames, which `load` leaves out too; until
  2026-10-18 it went to the first frame after it) and completed at the next `BeginFrame` (its
  interval, the engine's time after it); the GPU timestamps come two frames
  later, when the slot is reused (`VK_FrameLogGpu`, matched by frame
  serial). Nothing of the frames, submissions, presents or acquires
  changes; the wrappers cost about 15 µs per frame (two clock reads around
  each of the ~300 2D calls of the HUD) while the log is on.
- **Shutdown and `vid_restart`.** `vk_shutdown` stops the submit thread
  (every frame handed over submitted first), waits for the device,
  releases the held image and retires the swapchain, destroys the retired
  swapchains and their semaphores once their presents are done (present
  fences, see Presentation semaphores and swapchains), then the present
  fences and everything else, including the instance; `vid_restart`
  recreates the window, so the backend starts from scratch.
- **Temporal path (Plan 41).** `r_temporal 1` (`vk_temporal.c`,
  `tr_temporal.c`), what temporal anti-aliasing and screen space ambient
  occlusion need from a frame; the anti-aliasing reads it (Plan 42, below).
  - *Images per reader* (Plan 45, `VK_TemporalUpdateFeatures`). Made with
    the path: the scene's color (one image), motion vectors and normals,
    and a 1 x 1 placeholder. The rest in groups (`VK_TEMPORAL_GROUP_*`,
    `vkt.groups`), made while a reader needs them: the anti-aliasing's
    history (`taa[2]`, `r_taa`), the depth pyramid (`hiz[2]`: `r_taa`, and
    `r_temporalShow` 1, 2, 4, 6, 7), the ambient occlusion's (`r_ao`) and
    the last scene's color (`color[1]`, `r_temporalShow` 5: with it the
    two color images alternate as before, the framebuffers made again),
    the motion blur's (`r_motionBlur`, `r_temporalShow` 11 and 12: 30 MB at
    2560x1440), the depth of field's (`r_dof`, `r_temporalShow` 13: 52 MB
    at 2560x1440).
    At 2560x1440: the color 30 MB, the motion vectors and the normals 15
    each, the history 59, the pyramid 20, the ambient occlusion 102, the
    last color 30. At the start of a frame's scene (`vk_begin_scene`,
    before anything of it is recorded) the cvars are latched for the
    scene (`vkt.taaOn`, `vkt.aoOn`, `vkt.showMode`: one changed in the
    middle of a frame can't reach an image not made) and the groups made
    or freed to match: then the submit thread is drained and the GPU
    waited for (`vk_finish`), the new images cleared in the frame, and the
    static sets written again, the placeholder (`GENERAL`) in the slots of
    the groups not made (the composite reads every binding in one view or
    another). Checked: the views of `mohdm6` the build before's to the bit
    with the path alone, with the anti-aliasing, with the ambient
    occlusion, with both, and in `r_temporalShow` 4, 5 and 9; toggled in
    a run (the anti-aliasing, then the ambient occlusion, each off again,
    the views 5, 4 and 9), the GPU's memory followed (about 2830 MiB used
    with nothing, +60 to 80 with the anti-aliasing, +95 with the ambient
    occlusion, back to 2800 at the end).
  - *Scene.* The back end starts every 3D view of the world with
    `vk_begin_scene` (`RB_TemporalBeginView`); the first of a frame begins
    the scene: its render passes have four attachments: the color
    (`R16G16B16A16_SFLOAT`: room above 1 for HDR to come; the frame's 8-bit
    format, tried in Plan 42's optimization, gave the frames without the
    path to the bit, where RGBA16F differs by one in up to a tenth of the
    pixels, and cost 0.03 ms less at 2560x1440; two images that alternate:
    this frame's and the last frame's), the motion vectors (`R16G16_SFLOAT`), the normals
    (`A2B10G10R10_UNORM`) and the usual depth image, then created with
    `SAMPLED` usage too. The scene's first render pass clears the color and
    the normals with its load operations and doesn't load the motion
    vectors (`vkt.renderPassClear`, from the undefined layout: no barrier or
    clear command), the others keep them (`vkt.renderPass`); the two differ
    in load operations and initial layouts only, so they are compatible.
    The scene's images are never storage images (which can cost them their
    compression). Its draws use pipelines of their own (`VK_PIPE_TEMPORAL`:
    that render pass, a pipeline layout with 240 bytes of push constants
    (`vkTemporalPush_t` after `vkPushConstants_t`) and a set 2 that adds the
    last frame's frame data, the shaders built with `TEMPORAL`);
    `vk_prebuild_pipeline` builds those. The scene ends (`vk_end_scene`) at
    `Set2DWindow`, at a view without the world, at a color readback or at
    the end of the frame. One scene a frame: a world view after it is drawn
    as without the path. Portals, mirrors and the sky portal are views of
    the scene.
  - *What is written.* The pipelines that write depth without blending
    (`VK_TemporalWritesBuffers`) write the normals; of them those that draw
    what moves (`VK_PIPE_MOTION`: a skeletal model, or an entity with a last
    frame, `vkt.objectMotion`) write the motion vectors too
    (`independentBlend`). The others mask them, so a translucent surface
    leaves those of what is behind it, and skip their work: specialization
    constants 11 (`TEMPORAL_NORMALS`) and 12 (`TEMPORAL_MOTION`) of the
    temporal shaders leave out the normals and the clip positions, and the
    second skinning with the last bones. Only the pipelines that write
    normals read them per vertex (`VK_PIPE_NORMALS`; the others read (0, 0,
    0), `vkt.zeroBuffer`), so a batch's normals are copied for the pass
    that writes them only. A normal is in view space, octahedral (rg),
    facing the eye; b has the flags (`TEMPORAL_FLAG_NORMAL`: a draw gave
    the normal; `TEMPORAL_FLAG_MOTION`: the motion vector is a draw's), the
    2 alpha bits the class: 0 nothing that hides what is behind it, 1
    surface, 2 the view weapon (its depth hack range, which depth alone
    can't tell apart without `r_depthClamp`), 3 sky (depth range [1, 1];
    the sky shaders write no depth, so the sky is what no draw wrote at
    depth 1). Motion vectors are this frame's position minus the last
    frame's, unjittered, in units of the viewport (x right, y down).
  - *What the readers work out* (`shaders/temporal_common.glsl`, with the
    primary view `vkt.view`, `TemporalView`): without
    `TEMPORAL_FLAG_MOTION` the camera's motion from the view distance
    (`TemporalCameraMotion`, the sky as a direction), without
    `TEMPORAL_FLAG_NORMAL` the normal from the view distances around the
    pixel (`TemporalDerivedNormal`: of the neighbours on each axis the
    nearer one in depth, so a normal at an edge belongs to its own
    surface), and the view distances from the depth buffer
    (`TemporalViewDistance`, with `vk_depth_floor` undone). Until the first
    version's review they were written by the scene's draws and a pass over
    every pixel: their writes and that pass cost most of the path.
  - *Motion.* The primary view (not a portal or the sky portal) keeps its
    projection and view matrix; the next frame's has a last frame unless
    the camera moved more than `r_temporalCutDist` or the field of view
    changed (`R_TemporalPrimary`). A draw's last position is its vertexes
    through the last projection and the last modelview: for entities of
    type `RT_MODEL` with a game entity number their last frame's model
    matrix (`vk_set_prev_modelview`, found by entity number, model and the
    order among the scene's entities with both; not after a jump of more
    than `r_temporalCutDist`), else the camera's motion times the modelview
    (a surface that doesn't move). Skeletal models skinned on the GPU skin
    the vertex again with the last frame's bones and morph weights: the
    frame data of the skinned draws is a ring of three buffers with the path
    on (the two frame slots' and one more; two frames in flight: the last
    frame's must stay while the next is written), and an entity's data in
    the scene starts with the words of `VK_SKIN_PREV_WORDS` (where its bones
    and morph weights are in the last frame's buffer; `skinPrev` makes the
    data again when it was made without them, which the temporal shaders
    would read out of bounds). Not moved: CPU deforms, the effects
    shader's deforms, skeletal models skinned on the CPU, the terrain's
    geomorphs. Portal views have no motion of their own.
  - *After the scene* (`VK_TemporalResolve`): the depth pyramid
    (`R32G32_SFLOAT`, min and max view distance, level 0 half the
    framebuffer's size; the weapon and the sky at the far distance: they
    hide nothing; a texel of an odd size's last row or column covers three
    of the level before), when a reader needs it (`VK_TEMPORAL_READ_DEPTH`
    in `vkt.readers`, or `r_temporalShow`'s views; `vkt.hizSerial` says
    which frame made a pyramid, or its level 0; the anti-aliasing reads
    only the last frame's level 0 and makes this frame's itself, see
    below): `hiz_build.comp` with `PREPARE` makes
    levels 0 and 1 from the depth buffer (an invocation per texel of level
    0, 2 x 2 pixels in one gather, 8 x 8 a workgroup, which makes 4 x 4 of
    level 1 through shared memory; a texel of level 1 that reaches into the
    next workgroup's texels makes those again itself), then two levels per
    dispatch the same way, then what is left of a level of 128 or less in
    one workgroup with barriers between the levels (`TAIL`): 3 or 4
    dispatches (one invocation per texel of level 1, looping over 4 x 4
    pixels, took twice the time: latency; a tail from 256 was slower too:
    one workgroup). The last frame's pyramid (level 0: the last frame's
    depth) is kept. The descriptor sets of the pyramid and the composite are
    written once per image pair; only the prepare pass's (the depth image's
    view can change) is written in each frame, with one barrier call per
    pass. `VK_TemporalConsumers` is where TAA (below) and AO go; then the scene is
    drawn into the frame's color target over the rectangle of its views
    (`temporal_composite.frag`, or what `r_temporalShow` shows), where the
    2D drawing follows.
  - *Jitter* (`r_temporalJitter 1`): the perspective projections of the
    scene's draws get a Halton (2, 3) offset of up to half a pixel, per
    frame (`vkt.jitterPixels` in pixels); the vertex shaders take it out of
    the motion vectors' current position.
  - *Primary view.* The first view of the scene that is neither a portal
    nor the sky portal gives `vkt.view` (a later one, if any, doesn't
    replace it).
  - *Cost* (NVIDIA, GPU time a frame, measured by leaving parts out), in
    the user's `dm/mohdm6` view at 2560x1440 (0.043 ms without the path):
    0.121 ms in all; the composite 0.035 ms, the RGBA16F color 0.03 ms
    against RGBA8 (B10G11R11 would halve it, with 5 or 6 bits of mantissa:
    banding in the 8-bit content), the normals 0.03 ms (their writes, not
    the shaders' work: with constant outputs the time was the same), the
    pyramid 0.04 ms when it is made (the prepare pass 0.027, about reading
    the depth buffer; the depth image's layout changes cost nothing
    measurable). Not measured: AMD, where the sampled depth image may be
    slower to draw into, as the readable one was (see Render target).
  - *Off* with multisampling, or when the device has less than 240 bytes of
    push constants, no `shaderStorageImageExtendedFormats` or
    `independentBlend`, or can't use the formats; the console says why.
    `vk_shaders.c` is unchanged: the temporal shaders are in
    `vk_shaders_temporal.c`.
- **Temporal anti-aliasing (Plan 42).** `r_taa 1` with the temporal path
  (`VK_TemporalConsumers`, `shaders/taa_resolve.comp`).
  - *When.* `vk_begin_scene` latches `r_taa` for the frame's scene
    (`vkt.taaOn`) and jitters it then (`vkt.jitter`, also without
    `r_temporalJitter`). The resolve needs the depth image and the last
    frame's level 0 of the pyramid (the last depth): when no other reader
    needs the pyramid (`VK_TemporalReaders`) its pass makes this frame's
    level 0 itself and the pyramid's passes are left out
    (`VK_TemporalResolve`), unless the views don't cover the framebuffer
    (its dispatch covers the views only).
  - *History.* Its own images, `vkt.taa[2]` (RGBA16F storage images,
    `vkt.taaFormat`; `GENERAL` between frames), alternating with `vkt.current` like the
    scene's color: the scene's last color is not the last result. The
    result of the frame before is the history when `vkt.taaSerial` says
    so and the view has a last frame (`vkt.view.params[2]`: no cut, no
    teleport, no field of view change); else, as after a load, a
    `vid_restart` or `r_taa` turned on again, the frame is the scene as
    it is.
  - *Resolve.* One dispatch over the scene's views (`vkt.rect`), 16 x 16
    pixels a workgroup, which loads its pixels and the ring around them
    (18 x 18: YCoCg color, view distance with the weapon's sign, one `vec4`)
    into shared memory first (each pixel reading its 3 x 3 itself took a
    quarter longer, 8 x 8 workgroups 5 %); 8 x 8 of its invocations then
    make level 0 of this frame's pyramid from those distances when no pass
    makes the pyramid (as `hiz_build.comp`'s `PREPARE`: the same frames to
    the bit, also at 1001x601, where a texel of the last row and column
    covers three pixels). Per pixel: of the 3 x 3 the nearest
    surface gives the motion (its motion vector when it moves, else the
    camera's, `TemporalCameraMotion`), so the edges of what moves carry its
    motion; outside the views last frame: no history. This frame's color
    is the 3 x 3 weighted by each sample's distance to the pixel's center
    (`exp(-2.29 d^2)`, close to Blackman-Harris; the samples are at their
    pixel's center minus `vkt.jitterPixels`; the 9 weights are the same for
    every pixel: `VK_TemporalConsumers` makes them, normalized, for the push
    constants): the pixel itself, as at first,
    moved with the jitter and shimmered through the blend (the opposite
    sign shimmers more). A surface that doesn't move and whose last frame's
    distance (`reproject`, the clip position the camera's motion was worked
    out with) is outside the min and max of the last
    pyramid's level 0 around it (its 2 x 2 texels nearest to the point, two
    gathers: 4 x 4 pixels; 3 % and 1 unit of tolerance) was hidden there:
    no history. With only the texel under the point a thin surface (a
    ladder's rung) was in it or not as the last frame's jitter had it and
    lost its history every few frames (the weapon and the sky are
    at the far distance in the pyramid, so what the weapon covered takes
    none either). The history is read with Catmull-Rom (5 bilinear taps,
    `vkt.linearSampler`), clipped in YCoCg toward the box of mean ± the
    deviation of the 3 x 3 (within their min and max), and blended with
    weights divided by 1 + luma (no flicker from bright pixels that come
    and go); the history's weight `r_taaFeedback` less up to a quarter for
    pixels that move 16 pixels or more a frame (resampling blurs). The
    descriptor set is written in each frame (`vkt.taaSets`: the depth
    image's view can change), the composite's are static
    (`vkt.compositeTaaSets` show `taa[current]`).
  - *Sharpening* (`r_taaSharpen`) in `temporal_composite.frag`: the pixel
    away from the mean of its 4 neighbours, clamped to their min and max (no
    halos); the result stays unsharpened as the next frame's history.
  - *Barriers.* `taa[current]` from undefined to `GENERAL` in the call that
    gives the scene's images to their readers (it was last read by the
    composite two frames before and the resolve one frame before), the
    dispatch's writes to the composite and the next frame's resolve in the
    call that gives the depth image back to the draws: two calls in all.
  - *Cost* (NVIDIA RTX 4070 Ti, GPU time a frame, the `dm/mohdm6` view of
    the temporal path, measured by leaving parts out): at 2560x1440 0.121
    ms without it, 0.36 ms with it: the pyramid 0.04, the tile load and the
    write 0.085, Catmull-Rom 0.03 against one bilinear tap, the depth test
    0.024, the rest (motion, clip, blend) about 0.07; at 1920x1080 0.069 to
    0.208 ms. The centered color and the wider depth test then 0.03 ms
    more: 0.39 and 0.225 ms. The optimization the same day (the user's
    request: the pass is limited by its bytes and its arithmetic, each
    change measured on its own): the weights from the CPU (9 `exp` a pixel
    before) and one reprojection for the motion and the depth test 0.027,
    level 0 made by the pass instead of the pyramid's 3 dispatches and
    barriers 0.015: 0.388 to 0.344 ms, at 1920x1080 0.227 to 0.185. The
    scene in the frame's 8-bit format (0.03) and the history in
    `A2B10G10R10` (0.025) were in for a commit (0.286 ms) and taken out
    again for HDR to come (the user's choice): they clip at 1 and added
    a little shimmer (below). Then, with them in, at 2560x1440: the fixed part
    (dispatch, barriers, the depth image's layouts, the composite's
    sharpening, 0.006 of it) 0.025, the tile load, level 0 and the write
    0.054, the arithmetic 0.113 (Catmull-Rom 0.019 against one bilinear
    tap, the clip box 0.025, the depth test 0.014, the luma weights
    0.004). Tried and left: one `vec4` read of the tile instead of two
    (nothing, kept: simpler), the weapon from the depth alone in the tile
    (nothing: the flags are read again at the nearest pixel), a 1-byte
    buffer of the flags and the class instead of the normals (0.004 ms:
    the scene's writes cost by the render target, not by its bytes; 0.025
    without that target, which would need the motion flag elsewhere), a
    10-bit scene (`A2B10G10R10`; the alpha isn't read: destination alpha
    blends are ONE and ZERO, `vk_pipeline.c`; no added shimmer, but 0.05 ms
    instead of 0.08 saved: 0.336 ms).
  - *Shimmer* (8 frames of a still `dm/mohdm6` view at 640x480, the share
    of pixels whose luma changes by more than 4 of 255 between them; more
    than 16 in brackets): no jitter 0 %, the jitter alone 25.6 (2.95),
    the first version 1.16 (0.25) and with `r_taaFeedback 0.5` and
    `r_taaSharpen 0.8` 12.2 (1.13); the centered color 0.73 (0.26), the
    wider depth test 0.39 (0.06), with 0.5 and 0.8 2.29 (0.31). A wider
    clip box (1.25 and 1.5 deviations) changed nothing; without the depth
    test 0.21 (0). At the defaults (`r_taaFeedback 0.75`, `r_taaSharpen
    0.8`): 1.25 (0.16), with the 8-bit scene 1.30 (0.16), with the 10-bit
    history too 1.44 (0.16); a 10-bit scene with the RGBA16F history 1.23
    (0.16). Those builds compared with the jitter on one position
    (`r_temporalJitterSamples 1`: otherwise the frame count since the
    start, which the loading time changes, picks it, and two runs of the
    same build differ by one in a fifth of the pixels): the formats change
    a third to half of the pixels by one (a few hundred by 4 to 15, a few
    by up to 63), no shift (under 0.04 of a level on average, 0.1 to 0.27
    on average apart); the rest of the optimization changes 0.003 to 0.1 %
    of the pixels, by one but for a handful (the CPU's `expf`).
- **Ambient occlusion (Plan 44).** `r_ao 1` with the temporal path
  (`VK_TemporalAo`, `shaders/ao_common.glsl`, `ao_prefilter.comp`,
  `gtao_main.comp`, `ao_temporal.comp`, `ao_denoise.comp`,
  `ao_apply.frag`).
  - *Where.* `RB_RenderDrawSurfList` calls `RB_TemporalAo` in the primary
    view before the first batch whose shader sorts at `SS_UNDERWATER` or
    later (after the opaque surfaces, decals, see-through and banners),
    or after the list when it has none; once a scene (`vkt.aoRan`).
    `vk_ao_scene` ends the scene's render pass (a scene that hasn't begun
    one clears its images first), records the passes, begins
    `vkt.renderPass` again (nothing cleared) and draws the apply pass
    first; the draws that follow bind everything again. Portals and the
    sky portal are drawn before the primary view, sprites
    (`RB_SpriteSurfs`) after it: neither is occluded. With every pass left
    out the break cost 0.002 ms.
  - *Images* (`GENERAL` always, made while `r_ao` is on, Temporal path,
    Images per reader: 102 MB at 2560x1440): `aoDepth[2]` view distances
    (R32F, 5 levels, the weapon's negative, the sky at `AoFar`; this
    frame's and the last one's), `aoRaw` (R8: one frame's visibility), `aoHistory[2]` (RG16F: the blended
    visibility, its frames), `aoBlur` (RG16F), `aoResult` (RGBA8: what the
    apply pass blends). One descriptor set and push constant block
    (`vkAoPush_t`, 208 bytes) for the compute passes, written each frame.
  - *Barriers.* One call before: the depth image to
    `DEPTH_STENCIL_READ_ONLY`, the normals and the motion vectors to
    `SHADER_READ_ONLY`, the shader writes before visible; one between the
    passes; one after: those back to attachments and the result to the
    apply pass.
  - *View distances* (`ao_prefilter.comp`): 8 x 8 invocations, 2 x 2
    pixels each from a gather of the depth and one of the normals' class;
    levels 1 to 4 through shared memory with XeGTAO's weights (the nearer
    values lose weight past the radius, so a coarse level keeps the
    surface behind a thin one), of 1 / d: linear across a plane in screen
    space, so a plane's texel is the plane at the texel's center (the
    mean of d bent a wall close to the eye toward it); the weapon is
    averaged only where a texel has nothing else.
  - *Occlusion* (`gtao_main.comp`): XeGTAO's main pass, a pipeline per
    `r_aoQuality` with the slices and steps as specialization constants
    (`VK_TemporalAoMainPipeline`: the loops unroll and a slice's depth
    fetches go out together; from the push constants the pass took 0.06
    ms longer), the view positions as (pixel * mul + add) * d worked out
    once a pixel. The normal from the depth (as `TemporalDerivedNormal`,
    the neighbours in 2 gathers);
    noise from the pixel's index on a 64 x 64 Hilbert curve and the R2
    sequence, shifted with the frame (not with `r_aoTemporal 1`); per
    slice the samples squared toward the pixel, the level by their
    distance (log2 of the pixels - 3.3; a coarse level's sample at its
    texel's center), those off the view left out (clamped to its edge
    they left the slice, and a point of the same wall beside it can be
    above the slice's horizon: close to a wall the radius is thousands of
    pixels and it went dark), the weapon and the world not
    occluding each other, faded over the last `r_aoFalloff` of the radius
    with the thin occluder compensation; the visible arcs on both sides
    integrated against the normal projected into the slice. Less where
    the radius covers under 10 pixels.
  - *Over frames and blur* (`ao_temporal.comp`, one pass of 16 x 16
    pixels a workgroup: the raw occlusion and the view distances of its
    pixels and 2 around them into shared memory, the blend of its pixels
    and 1 around them (the ring blended twice, in the neighbouring
    workgroups too), then the blur from there: the same values as a pass
    each, 0.1 ms less than the two passes). The blend: the motion of the
    pixel (the draw's motion vector where it moves, a weapon without one
    none, else the camera's); the last result read bilinear among the 2 x
    2 texels whose last view distance is the surface's (3 %, 10 % for
    what moves, and 1 unit), the texels picked by the shader and fetched
    one by one (a gather's are picked from the coordinate with 8 bits
    below the texel: a point a thousandth of a pixel before a texel center,
    as the camera of a player pushing into a wall puts it, came back as
    the texels one further, under weights worked out for these, and the
    history moved a pixel a frame: the noise streaked in the direction of
    that motion), clamped to this frame's 3 x 3 widened by 0.05; blended
    by 1 / frames up to `r_aoTemporal` (a quarter for what moves); written
    out before the blur (it doesn't pile up). No history after a cut, a
    field of view change, a load or `r_ao` turned on. The blur (`AoBlur`,
    `ao_common.glsl`): XeGTAO's 3 x 3 between edges both ways (`AoEdges`,
    from the view distances of the pixel's 3 x 3 and the 4 two away,
    unquantized: XeGTAO's 2 bits an edge flipped where an edge was near a
    step whenever the depth was rounded anew, every frame with the
    anti-aliasing's jitter and in the camera's sub-unit motion of a player
    pushing into a wall, and the occlusion wobbled in bands), the center
    weighted 1.2 (0.24 for the first of two) times 1 + a quarter of the
    frames blended past the first. Two blurs at quality 3 and without
    history: the first into `aoBlur`, the second `ao_denoise.comp`'s.
    The last makes the result (`AoResult`): the visibility to
    `r_aoStrength`, the multi-bounce fit, to the scene's encoding (1 /
    2.2: the game draws gamma-encoded values), and the fog's part: the
    scene is C f + fog (1 - f) (the draws' linear fog of the view
    distance, `RB_SetupFog`'s range and color), so rgb = fog (1 - f)(1 -
    a), blended `ONE`, `SRC_ALPHA`: dst a + rgb. The apply pass discards
    pixels whose factor rounds to 1.
  - *Normals.* The draws' normals buffer was the first choice: the stock
    static models smooth their normals across 90 degree corners (a desk's
    face goes from one corner's average to the other's), which put black
    triangles on them; the depth's normals have none. `r_aoNormals 1`
    takes the draw's where it is within about 25 degrees of the depth's.
  - *Look.* At `r_aoRadius 32` and `r_aoStrength 1` the occlusion was
    hard to see (the scene's encoding lightens it: a visibility of 0.5 is
    0.73); 48 and 1.5 ground the lockers, desks, crates and barrels of
    `dm/mohdm6`. Alpha-tested foliage gets busy, blotchy occlusion, as in
    any screen space method.
  - *Cost* (RTX 4070 Ti, GPU time a frame, the room of `dm/mohdm6` at
    2560x1440, measured by leaving passes out, quality 2), after the
    review: 0.69 ms over the path's 0.162, the occlusion 0.43, the blend
    and the blur 0.17, the view distances 0.03, the apply pass 0.03 (left
    out, the view distances leave their images empty and the occlusion
    takes its shortcuts: the 0.12 below was that). The occlusion waits on
    its 18 scattered depth fetches a pixel: worked out without `pow`,
    without a `sin` and `cos` a slice (a rotation) and without a
    square root a step it took the same time. Fewer samples, with 16
    frames blended (converged views of `mohdm6`, quality 2 against itself
    the same to the bit): 9 slices change 1 % of the pixels by more than
    2 levels, 2 slices of 3 steps 4 to 12 % (none by more than 8: more
    noise left, 0.1 ms less), 3 of 2 and 2 of 2 6 to 22 % (up to 10 % by
    more than 8): quality 2 stays 3 of 3. Before the review: 0.68 ms in
    all, the occlusion 0.41, the blending over frames 0.14, the blur
    0.13, the view distances 0.12, the apply pass 0.04 (before the fix
    below; after it 0.80 in all). The RGBA8 result instead of RGBA16F
    and R16F view distances were 0.07 ms less; R16F was taken out again
    the same day: a wall close to the eye or at a grazing angle stepped
    from pixel to pixel in half floats, each step a ledge that occluded
    the next (dark stripes). Tried and left (no change): gathers instead of single fetches in
    every pass (kept: fewer instructions), the blur's inputs sampled
    instead of loaded as storage images (kept), the view distances without
    the normals' class; one pixel an invocation in the view distances'
    pass was 0.03 ms slower, its level 0 written in 8 x 8 blocks the same.
- **Motion blur (Plan 46).** `r_motionBlur 1` with the temporal path
  (`VK_TemporalMotionBlur`, `shaders/motion_blur.comp`, the gather in
  `shaders/temporal_composite.frag`). McGuire, Hennessy, Bukowski and
  Osman, "A Reconstruction Filter for Plausible Motion Blur" (2012), as
  Jimenez refined it for Call of Duty: Advanced Warfare ("Next Generation
  Post Processing in Call of Duty", SIGGRAPH 2014).
  - *When.* `VK_TemporalUpdateFeatures` latches it for the scene
    (`vkt.mbOn`: the passes, also for `r_temporalShow` 11 and 12;
    `vkt.mbBlur`: the gather) and makes its group of images
    (`VK_TEMPORAL_GROUP_MOTIONBLUR`, `GENERAL` always): `mbVelocity`
    (RGBA16F, the framebuffer's size) and `mbTile`, `mbNeighbor` (RG16F, a
    texel a tile). `VK_TemporalResolve` runs its two passes after the
    anti-aliasing; the composite then binds `compositeBlurPipeline` (the
    spec constant `MOTION_BLUR`) instead of `compositePipeline`. Alone,
    it needs the depth image readable as the anti-aliasing does, but
    neither the pyramid nor its barrier (the pyramid's group may not be
    there): `VK_TemporalResolve` gives the scene's images to their readers
    then without them.
  - *Exposure.* The motion vectors are a frame's motion; the blur is that
    of an exposure of `r_motionBlurShutter` frames at `r_motionBlurFps`:
    times `shutter x 1000 / (fps x dt)`, where dt is the game time between
    the motion vectors' two frames in milliseconds (`R_TemporalPrimary`:
    the primary view's `backEnd.refdef.time` delta, smoothed over about 8
    frames, 1 to 100; `vkSceneView_t.frameMsec`; none without a last
    frame). With `fixedtime 8` against `fixedtime 16`, a turn's blur in
    `r_temporalShow 11` is as long (a mean of 79.9 against 79.3 of 255 in
    the middle of a 1600x900 view); with `r_motionBlurFps 0` half (38.7).
  - *Tiles* (`motion_blur.comp`, a 16 x 16 workgroup a tile of K x K
    pixels; K = max(16, height / 40): 23 at 900, 27 at 1080, 36 at 1440,
    54 at 2160, so the blur's reach is the same share of the view at any
    resolution). Per pixel of the views: its motion, the draw's motion
    vector where it moves (`TEMPORAL_FLAG_MOTION`), less the camera's
    part that `r_motionBlurCamera` leaves out (the camera's motion of that
    point as `TemporalCameraMotion` gives it), else the camera's times
    that share; the view weapon its own motion vector or none (it moves
    with the camera). In pixels, times the exposure, halved (the blur
    reaches as far on each side), no longer than K; with its view
    distance (the weapon's divided by 1024: in front of everything, as the
    depth hack draws it, in its own order; at most 65000, which a half
    float holds) into `mbVelocity`. The workgroup keeps the longest of the
    tile (a reduction in shared memory) in `mbTile`. Then (`NEIGHBOR`, an
    invocation a tile) the longest of the 3 x 3 tiles around each into
    `mbNeighbor`: every pixel a blur of K or less can reach.
  - *Gather* (`MotionBlur` in the composite, after the sharpening, before
    the HDR expansion and the saturation). Where the longest blur around
    the pixel's tile is under half a pixel the pixel is left as it is (a
    still view is the build before's to the bit, with and without the
    anti-aliasing). Else 4, 6 or 8 pairs of samples (`r_motionBlurQuality`)
    on both sides of the pixel along it, at offsets moved by an
    interleaved gradient noise fixed per pixel (nothing blends it over
    frames after the composite: a noise that changed would flicker), every
    other pair along the pixel's own blur when that points more than about
    25 degrees away. A sample counts where it is behind the pixel and the
    pixel's blur reaches it (what the pixel's motion uncovers) or in front
    of it and its own blur reaches the pixel (what moves over it), the
    depths compared over 5 % of the nearer and 1 unit, the reach with half
    a pixel of slack (a still pixel reaches only itself: with Jimenez's
    whole pixel the still pixels of a tile where something moved took a
    little of their neighbours, and the tiles showed around the running
    soldiers); the pair's mirror (a nearer sample that hides what the
    farther one shows gives both the farther one's weight) estimates the
    background behind what moves from the other side. The samples' colors
    are summed in linear light (gamma 2.2) and what they don't cover is the
    pixel's own color.
  - *Checked* (1600x900, `fixedtime 16`, screenshots taken while the game
    runs): turning streaks the world sideways, more toward the edges,
    the sky by the rotation; strafing by the parallax; the soldiers'
    running limbs blur with the camera still and the courtyard behind
    them stays as it is; with `r_motionBlurCamera 0` a turn is the frame
    without blur but for the soldiers' own motion (no tiles around them
    since the half pixel of slack). The rifle in hand doesn't blur while
    turning and the wall's streaks stop at its edge (its receiver is the
    frame without blur's to the bit). With the anti-aliasing the same
    look. Toggled live with the anti-aliasing and the ambient occlusion
    and through a `vid_restart`, its images are made and freed with it.
  - *Cost* (RTX 4070 Ti, GPU time a frame, the `dm/mohdm6` room at
    2560x1440, `perf.py`, 3 runs; turning: `--exec '+right'`, 2.24
    degrees a frame at `fixedtime 16`): still 0.163 to 0.236 ms (the
    passes run whatever moves; the gather reads one texel a pixel there),
    turning 0.134 to 0.278 (quality 0 and 2: 0.256, 0.310), with the
    anti-aliasing turning 0.330 to 0.544. Split with `r_temporalShow 12`
    (the passes without the gather) against a view that reads nothing: the
    passes 0.07 ms, still or turning, with or without the anti-aliasing
    (the tiles pass reads the depth and the normals and writes 8 bytes a
    pixel); the gather 0.09 turning, 0.16 with the anti-aliasing, whose
    result it reads then: a storage image in `GENERAL`, which its samples
    seem to read slower than the scene's color attachment (not looked
    into). Not optimized yet: the samples need only the blur's length and
    the distance, so 4 bytes a pixel (the direction packed) would halve
    what both passes move.
- **Depth of field (Plan 47).** `r_dof 1` with the temporal path
  (`VK_TemporalDof`, `shaders/dof.comp`). The gather is Jimenez's,
  "Next Generation Post Processing in Call of Duty: Advanced Warfare"
  (SIGGRAPH 2014), at half the resolution, the background and the
  foreground apart.
  - *When.* `VK_TemporalUpdateFeatures` latches it for the scene
    (`vkt.dofOn`, also for `r_temporalShow 13`; `vkt.dofShow`) and makes
    its group (`VK_TEMPORAL_GROUP_DOF`, `GENERAL` always): `dofFocus`
    (R32F, 1 x 1), `dofHalf`, `dofFar`, `dofNear` (RGBA16F, half size),
    `dofTile`, `dofNeighbor` (RG16F, a texel a tile), `dofResult`
    (RGBA16F, the framebuffer's size). `VK_TemporalResolve` runs its five
    passes after the anti-aliasing and before the motion blur; the
    composite then binds `compositeDofSets` (`dofResult` as its color),
    so the motion blur gathers the defocused scene. The sharpening
    (`r_taaSharpen`, 0.8 by default) stays in the composite, for the
    pixels the apply pass left as they were (`dofResult`'s alpha 1,
    `mb.z` of its push constants): in focus the frames are those without
    the depth of field to the bit, and the blur isn't sharpened (it would
    show the gather's noise). Sharpening in the apply pass instead, as
    first done, rounded the sharpened color to half floats in
    `dofResult`: one level off in 0.6 to 1.2 % of the pixels. Alone, it
    needs the depth image readable as the motion blur does, without the
    pyramid.
  - *Lens.* A pixel's blur, its circle of confusion, is the radius
    S (1 / F - 1 / d) in pixels for its view distance d and the focus
    distance F: positive behind the focus (the background), negative in
    front of it (the foreground), at most a tile's side. It is a thin lens
    with the focal length small against the distances: the blur goes with
    the difference of the inverse distances, so a far focus leaves what is
    a few meters away nearly sharp (a first form, K (1 - F / d), blurred
    the whole world as foreground when the crosshair was on the sky). S is
    `r_dofAperture` percent of the primary view's height times 128 (the
    far distance's blur when F is 128 units) times the zoom: the
    projection's [5] over 1.589 (fov 80 at 4:3; the game keeps the
    vertical field at every aspect, Hor+), clamped to 0.5 to 4. A camera
    lens of the same f-number would blur with the square of its focal
    length; a scope's 4 times the zoom would then blur 16 times as much,
    hiding what isn't at the crosshair's distance, so the zoom counts once.
    The view weapon (its class, or what is nearer than the world's depth
    floor with `r_depthClamp`) has none and is never blurred over.
  - *Focus* (`FOCUS`, one workgroup of 64). 8 x 8 points of the depth over
    a square of 2 % of the height on each side of the primary view's
    center; the weapon left out, the sky (and what nothing drew) at the far
    distance. The distance that ranks at a quarter of the others is the
    target: the nearest of what is under the crosshair, not a stray pixel,
    nor a sliver of roof at the edge of the sky (left out, the sky gave
    the few roof points the focus); with only the weapon there the focus
    stays. `dofFocus`
    holds 1 / F and eases toward 1 / target by 1 - exp(-dt / tau) a frame
    (tau `r_dofFocusSpeed`, dt `vkt.frameMsec`, the game time between the
    frames, smoothed), so near and far changes take as long; at once
    without a last frame (a cut: `r_temporalCutDist`) or when the group is
    made (0). `r_dofFocus` sets the target instead. While the game is
    paused it still eases (the smoothed time keeps its last value).
  - *Tiles* (`SETUP`, a workgroup of 8 x 8 a tile of T x T pixels, T =
    max(16, height / 20), even: 54 at 1080, 72 at 1440, 108 at 2160). Each
    texel of `dofHalf` has the blur of the nearest of its 2 x 2 pixels and
    their color in linear light (gamma 2.2), each weighted 1 / (1 + the
    difference of its blur from that one), so the foreground's blur takes
    no background color at its outline; the tile's largest
    foreground blur and largest blur into `dofTile`. `NEIGHBOR` the
    largest of the 3 x 3 tiles around each into `dofNeighbor`.
  - *Gather* (`GATHER`, half resolution). A golden angle spiral of 16, 32
    or 48 samples (`r_dofQuality`), turned by an interleaved gradient noise
    fixed per texel. The background over the texel's own blur, a sample
    counting where it is background and what of min(its blur, the texel's)
    reaches the offset (with a texel of slack): what is behind doesn't
    spread over what is less blurred in front of it, and what is in focus
    doesn't spread at all. The foreground over the largest foreground blur
    around the tile, each foreground sample adding (R^2 / n) / r^2 of what
    reaches the texel, R the gather's radius and r its own: the share of
    its disc that the sample stands for, which sums to its opacity (1
    inside a blurred object, about half at its edge). A tile without blur
    reads one texel.
  - *Apply* (`APPLY`, the views' pixels). Where the tile has no blur, or on
    the weapon, the sharp scene unchanged. Else the 3 x 3 half-resolution
    texels around the nearest, a tent of 1.5 texels (which smooths the
    gather's noise): the background from the texels that are background
    only (no edge of what is in focus bleeds into it), put over the pixel
    by smoothstep(0.5, 2) of its own blur; the foreground over that by its
    opacity, at least smoothstep(0.5, 2) of the pixel's own foreground blur
    (its edge pixels would keep half of their sharp outline otherwise).
    Into `dofResult`, gamma-encoded as the scene, alpha 1 where the pixel is
    the scene's unchanged (the composite sharpens those), 0 where blurred.
    `r_temporalShow 13` draws the blur there instead: the foreground's in
    red, the background's in blue (1 at 8 pixels), the focus area's square
    in green over the scene in gray.
  - *Checked* (1600x900, `fixedtime 16`): in the `mohdm6` room the
    ceiling above the camera blurs and the far wall under the crosshair is
    sharp; in the crate hall the near crates and the pillar blur over the
    floor without a sharp outline or a halo; `r_dofFocus 48` blurs all
    beyond; the rifle in hand stays sharp with the room behind it blurred
    (`r_dofFocus 4000`, `r_dofAperture 2`). The focus released from 48
    with the crosshair on the far wall: the crate in front of the camera
    goes from behind the focus to in front of it in about 0.3 s and the
    wall is sharp at 1 s. With `r_dof 0` the frames of the room and the
    hall are the build before's to the bit, with the anti-aliasing; with
    `r_dof 1` and `r_dofAperture 0` (nothing blurred, the passes running)
    they are those of `r_dof 0` to the bit, the sharpening included. A
    teleport focuses at once (with `r_dofFocusSpeed 5`). The crosshair on
    the sky over the yard focuses at the far distance; with the far
    building under a quarter of the focus area or more, on it. No
    validation layer on this machine: not run under validation.
  - *Limits.* Translucent surfaces (smoke, glass, effects, muzzle
    flashes) write no depth: they take the blur of what is behind them.
    Portal and sky portal views take the primary view's lens. While the
    game is paused the focus still eases (the smoothed game time keeps its
    last value). The gather's noise is fixed per pixel and hidden by the
    tent; at quality 0 it shows a little in strong blur.
  - *Cost* (RTX 4070 Ti, GPU time a frame, the `dm/mohdm6` room at
    2560x1440 with the anti-aliasing, `perf.py`, 3 runs, a third of the
    view blurred): 0.392 ms without, 0.650 at quality 1 (0.624 and 0.695
    at quality 0 and 2 with the sharpening still in the apply pass, which
    cost about 0.01). Not optimized yet: the apply pass reads 27 texels a
    blurred pixel at full resolution, and the passes run over the whole
    view even where nothing is blurred.
- **HDR output (Plan 43).** `r_hdrOutput` (latched; `vk_output.c`,
  `shaders/output.frag`).
  - *Why not HDR rendering.* The plan before it kept the lightmaps in
    RGBA16F, unnormalized, as the scene's range above 1, and was taken out
    again: the lightmap bytes of the stock maps stop at 127 (`dm/mohdm1`,
    2, 6, `obj/obj_team1`, `m1l1`; `m2l3` has a handful up to 177), 254/255
    after the overbright shift of `r_mapOverBrightBits 1`, so the frames
    differed by one in 1 to 3 % of the pixels and nothing went above 1.
    The range above SDR white comes from the expansion below and from
    additive effects that stack in the scene's RGBA16F.
  - *Detection.* Asked of Vulkan and SDL, the same code on every system:
    the instance gets `VK_EXT_swapchain_colorspace` when `r_hdrOutput` is
    set (else the console says so and the frames are SDR), the device
    `VK_EXT_hdr_metadata` when it has it. `VK_ChooseSurfaceFormat` asks
    `VK_OutputChooseFormat` first: an `A2B10G10R10` or `A2R10G10B10`
    format with `HDR10_ST2084`, else `RGBA16F` with
    `EXTENDED_SRGB_LINEAR`, that can be a color attachment, when SDL says
    the window is in HDR (`SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`; some
    Windows drivers list HDR10 on SDR displays too) or `r_hdrOutput` 3 or
    4 forces the mode. On Wayland the driver's WSI does the protocol
    (`wp_color_manager_v1`: scRGB through `create_windows_scrgb`, HDR10
    through a parametric image description with BT.2020 primaries and
    PQ), on Windows DXGI. The console lists the surface's HDR formats once
    and prints the mode, paper white and peak whenever the mode changes.
    `VK_OutputUpdate` (at `VK_SelectTarget`, every frame) reads SDL's
    state: a change makes the swapchain out of date, and it is made again
    in the other mode (an SDL window starts not in HDR on niri: the first
    frames are SDR, then HDR10 once the compositor has told SDL). On the
    dev machine (niri 26.04, NVIDIA 615) the surface offers HDR10 in both
    10-bit formats and scRGB; SDL reports an SDR white level of 1 (none)
    and a headroom of 2.06.
  - *Frame.* `vk.colorFormat` is RGBA16F (`SAMPLED` too) whatever the
    swapchain's format, which can change at any frame: it holds the values
    the game draws (gamma encoded, 1.0 = paper white), the 2D drawing as
    before. The swapchain images are never the color target
    (`VK_CreateSwapchainTargets` makes the output pass's views and
    framebuffers instead) and need only `COLOR_ATTACHMENT` usage.
  - *Output pass* (`VK_RecordPresentCopy`, `VK_OutputRecord`; also on the
    submit thread, whose acquire semaphore waits at the color attachment
    stage, `VK_PresentCopyStage`): the frame image to
    `SHADER_READ_ONLY`, a render pass of the swapchain's format (nothing
    loaded, `PRESENT_SRC` at its end) drawing `temporal_composite.vert`'s
    triangle, the frame sampled bilinear (texel centers at the same size,
    as the blit with another size), the frame image back. `output.frag`:
    SDR clamps at 1; otherwise decode (2.2), clip at the peak, HDR10 times
    paper white in nits, BT.709 to BT.2020, PQ; scRGB times paper white /
    80. Its push constants (`vkOutputPush_t`) are made on the main thread
    at the end of the frame (`frame->outputPush`). Metadata (BT.709
    primaries, max luminance and content light level the peak, frame
    average paper white) at every swapchain and when the levels change
    (the submit thread drained first).
  - *Levels.* Paper white `r_hdrPaperWhite`, else SDL's SDR white level
    times 80 when it is above 1, else 203 nits (ITU-R BT.2408, and
    Wayland's default reference); peak `r_hdrPeak` (1000), with 0 SDL's
    headroom times paper white when it is above 1, else 1000 nits; never
    below paper white. P, the peak over paper white, is 1 with an SDR
    swapchain. The system's peak is not the default: on the dev machine
    the compositor reports 418 nits (the monitor's EDID in its True Black
    400 mode) while the display shows more, as games that send up to 1000
    nits do; most HDR displays handle what is above their peak themselves.
    The game's white in the 3D view lands at paper white + `r_hdrExpand`
    (peak - paper white): both cvars move it.
  - *Expansion* (`temporal_composite.frag`'s `HdrExpand`, a
    specialization constant of the composite set with `r_hdrOutput`, its
    parameters from `VK_OutputExpansion`): in linear light, the pixel's
    largest channel m; above the knee k (`r_hdrExpandKnee`) k + (1 - k)(t +
    b t^2) with t = (m - k) / (1 - k), continuous in value and slope at k;
    the game's white lands at H = 1 + `r_hdrExpand` (P - 1), so b = (H - 1)
    / (1 - k). The knee was 0.15 (about 0.42 encoded), 0.1 since the
    user's calibration (with `r_hdrExpand` 0.15, also theirs); measured
    on SDR
    screenshots of `dm/mohdm1` and `dm/mohdm6`, 99.9 % of the pixels of
    sunlit views are at most 0.45 in linear light, and with the first
    version's 0.6 only the sky was expanded, so paper white was the only
    way to brighten the view. With knee 0.15, `r_hdrExpand` 0.75, 203 nits
    paper white and a 1000-nit peak a surface at 0.3 went from 61 to 80
    nits, 0.45 from 91 to 166, 0.65 (the sky) from 132 to 339, white to
    801. The sky (class 3 of the normals buffer) with `r_hdrExpand`
    times `r_hdrSkyBoost`. Only the pixel up to the game's white is
    expanded, each channel clamped at 1 (the color SDR shows), scaled by
    the ratio (its hue kept); what is above 1 (overexposed surfaces,
    stacked effects) is added as gray of its BT.709 luma, times the
    expanded white and `r_hdrEffectsBoost`. Scaling the whole pixel, as
    the first version did, showed an overexposed surface's own hue where
    SDR clips each channel to white (`dm/mohdm1`'s sky, red and green
    above 1 and blue under it: pale yellow instead of white), which the
    user saw as more saturation than in SDR. Then its
    saturation (`r_saturation`, about BT.709 luma; the composite applies
    it without the expansion too, in SDR), clamped at P and encoded
    again, with gamma 2.2 (an SDR monitor's; the output pass decodes with
    it too). A first `r_hdrGamma` for the black calibration was taken out
    the same day: a higher gamma darkened the midtones and raised their
    saturation, the user then raised paper white to make up for it (600
    nits) and the picture got a hard contrast; `r_hdrBlack` (below)
    changes the darkest shades only. After TAA and its sharpening: the history is not
    expanded. Off with an SDR swapchain, with `r_temporalShow`, and in a
    frame that is read back. Without `r_temporal` the 3D view is shown at
    paper white (the console says so): the HUD is drawn into the same
    frame and can't be told from it afterwards.
  - *Calibration* (`r_hdrOutputTest 1`, not a cheat). Two menus built
    into the client (`cl_ui.cpp`, loaded from C strings with
    `UILayout::LoadBuffer` after the `ui/*.urc` files, so they exist
    whatever the search path; a `ui/<name>.urc` of the same name
    replaces one): `hdr_calibrate_black`, a stock `Slider` on
    `r_hdrBlack` (-0.5 to 1, step 0.05) with its value and Next, and
    `hdr_calibrate_peak`, a slider on `r_hdrPeak` (100 to 10000, step 50)
    and Finish, and for the HDR look `hdr_calibrate_black_sdr` (the black
    level's, Next) and `hdr_calibrate_peak_sdr` (`r_hdrPeak`, Finish);
    their widgets in the bottom fifth of the screen,
    the sliders 32 units tall. The stock slider put the value back to
    where the press was when the mouse left it mid-drag (`MouseExited`,
    `uilib/uislider.cpp`), so a hand that drifted out of its 16 units
    reset it: it keeps the dragged value now, every slider of the game.
    `UI_HdrCalibrationUpdate` (every frame, in `UI_Update`) opens the
    first when `r_hdrOutputTest` becomes 1, sets it back to 0 when no
    calibration menu is on top any more (Escape on the black screen),
    closes them while it is 0 (Finish sets it, so do the console and the
    renderer, which refuses without an HDR swapchain), and sets the
    internal `r_hdrOutputTestPage` (read-only: 1 black, 2 peak) for the
    renderer. With `r_hdrOutput 2` the output pass exists too
    (`vko.sdrPass`: its pipeline for the 8-bit swapchain, the frame image
    sampled) but draws only the frames of its calibration screens, which
    are then drawn offscreen (`VK_OutputNeedsOffscreen`) and given
    `r_hdrBlack` (the composite applies it to the other frames' 3D view);
    the other frames go to the swapchain as before, to the bit
    (`dm/mohdm1`, `r_temporal 1`). Next pushes the peak's menu on top of the black one, as
    the stock menus chain theirs: a `pushmenu` right after a `popmenu` is
    dropped, both lock the menus for their motion time, so the menus are
    closed one a frame; Escape on the peak screen goes back to the
    black. Widget `showcommand`s were no way to track it: they run as
    widget events, not console commands. The output pass draws the
    screen's pattern over the top four fifths of the image (the menu's
    widgets show below) where the menu left its black background, so the
    mouse cursor stays in front of it, not clipped at the peak, so what shows is the
    limit of the display and of what is between (compositor, driver).
    Black: a box of the game's value 8/255 on black through
    `r_hdrBlack`: the output pass scales every pixel by (m / (m +
    0.005))^`r_hdrBlack`, m its largest channel in linear light of paper
    white (0.005 is 1 nit at 203), so a crush (above 0) or a lift (below
    0) under about a nit and next to nothing above (0.2, a midtone, 2.4 %
    darker at 1, 1.2 % brighter at -0.5); the box is 0.098 nits at 0,
    0.009 at 1, 0.33 at -0.5 (203 nits paper white). Peak: a box at
    `r_hdrPeak` on 10000 nits (the largest PQ signal, 125 in scRGB):
    below the display's limit the box is darker than the clipped
    background, at or above it the two are the same, so `r_hdrPeak` is
    the lowest value where the box is gone. The HDR look's peak: SDR
    white, the most it can show, around a box of the game's 0.75 (a sunlit
    surface) through the look's own curve (`VK_OutputSdrBox`, the
    composite's expansion and shoulder for a gray), placed on the
    slider's range: its lift at 100 nits at the bottom, white at 10000
    (the user's request: white at the top; the look itself keeps the
    surface under white, 0.89 linear at 10000 with `r_hdrExpand` 0.15),
    so the slider shows how far toward white of what it can the look
    lifts bright surfaces. At `r_hdrExpand` 0.15: 191 of 255 at 100
    nits, 212 at 1000, 240 at 4000, 255 at 10000; at 0.5 227 at 1000.
    With `r_hdrExpand` 0 the look lifts nothing and the box stays. A
    first version left the 3D view to judge by eye behind the menu. The patches and ramp of the
    first version (`r_hdrOutputTest 3`) were taken out. Paper white keeps
    the system's value (or `r_hdrPaperWhite`): the brightness of the
    whole picture, a matter of taste.
  - *The HDR look in SDR* (`r_hdrOutput 2`, `SdrLook` in
    `temporal_composite.frag`, mode 2 of `VK_OutputExpansion`): no HDR
    output (no extension, no RGBA16F frame, no output pass: the SDR
    path's cost but the composite's few instructions), the console says
    when `r_temporal` is off. With `r_hdrOutput 1` on an SDR display the
    frames are plain SDR. The expansion
    as above with P from the cvars (no system levels in SDR), then P set
    to the expanded white (the larger of the sky's and the rest's), and
    on the largest channel a shoulder 0.5 + 0.5 u (1 + u / w^2) / (1 +
    u), u = (m - 0.5) / 0.5, w = (P - 0.5) / 0.5 (Reinhard's extended
    curve: continuous in value and slope at 0.5, P lands at 1), the
    saturation, `r_hdrBlack`'s black level as in the output pass,
    clamped at 1. A shoulder up to the virtual peak (P = `r_hdrPeak` /
    paper white, 4.9) instead pulled white down to 0.85 at the user's
    `r_hdrExpand` 0.15: a gray sky. A runtime branch on the mode, not a
    specialization constant: it can change at any frame.
  - *Turning it off.* `r_hdrOutput` changes the instance's extensions and
    the frame's format: latched, it takes effect at `vid_restart` (the
    user's choice; a version that queued the restart itself was taken
    out the same day).
  - *Screenshots and video.* `R_QueueScreenshot` and the video frame
    command call `vk_sdr_frame`: that frame's scene is not expanded, and
    `vk_read_pixels` turns the RGBA16F frame into bytes as an 8-bit target
    stores them (clamped, rounded). On an HDR display that frame is shown
    without the expansion.
  - *Frames.* With `r_hdrOutput 0` the build before's to the bit
    (`dm/mohdm6`: plain, `r_temporal 1`, and `r_taa 1` with
    `r_temporalJitterSamples 1`; with the temporal path `r_saturation 1`,
    its default 1.125 changes the 3D view). With `r_hdrOutput 1` the screenshots
    differ from the build before's by one in 4 to 15 % of the pixels (a
    pixel by 2 or 3), from blending in float instead of 8 bits, in SDR and
    in HDR; the console's notify lines differ (the HDR output messages).
  - *Cost* (RTX 4070 Ti, `dm/mohdm6` at 2560x1440 with `r_temporal 1`, GPU
    time a frame): 0.139 ms without, 0.233 with HDR output, the output
    pass 0.034 of it, the rest the RGBA16F frame (the 2D drawing's
    blending and writes) and no frame drawn into the swapchain directly.

## GL1 quirks kept on purpose

- `Set2DWindow`, the stencil shadow code and `RE_DrawBackground_TexSubImage`
  change cull state behind `R_Cull`'s cache, like GL1 did.
- `R_DrawDebugLines` and `R_DebugGraphics` draw from the front end
  (`R_RenderView`), before the back end draws the view over them.
- `RB_RenderFlare` doesn't set `tess.vertexColorValid` (see Flares).
- A one-pass shader whose stage has a lightmap in the second bundle gets
  `RB_StageIteratorLightmappedMultitextureUnfogged` whatever the stage's
  texture env (ioq3 requires `GL_MODULATE`), and that iterator always
  modulates: a lightmap stage and an additive stage collapsed with
  `r_ext_texture_env_add 1` draw as lightmap x texture, not lightmap +
  texture as with two passes.
- `RE_StretchRaw` (cinematics) waits for the GPU before each frame
  (`vk_finish`), as GL1 called `glFinish`.
- `R_State` enables fog on any transition of the external `GLS_FOG` bit,
  whatever the stage's `GLS_FOG_ENABLED` bit.
- The `nofog` shader keyword sets `GLS_FOG` in the stage bits.

Accepted difference from GL1 (decided on 2026-10-15 for the speed): the
dynamic lights of `r_fastdlights 0` are added to the lightmap per pixel
(see Implementation notes, Dynamic lights) instead of being sampled at the
lightmap texels' centers (16 units apart on most maps) and filtered with
the lightmap, so:

- the falloff is smooth: no texel steps (GL1's contour lines in the light
  pools of `r_lightmap 1` frames); away from saturation the two agree on
  average (`mohdm1_dlight` lightmaps, lit pixels below 200: within ±0.3 of
  GL1 on average, 89 to 97 % of them within ±2);
- the division by the largest component happens per pixel: GL1 divided
  each texel, then filtered, so where a saturated texel neighbours one
  that isn't, its filtered result is darker. Pixels between 200 and 245
  are 1 to 3 levels brighter on average (all lit unsaturated pixels: +1.4
  to +2.5, never more than 4 levels darker), and next to a light, where
  the lightmap drops sharply (the foot of walls, steps), up to about 20
  levels brighter in a band a few pixels wide (the largest differences of
  the lit views);
- GL1 added the light to the raw lightmap data, before its overbright
  shift and normalization; the shader adds it to the lightmap texture,
  normalized already, which only differs where the lightmap itself
  saturates (its hue then shifts a little less towards the light's);
- in the unlit texels of a lit surface GL1's dynamic lightmap normalized
  the lightmap with float arithmetic, the static one with integers (±1);
  the shader keeps the static lightmap's value there.

Everything else is GL1's: the light model and colors (the 375 scale and
the truncation to lightmap bytes per light), which surfaces get which
lights (lightmapped world faces, patches and terrain; `r_dlightBacks 0`;
`SURF_NODLIGHT`, which doesn't stop them; nothing on the sky), the
overbright shift and normalization, and additive lights, which GL1's
dynamic lightmaps treated like the others (only `ProjectDlightTexture`
adds them with a different blend).

Dynamic lights since 2026-10-20 (no longer GL1's, see Implementation
notes, Dynamic lights): a scene keeps the `r_maxDlights` (default 4, up to
64) lights nearest the view, where GL1 kept the first 32 added to the
frame, and with exactly 32 GL1's mask of all lights, `( 1 << 32 ) - 1`
(undefined in C, 0 on x86), gave no world surface a light; brush models
get every light that reaches a lightmapped surface per pixel, where GL1 lit
their lightmaps with light 0 only and projected the others
(`ProjectDlightTexture`) on the surfaces light 0 didn't reach (`dl2_door3`:
the door takes the colors of its three lights, as the wall around it does),
and their surfaces without a lightmap no light, as the world's; with
`r_fastdlights 1` all their lights are projected, where GL1 projected
light 0 only.

Differences from GL1 (all in paths that were dead, broken or debug only):

- Strip/discrete drawing (`r_primitives 1/3`) draws triangles like mode 2.
- `RE_DrawBackground_DrawPixels` draws a textured quad (no `glDrawPixels`);
  neither background function is exported to the client.
- The portal multitexture `glPolygonMode` hack for old GeForce drivers is
  gone.
- `RB_ShowImages` clears to black instead of the last clear color.
- `GL_CLAMP` textures (lightmaps, dlights, scratch) clamp to the edge; GL
  blended with the border color, which texel-centred coordinates hide.
- Ghost textures are respecified as single-level RGB every frame, as
  `glTexImage2D(..., 3, ...)` did; GL1 left mipmapped ghost textures
  incomplete. A ghost texture without an image is skipped instead of
  overwriting the default image.
- The cinematic scratch image is recreated when the frame size differs from
  its storage size (GL1 compared the source size and its sub-upload failed
  with `r_picmip` set).
- Mipmapped DDS textures get anisotropic filtering, GL1 never set it on them.
- Immediate mode keeps at most 4092 vertexes per triangle strip, polygon or
  line loop (all current users are far below).
- Immediate mode vertexes with different colors pass them as 8-bit values;
  a single color for the whole primitive is passed as an exact float.
- A texture unit whose image has no storage samples opaque black, as an
  incomplete GL texture does.
- The depth buffer is 32-bit float on AMD (no D24 there), so z-fighting
  surfaces can resolve differently from GL1's 24-bit buffer (not seen in the
  comparisons: quantizing the depth to 24 bits in the shader changed no
  pixel of the test views).
- Screenshots, levelshots and resampled screenshots are taken at the end of
  the frame in which the command runs, once it is drawn; GL1 read the back
  buffer the driver handed out when the command ran, an older frame on
  Mesa. The "Wrote" centerprint comes after the file is written, so it is
  not in the picture (GL1's never was either).
- Light flares (`RB_TestFlare`, unreachable) see depth changes 2 frames
  later than GL1 and don't force a `glFinish`-like wait (see Flares).
- `GLS_CLAMP_EDGE` is ignored. Nothing sets it; GL1 handled it by changing
  the wrap mode of the textures bound at that moment, permanently.
- A world loaded without `RE_BeginRegistration` (the client's `connect` to
  another server from a game) starts with no view cluster. GL1 keeps the
  previous map's, and when the new map's first view is in a cluster of the
  same number its `R_MarkLeaves` marks nothing: no world surface and no sky
  is drawn (only models, on whatever the image held) until the view's
  cluster changes. Seen with a local dedicated server, 1 of 24 connects.
- Depth and stencil reads return 0 (`vk_queue_depth_reads`: lost) in a
  frame whose depth buffer isn't readable yet: the first frame with reads,
  unless `vk_want_depth_reads` announced them (`r_measureOverdraw` does, so
  its counts match GL1 from the first frame), and again after 300 frames
  without reads. The depth and stencil contents don't carry over into a
  frame where the depth buffer switches (no code needs them to).

## Plan

Done:

1. Instance, surface, device, swapchain (present mode from `r_swapInterval`,
   recreation on resize and out-of-date), frames and synchronisation,
   `vk_clear`, `vk_read_pixels` (color, depth, stencil), capabilities,
   `r_vkValidation`.
2. Textures (storage, uploads in order, samplers, swizzle, filter changes),
   the vertex/index buffer, `vk_draw`, texture envs, blending, depth state,
   cull, scissor/viewport, the pipeline cache. Check: the main menu matches
   GL1.

3. Deterministic 3D comparisons (see Testing) and the parity check of the
   second texture unit, `TEXENV_REPLACE` (`r_lightmap 1`), the alpha test,
   blended surfaces, decals (polygon offset), static and animated models, the
   view weapon, and the single-player maps `m2l3` and `m4l1`.
   `TEXENV_DECAL` is never produced (its collapse entry is `#if 0` in
   `tr_shader.c`). Found and fixed on the way: the vertex transform and depth
   convention now match GL's arithmetic, and clear colors are rounded like
   GL's. Not covered by the test views: the stencil state (`r_shadows 2`,
   `r_measureOverdraw`), checked by validation and a readback self-test
   only. (`TEXENV_ADD` was covered in milestone 5 with a test shader: the
   candidates listed here, `snowfield1`, `afrika_ocean`, `deepbluesea`,
   `subpen_water` and `cliffsky`, all use `nextBundle` without `add` or lack
   `rgbGen identity`, so none produces it.)
4. Fog (Mesa's arithmetic, see Fragment pipeline), the user clip plane
   (`gl_ClipDistance`, object space plane, fragment shader fallback), depth
   ranges (sky at 1, view weapon at 0..0.3) and the sky and sky portal in 3D
   views, checked on `dm/mohdm2`, `dm/mohdm4`, `dm/mohdm5`, `m2l1`, `m2l3`
   and `m4l1`.

5. Flares: the sun lens flare and its fade compared with GL1; the light
   flare depth test, unreachable in the game, reads depth without waiting
   (see Flares). Dynamic lights (dynamic lightmaps uploaded mid-frame and
   `r_fastdlights 1`), cinematics (`RE_StretchRaw`, scratch image),
   `TEXENV_ADD`, `r_showtris` 1 and 2, `r_shownormals`, GL's line rule and
   line stipple (`VK_KHR/EXT_line_rasterization`). Not reachable with
   OpenMoHAA and its data, so not compared: ghost textures (no `.gst`
   file), AVI capture (compiled out of the client), the game's debug lines
   (covered by the view, see GL1 quirks), `GLS_CLAMP_EDGE` (never set;
   ignored) and `RE_UploadCinematic` (the shader parser has no videoMap).

6. (first half) Parity pass against GL1 on the maps not covered before:
   `dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`, `m1l1`, `m1l2a`, `m1l3a`, `m3l1a`,
   `m3l2`, `m3l3`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`, `m6l2a`, `m6l3a`,
   `obj/obj_team1` to `obj/obj_team4` (see Testing for the content and the
   numbers). Every difference beyond the driver ones was traced to random
   game content, views from outside the world or GL1 saving a black or
   stale frame; no renderer change was needed. The tools gained the
   outside-world and black-frame warnings, `bspinfo.py` terrain, static
   model, decal and `--view` queries, `ui_gmbox 0` in `run.sh`, and
   `perf.py` with first frame times.

6. (second half) Performance, on an idle machine (see Performance for the
   method and numbers). The tools now measure the renderer's own time with
   a microsecond clock on both renderers (`renderer_timed.so`), GPU time
   from timestamps and backend counters (`vkinfo`), and sample the main
   thread's stack (`libstacks.so`, `stacks.py`). In order of measured
   benefit:
   - the depth image is only readable (`TRANSFER_SRC`) while depth or
     stencil readbacks are wanted (not in the original plan; it explains
     the main pass that took 6 to 7 % more GPU time than GL1's whole
     frame): RADV gives readable depth images TC-compatible HTILE, which
     keeps a depth tile compressed only up to 4 planes; 12 to 13 % less GPU
     time in the `m6l1a` forest, whose frames now take 7 % less time than
     GL1's at 3840x2160 and 3 % less at 1920x1080;
   - frames are drawn into the swapchain image instead of an offscreen
     image copied at present (not in the original plan; the profile of the
     `m6l1a` forest showed Vulkan was GPU-bound there and the copy took 8 %
     of its GPU time): 8 to 9 % more frames per second where the GPU
     limits;
   - frame pacing: the next swapchain image is acquired right after the
     present, so with vsync the wait for the display comes before the game
     runs the next frame (one refresh less of latency, jitter like GL1's);
   - the texture memory sub-allocator: 10 or 11 device allocations per map
     instead of 300 to 560, 10 to 25 ms less per map load, no per-frame
     change;
   - the pipeline cache saved to disk: 18 ms of pipeline compilation up to
     the first view of `m6l1a` when the driver's shader cache is cold, 0.4
     ms on RADV with its own cache;
   - reusing the vertex copies of multi-pass shaders: 1 to 4 % fewer vertex
     bytes, no measurable time;
   - fewer render pass breaks: not done, there are none to remove (one
     render pass per frame in every measured view, with dynamic lights and
     the sky portal too);
   - `r_ext_multisample`: multisampled images resolved into the frame's
     color target, the depth/stencil buffer only for readbacks (1.3 ms per
     frame saved at 1920x1080), matching GL1's multisampling about as
     closely as without it.

6. (second round) The renderer's CPU time and multisampling (see
   Performance, Second round), each change measured against the build
   before it and checked bit-identical on the 31 view lists: hidden symbol
   visibility (code 960 to 715 KB, the largest single CPU gain), static
   model culling without the model matrix and from a compact array, sun
   visibility traces kept by origin, a one-pass radix sort, the distance
   fade loop; alpha written with multisampling (the multisampled frame 6 %
   faster on the GPU, 3 % behind GL1 instead of 9 %). The stack sampler
   now samples at random intervals (it locked onto the 1000 frames per
   second cap), `stacks.py` has `--tree` and `--callers`, and the
   backend's duplicated framebuffer, present and readback barrier code is
   shared.

7. Step 1 of the redesign: static geometry (see Implementation notes,
   Static geometry, and Performance, Static geometry). The world's faces,
   triangle surfaces and grids and the static models are uploaded once at
   map load and drawn from device memory when their shader allows it,
   instead of being tessellated into `tess` and copied into the frame's
   vertex buffer every frame; consecutive static models with the same
   one-pass shader share one batch. Everything the GPU path doesn't
   support falls back to the CPU path, batch by batch, with GL1's batches
   and draw order. The frames are the same to the bit as the build before
   it in every view without random content (see Testing).

8. Dynamic lights per pixel (2026-10-15/16, see Implementation notes,
   Dynamic lights, Performance, Dynamic lights, and the accepted
   difference in GL1 quirks). First the exact CPU fix (each light tested
   against the face's lightmap texels, patch quads and terrain bounds
   before the texel loops; the same lightmaps, proven texel for texel
   against GL1's code over 2.1 million surface evaluations with random
   lights), then the lights evaluated in the fragment shader on the
   lightmap, which replaces the dynamic lightmaps (no atlas, no uploads,
   no batches of their own, lit surfaces stay on the static geometry); the
   per-surface masks are then made cheap with a box test of the texel
   centers and lights culled per BSP node. With 16 random lights the
   renderer's CPU time drops by 66 % in the `obj_team2` yard, 59 % in the
   `dm/mohdm3` overview and 26 % in the `m3l3` path (640x480), with no
   measurable GPU cost even at 3840x2160 (less GPU time than the dynamic
   lightmaps took). `r_fastdlights 1`, entities and static
   models are unchanged (see Remaining).

9. Step 3 of the redesign: skeletal models on the GPU (2026-10-16, see
   Implementation notes, Skeletal models, and Performance, Skeletal
   models), chosen from the profiles of new multiplayer views with 20
   player models (`tools/soldiers.py`, `views/mp_*.txt`,
   `views/perf_mp.txt`), where skinning, per-vertex lighting and the
   copies of the skinned vertexes took 35 to 50 % of the frame. The
   vertex shader skins the surfaces of the vertex-lit shaders (characters,
   weapons), lights them like `RB_Light_Real` and collapses their
   vertexes at a lower level of detail, with the CPU's floating point
   operations: the frames are the same to the bit. A bug of the sun trace
   cache of the second round was fixed on the way (every entity on
   whole-unit coordinates went to the same slot). The renderer's CPU time
   goes down 57 to 64 % in these views, with no measurable GPU cost.

10. The GPU time at 3840x2160 (2026-10-16 evening, see Performance, GPU
    time at 3840x2160): GL timestamps for GL1 (`tools/rtimer/glts.c`)
    and temporary ones in the Vulkan renderer put the 1 to 4 % that
    Vulkan took more than GL1 in the drawing, mostly in views that clear
    the color buffer, and A/B builds found three causes: partial color
    writes (the pipelines wrote RGB only to the BGRA swapchain image,
    GL1's BGRX buffer takes whole pixels), the unused stencil plane of
    the depth image, and RADV's comp-to-single clears of the fog color.
    The pipelines without blending write alpha, the depth image has no
    stencil plane when `r_stencilbits` is 0, and on RADV those clears
    are drawn: 2.7 to 5.4 % less GPU time per frame in six of the seven
    views measured at 3840x2160 (0.7 % in the seventh); Vulkan's frames
    are now faster than GL1's there but in the `dm/mohdm3` overview (0.6
    % slower). The frames are the same to the bit. What is left is
    RADV's choice not to compress textures (see Remaining).

11. Frame spikes, measurement only (2026-10-16 night, see Performance,
    Frame spikes): the frame log (`vk_framelog.c`, `r_vkFrameLog`,
    `vkframelog`), per-frame files of `renderer_timed.so` and `libglts.so`
    for GL1, demos in `perf.py` (`views/perf_demo.txt`) and
    `tools/framelog.py`. The causes it found are the first entries of
    Remaining.

12. Frame limiter (2026-10-17, an engine change, see Performance, Frame
    limiter): `Com_Frame` waits for a deadline in microseconds that
    advances by one period (`Sys_Microseconds`, `NET_SleepUs`, a timer
    slack of 1 µs on Linux), sleeping until 1 ms before it and polling the
    rest, instead of whole milliseconds since the last frame; the `bias`
    is gone, the 1 ms floor (no frame of 0 ms, uncapped at most 1000 a
    second) stays. Measured with both renderers in a match demo and a
    fixed view at nine caps: exact caps, even frame starts, the median
    input to present time the same or shorter, more CPU; shorter sleeps
    instead of the polling saved CPU but cost latency (the core clocks
    down). The deterministic views give the same frames to the bit. The
    tools gained `perf.py --binaries`, `--caps`, `--seconds`, `--preload`
    and the game's CPU use and core, `libinlat.so` (input read to present,
    either renderer), its files in `framelog.py`, and `MOH_BIN` in
    `run.sh`. Then caps that divide 1000 went back to starting frames on
    the tick of `Sys_Milliseconds` (a deadline in whole milliseconds; the
    wait knows the tick from the last one it saw and still polls the last
    millisecond): with the deadline in microseconds anywhere in the
    millisecond, the read of `com_frameTime` after the input sometimes fell
    in the next one, up to half of the frames 3 and 5 ms at 250 depending
    on the phase. Measured per frame with a temporary logging build, in a
    local game and a demo without `fixedtime`: every frame 1000 / cap ms
    again but for late frames, input to present and CPU those of the
    deadline in microseconds; the old way alone of waiting for the tick
    cost 0.02 to 0.05 ms of input to present.

13. Present timing, measurement only (2026-10-17, see Performance,
    Present timing): `libinlat.so` with `INLAT_DISPLAY=1` records what
    happens to each present after the call (Xwayland's completion events,
    Wayland's presentation feedback with present ids it enables, the GPU
    completion of every submission through a timeline semaphore of its
    own, the kernel's vblanks and the framebuffer on the primary plane,
    the window's fullscreen state; `INLAT_PRESENT_MODE` forces a present
    mode) and `framelog.py --display` works out each frame's time to the
    display; no renderer change. Measured at 1920x1080 with Xwayland and
    native Wayland, in a window and fullscreen, immediate, mailbox and
    FIFO, at caps 0, 59, 60 and 250, in the `obj_team2` yard and the
    `o2match` demo: vsync through KWin costs 3 to 7 ms over tearing
    uncapped and at 250 (8.5 to 11 ms at 59 and 60), composition 1 to 1.5
    ms of it; KWin bypasses itself (direct
    scanout) for fullscreen mailbox and FIFO but composites with tearing;
    FIFO queues three frames; Xwayland costs nothing measurable.

14. World culling (2026-10-17, see Performance, World culling, and
    Implementation notes, World culling), starting from the Metal port
    hand-over's review of JayRewd's exact world culls (omaha 1c2c8754),
    each taken only in a form that gives GL1's leaves and surfaces: world
    faces wholly outside the view culled (the bounds of each face, a unit
    of margin; not for deforms, the sky, portals and the sky portal, nor
    where dynamic lights reach, whose batches take the lights of all
    their faces), the
    BSP walk replaced by the potentially visible leaves in its order with
    culls of 8 leaves at a time (exact by box containment; the leaves
    that stick out of an ancestor's box tested along their path; the
    commit's list also took the brush models' leaves), what culling a face
    reads kept in its `msurface_t`, and the PVS listed from a compact copy
    of the leaves instead of GL1's pass over every node (0.23 ms at each
    cluster change in `obj/obj_team4`, 67 µs now on the median). A
    temporary verification build ran GL1's code beside it in 92254 views
    of 40 view lists and 4 demos: the same leaves in the same order, the
    same plane culls, every face culled against the view outside the clip
    volume; the frames are the same to the bit. The renderer's CPU time
    goes down 2 to 10 % in the `obj_team2` yard, 4 to 5 % in `o2match` and
    1 to 2 % in `o4match`, and stays within the noise in the `dm/mohdm3`
    overview; the slowest 1 % of the demos' frames, compared frame by
    frame, change by -0.05 to +0.02 ms.

15. The known risks before shipping (2026-10-18, see Remaining, Known
    risks, Implementation notes, Presentation semaphores and swapchains,
    Textures and Draws, and Performance, Present fences). Presentation:
    the semaphores presents wait for belong to their swapchain (one per
    image, reused once the image is acquired again), and a replaced or
    destroyed swapchain is retired with them and destroyed once the
    presentation engine is done with them: with
    `VK_KHR/EXT_swapchain_maintenance1` when the fence of its last present
    has signaled (present fences, chosen and reset after the present before:
    nothing between a frame's input and its present), otherwise after the
    `vkDeviceWaitIdle` that comes before (which, per the specification,
    waits for the presents' queue operations); the image acquired ahead is
    also handed back (`vkReleaseSwapchainImagesKHR`), and shutdown destroys
    the swapchains before their semaphores, not after. MoltenVK:
    `imageViewFormatSwizzle` is queried and enabled; without it the shader
    sets the alpha of RGB textures; the bindings of stride 0, which the
    portability subset never allows, are read per instance there. Lines
    without `VK_KHR/EXT_line_rasterization`: checked. `r_vkFallbacks`
    forces each fallback: the same frames to the bit as the build before
    (a temporary build with alpha 0 in RGB textures showed that the
    shader's alpha is what keeps them so), checked on RADV and llvmpipe,
    and every presentation path ran clean under synchronization
    validation. Input read to present the same within the noise, uncapped
    and at 250, in a window and fullscreen. A checklist for testing on
    Windows: [WINDOWS_TESTING.md](WINDOWS_TESTING.md).

16. Pipelines and skin data at load (2026-10-18, see Performance,
    Pipelines and skin data at load, and Implementation notes, Pipelines
    at load and Skeletal models). At the end of a registration
    (`RE_EndRegistration`) the pipelines of the draws the registered
    shaders will make are created (`tr_pipelines.c`: the stage iterators'
    state, with each value of what only the frame decides: where the
    shader is drawn, the fog, dynamic lights, the images of animations, the
    sky box, the clear to the fog color and the UI's boxes), through
    `vk_draw`'s own key function, so a draw finds the pipeline it would have
    created; a shader registered later gets its own when it is created; the
    keys that still come after a load are kept in `vkpipelines.keys` and
    created at the loads of the next runs (after a driver update too);
    unused ones are destroyed at the next registration. A skeletal model's
    skin data is added when the model is registered instead of at its
    first draw. With cold caches the 39 and 49 pipelines that two matches
    created on first use (60 and 80 ms, up to 58 ms in one frame) are
    created at load (the map load 0.18 to 0.20 s longer): 2 left in the
    match for the first play after an install, 0 after a driver update and
    with warm caches, where the load is as long as before; the 99.9th
    percentile with cold caches 2.08 to 1.56 ms and 3.66 to 2.76 ms, the
    maximum 33.6 to 21.2 and 58.4 to 28.5 ms (registration, shared with
    GL1). No skin data at first draws any more. The same frames, the same
    input to present time. The frame log no longer charges what happens
    between frames (a load) to the first frame after it.

17. Skeletal culling (2026-10-19, see Performance, Skeletal culling, and
    Implementation notes, Skeletal culling), from JayRewd's b26d8a67: a
    skeletal model whose sphere lies behind one of the view's side planes
    returns from `R_AddSkelSurfaces` before its bones, morphs and surfaces
    (and its lighting and draws in the back end). A temporary verification
    build computed every posed vertex of every model the cull could reject
    in 20 matches and the 41 view lists (12.9 million models): the
    animation frame's radius is exceeded by up to 1.2 times for players
    whose bone controllers turn the torso, 3 times by the stick grenade,
    and by the scaled axes of muzzle flashes and splash drops, whose
    negative scale also made the radius negative (JayRewd's rule culled
    such drops in view); radius times 1.25 plus 32 TIKI units, times the
    axes' norm, never culled a model with a vertex in view. The renderer's
    CPU time 6.5 to 8.5 % lower in the matches, 4 to 13 % in the soldier
    views, the slow frames and the input to present time lower; the same
    frames to the bit.

18. Command rate (2026-10-19, see Performance, Command rate), from
    JayRewd's `cl_cmdfps` (omaha 1620f310) and limiter change (b26d8a67):
    usercmds at a fixed rate (`cl_cmdfps`, 85 to 125 a second) on a grid of
    server time, every step the same length at every frame rate (his
    depend on it), several per frame below the rate; the view turned every
    frame through a new cgame import (`GetViewUserCmd`), no command
    rewritten after it is made; the client samples that stock step, with
    the keys held this frame, by a sub-millisecond fraction latched to
    `com_frameTime` (not a `Pmove` input); no packet
    without a new command; cgame keeps what the network doesn't carry when
    the snapshot has run every command; client caps above 1000 and uncapped
    on the microsecond deadline, frames of 0 ms counted as 0 ms, the `fps`
    counter in microseconds. Default 100 commands a second. Why short
    steps get stuck on bumps (pmove's step-up refused on a steep bevel,
    then the speed lost) was found with a pmove sweep of `dm/mohdm2` and
    walks in the game: 1 ms steps stop 3 to 4 times as many walks as 8 to
    12 ms, 4 ms 1.6 to 2 times, no step none. Steps exact from 60 to 3300 frames a
    second, the same moves to the bit at 144, 250, 1000 and uncapped,
    nothing lost between commands, prediction errors and input to present
    as before.

19. Submit thread (2026-10-19, see Performance, Submit thread, and
    Implementation notes, Submit thread), from JayRewd's a5f5a54a, behind
    `r_vkSubmitThread` (off by default): a thread submits and presents
    the frames and acquires the next image right after each present; a
    frame is drawn into that image when the thread has it by the frame's
    first render pass (his version drew every frame offscreen and copied
    it: 0.4 to 0.57 ms of GPU time per frame at 3840x2160), otherwise
    offscreen and copied by the thread; 2 frames in flight (his 8), waited
    for before the next frame's input, with FIFO the frame just handed over
    too; 5 swapchain images, 3 with FIFO. The queue and the swapchain are
    used by one thread at a time (the main thread drains the thread first),
    the thread calls no engine function, and `libinlat.so` is told which
    frame each present shows. Measured at 640x480, 1920x1080 and
    3840x2160, immediate, FIFO and mailbox, uncapped and at 250, against
    the build before and with the thread off: see Performance; the same
    frames to the bit, no validation message (synchronization validation
    too) through resizes, minimize, fullscreen, `vid_restart`, mode
    changes and switching it on and off, on RADV and llvmpipe.

20. Redundant state and empty flushes (2026-10-20, see Performance,
    Redundant state and empty flushes), from JayRewd's b26d8a67 without its
    2D batching: the setters of the viewport, the scissor, the projection,
    the modelview and the fog range return when the value doesn't change
    (what they mark is applied by the next draw, and every command buffer
    starts with all of it marked), and `R_IssuePendingRenderCommands`
    returns when no command is queued (each 2D call flushes first: 120 of
    122 flushes a frame in a match were empty). The renderer's CPU time 4
    to 7 % lower in two matches and the `dm/mohdm3` overview, the matches'
    99th and 99.9th percentiles lower; the same frames to the bit. With it,
    JayRewd's one-line fix of a world loaded without `RE_BeginRegistration`
    (an in-game `connect`): `RE_LoadWorldMap` resets the view cluster, which
    otherwise kept the old map's, so a first view in a cluster of that
    number drew no world (seen with a dedicated server).

21. Step 2 of the redesign: the effects shader (2026-10-20, see
    Implementation notes, Effects shader, and Performance, Effects shader),
    from JayRewd's b26d8a67: static batches with the deforms, `tcGen`s,
    `tcMod`s, `distFade` and static model lighting it takes are drawn from
    the static geometry (now with normals) by `shaders/effects.vert`, with
    what doesn't depend on the vertex in a record of the frame data per
    draw. Not as his: a stage with a constant color and `distFade` gets the
    alpha per vertex (his got the stale alpha of an earlier batch), a batch
    goes to the CPU when the frame data has no room for its records
    (his draws were left out), no binding of stride 0, the pipelines built
    at load; his indirect draws and terrain buffer not taken. `fx_mohdm1`
    (a test shader file) covers what the game data's views don't. The
    renderer's time 2 to 11 % lower at 640x480 (-53 µs in the `m6l1a`
    forest, -20 µs in the `dm/mohdm3` overview, -10 and -14 µs in the
    matches), input to present a little lower, the GPU time the same, the
    frames the same to the bit on RADV. `perf.py` loads the map at 250
    frames a second (its waits count frames: uncapped, the client of a
    large map like `m6l1a` hadn't connected yet when the view was set up).

22. cgame caches (2026-10-20, see Performance, cgame caches), from
    JayRewd's b26d8a67 (cgame, both renderers): shadow traces kept by origin,
    temporary marks (the shadows of `cg_shadows`) kept for the rest of a
    server snapshot when no fragment is on a brush entity, the active
    temporary models counted as they are added and freed, a dead conversion
    to Euler angles per sprite removed. A temporary cgame that also did all
    of it found the same results in 20 matches with `cg_shadows` 1 and 2;
    cgame's time per frame 2 to 4 µs lower in two matches; the same frames
    (new list `mp_shadows`).

23. Skinned passes (2026-10-20, see Performance, Skinned passes, and
    Implementation notes, Skeletal models), from JayRewd's a5f5a54a: the
    generic iterator's skeletal model shaders whose stages are one texture
    or an environment map, colored by the sphere's lighting, the light grid
    or a constant (with an alpha of their own where the color is one
    constant), each stage one draw per surface with draw records of its
    own, the environment map's coordinates in `skinned.vert` with
    `Q_rsqrt`'s operations (his `inversesqrt` moved a pixel), their skin
    data and pipelines at load. The same frames to the bit (new list
    `skin_passes`); the renderer's time 11 to 14 % lower in its views,
    unchanged in the matches, where these models are few.

24. Dynamic lights, 64 of them (2026-10-20, see Performance, Dynamic
    lights, 64 of them, and Implementation notes, Dynamic lights), from
    JayRewd's b26d8a67: 64-bit masks (`dlightMask_t`; the face keeps its
    layout: one mask where it had two-element arrays for a render thread
    that doesn't exist), `r_maxDlights` (latched, default 4, up to 64: the
    lights nearest the view, where GL1 kept the first 32 added and gave no
    world surface a light with exactly 32), a draw lit by lights more than
    32 apart in its list gets a list of them with their count (his took
    the first 32), brush models lit per pixel by every light that reaches a
    lightmapped surface (GL1: light 0, and `ProjectDlightTexture` for the
    others, now unused with `r_fastdlights 0`), the static models' light
    list out of each model's struct, two dead `tess` fields removed. The
    renderer's time the same at 0 to 16 lights and in the matches; the
    frames the same to the bit but where a brush model has other lights
    than its first or a scene more than 4 (new list `mohdm1_dlight64`, new
    view `dl2_door3`).

25. Terrain (2026-10-22, see Performance, Terrain, and Implementation
    notes, Terrain), from JayRewd's b26d8a67: the LOD terrain's batches are
    drawn from a copy of its vertex pool per frame slot in host visible
    memory, brought up to date by its own frame with the slots that changed
    since it was last written (a bit per slot, set when a split fills it
    and when a geomorph moves it), indexes only per patch; his single device
    local copy needed the staging buffer and two barriers every frame. The
    geomorph and merging walk a list of the patches something can happen
    to, in the patches' order, so the tessellation stays GL1's (his order
    changed it). A verification build over 20 matches found every patch it
    left out one GL1's walk does nothing to and every pool vertex equal to
    the CPU's. The renderer's time 7 to 8 % lower where terrain fills the
    view at 640x480, 1.7 to 3 % in two matches, the GPU time and the frames
    the same.

26. 2D batches (2026-10-22, see Performance, 2D batches, and
    Implementation notes, 2D batches), from JayRewd's b26d8a67: the 2D draws
    of a single-pass shader whose colors and texture coordinates
    `ComputeColors` and `ComputeTexCoords` make from each vertex alone (or
    the 2D color and time) stay open in `tess`, and the next ones with that
    shader join them, until something else is about to use the renderer: the
    paths that run the back end (`R_IssuePendingRenderCommands`,
    `R_IssueRenderCommands`) and the exports that change what the batch reads
    (the 2D color, the shader time, the scissor, a shader's image, a
    cinematic's upload, a font's shader, the frame's start); his version
    ended it from every state setter of the backend and joined any shader,
    whose colors could come from an earlier surface. `DrawBox`'s boxes with
    alpha 0 (the UI widgets' transparent backgrounds and borders: 37 of 40 a
    frame in matches) are left out. A temporary build that compared the
    renderer's state at every draw that joined a batch and before drawing it
    found nothing else running while one was open over 20 matches; a paused
    frame and frozen demo frames give the same pixels merged and drawn one by
    one (`r_batch2D` 3 and 0 in one run). The renderer's time 5 and 2.3 %
    lower in two matches at 640x480, 13 µs less GPU time a frame; the
    frames the same to the bit (new list `hud`).

27. Light grid lookups (2026-10-22, see Performance, Light grid lookups,
    and Implementation notes, Light grid lookups), from JayRewd's b26d8a67:
    the samples of the last 1024 light grid cells looked up, decoded from
    the run-length encoded columns once, and the results of the last 1024
    points by the point's bits (debris lit by cgame every frame mostly lies
    still), in tables allocated with the world; his were keyed by the
    grid's address, which the next map can reuse. A verification build
    compared every lookup with the old function in 4 matches (5.74 million,
    none different). In matches the lookups are 0.2 to 1.3 % of the frame,
    nearly all cgame's lighting of debris and smoke; 13 to 35 ns less per
    smoke lookup, 8 to 12 µs less in the frames with 200 or more, the
    slowest frames' percentiles of cgame and the renderer 2 to 13 µs lower
    (0.3 to 1.6 %), within the runs' noise as a whole (JayRewd measured 3.5
    to 3.8 % off the 99th percentile frame time in a 22-player fight on
    `dm/mohdm6`; new list `smoke`).

28. Registration in the match (2026-10-22, see Performance, Registration
    in the match, and Implementation notes, Registration at load; cgame and
    the engine, both renderers): first measured over 21 real matches with a
    temporary build that timed each registration cgame makes (what loads
    in the match: the objectives' explosions of the map's entities, 17 to
    18 ms; players' models the server's precache list lacks, 7 to 8 ms;
    items, turrets, vehicles, projectiles; cgame's decals and HUD shaders;
    the player's first person model in the first frame). Then, in
    multiplayer, cgame registers at the load every player model of the
    data with its first person model, the `cache` lines of the map's
    precache script and the scripts it executes, the `.tik` models of the
    map's entities (a new cgame import, `CM_EntityString`) and of its
    script, the projectiles, explosions and turret view models these name,
    the health box a dying player drops and its own first-use shaders and
    sprites (`cg_precache`, bits 1, 2 and 4); a model so registered is
    cgame's own and stays when its configstring goes. The file system
    answers a lookup that a search directory can't satisfy from a listing
    of the directory, valid while its modification time stays the same,
    instead of `FS_CorrectCase`'s `access()`es and directory scan and a
    failed `fopen` (`FS_DirMayHave`, `fs_listDirs`; 17 to 23 µs to 1.2 to
    1.5 µs per search directory and lookup). Of the 61 frames with 0.3 ms or
    more of registration in the 21 matches, 7 are left (one a load: a map
    script that a mod pk3 on this machine replaces); the maximum frame time
    of `o2match` and `o4match` 21.4 and 27.9 ms to 2.9 and 5.4 ms, the
    99.9th percentile the same or lower, the map load 0.04 to 0.06 s
    shorter, the texture memory 2 to 4 MB larger at the end of a match.
    (b) a faster inflate and (d) the mipmaps on the GPU (Remaining 1 before
    this step) were not done: what they would speed up now happens at the
    load.

29. Round changes (2026-10-23, see Performance, Round changes, and
    Implementation notes, Round changes; the engine and cgame, both
    renderers): measured first with temporary builds that timed the sound
    system's calls and the configstring changes in each frame: a round
    restart started the soundtrack again (its mp3 opened by scanning the
    whole file, two reads of 16384 samples decoded: 1.0 to 1.1 ms with this
    machine's 0.24 s songs, 2.7 ms with a 3 minute one) and changed 60 to
    70 configstrings, each copying the client's whole gamestate twice and
    cgame's once (0.6 ms), and a playing song decoded 8 buffers in 8 frames
    in a row every few seconds. Now the soundtrack's mp3s are read into
    memory when it loads and decoded ahead on a thread of the sound system
    (`s_streamThread`; the same PCM, checked buffer by buffer), the
    soundtrack file's text is kept (`s_soundtrackCache`), and a changed
    configstring goes after the others' text in the client's gamestate and
    in cgame's (`cl_csAppend`, `cg_csAppend`, with a new cgame import):
    the round changes' frames 3.1 to 3.8 ms to 1.9 to 2.4 ms in `o4match`
    uncapped (4.9 to 5.4 to 1.9 to 2.5 with a 3 minute test song), their
    input to present 0.5 to 0.7 ms shorter at 250 frames a second, the
    99.9th percentile the same or lower; the same frames. Part (b) of the
    item (the first frame's HUD registrations, the body's water sounds)
    was not done.

30. Busy moments (2026-10-23, see Performance, Busy moments, and
    Implementation notes, Terrain and Sound buffers at load; the renderer
    and the engine's sound system): the slow frames of
    `o2match` and `o4match` profiled again (the frame log and stack
    samples of the same runs): in `o4match` the terrain's tessellation
    leads (0.23 of the slow frames' 0.73 ms excess), in `o2match` cgame's
    events of the shots (0.14 of 0.65 ms). Three exact parts for the
    terrain's walks (`r_terrainCache` 1 to 4: the geomorph and the merging
    leave out the patches where they would change nothing, a patch's
    indexes are kept until its triangles change; checked over 21 matches),
    and the sounds' OpenAL buffers made when the registration ends instead
    of on their first play (`s_loadBuffers`, 0.2 to 0.6 ms each, 131 to 146
    a match):
    `o4match`'s 99th percentile 1.518 to 1.373 ms uncapped, its slowest 1
    % of frames 87 µs shorter, the mean 70 µs lower, input to present
    lower at every percentile (at 250 the 99.9th 2.224 to 1.802 ms);
    `o2match` 0.03 to 0.06 ms lower percentiles uncapped, the same within
    the spread at 250; the slowest 0.1 % of `o4match` unchanged (the
    terrain's splits after a large turn, not shortened); the same frames,
    the same tessellation, the same samples given to OpenAL; 47.6 MB of
    sound buffers and 0.02 to 0.03 s more at the load. The sky's faces
    outside the view were tried and left out (1.5 µs a frame; the sky no
    longer covered the cracks between faces where it did before).

31. The sound system (2026-10-23, see Performance, Sound system, and
    Implementation notes, Sound system; the engine's client, both
    renderers): JayRewd's pull request #2 (the Omaha mixer: miniaudio,
    HRTF with libmysofa and PFFFT, OpenAL dropped from the build and the
    CI) merged into the branch, its conflicts with Plans 29 and 30 resolved
    (the OpenAL host's soundtrack cache and load-time buffers moved to the
    new host), then measured with temporary builds that timed the sound
    host's calls in each frame: its obstruction traces (every playing world
    sound every 100 ms, the loops started at a load due in the same frames),
    a walk of every global alias at each sound's first play and at each
    start of a streamed taunt (0.2 to 0.9 ms), and its streams waiting for
    their worker in the frame (0.1 to 0.16 ms at a song's start, seek or
    loop). Improved without changing the mixed output (the user, 2026-10-23:
    its filters, levels and 3D space are finely tuned): the streams never
    wait for the worker, the soundtrack's mp3s read, scanned and their head
    decoded when it loads (`snd_stream_memory.cpp`, Plan 29's stream thread
    gone; it lost 30 ms at every loop of a song with the pull request's
    reads), the aliases read once per registration and every registered
    sound classified and its pain lines loaded at the load; a lost device
    found by its callbacks stopping, no command for a freed voice id, the
    HRTF set read from the client's folder, `playmp3`'s crash (a GCC
    devirtualization) and the mp3 codec's memory fixed, `s_khz 48`, the
    unused capture path removed, the Windows package with the HRTF set and
    notices. The obstruction traces stay as they are; a budget for them
    (`s_obstructionBudget`, off) was a proposal, removed once they were
    timed at real speed. Then (the
    user's OK): headphones mix at 48 kHz, where the baked HRTF filters pass
    the pull request's check (at 44.1 kHz every voice's filters were
    designed on the mixer thread), and filters designed from the
    measurements are made on a thread of their own (bit-identical to the
    merged code's from 10 ms after a voice starts or stops turning).
    Against OpenAL (5 runs, 1920x1080): the pull
    request costs `o2match` 0.06 ms of mean frame time and 0.2 to 0.3 ms at
    the 99th and 99.9th percentiles uncapped, `o4match` 0.02 and 0.07 ms;
    Plan 31 takes 0.05 and 0.12 ms off their 99.9th percentiles, the budget
    the rest of `o2match`'s; input to present at 250 within the runs'
    spread. Most of the pull request's cost there came from the playback's
    speed (the sounds play in real time, so each stays playing 4 to 30
    times longer in a match played back faster): at real speed its
    obstruction traces take 1.5 to 1.7 µs a frame, 0.05 to 0.14 ms in the
    slowest 0.1 % (the pull request's author had less than a quarter);
    the same stream chunks, seeks and PCM as the pull request (its
    loops' 30 ms loss fixed), ThreadSanitizer clean but RADV's own report,
    GL1 plays with the new engine.

32. The surface list (2026-10-23, see Performance, Surface list, and
    Implementation notes, CPU stages and skeletal meshes; the renderer):
    JayRewd's pull request #3 ("cut beSurfs CPU ~69µs on outdoor scenes",
    measured on its author's benchmark with per-surface profiling timers
    that aren't part of it) merged into the branch, its one conflict
    resolved (dynamic light lines that Plan 24 had removed), then measured
    with temporary counting and timing builds in `o2match` and `o4match`
    and reworked. Kept: a stage drawn on the CPU takes its texture
    coordinates from tess when they are the mesh's or the lightmap's, a
    stage with one color for all its vertexes (and the lightmapped
    multitexture iterator's white) gets it as the constant color,
    `RB_SkelMesh` finds a surface's mesh in a table, the deforms are only
    called for a shader that has some. Fixed: the table is cleared with
    the skin data at each map load (it was never cleared, so freed
    models' pointers stayed in it, and once its 1024 entries were full
    every new surface's lookup probed all of them) and filled at a
    surface's first lookup instead of by the front end for every surface
    of every model in every frame; `sCoord` and `tCoord` stages, which
    read the copied texture coordinates, get the copy; the constant
    colors' pipelines are built at the load (`tr_pipelines.c`: the pull
    request made 2 and 4 pipelines in the two matches' frames when nothing
    was recorded). Left out: the cache of the entities' lighting setup (6
    to 11 hits a frame, all but 2 to 4 where a cached value was taken
    anyway; kept in static variables, it carried a setup from one draw
    list to the next, twice in `o2match`), the loop for the surfaces of
    one sort (the same work as the list's own loop; its gain was the
    author's timers), the canary checks limited to a nearly full tess,
    and edits that change nothing. The code it changes takes 1.5 and 2.2
    µs less a frame (0.9 to 1.6 µs in the stages, 0.15 to 0.4 µs in the
    mesh lookups, 0.26 to 0.43 µs in the upload); the frame times and
    input to present of the merged pull request and of the final build
    are within the runs' spread of the build before's, uncapped and at
    250. The same frames as the build before, no validation message.

33. No OpenGL, `cl_movefps` and no `m_filter` (2026-10-23, the user's
    requests, to match the `metal` branch; see Status, Implementation
    notes, No OpenGL, and Testing): upstream's GL1 and GL2 renderers,
    their SDL glue, build options and platform (Emscripten) and the GL
    code that was never built deleted, the GL-era names of this renderer
    renamed, `cl_renderer` only `vulkan` (an old config's `opengl1` is
    reset), and the tools compare builds with each other (`compare.sh`,
    `cmplists.sh`) and check that no OpenGL comes back (`glcheck.sh`);
    the command rate's cvar is `cl_movefps`, `metal`'s name (its behavior
    unchanged: always on, 85 to 125 a second; `metal`'s `0`, one command
    per rendered frame, followed on 2026-10-24 at the user's word, see
    Performance, Command rate); `m_filter` removed. The same frames as
    the build before, no validation message.

34. `r_fastSync` (2026-10-24, see Status and Performance, Present timing;
    the renderer): JayRewd's pull request #1 on MoHAA-Reloaded
    ("r_fastSync: prefer mailbox on Wayland when uncapped", against an
    acquire stall of immediate, 0.2 ms a frame in fullscreen, that its
    author saw on Wayland with NVIDIA) merged without conflicts, then
    measured and reworked. Kept: the latched, archived `r_fastSync`
    (default 0), mailbox first and immediate as its fallback when
    `r_swapInterval` is 0. Changed: no test of SDL's video driver (the
    pull request applied it to native Wayland windows only, so under SDL
    2's default on a Wayland desktop, Xwayland, `r_fastSync 1` did
    nothing, while mailbox works the same there, on X11 and on Windows),
    the choice in two lines, a description that says what it does and
    costs. Measured: no tearing for 4 to 6 ms more input to display at the
    60 Hz display; with RADV mailbox's acquire after the present waits up
    to 1 ms in 5 to 17 % of the frames, and immediate has no stall; a
    fifth swapchain image for mailbox took that wait away but added 0.15
    to 0.24 ms of input to display on native Wayland at 1000 frames a
    second and more, so it was left out. The same frames as the build
    before, no validation message.

35. SDL 3 (2026-10-24, the user's request; see Status, Implementation
    notes, SDL 3, Performance, Present timing, and Testing): the client's
    window, input and events (`code/sdl`), the system layer (`code/sys`:
    version check, `WinMain` from `SDL_main.h`, library loading, CPU
    features), the renderer's Vulkan loading, surface, drawable size and
    submit thread, the Omaha mixer's HRTF semaphore, CMake (the shared
    `SDL3::SDL3`, `RUNPATH $ORIGIN` for the package's `libSDL3.so.0`), the
    vendored headers and Windows libraries (SDL 3.4.18), CI (SDL 3.4.18
    from its tag; the Linux build on an x86-64 runner so that SDL has its
    X11 and Wayland drivers, checked), the Windows installer, `update-libs.sh`
    and `libinlat.so` moved from SDL 2.32 to SDL 3; the same day macOS left
    this branch at the user's word (the `metal` branch is the macOS port).
    Each lower-latency API of SDL 3 weighed: native Wayland (taken: no
    Xwayland between KWin and the game), the event pump's sentinel and
    Windows' raw input thread (SDL's defaults, taken), raw keyboard on
    Windows and event timestamps (not taken: nothing read sooner, and
    timestamps would change movement), `SDL_GetTicksNS` and precise delays
    (no SDL timing in the frame limiter). SDL 3 has no gamma ramps:
    `r_gamma` goes to the textures on Linux, Windows keeps the ramp. The
    same frames as the build before, no validation message; input to
    display not longer uncapped or at 250, input to present at 250 0.005
    ms longer on the median from the later read inside SDL 3's pump.
36. SDL 3's event pump (2026-10-24, the user's request: "check if u can fix
    the SDL3 regression"; see Status, Implementation notes, SDL 3, and
    Performance, Present timing): Plan 35's 5 µs more input to present at
    250, taken apart inside a copy of SDL 3.4.18: SDL's D-Bus check, the
    dispatch of what its cursor thread read (mostly KWin's frame callback)
    and the flush of SDL's answer, all before the socket's read, and udev
    and joystick polls after it. Engine only, stock SDL: the frame limiter
    pumps SDL's events once when its polling starts after a sleep
    (`IN_PumpEvents`, a stub for the dedicated server), and `in_joystick 0`
    shuts SDL's joystick subsystem down. The frame's pump at 250: 3.1 µs
    on the median against SDL 2's 7.8 to 8.2 and Plan 35's 14.1 to 14.6.
    Input to present at 250 0.007 ms shorter than SDL 2's on the median
    and 0.009 at the 90th percentile, the same at the 99th and 99.9th
    within the noise; uncapped and input to display the same or lower.
    Not taken: a patch of SDL that checks D-Bus every 100 ms (1.2 µs more;
    CI builds SDL from its tag and would have to patch it).
37. The internet server browser (2026-10-24, JayRewd's pull request #3 on
    MoHAA-Reloaded, "pipelined server discovery so the browser fills
    faster", merged as a654b3bc; see Status and Testing): kept the
    queries that start while the masters still send, 32 queries in flight
    shared by the lists, a first query of 0.4 s and a second for a server
    that didn't answer, the scheduler (`gserverlist_scheduler.c`) and its
    unit test, the answered and timed-out counts in the status line, and
    the pull request's fixes of the code before it (UDP sockets leaked at
    every master connection, master sockets that started as socket 0,
    the filter's termination, sockets left open by `ServerListFree`).
    Changed: the masters' names resolved on threads of the GameSpy SDK
    (`gsiStartResolvingHostname`, three fixes in it: IPv4 only, 0.0.0.0
    an error, no read of the freed handle at the thread's end), a list
    idle only once its lookups ended (the SDK can't cancel one safely), a
    master skipped when it can't be resolved or connected, 3 s for a
    master to connect and send its challenge, 1 s for the second query
    (3 s kept the list busy for 9 to 12 s after the last answer with ~95
    silent servers), the done test without the count of completed
    queries (an update of one server on an idle list never ended), LAN
    updates without masters and with a timestamp on their broadcast
    socket (the master timeout closed it at once). Removed: the setters
    of master concurrency, timeouts and pipelining, four callback
    messages, `cl_browserDebug`, clamps of constants, the pause of the
    queries while masters send. `cmake/tests/all.cmake` is included
    (upstream doesn't): CI's Unit Tests run `test_lz77` and
    `test_serverlist_scheduler`.
38. Shots and jumps on the sub-grid (2026-10-24, the user's request:
    "merging the subgrid thing from the metal port"; see Status and
    Performance, Command rate): `metal`'s e38910e4 (`cl_movesubgrid`,
    default 1) ported to this branch's grid, whose usercmds
    `CL_CreateNewCommands` makes (`metal` has a grid function of its own
    and a frame of its own at each grid point, `CL_CmdClock`; here the
    first frame at the half step makes the early command, as for the
    grid's points). Kept as on `metal`: the split (the grid goes on after
    the early command), shots and jumps only, one early command a step,
    the early command and the split's rest sent at once, the open step of
    the shorter length. Changed: a jump is early by the click's rule (SDL
    stamps a key event with the frame before's input read, so the command
    of the frame that sees it has a share of the key; `metal`'s rule left
    seven of eight jumps on the grid at 250 frames a second here); the
    state is one struct reset in one place (`cl_movefps 0`, before the
    first snapshot, a grid started again); `metal`'s count of the events
    seen in a step is gone (an event seen too late in a step stays too
    late); the open step's length goes to cgame in the live command's
    time instead of cgame's own copy of the interval (an engine without
    it gives the interval as before); the packet sent at once only while
    its command is unsent. Measured at 250, 1000 and 125 frames a second
    against a local dedicated server; the stop rate at `dm/mohdm2`'s bumps
    with a sweep of the server's own moves (temporary builds, not kept).
39. The crosshair (2026-10-24, the user's request: "merging ... the
    crosshair"; see Status and Implementation notes, Crosshair): `metal`'s
    bccff6f9, the crosshair drawn as four boxes in one draw, the stock
    image's shape by default. The cgame code is `metal`'s; the renderer's
    `DrawBoxes` converts the color as the 2D color is converted, the
    renderer's API is version 15 (an older `renderer_vulkan.so` is refused
    at startup instead of calling a missing export), the frame log and
    `renderer_timed.so` wrap the new export, and the load's registration
    of the two crosshair images is gone. The frames differ from the build
    before in the stock image's four half-dark pixels only; every size and
    setting tried gives the expected pixels, symmetric; frame times the
    same, cgame 0.2 to 0.3 µs less a frame.
40. `cl_maxpackets` by the commands' own time (2026-10-24, the user's go
    after Plan 38's press test found the waits; see Status and
    Performance, Command rate): with `cl_movefps` on, a new command's
    packet waits by the commands' server time, as `metal`'s b5112fb9 does;
    unlike `metal`'s, a packet without a new command keeps counting real
    time (by server time it would wait for the next command). At 144
    frames a second half of all commands no longer wait 7 ms at the
    defaults, at 250 with `cl_movefps 125` half or more no longer wait 4
    ms; at 250 with `cl_movefps 100` nothing changes.
41. The temporal path (2026-10-26, the user's request: motion vectors,
    the last frame's color and depth, a jitter sequence, a depth pyramid
    and normals, for ambient occlusion and anti-aliasing later; see
    Status and Implementation notes, Temporal path): `r_temporal 1`. With
    it off, the views of `mohdm1`, `mp_mohdm1` (skinned soldiers),
    `mohdm2_fog`, `m2l1_skyportal` and `mohdm1_dlight` are the build
    before's to the bit (the few pixels of `m1_spawn` and the sky portal's
    `s2_a` and `s2_b` that differed in one run differ between two runs of
    the build before as well). With it on: no motion with a still camera,
    sideways when turning, parallax when strafing, outwards when moving
    forward, the running soldiers' limbs on their own, the sky by the
    camera's rotation; `r_temporalShow` 2 to 6 look right. No validation
    message (the validation layer of Steam's runtime, with `r_vkFallbacks
    8`, which it needs) through map loads, `vid_restart`, a switch to
    multisampling and back and the soldiers' scene, and none of
    synchronization validation but 7 `SYNC-HAZARD-WRITE-AFTER-READ` on the
    skin data's uploads that the build before has too. Not done: the
    effects shader's deforms and CPU deforms have no motion of their own,
    and nothing measured on AMD yet.
    A review the same day (the user's request): render passes compatible
    only with identical dependencies (the clearing one had its own,
    caught by validation), a second primary view no longer replaces the
    first's projection, skinned frame data made outside the scene is made
    again inside it (its shaders would read the last frame's bones from
    the wrong words), sprites write no stale normals, the view weapon's
    view distance by its own depth range, `VK_PIPE_NORMALS` only with
    `VK_PIPE_TEMPORAL`, no duplicated helpers (`VK_CreateImage`,
    `VK_FormatHasFeatures`, `VK_CreateShaderModule` shared). Performance:
    blended passes skip the temporal work (specialization constant), the
    normals are copied for the writing pass only, one fused full-size
    pass and two-level and tail dispatches (4 instead of 12 at 1920x1080,
    the full-size `R32G32` level replaced by an `R32` view distance and a
    half-size level 0), static descriptor sets (one set written a frame
    instead of about 90 descriptors), the scene cleared by its render pass,
    one barrier call per pass, the two frame slots' skin buffers in the ring
    (4 MB less host-visible memory). GPU time 0.307 to 0.279 ms, renderer
    time 0.335 to 0.294 ms at 1920x1080; the views as before, parity with
    `r_temporal 0` and validation as before (`mohdm4_decals` added).
    Then the user's view of `dm/mohdm6` (668 -355 -24, 10 -137 0), from
    10700 to 3300 frames a second at 2560x1440 (GPU time 0.043 to 0.259
    ms; the pass over every pixel 0.106 ms): the motion vectors only from
    what moves, the camera's and the missing normals worked out by the
    readers (`temporal_common.glsl`), so the fill went and the scene's
    images are no storage images any more; the full-size view distances
    went (the readers have the depth buffer, the history is the pyramid's
    level 0); the pyramid's first pass an invocation per 2 x 2 pixels with
    a gather and shared memory for level 1; the pyramid only when a reader
    needs it. 5840 frames a second, GPU time 0.121 ms (0.161 with the
    pyramid); at 1920x1080 0.143 to 0.07 ms. Parity, validation and the
    views as before.
42. Temporal anti-aliasing (2026-10-26, the user's request: TAA for better
    anti-aliasing; see Status and Implementation notes, Temporal
    anti-aliasing): `r_taa 1`, `r_taaFeedback`, `r_taaSharpen`,
    `r_temporalShow 8`. With `r_temporal 0`, and with `r_temporal 1` and
    `r_taa 0`, the views of `mohdm6` are the build before's to the bit.
    With it on the edges of the views (roof and ceiling edges, ladders,
    railings) lose their steps and the textures keep their detail; no
    ghost or trail in frames taken while strafing past a railing and
    turning at 400 degrees a second. No validation message with
    synchronization validation (`r_vkValidation 2`) through the `mohdm6`
    views, `r_taa` switched off and on, a `vid_restart`, `r_temporalShow 8`
    and `r_taaSharpen` 0 and 1. GPU time in the user's `dm/mohdm6` view
    0.121 to 0.36 ms at 2560x1440, 0.069 to 0.208 ms at 1920x1080 (see
    Implementation notes, Temporal anti-aliasing, Cost). Not checked: the
    view weapon's animation and moving players (no weapon in the scripted
    runs, no match demo on this machine), AMD.
    The same day (the user's report: below 250 frames a second the image
    jittered): this frame's color made at the pixel's center from its 3 x 3
    and the depth test over 4 x 4 pixels (see Implementation notes, Temporal
    anti-aliasing, Shimmer): the pixels that change between frames of a
    still view from 1.16 to 0.39 %, with the user's `r_taaFeedback 0.5`
    and `r_taaSharpen 0.8` from 12.2 to 2.29 %. No ghost while strafing or
    turning, the edges as smooth, no validation message with
    synchronization validation through the toggle run; 0.03 ms more.
    Then the user's defaults: `r_taaFeedback` 0.75 and `r_taaSharpen` 0.8
    (from 0.9 and 0.3; fine at 60 frames a second, good at 125, excellent
    at 250 by the user's eye): 1.25 % of the pixels of the still view
    change between frames (0.16 % by more than 16).
    Then its optimization (the user's request: 87.5 % of the frames a
    second lost with the path and the anti-aliasing on; at first without a
    loss of quality, then a very minor one allowed): measured part by part
    (see Implementation notes, Temporal anti-aliasing, Cost): the scene in
    the frame's 8-bit format, the history in 10 bits, the filter's weights
    from the CPU, one reprojection, level 0 of the pyramid made by the pass,
    two barrier calls. GPU time at 2560x1440 0.388 to 0.286 ms (0.096
    without the anti-aliasing, from 0.122), at 1920x1080 0.227 to 0.164
    (0.059 from 0.068); with the path off the same. The two formats were
    taken out again in the next commit (the user's choice, for HDR to come:
    they clip at 1): 0.344 ms at 2560x1440, 0.185 at 1920x1080, the
    temporal path 0.123; the frames those of the optimization without them
    (the same to the bit with the jitter on one position), shimmer 1.25 %
    as before, no validation message with synchronization validation
    through the same run. What follows describes the commit with them. With `r_temporal 0` and
    with `r_temporal 1` and `r_taa 0` the views of `mohdm6`, `mohdm1`,
    `mp_mohdm1`, `m2l1_skyportal` and `mohdm4_decals` are those of the
    build before without the path to the bit (but for pixels that differ
    between two runs of it: animated content); with the anti-aliasing the
    shimmer of the still view from 1.25 to 1.44 % of the pixels (0.16 %
    by more than 16 as before) and changes by one in a third to half of the
    pixels, no shift; level 0 made by the pass gives the frames of the
    pyramid's to the bit, also at 1001x601. No validation message with
    synchronization validation through `r_temporalShow` 4 and 7 (the
    pyramid beside the anti-aliasing), `r_taa` off and on, a `vid_restart`,
    at 640x480 and 1001x601.
44. Ambient occlusion (2026-10-27, the user's request: an implementation
    of AAA studio quality ground truth ambient occlusion; see Status and
    Implementation notes, Ambient occlusion): `r_ao 1` and its cvars,
    `r_temporalShow` 9 and 10. With `r_temporal 0`, with `r_temporal 1`
    and with `r_taa 1` (`r_temporalJitterSamples 1`), `r_ao 0`, the views
    of `mohdm6` are the build before's to the bit (`d6_sky` differs in 28
    pixels between two runs of the build before too). With it on, in
    `mohdm6`, `mohdm4_fog` and `hud` (the view weapon drawn with
    `cg_drawviewmodel 2`): corners, the lockers, desks, crates and barrels
    on the floor occluded, the lamps' glows not darkened, nothing in the
    fogged distance, no halo around the weapon (its own creases only).
    The SPIR-V of `vk_shaders_temporal.c` from libshaderc (the same
    compiler that gave the committed arrays to the byte). Not checked:
    validation (no validation layer on this machine), motion (ghosting
    behind moving players, turning fast) in a running game, AMD.
    The same day (the user's report: a wall right in front of the eye
    went black, otherwise it looked great): levels 2 to 4 of the view
    distances were read from the wrong shared memory (a step of 2 where 1
    was due) and stayed 0: every coarse sample was a wall at the eye,
    which fully occluded surfaces nearer than the radius. Fixed, with the
    coarse levels the mean of 1 / d sampled at their texels' centers,
    samples off the view left out, and the view distances in R32F again.
    Walking into a wall of `mohdm6` (8 steps of 40 units to touching it)
    the wall stays white in `r_temporalShow 9`; the room's look at the
    defaults is as before. GPU time at quality 2 0.84 to 0.96 ms at
    2560x1440, 0.47 to 0.53 at 1920x1080.
    Then (the user's report: walking into a wall or a railing, the
    occlusion wobbled, the noise moving with the strafe): the camera of a
    player pushing into a wall moves by a ten-thousandth of a unit in most
    frames (prediction), the depth is rounded anew, and the blur's edges,
    packed in 2 bits as XeGTAO's, flipped by a third where they were near a
    step. Reproduced in a scripted run launched as the user plays (their
    config at startup, fullscreen, 2 s of forward and right into the corner
    of the `mohdm6` boiler room, then 2 s still): pixels of
    `r_temporalShow 9` changing by 16 or more between captures 200 ms
    apart, 35000 pushing and 20000 still before, 86 and 47 with the edges
    worked out unquantized in the blur from the view distances; 0.01 ms
    more GPU time.
    The wobble wasn't gone (the user's test in the game: the noise
    streaked, upward with forward and right, to the left with forward;
    the user's screenshots showed it): the history was read with weights
    for one 2 x 2 texels from a gather whose texels the hardware picks
    from the coordinate with 8 bits below the texel; under a thousandth
    of a pixel's motion the gather came back a texel further, and the
    history moved a pixel a frame along the sign of that motion, which the
    movement keys and the view angles set. The texels are picked in the
    shader now.
    Then a review and optimization (the user's request; up to 1 to 2 % of
    the look for a meaningful gain, as long as it stays stable): the main
    pass's slices and steps as specialization constants (a pipeline per
    quality: its loops unroll, 0.49 to 0.43 ms), its view positions and
    slice directions worked out once (no change), the blend over frames
    and the blur in one pass with shared memory (0.28 to 0.17 ms), the
    blur and the result shared (`AoBlur`, `AoResult`). Quality 2 0.82 to
    0.69 ms over the path at 2560x1440 (quality 0 to 3: 0.43, 0.51, 0.69,
    1.62), 0.45 to 0.38 at 1920x1080 (0.23, 0.28, 0.38, 0.80). With
    `r_ao 0`, with and without the path and with the anti-aliasing, the
    views of `mohdm6` are the build before's to the bit; with it on, the
    frames of the build before the review but for one level in 0.7 to
    3.6 % of the pixels (the occlusion: one level in 3 to 15 %, a few
    pixels up to 22 through the history), the two blurs' path (quality 3,
    `r_aoTemporal 1`) one level at most. Fewer samples at quality 2 were
    measured and left (see Implementation notes, Ambient occlusion, Cost):
    2 slices of 3 steps keep more noise in 4 to 12 % of the pixels.
45. The temporal path's images per reader (2026-10-27, the user's
    request: every part of the temporal path on only while something that
    depends on it is; see Status and Implementation notes, Temporal path,
    Images per reader): the anti-aliasing's history, the depth pyramid,
    the ambient occlusion's images and the last scene's color made and
    freed with the cvars that read them, the static sets pointing at a
    placeholder in between. The frames of the build before to the bit
    (`mohdm6` with the path alone, with the anti-aliasing, the ambient
    occlusion, both, and `r_temporalShow` 4, 5, 9; the views that differed
    in one run differ between two runs of the build before as well), the
    cvars toggled live without a hitch beyond the GPU wait, the GPU's
    memory following them.
46. Motion blur (2026-10-27, the user's request: a velocity motion blur
    from the motion vector image and the depth; see Status and
    Implementation notes, Motion blur): `r_motionBlur`, the user's
    choices: the camera's motion too, the same exposure at any frame rate,
    the view weapon by its own motion only. With it off, and on in still
    views, the views of `mohdm6` are the build before's to the bit, also
    with the anti-aliasing; in motion as described there.
47. Depth of field (2026-10-29, the user's request: an implementation of
    depth of field; see Status and Implementation notes, Depth of
    field): `r_dof`, the user's choices: autofocus at the crosshair, a
    bokeh gather, Vulkan only (the metal port can follow). With it off
    the views of `mohdm6` are the build before's to the bit, also with the
    anti-aliasing, and with it on so is what is in focus. A review pass
    (2026-10-30): the scope's blur by its zoom, not its square; the focus
    from the primary view, the sky counted as far; the sharpening back in
    the composite; the half resolution's color weighted by blur.

Remaining:

- What makes the slow frames, in order (see Performance, Frame spikes;
  frame times in two matches played back, 1920x1080 unless named):
  1. Registration in the middle of a match: done (2026-10-22, Plan 28):
     cgame registers at the load what a multiplayer match announces or
     draws later, and the file system's lookups in the search directories
     use listings. Left in 21 real matches: the water sounds of the body
     (0.2 to 0.3 ms), and
     cgame's init commands of each new effect (0.1 ms each, 2 ms in a frame
     with many crate debris). The first frame after a load still has 0.9
     ms of registration from the engine's UI (the HUD). The one load of
     9.4 ms that was left (`obj/obj_team1`'s flak wreck and its explosion,
     named in the stock map script, which a mod pk3 on this machine,
     `z_forteffa.pk3`, replaced for the client) is gone: the mod was
     removed from this machine's game data on 2026-10-23, and in one traced
     run of that match the wreck (6.9 ms) and the explosion (1.4 ms) load
     with the map and no frame has 0.3 ms or more of registration. What a
     server's own scripts or mods spawn that the client's files don't name
     stays a load in the match. Not done, all of
     them for the load now: (b) a faster inflate for the pk3 reads
     (zlib-ng's or libdeflate's decompression, the same bytes, reported
     about twice as fast as stock zlib, 48 % of the registration's time;
     a new dependency: the user skipped it 2026-10-23, map loads are
     fine), (d) the mipmaps on the GPU (a
     compute shader doing `R_MipMap`'s integer box filter, 10 %), (e) a
     cache of decoded images (disk space). The registration's time,
     measured 2026-10-18 (see Performance, Pipelines and skin data at
     load): zlib 48 %, the file system's lookups 17 % and `FS_CorrectCase`
     11 % (the listings take most of these two off), mipmaps and scaling
     10 %, TIKI files 7.5 %. A list of what each map loaded in the match,
     kept from play to play and registered at its next load (like
     `vkpipelines.keys`), would cover what no file names; not done.
  2. Pipelines on first use, with cold caches: done (2026-10-18, Plan 16):
     the map's pipelines are created at load (0.18 to 0.20 s more map load
     with cold caches, the same with warm ones), 2 are left in the match
     for the first play after an install (the head icon sprite, registered
     in the first frame), none after a driver update or with warm caches
     (`vkpipelines.keys`). What a first play can still create: mirror and
     portal views (cull swapped, clip plane), debug drawing, stencil
     shadows, `r_measureOverdraw`, immediate mode draws other than the UI's
     boxes and the clear, shaders whose state combination no load shader
     has. Untested where pipelines compile more slowly than on RADV
     (NVIDIA, MoltenVK), where the load would grow more and the gain too.
  3. The busier moments of a match on the CPU: done (2026-10-23, Plan 30):
     profiled again, the terrain's walks leave out what changes nothing
     (`r_terrainCache`) and the sounds' buffers are made at the load
     (`s_loadBuffers`): `o4match`'s 99th percentile 1.52 to 1.37 ms
     uncapped, the mean 70 µs lower, input to present lower at every
     percentile; `o2match` 0.03 to 0.06 ms lower. What is left in the slow frames
     (see Performance, Busy moments): the terrain's splits after a large
     turn of the view (about 1000 in the slowest 0.1 % of `o4match`'s
     frames, 0.3 ms: an exact speedup would need the triangles of a patch
     close together in memory, not a free list's order), the back end's
     draws and the world's leaves (they grow with what is in view; the
     world culling of Plan 14 took what it found exact), cgame's events of
     the shots (the event lookups by name about 15 µs of `o2match`'s slow
     frames; the traces, marks and sounds are the game's work) and the
     init commands of a new effect (0.1 ms each; e.g. keep each model's
     parsed commands), cgame's entities' animation, and JayRewd's exact
     early rejects in the mark fragments (`R_MarkFragments`, 0.017 ms of
     `o2match`'s slow frames, of which they save part); the sky's faces
     outside the view (1.5 µs a frame) only with a sky box that still
     covers the cracks between the faces in the view.
  4. CPU-side frame pacing where the GPU limits (3840x2160 fixed views;
     with `r_vkSubmitThread 1` the wait comes at the end of the frame,
     before the next input instead, see below):
     Vulkan's frame times spread more than GL1's (99th percentile 0.2 to
     0.4 ms higher) while the GPU finishes its frames as evenly as GL1's:
     the fence wait comes before cgame and the front end in the frame
     (the first UI widget's `set2D()` runs `RB_DrawBuffer`), so their
     noise isn't absorbed in the same frame as with GL1's wait in the
     swap; and the acquire after the present waits 0.6 to 1 ms in up to 4
     % of the frames under Xwayland. Starting the backend's frame (and its
     wait) at the frame's first recorded command instead of in
     `RB_DrawBuffer` would put the wait after the CPU work like GL1's,
     with the same latency (the frame is still submitted when the GPU
     frees its slot): to be tried and measured, with the input to present
     time checked. Tools that report frame times from the CPU's side
     (present to present) see this spread; what is displayed is even.
  5. Round changes: done (2026-10-23, Plan 29): the soundtrack's mp3s are
     decoded on a thread of the sound system (since Plan 31 on the Omaha
     mixer's worker) and the configstrings change in place. Their frames take 1.0 to 1.4 ms less uncapped (`o4match`
     1.9 to 2.4 ms, below the first frame after the load; 2.6 to 3.0 ms
     less with a 3 minute song) and their input to present 0.5 to 0.7 ms
     less at 250. What is left in those frames is cgame's
     server restart (`CG_ServerRestarted`, 0.15 ms: the server info parsed
     twice, the marks cleared), its 84 configstrings (0.12 ms) and the
     first view at the new spawn; the first frame after a load (4.6 ms in
     `o4match`, the 0.9 ms of the engine UI's HUD registrations in it) and
     the body's water sounds (0.2 to 0.3 ms, in 2 of 21 matches) were not
     done (optional).
  6. Where the GPU limits, the GPU-heavy moments (the fence wait is 37 to
     53 % of the slow frames at 3840x2160): GPU time, see the texture
     compression item below.

- The sound system (Plan 31, see Implementation notes and Performance,
  Sound system). The obstruction traces (every playing world sound traces
  3 rays every 100 ms) looked like its largest main-thread cost in the
  matches played back faster than real time (`o2match`: 109 thousand
  traces, 0.68 ms in the slowest 0.1 % of frames), where every sound stays
  playing 4 to 30 times longer; at real speed they take 1.5 to 1.7 µs a
  frame and 0.05 to 0.14 ms in the slowest 0.1 % (see Performance, Sound
  system), so the budget for them (`s_obstructionBudget`) was removed.
  Sound-system costs in a frame are to be timed at real speed (`--caps
  250 --exec 'set fixedtime 4'`, a quarter of the frames in the demo
  list).
  Not done (no change to the sound): a lost device is closed and opened
  again on the main thread (an `ma_context_init` and a device start, tens
  of ms, once a second while it stays lost); the null device that a start
  without a device falls back to is kept until `snd_restart`; a stream's
  file is still opened on the main thread (a pk3 handle of its own: 0.04
  to 0.14 ms for a dialog line or a plane's fly-by); a freed sound makes
  the mixer's callback walk every voice (some hundred thousand checks at a
  map load, on the device's thread); the HRTF set's file is 12.5 MB, 6.4
  MB re-saved without its HDF5 chunks' padding (a changed file under
  Apache 2.0, and git keeps the old one); the obstruction traces stay on
  the main thread (the collision model's trace isn't thread-safe:
  `cm.checkcount`). Untested: Windows (WASAPI; the package and installer
  changes too), macOS (Core Audio, the `.app` layout), emscripten (no
  threads: the streams decode on the main thread there). The device's
  period on this machine's PulseAudio is 15 ms (5 ms x 3 requested); the
  audio latency against OpenAL's was not measured.
- The surface list (Plan 32, see Performance, Surface list): not measured
  on the pull request's author's hardware or outdoor demo, where its 69 µs
  were measured with profiling timers. Left in the code:
  `tess.constantColor255` (8 KB of tess) is only read by `RB_StaticDraw`'s
  case of a draw with a color array that isn't the stage's, which no
  iterator makes any more (removing it moves tess's other arrays: to be
  timed); the back end tests `RDF_HUD` in the front end's `tr.refdef` (the
  last scene the front end took), not the drawn list's `backEnd.refdef`, as
  GL1 does, and computes the inventory's item models' sphere for each of
  their surfaces.
- `r_fastSync` (Plan 34, see Performance, Present timing): not measured on
  NVIDIA, where its author saw immediate's acquire stall (runs with
  `libinlat.so`, `INLAT_DISPLAY=1` and `r_vkFrameLog 1`, `r_fastSync` 0
  and 1, would show that stall and what mailbox costs there), on Windows
  or on a 240 Hz display. Mailbox's acquire after the present waits up to
  1 ms at times on RADV under KWin; a fifth swapchain image for mailbox
  takes that away for 0.15 to 0.24 ms more input to display on native
  Wayland at 1000 frames a second and more (none at 250): not done, as it
  adds input lag there.
- The submit thread (see Performance, Submit thread): not measured on
  NVIDIA, where its gain was reported (a tester's input to display runs
  with `libinlat.so` and `INLAT_DISPLAY=1`, the thread on and off, would
  show whether the hand-off costs anything there), nor on Windows (see
  [WINDOWS_TESTING.md](WINDOWS_TESTING.md)) or MoltenVK. Its wait for the
  GPU at the end of the frame, before the next input, took 1.0 to 1.2 ms
  of input to GPU done off where the GPU limits, at the cost of frame
  rate where CPU and GPU are about even (the engine's work between frames
  no longer overlaps the GPU); the same wait without the thread is a
  candidate, the opposite of item 4 above (which moves the wait later,
  for even frame starts). Off by default: at 250 frames a second it buys
  nothing measurable here, uncapped it is the faster mode.
- Present timing (see Performance, Present timing), pacing changes not
  made, each lowering input to display without adding lag (the waits come
  before the input read): (1) with FIFO, wait before the input read until
  the previous present has completed (`vkWaitForPresentKHR` on the main
  thread), so at most one frame is queued: 38 to 59 ms less uncapped at
  60 Hz, and no queue left at a cap below the refresh; (2) with vsync
  (mailbox, a window, FIFO), start each frame just in time for the
  compositor's latch: up to the measured slack, 0.9 to 1.4 ms uncapped,
  2.3 to 2.7 ms at 250 and 7.5 to 9.9 ms at 59 and 60, but KWin doesn't
  report its latch (3.3 to 3.9 ms before the vblank here, adaptive) and a
  frame that misses it waits a whole refresh; an engine change (when
  `Com_Frame` starts a frame). With tearing (fullscreen immediate, the
  lowest: 1.5 ms uncapped, 2.1 at 250) the slack is under 0.5 ms: only
  less CPU and GPU time per frame lowers it. Not measured: a VRR or 240 Hz
  display, other compositors (GNOME's, wlroots), NVIDIA.
- The engine's error dialog hung games, with either renderer (fixed in
  `code/sys/sys_unix.c`; upstream code, the same in ioq3 and openmohaa
  today): `Sys_Exec` forked and called `exit(-1)` in the child when
  `execvp` failed (no `zenity`: KDE without it), so the child ran the
  parent's exit handlers, and OpenAL Soft's waits forever to join
  PipeWire's event thread, which the child doesn't have (backtrace of the
  child under gdb; not Mesa); the parent waits for the child. The child
  now calls only `execvp` and `_exit(127)`, and the parent waits for that
  pid and counts 127 or a killed program as no dialog. That made
  `Sys_Dialog`'s fallback work for the first time (the failed child's 255
  counted as an answer, so kdialog and xmessage were never tried), and the
  fallback then showed `--title=Error` in place of the message: the zenity
  command's two `va()` calls overwrote the caller's `va()` message (two
  buffers); the command builders no longer use `va()`. The hang needs the
  sound still up when the dialog starts: a plain fatal error shuts the
  client down first, a recursive error (one raised while an error is
  handled) goes straight to `Sys_Error`. Tested with a temporary
  `error sys` command calling `Sys_Error` with the renderer and sound up,
  `cl_renderer vulkan` and `opengl1`, no zenity: the old build hangs
  (parent in `do_wait`, child in `futex_do_wait`), the new one shows
  kdialog with the message, then xmessage when kdialog is killed, and
  exits (status 3, crash log written) when the dialog closes or when no
  dialog program is on the `PATH`; the plain `error` command the same.
- The frame limiter (see Performance, Frame limiter): at caps of 60 and
  144 the tail of the input to present time is up to 0.25 ms longer than
  with the old limiter's 62.5 and 166.7 frames per second, after
  `BeginFrame` (renderer and driver), and the new limiter at those old
  rates has the old tails: likely the frame rate against the 59.96 Hz
  display, untested with a VRR display. Untested on Windows (whole
  millisecond sleeps, the last 2 ms polled). The polled millisecond costs
  0.4 to 0.7 ms of a core per frame; on this CPU sleeping through it cost
  up to 0.03 ms of input to present at 144 (lower clocks). Caps that
  divide 1000 start on the millisecond tick again: GL1's 99th percentile
  of input to present in `o2match` at 250 is then about 0.08 ms higher
  than with a deadline anywhere in the millisecond (the old limiter's
  level), Vulkan's about the same; not explained. The usercmd's step
  moved by 1 or -2 ms on every snapshot (`CL_AdjustTimeDelta`, Quake 3's
  clock drift: 8 % of the usercmds at 250 in a local game), whatever the
  limiter, until the command rate change (steps on a fixed grid since).
- The command rate (see Performance, Command rate): no step gets over
  every bump of `dm/mohdm2` (the default, 100 a second, 10 ms, stopped the
  fewest walks of the sweep). The step-up that pmove refuses when
  its trace back down lands on a short steep bevel (the cause at the bump
  measured) could be fixed in pmove itself, for the servers that run this
  game code and cgame's prediction alike; not done (a gameplay change).
  The world is drawn at whole milliseconds of game time (`cg.time` is an
  integer): above 1000 frames a second consecutive frames in the same
  millisecond show the same moment of the world (the view turns); a render
  time in microseconds for interpolation and animation was proposed, not
  done. The client samples the current stock `cl_movefps` step with a
  sub-millisecond fraction latched to `com_frameTime`. That fraction is not
  a `Pmove` input. A button or movement key waits for the next command (5
  ms on average at 100 a second, 2 at 250 frames a second before): an
  early command when a button is pressed between commands would take most
  of that off (a shorter step now and then); proposed, not done. Uncapped beyond what the GPU can draw (1500 frames
  a second at 1920x1080 on this machine) shows the input later than a cap
  just below it.
- Step 2 of the redesign, the per-vertex shader effects on the GPU: done
  for what the effects shader takes (Plan 21: the deforms but `normal`,
  the autosprites and `lightglow`, the `tcGen`s but `sunreflection`, the
  `tcMod`s but `macro`, `distFade`, static models near dynamic lights).
  Still on the CPU: the other `rgbGen`/`alphaGen` cases of
  `RB_StaticStageColor` (`lightingGrid`, `dot`, `oneMinusVertex`,
  `portal`, `heightFade`, ...: the sea of `obj_team3` has `alphaGen dot`),
  and the dynamic light pass (`ProjectDlightTexture`). JayRewd's commit
  also drew a merged batch's static models with one indirect draw (an
  `INSTANCED` variant of `generic.vert`); measured in its evaluation it was
  slower (`o4match` +13 µs), not taken.
- Dynamic lights: the per-vertex lighting of static models near a light
  is done by the effects shader since Plan 21, the entities' lighting
  (`RB_Light_Real`) by the vertex shader since step 3. `r_fastdlights 1`
  (`ProjectDlightTexture`: a pass per light with per-vertex colors and
  texture coordinates) is GL1's look of that mode; since Plan 24
  `r_fastdlights 0` never uses it (brush models are lit per pixel). An
  entity takes at most 32 lights (`MAX_REAL_LIGHTS` in `tr_types.h`, the
  map's sphere lights first, then the dynamic ones), which 64 dynamic
  lights near one entity can exceed (GL1's limit, unchanged).
- After step 3, skeletal models: the shaders that neither take the
  vertex-lit iterator nor have only stages that the skinned passes take
  (Plan 23: vertex colors and alphas, `alphaGen distFade`, waves, `tcMod`s,
  deforms, two textures: in the matches smoke, muzzle flashes, health
  packs, explosives, vehicle wheels and treads, fire) are still skinned on
  the CPU. Skeletal models outside the view are culled
  since Plan 17; their pose is still set (the radius comes from it: 1 to 2
  µs a frame in the matches), and models drawn with vertex deforms are
  never culled (0.2 % of the culls). The sun visibility traces of the
  entities that move (5 to 8 % of those views) are the engine's collision
  code. The `m3l3` path, whose sun traces took 23 % of the frame, wasn't
  measured again after the trace cache fix. The LOD terrain is drawn from
  its vertex pool since Plan 25; its tessellation still runs on the CPU
  every frame.

- At 3840x2160, where the GPU limits, the views measured since the GPU
  time work (see Performance, GPU time at 3840x2160) take 1 to 3 % less
  time than GL1's frames, but the `dm/mohdm3` overview, 0.6 % more (2.497
  against 2.483 ms), and the `obj_team2` yard with `r_fastsky 1`, 3.9 %
  more. What is left of the difference is the textures: radeonsi gives
  textures DCC, RADV only images that can be color attachments and are
  larger than 512x512, and with `RADV_DEBUG=forcecompress` (DCC for every
  image) Vulkan's frames take 0.03 to 0.06 ms less and are faster than
  GL1's in the `dm/mohdm3` overview, the `obj_team2` yard and the `m3l3`
  path. The textures (all 512x512 or smaller in `dm/mohdm3`, the lightmaps
  128x128) are below RADV's threshold whatever the renderer asks for; the fix
  would be in RADV (compressing sampled images that copies write, as
  radeonsi does), not in the renderer; not tried.
- A lead left from the main pass investigation (see Performance): RADV's
  primitive binning uses 128x128 bins where radeonsi uses 128x64 for
  depth-tested draws on this GPU (in `radv_gfx10_compute_bin_size`, Mesa
  26.0.8, the final `extent.height = MAX2(extent.width, ...)` and the depth
  bin's height taken from the color bins look like typos; a 128x128 bin
  needs more depth tag space than the 78 KB the formula allows). It can't
  be changed from the application; testing it needs a patched RADV (not
  done), and if it matters the fix belongs in Mesa.
- `r_ext_multisample 4` is 3 % slower than GL1's (2.77 against 2.70 ms per
  frame in the `m6l1a` forest at 1080p); the resolve into the swapchain
  image costs 0.26 ms of GPU time (RADV resolves into a DCC-compressed
  image with a shader; `vkCmdResolveImage` costs the same, and no image
  without DCC could be had to try the hardware resolve).
- The renderer's own CPU cost per draw (`vk_draw`, about 0.3 µs, 10 % of
  a 640x480 frame in the `m6l1a` forest before the static geometry) is
  RADV's command recording, the backend's bookkeeping and, for the CPU
  path, the vertex copies. The end of the frame (0.07 ms) is RADV's
  submission and the sync objects of the X11 present and acquire.
- The front end used to take up to 8 % longer than GL1's in the `m6l1a`
  forest: explained, the calls through the PLT and the static model loop
  (see Performance, Second round); it now takes 0.19 against GL1's 0.32
  ms per frame there at 1080p.

Known risks before shipping, handled (2026-10-18, Plan 15), and what is
left of them:

- Presentation semaphores: they belong to their swapchain now, and a
  replaced or destroyed swapchain and its semaphores are destroyed once
  the presentation engine is done with them, known from present fences
  (`VK_KHR/EXT_swapchain_maintenance1`) or, without them, from the
  `vkDeviceWaitIdle` before the swapchain is replaced, which the
  specification makes wait for presents too (see Implementation notes,
  Presentation semaphores and swapchains). The image acquired ahead is
  still released with an empty submission (that is correct: the
  submission waits for its semaphore and completes before the swapchain
  goes) and now also handed back with `vkReleaseSwapchainImagesKHR`. Left:
  not run on a driver that defers presentation (NVIDIA, MoltenVK) or on
  Windows (see [WINDOWS_TESTING.md](WINDOWS_TESTING.md)); the validation
  layer doesn't check this part (see Testing, Fallbacks).
- MoltenVK (portability subset): `imageViewFormatSwizzle` is queried and
  enabled; without it the fragment shader sets the alpha of RGB textures.
  The bindings of stride 0 aren't allowed by the portability subset at
  all (not only without `vertexAttributeAccessBeyondStride`: a stride must
  be at least `minVertexInputBindingStrideAlignment`), so on such a device
  they are read per instance with a real stride (see Implementation
  notes, Draws). Both forced on RADV and llvmpipe (`r_vkFallbacks 3`): the
  same frames. Left: not run on MoltenVK.
- Lines without `VK_KHR/EXT_line_rasterization` or `bresenhamLines`:
  forced with `r_vkFallbacks 4`, the same frames on RADV, line pixels
  moved here and there on llvmpipe (strict lines), no stipple (see
  Fragment pipeline). Debug drawing only.
- SDL 3 (Plan 35): `r_gamma` as a pass of the renderer (on the final
  image, or in the copy to the swapchain image when there is one) would
  apply it at once and the same on every platform and window system
  (Wayland has no gamma ramps, SDL 3 none at all; on Linux it goes to the
  textures now and a change shows after `vid_restart`); not done. The
  Windows parts (`WinMain` from `SDL_main.h`, the gamma ramp through
  `SetDeviceGammaRamp`, SDL 3's raw input thread) are built by CI but not
  run on Windows. SDL 3's pump checks its D-Bus connection before it
  reads the socket (1.2 µs a frame at 250 since Plan 36); no hint turns it
  off, a patch of SDL could check every 100 ms.
- The server browser (Plan 37): the SDK's resolver thread on Windows
  (`CreateThread`, `gethostbyname`) is built by CI but not run. One UDP
  socket for all queries, the replies told apart by their address, would
  replace the 32 sockets of a list (and the first refresh's 5 to 10 ms
  frame when the process passes 64 open files); not done: a late answer
  to a first query would then count as the answer to the second.

Windows: the same code applies; the CI build fetches Khronos' Vulkan-Headers
(see Build and run), so no Vulkan SDK is needed. The Vulkan-specific files
had never been compiled with MSVC before that CI build (they have no
variable-length array, checked with clang's `-Wvla`).
