# Dulcamara - the test game's screens: the dialogue, the menu, and renpy.input's (DULCAMARA_TEST_INPUT=1).
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

# renpy.input's: the question and the input at the foot of the screen, where Ren'Py's own GUI puts them
screen input(prompt):
    window:
        background Solid("#000000c0")
        xfill True
        yalign 1.0
        ysize 180
        padding (40, 30)
        vbox:
            spacing 10
            text prompt size 32 color "#ffffff"
            input id "input" size 32 color "#ffffff"
