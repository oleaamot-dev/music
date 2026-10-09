"""Application entry point."""

from __future__ import annotations

import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, Gtk  # noqa: E402

from . import APP_ID  # noqa: E402
from .engine import init as init_gst  # noqa: E402

CSS = """
.transport {
  padding: 6px 12px;
  border-bottom: 1px solid alpha(currentColor, 0.12);
}
.lcd {
  background: alpha(currentColor, 0.07);
  border-radius: 10px;
  padding: 3px 14px;
  margin: 0 6px;
  min-width: 112px;
}
.lcd label { font-family: monospace; font-feature-settings: "tnum"; }
.lcd .bars { font-size: 1.3em; font-weight: 800; }
.lcd .clock { font-size: 0.82em; opacity: 0.65; }
button.record-button { color: @error_color; }
button.record-button.recording {
  background: @error_bg_color; color: @error_fg_color;
  animation: pulse 1.2s ease-in-out infinite;
}
@keyframes pulse { 50% { opacity: 0.6; } }
.play-button { min-width: 42px; min-height: 42px; }

.track-headers { background: @view_bg_color; }
.ruler-spacer { border-bottom: 1px solid alpha(currentColor, 0.15); background: alpha(currentColor, 0.04); }
.track-header {
  padding: 6px 10px 6px 8px;
  border-bottom: 1px solid alpha(currentColor, 0.08);
}
.track-header.selected { background: alpha(@accent_bg_color, 0.14); }
.track-header entry.track-name { font-weight: 700; min-height: 30px; }
.track-header button.circular { min-width: 28px; min-height: 28px; padding: 0; font-weight: 800; }
.track-header togglebutton.mute:checked, .track-header button.mute:checked {
  background: @warning_bg_color; color: @warning_fg_color; }
.track-header togglebutton.solo:checked, .track-header button.solo:checked {
  background: @success_bg_color; color: @success_fg_color; }
.track-header button.arm { color: alpha(@error_color, 0.8); }
.track-header button.arm:checked { background: @error_bg_color; color: @error_fg_color; }
.track-header scale { padding-top: 2px; padding-bottom: 2px; }
.track-color button { padding: 3px; min-width: 22px; min-height: 22px; }
levelbar.meter trough { min-height: 8px; }
levelbar.meter.thin trough { min-height: 4px; }
levelbar.meter block.filled.low { background: @success_color; }
levelbar.meter block.filled.high { background: @warning_color; }
levelbar.meter block.filled.full { background: @error_color; }
"""


class GarageJamApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID,
                         flags=Gio.ApplicationFlags.HANDLES_OPEN)
        self.connect("startup", self._on_startup)

    def _on_startup(self, _app):
        init_gst()
        provider = Gtk.CssProvider()
        provider.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        quit_action = Gio.SimpleAction.new("quit", None)
        quit_action.connect("activate", lambda *_: self._quit())
        self.add_action(quit_action)
        self.set_accels_for_action("app.quit", ["<Ctrl>q"])
        self.set_accels_for_action("window.close", ["<Ctrl>w"])

    def _quit(self):
        for w in self.get_windows():
            w.close()

    def _window(self):
        from .window import GarageJamWindow
        win = self.get_active_window()
        if win is None:
            win = GarageJamWindow(self)
        return win

    def do_activate(self):
        self._window().present()

    def do_open(self, files, n_files, hint):
        win = self._window()
        win.present()
        for f in files:
            path = f.get_path()
            if path and path.endswith(".gjxml"):
                win.open_path(path)
                break
        else:
            win.import_files(list(files), -1, 0.0)


def main(argv=None) -> int:
    return GarageJamApp().run(sys.argv if argv is None else argv)
