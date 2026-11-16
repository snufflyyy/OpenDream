# Input research: DS4 motion → imp, keyboard and mouse (2026-11-15)

Research/diagnosis only; no change to `port/`. Evidence is from a static disassembly of the
CUSA08010 v2.52 eboot (`objdump` on the text segment of `work/out/eboot.elf`, PLT names mapped
with a small script over `port/src/import_names_dreams.inc`), from shadPS4 (`ref/shadps4` branch
`vcore`) and from the user's run logs/captures. The runtime verification run is **not done yet**: the
lead paused game launches until a headless `tools/agent-run.sh` exists (it must not open the user's
DS4, window, or audio). The diagnostic build for that run is ready (section 5).

Addresses below are eboot virtual addresses (image offset; the text segment starts at 0).
On v2.64 they will differ; the *structure* (ring, interpolation, process-clock compare) is what
matters.

## 1. Root cause: the imp didn't move because of the pad timestamp clock (fixed by 3a31dd3)

### 1.1 How Dreams consumes scePadRead

* **One call site** of `scePadRead`: `0x7bd4f7` inside the pad update function `0x7bd4c0`
  (`scePadRead(handle, stack_buf, 64)`: `mov edx,0x40` at `0x7bd4e5`). Dreams asks for up to 64
  samples and processes **every returned sample** in a loop (`0x7bd5c0..0x7bdc52`, stride `0x78` =
  `sizeof(OrbisPadData)`).
* Each sample is copied into a **256-entry history ring** in the pad object (`obj+0x58 + i*0x78`,
  head index at `obj+0x54`; copy at `0x7bd6e0..0x7bd715`). The main pad object for user 0 is the
  global `0x5849ca0+0x70`.
* `0x7bd4c0` is called from 6 places (`0xd1e6d`, `0x771c17`, `0xb7993b`, `0xcce482` (main input
  update), `0xcce6e6` (second handle), `0x117daca`), each under the object's mutex, so with our
  port (1 sample per call) the ring gets several samples per frame.
* After `scePadRead`, Dreams calls `scePadGetControllerInformation(handle, obj+0x38)` and uses:
  `connectionType` (`obj+0x42`), the stick dead zones (`obj+0x40/0x41`, idle detection) and
  `deviceClass` (`obj+0x48`, compared with 9 at `0x772065`).
* **connectionType == 0 (LOCAL) → Dreams uses the PS4's `orientation` as given.** If it is non-zero
  (remote play), Dreams runs its *own* fusion instead (`0x7bd722..0x7bdb18`: integrates
  `angular_velocity` with dt = Δtimestamp µs × 1e-6 clamped to 0.1 s, plus an accelerometer tilt
  filter using `expf`, written back over the ring's orientation). We report 0, like shadPS4.
* Per sample it computes an activity value (|‖accel‖−1|·0.2 + max|ω|·1.5 + stick/trigger
  deflection) and remembers the timestamps of activity (> 0.06, > 0.8), of Options presses and of any
  non-idle input. From these it toggles `scePadSetAngularVelocityBiasCorrectionState`
  (`0x7bdc63..0x7bdcd4`): on when idle > 20 s (`0x1312d00` µs) or Options pressed within 6 s
  (`0x5b8d80`). **This explains the frequent bias-correction toggling in our logs.** It is normal.
* The input arbitration (`0xcce49b...`) treats a device as active if its newest ring timestamp is
  within 0.5 s (`0x7a120` µs) of its last activity timestamp.

### 1.2 The imp's orientation lookup (`0x771ac0`)

`0x771ac0` (called from `0xca6121`/`0xca6214` for the pad and the second handle) does this:

1. `period` = 128 s·1e6 / (ts[head−1] − ts[head−129]): the mean sample period from the ring's
   timestamps (`0x771b3d..0x771d5c`, constants 128000000.0 and 1.0).
2. `now = sceKernelGetProcessTime()` (`0x771b93`, PLT `0x169a6d0`).
3. `t = now − 1.5 × period` (µs) (constants 1e6 and 1.5).
4. When the flag byte `0x5849cd4` is set (it is: initialised to 1 at `0x770c64`, never cleared),
   it searches the ring backwards (up to 255 entries) for the newest sample with
   `timestamp <= t` (`0x771ca0..0x771cc4`), then **nlerps the orientation** between that sample and
   the next newer one by `(t − ts_a)/(ts_b − ts_a)` (`0x771ced..0x771e29`).
5. **If no sample has `timestamp <= t`, it uses the constant `(0,0,0,1)` (identity) from
   `0x16bbde0`** (`0x771c84`, `0x771cc6 → 0x771d17 → 0x771e2d`).
6. The imp then moves by the **change** in orientation since the previous update: q_rel =
   conj(q_prev) ⊗ q (`0x7721ee..0x772342`, previous q kept at `[state+0x10]`, valid if ‖q_prev‖ >
   0.999), the controller's axis rotated by q_rel, perspective-divided (min 0.1), scaled by
   tan(fov/2) (fov 1.0838 rad from `0x5849cd0`, `tanf` at `0x771b0b`), 16/9 aspect, (1.75, 1.25) /
   1.0703, and by the Imp Sensitivity setting (`powf(2.8, …)` or `0.4 + 2.4·s`, `0x7723e3..0x7724a5`).

So the imp is **incremental**: it moves only when the orientation it gets changes from frame to frame.

### 1.3 Why it was frozen

Before commit `3a31dd3` our `PadData.timestamp` was CLOCK_MONOTONIC **since boot** in µs (hours of
uptime ≈ 1e10 µs). `sceKernelGetProcessTime` counts from process start (≈ 1e7–1e8 µs). Every ring
sample therefore had `timestamp > t`, the search in step 4 always failed, and the imp always got the
identity quaternion → zero orientation change → **the imp never moves**, however good the sensor
data. This is deterministic (boot time is always earlier than process start), which fits the user's
report even though `DREAMS_PAD_SENSOR_TRACE` showed real motion in our PadData
(`work/logs/run-20261115-192439.log`).

`3a31dd3` converts the game-facing timestamp with `runtime_monotonic_to_process_us`, which uses the
same origin (`process_start`) as our `sceKernelGetProcessTime` (`port/src/runtime_kernel.c:87-96`;
they can differ by ≤ 1 µs from rounding, which is harmless: the search then picks the previous
sample). With the fix the search succeeds and the imp should follow the controller. **This is the
root cause; the fix is the right one.** It still needs a run to confirm it (section 5). The user's
test after the fix (`work/logs/run-20261115-193516.log`, captures `work/captures/20261115-193516`)
can't settle it: the log shows motion reaching PadData (samples 9750–18508, 32130–39914) but has no
game-facing timestamp trace. In the captures the white loop under the selected item moves only in
d-pad steps (preferences screen, t=66–112 s), and I can't tell the imp apart at 480 px.

Other things I checked that are **not** the cause:
* Sample count: Dreams accepts any count ≥ 1 (`jle` skips count ≤ 0). Our "1 sample per call" is fine.
  shadPS4 pushes a state every 4 ms (SDL timer, `src/sdl_window.cpp:344`) and on events, and
  `scePadRead(…, 64)` pops the queue (`src/input/controller.cpp:69-96`). That is closer to the PS4,
  but not required.
* Orientation convention: same integration as shadPS4 (`CalculateOrientation`, body-frame ω,
  q ← q + ½ q⊗ω dt). Sign check against shadPS4's mouse gyro (`input_mouse.cpp:92-99`: gyro =
  (−dy, −dx, 0)/100), ours (`sensors.c` `sensors_mouse`) uses the same signs.
* Units: rad/s, g (shadPS4 scales SDL m/s² by 0.098, we use 1/9.80665). Both are fine.
* connected / connectedCount: always 1. ControllerInfo: connectionType 0, deviceClass 0, as shadPS4.

## 2. Things in the scePadRead path that can break other things if changed

* **Timestamps must stay on the process clock (µs, sceKernelGetProcessTime origin) and strictly
  increase.** Any other clock re-freezes the imp. Equal timestamps across reads are harmless (the
  search needs ts_a ≤ t < ts_b, so the divisor is never 0), but a clock that jumps backwards breaks
  the interpolation.
* **connectionType must stay 0.** Non-zero switches Dreams to its own fusion and overwrites our
  orientation.
* **Returning several buffered samples per read** (the PS4/shadPS4 model) is supported by Dreams,
  but each read must return only *new* samples. With ~6 readers per frame, a shared queue would
  hand different samples to different call sites, which is fine since they all feed the same ring.
  Don't return the same old samples twice: duplicated timestamps distort the `period` estimate
  (step 1) and the activity windows.
* Gyro/stick idle detection uses the dead zones from GetControllerInformation (2 in our port, 1 in
  shadPS4). A larger dead zone makes Dreams consider the pad idle sooner (bias correction on).
* Second `scePadOpen(user, type 2 SPECIAL, 0)` (`0xccdd51`): our `pad_open` returns
  `ERR_ALREADY_OPENED` (0x80920004). shadPS4 returns a valid handle to an unused, disconnected
  controller. Dreams copes with both (the read on the bad handle fails → count ≤ 0 → skipped;
  `0xcce711` treats a failed GetControllerInformation as deviceClass −1). If changed, it must
  read as **disconnected**: handing it the DS4 too would feed the "special device" path (deviceClass 9
  check at `0xcce72c`).
* Mouse deltas and keyboard state are sampled inside `sample_host`, i.e. once per scePadRead
  call. Several calls per frame are normal; anything frame-based (e.g. "pulse for one frame")
  must use time, not read count.
* The `angular_velocity` field feeds Dreams' activity detection: synthetic orientation changes
  (mouse) should also report a matching angular velocity, or Dreams may treat the pad as idle and
  turn bias correction on (harmless for us, since we apply bias only to real gyro data).

## 3. Keyboard and mouse: what exists, what Dreams supports, proposed design

### 3.1 What Dreams itself supports (PS4)
* No `libSceMouse` import, so **Dreams has no native mouse**.
* USB keyboard: Dreams opens `sceImeKeyboardOpen` at boot and uses `sceImeOpen` for text fields
  (`port/src/runtime_ime.c` implements both, events through `sceImeUpdate`). Whether Dreams has
  keyboard *shortcuts* beyond text entry is unverified.
* Other pointer devices: PS Move (`libSceMove`, `sceMoveReadStateRecent` at `0x7bdd00`, 64-byte
  states), which needs camera tracking. Not usable offline.
* So for PC, mouse and keyboard must be **translated into DualShock 4 input**. The imp is driven by
  orientation *changes* (§1.2), so a mouse maps onto it naturally as synthetic rotation.

### 3.2 What the port already has (bbport heritage)
* `src/runtime_pad/bindings.c`: keyboard and mouse-button → pad bindings with `bbport.ini`
  (`BB_CONFIG`) `key.<input>=` / `pad.<input>=` overrides, chords ("Left Shift+Mouse Left"), wheel
  as d-pad up/down. Defaults are **Bloodborne's**: WASD left stick, IJKL right stick, Space =
  dodge (a Circle *pulse*), Left Shift = sprint (Circle *held*), Shift+Space jump logic
  (`keyboard_circle`), F/Enter Cross, Q Triangle, E L1, Tab/R Square, C R3, Ctrl L3, Esc Options,
  G/Backspace touchpad, 1-4 and arrows d-pad, LMB R1, Shift+LMB R2, RMB L2, MMB R3, X1 R2, X2 L1.
  `video_skip`: Enter/Esc skips AvPlayer videos.
* Mouse look (`apply_mouse_look`) turns captured mouse motion into the right stick (camera).
* `src/runtime_pad/sensors.c` `sensors_mouse`: with `DREAMS_PAD_MOUSE_GYRO=1` and **no** gyro,
  mouse motion becomes angular velocity and is integrated into the orientation (0.0025 rad/px).
* **vcore has no mouse at all**: `port/vcore/shim/bbgpu.cpp:289-299`, `bbgpu_mouse_take`,
  `bbgpu_mouse_captured`, `bbgpu_mouse_capture`, `bbgpu_mouse_look_enable` are stubs returning 0,
  and `vcore/shim/window.cpp` handles no mouse events. So today the mouse does nothing in Dreams.
  bbport's full implementation is in `port/gpu/shim/window.cpp` (~lines 260-380: relative mode
  on click, F1 release, wheel accumulation, focus-lost release).

### 3.3 Conflicts to fix
1. **IME text entry vs pad bindings.** The window swallows key events while text entry is active
   (`window.cpp` `text_active`), but `bindings.c` reads `SDL_GetKeyboardState` plus its own
   event-watch key latch (`key_watch`, runs before the window sees events). So typing "w" moves the
   left stick, and Enter presses Cross / skips a video, while the game's text field is open. Fix:
   expose `runtime_ime_text_active()` (or a window flag) and make `sample_host` ignore keys
   (and the latch) while it is set.
2. **USB keyboard events + pad bindings both reach the game** (runtime_ime.c note). If Dreams turns
   out to have keyboard shortcuts, a key could act twice. Keep `DREAMS_IME_KEYBOARD` and decide
   after testing. Default: keep both (text fields need the keyboard device).
3. Bloodborne-specific semantics (Circle pulse/hold for Space/Shift, auto-rotation hold, native
   camera hooks) must not apply to Dreams: Circle is "back" in Dreams, so Shift (sprint = Circle
   held) would cancel things. These need a Dreams binding profile (see below).
4. A real DS4 and the mouse at the same time: `sensors_mouse` returns early when the pad has a
   gyro, so with a DS4 plugged in, the mouse would go to the camera. The design below combines
   them instead.

### 3.4 Proposed design

**vcore (`vcore/shim/window.cpp`, `bbgpu.cpp`)**: port bbport's mouse capture:
* `SDL_EVENT_MOUSE_BUTTON_DOWN` in the window → `SDL_SetWindowRelativeMouseMode(window, true)`
  (the click that captures is *not* passed on as a button). F1 toggles release/capture. Release on
  `SDL_EVENT_WINDOW_FOCUS_LOST`. Hyprland/Wayland supports this through the
  relative-pointer/pointer-constraints protocols, which SDL3 uses.
* Accumulate `event.motion.xrel/yrel` and wheel steps in atomics/mutex. `bbgpu_mouse_take` returns
  and resets them. `bbgpu_mouse_captured` returns the capture state. `bbgpu_mouse_capture` asks the
  window thread to capture. Keep `bbgpu_mouse_look_enable` (no-op is fine).
* Headless agent-run mode: no capture; the runtime's `BB_PAD_FILE` `mdx=/mdy=` injection already
  simulates mouse motion "as if captured".

**Runtime (`src/runtime_pad/*`), a Dreams input profile (default for Dreams):**
* **Mouse → imp.** Keep a separate mouse rotation `q_mouse` (yaw from dx about the controller's y,
  pitch from dy about x, same signs as `sensors_mouse` and shadPS4), and report
  `orientation = q_sensor ⊗ q_mouse`, so a real DS4 and the mouse can be used together and tilt
  correction (which acts on `q_sensor` only) never pulls the mouse offset back. Add the mouse rate
  to `angular_velocity`. Sensitivity in rad/px (`DREAMS_MOUSE_IMP_SENS`, `bbport.ini
  mouse_imp_sensitivity`), start ~0.0015 rad/px and calibrate in a run so a mouse sweep of
  ~1000 px crosses the screen at Imp Sensitivity 6. Optionally clamp the pitch to ±80°. With
  the mouse going to the imp, `apply_mouse_look` must not *also* move the camera.
  Optional "camera drag": hold Middle (or a chosen key) to send mouse motion to the right stick
  instead (bbport's mouse look path).
* **Recenter**: since the imp is incremental, there's no drift with a mouse. `scePadResetOrientation`
  isn't imported by Dreams, so no recenter key is needed.
* **Buttons (proposal; verify against the in-game prompts before making it the default)**:
  LMB → R1 (grab/select), RMB → R2 (action/use tool), MMB → camera-drag modifier, X1/X2 →
  L1/L2, wheel → right stick Y pulses (camera zoom) or d-pad up/down (currently d-pad; Dreams
  menus also scroll with the d-pad). Keyboard: WASD → left stick, arrows → right stick (camera),
  Space/Enter → Cross, Backspace/Esc-in-menus → Circle, Tab → Square, Q → Triangle, E → L1,
  F → L2, Esc → Options, Ctrl+Z → d-pad left, Ctrl+Y / Ctrl+Shift+Z → d-pad right (assumed
  undo/redo, to verify), G → touchpad. Drop Bloodborne's Circle-pulse/hold logic in this profile.
  Everything stays overridable through the existing `key.<input>=` lines. A `BB_GAME_PROFILE`-like
  switch (`DREAMS_INPUT_PROFILE=dreams|bbport`) picks the defaults.
* **Without a gyro** (keyboard/mouse only), the mouse path is the default (today it needs
  `DREAMS_PAD_MOUSE_GYRO=1`). Users who prefer it can pick Dreams' "Left & Right Sticks" scheme:
  WASD/arrows then drive the imp through the sticks.
* **IME interplay**: suspend all keyboard → pad bindings while `sceImeOpen` text entry is active
  (§3.3.1). Leave mouse buttons active (they don't type).

### 3.5 Implementation steps (in order)
1. Confirm the timestamp fix with the synthetic-motion run (§5) and the A/B with
   `DREAMS_PAD_TS_LEGACY=1`. Then fold `DREAMS_PAD_SYNTH_MOTION` into the main tree as a test aid,
   and add `motion=`/`gyro=` tokens to `BB_PAD_FILE` for scripted tests.
2. vcore mouse capture (window.cpp + bbgpu.cpp), ported from bbport's `gpu/shim/window.cpp`.
   This changes vcore and needs a vcore rebuild. The bbgpu.h ABI is unchanged (the functions
   already exist as stubs).
3. Runtime: `q_mouse` composition + angular velocity, the Dreams binding profile, and the IME
   suspension of key bindings.
4. Calibrate the sensitivity and verify the buttons against the in-game prompts using agent runs
   (`BB_PAD_FILE` with `mdx=`), then the user's hands.
5. Optional fidelity: a 250 Hz sample queue like shadPS4/PS4 (pad thread pushes states;
   `scePadRead` pops only new ones; return 0 when there are none). This needs tests for the six
   readers per frame (§2).

## 4. Other findings
* Dreams sets tilt correction on at open (`vDLMoJLde8I` = `scePadSetTiltCorrectionState(h,1)` at
  `0xccdac5`/`0xccdd60`).
* The Dreams fork's `src/input/dreams_background_control.h` (Windows-only debug endpoint) injects
  `angularVelocity` *and* `orientation` together for its scripted tests. Its authors drove the imp
  through orientation too, which agrees with §1.2.
* Constants worth knowing (v2.52): activity thresholds 0.06/0.8, activity weights 0.2 (accel) and
  1.5 (gyro), the remote-fusion dt clamp of 0.1 s, accel gate 0.5625..2.25 g² (= 0.75..1.5 g),
  the interpolation delay of 1.5 sample periods, and the FOV of 1.0838 rad.

## 5. Diagnostic worktree (ready, not yet run)

`/home/braden/Documents/Programming/Dreams-Recomp/port-diag` is a detached git worktree of `port`
at `3b5cb71`. It sits next to `port/` so `run-dreams.sh`'s `ROOT=..` resolves; remove it with
`git -C port worktree remove ../port-diag`. Changes (uncommitted, in the worktree only):
* `src/runtime_pad/sensors.c`: `DREAMS_PAD_SYNTH_MOTION=<deg>[,<period s>[,<delay s>]]`. It
  replaces the sensors with a synthetic figure eight (yaw A·sin, pitch A/2·sin(2·)) as angular
  velocity at 500 Hz, with the accelerometer consistent with the integrated orientation, starting
  `<delay>` s after pad open. No real pad is needed.
* `src/runtime_pad.c`: `DREAMS_PAD_TS_LEGACY=1` restores the pre-3a31dd3 boot-monotonic stamps
  (A/B). `DREAMS_PAD_READ_TRACE=1` prints the game-facing timestamp, orientation and ω every 600th
  read.
* Built `port-diag/out/dreams-probe` against the main tree's `port/vcore/build/libvcore.so` (same
  commit, same ABI; no vcore rebuild) with
  `port-diag/build-diag.sh` (untracked): the `build-dreams.sh` gcc line plus `-idirafter port` (the
  worktree lacks the untracked `third_party/LibAtrac9`); `out/libdreams-{av,codecs}.so`,
  `libatrac9.a` copied from `port/out`, `out/gpu-library.txt` = vcore.

Planned verification (once `tools/agent-run.sh` exists; it must run the worktree's binary):
* Run A: `DREAMS_PAD_SYNTH_MOTION=25,4,45 DREAMS_PAD_READ_TRACE=1 DREAMS_CAPTURE=250` for ~100 s.
  On the preferences screen something (the imp) should trace a figure eight with a 4 s period
  in the capture sheet (`tools/capture_sheet.py sheet … --from <t> --to <t+8>`).
* Run B: the same plus `DREAMS_PAD_TS_LEGACY=1`. The imp should be static (identity fallback).
* A vs B confirms §1.3 end to end. The `Pad read` trace shows the timestamps next to the process
  time.

## 6. Outcome (2026-11-15, branch `input` in `port-diag`)

Confirmed in headless agent runs: with the process-clock timestamps the imp follows synthetic
motion (`work/captures/20261115-201803`), with the old boot-monotonic stamps it stays still
(`work/captures/20261115-202118`). `DREAMS_PAD_TS_LEGACY` was removed after this A/B;
`DREAMS_PAD_SYNTH_MOTION` and `DREAMS_PAD_READ_TRACE` stay as documented diagnostics. Keyboard and
mouse were implemented as designed in §3.4 (`port/docs/dreams/INPUT.md` on that branch). The mouse
default is 0.0004 rad/px, calibrated from the synthetic run.
