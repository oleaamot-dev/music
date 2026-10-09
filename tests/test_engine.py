"""Headless engine tests (no sound card needed)."""

import os

import pytest

os.environ.setdefault("GARAGEJAM_AUDIO_SINK", "fakesink sync=true")
os.environ.setdefault("GARAGEJAM_AUDIO_SRC", "audiotestsrc is-live=true")

from garagejam import engine as E  # noqa: E402  (selects GStreamer 1.0)
from gi.repository import GLib, Gst, GstPbutils  # noqa: E402
from garagejam.project import Clip, Project  # noqa: E402

E.init()


def tone(path, freq=440, buffers=100):
    pipe = Gst.parse_launch(f"audiotestsrc freq={freq} num-buffers={buffers} ! audioconvert ! "
                            f"wavenc ! filesink location={path}")
    pipe.set_state(Gst.State.PLAYING)
    pipe.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS)
    pipe.set_state(Gst.State.NULL)
    return str(path)


@pytest.fixture
def project(tmp_path):
    p = Project(name="Test")
    a, b = p.add_track("A"), p.add_track("B")
    a.clips.append(Clip(tone(tmp_path / "a.wav"), start=0.5))       # ~2.32 s
    b.clips.append(Clip(tone(tmp_path / "b.wav", 660, 50), start=2.0))
    p.metadata["artist"] = "Tester"
    return p


def discover(path):
    return GstPbutils.Discoverer.new(10 * Gst.SECOND).discover_uri(Gst.filename_to_uri(path))


def test_sync_fills_durations_and_follows_edits(project):
    m = E.TimelineMirror(padding=False)
    m.sync(project)
    a, b = project.tracks
    assert a.clips[0].duration == pytest.approx(2.32, abs=0.01)
    # move clip from B to A and trim it
    clip = b.clips.pop()
    clip.start, clip.duration = 4.0, 0.5
    a.clips.append(clip)
    m.sync(project)
    layers = m.timeline.get_layers()
    starts = sorted(c.get_start() / Gst.SECOND for c in layers[0].get_clips())
    assert starts == [0.5, 4.0] and not layers[1].get_clips()


@pytest.mark.parametrize("fmt", ["wav", "ogg"])
def test_export(project, tmp_path, fmt):
    out = str(tmp_path / f"mix.{fmt}")
    ok, detail = E.Exporter(project, out, fmt).run_sync()
    assert ok, detail
    info = discover(out)
    assert to_s(info.get_duration()) == pytest.approx(project.length, abs=0.1)
    if fmt == "ogg":
        tags = info.get_audio_streams()[0].get_tags()
        assert tags.get_string(Gst.TAG_ARTIST)[1] == "Tester"


def to_s(ns):
    return ns / Gst.SECOND


def test_peaks(project):
    peaks = E.compute_peaks(project.tracks[0].clips[0].path)
    assert len(peaks) == pytest.approx(232, abs=3)
    assert 0.5 < max(peaks) <= 1.0


def test_record_overdub(project, tmp_path):
    eng = E.Engine()
    eng.load(project)
    loop = GLib.MainLoop()
    result = {}
    eng.connect("recording-finished", lambda _e, path, start: (result.update(path=path, start=start), loop.quit()))
    eng.connect("error", lambda _e, msg: (result.update(error=msg), loop.quit()))
    eng.seek(1.0)
    assert eng.start_recording(str(tmp_path / "take.wav"))
    GLib.timeout_add(1200, lambda: eng.stop_recording() and False)
    GLib.timeout_add(10000, loop.quit)
    loop.run()
    eng.shutdown()
    assert "error" not in result
    assert result["start"] == pytest.approx(1.0, abs=0.2)
    assert to_s(discover(result["path"]).get_duration()) == pytest.approx(1.2, abs=0.4)
