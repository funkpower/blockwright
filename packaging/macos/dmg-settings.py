# dmgbuild settings for the Blockwright disk image (scripts/package-macos):
#   dmgbuild -s dmg-settings.py -D app=<Blockwright.app> -D background=<dmg-background.png> \
#            "Blockwright <version>" <out.dmg>
# The window shows the app beside a link to /Applications over the
# background (its @2x twin beside it on Retina screens).
import os.path

application = defines["app"]  # noqa: F821 (dmgbuild provides `defines`)
appname = os.path.basename(application)

format = "UDZO"
filesystem = "HFS+"
files = [application]
symlinks = {"Applications": "/Applications"}
icon = os.path.join(application, "Contents", "Resources", "blockwright.icns")
badge_icon = None

background = defines["background"]  # noqa: F821
window_rect = ((200, 120), (660, 400))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = False
include_icon_view_settings = True
include_list_view_settings = False

arrange_by = None
icon_size = 128
text_size = 13
label_pos = "bottom"
icon_locations = {appname: (170, 230), "Applications": (490, 230)}
