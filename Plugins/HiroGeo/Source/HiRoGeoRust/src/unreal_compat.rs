//! Feature selection for the current Unreal rendering path.
use crate::{ElementKind, Extract, Feature, FeatureKind};

pub fn includes(result: &Extract, feature: &Feature) -> bool {
    let tags = match feature.source {
        ElementKind::Way => result.osm.ways.get(&feature.id).map(|way| &way.tags),
        ElementKind::Relation => result.osm.relations.get(&feature.id).map(|relation| &relation.tags),
        ElementKind::Node => result.osm.nodes.get(&feature.id).map(|node| &node.tags),
    };
    let Some(tags) = tags else { return false; };
    let tag = |key: &str| tags.get(key).map_or("", String::as_str);
    match feature.kind {
        FeatureKind::Road => {
            let tunnel = tag("tunnel");
            let level = tag("level").split(';').next().unwrap_or_default();
            !tag("location").eq_ignore_ascii_case("underground")
                && (tunnel.is_empty() || tunnel.eq_ignore_ascii_case("no"))
                && !level.trim().parse::<f32>().is_ok_and(|level| level < 0.0)
        }
        FeatureKind::Building => {
            let building = tag("building");
            !["construction", "proposed", "demolished"].iter()
                .any(|excluded| building.eq_ignore_ascii_case(excluded))
        }
        FeatureKind::BuildingPart | FeatureKind::Water => true,
    }
}
