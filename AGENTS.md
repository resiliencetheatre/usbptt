# QX-18 USB PTT audio demo

## Goal
Build a small standalone C demo for Debian 13: hold the microphone's PTT
button to record a WAV; release it to finish the recording and play it through
the microphone's speaker. Repeat until Ctrl+C. No network or GUI is needed.
Use ALSA and Linux evdev directly. Keep the implementation small and readable.

## Verified hardware behavior
These observations apply to the unit tested on desk; do not assume all QX-18
units have identical configuration.
- USB VID:PID: 7273:8378; name UACDemoV1.0; serial string Jieli Technology.
- ALSA capture: hw:CARD=UACDemoV10,DEV=0, S16_LE, mono, 16000 Hz.
- Playback: plughw:CARD=UACDemoV10,DEV=0. Native playback is mono S16_LE,
  48000 Hz; ALSA plug conversion can play the 16000 Hz recording.
- PTT produces EV_KEY / KEY_F2 (Linux key code 60): 1 press, 0 release,
  2 autorepeat. Ignore value 2.
- Tested input node was /dev/input/event18 and hidraw node /dev/hidraw6.
  These node numbers are temporary, not stable identifiers.
- Capture started with PTT released failed with EIO and produced only a
  44-byte WAV header. Capture started with PTT held succeeded.
- Releasing PTT during capture caused EIO; already captured audio was valid.
  A 72044-byte WAV contained 2.25 seconds of audio.
- Speaker playback of the successful recording was audible.
- HID report IDs: 01 consumer control, 02 keyboard, 03 mouse, 04 vendor data.
  No vendor command or mode change is needed for this demo.

## Required behavior
1. Open the evdev keyboard interface and obtain EVIOCGRAB so F2 cannot reach
   other applications. Fail clearly if the grab fails.
2. Start idle, with ALSA capture closed. On F2 down, stop any ongoing playback,
   open/configure/start capture and create a unique recording in ./recordings.
3. On F2 up, stop accepting capture frames immediately, drop and close capture,
   finalize the WAV header, then play the file if it contains audio.
4. Keep reading input during both capture and playback. A new press interrupts
   playback and starts a new recording. Do not replay files with no audio.
5. EIO can race with the release event. Preserve captured frames, close capture,
   and wait for release before playing; do not exit or restart capture repeatedly
   while the same press remains active. If release arrives first, finalize once.
6. On device removal, stop audio and finalize any recording, report the removal
   and exit cleanly. Never leave a recording/transmitting state stuck active.
7. Ctrl+C terminates from every state, releases the grab and closes resources.
   Use a signal-safe notification mechanism; no ALSA calls in signal handlers.

## Implementation
- Prefer a single poll-based event loop with nonblocking ALSA and evdev.
  Incorporate ALSA poll descriptors correctly; never block input handling in
  snd_pcm_readi, snd_pcm_writei, playback subprocesses or drain calls.
- A capture/playback worker design is acceptable only if ownership and shutdown
  are explicit. Avoid concurrently closing an ALSA handle used by another thread.
- Handle EAGAIN, EINTR, partial reads/writes and ALSA xruns. Recover EPIPE when
  appropriate; do not blindly recover/retry capture EIO after hardware release.
- Account for short taps: release can arrive before capture yields any frames.
- If startup has a brief transient failure, permit a bounded retry only while
  PTT is still held and without delaying processing of input events.
- Write valid little-endian PCM RIFF/WAV files, updating sizes on finalization.
- Discover the matching keyboard interface through sysfs USB ancestry and
  capabilities (KEY_F2). Do not identify it solely by name or hardcode event18.
  If multiple matching units exist, require --input selection rather than guess.
- Offer --input /dev/input/eventN, --capture PCM, --playback PCM, --output-dir DIR
  and --help. Default audio names are those above. Explain that duplicate ALSA
  card IDs may require overrides if several units are connected.
- Keep WAV recordings bounded (for example a configurable 60-second maximum).
  At the limit, finalize capture and wait for release before playback.
- Print clear state transitions, recording path/duration and actionable errors.
- Do not change system configuration, microphone firmware or HID settings.

## Deliverables
- C source, Makefile, and concise README with install/build/run instructions.
- Build dependencies: build-essential, libasound2-dev; evtest for diagnostics.
- Build using warnings such as -Wall -Wextra -Wpedantic and link libasound.
- README run example using sudo for access; do not install broad input-device
  permissions or services automatically. Run as a normal user when existing
  device permissions allow it.
- Document optional restrictive udev permissions separately if useful.

## Verification
Compile and fix warnings. Verify WAV header/finalization with a small meaningful
check, including empty and short recordings. Do not claim hardware testing if
hardware is unavailable. Provide a manual checklist covering: long hold, short
press, repeated presses, playback interruption, EIO on release, unplug during
capture/playback, and Ctrl+C. The user will perform hardware checks on desk.

## Scope
Implement only this local record-on-press / playback-on-release demo. Do not add
UDPPTT integration, encryption, GUI, Windows support or DLL reverse engineering.
