"""Project model and the GJXML file format.

GJXML 1.0 example::

    <?xml version="1.0" encoding="UTF-8"?>
    <gjxml version="1.0" generator="GarageJam 8.0.0">
      <project name="My Song" tempo="120" beats-per-bar="4" beat-unit="4"
               sample-rate="48000">
        <metadata>
          <title>My Song</title>
          <artist>Ole</artist>
          ...
        </metadata>
        <tracks>
          <track id="t1" name="Vocals" color="#3584e4" volume="1.0" pan="0.0"
                 mute="false" solo="false" armed="true">
            <clip id="c1" name="Take 1" src="My Song-audio/take-001.wav"
                  start="0.0" inpoint="0.0" duration="12.5" gain="1.0"/>
          </track>
        </tracks>
      </project>
    </gjxml>

All times are in seconds.  ``src`` is relative to the .gjxml file when the
audio lives below the project's folder, absolute otherwise.
"""

from __future__ import annotations

import os
import uuid
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field

from . import GJXML_VERSION, VERSION

TRACK_COLORS = [
    "#3584e4",  # blue
    "#e66100",  # orange
    "#2ec27e",  # green
    "#c061cb",  # purple
    "#f5c211",  # yellow
    "#e01b24",  # red
    "#1c71d8",  # dark blue
    "#865e3c",  # brown
]

METADATA_KEYS = ("title", "artist", "album", "genre", "comment")
EPSILON = 1e-6


def new_id(prefix: str) -> str:
    return f"{prefix}{uuid.uuid4().hex[:8]}"


@dataclass
class Clip:
    path: str
    start: float = 0.0
    inpoint: float = 0.0
    duration: float = 0.0
    name: str = ""
    gain: float = 1.0
    id: str = field(default_factory=lambda: new_id("c"))
    # Runtime only: length of the underlying audio file (filled by the engine).
    source_duration: float | None = field(default=None, compare=False)

    @property
    def end(self) -> float:
        return self.start + self.duration

    def copy(self) -> "Clip":
        return Clip(self.path, self.start, self.inpoint, self.duration,
                    self.name, self.gain, new_id("c"), self.source_duration)


@dataclass
class Track:
    name: str
    color: str = TRACK_COLORS[0]
    volume: float = 1.0
    pan: float = 0.0
    mute: bool = False
    solo: bool = False
    armed: bool = False
    clips: list[Clip] = field(default_factory=list)
    id: str = field(default_factory=lambda: new_id("t"))

    def overlaps(self, start: float, end: float, exclude: Clip | None = None) -> bool:
        for c in self.clips:
            if c is exclude:
                continue
            if start < c.end - EPSILON and end > c.start + EPSILON:
                return True
        return False

    def neighbours(self, clip: Clip) -> tuple[float, float]:
        """Free space around ``clip``: (earliest start, latest end)."""
        lo, hi = 0.0, float("inf")
        for c in self.clips:
            if c is clip:
                continue
            if c.end <= clip.start + EPSILON:
                lo = max(lo, c.end)
            elif c.start >= clip.end - EPSILON:
                hi = min(hi, c.start)
        return lo, hi

    def sort(self) -> None:
        self.clips.sort(key=lambda c: c.start)


@dataclass
class Project:
    name: str = "Untitled Jam"
    tempo: float = 120.0
    beats_per_bar: int = 4
    beat_unit: int = 4
    sample_rate: int = 48000
    tracks: list[Track] = field(default_factory=list)
    metadata: dict[str, str] = field(default_factory=dict)
    path: str | None = None  # .gjxml file on disk, if saved

    # ---- queries -------------------------------------------------------
    @property
    def length(self) -> float:
        return max((c.end for t in self.tracks for c in t.clips), default=0.0)

    @property
    def beat_seconds(self) -> float:
        return 60.0 / self.tempo

    @property
    def bar_seconds(self) -> float:
        return self.beat_seconds * self.beats_per_bar

    def snap(self, t: float, division: int = 1) -> float:
        step = self.beat_seconds / division
        return max(0.0, round(t / step) * step)

    def bar_beat(self, t: float) -> tuple[int, int, int]:
        """1-based bar, beat and 0-99 ticks for a time in seconds."""
        beats = t / self.beat_seconds
        bar = int(beats // self.beats_per_bar) + 1
        beat = int(beats % self.beats_per_bar) + 1
        ticks = int((beats - int(beats)) * 100)
        return bar, beat, ticks

    def track_for_clip(self, clip: Clip) -> Track | None:
        for t in self.tracks:
            if clip in t.clips:
                return t
        return None

    def track_by_id(self, track_id: str) -> Track | None:
        return next((t for t in self.tracks if t.id == track_id), None)

    @property
    def any_solo(self) -> bool:
        return any(t.solo for t in self.tracks)

    def audible(self, track: Track) -> bool:
        if track.mute:
            return False
        return track.solo or not self.any_solo

    # ---- edits ---------------------------------------------------------
    def add_track(self, name: str | None = None) -> Track:
        color = TRACK_COLORS[len(self.tracks) % len(TRACK_COLORS)]
        track = Track(name or f"Track {len(self.tracks) + 1}", color=color)
        self.tracks.append(track)
        return track

    def split_clip(self, clip: Clip, at: float) -> Clip | None:
        if not (clip.start + 0.01 < at < clip.end - 0.01):
            return None
        track = self.track_for_clip(clip)
        right = clip.copy()
        offset = at - clip.start
        right.start = at
        right.inpoint = clip.inpoint + offset
        right.duration = clip.duration - offset
        clip.duration = offset
        track.clips.append(right)
        track.sort()
        return right

    def audio_dir(self) -> str:
        """Folder where new recordings go."""
        if self.path:
            base = os.path.splitext(self.path)[0] + "-audio"
        else:
            from gi.repository import GLib
            music = GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_MUSIC) \
                or os.path.expanduser("~/Music")
            base = os.path.join(music, "GarageJam", _safe(self.name))
        os.makedirs(base, exist_ok=True)
        return base


def _safe(name: str) -> str:
    keep = "".join(ch if ch.isalnum() or ch in " -_." else "_" for ch in name).strip()
    return keep or "Untitled"


# ---------------------------------------------------------------------------
# GJXML
# ---------------------------------------------------------------------------

class GJXMLError(Exception):
    pass


def _b(value: bool) -> str:
    return "true" if value else "false"


def _parse_bool(value: str | None, default: bool = False) -> bool:
    if value is None:
        return default
    return value.strip().lower() in ("1", "true", "yes", "on")


def _f(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".") if value else "0"


def to_gjxml(project: Project, path: str | None = None) -> str:
    base_dir = os.path.dirname(os.path.abspath(path)) if path else None
    root = ET.Element("gjxml", version=GJXML_VERSION, generator=f"GarageJam {VERSION}")
    p = ET.SubElement(root, "project", {
        "name": project.name,
        "tempo": _f(project.tempo),
        "beats-per-bar": str(project.beats_per_bar),
        "beat-unit": str(project.beat_unit),
        "sample-rate": str(project.sample_rate),
    })
    meta = ET.SubElement(p, "metadata")
    for key in METADATA_KEYS:
        if project.metadata.get(key):
            ET.SubElement(meta, key).text = project.metadata[key]
    tracks = ET.SubElement(p, "tracks")
    for t in project.tracks:
        te = ET.SubElement(tracks, "track", {
            "id": t.id, "name": t.name, "color": t.color,
            "volume": _f(t.volume), "pan": _f(t.pan),
            "mute": _b(t.mute), "solo": _b(t.solo), "armed": _b(t.armed),
        })
        for c in sorted(t.clips, key=lambda c: c.start):
            src = os.path.abspath(c.path)
            if base_dir:
                rel = os.path.relpath(src, base_dir)
                if not rel.startswith(".."):
                    src = rel
            ET.SubElement(te, "clip", {
                "id": c.id, "name": c.name, "src": src.replace(os.sep, "/"),
                "start": _f(c.start), "inpoint": _f(c.inpoint),
                "duration": _f(c.duration), "gain": _f(c.gain),
            })
    ET.indent(root, space="  ")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n"


def save_gjxml(project: Project, path: str) -> None:
    data = to_gjxml(project, path)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(data)
    os.replace(tmp, path)  # atomic: never leave a half-written project
    project.path = path


def from_gjxml(text: str, path: str | None = None) -> Project:
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        raise GJXMLError(f"Not valid XML: {exc}") from exc
    if root.tag != "gjxml":
        raise GJXMLError("Not a GJXML file (root element must be <gjxml>)")
    major = root.get("version", "1.0").split(".")[0]
    if major != GJXML_VERSION.split(".")[0]:
        raise GJXMLError(f"Unsupported GJXML version {root.get('version')}")
    p = root.find("project")
    if p is None:
        raise GJXMLError("Missing <project> element")

    base_dir = os.path.dirname(os.path.abspath(path)) if path else os.getcwd()

    def num(el, key, default, cast=float):
        try:
            return cast(el.get(key, default))
        except (TypeError, ValueError):
            return cast(default)

    project = Project(
        name=p.get("name", "Untitled Jam"),
        tempo=min(300.0, max(20.0, num(p, "tempo", 120.0))),
        beats_per_bar=max(1, num(p, "beats-per-bar", 4, int)),
        beat_unit=max(1, num(p, "beat-unit", 4, int)),
        sample_rate=num(p, "sample-rate", 48000, int),
        path=path,
    )
    meta = p.find("metadata")
    if meta is not None:
        for key in METADATA_KEYS:
            el = meta.find(key)
            if el is not None and el.text:
                project.metadata[key] = el.text
    tracks = p.find("tracks")
    for i, te in enumerate(tracks.findall("track") if tracks is not None else []):
        track = Track(
            name=te.get("name", f"Track {i + 1}"),
            color=te.get("color", TRACK_COLORS[i % len(TRACK_COLORS)]),
            volume=min(2.0, max(0.0, num(te, "volume", 1.0))),
            pan=min(1.0, max(-1.0, num(te, "pan", 0.0))),
            mute=_parse_bool(te.get("mute")),
            solo=_parse_bool(te.get("solo")),
            armed=_parse_bool(te.get("armed")),
            id=te.get("id") or new_id("t"),
        )
        for ce in te.findall("clip"):
            src = ce.get("src")
            if not src:
                continue
            src = src.replace("/", os.sep)
            if not os.path.isabs(src):
                src = os.path.normpath(os.path.join(base_dir, src))
            track.clips.append(Clip(
                path=src,
                start=max(0.0, num(ce, "start", 0.0)),
                inpoint=max(0.0, num(ce, "inpoint", 0.0)),
                duration=max(0.0, num(ce, "duration", 0.0)),
                name=ce.get("name", os.path.basename(src)),
                gain=max(0.0, num(ce, "gain", 1.0)),
                id=ce.get("id") or new_id("c"),
            ))
        track.sort()
        project.tracks.append(track)
    return project


def load_gjxml(path: str) -> Project:
    with open(path, encoding="utf-8") as fh:
        return from_gjxml(fh.read(), path)
