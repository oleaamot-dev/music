"""The arrange view: ruler, lanes, clips with waveforms, playhead."""

from __future__ import annotations

import math

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_foreign("cairo")  # needs python3-gi-cairo / pycairo
from gi.repository import Adw, Gdk, GObject, Gtk  # noqa: E402

from .engine import PEAKS_PER_SECOND, PeakCache  # noqa: E402
from .project import Clip, Project, Track  # noqa: E402

RULER_H = 30
ROW_H = 112
HANDLE = 7
MIN_CLIP = 0.05
MIN_PPS, MAX_PPS = 4.0, 800.0


def hex_rgb(color: str) -> tuple[float, float, float]:
    rgba = Gdk.RGBA()
    if not rgba.parse(color):
        rgba.parse("#3584e4")
    return rgba.red, rgba.green, rgba.blue


def rounded_rect(cr, x, y, w, h, r):
    r = min(r, w / 2, h / 2)
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    cr.close_path()


class TimelineView(Gtk.DrawingArea):
    __gtype_name__ = "GarageJamTimeline"
    __gsignals__ = {
        "seek": (GObject.SignalFlags.RUN_FIRST, None, (float,)),
        "edited": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "selection-changed": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "track-selected": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
        "invalid-drop": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "files-dropped": (GObject.SignalFlags.RUN_FIRST, None, (object, int, float)),
    }

    def __init__(self, peaks: PeakCache):
        super().__init__()
        self.peaks = peaks
        self.project: Project | None = None
        self.pps = 60.0
        self.position = 0.0
        self.selected: Clip | None = None
        self.selected_track: Track | None = None
        self.snap = True
        self.recording_track: str | None = None
        self.recording_start = 0.0
        self._drag = None
        self._preview = None  # (row, start, valid) while moving a clip

        self.set_hexpand(True)
        self.set_vexpand(True)
        self.set_focusable(True)
        self.set_draw_func(self._draw)

        drag = Gtk.GestureDrag(button=Gdk.BUTTON_PRIMARY)
        drag.connect("drag-begin", self._on_drag_begin)
        drag.connect("drag-update", self._on_drag_update)
        drag.connect("drag-end", self._on_drag_end)
        self.add_controller(drag)

        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self._on_motion)
        self.add_controller(motion)

        drop = Gtk.DropTarget.new(Gdk.FileList, Gdk.DragAction.COPY)
        drop.connect("drop", self._on_drop)
        self.add_controller(drop)

        peaks.connect("ready", lambda *_: self.queue_draw())
        Adw.StyleManager.get_default().connect("notify::dark", lambda *_: self.queue_draw())

    # ---- public -----------------------------------------------------------
    def set_project(self, project: Project) -> None:
        self.project = project
        self.selected = None
        self.selected_track = project.tracks[0] if project.tracks else None
        self.refresh()

    def refresh(self) -> None:
        if self.project is None:
            return
        length = max(self.project.length, self.position, 30.0) + 60.0
        self.set_content_width(int(length * self.pps))
        self.set_content_height(RULER_H + len(self.project.tracks) * ROW_H)
        self.queue_draw()

    def set_position(self, seconds: float) -> None:
        self.position = seconds
        if self.recording_track or seconds * self.pps > self.get_content_width() - 200:
            self.refresh()
        else:
            self.queue_draw()

    def set_zoom(self, pps: float) -> None:
        self.pps = min(MAX_PPS, max(MIN_PPS, pps))
        self.refresh()

    def row_at(self, y: float) -> int:
        return int((y - RULER_H) // ROW_H)

    # ---- hit testing ------------------------------------------------------
    def _hit(self, x: float, y: float):
        if self.project is None or y < RULER_H:
            return None, None, None
        row = self.row_at(y)
        if not 0 <= row < len(self.project.tracks):
            return None, None, None
        track = self.project.tracks[row]
        t = x / self.pps
        for clip in reversed(track.clips):
            x0, x1 = clip.start * self.pps, clip.end * self.pps
            if x0 - 2 <= x <= x1 + 2:
                if x1 - x0 > 3 * HANDLE:
                    if x <= x0 + HANDLE:
                        return track, clip, "trim-left"
                    if x >= x1 - HANDLE:
                        return track, clip, "trim-right"
                return track, clip, "move"
        return track, None, None if t >= 0 else None

    # ---- gestures ---------------------------------------------------------
    def _snap_on(self, gesture) -> bool:
        state = gesture.get_current_event_state()
        return self.snap and not (state & Gdk.ModifierType.SHIFT_MASK)

    def _on_drag_begin(self, gesture, x, y):
        self.grab_focus()
        if self.project is None:
            return
        if y < RULER_H:
            self._drag = {"mode": "seek"}
            self.emit("seek", max(0.0, x / self.pps))
            return
        track, clip, mode = self._hit(x, y)
        if track is not None and track is not self.selected_track:
            self.selected_track = track
            self.emit("track-selected", track.id)
        if clip is not self.selected:
            self.selected = clip
            self.emit("selection-changed")
        if clip is None:
            self._drag = None
            if track is not None and y >= RULER_H:
                self.emit("seek", max(0.0, x / self.pps))
        else:
            self._drag = {
                "mode": mode, "clip": clip, "track": track,
                "start": clip.start, "inpoint": clip.inpoint,
                "duration": clip.duration, "moved": False,
            }
        self.queue_draw()

    def _on_drag_update(self, gesture, dx, dy):
        d = self._drag
        if not d or self.project is None:
            return
        ok, sx, sy = gesture.get_start_point()
        p = self.project
        if d["mode"] == "seek":
            self.emit("seek", max(0.0, (sx + dx) / self.pps))
            return
        if abs(dx) < 3 and abs(dy) < 3 and not d["moved"]:
            return
        d["moved"] = True
        clip, track = d["clip"], d["track"]
        snap = self._snap_on(gesture)
        delta = dx / self.pps

        if d["mode"] == "move":
            start = d["start"] + delta
            if snap:
                start = p.snap(start, 2)
            start = max(0.0, start)
            row = min(len(p.tracks) - 1, max(0, self.row_at(sy + dy)))
            target = p.tracks[row]
            valid = not target.overlaps(start, start + clip.duration, exclude=clip)
            self._preview = (row, start, valid)
        elif d["mode"] == "trim-right":
            lo, hi = track.neighbours(clip)
            end = d["start"] + d["duration"] + delta
            if snap:
                end = p.snap(end, 2)
            max_len = (clip.source_duration or float("inf")) - clip.inpoint
            end = min(end, hi, clip.start + max_len)
            clip.duration = max(MIN_CLIP, end - clip.start)
        elif d["mode"] == "trim-left":
            lo, hi = track.neighbours(clip)
            orig_end = d["start"] + d["duration"]
            start = d["start"] + delta
            if snap:
                start = p.snap(start, 2)
            start = max(start, lo, d["start"] - d["inpoint"], 0.0)
            start = min(start, orig_end - MIN_CLIP)
            clip.start = start
            clip.inpoint = d["inpoint"] + (start - d["start"])
            clip.duration = orig_end - start
        self.queue_draw()

    def _on_drag_end(self, gesture, dx, dy):
        d, self._drag = self._drag, None
        preview, self._preview = self._preview, None
        if not d or d["mode"] == "seek" or not d["moved"] or self.project is None:
            self.queue_draw()
            return
        clip, track = d["clip"], d["track"]
        if d["mode"] == "move" and preview is not None:
            row, start, valid = preview
            if not valid:
                self.emit("invalid-drop")
                self.queue_draw()
                return
            target = self.project.tracks[row]
            if target is not track:
                track.clips.remove(clip)
                target.clips.append(clip)
                self.selected_track = target
                self.emit("track-selected", target.id)
            clip.start = start
            target.sort()
        else:
            track.sort()
        self.refresh()
        self.emit("edited")

    def _on_motion(self, _ctrl, x, y):
        if self._drag:
            return
        if y < RULER_H:
            self.set_cursor_from_name("pointer")
            return
        _t, clip, mode = self._hit(x, y)
        if clip is None:
            self.set_cursor_from_name("default")
        elif mode in ("trim-left", "trim-right"):
            self.set_cursor_from_name("col-resize")
        else:
            self.set_cursor_from_name("grab")

    def _on_drop(self, _target, value, x, y):
        files = value.get_files() if value else []
        if not files or self.project is None:
            return False
        row = self.row_at(y) if y >= RULER_H else -1
        start = max(0.0, x / self.pps)
        if self.snap:
            start = self.project.snap(start)
        self.emit("files-dropped", files, row, start)
        return True

    # ---- drawing ----------------------------------------------------------
    def _draw(self, _area, cr, width, height):
        p = self.project
        dark = Adw.StyleManager.get_default().get_dark()
        fg = (1, 1, 1) if dark else (0, 0, 0)
        bg = (0.13, 0.13, 0.14) if dark else (0.985, 0.985, 0.985)
        cr.set_source_rgb(*bg)
        cr.paint()
        if p is None:
            return
        x0, _y0, x1, _y1 = cr.clip_extents()
        pps = self.pps

        # lanes
        accent = Adw.StyleManager.get_default().get_accent_color_rgba() \
            if hasattr(Adw.StyleManager, "get_accent_color_rgba") else None
        acc = (accent.red, accent.green, accent.blue) if accent else (0.21, 0.52, 0.89)
        for i, t in enumerate(p.tracks):
            y = RULER_H + i * ROW_H
            if t is self.selected_track:
                cr.set_source_rgba(*acc, 0.10)
            else:
                cr.set_source_rgba(*fg, 0.025 if i % 2 else 0.0)
            cr.rectangle(x0, y, x1 - x0, ROW_H)
            cr.fill()
            cr.set_source_rgba(*fg, 0.08)
            cr.rectangle(x0, y + ROW_H - 1, x1 - x0, 1)
            cr.fill()

        # beat / bar grid
        beat = p.beat_seconds
        beat_px = beat * pps
        bar_every = 1
        while p.bar_seconds * bar_every * pps < 48:
            bar_every *= 2
        first = max(0, int(x0 / beat_px) - 1)
        last = int(x1 / beat_px) + 1
        for b in range(first, last + 1):
            x = round(b * beat_px) + 0.5
            is_bar = b % p.beats_per_bar == 0
            bar_no = b // p.beats_per_bar
            if is_bar:
                strong = bar_no % bar_every == 0
                cr.set_source_rgba(*fg, 0.16 if strong else 0.07)
            elif beat_px >= 10:
                cr.set_source_rgba(*fg, 0.05)
            else:
                continue
            cr.rectangle(x - 0.5, RULER_H, 1, height - RULER_H)
            cr.fill()

        # clips
        dragging = self._drag["clip"] if self._drag and "clip" in self._drag else None
        for i, t in enumerate(p.tracks):
            audible = p.audible(t)
            for c in t.clips:
                if c is dragging and self._preview is not None:
                    continue
                self._draw_clip(cr, c, i, t.color, audible, x0, x1, fg)
        if self._preview is not None and dragging is not None:
            row, start, valid = self._preview
            t = p.tracks[row]
            ghost = Clip(dragging.path, start, dragging.inpoint, dragging.duration,
                         dragging.name, dragging.gain, dragging.id)
            self._draw_clip(cr, ghost, row, t.color if valid else "#e01b24",
                            True, x0, x1, fg, ghost=True, peaks_for=dragging.path)

        # live recording region
        if self.recording_track:
            row = next((i for i, t in enumerate(p.tracks) if t.id == self.recording_track), None)
            if row is not None and self.position > self.recording_start:
                rx = self.recording_start * pps
                rw = (self.position - self.recording_start) * pps
                ry = RULER_H + row * ROW_H + 4
                rounded_rect(cr, rx, ry, rw, ROW_H - 8, 6)
                cr.set_source_rgba(0.88, 0.11, 0.14, 0.35)
                cr.fill_preserve()
                cr.set_source_rgba(0.88, 0.11, 0.14, 0.9)
                cr.set_line_width(1.5)
                cr.stroke()

        # ruler
        cr.set_source_rgb(*[v * 0.94 + (0.06 if dark else 0) for v in bg])
        cr.rectangle(x0, 0, x1 - x0, RULER_H)
        cr.fill()
        cr.set_source_rgba(*fg, 0.15)
        cr.rectangle(x0, RULER_H - 1, x1 - x0, 1)
        cr.fill()
        cr.select_font_face("Cantarell", 0, 1)
        cr.set_font_size(11)
        bar_px = p.bar_seconds * pps
        for bar in range(max(0, int(x0 / bar_px) - 1), int(x1 / bar_px) + 2):
            x = bar * bar_px
            if bar % bar_every == 0:
                cr.set_source_rgba(*fg, 0.55)
                cr.rectangle(round(x), RULER_H - 12, 1, 11)
                cr.fill()
                cr.move_to(round(x) + 4, RULER_H - 13)
                cr.show_text(str(bar + 1))
            elif bar_px * bar_every >= 48:
                cr.set_source_rgba(*fg, 0.25)
                cr.rectangle(round(x), RULER_H - 6, 1, 5)
                cr.fill()

        # playhead
        px = round(self.position * pps) + 0.5
        color = (0.88, 0.11, 0.14) if self.recording_track else acc
        cr.set_source_rgb(*color)
        cr.set_line_width(1.5)
        cr.move_to(px, RULER_H - 6)
        cr.line_to(px, height)
        cr.stroke()
        cr.move_to(px - 6, RULER_H - 12)
        cr.line_to(px + 6, RULER_H - 12)
        cr.line_to(px, RULER_H - 4)
        cr.close_path()
        cr.fill()

    def _draw_clip(self, cr, c: Clip, row: int, color: str, audible: bool,
                   x0: float, x1: float, fg, ghost=False, peaks_for=None):
        pps = self.pps
        cx, cw = c.start * pps, max(2.0, c.duration * pps)
        if cx > x1 or cx + cw < x0:
            return
        cy, ch = RULER_H + row * ROW_H + 4, ROW_H - 8
        r, g, b = hex_rgb(color)
        alpha = 0.55 if ghost else (1.0 if audible else 0.4)
        selected = c is self.selected and not ghost

        rounded_rect(cr, cx + 0.5, cy + 0.5, cw - 1, ch - 1, 6)
        cr.set_source_rgba(r, g, b, 0.28 * alpha)
        cr.fill_preserve()
        cr.set_source_rgba(r, g, b, (1.0 if selected else 0.75) * alpha)
        cr.set_line_width(2.5 if selected else 1.0)
        cr.stroke()

        # title strip
        cr.save()
        rounded_rect(cr, cx + 0.5, cy + 0.5, cw - 1, ch - 1, 6)
        cr.clip()
        cr.set_source_rgba(r, g, b, 0.85 * alpha)
        cr.rectangle(cx, cy, cw, 18)
        cr.fill()
        cr.set_source_rgba(1, 1, 1, alpha)
        cr.select_font_face("Cantarell", 0, 1)
        cr.set_font_size(11)
        cr.move_to(cx + 6, cy + 13)
        cr.show_text(c.name or "Clip")

        # waveform
        peaks = self.peaks.get(peaks_for or c.path)
        if peaks:
            top, bottom = cy + 20, cy + ch - 3
            mid, half = (top + bottom) / 2, (bottom - top) / 2
            step = PEAKS_PER_SECOND / pps
            first_px = int(max(cx, x0) - cx)
            last_px = int(min(cx + cw, x1) - cx)
            base = c.inpoint * PEAKS_PER_SECOND
            n = len(peaks)
            gain = min(1.5, c.gain)
            cr.set_source_rgba(r * 0.75, g * 0.75, b * 0.75, 0.95 * alpha)
            cr.set_line_width(1.0)
            for px in range(first_px, last_px):
                a = int(base + px * step)
                if a >= n:
                    break
                e = max(a + 1, int(base + (px + 1) * step))
                v = max(peaks[a:e]) * gain
                h = max(0.5, min(1.0, v) * half)
                cr.move_to(cx + px + 0.5, mid - h)
                cr.line_to(cx + px + 0.5, mid + h)
            cr.stroke()
        elif not ghost:
            cr.set_source_rgba(*fg, 0.35)
            cr.move_to(cx + 6, cy + 36)
            cr.set_font_size(10)
            cr.show_text("Reading audio…" if c.source_duration is not None else "Missing file")
        cr.restore()
