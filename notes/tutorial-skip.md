# Tutorial skip save file

Source: Dreams on PC Discord server (shared by user, 2026-11-15).

On first run the Dreams tutorial won't let you progress under shadPS4. Copy
`tutorial-skip-savedata.json` to the shadPS4 savedata path as `savedata.json`:

- Windows (as given): `%APPDATA%\shadPS4\home\1000\savedata\CUSA04301\SYSTEM\savedata.json`
- Linux equivalent (likely): `~/.local/share/shadPS4/home/1000/savedata/<TITLE_ID>/SYSTEM/savedata.json`

Note: the path given uses title ID `CUSA04301`; our copy is `CUSA08010`. Use the
folder name shadPS4 actually creates for our copy.

It unlocks all progress flags and marks tutorial VO/cutscenes as played.
`control_scheme` is set to `"Motion"` (DualShock gyro); `"Stick"`-style schemes
may matter on PC.
