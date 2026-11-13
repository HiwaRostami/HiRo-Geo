use crate::{ElementKind, Extract, Feature, FeatureKind, Osm, Point, Relation, Ring, Tags};
use std::collections::BTreeSet;

fn tag<'a>(tags: &'a Tags, key: &str) -> &'a str {
    tags.get(key).map_or("", String::as_str)
}

fn classify(tags: &Tags) -> Option<FeatureKind> {
    let present = |key| !matches!(tag(tags, key), "" | "no");
    if present("building:part") {
        Some(FeatureKind::BuildingPart)
    } else if present("building") || tag(tags, "type") == "building" {
        Some(FeatureKind::Building)
    } else if tag(tags, "natural") == "water"
        || matches!(tag(tags, "landuse"), "reservoir" | "basin")
        || tag(tags, "waterway") == "riverbank"
    {
        Some(FeatureKind::Water)
    } else if present("highway") {
        Some(FeatureKind::Road)
    } else {
        None
    }
}

fn ring(osm: &Osm, refs: Vec<i64>, closed: bool) -> Option<Ring> {
    if refs.len() < 2 || (closed && (refs.len() < 4 || refs.first() != refs.last()
        || refs.iter().collect::<BTreeSet<_>>().len() < 3)) {
        return None;
    }
    let points = refs.iter().map(|id| osm.nodes.get(id).map(|node| node.point))
        .collect::<Option<_>>()?;
    Some(Ring { polygon: 0, inner: false, node_ids: refs, points })
}

fn join_rings(osm: &Osm, mut segments: Vec<Vec<i64>>) -> Option<Vec<Ring>> {
    let mut output = Vec::new();
    while let Some(mut refs) = segments.pop() {
        while refs.first() != refs.last() {
            let end = refs.last()?;
            let index = segments.iter().position(|s| s.first() == Some(end) || s.last() == Some(end))?;
            let mut next = segments.swap_remove(index);
            if next.first() != Some(end) { next.reverse(); }
            refs.extend(next.into_iter().skip(1));
        }
        output.push(ring(osm, refs, true)?);
    }
    Some(output)
}

fn contains(ring: &Ring, point: Point) -> bool {
    let mut inside = false;
    for pair in ring.points.windows(2) {
        let [a, b] = [pair[0], pair[1]];
        if (a.lat > point.lat) != (b.lat > point.lat)
            && point.lon < (b.lon - a.lon) * (point.lat - a.lat) / (b.lat - a.lat) + a.lon {
            inside = !inside;
        }
    }
    inside
}

fn area(ring: &Ring) -> f64 {
    ring.points.windows(2).map(|p| p[0].lon * p[1].lat - p[1].lon * p[0].lat)
        .sum::<f64>().abs()
}

fn relation_rings(osm: &Osm, relation: &Relation) -> Option<(Vec<Ring>, Vec<i64>)> {
    let mut outer_ids = Vec::new();
    let mut outer = Vec::new();
    let mut inner = Vec::new();
    for member in &relation.members {
        let target = match member.role.as_str() {
            "" | "outer" | "outline" if member.kind != ElementKind::Node => &mut outer,
            "inner" if member.kind != ElementKind::Node => &mut inner,
            _ => continue,
        };
        if member.kind != ElementKind::Way { return None; }
        let way = osm.ways.get(&member.id)?;
        if way.refs.len() < 2 { return None; }
        target.push(way.refs.clone());
        if member.role != "inner" {
            outer_ids.push(member.id);
        }
    }
    let mut rings = join_rings(osm, outer)?;
    if rings.is_empty() { return None; }
    for (index, ring) in rings.iter_mut().enumerate() {
        ring.polygon = u32::try_from(index).ok()?;
    }
    let outer_count = rings.len();
    for mut hole in join_rings(osm, inner)? {
        let parent = rings[..outer_count].iter()
            .filter(|ring| contains(ring, hole.points[0]))
            .min_by(|a, b| area(a).total_cmp(&area(b)))?;
        hole.polygon = parent.polygon;
        hole.inner = true;
        rings.push(hole);
    }
    Some((rings, outer_ids))
}

pub fn extract(osm: Osm) -> Extract {
    let mut features = Vec::new();
    let mut relation_members = BTreeSet::new();
    let mut skipped_geometry = 0;
    for (&id, relation) in &osm.relations {
        if !matches!(tag(&relation.tags, "type"), "multipolygon" | "building") { continue; }
        let Some(kind) = classify(&relation.tags).filter(|kind| *kind != FeatureKind::Road) else { continue; };
        if let Some((rings, members)) = relation_rings(&osm, relation) {
            relation_members.extend(members.into_iter().map(|member| (member, kind as u32)));
            features.push(Feature { kind, source: ElementKind::Relation, id, rings });
        } else {
            skipped_geometry += 1;
        }
    }
    for (&id, way) in &osm.ways {
        let Some(kind) = classify(&way.tags) else { continue; };
        if relation_members.contains(&(id, kind as u32)) { continue; }
        if let Some(geometry) = ring(&osm, way.refs.clone(), kind != FeatureKind::Road) {
            features.push(Feature { kind, source: ElementKind::Way, id, rings: vec![geometry] });
        } else {
            skipped_geometry += 1;
        }
    }
    Extract { osm, features, skipped_geometry }
}
