# Dulcamara - the test game: a picture, a sound, a line of dialogue, a menu and a save. It writes what it does to
# the log ("DULCAMARA-TEST: ..."), and the headless frontend acts on that. With DULCAMARA_TEST_LOAD=1 it loads its
# save instead of starting over: Ren'Py then replays the menu choice the save holds.
# SPDX-License-Identifier: GPL-3.0-or-later

init python:
    import os

    def dulcamara_test(message):
        print("DULCAMARA-TEST: " + message)

    def dulcamara_after_load():
        dulcamara_test("loaded")

    config.after_load_callbacks.append(dulcamara_after_load)

default choice = None

# DULCAMARA_TEST_SKIP=1: unread dialogue may be skipped too, so that fast-forward's skip shows on a first run.
default preferences.skip_unseen = os.environ.get("DULCAMARA_TEST_SKIP") == "1"

# No main menu: the game starts at once.
label main_menu:
    return

label start:
    $ dulcamara_test("started")
    if os.environ.get("DULCAMARA_TEST_LOAD") == "1" and renpy.can_load("1-1"):
        $ renpy.load("1-1")
    scene expression Solid("#203040")
    show dulcamara at truecenter
    play music "audio/tone.wav"
    "Hello from Dulcamara's test game."
    $ dulcamara_test("advanced")
    menu:
        "Left":
            $ choice = "left"
        "Right":
            $ choice = "right"
    $ dulcamara_test("choice=" + choice)
    $ renpy.save("1-1")
    $ dulcamara_test("saved")
    "Saved."
    $ renpy.quit()
