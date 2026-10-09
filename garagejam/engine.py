"""Audio engine built on GStreamer and GStreamer Editing Services (GES).

* Every GarageJam track is a GES layer; every clip a GES.UriClip with a
  ``volume ! audiopanorama`` effect that carries the track's mix settings.
* A muted test clip on a hidden bottom layer keeps the timeline "running" past
  the last clip so the transport can play (and record) on an empty project.
* Recording runs in a separate capture pipeline (source -> level -> wavenc)
  while the timeline plays, so you can overdub.
* Export renders the same project through a fresh GES pipeline in render mode.
"""

from __future__ import annotations

import os
import threading
from array import array

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GES", "1.0")
gi.require_version("GstPbutils", "1.0")
from gi.repository import GES, GLib, GObject, Gst, GstPbutils  # noqa: E402

from .project import Clip, Project  # noqa: E402

SECOND = Gst.SECOND
PEAKS_PER_SECOND = 100
PADDING_SECONDS = 600

_initialised = False


def init() -> None:
    global _initialised
    if not _initialised:
        Gst.init(None)
        GES.init()
        _initialised = True


def to_ns(seconds: float) -> int:
    return max(0, int(round(seconds * SECOND)))


def to_s(ns: int) -> float:
    return ns / SECOND


def db_to_linear(db: float) -> float:
    if db <= -90:
        return 0.0
    return min(1.0, 10 ** (db / 20.0))


EXPORT_FORMATS = {
    # key: (label, extension, container caps, audio caps)
    "wav": ("WAV (lossless)", "wav", "audio/x-wav", "audio/x-raw"),
    "ogg": ("Ogg Vorbis", "ogg", "application/ogg", "audio/x-vorbis"),
}


# ---------------------------------------------------------------------------
# Project -> GES timeline mirror
# ---------------------------------------------------------------------------

class TimelineMirror:
    """Keeps a GES.Timeline in step with a :class:`Project`, incrementally."""

    def __init__(self, padding: bool = True):
        self.timeline = GES.Timeline.new()
        self.track = GES.AudioTrack.new()
        self.timeline.add_track(self.track)
        self._layers: dict[str, GES.Layer] = {}
        self._clips: dict[str, tuple[GES.Clip, GES.Effect]] = {}
        self._assets: dict[str, GES.UriClipAsset] = {}
        self.missing: set[str] = set()
        self._pad_layer = None
        self._pad_clip = None
        if padding:
            self._pad_layer = self.timeline.append_layer()
            self._pad_clip = GES.TestClip.new()
            self._pad_clip.set_mute(True)
            self._pad_clip.set_duration(to_ns(PADDING_SECONDS))
            self._pad_layer.add_clip(self._pad_clip)

    # -- assets --
    def asset(self, path: str) -> GES.UriClipAsset | None:
        if path in self._assets:
            return self._assets[path]
        if not os.path.exists(path):
            self.missing.add(path)
            return None
        try:
            a = GES.UriClipAsset.request_sync(Gst.filename_to_uri(path))
        except GLib.Error:
            self.missing.add(path)
            return None
        self._assets[path] = a
        self.missing.discard(path)
        return a

    def forget_asset(self, path: str) -> None:
        self._assets.pop(path, None)

    # -- sync --
    def sync(self, project: Project) -> None:
        wanted = {t.id for t in project.tracks}
        for tid in list(self._layers):
            if tid not in wanted:
                layer = self._layers.pop(tid)
                for cid, (gc, _e) in list(self._clips.items()):
                    if gc.get_layer() is layer:
                        del self._clips[cid]
                self.timeline.remove_layer(layer)

        for i, t in enumerate(project.tracks):
            layer = self._layers.get(t.id)
            if layer is None:
                layer = self.timeline.append_layer()
                self._layers[t.id] = layer
            if layer.get_priority() != i:
                self.timeline.move_layer(layer, i)
        if self._pad_layer is not None:
            last = len(project.tracks)
            if self._pad_layer.get_priority() != last:
                self.timeline.move_layer(self._pad_layer, last)

        seen: set[str] = set()
        retry: list[tuple[Clip, GES.Layer]] = []
        for t in project.tracks:
            layer = self._layers[t.id]
            for c in t.clips:
                seen.add(c.id)
                entry = self._clips.get(c.id)
                if entry is None:
                    retry.append((c, layer))
                    continue
                gc, _effect = entry
                ok = True
                if gc.get_layer() is not layer:
                    ok = gc.move_to_layer(layer)
                start, inp, dur = to_ns(c.start), to_ns(c.inpoint), to_ns(c.duration)
                if ok and gc.get_duration() > dur:
                    ok = gc.set_duration(dur)
                if ok and gc.get_inpoint() != inp:
                    ok = gc.set_inpoint(inp)
                if ok and gc.get_duration() != dur:
                    ok = gc.set_duration(dur)
                if ok and gc.get_start() != start:
                    ok = gc.set_start(start)
                if not ok:  # GES refused an intermediate state: rebuild it
                    gc.get_layer().remove_clip(gc)
                    del self._clips[c.id]
                    retry.append((c, layer))

        for cid in list(self._clips):
            if cid not in seen:
                gc, _e = self._clips.pop(cid)
                if gc.get_layer() is not None:
                    gc.get_layer().remove_clip(gc)

        for c, layer in retry:
            self._add_clip(c, layer)

        if self._pad_clip is not None:
            self._pad_clip.set_duration(to_ns(project.length + PADDING_SECONDS))
        self.update_mix(project)
        self.timeline.commit()

    def _add_clip(self, c: Clip, layer: GES.Layer) -> None:
        asset = self.asset(c.path)
        if asset is None:
            return
        src_len = to_s(asset.get_duration())
        c.source_duration = src_len
        c.inpoint = min(c.inpoint, max(0.0, src_len - 0.01))
        if c.duration <= 0 or c.inpoint + c.duration > src_len + 1e-3:
            c.duration = max(0.01, src_len - c.inpoint)
        gc = layer.add_asset(asset, to_ns(c.start), to_ns(c.inpoint),
                             to_ns(c.duration), GES.TrackType.AUDIO)
        if gc is None:
            return
        effect = GES.Effect.new("volume name=gjvol ! audiopanorama name=gjpan")
        gc.add_top_effect(effect, 0)
        self._clips[c.id] = (gc, effect)

    def update_mix(self, project: Project) -> None:
        for t in project.tracks:
            audible = project.audible(t)
            for c in t.clips:
                entry = self._clips.get(c.id)
                if entry is None:
                    continue
                _gc, effect = entry
                effect.set_child_property("volume", float(t.volume * c.gain))
                effect.set_child_property("mute", not audible)
                effect.set_child_property("panorama", float(t.pan))


# ---------------------------------------------------------------------------
# Engine: transport, metering, recording
# ---------------------------------------------------------------------------

class Engine(GObject.Object):
    __gtype_name__ = "GarageJamEngine"
    __gsignals__ = {
        "state-changed": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
        "tick": (GObject.SignalFlags.RUN_FIRST, None, (float,)),
        "master-level": (GObject.SignalFlags.RUN_FIRST, None, (float, float)),
        "input-level": (GObject.SignalFlags.RUN_FIRST, None, (float,)),
        "recording-finished": (GObject.SignalFlags.RUN_FIRST, None, (str, float)),
        "error": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
    }

    def __init__(self):
        super().__init__()
        init()
        self.mirror = TimelineMirror(padding=True)
        self.pipeline = GES.Pipeline()
        self.pipeline.set_timeline(self.mirror.timeline)

        sink = os.environ.get("GARAGEJAM_AUDIO_SINK", "autoaudiosink")
        self._out = Gst.parse_bin_from_description(
            "level name=masterlevel interval=50000000 post-messages=true ! "
            f"audioconvert ! audioresample ! {sink}", True)
        self.pipeline.preview_set_audio_sink(self._out)

        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_message)

        self.state = "stopped"
        self._tick_id = 0
        self._rec = None
        self._rec_path = None
        self._rec_start = 0.0
        self._last_pos = 0.0
        self.pipeline.set_state(Gst.State.PAUSED)

    # -- project --
    def load(self, project: Project) -> None:
        self.mirror.sync(project)

    def update_mix(self, project: Project) -> None:
        self.mirror.update_mix(project)

    # -- transport --
    def position(self) -> float:
        ok, pos = self.pipeline.query_position(Gst.Format.TIME)
        if ok and pos >= 0:
            self._last_pos = to_s(pos)
        return self._last_pos

    def seek(self, seconds: float) -> None:
        seconds = max(0.0, seconds)
        self._last_pos = seconds
        self.pipeline.seek_simple(Gst.Format.TIME,
                                  Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                                  to_ns(seconds))
        self.emit("tick", seconds)

    def play(self) -> None:
        if self.state in ("playing", "recording"):
            return
        self.pipeline.set_state(Gst.State.PLAYING)
        self._set_state("playing")

    def pause(self) -> None:
        if self.state == "recording":
            self.stop_recording()
            return
        self.pipeline.set_state(Gst.State.PAUSED)
        self._set_state("paused")

    def stop(self) -> None:
        """First press stops in place; a second press returns to zero."""
        if self.state == "recording":
            self.stop_recording()
            return
        if self.state in ("stopped",):
            self.seek(0.0)
        self.pipeline.set_state(Gst.State.PAUSED)
        self._set_state("stopped")

    def _set_state(self, state: str) -> None:
        self.state = state
        running = state in ("playing", "recording")
        if running and not self._tick_id:
            self._tick_id = GLib.timeout_add(33, self._on_tick)
        elif not running and self._tick_id:
            GLib.source_remove(self._tick_id)
            self._tick_id = 0
            self.emit("tick", self.position())
        self.emit("state-changed", state)

    def _on_tick(self) -> bool:
        self.emit("tick", self.position())
        return True

    def _on_message(self, _bus, msg: Gst.Message) -> None:
        t = msg.type
        if t == Gst.MessageType.ELEMENT:
            s = msg.get_structure()
            if s and s.get_name() == "level" and msg.src.get_name() == "masterlevel":
                peaks = list(s.get_value("peak"))
                left = db_to_linear(peaks[0]) if peaks else 0.0
                right = db_to_linear(peaks[1]) if len(peaks) > 1 else left
                self.emit("master-level", left, right)
        elif t == Gst.MessageType.EOS:
            if self.state == "recording":
                self.stop_recording()
            self.pipeline.set_state(Gst.State.PAUSED)
            self._set_state("stopped")
        elif t == Gst.MessageType.ERROR:
            err, _dbg = msg.parse_error()
            self.emit("error", err.message)
            self.pipeline.set_state(Gst.State.PAUSED)
            self._set_state("stopped")

    # -- recording --
    def start_recording(self, path: str, sample_rate: int = 48000) -> bool:
        if self._rec is not None:
            return False
        src = os.environ.get("GARAGEJAM_AUDIO_SRC", "autoaudiosrc")
        try:
            rec = Gst.parse_launch(
                f"{src} ! queue ! audioconvert ! audioresample ! "
                f"audio/x-raw,rate={sample_rate},channels=2 ! "
                "level name=inlevel interval=50000000 post-messages=true ! "
                "wavenc ! filesink name=recsink")
        except GLib.Error as exc:
            self.emit("error", f"Cannot open the audio input: {exc.message}")
            return False
        rec.get_by_name("recsink").set_property("location", path)

        # Align the take with the timeline: note the playhead at the moment
        # the first captured buffer actually arrives.
        self._rec_start = self.position()
        level = rec.get_by_name("inlevel")

        def first_buffer(_pad, _info):
            self._rec_start = self.position()
            return Gst.PadProbeReturn.REMOVE

        level.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, first_buffer)

        bus = rec.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_rec_message)
        if rec.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            rec.set_state(Gst.State.NULL)
            self.emit("error", "Cannot start recording — is a microphone or sound card connected?")
            return False
        self._rec, self._rec_path = rec, path
        self.pipeline.set_state(Gst.State.PLAYING)
        self._set_state("recording")
        return True

    def stop_recording(self) -> None:
        if self._rec is None:
            return
        self._rec.send_event(Gst.Event.new_eos())
        self.pipeline.set_state(Gst.State.PAUSED)
        self._set_state("paused")

    def _finish_recording(self, ok: bool) -> None:
        rec, path, start = self._rec, self._rec_path, self._rec_start
        if rec is None:
            return
        rec.get_bus().remove_signal_watch()
        rec.set_state(Gst.State.NULL)
        self._rec = self._rec_path = None
        self.emit("input-level", 0.0)
        if ok and path and os.path.exists(path) and os.path.getsize(path) > 44:
            self.mirror.forget_asset(path)
            self.emit("recording-finished", path, start)

    def _on_rec_message(self, _bus, msg: Gst.Message) -> None:
        t = msg.type
        if t == Gst.MessageType.ELEMENT:
            s = msg.get_structure()
            if s and s.get_name() == "level":
                peaks = list(s.get_value("peak"))
                self.emit("input-level", max((db_to_linear(p) for p in peaks), default=0.0))
        elif t == Gst.MessageType.EOS:
            self._finish_recording(True)
        elif t == Gst.MessageType.ERROR:
            err, _dbg = msg.parse_error()
            self.emit("error", f"Recording failed: {err.message}")
            self._finish_recording(True)  # keep whatever was captured
            if self.state == "recording":
                self.pipeline.set_state(Gst.State.PAUSED)
                self._set_state("paused")

    @property
    def recording_start(self) -> float:
        return self._rec_start

    def shutdown(self) -> None:
        if self._rec is not None:
            self._rec.send_event(Gst.Event.new_eos())
            self._rec.get_bus().timed_pop_filtered(2 * SECOND, Gst.MessageType.EOS)
            self._finish_recording(True)
        self.pipeline.set_state(Gst.State.NULL)


# ---------------------------------------------------------------------------
# Export (mixdown)
# ---------------------------------------------------------------------------

class Exporter(GObject.Object):
    __gtype_name__ = "GarageJamExporter"
    __gsignals__ = {
        "progress": (GObject.SignalFlags.RUN_FIRST, None, (float,)),
        "finished": (GObject.SignalFlags.RUN_FIRST, None, (bool, str)),
    }

    def __init__(self, project: Project, path: str, fmt: str = "wav"):
        super().__init__()
        init()
        self.path = path
        self.length = project.length
        self.mirror = TimelineMirror(padding=False)
        self.mirror.sync(project)
        self.mirror.timeline.commit_sync()

        _label, _ext, container, audio = EXPORT_FORMATS[fmt]
        profile = GstPbutils.EncodingContainerProfile.new(
            "garagejam", None, Gst.Caps.from_string(container), None)
        profile.add_profile(GstPbutils.EncodingAudioProfile.new(
            Gst.Caps.from_string(audio), None,
            Gst.Caps.from_string(f"audio/x-raw,rate={project.sample_rate}"), 0))

        self.pipeline = GES.Pipeline()
        self.pipeline.set_timeline(self.mirror.timeline)
        self.pipeline.set_render_settings(Gst.filename_to_uri(path), profile)
        self.pipeline.set_mode(GES.PipelineFlags.RENDER)

        self._tags = Gst.TagList.new_empty()
        meta = dict(project.metadata)
        meta.setdefault("title", project.name)
        for key, tag in (("title", Gst.TAG_TITLE), ("artist", Gst.TAG_ARTIST),
                         ("album", Gst.TAG_ALBUM), ("genre", Gst.TAG_GENRE),
                         ("comment", Gst.TAG_COMMENT)):
            if meta.get(key):
                self._tags.add_value(Gst.TagMergeMode.REPLACE, tag, meta[key])
        self._tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_APPLICATION_NAME, "GarageJam")
        self._timer = 0
        self._done = False

    def _apply_tags(self) -> None:
        """Hand the project metadata to every tag-capable encoder.

        Encoders build their headers when the first buffer arrives, so a
        tag event is injected right in front of it.
        """
        it = self.pipeline.iterate_all_by_interface(Gst.TagSetter)
        while True:
            res, element = it.next()
            if res != Gst.IteratorResult.OK:
                break
            if getattr(element, "_gj_tagged", False):
                continue
            element._gj_tagged = True
            pad = element.get_static_pad("sink")

            def before_first_buffer(sink_pad, _info):
                # A downstream TAG event is the one route every encoder
                # honours (setter tags get reset when encoders start).
                sink_pad.send_event(Gst.Event.new_tag(self._tags.copy()))
                return Gst.PadProbeReturn.REMOVE

            if pad is not None:
                pad.add_probe(Gst.PadProbeType.BUFFER, before_first_buffer)

    def start(self) -> None:
        self._apply_tags()
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_message)
        self.pipeline.set_state(Gst.State.PLAYING)
        self._timer = GLib.timeout_add(100, self._poll)

    def run_sync(self, timeout: float = 120.0) -> tuple[bool, str]:
        """Blocking render (for tests and command-line use)."""
        self._apply_tags()
        self.pipeline.set_state(Gst.State.PLAYING)
        msg = self.pipeline.get_bus().timed_pop_filtered(
            to_ns(timeout), Gst.MessageType.EOS | Gst.MessageType.ERROR)
        self.pipeline.set_state(Gst.State.NULL)
        if msg is None:
            return False, "Timed out"
        if msg.type == Gst.MessageType.ERROR:
            return False, msg.parse_error()[0].message
        return True, self.path

    def _poll(self) -> bool:
        ok, pos = self.pipeline.query_position(Gst.Format.TIME)
        if ok and self.length > 0:
            self.emit("progress", min(1.0, to_s(pos) / self.length))
        return not self._done

    def _on_message(self, bus, msg):
        if self._done:
            return
        if msg.type == Gst.MessageType.EOS:
            self._finish(True, self.path)
        elif msg.type == Gst.MessageType.ERROR:
            self._finish(False, msg.parse_error()[0].message)

    def _finish(self, ok: bool, detail: str) -> None:
        self._done = True
        self.pipeline.get_bus().remove_signal_watch()
        self.pipeline.set_state(Gst.State.NULL)
        self.emit("progress", 1.0)
        self.emit("finished", ok, detail)

    def cancel(self) -> None:
        if not self._done:
            self._finish(False, "Cancelled")


# ---------------------------------------------------------------------------
# Waveform peaks
# ---------------------------------------------------------------------------

class PeakCache(GObject.Object):
    """Computes waveform peaks (PEAKS_PER_SECOND per second) in a worker thread."""

    __gtype_name__ = "GarageJamPeakCache"
    __gsignals__ = {"ready": (GObject.SignalFlags.RUN_FIRST, None, (str,))}

    def __init__(self):
        super().__init__()
        init()
        self._peaks: dict[str, array] = {}
        self._pending: set[str] = set()

    def get(self, path: str) -> array | None:
        if path in self._peaks:
            return self._peaks[path]
        if path not in self._pending and os.path.exists(path):
            self._pending.add(path)
            threading.Thread(target=self._work, args=(path,), daemon=True).start()
        return None

    def invalidate(self, path: str) -> None:
        self._peaks.pop(path, None)

    def _work(self, path: str) -> None:
        peaks = compute_peaks(path)
        GLib.idle_add(self._done, path, peaks)

    def _done(self, path: str, peaks: array) -> bool:
        self._pending.discard(path)
        self._peaks[path] = peaks
        self.emit("ready", path)
        return False


def compute_peaks(path: str) -> array:
    init()
    interval = SECOND // PEAKS_PER_SECOND
    peaks = array("f")
    try:
        pipe = Gst.parse_launch(
            "filesrc name=src ! decodebin ! audioconvert ! "
            f"level name=lvl interval={interval} post-messages=true ! fakesink sync=false")
    except GLib.Error:
        return peaks
    pipe.get_by_name("src").set_property("location", path)
    bus = pipe.get_bus()
    pipe.set_state(Gst.State.PLAYING)
    while True:
        msg = bus.timed_pop_filtered(
            10 * SECOND, Gst.MessageType.ELEMENT | Gst.MessageType.EOS | Gst.MessageType.ERROR)
        if msg is None or msg.type != Gst.MessageType.ELEMENT:
            break
        s = msg.get_structure()
        if s and s.get_name() == "level":
            values = list(s.get_value("peak"))
            peaks.append(max((db_to_linear(v) for v in values), default=0.0))
    pipe.set_state(Gst.State.NULL)
    return peaks
