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

    # DULCAMARA_TEST_LOOP_FAIL=<n>: audio/flaky.wav, a copy of the tone, whose n reads after its first fail in Ren'Py's
    # loader, as an archive read can once
    dulcamara_flaky = {"loads": 0}

    def dulcamara_flaky_copy():
        with open(os.path.join(config.gamedir, "audio", "tone.wav"), "rb") as f:
            data = f.read()
        with open(os.path.join(config.gamedir, "audio", "flaky.wav"), "wb") as f:
            f.write(data)
        fails = int(os.environ.get("DULCAMARA_TEST_LOOP_FAIL"))
        load = renpy.loader.load

        def flaky_load(name, *args, **kwargs):
            if "flaky" in name:
                dulcamara_flaky["loads"] += 1
                if 1 < dulcamara_flaky["loads"] <= 1 + fails:
                    raise IOError("injected: the read of %s failed" % name)
            return load(name, *args, **kwargs)

        renpy.loader.load = flaky_load

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

    # DULCAMARA_TEST_REFUSE_FILE=<path>: a thread of the game's own that the system refuses (the test's
    # refuse_threads library refuses every thread while that file exists): what the game was told
    def dulcamara_refused_thread():
        import threading
        path = os.environ.get("DULCAMARA_TEST_REFUSE_FILE")
        open(path, "w").close()
        try:
            threading.Thread(target=lambda: None).start()
            told = "started"
        except Exception as e:
            told = str(e)
        finally:
            os.remove(path)
        return told

    # how many files this process has open (DULCAMARA_TEST_FILES)
    def dulcamara_open_files():
        count = 0
        for fd in range(4096):
            try:
                os.fstat(fd)
                count += 1
            except OSError:
                pass
        return count

    # DULCAMARA_TEST_VOLUME=<main>,<music>: the game's main and music volume sliders moved to those fractions of
    # their length, as a player moves them (the bar's own adjustment), before its music plays
    # ("-" leaves that slider where it is; "set=<v>", preferences.set_mixer(<v>) as the game's Preference("music
    # volume", <v>) does; "amp=<v>", the amplitude <v> stored as an earlier Dulcamara stored a slider, and saved).
    # DULCAMARA_TEST_DEFAULT_MUSIC=<v>: the game's config.default_music_volume.
    def dulcamara_sliders(setting):
        for mixer, fraction in zip(("main", "music"), setting.split(",")):
            if fraction.startswith("set="):
                _preferences.set_mixer(mixer, float(fraction[4:]))
            elif fraction.startswith("amp="):
                _preferences.set_volume(mixer, float(fraction[4:]))
                renpy.save_persistent()
            elif fraction != "-":
                adjustment = MixerValue(mixer).get_adjustment()
                adjustment.change(adjustment.range * float(fraction))
        dulcamara_test("volumes: main %.4f, music %.4f" % (_preferences.get_volume("main"),
                                                            _preferences.get_volume("music")))
        dulcamara_test("sliders: main %.4f, music %.4f" % (_preferences.get_mixer("main"),
                                                            _preferences.get_mixer("music")))

    if os.environ.get("DULCAMARA_TEST_DEFAULT_MUSIC"):
        config.default_music_volume = float(os.environ.get("DULCAMARA_TEST_DEFAULT_MUSIC"))

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
    # tests/test_renpy_soak.py's long session, in a file of its own it writes beside this one
    if renpy.has_label("dulcamara_soak"):
        jump dulcamara_soak
    # DULCAMARA_TEST_LABEL: a label of a file a test writes beside this one, played instead
    if os.environ.get("DULCAMARA_TEST_LABEL"):
        jump expression os.environ.get("DULCAMARA_TEST_LABEL")
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
    # DULCAMARA_TEST_LOOP=<a sound>: played as the game's music (which loops) for three times its second: whether it
    # still plays after
    if os.environ.get("DULCAMARA_TEST_LOOP"):
        play music os.environ.get("DULCAMARA_TEST_LOOP")
        $ renpy.pause(3.5, hard=True)
        $ dulcamara_test("music after 3.5 s: " + str(renpy.music.get_playing()))
    # DULCAMARA_TEST_LOOP_GONE=1: the game's music, a copy of its tone, taken away as it plays: Ren'Py stops looping it,
    # and the log says why
    if os.environ.get("DULCAMARA_TEST_LOOP_GONE") == "1":
        $ dulcamara_once = os.path.join(config.gamedir, "audio", "once.ogg")
        $ dulcamara_tone = open(os.path.join(config.gamedir, "audio", "tone.ogg"), "rb").read()
        $ open(dulcamara_once, "wb").write(dulcamara_tone)
        play music "audio/once.ogg"
        $ renpy.pause(0.3, hard=True)
        $ os.remove(dulcamara_once)
        $ renpy.pause(9.0, hard=True)
        $ dulcamara_test("music after it was gone: " + str(renpy.music.get_playing()))
    # DULCAMARA_TEST_LOOP_FAIL=<n>: the game's music, a second long, whose n reads after its first fail: four and a
    # half seconds on, the frontend listens whether it still plays
    if os.environ.get("DULCAMARA_TEST_LOOP_FAIL"):
        $ dulcamara_flaky_copy()
        play music "audio/flaky.wav"
        $ renpy.pause(4.5, hard=True)
        $ dulcamara_test("music after 4.5 s, listen: " + str(renpy.music.get_playing()))
        $ renpy.pause(2.0, hard=True)
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
    if os.environ.get("DULCAMARA_TEST_REFUSE_FILE"):
        $ dulcamara_test("thread refused: " + dulcamara_refused_thread())
    # DULCAMARA_TEST_FILES=<n>: the game's sound played and stopped n times: how many more files the process has open
    # after than before (each play opens the sound's file, and Ren'Py's sound closes it as it ends)
    if os.environ.get("DULCAMARA_TEST_FILES"):
        $ dulcamara_files_before = dulcamara_open_files()
        $ dulcamara_k = 0
        while dulcamara_k < int(os.environ.get("DULCAMARA_TEST_FILES")):
            play sound "audio/tone.wav"
            $ renpy.pause(0.1, hard=True)
            stop sound
            $ renpy.pause(0.05, hard=True)
            $ dulcamara_k += 1
        $ renpy.pause(0.5, hard=True)
        $ dulcamara_test("files: %d more open after %d sounds" % (dulcamara_open_files() - dulcamara_files_before,
                                                                 dulcamara_k))
    if os.environ.get("DULCAMARA_TEST_VOLUME"):
        $ dulcamara_sliders(os.environ.get("DULCAMARA_TEST_VOLUME"))
    scene expression Solid("#203040")
    show dulcamara at truecenter
    # DULCAMARA_TEST_MUSIC: another of the game's sounds than its tone (audio/tone.ogg, say)
    play music os.environ.get("DULCAMARA_TEST_MUSIC", "audio/tone.wav")
    if os.environ.get("DULCAMARA_TEST_VOLUME"):
        $ renpy.pause(1.0, hard=True)
        $ dulcamara_test("music played at those volumes")
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
