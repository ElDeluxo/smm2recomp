# SMM2 recomp mods

Mods change the recompiled game at run time. Their code is ours (`games/smm2/mods/`, `runtime/net/`); they ship no
game code or data. Anything they need from a game is made on the player's machine from the player's own dumps, into
`private/`, which git ignores.

## Online against our server, and the 100 Mario Challenge

One mod does two things:

- **Course World against our own server** (`mmk server`, core/crates/server) or a **local library** run the same way,
  never Nintendo's servers. SMM2's own **Endless Challenge** works as SMM2 made it, under the server's `smm2` rules
  (5 lives, endless).
- **SMM1's 100 Mario Challenge**, SMM1's rules: **100 lives**, lives cap at **100**, a run is won after
  **8 / 16 / 16 / 6** clears (Easy / Normal / Expert / Super Expert), **skips are free** (also during the miss
  animation), a miss throws away the coins and score of the failed attempt, an award worth several lives banks one
  extra life (at most 3 per course, paid on the clear), Expert opens after a won Normal run and Super Expert after a
  won Expert run. SMM1's screens, map and ending are drawn host-side from the player's SMM1 dump (another part of the
  mod: `games/smm2/mods/smm1/`, the SMM1 screens; needs the SMM1 dump: `RECOMP_SMM1_DUMP`); the courses play in SMM2's own course play. The run side is
  described below ("The bridge").

### Turning it on

Environment variables, or `smm2mods.ini` (`key=value` lines) in the data folder (`private/smm2/` in a source build):

| Variable | ini key | Meaning |
|---|---|---|
| `RECOMP_MOD_100MARIO=1` | `hundred=1` | the mod is on (Course World online against our server) |
| `RECOMP_MOD_SERVER=<url>` | `server=` | the course server, plain `http://` (default `http://127.0.0.1:8080`) |
| `RECOMP_MOD_TOKEN=<token>` | `token=` | your account's token on that server (see below); keep it private |
| `RECOMP_MOD_RULESET=hundred\|smm2` | `ruleset=` | `hundred` (default): 100 Mario is offered: Course World's Endless Challenge button opens SMM1's 100 Mario Challenge, and B on its difficulty select leads on to SMM2's own Endless Challenge (both in one build; SMM2's rules there until the player leaves that menu); `smm2`: the button opens SMM2's own Endless Challenge only |
| `RECOMP_MOD_COURSES=server\|tgrcode` | `courses=` | `server` (default): the server above; `tgrcode` (iOS default, with the mod on): a course server built into the app on a loopback port, with a local account, fed from TGRCode's archive of SMM2 courses (needs the course key table: the mod writes it once from your game into `mods-generated/course_table.bin`, private); falls back to `server=` when the build has no built-in server. Also in the Mods tab ("Courses: TGRCode"), from the next start |
| `RECOMP_MOD_TIMEOUT_MS=<n>` | `timeout_ms=` | per request (default 8000) |
| `RECOMP_MOD_LOG=1` | `log=1` | log every request and answer |

    RECOMP_MOD_100MARIO=1 RECOMP_MOD_SERVER=http://127.0.0.1:8640 RECOMP_MOD_TOKEN=... make -C recomp run-smm2

Then: Play -> Course World -> Endless Challenge.

### Server

Any `mmk server` works (yours, a friend's, a community one):

    cd core && cargo build --release -p mmk-cli
    target/release/mmk server --dir my-courses --addr 127.0.0.1:8640
    target/release/mmk register MyName --server http://127.0.0.1:8640      # prints your maker ID and token
    target/release/mmk upload --server http://127.0.0.1:8640 courses/*.zlib # raw, zlib, or .bcd with --keys

**Local library** (no network at all): the same server on 127.0.0.1 with a folder of your own courses (your saved
courses, the decrypted course data, or exports). Courses without play statistics fall into the difficulty bands by
the server's rules (unplayed = 50 % fail rate, the band widens when one runs dry).

The server holds the run: lives, clears, the course queue, the history, completed runs (`hundred_<difficulty>`
leaderboards) and SMM2-rule high scores.

### How it works

| Piece | Where | What |
|---|---|---|
| HTTP client | `runtime/net/http.c` | blocking HTTP/1.1, timeouts, sockets (Winsock on Windows); one connection per request |
| JSON | `runtime/net/json.c` | a small reader for the server's answers |
| Pretend online | `runtime/net/online.h`, `services/nifm.c`, `acc.c`, `mii.c` | with the mod on: nifm reports a connection, acc a network service account (a local placeholder token), mii a default Mii; nothing talks to Nintendo |
| Login | `games/smm2/mods/online.c` | the game's network session ("enl") logs in without NEX; Course World's login sequence gets neutral answers and the player's record (name from the server, a default Mii, registered) |
| Endless | `games/smm2/mods/endless.c` | the Endless Challenge's request functions answered from the server (below) |
| Bridge | `games/smm2/mods/bridge.c`, `hundred.h` | 100 Mario: SMM1's screens on top, SMM2's play underneath |
| Rules | `games/smm2/mods/hooks.c` + `patches.py` | host code after single instructions (22 points): lives clamps 99 -> 100, free skip, coin / score rollback on a miss, one banked life per award; only in a 100 Mario run |
| Settings | `games/smm2/mods/settings.c` | the variables above |

The game's Course World requests are each one function (a "wrapper") that the network worker calls through a
pointer; the mod replaces those it needs with `hle_override` and runs the game's own otherwise. Every other Course
World screen runs the game's own code, which finds no connection and shows "A communication error occurred."

| Request (v3.0.3) | Answered from |
|---|---|
| GetEndlessModeStatus | `GET /api/endless`: lives, clears, coins, score per difficulty; start lives 100 (5 under `smm2`) |
| InitEndlessMode (New Game) | `POST /api/endless/<d>/start {"ruleset"}` |
| GetEndlessModePlayInfo | `GET /api/endless/<d>/history` |
| GetEndlessModeRank | `GET /api/rankings/hundred_<d>` (or `endless_<d>`), the `me` row |
| SearchCoursesEndlessMode | `GET /api/endless/<d>/next?count=10` |
| GetObject (course file) | `GET /api/courses/<code>/data`, given the game's header and its own course encryption, so its CRC and validity checks run unchanged |
| StartEndlessModeCourse | first post for a course: the run as it is; a second post for the same course (sent during a miss): `result: death` |
| DominateEndlessModeCourse (clear) | `POST /api/endless/<d>/result {"result":"clear", pending, coins, score}` |
| PassEndlessModeCourse (skip) | `result: skip` |
| Suspend / FinishEndlessMode | `POST /api/endless/<d>/suspend`, `/finish` |
| GetUsers (own) | `GET /api/me` (name, maker ID), best per difficulty (completed 100 Mario runs under `hundred`) |
| GetCourses (by id) | `GET /api/courses/<code>` |
| course list info, an id check | answered empty / passes |
| ReportFromBugDetection (the game found a downloaded course invalid: "The course data is corrupt.") | in a run: `result: broken` (the course leaves the queue); the bridge answers the window, the flow hears `HUNDRED_SKIP`, the next course comes |
| GetApplicationConfig | Endless switched on for the four difficulties |

Data ids and course codes map 1:1 (`games/smm2/mods/course.c`, the same arithmetic as `mmk_formats::course_id`).
A maker's player id is a hash of their maker ID on our server (the player's own too), so the game's "my course"
and "me" checks work.

### Course World against our server (`games/smm2/mods/world.c`)

The rest of Course World goes to our server too. Each request's name is the DataStore (SMM2) method its function
calls (read from the client vtable slot it uses):

| Request (method id) | Function (main+) | Answered from |
|---|---|---|
| course lists: PickUp (84, "Hot"), PointRanking (71, "Popular", by difficulty), Latest (73, "New"), Advanced (72, detailed search: difficulty, style, sort), PostedBy (74, a maker's courses), BestTime / FirstClear (81 / 80) | 0x14d8550 (search type +0x746e8) | `GET /api/courses?sort=popular\|new&style=&offset=&limit=100`, filtered into the game's difficulty bands; a maker's courses from `GET /api/makers/<id>`; a player's Played Courses / Liked Courses (PlayedBy 76, PositiveRatedBy 75) from `GET /api/makers/<id>/played` / `liked`; followed makers and versus: empty |
| GetUserOrCourse (131): the ID search | 0x14dc0d0 | `GET /api/courses/<code>`, else `GET /api/makers/<code>` |
| GetUsers (48): the makers of listed courses, a maker's page | 0x14d02d0, 0x14d7780, 0x14d2f00 | `GET /api/makers/<id>`: name, maker code, likes received and maker points from their courses; the default Mii |
| SearchUsers* (50, 51, 57, 58): leaderboards | 0x14d1820 (kind +0x73a14) | `GET /api/rankings/maker_points` (makers, also the period's board), `endless_<d>` (Endless), `clears` |
| CanPostRatingAndComment (61) | 0x14ebc60 | allowed |
| PostRatingInfo (104): like / boo | 0x14e6b90 | `POST /api/courses/<code>/like` or `/boo` |
| PostPlayResult (96) | 0x14e30e0 | `POST /api/courses/<code>/play`, and `/clear {time_ms}` on a clear (not for Endless courses: those are counted by the Endless reports) |
| SearchComments (94 / 95), PostCommentText (91) | 0x14e8ec0, 0x14e96b0 | `GET` / `POST /api/courses/<code>/comments` (text) |
| CanPostCourse (60), the course upload (66 + upload + 67), DeleteObject | 0x14e0d40, 0x14de3f0, 0x14e0c50 | allowed; the game's own course (header "SCDL", not yet encrypted) to `POST /api/courses` as the signed-in maker (`?based_on=` for an edited copy, below), its code back as the upload's data id; `DELETE /api/courses/<code>` |
| GetReqGetInfoHeadersInfo (134) + HTTP: a course's picture ("relation objects", 5 at a time) | 0x14e0e70 (`edit.c`) | `GET /api/courses/<code>/thumbnail?wait_ms=` (below) |
| the same for a course's whole-course map (More info) | 0x14e2cf0 (`edit.c`) | `GET /api/courses/<code>/entire_thumbnail?wait_ms=` |

Course World courses download and play exactly like the Endless ones (the same GetObject). Not served: Super Worlds,
Ninji speedruns, notifications, follows, Network Play.

**Thumbnails** (a course's picture on its card and course page, its whole-course map under More info): the server
keeps them as JPEGs (`thumbnails/<code>.jpg`, `<code>-entire.jpg` in its folder). For a course from TGRCode's archive
the server fetches both from TGRCode (`level_thumbnail` is the game's thumbnail block, `level_entire_thumbnail` a
plain JPEG), one request at a time with the course imports: first those a player is waiting for, then, while it has
nothing else to do, those of every course it holds. TGRCode takes a second or more a picture and rate-limits, so the
mod asks with `wait_ms` (a few seconds in all for a list's five, 6 s for a map): what hasn't come by then shows the
game's placeholder this time and is there next time. The mod signs each JPEG into the block the game checks (the
JPEG, then in the last 0x64 bytes its size, HMAC-SHA256 under a key from four seed words, the seeds; main+0x1698a80)
with the game's own functions; the map's 0x19000-byte block takes the same tail [obs: the map shows]. Our own
uploads have no thumbnails yet (the game uploads them with the course; not kept).

**Download** (the course page's Download, into the Coursebot's Downloaded Courses) works: SMM2 waits for the
course's thumbnail before it hands the download to the Coursebot (cDownload, main+0x17661f0, needs a valid entry in
the thumbnail cache, slots of 0x1c1b8 bytes: +0x1c000 data id, +0x1c1b2 failed, +0x1c1b4 valid) and keeps a course
only with a thumbnail it can check (otherwise "Corrupt data was found so the course has been deleted."). A course the
server has no picture for gets a stand-in: a 640 x 360 one-colour JPEG encoded by the mod (grey-blue [inf]), signed
the same way. A course whose picture didn't come with the list stopped the download at "A communication error
occurred." [obs]: the download asks for the picture again and the cache takes it, but keeps the list's failed mark,
which cDownload checks first; the mod clears that mark (and fills a slot still without a picture) while cDownload
waits. [obs] downloads saved (`course_data_120.bcd` + `course_thumb_120.btl`, the real picture; `_121` after a
withheld picture, `RECOMP_MOD_TEST_NO_THUMB=<code>`, the stand-in).

**Played Courses** (a maker page's tab): the server keeps each player's played courses, newest first (300): a play
posted from Course World, and every course of a challenge run (Endless, 100 Mario) on its first report; a store from
before this starts from the courses its runs passed. **Liked Courses**: the courses the player liked.

### Edit any course (`games/smm2/mods/edit.c`)

Any downloaded course opens in SMM2's own Course Maker (DECISIONS.md "Edit any course"). The placement is SMM1's:
SMM1's Course World offers **Download** (`Nwk_CourseListFrame_01/02` "Download"), and its Coursebot window of a
downloaded or sample course has **Edit** above **Play** (`Lcl_CourseInfoWindow_00` `N_SampleDataWin_00`:
`L_EditBtn_01`, `L_PlayBtn_01`; the own-course window adds Upload, Change Name, Delete) [obs SMM1 v208 layouts]; the
pause menu of a Coursebot course has "Edit Course". SMM1's 100 Mario and course clear offer no edit [obs layouts].
SMM2's window of a downloaded course has only Play, Play Together and Delete, so the mod gives it SMM2's own
**Make** button:

- **The button.** The window is one layout for both lists (`TypeMode`: My Courses' `N_MyDataBtn_00` or the
  downloaded course's `N_DLData_00`), and main+0x1891880 switches off every button but the shown list's. In a
  downloaded course's window the mod shows My Courses' Make (`L_Edit_00`) alone, at the top of the button column where
  My Courses has it (a little smaller: it fits between the header and the stats [inf]), and switches it on
  (main+0x761190) after the info window machine's Appear / Disp (main+0x188c980 / +0x188cb10) each frame; for a My
  Courses slot everything goes back. Touch reaches it; the d-pad cursor does not yet [obs] (below).
- **The press** (UI event 0x130, read in the Downloaded list's cCourseInfoWindow, main+0x16e2db0) sends the list
  to its cToPlay (main+0x16e33c0), which the mod turns into My Courses' Make: the slot is prepared as for Play
  (main+0x16e44e0) and the scene change goes to the Course Maker (reason 3, cRoboToEdit, with the slot's course
  number), as main+0x16e5c10 does for My Courses. The Course Maker loads any Coursebot slot the same way.
- **The copy** is the player's own course: upload id 0, management flags 1 and 4 cleared, no clear check yet, a
  fresh creation id (so it can be uploaded as a new course). Name and description stay. The editor's Save is "Save
  As" into My Courses (the download stays as it was).
- **The original** (code and maker name from the downloaded-course record: save buffer +0xc010 + slot · 0x2a8, +0x08
  data id, +0x16c maker) is kept in `smm2edit.txt` in the data folder under the copy's creation id. When the copy is
  uploaded, `world.c` posts it as `POST /api/courses?based_on=<code>&based_on_maker=<name>`; the server credits a
  course it holds to its own maker (otherwise the given name), and Course World shows "Based on <code> by <maker>"
  under the description (`edit_listing_description`). The original's record on the server is not touched.
- `RECOMP_MOD_EDIT=0` turns it off.

**Where this stands (2026-10-19, WIP, paused):**
- [obs] Download from Course World into the Coursebot; the downloaded course's window shows Make; a tap on it opens
  the Course Maker with that course; an edit (a row of ? blocks) saved with Save As into My Courses
  (`course_data_001.bcd`), the download untouched; the saved copy opens in My Courses and plays with the edit.
  Screens in `private/smm2/build/` of the test data folder (`e1_*`, `e2_*`, `s19_*`; private).
- Not yet: the d-pad cursor doesn't stop on Make (touch does): the window's cursor targets seem fixed before the
  mod switches Make on [?]. The saved copy still showed the "uploaded" mark and a greyed Upload in My Courses in the
  first test; the reset now also covers the Coursebot's "current course" copy (save manager +0x58), not yet checked
  in game, and the current course was 182 (the title course) in that run, so where the Course Maker takes the
  upload id from is still [?]. The upload with `based_on` hasn't been run in game (the server side is tested:
  `edited_copies_credit_the_original`). Typed course names arrive garbled: runtime/hle/swkbd.c answers in UTF-8,
  SMM2 reads UTF-16 (not this mod's file).
- 2026-10-21 re-test (main build, `RECOMP_EDIT_CTL`): Download → Make (tap) → Course Maker → an edit → Save As into an
  empty My Courses slot works, and the typed name now arrives intact ("Edited Copy"; the swkbd UTF-16 fix). The
  d-pad cursor still goes Play → Delete → the list tabs, skipping Make. The game's enable call (main+0x761190) only
  sets the button's enabled bytes (+0x3a / +0xbc in its two lists) and a bit in a pane controller's mask (+0x218 →
  +0x14b0), so the cursor's targets come from somewhere else (not found yet). On that run SMM2's current course was
  slot 2, not the download, so the Course Maker took the download from storage, not the reset copy. The copy and the
  editor's Save / Load card show the yellow arrow mark. Upload is greyed until a clear check, which is SMM2's normal
  rule.
- Fixed 2026-10-21: the Course Maker reloads the course into the current-course buffer (save manager +0x58) during
  the scene change, after the mod's reset of the prepared slot. Both buffers held creation id 1 and upload id
  0x100001 once it was up. A helper thread now resets that buffer's header whenever the original's comes back (20 s
  after the request). Checked in game: the Save / Load menu then offers Save As and Load only, as for a new course;
  after Save As the saved copy's header has flags 0 and upload id 0. The yellow arrow mark on its card stays, so it
  isn't an "uploaded" mark: own, uncleared courses have it too.
- Found 2026-10-21: Save As gives the copy a new creation id (e.g. e099c3d7 -> cf2b3d34), so `smm2edit.txt`, which is
  keyed by creation id, doesn't find it at upload. Fixed the same day: Save As asks for a name and Load doesn't, so
  while the copy is in the Course Maker, a new creation id in its buffer right after a name entry
  (`g_swkbd_entries`, runtime/hle/swkbd.c) carries the record to the new id. A change without a name entry (another
  course loaded) ends the watch. Checked in game: `655ff576 -> 7f8d0ecf`, both in `smm2edit.txt`. The upload itself
  needs a clear check first and hasn't been run in game.
- Proposed, not built: an Edit on Course World's course card (Download, then straight into the Course Maker when
  the Coursebot has saved it); in Endless / 100 Mario, "save to Coursebot for later" from SMM2's result screen
  (SMM1 offers no edit there).

Tests: `RECOMP_EDIT_CTL=<file>` (`edit_ctl.c`) reads commands appended to the file while the game runs (`tap x y
[frames]`, `btn A+B [frames]`, `shot <name>`, `wait <frames>`, `peek <addr> [len]`, `poke <addr> <byte>`, `find <addr>
<name> [depth]`); test input must be on (`LARECOMP_INPUT=999999:A:1`). `LARECOMP_KBD_AUTO=<name>` answers the
course-name keyboard of Save As. `RECOMP_EDIT_TRACE=1` logs the window's UI events.

### The bridge (100 Mario)

`hundred.h` is the interface between the run side (here) and the flow (SMM1's screens). With `ruleset=hundred`,
when Challenge Modes is chosen on Course World (its Endless sub-sequence's Activate, main+0x1a22730, before the menu
comes in) the run side calls `hundred_flow_open()`; the flow then shows SMM1's screens
and calls `hundred_run_start()`, `hundred_play_next()`, `hundred_play_skip()`, `hundred_run_finish()` and reads
`hundred_run_status()`; the run side reports `hundred_flow_course_started()` and `hundred_course_result()`.

How a server course starts in SMM2's play without SMM2's Endless screens: they stay underneath, hidden by the flow,
and the run side drives them. SMM2's Endless machinery is what makes a server course play in SMM2's own play (the
run manager, the course list and download jobs, the checks, the course-in card, the HUD with the lives and three
banked 1-Ups, the skip, the retry after a miss, the goal), and its requests already go to our server. The run side
picks the difficulty with L / R until the menu's difficulty (main+0x2c3f618) is the one wanted, then Down to the
buttons (after L / R the focus is on the tab bar), and presses the menu's own buttons (New Game or Continue, the
confirm window's Yes, the map's Play, skip, back) through the test-input hook while the player's input goes to the
flow. The course's outcome comes back through the game's own requests (course start,
miss, clear, skip). After a skip SMM2's Endless starts the next course at once; after a miss it retries by itself.

**The clear is SMM2's own:** goal, fanfare and the result screen (Boo / I like it! / Quit / Next) belong to the
player; the run side presses nothing there. `HUNDRED_CLEAR` comes at the goal (the flow shows nothing for it);
`HUNDRED_NEXT` when the player leaves the result screen with Next, `HUNDRED_GIVEUP_SAVE` with Quit (the run is kept,
SMM2 goes back to its menu). The course-end machine (registered at main+0x1bed050) tells them apart: its
cPostRatingCheckCan exec (main+0x1bef440) reads the screen's choice (+0x2a0: 5 Next, 6 / 7 the ratings, 8 Quit,
9 comment, 10 tags) and goes to cNextScene (0x4b, enter main+0x1bf6430) for Next and, in the Endless Challenge, to
cEndlessMode_GetStatus (0x4d, enter main+0x1bf6630) for Quit; the run side hooks the two enters. A miss, the skip
and SMM2's pause are SMM2's own too (the flow draws nothing for them); at 0 lives the flow calls
`hundred_run_finish()`, SMM2's game over shows, and its return to the Endless menu opens the flow's select again.

The win: the Nth clear's answer has `run_complete`; on its Next the flow plays SMM1's ending (`hundred_ending_show`)
and calls `hundred_run_finish()`, which finishes the run on the server (the completion is on its `hundred_<d>` board
already).

The flow is `games/smm2/mods/smm1/` (strong `hundred_flow_*`, `hundred_course_result`, the ending); it keeps weak
stand-ins of the run side's functions, which `bridge.c` / `endless.c` replace. `hundred_run_status()` returns a
snapshot under the run's lock (it is called on the render thread); `style` is the current course's game style.

`RECOMP_MOD_FLOW_TEST=<difficulty>` plays a whole run unattended in place of the flow (the flow's screens are left
out): the run starts on entering the Endless menu, Play follows every clear, the win plays the ending and finishes
the run. `RECOMP_MOD_AUTOPLAY=1` (or `=jump`) plays the courses (right + run [+ jump], skip after
`RECOMP_MOD_AUTOPLAY_SKIP` seconds, default 20). Both need `LARECOMP_INPUT` set (the test-input hook), which also
scripts the way to the Endless menu. `RECOMP_MOD_UNLOCK_ALL=1` opens every difficulty; `RECOMP_MOD_TRACE=1` logs
Course World's states and every request the mod doesn't answer.

**Challenge Modes** (owner, 2026-10-19). With 100 Mario offered, Course World's Endless Challenge button reads
"Challenge Modes" and opens the mode used last (`challenge_mode=` in smm2mods.ini): SMM1's 100 Mario Challenge or
SMM2's own Endless Challenge, which runs under SMM2's rules (5 lives: the start lives are switched in the game's run
records) and SMM2's screens. ZL / ZR or a tap on the header switch: on SMM2's menu its header reads
"ZL < Endless Challenge > ZR" and the run side opens the flow; on the flow's select the flow draws
"ZL ◀ 100 Mario Challenge ▶ ZR" and calls `hundred_mode_switch()`. L / R stay the difficulty, B stays back
(`hundred_flow_exit()`: the run side presses B on SMM2's menu underneath, back to Course World). **After a win**
the last result screen's Next is turned into its Quit (cPostRatingCheckCan exec, the screen's choice 5 -> 8) while the
flow plays the ending, so SMM2 goes back to its Endless menu for the next run; `hundred_run_start()` waits until it
is. **Course start** (owner, 2026-10-20: SMM2's own course-in card, the lives / loading card it shows after a miss,
also at a course's start): the map Mario's Play is pressed on SMM2's map underneath, and the menu's CourseIn state
(index 0xb, exec main+0x1a258f0: it posts 0x80, to the course, once the Decide animation is over) is held until
`hundred_flow_ready_to_start()`, which turns true when the flow's iris has closed on Mario; SMM2 then changes scene
under the flow's black, and the course scene's first state (cCourseListDownLoad enter main+0x1bbb820 or
cCourseDataDownLoad enter main+0x1bbb9a0, else the course start request) goes to the flow as
`hundred_flow_course_in()`, which lets the black go: SMM2's card comes up out of it. With the "SMM1 course card"
setting (`hundred_smm1_card=1`) the flow shows SMM1's card instead, and a course SMM2 loads underneath the map waits
at its first play frame until `hundred_flow_ready_to_start()` (the card is up). **Transitions:** after the result screen's
Next SMM2's own block wipe (`Cmn_Fade_00` OutBlock / InBlock from the game's romfs, drawn by the flow) covers it and
uncovers the map (or the ending); the ending goes to the select the same way; the
select fades in over Course World when it opens (SMM2's Activate is held until the select is in, so SMM2's Endless
menu comes in under it unseen; it used to open on that menu's first Disp, which showed SMM2's menu for half a second
first) and out over SMM2's Endless menu when Challenge Modes switches or B leaves (B starts SMM2's way back to Course
World at once, under the map's Out; a B pressed while that menu is still coming in counts once it is in,
`hundred_menu_in()`). After a Save & Quit (the pause's, or Quit on the result screen) SMM2's way back skips that
Activate and shows its menu as it fades in from black; the flow's black comes up on SMM2's black (the scene fade's
waiting state, enter main+0x192f580: `hundred_flow_cover()`) and the select comes out of it. A won run goes back under
the ending; a game over goes to Course World through SMM2's GAME OVER scene. **Challenge Modes input:** the buttons and the
touch held when the flow opens count as held, not pressed, until they are let go: SMM2's menu acts on a ZL / ZR in
the very frame it comes, before the flow has seen it held, so the select used to take the same press and switch
straight back to Endless (half the switches back to 100 Mario bounced, in a test with the player's own key path); on SMM2's menu a
press is caught on the run side's driver thread as well as on the menu's Disp and is good for a second (the Disp
doesn't run while the menu changes difficulty or redraws its map, as right after a switch, and a press then was
lost). `RECOMP_MOD_TEST_SWITCH=<letters>` makes switches as a player does (z ZL, Z ZR, t a tap on the header, . a
pause), `RECOMP_MOD_SHOT_BURST=<frames>` takes a window shot every few frames after Next and after SMM2 leaves its
map.

**The RomFS overlay** (`romfs_overlay.{h,c}`): the mods' generated game files (the Challenge Modes texts,
`relabel.c`; costumes; layout edits) come from the player's own dump at the first launch that needs them, into
`<data>/mods-generated/romfs/` (private), and replace the game's files (`game_romfs_overlay()`). A feature registers
`ROV_GENERATOR(name, version, fn)`; the folder is generated again when the set of names and versions changes.
Helpers: files (`rov_read`, `rov_write`), SARC with or without Yaz0 (`rov_sarc_*`), MSBT texts (`rov_msbt_set`),
BFLYT sections (`rov_flyt_*`).
**Loading:** a course list may take long (TGRCode upstream: about a minute for a difficulty's first batch); the
request waits up to 3 minutes and `hundred_run_status().loading` is set meanwhile.

**Pause.** SMM2's own pause opens in a run course (Start Over costs a life, posted as `start_over`; its Save & Quit
suspends the run and the menu's return sends `HUNDRED_GIVEUP_SAVE`). `RECOMP_MOD_SMM1_PAUSE=1` brings back the
earlier interception for the flow's own pause: SMM2's pause state (`cPausePlay`, state 3 of the Endless play scene's
machine; its exec is the scene's virtual at vtable+0x390, main+0x1bba050) is held without opening its menu and the
flow gets `hundred_flow_pause()`; `hundred_play_resume()` leaves it as the menu's close does (state 1),
`hundred_play_retry()` as Start Over (state 0x13), `hundred_run_save_quit()` as the confirm window's Yes (state
0x12, the suspend). **Next course data:** when a course starts, the one after it is downloaded in the background
(`/next?count=2`, its `/data`), so the next course-in doesn't wait on the server. **Continue:** the run record's state byte (+4 = 1) makes the menu
offer Continue (and show the run on its map); `hundred_run_start(d, true)` presses it. **Course on screen:**
`hundred_flow_course_started()` comes on the first frame of the play scene's play state (state 1, vtable+0x350,
main+0x1bb9440) after a course start or a retry. **Next course:** the head of the server's queue is read after New
Game, each clear / skip and on Continue, so `hundred_run_status().current` names the next course before the game
asks. `form` is player 0's form at the goal (+0x4a8, small / big from the size class table main+0x23d5e0c).

Flow test extras: `RECOMP_MOD_FLOW_TEST_CONTINUE=1` (Continue instead of New Game), `RECOMP_MOD_FLOW_TEST_PAUSE=<letters>`
(pause 8 s into a course and answer r resume, o Start Over, q Save & Quit; with `RECOMP_MOD_SMM1_PAUSE=1`),
`RECOMP_MOD_FLOW_TEST_QUIT_AFTER_CLEAR=<n>` (Quit, tapped, on the first n result screens),
`RECOMP_MOD_FLOW_TEST_RESULT_KEYS=<letters>` (keys, stick moves or a tap on the result screen first; see
`result_keys` in `bridge.c`), `RECOMP_MOD_SHOTS=<name>` (window shots at the run's moments into
`build/<name>_<n>_<what>.png`). On the result screen the D-pad and the stick don't move the focus off Next in the
port yet [obs]; touch reaches every button.

### Test recipe (end to end, unattended)

The whole chain (difficulty select -> map -> card -> course -> clear -> ... -> ending) against a local server, the
window hidden. Paths are the ones on the dev machine; any folder works.

**1. The server.** The test server is `/Volumes/SSD/smm-work/recomp-mods/server-test` on port **8650**, 12 short
test courses (flat / items / power-up courses in each style; running right and jumping clears most), one account "Runner" whose token is in
`/Volumes/SSD/smm-work/recomp-mods/account-test.txt` (line `token <hex>`; keep it out of logs and commits). The
corpus server is `server-data` on port 8640 (550 real courses, account in `account.txt`). Start one (if
`curl http://127.0.0.1:8650/api/health` doesn't answer):

    cd /Volumes/SSD/smm-work/recomp-mods
    ./cargo-target/release/mmk server --dir server-test --addr 127.0.0.1:8650 \
        --writes-per-minute 100000 --accounts-per-hour 1000 > server-test.log 2>&1 &

A new one, seeded (any raw / zlib SMM2 course files; the 30 short ones in `testcourses/` were made for this):

    cd core && cargo build --release -p mmk-cli          # (target/release/mmk)
    mmk server --dir my-test --addr 127.0.0.1:8651 --writes-per-minute 100000 --accounts-per-hour 1000 &
    mmk register Runner --server http://127.0.0.1:8651 > my-account.txt   # maker ID + token
    mmk upload --server http://127.0.0.1:8651 --maker testlib /Volumes/SSD/smm-work/recomp-mods/testcourses/*.course

A run needs at least `total` courses in the difficulty's band (or the band widens): with only short test courses,
every difficulty draws from all of them.

**2. The game.** Build (`cd recomp && make GAME=smm2 -j10 all`), then from the data folder (`recomp/private/smm2`):

    cd recomp/private/smm2
    export LARECOMP_HIDDEN=1 SMM2_SEED=4 RECOMP_LOG_TIME=1
    export RECOMP_MOD_100MARIO=1 RECOMP_MOD_RULESET=hundred RECOMP_MOD_LOG=1 RECOMP_MOD_UNLOCK_ALL=1
    export RECOMP_MOD_SERVER=http://127.0.0.1:8650
    export RECOMP_MOD_TOKEN="$(sed -n 's/^token //p' /Volumes/SSD/smm-work/recomp-mods/account-test.txt)"
    export RECOMP_MOD_AUTOPLAY=jump RECOMP_MOD_AUTOPLAY_SKIP=25
    export LARECOMP_INPUT="2500:L:20,2500:R:20,2800:RIGHT:8,2900:A:8,3300:DOWN:8,3400:A:8,4600:DOWN:8,4700:RIGHT:8,4800:A:8"
    ../../build/smm2/smm2recomp extracted > run.log 2>&1

- `LARECOMP_INPUT` (`frame:BUTTON:frames`, presented frames from boot) goes from the title to the main menu, to
  Course World, then to Endless Challenge (frame 4800: DOWN, RIGHT, A).
  It is timed for `SMM2_SEED=4` and the save in `/Volumes/SSD/smm-work/recomp-mods/private/smm2/save` (Course
  World seen before, no first-visit intro): copy that `save/` over yours if the script lands elsewhere.
  `LARECOMP_HOST_INPUT=<frame>:shot:<name>,...` saves window shots into `private/smm2/build/`.
- Choosing Challenge Modes under `ruleset=hundred` calls `hundred_flow_open()`: the flow takes over from there
  (it calls `hundred_run_start()`, `hundred_play_next()` ...). `RECOMP_MOD_FLOW_TEST=<0..3>` instead plays the run
  without the flow (the stand-in in `bridge.c`).
- `RECOMP_MOD_AUTOPLAY=jump` plays each course (right + run + jump) and skips it with the test input after
  `RECOMP_MOD_AUTOPLAY_SKIP` seconds; under `RECOMP_MOD_FLOW_TEST` the test presses the result screen's Next
  after a clear (a real player's run side presses nothing there).
  `RECOMP_MOD_UNLOCK_ALL=1` opens Expert and Super Expert without winning the runs before.
- A Super Expert run (6 clears) takes about 6-12 minutes of game time; Easy (8) about 10. Expected log lines
  (`grep '\[mod\]' run.log`): `New Game: super_expert, hundred rules, 100 lives`, `course start <code>`,
  `clear <code> ... clears n`, `... -- 100 Mario Challenge complete!`, `bridge: run finished`.
- After the run: `curl -s http://127.0.0.1:8650/api/rankings/hundred_super_expert` lists the completion.

### Where this stands (2026-10-19)

- Done [obs on macOS]:
  - Challenge Modes: the relabel, ZL / ZR / the tap, the remembered mode, B back to Course World, and 5 lives on Endless right after a switch.
  - Separate run slots per ruleset on the server.
  - The 100 Mario run side: Next / Quit on SMM2's result screen, a won run back to SMM2's menu, no course twice across a player's runs.
  - Broken courses: a course the game finds corrupt counts as a skip. The run comes back after the window (Course World → a tap on Challenge Modes → Continue → the next course; `RECOMP_MOD_TEST_BREAK=<n>:<code>` reproduces it).
  - The result screen's comment and tag buttons are enabled (comment allowances 10/10, the course's comments-on flag course global +0x860).
- Half done:
  - Likes and boos: the hearts are enabled. A tap on "I like it!" didn't post a rating before the test was stopped, so whether PostRatingInfo (world.c `post_rating`) reaches the server from the result screen is unverified. Course World's own clear screen is untested too.
  - Posting a comment is untested.
  - Test-injected D-pad presses don't move the result screen's focus. Real keys and pads do.
- Next:
  - Verify a like / boo from the result screen (a tap, then Next: `rating <code>: 1 (like)` in the log, then the course's likes on the server).
  - Comment posting.
  - The 0-lives game-over round trip with the flow.
  - Local play over the vlan relay (ldn emulation).

### Limits

- Not online yet: Network Play (plan: `DECISIONS.md` "Online multiplayer"), Super Worlds, Ninji speedruns,
  follows, notifications; they answer "not connected" or empty.
- The Maker avatar is the console's default Mii with your account's name (creating one needs the Mii editor applet,
  which the runtime doesn't have). `online.c` puts it in the save's user settings (+0x60) when there is none, so the
  avatar icon at the top left of Course World's menus shows it from the start (it was a white box until a maker page
  brought the record's Mii).
- No SMM2 Mii-costume rewards (the server sends no notifications).

## SMM1 extras: one setting per SMM1 feature

Each setting turns on one SMM1 behaviour in the otherwise exact SMM2 game. All are **off by default**: with every one
off, the game is SMM2's (an input replay against Ryujinx stays identical, below). They are switches in the overlay
menu's Mods tab (always shown), kept in `smm2mods.ini`; an environment variable wins over the ini.

| Setting (Mods tab) | ini key | Variable | When |
|---|---|---|---|
| Overlapping pipes (SMM1) | `smm1_overlap_pipes=1` | `RECOMP_SMM1_PIPES=1` | from the next course load / placement |
| Mid-air shell jumps (SMM1) | `smm1_shell_jumps=1` | `RECOMP_SMM1_SHELL_JUMPS=1` | immediately |
| SMM1 course card (100 Mario) | `hundred_smm1_card=1` | `RECOMP_HUNDRED_SMM1_CARD=1` | from the next course |

`RECOMP_SMM1_LOG=1` logs every decision the hooks see (`[smm1] ...`), on or off.

How: host code after single instructions of the game (`games/smm2/mods/smm1_extras.py` lists them with the
instruction word each expects; `smm1_extras.c` acts on them). With a setting off its hooks change nothing.

**Overlapping pipes.** SMM1 let a pipe overlap another pipe: Nintendo's own SMM1 sample courses have six such
overlaps (elbows, T joints, a pipe inside a pipe) [obs, the course files]; none of the 550 sample SMM2 courses has one.
SMM2 forbids it in one place, its placement check (the course analyzer the Course Maker and the course loader both
run): for each part with an overlap rule it collects the parts in the cells it covers and asks, per pair, whether the
other part is "solid" (main+0xfe8490: pipes count); a solid part whose shape overlaps is a conflict [obs].
- Course Maker: a pipe dragged onto another turns red and goes back where it was [obs].
- Course load: a course with overlapping pipes counts as broken and a blank course plays instead [obs].
- With the setting on, the hook after that call (main+0xfe827c) answers "not solid" for a pipe against a pipe, so
  both accept it; pipes against anything else are checked as before. In play, overlapping pipes are two pipes:
  each draws and collides as itself [obs: in the test course Mario walks into one, stands on the short one and
  jumps up onto the tall one it overlaps].
- A course saved with overlapping pipes needs the setting to load (SMM2, and the setting off, treat it as broken).

**Mid-air shell jumps** (in progress: hooks in place, the test course is not done). SMM1's shell code is SMM2's (the
kicks, the mid-air kick off a wall with its 3.52 hop, the speeds) except two rules [obs, Block.rpx v208 against
SMM2]; on a shell the setting applies SMM1's:
- the stomp window when the player is falling onto an ordinary-size target: SMM1 a flat 10 px; SMM2 3 px less plus
  the player's fall this frame (main+0x1267ce8: the "falling?" compare before that step reads "no");
- the kicker's no-hit time after a mid-air kick off a wall: SMM1 5 frames (the contact's own minimum), SMM2 16
  (main+0x1202c88: the 16 becomes 0, so the 5 applies). The shell coming back off the wall can be landed on sooner.

SMM1 applied the flat window to every small enemy; the setting limits it to shells.

**Test courses.** Made at run time from the player's own data, never in the repository: our pipe courses are built
from SMCraft's flat test course, the SMM1 one copies the pipes, ground and blocks of SMM1's sample course 95 from the
player's SMM1 dump into an SMM2 SMB1-style course. Installed as the title course (`RECOMP_MOD_DIR`) they play straight
away; the Course Maker is reached with `LARECOMP_HOST_INPUT` (L + R, A, then taps; `frame:drag:x0/y0/x1/y1:frames`
moves a finger across the touch screen).

**Where this stands (2026-10-19).**
- Overlapping pipes: done. It covers the Course Maker drag, the course load and play, and the off-case replay.
- Mid-air shell jumps: the hooks are in, and `RECOMP_SMM1_LOG=1` shows both windows at every shell stomp test. No
  test course yet shows a mid-air shell jump that works with the setting on and fails with it off. The candidate is
  a ledge course: Mario drops off a 2-tile start onto a shell sliding back from a wall, and the frame he walks off
  is swept.
  - [obs] Dropping at updates 441-447, the shell meets him while he falls at 1.5-2.5 px a frame. SMM2's window
    there is 8.5-9.5 px against SMM1's 10.
  - Left: the same sweep with the setting off, to find the frame where the outcome differs.
  - Left: an off-case replay with shell contacts. The replay so far has pipes but no enemy contact.
- More SMM1 behaviours from research/specs/smm1-extras* are not started: pipe spawn rates per enemy, spawn caps,
  Clown Car weights, attack reactions. They are EnemyDB data changes.

**Off-case check.** The same input session on a course with side-by-side pipes, played in the recomp (settings off)
and in the Ryujinx trace build (`tools/replay_sync.py`'s comparison): 606 of 606 player updates identical, the whole
12 KB player object and the input block too.

## SMW goal wipe (`games/smm2/mods/smw_goal.c`)

In Super Mario World style courses, a goal-tape clear in real play ends the way SMW's levels do: once the fanfare is
over, a black circle closes on Mario with SMW's own goal-wipe sound. Then:

- where SMM2 shows its clear panel (Course World, Story, Endless / 100 Mario...), the black fades into the panel's own
  wipe and COURSE CLEAR! card; Next then leaves the course as SMM2 does;
- where it shows none (My Courses), the screen stays black while SMM2 changes scene under it, and the black fades away
  when the next screen is up.

The castle's axe, online versus and the Course Maker's test play are left as they were. On by default: the Mods tab's
"SMW goal wipe" switch, `smw_goal_wipe=0/1` in `smm2mods.ini`, or `RECOMP_SMW_GOAL_WIPE=0/1` (wins over the ini).

- **The sound** is SMW's original (`SuperMarioWorld_43_GoalWipe`, 0.8 s at 32 kHz), which SMM2 keeps in its story
  mode's sound bank `Sound/Resource/Quest_UndoDog.bars`. The mod reads it from the player's RomFS (`rov_read_game`) and
  decodes it (`smm1_bars_sound`, `smm1_bwav_decode`: SMM2's BARS of BWAVs, DSP-ADPCM, in `smm1/smm1_audio.c`); it
  plays through the mods' mixer (`smm1_play`). Nothing is shipped.
- **The circle** is drawn host-side (`smw_iris`, an antialiased circle over 1280 x 720) through
  `game_overlay_image`, which the 100 Mario flow owns: it hands its frames to `smw_goal_image` while it has nothing
  of its own to show, and calls `smw_goal_poll` every frame. The centre is the body of the player who broke the tape
  (actor x / y at +0x230 / +0x234, mapped through the camera's view rect); the screen centre when that can't be read.
  The circle takes the sound's 0.8 s, then 0.25 s of black.
- **The timing** comes from MariowGoal's own Fade-state thunks (enter main+0x12d2890, execute main+0x12d28a0: every
  goal type has its own, so the axe never comes here). The enter override arms the wipe once per goal (the demo
  controller sits at the same address course after course, and the Fade can start after the fanfare's counter has run
  out: the Fade comes on a 480-frame timer, the fanfare plays in real time, so slow frames leave no counter to see).
  SMM2's Fade waits while a fade is under way or while the demo's counter (fanfare, then 35 frames) runs; then it
  opens the clear panel, or asks for the scene change. The execute override starts the armed circle on the first
  frame the Fade would go on and skips the Fade until the circle is shut (a skipped frame changes nothing: the Fade
  only waits). Then the Fade runs: a scene change asked for (the scene manager's byte at +0x344)
  keeps the black until the new scene exists; otherwise the black fades 54 frames later, when the panel's staircase
  wipe covers the course.
- Research (private): `research/specs/smw-goal-wipe.md`.
- Test: `RECOMP_SMW_GOAL_SHOTS=<every>` saves a window shot every `<every>` frames for 4 s from the wipe's start
  (`build/smwgoal_<frame>.png`). The 100 Mario test recipe above with a server holding only `MW_*` test courses
  clears SMW courses unattended.

## Muncher hit box (SMM1) (`games/smm2/mods/muncher_hitbox.c`)

Munchers hurt the player the way SMM1's did. In SMM2 a one-tile gap between two Muncher columns can only be fallen or
jumped through dead centre; with the mod Mario has SMM1's room of about 2 px either side. Off by default: the Mods
tab's "Muncher hit box (SMM1)" switch, `muncher_smm1_hitbox=0/1` in `smm2mods.ini`, or `RECOMP_MUNCHER_SMM1_HITBOX=0/1`
(wins over the ini). It applies from the next frame.

- **SMM2** gives a Muncher one body, a 16 x 16 box that is also a spike: any side the player touches hurts. The
  player's wall sensors reach 7.9 px either side of him, so in a 16 px gap more than 0.1 px off centre is a touch
  [obs: in a test course Mario 2.2 px off centre is pushed to x = gap centre - 0.1 and dies].
- **SMM1** (Block.rpx v208) builds the Muncher from two bodies: the same 16 x 16 box, harmless, for everything else,
  and a spike box 2 px smaller on every side (12 x 12) for the player. Enemies' sensors skip the spike box. The player
  meets the spike 2 px later from any side, so for him the gap is 20 px wide and he can be 2.1 px off centre.
- **The mod** gives the player SMM1's box. While a player's per-frame update runs (main+0x158a590, called through the
  player's vtable), every live Muncher's body is shrunk to the inner box: its edges and its world bounds (the player's
  queries go by both) are scaled by 12/16 about the box's centre, so a Big Muncher's loses 4 px a side. They are put
  back byte for byte when the update returns. Enemies, items and everything else still meet the 16 x 16 box. Munchers
  are found when they are built (main+0x107d340, the build function both Muncher profiles point at) and dropped when
  they are destroyed (main+0x107ed30 / 0x107ed80).
- **Tested** on macOS with a title-screen test course (two Muncher columns, a one-tile gap, small Mario riding donut
  blocks straight down at offsets -2.2, -0.7, +0.8, +2.3, +3.8 px from the gap's centre): off, he dies at every offset;
  on, he reaches the floor at -0.7 and +0.8 and dies at the others, pushed out 2 px later than without the mod. Big
  Munchers and enemies on Munchers weren't run.
- Research (private): `research/specs/objects/57-muncher.md` (section "Hit box").

## Quick level return (`games/smm2/mods/quick_retry.c`)

After a miss the course starts again (at its start or the checkpoint) much sooner. Off by default: the Mods tab's
"Quick level return" switch, `quick_retry=0/1` in `smm2mods.ini`, or `RECOMP_QUICK_RETRY=0/1` (wins over the ini).
It applies immediately.

- **How:** the game plays its own miss (the lost life, the lives card, the course set up again). The frames from
  the death fanfare until the restarted course run as fast as the machine can and aren't shown
  (`runtime/nvn/objects.c` `g_present_hidden`): no display pacing and no window image, but each frame's GPU work,
  the renderers' per-frame work and the 100 Mario flow's tick still run. Nothing in the game's logic is changed.
  Lives count as before (100 Mario and Endless: a miss still costs one), and so do clears, checkpoints and skips.
  Off, the hooks only return.
- **Start:** the death fanfare (music state 7: `costume.c` `down_fanfare`, main+0x17a4bc0).
- **End:** in Endless and 100 Mario, the Endless scene's play state running again (`bridge.c` `play_exec`,
  main+0x1bb9440), up to 900 frames. Elsewhere, the first player update after frames without one (`trace.c`), each
  update extending the fast-forward by 60 frames, up to 360. Where no player comes (test play's miss goes back to
  the Course Maker) it lapses by itself.
- **Measured** (M-series Mac, muted test runs), miss to playing again:

  | | SMM2 | Quick level return |
  |---|---|---|
  | Coursebot course | ~4.6 s | 1.6 s |
  | 100 Mario (lives card included) | 9.7 s | 2.3 s |

  Test play's miss went back to the Course Maker about 0.5 s after the miss. About 0.7 s of each restart is the
  course's reload, which the fast-forward can't shorten. With the switch off, the same 100 Mario run missed at the
  same point, took SMM2's 9.7 s, and never ran above 60 fps.
- **Test:** a one-course server holding `testcourses/3W_water.course`, the 100 Mario recipe above with
  `courses=server` (a copy of `smm2mods.ini` through `RECOMP_MOD_INI`), `RECOMP_HUNDRED_INPUT="5950:A:8,6150:A:8,6350:A:8"`
  for the flow's map, and right held from frame 6700: Mario misses at frame ~9780. `LARECOMP_FPS_INTERVAL=0.25`
  shows the fast-forward's rate. Don't save frames during it: the PNG writes slow every frame down.

## The game's own network stack (backend local-nex): groundwork for Nextendo and a local NEX server

The mod above *fakes* being online: nifm / acc / friends / mii answer locally, the enl session logs in without NEX,
and the game's request functions are answered from our HTTP server. The runtime also has a **real network stack**
for the game's own NEX client, so that SMM2's NEX code can one day talk to a community server (Nextendo Network)
or our own NEX server. For now it is **offline: local test server only**. Nothing reaches Nextendo's servers until
the owner has their permission and an app registration, and nothing ever reaches Nintendo's.

### Backends

| `backend=` (smm2mods.ini) / `RECOMP_NET_BACKEND` | What runs |
|---|---|
| `ours` (default) | the mod path above; the game's sockets stay off (only the virtual LAN), every name lookup fails |
| `local-nex` | the mod's online overrides switch **off** (`mod_hundred()` is false), so the game's own enl / NEX code runs on real host sockets against a local NEX test server |
| `nextendo` | refused (logged: awaiting registration); the backend stays `ours` |

In the Mods tab: "Online: local NEX test server (developers)" (the next start). For the coming app-level settings
page, `net_backend_options()` / `net_backend_select()` (`runtime/net/netcfg.h`) list the three with Nextendo greyed
out ("awaiting registration").

### Settings (all in smm2mods.ini, or the environment)

| ini key | Variable | Meaning |
|---|---|---|
| `backend=` | `RECOMP_NET_BACKEND` | above |
| `nex_test_server=` | `RECOMP_NET_TEST_SERVER` | `ip[:port]` of the test server (default `127.0.0.1:44300`); local addresses only (loopback, private, link-local, 100.64/10). The game's TCP connections to it on 443 go to that port |
| `net_hosts=` | `RECOMP_NET_HOSTS` | extra redirect entries, `name=ip;*.domain=ip` (local addresses only) |
| `tls_trust=` | `RECOMP_NET_TLS_TRUST` | `local-ca` (default): the server must present a certificate from the local test CA (`net/test-ca.pem`); `any`: any certificate, only towards local addresses |
| `tls_ca=` | `RECOMP_NET_TLS_CA` | another CA file |
| `id_token_file=` | `RECOMP_NET_ID_TOKEN_FILE` | a backend-issued ID token to send instead of the local one |
| `nex_token_file=` | `RECOMP_NET_NEX_TOKEN_FILE` | a backend's NEX login token (Nextendo's `nx2.` token), carried in the local ID token's `nnex` claim; a secret: never logged |
| `net_log=1` | `RECOMP_NET_LOG=1` | log every socket, name, TLS and token call (TLS plaintext: the first 160 bytes of each read / write) |

### Pieces

| Piece | Where | What |
|---|---|---|
| Configuration | `runtime/net/netcfg.c` | backend, **deny-by-default redirect table** (with `local-nex`: `*.nintendo.net`, `*.nintendo.com`, `*.nintendowifi.net`, `*.nintendo.co.jp`, `*.nintendoswitch.cn` → the test server; every other name: no such name), destination policy (a game socket reaches loopback and the table's targets only), TLS trust |
| Sockets | `runtime/services/bsd_host.c` (bsd.c dispatches) | `bsd:u` / `bsd:s` on host sockets (POSIX and Winsock, `runtime/net/hostsock.h`): TCP and UDP, non-blocking I/O, poll / select, socket options, accept / listen, FreeBSD errno numbers. Blocking calls wait in 50 ms slices with the guest thread's core released (`kernel_host_wait_begin`). A UDP port the game binds that is taken on the host gets another host port, and the game keeps seeing its own, the same pair again on a rebind (two instances on one machine). A UDP socket the game bound and closes is **parked** for 10 s and handed back when the game binds that port again (PIA closes and rebinds after its NAT check; the NAT mapping must survive). Connects / sends elsewhere: `ENETUNREACH`, logged. The virtual LAN keeps its own path |
| Names | `runtime/services/names.c` | `nsd:u` / `nsd:a` (`%` → `lp1`: the game asks for `g22306d00-%.s.n.srv.nintendo.net`) and `sfdnsres` (GetAddrInfo(WithOptions), GetHostByName(WithOptions)) from the redirect table only; the host's DNS is never asked |
| TLS | `runtime/services/ssl.c` + `third_party/mbedtls` (3.6.7 LTS, Apache-2.0) | `ssl`: contexts, connections over the game's socket, handshake (TLS 1.2), read / write / peek / pending / poll, ALPN (http/1.1 only), server certificate out. The **runtime's** trust policy decides (the game's own CAs and client certificates are ignored); when the game names an address as the host, the name last resolved to it is used for SNI and the check ("rebuilt") |
| Account | `runtime/net/token.c`, `services/acc.c` | a local network service account ID (`GetAccountId`; the game logs in with it as its NEX user name) and an ID token: a JWT (HS256, `kid` `recomp-local`, claims shaped like a network service account's plus `tv` = the game version and, with `nex_token_file=`, `nnex`) signed with a local key, both kept in `net/account.txt` in the data folder (private). `id_token_file=` plugs a whole backend-issued token in instead |
| Test server | `tools/nex_test_server/` (Python, standard library + the `openssl` command) | a local NEX server: TLS on a local address, the HTTP/1.1 WebSocket upgrade, PRUDP Lite (SYN, CONNECT, acks, pings, fragments, resends), RMC, the Kerberos-style ticket (RC4 + HMAC-MD5), the secure server on PRUDP port 2, and enough of SMM2's DataStore for Course World's login (table below). Every request is logged and decoded where known; the token's signature is checked with `net/account.txt` and masked in the log. Makes the local test CA and its certificate in `recomp/private/smm2/net/` on first run (git-ignored; the proprietary guard blocks keys and certificates) |

### What the game does [obs, v3.0.3, backend local-nex]

When the enl session logs in, the game: loads the ID token (acc), asks nsd to resolve
`g22306d00-%.s.n.srv.nintendo.net` (→ `-lp1`), resolves it with getaddrinfo, then gethostbyname, opens a TCP
socket (SO_NOSIGPIPE), connects to port 443, makes an ssl context (version 1) and a connection (option
DoNotCloseSocket on, verify option 0x3, host name set), does the handshake, sends
`GET / HTTP/1.1` with `Upgrade: websocket`, `Sec-WebSocket-Key: NEX`, `Sec-WebSocket-Protocol: NEX`, and then
PRUDP Lite in binary frames: SYN (stream RVSecure, port 31 → 1, supported functions 6), CONNECT (with a lite
connection signature), DATA: RMC `Authentication.ValidateAndRequestTicketWithParam` {platform 3, user name = the
account ID in decimal, `AuthenticationInfo` {the ID token, NGS version 4, token type 2, server version 0}, NEX
version 40605, client version 60}. Given a ticket and the station URL `prudps:/address=0.0.0.1;port=1;...;sid=2`
("the same server"), it opens PRUDP port 30 → 2 **on the same WebSocket**, sends the ticket in its CONNECT, then
calls, in this order and all at boot (about frame 280, long before Course World opens):

| Request | The test server's answer |
|---|---|
| SecureConnection.Register (its station URL `prudp:/address=127.0.0.1;port=1;Pl=3;Tpt=2;...;sid=30`) | success, connection id, the URL back |
| Utility.GetIntegerSettings(0) | an empty map |
| DataStore.GetUsers (48) {own PID, option 0xe284} | one UserInfo (revision 3): the PID, a maker code, the name, a default Mii (CharInfo, the same as `services/mii.c`'s), country US. An empty list sends Course World to "Create Maker Avatar" (which needs the console's Mii editor) |
| DataStore.GetUserNameNgType (65) | 0 (the name is fine) |
| DataStore.SyncUserProfile (49) {name, Mii (the console's CharInfo), region, country ...} | the profile back |
| DataStore.GetMiiClothes (63) | none |
| DataStore.GetNewNotification (125) | a structure of three bools, all false [obs: its response extractor, main+0x5ef890, reads the structure header and three bools] (an error answer here crashed the game's network worker later: "Pure virtual function called") |

[obs] The Mii in a UserInfo is the 0x58-byte `nn::mii::CharInfo`: the GetUsers converter (main+0x14d0a60) copies
the buffer into the user entry as it is.

**Proof 2** (2026-10-19): with these answers Course World's top screen opens through the game's **own** NEX client
(Courses, Leaderboards, Network Play, Endless Challenge, Ninji Speedruns, the Maker card with the Mii); no mod
override ran (no `[mod]` line). Evidence (private): `/Volumes/SSD/smm-work/recomp-net/evidence/proof2/`. Course
lists, course downloads and the rest of DataStore are not answered yet (they log and get Core::NotImplemented).

### Try it

    python3 -m tools.nex_test_server --log /tmp/nex.log        # from the repository root; listens on 127.0.0.1:44300
    cd recomp/private/smm2 && RECOMP_NET_BACKEND=local-nex RECOMP_NET_LOG=1 ../../build/smm2/smm2recomp extracted

The server logs the game's TLS connection, the upgrade, the PRUDP handshakes, the decoded login (the token's
signature masked) and every request; then go to Course World: its top screen opens.

### Before a real Nextendo connection (research/specs/recomp-nextendo.md)

- the owner's go-ahead: Nextendo's permission and **an app registration** (a client ID for the recomp; their
  developer portal is team-only, and their first-party client ID must not be used);
- **app-level sign-in**: OAuth 2.0 with PKCE (loopback redirect on desktop, `ASWebAuthenticationSession` on iOS),
  then their NEX token each launch (24 h); it goes into the ID token's `nnex` claim (`nex_token_file=` today);
  `GetAccountId` must return their PID (today: the local account's ID);
- **trust and addresses**: their CA pinned (a third `tls_trust` mode) and redirect entries to their public
  addresses, both behind an explicit setting (today every redirect target must be a local address and nothing
  else is reachable); UDP to arbitrary peers for PIA (today: refused);
- the rest is the game's own code: NAT check (`nncs1/2`), MatchmakeExtension, PIA peer-to-peer; their gates (a
  verified e-mail, one device at a time) show as login errors.

### Proof 3 (two recomp instances in one Network Play lobby): where this stands (WIP, paused 2026-10-19)

- **Server side built** (`tools/nex_test_server`): the NAT check server (UDP 10025 / 10125, replies from 10026 /
  10027), server-sent requests (notifications, NAT traversal's InitiateProbe), MatchmakeExtension auto-matchmake
  (3 / 15 / 40), create / join (6 / 7 / 38 / 39), MatchMaking.GetSessionURLs, NAT traversal (1 / 3 / 4 / 5 / 6 / 7),
  SecureConnection UpdateURLs / ReplaceURL, MatchmakeExtension's no-answer methods, EndParticipation; sessions
  matched by game mode (`matchmaking.py`); accounts of several data folders (`--accounts`).
- **[obs] the application config is Utility.GetIntegerSettings(0)** (keys 0..37 -> the game's config array; the
  request function main+0x14ce2f0). With an empty map Co-op shows "This service is not active right now"; with every
  key 1 (now the default, `--config k=v` overrides) Co-op goes on: Network Play menu -> `cInetMultiClear` ->
  `cGetMyUserInfo` -> `cAutoMatchReadyEx` -> NetAppTask `cStartAutoMatch` -> enl `cAutoMatchReady` ->
  **MatchmakeExtension.AutoMatchmakeWithParam_Postpone** (game mode 11 for co-op, 6 attributes) arrives at the server.
- **Where it stops:** the game refuses the server's answer (the new session it creates): enl `cSilentError`,
  then MatchMaking method 2 and the Network Play error window. Next: check the MatchmakeSession encoding against the
  game's extractor (structure versions, the MatchmakeParam and later fields), as was done for GetNewNotification.
- **Still to do:** the NAT check and PIA's UDP (not reached yet); two instances on one Mac probably need a
  per-instance virtual address for PIA's UDP ports (127.0.0.2 can't be bound without root); the co-op course download
  (DataStore PrepareGetObject + an HTTPS course file); then both instances in one lobby, a course started, inputs.
- **Tools** (private, `/Volumes/SSD/smm-work/recomp-net/`): `run2.sh` (server + two instances with their own data
  folders / accounts), `cycle.sh` (one instance), `RECOMP_NET_TRACE_STATES=<states.txt>` (the enl / Network Play /
  NetAppTask state names, made from the player's own game by `stategen.py`; never committed) logs each state entered.
- Machine load matters: with ~28 other game instances on this Mac one instance reaches Network Play in 4-12 min.
