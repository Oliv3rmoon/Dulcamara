"""Controller integration through the real scripted SDL app, no private state mutation.

The preview is built from the current sources first (tools/build-desktop.sh preview, incremental), so a stale
binary is never what these tests exercise. The only skips are a missing cmake or missing SDL2 development files.
"""
import csv, json, os, shutil, statistics, subprocess, tempfile, threading, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PREVIEW=ROOT/'build/desktop-preview/dulcamara'
_built=None

def build_preview():
    """Build once per process; returns None or the exact reason the host cannot build the preview."""
    global _built
    if _built is None:
        if not shutil.which('cmake'):
            _built='needs cmake'
        else:
            build=subprocess.run(['bash',str(ROOT/'tools/build-desktop.sh'),'preview'],capture_output=True,text=True)
            log=ROOT/'build/desktop-preview.log'
            configure=log.read_text(errors='replace') if log.exists() else ''
            if build.returncode and 'Could not find a package configuration file provided by "SDL2"' in configure:
                _built='needs SDL2 development files (CMake found no SDL2Config.cmake)'
            elif build.returncode:
                raise AssertionError('tools/build-desktop.sh preview failed:\n'+build.stdout[-4000:]+build.stderr[-4000:])
            else:
                _built=''
                print(build.stdout.strip().splitlines()[-1])
    return _built or None

def run_preview(root, steps, timeout=180, metrics=None, on_line=None, env_extra=None):
    """Runs the scripted app on a disposable root; returns (returncode, combined output)."""
    script=Path(root)/'steps.txt';script.write_text('\n'.join(steps)+'\n',encoding='utf-8')
    env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','NP_ROOT':str(root),
         'NP_ASSETS':str(ROOT/'assets'),'NP_SCRIPT':str(script)}
    if metrics:env['NP_METRICS']=str(metrics)
    if env_extra:env.update(env_extra)
    proc=subprocess.Popen([str(PREVIEW)],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace')
    timer=threading.Timer(timeout,proc.kill);timer.start();lines=[]
    try:
        for line in proc.stdout:
            lines.append(line)
            if on_line:on_line(line)
        proc.wait()
    finally:
        timer.cancel();proc.stdout.close()
    return proc.returncode,''.join(lines)

class PreviewTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        reason=build_preview()
        if reason:raise unittest.SkipTest(reason)

    def test_library_profile_search_filters(self):
        with tempfile.TemporaryDirectory(prefix='eclipse-ui-v4-') as td:
            root=Path(td);games=root/'games';games.mkdir()
            for name in ('Alpha','Beta'):
                game=games/name;(game/'game').mkdir(parents=True)
                (game/'game/script.rpy').write_text('label start:\n    return\n')
            # the shelf's flow (Right moves to the next game): Story's own is test_story_actions_reach_shared_state
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\nlibrary_view = 1\n')
            steps=['wait 100','assert screen library','assert view shelf','assert filtered 2','press menu','wait 5','assert screen details',
                   'press down','press cross','assert favorite 1','press down','press cross','wait 5','assert screen profile']
            steps+=['press down']*6
            steps+=['press left','assert profile_volume 9','assert global_volume 10','press circle','press circle',
                    'press right','press menu','press down','press down','press cross','assert profile_volume 10',
                    'press circle','press circle','press l2','press cross','press circle','assert query <empty>',
                    'press l2','press cross','hold start 10','assert screen library','assert query q','assert filtered 0',
                    'press l2','press r1','press start','assert query <empty>','assert filtered 2',
                    'press r2','press cross','assert filtered 1','press circle','assert screen library',
                    'press menu','assert favorite 1','press down','press down','press cross']
            steps+=['press down']*6
            steps+=['press square','assert profile_volume 10','assert global_volume 10','press circle','press circle','quit']
            script=root/'steps.txt';script.write_text('\n'.join(steps)+'\n')
            env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','NP_ROOT':td,'NP_ASSETS':str(ROOT/'assets'),'NP_SCRIPT':str(script)}
            run=subprocess.run([str(PREVIEW)],env=env,text=True,capture_output=True,timeout=120)
            output=run.stdout+run.stderr
            self.assertEqual(run.returncode,0,output)
            self.assertNotIn('ASSERT FAIL',output)
            self.assertEqual(output.count('ASSERT PASS'),sum(x.startswith('assert ') for x in steps),output)
            print('Real preview assertions passed: favorites, filtered selection, search cancel/clear/empty, profile inheritance/reset/no leakage')

    @staticmethod
    def pinned_testgame():
        out=subprocess.run(['bash',str(ROOT/'tools/fetch-testgame.sh')],check=True,capture_output=True,text=True)
        return Path(out.stdout.strip().splitlines()[-1])

    def test_easyrpg_profile_and_backup_actions(self):
        """The same controller routes in both library views: Story draws details and backups differently and maps
        Left/Right onto their rows, but every confirmation, release gate and recovery copy is the panels' own."""
        for view in ('shelf','story'):
            with self.subTest(view=view):
                self.easyrpg_profile_and_backup_actions(view)

    def easyrpg_profile_and_backup_actions(self, view):
        testgame=self.pinned_testgame()
        with tempfile.TemporaryDirectory(prefix='eclipse-native-ui-v4-') as td:
            root=Path(td);game=root/'games/Native';game.parent.mkdir()
            shutil.copytree(testgame/'TestGame-2000',game)
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\nlibrary_view = %d\n'%(view=='shelf'))
            env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','NP_ROOT':td,'NP_ASSETS':str(ROOT/'assets')}
            def run(steps):
                script=root/'steps.txt';script.write_text('\n'.join(steps)+'\n');env['NP_SCRIPT']=str(script)
                p=subprocess.run([str(PREVIEW)],env=env,text=True,capture_output=True,timeout=120)
                output=p.stdout+p.stderr;self.assertEqual(p.returncode,0,output);self.assertNotIn('ASSERT FAIL',output)
                self.assertEqual(output.count('ASSERT PASS'),sum(s.startswith('assert ') for s in steps),output)
            run(['wait 100','press menu','press down','press cross','assert favorite 1','press circle',
                 'press l2','press cross','press start','assert query q','assert filtered 0',
                 'press l2','press r1','press start','assert query <empty>','assert filtered 1',
                 'press r2','press cross','assert filtered 1','press circle',
                 'press menu','press down','press down','press cross']+['press down']*6+
                ['press left','assert profile_volume 9','press circle','press circle','press cross','wait 180',
                 'assert runtime running','assert effective_volume 9','assert global_volume 10','press menu']+['press down']*5+
                ['press cross','wait 180','assert runtime idle','assert screen library','assert effective_volume 10',
                 'press menu']+['press down']*3+['press cross','wait 60','press cross','wait 80','assert save_state done','quit'])
            marker=b'UI recovery copy must retain this native slot\n';slot=game/'Save01.lsd';slot.write_bytes(marker)
            run(['wait 100','press menu']+['press down']*3+['press cross','wait 60','press down','hold cross 180','wait 60',
                'assert confirmation 1','press circle','wait 60','assert confirmation 0','quit'])
            self.assertEqual(slot.read_bytes(),marker,'Circle must preserve native saves')
            run(['wait 100','press menu']+['press down']*3+['press cross','wait 60','press down','press cross','wait 60',
                'assert confirmation 1','press cross','wait 120','assert save_state done','quit'])
            self.assertFalse(slot.exists(),'confirmed restore of the empty snapshot removes the later native slot')
            self.assertTrue(any(p.is_file() and p.read_bytes()==marker for p in (root/'backups').rglob('*') if p.name!='manifest'), 'pre-restore recovery copy retained')
            snapshots_before=sum(p.name=='manifest' for p in (root/'backups').rglob('*'))
            run(['wait 100','press menu']+['press down']*3+['press cross','wait 60','press down','press down','hold square 180','wait 60',
                'assert confirmation 1','press circle','wait 60','assert confirmation 0','press square','wait 60','assert confirmation 1',
                'press cross','wait 100','assert save_state done','quit'])
            snapshots_after=sum(p.name=='manifest' for p in (root/'backups').rglob('*'))
            self.assertEqual(snapshots_after,snapshots_before-1,'confirmed delete removes exactly one snapshot')
            print('Real EasyRPG UI (%s view): held confirmation requires release/new press, effective/global separation, clean-stop return, manual/create, restore/cancel/recovery, delete/cancel/confirm'%view)

    def test_thousand_game_browsing_responsiveness(self):
        """1000 synthetic Ren'Py folders (one Unicode title, 40 with covers): browsing while rescans run, sort,
        search and its cancel, and a folder vanishing between scans. Selection follows the game's stable ID. Every
        scripted input frame must reach presentation within 100 ms of that frame's start on the host's real
        monotonic clock (NP_METRICS). Prints the measured summary; offscreen software presentation, not console.
        Shelf view browses with Right/Left; Story view with Down/Up through its list window (only visible rows are
        drawn), the same selection, sort, search and rescan underneath."""
        for view in ('shelf','story'):
            with self.subTest(view=view):
                self.thousand_game_browsing(view)

    def thousand_game_browsing(self, view):
        nxt,prev=('right','left') if view=='shelf' else ('down','up')
        with tempfile.TemporaryDirectory(prefix='eclipse-1000-') as td:
            root=Path(td);games=root/'games';games.mkdir()
            cover=ROOT/'assets/ui/eclipse/placeholder-cover.png'
            unicode_title='Game 0003 月の旅 日本語'
            for i in range(1000):
                game=games/(unicode_title if i==3 else 'Game %04d'%i)/'game';game.mkdir(parents=True)
                (game/'script.rpy').write_text('label start:\n    return\n')
                if i<40:
                    (game/'gui').mkdir();shutil.copyfile(cover,game/'gui/main_menu.png')
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\nlibrary_view = %d\n'%(view=='shelf'))
            steps=['wait 120','assert view '+view,'assert games 1000','assert filtered 1000','assert selected Game 0000']
            for _ in range(3):  # rescan, then browse both ways while it runs and publishes
                steps+=['press square','hold %s 120'%nxt,'hold %s 240'%prev,'assert selected Game 0000']
            steps+=['press '+nxt]*3+['assert selected '+unicode_title]
            steps+=['press '+nxt]*2+['assert selected Game 0005']
            steps+=['press triangle','wait 10','assert selected Game 0005','press triangle','wait 10','assert selected Game 0005']
            steps+=['press l2','press cross','press start','assert query q','assert filtered 0',
                    'press l2','press r1','press start','assert query <empty>','assert filtered 1000','assert selected Game 0005']
            # The harness removes a folder on this marker; the next rescan must publish 999 and keep the selection.
            steps+=['assert still 0','wait 60','press square','wait 120','assert games 999','assert filtered 999',
                    'assert selected Game 0005','hold %s 120'%nxt,'hold %s 240'%prev,'assert selected Game 0000','quit']
            vanished=[]
            def on_line(line):
                if 'preview ASSERT PASS still' in line and not vanished:
                    (games/'Game 0999').rename(root/'vanished');vanished.append(True)
            metrics=root/'metrics.csv'
            code,output=run_preview(root,steps,metrics=metrics,on_line=on_line)
            self.assertEqual(code,0,output[-6000:]);self.assertNotIn('ASSERT FAIL',output)
            self.assertEqual(output.count('ASSERT PASS'),sum(s.startswith('assert ') for s in steps),output[-6000:])
            self.assertTrue(vanished)
            with metrics.open() as stream:rows=list(csv.DictReader(stream))
            events=[r for r in rows if int(r['pressed'])]
            inputs=[float(r['input_ms']) for r in events];frames=[float(r['frame_ms']) for r in rows]
            self.assertTrue(events and max(int(r['count']) for r in rows)==1000 and int(rows[-1]['count'])==999)
            slow=[(r['frame'],r['input_ms']) for r in events if float(r['input_ms'])>=100]
            self.assertFalse(slow,'input frames at or over 100 ms (frame, ms): %r'%slow[:20])
            ordered=sorted(inputs)
            summary=dict(frames=len(rows),input_frames=len(events),
                         input_ms_max=round(max(inputs),3),input_ms_p95=round(ordered[int(0.95*(len(ordered)-1))],3),
                         input_ms_mean=round(statistics.mean(inputs),3),frame_ms_max=round(max(frames),3),
                         frame_ms_mean_after_120=round(statistics.mean(frames[120:]),3),
                         publish_ms_max=round(max(float(r['publish_ms']) for r in rows),3),
                         cover_decoded_bytes_max=max(int(r['decoded_bytes']) for r in rows),
                         cover_texture_bytes_max=max(int(r['texture_bytes']) for r in rows),
                         cover_upload_bytes_max=max(int(r['upload_bytes']) for r in rows))
            print('\n1000-game browsing, %s view (host clock, offscreen software preview): '%view+json.dumps(summary))

    def test_legacy_settings_and_native_save_through_upgrade(self):
        """A 0.2.12 data root (that release's settings.ini format, its last_game index, an existing native RPG Maker
        slot) on this build: settings and the selected game carry over; three launch/stop cycles keep the slot's bytes
        with a verified automatic backup of them; a later change is undone by a confirmed restore through the UI,
        which keeps a recovery copy of the changed bytes."""
        testgame=self.pinned_testgame()
        with tempfile.TemporaryDirectory(prefix='eclipse-upgrade-') as td:
            root=Path(td);games=root/'games';(games/'Alpha/game').mkdir(parents=True)
            (games/'Alpha/game/script.rpy').write_text('label start:\n    return\n')
            shutil.copytree(testgame/'TestGame-2000',games/'Native 2000')
            game=games/'Native 2003';shutil.copytree(testgame/'TestGame-2003',game)
            marker=b'0.2.12 native slot kept through the Eclipse upgrade\n';slot=game/'Save01.lsd';slot.write_bytes(marker)
            (root/'settings.ini').write_text(
                '# Dulcamara 0.2.12 settings\nversion = 0.2.12\n'
                'integer_scale = 1\nsmooth = 0\nshader = 0\naspect = 0\nupscale = 0\nupscale_filter = 0\n'
                'ff_speed = 2\nff_toggle = 0\nstick_dpad = 1\nshow_fps = 0\nrenpy_skip_unseen = 0\n'
                'ui_sound = 1\nui_volume = 10\ngame_volume = 7\nupdate_check = 0\nsort_mode = 0\nlast_game = 1\n'
                'community_seen = 1\nupdate_test_builds = 0\n')
            # 0.2.12 sorted section first, then title: RPG Maker 2000 (0), RPG Maker 2003 (1), Ren'Py Alpha (2).
            title='RPG Maker 2003 Test suite'
            # a 0.2.12 settings file has no view: Story, the default; Cross plays, the touchpad opens details there too
            steps=['wait 100','assert view story','assert games 3','assert selected '+title,'assert global_volume 7',
                   'assert effective_volume 7']
            for _ in range(3):
                steps+=['press cross','wait 180','assert runtime running','assert effective_volume 7','press menu']
                steps+=['press down']*5+['press cross','wait 180','assert runtime idle','assert screen library']
            steps+=['quit']
            code,output=run_preview(root,steps)
            self.assertEqual(code,0,output[-6000:]);self.assertNotIn('ASSERT FAIL',output)
            self.assertEqual(output.count('ASSERT PASS'),sum(s.startswith('assert ') for s in steps),output[-6000:])
            self.assertEqual(slot.read_bytes(),marker,'launch/stop cycles must not alter the existing native slot')
            blobs=[p for p in (root/'backups').rglob('*') if p.is_file() and p.name!='manifest']
            self.assertTrue(any(p.read_bytes()==marker for p in blobs),'first automatic backup holds the legacy slot')
            settings=(root/'settings.ini').read_text()
            self.assertIn('game_volume = 7',settings);self.assertIn('selected_game_id = ',settings)
            leftovers=[p.name for p in (root/'backups').rglob('*') if p.name.startswith(('.pending-','.deleting-','.cleanup-'))]
            self.assertEqual(leftovers,[])
            changed=b'changed after the upgrade\n';slot.write_bytes(changed)
            steps=['wait 100','assert selected '+title,'press menu']+['press down']*3+['press cross','wait 60',
                   'press down','press cross','wait 60','assert confirmation 1','press cross','wait 120',
                   'assert save_state done','quit']
            code,output=run_preview(root,steps)
            self.assertEqual(code,0,output[-6000:]);self.assertNotIn('ASSERT FAIL',output)
            self.assertEqual(output.count('ASSERT PASS'),sum(s.startswith('assert ') for s in steps),output[-6000:])
            self.assertEqual(slot.read_bytes(),marker,'confirmed restore brings back the backed-up legacy slot')
            self.assertTrue(any(p.is_file() and p.read_bytes()==changed for p in (root/'backups').rglob('*')),
                            'pre-restore recovery copy keeps the changed bytes')
            print('Legacy 0.2.12 root: settings/selection kept, 3 launch/stop cycles, slot intact, automatic backup, UI restore with recovery copy')

    # ---------------------------------------------------------------- Story mode (src/ui/story.c)

    @staticmethod
    def renpy_games(games, names, cover_every=0):
        cover=ROOT/'assets/ui/eclipse/placeholder-cover.png'
        for i,name in enumerate(names):
            game=games/name/'game';game.mkdir(parents=True)
            (game/'script.rpy').write_text('label start:\n    return\n')
            if cover_every and i%cover_every==0:
                (game/'gui').mkdir();shutil.copyfile(cover,game/'gui/main_menu.png')

    def assert_run(self, root, steps, **kw):
        code,output=run_preview(root,steps,**kw)
        self.assertEqual(code,0,output[-6000:]);self.assertNotIn('ASSERT FAIL',output)
        self.assertEqual(output.count('ASSERT PASS'),sum(x.startswith('assert ') for x in steps),output[-6000:])
        return output

    def test_install_bundled_shelf_from_library_settings(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            steps = ['wait 100', 'press start', 'assert screen settings'] + ['press r1']*4 + ['press down']*7
            self.assert_run(root, steps + ['press cross', 'wait 10', 'press circle', 'wait 100',
                                           'assert filtered 1', 'assert selected Dulcamara itch.io Shelf', 'quit'])
            self.assertTrue((root/'games/Dulcamara itch.io Shelf/game/shelf.rpy').is_file())

    def test_story_view_setting_switch_and_persist(self):
        """Story is the default; Settings > Library > View switches to the shelf, which then works as before (Right
        moves to the next game), and the choice persists through settings.ini; switching back restores Story."""
        with tempfile.TemporaryDirectory(prefix='story-view-') as td:
            root=Path(td);self.renpy_games(root/'games',['Alpha','Beta','Gamma'])
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            to_view=['press start','wait 5','assert screen settings']+['press r1']*4+['press down']*6
            steps=['wait 100','assert view story','assert selected Alpha','press down','assert selected Beta',
                   'press up','assert selected Alpha']
            steps+=to_view+['press right','assert view shelf','press circle','wait 5','assert screen library',
                            'press right','assert selected Beta','quit']
            self.assert_run(root,steps)
            self.assertIn('library_view = 1',(root/'settings.ini').read_text())
            steps=['wait 100','assert view shelf','assert selected Beta','press right','assert selected Gamma',
                   'press down','assert selected Gamma']  # on the shelf Down changes section, not the game
            steps+=to_view+['press left','assert view story','press circle','wait 5','press up',
                            'assert selected Beta','quit']
            self.assert_run(root,steps)
            self.assertIn('library_view = 0',(root/'settings.ini').read_text())
            print('Story/Shelf view: default Story, switched in Settings > Library, persisted both ways, each browses its own way')

    def test_story_actions_reach_shared_state(self):
        """Every Story action lands on the shelf's and the panels' own state: Favorite on the metadata the details
        panel writes, Details/Backups on their screens, the touchpad, Triangle sort, Square rescan, L2 search,
        R2 filters, Options settings. Presses act on the frame they arrive while the line is still typing, and
        holding Down through the list queues nothing. The "N not shown" row explains and returns. Square's rescan
        must find a game added after the first scan."""
        with tempfile.TemporaryDirectory(prefix='story-actions-') as td:
            root=Path(td);games=root/'games'
            self.renpy_games(games,['Alpha','Bravo','Charlie','Delta','Echo'],cover_every=2)
            mv=games/'Other Engine'/'www';mv.mkdir(parents=True)  # an RPG Maker MV game: not shown, with its reason
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            # the harness adds Foxtrot on the "assert still 0" line: only a rescan by Square can find it
            steps=['wait 100','assert view story','assert selected Alpha','assert games 5','assert still 0','wait 60',
                   'press square','wait 120','assert games 6']
            # Favorite: Right x3, Cross; the line has only begun typing for Bravo when these land
            steps+=['press down','press right','press right','press right','press cross','assert selected Bravo',
                    'assert favorite 1','press cross','assert favorite 0','press cross','assert favorite 1']
            # Details and Backups as choices, the touchpad as on the shelf
            steps+=['press left','press left','press cross','assert screen details','press circle',
                    'assert screen library','press right','press cross','wait 30','assert screen backups',
                    'press circle','wait 5','assert screen library','assert selected Bravo','press menu',
                    'assert screen details','press circle','assert screen library']
            # Triangle sorts (selection kept), L2 search, R2 filters, Options settings
            steps+=['assert sort 0','press triangle','assert sort 1','assert selected Bravo','press triangle',
                    'assert sort 0','press l2','assert screen details','press cross','press start','assert query q',
                    'assert filtered 0','press l2','press r1','press start','assert query <empty>','assert filtered 6',
                    'press r2','press cross','assert filtered 1','press cross','assert filtered 6','press circle',
                    'assert screen library','press start','assert screen settings','press circle','wait 5',
                    'assert screen library']
            # holding Down to the end: nothing queued behind it; the last row is "1 not shown"
            steps+=['hold down 90','press cross','assert screen library','assert selected Foxtrot',
                    'press menu','assert screen details','press circle','hold down 30','press menu',
                    'assert screen library','press cross','press right','press right','press right','press cross',
                    'assert selected Foxtrot','assert favorite 1','press circle','assert favorite 1','quit']
            added=[]
            def on_line(line):
                if 'preview ASSERT PASS still' in line and not added:
                    self.renpy_games(games,['Foxtrot']);added.append(True)
            output=self.assert_run(root,steps,on_line=on_line)
            self.assertTrue(added)
            self.assertIn('library: not shown',output)
            print('Story actions: favorite/details/backups/touchpad/sort/search/filters/settings on shared state; presses act while typing; not-shown row')

    def test_story_titles_cover_and_empty_library(self):
        """Long and Japanese titles, a game without a cover and an empty library draw without failing, in motion and
        with reduced motion (whole lines); the empty library still answers its keys."""
        with tempfile.TemporaryDirectory(prefix='story-titles-') as td:
            root=Path(td);games=root/'games'
            long_title='The Extraordinarily Long Chronicle of the Lighthouse Keeper Who Counted Every Wave'
            self.renpy_games(games,['月の旅 日本語の物語',long_title,'No Cover'],cover_every=0)
            for still in (0,1):
                (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\nstill_background = %d\n'%still)
                steps=['wait 100','assert still %d'%still,'assert selected No Cover','press down',
                       'assert selected '+long_title,'press menu','assert screen details','press circle','press down',
                       'assert selected 月の旅 日本語の物語','press menu','assert screen details','press right',
                       'press right','press right','press cross','wait 30','assert screen backups','press circle',
                       'press circle','assert screen library','quit']
                self.assert_run(root,steps)
        with tempfile.TemporaryDirectory(prefix='story-empty-') as td:
            root=Path(td);(root/'games').mkdir()
            (root/'settings.ini').write_text('update_check = 0\n')
            steps=['wait 100','assert view story','assert games 0','press down','press up','press right','press cross',
                   'press menu','press triangle','assert screen library','press l2','assert screen details',
                   'press circle','assert screen library','press square','wait 30','assert games 0','quit']
            self.assert_run(root,steps)
            self.assertIn('community_seen = 1',(root/'settings.ini').read_text(),'the empty library shows nightly once, as the shelf does')
        print('Story text: long, Japanese and coverless titles, still and moving; empty library keys')

    def test_story_confirmation_skip_press_does_not_confirm(self):
        """The visual-novel habit of pressing Cross as a question appears must not restore or delete: in Story view
        the restore and delete questions appear whole, and a Cross in their first frames does nothing; a second,
        separate press confirms. (On ordinary screens a press still finishes the line and acts on the same frame:
        test_story_actions_reach_shared_state.)"""
        testgame=self.pinned_testgame()
        with tempfile.TemporaryDirectory(prefix='story-skip-') as td:
            root=Path(td);game=root/'games/Native';game.parent.mkdir()
            shutil.copytree(testgame/'TestGame-2000',game)
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            slot=game/'Save01.lsd';slot.write_bytes(b'kept in the backup\n')
            open_backups=['wait 100','assert view story','press right','press right','press cross','wait 60',
                          'assert screen backups']
            # play and stop once (the native save locations become known), then a manual backup of the slot
            play=['wait 100','press cross','wait 180','assert runtime running','press menu']+['press down']*5+[
                  'press cross','wait 180','assert runtime idle','assert screen library','press circle']
            self.assert_run(root,play+['press right','press right','press cross','wait 60','assert screen backups',
                                       'press cross','wait 90','assert save_state done','quit'])
            snapshots=lambda:sum(p.name=='manifest' for p in (root/'backups').rglob('*'))
            self.assertGreaterEqual(snapshots(),1)
            slot.write_bytes(b'changed after the backup\n')
            # Cross on the slot, the question appears, Cross again at once: nothing; a later, separate Cross restores
            early=['press right','press cross','wait 10','assert confirmation 1','press cross','wait 40',
                   'assert confirmation 1']
            self.assert_run(root,open_backups+early+['quit'])
            self.assertEqual(slot.read_bytes(),b'changed after the backup\n','the skip press must not restore')
            self.assert_run(root,open_backups+early+['press cross','wait 120','assert confirmation 0',
                                                     'assert save_state done','quit'])
            self.assertEqual(slot.read_bytes(),b'kept in the backup\n','the second press restores')
            # Delete the same way (Square on the slot; slot 1 is now the retained recovery copy, so slot 2): the skip
            # press keeps the snapshot, the second press deletes it
            before=snapshots()
            early=['press right','press right','press square','wait 10','assert confirmation 1','press cross','wait 40',
                   'assert confirmation 1']
            self.assert_run(root,open_backups+early+['quit'])
            self.assertEqual(snapshots(),before,'the skip press must not delete')
            self.assert_run(root,open_backups+early+['press cross','wait 120','assert confirmation 0',
                                                     'assert save_state done','quit'])
            self.assertEqual(snapshots(),before-1,'the second press deletes exactly one snapshot')
            print('Story confirmation: a Cross in the question\'s first frames neither restores nor deletes; a second press does')

    def test_story_status_backup_count_never_stale(self):
        """The status screen's backup count is the last list's only while no save job has run since: after a play
        session (its automatic backups) it says to open Backups, and Backups counts them again."""
        testgame=self.pinned_testgame()
        with tempfile.TemporaryDirectory(prefix='story-count-') as td:
            root=Path(td);game=root/'games/Native';game.parent.mkdir()
            shutil.copytree(testgame/'TestGame-2000',game)
            (game/'Save01.lsd').write_bytes(b'counted\n')
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            backups=['press right','press right','press cross','wait 60','assert screen backups','press circle',
                     'wait 10','assert screen library','press circle']
            status=['press menu','wait 10','assert screen details','probe story_backup_count','press circle','wait 5']
            play=['press cross','wait 180','assert runtime running','press menu']+['press down']*5+[
                  'press cross','wait 180','assert runtime idle','assert screen library']
            steps=['wait 100','assert view story']+backups+status+play+status+backups+status+['quit']
            output=self.assert_run(root,steps)
            shown=[l.split('story_backup_count [',1)[1].rsplit(']',1)[0] for l in output.splitlines() if 'PROBE story_backup_count' in l]
            self.assertEqual(len(shown),3,output[-3000:])
            self.assertEqual(shown[0],'0 automatic of 5, 0 in all')
            self.assertNotIn('in all',shown[1],'a count from before the play session is stale: %r'%shown)
            self.assertRegex(shown[2],r'^[1-5] automatic of 5, [1-9][0-9]* in all$')
            print('Story status backup count: %r'%shown)

    def test_story_working_line_types_once(self):
        """While a restore works, the text box's line is said once and types on: the progress figures are drawn in
        the window, not in the typed line, so a progress update never restarts it."""
        import re
        testgame=self.pinned_testgame()
        with tempfile.TemporaryDirectory(prefix='story-working-') as td:
            root=Path(td);game=root/'games/Native';game.parent.mkdir()
            shutil.copytree(testgame/'TestGame-2000',game)
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            # play and stop once (the native save locations become known), then fifteen large slots: the restore
            # reports its progress file by file
            self.assert_run(root,['wait 100','press cross','wait 180','assert runtime running','press menu']+
                            ['press down']*5+['press cross','wait 180','assert runtime idle','quit'])
            for k in range(1,16):
                (game/('Save%02d.lsd'%k)).write_bytes(os.urandom(1<<20)*4)
            steps=['wait 100','press right','press right','press cross','wait 60','assert screen backups','press cross',
                        'wait 600','assert save_state done','press right','press cross','wait 60',
                        'assert confirmation 1','press cross']
            steps+=['probe story_text','wait 2']*40+['wait 240','assert save_state done','quit']
            output=self.assert_run(root,steps)
            shown=[l.split('story_text [',1)[1].rsplit(']',1)[0] for l in output.splitlines() if 'PROBE story_text' in l]
            working=[t for t in shown if t.startswith('Working')]
            self.assertGreaterEqual(len(working),3,'the restore must still be working at three probes: %r'%shown)
            self.assertFalse([t for t in working if re.search(r'\d+ / \d+',t)],'progress figures in the typed line: %r'%working)
            self.assertTrue(all(b.startswith(a) for a,b in zip(working,working[1:])),'the line restarted: %r'%working)
            print('Story working line: %d probes while working, typed on without restarting'%len(working))

    def test_story_stick_diagonal_moves_one_axis(self):
        """The stick pushed askew moves along its stronger axis only: the game or the choice, never both in a frame
        (the shelf's guard for the stick pushed along its row askew)."""
        with tempfile.TemporaryDirectory(prefix='story-stick-') as td:
            root=Path(td);self.renpy_games(root/'games',['Alpha','Bravo','Charlie'])
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            # on Backups (choice 2): down and right at once, down stronger: the next game, back to Play, no Right;
            # then Right is Details
            steps=['wait 100','assert selected Alpha','press right','press right','stick 23000 30000',
                   'assert selected Bravo','press right','press cross','assert screen details','press circle',
                   'assert screen library']
            # down and right at once, right stronger: Details, the game unchanged; Cross opens Details
            steps+=['press left','stick 30000 23000','assert selected Bravo','press cross','assert screen details',
                    'press circle','quit']
            self.assert_run(root,steps)
            print('Story stick: a diagonal moves the game or the choice by its stronger axis, never both')

    def test_story_titles_are_plain_text(self):
        """Titles and folder names are data, never markup: braces, a doubled brace, a {b} or a {Cross} in a title and a
        lone backslash are drawn as they are, and the text box shows its whole line once typed (an unmatched brace
        used to cut it there for good)."""
        titles=['Test {beta','Close } brace','Double {{ open','Bold {b} tag','Back\\slash','Glyph {Cross} word']
        with tempfile.TemporaryDirectory(prefix='story-plain-') as td:
            root=Path(td);self.renpy_games(root/'games',titles)
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            steps=['wait 100','assert games 6']
            for _ in titles:
                steps+=['wait 120','probe story_text','press down']
            steps+=['assert selected Test {beta','press menu','wait 120','probe story_text','press circle','quit']
            output=self.assert_run(root,steps)
            shown=[l.split('story_text [',1)[1].rsplit(']',1)[0] for l in output.splitlines() if 'PROBE story_text' in l]
            self.assertEqual(len(shown),len(titles)+1,output[-3000:])
            for title in titles:
                home='"%s." You have not started this one yet. What will you do?'%title
                self.assertIn(home,shown,'the whole home line for %r'%title)
            self.assertTrue(shown[-1].startswith('Test {beta. Ren') and shown[-1].endswith('in Preferences.'),shown[-1])
            print('Story plain titles: %d home lines and the status line shown whole'%len(titles))

    @staticmethod
    def outline_probes(output):
        return [int(l.split('live=')[1]) for l in output.splitlines() if 'preview PROBE outline_textures' in l]

    def test_story_outline_textures_stop_growing(self):
        """Sitting on Story's backups (with slots), home and status screens for hundreds of frames creates no new
        rounded-outline textures once each screen has been drawn: the ring cache is bounded and nothing is made
        uncached per frame."""
        testgame=self.pinned_testgame()
        with tempfile.TemporaryDirectory(prefix='story-rings-') as td:
            root=Path(td);games=root/'games';games.mkdir()
            shutil.copytree(testgame/'TestGame-2000',games/'Native')
            self.renpy_games(games,['Alpha','Bravo'],cover_every=1)
            (games/'Native/Save01.lsd').write_bytes(b'outline texture fixture\n')
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            steps=['wait 100','assert view story','assert selected RPG Maker 2000 Test suite','press right','press right','press cross','wait 60','assert screen backups',
                   'press cross','wait 90','press cross','wait 90','press right','wait 30']
            sit=lambda:['probe outline_textures','wait 300','probe outline_textures']
            steps+=sit()+['press circle','wait 30','assert screen library','wait 30']+sit()
            steps+=['press menu','wait 30','assert screen details']+sit()+['press circle','quit']
            output=self.assert_run(root,steps)
            live=self.outline_probes(output)
            self.assertEqual(len(live),6,output[-3000:])
            pairs=[(live[i],live[i+1]) for i in range(0,6,2)]
            self.assertTrue(all(a==b for a,b in pairs),'outline textures grew while a screen sat still (backups, home, '
                            'status: before, after 300 frames): %r'%pairs)
            print('Story outline textures over 300 still frames (backups, home, status): %r'%pairs)

    @staticmethod
    def bmp_pixels(path):
        data=Path(path).read_bytes();offset=int.from_bytes(data[10:14],'little')
        w=int.from_bytes(data[18:22],'little',signed=True);h=int.from_bytes(data[22:26],'little',signed=True)
        def at(x,y):
            row=y if h<0 else abs(h)-1-y;i=offset+(row*w+x)*4
            return data[i+2],data[i+1],data[i]
        return at

    def test_story_watermark_over_story_screens(self):
        """A private-test build keeps its mark over Story's home, status and backups screens (drawn last, in the band
        Story leaves free under the header); an unmarked build has nothing there. Rebuilds the preview marked, then
        unmarked again."""
        def bright(shot):
            at=self.bmp_pixels(shot)
            return sum(1 for y in range(118,145) for x in range(64,300) if min(at(x,y))>190)
        env={**os.environ}
        with tempfile.TemporaryDirectory(prefix='story-mark-') as td:
            root=Path(td);self.renpy_games(root/'games',['Alpha','Beta'])
            (root/'settings.ini').write_text('update_check = 0\ncommunity_seen = 1\n')
            steps=['wait 100','shot home','press menu','wait 20','shot status','press right','press right','press right',
                   'press cross','wait 30','shot backups','press circle','press circle','quit']
            counts={}
            try:
                for marked in (True,False):
                    build_env={**env,'NP_PRIVATE_TEST':'1','NP_TEST_RECIPIENT':'Tester','NP_TEST_BUILD':'story-check'} if marked else env
                    if not marked:
                        for k in ('NP_PRIVATE_TEST','NP_TEST_RECIPIENT','NP_TEST_BUILD'):build_env.pop(k,None)
                    b=subprocess.run(['bash',str(ROOT/'tools/build-desktop.sh'),'preview'],env=build_env,capture_output=True,text=True)
                    self.assertEqual(b.returncode,0,b.stdout[-3000:]+b.stderr[-3000:])
                    shots=root/('marked' if marked else 'plain');shots.mkdir()
                    code,output=run_preview(root,steps,env_extra={'NP_SHOTS':str(shots)})
                    self.assertEqual(code,0,output[-4000:])
                    counts[marked]=[bright(shots/(n+'.bmp')) for n in ('home','status','backups')]
            finally:
                plain={k:v for k,v in env.items() if k not in ('NP_PRIVATE_TEST','NP_TEST_RECIPIENT','NP_TEST_BUILD')}
                subprocess.run(['bash',str(ROOT/'tools/build-desktop.sh'),'preview'],env=plain,capture_output=True)
            self.assertTrue(all(c>40 for c in counts[True]),counts)
            self.assertTrue(all(c<5 for c in counts[False]),counts)
            print('Story watermark: marked build shows the mark on home/status/backups %r; unmarked %r'%(counts[True],counts[False]))
