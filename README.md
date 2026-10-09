# QX-18 USB PTT audio demo

Standalone C demo for Debian 13, using ALSA and Linux evdev directly. Hold the
microphone's PTT button to record; release to finish the WAV and play it through
the microphone's speaker. A new press interrupts playback. Ctrl+C exits.
No network, GUI, firmware changes, HID commands, or system services are used.

## Build and run

```sh
sudo apt install build-essential libasound2-dev
make
make check
sudo ./qx18
```

Run `./qx18` without sudo when your existing permissions allow access to the
input and ALSA devices. Recordings are private files (mode 0600) in
`./recordings`, created with unique names. A newly created output directory has
mode 0700. Recordings are retained, including empty WAVs from very short taps;
empty files are never played. Remove old files yourself to bound total disk use.

```sh
./qx18 --help
sudo ./qx18 --input /dev/input/event18 \
  --capture hw:CARD=UACDemoV10,DEV=0 \
  --playback plughw:CARD=UACDemoV10,DEV=0 \
  --output-dir ./recordings --max-seconds 60
```

`event18` is only an example: event numbers change. Automatic discovery checks
sysfs USB ancestry for VID:PID `7273:8378` and evdev capability `KEY_F2`. Multiple
matching keyboards require `--input`; the explicit interface is checked too.
The program must acquire an exclusive `EVIOCGRAB`, otherwise it exits with an
error. While grabbed, events from that keyboard interface cannot reach other
applications. Other input interfaces are not grabbed.

The default audio devices match the tested unit, named `UACDemoV1.0` with serial
string `Jieli Technology`. With several units, duplicate ALSA card IDs may
require explicit `--capture` and `--playback` overrides: selecting an input
interface does not automatically select its ALSA card. Inspect `arecord -l`
and `aplay -l` (optional `alsa-utils` package) to choose the intended card.

### Run without sudo

To give your current user access to devices owned by the `input` group, run
this from your normal user account:

```sh
sudo usermod -aG input "$USER"
```

Log out completely and log back in (or reboot) so the new group membership
applies. Check that `id -nG` includes `input`, then run:

```sh
./qx18
```

You can also omit `sudo` from the longer example above. Membership in `input`
grants access to other input devices, including keyboards, not just the PTT
button. This is a manual, system-wide permission choice.

ALSA devices need separate access. If capture or playback reports permission
denied, inspect `ls -l /dev/snd/`. If those devices belong to the `audio` group
and your session does not already provide access, add that group too:

```sh
sudo usermod -aG audio "$USER"
```

Log out and back in again after this change. If an earlier sudo run created
the default `recordings` directory, use a fresh directory owned by your user:

```sh
./qx18 --output-dir "$HOME/qx18-recordings"
```

## Behavior and limits

- Starts idle with capture closed. If PTT is already down at startup, release
  it before making the first recording. PTT is `EV_KEY / KEY_F2` (60); repeat
  events (value 2) are ignored.
- WAV format is little-endian PCM, mono, signed 16-bit, 16000 Hz. Playback uses
  ALSA's `plughw` conversion to the unit's native 48000 Hz playback format.
- Input is processed before audio in one poll loop. Audio transfers and drain
  are nonblocking; signals are received through `signalfd`. No audio thread,
  subprocess, or signal-handler ALSA calls are used.
- Release drops capture immediately when processed, closes it, and finalizes
  the header. Audio still buffered in ALSA is intentionally not collected.
- Capture EIO preserves collected frames and waits for release before playback.
  It does not restart capture while the same press remains down. Startup
  EBUSY/EAGAIN/EINTR gets at most four attempts, spaced 50 ms apart while held;
  startup EIO is not retried. Capture/playback xruns are prepared again; lost
  audio at an xrun cannot be restored. Other PCM errors stop that audio operation.
- Both recorded frames and elapsed capture time are limited by `--max-seconds`
  (default 60, range 1–3600). At the limit, capture is finalized and waits for
  release before playback. Each new press can create another file.
- Input removal or ALSA ENODEV stops audio, finalizes any recording and exits.
  An evdev queue overflow also exits safely because button state is uncertain.
  Ctrl+C and SIGTERM finalize recording, stop playback, and release the grab.
  SIGKILL/power loss cannot finalize an in-progress WAV.

## Diagnostics and desk checklist

Optionally install `evtest` with `sudo apt install evtest`, then use
`sudo evtest /dev/input/eventN` to check press/release events. Close other
programs that exclusively grab the input before starting this demo. Permission
errors need appropriate existing device access or sudo; PCM errors may need
an audio-device override or closing another application using the hardware.
No broad input permissions or automatic udev rules are installed.

`make check` checks exact WAV header bytes, file size and preserved PCM payload
for empty, one-sample and three-sample recordings, including repeated header
finalization. It also checks CLI help and rejection of a zero recording limit.
These checks do not exercise real USB/ALSA hardware. The implementation has
not been tested on the microphone as part of this work; the hardware facts in
`AGENTS.md` are observations from the user's tested unit.

On desk, verify:

- **Long hold:** record several seconds; release plays intelligible audio.
  Also try `--max-seconds 2`: capture ends at the limit and playback waits for release.
- **Short press:** tap quickly; valid empty/short WAV, no empty playback, no stuck state.
- **Repeated presses:** each press produces a distinct file; autorepeat creates no extras.
- **Playback interruption:** press during playback; playback stops and a new recording starts.
- **EIO on release:** collected audio survives whether EIO or release is processed first;
  only one finalization/playback occurs and capture is not repeatedly restarted.
- **Unplug:** disconnect during capture and during playback, and while waiting for release
  after the limit/EIO; audio stops, any recording is finalized, removal is reported, process exits.
- **Ctrl+C:** try idle, capturing, playback, startup retry and waiting for release;
  process exits promptly and subsequent runs can grab the keyboard.
- **Input isolation:** F2 does not reach other applications while the demo runs.
  Startup with PTT held waits for release. Multiple units require explicit selection.

If `alsa-utils` is installed, `aplay --dump-hw-params recordings/<file>.wav`
can inspect/play a recording; use `-D plughw:CARD=UACDemoV10,DEV=0` to direct it
to the microphone speaker.
