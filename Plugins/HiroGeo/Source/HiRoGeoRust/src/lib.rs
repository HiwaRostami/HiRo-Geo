mod ffi;
mod geometry;
mod parse;
pub mod unreal_compat;

use std::collections::BTreeMap;

pub type Tags = BTreeMap<String, String>;

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Point {
    pub lon: f64,
    pub lat: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct LocalPoint {
    pub x: f64,
    pub y: f64,
}

impl Point {
    fn valid(self) -> bool {
        (-180.0..=180.0).contains(&self.lon) && (-90.0..=90.0).contains(&self.lat)
    }
}

pub fn to_local(point: Point, origin: Point) -> Option<LocalPoint> {
    if !point.valid() || !origin.valid() {
        return None;
    }
    const RADIUS: f64 = 6_371_000.0;
    Some(LocalPoint {
        x: (RADIUS * ((point.lon - origin.lon) * std::f64::consts::PI / 180.0)
            * (origin.lat * std::f64::consts::PI / 180.0).cos()) as f32 as f64,
        y: (RADIUS * ((point.lat - origin.lat) * std::f64::consts::PI / 180.0)) as f32 as f64,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ElementKind {
    Node = 1,
    Way = 2,
    Relation = 3,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Member {
    pub kind: ElementKind,
    pub id: i64,
    pub role: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Node {
    pub point: Point,
    pub tags: Tags,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Way {
    pub refs: Vec<i64>,
    pub tags: Tags,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Relation {
    pub members: Vec<Member>,
    pub tags: Tags,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Osm {
    pub nodes: BTreeMap<i64, Node>,
    pub ways: BTreeMap<i64, Way>,
    pub relations: BTreeMap<i64, Relation>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum FeatureKind {
    Building = 1,
    BuildingPart = 2,
    Road = 3,
    Water = 4,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Ring {
    pub polygon: u32,
    pub inner: bool,
    pub node_ids: Vec<i64>,
    pub points: Vec<Point>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Feature {
    pub kind: FeatureKind,
    pub source: ElementKind,
    pub id: i64,
    pub rings: Vec<Ring>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Extract {
    pub osm: Osm,
    pub features: Vec<Feature>,
    pub skipped_geometry: u64,
}

pub fn parse_xml(input: &[u8]) -> Result<Extract, String> {
    parse::xml(input).map(geometry::extract)
}

pub fn parse_pbf(input: &[u8]) -> Result<Extract, String> {
    parse::pbf(input).map(geometry::extract)
}
