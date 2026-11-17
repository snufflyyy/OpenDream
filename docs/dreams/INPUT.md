# Input: DualShock 4 / DualSense, keyboard and mouse

Working directory: `$REPO/port` (`$REPO` = your OpenDream checkout).

How the port feeds Dreams' controls. Research behind it: `$REPO/notes/input-research.md` (disassembly
of how Dreams reads the pad and moves the imp).

## Controls

Dreams' on-screen prompts show PlayStation buttons. A DualShock 4 / DualSense (USB or Bluetooth,
via SDL3) works as on a PS4, motion sensors included. The keyboard and mouse stand in for the
pad and work alongside it: their presses are added to the gamepad's.

**Mouse**: click in the game window to give it the mouse (the pointer disappears; that click is
not passed to the game). **F1** lets the mouse go and takes it back. Switching to another window
also lets it go.

| PC | PS4 | Notes |
|---|---|---|
| Mouse movement | **imp** (controller motion) | As if you turned the controller: right/left = yaw, up/down = pitch. Use Dreams' "Motion Sensor Function" control scheme (the default). Speed: Dreams' *Imp Sensitivity* setting and `DREAMS_MOUSE_IMP_SENS` |
| Left button | R1 | |
| Right button | R2 | |
| Middle button | L1 | |
| Side button back (X1) | L2 | |
| Side button forward (X2) | Square | |
| Wheel up / down | d-pad up / down | one press per notch |
| W A S D | left stick | |
| I J K L | right stick | |
| Arrow keys, or 1 2 3 4 | d-pad (← → ↑ ↓ for 1-4) | |
| Space, Enter | Cross ✕ | Enter/Esc also skip a playing video |
| Backspace | Circle ○ | |
| Tab | Square □ | |
| F | Triangle △ | |
| Q / E | L1 / R1 | |
| Z / C | L2 / R2 | |
| Left Shift / Left Ctrl | L3 / R3 | |
| Esc | Options | |
| G / H | touchpad click (left / right half) | |
| F1 | release / take the mouse | window key, not a game button |

This is a first layout. Its button choices haven't been tested against what Dreams does with each
button yet. Change any of it in a settings file (`BB_CONFIG=<file>`, bbport's format): one line per
PS4 input,
`key.<input>=<SDL key names, comma-separated>` (mouse: `Mouse Left`, `Mouse Right`,
`Mouse Middle`, `Mouse X1`, `Mouse X2`, `Wheel Up`, `Wheel Down`; chords: `Left Ctrl+Z`).
Inputs: `cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_right up down
left right move_up move_down move_left move_right look_up look_down look_left look_right`.
Example:
```
key.l2=Left Ctrl+Z
key.circle=Backspace,Mouse X1
mouse_invert_y=1
```

**Text boxes**: when Dreams asks for text (naming a creation, search), type in the game window.
The text shows in the window title. Enter confirms, Esc cancels. While you type, keys and
mouse buttons don't act as pad buttons (W doesn't move the stick), and after the box closes
they stay off until all keys are released (so the confirming Enter isn't also a ✕).

**Without a DualShock 4**: the mouse is the imp. If you'd rather use the sticks, choose Dreams'
"Left & Right Sticks" control scheme in its preferences. WASD and IJKL then drive the sticks.

## Switches

| Env (or bbport.ini key) | Effect |
|---|---|
| `DREAMS_INPUT_PROFILE=bbport` (`input_profile=bbport`) | bbport's Bloodborne layout (Space dodge = Circle pulse, Shift run, ...) and mouse look on the right stick |
| `DREAMS_MOUSE_MODE=camera` (`mouse_mode=camera`) | the mouse turns the camera (right stick) instead of moving the imp |
| `DREAMS_MOUSE_IMP_SENS=<rad/px>` | mouse → imp rotation per pixel (default 0.0004: about one screen pixel per mouse pixel at 1080p with Imp Sensitivity 6) |
| `mouse_invert_y=1` | mouse up/down reversed (imp and camera) |
| `DREAMS_PAD_SENSORS=0` | ignore the controller's motion sensors |
| `DREAMS_PAD_SENSOR_TRACE=1` | raw SDL gyro/accel samples and what the game reads (first 20, then every 2 s) |
| `DREAMS_PAD_READ_TRACE=1` | every 600th scePadRead: the game-facing timestamp (process clock, µs), orientation, angular velocity |
| `DREAMS_PAD_SYNTH_MOTION=<deg>[,<period s>[,<delay s>]]` | test aid: the controller's motion is replaced by a synthetic figure eight (yaw ±deg, pitch ±deg/2); no controller needed (agent runs) |
| `BB_PAD_FILE=<file>` | scripted buttons/sticks (`cross`, `lx=`, ...) and mouse motion (`mdx=`/`mdy=` pixels per read, as if the window held the mouse) |
| `DREAMS_IME_KEYBOARD=0` | no USB-keyboard events to the game |

## Design

- **How Dreams moves the imp** (v2.52 eboot, `$REPO/notes/input-research.md` §1): every `scePadRead`
  sample goes into a 256-entry history ring. The imp takes the orientation at
  `sceKernelGetProcessTime() − 1.5 sample periods`, interpolated between ring samples, and moves by
  the *change* of orientation since its last update. So:
  - `PadData.timestamp` must be on the process clock (µs since process start). With another clock
    no sample is old enough, Dreams falls back to the identity orientation and the imp freezes.
    That was the "gyro doesn't move the imp" bug, fixed in `3a31dd3`.
  - `connectionType` must stay 0 (local). Otherwise Dreams runs its own sensor fusion over our
    data.
- **Sensors** (`src/runtime_pad/sensors.c`): SDL3 gyro/accelerometer at ~500 Hz on a "PadMotion"
  thread, integrated to an orientation (shadPS4's CalculateOrientation), with Dreams' tilt
  correction and bias settings.
- **Mouse → imp** (`sensors.c` `sensors_mouse`/`sensors_report`): mouse motion is a separate
  yaw/pitch rotation (pitch limited to ±80°), reported as `orientation = sensors ⊗ mouse` and added
  to `angular_velocity` (Dreams counts angular velocity as "the pad is in use"). Tilt correction
  acts on the sensors only, so it never pulls the mouse back. `scePadResetOrientation` clears both.
- **Window** (`vcore/shim/window.cpp`, `bbgpu.cpp` `bbgpu_mouse_*`): relative mouse mode on click,
  F1 toggles, focus loss releases. Motion and wheel notches are accumulated and taken by the runtime
  at each pad read. `bbgpu_text_input_end` stops the window's text entry when the game closes its
  IME first.
- **Profiles** (`src/runtime_pad/bindings.c`): `dreams_defaults()` for Dreams; bbport's layout with
  its Bloodborne tricks (`keyboard_circle`, walk, camera auto-rotation byte patches) only in the
  `bbport` profile.
- **IME** (`src/runtime_ime.c` `runtime_ime_typing`, `sample.c` `ime_suspends_keys`): key → pad
  bindings pause while the text box is open and until all keys are up afterwards.

## Tests

- `tests/test_pad_input.c` (from `$REPO/port`): Dreams profile keys and mouse buttons, mouse → orientation
  (signs, scale, pitch limit, composition with the sensors, reset), camera mode, IME pause,
  synthetic pose.
- `tests/test_pad_motion.c`: sensor integration, tilt, bias, SDL virtual gamepad with sensors.
- Game check without a controller: `DREAMS_PAD_SYNTH_MOTION` in an agent run
  (`docs/dreams/AGENT_RUNS.md`). The imp traces the figure eight in the captures (WORKLOG
  2026-11-15).
