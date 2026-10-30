# Dulcamara - the test game's two screens: the dialogue and the menu.
# SPDX-License-Identifier: GPL-3.0-or-later

screen say(who, what):
    window id "window":
        background Solid("#000000c0")
        xfill True
        yalign 1.0
        ysize 180
        padding (40, 30)
        text what id "what" size 32 color "#ffffff"

# Left has the focus when the menu opens, so Down then Cross chooses Right.
screen choice(items):
    vbox:
        xalign 0.5
        yalign 0.5
        spacing 20
        for i in items:
            textbutton i.caption:
                action i.action
                default_focus (1 if i.caption == "Left" else 0)
                text_size 40
                text_idle_color "#a0a0a0"
                text_hover_color "#ffffff"
