"""Main GarageJam window."""

from __future__ import annotations

import os
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk  # noqa: E402

from . import APP_NAME, VERSION  # noqa: E402
from .engine import EXPORT_FORMATS, Engine, Exporter, PeakCache, to_s  # noqa: E402
from .project import (Clip, GJXMLError, Project, Track, load_gjxml,  # noqa: E402
                      save_gjxml)
from .timeline import RULER_H, TimelineView  # noqa: E402
from .trackheader import HEADER_W, TrackHeader  # noqa: E402

AUDIO_MIME = ["audio/*"]


def _filter(name: str, patterns=(), mimes=()) -> Gtk.FileFilter:
    f = Gtk.FileFilter(name=name)
    for p in patterns:
        f.add_pattern(p)
    for m in mimes:
        f.add_mime_type(m)
    return f


def _filters(*filters) -> Gio.ListStore:
    store = Gio.ListStore.new(Gtk.FileFilter)
    for f in filters:
        store.append(f)
    return store


class GarageJamWindow(Adw.ApplicationWindow):
    __gtype_name__ = "GarageJamWindow"

    def __init__(self, app: Adw.Application):
        super().__init__(application=app, default_width=1280, default_height=760)
        self.set_title(APP_NAME)
        self.engine = Engine()
        self.peaks = PeakCache()
        self.project = Project()
        self.dirty = False
        self.headers: list[TrackHeader] = []
        self._exporter: Exporter | None = None
        self._take_counter = 0

        self._build_ui()
        self._install_actions(app)
        self._connect_engine()
        self.connect("close-request", self._on_close_request)
        self.set_project(Project(), dirty=False)

    # =====================================================================
    # UI
    # =====================================================================
    def _build_ui(self):
        self.toasts = Adw.ToastOverlay()
        view = Adw.ToolbarView()
        self.toasts.set_child(view)
        self.set_content(self.toasts)

        # ---- header bar
        header = Adw.HeaderBar()
        self.title_widget = Adw.WindowTitle(title=APP_NAME)
        header.set_title_widget(self.title_widget)
        open_btn = Gtk.Button(icon_name="document-open-symbolic", action_name="win.open",
                              tooltip_text="Open Project (Ctrl+O)")
        header.pack_start(open_btn)
        import_btn = Gtk.Button(icon_name="list-add-symbolic", action_name="win.import",
                                tooltip_text="Import Audio (Ctrl+I)")
        header.pack_start(import_btn)

        menu = Gio.Menu()
        s1 = Gio.Menu()
        s1.append("New Project", "win.new")
        s1.append("Open…", "win.open")
        s1.append("Save", "win.save")
        s1.append("Save As…", "win.save-as")
        menu.append_section(None, s1)
        s2 = Gio.Menu()
        s2.append("Import Audio…", "win.import")
        s2.append("Export Mix…", "win.export")
        menu.append_section(None, s2)
        s3 = Gio.Menu()
        s3.append("Project Settings", "win.settings")
        s3.append("Keyboard Shortcuts", "win.shortcuts")
        s3.append(f"About {APP_NAME}", "win.about")
        menu.append_section(None, s3)
        menu_btn = Gtk.MenuButton(icon_name="open-menu-symbolic", menu_model=menu,
                                  primary=True, tooltip_text="Main Menu")
        header.pack_end(menu_btn)
        save_btn = Gtk.Button(icon_name="document-save-symbolic", action_name="win.save",
                              tooltip_text="Save (Ctrl+S)")
        header.pack_end(save_btn)
        export_btn = Gtk.Button(icon_name="document-send-symbolic", action_name="win.export",
                                tooltip_text="Export Mix (Ctrl+E)")
        header.pack_end(export_btn)
        view.add_top_bar(header)

        # ---- transport bar
        view.add_top_bar(self._build_transport())

        # ---- export banner
        self.banner = Adw.Banner(button_label="Cancel")
        self.banner.connect("button-clicked", lambda *_: self._exporter and self._exporter.cancel())
        view.add_top_bar(self.banner)

        # ---- content: empty state or studio
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE)
        self.stack.add_named(self._build_empty_state(), "empty")
        self.stack.add_named(self._build_studio(), "studio")
        view.set_content(self.stack)

        key = Gtk.EventControllerKey()
        key.connect("key-pressed", self._on_key)
        self.add_controller(key)

    def _build_transport(self) -> Gtk.Widget:
        bar = Gtk.Box(spacing=6)
        bar.add_css_class("transport")

        def tbutton(icon, action, tip, *css):
            b = Gtk.Button(icon_name=icon, action_name=action, tooltip_text=tip,
                           valign=Gtk.Align.CENTER)
            for c in css:
                b.add_css_class(c)
            return b

        controls = Gtk.Box(spacing=4)
        controls.append(tbutton("media-skip-backward-symbolic", "win.go-start", "Go to Start (Home)", "circular"))
        self.play_btn = tbutton("media-playback-start-symbolic", "win.play-pause", "Play / Pause (Space)",
                                "circular", "play-button")
        controls.append(self.play_btn)
        controls.append(tbutton("media-playback-stop-symbolic", "win.stop", "Stop", "circular"))
        self.rec_btn = tbutton("media-record-symbolic", "win.record", "Record (R)", "circular", "record-button")
        controls.append(self.rec_btn)
        bar.append(controls)

        lcd = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER)
        lcd.add_css_class("lcd")
        self.bars_label = Gtk.Label(label="001.1.00", xalign=0)
        self.bars_label.add_css_class("bars")
        self.clock_label = Gtk.Label(label="00:00.00", xalign=0)
        self.clock_label.add_css_class("clock")
        lcd.append(self.bars_label)
        lcd.append(self.clock_label)
        bar.append(lcd)

        tempo_box = Gtk.Box(spacing=4, valign=Gtk.Align.CENTER)
        self.tempo_spin = Gtk.SpinButton.new_with_range(20, 300, 1)
        self.tempo_spin.set_digits(0)
        self.tempo_spin.set_tooltip_text("Tempo")
        self.tempo_spin.connect("value-changed", self._on_tempo)
        tempo_box.append(self.tempo_spin)
        bpm = Gtk.Label(label="BPM")
        bpm.add_css_class("dim-label")
        tempo_box.append(bpm)
        self.sig_label = Gtk.Label(label="4/4")
        self.sig_label.add_css_class("dim-label")
        self.sig_label.add_css_class("numeric")
        tempo_box.append(self.sig_label)
        bar.append(tempo_box)

        self.snap_btn = Gtk.ToggleButton(label="Snap", active=True, valign=Gtk.Align.CENTER,
                                         tooltip_text="Snap to beat (hold Shift to bypass)")
        self.snap_btn.connect("toggled", lambda b: setattr(self.timeline, "snap", b.get_active()))
        bar.append(self.snap_btn)

        bar.append(Gtk.Box(hexpand=True))

        meters = Gtk.Box(spacing=10, valign=Gtk.Align.CENTER)
        in_box = Gtk.Box(spacing=6)
        in_icon = Gtk.Image(icon_name="audio-input-microphone-symbolic", tooltip_text="Input level")
        in_icon.add_css_class("dim-label")
        in_box.append(in_icon)
        self.in_meter = Gtk.LevelBar(min_value=0, max_value=1, valign=Gtk.Align.CENTER)
        self.in_meter.set_size_request(90, -1)
        self.in_meter.add_css_class("meter")
        in_box.append(self.in_meter)
        meters.append(in_box)

        out_box = Gtk.Box(spacing=6)
        out_icon = Gtk.Image(icon_name="audio-speakers-symbolic", tooltip_text="Master output")
        out_icon.add_css_class("dim-label")
        out_box.append(out_icon)
        lr = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2, valign=Gtk.Align.CENTER)
        self.out_meters = []
        for _ in range(2):
            m = Gtk.LevelBar(min_value=0, max_value=1)
            m.set_size_request(120, -1)
            m.add_css_class("meter")
            m.add_css_class("thin")
            lr.append(m)
            self.out_meters.append(m)
        out_box.append(lr)
        meters.append(out_box)
        for m in [self.in_meter] + self.out_meters:
            m.add_offset_value("high", 0.7)
            m.add_offset_value("full", 0.95)
        bar.append(meters)

        zoom = Gtk.Box(valign=Gtk.Align.CENTER)
        zoom.add_css_class("linked")
        zoom.append(Gtk.Button(icon_name="zoom-out-symbolic", action_name="win.zoom-out",
                               tooltip_text="Zoom Out (Ctrl+−)"))
        zoom.append(Gtk.Button(icon_name="zoom-fit-best-symbolic", action_name="win.zoom-fit",
                               tooltip_text="Zoom to Fit (Ctrl+0)"))
        zoom.append(Gtk.Button(icon_name="zoom-in-symbolic", action_name="win.zoom-in",
                               tooltip_text="Zoom In (Ctrl++)"))
        bar.append(zoom)

        add = Gtk.Button(valign=Gtk.Align.CENTER, action_name="win.add-track",
                         tooltip_text="Add Track (Ctrl+T)")
        add.set_child(Adw.ButtonContent(icon_name="list-add-symbolic", label="Track"))
        bar.append(add)
        return bar

    def _build_empty_state(self) -> Gtk.Widget:
        page = Adw.StatusPage(icon_name="audio-input-microphone-symbolic",
                              title="Start a Jam",
                              description="Add a track and hit record, or bring in audio you already have.")
        box = Gtk.Box(spacing=12, halign=Gtk.Align.CENTER)
        b1 = Gtk.Button(label="Add Track", action_name="win.add-track")
        b1.add_css_class("pill")
        b1.add_css_class("suggested-action")
        b2 = Gtk.Button(label="Import Audio…", action_name="win.import")
        b2.add_css_class("pill")
        b3 = Gtk.Button(label="Open Project…", action_name="win.open")
        b3.add_css_class("pill")
        for b in (b1, b2, b3):
            box.append(b)
        page.set_child(box)
        return page

    def _build_studio(self) -> Gtk.Widget:
        self.timeline = TimelineView(self.peaks)
        self.timeline.connect("seek", lambda _t, s: self.engine.seek(s))
        self.timeline.connect("edited", lambda *_: self.changed())
        self.timeline.connect("selection-changed", lambda *_: self._update_actions())
        self.timeline.connect("track-selected", self._on_track_selected)
        self.timeline.connect("invalid-drop", lambda *_: self.toast("Clips on a track can't overlap"))
        self.timeline.connect("files-dropped", self._on_files_dropped)

        self.timeline_sw = Gtk.ScrolledWindow(hexpand=True, vexpand=True)
        self.timeline_sw.set_child(self.timeline)
        scroll = Gtk.EventControllerScroll(flags=Gtk.EventControllerScrollFlags.VERTICAL)
        scroll.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        scroll.connect("scroll", self._on_scroll)
        self.timeline_sw.add_controller(scroll)

        self.header_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.header_box.set_size_request(HEADER_W, -1)
        header_sw = Gtk.ScrolledWindow(vadjustment=self.timeline_sw.get_vadjustment())
        header_sw.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.EXTERNAL)
        header_sw.set_child(self.header_box)
        header_sw.add_css_class("track-headers")

        # Child widgets (entries, scales) ask to hexpand; stop that from
        # propagating up so the header column keeps its fixed width.
        header_sw.set_hexpand(False)
        header_sw.set_size_request(HEADER_W, -1)
        box = Gtk.Box()
        box.append(header_sw)
        box.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))
        box.append(self.timeline_sw)
        return box

    # =====================================================================
    # Actions
    # =====================================================================
    def _install_actions(self, app):
        actions = {
            "new": (self.on_new, ["<Ctrl>n"]),
            "open": (self.on_open, ["<Ctrl>o"]),
            "save": (self.on_save, ["<Ctrl>s"]),
            "save-as": (self.on_save_as, ["<Ctrl><Shift>s"]),
            "import": (self.on_import, ["<Ctrl>i"]),
            "export": (self.on_export, ["<Ctrl>e"]),
            "settings": (self.on_settings, ["<Ctrl>comma"]),
            "about": (self.on_about, []),
            "shortcuts": (self.on_shortcuts, ["<Ctrl>question"]),
            "add-track": (self.on_add_track, ["<Ctrl>t"]),
            "play-pause": (self.on_play_pause, []),
            "stop": (self.on_stop, []),
            "record": (self.on_record, []),
            "go-start": (lambda: self.engine.seek(0.0), []),
            "delete-clip": (self.on_delete_clip, []),
            "split": (self.on_split, []),
            "duplicate": (self.on_duplicate, ["<Ctrl>d"]),
            "zoom-in": (lambda: self.zoom(1.4), ["<Ctrl>plus", "<Ctrl>equal"]),
            "zoom-out": (lambda: self.zoom(1 / 1.4), ["<Ctrl>minus"]),
            "zoom-fit": (self.zoom_fit, ["<Ctrl>0"]),
        }
        self.actions = {}
        for name, (cb, accels) in actions.items():
            action = Gio.SimpleAction.new(name, None)
            action.connect("activate", lambda _a, _p, cb=cb: cb())
            self.add_action(action)
            self.actions[name] = action
            if accels:
                app.set_accels_for_action(f"win.{name}", accels)

    def _update_actions(self):
        has_clip = self.timeline.selected is not None
        for name in ("delete-clip", "split", "duplicate"):
            self.actions[name].set_enabled(has_clip)
        busy = self._exporter is not None
        self.actions["export"].set_enabled(not busy and self.project.length > 0)

    def _on_key(self, _ctrl, keyval, _code, state):
        if state & (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK):
            return False
        focus = self.get_focus()
        if isinstance(focus, (Gtk.Text, Gtk.Editable)):
            return False
        mapping = {
            Gdk.KEY_space: "play-pause", Gdk.KEY_r: "record", Gdk.KEY_R: "record",
            Gdk.KEY_Delete: "delete-clip", Gdk.KEY_BackSpace: "delete-clip",
            Gdk.KEY_s: "split", Gdk.KEY_S: "split", Gdk.KEY_Home: "go-start",
            Gdk.KEY_Escape: "stop",
        }
        name = mapping.get(keyval)
        if name and self.actions[name].get_enabled():
            self.actions[name].activate(None)
            return True
        return False

    # =====================================================================
    # Project state
    # =====================================================================
    def set_project(self, project: Project, dirty: bool = False):
        self.engine.stop()
        self.project = project
        self.engine.load(project)
        self.timeline.set_project(project)
        self.tempo_spin.set_value(project.tempo)
        self._rebuild_headers()
        self.engine.seek(0.0)
        self.dirty = dirty
        self._update_title()
        self._update_actions()
        for t in project.tracks:
            for c in t.clips:
                self.peaks.get(c.path)
        missing = len(self.engine.mirror.missing)
        if missing:
            self.toast(f"{missing} audio file{'s' if missing > 1 else ''} could not be found")

    def changed(self, structure: bool = False):
        """Call after editing the model."""
        self.dirty = True
        self.engine.load(self.project)
        if structure:
            self._rebuild_headers()
        self.timeline.refresh()
        self._update_title()
        self._update_actions()

    def _update_title(self):
        name = self.project.name
        self.title_widget.set_title(("• " if self.dirty else "") + name)
        self.title_widget.set_subtitle(
            GLib.filename_display_basename(self.project.path) if self.project.path else "Not saved")
        self.set_title(f"{name} — {APP_NAME}")
        self.stack.set_visible_child_name("studio" if self.project.tracks else "empty")

    def _rebuild_headers(self):
        while (child := self.header_box.get_first_child()) is not None:
            self.header_box.remove(child)
        spacer = Gtk.Box()
        spacer.set_size_request(-1, RULER_H)
        spacer.add_css_class("ruler-spacer")
        self.header_box.append(spacer)
        self.headers = []
        for track in self.project.tracks:
            h = TrackHeader(track)
            h.connect("mix-changed", self._on_mix_changed)
            h.connect("edited", self._on_header_edited)
            h.connect("selected", lambda h: self.select_track(h.track))
            h.connect("request-move", self._on_move_track)
            h.connect("request-delete", self._on_delete_track)
            h.set_selected(track is self.timeline.selected_track)
            self.header_box.append(h)
            self.headers.append(h)

    def select_track(self, track: Track | None):
        self.timeline.selected_track = track
        for h in self.headers:
            h.set_selected(h.track is track)
        self.timeline.queue_draw()

    def _on_track_selected(self, _timeline, track_id):
        self.select_track(self.project.track_by_id(track_id))

    def _on_mix_changed(self, _header):
        self.engine.update_mix(self.project)
        self.dirty = True
        self._update_title()
        self.timeline.queue_draw()

    def _on_header_edited(self, _header):
        self.dirty = True
        self._update_title()
        self.timeline.queue_draw()

    def _on_move_track(self, header, delta):
        tracks = self.project.tracks
        i = tracks.index(header.track)
        j = i + delta
        if 0 <= j < len(tracks):
            tracks[i], tracks[j] = tracks[j], tracks[i]
            self.changed(structure=True)

    def _on_delete_track(self, header):
        track = header.track

        def delete():
            if self.timeline.selected and self.timeline.selected in track.clips:
                self.timeline.selected = None
            self.project.tracks.remove(track)
            if self.timeline.selected_track is track:
                self.timeline.selected_track = self.project.tracks[0] if self.project.tracks else None
            self.changed(structure=True)

        if not track.clips:
            delete()
            return
        dialog = Adw.AlertDialog(heading=f"Delete “{track.name}”?",
                                 body=f"The track and its {len(track.clips)} clip(s) will be removed "
                                      "from the project. Audio files stay on disk.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("delete", "Delete")
        dialog.set_response_appearance("delete", Adw.ResponseAppearance.DESTRUCTIVE)
        dialog.connect("response", lambda _d, r: r == "delete" and delete())
        dialog.present(self)

    def _on_tempo(self, spin):
        tempo = spin.get_value()
        if abs(tempo - self.project.tempo) > 1e-6:
            self.project.tempo = tempo
            self.dirty = True
            self._update_title()
            self.timeline.queue_draw()
            self._update_clock(self.engine.position())

    # =====================================================================
    # Engine
    # =====================================================================
    def _connect_engine(self):
        self.engine.connect("tick", self._on_tick)
        self.engine.connect("state-changed", self._on_state)
        self.engine.connect("master-level", self._on_master_level)
        self.engine.connect("input-level", lambda _e, v: self.in_meter.set_value(v))
        self.engine.connect("recording-finished", self._on_recording_finished)
        self.engine.connect("error", lambda _e, msg: self.toast(msg))

    def _on_tick(self, _engine, pos):
        self.timeline.recording_start = self.engine.recording_start
        self.timeline.set_position(pos)
        self._update_clock(pos)
        if self.engine.state in ("playing", "recording"):
            hadj = self.timeline_sw.get_hadjustment()
            x = pos * self.timeline.pps
            page = hadj.get_page_size()
            if x > hadj.get_value() + page * 0.92 or x < hadj.get_value():
                hadj.set_value(max(0, x - page * 0.08))

    def _update_clock(self, pos):
        bar, beat, ticks = self.project.bar_beat(pos)
        self.bars_label.set_label(f"{bar:03d}.{beat}.{ticks:02d}")
        m, s = divmod(pos, 60)
        self.clock_label.set_label(f"{int(m):02d}:{s:05.2f}")

    def _on_state(self, _engine, state):
        playing = state in ("playing", "recording")
        self.play_btn.set_icon_name("media-playback-pause-symbolic" if playing
                                    else "media-playback-start-symbolic")
        if state == "recording":
            self.rec_btn.add_css_class("recording")
        else:
            self.rec_btn.remove_css_class("recording")
            self.timeline.recording_track = None
            self.timeline.queue_draw()
        if not playing:
            for m in self.out_meters:
                m.set_value(0)

    def _on_master_level(self, _engine, left, right):
        if self.engine.state not in ("playing", "recording"):
            left = right = 0.0  # late messages after pausing
        self.out_meters[0].set_value(left)
        self.out_meters[1].set_value(right)

    # =====================================================================
    # Transport
    # =====================================================================
    def on_play_pause(self):
        if self.engine.state in ("playing", "recording"):
            self.engine.pause()
        else:
            self.engine.play()

    def on_stop(self):
        self.engine.stop()

    def on_record(self):
        if self.engine.state == "recording":
            self.engine.stop_recording()
            return
        if not self.project.tracks:
            self.project.add_track("Recording 1")
            self.select_track(self.project.tracks[0])
            self.changed(structure=True)
        armed = [t for t in self.project.tracks if t.armed]
        if not armed:
            track = self.timeline.selected_track or self.project.tracks[0]
            track.armed = True
            for h in self.headers:
                if h.track is track:
                    h.set_armed(True)
            armed = [track]
        self._take_counter += 1
        stamp = time.strftime("%Y%m%d-%H%M%S")
        path = os.path.join(self.project.audio_dir(), f"take-{stamp}.wav")
        if self.engine.start_recording(path, self.project.sample_rate):
            self._record_track = armed[0]
            self.timeline.recording_track = armed[0].id

    def _on_recording_finished(self, _engine, path, start):
        asset = self.engine.mirror.asset(path)
        if asset is None:
            self.toast("The recording could not be read back")
            return
        duration = to_s(asset.get_duration())
        track = getattr(self, "_record_track", None)
        if track not in self.project.tracks:
            track = next((t for t in self.project.tracks if t.armed), None)
        takes = sum(1 for t in self.project.tracks for c in t.clips if c.name.startswith("Take"))
        clip = Clip(path, start=start, duration=duration, name=f"Take {takes + 1}")
        structure = False
        if track is None or track.overlaps(start, start + duration):
            base = track.name if track else "Recording"
            new = self.project.add_track(f"{base} (take {takes + 1})")
            if track is not None:
                self.project.tracks.remove(new)
                self.project.tracks.insert(self.project.tracks.index(track) + 1, new)
            track, structure = new, True
        track.clips.append(clip)
        track.sort()
        self.peaks.invalidate(path)
        self.peaks.get(path)
        self.timeline.selected = clip
        self.changed(structure=structure)
        self.toast(f"Recorded {duration:.1f} s on “{track.name}”")

    # =====================================================================
    # Editing
    # =====================================================================
    def on_add_track(self):
        track = self.project.add_track()
        self.timeline.selected_track = track
        self.changed(structure=True)
        self.select_track(track)
        self.headers[-1].name_entry.grab_focus()

    def on_delete_clip(self):
        clip = self.timeline.selected
        track = clip and self.project.track_for_clip(clip)
        if track:
            track.clips.remove(clip)
            self.timeline.selected = None
            self.changed()

    def on_split(self):
        clip = self.timeline.selected
        if clip is None:
            return
        right = self.project.split_clip(clip, self.engine.position())
        if right is None:
            self.toast("Move the playhead inside the selected clip to split it")
            return
        self.timeline.selected = right
        self.changed()

    def on_duplicate(self):
        clip = self.timeline.selected
        track = clip and self.project.track_for_clip(clip)
        if not track:
            return
        copy = clip.copy()
        copy.start = clip.end
        while track.overlaps(copy.start, copy.end, exclude=copy):
            nxt = min(c.end for c in track.clips if c.end > copy.start and c.start < copy.end)
            copy.start = nxt
        track.clips.append(copy)
        track.sort()
        self.timeline.selected = copy
        self.changed()

    def zoom(self, factor):
        hadj = self.timeline_sw.get_hadjustment()
        pos = self.engine.position()
        offset = pos * self.timeline.pps - hadj.get_value()
        self.timeline.set_zoom(self.timeline.pps * factor)
        GLib.idle_add(lambda: hadj.set_value(max(0, pos * self.timeline.pps - offset)) and False)

    def zoom_fit(self):
        width = max(200, self.timeline_sw.get_width() - 40)
        self.timeline.set_zoom(width / max(self.project.length * 1.05, 10.0))
        self.timeline_sw.get_hadjustment().set_value(0)

    def _on_scroll(self, ctrl, _dx, dy):
        if ctrl.get_current_event_state() & Gdk.ModifierType.CONTROL_MASK:
            self.zoom(1.15 if dy < 0 else 1 / 1.15)
            return True
        return False

    # =====================================================================
    # Files
    # =====================================================================
    def on_new(self):
        self._confirm_discard(lambda: self.set_project(Project(), dirty=False))

    def on_open(self):
        def pick():
            dialog = Gtk.FileDialog(title="Open Project", filters=_filters(
                _filter("GarageJam projects", ["*.gjxml"]), _filter("All files", ["*"])))
            dialog.open(self, None, self._on_open_done)
        self._confirm_discard(pick)

    def _on_open_done(self, dialog, result):
        try:
            f = dialog.open_finish(result)
        except GLib.Error:
            return
        if f:
            self.open_path(f.get_path())

    def open_path(self, path: str):
        try:
            project = load_gjxml(path)
        except (OSError, GJXMLError) as exc:
            self.toast(f"Could not open {os.path.basename(path)}: {exc}")
            return
        self.set_project(project)

    def on_save(self, then=None):
        if self.project.path:
            self._save_to(self.project.path, then)
        else:
            self.on_save_as(then)

    def on_save_as(self, then=None):
        dialog = Gtk.FileDialog(title="Save Project", initial_name=f"{self.project.name}.gjxml",
                                filters=_filters(_filter("GarageJam projects", ["*.gjxml"])))

        def done(d, result):
            try:
                f = d.save_finish(result)
            except GLib.Error:
                return
            if f:
                path = f.get_path()
                if not path.endswith(".gjxml"):
                    path += ".gjxml"
                if self.project.name == "Untitled Jam":
                    self.project.name = os.path.splitext(os.path.basename(path))[0]
                self._save_to(path, then)
        dialog.save(self, None, done)

    def _save_to(self, path, then=None):
        try:
            save_gjxml(self.project, path)
        except OSError as exc:
            self.toast(f"Could not save: {exc.strerror}")
            return
        self.dirty = False
        self._update_title()
        self.toast(f"Saved {os.path.basename(path)}")
        if callable(then):
            then()

    def on_import(self):
        dialog = Gtk.FileDialog(title="Import Audio", filters=_filters(
            _filter("Audio files", mimes=AUDIO_MIME), _filter("All files", ["*"])))

        def done(d, result):
            try:
                files = d.open_multiple_finish(result)
            except GLib.Error:
                return
            track = self.timeline.selected_track
            row = self.project.tracks.index(track) if track in self.project.tracks else -1
            self.import_files([files.get_item(i) for i in range(files.get_n_items())],
                              row, self.engine.position())
        dialog.open_multiple(self, None, done)

    def _on_files_dropped(self, _timeline, files, row, start):
        self.import_files(list(files), row, start)

    def import_files(self, files, row: int, start: float):
        added, failed, structure = 0, 0, False
        for f in files:
            path = f.get_path() if hasattr(f, "get_path") else str(f)
            asset = path and self.engine.mirror.asset(path)
            if not asset:
                failed += 1
                continue
            duration = to_s(asset.get_duration())
            name = os.path.splitext(os.path.basename(path))[0]
            track = self.project.tracks[row] if 0 <= row < len(self.project.tracks) else None
            if track is None or track.overlaps(start, start + duration):
                track = self.project.add_track(name)
                row = len(self.project.tracks) - 1
                structure = True
            clip = Clip(path, start=start, duration=duration, name=name)
            track.clips.append(clip)
            track.sort()
            self.peaks.get(path)
            self.timeline.selected = clip
            added += 1
            row = -1  # further files go on fresh tracks, lined up at the same start
        if added:
            self.changed(structure=structure)
        if failed:
            self.toast(f"{failed} file{'s' if failed > 1 else ''} could not be imported")

    def on_export(self):
        if self.project.length <= 0:
            self.toast("Nothing to export yet")
            return
        wav = _filter(EXPORT_FORMATS["wav"][0], ["*.wav"])
        ogg = _filter(EXPORT_FORMATS["ogg"][0], ["*.ogg"])
        dialog = Gtk.FileDialog(title="Export Mix", initial_name=f"{self.project.name}.wav",
                                filters=_filters(wav, ogg), default_filter=wav)

        def done(d, result):
            try:
                f = d.save_finish(result)
            except GLib.Error:
                return
            if f:
                path = f.get_path()
                ext = os.path.splitext(path)[1].lower().lstrip(".")
                if ext not in EXPORT_FORMATS:
                    path += ".wav"
                    ext = "wav"
                self._start_export(path, ext)
        dialog.save(self, None, done)

    def _start_export(self, path, fmt):
        self.engine.pause()
        exporter = Exporter(self.project, path, fmt)
        self._exporter = exporter
        self.banner.set_title("Exporting mix… 0%")
        self.banner.set_revealed(True)
        exporter.connect("progress", lambda _e, p: self.banner.set_title(f"Exporting mix… {int(p * 100)}%"))

        def finished(_e, ok, detail):
            self._exporter = None
            self.banner.set_revealed(False)
            self._update_actions()
            if ok:
                toast = Adw.Toast(title=f"Exported {os.path.basename(path)}", button_label="Open")
                toast.connect("button-clicked",
                              lambda *_: Gtk.FileLauncher(file=Gio.File.new_for_path(path)).launch(self, None, None))
                self.toasts.add_toast(toast)
            elif detail != "Cancelled":
                self.toast(f"Export failed: {detail}")
        exporter.connect("finished", finished)
        self._update_actions()
        exporter.start()

    # =====================================================================
    # Dialogs
    # =====================================================================
    def on_settings(self):
        p = self.project
        dialog = Adw.PreferencesDialog(title="Project Settings")
        page = Adw.PreferencesPage(title="Project", icon_name="emblem-music-symbolic")
        dialog.add(page)

        song = Adw.PreferencesGroup(title="Song")
        name = Adw.EntryRow(title="Project name", text=p.name)
        tempo = Adw.SpinRow.new_with_range(20, 300, 1)
        tempo.set_title("Tempo (BPM)")
        tempo.set_value(p.tempo)
        beats = Adw.SpinRow.new_with_range(1, 16, 1)
        beats.set_title("Beats per bar")
        beats.set_value(p.beats_per_bar)
        rates = ["44100", "48000", "96000"]
        rate = Adw.ComboRow(title="Recording sample rate", subtitle="Hz",
                            model=Gtk.StringList.new(rates))
        rate.set_selected(rates.index(str(p.sample_rate)) if str(p.sample_rate) in rates else 1)
        for row in (name, tempo, beats, rate):
            song.add(row)
        page.add(song)

        meta = Adw.PreferencesGroup(title="Metadata", description="Written as tags when you export")
        rows = {}
        for key in ("title", "artist", "album", "genre", "comment"):
            row = Adw.EntryRow(title=key.capitalize(), text=p.metadata.get(key, ""))
            meta.add(row)
            rows[key] = row
        page.add(meta)

        def apply(*_):
            p.name = name.get_text().strip() or p.name
            p.tempo = tempo.get_value()
            p.beats_per_bar = int(beats.get_value())
            p.sample_rate = int(rates[rate.get_selected()])
            for key, row in rows.items():
                value = row.get_text().strip()
                if value:
                    p.metadata[key] = value
                else:
                    p.metadata.pop(key, None)
            self.tempo_spin.set_value(p.tempo)
            self.sig_label.set_label(f"{p.beats_per_bar}/{p.beat_unit}")
            self.changed()
        dialog.connect("closed", apply)
        dialog.present(self)

    def on_about(self):
        about = Adw.AboutDialog(
            application_name=APP_NAME, application_icon="audio-input-microphone-symbolic",
            version=VERSION, developer_name="Ole Kristian Aamot",
            website="https://www.garagejam.org",
            comments="Multitrack music studio for GNOME, built with GTK 4, libadwaita and GStreamer.",
            developers=["Ole Kristian Aamot"])
        about.present(self)

    def on_shortcuts(self):
        rows = [
            ("Space", "Play / pause"), ("R", "Record / stop recording"), ("Esc", "Stop (twice: to start)"),
            ("Home", "Go to start"), ("S", "Split clip at playhead"), ("Delete", "Delete clip"),
            ("Ctrl+D", "Duplicate clip"), ("Ctrl+T", "Add track"), ("Ctrl+I", "Import audio"),
            ("Ctrl+E", "Export mix"), ("Ctrl+S", "Save"), ("Ctrl+O", "Open"),
            ("Ctrl+scroll", "Zoom"), ("Ctrl+0", "Zoom to fit"), ("Shift+drag", "Move without snapping"),
        ]
        dialog = Adw.Dialog(title="Keyboard Shortcuts", content_width=420)
        tv = Adw.ToolbarView()
        tv.add_top_bar(Adw.HeaderBar())
        group = Adw.PreferencesGroup(margin_top=12, margin_bottom=18, margin_start=18, margin_end=18)
        for key, desc in rows:
            row = Adw.ActionRow(title=desc)
            label = Gtk.Label(label=key)
            label.add_css_class("dim-label")
            label.add_css_class("monospace")
            row.add_suffix(label)
            group.add(row)
        sw = Gtk.ScrolledWindow(propagate_natural_height=True)
        sw.set_child(group)
        tv.set_content(sw)
        dialog.set_child(tv)
        dialog.present(self)

    def toast(self, text: str):
        self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(text), timeout=4))

    def _confirm_discard(self, then):
        if not self.dirty:
            then()
            return
        dialog = Adw.AlertDialog(heading="Save changes?",
                                 body=f"“{self.project.name}” has unsaved changes.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("discard", "Discard")
        dialog.add_response("save", "Save")
        dialog.set_response_appearance("discard", Adw.ResponseAppearance.DESTRUCTIVE)
        dialog.set_response_appearance("save", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_default_response("save")
        dialog.set_close_response("cancel")

        def response(_d, r):
            if r == "discard":
                then()
            elif r == "save":
                self.on_save(then)
        dialog.connect("response", response)
        dialog.present(self)

    def _on_close_request(self, _win):
        if self.dirty and not getattr(self, "_closing", False):
            def close():
                self._closing = True
                self.close()
            self._confirm_discard(close)
            return True
        if self._exporter:
            self._exporter.cancel()
        self.engine.shutdown()
        return False
