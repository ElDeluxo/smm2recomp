//! Course World's Popular, Hot and New lists from an upstream source ([`crate::upstream`]): the
//! source's live lists, kept here and served page by page in place of the server's own sorting of
//! the courses it holds, which follow them (and are all there is until the source has answered, or
//! when it fails). The source has no hot list: Hot is Popular's courses, the latest uploads first.
//! The fetcher fills the lists and refreshes them; every page asked for tells it, so a stale list
//! is refreshed in the background and one read near its end is extended ahead of the player.
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

/// Pictures fetched before any page is asked for: the first courses of Hot and New (the game shows
/// five at a time), then of Popular (its tab shows one difficulty of the four mixed).
const PREWARM: [(View, usize); 3] = [(View::Hot, 8), (View::New, 8), (View::Popular, 32)];

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
    asked: Instant,
}

#[derive(Default)]
pub(crate) struct Lists {
    lists: [List; 2],
    page: Option<Page>,
    /// The courses of the source's Super Worlds held ([`crate::superworlds`]), by code.
    worlds: HashMap<String, Listed>,
}

impl Lists {
    fn find(&self, code: &str) -> Option<&Listed> {
        self.lists
            .iter()
            .flat_map(|l| &l.courses)
            .find(|c| c.meta.code == code)
            .or_else(|| self.worlds.get(code))
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
        let sort = if kind == Kind::Popular {
            "popular"
        } else {
            "new"
        };
        let held = self.sorted("", style, sort);
        let limit = limit.min(100);
        let page = {
            let mut g = self.lists.lock().unwrap();
            let listed: Vec<(usize, Listed)> = g
                .ordered(view, style)
                .into_iter()
                .map(|(i, c)| (i, c.clone()))
                .collect();
            let codes: HashSet<&str> = listed.iter().map(|(_, c)| c.meta.code.as_str()).collect();
            let rest = held
                .iter()
                .filter(|m| !codes.contains(m.code.as_str()))
                .map(|m| Listed {
                    meta: m.clone(),
                    class: None,
                });
            let mut all: Vec<(Option<usize>, Listed)> = listed
                .iter()
                .map(|(i, c)| (Some(*i), c.clone()))
                .chain(rest.map(|c| (None, c)))
                .collect();
            if kind == Kind::New {
                all.sort_by_key(|(_, c)| std::cmp::Reverse(c.meta.uploaded));
            }
            let page: Vec<(Option<usize>, Listed)> =
                all.into_iter().skip(offset).take(limit).collect();
            // how far into the whole list this page reached (past its end: all of it; Hot, not in
            // the list's order: as far as the page goes)
            let reached = if listed.len() <= offset + limit {
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
            g.page = Some(Page {
                view,
                style: style.map(str::to_string),
                offset,
                limit,
                asked: Instant::now(),
            });
            page
        };
        tell();
        Some(page.iter().map(|(_, c)| c.to_json()).collect())
    }

    /// Listed upstream courses whose pictures aren't held, in the order served from the last page
    /// asked for to the end of the page after it; before any page is asked for, the first ones of
    /// each list ([`PREWARM`]). At most `limit`.
    pub fn list_thumbnails_ahead(&self, limit: usize) -> Vec<String> {
        let codes: Vec<String> = {
            let g = self.lists.lock().unwrap();
            let upstream = |c: &&Listed| c.meta.upstream;
            match &g.page {
                Some(p) => g
                    .ordered(p.view, p.style.as_deref())
                    .into_iter()
                    .map(|(_, c)| c)
                    .skip(p.offset)
                    .take(p.limit * 2)
                    .filter(upstream)
                    .map(|c| c.meta.code.clone())
                    .collect(),
                None => PREWARM
                    .iter()
                    .flat_map(|&(view, n)| {
                        g.ordered(view, None)
                            .into_iter()
                            .map(|(_, c)| c)
                            .take(n)
                            .filter(upstream)
                            .map(|c| c.meta.code.clone())
                            .collect::<Vec<_>>()
                    })
                    .collect(),
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
