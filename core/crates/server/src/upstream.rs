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
//! doesn't hold (ahead of a course import under way), then, while the fetcher has nothing else to
//! do, the ones of every course held. Both kinds are kept on disk.
//!
//! Course World's Popular and New come from the source's live lists ([`crate::lists`]): filled at
//! the start, filled again when a client asks for one that has gone stale (Popular: a mix of the
//! four difficulties' lists; New: the latest courses merged into the ones seen before), and
//! Popular extended in the background while the pages asked for near its end. A listed course's
//! data is fetched when a client asks for it, ahead of everything but a request under way; while
//! idle, the fetcher fetches the pictures of the page a client looks at and the page after it,
//! before the held courses'. A list the source fails to give is asked for again later (the one
//! held meanwhile, or the server's own courses, are served).

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
    /// After a "busy" answer without a Retry-After: this, doubling per retry (at most 2 min).
    pub backoff: Duration,
    /// Retries of a busy request.
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
}

impl Default for Options {
    fn default() -> Self {
        Self {
            interval: Duration::from_secs(3),
            backoff: Duration::from_secs(15),
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
        let mut q = queue.lock().unwrap();
        let t = (code.to_string(), entire);
        q.retain(|x| *x != t);
        q.push_front(t); // (newest first: what the player looks at now)
        drop(q);
        let _ = wake.lock().unwrap().send(Job::Wake);
    });
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

struct Fetcher {
    source: Box<dyn Source>,
    keys: Keys,
    table: Option<KeyTable>,
    opts: Options,
    searched: [Option<Instant>; 4],
    last_request: Option<Instant>,
    /// Thumbnails clients asked for, (code, entire), the latest first.
    wanted_thumbnails: Arc<Mutex<std::collections::VecDeque<(String, bool)>>>,
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
            wanted_thumbnails: Default::default(),
            no_thumbnail: HashSet::new(),
            wanted_courses: Default::default(),
            failed_courses: Default::default(),
            list_retry: [None; 2],
            list_backoff: [Duration::ZERO; 2],
            popular_exhausted: false,
        }
    }

    /// One request to the source, at least `interval` after the one before.
    fn request<T>(
        &mut self,
        interval: Duration,
        req: impl FnOnce(&mut dyn Source) -> Result<T, SourceError>,
    ) -> Result<T, SourceError> {
        if let Some(t) = self.last_request {
            let left = interval.saturating_sub(t.elapsed());
            if !left.is_zero() {
                std::thread::sleep(left);
            }
        }
        let r = req(&mut *self.source);
        self.last_request = Some(Instant::now());
        r
    }

    /// One thumbnail into the store (a busy source: asked once more after its wait, if short).
    fn fetch_thumbnail(&mut self, store: &Store, code: &str, entire: bool) -> bool {
        let raw = code.replace('-', "");
        let interval = self.opts.thumbnail_interval;
        let mut r = self.request(interval, |s| s.thumbnail(&raw, entire));
        if let Err(SourceError::Busy(after)) = &r {
            let w = after.unwrap_or(self.opts.backoff);
            if w <= Duration::from_secs(20) {
                std::thread::sleep(w);
                r = self.request(interval, |s| s.thumbnail(&raw, entire));
            }
        }
        match r {
            Ok(b) if store.put_thumbnail(code, entire, &b).is_ok() => true,
            Ok(_) => {
                eprintln!("[upstream] thumbnail {code}: not a JPEG");
                false
            }
            Err(e) => {
                eprintln!("[upstream] thumbnail {code}: {e}");
                false
            }
        }
    }

    /// The thumbnails clients are waiting for.
    fn serve_thumbnails(&mut self, store: &Store) {
        loop {
            let next = self.wanted_thumbnails.lock().unwrap().pop_front();
            let Some((code, entire)) = next else { return };
            if !store.has_thumbnail(&code, entire)
                && !self.no_thumbnail.contains(&(code.clone(), entire))
                && !self.fetch_thumbnail(store, &code, entire)
            {
                self.no_thumbnail.insert((code, entire));
            }
        }
    }

    /// One thumbnail of a held upstream course that the store doesn't have yet; false: none left.
    fn warm_thumbnail(&mut self, store: &Store) -> bool {
        let next = store
            .missing_thumbnails(self.no_thumbnail.len() + 1)
            .into_iter()
            .find(|t| !self.no_thumbnail.contains(t));
        let Some((code, entire)) = next else {
            return false;
        };
        if !self.fetch_thumbnail(store, &code, entire) {
            self.no_thumbnail.insert((code, entire));
        }
        true
    }

    /// One thumbnail of the page a client looks at (or the page after it) that the store doesn't
    /// have yet; false: none left.
    fn warm_listed(&mut self, store: &Store) -> bool {
        let next = store
            .list_thumbnails_ahead(self.no_thumbnail.len() + 1)
            .into_iter()
            .find(|c| !self.no_thumbnail.contains(&(c.clone(), false)));
        let Some(code) = next else {
            return false;
        };
        if !self.fetch_thumbnail(store, &code, false) {
            self.no_thumbnail.insert((code, false));
        }
        true
    }

    /// What clients wait for: the data of listed courses, then thumbnails.
    fn serve_waiting(&mut self, store: &Store) {
        self.import_wanted(store);
        self.serve_thumbnails(store);
    }

    /// [`Self::serve_waiting`], then the lists that are due; true: one was updated.
    fn serve_urgent(&mut self, store: &Store) -> bool {
        self.serve_waiting(store);
        let mut updated = false;
        // (a list that fails waits a second at least: it isn't due again in this loop)
        while let Some((kind, more)) = self.list_work(store) {
            updated |= self.update_list(store, kind, more);
        }
        updated
    }

    /// The listed courses clients asked for the data of, each fetched and imported.
    fn import_wanted(&mut self, store: &Store) {
        loop {
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
            let mut r = self.request(interval, |s| s.course(&raw));
            if let Err(SourceError::Busy(after)) = &r {
                let w = after.unwrap_or(self.opts.backoff);
                if w <= Duration::from_secs(20) {
                    std::thread::sleep(w);
                    r = self.request(interval, |s| s.course(&raw));
                }
            }
            let imported = match r {
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
        self.serve_waiting(store);
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

    /// One request, paced, retried while the source says it's busy; what clients wait for and a
    /// list that's due go first.
    fn call<T>(
        &mut self,
        store: &Store,
        mut req: impl FnMut(&mut dyn Source) -> Result<T, SourceError>,
    ) -> Result<T, SourceError> {
        let mut wait = self.opts.backoff;
        for attempt in 0..=self.opts.retries {
            self.serve_urgent(store);
            match self.request(self.opts.interval, &mut req) {
                Err(SourceError::Busy(after)) if attempt < self.opts.retries => {
                    let w = after.unwrap_or(wait).min(Duration::from_secs(120));
                    eprintln!("[upstream] the source is busy: again in {} s", w.as_secs());
                    std::thread::sleep(w);
                    wait = (wait * 2).min(Duration::from_secs(120));
                }
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
fn listed_of(v: &Value, class: Option<u8>) -> Option<Listed> {
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
        /// A course download takes this long.
        delay: Duration,
    }

    impl Fake {
        fn list_search(&mut self) -> Result<(), SourceError> {
            self.list_searches
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            if self.fail_lists.load(std::sync::atomic::Ordering::SeqCst) {
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
            delay: Duration::ZERO,
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
            .push_back(("PDC-YMM-WDF".into(), true));
        fetch
            .wanted_thumbnails
            .lock()
            .unwrap()
            .push_back(("PDC-YMM-WDF".into(), false));
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
        assert!(!fetch.warm_listed(&store), "no page asked for yet");
        courses(&store, "/api/courses?sort=popular&offset=0&limit=1");
        let mut rounds = 0;
        while fetch.warm_listed(&store) {
            rounds += 1;
            assert!(rounds < 10);
        }
        // (the page and the one after it; the source has no picture of the second course)
        assert_eq!(rounds, 2);
        assert!(store.has_thumbnail("N24-N99-PNF", false));
        assert!(!store.has_thumbnail("PDC-YMM-WDF", false));
        assert!(store.get("N24-N99-PNF").is_none(), "pictures only");
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
