//! Courses from an upstream source for the Endless Challenge and 100 Mario runs: TGRCode's SMM2 API
//! (`https://tgrcode.com/mm2`, a third-party service that reads Nintendo's Course World). Off unless
//! the server is started with it (`mmk server --upstream tgrcode`, or an app's own server).
//!
//! A run that starts, or a pick whose clear-rate bands ran dry, tells the fetcher which difficulty
//! is short ([`Store::on_short`]). The fetcher searches the source's Endless pool for that
//! difficulty, downloads the courses the server doesn't hold, decrypts them (the course key table
//! comes from the player's own game: given, or found with the first download, never stored) and
//! imports them under the source's code with its attempts and clears, so the pick's bands apply as
//! to any course. One request at a time, `interval` apart; a difficulty is searched at most once per
//! `cooldown`. When the source fails, it's logged and the runs play the courses the server holds.
//!
//! Thumbnails (a course's picture and its whole-course map, for Course World) come from the source
//! too, one request at a time like the rest: first the ones a client asked for and the server
//! doesn't hold (the latest burst of requests first: the pictures on the player's screen; ahead of
//! a course import under way, and served while background requests wait their turn), then, while
//! the fetcher has nothing else to do, the ones of every course held. Both kinds are kept on disk.
//!
//! The source answers "busy" (HTTP 429, without a Retry-After) after a couple of requests close
//! together, and allows about one every few seconds: after a busy answer nothing is asked for a
//! while (longer for each in a row), and what was asked for is asked again then.
//!
//! Course World's Popular and New come from the source's live lists ([`crate::lists`]): filled at
//! the start, filled again when a client asks for one that has gone stale (Popular: a mix of the
//! four difficulties' lists; New: the latest courses merged into the ones seen before), and
//! Popular extended in the background while the pages asked for near its end. A listed course's
//! data is fetched when a client asks for it, ahead of everything but a request under way. The
//! pictures of the page a client looks at and the page after it (before any page is asked for: the
//! first ones of each list) are fetched while idle, and between the background imports while the
//! client looks at the lists, so they are there when the game asks; then the held courses'. A list
//! the source fails to give is asked for again later (the one held meanwhile, or the server's own
//! courses, are served).
//!
//! Course World's Super Worlds ([`crate::superworlds`]) come from the source too: the pool is asked
//! for again when a client browses it and it is older than `super_worlds_refresh` (each answer
//! names about ten random recent worlds, so it grows over time; several answers while it is small),
//! and a world a client asks for that the store doesn't hold is fetched ahead of everything else
//! (its details and map; its courses are then listed, fetched when a client asks for their data).

use std::cmp::Reverse;
use std::collections::HashSet;
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};

use mmk_formats::smm2::crypto::{self, BCD_SIZE};
use mmk_formats::smm2::{COURSE_SIZE, KeyTable, keyfind};
use mmk_modes::endless::Difficulty;
use serde_json::Value;

use crate::lists::{Kind, Listed};
use crate::{Meta, Store};

/// Why a request to the source failed.
#[derive(Debug, PartialEq)]
pub enum SourceError {
    /// Too many requests (HTTP 429): wait (the source's Retry-After, if it gave one) and retry.
    Busy(Option<Duration>),
    Failed(String),
}

impl std::fmt::Display for SourceError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            SourceError::Busy(_) => write!(f, "busy (HTTP 429)"),
            SourceError::Failed(e) => write!(f, "{e}"),
        }
    }
}

/// Where the courses come from.
pub trait Source: Send {
    /// Up to `count` course records (TGRCode's JSON objects) from the source's Endless pool.
    fn endless(&mut self, d: Difficulty, count: usize) -> Result<Vec<Value>, SourceError>;
    /// A course file by its code (no dashes): the encrypted `.bcd` or the plain body.
    fn course(&mut self, code: &str) -> Result<Vec<u8>, SourceError>;
    /// A course's thumbnail by its code (no dashes; `entire`: the whole-course map): a JPEG, or the
    /// game's thumbnail block.
    fn thumbnail(&mut self, _code: &str, _entire: bool) -> Result<Vec<u8>, SourceError> {
        Err(SourceError::Failed("no thumbnails".into()))
    }
    /// Up to `count` course records of the source's Popular list for a difficulty (the source may
    /// give fewer).
    fn popular(&mut self, _d: Difficulty, _count: usize) -> Result<Vec<Value>, SourceError> {
        Err(SourceError::Failed("no lists".into()))
    }
    /// Up to `count` course records of the latest uploads (the source may give fewer).
    fn newest(&mut self, _count: usize) -> Result<Vec<Value>, SourceError> {
        Err(SourceError::Failed("no lists".into()))
    }
    /// Some Super World records (TGRCode: ten random recent ones; no map, no courses).
    fn super_worlds(&mut self) -> Result<Vec<Value>, SourceError> {
        Err(SourceError::Failed("no super worlds".into()))
    }
    /// One Super World by its ID: the record with its map (`unk1`, base64), courses and ninjis.
    fn super_world(&mut self, _id: &str) -> Result<Value, SourceError> {
        Err(SourceError::Failed("no super worlds".into()))
    }
}

/// TGRCode's API.
pub struct TgrCode {
    base: String,
    agent: ureq::Agent,
}

impl TgrCode {
    pub fn new() -> Self {
        Self::with_base("https://tgrcode.com/mm2")
    }

    pub fn with_base(base: &str) -> Self {
        let agent = ureq::Agent::config_builder()
            .http_status_as_error(false)
            .timeout_global(Some(Duration::from_secs(40)))
            .user_agent("mmk-server (Super Mario Maker 2 recomp, endless courses)")
            .build()
            .into();
        Self {
            base: base.trim_end_matches('/').to_string(),
            agent,
        }
    }

    fn get(&self, path: &str) -> Result<Vec<u8>, SourceError> {
        let url = format!("{}{path}", self.base);
        let failed = |e: String| SourceError::Failed(format!("{url}: {e}"));
        let mut resp = self
            .agent
            .get(&url)
            .call()
            .map_err(|e| failed(e.to_string()))?;
        match resp.status().as_u16() {
            200 => {}
            429 | 503 => {
                let after = resp
                    .headers()
                    .get("retry-after")
                    .and_then(|v| v.to_str().ok());
                return Err(SourceError::Busy(
                    after
                        .and_then(|v| v.trim().parse().ok())
                        .map(Duration::from_secs),
                ));
            }
            st => return Err(failed(format!("HTTP {st}"))),
        }
        resp.body_mut()
            .with_config()
            .limit(4 << 20)
            .read_to_vec()
            .map_err(|e| failed(e.to_string()))
    }
}

impl Default for TgrCode {
    fn default() -> Self {
        Self::new()
    }
}

/// TGRCode's name of a difficulty.
fn difficulty_code(d: Difficulty) -> &'static str {
    match d {
        Difficulty::Easy => "e",
        Difficulty::Normal => "n",
        Difficulty::Expert => "ex",
        Difficulty::SuperExpert => "sex",
    }
}

/// The course records of a TGRCode search answer (`{"courses": [...]}`).
fn courses_of(body: &[u8], what: &str) -> Result<Vec<Value>, SourceError> {
    let v: Value =
        serde_json::from_slice(body).map_err(|e| SourceError::Failed(format!("{what}: {e}")))?;
    v["courses"]
        .as_array()
        .cloned()
        .ok_or_else(|| SourceError::Failed(format!("{what}: no courses ({})", v["error"])))
}

impl Source for TgrCode {
    fn endless(&mut self, d: Difficulty, count: usize) -> Result<Vec<Value>, SourceError> {
        let d = difficulty_code(d);
        let body = self.get(&format!(
            "/search_endless_mode?count={count}&difficulty={d}"
        ))?;
        courses_of(&body, "endless search")
    }

    fn popular(&mut self, d: Difficulty, count: usize) -> Result<Vec<Value>, SourceError> {
        // (TGRCode: at most 100 a search, a random pick of the difficulty's popular courses)
        let count = count.clamp(1, 100);
        let d = difficulty_code(d);
        let body = self.get(&format!("/search_popular?count={count}&difficulty={d}"))?;
        courses_of(&body, "popular search")
    }

    fn newest(&mut self, count: usize) -> Result<Vec<Value>, SourceError> {
        // (TGRCode: at most 20 a search, the same ones for a few minutes)
        let count = count.clamp(1, 20);
        let body = self.get(&format!("/search_new?count={count}"))?;
        courses_of(&body, "new search")
    }

    fn course(&mut self, code: &str) -> Result<Vec<u8>, SourceError> {
        self.get(&format!("/level_data/{code}"))
    }

    fn super_worlds(&mut self) -> Result<Vec<Value>, SourceError> {
        let body = self.get("/get_super_worlds")?;
        let v: Value = serde_json::from_slice(&body)
            .map_err(|e| SourceError::Failed(format!("super worlds: {e}")))?;
        v["super_worlds"]
            .as_array()
            .cloned()
            .ok_or_else(|| SourceError::Failed(format!("super worlds: none ({})", v["error"])))
    }

    fn super_world(&mut self, id: &str) -> Result<Value, SourceError> {
        let body = self.get(&format!("/super_world/{id}"))?;
        let v: Value = serde_json::from_slice(&body)
            .map_err(|e| SourceError::Failed(format!("super world {id}: {e}")))?;
        if v["id"].is_string() {
            Ok(v)
        } else {
            Err(SourceError::Failed(format!(
                "super world {id}: {}",
                v["error"]
            )))
        }
    }

    fn thumbnail(&mut self, code: &str, entire: bool) -> Result<Vec<u8>, SourceError> {
        let kind = if entire {
            "level_entire_thumbnail"
        } else {
            "level_thumbnail"
        };
        self.get(&format!("/{kind}/{code}"))
    }
}

/// The course key table: given, or found in the player's executable with the first download.
pub enum Keys {
    Table(Box<KeyTable>),
    /// Regions of the player's own game executable to search (read-only and data segments, or
    /// the whole loaded image).
    Find(Vec<Vec<u8>>),
}

pub struct Options {
    /// Between two requests to the source.
    pub interval: Duration,
    /// After a "busy" answer without a Retry-After, nothing is asked for this long, doubling per
    /// busy answer in a row (at most a minute).
    pub backoff: Duration,
    /// Retries of a busy background request.
    pub retries: u32,
    /// Between two searches for the same difficulty.
    pub cooldown: Duration,
    /// Courses asked for per search.
    pub batch: usize,
    /// Search every difficulty once at the start (an empty server has something to play).
    pub prime: bool,
    /// Courses a search for a difficulty gathers before it stops (more searches while the source keeps
    /// answering full batches): about three runs' worth, so a player's next runs find fresh courses.
    pub pool: [usize; 4],
    /// Before a thumbnail a client is waiting for (shorter than `interval`; one try, no retries),
    /// and before a listed course's data a client asked for.
    pub thumbnail_interval: Duration,
    /// Serve Course World's Popular and New from the source's lists ([`crate::lists`]).
    pub lists: bool,
    /// A list a client asks for this long after the source filled it is filled again: Popular, New.
    pub list_refresh: Duration,
    pub new_refresh: Duration,
    /// Courses asked for per list request (TGRCode gives at most 100 for Popular, 20 for New).
    pub list_batch: usize,
    /// Popular is extended while a page asked for ends less than this many courses before its end.
    pub list_ahead: usize,
    /// The most courses Popular and New keep.
    pub list_max: usize,
    pub new_max: usize,
    /// The Super World pool a client browses is asked for again this long after the source last
    /// answered; while it holds fewer than `super_worlds_fill`, a refresh asks
    /// `super_worlds_requests` times (each answer: about ten random worlds).
    pub super_worlds_refresh: Duration,
    pub super_worlds_fill: usize,
    pub super_worlds_requests: usize,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            interval: Duration::from_secs(3),
            backoff: Duration::from_secs(4), // (TGRCode: the next one goes through 3-5 s later)
            retries: 4,
            cooldown: Duration::from_secs(60),
            batch: 10,
            prime: true,
            pool: [24, 48, 48, 18], // (3 x the 100 Mario run lengths 8 / 16 / 16 / 6)
            thumbnail_interval: Duration::from_millis(1500),
            lists: true,
            list_refresh: Duration::from_secs(12 * 60),
            new_refresh: Duration::from_secs(2 * 60),
            list_batch: 100,
            list_ahead: 300, // (three of the game's pages of 100)
            list_max: 2000,
            new_max: 300,
            super_worlds_refresh: Duration::from_secs(10 * 60),
            super_worlds_fill: 30,
            super_worlds_requests: 3,
        }
    }
}

/// What the fetcher is told.
enum Job {
    /// A difficulty is short of courses.
    Short(Difficulty),
    /// A client is waiting (thumbnails or a course's data in the fetcher's queues), or asked for a
    /// page of a list.
    Wake,
}

/// Start the fetcher for `store` (a thread; it lives as long as the process).
pub fn start(store: Arc<Store>, source: Box<dyn Source>, keys: Keys, opts: Options) {
    let (tx, rx) = mpsc::channel::<Job>();
    if opts.prime {
        for d in Difficulty::ALL {
            let _ = tx.send(Job::Short(d));
        }
    }
    let tx = Arc::new(Mutex::new(tx));
    let short = tx.clone();
    store.on_short(move |d| {
        let _ = short.lock().unwrap().send(Job::Short(d));
    });
    let lists = opts.lists;
    let mut f = Fetcher::new(source, keys, opts);
    let queue = f.wanted_thumbnails.clone();
    let wake = tx.clone();
    store.on_thumbnail(move |code, entire| {
        queue.lock().unwrap().ask(code, entire);
        let _ = wake.lock().unwrap().send(Job::Wake);
    });
    {
        let wake = tx.clone();
        store.on_super_worlds(move || {
            let _ = wake.lock().unwrap().send(Job::Wake);
        });
        let queue = f.wanted_worlds.clone();
        let wake = tx.clone();
        store.on_super_world(move |id| {
            let mut q = queue.lock().unwrap();
            if !q.iter().any(|w| w == id) {
                q.push_back(id.to_string());
            }
            drop(q);
            let _ = wake.lock().unwrap().send(Job::Wake);
        });
    }
    if lists {
        let wake = tx.clone();
        store.on_lists(move || {
            let _ = wake.lock().unwrap().send(Job::Wake);
        });
        let queue = f.wanted_courses.clone();
        store.on_course(move |code| {
            let mut q = queue.lock().unwrap();
            if !q.iter().any(|c| c == code) {
                q.push_back(code.to_string());
            }
            drop(q);
            let _ = tx.lock().unwrap().send(Job::Wake);
        });
    }
    std::thread::spawn(move || {
        loop {
            // what a client waits for first (a listed course's data, thumbnails), then a list
            // that's due, then a difficulty that's short; with nothing waiting, one thumbnail of
            // the page a client looks at, or of a course held
            if f.serve_urgent(&store) {
                continue;
            }
            if f.clients_waiting() {
                // (the source is busy: asked again when it's its turn)
                f.wait_turn(&store, Duration::ZERO);
                continue;
            }
            let job = match rx.try_recv() {
                Ok(j) => Some(j),
                Err(mpsc::TryRecvError::Empty)
                    if f.warm_listed(&store) || f.warm_thumbnail(&store) =>
                {
                    continue;
                }
                Err(mpsc::TryRecvError::Empty) => rx.recv().ok(),
                Err(mpsc::TryRecvError::Disconnected) => None,
            };
            match job {
                Some(Job::Short(d)) => {
                    f.wanted(&store, d);
                }
                Some(Job::Wake) => {}
                None => return,
            }
        }
    });
}

/// Requests this close together are one burst (a page's pictures, asked for one after another).
const BURST: Duration = Duration::from_millis(250);

/// A thumbnail a client asked for, (code, entire), and its place in the queue.
struct Want {
    key: (String, bool),
    burst: u64,
    seq: i64,
}

/// Thumbnails clients asked for: the latest burst first, in the order asked within it; the ones
/// asked for again (a client now waits for them) ahead of the rest of their burst.
#[derive(Default)]
struct Wanted {
    items: Vec<Want>,
    burst: u64,
    seq: i64,
    last: Option<Instant>,
}

impl Wanted {
    fn ask(&mut self, code: &str, entire: bool) {
        if self.last.is_none_or(|t| t.elapsed() > BURST) {
            self.burst += 1;
        }
        self.last = Some(Instant::now());
        self.seq += 1;
        let key = (code.to_string(), entire);
        match self.items.iter_mut().find(|w| w.key == key) {
            Some(w) => {
                w.burst = self.burst;
                w.seq = self.seq - (1 << 40);
            }
            None => self.items.push(Want {
                key,
                burst: self.burst,
                seq: self.seq,
            }),
        }
    }

    fn next(&mut self) -> Option<Want> {
        let i = (0..self.items.len())
            .max_by_key(|&i| (self.items[i].burst, Reverse(self.items[i].seq)))?;
        Some(self.items.swap_remove(i))
    }

    /// One the source was busy for, in its place (unless asked for again meanwhile).
    fn put_back(&mut self, w: Want) {
        if !self.items.iter().any(|x| x.key == w.key) {
            self.items.push(w);
        }
    }

    fn is_empty(&self) -> bool {
        self.items.is_empty()
    }
}

/// What became of a thumbnail asked for.
#[derive(Debug, PartialEq)]
enum Fetched {
    Got,
    /// The source has none (not asked for again).
    Missing,
    /// The source was busy (asked for again after its pause).
    Busy,
}

struct Fetcher {
    source: Box<dyn Source>,
    keys: Keys,
    table: Option<KeyTable>,
    opts: Options,
    searched: [Option<Instant>; 4],
    last_request: Option<Instant>,
    /// After a busy answer: nothing asked before then, and the pause (doubling per busy answer in a
    /// row).
    busy_until: Option<Instant>,
    busy_pause: Duration,
    /// Thumbnails clients asked for.
    wanted_thumbnails: Arc<Mutex<Wanted>>,
    /// Thumbnails the source didn't give (not asked for again in the background).
    no_thumbnail: HashSet<(String, bool)>,
    /// Listed courses a client asked for the data of, in order.
    wanted_courses: Arc<Mutex<std::collections::VecDeque<String>>>,
    /// Listed courses the source didn't give, and when (not asked for again for a minute).
    failed_courses: std::collections::HashMap<String, Instant>,
    /// Per list (Popular, New): no request before then (after a failure), and that wait.
    list_retry: [Option<Instant>; 2],
    list_backoff: [Duration; 2],
    /// Popular's last extension brought nothing new (none more until it's filled again).
    popular_exhausted: bool,
    /// Popular's first fill stopped part way (filled again when its retry comes).
    popular_partial: bool,
    /// Super Worlds clients asked for that the store doesn't hold, in order; the ones the source
    /// didn't give, and when (not asked for again for a minute).
    wanted_worlds: Arc<Mutex<std::collections::VecDeque<String>>>,
    failed_worlds: std::collections::HashMap<String, Instant>,
    /// The Super World pool: no request before then (after a failure), and that wait.
    pool_retry: Option<Instant>,
    pool_backoff: Duration,
}

impl Fetcher {
    fn new(source: Box<dyn Source>, keys: Keys, opts: Options) -> Self {
        let table = match &keys {
            Keys::Table(t) => Some(**t),
            Keys::Find(_) => None,
        };
        Self {
            source,
            keys,
            table,
            opts,
            searched: [None; 4],
            last_request: None,
            busy_until: None,
            busy_pause: Duration::ZERO,
            wanted_thumbnails: Default::default(),
            no_thumbnail: HashSet::new(),
            wanted_courses: Default::default(),
            failed_courses: Default::default(),
            list_retry: [None; 2],
            list_backoff: [Duration::ZERO; 2],
            popular_exhausted: false,
            popular_partial: false,
            wanted_worlds: Default::default(),
            failed_worlds: Default::default(),
            pool_retry: None,
            pool_backoff: Duration::ZERO,
        }
    }

    /// When a request `interval` after the one before may go (the source not busy).
    fn turn(&self, interval: Duration) -> Instant {
        let now = Instant::now();
        let paced = self.last_request.map_or(now, |t| t + interval);
        self.busy_until.map_or(paced, |b| b.max(paced))
    }

    fn busy(&self) -> bool {
        self.busy_until.is_some_and(|t| Instant::now() < t)
    }

    /// Whether a client waits for a listed course's data or a thumbnail.
    fn clients_waiting(&self) -> bool {
        !self.wanted_thumbnails.lock().unwrap().is_empty()
            || !self.wanted_courses.lock().unwrap().is_empty()
            || !self.wanted_worlds.lock().unwrap().is_empty()
    }

    /// One request to the source, at least `interval` after the one before and after the source's
    /// pause; a busy answer starts a pause.
    fn request<T>(
        &mut self,
        interval: Duration,
        req: impl FnOnce(&mut dyn Source) -> Result<T, SourceError>,
    ) -> Result<T, SourceError> {
        let left = self
            .turn(interval)
            .saturating_duration_since(Instant::now());
        if !left.is_zero() {
            std::thread::sleep(left);
        }
        let r = req(&mut *self.source);
        self.last_request = Some(Instant::now());
        match &r {
            Err(SourceError::Busy(after)) => {
                let pause = match after {
                    Some(a) => (*a).min(Duration::from_secs(120)),
                    None => {
                        self.busy_pause = (self.busy_pause * 2)
                            .max(self.opts.backoff)
                            .min(Duration::from_secs(60));
                        self.busy_pause
                    }
                };
                self.busy_until = Some(Instant::now() + pause);
                eprintln!(
                    "[upstream] the source is busy: nothing asked for {:.1} s",
                    pause.as_secs_f32()
                );
            }
            _ => self.busy_pause = Duration::ZERO,
        }
        r
    }

    /// Until a background request's turn (`interval` after the one before, the source not busy),
    /// serving what clients wait for meanwhile.
    fn wait_turn(&mut self, store: &Store, interval: Duration) {
        loop {
            let serve_at = self
                .clients_waiting()
                .then(|| self.turn(self.opts.thumbnail_interval));
            let at = self.turn(interval);
            let now = Instant::now(); // (after the turns: one with nothing to wait for is now)
            if serve_at.is_some_and(|t| now >= t) {
                self.serve_waiting(store);
                continue;
            }
            let wake = match serve_at {
                None if now >= at => return,
                None => at,
                // (clients first, even when the background request's turn has come)
                Some(t) if now >= at => t,
                Some(t) => t.min(at),
            };
            let left = wake.saturating_duration_since(now);
            std::thread::sleep(left.clamp(Duration::from_millis(1), Duration::from_millis(50)));
        }
    }

    /// One thumbnail into the store.
    fn fetch_thumbnail(&mut self, store: &Store, code: &str, entire: bool) -> Fetched {
        let raw = code.replace('-', "");
        let interval = self.opts.thumbnail_interval;
        match self.request(interval, |s| s.thumbnail(&raw, entire)) {
            Ok(b) if store.put_thumbnail(code, entire, &b).is_ok() => Fetched::Got,
            Ok(_) => {
                eprintln!("[upstream] thumbnail {code}: not a JPEG");
                Fetched::Missing
            }
            Err(SourceError::Busy(_)) => Fetched::Busy,
            Err(e) => {
                eprintln!("[upstream] thumbnail {code}: {e}");
                Fetched::Missing
            }
        }
    }

    /// The thumbnails clients are waiting for (while the source isn't busy).
    fn serve_thumbnails(&mut self, store: &Store) {
        while !self.busy() {
            let next = self.wanted_thumbnails.lock().unwrap().next();
            let Some(w) = next else { return };
            let (code, entire) = (w.key.0.clone(), w.key.1);
            if store.has_thumbnail(&code, entire) || self.no_thumbnail.contains(&w.key) {
                continue;
            }
            match self.fetch_thumbnail(store, &code, entire) {
                Fetched::Got => {}
                Fetched::Missing => {
                    self.no_thumbnail.insert(w.key);
                }
                Fetched::Busy => self.wanted_thumbnails.lock().unwrap().put_back(w),
            }
        }
    }

    /// A background thumbnail, in its turn; false: none (`next` gives the candidates, at most
    /// `limit`).
    fn warm(&mut self, store: &Store, next: impl Fn(&Store, usize) -> Vec<(String, bool)>) -> bool {
        let pick = |f: &Self| {
            next(store, f.no_thumbnail.len() + 1)
                .into_iter()
                .find(|t| !f.no_thumbnail.contains(t))
        };
        if pick(self).is_none() {
            return false;
        }
        self.wait_turn(store, self.opts.thumbnail_interval);
        // (what clients waited for may have been it)
        let Some((code, entire)) = pick(self) else {
            return true;
        };
        if self.fetch_thumbnail(store, &code, entire) == Fetched::Missing {
            self.no_thumbnail.insert((code, entire));
        }
        true
    }

    /// One thumbnail of a held upstream course that the store doesn't have yet; false: none left.
    fn warm_thumbnail(&mut self, store: &Store) -> bool {
        self.warm(store, |s, n| s.missing_thumbnails(n))
    }

    /// One thumbnail of the page a client looks at (or the page after it; before any, of the
    /// lists' first courses) that the store doesn't have yet; false: none left.
    fn warm_listed(&mut self, store: &Store) -> bool {
        self.warm(store, |s, n| {
            s.list_thumbnails_ahead(n)
                .into_iter()
                .map(|c| (c, false))
                .collect()
        })
    }

    /// What clients wait for: Super Worlds, the data of listed courses, then thumbnails.
    fn serve_waiting(&mut self, store: &Store) {
        self.fetch_worlds(store);
        self.import_wanted(store);
        self.serve_thumbnails(store);
    }

    /// [`Self::serve_waiting`], then the lists that are due and the Super World pool if it is; true:
    /// one was updated.
    fn serve_urgent(&mut self, store: &Store) -> bool {
        self.serve_waiting(store);
        let mut updated = false;
        // (a list that fails waits a second at least: it isn't due again in this loop)
        while let Some((kind, more)) = self.list_work(store) {
            updated |= self.update_list(store, kind, more);
        }
        if self.pool_due(store) {
            updated |= self.refresh_pool(store);
        }
        updated
    }

    /// The Super Worlds clients asked for, each fetched and kept (while the source isn't busy).
    fn fetch_worlds(&mut self, store: &Store) {
        while !self.busy() {
            let next = self.wanted_worlds.lock().unwrap().pop_front();
            let Some(id) = next else { return };
            let failed = self
                .failed_worlds
                .get(&id)
                .is_some_and(|t| t.elapsed() < Duration::from_secs(60));
            if failed || store.super_world(&id).is_some() {
                continue;
            }
            let interval = self.opts.thumbnail_interval;
            let kept = match self.request(interval, |s| s.super_world(&id)) {
                Err(SourceError::Busy(_)) => {
                    // (asked again after the pause)
                    self.wanted_worlds.lock().unwrap().push_front(id);
                    return;
                }
                Ok(v) => match crate::superworlds::Detail::of_source(&v) {
                    Some((w, map)) if w.summary.id == id => {
                        let n = w.courses.len();
                        store
                            .put_super_world(w, &map)
                            .map(|_| n)
                            .map_err(|e| format!("{e:?}"))
                    }
                    _ => Err("not a Super World it can read".to_string()),
                },
                Err(e) => Err(e.to_string()),
            };
            match kept {
                Ok(n) => eprintln!("[upstream] Super World {id}: fetched ({n} courses)"),
                Err(e) => {
                    eprintln!("[upstream] Super World {id}: {e}");
                    self.failed_worlds.insert(id, Instant::now());
                }
            }
        }
    }

    /// Whether the Super World pool is to be asked for: a client browses it, and the source hasn't
    /// answered since the start or did longer than the refresh time before the client asked.
    fn pool_due(&self, store: &Store) -> bool {
        if self.pool_retry.is_some_and(|t| Instant::now() < t) {
            return false;
        }
        let s = store.super_worlds_status();
        match s.fetched {
            None => s.asked.is_some(),
            Some(f) => {
                s.asked.is_some_and(|a| a > f) && f.elapsed() >= self.opts.super_worlds_refresh
            }
        }
    }

    /// The Super World pool from the source (several answers while it is small); false: the first
    /// request failed (asked for again later).
    fn refresh_pool(&mut self, store: &Store) -> bool {
        let n = if store.super_worlds_status().len < self.opts.super_worlds_fill {
            self.opts.super_worlds_requests.max(1)
        } else {
            1
        };
        let mut got = 0;
        for i in 0..n {
            match self.list_request(store, |s| s.super_worlds()) {
                Ok(records) => {
                    let worlds: Vec<_> = records
                        .iter()
                        .filter_map(crate::superworlds::Summary::of_source)
                        .collect();
                    got += worlds.len();
                    if let Err(e) = store.merge_super_worlds(worlds) {
                        eprintln!("[upstream] Super Worlds: {e:?}");
                    }
                }
                Err(e) => {
                    if i == 0 {
                        let wait = (self.pool_backoff * 2)
                            .max(self.opts.backoff)
                            .max(Duration::from_secs(1))
                            .min(Duration::from_secs(600));
                        self.pool_backoff = wait;
                        self.pool_retry = Some(Instant::now() + wait);
                        eprintln!(
                            "[upstream] Super Worlds: {e} (again in {} s)",
                            wait.as_secs()
                        );
                        return false;
                    }
                    break;
                }
            }
        }
        self.pool_retry = None;
        self.pool_backoff = Duration::ZERO;
        eprintln!(
            "[upstream] Super Worlds: {got} from the source ({} in the pool)",
            store.super_worlds_status().len
        );
        true
    }

    /// The listed courses clients asked for the data of, each fetched and imported (while the
    /// source isn't busy).
    fn import_wanted(&mut self, store: &Store) {
        while !self.busy() {
            let next = self.wanted_courses.lock().unwrap().pop_front();
            let Some(code) = next else { return };
            let failed = self
                .failed_courses
                .get(&code)
                .is_some_and(|t| t.elapsed() < Duration::from_secs(60));
            if failed || store.get(&code).is_some() {
                continue;
            }
            let Some(listed) = store.listed(&code) else {
                continue;
            };
            let raw = code.replace('-', "");
            let interval = self.opts.thumbnail_interval;
            let imported = match self.request(interval, |s| s.course(&raw)) {
                Err(SourceError::Busy(_)) => {
                    // (asked again after the pause)
                    self.wanted_courses.lock().unwrap().push_front(code);
                    return;
                }
                Ok(file) => match self.body(&file) {
                    Some(body) => store
                        .import(&body, listed.meta)
                        .map_err(|e| format!("import: {e:?}")),
                    None => Err(format!("not a course it can read ({} bytes)", file.len())),
                },
                Err(e) => Err(e.to_string()),
            };
            match imported {
                Ok(_) => eprintln!("[upstream] {code}: fetched for a client"),
                Err(e) => {
                    eprintln!("[upstream] {code}: {e}");
                    self.failed_courses.insert(code, Instant::now());
                }
            }
        }
    }

    /// The list that's due, if any: (which, extend rather than fill again).
    fn list_work(&self, store: &Store) -> Option<(Kind, bool)> {
        if !self.opts.lists {
            return None;
        }
        for kind in Kind::ALL {
            if self.list_retry[kind as usize].is_some_and(|t| Instant::now() < t) {
                continue;
            }
            let s = store.list_status(kind);
            let stale = match kind {
                Kind::Popular => self.opts.list_refresh,
                Kind::New => self.opts.new_refresh,
            };
            let refill = match s.fetched {
                None => self.opts.prime || s.asked.is_some(),
                Some(_) if kind == Kind::Popular && self.popular_partial => true,
                Some(f) => s.asked.is_some_and(|a| a > f) && f.elapsed() >= stale,
            };
            if refill {
                return Some((kind, false));
            }
            if kind == Kind::Popular
                && s.fetched.is_some()
                && !self.popular_exhausted
                && s.depth > 0
                && s.depth + self.opts.list_ahead > s.len
                && s.len < self.opts.list_max
            {
                return Some((kind, true));
            }
        }
        None
    }

    /// One list request, paced; what clients wait for goes first.
    fn list_request(
        &mut self,
        store: &Store,
        req: impl FnOnce(&mut dyn Source) -> Result<Vec<Value>, SourceError>,
    ) -> Result<Vec<Value>, SourceError> {
        self.wait_turn(store, self.opts.interval);
        self.request(self.opts.interval, req)
    }

    /// Fill (or extend) a list from the source; false: it failed (asked for again later).
    fn update_list(&mut self, store: &Store, kind: Kind, more: bool) -> bool {
        let r = match kind {
            Kind::Popular => self.fetch_popular(store, more),
            Kind::New => self.fetch_new(store),
        };
        let k = kind as usize;
        match r {
            Ok(()) => {
                self.list_retry[k] = None;
                self.list_backoff[k] = Duration::ZERO;
                true
            }
            Err(e) => {
                let wait = match e {
                    SourceError::Busy(Some(after)) => after.max(Duration::from_secs(1)),
                    _ => (self.list_backoff[k] * 2)
                        .max(self.opts.backoff)
                        .max(Duration::from_secs(1))
                        .min(Duration::from_secs(600)),
                };
                self.list_backoff[k] = wait;
                self.list_retry[k] = Some(Instant::now() + wait);
                let held = store.list_status(kind).len;
                eprintln!(
                    "[upstream] {kind:?} list: {e} (again in {} s; Course World shows {})",
                    wait.as_secs(),
                    if held == 0 {
                        "the courses held"
                    } else {
                        "the list as it was"
                    }
                );
                false
            }
        }
    }

    /// Popular: each difficulty's list, mixed (a course of each in turn), in place of the one
    /// held or after it (`more`).
    fn fetch_popular(&mut self, store: &Store, more: bool) -> Result<(), SourceError> {
        let batch = self.opts.list_batch;
        let first = store.list_status(Kind::Popular).len == 0;
        let mut per: Vec<Vec<Listed>> = Vec::new();
        for d in Difficulty::ALL {
            let records = match self.list_request(store, |s| s.popular(d, batch)) {
                Ok(r) => r,
                Err(e) => {
                    if more {
                        store.extend_list(Kind::Popular, interleave(&per));
                    }
                    if first && !more {
                        self.popular_partial = true;
                    }
                    return Err(e);
                }
            };
            per.push(
                records
                    .iter()
                    .filter_map(|r| listed_of(r, Some(d.index() as u8)))
                    .collect(),
            );
            if first && !more {
                // (an empty list fills as the answers come)
                store.set_list(Kind::Popular, interleave(&per));
            }
        }
        let got: usize = per.iter().map(Vec::len).sum();
        if more {
            let added = store.extend_list(Kind::Popular, interleave(&per));
            self.popular_exhausted = added == 0;
            eprintln!(
                "[upstream] Popular: {added} more ({} listed)",
                store.list_status(Kind::Popular).len
            );
        } else {
            store.set_list(Kind::Popular, interleave(&per));
            self.popular_exhausted = false;
            self.popular_partial = false;
            eprintln!("[upstream] Popular: {got} courses");
        }
        Ok(())
    }

    /// New: the source's latest courses merged into the ones listed.
    fn fetch_new(&mut self, store: &Store) -> Result<(), SourceError> {
        let batch = self.opts.list_batch;
        let records = self.list_request(store, |s| s.newest(batch))?;
        let latest: Vec<Listed> = records.iter().filter_map(|r| listed_of(r, None)).collect();
        let got = latest.len();
        store.merge_newest(Kind::New, latest, self.opts.new_max);
        eprintln!(
            "[upstream] New: {got} latest ({} listed)",
            store.list_status(Kind::New).len
        );
        Ok(())
    }

    /// One background request, paced, retried while the source says it's busy (after its pause);
    /// what clients wait for, a list that's due and, while a client looks at the lists, one of
    /// their pictures go first.
    fn call<T>(
        &mut self,
        store: &Store,
        mut req: impl FnMut(&mut dyn Source) -> Result<T, SourceError>,
    ) -> Result<T, SourceError> {
        for attempt in 0..=self.opts.retries {
            self.serve_urgent(store);
            if store.browsing() {
                self.warm_listed(store);
            }
            self.wait_turn(store, self.opts.interval);
            match self.request(self.opts.interval, &mut req) {
                Err(SourceError::Busy(_)) if attempt < self.opts.retries => {}
                r => return r,
            }
        }
        unreachable!()
    }

    /// `d` is short: one search, then the courses not held yet. Returns how many were imported.
    fn wanted(&mut self, store: &Store, d: Difficulty) -> usize {
        if self.searched[d.index()].is_some_and(|t| t.elapsed() < self.opts.cooldown) {
            return 0;
        }
        self.searched[d.index()] = Some(Instant::now());
        let batch = self.opts.batch;
        // searches until the difficulty's pool target is seen, while the source answers full batches with
        // courses not seen yet
        let mut records: Vec<Value> = Vec::new();
        let mut seen: HashSet<String> = HashSet::new();
        while seen.len() < self.opts.pool[d.index()] {
            let round = match self.call(store, |s| s.endless(d, batch)) {
                Ok(r) => r,
                Err(e) => {
                    eprintln!(
                        "[upstream] {}: {e} (the runs play the courses held)",
                        d.name()
                    );
                    break;
                }
            };
            let full = round.len() >= batch;
            let before = seen.len();
            for r in round {
                if let Some(m) = meta_of(&r)
                    && seen.insert(m.code)
                {
                    records.push(r);
                }
            }
            if !full || seen.len() == before {
                break;
            }
        }
        let mut added = 0;
        for r in &records {
            let Some(meta) = meta_of(r) else { continue };
            if store.get(&meta.code).is_some() {
                continue;
            }
            let code = meta.code.replace('-', "");
            let raw = match self.call(store, |s| s.course(&code)) {
                Ok(b) => b,
                Err(e) => {
                    eprintln!("[upstream] {}: {e}", meta.code);
                    continue;
                }
            };
            let Some(body) = self.body(&raw) else {
                eprintln!(
                    "[upstream] {}: not a course it can read ({} bytes)",
                    meta.code,
                    raw.len()
                );
                continue;
            };
            match store.import(&body, meta) {
                Ok(_) => added += 1,
                Err(e) => eprintln!("[upstream] import: {e:?}"),
            }
        }
        eprintln!(
            "[upstream] {}: {added} new of {} ({} upstream courses held)",
            d.name(),
            records.len(),
            store.upstream_count()
        );
        added
    }

    /// The plain course body of a downloaded file.
    fn body(&mut self, raw: &[u8]) -> Option<Vec<u8>> {
        if raw.len() == COURSE_SIZE {
            return Some(raw.to_vec());
        }
        if raw.len() != BCD_SIZE {
            return None;
        }
        if self.table.is_none()
            && let Keys::Find(regions) = &self.keys
        {
            let t0 = Instant::now();
            self.table = regions
                .iter()
                .find_map(|r| keyfind::find_in(r, raw).map(|(_, t)| t));
            eprintln!(
                "[upstream] course key table {} in the game ({} ms)",
                if self.table.is_some() {
                    "found"
                } else {
                    "not found"
                },
                t0.elapsed().as_millis()
            );
            if self.table.is_some() {
                self.keys = Keys::Find(Vec::new()); // (the image copy is no longer needed)
            }
        }
        crypto::decrypt_bcd(raw, self.table.as_ref()?, true)
            .ok()
            .map(|b| b.body)
    }
}

/// Lists mixed: the first course of each, then the second of each, …
fn interleave(lists: &[Vec<Listed>]) -> Vec<Listed> {
    let longest = lists.iter().map(Vec::len).max().unwrap_or(0);
    (0..longest)
        .flat_map(|i| lists.iter().filter_map(move |l| l.get(i).cloned()))
        .collect()
}

/// A course record of a list -> the course listed, with the record's difficulty class (or
/// `class`, the one the list was asked for).
pub(crate) fn listed_of(v: &Value, class: Option<u8>) -> Option<Listed> {
    let own = v["difficulty"]
        .as_u64()
        .filter(|&c| c <= 3)
        .map(|c| c as u8);
    Some(Listed {
        meta: meta_of(v)?,
        class: own.or(class),
    })
}

/// A course record of the source -> metadata (code in the server's XXX-XXX-XXX form).
fn meta_of(v: &Value) -> Option<Meta> {
    let id = v["course_id"].as_str()?;
    if id.len() != 9 || !id.is_ascii() {
        return None;
    }
    let s = |v: &Value| v.as_str().unwrap_or_default().to_string();
    let n = |k: &str| v[k].as_u64().unwrap_or(0);
    Some(Meta {
        code: format!("{}-{}-{}", &id[..3], &id[3..6], &id[6..]),
        data_id: v["data_id"].as_u64()? as u32,
        name: s(&v["name"]),
        description: s(&v["description"]),
        maker: s(&v["uploader"]["name"]),
        // (game_style 0-4; a course imported takes its own from the course file)
        style: v["game_style"]
            .as_u64()
            .and_then(|i| ["M1", "M3", "MW", "WU", "3W"].get(i as usize))
            .map_or(String::new(), |s| s.to_string()),
        theme: n("theme") as u8,
        uploaded: n("uploaded"),
        plays: n("plays"),
        clears: n("clears"),
        attempts: n("attempts"),
        likes: n("likes"),
        boos: n("boos"),
        owner: 0,
        maker_id: s(&v["uploader"]["code"]),
        record_ms: n("world_record"),
        record_holder: s(&v["record_holder"]["name"]),
        record_verified: false,
        first_clear: s(&v["first_completer"]["name"]),
        upstream: true,
        based_on: String::new(),
        based_on_maker: String::new(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn tmp(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("mmk-upstream-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        d
    }

    /// A made-up key table (tests only; the game's own is never part of the project).
    fn table() -> KeyTable {
        std::array::from_fn(|i| 0x9E37_79B9u32.wrapping_mul(i as u32 + 1) | 0x0101_0101)
    }

    /// A blank course in a game style (distinct bodies per style).
    fn course_body(style: &str) -> Vec<u8> {
        mmk_formats::smm2::Course::blank(style).to_bytes().unwrap()
    }

    fn bcd(body: &[u8]) -> Vec<u8> {
        crypto::encrypt_bcd(
            body,
            &table(),
            crypto::default_header(),
            [7; 16],
            [1, 2, 3, 4],
        )
        .unwrap()
    }

    struct Fake {
        records: Vec<Value>,
        files: std::collections::HashMap<String, Vec<u8>>,
        searches: Arc<std::sync::atomic::AtomicUsize>,
        fail: bool,
        /// Course downloads answered "busy" before one goes through.
        busy: u32,
        /// Popular per difficulty, `count` at a time (each search the next ones); New.
        popular: Vec<Vec<Value>>,
        popular_at: [usize; 4],
        new: Vec<Value>,
        /// List searches, and whether they fail.
        list_searches: Arc<std::sync::atomic::AtomicUsize>,
        fail_lists: Arc<std::sync::atomic::AtomicBool>,
        /// List searches from this one on fail.
        fail_from: Arc<std::sync::atomic::AtomicUsize>,
        /// A course download takes this long.
        delay: Duration,
        /// Thumbnail requests answered "busy" before they go through; the codes asked for.
        thumb_busy: u32,
        thumbs: Arc<Mutex<Vec<String>>>,
        /// Super Worlds: the pool's answer, the worlds by ID, requests made, "busy" answers to give.
        worlds: Vec<Value>,
        world_details: std::collections::HashMap<String, Value>,
        world_requests: Arc<std::sync::atomic::AtomicUsize>,
        world_busy: u32,
    }

    impl Fake {
        fn list_search(&mut self) -> Result<(), SourceError> {
            let n = self
                .list_searches
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            if self.fail_lists.load(std::sync::atomic::Ordering::SeqCst)
                || n >= self.fail_from.load(std::sync::atomic::Ordering::SeqCst)
            {
                return Err(SourceError::Failed("HTTP 502".into()));
            }
            Ok(())
        }
    }

    impl Source for Fake {
        fn endless(&mut self, _: Difficulty, count: usize) -> Result<Vec<Value>, SourceError> {
            self.searches
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            if self.fail {
                return Err(SourceError::Failed("HTTP 502".into()));
            }
            Ok(self.records.iter().take(count).cloned().collect())
        }
        fn course(&mut self, code: &str) -> Result<Vec<u8>, SourceError> {
            std::thread::sleep(self.delay);
            if self.busy > 0 {
                self.busy -= 1;
                return Err(SourceError::Busy(Some(Duration::ZERO)));
            }
            self.files
                .get(code)
                .cloned()
                .ok_or_else(|| SourceError::Failed("HTTP 404".into()))
        }

        fn thumbnail(&mut self, code: &str, entire: bool) -> Result<Vec<u8>, SourceError> {
            self.thumbs.lock().unwrap().push(code.to_string());
            if self.thumb_busy > 0 {
                self.thumb_busy -= 1;
                return Err(SourceError::Busy(None));
            }
            // one course has both pictures, the other only its map
            match (code, entire) {
                ("N24N99PNF", _) | ("PDCYMMWDF", true) => {
                    Ok(vec![0xFF, 0xD8, entire as u8, 0xFF, 0xD9])
                }
                _ => Err(SourceError::Failed("HTTP 404".into())),
            }
        }

        fn popular(&mut self, d: Difficulty, count: usize) -> Result<Vec<Value>, SourceError> {
            self.list_search()?;
            let list = self.popular.get(d.index()).cloned().unwrap_or_default();
            let at = &mut self.popular_at[d.index()];
            let out: Vec<Value> = list.into_iter().skip(*at).take(count).collect();
            *at += out.len();
            Ok(out)
        }

        fn newest(&mut self, count: usize) -> Result<Vec<Value>, SourceError> {
            self.list_search()?;
            Ok(self.new.iter().take(count).cloned().collect())
        }

        fn super_worlds(&mut self) -> Result<Vec<Value>, SourceError> {
            self.world_requests
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            Ok(self.worlds.clone())
        }

        fn super_world(&mut self, id: &str) -> Result<Value, SourceError> {
            self.world_requests
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            if self.world_busy > 0 {
                self.world_busy -= 1;
                return Err(SourceError::Busy(Some(Duration::ZERO)));
            }
            self.world_details
                .get(id)
                .cloned()
                .ok_or_else(|| SourceError::Failed("HTTP 400".into()))
        }
    }

    fn base64(b: &[u8]) -> String {
        const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let mut out = String::new();
        for ch in b.chunks(3) {
            let n = ch.iter().fold(0u32, |a, &x| a << 8 | x as u32) << (8 * (3 - ch.len()));
            for i in 0..4 {
                out.push(if i <= ch.len() {
                    A[(n >> (18 - 6 * i) & 63) as usize] as char
                } else {
                    '='
                });
            }
        }
        out
    }

    /// A Super World record of the source (TGRCode's fields), with its map and courses when `full`.
    fn world_record(id: &str, created: u64, courses: &[Value], full: bool) -> Value {
        let mut v = json!({"id": id, "worlds": 2, "levels": courses.len(), "planet_type": 4,
            "created": created, "unk5": 3, "unk6": 1, "unk7": 1,
            "thumbnail": {"url": "https://example.invalid/never", "size": 51204, "filename": "x"},
            "uploader": {"name": "World Maker", "code": "WMK123456"}});
        if full {
            let map: Vec<u8> = (0..crate::superworlds::MAP_SIZE).map(|i| i as u8).collect();
            v["unk1"] = json!(base64(&map));
            v["courses"] = json!(courses);
            v["ninjis"] = json!([3, 0]);
        }
        v
    }

    fn record(id: &str, data_id: u64, attempts: u64, clears: u64) -> Value {
        json!({"course_id": id, "data_id": data_id, "name": format!("course {id}"), "attempts": attempts,
               "clears": clears, "plays": 5, "likes": 2, "uploaded": 1_600_000_000u64,
               "uploader": {"name": "Maker", "code": "ABCDEFGHJ"}, "world_record": 6743})
    }

    fn fake(fail: bool) -> (Fake, Arc<std::sync::atomic::AtomicUsize>) {
        let searches = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let mut files = std::collections::HashMap::new();
        files.insert("N24N99PNF".to_string(), bcd(&course_body("M1")));
        files.insert("PDCYMMWDF".to_string(), course_body("M3"));
        let f = Fake {
            records: vec![
                record("N24N99PNF", 26065833, 40, 10),
                record("PDCYMMWDF", 60709023, 21, 4),
                record("XXXXXXXXX", 1, 30, 3), // (no file: skipped)
                json!({"course_id": "short"}),
            ],
            files,
            searches: searches.clone(),
            fail,
            busy: 0,
            popular: Vec::new(),
            popular_at: [0; 4],
            new: Vec::new(),
            list_searches: Default::default(),
            fail_lists: Default::default(),
            fail_from: Arc::new(usize::MAX.into()),
            delay: Duration::ZERO,
            thumb_busy: 0,
            thumbs: Default::default(),
            worlds: Vec::new(),
            world_details: Default::default(),
            world_requests: Default::default(),
            world_busy: 0,
        };
        (f, searches)
    }

    /// A course record of a list (TGRCode's fields: class, game style, theme, upload time).
    fn listed_record(id: &str, class: u64, uploaded: u64) -> Value {
        json!({"course_id": id, "data_id": 50_000_000u64 + uploaded % 1000, "name": format!("course {id}"),
               "attempts": 10, "clears": 5, "plays": 7, "likes": 3, "uploaded": uploaded,
               "difficulty": class, "game_style": 3, "theme": 2,
               "uploader": {"name": "Maker", "code": "ABCDEFGHJ"}})
    }

    /// Popular per difficulty: `n` courses each, P<d><i>.
    fn popular_records(n: usize) -> Vec<Vec<Value>> {
        (0..4u64)
            .map(|d| {
                (0..n)
                    .map(|i| listed_record(&format!("P{d}{i:07}"), d, 1_600_000_000))
                    .collect()
            })
            .collect()
    }

    /// Course World's band of a difficulty value (world.c `band()`).
    fn band(v: u64) -> u64 {
        match v {
            0..35 => 0,
            35..75 => 1,
            75..96 => 2,
            _ => 3,
        }
    }

    fn get(store: &Store, url: &str) -> (u16, Vec<u8>) {
        let (st, _, body) = crate::handle(store, "GET", url, &[], &[]);
        (st, body)
    }

    fn courses(store: &Store, url: &str) -> Vec<Value> {
        let (st, body) = get(store, url);
        assert_eq!(st, 200, "{url}");
        let v: Value = serde_json::from_slice(&body).unwrap();
        v["courses"].as_array().unwrap().clone()
    }

    fn codes(list: &[Value]) -> Vec<String> {
        list.iter()
            .map(|c| c["code"].as_str().unwrap().to_string())
            .collect()
    }

    fn now() -> u64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs()
    }

    fn quick() -> Options {
        Options {
            interval: Duration::ZERO,
            backoff: Duration::ZERO,
            retries: 4,
            cooldown: Duration::from_secs(3600),
            batch: 10,
            prime: false,
            pool: [24, 48, 48, 18],
            thumbnail_interval: Duration::ZERO,
            lists: true,
            list_refresh: Duration::from_secs(3600),
            new_refresh: Duration::from_secs(3600),
            list_batch: 3,
            list_ahead: 4,
            list_max: 2000,
            new_max: 300,
            super_worlds_refresh: Duration::from_secs(3600),
            super_worlds_fill: 30,
            super_worlds_requests: 3,
        }
    }

    #[test]
    fn courses_are_imported_under_their_codes_with_their_statistics() {
        let store = Store::open(tmp("import")).unwrap();
        let (f, searches) = fake(false);
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), quick());
        assert_eq!(fetch.wanted(&store, Difficulty::Normal), 2);
        let m = store.get("N24-N99-PNF").unwrap();
        assert!(m.upstream);
        assert_eq!((m.attempts, m.clears, m.data_id), (40, 10, 26065833));
        assert_eq!(m.maker, "Maker");
        assert_eq!(m.difficulty(), 75);
        assert_eq!(store.data("N24-N99-PNF").unwrap(), course_body("M1"));
        assert_eq!(store.data("PDC-YMM-WDF").unwrap(), course_body("M3"));
        // the same difficulty again within the cooldown: no request
        assert_eq!(fetch.wanted(&store, Difficulty::Normal), 0);
        assert_eq!(searches.load(std::sync::atomic::Ordering::SeqCst), 1);
        // another difficulty: courses already held aren't fetched again
        assert_eq!(fetch.wanted(&store, Difficulty::Easy), 0);
        assert_eq!(store.upstream_count(), 2);
    }

    #[test]
    fn local_ids_skip_upstream_courses() {
        let dir = tmp("ids");
        let store = Store::open(&dir).unwrap();
        let (f, _) = fake(false);
        Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), quick())
            .wanted(&store, Difficulty::Easy);
        drop(store);
        let store = Store::open(&dir).unwrap();
        let m = store.upload(&course_body("WU"), "me").unwrap();
        assert!(!m.upstream);
        assert_eq!(m.data_id, 0x10_0001);
    }

    #[test]
    fn the_key_table_is_found_in_the_game_image_with_the_first_download() {
        let store = Store::open(tmp("find")).unwrap();
        let (f, _) = fake(false);
        let mut image = vec![0x5Au8; 4096];
        image.extend(table().iter().flat_map(|w| w.to_le_bytes()));
        image.extend(vec![0xA5u8; 4096]);
        let mut fetch = Fetcher::new(Box::new(f), Keys::Find(vec![image]), quick());
        assert_eq!(fetch.wanted(&store, Difficulty::Expert), 2);
        assert_eq!(fetch.table, Some(table()));
    }

    #[test]
    fn thumbnails_are_fetched_in_the_background_once_each() {
        let store = Store::open(tmp("warm")).unwrap();
        let (f, _) = fake(false);
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), quick());
        fetch.wanted(&store, Difficulty::Normal);
        let mut rounds = 0;
        while fetch.warm_thumbnail(&store) {
            rounds += 1;
            assert!(
                rounds < 10,
                "a thumbnail the source doesn't have isn't asked for again"
            );
        }
        assert_eq!(rounds, 4);
        assert!(
            store.has_thumbnail("N24-N99-PNF", false) && store.has_thumbnail("N24-N99-PNF", true)
        );
        assert!(
            store.has_thumbnail("PDC-YMM-WDF", true) && !store.has_thumbnail("PDC-YMM-WDF", false)
        );
    }

    #[test]
    fn thumbnails_clients_wait_for_go_first() {
        let store = Store::open(tmp("urgent")).unwrap();
        let (f, _) = fake(false);
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), quick());
        fetch.wanted(&store, Difficulty::Normal);
        fetch
            .wanted_thumbnails
            .lock()
            .unwrap()
            .ask("PDC-YMM-WDF", true);
        fetch
            .wanted_thumbnails
            .lock()
            .unwrap()
            .ask("PDC-YMM-WDF", false);
        fetch.serve_thumbnails(&store);
        assert!(store.has_thumbnail("PDC-YMM-WDF", true));
        assert!(
            fetch.no_thumbnail.contains(&("PDC-YMM-WDF".into(), false)),
            "the source has none"
        );
        assert!(
            !store.has_thumbnail("N24-N99-PNF", false),
            "not asked for: left for later"
        );
        assert!(fetch.wanted_thumbnails.lock().unwrap().is_empty());
    }

    #[test]
    fn a_busy_source_is_asked_again() {
        let store = Store::open(tmp("busy")).unwrap();
        let (mut f, _) = fake(false);
        f.busy = 3;
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), quick());
        assert_eq!(fetch.wanted(&store, Difficulty::Easy), 2);
    }

    #[test]
    fn a_failing_source_imports_nothing() {
        let store = Store::open(tmp("fail")).unwrap();
        let (f, _) = fake(true);
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), quick());
        assert_eq!(fetch.wanted(&store, Difficulty::SuperExpert), 0);
        assert_eq!(store.upstream_count(), 0);
    }

    #[test]
    fn popular_and_new_come_from_the_source_lists() {
        let store = Store::open(tmp("lists")).unwrap();
        let own = store.upload(&course_body("WU"), "me").unwrap(); // (uploaded now)
        let (mut f, _) = fake(false);
        f.popular = popular_records(3);
        let t = now();
        f.new = vec![
            listed_record("NEW000001", 0, t - 100),
            listed_record("NEW000002", 3, t - 50),
        ];
        let opts = Options {
            prime: true,
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        store.on_lists(|| {});
        while fetch.serve_urgent(&store) {}

        // Popular: the four difficulties in turn, then the courses held
        let list = courses(&store, "/api/courses?sort=popular&offset=0&limit=100");
        let dashed = |id: &str| format!("{}-{}-{}", &id[..3], &id[3..6], &id[6..]);
        let mut want: Vec<String> = (0..3)
            .flat_map(|i| (0..4).map(move |d| format!("P{d}{i:07}")))
            .map(|id| dashed(&id))
            .collect();
        want.push(own.code.clone());
        assert_eq!(codes(&list), want);
        // the same shape as the courses held, the source's class as Course World's band
        let keys = |v: &Value| v.as_object().unwrap().keys().cloned().collect::<Vec<_>>();
        assert_eq!(keys(&list[0]), keys(&own.to_json()));
        for (i, c) in list[..12].iter().enumerate() {
            assert_eq!(band(c["difficulty"].as_u64().unwrap()), i as u64 % 4, "{c}");
            assert_eq!(
                (c["style"].as_str(), c["theme"].as_u64()),
                (Some("WU"), Some(2))
            );
            assert_eq!(c["upstream"], true);
        }
        // paging and the style filter
        assert_eq!(
            codes(&courses(
                &store,
                "/api/courses?sort=popular&offset=10&limit=3"
            )),
            want[10..13]
        );
        assert_eq!(
            courses(&store, "/api/courses?sort=popular&style=WU&limit=100").len(),
            13
        );
        assert!(courses(&store, "/api/courses?sort=popular&style=M1").is_empty());
        // a text search: the courses held only
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=popular&q=me")),
            std::slice::from_ref(&own.code)
        );

        // New: the latest courses among the ones held, newest first
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=new")),
            [own.code.clone(), "NEW-000-002".into(), "NEW-000-001".into()]
        );

        // a listed course the server doesn't hold: its metadata from the list
        assert!(store.get("NEW-000-002").is_none());
        let (st, body) = get(&store, "/api/courses/NEW-000-002");
        assert_eq!(st, 200);
        let v: Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(v["upstream"], true);
        assert_eq!(band(v["difficulty"].as_u64().unwrap()), 3);
    }

    #[test]
    fn pages_near_the_end_of_popular_extend_it_ahead() {
        let store = Store::open(tmp("ahead")).unwrap();
        let own = store.upload(&course_body("M1"), "me").unwrap();
        let (mut f, _) = fake(false);
        f.popular = popular_records(9);
        let searches = f.list_searches.clone();
        let count = || searches.load(std::sync::atomic::Ordering::SeqCst);
        let opts = Options {
            prime: true,
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        store.on_lists(|| {});
        while fetch.serve_urgent(&store) {}
        assert_eq!(store.list_status(Kind::Popular).len, 12); // (3 a search, 4 difficulties)
        let after_fill = count();

        // a page well before the end: nothing more
        courses(&store, "/api/courses?sort=popular&offset=0&limit=4");
        assert_eq!(fetch.list_work(&store), None);
        // a page that reaches it: one search per difficulty more, after the courses listed
        courses(&store, "/api/courses?sort=popular&offset=8&limit=4");
        assert_eq!(fetch.list_work(&store), Some((Kind::Popular, true)));
        assert!(fetch.serve_urgent(&store));
        assert_eq!(count(), after_fill + 4);
        assert_eq!(store.list_status(Kind::Popular).len, 24);
        assert_eq!(
            codes(&courses(
                &store,
                "/api/courses?sort=popular&offset=12&limit=4"
            )),
            ["P00-000-003", "P10-000-003", "P20-000-003", "P30-000-003"]
        );
        // to the source's end: an extension that brings nothing stops them
        courses(&store, "/api/courses?sort=popular&offset=20&limit=4");
        while fetch.serve_urgent(&store) {
            courses(&store, "/api/courses?sort=popular&offset=40&limit=4");
        }
        assert_eq!(store.list_status(Kind::Popular).len, 36);
        assert_eq!(fetch.list_work(&store), None);
        // past the list: the courses held
        assert_eq!(
            codes(&courses(
                &store,
                "/api/courses?sort=popular&offset=36&limit=10"
            )),
            [own.code]
        );
    }

    #[test]
    fn a_failing_source_leaves_the_courses_held_or_the_list_as_it_was() {
        let store = Store::open(tmp("listfail")).unwrap();
        let a = store.upload(&course_body("M1"), "me").unwrap();
        store.record(&a.code, "play").unwrap();
        let b = store.upload(&course_body("M3"), "me").unwrap();
        let (mut f, _) = fake(false);
        f.popular = popular_records(2);
        let fail = f.fail_lists.clone();
        fail.store(true, std::sync::atomic::Ordering::SeqCst);
        let opts = Options {
            prime: true,
            list_refresh: Duration::ZERO,
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        store.on_lists(|| {});
        // no list yet: the courses held, sorted as the server sorts them
        while fetch.serve_urgent(&store) {}
        for sort in ["popular", "new"] {
            let held: Vec<Value> = store
                .search("", None, sort, 0, 30)
                .iter()
                .map(Meta::to_json)
                .collect();
            assert_eq!(held.len(), 2);
            assert_eq!(courses(&store, &format!("/api/courses?sort={sort}")), held);
        }
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=popular")),
            [a.code.clone(), b.code.clone()]
        );
        // asked for again only after a wait
        assert_eq!(fetch.list_work(&store), None);

        // a list filled, then a refresh that fails: the list stays
        fail.store(false, std::sync::atomic::Ordering::SeqCst);
        fetch.list_retry = [None; 2];
        while fetch.serve_urgent(&store) {
            if store.list_status(Kind::Popular).len > 0 {
                break;
            }
        }
        let filled = courses(&store, "/api/courses?sort=popular");
        assert_eq!(filled.len(), 8 + 2);
        fail.store(true, std::sync::atomic::Ordering::SeqCst);
        std::thread::sleep(Duration::from_millis(5));
        courses(&store, "/api/courses?sort=popular");
        assert_eq!(fetch.list_work(&store), Some((Kind::Popular, false)));
        assert!(!fetch.serve_urgent(&store));
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=popular")),
            codes(&filled)
        );
    }

    #[test]
    fn a_first_fill_of_popular_that_stops_part_way_is_done_again() {
        let store = Store::open(tmp("partial")).unwrap();
        let (mut f, _) = fake(false);
        f.popular = popular_records(4);
        let fail_from = f.fail_from.clone();
        fail_from.store(2, std::sync::atomic::Ordering::SeqCst); // (Expert's search fails)
        let opts = Options {
            prime: true,
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        store.on_lists(|| {});
        while fetch.serve_urgent(&store) {}
        assert_eq!(store.list_status(Kind::Popular).len, 6); // (3 a search)
        // its retry: all four difficulties
        fail_from.store(usize::MAX, std::sync::atomic::Ordering::SeqCst);
        fetch.list_retry = [None; 2];
        assert_eq!(fetch.list_work(&store), Some((Kind::Popular, false)));
        while fetch.serve_urgent(&store) {}
        assert_eq!(fetch.list_work(&store), None);
        let list = courses(&store, "/api/courses?sort=popular&limit=100");
        for d in 0..4 {
            assert!(
                list.iter()
                    .any(|c| band(c["difficulty"].as_u64().unwrap()) == d)
            );
        }
    }

    #[test]
    fn a_listed_course_is_fetched_when_its_data_is_asked_for() {
        let store = Arc::new(Store::open(tmp("ondemand")).unwrap());
        let (mut f, _) = fake(false);
        f.popular = vec![vec![record("N24N99PNF", 26065833, 40, 10)]];
        f.new = vec![listed_record("PDCYMMWDF", 1, now())];
        f.delay = Duration::from_millis(400);
        let opts = Options {
            prime: true,
            ..quick()
        };
        start(
            store.clone(),
            Box::new(f),
            Keys::Table(Box::new(table())),
            opts,
        );
        let t0 = Instant::now();
        while (store.list_status(Kind::Popular).len == 0 || store.list_status(Kind::New).len == 0)
            && t0.elapsed() < Duration::from_secs(10)
        {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(store.get("N24-N99-PNF").is_none(), "listed, not held");
        assert_eq!(get(&store, "/api/courses/N24-N99-PNF").0, 200);

        // the data: fetched and imported while the request waits; other requests go on
        let s = store.clone();
        let data =
            std::thread::spawn(move || get(&s, "/api/courses/N24-N99-PNF/data?wait_ms=10000"));
        std::thread::sleep(Duration::from_millis(50));
        let t1 = Instant::now();
        assert_eq!(get(&store, "/api/health").0, 200);
        assert_eq!(get(&store, "/api/courses?sort=popular").0, 200);
        assert!(t1.elapsed() < Duration::from_millis(200));
        let (st, body) = data.join().unwrap();
        assert_eq!(st, 200);
        assert_eq!(body, course_body("M1"));
        let m = store.get("N24-N99-PNF").unwrap();
        assert!(m.upstream);
        assert_eq!((m.attempts, m.clears, m.data_id), (40, 10, 26065833));

        // a listed course's map, fetched like a held one's
        let (st, body) = get(
            &store,
            "/api/courses/PDC-YMM-WDF/entire_thumbnail?wait_ms=5000",
        );
        assert_eq!((st, body), (200, vec![0xFF, 0xD8, 1, 0xFF, 0xD9]));
        // neither held nor listed: no wait
        let t2 = Instant::now();
        assert_eq!(get(&store, "/api/courses/ABC-DEF-GHJ/data").0, 404);
        assert!(t2.elapsed() < Duration::from_millis(200));
    }

    #[test]
    fn thumbnails_of_the_page_looked_at_are_fetched_ahead() {
        let store = Store::open(tmp("listthumbs")).unwrap();
        let (mut f, _) = fake(false);
        f.popular = vec![
            vec![record("N24N99PNF", 26065833, 40, 10)],
            vec![record("PDCYMMWDF", 60709023, 21, 4)],
        ];
        let opts = Options {
            prime: true,
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        store.on_lists(|| {});
        while fetch.serve_urgent(&store) {}
        assert!(
            store.browsing(),
            "no page asked for yet: the lists' first pictures"
        );
        let mut rounds = 0;
        while fetch.warm_listed(&store) {
            rounds += 1;
            assert!(rounds < 10);
        }
        // (the source has no picture of the second course)
        assert_eq!(rounds, 2);
        assert!(store.has_thumbnail("N24-N99-PNF", false));
        assert!(!store.has_thumbnail("PDC-YMM-WDF", false));
        assert!(store.get("N24-N99-PNF").is_none(), "pictures only");
    }

    /// A list of `n` courses L0000000… (uploaded `t0 + i * step`).
    fn listed(n: usize, t0: u64, step: i64) -> Vec<Listed> {
        (0..n)
            .map(|i| {
                let up = (t0 as i64 + i as i64 * step) as u64;
                listed_of(&listed_record(&format!("L{i:08}"), 1, up), None).unwrap()
            })
            .collect()
    }

    #[test]
    fn pictures_ahead_follow_the_page_looked_at_in_its_order() {
        let store = Store::open(tmp("aheadorder")).unwrap();
        store.on_lists(|| {});
        // the oldest uploads first: Hot serves them the other way round
        store.set_list(Kind::Popular, listed(10, 1_600_000_000, 1000));
        store.set_list(Kind::New, listed(3, 1_700_000_000, 0));
        // before any page: the first ones of Hot, New, Popular
        let first = store.list_thumbnails_ahead(100);
        assert_eq!(&first[..2], ["L00-000-009", "L00-000-008"]);
        assert!(first.contains(&"L00-000-000".to_string()));
        // a page of Hot: it and the next one, newest first
        courses(&store, "/api/courses?sort=hot&offset=2&limit=2");
        assert_eq!(
            store.list_thumbnails_ahead(100),
            ["L00-000-007", "L00-000-006", "L00-000-005", "L00-000-004"]
        );
        store
            .put_thumbnail("L00-000-006", false, &[0xFF, 0xD8, 0xFF, 0xD9])
            .unwrap();
        assert_eq!(
            store.list_thumbnails_ahead(2),
            ["L00-000-007", "L00-000-005"]
        );
        assert!(store.browsing());
    }

    #[test]
    fn hot_is_popular_newest_first_then_the_courses_held_by_likes() {
        let store = Store::open(tmp("hot")).unwrap();
        let played = store.upload(&course_body("M1"), "me").unwrap();
        let other = store.upload(&course_body("M3"), "me").unwrap();
        store.record(&played.code, "play").unwrap();
        // no upstream lists: the courses held, by likes
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=hot")),
            [played.code.clone(), other.code.clone()]
        );
        store.on_lists(|| {});
        let mut list = listed(4, 1_600_000_000, 0);
        for (c, up) in list.iter_mut().zip([
            1_560_000_000u64,
            1_700_000_000,
            1_600_000_000,
            1_650_000_000,
        ]) {
            c.meta.uploaded = up;
        }
        store.set_list(Kind::Popular, list);
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=hot&limit=100")),
            [
                "L00-000-001",
                "L00-000-003",
                "L00-000-002",
                "L00-000-000",
                &played.code,
                &other.code
            ]
        );
        // Popular keeps the source's order; paging
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=popular&limit=2")),
            ["L00-000-000", "L00-000-001"]
        );
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=hot&offset=3&limit=2")),
            ["L00-000-000", &played.code]
        );
        // a text search: the courses held only
        assert_eq!(
            codes(&courses(&store, "/api/courses?sort=hot&q=me")),
            [played.code.clone(), other.code.clone()]
        );
    }

    #[test]
    fn thumbnails_asked_for_go_latest_burst_first_in_the_order_asked() {
        let mut w = Wanted::default();
        for c in ["A", "B", "C"] {
            w.ask(c, false);
        }
        w.ask("A", false); // (a client now waits for it)
        std::thread::sleep(BURST + Duration::from_millis(50));
        for c in ["D", "E", "F"] {
            w.ask(c, false);
        }
        w.ask("F", false);
        w.ask("E", false);
        let order: Vec<String> = std::iter::from_fn(|| w.next().map(|x| x.key.0)).collect();
        assert_eq!(order, ["F", "E", "D", "A", "B", "C"]);
    }

    #[test]
    fn a_busy_source_pauses_and_the_thumbnail_is_asked_again() {
        let store = Store::open(tmp("thumbbusy")).unwrap();
        let (f, _) = fake(false);
        let asked = f.thumbs.clone();
        let opts = Options {
            backoff: Duration::from_millis(300),
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        fetch.wanted(&store, Difficulty::Normal);
        let (mut f, _) = fake(false);
        f.thumb_busy = 2;
        f.thumbs = asked.clone();
        fetch.source = Box::new(f);
        fetch
            .wanted_thumbnails
            .lock()
            .unwrap()
            .ask("N24-N99-PNF", false);
        let t0 = Instant::now();
        fetch.serve_waiting(&store);
        // busy: kept for later, not taken for missing
        assert!(fetch.busy() && fetch.clients_waiting());
        assert!(fetch.no_thumbnail.is_empty());
        fetch.wait_turn(&store, Duration::ZERO);
        assert!(store.has_thumbnail("N24-N99-PNF", false));
        // (paused 300 ms, then 600 ms: twice as long for a second busy answer in a row)
        assert!(
            t0.elapsed() >= Duration::from_millis(900),
            "{:?}",
            t0.elapsed()
        );
        assert_eq!(asked.lock().unwrap().len(), 3);
        assert_eq!(fetch.busy_pause, Duration::ZERO);
    }

    #[test]
    fn a_background_request_waiting_its_turn_lets_a_waited_thumbnail_go_first() {
        let store = Store::open(tmp("turn")).unwrap();
        let (f, _) = fake(false);
        let opts = Options {
            interval: Duration::from_secs(1),
            ..quick()
        };
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        fetch.wanted(&store, Difficulty::Normal);
        fetch.last_request = Some(Instant::now());
        fetch
            .wanted_thumbnails
            .lock()
            .unwrap()
            .ask("N24-N99-PNF", false);
        let t0 = Instant::now();
        fetch.wait_turn(&store, Duration::from_secs(1));
        assert!(store.has_thumbnail("N24-N99-PNF", false));
        assert!(t0.elapsed() >= Duration::from_millis(900), "then its turn");
    }

    #[test]
    fn super_worlds_come_from_the_source_and_their_courses_are_listed() {
        let dir = tmp("superworlds");
        let store = Arc::new(Store::open(&dir).unwrap());
        let (mut f, _) = fake(false);
        let a = "aaaaaaaaaaaaaaaa_20250101000000000001";
        let b = "bbbbbbbbbbbbbbbb_20260101000000000002";
        let courses = [
            record("PDCYMMWDF", 60709023, 21, 4),
            record("N24N99PNF", 26065833, 40, 10),
        ];
        f.worlds = vec![
            world_record(a, 1_735_689_600, &[], false),
            world_record(b, 1_767_225_600, &[], false),
            json!({"id": "../bad"}),
        ];
        f.world_details.insert(
            b.to_string(),
            world_record(b, 1_767_225_600, &courses, true),
        );
        f.world_busy = 1;
        let requests = f.world_requests.clone();
        start(
            store.clone(),
            Box::new(f),
            Keys::Table(Box::new(table())),
            quick(),
        );
        let worlds = |url: &str| -> Vec<Value> {
            let (st, body) = get(&store, url);
            assert_eq!(st, 200, "{url}");
            let v: Value = serde_json::from_slice(&body).unwrap();
            v["super_worlds"].as_array().unwrap().clone()
        };
        // the pool: asked for when a client browses (an empty one waited for), newest first
        let list = worlds("/api/super_worlds?wait_ms=5000");
        let ids: Vec<&str> = list.iter().map(|w| w["id"].as_str().unwrap()).collect();
        assert_eq!(ids, [b, a]);
        assert_eq!(list[0]["maker_name"], "World Maker");
        assert_eq!(list[0]["maker_id"], "WMK123456");
        assert_eq!(list[0]["planet_type"], 4);
        assert_eq!(list[0]["created"], 1_767_225_600u64);
        // (three answers while it is small, then none until the refresh time)
        let t0 = Instant::now();
        while requests.load(std::sync::atomic::Ordering::SeqCst) < 3
            && t0.elapsed() < Duration::from_secs(5)
        {
            std::thread::sleep(Duration::from_millis(10));
        }
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(worlds("/api/super_worlds?offset=1&limit=5").len(), 1);
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(requests.load(std::sync::atomic::Ordering::SeqCst), 3);

        // a world's details: fetched while the client waits (the source busy once), kept
        let (st, body) = get(&store, &format!("/api/super_worlds/{b}?wait_ms=5000"));
        assert_eq!(st, 200);
        let w: Value = serde_json::from_slice(&body).unwrap();
        let codes: Vec<&str> = w["courses"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c["code"].as_str().unwrap())
            .collect();
        assert_eq!(codes, ["PDC-YMM-WDF", "N24-N99-PNF"], "in course order");
        assert_eq!(w["courses"][0]["data_id"], 60709023);
        assert_eq!(w["ninjis"], json!([3, 0]));
        let (st, map) = get(&store, &format!("/api/super_worlds/{b}/map"));
        assert_eq!((st, map.len(), map[0x4ff]), (200, 0x500, 0xff));
        assert_eq!(
            get(&store, &format!("/api/super_worlds/{b}/thumbnail")).0,
            404,
            "the source's thumbnails are never fetched"
        );
        // one the source doesn't know: not found, and not asked for again at once
        assert_eq!(
            get(&store, "/api/super_worlds/cccccccccccccccc_1?wait_ms=300").0,
            404
        );
        assert_eq!(get(&store, "/api/super_worlds/x..y").0, 404);

        // its courses: listed, imported when their data is asked for
        assert!(store.get("PDC-YMM-WDF").is_none());
        let (st, body) = get(&store, "/api/courses/PDC-YMM-WDF");
        assert_eq!(st, 200);
        let m: Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(m["upstream"], true);
        let (st, data) = get(&store, "/api/courses/PDC-YMM-WDF/data?wait_ms=5000");
        assert_eq!((st, data), (200, course_body("M3")));

        // kept for good: the pool, the details and the courses' listing after a restart
        drop(store);
        let store = Store::open(&dir).unwrap();
        assert_eq!(store.super_worlds_status().len, 2);
        let w = store.super_world(b).unwrap();
        assert_eq!(w.courses.len(), 2);
        assert_eq!(w.courses[1].meta.data_id, 26065833);
        assert_eq!(store.super_world_map(b).unwrap().len(), 0x500);
        assert_eq!(get(&store, "/api/courses/N24-N99-PNF").0, 200);
    }

    #[test]
    fn a_failing_super_world_pool_is_asked_for_again_later() {
        let store = Store::open(tmp("poolfail")).unwrap();
        let (f, _) = fake(false);
        let opts = Options {
            backoff: Duration::from_secs(30),
            ..quick()
        };
        // (the default source: no Super Worlds)
        let mut fetch = Fetcher::new(Box::new(f), Keys::Table(Box::new(table())), opts);
        struct NoWorlds;
        impl Source for NoWorlds {
            fn endless(&mut self, _: Difficulty, _: usize) -> Result<Vec<Value>, SourceError> {
                Ok(Vec::new())
            }
            fn course(&mut self, _: &str) -> Result<Vec<u8>, SourceError> {
                Err(SourceError::Failed("none".into()))
            }
        }
        fetch.source = Box::new(NoWorlds);
        assert!(!fetch.pool_due(&store), "nobody browses");
        store.super_world_page(0, 10);
        assert!(fetch.pool_due(&store));
        assert!(!fetch.serve_urgent(&store));
        assert!(!fetch.pool_due(&store), "after a wait");
        assert_eq!(store.super_worlds_status().len, 0);
    }

    #[test]
    fn a_run_start_asks_for_its_difficulty() {
        let store = Arc::new(Store::open(tmp("signal")).unwrap());
        let (f, searches) = fake(false);
        start(
            store.clone(),
            Box::new(f),
            Keys::Table(Box::new(table())),
            quick(),
        );
        let (_, token) = store.register("Runner").unwrap();
        let who = store.account(&token).unwrap().maker_id;
        store
            .endless_start(&who, Difficulty::Easy, mmk_modes::endless::Ruleset::Hundred)
            .unwrap();
        let t0 = Instant::now();
        while store.upstream_count() < 2 && t0.elapsed() < Duration::from_secs(10) {
            std::thread::sleep(Duration::from_millis(20));
        }
        assert_eq!(store.upstream_count(), 2);
        assert_eq!(searches.load(std::sync::atomic::Ordering::SeqCst), 1);
    }
}
