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

    # DULCAMARA_TEST_HOLD=1: a game with a handler of its own that catches the first quit the core raises in it (half
    # a second in, it spins until something is raised), and then spins on: only the core asking again ends it.
    dulcamara_hold = {"calls": 0, "done": False}

    def dulcamara_hold_on():
        dulcamara_hold["calls"] += 1
        if dulcamara_hold["done"] or dulcamara_hold["calls"] < 10:
            return
        dulcamara_hold["done"] = True
        try:
            while True:
                pass
        except Exception:
            dulcamara_test("held on to a quit")
        while True:
            pass

    if os.environ.get("DULCAMARA_TEST_HOLD") == "1":
        config.periodic_callbacks.append(dulcamara_hold_on)

    # DULCAMARA_TEST_MOVIE_MASK=1: a movie's colour and mask frames as get_movie_texture reads them, the mask's a tick
    # late in turn: whether each colour frame drawn had its mask's transparency (each tick's "new picture", then each
    # colour frame's alpha: 0 once its mask, clear all over, is applied)
    def dulcamara_movie_mask():
        import renpy.display.video as video
        frames = {"colour": [], "mask": []}

        class Channel(object):
            def __init__(self, name):
                self.name = name

            def read_video(self):
                return frames[self.name].pop(0) if frames[self.name] else None

        def surface(red):
            s = renpy.display.pgrender.surface((4, 4), True)
            s.fill((red, 0, 0, 255))
            return s

        music = renpy.audio.music
        playing, channel = music.get_playing, music.get_channel
        music.get_playing = lambda name=None: True
        music.get_channel = lambda name: Channel(name)
        try:
            colours = [surface(255), surface(255), surface(255)]
            ticks = [(colours[0], surface(0)), (colours[1], None), (None, surface(0)), (colours[2], None)]
            news = []
            for colour, mask in ticks:
                frames["colour"] = [colour] if colour is not None else []
                frames["mask"] = [mask] if mask is not None else []
                news.append(video.get_movie_texture("colour", "mask")[1])
        finally:
            music.get_playing, music.get_channel = playing, channel
        return "new %s alpha %s" % (news, [s.get_at((0, 0))[3] for s in colours])

    # DULCAMARA_TEST_COPIES=1: a sound played six times at once on Ren'Py's audio channel, which opens another channel
    # for each while the others play (and starts them as it next goes through its channels): on how many channels it
    # plays after
    def dulcamara_copies_play():
        for _i in range(6):
            renpy.play("audio/tone.ogg", channel="audio")

    def dulcamara_copies():
        import renpy.audio.renpysound as renpysound
        playing = 0
        for c in renpy.audio.audio.all_channels:
            number = getattr(c, "_number", None)
            name = renpysound.playing_name(number) if number is not None else None
            if name and name.endswith("tone.ogg"):
                playing += 1
        return str(playing)

    # DULCAMARA_TEST_AUTOSAVE=1: an autosave the console gives no thread for (threading's start raising as Python's does
    # then): whether its file is written, and whether Ren'Py takes the next autosave as one that may run
    def dulcamara_autosave():
        import threading
        start = threading.Thread.start

        def refused(self):
            raise RuntimeError("can't start new thread")

        threading.Thread.start = refused
        config.has_autosave = True  # the test game's are off (options.rpy)
        try:
            renpy.force_autosave()
        finally:
            threading.Thread.start = start
            config.has_autosave = False
        loadsave = renpy.loadsave
        return "autosave %s, another may run: %s" % ("made" if renpy.can_load("auto-1") else "missing",
                                                    loadsave.autosave_not_running.is_set())

    # DULCAMARA_TEST_THREAD=1: a thread of the game's own that is still running when the game is closed
    def dulcamara_forever():
        import time
        while True:
            time.sleep(0.05)

    if os.environ.get("DULCAMARA_TEST_THREAD") == "1":
        renpy.invoke_in_thread(dulcamara_forever)

    # DULCAMARA_TEST_REPORT=error: an error reported as Ren'Py reports one that escapes it (in an editor too, unless
    # told not to); =editor: the editor opened as Ren'Py would, to show the test's stand-in editor would see it.
    def dulcamara_report():
        import sys
        what = os.environ.get("DULCAMARA_TEST_REPORT")
        if what == "error":
            try:
                raise Exception("a test error, reported")
            except Exception as e:
                sys.modules["renpy.error"].report_exception(e)
        elif what == "editor":
            renpy.launch_editor([config.gamedir + "/script.rpy"], 1, transient=True)

default choice = None

# DULCAMARA_TEST_SKIP=1: unread dialogue may be skipped too, so that fast-forward's skip shows on a first run.
default preferences.skip_unseen = os.environ.get("DULCAMARA_TEST_SKIP") == "1"

# No main menu: the game starts at once.
label main_menu:
    return

label start:
    $ dulcamara_test("started")
    # DULCAMARA_TEST_ALPHA=1: two images partly clear (images/dulcamara_glass.png, 96x80, and dulcamara_dot.png,
    # 24x30, written by the test) over the picture, and nothing else moves: the picture made with Ren'Py's own way of
    # making images ready and with the core's on the CPU must be the same
    if os.environ.get("DULCAMARA_TEST_ALPHA") == "1":
        scene expression Solid("#203040")
        show dulcamara at truecenter
        show dulcamara_glass at Position(xpos=200, ypos=150, xanchor=0, yanchor=0)
        show dulcamara_dot at Position(xpos=900, ypos=200, xanchor=0, yanchor=0)
        $ dulcamara_test("alpha shown")
        $ renpy.pause(hard=True)
    $ dulcamara_report()
    if os.environ.get("DULCAMARA_TEST_LOAD") == "1" and renpy.can_load("1-1"):
        $ renpy.load("1-1")
    # DULCAMARA_TEST_FULLSCREEN=1: the game turns fullscreen on, as its own Options can; it stays off on the console
    if os.environ.get("DULCAMARA_TEST_FULLSCREEN") == "1":
        $ _preferences.fullscreen = True
        $ dulcamara_test("fullscreen=" + str(_preferences.fullscreen))
    if os.environ.get("DULCAMARA_TEST_COPIES") == "1":
        $ dulcamara_copies_play()
        $ renpy.pause(0.3, hard=True)
        $ dulcamara_test("copies playing: " + dulcamara_copies())
    if os.environ.get("DULCAMARA_TEST_MOVIE_MASK") == "1":
        $ dulcamara_test("movie mask: " + dulcamara_movie_mask())
    if os.environ.get("DULCAMARA_TEST_AUTOSAVE") == "1":
        $ dulcamara_test(dulcamara_autosave())
    # DULCAMARA_TEST_FLASH=1: a black box, 160x120 at 320,240, over the picture for a moment, as a tester's boxes
    # flash black: the core's log finds it in its pictures
    if os.environ.get("DULCAMARA_TEST_FLASH") == "1":
        scene expression Solid("#203040")
        $ renpy.pause(0.3, hard=True)
        show expression Solid("#000000", xysize=(160, 120)) as dulcamara_flash:
            xpos 320 ypos 240
        $ renpy.pause(0.05, hard=True)
        hide dulcamara_flash
        $ renpy.pause(0.5, hard=True)
        $ dulcamara_test("flash shown")
    scene expression Solid("#203040")
    show dulcamara at truecenter
    # DULCAMARA_TEST_MUSIC: another of the game's sounds than its tone (audio/tone.ogg, say)
    play music os.environ.get("DULCAMARA_TEST_MUSIC", "audio/tone.wav")
    # DULCAMARA_TEST_INPUT=1: a name asked for, as a game asks (renpy.input); the headless frontend types it through
    # the core's keyboard callback, as Dulcamara's on-screen keyboard does
    if os.environ.get("DULCAMARA_TEST_INPUT") == "1":
        $ dulcamara_name = renpy.input("What is your name?", length=20)
        $ dulcamara_test("name=" + dulcamara_name)
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
