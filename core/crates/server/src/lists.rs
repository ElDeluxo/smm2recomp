//! Course World's Popular, Hot and New lists from an upstream source ([`crate::upstream`]): the
//! source's live lists, kept here and served page by page in place of the server's own sorting of
//! the courses it holds, which follow them (and are all there is until the source has answered, or
//! when it fails). The source has no hot list: Hot is Popular's courses, the latest uploads first.
//! The fetcher fills the lists and refreshes them; every page asked for tells it, so a stale list
//! is refreshed in the background and one read near its end is extended ahead of the player.
//!
//! Pictures: TGRCode gives one a request, one request in 6 s at most, while the game shows three cards
//! at a time and asks for each card's picture as it scrolls into view. So the pictures are a stock
//! fetched ahead of the player: before any page, the first ones of each list; while a client browses,
//! the cards around the last one whose picture it asked for (where the player is) and well past it,
//! then the rest of the page and the next ([`Store::list_thumbnails_ahead`]).
//!
//! New: TGRCode's latest courses cover only the last ten minutes or so of uploads (about ten to twenty
//! courses, the same for a few minutes). The server keeps the ones it saw (`lists-new.json`, the
//! newest `new_max`), so the list grows across sessions, and Popular's listed courses and the ones held
//! follow them by upload time.
//!
//! A listed course the server doesn't hold is answered from the list (`GET /api/courses/<code>`),
//! its thumbnails are fetched like a held upstream course's, and its data is fetched and imported
//! when a client asks for it ([`Store::data_wait`]).

use std::collections::{HashMap, HashSet};
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::{Meta, Store, StoreError};

/// Which list.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Popular,
    New,
}

impl Kind {
    pub const ALL: [Kind; 2] = [Kind::Popular, Kind::New];

    fn index(self) -> usize {
        self as usize
    }
}

/// How a list is served (a `sort=` of `/api/courses`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum View {
    Popular,
    /// Popular's courses, the latest uploads first (SMM2's Hot Courses).
    Hot,
    New,
}

impl View {
    pub fn of_sort(sort: &str) -> Option<View> {
        match sort {
            "popular" => Some(View::Popular),
            "hot" => Some(View::Hot),
            "new" => Some(View::New),
            _ => None,
        }
    }

    pub fn kind(self) -> Kind {
        match self {
            View::Popular | View::Hot => Kind::Popular,
            View::New => Kind::New,
        }
    }
}

/// Pictures fetched before any page is asked for, round by round: the first cards of Hot and New
/// (the game shows three, and asks for five at the start), then of Popular (its tab shows one
/// difficulty of the four mixed); then more of each.
const PREWARM: [&[(View, usize)]; 2] = [
    &[(View::Hot, 6), (View::New, 6), (View::Popular, 12)],
    &[(View::Hot, 20), (View::New, 30), (View::Popular, 40)],
];

/// While a client browses a list: the pictures of the cards from this many before the last one it
/// asked for to this many after it come first (the stock ahead of the player).
const STOCK_BEHIND: usize = 3;
const STOCK_AHEAD: usize = 40;

/// Where the server keeps the New list it saw.
pub(crate) const NEW_FILE: &str = "lists-new.json";

/// A page asked for this long ago still has its pictures fetched ahead of the background work.
const BROWSING: Duration = Duration::from_secs(120);

/// A course on a list, with the source's difficulty class (0 Easy … 3 Super Expert) if it gave one.
#[derive(Clone, Debug, PartialEq)]
pub struct Listed {
    pub meta: Meta,
    pub class: Option<u8>,
}

impl Listed {
    /// [`Meta::to_json`], its difficulty value inside the source's class.
    pub fn to_json(&self) -> Value {
        let mut v = self.meta.to_json();
        if let Some(c) = self.class {
            v["difficulty"] = in_class(self.meta.difficulty(), c).into();
        }
        v
    }
}

/// A difficulty value moved into class `class`'s range (Course World's bands, world.c `band()`:
/// under 35 Easy, under 75 Normal, under 96 Expert, then Super Expert), so the game shows the
/// class the source gives.
pub fn in_class(v: u8, class: u8) -> u8 {
    let (lo, hi) = match class {
        0 => (0, 34),
        1 => (35, 74),
        2 => (75, 95),
        _ => (96, 100),
    };
    v.clamp(lo, hi)
}

/// A list as it is served: each course with its place on the upstream list (None: not on it).
type Served = Vec<(Option<usize>, Listed)>;

/// What the fetcher sees of a list.
#[derive(Clone, Copy, Debug, Default)]
pub struct Status {
    pub len: usize,
    /// When the source last filled it (None: never).
    pub fetched: Option<Instant>,
    /// When a client last asked for a page of it.
    pub asked: Option<Instant>,
    /// How far into it the pages asked for since it was filled reached.
    pub depth: usize,
}

#[derive(Default)]
struct List {
    courses: Vec<Listed>,
    status: Status,
}

/// The last page a client asked for (its thumbnails are fetched ahead).
struct Page {
    view: View,
    style: Option<String>,
    offset: usize,
    limit: usize,
    /// When the page, or a picture on it, was last asked for.
    asked: Instant,
    /// The place (in the served order) of the last card whose picture a client asked for.
    cursor: Option<usize>,
}

#[derive(Default)]
pub(crate) struct Lists {
    lists: [List; 2],
    page: Option<Page>,
    /// The courses of the source's Super Worlds held ([`crate::superworlds`]), by code.
    worlds: HashMap<String, Listed>,
    /// Courses looked up at the source by their ID (search by ID), by code.
    found: HashMap<String, Listed>,
}

impl Lists {
    fn find(&self, code: &str) -> Option<&Listed> {
        self.lists
            .iter()
            .flat_map(|l| &l.courses)
            .find(|c| c.meta.code == code)
            .or_else(|| self.worlds.get(code))
            .or_else(|| self.found.get(code))
    }

    /// A Super World's courses, listed from now on (a code listed already keeps its record).
    pub(crate) fn register_world_courses(&mut self, courses: impl IntoIterator<Item = Listed>) {
        for c in courses {
            self.worlds.entry(c.meta.code.clone()).or_insert(c);
        }
    }

    /// The listed courses of `kind` in `style` (None: any), with their places on the whole list.
    fn of_style<'a>(
        &'a self,
        kind: Kind,
        style: Option<&'a str>,
    ) -> impl Iterator<Item = (usize, &'a Listed)> + 'a {
        self.lists[kind.index()]
            .courses
            .iter()
            .enumerate()
            .filter(move |(_, c)| style.is_none_or(|s| c.meta.style == s))
    }

    /// [`Self::of_style`] in the order `view` serves them (Hot: the latest uploads first).
    fn ordered<'a>(&'a self, view: View, style: Option<&'a str>) -> Vec<(usize, &'a Listed)> {
        let mut v: Vec<(usize, &Listed)> = self.of_style(view.kind(), style).collect();
        if view == View::Hot {
            v.sort_by_key(|(_, c)| std::cmp::Reverse(c.meta.uploaded));
        }
        v
    }

    /// All of `view` as it is served, before paging: the list's courses (with their places on it), then
    /// the courses `held` (sorted for the view) it doesn't list; New: Popular's listed courses too, and
    /// everything by upload time, newest first.
    fn served(&self, view: View, style: Option<&str>, held: &[Meta]) -> Served {
        let mut all: Served = self
            .ordered(view, style)
            .into_iter()
            .map(|(i, c)| (Some(i), c.clone()))
            .collect();
        if view == View::New {
            all.extend(
                self.of_style(Kind::Popular, style)
                    .map(|(_, c)| (None, c.clone())),
            );
        }
        all.extend(held.iter().map(|m| {
            (
                None,
                Listed {
                    meta: m.clone(),
                    class: None,
                },
            )
        }));
        let mut seen = HashSet::new();
        all.retain(|(_, c)| seen.insert(c.meta.code.clone()));
        if view == View::New {
            all.sort_by_key(|(_, c)| std::cmp::Reverse(c.meta.uploaded));
        }
        all
    }

    /// New as the server last saw it (what [`NEW_FILE`] holds), until the source fills it.
    pub(crate) fn load_new(&mut self, v: &Value) {
        let courses = v["courses"].as_array().map(|a| {
            a.iter()
                .filter_map(|c| {
                    Some(Listed {
                        meta: Meta::from_json(c)?,
                        class: c["class"].as_u64().filter(|&x| x <= 3).map(|x| x as u8),
                    })
                })
                .collect()
        });
        self.lists[Kind::New.index()].courses = courses.unwrap_or_default();
    }
}

pub(crate) type ListsWanted = Box<dyn Fn() + Send + Sync>;
pub(crate) type CourseWanted = Box<dyn Fn(&str) + Send + Sync>;

impl Store {
    /// Serve Course World's Popular and New from the upstream lists (once per store); `f` is told
    /// whenever a page of one is asked for (the fetcher refreshes or extends it); never blocks.
    pub fn on_lists(&self, f: impl Fn() + Send + Sync + 'static) {
        let _ = self.lists_wanted.set(Box::new(f));
    }

    /// Who fetches a listed course the store doesn't hold when its data is asked for (once per
    /// store); never blocks.
    pub fn on_course(&self, f: impl Fn(&str) + Send + Sync + 'static) {
        let _ = self.course_wanted.set(Box::new(f));
    }

    /// Who looks up a course nobody listed by its ID when a client asks for it (once per store);
    /// never blocks.
    pub fn on_course_info(&self, f: impl Fn(&str) + Send + Sync + 'static) {
        let _ = self.info_wanted.set(Box::new(f));
    }

    /// A course the source found by its ID: listed from now on (its data and thumbnails are fetched
    /// as for any listed course).
    pub fn add_found(&self, course: Listed) {
        let mut g = self.lists.lock().unwrap();
        g.found.insert(course.meta.code.clone(), course);
    }

    /// The source has no course `code` (a lookup by ID that failed).
    pub fn note_info_failed(&self, code: &str) {
        self.info_failed
            .lock()
            .unwrap()
            .insert(code.to_string(), Instant::now());
    }

    /// [`Self::course_json`], and for a course nobody listed, a lookup at the source by its ID,
    /// waited for up to `wait` (none when there is no source, or it found nothing in the last
    /// minute).
    pub fn course_json_wait(&self, code: &str, wait: Duration) -> Option<Value> {
        if let Some(v) = self.course_json(code) {
            return Some(v);
        }
        let valid = code.len() == 11
            && code.bytes().enumerate().all(|(i, b)| {
                if i == 3 || i == 7 {
                    b == b'-'
                } else {
                    b.is_ascii_alphanumeric()
                }
            });
        let fetch = self.info_wanted.get()?;
        let recent = |s: &Self| {
            s.info_failed
                .lock()
                .unwrap()
                .get(code)
                .is_some_and(|t| t.elapsed() < Duration::from_secs(60))
        };
        if !valid || recent(self) {
            return None;
        }
        fetch(code);
        let t0 = Instant::now();
        while t0.elapsed() < wait {
            std::thread::sleep(Duration::from_millis(50));
            if let Some(v) = self.course_json(code) {
                return Some(v);
            }
            if recent(self) {
                return None;
            }
        }
        None
    }

    pub fn list_status(&self, kind: Kind) -> Status {
        let g = self.lists.lock().unwrap();
        let l = &g.lists[kind.index()];
        Status {
            len: l.courses.len(),
            ..l.status
        }
    }

    /// The list `kind`, now the source's `courses` (in its order, a code once).
    pub fn set_list(&self, kind: Kind, courses: Vec<Listed>) {
        let mut seen = HashSet::new();
        let courses = courses
            .into_iter()
            .filter(|c| seen.insert(c.meta.code.clone()))
            .collect();
        let mut g = self.lists.lock().unwrap();
        let l = &mut g.lists[kind.index()];
        l.courses = courses;
        l.status.fetched = Some(Instant::now());
        l.status.depth = 0;
    }

    /// More of the source's courses at the end of list `kind` (codes listed already left out);
    /// returns how many were new.
    pub fn extend_list(&self, kind: Kind, more: Vec<Listed>) -> usize {
        let mut g = self.lists.lock().unwrap();
        let l = &mut g.lists[kind.index()];
        let mut seen: HashSet<String> = l.courses.iter().map(|c| c.meta.code.clone()).collect();
        let before = l.courses.len();
        l.courses.extend(
            more.into_iter()
                .filter(|c| seen.insert(c.meta.code.clone())),
        );
        l.courses.len() - before
    }

    /// The source's latest courses into list `kind`, newest first: a code listed already takes the
    /// new record (its statistics), and the list keeps at most `max`.
    pub fn merge_newest(&self, kind: Kind, latest: Vec<Listed>, max: usize) {
        let mut g = self.lists.lock().unwrap();
        let l = &mut g.lists[kind.index()];
        for c in latest {
            match l.courses.iter_mut().find(|x| x.meta.code == c.meta.code) {
                Some(x) => *x = c,
                None => l.courses.push(c),
            }
        }
        l.courses
            .sort_by_key(|c| std::cmp::Reverse(c.meta.uploaded));
        l.courses.truncate(max);
        l.status.fetched = Some(Instant::now());
        if kind == Kind::New {
            let v = serde_json::json!({
                "courses": l.courses.iter().map(|c| {
                    let mut j = c.meta.to_json();
                    j["class"] = c.class.into();
                    j
                }).collect::<Vec<_>>()
            });
            drop(g);
            let path = self.dir.join(NEW_FILE);
            let tmp = path.with_extension("tmp");
            if let Err(e) =
                std::fs::write(&tmp, v.to_string()).and_then(|_| std::fs::rename(&tmp, &path))
            {
                eprintln!("[lists] {}: {e}", path.display());
            }
        }
    }

    /// A course on either list.
    pub fn listed(&self, code: &str) -> Option<Listed> {
        self.lists.lock().unwrap().find(code).cloned()
    }

    /// A course's metadata (`GET /api/courses/<code>`): the one held, or a listed one the server
    /// doesn't hold (with its list's difficulty class either way).
    pub fn course_json(&self, code: &str) -> Option<Value> {
        let listed = self.listed(code);
        match (self.get(code), listed) {
            (Some(meta), l) => Some(
                Listed {
                    meta,
                    class: l.and_then(|l| l.class),
                }
                .to_json(),
            ),
            (None, l) => l.map(|l| l.to_json()),
        }
    }

    /// A page of Popular, Hot or New (`/api/courses?sort=popular|hot|new` without a text search):
    /// the upstream list's courses, then the ones held (Popular, Hot: by likes; New: all of them by
    /// upload time, the list's among them). None: no upstream lists.
    pub fn list_page(
        &self,
        view: View,
        style: Option<&str>,
        offset: usize,
        limit: usize,
    ) -> Option<Vec<Value>> {
        let tell = self.lists_wanted.get()?;
        let kind = view.kind();
        let limit = limit.min(100);
        let all = self.served(view, style);
        let page = {
            let page: Served = all.iter().skip(offset).take(limit).cloned().collect();
            let mut g = self.lists.lock().unwrap();
            let listed = all.iter().filter(|(i, _)| i.is_some()).count();
            // how far into the whole list this page reached (past its end: all of it; Hot, not in
            // the list's order: as far as the page goes)
            let reached = if listed <= offset + limit {
                g.lists[kind.index()].courses.len()
            } else if view == View::Hot {
                offset + limit
            } else {
                page.iter()
                    .filter_map(|(i, _)| *i)
                    .max()
                    .map_or(0, |i| i + 1)
            };
            let st = &mut g.lists[kind.index()].status;
            st.asked = Some(Instant::now());
            st.depth = st.depth.max(reached);
            // (the same page asked for again keeps where the player was on it)
            let same =
                |p: &Page| p.view == view && p.style.as_deref() == style && p.offset == offset;
            let cursor = g.page.as_ref().filter(|p| same(p)).and_then(|p| p.cursor);
            g.page = Some(Page {
                view,
                style: style.map(str::to_string),
                offset,
                limit,
                asked: Instant::now(),
                cursor,
            });
            page
        };
        tell();
        Some(page.iter().map(|(_, c)| c.to_json()).collect())
    }

    /// [`Lists::served`] with the courses held.
    fn served(&self, view: View, style: Option<&str>) -> Served {
        let sort = if view.kind() == Kind::Popular {
            "popular"
        } else {
            "new"
        };
        let held = self.sorted("", style, sort);
        self.lists.lock().unwrap().served(view, style, &held)
    }

    /// A client asked for the picture of course `code`: if it is on the list looked at, that is where
    /// the player is (the game asks for a card's picture as the card scrolls into view).
    pub(crate) fn note_picture_asked(&self, code: &str) {
        let page = self
            .lists
            .lock()
            .unwrap()
            .page
            .as_ref()
            .map(|p| (p.view, p.style.clone()));
        let Some((view, style)) = page else { return };
        let all = self.served(view, style.as_deref());
        let Some(i) = all.iter().position(|(_, c)| c.meta.code == code) else {
            return;
        };
        let mut g = self.lists.lock().unwrap();
        if let Some(p) = g.page.as_mut()
            && p.view == view
            && p.style == style
        {
            p.cursor = Some(i);
            p.asked = Instant::now();
        }
    }

    /// Upstream courses on the lists whose pictures aren't held, the ones wanted soonest first; at
    /// most `limit`. While a client browses a page: the cards around where the player is
    /// ([`STOCK_BEHIND`], [`STOCK_AHEAD`]; before any picture on the page was asked for, from its
    /// start), then the rest of the page and the next; a page left a while ago: the cards around the
    /// player only. Before any page: the first ones of each list ([`PREWARM`]).
    pub fn list_thumbnails_ahead(&self, limit: usize) -> Vec<String> {
        self.pictures_ahead(limit, PREWARM.len())
    }

    /// [`Self::list_thumbnails_ahead`] that go before the start's searches for Endless courses: all
    /// of them while a client browses a page, else the first round of [`PREWARM`] or the cards
    /// around the player.
    pub fn list_thumbnails_soon(&self, limit: usize) -> Vec<String> {
        self.pictures_ahead(limit, 1)
    }

    fn pictures_ahead(&self, limit: usize, rounds: usize) -> Vec<String> {
        let page = self.lists.lock().unwrap().page.as_ref().map(|p| {
            (
                p.view,
                p.style.clone(),
                p.offset,
                p.limit,
                p.cursor,
                p.asked.elapsed() < BROWSING,
            )
        });
        let codes: Vec<String> = match page {
            Some((view, style, offset, n, cursor, browsing)) => {
                let all = self.served(view, style.as_deref());
                let from = cursor.map_or(offset, |c| c.saturating_sub(STOCK_BEHIND));
                let to = cursor.map_or(offset, |c| c + 1) + STOCK_AHEAD;
                let rest = if browsing {
                    offset..offset + 2 * n
                } else {
                    0..0
                };
                let mut seen = HashSet::new();
                (from..to)
                    .chain(rest)
                    .filter(|&i| seen.insert(i))
                    .filter_map(|i| all.get(i))
                    .filter(|(_, c)| c.meta.upstream)
                    .map(|(_, c)| c.meta.code.clone())
                    .collect()
            }
            None => {
                let views: Vec<(View, Served)> = [View::Hot, View::New, View::Popular]
                    .into_iter()
                    .map(|v| (v, self.served(v, None)))
                    .collect();
                let mut seen = HashSet::new();
                PREWARM[..rounds]
                    .iter()
                    .flat_map(|round| round.iter())
                    .flat_map(|&(view, n)| {
                        let all = &views.iter().find(|(v, _)| *v == view).unwrap().1;
                        all.iter()
                            .take(n)
                            .filter(|(_, c)| c.meta.upstream)
                            .map(|(_, c)| c.meta.code.clone())
                            .collect::<Vec<_>>()
                    })
                    .filter(|c| seen.insert(c.clone()))
                    .collect()
            }
        };
        codes
            .into_iter()
            .filter(|c| !self.has_thumbnail(c, false))
            .take(limit)
            .collect()
    }

    /// Whether a client is looking at the lists (a page asked for in the last two minutes, or none
    /// yet: the first pictures of each are fetched for it): their pictures go before background work.
    pub fn browsing(&self) -> bool {
        self.lists
            .lock()
            .unwrap()
            .page
            .as_ref()
            .is_none_or(|p| p.asked.elapsed() < BROWSING)
    }

    /// A course's data. A listed course the store doesn't hold is fetched from the source and
    /// imported, waiting up to `wait` for it (not found meanwhile).
    pub fn data_wait(&self, code: &str, wait: Duration) -> Result<Vec<u8>, StoreError> {
        if self.get(code).is_some() {
            return self.data(code);
        }
        if self.listed(code).is_none() {
            return Err(StoreError::NotFound);
        }
        let Some(fetch) = self.course_wanted.get() else {
            return Err(StoreError::NotFound);
        };
        fetch(code);
        let t0 = Instant::now();
        while t0.elapsed() < wait {
            std::thread::sleep(Duration::from_millis(50).min(wait));
            if self.get(code).is_some() {
                return self.data(code);
            }
        }
        Err(StoreError::NotFound)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_class_moves_the_value_into_its_band() {
        assert_eq!(in_class(20, 0), 20);
        assert_eq!(in_class(50, 0), 34);
        assert_eq!(in_class(50, 2), 75);
        assert_eq!(in_class(99, 1), 74);
        assert_eq!(in_class(0, 3), 96);
    }
}
