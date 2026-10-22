//! A small self-hostable course server (Phase 10): our own open protocol, not Nintendo's.
//!
//! Courses live in a directory: `courses/<code>.bin` (raw course data, validated on upload) and
//! `index.json` (metadata and play statistics). HTTP API (JSON unless noted):
//!
//! | method | path | does |
//! |---|---|---|
//! | POST | `/api/courses` (body: course data; header `X-Maker`) | upload → `{code, …}` |
//! | GET | `/api/courses?q=&style=&sort=new\|popular\|hot\|hard&limit=&offset=` | search (`hot`: as `popular`); with an upstream source, `popular`, `hot` (Popular's latest uploads first) and `new` (no `q`) are its live lists first (`lists`) |
//! | GET | `/api/courses/<code>` | metadata (of a held or an upstream-listed course); here and in the course lists (search, a maker's, played / liked), its `comments` count and, signed in, the player's own `you` = `{played, cleared, vote}` |
//! | GET | `/api/courses/<code>/data` (`?wait_ms=`, at most 20000) | course data (bytes); an upstream-listed course the server doesn't hold is fetched and imported, waited for up to `wait_ms` (default 7000; 404 meanwhile) |
//! | GET | `/api/courses/<code>/thumbnail`, `/entire_thumbnail` (`?wait_ms=`, at most 20000) | the course's picture and its whole-course map (JPEG); one of an upstream course the server doesn't hold yet is fetched from the source, waited for up to `wait_ms` (404 meanwhile) |
//! | PUT | `/api/courses/<code>/thumbnail`, `/entire_thumbnail` (body: a JPEG or the game's thumbnail block; signed in, the course's uploader) | an uploaded course's picture and whole-course map, as the game made them |
//! | POST | `/api/courses/<code>/play`, `/clear` | statistics |
//! | GET | `/api/health` | liveness: counts and the replay engine version |
//! | GET | `/api/pool?min=&max=&count=` | random courses by difficulty value (fail rate 0–100) |
//! | POST | `/api/accounts` (body `{"name"}`) | new account → `{account, token}` (token shown once) |
//! | GET | `/api/me` | the caller's account |
//! | GET | `/api/makers/<maker id>` | a maker's profile, courses, `super_world_id` and `upload_limit` |
//! | GET | `/api/makers/<maker id>/played`, `/liked` | the courses a player played (newest first: Course World and Endless) or liked |
//! | POST | `/api/courses/<code>/like`, `/boo` | one vote per account (replaces its earlier vote) |
//! | DELETE | `/api/courses/<code>/vote` | take the account's vote back |
//! | POST | `/api/courses/<code>/clear` (body `{"time_ms"}`, optional) | clear; first clear / record |
//! | POST | `/api/courses/<code>/replay` (body: replay, `mmk_modes::replay`) | clear proven by a replay: re-simulated, its time can set a verified record |
//! | GET | `/api/courses/<code>/replay` | the verified record's replay (for ghosts) |
//! | GET, POST | `/api/courses/<code>/comments` (POST body `{"text"}`, or SMM2's whole comment: `{"type": 0 drawing \| 1 text \| 2 stamp, "text", "stamp", "x", "y", "sub_area", "face", "clear_required", "cleared"}`) | comments (each with its `id`) |
//! | PUT, GET | `/api/comments/<id>/picture` (PUT: the poster) | a drawn comment's picture (the game's data) |
//! | DELETE | `/api/comments/<id>` | remove a comment (its poster, the course's uploader or the admin) |
//! | DELETE | `/api/courses/<code>` | remove (the uploader, or the admin) |
//! | POST | `/api/admin/ban/<account id>`, `/api/admin/unban/<id>` | moderation (admin token) |
//! | POST | `/api/endless/<easy\|normal\|expert\|super_expert>` (body `{"cleared"}`, or `{"ruleset": "hundred"}` for a completed 100 Mario run) | a run the client held itself; keeps the best (100 Mario: counts the completion) |
//! | GET | `/api/endless` | Endless: the four runs (`RunStatus`), best clears, lifetime skips, last difficulty |
//! | POST | `/api/endless/<d>/start` (body optional `{"ruleset": "smm2"\|"hundred"}`) | New Game on `<d>` (wipes that run): SMM2's rules (5 lives), or SMM1's 100 Mario Challenge (100 lives, done after 8 / 16 / 16 / 6 clears, SMM1's fail-rate bands, free skips) → `{difficulty, run}` |
//! | GET | `/api/endless/<d>/next?count=` | the run's next courses (≤ 10, by clear-rate band) → `{courses, widened, run}` |
//! | POST | `/api/endless/<d>/result` (body `Report`: `{code, result, pending, coins, score, frames_left, during_miss}`) | a course result → `{lives, clears, passed, game_over, run_complete, best}` (100 Mario: `best` = completed runs) |
//! | POST | `/api/endless/<d>/suspend`, `/finish` | quit (run kept) → `{run}`; game over → `{clears, best, new_high_score}` |
//! | GET | `/api/endless/<d>/history` | the run's last 48 courses |
//! | GET | `/api/rankings/<maker_points\|clears\|first_clears\|records\|endless_<difficulty>\|hundred_<difficulty>>?limit=` | leaderboards; `me`: the signed-in caller's row |
//! | GET | `/api/super_worlds?offset=&limit=&wait_ms=` | Super Worlds (`superworlds`): the pool, this server's uploads first, then the newest; an empty pool the source is asked for is waited for up to `wait_ms` |
//! | GET | `/api/super_worlds/<id>?wait_ms=` | a world: summary, `courses` (`code`, `data_id`, `name`, in course order), `ninjis`; one not held is fetched from the source, waited for up to `wait_ms` (404 meanwhile) |
//! | GET | `/api/super_worlds/<id>/map`, `/thumbnail` | the game's encrypted map blob (0x500 bytes); an uploaded world's thumbnail block (none for the source's) |
//! | POST | `/api/super_worlds?data_ids=&worlds=&planet_type=&version=` (body: the map blob, then optionally the thumbnail block) | upload the signed-in player's world (their own courses here, 1–40; replaces their earlier one) → the summary |
//! | DELETE | `/api/super_worlds/mine` | remove the player's uploaded world |
//! | GET, POST | `/api/super_worlds/<id>/progress`, POST `…/progress/reset` | the signed-in player's play record of a world (`status`: 2 progress to continue, 1 none; `clear`: 2 once the world is cleared; `counter_a`, `counter_b`, `score`, `world`, `x`, `y`, `extra`; `cleared`, `flags` as decimal strings) |
//! | GET | `/api/makers/<maker id>/super_worlds` | the worlds a player has a play record of, the latest first |
//!
//! The Endless Challenge routes (`endless` module) need a signed-in account; the server holds
//! the run as SMM2's does.
//!
//! Accounts (`accounts` module) are a name and a secret token sent as `Authorization: Bearer
//! <token>`; uploads, votes and comments from an account carry its name and maker ID. Anonymous
//! uploads stay possible (header `X-Maker`). The admin token comes from `Store::with_admin`.
//!
//! The difficulty value is 100 − clear rate once a course has been played, as SMM1's 100 Mario
//! bands use it. Courses are identified by SMM2-style codes (XXX-XXX-XXX).

pub mod limits;
pub mod ws;

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use mmk_formats::course_id::smm2_data_id_to_code;
use mmk_formats::smm2::Course;
use serde_json::{Value, json};

pub mod accounts;
pub mod endless;
pub mod lists;
pub mod superworlds;
pub mod upstream;

use accounts::Accounts;

/// Longest comment (characters).
pub const MAX_COMMENT: usize = 75;

/// Largest upload accepted (a course is 0x5BFC0 bytes raw).
pub const MAX_UPLOAD: usize = 0x60000;

/// The most courses a player may upload as the game shows it (My Info's "Courses uploaded", a maker's
/// `upload_limit`): SMM2's own last limit [inf]. Shown only; uploads are not refused at it.
pub const UPLOAD_LIMIT: usize = 100;

#[derive(Clone, Debug, PartialEq)]
pub struct Meta {
    pub code: String,
    pub data_id: u32,
    pub name: String,
    pub description: String,
    pub maker: String,
    pub style: String,
    pub theme: u8,
    pub uploaded: u64,
    pub plays: u64,
    pub clears: u64,
    pub attempts: u64,
    pub likes: u64,
    pub boos: u64,
    /// Uploader's account (0 anonymous) and maker ID.
    pub owner: u32,
    pub maker_id: String,
    /// Best clear time (ms, 0 none) and its holder's maker ID; the first clearer's.
    pub record_ms: u64,
    pub record_holder: String,
    /// The record was set by a replay the server re-simulated (its run is at `…/replay`).
    pub record_verified: bool,
    pub first_clear: String,
    /// Fetched from an upstream course source ([`upstream`]): its code and data ID are the
    /// source's, and local IDs never count it.
    pub upstream: bool,
    /// An edited copy of another maker's course: the original's code and maker name ("" for an
    /// original course). Set at upload (`POST /api/courses?based_on=<code>`), shown in listings.
    pub based_on: String,
    pub based_on_maker: String,
}

impl Meta {
    /// Difficulty value 0–100 (fail rate); unplayed courses count as 50.
    pub fn difficulty(&self) -> u8 {
        (self.clears * 100)
            .checked_div(self.attempts)
            .map_or(50, |rate| (100 - rate.min(100)) as u8)
    }

    pub fn to_json(&self) -> Value {
        json!({
            "code": self.code, "name": self.name, "description": self.description,
            "maker": self.maker, "style": self.style, "theme": self.theme,
            "uploaded": self.uploaded, "plays": self.plays, "clears": self.clears,
            "attempts": self.attempts, "likes": self.likes, "difficulty": self.difficulty(),
            "boos": self.boos, "maker_id": self.maker_id, "record_ms": self.record_ms,
            "record_holder": self.record_holder, "first_clear": self.first_clear,
            "record_verified": self.record_verified, "upstream": self.upstream,
            "based_on": self.based_on, "based_on_maker": self.based_on_maker,
        })
    }

    fn from_json(v: &Value) -> Option<Self> {
        let s = |k: &str| v[k].as_str().unwrap_or_default().to_string();
        let n = |k: &str| v[k].as_u64().unwrap_or(0);
        Some(Self {
            code: v["code"].as_str()?.to_string(),
            data_id: n("data_id") as u32,
            name: s("name"),
            description: s("description"),
            maker: s("maker"),
            style: s("style"),
            theme: n("theme") as u8,
            uploaded: n("uploaded"),
            plays: n("plays"),
            clears: n("clears"),
            attempts: n("attempts"),
            likes: n("likes"),
            boos: n("boos"),
            owner: n("owner") as u32,
            maker_id: s("maker_id"),
            record_ms: n("record_ms"),
            record_holder: s("record_holder"),
            record_verified: v["record_verified"].as_bool().unwrap_or(false),
            first_clear: s("first_clear"),
            upstream: v["upstream"].as_bool().unwrap_or(false),
            based_on: s("based_on"),
            based_on_maker: s("based_on_maker"),
        })
    }
}

/// The course store (thread-safe; one per server).
pub struct Store {
    dir: PathBuf,
    inner: Mutex<Inner>,
    /// Moderation token (None: no admin).
    admin: Option<String>,
    /// Records only from verified replays (typed-in times count as clears only).
    verified_records: bool,
    /// Told when a run's difficulty is short of courses ([`upstream`] fetches more); never
    /// blocks.
    wants: std::sync::OnceLock<Box<dyn Fn(mmk_modes::endless::Difficulty) + Send + Sync>>,
    /// Told when a client wants an upstream course's thumbnail the store doesn't hold (code,
    /// whole-course map?): [`upstream`] fetches it; never blocks.
    thumbs_wanted: std::sync::OnceLock<ThumbsWanted>,
    /// Course World's Popular and New from an upstream source ([`lists`]).
    lists: Mutex<lists::Lists>,
    /// Told when a page of an upstream list is asked for (set: the lists are served).
    lists_wanted: std::sync::OnceLock<lists::ListsWanted>,
    /// Told when a listed course the store doesn't hold is wanted (code).
    course_wanted: std::sync::OnceLock<lists::CourseWanted>,
    /// Course World's Super Worlds ([`superworlds`]), and who is told when a client asks for the
    /// pool or for a world the store doesn't hold.
    superworlds: Mutex<superworlds::SuperWorlds>,
    sw_pool_wanted: std::sync::OnceLock<superworlds::PoolWanted>,
    sw_world_wanted: std::sync::OnceLock<superworlds::WorldWanted>,
}

type ThumbsWanted = Box<dyn Fn(&str, bool) + Send + Sync>;

/// The largest thumbnail the game takes (its thumbnail block: the JPEG, then its size and signature).
pub const MAX_THUMBNAIL: usize = 0x1bf9c;

/// The JPEG in a thumbnail as a source gives it: a plain JPEG, or the game's thumbnail block (the JPEG,
/// zero padding, its size and signature at the end), up to the end-of-image marker.
pub fn thumbnail_jpeg(b: &[u8]) -> Option<&[u8]> {
    if !b.starts_with(&[0xFF, 0xD8]) {
        return None;
    }
    let area = &b[..b.len().min(MAX_THUMBNAIL)];
    let end = area.windows(2).rposition(|w| w == [0xFF, 0xD9])? + 2;
    Some(&b[..end])
}

struct Inner {
    courses: Vec<Meta>,
    by_code: HashMap<String, usize>,
    next_id: u32,
    rng: u64,
    accounts: Accounts,
    /// code → account id → +1 like / −1 boo.
    votes: HashMap<String, HashMap<u32, i8>>,
    /// code → comments.
    comments: HashMap<String, Vec<Comment>>,
    /// maker ID → codes of the courses they cleared.
    cleared: HashMap<String, Vec<String>>,
    /// maker ID → codes of the courses they played, newest first (at most [`PLAYED_KEPT`]).
    played: HashMap<String, Vec<String>>,
    /// Endless Challenge: difficulty → maker ID → best run (courses cleared).
    endless: HashMap<String, HashMap<String, u64>>,
    /// Endless Challenge runs: maker ID → the four run records (`endless.json`).
    endless_runs: HashMap<String, mmk_modes::endless::EndlessSave>,
}

/// Courses kept on a player's played list.
pub const PLAYED_KEPT: usize = 300;

impl Inner {
    pub(crate) fn note_played(&mut self, who: &str, code: &str) {
        let list = self.played.entry(who.to_string()).or_default();
        list.retain(|c| c != code);
        list.insert(0, code.to_string());
        list.truncate(PLAYED_KEPT);
    }
}

/// A comment on a course as SMM2 posts one: text, a stamp (one of the game's reaction pictures)
/// or a drawing (the game's own picture data, kept as `comments/<id>.bin`), placed where the
/// player was in the course.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Comment {
    /// Unique on the server.
    pub id: String,
    pub maker_id: String,
    pub name: String,
    /// SMM2's comment type: 0 drawing, 1 text, 2 stamp (JSON `type`).
    pub kind: u8,
    pub text: String,
    /// The stamp's number (type 2).
    pub stamp: u16,
    pub posted: u64,
    /// Where the player was: position in the course, and whether in its sub area.
    pub x: i32,
    pub y: i32,
    pub sub_area: bool,
    /// The face the poster picked for their Mii.
    pub face: u8,
    /// Shown only to players who cleared the course ("Require course clear to view").
    pub clear_required: bool,
    /// Posted after clearing the course.
    pub cleared: bool,
    /// A drawing's picture is held (`comments/<id>.bin`).
    pub picture: bool,
}

/// Comment types (SMM2's).
pub const COMMENT_DRAWING: u8 = 0;
pub const COMMENT_TEXT: u8 = 1;
pub const COMMENT_STAMP: u8 = 2;
/// The most a drawing's picture data may take (the game's two 0x1c200-byte buffers).
pub const MAX_COMMENT_PICTURE: usize = 2 * 0x1c200;

impl Comment {
    fn to_json(&self) -> Value {
        json!({ "id": self.id, "maker_id": self.maker_id, "name": self.name, "type": self.kind,
                "text": self.text, "stamp": self.stamp, "posted": self.posted, "x": self.x,
                "y": self.y, "sub_area": self.sub_area, "face": self.face,
                "clear_required": self.clear_required, "cleared": self.cleared,
                "picture": self.picture })
    }
    fn from_json(v: &Value) -> Option<Self> {
        Some(Self {
            id: v["id"].as_str().unwrap_or_default().to_string(),
            maker_id: v["maker_id"].as_str()?.to_string(),
            name: v["name"].as_str().unwrap_or_default().to_string(),
            posted: v["posted"].as_u64().unwrap_or(0),
            picture: v["picture"].as_bool().unwrap_or(false),
            ..Self::from_post(v)
        })
    }
    /// What a player posts (the server sets the id, the poster, the time and the picture).
    fn from_post(v: &Value) -> Self {
        let b = |k: &str| v[k].as_bool().unwrap_or(false);
        Self {
            kind: v["type"].as_u64().unwrap_or(COMMENT_TEXT as u64) as u8,
            text: v["text"].as_str().unwrap_or_default().to_string(),
            stamp: v["stamp"].as_u64().unwrap_or(0) as u16,
            x: v["x"].as_i64().unwrap_or(0) as i32,
            y: v["y"].as_i64().unwrap_or(0) as i32,
            sub_area: b("sub_area"),
            face: v["face"].as_u64().unwrap_or(0) as u8,
            clear_required: b("clear_required"),
            cleared: b("cleared"),
            ..Self::default()
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum StoreError {
    NotACourse(String),
    TooLarge,
    NotFound,
    Io(String),
    /// No (valid) account token, or the account may not do this.
    Forbidden,
    Bad(String),
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

impl Store {
    pub fn open(dir: impl AsRef<Path>) -> Result<Self, StoreError> {
        let dir = dir.as_ref().to_path_buf();
        fs::create_dir_all(dir.join("courses")).map_err(|e| StoreError::Io(e.to_string()))?;
        let courses: Vec<Meta> = fs::read(dir.join("index.json"))
            .ok()
            .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
            .and_then(|v| v["courses"].as_array().cloned())
            .map(|a| a.iter().filter_map(Meta::from_json).collect())
            .unwrap_or_default();
        let next_id = courses
            .iter()
            .filter(|m| !m.upstream)
            .map(|m| m.data_id)
            .max()
            .unwrap_or(0x10_0000)
            + 1;
        let by_code = courses
            .iter()
            .enumerate()
            .map(|(i, m)| (m.code.clone(), i))
            .collect();
        let read = |name: &str| {
            fs::read(dir.join(name))
                .ok()
                .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
                .unwrap_or(Value::Null)
        };
        let accounts = Accounts::from_json(&read("accounts.json"));
        let social = read("social.json");
        let votes = social["votes"]
            .as_object()
            .map(|o| {
                o.iter()
                    .map(|(code, v)| {
                        let m = v
                            .as_object()
                            .map(|m| {
                                m.iter()
                                    .filter_map(|(id, x)| {
                                        Some((id.parse().ok()?, x.as_i64()? as i8))
                                    })
                                    .collect()
                            })
                            .unwrap_or_default();
                        (code.clone(), m)
                    })
                    .collect()
            })
            .unwrap_or_default();
        let comments = social["comments"]
            .as_object()
            .map(|o| {
                o.iter()
                    .map(|(code, v)| {
                        let mut list: Vec<Comment> = v
                            .as_array()
                            .map(|a| a.iter().filter_map(Comment::from_json).collect())
                            .unwrap_or_default();
                        // (comments from before ids: one from their place)
                        for (i, c) in list.iter_mut().enumerate() {
                            if c.id.is_empty() {
                                c.id = format!("{code}-{}", i + 1);
                            }
                        }
                        (code.clone(), list)
                    })
                    .collect()
            })
            .unwrap_or_default();
        let codes_by_maker = |v: &Value| -> HashMap<String, Vec<String>> {
            v.as_object()
                .map(|o| {
                    o.iter()
                        .map(|(who, v)| {
                            let codes = v
                                .as_array()
                                .map(|a| {
                                    a.iter()
                                        .filter_map(|c| Some(c.as_str()?.to_string()))
                                        .collect()
                                })
                                .unwrap_or_default();
                            (who.clone(), codes)
                        })
                        .collect()
                })
                .unwrap_or_default()
        };
        let played = codes_by_maker(&social["played"]);
        let cleared = codes_by_maker(&social["cleared"]);
        let endless = social["endless"]
            .as_object()
            .map(|o| {
                o.iter()
                    .map(|(d, v)| {
                        let best = v
                            .as_object()
                            .map(|m| {
                                m.iter()
                                    .filter_map(|(who, n)| Some((who.clone(), n.as_u64()?)))
                                    .collect()
                            })
                            .unwrap_or_default();
                        (d.clone(), best)
                    })
                    .collect()
            })
            .unwrap_or_default();
        let endless_runs = endless::load(&dir);
        let (superworlds, world_courses) = superworlds::load(&dir);
        let mut lists = lists::Lists::default();
        lists.register_world_courses(world_courses);
        let mut played = played;
        for (who, save) in &endless_runs {
            played
                .entry(who.clone())
                .or_insert_with(|| endless::played_in_runs(save));
        }
        Ok(Self {
            dir,
            inner: Mutex::new(Inner {
                courses,
                by_code,
                next_id,
                rng: now() | 1,
                accounts,
                votes,
                comments,
                cleared,
                played,
                endless,
                endless_runs,
            }),
            admin: None,
            verified_records: false,
            wants: std::sync::OnceLock::new(),
            thumbs_wanted: std::sync::OnceLock::new(),
            lists: Mutex::new(lists),
            lists_wanted: std::sync::OnceLock::new(),
            course_wanted: std::sync::OnceLock::new(),
            superworlds: Mutex::new(superworlds),
            sw_pool_wanted: std::sync::OnceLock::new(),
            sw_world_wanted: std::sync::OnceLock::new(),
        })
    }

    /// Who to tell when a run's difficulty is short of courses (once per store).
    pub fn on_short(&self, f: impl Fn(mmk_modes::endless::Difficulty) + Send + Sync + 'static) {
        let _ = self.wants.set(Box::new(f));
    }

    pub(crate) fn short_of(&self, d: mmk_modes::endless::Difficulty) {
        if let Some(f) = self.wants.get() {
            f(d);
        }
    }

    /// Who to tell when a client wants an upstream course's thumbnail the store doesn't hold (once
    /// per store).
    pub fn on_thumbnail(&self, f: impl Fn(&str, bool) + Send + Sync + 'static) {
        let _ = self.thumbs_wanted.set(Box::new(f));
    }

    fn thumbnail_path(&self, code: &str, entire: bool) -> PathBuf {
        let kind = if entire { "-entire" } else { "" };
        self.dir.join(format!("thumbnails/{code}{kind}.jpg"))
    }

    /// Whether a course's thumbnail (`entire`: its whole-course map) is held.
    pub fn has_thumbnail(&self, code: &str, entire: bool) -> bool {
        self.thumbnail_path(code, entire).exists()
    }

    /// A course's thumbnail (JPEG; `entire`: the whole-course map). One the store doesn't hold, of
    /// an upstream course (held or listed), is asked for from the source (and kept), waiting up to
    /// `wait` for it.
    pub fn thumbnail(
        &self,
        code: &str,
        entire: bool,
        wait: std::time::Duration,
    ) -> Option<Vec<u8>> {
        let path = self.thumbnail_path(code, entire);
        if let Ok(b) = fs::read(&path) {
            return Some(b);
        }
        let upstream = match self.get(code) {
            Some(m) => m.upstream,
            None => self.listed(code)?.meta.upstream,
        };
        if !upstream {
            return None;
        }
        (self.thumbs_wanted.get()?)(code, entire);
        let t0 = std::time::Instant::now();
        while t0.elapsed() < wait {
            std::thread::sleep(std::time::Duration::from_millis(50).min(wait));
            if let Ok(b) = fs::read(&path) {
                return Some(b);
            }
        }
        None
    }

    /// Keep a course's thumbnail (a JPEG, or the game's thumbnail block).
    pub fn put_thumbnail(&self, code: &str, entire: bool, data: &[u8]) -> Result<(), StoreError> {
        if self.get(code).is_none() && self.listed(code).is_none() {
            return Err(StoreError::NotFound);
        }
        let jpeg = thumbnail_jpeg(data).ok_or_else(|| StoreError::Bad("not a JPEG".into()))?;
        let path = self.thumbnail_path(code, entire);
        let tmp = path.with_extension("tmp");
        fs::create_dir_all(self.dir.join("thumbnails"))
            .and_then(|_| fs::write(&tmp, jpeg))
            .and_then(|_| fs::rename(&tmp, &path))
            .map_err(|e| StoreError::Io(e.to_string()))
    }

    /// An uploaded course's thumbnail from its uploader (or the admin): the picture the game makes in the
    /// Course Maker, sent after the course (`entire`: its whole-course map).
    pub fn put_own_thumbnail(
        &self,
        code: &str,
        entire: bool,
        by: Option<u32>,
        admin: bool,
        data: &[u8],
    ) -> Result<(), StoreError> {
        {
            let g = self.inner.lock().unwrap();
            let i = *g.by_code.get(code).ok_or(StoreError::NotFound)?;
            let owner = g.courses[i].owner;
            if !(admin || (owner != 0 && Some(owner) == by)) {
                return Err(StoreError::Forbidden);
            }
        }
        if data.len() > 0x1c000 {
            return Err(StoreError::TooLarge);
        }
        self.put_thumbnail(code, entire, data)
    }

    /// Upstream courses whose thumbnails aren't held yet, (code, entire), newest courses first.
    pub fn missing_thumbnails(&self, limit: usize) -> Vec<(String, bool)> {
        let codes: Vec<String> = {
            let g = self.inner.lock().unwrap();
            g.courses
                .iter()
                .rev()
                .filter(|m| m.upstream)
                .map(|m| m.code.clone())
                .collect()
        };
        codes
            .into_iter()
            .flat_map(|c| [(c.clone(), false), (c, true)])
            .filter(|(c, e)| !self.has_thumbnail(c, *e))
            .take(limit)
            .collect()
    }

    /// `who` played the course `code` (Course World or a challenge run): first on their list.
    pub fn note_played(&self, who: &str, code: &str) -> Result<(), StoreError> {
        let mut g = self.inner.lock().unwrap();
        g.note_played(who, code);
        self.save_social(&g)
    }

    /// The courses `maker_id` played (newest first) or liked, of those held.
    pub fn courses_of_player(&self, maker_id: &str, liked: bool) -> Vec<Meta> {
        let g = self.inner.lock().unwrap();
        let codes: Vec<String> = if liked {
            let Some(a) = g.accounts.by_maker_id(maker_id) else {
                return Vec::new();
            };
            let mut v: Vec<String> = g
                .votes
                .iter()
                .filter(|(_, m)| m.get(&a.id) == Some(&1))
                .map(|(code, _)| code.clone())
                .collect();
            v.sort();
            v
        } else {
            g.played.get(maker_id).cloned().unwrap_or_default()
        };
        codes
            .iter()
            .filter_map(|c| g.by_code.get(c).map(|&i| g.courses[i].clone()))
            .collect()
    }

    /// Store a course from an upstream source under its own code (`meta.code`, the source's
    /// data ID and statistics); `data` is the plain course body. A code already held is kept.
    pub fn import(&self, data: &[u8], mut meta: Meta) -> Result<Meta, StoreError> {
        if data.len() > MAX_UPLOAD {
            return Err(StoreError::TooLarge);
        }
        let course = Course::parse(data).map_err(|e| StoreError::NotACourse(e.to_string()))?;
        let mut g = self.inner.lock().unwrap();
        if let Some(&i) = g.by_code.get(&meta.code) {
            return Ok(g.courses[i].clone());
        }
        meta.upstream = true;
        meta.owner = 0;
        meta.style = course.header.game_style.as_str_lossy();
        meta.theme = course.main.header.theme;
        if meta.name.is_empty() {
            meta.name = course.header.name.to_string_lossy();
        }
        let code = meta.code.clone();
        fs::write(self.dir.join(format!("courses/{code}.bin")), data)
            .map_err(|e| StoreError::Io(e.to_string()))?;
        let i = g.courses.len();
        g.courses.push(meta.clone());
        g.by_code.insert(code, i);
        self.save_index(&g)?;
        Ok(meta)
    }

    /// Courses held from an upstream source.
    pub fn upstream_count(&self) -> usize {
        let g = self.inner.lock().unwrap();
        g.courses.iter().filter(|m| m.upstream).count()
    }

    /// (courses, accounts) held.
    pub fn counts(&self) -> (usize, usize) {
        let g = self.inner.lock().unwrap();
        (g.courses.len(), g.accounts.list.len())
    }

    /// Enable moderation with this admin token.
    /// Accept record times only from replays the server re-simulates.
    pub fn with_verified_records(mut self, on: bool) -> Self {
        self.verified_records = on;
        self
    }

    pub fn with_admin(mut self, token: Option<String>) -> Self {
        self.admin = token.filter(|t| !t.is_empty());
        self
    }

    fn write_json(&self, name: &str, v: &Value) -> Result<(), StoreError> {
        let tmp = self.dir.join(format!("{name}.tmp"));
        fs::write(&tmp, serde_json::to_vec(v).unwrap())
            .and_then(|_| fs::rename(&tmp, self.dir.join(name)))
            .map_err(|e| StoreError::Io(e.to_string()))
    }

    fn save_social(&self, g: &Inner) -> Result<(), StoreError> {
        let votes: serde_json::Map<String, Value> = g
            .votes
            .iter()
            .map(|(code, m)| {
                let o: serde_json::Map<String, Value> =
                    m.iter().map(|(id, x)| (id.to_string(), json!(x))).collect();
                (code.clone(), Value::Object(o))
            })
            .collect();
        let comments: serde_json::Map<String, Value> = g
            .comments
            .iter()
            .map(|(code, list)| {
                (
                    code.clone(),
                    Value::Array(list.iter().map(Comment::to_json).collect()),
                )
            })
            .collect();
        self.write_json(
            "social.json",
            &json!({ "votes": votes, "comments": comments, "cleared": g.cleared,
                     "played": g.played, "endless": g.endless }),
        )
    }

    /// A new account: (public account JSON, token).
    pub fn register(&self, name: &str) -> Result<(Value, String), StoreError> {
        let mut g = self.inner.lock().unwrap();
        let (a, token) = g.accounts.register(name, now()).map_err(StoreError::Bad)?;
        self.write_json("accounts.json", &g.accounts.to_json())?;
        Ok((a.public_json(), token))
    }

    /// The account of a bearer token.
    pub fn account(&self, token: &str) -> Option<accounts::Account> {
        self.inner.lock().unwrap().accounts.by_token(token).cloned()
    }

    /// A maker's profile and courses.
    pub fn maker(&self, maker_id: &str) -> Option<Value> {
        let g = self.inner.lock().unwrap();
        let a = g.accounts.by_maker_id(maker_id)?;
        let courses: Vec<Value> = g
            .courses
            .iter()
            .filter(|m| m.owner == a.id)
            .map(Meta::to_json)
            .collect();
        let (maker_id, public) = (a.maker_id.clone(), a.public_json());
        drop(g);
        let world = self.own_super_world(&maker_id).unwrap_or_default();
        Some(
            json!({ "maker": public, "courses": courses, "super_world_id": world, "upload_limit": UPLOAD_LIMIT }),
        )
    }

    /// Like (+1) or boo (−1) as `account`; a second vote replaces the first.
    pub fn vote(&self, code: &str, account: u32, v: i8) -> Result<Meta, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let i = *g.by_code.get(code).ok_or(StoreError::NotFound)?;
        let old = g
            .votes
            .entry(code.to_string())
            .or_default()
            .insert(account, v);
        let m = &mut g.courses[i];
        match old {
            Some(1) => m.likes -= 1,
            Some(-1) => m.boos -= 1,
            _ => {}
        }
        if v > 0 {
            m.likes += 1;
        } else {
            m.boos += 1;
        }
        let out = m.clone();
        self.save_index(&g)?;
        self.save_social(&g)?;
        Ok(out)
    }

    /// Take back `account`'s like or boo on `code` (none: nothing changes).
    pub fn unvote(&self, code: &str, account: u32) -> Result<Meta, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let i = *g.by_code.get(code).ok_or(StoreError::NotFound)?;
        let old = g.votes.get_mut(code).and_then(|m| m.remove(&account));
        let m = &mut g.courses[i];
        match old {
            Some(1) => m.likes -= 1,
            Some(-1) => m.boos -= 1,
            _ => {}
        }
        let out = m.clone();
        if old.is_some() {
            self.save_index(&g)?;
            self.save_social(&g)?;
        }
        Ok(out)
    }

    /// Course objects as the game's course records carry them: `comments` (how many), and for a
    /// signed-in player their own relation to the course, `you` = `{"played", "cleared", "vote"}`
    /// (vote 1 like, −1 boo, 0 none). `v`: one course object or an array of them; objects
    /// without a `code` are left alone.
    pub fn annotate(&self, a: Option<&accounts::Account>, v: &mut Value) {
        let g = self.inner.lock().unwrap();
        let has = |m: &HashMap<String, Vec<String>>, who: &str, code: &str| {
            m.get(who).is_some_and(|l| l.iter().any(|c| c == code))
        };
        let mut one = |c: &mut Value| {
            let Some(code) = c["code"].as_str().map(str::to_string) else {
                return;
            };
            c["comments"] = json!(g.comments.get(&code).map_or(0, Vec::len));
            let Some(a) = a else { return };
            let vote = g
                .votes
                .get(&code)
                .and_then(|m| m.get(&a.id))
                .copied()
                .unwrap_or(0);
            let cleared = has(&g.cleared, &a.maker_id, &code);
            let played = cleared || vote != 0 || has(&g.played, &a.maker_id, &code);
            c["you"] = json!({ "played": played, "cleared": cleared, "vote": vote });
        };
        match v {
            Value::Array(list) => list.iter_mut().for_each(&mut one),
            Value::Object(_) => one(v),
            _ => {}
        }
    }

    /// A clear by `who` (maker ID, if signed in) in `time_ms` (0 unknown). The time can set the
    /// record unless the server takes records only from replays.
    pub fn clear(&self, code: &str, who: Option<&str>, time_ms: u64) -> Result<Meta, StoreError> {
        let time = if self.verified_records { 0 } else { time_ms };
        self.clear_inner(code, who, time, None)
    }

    /// A clear by `who` proven by a replay (`mmk_modes::replay`): the server plays it on the
    /// course and the clear time is the replay's own. Errors when it doesn't clear.
    pub fn clear_replay(&self, code: &str, who: &str, replay: &[u8]) -> Result<Meta, StoreError> {
        use mmk_modes::replay::{Replay, SIM_VERSION};
        let r = Replay::decode(replay).ok_or_else(|| StoreError::Bad("not a replay".into()))?;
        if r.sim_version != SIM_VERSION {
            return Err(StoreError::Bad(format!(
                "replay from engine version {} (this server plays {SIM_VERSION})",
                r.sim_version
            )));
        }
        let course = mmk_formats::smm2::Course::parse(&self.data(code)?)
            .map_err(|e| StoreError::Io(e.to_string()))?;
        let time = r
            .run(&course)
            .time_ms()
            .ok_or_else(|| StoreError::Bad("the replay doesn't clear the course".into()))?;
        self.clear_inner(code, Some(who), time, Some(replay))
    }

    /// The record holder's replay, when the record was verified.
    pub fn record_replay(&self, code: &str) -> Option<Vec<u8>> {
        self.get(code).filter(|m| m.record_verified)?;
        fs::read(self.dir.join(format!("replays/{code}.mmkr"))).ok()
    }

    fn clear_inner(
        &self,
        code: &str,
        who: Option<&str>,
        time_ms: u64,
        replay: Option<&[u8]>,
    ) -> Result<Meta, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let i = *g.by_code.get(code).ok_or(StoreError::NotFound)?;
        let m = &mut g.courses[i];
        m.clears += 1;
        if let Some(who) = who {
            let mine = g.cleared.entry(who.to_string()).or_default();
            if !mine.iter().any(|c| c == code) {
                mine.push(code.to_string());
            }
            let m = &mut g.courses[i];
            if m.first_clear.is_empty() {
                m.first_clear = who.to_string();
            }
            if time_ms > 0 && (m.record_ms == 0 || time_ms < m.record_ms) {
                m.record_ms = time_ms;
                m.record_holder = who.to_string();
                m.record_verified = replay.is_some();
                let path = self.dir.join(format!("replays/{code}.mmkr"));
                match replay {
                    Some(r) => fs::create_dir_all(self.dir.join("replays"))
                        .and_then(|_| fs::write(&path, r))
                        .map_err(|e| StoreError::Io(e.to_string()))?,
                    None => {
                        let _ = fs::remove_file(&path);
                    }
                }
            }
        }
        let out = g.courses[i].clone();
        self.save_index(&g)?;
        if who.is_some() {
            self.save_social(&g)?;
        }
        Ok(out)
    }

    pub fn comments(&self, code: &str) -> Vec<Comment> {
        let g = self.inner.lock().unwrap();
        g.comments.get(code).cloned().unwrap_or_default()
    }

    /// A new comment on `code` by `a` (`c`: what the game posted; its id, poster, time and
    /// picture are set here). Text is required for a text comment, a stamp for a stamp.
    pub fn comment(
        &self,
        code: &str,
        a: &accounts::Account,
        mut c: Comment,
    ) -> Result<Comment, StoreError> {
        c.text = c.text.trim().chars().take(MAX_COMMENT).collect();
        let bad = match c.kind {
            COMMENT_TEXT => c.text.is_empty(),
            COMMENT_STAMP | COMMENT_DRAWING => false,
            _ => true,
        };
        if bad || c.text.chars().any(|ch| ch.is_control() && ch != '\n') {
            return Err(StoreError::Bad("bad comment".into()));
        }
        if c.kind != COMMENT_TEXT {
            c.text.clear();
        }
        let mut g = self.inner.lock().unwrap();
        if !g.by_code.contains_key(code) {
            return Err(StoreError::NotFound);
        }
        g.rng ^= g.rng << 13;
        g.rng ^= g.rng >> 7;
        g.rng ^= g.rng << 17;
        let posted = now();
        c.id = format!("{posted:x}{:06x}", g.rng & 0xff_ffff);
        c.maker_id = a.maker_id.clone();
        c.name = a.name.clone();
        c.posted = posted;
        c.picture = false;
        g.comments
            .entry(code.to_string())
            .or_default()
            .push(c.clone());
        self.save_social(&g)?;
        Ok(c)
    }

    fn comment_picture_path(&self, id: &str) -> Option<PathBuf> {
        id.chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '-')
            .then(|| self.dir.join(format!("comments/{id}.bin")))
    }

    /// A drawing's picture (the game's data, as it posted it), from the comment's poster.
    pub fn put_comment_picture(&self, id: &str, by: &str, data: &[u8]) -> Result<(), StoreError> {
        if data.is_empty() || data.len() > MAX_COMMENT_PICTURE {
            return Err(StoreError::TooLarge);
        }
        let path = self.comment_picture_path(id).ok_or(StoreError::NotFound)?;
        let mut g = self.inner.lock().unwrap();
        let c = g
            .comments
            .values_mut()
            .flat_map(|l| l.iter_mut())
            .find(|c| c.id == id)
            .ok_or(StoreError::NotFound)?;
        if c.maker_id != by || c.kind != COMMENT_DRAWING {
            return Err(StoreError::Forbidden);
        }
        fs::create_dir_all(self.dir.join("comments"))
            .and_then(|_| fs::write(&path, data))
            .map_err(|e| StoreError::Io(e.to_string()))?;
        c.picture = true;
        self.save_social(&g)
    }

    /// A drawing's picture data.
    pub fn comment_picture(&self, id: &str) -> Option<Vec<u8>> {
        fs::read(self.comment_picture_path(id)?).ok()
    }

    /// Remove a comment (its poster, the course's uploader or the admin).
    pub fn delete_comment(
        &self,
        id: &str,
        by: Option<&accounts::Account>,
        admin: bool,
    ) -> Result<(), StoreError> {
        let mut g = self.inner.lock().unwrap();
        let (code, i) = g
            .comments
            .iter()
            .find_map(|(code, l)| Some((code.clone(), l.iter().position(|c| c.id == id)?)))
            .ok_or(StoreError::NotFound)?;
        let owner = g.by_code.get(&code).map(|&i| g.courses[i].owner);
        let list = g.comments.get_mut(&code).ok_or(StoreError::NotFound)?;
        let mine = by.is_some_and(|a| {
            a.maker_id == list[i].maker_id || owner.is_some_and(|o| o != 0 && o == a.id)
        });
        if !(admin || mine) {
            return Err(StoreError::Forbidden);
        }
        list.remove(i);
        if let Some(p) = self.comment_picture_path(id) {
            let _ = fs::remove_file(p);
        }
        self.save_social(&g)
    }

    /// Remove a course (its uploader or the admin).
    pub fn delete(&self, code: &str, by: Option<u32>, admin: bool) -> Result<(), StoreError> {
        let mut g = self.inner.lock().unwrap();
        let i = *g.by_code.get(code).ok_or(StoreError::NotFound)?;
        let owner = g.courses[i].owner;
        if !(admin || (owner != 0 && Some(owner) == by)) {
            return Err(StoreError::Forbidden);
        }
        g.courses.remove(i);
        g.by_code = g
            .courses
            .iter()
            .enumerate()
            .map(|(i, m)| (m.code.clone(), i))
            .collect();
        g.votes.remove(code);
        for c in g.comments.remove(code).unwrap_or_default() {
            if let Some(p) = self.comment_picture_path(&c.id) {
                let _ = fs::remove_file(p);
            }
        }
        let _ = fs::remove_file(self.dir.join(format!("courses/{code}.bin")));
        self.save_index(&g)?;
        self.save_social(&g)
    }

    pub fn set_banned(&self, id: u32, banned: bool) -> Result<(), StoreError> {
        let mut g = self.inner.lock().unwrap();
        if !g.accounts.set_banned(id, banned) {
            return Err(StoreError::NotFound);
        }
        self.write_json("accounts.json", &g.accounts.to_json())
    }

    pub fn is_admin(&self, token: &str) -> bool {
        // constant time, so response timing doesn't leak the token
        self.admin.as_deref().is_some_and(|a| {
            a.len() == token.len()
                && a.bytes()
                    .zip(token.bytes())
                    .fold(0u8, |d, (x, y)| d | (x ^ y))
                    == 0
        })
    }

    fn save_index(&self, inner: &Inner) -> Result<(), StoreError> {
        let list: Vec<Value> = inner
            .courses
            .iter()
            .map(|m| {
                let mut v = m.to_json();
                v["data_id"] = json!(m.data_id);
                v["owner"] = json!(m.owner);
                v
            })
            .collect();
        let tmp = self.dir.join("index.json.tmp");
        fs::write(
            &tmp,
            serde_json::to_vec(&json!({ "courses": list })).unwrap(),
        )
        .and_then(|_| fs::rename(&tmp, self.dir.join("index.json")))
        .map_err(|e| StoreError::Io(e.to_string()))
    }

    /// Validate and store a course; returns its metadata.
    pub fn upload(&self, data: &[u8], maker: &str) -> Result<Meta, StoreError> {
        self.upload_as(data, maker, None)
    }

    /// Upload as `account` (its name and maker ID are recorded), or anonymously as `maker`.
    pub fn upload_as(
        &self,
        data: &[u8],
        maker: &str,
        account: Option<&accounts::Account>,
    ) -> Result<Meta, StoreError> {
        self.upload_based_on(data, maker, account, None)
    }

    /// [`Store::upload_as`] for an edited copy of another course: `based_on` is the original's code
    /// and the maker name the uploader knows it by. A course this server holds is credited to its
    /// own maker; another source's (an upstream, another server) to the given name.
    pub fn upload_based_on(
        &self,
        data: &[u8],
        maker: &str,
        account: Option<&accounts::Account>,
        based_on: Option<(&str, &str)>,
    ) -> Result<Meta, StoreError> {
        if data.len() > MAX_UPLOAD {
            return Err(StoreError::TooLarge);
        }
        let based_on = match based_on {
            Some((code, _)) if !is_course_code(code) => {
                return Err(StoreError::Bad("based_on: not a course code".into()));
            }
            Some((code, name)) => Some((
                code.to_ascii_uppercase(),
                name.chars().take(32).collect::<String>(),
            )),
            None => None,
        };
        let course = Course::parse(data).map_err(|e| StoreError::NotACourse(e.to_string()))?;
        let mut g = self.inner.lock().unwrap();
        let (based_on, based_on_maker) = match based_on {
            Some((code, name)) => {
                let known = g.by_code.get(&code).map(|&i| g.courses[i].maker.clone());
                (code, known.unwrap_or(name))
            }
            None => Default::default(),
        };
        let data_id = g.next_id;
        g.next_id += 1;
        let code = smm2_data_id_to_code(data_id, false);
        let meta = Meta {
            code: code.clone(),
            data_id,
            name: course.header.name.to_string_lossy(),
            description: course.header.description.to_string_lossy(),
            maker: account
                .map(|a| a.name.clone())
                .unwrap_or_else(|| maker.chars().take(32).collect()),
            style: course.header.game_style.as_str_lossy(),
            theme: course.main.header.theme,
            uploaded: now(),
            plays: 0,
            clears: 0,
            attempts: 0,
            likes: 0,
            boos: 0,
            owner: account.map_or(0, |a| a.id),
            maker_id: account.map(|a| a.maker_id.clone()).unwrap_or_default(),
            record_ms: 0,
            record_holder: String::new(),
            record_verified: false,
            first_clear: String::new(),
            upstream: false,
            based_on,
            based_on_maker,
        };
        fs::write(self.dir.join(format!("courses/{code}.bin")), data)
            .map_err(|e| StoreError::Io(e.to_string()))?;
        let i = g.courses.len();
        g.courses.push(meta.clone());
        g.by_code.insert(code, i);
        self.save_index(&g)?;
        Ok(meta)
    }

    pub fn get(&self, code: &str) -> Option<Meta> {
        let g = self.inner.lock().unwrap();
        g.by_code.get(code).map(|&i| g.courses[i].clone())
    }

    pub fn data(&self, code: &str) -> Result<Vec<u8>, StoreError> {
        if self.get(code).is_none() {
            return Err(StoreError::NotFound);
        }
        fs::read(self.dir.join(format!("courses/{code}.bin")))
            .map_err(|e| StoreError::Io(e.to_string()))
    }

    /// An Endless Challenge run on `difficulty` that cleared `cleared` courses; the account's best
    /// on that difficulty is kept. Returns the best.
    pub fn endless(&self, difficulty: &str, who: &str, cleared: u64) -> Result<u64, StoreError> {
        if !ENDLESS.contains(&difficulty) {
            return Err(StoreError::NotFound);
        }
        let mut g = self.inner.lock().unwrap();
        let best = g
            .endless
            .entry(difficulty.to_string())
            .or_default()
            .entry(who.to_string())
            .or_insert(0);
        *best = (*best).max(cleared);
        let out = *best;
        self.save_social(&g)?;
        Ok(out)
    }

    /// A 100 Mario run on `difficulty` completed by a client that ran it itself; the account's
    /// completed runs on that difficulty.
    pub fn hundred_complete(&self, difficulty: &str, who: &str) -> Result<u64, StoreError> {
        if !ENDLESS.contains(&difficulty) {
            return Err(StoreError::NotFound);
        }
        let mut g = self.inner.lock().unwrap();
        let n = g
            .endless
            .entry(format!("hundred_{difficulty}"))
            .or_default()
            .entry(who.to_string())
            .or_insert(0);
        *n += 1;
        let out = *n;
        self.save_social(&g)?;
        Ok(out)
    }

    /// Leaderboard `kind`: maker_points (likes − boos on their courses [inf: SMM2 adds points
    /// for likes and takes them for boos; the exact weights weren't decoded]), clears (courses
    /// cleared), first_clears, records (world records held), endless_<d> (Endless best clears),
    /// hundred_<d> (completed 100 Mario runs). Banned accounts are left out.
    pub fn rankings(&self, kind: &str, limit: usize) -> Option<Vec<Value>> {
        let g = self.inner.lock().unwrap();
        let score = |a: &accounts::Account| -> i64 {
            let count =
                |f: &dyn Fn(&Meta) -> bool| g.courses.iter().filter(|m| f(m)).count() as i64;
            match kind {
                "maker_points" => g
                    .courses
                    .iter()
                    .filter(|m| m.owner == a.id)
                    .map(|m| m.likes as i64 - m.boos as i64)
                    .sum::<i64>()
                    .max(0),
                "clears" => g.cleared.get(&a.maker_id).map_or(0, |c| c.len() as i64),
                "first_clears" => count(&|m| m.first_clear == a.maker_id),
                "records" => count(&|m| m.record_holder == a.maker_id),
                k if k
                    .strip_prefix("endless_")
                    .is_some_and(|d| ENDLESS.contains(&d)) =>
                {
                    g.endless
                        .get(&k["endless_".len()..])
                        .and_then(|m| m.get(&a.maker_id))
                        .map_or(0, |&n| n as i64)
                }
                // completed 100 Mario runs
                k if k
                    .strip_prefix("hundred_")
                    .is_some_and(|d| ENDLESS.contains(&d)) =>
                {
                    g.endless
                        .get(k)
                        .and_then(|m| m.get(&a.maker_id))
                        .map_or(0, |&n| n as i64)
                }
                _ => -1,
            }
        };
        if score(&accounts::Account::default()) < 0 {
            return None;
        }
        let mut rows: Vec<(i64, &accounts::Account)> = g
            .accounts
            .list
            .iter()
            .filter(|a| !a.banned)
            .map(|a| (score(a), a))
            .filter(|(s, _)| *s > 0)
            .collect();
        rows.sort_by_key(|(s, a)| (std::cmp::Reverse(*s), a.id));
        Some(
            rows.iter()
                .take(limit)
                .enumerate()
                .map(|(i, (s, a))| {
                    json!({ "rank": i + 1, "score": s, "maker_id": a.maker_id, "name": a.name })
                })
                .collect(),
        )
    }

    /// `maker_id`'s row on leaderboard `kind` (None: not ranked, or no such leaderboard).
    pub fn rank_of(&self, kind: &str, maker_id: &str) -> Option<Value> {
        self.rankings(kind, usize::MAX)?
            .into_iter()
            .find(|r| r["maker_id"] == maker_id)
    }

    /// Record a play / anonymous clear.
    pub fn record(&self, code: &str, what: &str) -> Result<Meta, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let i = *g.by_code.get(code).ok_or(StoreError::NotFound)?;
        let m = &mut g.courses[i];
        match what {
            "play" => {
                m.plays += 1;
                m.attempts += 1;
            }
            "clear" => m.clears += 1,
            _ => return Err(StoreError::NotFound),
        }
        let out = m.clone();
        self.save_index(&g)?;
        Ok(out)
    }

    /// Search: text in name/maker/description, optional style, sort, paging.
    pub fn search(
        &self,
        q: &str,
        style: Option<&str>,
        sort: &str,
        offset: usize,
        limit: usize,
    ) -> Vec<Meta> {
        self.sorted(q, style, sort)
            .into_iter()
            .skip(offset)
            .take(limit.min(100))
            .collect()
    }

    /// All the courses a search finds, sorted.
    fn sorted(&self, q: &str, style: Option<&str>, sort: &str) -> Vec<Meta> {
        let g = self.inner.lock().unwrap();
        let q = q.to_lowercase();
        let mut v: Vec<Meta> = g
            .courses
            .iter()
            .filter(|m| style.is_none_or(|s| m.style == s))
            .filter(|m| {
                q.is_empty()
                    || m.name.to_lowercase().contains(&q)
                    || m.maker.to_lowercase().contains(&q)
                    || m.description.to_lowercase().contains(&q)
                    || m.code.eq_ignore_ascii_case(&q)
            })
            .cloned()
            .collect();
        match sort {
            "popular" | "hot" => v.sort_by_key(|m| std::cmp::Reverse((m.likes, m.plays))),
            "hard" => v.sort_by_key(|m| std::cmp::Reverse(m.difficulty())),
            _ => v.sort_by_key(|m| std::cmp::Reverse(m.uploaded)),
        }
        v
    }

    /// Up to `count` random courses whose difficulty value is in min..=max (100 Mario pools).
    pub fn pool(&self, min: u8, max: u8, count: usize) -> Vec<Meta> {
        let mut g = self.inner.lock().unwrap();
        let mut v: Vec<Meta> = g
            .courses
            .iter()
            .filter(|m| (min..=max).contains(&m.difficulty()))
            .cloned()
            .collect();
        for i in 0..v.len() {
            g.rng ^= g.rng << 13;
            g.rng ^= g.rng >> 7;
            g.rng ^= g.rng << 17;
            let j = i + (g.rng % (v.len() - i) as u64) as usize;
            v.swap(i, j);
        }
        v.truncate(count.min(100));
        v
    }
}

/// `XXX-XXX-XXX` (letters and digits).
fn is_course_code(code: &str) -> bool {
    let b = code.as_bytes();
    b.len() == 11
        && b.iter().enumerate().all(|(i, &c)| {
            if i == 3 || i == 7 {
                c == b'-'
            } else {
                c.is_ascii_alphanumeric()
            }
        })
}

fn query(url: &str) -> HashMap<String, String> {
    url.split_once('?')
        .map(|(_, q)| {
            q.split('&')
                .filter_map(|kv| kv.split_once('='))
                .map(|(k, v)| (k.to_string(), decode(v)))
                .collect()
        })
        .unwrap_or_default()
}

fn decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() => {
                if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                    out.push(v);
                    i += 2;
                }
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Response of one API request: status, content type, body.
pub fn handle(
    store: &Store,
    method: &str,
    url: &str,
    headers: &[(String, String)],
    body: &[u8],
) -> (u16, &'static str, Vec<u8>) {
    let path = url.split('?').next().unwrap_or("");
    let q = query(url);
    let json =
        |status: u16, v: Value| (status, "application/json", serde_json::to_vec(&v).unwrap());
    let err = |status: u16, msg: &str| json(status, json!({ "error": msg }));
    let parts: Vec<&str> = path.trim_matches('/').split('/').collect();
    let token = headers
        .iter()
        .find(|(k, _)| k.eq_ignore_ascii_case("authorization"))
        .and_then(|(_, v)| v.strip_prefix("Bearer "))
        .map(str::trim);
    let account = token.and_then(|t| store.account(t));
    let admin = token.is_some_and(|t| store.is_admin(t));
    let body_json = || serde_json::from_slice::<Value>(body).unwrap_or(Value::Null);
    let store_err = |e: StoreError| match e {
        StoreError::NotFound => err(404, "not found"),
        StoreError::Forbidden => err(403, "forbidden"),
        StoreError::Bad(m) | StoreError::NotACourse(m) => err(400, &m),
        StoreError::TooLarge => err(413, "too large"),
        StoreError::Io(m) => err(500, &m),
    };
    // signed-in actions need an account that isn't banned
    let signed_in = || account.as_ref().filter(|a| !a.banned);
    let who = signed_in().map(|a| a.maker_id.as_str());
    // course objects carry their comment count and the signed-in player's own relation to them
    let mark = |v: &mut Value| store.annotate(signed_in(), v);
    if let Some(r) = endless::route(store, method, &parts, &q, who, body) {
        return r;
    }
    if let Some(r) = superworlds::route(store, method, &parts, &q, signed_in(), body) {
        return r;
    }
    match (method, parts.as_slice()) {
        ("POST", ["api", "accounts"]) => {
            let name = body_json()["name"].as_str().unwrap_or("").to_string();
            match store.register(&name) {
                Ok((a, token)) => json(201, json!({ "account": a, "token": token })),
                Err(e) => store_err(e),
            }
        }
        ("GET", ["api", "me"]) => match &account {
            Some(a) => {
                let mut v = a.public_json();
                v["super_world_id"] = json!(store.own_super_world(&a.maker_id).unwrap_or_default());
                json(200, v)
            }
            None => err(401, "sign in"),
        },
        ("GET", ["api", "makers", id, list @ ("played" | "liked")]) => {
            let courses = store.courses_of_player(id, *list == "liked");
            let mut v = json!({ "courses": courses.iter().map(Meta::to_json).collect::<Vec<_>>() });
            mark(&mut v["courses"]);
            json(200, v)
        }
        ("GET", ["api", "makers", id]) => match store.maker(id) {
            Some(mut v) => {
                mark(&mut v["courses"]);
                json(200, v)
            }
            None => err(404, "no such maker"),
        },
        ("POST", ["api", "courses", code, vote @ ("like" | "boo")]) => match signed_in() {
            Some(a) => match store.vote(code, a.id, if *vote == "like" { 1 } else { -1 }) {
                Ok(m) => json(200, m.to_json()),
                Err(e) => store_err(e),
            },
            None => err(401, "sign in"),
        },
        ("DELETE", ["api", "courses", code, "vote"]) => match signed_in() {
            Some(a) => match store.unvote(code, a.id) {
                Ok(m) => json(200, m.to_json()),
                Err(e) => store_err(e),
            },
            None => err(401, "sign in"),
        },
        ("POST", ["api", "courses", code, "clear"]) => {
            let time = body_json()["time_ms"].as_u64().unwrap_or(0);
            let who = signed_in().map(|a| a.maker_id.clone());
            match store.clear(code, who.as_deref(), time) {
                Ok(m) => json(200, m.to_json()),
                Err(e) => store_err(e),
            }
        }
        ("POST", ["api", "courses", code, "replay"]) => match signed_in() {
            Some(a) => match store.clear_replay(code, &a.maker_id, body) {
                Ok(m) => json(200, m.to_json()),
                Err(e) => store_err(e),
            },
            None => err(401, "sign in"),
        },
        ("GET", ["api", "courses", code, "replay"]) => match store.record_replay(code) {
            Some(b) => (200, "application/octet-stream", b),
            None => err(404, "no verified record"),
        },
        ("GET", ["api", "courses", code, "comments"]) => json(
            200,
            json!({ "comments": store.comments(code).iter().map(Comment::to_json)
                .collect::<Vec<_>>() }),
        ),
        ("POST", ["api", "courses", code, "comments"]) => match signed_in() {
            Some(a) => match store.comment(code, a, Comment::from_post(&body_json())) {
                Ok(c) => json(201, c.to_json()),
                Err(e) => store_err(e),
            },
            None => err(401, "sign in"),
        },
        ("PUT", ["api", "comments", id, "picture"]) => match signed_in() {
            Some(a) => match store.put_comment_picture(id, &a.maker_id, body) {
                Ok(()) => json(200, json!({ "ok": true })),
                Err(e) => store_err(e),
            },
            None => err(401, "sign in"),
        },
        ("GET", ["api", "comments", id, "picture"]) => match store.comment_picture(id) {
            Some(b) => (200, "application/octet-stream", b),
            None => err(404, "no picture"),
        },
        ("DELETE", ["api", "comments", id]) => match store.delete_comment(id, signed_in(), admin) {
            Ok(()) => json(200, json!({ "deleted": id })),
            Err(e) => store_err(e),
        },
        (
            "PUT",
            [
                "api",
                "courses",
                code,
                kind @ ("thumbnail" | "entire_thumbnail"),
            ],
        ) => {
            let by = signed_in().map(|a| a.id);
            match store.put_own_thumbnail(code, *kind == "entire_thumbnail", by, admin, body) {
                Ok(()) => json(200, json!({ "ok": true })),
                Err(e) => store_err(e),
            }
        }
        ("DELETE", ["api", "courses", code]) => {
            match store.delete(code, account.as_ref().map(|a| a.id), admin) {
                Ok(()) => json(200, json!({ "deleted": code })),
                Err(e) => store_err(e),
            }
        }
        ("POST", ["api", "admin", what @ ("ban" | "unban"), id]) if admin => {
            match id
                .parse()
                .map_err(|_| StoreError::NotFound)
                .and_then(|id| store.set_banned(id, *what == "ban"))
            {
                Ok(()) => json(200, json!({ "ok": true })),
                Err(e) => store_err(e),
            }
        }
        ("POST", ["api", "endless", difficulty]) => match signed_in() {
            Some(a) => {
                let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
                let n = v["cleared"].as_u64().unwrap_or(0);
                let r = if v["ruleset"] == "hundred" {
                    store.hundred_complete(difficulty, &a.maker_id)
                } else {
                    store.endless(difficulty, &a.maker_id, n)
                };
                match r {
                    Ok(best) => json(200, json!({ "difficulty": difficulty, "best": best })),
                    Err(e) => store_err(e),
                }
            }
            None => err(401, "sign in"),
        },
        ("GET", ["api", "rankings", kind]) => {
            let limit = q
                .get("limit")
                .and_then(|l| l.parse().ok())
                .unwrap_or(100)
                .min(500);
            match store.rankings(kind, limit) {
                Some(rows) => {
                    // the caller's own row, wherever it is (null: not ranked)
                    let me = who.and_then(|w| store.rank_of(kind, w));
                    json(200, json!({ "kind": kind, "rankings": rows, "me": me }))
                }
                None => err(404, "no such ranking"),
            }
        }
        ("GET", ["api", "health"]) => {
            let (courses, accounts) = store.counts();
            json(
                200,
                json!({ "ok": true, "courses": courses, "accounts": accounts,
                        "sim_version": mmk_modes::replay::SIM_VERSION }),
            )
        }
        ("GET", ["api"]) | ("GET", [""]) => {
            json(200, json!({ "server": "mmk-server", "version": 1 }))
        }
        ("POST", ["api", "courses"]) => {
            let maker = headers
                .iter()
                .find(|(k, _)| k.eq_ignore_ascii_case("x-maker"))
                .map(|(_, v)| v.as_str())
                .unwrap_or("anonymous");
            if account.as_ref().is_some_and(|a| a.banned) {
                return err(403, "banned");
            }
            let based_on = q.get("based_on").map(|c| {
                (
                    c.as_str(),
                    q.get("based_on_maker").map(String::as_str).unwrap_or(""),
                )
            });
            match store.upload_based_on(body, maker, account.as_ref(), based_on) {
                Ok(m) => json(201, m.to_json()),
                Err(StoreError::TooLarge) => err(413, "too large"),
                Err(StoreError::NotACourse(e)) => err(400, &format!("not a course: {e}")),
                Err(StoreError::Bad(e)) => err(400, &e),
                Err(e) => err(500, &format!("{e:?}")),
            }
        }
        ("GET", ["api", "courses"]) => {
            let n = |k: &str, d: usize| q.get(k).and_then(|v| v.parse().ok()).unwrap_or(d);
            let text = q.get("q").map(String::as_str).unwrap_or("");
            let style = q.get("style").map(String::as_str);
            let sort = q.get("sort").map(String::as_str).unwrap_or("new");
            // Popular, Hot and New: the upstream lists, when the server has them
            let listed = lists::View::of_sort(sort)
                .filter(|_| text.is_empty())
                .and_then(|v| store.list_page(v, style, n("offset", 0), n("limit", 30)));
            let courses = listed.unwrap_or_else(|| {
                store
                    .search(text, style, sort, n("offset", 0), n("limit", 30))
                    .iter()
                    .map(Meta::to_json)
                    .collect()
            });
            let mut v = json!({ "courses": courses });
            mark(&mut v["courses"]);
            json(200, v)
        }
        ("GET", ["api", "courses", code]) => match store.course_json(code) {
            Some(mut v) => {
                mark(&mut v);
                json(200, v)
            }
            None => err(404, "no such course"),
        },
        ("GET", ["api", "courses", code, "data"]) => {
            let wait = q
                .get("wait_ms")
                .and_then(|v| v.parse().ok())
                .unwrap_or(DATA_WAIT_MS)
                .min(20_000);
            match store.data_wait(code, std::time::Duration::from_millis(wait)) {
                Ok(b) => (200, "application/octet-stream", b),
                Err(_) => err(404, "no such course"),
            }
        }
        (
            "GET",
            [
                "api",
                "courses",
                code,
                kind @ ("thumbnail" | "entire_thumbnail"),
            ],
        ) => {
            let wait = q
                .get("wait_ms")
                .and_then(|v| v.parse().ok())
                .unwrap_or(0u64)
                .min(20_000);
            let wait = std::time::Duration::from_millis(wait);
            match store.thumbnail(code, *kind == "entire_thumbnail", wait) {
                Some(b) => (200, "image/jpeg", b),
                None => err(404, "no thumbnail"),
            }
        }
        ("POST", ["api", "courses", code, what]) => match store.record(code, what) {
            Ok(m) => {
                if *what == "play"
                    && let Some(who) = who
                {
                    let _ = store.note_played(who, code);
                }
                json(200, m.to_json())
            }
            Err(_) => err(404, "no such course or action"),
        },
        ("GET", ["api", "pool"]) => {
            let n = |k: &str, d: u64| q.get(k).and_then(|v| v.parse().ok()).unwrap_or(d);
            let list = store.pool(
                n("min", 0) as u8,
                n("max", 100) as u8,
                n("count", 50) as usize,
            );
            json(
                200,
                json!({ "courses": list.iter().map(Meta::to_json).collect::<Vec<_>>() }),
            )
        }
        _ => err(404, "not found"),
    }
}

/// Serve the API on `addr` (e.g. "0.0.0.0:8640") until the process ends.
/// Start the online-play relay: UDP on `udp_addr` (native players) and WebSocket on `ws_addr`
/// (browsers), each in its own thread.
pub fn start_relay(
    udp_addr: &str,
    ws_addr: &str,
) -> Result<std::sync::Arc<mmk_netplay::relay::Hub>, String> {
    let udp = std::net::UdpSocket::bind(udp_addr).map_err(|e| format!("udp {udp_addr}: {e}"))?;
    let tcp = std::net::TcpListener::bind(ws_addr).map_err(|e| format!("tcp {ws_addr}: {e}"))?;
    let hub = mmk_netplay::relay::Hub::new(udp);
    let h = hub.clone();
    std::thread::spawn(move || h.run_udp());
    let h = hub.clone();
    std::thread::spawn(move || ws::serve(h, tcp));
    Ok(hub)
}

/// How long `GET /api/courses/<code>/data` waits for a listed course the server doesn't hold
/// (under the game's 8 s request timeout).
pub const DATA_WAIT_MS: u64 = 7000;

/// Endless Challenge difficulties.
pub const ENDLESS: &[&str] = &["easy", "normal", "expert", "super_expert"];

/// How `serve` runs.
#[derive(Clone, Copy, Debug)]
pub struct ServeOptions {
    /// Request threads (replay verification and big uploads don't hold up other requests).
    pub threads: usize,
    pub limits: limits::Limits,
    /// Take the client address from `X-Forwarded-For` (only behind a reverse proxy you run).
    pub trust_proxy: bool,
}

impl Default for ServeOptions {
    fn default() -> Self {
        Self {
            threads: 8,
            limits: limits::Limits::default(),
            trust_proxy: false,
        }
    }
}

/// CORS headers for browsers (any origin: the API is public and uses bearer tokens, not
/// cookies).
fn cors(
    resp: tiny_http::Response<std::io::Cursor<Vec<u8>>>,
) -> tiny_http::Response<std::io::Cursor<Vec<u8>>> {
    let h = |k: &str, v: &str| tiny_http::Header::from_bytes(k, v).unwrap();
    resp.with_header(h("Access-Control-Allow-Origin", "*"))
        .with_header(h(
            "Access-Control-Allow-Methods",
            "GET, POST, DELETE, OPTIONS",
        ))
        .with_header(h(
            "Access-Control-Allow-Headers",
            "Authorization, Content-Type, X-Maker",
        ))
        .with_header(h("Access-Control-Max-Age", "86400"))
}

pub fn serve(store: Store, addr: &str) -> Result<(), String> {
    serve_with(store, addr, ServeOptions::default())
}

pub fn serve_with(store: Store, addr: &str, opts: ServeOptions) -> Result<(), String> {
    serve_shared(std::sync::Arc::new(store), addr, opts)
}

/// [`serve_with`] for a store shared with something else (an upstream fetcher).
pub fn serve_shared(
    store: std::sync::Arc<Store>,
    addr: &str,
    opts: ServeOptions,
) -> Result<(), String> {
    let (_, workers) = start(store, addr, opts)?;
    for w in workers {
        let _ = w.join();
    }
    Ok(())
}

/// Serve on `addr` from worker threads and return the bound port at once (`addr` may end in
/// `:0`: a free port). The server runs as long as the process does (an app's own server).
pub fn serve_background(
    store: std::sync::Arc<Store>,
    addr: &str,
    opts: ServeOptions,
) -> Result<u16, String> {
    start(store, addr, opts).map(|(port, _)| port)
}

fn start(
    store: std::sync::Arc<Store>,
    addr: &str,
    opts: ServeOptions,
) -> Result<(u16, Vec<std::thread::JoinHandle<()>>), String> {
    let server = tiny_http::Server::http(addr).map_err(|e| e.to_string())?;
    let gate = limits::Gate::new(opts.limits);
    Ok(listen(server, opts.threads, move |req| {
        respond(&store, &gate, opts.trust_proxy, req)
    }))
}

/// Run `server` on `threads` worker threads that hand each request to `handle`; returns the
/// bound port and the workers (which run as long as the process does).
fn listen(
    server: tiny_http::Server,
    threads: usize,
    handle: impl Fn(tiny_http::Request) + Send + Sync + 'static,
) -> (u16, Vec<std::thread::JoinHandle<()>>) {
    let threads = threads.max(1);
    let addr = server.server_addr().to_ip();
    let listener = std::sync::Arc::new(Listener {
        addr,
        threads,
        current: std::sync::Mutex::new(std::sync::Arc::new(server)),
    });
    let handle = std::sync::Arc::new(handle);
    let workers = (0..threads)
        .map(|_| {
            let (listener, handle) = (listener.clone(), handle.clone());
            std::thread::spawn(move || {
                while let Some(req) = listener.recv() {
                    handle(req);
                }
            })
        })
        .collect();
    (addr.map_or(0, |a| a.port()), workers)
}

/// The server the workers take requests from. tiny_http stops accepting for good after one
/// failed accept (iOS reclaims a suspended app's sockets: after hours in the background the
/// app's own course server answered nothing until a restart), so the first worker to see the
/// failure listens on the same address again and moves the others onto the new server.
struct Listener {
    addr: Option<std::net::SocketAddr>,
    threads: usize,
    current: std::sync::Mutex<std::sync::Arc<tiny_http::Server>>,
}

impl Listener {
    /// The next request; `None` when the server can't listen again.
    fn recv(&self) -> Option<tiny_http::Request> {
        loop {
            let server = self.current.lock().unwrap().clone();
            match server.recv() {
                Ok(req) => return Some(req),
                Err(e) => {
                    if !self.relisten(&server, e) {
                        return None;
                    }
                }
            }
        }
    }

    /// Replace `failed` with a new server on its address (unless another worker has); false when
    /// there is no address to listen on.
    fn relisten(&self, failed: &std::sync::Arc<tiny_http::Server>, e: std::io::Error) -> bool {
        let mut current = self.current.lock().unwrap();
        if !std::sync::Arc::ptr_eq(&current, failed) {
            return true;
        }
        let Some(addr) = self.addr else {
            eprintln!("[server] accept: {e}; not a TCP address, stopping");
            return false;
        };
        eprintln!("[server] accept on {addr}: {e}; listening again");
        let mut wait = std::time::Duration::from_millis(100);
        let server = loop {
            match tiny_http::Server::http(addr) {
                Ok(s) => break s,
                Err(e) => {
                    eprintln!(
                        "[server] listen on {addr}: {e}; again in {} ms",
                        wait.as_millis()
                    );
                    std::thread::sleep(wait);
                    wait = (wait * 2).min(std::time::Duration::from_secs(5));
                }
            }
        };
        *current = std::sync::Arc::new(server);
        // the other workers wait in the failed server's queue: one unblock wakes one
        for _ in 1..self.threads {
            failed.unblock();
        }
        true
    }
}

fn respond(store: &Store, gate: &limits::Gate, trust_proxy: bool, mut req: tiny_http::Request) {
    let method = req.method().as_str().to_string();
    let url = req.url().to_string();
    let headers: Vec<(String, String)> = req
        .headers()
        .iter()
        .map(|h| {
            (
                h.field.as_str().as_str().to_string(),
                h.value.as_str().to_string(),
            )
        })
        .collect();
    let reply = |req: tiny_http::Request, status: u16, ctype: &str, out: Vec<u8>| {
        let resp = tiny_http::Response::from_data(out)
            .with_status_code(status)
            .with_header(tiny_http::Header::from_bytes("Content-Type", ctype).unwrap());
        let _ = req.respond(cors(resp));
    };
    if method == "OPTIONS" {
        // CORS preflight
        return reply(req, 204, "text/plain", Vec::new());
    }
    let forwarded = headers
        .iter()
        .find(|(k, _)| k.eq_ignore_ascii_case("x-forwarded-for"))
        .and_then(|(_, v)| v.split(',').next()?.trim().parse().ok());
    let ip = match (trust_proxy, forwarded) {
        (true, Some(ip)) => Some(ip),
        _ => req.remote_addr().map(|a| a.ip()),
    };
    let path = url.split('?').next().unwrap_or("");
    // the admin (seeding a server, moderating) isn't limited
    let admin = headers
        .iter()
        .find(|(k, _)| k.eq_ignore_ascii_case("authorization"))
        .and_then(|(_, v)| v.strip_prefix("Bearer "))
        .is_some_and(|t| store.is_admin(t.trim()));
    if !admin
        && let (Some(ip), Some(kind)) = (ip, limits::kind_of(&method, path))
        && !gate.allow(ip, kind, std::time::Instant::now())
    {
        let body = serde_json::to_vec(&json!({ "error": "too many requests, slow down" })).unwrap();
        return reply(req, 429, "application/json", body);
    }
    let mut body = Vec::new();
    let _ = std::io::Read::read_to_end(
        &mut std::io::Read::take(req.as_reader(), MAX_UPLOAD as u64 + 1),
        &mut body,
    );
    let (status, ctype, out) = handle(store, &method, &url, &headers, &body);
    reply(req, status, ctype, out);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!(
            "mmk-server-test-{name}-{}-{}",
            std::process::id(),
            now()
        ));
        let _ = fs::remove_dir_all(&d);
        d
    }

    /// A failed accept (iOS reclaiming a suspended app's listening socket) doesn't stop the
    /// server: it listens on the same port again.
    #[cfg(target_os = "linux")]
    #[test]
    fn listens_again_after_a_failed_accept() {
        use std::io::{Read, Write};
        use std::os::fd::AsRawFd;
        let tcp = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let fd = tcp.as_raw_fd();
        let server = tiny_http::Server::from_listener(tcp, None).unwrap();
        let (port, _) = listen(server, 3, |req| {
            let _ = req.respond(tiny_http::Response::from_string("ok"));
        });
        let get = || -> Option<String> {
            let mut s = std::net::TcpStream::connect(("127.0.0.1", port)).ok()?;
            s.set_read_timeout(Some(std::time::Duration::from_secs(2)))
                .ok()?;
            s.write_all(b"GET / HTTP/1.0\r\n\r\n").ok()?;
            let mut out = String::new();
            s.read_to_string(&mut out).ok()?;
            Some(out)
        };
        assert!(get().unwrap().ends_with("ok"));
        // fail the waiting accept (Linux wakes it for a shut-down listener; on macOS only closing
        // the fd under it does, and the listener's own close then aborts a debug build)
        unsafe extern "C" {
            fn shutdown(fd: i32, how: i32) -> i32;
        }
        assert_eq!(unsafe { shutdown(fd, 0) }, 0); // SHUT_RD
        let t0 = std::time::Instant::now();
        let mut answered = 0;
        while answered < 5 && t0.elapsed() < std::time::Duration::from_secs(5) {
            match get() {
                Some(r) if r.ends_with("ok") => answered += 1,
                _ => std::thread::sleep(std::time::Duration::from_millis(20)),
            }
        }
        assert_eq!(answered, 5, "the server listens again on port {port}");
    }

    #[test]
    fn upload_search_download_stats_and_pools() {
        let dir = tmp("courses");
        let store = Store::open(&dir).unwrap();
        let mut c = Course::blank("M3");
        c.header.name.set("Sky Fortress");
        let data = c.to_bytes().unwrap();
        let (st, _, body) = handle(
            &store,
            "POST",
            "/api/courses",
            &[("X-Maker".into(), "ada".into())],
            &data,
        );
        assert_eq!(st, 201);
        let meta: Value = serde_json::from_slice(&body).unwrap();
        let code = meta["code"].as_str().unwrap().to_string();
        assert_eq!(code.len(), 11, "XXX-XXX-XXX");
        assert_eq!(meta["maker"], "ada");
        // not a course
        assert_eq!(handle(&store, "POST", "/api/courses", &[], b"hello").0, 400);
        // search by text and style
        let (_, _, b) = handle(&store, "GET", "/api/courses?q=sky+fort&style=M3", &[], &[]);
        let v: Value = serde_json::from_slice(&b).unwrap();
        assert_eq!(v["courses"].as_array().unwrap().len(), 1);
        // data round trip
        let (st, ct, b) = handle(
            &store,
            "GET",
            &format!("/api/courses/{code}/data"),
            &[],
            &[],
        );
        assert_eq!((st, ct), (200, "application/octet-stream"));
        assert_eq!(b, data);
        // stats → difficulty: 4 plays, 1 clear = 75
        for _ in 0..4 {
            handle(
                &store,
                "POST",
                &format!("/api/courses/{code}/play"),
                &[],
                &[],
            );
        }
        let (_, _, b) = handle(
            &store,
            "POST",
            &format!("/api/courses/{code}/clear"),
            &[],
            &[],
        );
        let m: Value = serde_json::from_slice(&b).unwrap();
        assert_eq!(m["difficulty"], 75);
        let (_, _, b) = handle(&store, "GET", "/api/pool?min=75&max=95", &[], &[]);
        let v: Value = serde_json::from_slice(&b).unwrap();
        assert_eq!(v["courses"].as_array().unwrap().len(), 1);
        // persisted
        let again = Store::open(&dir).unwrap();
        assert_eq!(again.get(&code).unwrap().clears, 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn edited_copies_credit_the_original() {
        let dir = tmp("based_on");
        let store = Store::open(&dir).unwrap();
        let call = |method: &str, url: &str, token: Option<&str>, body: &[u8]| {
            let headers: Vec<(String, String)> = token
                .map(|t| vec![("Authorization".into(), format!("Bearer {t}"))])
                .unwrap_or_default();
            let (st, _, b) = handle(&store, method, url, &headers, body);
            (
                st,
                serde_json::from_slice::<Value>(&b).unwrap_or(Value::Null),
            )
        };
        let data = Course::blank("M1").to_bytes().unwrap();
        let (_, ada) = call("POST", "/api/accounts", None, br#"{"name":"Ada"}"#);
        let (_, bob) = call("POST", "/api/accounts", None, br#"{"name":"Bob"}"#);
        let (ada, bob) = (
            ada["token"].as_str().unwrap(),
            bob["token"].as_str().unwrap(),
        );
        let (st, orig) = call("POST", "/api/courses", Some(ada), &data);
        assert_eq!(st, 201);
        assert_eq!(orig["based_on"], "");
        let code = orig["code"].as_str().unwrap().to_string();
        // a copy of a course held here: credited to its own maker, whatever the client says
        let url = format!(
            "/api/courses?based_on={}&based_on_maker=Someone",
            code.to_lowercase()
        );
        let (st, copy) = call("POST", &url, Some(bob), &data);
        assert_eq!(st, 201);
        assert_eq!(
            (copy["maker"].as_str(), copy["based_on"].as_str()),
            (Some("Bob"), Some(code.as_str()))
        );
        assert_eq!(copy["based_on_maker"], "Ada");
        assert_ne!(copy["code"], orig["code"]);
        // a course from elsewhere: the given name; a bad code is refused
        let (_, other) = call(
            "POST",
            "/api/courses?based_on=ABC-DEF-GHJ&based_on_maker=Cy",
            Some(bob),
            &data,
        );
        assert_eq!(
            (other["based_on"].as_str(), other["based_on_maker"].as_str()),
            (Some("ABC-DEF-GHJ"), Some("Cy"))
        );
        assert_eq!(
            call("POST", "/api/courses?based_on=nope", Some(bob), &data).0,
            400
        );
        // kept across a restart and in the listings
        let again = Store::open(&dir).unwrap();
        let copy_code = copy["code"].as_str().unwrap();
        assert_eq!(again.get(copy_code).unwrap().based_on_maker, "Ada");
        let (_, list) = call("GET", "/api/courses?limit=10", None, b"");
        let row = list["courses"]
            .as_array()
            .unwrap()
            .iter()
            .find(|c| c["code"] == copy_code)
            .cloned();
        assert_eq!(row.unwrap()["based_on"], code.as_str());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn played_and_liked_lists_per_player() {
        let dir = tmp("played");
        let store = Store::open(&dir).unwrap();
        let call = |method: &str, url: &str, token: Option<&str>| {
            let headers: Vec<(String, String)> = token
                .map(|t| vec![("Authorization".into(), format!("Bearer {t}"))])
                .unwrap_or_default();
            let (st, _, b) = handle(&store, method, url, &headers, b"{}");
            (
                st,
                serde_json::from_slice::<Value>(&b).unwrap_or(Value::Null),
            )
        };
        let (ada, token) = store.register("Ada").unwrap();
        let me = ada["maker_id"].as_str().unwrap().to_string();
        let codes: Vec<String> = ["M1", "M3", "WU"]
            .iter()
            .map(|st| {
                store
                    .upload(&Course::blank(st).to_bytes().unwrap(), "x")
                    .unwrap()
                    .code
            })
            .collect();
        for c in [&codes[0], &codes[1], &codes[0]] {
            assert_eq!(
                call("POST", &format!("/api/courses/{c}/play"), Some(&token)).0,
                200
            );
        }
        call("POST", &format!("/api/courses/{}/play", codes[2]), None); // (anonymous: no list)
        call(
            "POST",
            &format!("/api/courses/{}/like", codes[1]),
            Some(&token),
        );
        let list = |kind: &str| -> Vec<String> {
            call("GET", &format!("/api/makers/{me}/{kind}"), None).1["courses"]
                .as_array()
                .unwrap()
                .iter()
                .map(|m| m["code"].as_str().unwrap().to_string())
                .collect()
        };
        assert_eq!(
            list("played"),
            vec![codes[0].clone(), codes[1].clone()],
            "newest first, once each"
        );
        assert_eq!(list("liked"), vec![codes[1].clone()]);
        // each course object tells the signed-in player their own relation to it
        let you = |c: &str, t: Option<&str>| {
            call("GET", &format!("/api/courses/{c}"), t).1["you"].clone()
        };
        assert_eq!(
            you(&codes[0], Some(&token)),
            json!({"played": true, "cleared": false, "vote": 0})
        );
        assert_eq!(
            you(&codes[1], Some(&token)),
            json!({"played": true, "cleared": false, "vote": 1})
        );
        assert_eq!(
            you(&codes[2], Some(&token)),
            json!({"played": false, "cleared": false, "vote": 0})
        );
        assert_eq!(you(&codes[1], None), Value::Null, "only signed in");
        call(
            "POST",
            &format!("/api/courses/{}/clear", codes[2]),
            Some(&token),
        );
        assert_eq!(you(&codes[2], Some(&token))["cleared"], true);
        let (_, all) = call("GET", "/api/courses?limit=10", Some(&token));
        assert!(
            all["courses"]
                .as_array()
                .unwrap()
                .iter()
                .all(|c| c["you"].is_object())
        );
        let (_, mine) = call("GET", &format!("/api/makers/{me}/liked"), Some(&token));
        assert_eq!(mine["courses"][0]["you"]["vote"], 1);
        // a vote taken back
        let (st, m) = call(
            "DELETE",
            &format!("/api/courses/{}/vote", codes[1]),
            Some(&token),
        );
        assert_eq!((st, m["likes"].as_u64()), (200, Some(0)));
        assert_eq!(you(&codes[1], Some(&token))["vote"], 0);
        assert_eq!(list("liked"), Vec::<String>::new());
        call(
            "POST",
            &format!("/api/courses/{}/like", codes[1]),
            Some(&token),
        );
        drop(store);
        let store = Store::open(&dir).unwrap();
        assert_eq!(
            store.courses_of_player(&me, false).len(),
            2,
            "kept across restarts"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn thumbnails_held_or_from_the_upstream() {
        let dir = tmp("thumbs");
        let store = Store::open(&dir).unwrap();
        let jpeg = [0xFF, 0xD8, 1, 2, 3, 0xFF, 0xD9];
        // the game's block: the JPEG, padding, its size and signature at the end
        let mut block = vec![0u8; 0x1c000];
        block[..jpeg.len()].copy_from_slice(&jpeg);
        block[MAX_THUMBNAIL..MAX_THUMBNAIL + 4]
            .copy_from_slice(&(MAX_THUMBNAIL as u32).to_le_bytes());
        block[MAX_THUMBNAIL + 4..MAX_THUMBNAIL + 8].copy_from_slice(&[0xFF, 0xD9, 0xFF, 0xD9]);
        assert_eq!(thumbnail_jpeg(&block), Some(&jpeg[..]));
        assert_eq!(thumbnail_jpeg(b"<html>"), None);
        let own = store
            .upload(&Course::blank("M1").to_bytes().unwrap(), "x")
            .unwrap();
        let mut up = own.clone();
        up.code = "ABC-DEF-GHJ".into();
        store
            .import(&Course::blank("M3").to_bytes().unwrap(), up)
            .unwrap();
        let asked = std::sync::Arc::new(Mutex::new(Vec::new()));
        let a = asked.clone();
        store.on_thumbnail(move |code, entire| a.lock().unwrap().push((code.to_string(), entire)));
        assert_eq!(
            store.missing_thumbnails(10),
            vec![("ABC-DEF-GHJ".into(), false), ("ABC-DEF-GHJ".into(), true)]
        );
        let get = |code: &str, kind: &str| {
            handle(
                &store,
                "GET",
                &format!("/api/courses/{code}/{kind}"),
                &[],
                b"",
            )
        };
        assert_eq!(
            get("ABC-DEF-GHJ", "thumbnail").0,
            404,
            "not held yet: asked for"
        );
        assert_eq!(
            get(&own.code, "thumbnail").0,
            404,
            "not an upstream course: nothing to ask"
        );
        assert_eq!(
            *asked.lock().unwrap(),
            vec![("ABC-DEF-GHJ".to_string(), false)]
        );
        store.put_thumbnail("ABC-DEF-GHJ", false, &block).unwrap(); // (the fetcher's answer)
        let (st, ty, body) = get("ABC-DEF-GHJ", "thumbnail");
        assert_eq!(
            (st, ty, body.as_slice()),
            (200, "image/jpeg", &jpeg[..]),
            "kept as its JPEG"
        );
        assert_eq!(asked.lock().unwrap().len(), 1, "held: not asked again");
        // a client may wait for it
        let t0 = std::time::Instant::now();
        assert_eq!(get("ABC-DEF-GHJ", "entire_thumbnail?wait_ms=100").0, 404);
        assert!(t0.elapsed() >= std::time::Duration::from_millis(100));
        store.put_thumbnail(&own.code, true, &jpeg).unwrap();
        assert_eq!(get(&own.code, "entire_thumbnail").2, jpeg);
        assert_eq!(
            store.missing_thumbnails(10),
            vec![("ABC-DEF-GHJ".into(), true)]
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_uploaded_course_gets_its_pictures_from_its_uploader() {
        let dir = tmp("own-thumbs");
        let store = Store::open(&dir).unwrap();
        let call = |method: &str, url: &str, token: Option<&str>, body: &[u8]| {
            let headers: Vec<(String, String)> = token
                .map(|t| vec![("Authorization".into(), format!("Bearer {t}"))])
                .unwrap_or_default();
            let (st, _, b) = handle(&store, method, url, &headers, body);
            (st, b)
        };
        let token = |name: &str| {
            let (_, b) = call(
                "POST",
                "/api/accounts",
                None,
                format!(r#"{{"name":"{name}"}}"#).as_bytes(),
            );
            serde_json::from_slice::<Value>(&b).unwrap()["token"]
                .as_str()
                .unwrap()
                .to_string()
        };
        let (ada, bob) = (token("Ada"), token("Bob"));
        let (_, m) = call(
            "POST",
            "/api/courses",
            Some(&ada),
            &Course::blank("M1").to_bytes().unwrap(),
        );
        let code = serde_json::from_slice::<Value>(&m).unwrap()["code"]
            .as_str()
            .unwrap()
            .to_string();
        let jpeg = [0xFF, 0xD8, 9, 8, 7, 0xFF, 0xD9];
        let url = |kind: &str| format!("/api/courses/{code}/{kind}");
        assert_eq!(call("GET", &url("thumbnail"), None, b"").0, 404, "none yet");
        assert_eq!(
            call("PUT", &url("thumbnail"), None, &jpeg).0,
            403,
            "signed in only"
        );
        assert_eq!(
            call("PUT", &url("thumbnail"), Some(&bob), &jpeg).0,
            403,
            "the uploader only"
        );
        assert_eq!(
            call("PUT", &url("thumbnail"), Some(&ada), b"<html>").0,
            400,
            "a JPEG only"
        );
        assert_eq!(call("PUT", &url("thumbnail"), Some(&ada), &jpeg).0, 200);
        assert_eq!(
            call("PUT", &url("entire_thumbnail"), Some(&ada), &jpeg).0,
            200
        );
        assert_eq!(
            call("GET", &url("thumbnail"), None, b""),
            (200, jpeg.to_vec())
        );
        assert_eq!(
            call("GET", &url("entire_thumbnail"), None, b""),
            (200, jpeg.to_vec())
        );
        assert_eq!(
            call(
                "PUT",
                "/api/courses/ABC-DEF-GHJ/thumbnail",
                Some(&ada),
                &jpeg
            )
            .0,
            404
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn accounts_votes_records_comments_and_moderation() {
        let dir = tmp("accounts");
        let store = Store::open(&dir).unwrap().with_admin(Some("root".into()));
        let call = |method: &str, url: &str, token: Option<&str>, body: &[u8]| {
            let headers: Vec<(String, String)> = token
                .map(|t| vec![("Authorization".into(), format!("Bearer {t}"))])
                .unwrap_or_default();
            let (st, _, b) = handle(&store, method, url, &headers, body);
            (
                st,
                serde_json::from_slice::<Value>(&b).unwrap_or(Value::Null),
            )
        };
        let (st, v) = call("POST", "/api/accounts", None, br#"{"name":"Ada"}"#);
        assert_eq!(st, 201);
        let token = v["token"].as_str().unwrap().to_string();
        let maker_id = v["account"]["maker_id"].as_str().unwrap().to_string();
        let (_, me) = call("GET", "/api/me", Some(&token), b"");
        assert_eq!(me["name"], "Ada");
        // upload as the account
        let data = Course::blank("M1").to_bytes().unwrap();
        let (st, m) = call("POST", "/api/courses", Some(&token), &data);
        assert_eq!(st, 201);
        assert_eq!(
            (m["maker"].as_str(), m["maker_id"].as_str()),
            (Some("Ada"), Some(maker_id.as_str()))
        );
        let code = m["code"].as_str().unwrap().to_string();
        // votes: one per account, a boo replaces the like
        assert_eq!(
            call("POST", &format!("/api/courses/{code}/like"), None, b"").0,
            401
        );
        call(
            "POST",
            &format!("/api/courses/{code}/like"),
            Some(&token),
            b"",
        );
        let (_, m) = call(
            "POST",
            &format!("/api/courses/{code}/like"),
            Some(&token),
            b"",
        );
        assert_eq!(m["likes"], 1);
        let (_, m) = call(
            "POST",
            &format!("/api/courses/{code}/boo"),
            Some(&token),
            b"",
        );
        assert_eq!(
            (m["likes"].as_u64(), m["boos"].as_u64()),
            (Some(0), Some(1))
        );
        // clears: first clear and record
        let (_, m) = call(
            "POST",
            &format!("/api/courses/{code}/clear"),
            Some(&token),
            br#"{"time_ms":51000}"#,
        );
        assert_eq!(
            (m["first_clear"].as_str(), m["record_ms"].as_u64()),
            (Some(maker_id.as_str()), Some(51000))
        );
        let (_, m) = call(
            "POST",
            &format!("/api/courses/{code}/clear"),
            Some(&token),
            br#"{"time_ms":60000}"#,
        );
        assert_eq!(m["record_ms"], 51000, "a slower clear keeps the record");
        // comments
        call(
            "POST",
            &format!("/api/courses/{code}/comments"),
            Some(&token),
            br#"{"text":"nice jumps"}"#,
        );
        let (_, c) = call("GET", &format!("/api/courses/{code}/comments"), None, b"");
        assert_eq!(c["comments"][0]["text"], "nice jumps");
        assert_eq!(c["comments"][0]["type"], COMMENT_TEXT);
        // SMM2's whole comment: a stamp and a drawing with its picture; counted on the course
        let (st, stamp) = call(
            "POST",
            &format!("/api/courses/{code}/comments"),
            Some(&token),
            br#"{"type":2,"stamp":5,"x":432,"y":48,"sub_area":true,"face":3,"clear_required":true,"cleared":true}"#,
        );
        assert_eq!(st, 201);
        assert_eq!(
            (
                &stamp["stamp"],
                &stamp["x"],
                &stamp["face"],
                &stamp["sub_area"],
                &stamp["clear_required"]
            ),
            (
                &json!(5),
                &json!(432),
                &json!(3),
                &json!(true),
                &json!(true)
            )
        );
        let (_, drawn) = call(
            "POST",
            &format!("/api/courses/{code}/comments"),
            Some(&token),
            br#"{"type":0}"#,
        );
        let drawn_id = drawn["id"].as_str().unwrap().to_string();
        let pic = format!("/api/comments/{drawn_id}/picture");
        assert_eq!(call("PUT", &pic, None, b"pixels").0, 401);
        assert_eq!(call("PUT", &pic, Some(&token), b"pixels").0, 200);
        let (st, _, b) = handle(&store, "GET", &pic, &[], b"");
        assert_eq!((st, b.as_slice()), (200, b"pixels".as_slice()));
        let (_, m) = call("GET", &format!("/api/courses/{code}"), None, b"");
        assert_eq!(m["comments"], 3);
        // removed by its poster only
        let del = format!("/api/comments/{drawn_id}");
        assert_eq!(call("DELETE", &del, None, b"").0, 403);
        assert_eq!(call("DELETE", &del, Some(&token), b"").0, 200);
        assert_eq!(handle(&store, "GET", &pic, &[], b"").0, 404);
        let (_, c) = call("GET", &format!("/api/courses/{code}/comments"), None, b"");
        assert_eq!(c["comments"].as_array().unwrap().len(), 2);
        // profile
        let (_, p) = call("GET", &format!("/api/makers/{maker_id}"), None, b"");
        assert_eq!(p["courses"][0]["code"].as_str(), Some(code.as_str()));
        assert_eq!(p["upload_limit"], UPLOAD_LIMIT);
        // leaderboards (the course was booed, so no maker points)
        for kind in ["clears", "first_clears", "records"] {
            let (st, r) = call("GET", &format!("/api/rankings/{kind}"), None, b"");
            assert_eq!(st, 200);
            assert_eq!(
                r["rankings"][0]["maker_id"].as_str(),
                Some(maker_id.as_str())
            );
            assert_eq!(r["rankings"][0]["score"], 1, "{kind}");
        }
        assert_eq!(
            call("POST", "/api/endless/expert", None, br#"{"cleared":3}"#).0,
            401
        );
        for n in [7, 4] {
            let (st, e) = call(
                "POST",
                "/api/endless/expert",
                Some(&token),
                format!(r#"{{"cleared":{n}}}"#).as_bytes(),
            );
            assert_eq!((st, e["best"].as_u64()), (200, Some(7)));
        }
        let (_, r) = call("GET", "/api/rankings/endless_expert", None, b"");
        assert_eq!(r["rankings"][0]["score"], 7);
        assert_eq!(
            call("POST", "/api/endless/silly", Some(&token), b"{}").0,
            404
        );
        let (_, r) = call("GET", "/api/rankings/maker_points", None, b"");
        assert_eq!(r["rankings"].as_array().map(Vec::len), Some(0));
        assert_eq!(call("GET", "/api/rankings/nope", None, b"").0, 404);
        // persisted across a restart
        let store2 = Store::open(&dir).unwrap();
        assert!(
            store2.account(&token).is_some(),
            "{:?}",
            fs::read_to_string(dir.join("accounts.json"))
        );
        assert_eq!(store2.comments(&code).len(), 2);
        assert_eq!(store2.rankings("clears", 10).unwrap().len(), 1);
        // moderation: a ban blocks votes; the admin can delete
        let id = me["id"].as_u64().unwrap();
        assert_eq!(
            call("POST", &format!("/api/admin/ban/{id}"), Some(&token), b"").0,
            404
        );
        assert_eq!(
            call("POST", &format!("/api/admin/ban/{id}"), Some("root"), b"").0,
            200
        );
        assert_eq!(
            call(
                "POST",
                &format!("/api/courses/{code}/like"),
                Some(&token),
                b""
            )
            .0,
            401
        );
        assert_eq!(
            call("DELETE", &format!("/api/courses/{code}"), Some("root"), b"").0,
            200
        );
        assert!(store.get(&code).is_none());
    }

    #[test]
    fn replays_prove_record_times() {
        use mmk_modes::replay::Replay;
        use mmk_sim::{Input, World, input};
        let dir = tmp("replays");
        let store = Store::open(&dir).unwrap().with_verified_records(true);
        // a flat course and a run that clears it
        let mut ed = mmk_editor::Editor::new("M1");
        for tx in 0..ed.size().0 {
            let _ = ed.paint_ground(tx, 0);
        }
        let data = ed.save().unwrap();
        let course = mmk_formats::smm2::Course::parse(&data).unwrap();
        let mut w = World::from_course(&course);
        let mut r = Replay::new();
        let mut touch = 0;
        while !w.cleared && r.inputs.len() < 20_000 {
            let jump = if r.inputs.len() % 90 >= 70 {
                input::JUMP
            } else {
                0
            };
            let i = Input {
                buttons: input::RIGHT | input::RUN | jump,
            };
            r.push(i);
            w.step(i);
            if touch == 0 && w.goal_phase != mmk_sim::world::GoalPhase::None {
                touch = r.inputs.len();
            }
        }
        assert!(w.cleared);
        let (st, _, v) = handle(&store, "POST", "/api/accounts", &[], br#"{"name":"Rex"}"#);
        assert_eq!(st, 201);
        let v: Value = serde_json::from_slice(&v).unwrap();
        let token = v["token"].as_str().unwrap().to_string();
        let auth = [("Authorization".to_string(), format!("Bearer {token}"))];
        let m = store.upload(&data, "rex").unwrap();
        let url = format!("/api/courses/{}/replay", m.code);
        // typed-in times don't make records on this server
        let (_, _, b) = handle(
            &store,
            "POST",
            &format!("/api/courses/{}/clear", m.code),
            &auth,
            br#"{"time_ms":1}"#,
        );
        let v: Value = serde_json::from_slice(&b).unwrap();
        assert_eq!(v["record_ms"], 0);
        // a replay that stops short is refused
        let mut short = r.clone();
        short.inputs.truncate(r.inputs.len() - 1);
        assert_eq!(handle(&store, "POST", &url, &auth, &short.encode()).0, 400);
        assert_eq!(handle(&store, "POST", &url, &[], &r.encode()).0, 401);
        let (st, _, b) = handle(&store, "POST", &url, &auth, &r.encode());
        assert_eq!(st, 200);
        let v: Value = serde_json::from_slice(&b).unwrap();
        // timed at the goal touch, as the game's clear screen shows
        let ms = (touch as u64 * 1000).div_ceil(60);
        assert_eq!(
            (v["record_ms"].as_u64(), v["record_verified"].as_bool()),
            (Some(ms), Some(true))
        );
        let (st, _, ghost) = handle(&store, "GET", &url, &[], b"");
        assert_eq!((st, Replay::decode(&ghost)), (200, Some(r)));
        let _ = fs::remove_dir_all(&dir);
    }
}
