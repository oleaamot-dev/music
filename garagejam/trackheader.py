"""Per-track header: name, colour, mute / solo / record-arm, pan and volume."""

from __future__ import annotations

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, Gio, GObject, Gtk  # noqa: E402

from .project import Track  # noqa: E402
from .timeline import ROW_H  # noqa: E402

HEADER_W = 264


class TrackHeader(Gtk.Box):
    __gtype_name__ = "GarageJamTrackHeader"
    __gsignals__ = {
        # mix: volume / pan / mute / solo changed (no timeline rebuild needed)
        "mix-changed": (GObject.SignalFlags.RUN_FIRST, None, ()),
        # edited: name / colour / arm changed (marks project dirty)
        "edited": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "selected": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "request-move": (GObject.SignalFlags.RUN_FIRST, None, (int,)),
        "request-delete": (GObject.SignalFlags.RUN_FIRST, None, ()),
    }

    def __init__(self, track: Track):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.track = track
        self.add_css_class("track-header")
        self.set_size_request(HEADER_W, ROW_H)
        self.set_vexpand(False)

        # Row 1: colour, name, menu
        row1 = Gtk.Box(spacing=6)
        self.color_button = Gtk.ColorDialogButton(dialog=Gtk.ColorDialog(with_alpha=False))
        rgba = Gdk.RGBA()
        rgba.parse(track.color)
        self.color_button.set_rgba(rgba)
        self.color_button.set_valign(Gtk.Align.CENTER)
        self.color_button.add_css_class("track-color")
        self.color_button.set_tooltip_text("Track colour")
        self.color_button.connect("notify::rgba", self._on_color)
        row1.append(self.color_button)

        self.name_entry = Gtk.Entry(text=track.name, hexpand=True)
        self.name_entry.add_css_class("track-name")
        self.name_entry.connect("changed", self._on_name)
        row1.append(self.name_entry)

        menu = Gio.Menu()
        menu.append("Move Up", "track.move-up")
        menu.append("Move Down", "track.move-down")
        section = Gio.Menu()
        section.append("Delete Track", "track.delete")
        menu.append_section(None, section)
        more = Gtk.MenuButton(icon_name="view-more-symbolic", menu_model=menu,
                              valign=Gtk.Align.CENTER, tooltip_text="Track options")
        more.add_css_class("flat")
        row1.append(more)
        self.append(row1)

        group = Gio.SimpleActionGroup()
        for name, cb in (("move-up", lambda *_: self.emit("request-move", -1)),
                         ("move-down", lambda *_: self.emit("request-move", 1)),
                         ("delete", lambda *_: self.emit("request-delete"))):
            action = Gio.SimpleAction.new(name, None)
            action.connect("activate", cb)
            group.add_action(action)
        self.insert_action_group("track", group)

        # Row 2: M / S / arm + pan
        row2 = Gtk.Box(spacing=4)
        self.mute = self._toggle("M", "mute", "Mute", track.mute)
        self.solo = self._toggle("S", "solo", "Solo", track.solo)
        self.arm = Gtk.ToggleButton(icon_name="media-record-symbolic", active=track.armed,
                                    tooltip_text="Arm for recording")
        self.arm.add_css_class("arm")
        self.arm.add_css_class("circular")
        self.arm.connect("toggled", self._on_arm)
        for w in (self.mute, self.solo, self.arm):
            row2.append(w)

        pan_box = Gtk.Box(spacing=2, hexpand=True, margin_start=6)
        pan_box.append(self._dim_label("L"))
        self.pan = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, -1.0, 1.0, 0.05)
        self.pan.set_value(track.pan)
        self.pan.set_hexpand(True)
        self.pan.add_mark(0.0, Gtk.PositionType.BOTTOM, None)
        self.pan.set_tooltip_text("Pan")
        self.pan.connect("value-changed", self._on_pan)
        pan_box.append(self.pan)
        pan_box.append(self._dim_label("R"))
        row2.append(pan_box)
        self.append(row2)

        # Row 3: volume
        row3 = Gtk.Box(spacing=4)
        self.vol_icon = Gtk.Image(icon_name="audio-volume-high-symbolic")
        self.vol_icon.add_css_class("dim-label")
        row3.append(self.vol_icon)
        self.volume = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 1.5, 0.01)
        self.volume.set_value(track.volume)
        self.volume.set_hexpand(True)
        self.volume.add_mark(1.0, Gtk.PositionType.BOTTOM, None)
        self.volume.connect("value-changed", self._on_volume)
        row3.append(self.volume)
        self.vol_label = Gtk.Label(width_chars=6, xalign=1)
        self.vol_label.add_css_class("dim-label")
        self.vol_label.add_css_class("numeric")
        self.vol_label.add_css_class("caption")
        row3.append(self.vol_label)
        self._update_vol_label()
        self.append(row3)

        click = Gtk.GestureClick()
        click.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        click.connect("pressed", lambda *_: self.emit("selected"))
        self.add_controller(click)

    @staticmethod
    def _dim_label(text):
        label = Gtk.Label(label=text)
        label.add_css_class("dim-label")
        label.add_css_class("caption")
        return label

    def _toggle(self, label, css, tooltip, active):
        b = Gtk.ToggleButton(label=label, active=active, tooltip_text=tooltip)
        b.add_css_class(css)
        b.add_css_class("circular")
        b.connect("toggled", self._on_toggle, css)
        return b

    def set_selected(self, selected: bool) -> None:
        if selected:
            self.add_css_class("selected")
        else:
            self.remove_css_class("selected")

    def set_armed(self, armed: bool) -> None:
        self.arm.set_active(armed)

    # ---- handlers ---------------------------------------------------------
    def _on_color(self, button, _pspec):
        c = button.get_rgba()
        self.track.color = "#%02x%02x%02x" % (int(c.red * 255), int(c.green * 255), int(c.blue * 255))
        self.emit("edited")

    def _on_name(self, entry):
        self.track.name = entry.get_text()
        self.emit("edited")

    def _on_toggle(self, button, which):
        setattr(self.track, which, button.get_active())
        self.emit("mix-changed")

    def _on_arm(self, button):
        self.track.armed = button.get_active()
        self.emit("edited")

    def _on_pan(self, scale):
        self.track.pan = round(scale.get_value(), 3)
        self.emit("mix-changed")

    def _on_volume(self, scale):
        self.track.volume = round(scale.get_value(), 3)
        self._update_vol_label()
        icon = ("audio-volume-muted-symbolic" if self.track.volume < 0.01 else
                "audio-volume-low-symbolic" if self.track.volume < 0.5 else
                "audio-volume-medium-symbolic" if self.track.volume < 0.9 else
                "audio-volume-high-symbolic")
        self.vol_icon.set_from_icon_name(icon)
        self.emit("mix-changed")

    def _update_vol_label(self):
        import math
        v = self.track.volume
        self.vol_label.set_label("−∞ dB" if v < 0.001 else f"{20 * math.log10(v):+.1f} dB")
