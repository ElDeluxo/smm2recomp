//! Course World's Super Worlds: other makers' worlds to browse, download and play
//! (`research/specs/super-worlds-v303.md`).
//!
//! A Super World is a map (the game's own 0x500-byte encrypted blob, kept and served as it is: the
//! game decrypts it itself) and up to 40 courses, which are ordinary courses by data ID. Worlds come
//! from the upstream source ([`crate::upstream`]: TGRCode's `/get_super_worlds`, about ten random
//! recent worlds an answer, and `/super_world/<id>`) or from this server's own players (an upload
//! is kept here only).
//!
//! - The **pool** is the list Course World shows: every world any source answer named, newest first
//!   (at most [`POOL_MAX`] from the source; this server's uploads first and always). A client asking
//!   for it tells the fetcher, which asks the source again when the pool is older than its refresh
//!   time, so it grows while players browse. Kept in `superworlds/pool.json`.
//! - A world's **details** (its courses in course order, ninjis) and its map are fetched the first
//!   time a client asks for them and kept for good (`superworlds/<id>.json`, `<id>.map`). A world
//!   fetched registers its courses as listed ([`crate::lists`]): `GET /api/courses/<code>` answers
//!   from the record and `/data` fetches and imports the course when a client asks for it.
//! - **Progress** (the game's online play record of a world, opaque fields) is kept per player and
//!   world (`superworlds/progress.json`).
//! - A player's **upload** replaces their earlier one (one world a maker, as on Nintendo's
//!   servers); its courses must be the uploader's courses on this server. Its thumbnail (the game's
//!   0xC804-byte block) is kept with it. The source's worlds have none here: theirs are on
//!   Nintendo's CDN only, which is never contacted.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{Value, json};

use crate::accounts::Account;
use crate::lists::Listed;
use crate::{Meta, Store, StoreError};

/// The encrypted world map (AES-128-CBC map 0x4D0, IV, random state, CMAC).
pub const MAP_SIZE: usize = 0x500;
/// The game's world thumbnail block.
pub const THUMBNAIL_SIZE: usize = 0xC804;
/// Courses a world holds at most (8 worlds of 5).
pub const MAX_LEVELS: usize = 40;
/// Source worlds the pool keeps (this server's uploads come on top).
pub const POOL_MAX: usize = 300;

type Response = (u16, &'static str, Vec<u8>);

/// A world as Course World's list shows it.
#[derive(Clone, Debug, PartialEq)]
pub struct Summary {
    /// The game's form: 16 hex digits of the owner's player ID, `_`, the upload time.
    pub id: String,
    pub maker_id: String,
    pub maker_name: String,
    pub worlds: u8,
    pub levels: u8,
    /// 0 Earth, 1 Moon, 2 Sand, 3 Green, 4 Ice.
    pub planet_type: u8,
    /// Upload time (POSIX seconds).
    pub created: u64,
    /// The source's `unk5` (the map format version, 3), `unk6`, `unk7` (sent to the game as they are).
    pub unk5: u32,
    pub unk6: u8,
    pub unk7: u8,
    /// Uploaded by one of this server's players.
    pub own: bool,
}

impl Summary {
    pub fn to_json(&self) -> Value {
        json!({ "id": self.id, "maker_id": self.maker_id, "maker_name": self.maker_name,
                "worlds": self.worlds, "levels": self.levels, "planet_type": self.planet_type,
                "created": self.created, "unk5": self.unk5, "unk6": self.unk6, "unk7": self.unk7,
                "own": self.own })
    }

    fn from_json(v: &Value) -> Option<Self> {
        let n = |k: &str| v[k].as_u64().unwrap_or(0);
        let id = v["id"].as_str().filter(|id| is_world_id(id))?;
        Some(Self {
            id: id.to_string(),
            maker_id: v["maker_id"].as_str().unwrap_or_default().to_string(),
            maker_name: v["maker_name"].as_str().unwrap_or_default().to_string(),
            worlds: n("worlds") as u8,
            levels: n("levels") as u8,
            planet_type: n("planet_type") as u8,
            created: n("created"),
            unk5: n("unk5") as u32,
            unk6: n("unk6") as u8,
            unk7: n("unk7") as u8,
            own: v["own"].as_bool().unwrap_or(false),
        })
    }

    /// A world record of the source (TGRCode's `/get_super_worlds` entries and `/super_world/<id>`).
    pub fn of_source(v: &Value) -> Option<Self> {
        let n = |k: &str| v[k].as_u64().unwrap_or(0);
        let id = v["id"].as_str().filter(|id| is_world_id(id))?;
        Some(Self {
            id: id.to_string(),
            maker_id: v["uploader"]["code"]
                .as_str()
                .unwrap_or_default()
                .to_string(),
            maker_name: v["uploader"]["name"]
                .as_str()
                .unwrap_or_default()
                .to_string(),
            worlds: n("worlds").min(8) as u8,
            levels: n("levels").min(MAX_LEVELS as u64) as u8,
            planet_type: n("planet_type") as u8,
            created: n("created"),
            unk5: n("unk5") as u32,
            unk6: n("unk6") as u8,
            unk7: n("unk7") as u8,
            own: false,
        })
    }
}

/// A world's details: its courses in course order (world by world), and the source's ninjis.
#[derive(Clone, Debug, PartialEq)]
pub struct Detail {
    pub summary: Summary,
    pub courses: Vec<Listed>,
    pub ninjis: Vec<u32>,
}

impl Detail {
    /// What a client gets: the summary, the courses (code, data ID, name) and the ninjis.
    pub fn to_json(&self) -> Value {
        let mut v = self.summary.to_json();
        v["courses"] = self
            .courses
            .iter()
            .map(|c| json!({ "code": c.meta.code, "data_id": c.meta.data_id, "name": c.meta.name }))
            .collect();
        v["ninjis"] = json!(self.ninjis);
        v
    }

    /// The form kept on disk (the courses' whole records: they are listed again at the start).
    fn to_disk(&self) -> Value {
        let mut v = self.summary.to_json();
        v["courses"] = self
            .courses
            .iter()
            .map(|c| {
                let mut m = c.meta.to_json();
                m["data_id"] = json!(c.meta.data_id);
                m["class"] = json!(c.class);
                m
            })
            .collect();
        v["ninjis"] = json!(self.ninjis);
        v
    }

    fn from_disk(v: &Value) -> Option<Self> {
        let courses = v["courses"]
            .as_array()?
            .iter()
            .filter_map(|c| {
                Some(Listed {
                    meta: Meta::from_json(c)?,
                    class: c["class"].as_u64().map(|c| c as u8),
                })
            })
            .collect();
        Some(Self {
            summary: Summary::from_json(v)?,
            courses,
            ninjis: ninjis_of(&v["ninjis"]),
        })
    }

    /// A source's `/super_world/<id>` answer → the details and the map blob (`unk1`, base64).
    pub fn of_source(v: &Value) -> Option<(Self, Vec<u8>)> {
        let summary = Summary::of_source(v)?;
        let map = base64_decode(v["unk1"].as_str()?)?;
        if map.len() != MAP_SIZE {
            return None;
        }
        let courses: Vec<Listed> = v["courses"]
            .as_array()?
            .iter()
            .filter_map(|r| crate::upstream::listed_of(r, None))
            .take(MAX_LEVELS)
            .collect();
        Some((
            Self {
                summary,
                courses,
                ninjis: ninjis_of(&v["ninjis"]),
            },
            map,
        ))
    }
}

fn ninjis_of(v: &Value) -> Vec<u32> {
    v.as_array()
        .map(|a| {
            a.iter()
                .map(|n| n.as_u64().unwrap_or(0) as u32)
                .take(MAX_LEVELS)
                .collect()
        })
        .unwrap_or_default()
}

/// A world ID safe in a path: what the game and the source use (hex, `_`, digits), at most 63.
pub fn is_world_id(id: &str) -> bool {
    !id.is_empty() && id.len() < 64 && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_')
}

/// What the fetcher sees of the pool.
#[derive(Clone, Copy, Debug, Default)]
pub struct PoolStatus {
    pub len: usize,
    /// When the source last answered (None: not since the start).
    pub fetched: Option<Instant>,
    /// When a client last asked for a page.
    pub asked: Option<Instant>,
}

#[derive(Default)]
pub(crate) struct SuperWorlds {
    pool: Vec<Summary>,
    held: HashMap<String, Detail>,
    /// maker ID → world ID → the play record (plus `updated`).
    progress: HashMap<String, HashMap<String, Value>>,
    status: PoolStatus,
}

pub(crate) type PoolWanted = Box<dyn Fn() + Send + Sync>;
pub(crate) type WorldWanted = Box<dyn Fn(&str) + Send + Sync>;

fn sw_dir(dir: &Path) -> PathBuf {
    dir.join("superworlds")
}

/// The Super Worlds kept in `dir`, and the courses of the source's worlds (listed again).
pub(crate) fn load(dir: &Path) -> (SuperWorlds, Vec<Listed>) {
    let d = sw_dir(dir);
    let read = |p: PathBuf| {
        fs::read(p)
            .ok()
            .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
            .unwrap_or(Value::Null)
    };
    let pool = read(d.join("pool.json"))["super_worlds"]
        .as_array()
        .map(|a| a.iter().filter_map(Summary::from_json).collect())
        .unwrap_or_default();
    let mut held = HashMap::new();
    let mut courses = Vec::new();
    if let Ok(entries) = fs::read_dir(&d) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let Some(id) = name.strip_suffix(".json") else {
                continue;
            };
            if !is_world_id(id) || id == "pool" || id == "progress" {
                continue;
            }
            if let Some(w) = Detail::from_disk(&read(e.path())) {
                if !w.summary.own {
                    courses.extend(w.courses.iter().cloned());
                }
                held.insert(w.summary.id.clone(), w);
            }
        }
    }
    let progress = read(d.join("progress.json"))
        .as_object()
        .map(|o| {
            o.iter()
                .map(|(who, m)| {
                    let worlds = m
                        .as_object()
                        .map(|m| m.iter().map(|(k, v)| (k.clone(), v.clone())).collect())
                        .unwrap_or_default();
                    (who.clone(), worlds)
                })
                .collect()
        })
        .unwrap_or_default();
    (
        SuperWorlds {
            pool,
            held,
            progress,
            status: PoolStatus::default(),
        },
        courses,
    )
}

/// The newest first; this server's uploads before the source's, which keep at most [`POOL_MAX`].
fn order(pool: &mut Vec<Summary>) {
    pool.sort_by_key(|w| (!w.own, std::cmp::Reverse(w.created)));
    let mut source = 0;
    pool.retain(|w| {
        source += !w.own as usize;
        w.own || source <= POOL_MAX
    });
}

/// A player ID in the form the game mod gives every maker (FNV-1a of the maker ID; games/smm2/mods
/// endless.c): the first part of an uploaded world's ID.
pub fn player_id(maker_id: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in maker_id.bytes() {
        h = (h ^ b as u64).wrapping_mul(0x0000_0100_0000_01b3);
    }
    0x5000_0000_0000_0000 | (h & 0x0000_ffff_ffff_ffff) | 1
}

/// `YYYYMMDDhhmmss` of a POSIX time (UTC).
fn timestamp(t: u64) -> String {
    let (days, secs) = ((t / 86_400) as i64, t % 86_400);
    // (civil from days: H. Hinnant's algorithm)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + (month <= 2) as i64;
    format!(
        "{year:04}{month:02}{day:02}{:02}{:02}{:02}",
        secs / 3600,
        secs / 60 % 60,
        secs % 60
    )
}

/// Standard base64 (padding optional).
pub fn base64_decode(s: &str) -> Option<Vec<u8>> {
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            _ => return None,
        } as u32)
    };
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    let (mut acc, mut bits) = (0u32, 0);
    for c in s.bytes().filter(|c| !c.is_ascii_whitespace() && *c != b'=') {
        acc = acc << 6 | val(c)?;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

/// The progress fields a record keeps (the game's online play record, `super-worlds-v303.md` §2.5),
/// with their largest values; the 64-bit ones travel as decimal strings.
const PROGRESS_FIELDS: [(&str, u64); 8] = [
    ("status", u32::MAX as u64),
    ("counter_a", 0xff),
    ("counter_b", 0xff),
    ("score", u32::MAX as u64),
    ("world", 0xff),
    ("x", 0xff),
    ("y", 0xff),
    ("extra", u32::MAX as u64),
];
const PROGRESS_BITS: [&str; 2] = ["cleared", "flags"];

fn progress_record(v: &Value, status: Option<u64>) -> Value {
    let mut out = json!({});
    for (k, max) in PROGRESS_FIELDS {
        out[k] = json!(v[k].as_u64().unwrap_or(0).min(max));
    }
    for k in PROGRESS_BITS {
        let n = match &v[k] {
            Value::String(s) => s.parse::<u64>().unwrap_or(0),
            n => n.as_u64().unwrap_or(0),
        };
        out[k] = json!(n.to_string());
    }
    if let Some(s) = status {
        out["status"] = json!(s);
    }
    out
}

impl Store {
    /// Who to tell when a client asks for a page of the pool (once per store); never blocks.
    pub fn on_super_worlds(&self, f: impl Fn() + Send + Sync + 'static) {
        let _ = self.sw_pool_wanted.set(Box::new(f));
    }

    /// Who fetches a world the store doesn't hold when a client asks for it (once per store); never
    /// blocks.
    pub fn on_super_world(&self, f: impl Fn(&str) + Send + Sync + 'static) {
        let _ = self.sw_world_wanted.set(Box::new(f));
    }

    pub fn super_worlds_status(&self) -> PoolStatus {
        let g = self.superworlds.lock().unwrap();
        PoolStatus {
            len: g.pool.len(),
            ..g.status
        }
    }

    fn save_pool(&self, g: &SuperWorlds) -> Result<(), StoreError> {
        let list: Vec<Value> = g.pool.iter().map(Summary::to_json).collect();
        self.write_sw(
            "pool.json",
            &serde_json::to_vec(&json!({ "super_worlds": list })).unwrap(),
        )
    }

    fn save_progress(&self, g: &SuperWorlds) -> Result<(), StoreError> {
        self.write_sw("progress.json", &serde_json::to_vec(&g.progress).unwrap())
    }

    fn write_sw(&self, name: &str, data: &[u8]) -> Result<(), StoreError> {
        let d = sw_dir(&self.dir);
        let path = d.join(name);
        let tmp = d.join(format!("{name}.tmp"));
        fs::create_dir_all(&d)
            .and_then(|_| fs::write(&tmp, data))
            .and_then(|_| fs::rename(&tmp, &path))
            .map_err(|e| StoreError::Io(e.to_string()))
    }

    /// The source's worlds into the pool (one record an ID: the newer answer's).
    pub fn merge_super_worlds(&self, worlds: Vec<Summary>) -> Result<(), StoreError> {
        let mut g = self.superworlds.lock().unwrap();
        for w in worlds.into_iter().filter(|w| !w.own) {
            match g.pool.iter_mut().find(|x| x.id == w.id) {
                Some(x) if x.own => {}
                Some(x) => *x = w,
                None => g.pool.push(w),
            }
        }
        order(&mut g.pool);
        g.status.fetched = Some(Instant::now());
        self.save_pool(&g)
    }

    /// A page of the pool (`GET /api/super_worlds`); tells the fetcher a client is browsing.
    pub fn super_world_page(&self, offset: usize, limit: usize) -> Vec<Summary> {
        let page = {
            let mut g = self.superworlds.lock().unwrap();
            g.status.asked = Some(Instant::now());
            g.pool.iter().skip(offset).take(limit).cloned().collect()
        };
        if let Some(tell) = self.sw_pool_wanted.get() {
            tell();
        }
        page
    }

    /// A world's details, if held.
    pub fn super_world(&self, id: &str) -> Option<Detail> {
        self.superworlds.lock().unwrap().held.get(id).cloned()
    }

    /// A world's details; one not held is fetched from the source, waiting up to `wait`.
    pub fn super_world_wait(&self, id: &str, wait: Duration) -> Option<Detail> {
        if let Some(w) = self.super_world(id) {
            return Some(w);
        }
        let fetch = self.sw_world_wanted.get()?;
        if !is_world_id(id) {
            return None;
        }
        fetch(id);
        let t0 = Instant::now();
        while t0.elapsed() < wait {
            std::thread::sleep(Duration::from_millis(50).min(wait));
            if let Some(w) = self.super_world(id) {
                return Some(w);
            }
        }
        None
    }

    /// Keep a source world's details and map: its courses are listed from now on, and it joins the
    /// pool.
    pub fn put_super_world(&self, w: Detail, map: &[u8]) -> Result<(), StoreError> {
        if map.len() != MAP_SIZE || !is_world_id(&w.summary.id) {
            return Err(StoreError::Bad("not a world".into()));
        }
        let id = w.summary.id.clone();
        self.write_sw(&format!("{id}.map"), map)?;
        self.write_sw(
            &format!("{id}.json"),
            &serde_json::to_vec(&w.to_disk()).unwrap(),
        )?;
        if !w.summary.own {
            self.lists
                .lock()
                .unwrap()
                .register_world_courses(w.courses.iter().cloned());
        }
        let mut g = self.superworlds.lock().unwrap();
        if !g.pool.iter().any(|x| x.id == id) {
            g.pool.push(w.summary.clone());
            order(&mut g.pool);
        }
        g.held.insert(id, w);
        self.save_pool(&g)
    }

    /// A world's map blob (the game's encrypted 0x500 bytes).
    pub fn super_world_map(&self, id: &str) -> Option<Vec<u8>> {
        if !is_world_id(id) {
            return None;
        }
        fs::read(sw_dir(&self.dir).join(format!("{id}.map")))
            .ok()
            .filter(|b| b.len() == MAP_SIZE)
    }

    /// An uploaded world's thumbnail block (the source's worlds have none here).
    pub fn super_world_thumbnail(&self, id: &str) -> Option<Vec<u8>> {
        if !is_world_id(id) {
            return None;
        }
        fs::read(sw_dir(&self.dir).join(format!("{id}.thumb"))).ok()
    }

    /// The world `maker_id` uploaded here, if any.
    pub fn own_super_world(&self, maker_id: &str) -> Option<String> {
        let g = self.superworlds.lock().unwrap();
        g.pool
            .iter()
            .find(|w| w.own && w.maker_id == maker_id)
            .map(|w| w.id.clone())
    }

    /// A player's world (`POST /api/super_worlds`): `body` is the map blob, then optionally the
    /// thumbnail block; `data_ids` the courses in course order, each one of the uploader's courses
    /// on this server. Replaces the player's earlier world.
    pub fn upload_super_world(
        &self,
        who: &Account,
        data_ids: &[u32],
        worlds: u8,
        planet_type: u8,
        version: u32,
        body: &[u8],
    ) -> Result<Summary, StoreError> {
        let (map, thumb) = match body.len() {
            MAP_SIZE => (body, None),
            n if n == MAP_SIZE + THUMBNAIL_SIZE => (&body[..MAP_SIZE], Some(&body[MAP_SIZE..])),
            _ => {
                return Err(StoreError::Bad(
                    "the map (0x500 bytes), then the thumbnail".into(),
                ));
            }
        };
        if !(1..=8).contains(&worlds) {
            return Err(StoreError::Bad("worlds: 1 to 8".into()));
        }
        if data_ids.is_empty() || data_ids.len() > MAX_LEVELS {
            return Err(StoreError::Bad("1 to 40 courses".into()));
        }
        let courses: Vec<Listed> = {
            let g = self.inner.lock().unwrap();
            data_ids
                .iter()
                .map(|&id| {
                    g.courses
                        .iter()
                        .find(|m| m.data_id == id && !m.upstream && m.owner == who.id)
                        .map(|m| Listed {
                            meta: m.clone(),
                            class: None,
                        })
                        .ok_or_else(|| {
                            StoreError::Bad(format!("course {id} is not one of the uploader's"))
                        })
                })
                .collect::<Result<_, _>>()?
        };
        let t = crate::now();
        let micros = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.subsec_micros());
        let summary = Summary {
            id: format!(
                "{:016x}_{}{micros:06}",
                player_id(&who.maker_id),
                timestamp(t)
            ),
            maker_id: who.maker_id.clone(),
            maker_name: who.name.clone(),
            worlds,
            levels: courses.len() as u8,
            planet_type: planet_type.min(4),
            created: t,
            unk5: version,
            unk6: 1,
            unk7: 1,
            own: true,
        };
        let old = self.own_super_world(&who.maker_id);
        let w = Detail {
            summary: summary.clone(),
            courses,
            ninjis: Vec::new(),
        };
        if let Some(t) = thumb {
            self.write_sw(&format!("{}.thumb", summary.id), t)?;
        }
        self.put_super_world(w, map)?;
        if let Some(old) = old {
            self.remove_world(&old)?;
        }
        Ok(summary)
    }

    fn remove_world(&self, id: &str) -> Result<(), StoreError> {
        let mut g = self.superworlds.lock().unwrap();
        g.pool.retain(|w| w.id != id);
        g.held.remove(id);
        let d = sw_dir(&self.dir);
        for ext in ["json", "map", "thumb"] {
            let _ = fs::remove_file(d.join(format!("{id}.{ext}")));
        }
        self.save_pool(&g)
    }

    /// The player's uploaded world removed (`DELETE /api/super_worlds/mine`); the ID, if there was one.
    pub fn delete_own_super_world(&self, maker_id: &str) -> Result<Option<String>, StoreError> {
        match self.own_super_world(maker_id) {
            Some(id) => self.remove_world(&id).map(|_| Some(id)),
            None => Ok(None),
        }
    }

    /// `maker_id`'s play record of a world (all zero and `held: false` when none).
    pub fn super_world_progress(&self, maker_id: &str, id: &str) -> Value {
        let g = self.superworlds.lock().unwrap();
        match g.progress.get(maker_id).and_then(|m| m.get(id)) {
            Some(v) => {
                let mut r = progress_record(v, None);
                r["held"] = json!(true);
                r
            }
            None => {
                let mut r = progress_record(&Value::Null, None);
                r["held"] = json!(false);
                r
            }
        }
    }

    /// Keep `maker_id`'s play record of a world (`status`: the save's own, else the record's).
    pub fn set_super_world_progress(
        &self,
        maker_id: &str,
        id: &str,
        v: &Value,
    ) -> Result<Value, StoreError> {
        if !is_world_id(id) {
            return Err(StoreError::NotFound);
        }
        let mut r = progress_record(v, None);
        r["updated"] = json!(crate::now());
        let mut g = self.superworlds.lock().unwrap();
        g.progress
            .entry(maker_id.to_string())
            .or_default()
            .insert(id.to_string(), r.clone());
        self.save_progress(&g)?;
        Ok(r)
    }

    /// A world's play record started over (the game's network progress reset): status 1 (playing),
    /// the rest zero but the game's `extra` value.
    pub fn reset_super_world_progress(
        &self,
        maker_id: &str,
        id: &str,
        extra: u64,
    ) -> Result<Value, StoreError> {
        self.set_super_world_progress(maker_id, id, &json!({ "status": 1, "extra": extra }))
    }

    /// The worlds `maker_id` has a play record of, the latest played first (Course World's "My
    /// Info" world list).
    pub fn super_worlds_played(&self, maker_id: &str) -> Vec<Summary> {
        let g = self.superworlds.lock().unwrap();
        let Some(m) = g.progress.get(maker_id) else {
            return Vec::new();
        };
        let mut ids: Vec<(&String, u64)> = m
            .iter()
            .map(|(id, v)| (id, v["updated"].as_u64().unwrap_or(0)))
            .collect();
        ids.sort_by_key(|(_, t)| std::cmp::Reverse(*t));
        ids.into_iter()
            .filter_map(|(id, _)| {
                g.pool
                    .iter()
                    .find(|w| &w.id == id)
                    .or_else(|| g.held.get(id).map(|w| &w.summary))
                    .cloned()
            })
            .collect()
    }
}

fn json_resp(status: u16, v: Value) -> Response {
    (status, "application/json", serde_json::to_vec(&v).unwrap())
}

fn err(status: u16, msg: &str) -> Response {
    json_resp(status, json!({ "error": msg }))
}

fn store_err(e: StoreError) -> Response {
    match e {
        StoreError::NotFound => err(404, "not found"),
        StoreError::Forbidden => err(403, "forbidden"),
        StoreError::Bad(m) | StoreError::NotACourse(m) => err(400, &m),
        StoreError::TooLarge => err(413, "too large"),
        StoreError::Io(m) => err(500, &m),
    }
}

/// Longest a client may wait for a world the source is asked for.
const WAIT_MAX_MS: u64 = 25_000;

/// The Super World routes (see the table in the crate docs); None for any other request. `who`: the
/// signed-in, unbanned account.
pub(crate) fn route(
    store: &Store,
    method: &str,
    parts: &[&str],
    q: &HashMap<String, String>,
    who: Option<&Account>,
    body: &[u8],
) -> Option<Response> {
    let n = |k: &str, d: u64| q.get(k).and_then(|v| v.parse().ok()).unwrap_or(d);
    let sign_in = || err(401, "sign in");
    let list = |worlds: Vec<Summary>| {
        json_resp(
            200,
            json!({ "super_worlds": worlds.iter().map(Summary::to_json).collect::<Vec<_>>() }),
        )
    };
    Some(match (method, parts) {
        ("GET", ["api", "super_worlds"]) => {
            let (offset, limit) = (n("offset", 0) as usize, n("limit", 50).min(100) as usize);
            let wait = Duration::from_millis(n("wait_ms", 0).min(WAIT_MAX_MS));
            let mut page = store.super_world_page(offset, limit);
            // (an empty pool the source is asked for: the first answer, if it comes in time)
            let t0 = Instant::now();
            while page.is_empty() && offset == 0 && t0.elapsed() < wait {
                std::thread::sleep(Duration::from_millis(100));
                if store.super_worlds_status().len > 0 {
                    page = store.super_world_page(offset, limit);
                }
            }
            list(page)
        }
        ("POST", ["api", "super_worlds"]) => {
            let Some(a) = who else { return Some(sign_in()) };
            let ids: Option<Vec<u32>> = q
                .get("data_ids")
                .map(String::as_str)
                .unwrap_or("")
                .split(',')
                .filter(|s| !s.is_empty())
                .map(|s| s.parse().ok())
                .collect();
            let Some(ids) = ids else {
                return Some(err(400, "data_ids: numbers"));
            };
            match store.upload_super_world(
                a,
                &ids,
                n("worlds", 0) as u8,
                n("planet_type", 0) as u8,
                n("version", 3) as u32,
                body,
            ) {
                Ok(s) => json_resp(201, s.to_json()),
                Err(e) => store_err(e),
            }
        }
        ("DELETE", ["api", "super_worlds", "mine"]) => {
            let Some(a) = who else { return Some(sign_in()) };
            match store.delete_own_super_world(&a.maker_id) {
                Ok(id) => json_resp(200, json!({ "deleted": id })),
                Err(e) => store_err(e),
            }
        }
        ("GET", ["api", "super_worlds", id]) => {
            let wait = Duration::from_millis(n("wait_ms", 0).min(WAIT_MAX_MS));
            match store.super_world_wait(id, wait) {
                Some(w) => json_resp(200, w.to_json()),
                None => err(404, "no such world (yet)"),
            }
        }
        ("GET", ["api", "super_worlds", id, "map"]) => match store.super_world_map(id) {
            Some(b) => (200, "application/octet-stream", b),
            None => err(404, "no map"),
        },
        ("GET", ["api", "super_worlds", id, "thumbnail"]) => {
            match store.super_world_thumbnail(id) {
                Some(b) => (200, "application/octet-stream", b),
                None => err(404, "no thumbnail"),
            }
        }
        ("GET", ["api", "super_worlds", id, "progress"]) => match who {
            Some(a) => json_resp(200, store.super_world_progress(&a.maker_id, id)),
            None => sign_in(),
        },
        ("POST", ["api", "super_worlds", id, "progress"]) => match who {
            Some(a) => {
                let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
                match store.set_super_world_progress(&a.maker_id, id, &v) {
                    Ok(r) => json_resp(200, r),
                    Err(e) => store_err(e),
                }
            }
            None => sign_in(),
        },
        ("POST", ["api", "super_worlds", id, "progress", "reset"]) => match who {
            Some(a) => {
                let v: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
                let extra = v["extra"].as_u64().unwrap_or(0);
                match store.reset_super_world_progress(&a.maker_id, id, extra) {
                    Ok(r) => json_resp(200, r),
                    Err(e) => store_err(e),
                }
            }
            None => sign_in(),
        },
        ("GET", ["api", "makers", id, "super_worlds"]) => list(store.super_worlds_played(id)),
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_and_timestamps() {
        assert_eq!(base64_decode("TWFu").unwrap(), b"Man");
        assert_eq!(base64_decode("TWE=").unwrap(), b"Ma");
        assert_eq!(base64_decode("TQ==").unwrap(), b"M");
        assert!(base64_decode("T*Q=").is_none());
        assert_eq!(timestamp(1_758_838_187), "20250925220947");
        assert_eq!(timestamp(0), "19700101000000");
    }

    #[test]
    fn world_ids_are_safe_in_paths() {
        assert!(is_world_id("c0952b3339712eb7_20250925220946794214"));
        assert!(!is_world_id("../x"));
        assert!(!is_world_id(""));
        assert!(!is_world_id("pool.json"));
    }

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("mmk-sw-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        d
    }

    fn call(
        store: &Store,
        method: &str,
        url: &str,
        token: Option<&str>,
        body: &[u8],
    ) -> (u16, Value) {
        let headers: Vec<(String, String)> = token
            .map(|t| vec![("Authorization".to_string(), format!("Bearer {t}"))])
            .unwrap_or_default();
        let (st, _, b) = crate::handle(store, method, url, &headers, body);
        (st, serde_json::from_slice(&b).unwrap_or(Value::Null))
    }

    #[test]
    fn players_upload_play_and_delete_their_worlds() {
        let dir = tmp("upload");
        let store = Store::open(&dir).unwrap();
        let account = |name: &str| {
            let (_, v) = call(
                &store,
                "POST",
                "/api/accounts",
                None,
                format!(r#"{{"name":"{name}"}}"#).as_bytes(),
            );
            (
                v["token"].as_str().unwrap().to_string(),
                v["account"]["maker_id"].as_str().unwrap().to_string(),
            )
        };
        let (ada, ada_id) = account("Ada");
        let (bob, bob_id) = account("Bob");
        let course = |style: &str, token: &str| -> u64 {
            let data = mmk_formats::smm2::Course::blank(style).to_bytes().unwrap();
            let (st, _, b) = crate::handle(
                &store,
                "POST",
                "/api/courses",
                &[("Authorization".into(), format!("Bearer {token}"))],
                &data,
            );
            assert_eq!(st, 201);
            let code = serde_json::from_slice::<Value>(&b).unwrap()["code"]
                .as_str()
                .unwrap()
                .to_string();
            store.get(&code).unwrap().data_id as u64
        };
        let (c1, c2, theirs) = (course("M1", &ada), course("M3", &ada), course("WU", &bob));
        let mut body = vec![7u8; MAP_SIZE];
        body.extend(vec![0xFFu8; THUMBNAIL_SIZE]);

        // checks: signed in, the uploader's own courses, 1 to 8 worlds, the map's size
        let url = |ids: &str, worlds: u8| {
            format!("/api/super_worlds?data_ids={ids}&worlds={worlds}&planet_type=2&version=3")
        };
        assert_eq!(
            call(&store, "POST", &url(&format!("{c1}"), 1), None, &body).0,
            401
        );
        assert_eq!(
            call(
                &store,
                "POST",
                &url(&format!("{c1},{theirs}"), 1),
                Some(&ada),
                &body
            )
            .0,
            400
        );
        assert_eq!(
            call(&store, "POST", &url(&format!("{c1}"), 9), Some(&ada), &body).0,
            400
        );
        assert_eq!(
            call(
                &store,
                "POST",
                &url(&format!("{c1}"), 1),
                Some(&ada),
                &body[..10]
            )
            .0,
            400
        );

        let (st, w) = call(
            &store,
            "POST",
            &url(&format!("{c2},{c1}"), 1),
            Some(&ada),
            &body,
        );
        assert_eq!(st, 201, "{w}");
        let id = w["id"].as_str().unwrap().to_string();
        assert!(
            id.starts_with(&format!("{:016x}_", player_id(&ada_id))),
            "{id}"
        );
        assert_eq!(id.len(), 16 + 1 + 14 + 6);
        assert_eq!(
            (
                w["levels"].as_u64(),
                w["planet_type"].as_u64(),
                w["own"].as_bool()
            ),
            (Some(2), Some(2), Some(true))
        );
        // the maker's world, their record and Course World's list (own uploads first)
        assert_eq!(
            call(&store, "GET", "/api/me", Some(&ada), b"").1["super_world_id"],
            id.as_str()
        );
        assert_eq!(
            call(&store, "GET", &format!("/api/makers/{ada_id}"), None, b"").1["super_world_id"],
            id.as_str()
        );
        assert_eq!(
            call(&store, "GET", &format!("/api/makers/{bob_id}"), None, b"").1["super_world_id"],
            ""
        );
        store
            .merge_super_worlds(vec![Summary {
                own: false,
                id: "ffffffffffffffff_29990101000000000000".into(),
                created: u64::MAX,
                ..w_summary(&w)
            }])
            .unwrap();
        let list = call(&store, "GET", "/api/super_worlds", None, b"").1;
        assert_eq!(list["super_worlds"][0]["id"], id.as_str());
        assert_eq!(list["super_worlds"].as_array().unwrap().len(), 2);
        let d = call(&store, "GET", &format!("/api/super_worlds/{id}"), None, b"").1;
        let codes: Vec<String> = d["courses"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c["data_id"].to_string())
            .collect();
        assert_eq!(codes, [c2.to_string(), c1.to_string()]);
        let (st, _, map) = crate::handle(
            &store,
            "GET",
            &format!("/api/super_worlds/{id}/map"),
            &[],
            b"",
        );
        assert_eq!((st, map), (200, vec![7u8; MAP_SIZE]));
        let (st, _, thumb) = crate::handle(
            &store,
            "GET",
            &format!("/api/super_worlds/{id}/thumbnail"),
            &[],
            b"",
        );
        assert_eq!((st, thumb.len()), (200, THUMBNAIL_SIZE));

        // progress: per player; none yet, saved, started over
        let p = format!("/api/super_worlds/{id}/progress");
        assert_eq!(call(&store, "GET", &p, None, b"").0, 401);
        let none = call(&store, "GET", &p, Some(&bob), b"").1;
        assert_eq!(
            (
                none["held"].as_bool(),
                none["status"].as_u64(),
                none["cleared"].as_str()
            ),
            (Some(false), Some(0), Some("0"))
        );
        let save = br#"{"status":1,"counter_a":5,"counter_b":2,"score":12345,"world":1,"x":3,"y":2,
                        "cleared":"18446744073709551615","flags":"258","extra":7}"#;
        assert_eq!(call(&store, "POST", &p, Some(&bob), save).0, 200);
        let got = call(&store, "GET", &p, Some(&bob), b"").1;
        assert_eq!(got["held"], true);
        assert_eq!(
            (
                got["score"].as_u64(),
                got["x"].as_u64(),
                got["world"].as_u64()
            ),
            (Some(12345), Some(3), Some(1))
        );
        assert_eq!(
            got["cleared"], "18446744073709551615",
            "64 bits, kept whole"
        );
        assert_eq!(call(&store, "GET", &p, Some(&ada), b"").1["held"], false);
        let played = call(
            &store,
            "GET",
            &format!("/api/makers/{bob_id}/super_worlds"),
            None,
            b"",
        )
        .1;
        assert_eq!(played["super_worlds"][0]["id"], id.as_str());
        let r = call(
            &store,
            "POST",
            &format!("{p}/reset"),
            Some(&bob),
            br#"{"extra":9}"#,
        )
        .1;
        assert_eq!(
            (
                r["status"].as_u64(),
                r["score"].as_u64(),
                r["extra"].as_u64()
            ),
            (Some(1), Some(0), Some(9))
        );

        // kept after a restart; a new upload replaces the old one; deleted
        drop(store);
        let store = Store::open(&dir).unwrap();
        assert_eq!(store.own_super_world(&ada_id).as_deref(), Some(id.as_str()));
        assert_eq!(store.super_world_progress(&bob_id, &id)["extra"], 9);
        std::thread::sleep(std::time::Duration::from_millis(2));
        let (st, w2) = call(
            &store,
            "POST",
            &url(&format!("{c1}"), 1),
            Some(&ada),
            &body[..MAP_SIZE],
        );
        assert_eq!(st, 201);
        let id2 = w2["id"].as_str().unwrap().to_string();
        assert_ne!(id2, id);
        assert!(store.super_world(&id).is_none());
        assert_eq!(
            call(
                &store,
                "GET",
                &format!("/api/super_worlds/{id2}/thumbnail"),
                None,
                b""
            )
            .0,
            404
        );
        assert_eq!(
            call(&store, "DELETE", "/api/super_worlds/mine", Some(&ada), b"").1["deleted"],
            id2.as_str()
        );
        assert_eq!(
            call(&store, "DELETE", "/api/super_worlds/mine", Some(&ada), b"").1["deleted"],
            Value::Null
        );
        assert_eq!(
            call(
                &store,
                "GET",
                &format!("/api/super_worlds/{id2}"),
                None,
                b""
            )
            .0,
            404
        );
        assert_eq!(
            call(&store, "GET", "/api/me", Some(&ada), b"").1["super_world_id"],
            ""
        );
    }

    fn w_summary(v: &Value) -> Summary {
        Summary::from_json(v).unwrap()
    }

    #[test]
    fn the_pool_keeps_own_worlds_first_then_the_newest() {
        let w = |id: &str, created: u64, own: bool| Summary {
            id: id.into(),
            maker_id: String::new(),
            maker_name: String::new(),
            worlds: 1,
            levels: 1,
            planet_type: 0,
            created,
            unk5: 3,
            unk6: 1,
            unk7: 1,
            own,
        };
        let mut pool: Vec<Summary> = (0..POOL_MAX as u64 + 5)
            .map(|i| w(&format!("s{i}"), i, false))
            .collect();
        pool.push(w("mine", 1, true));
        order(&mut pool);
        assert_eq!(pool.len(), POOL_MAX + 1);
        assert_eq!(pool[0].id, "mine");
        assert_eq!(pool[1].id, format!("s{}", POOL_MAX + 4));
    }
}
