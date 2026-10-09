import os

import pytest

from garagejam.project import (Clip, GJXMLError, Project, from_gjxml, load_gjxml,
                               save_gjxml, to_gjxml)


def make_project(tmp_path):
    p = Project(name="Song", tempo=100)
    a = p.add_track("Drums")
    b = p.add_track("Bass")
    a.clips.append(Clip(str(tmp_path / "Song-audio" / "a.wav"), start=1, duration=2, name="A"))
    b.clips.append(Clip("/elsewhere/b.wav", start=0.5, inpoint=0.25, duration=3, gain=0.8))
    b.mute, a.solo, a.pan, a.volume = True, True, -0.5, 0.7
    p.metadata = {"artist": "Ole", "title": "Song"}
    return p


def test_roundtrip(tmp_path):
    p = make_project(tmp_path)
    path = str(tmp_path / "song.gjxml")
    save_gjxml(p, path)
    q = load_gjxml(path)
    assert q.name == "Song" and q.tempo == 100 and q.metadata == p.metadata
    assert [t.name for t in q.tracks] == ["Drums", "Bass"]
    assert q.tracks[0].solo and q.tracks[1].mute and q.tracks[0].pan == -0.5
    assert [(c.path, c.start, c.inpoint, c.duration, c.gain) for t in q.tracks for c in t.clips] == \
        [(c.path, c.start, c.inpoint, c.duration, c.gain) for t in p.tracks for c in t.clips]


def test_paths_relative_inside_project_absolute_outside(tmp_path):
    p = make_project(tmp_path)
    text = to_gjxml(p, str(tmp_path / "song.gjxml"))
    assert 'src="Song-audio/a.wav"' in text
    assert 'src="/elsewhere/b.wav"' in text


def test_bad_files():
    with pytest.raises(GJXMLError):
        from_gjxml("<notgjxml/>")
    with pytest.raises(GJXMLError):
        from_gjxml("not xml")
    with pytest.raises(GJXMLError):
        from_gjxml('<gjxml version="2.0"><project/></gjxml>')


def test_tolerant_values():
    p = from_gjxml('<gjxml version="1.0"><project tempo="abc"><tracks>'
                   '<track name="x" volume="9" pan="-4"><clip src="/a.wav" start="-1"/></track>'
                   '</tracks></project></gjxml>')
    t = p.tracks[0]
    assert p.tempo == 120 and t.volume == 2.0 and t.pan == -1.0 and t.clips[0].start == 0


def test_overlap_split_snap():
    p = Project(tempo=120)
    t = p.add_track()
    c = Clip("/a.wav", start=1, duration=4)
    t.clips.append(c)
    assert t.overlaps(4, 6) and not t.overlaps(5, 6) and not t.overlaps(0, 1)
    right = p.split_clip(c, 3)
    assert (c.start, c.duration) == (1, 2)
    assert (right.start, right.inpoint, right.duration) == (3, 2, 2)
    assert p.split_clip(c, 10) is None
    assert t.neighbours(c) == (0.0, 3)
    assert p.snap(0.26) == 0.5 and p.bar_beat(2.75) == (2, 2, 50)


def test_solo_logic():
    p = Project()
    a, b = p.add_track(), p.add_track()
    assert p.audible(a) and p.audible(b)
    b.solo = True
    assert not p.audible(a) and p.audible(b)
    b.mute = True
    assert not p.audible(b)
