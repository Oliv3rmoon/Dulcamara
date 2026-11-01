# Changelog

## 0.2.10

A test build after 0.2.9's console runs. The OGG crash is gone: the tester's title screen plays, she loaded a save
into the game, and 0.2.8's transparent menu is back. Left were black flashes at the borders of the game's boxes
(the dialogue box, the title screen's), its animations stopping, and fast cursor movement over its title menu
hanging the app for a while and losing the menu's background.

- **Images are made ready on the CPU (PS5).** A tester's shots showed images drawn with their colours right and black
  where they should be clear, changing from one picture to the next: a box around a small cup on a table, the title
  screen's whole logo, a strip beside a name tag, black at the borders of boxes. Ren'Py makes each image ready by
  drawing it premultiplied into its framebuffer (a blend of its own) and copying that into the image's texture, a
  movie's frame each other picture; the console took what was clear for black in the draws around them. Now none of
  it but the upload reaches the driver: as Ren'Py sets that blend after an upload, the core makes the pixels ready
  itself, premultiplied as the draw would to the bit (or as they are, opaque all over), and the blend, the draw and
  the copy are not made; the copy is an upload of them, whole, into the texture. On the build machine the picture of
  two images partly clear, one large and one under 64x64, is the same to the pixel made either way, and differs when
  the premultiplying is wrong (`DULCAMARA_GL_CPU_LOAD` chooses either).
- **A movie's picture waits for its mask's.** A movie with a mask of its own (`Movie(play=..., mask=...)`, as a
  box's animated border is) is two streams, each decoded on its own thread. Ren'Py uploaded a colour frame whose
  mask frame had not come yet as it was (`get_movie_texture`), and a masked movie's colour frames are black wherever
  its mask makes it clear: the borders flashed black each time the mask's frame came late on the console, and
  stayed black when the mask's stream did not open. Dulcamara's hook now keeps such a frame until its mask's comes
  (the movie keeps its last picture meanwhile), and the log counts the waits. The test game feeds Ren'Py's own
  function frames as the console's late masks come, in both Ren'Py cores.
- **FFmpeg starts no threads of its own (PS5).** 0.2.9's 4 threads a codec brought the menu back, but a screen
  can play several movies at once, each with its mask's stream and its next loop opened early, and 0.2.8's log had
  the console refuse threads at about 31 of the core's. Each stream now decodes on Ren'Py's own thread for it
  (`DULCAMARA_CODEC_THREADS` gives more).
- **A movie or sound the console gives no thread for no longer hangs the game.** Ren'Py makes a decode thread for
  each movie and sound, and drawing a movie's picture waited for that thread forever (`media_read_video`); fast
  cursor movement over a title menu, a sound for each button, could use up the threads. With none, it is now ready
  with nothing in it, as one whose file does not open is (`tools/patches/renpy-ffmedia-no-thread.diff`, Ren'Py's
  `ffmedia.c` in both cores).
- **A new sound channel is made while the mixer waits.** Ren'Py grows its channels (a movie's and its mask's among
  them) without the audio lock, while the mixer goes through them on its own thread: when they moved, the mixer read
  freed memory. The host tests crashed on it as they made many channels at once
  (`tools/patches/renpy-renpysound-channels.diff`, Ren'Py's `renpysound_core.c` in both cores).
- **The log names what does not open.** Whatever the trace's count: each movie file the first time it opens, each
  file and codec that does not open (with its file), each thread the console refuses, and the most of the core's
  threads that have run at once.
- **Sounds for this game.** A Ren'Py game's sounds, from its archives and its own files, are listed as it runs;
  Dulcamara's menu in game (or **Settings > Ren'Py > Sounds for a game**) turns any one off or on again. One that is
  off plays as a second of silence from memory, so the game's sound goes on as if it played, and its archive is not
  touched; a change counts from the next play. Asked for by a tester, for a game's hover sound.
- **Settings > System > Test builds.** With it on, the update offers test builds (pre-releases) too: the newest of
  the 20 newest releases, with the same checks as a release (this repository's own `PPSA99010.zip` and `SHA256SUMS`
  for its tag, the ZIP checked against them), so testers need not install each by hand. Turned off, a test build
  already offered is not installed. Asked for by a tester.
- Not yet tried on a console, any of it: the host tests (both Ren'Py cores, the update's checks) pass.

## 0.2.9

A test build after 0.2.8's console log, which ended at the same step in both of a tester's runs: the menu's MP3 and
its WebM movie, both read from the game's archives into memory, decoded; then the hover sound's OGG, read the same
way, opened its Vorbis decoder and took its first packet, and the app ended with no crash report. So the archive was
not it.

- **FFmpeg's FFT keeps its dispatch tables out of the code (PS5).** The app's code is mapped execute-only on the
  console (the executable's text segment has no read permission). FFmpeg 4.3.1's x86 FFT (`libavcodec/x86/fft.asm`)
  calls each transform size through a table of offsets kept in `.text` itself, so the IMDCT of a Vorbis packet (or
  an Opus one, through `mdct15`) read the app's own code and ended it; MP3 and the video codecs use no FFT. A patch
  (`tools/patches/ffmpeg-4.3.1-fft-tables.diff`) moves the four tables to `.rodata`, as offsets from a label at the
  start of the code, and the PS5 build fails if `fft.o` keeps them in its code. Each entry names the same function as
  before; an x86-64 build of the patched FFmpeg decodes a Vorbis file through the SSE FFT to the same samples as the
  unpatched one and FFmpeg's C code. The host tests play an OGG from a game's archive in both Ren'Py cores (the host
  build uses FFmpeg's C code). Not yet tried on a console.
- **A codec gets 4 threads at most (PS5).** Ren'Py opens each codec with FFmpeg's `threads=auto`, 16 threads for a
  movie on the console, each with a core thread's 2 MB stack. When the menu's movie was opened a second time (its
  next loop, it seems) while the first still decoded, its VP8 decoder got 6 of them and the console refused the rest
  (`EAGAIN`), so that decoder never opened. Each codec now gets 4
  (`DULCAMARA_CODEC_THREADS` sets another number). Not yet tried on a console.
- 0.2.8's presplash showed on the console as its game loaded.

## 0.2.8

A test build for Ren'Py on the console. 0.2.7 got the controller into Ren'Py (the D-pad moved the focus on the
menu), and showed that a thread's stack on the console is 64 KB unless asked for (the core's now get 2 MB). But the
game still ended without a crash report, even with a signal stack on every thread, at the same moment: as Ren'Py
played its first OGG, the hover sound, beside an MP3 (the music) and a WebM (the menu's movie). The build machine
plays the same three in both Ren'Py cores. Then a tester deleted the game's archive (`.rpa`) that holds the hover
sound, and the game played on past its menu.

- **Sounds in a game's archives are read before Ren'Py plays them.** Ren'Py hands its decode thread an archived
  sound as the archive itself, opened through SDL's own file reading, at the sound's offset. Dulcamara now reads the
  sound from the archive first, with Python's own file: up to 32 MB into memory, which Ren'Py then plays from; a larger
  one (a movie) is read through Python as it plays. Sounds in their own files are played as before. Not yet tried on
  a console: the build machine played archived sounds both ways.
- **Copied textures are written back from the cache first (PS5).** The missing options and save panels, the
  version number's flashing background and the dialogue box gone black: ps5-opengl zeroes a new texture through the
  CPU's cache and leaves those lines unwritten, while the GPU fills a texture Ren'Py copies (each image of 64x64 or
  more, each movie frame) past the cache; when the zeroed lines are written back later they put zeros over the copy.
  The core now has Ren'Py's `glCopyTexImage2D` write one texel first, which makes the driver write the whole empty
  texture back, and then copy into that storage (`glCopyTexSubImage2D`). The first four copies are checked against
  their source in the log, the old way and the new. Not yet tried on a console.
- **The picture is read back straight into the frame (PS5).** Reading it back took 13.5 ms of each 16.7 ms frame;
  ps5-opengl reads on the CPU at once anyway, so the pixel-pack buffers only added a conversion, two cache flushes
  and a copy. Every 600 frames the log now also gives what Ren'Py's OpenGL work costs (draws, uploads, new textures,
  copies, the slowest picture), and a picture whose OpenGL work took over 50 ms gets a line of its own; each image's
  size as Ren'Py loads it is logged too (the first 200).
- **A game's Fullscreen no longer blacks out the screen.** Picking Fullscreen in a game's own Options had Ren'Py
  make its window the display's size (3840x2160, as SDL reports the console's) and the picture with it; the screen
  went black. Fullscreen now reads as off in Ren'Py's games (the game's own value is kept), and the picture stays the
  game's own size, which the app scales to the screen.
- **An on-screen keyboard for a Ren'Py game's text.** When a game asks for text (a name to enter), Dulcamara's own
  keyboard comes up, away from the game's input, and goes away once the game has its text; the console's keyboard
  cannot be used (a title that imports it does not start). The D-pad, the left stick or the touchpad move on it, Cross
  types, Square erases, Triangle is a space, L1 or L2 shift (twice locks it), a short press of Options is Done, Circle
  puts it away for a game with its own OK button and R3 brings it back. The core sees Ren'Py's input
  (`interface.old_text_rect`), tells the frontend through Dulcamara's own environment call
  (`src/core/dulcamara_libretro.h`), and takes what is typed through libretro's keyboard callback, as pygame's own
  text events.
- **A game's presplash is shown while it loads.** Ren'Py draws `presplash.png` (or `.jpg`, or the
  `presplash_foreground` and `presplash_background` of a progress bar) into a window of its own, which the console
  never shows: the core now makes that window's picture, centred on black, its own picture until the game's first.
- **Each sound's source is logged.** Each sound Ren'Py plays or queues says, in `dulcamara.log`, where it comes from:
  the archive, the offset and the size, and how it is read, or its own file.
- **Nothing runs while another app has the screen.** Switching from a running Ren'Py game to another app, back, and
  away again took the console down, as opening Dulcamara after rest mode once did. While the PS5 has another app in
  front (`sceSystemServiceGetStatus`), Dulcamara now runs no game, draws and presents nothing and plays no sound, and
  `dulcamara.log` says when it leaves and gets the screen back.
- **What the game holds is logged every 3 minutes.** A tester's console went down after a 15-minute session: a
  `memory:` line in `dulcamara.log` now gives the app's heap, the direct memory ps5-opengl holds (each core counts
  its `sceKernelAllocateDirectMemory` and `sceKernelReleaseDirectMemory`, `ps5_memory.c`), Ren'Py's textures and
  image cache, and Python's objects, so a long session's log shows what grows.
- **What ends the app is logged.** `abort`, `exit`, `_exit` and `_Exit`, called anywhere in a core, write where they
  were called from to `dulcamara.log` before they end the app (`ps5_trace.c`).
- **FFmpeg's steps are logged.** For each stream Ren'Py opens: opening it, reading its streams, opening its codec,
  and its first packet, frame, read and conversion, each before and after, so the log ends at the step that did not
  come back. Each thread's start and end are logged too (Ren'Py starts one for each sound).

## 0.2.7

A test build for Ren'Py on the console: 0.2.6 showed a game's menu and its cursor, but the game ended at once, with
no crash report, on the first thing the controller did to it: the first move of the cursor, or the first arrow
(from the stick). Both are a game's first hover, and a hover is where a game first plays a sound after its music,
loads its buttons' hover pictures and asks whether the pointer is over a picture's opaque part.

- **The core's threads get room, and a stack for signals.** Ren'Py starts a thread to decode each sound it plays (with
  SDL's default stack), Python and ps5-opengl their own: each core's relocatable link now sends their
  `pthread_create` to `ps5_threads.c`. Each thread gets a stack of 2 MB at least, where the console's default may be
  far smaller, and a stack of its own for signals, so that a thread that overflows its stack is reported by the crash
  handler instead of ending the app without a word, as 0.2.6 ended. The log says once what stack a thread asked for.
- **The log says what Ren'Py was doing, at once.** Each sound Ren'Py plays or queues, each new focus and the first
  use of each picture go to `dulcamara.log` as they happen (`_dulcamara.now`), so a crash that leaves no report ends
  the log on the last of them.
- **In a Ren'Py game the touchpad is the pointer, and Options held opens Dulcamara's menu.** In 0.2.6 the touchpad's
  click did nothing in a Ren'Py game ("Touchpad isn't a button!": the app took it from SDL's joystick button 4, which
  is not the touchpad on every pad), so the menu with Keys for this game could not be reached, and the tester asked to
  keep the touchpad for the cursor. While a Ren'Py game runs, the app reads the click from the pad itself too, and it
  is a click where the pointer is; a touch moves the pointer. Dulcamara's menu opens with Options held for 0.6 s; a
  short press of Options is still the game's (its menu), given to it as Options is let go.
- **Ren'Py in Settings, outside a game too.** The tester found no Ren'Py settings in the Settings tab. Settings >
  Controls has "Buttons of" at its top, outside a game: RPG Maker, Ren'Py 8 or Ren'Py 7 / 6, each with its own buttons
  (a running game's are shown, fixed), and Ren'Py's rows say what they do in Ren'Py ("Pick and go on", "Game menu and
  back", "Free for a key"). Settings > Ren'Py is Ren'Py's page: fast-forward, **Keys for a game** (any of the
  library's Ren'Py games, Left and Right, Cross opens its keys page, which saves to that game's file and applies them
  at its next start), what each button does by default, the cursor, and Dulcamara's menu.
- **The core's last lines.** With no log callback left (as the core is being closed), the core wrote its lines with
  `fprintf`, which on the console is now the core's own and logs through the same place: they go to the descriptor
  itself.

## 0.2.6

A test build for Ren'Py on the console, with what its first tester asked for. 0.2.5 showed Ren'Py's picture (its
stand-in TV output worked: ps5-opengl asked for the output, and pictures came back lit), but the game's menu did not
answer the controller, its first launch ended without a crash report, and turning fast-forward on and off crashed
Ren'Py and then the console.

- **The controller is Ren'Py's keyboard and mouse.** 0.2.5 gave Ren'Py an SDL virtual controller, which played on
  the build machine and did nothing on the console. Each button is now a key of Ren'Py's keymap, put into SDL's
  events as the skip key always was, which every Ren'Py game knows: the D-pad (and the left stick) move between
  choices as the arrows, Cross is Return (pick, and move the story on), Circle and Options are Escape (the game's
  menu, and back out of it), L1 and R1 are Page Up and Down (roll back and forward), Square is H (hide the text
  window) and Triangle is Tab (skip on and off). A held direction repeats, as a key does. Letter keys carry their
  text, as Ren'Py's bindings by character need.
- **The cursor.** L3 turns it on and off: the left stick moves a pointer, drawn on the picture, and Cross clicks
  where it points. The touchpad moves it too, and it shows for two seconds after.
- **A game's own keys.** In a Ren'Py game, the menu (the touchpad) has **Keys for this game**: a page with each
  button but R2 and what it presses, its default or a key the game uses (its inventory's, say): a letter, a digit,
  F1 to F12, Space, Tab, Return, Escape, the arrows, Page Up and Down, Home, End, Delete, Backspace, Shift, Ctrl or
  Alt, or the cursor, a click or a right click. Left and Right go through them, L1 and R1 jump from letters to digits
  to F keys. A change applies at once, and Dulcamara keeps a game's keys for it in `keys/<the game's id>.txt` in its
  data folder (only the buttons that do not do their default). The core takes them as options
  (`renpy_key_<button>`) at the game's start and whenever they change; L2 now reaches the Ren'Py cores for it, and
  Settings > Controls has an L2 to give a button.
- **Ren'Py games have their own section in the library.** With both kinds of game, the shelf shows RPG Maker's games
  or Ren'Py's (8's, 7's and 6's together), under two tabs: Up and Down switch, and each section comes back to the game
  last chosen in it. Sorting applies within each. With one kind of game, the library is as it was.
- **Fast-forward in a Ren'Py game is Ren'Py's own skip only.** The app ran a Ren'Py core up to 8 times a frame while
  fast-forwarding, as it runs EasyRPG Player's, and each run is a picture drawn on the GPU and read back (14 ms on the
  console): the most GPU work a Ren'Py game gave the console. A Ren'Py core now runs once a frame while
  fast-forwarding, and Ren'Py skips at its own pace; the badge says "Skip". Each picture's read-back buffer is also
  made once for its size, where 0.2.5 had ps5-opengl allocate and retire one each frame. The log notes each time
  fast-forward is turned on and off.
- **What the core's libraries print is written to the log at once.** When the GPU stops answering for 30 seconds,
  ps5-opengl prints why and ends the app at once (`_Exit`, so no crash report); 0.2.5 held such lines for the next
  frame, which never came. 0.2.5's first launch ended that way or another way that leaves no report.
- **Ren'Py's own log goes to `dulcamara.log`:** how long each stage of a game's start took ("Loading script took
  ..."), and its warnings, the first 200 lines. 0.2.5's 6.5 seconds before the first picture are Ren'Py's start,
  and the next log says which stage they are.
- **Ren'Py no longer backs up a game's scripts** into the saves folder each time it compiles them, a developer's
  backup that only cost a game's start time and storage on the console.
- **No mipmaps in Ren'Py 8 unless a game asks for them** (`config.mipmap` is off by default). ps5-opengl makes the
  mipmaps of a picture whose sides are not powers of two, nearly every picture a visual novel has, on the CPU, pixel
  by pixel, while Ren'Py waits to draw: seconds of work as a scene's pictures load. Both of 0.2.5's silent ends came
  right after it had made some (and its log held the next frame's lines, so what came after is not known). Ren'Py 7
  makes mipmaps in its compiled code, which a setting does not reach.
- **Checked on the build machine:** both Ren'Py cores play the test game with the controller as keys (Cross, Down,
  Cross), move it on from a game's own key on L2, and draw the cursor, move it with the stick and click with Cross
  (`tests/test_renpy_core.py`, `tests/test_renpy7_core.py`); the library's sections keep each kind of game together
  in each sort (`tests/library_check.c`).

## 0.2.5

A test build for Ren'Py on the console: 0.2.4 ran a Ren'Py game without crashing, with its music, but its picture
stayed black. Its log shows Ren'Py drawing, and its pictures reaching the screen.

- **ps5-opengl no longer asks for the TV.** Before its first draw, even one into a framebuffer of its own (as
  Ren'Py's are), ps5-opengl opens the console's video output and registers buffers of its own on it, and it drops
  every draw until that works. The app already has the output, for its Vulkan screen. The core only reads Ren'Py's
  pictures back, and never shows a GL window surface, which is the only way ps5-opengl puts anything on the TV.
  Both Ren'Py cores now give ps5-opengl a stand-in output (`ps5_gl_output.c`): a port that is never the TV's,
  buffers that are noted and never shown, and flips done as soon as they are asked for.
- **The log says more about the picture.** For the first pictures read back, and every 600th after them, it says
  how much of each is not black, and any OpenGL error. ps5-opengl's own reports, and anything else the core's
  libraries print (Python's and FFmpeg's errors), go to `dulcamara.log` too, up to 300 lines.
- The 13 seconds of black before Ren'Py's window opened, in 0.2.4's log, are most likely Ren'Py loading the game
  behind its presplash (the 480x300 window the log names first), which the core does not show yet.

## 0.2.4

A test build for Ren'Py on the console: 0.2.3 got Ren'Py started (Python, its boot script, SDL) and crashed as
Ren'Py opened its log.

- **The cause, from 0.2.3's crash report:** the crash was in the console's `strlen`, given no string, from Python's
  `time.gmtime()`, which Ren'Py calls for its log's first line (the time it opened, in UTC). Python is built for a
  `struct tm` that names its time zone (`tm_zone`) and reads that name without checking it; the core's own `gmtime_r`
  left it empty. The stack read back through the 0.2.3 build itself: `strlen` from `PyUnicode_DecodeLocale` from
  `tmtotuple` from `time_gmtime`.
- **Both Ren'Py cores' times give a zone.** UTC's is "UTC". Local time takes only the C standard's nine fields from
  the console's `localtime` (its `struct tm` may not have the offset or the zone where Python looks), works out the
  offset from them, and names the zone as the time zone database names one without an abbreviation ("+0530", "-08").
- **Python's `mbstowcs` is UTF-8.** Python decodes that zone name and every `OSError`'s message through `mbstowcs`,
  even in its UTF-8 mode, and the only `mbstowcs` a title has is `libSceLibcInternal`'s, as with `getcwd`, which
  crashed in 0.2.2; EasyRPG Player's core never calls it. The cores now decode such text as UTF-8 themselves.
- **The log says when each step came:** every "renpy: step:" line ends with the seconds since the game started
  loading, so a log shows where a long black screen goes (0.2.3's grew longer on each launch, and its log could not
  say why).
- **Checked on the build machine:** `tests/ps5_libc_check.c` (run by `tests/test_renpy_core.py`) checks the cores'
  `gmtime_r` against the build machine's from 1900 to 2100 and its `localtime_r` in ten time zones from 2000 to 2031,
  given only the nine fields, and their `mbstowcs` against a UTF-8 locale's. 0.2.3's code fails it.

## 0.2.3

A test build for Ren'Py on the console (0.2.2 still crashed as Python started, one logged step later, in its
main part), and the community in the app: nightly, the Discord of Dulcamara and the other Nightshade apps.

- **The crash reports were read wrongly, and 0.2.2's cause was not the cause.** The console's kernel puts a crashed
  thread's registers 64 bytes into the context a handler is given, not 16 as the SDK's header says (a PS5 port of
  Wine measured the same). Every report so far read them 48 bytes early: its "rsp" was the fault address and its
  "rip" another register, which is why the stack pointer looked like a string's address. `readlink` was still worth
  standing in for (a title cannot reach it), but it was not what crashed.
- **The cause, read again at the right place:** Python's start calls `getcwd`. Finding no `renpy` program to start
  from, Python takes its own folder as the current one, and a PS5 title's libc has no `getcwd`: the one the link
  took, `libSceLibcInternal`'s, crashed. 0.2.2's registers are `getcwd`'s buffer, its size (1024) and the buffer's
  end. Both Ren'Py cores now keep the working folder themselves: each starts in its game's folder, `chdir` changes
  the console's and notes the new one, and `getcwd` and `realpath` answer from it.
- **`time.strftime` goes through `strftime`.** Python calls `wcsftime` for it (Ren'Py's save times), which, like
  `getcwd`, only `libSceLibcInternal` has and no title is known to have run; `strftime` runs on the console (EasyRPG
  Player's core uses it).
- **Crash reports** read the registers where the console puts them (checked against the fault address, and saying
  which layout they used), name the system module holding an address that is not in the app's list, and find return
  addresses in any executable memory.
- **Settings > About > Join the community.** About's first row has the server's name and short link (nightly ·
  discord.gg/pSqgRU3YP9). Cross shows a card with a QR code that opens `https://discord.gg/pSqgRU3YP9` on a phone;
  Cross or Circle puts it away. About's rows now all fit on its page, a little closer together.
- **Once, on the empty library.** The first time the library is empty, its card ends with "Join nightly, Dulcamara's
  community on Discord: discord.gg/pSqgRU3YP9" and where the QR code is. It stays while the app is open and never
  comes back: the settings file keeps `community_seen = 1` as soon as it is shown. A library with games never shows
  it; About always has the invitation.
- **The QR code is made on the console**, by Project Nayuki's QR Code generator (MIT): Mandrake's copy, unmodified,
  in `third_party/qrcodegen`.
- **Checked on the build machine.** `tests/test_community.py` (CI runs it before the long builds) makes the QR code
  with the app's own encoder (`src/ui/qr.h`) and reads it back with a reader of its own: it must open exactly
  `https://discord.gg/pSqgRU3YP9`. The reader also reads a QR code segno made. The About row, its card, the
  one-time line and its flag are checked in the sources, and the interface fonts have every character of the new
  texts. `tools/desktop/community.txt` is an interface preview script for the three screens.

The interface is still in English only.

## 0.2.2

A test build for Ren'Py on the console: 0.2.1 still crashed as Python started, at the same place.

- **The cause, found from 0.2.1's log:** Python's start calls `readlink` (to find its own path), and the core's
  `readlink` came from `libkernel_sys`, whose functions a title's import does not reach: the call ran something else
  with readlink's arguments, which set the stack pointer to a string's address (the crash's address, the same in
  both builds). Both Ren'Py cores now stand in for every function they could only get from `libkernel_sys`
  (`readlink`, `link`, `symlink`, `chown`, `umask` and the rest: a title has no links, owners or sessions to
  change), and the build fails if a core needs one it does not stand in for.
- **Closing a Ren'Py game no longer hangs Ren'Py 7.** The core asked Ren'Py to quit with an exception raised in its
  thread wherever it was; inside Python 2's `threading.RLock` that left a lock taken and Ren'Py 7 waiting on it for
  good (CI's tests caught it, and the log's stack showed where). Ren'Py now quits at its next look for events, as
  its own Quit does, without asking "are you sure"; the exception is only a fallback after a second, raised through
  Python's pending calls so the app never waits on Ren'Py for it.
- **Python starts in two logged parts** (its core, then its main part), so a log narrows a stop further.
- **Crash reports** give the registers and the code at the faulting instruction where it can be read, list where
  the app and the system modules are in the log, and no longer read a stack that is not mapped.

## 0.2.1

A test build for Ren'Py on the console: 0.2.0 crashed on a PS5 as a Ren'Py 8 game started.

- **Ren'Py's cores keep an environment of their own.** On the console 0.2.0's crash came a moment after Ren'Py's
  thread started, in a system library reading a string through a bad pointer, where Python copies the console's
  environment (`environ`) into `os.environ`. The cores no longer read the console's environment or call its
  `setenv`: Python, SDL and FFmpeg inside them see the core's own few variables.
- **The log says how far a Ren'Py game got.** Each step of starting it is written to `dulcamara.log` at once
  ("renpy: step: ..."): loading, Ren'Py's thread, Python starting and started, the boot script, SDL, OpenGL and the
  first picture.
- **Crash reports name what crashed.** A report now says which module the faulting instruction, the faulting
  address and each return address are in (the title or a system module, with the offset), and whether the main
  thread or another crashed. It reads the crashing thread's stack only as far as it is mapped: 0.2.0's report
  stopped short on a thread that had just started.

## 0.2.0

Ren'Py visual novels in the same app, on Ren'Py itself.

- **Ren'Py 8 inside the app.** Ren'Py 8.5.3 as Dulcamara's own libretro core (`src/core/renpy`), with CPython
  3.12.8, FFmpeg 4.3.1 (LGPL) and Assimp, drawing through ps5-opengl into the Vulkan screen. Ren'Py games are found
  by their `game/` folder, with their launcher's name and `gui/main_menu.png` as the cover; Cross moves the story
  on, Circle opens the game's menu, L1 and R1 roll back and forward, R2 also skips read text, the touchpad is the
  mouse, and each core keeps its own buttons. Saves go to `/data/dulcamara/saves/renpy`. Checked on the build
  machine and in CI by `tests/test_renpy_core.py`, which plays a test game written for it.
- **Ren'Py 7 inside the app,** for Ren'Py 7 and 6 games: Ren'Py 7.8.7 on the same core, with CPython 2.7.18 and
  pygame_sdl2 (`tools/build-renpy7.sh`). The library sends each game to the Ren'Py it was made with, by its
  `renpy/` folder or, without one, its `lib/`. `tests/test_renpy7_core.py` plays the same test game on it.
- Ren'Py runs one game each time the app starts: opening another Ren'Py game starts the app again, and the game
  opens then.
- The shelf names a Ren'Py game's Ren'Py version.

## 0.1.0

The first Dulcamara, a Nightshade app: RPG Maker 2000 and 2003 games on the PS5, on EasyRPG Player.

- **EasyRPG Player inside the app.** Its libretro core (EasyRPG/Player at `096c004`, liblcf at `6854310`) is
  built for the PS5 into the app, with the text encodings of Japanese, Korean, Chinese, Cyrillic and Western
  games through iconv.
- **The library.** Games in `/data/dulcamara/games`: a folder holding `RPG_RT.ldb` and `RPG_RT.lmt`, one
  folder down, or a `.zip` / `.easyrpg`. Titles from `RPG_RT.ini`, covers from each game's title screen, sorted by
  title or the last played.
- **Playing.** Cross confirms and Circle cancels; every button can be mapped. R2 fast-forwards (x2 to x8), the
  touchpad opens the menu (resume, fast-forward, back to the title screen, settings, quit).
- **The picture.** Mandrake's shaders (CRT Royale and the rest) on EasyRPG's picture, sharp or smooth
  scaling, whole-number scaling, 4:3 or 16:9, Scale2x and xBR.
- **Nightshade's look,** its own icon and home-screen pictures, and updates from
  [Oliv3rmoon/Dulcamara](https://github.com/Oliv3rmoon/Dulcamara/releases).
- Built on the frontend of Mandrake (formerly Nightshade Station) 1.6.0, without its PS1 parts.
