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

use std::collections::HashSet;
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};

use mmk_formats::smm2::crypto::{self, BCD_SIZE};
use mmk_formats::smm2::{COURSE_SIZE, KeyTable, keyfind};
use mmk_modes::endless::Difficulty;
use serde_json::Value;

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

impl Source for TgrCode {
    fn endless(&mut self, d: Difficulty, count: usize) -> Result<Vec<Value>, SourceError> {
        let d = match d {
            Difficulty::Easy => "e",
            Difficulty::Normal => "n",
            Difficulty::Expert => "ex",
            Difficulty::SuperExpert => "sex",
        };
        let body = self.get(&format!(
            "/search_endless_mode?count={count}&difficulty={d}"
        ))?;
        let v: Value = serde_json::from_slice(&body)
            .map_err(|e| SourceError::Failed(format!("endless search: {e}")))?;
        v["courses"].as_array().cloned().ok_or_else(|| {
            SourceError::Failed(format!("endless search: no courses ({})", v["error"]))
        })
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
    /// Before a thumbnail a client is waiting for (shorter than `interval`; one try, no retries).
    pub thumbnail_interval: Duration,
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
        }
    }
}

/// What the fetcher is told.
enum Job {
    /// A difficulty is short of courses.
    Short(Difficulty),
    /// A client is waiting for thumbnails (in the fetcher's queue).
    Thumbnails,
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
    let mut f = Fetcher::new(source, keys, opts);
    let queue = f.wanted_thumbnails.clone();
    store.on_thumbnail(move |code, entire| {
        let mut q = queue.lock().unwrap();
        let t = (code.to_string(), entire);
        q.retain(|x| *x != t);
        q.push_front(t); // (newest first: what the player looks at now)
        drop(q);
        let _ = tx.lock().unwrap().send(Job::Thumbnails);
    });
    std::thread::spawn(move || {
        loop {
            // thumbnails a client waits for first, then a difficulty that's short; with nothing
            // waiting, one thumbnail of a course held
            f.serve_thumbnails(&store);
            let job = match rx.try_recv() {
                Ok(j) => Some(j),
                Err(mpsc::TryRecvError::Empty) if f.warm_thumbnail(&store) => continue,
                Err(mpsc::TryRecvError::Empty) => rx.recv().ok(),
                Err(mpsc::TryRecvError::Disconnected) => None,
            };
            match job {
                Some(Job::Short(d)) => {
                    f.wanted(&store, d);
                }
                Some(Job::Thumbnails) => {}
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

    /// One request, paced, retried while the source says it's busy; the thumbnails clients wait
    /// for go first.
    fn call<T>(
        &mut self,
        store: &Store,
        mut req: impl FnMut(&mut dyn Source) -> Result<T, SourceError>,
    ) -> Result<T, SourceError> {
        let mut wait = self.opts.backoff;
        for attempt in 0..=self.opts.retries {
            self.serve_thumbnails(store);
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
        style: String::new(),
        theme: 0,
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
        };
        (f, searches)
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
