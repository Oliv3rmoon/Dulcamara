# SPDX-License-Identifier: GPL-3.0-or-later

define config.name = "Dulcamara itch.io Shelf"
define config.version = "1.0"
define config.screen_width = 1280
define config.screen_height = 720
define config.save_directory = "dulcamara-itch-shelf"
define config.has_autosave = False
define config.has_quicksave = False
define config.rollback_enabled = False
define config.window_icon = None
define _game_menu_screen = None

init python:
    import dulcamara_shelf

    def shelf_text(value):
        return value.replace("{", "{{")

style shelf_button is button:
    background Solid("#21171b")
    hover_background Solid("#6a2638")
    padding (18, 9)

style shelf_button_text is button_text:
    color "#f5ecee"
    hover_color "#ffffff"
    size 25

style shelf_text is text:
    color "#e9dfe2"
    size 25

label main_menu:
    jump start

label start:
    $ shelf_browser = dulcamara_shelf.Browser(config.basedir)
    call screen itch_shelf(shelf_browser)
    $ shelf_browser.cancel()
    $ renpy.quit()

screen itch_shelf(b):
    style_prefix "shelf"
    add Solid("#100d10")
    add Solid("#df536d") xpos 56 ypos 52 xysize (5, 58)
    vbox:
        xpos 82 ypos 45 xmaximum 1120 spacing 5
        text "D U L C A M A R A" size 19 color "#f27a8e"
        text "itch.io shelf" size 44 bold True

    vbox:
        xpos 82 ypos 155 xmaximum 1115 spacing 12
        if b.mode == "home":
            text "Free visual novels" size 39
            text "Browse itch.io’s free Ren’Py listings and install a creator’s PC or Linux ZIP."
            textbutton "Browse games" default_focus True action Function(b.refresh)
            textbutton "Return to Dulcamara" action Return()
        elif b.mode == "games":
            text "Free Ren’Py games" size 32
            for title, url in b.rows[b.page:b.page+b.per_page]:
                button:
                    action Function(b.select, title, url)
                    text shelf_text(title) substitute False
            hbox:
                spacing 12
                if b.page:
                    textbutton "Previous" action SetField(b, "page", b.page-b.per_page)
                if b.page + b.per_page < len(b.rows):
                    textbutton "Next" action SetField(b, "page", b.page+b.per_page)
                textbutton "Refresh" action Function(b.refresh)
                textbutton "Close shelf" action Return()
        elif b.mode == "project":
            text shelf_text(b.title) size 32 substitute False
            text b.project.page size 20 color "#d797a5" substitute False
            if b.files:
                for name, upload in b.files[b.file_page:b.file_page+3]:
                    button:
                        action Function(b.choose, name, upload)
                        text shelf_text(name) size 21 substitute False
            hbox:
                spacing 12
                if not b.files:
                    textbutton "Find free downloads" action Function(b.list_files)
                if b.file_page:
                    textbutton "Previous" action SetField(b, "file_page", b.file_page-3)
                if b.file_page + 3 < len(b.files):
                    textbutton "More files" action SetField(b, "file_page", b.file_page+3)
                textbutton "Back to games" action [SetField(b, "mode", "games"), SetField(b, "status", "")]
        elif b.mode == "confirm":
            text "Add to your library?" size 36
            text shelf_text(b.name) substitute False
            text b.project.page size 22 color "#d797a5" substitute False
            text "Games can run creator-supplied Python. Choose creators you trust."
            hbox:
                spacing 12
                textbutton "Install game" action Function(b.begin)
                textbutton "Back" action SetField(b, "mode", "project")
        elif b.mode == "download":
            text shelf_text(b.title) size 32 substitute False
            textbutton "Cancel download" action Function(b.cancel)
            timer 0.04 repeat True action Function(b.tick)
        elif b.mode == "installed":
            text "Added to your games folder" size 38
            text "Close the shelf and rescan the Dulcamara library to play."
            textbutton "Return to Dulcamara" action Return()
            textbutton "Browse more games" action [SetField(b, "mode", "games"), SetField(b, "status", "")]
        if b.status:
            text shelf_text(b.status) size 24 color "#ffd0d8" substitute False
