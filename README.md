# GarageJam 8.0 (Python)

Multitrack music studio for GNOME, written in Python 3 with GTK 4, libadwaita
and GStreamer (including GStreamer Editing Services for the timeline).

## Features

- **Multitrack arrange view** — ruler in bars, beat grid, waveforms, playhead,
  autoscroll, zoom (Ctrl+scroll, Ctrl+±, zoom-to-fit).
- **Clip editing** — drag clips in time and between tracks (snaps to half-beats,
  hold Shift to move freely), trim either edge, split at the playhead (S),
  duplicate (Ctrl+D), delete. Overlaps on a track are refused.
- **Tracks** — name, colour, mute / solo / record-arm, pan, volume (dB readout),
  reorder and delete.
- **Recording with overdub** — the timeline plays while the armed track
  records; the take lands where recording started (aligned to the first
  captured buffer). If the armed track already has audio there, the take goes
  on a new track right below. Live input meter and a growing red take region.
- **Import** audio files via dialog or drag-and-drop onto a lane.
- **Export mix** to WAV or Ogg Vorbis, with project metadata written as tags
  (Ogg).
- **GJXML 1.0** project files (atomic save; audio paths relative to the project
  when they live beside it). Recordings go to `<project>-audio/`, or
  `~/Music/GarageJam/<name>/` for an unsaved project.
- Master L/R meters, project settings (tempo, beats per bar, sample rate,
  metadata), unsaved-changes prompts, keyboard shortcuts window, dark mode.

## Requirements

Fedora:

    sudo dnf install python3-gobject python3-cairo gtk4 libadwaita \
        gstreamer1 gstreamer1-plugins-base gstreamer1-plugins-good \
        gstreamer1-plugins-bad-free gst-editing-services

Debian / Ubuntu:

    sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-4.0 gir1.2-adw-1 \
        gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 gir1.2-ges-1.0 \
        gstreamer1.0-plugins-base gstreamer1.0-plugins-good

libadwaita ≥ 1.5 and GTK ≥ 4.12 are needed.

## Running

    ./bin/garagejam                 # from the source tree
    ./bin/garagejam song.gjxml      # open a project
    pip install --user .  &&  garagejam

Install `data/org.garagejam.GarageJam.desktop` into
`~/.local/share/applications/` and the MIME file with
`xdg-mime install data/org.garagejam.GarageJam.mime.xml` to open `.gjxml`
files from Files.

Environment overrides (handy for testing or a specific device):

    GARAGEJAM_AUDIO_SINK="pulsesink"         # default: autoaudiosink
    GARAGEJAM_AUDIO_SRC="pipewiresrc"        # default: autoaudiosrc

## Keyboard

| Key | Action |
|---|---|
| Space | Play / pause |
| R | Record / stop recording |
| Esc | Stop (press twice to return to start) |
| Home | Go to start |
| S | Split selected clip at playhead |
| Delete | Delete selected clip |
| Ctrl+D / T / I / E | Duplicate clip / add track / import / export |
| Ctrl+S / O / N | Save / open / new |

## GJXML 1.0

```xml
<?xml version="1.0" encoding="UTF-8"?>
<gjxml version="1.0" generator="GarageJam 8.0.0">
  <project name="Demo Jam" tempo="96" beats-per-bar="4" beat-unit="4" sample-rate="48000">
    <metadata>
      <artist>Ole Kristian Aamot</artist>
    </metadata>
    <tracks>
      <track id="t1" name="Drums" color="#e66100" volume="0.9" pan="0"
             mute="false" solo="false" armed="false">
        <clip id="c1" name="Groove" src="Demo Jam-audio/take-1.wav"
              start="0" inpoint="0" duration="5" gain="1"/>
      </track>
    </tracks>
  </project>
</gjxml>
```

Times are seconds. `inpoint` is the offset into the source file, `gain` a
per-clip linear multiplier on top of the track volume. Unknown elements and
attributes are ignored on load; out-of-range values are clamped.

## Layout

    garagejam/
      project.py      model (Project / Track / Clip) + GJXML read/write
      engine.py       GES timeline mirror, transport, metering, recording,
                      export, waveform peaks
      timeline.py     arrange view (Cairo drawing, drag / trim / drop)
      trackheader.py  per-track controls
      window.py       main window, actions, dialogs
      app.py          Adw.Application, CSS
    tests/            model + headless engine tests (pytest)

## Tests

    python3 -m pytest

The engine tests run headless (fakesink / audiotestsrc), no sound card needed.
