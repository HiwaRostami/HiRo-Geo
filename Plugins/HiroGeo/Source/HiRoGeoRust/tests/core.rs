use hirogeo_osm::{parse_pbf, parse_xml, to_local, FeatureKind, Point};

const XML: &[u8] = include_bytes!("fixtures/area.osm");
const PBF: &[u8] = include_bytes!("fixtures/area.osm.pbf");

#[test]
fn xml_and_pbf_have_identical_semantics() {
    let xml = parse_xml(XML).unwrap();
    let pbf = parse_pbf(PBF).unwrap();
    assert_eq!(xml.osm.ways, pbf.osm.ways);
    assert_eq!(xml.osm.relations, pbf.osm.relations);
    assert_eq!(xml.osm.nodes.len(), pbf.osm.nodes.len());
    for (id, node) in &xml.osm.nodes {
        let other = &pbf.osm.nodes[id];
        assert_eq!(node.tags, other.tags);
        assert!((node.point.lon - other.point.lon).abs() < 1e-9);
        assert!((node.point.lat - other.point.lat).abs() < 1e-9);
    }
    assert_eq!(xml.features.len(), 4);
    assert_eq!(xml.features.len(), pbf.features.len());
    for (a, b) in xml.features.iter().zip(&pbf.features) {
        assert_eq!((a.kind, a.source, a.id), (b.kind, b.source, b.id));
        assert_eq!(a.rings.len(), b.rings.len());
        for (a, b) in a.rings.iter().zip(&b.rings) {
            assert_eq!((&a.node_ids, a.polygon, a.inner), (&b.node_ids, b.polygon, b.inner));
        }
    }
    assert_eq!(xml.skipped_geometry, 1);
    assert_eq!(pbf.skipped_geometry, 1);
    let water = xml.features.iter().find(|f| f.kind == FeatureKind::Water).unwrap();
    assert_eq!(water.rings.iter().filter(|r| !r.inner).count(), 2);
    assert_eq!(water.rings.iter().filter(|r| r.inner).count(), 1);
    assert!(xml.features.iter().any(|f| f.kind == FeatureKind::BuildingPart));
}

#[test]
fn invalid_inputs_are_errors() {
    assert!(parse_xml(b"<osm><node id='1'").is_err());
    assert!(parse_xml(b"<html/>").is_err());
    assert!(parse_xml(b"<osm><node id='1' lon='0' lat='NaN'/></osm>").is_err());
    assert!(parse_pbf(b"not a pbf").is_err());
    assert!(parse_pbf(&PBF[..PBF.len() - 1]).is_err());
}

#[test]
fn tagged_inner_way_keeps_its_independent_feature() {
    let xml = std::str::from_utf8(XML).unwrap()
        .replace(r#"<way id="22">"#, r#"<way id="22"><tag k="natural" v="water"/>"#)
        .replace(r#"<way id="23">"#, r#"<way id="23"><tag k="natural" v="water"/>"#);
    let result = parse_xml(xml.as_bytes()).unwrap();
    let waters: Vec<_> = result.features.iter().filter(|f| f.kind == FeatureKind::Water).collect();
    assert_eq!(waters.len(), 2);
    assert!(waters.iter().any(|f| f.id == 22));
    assert!(!waters.iter().any(|f| f.id == 23));
    assert!(waters.iter().any(|f| f.id == 40 && f.rings.iter().any(|ring| ring.inner)));
}

#[test]
fn local_projection_matches_cpp_formula() {
    let origin = Point { lon: 8.53, lat: 47.37 };
    let local = to_local(Point { lon: 8.54, lat: 47.38 }, origin).unwrap();
    assert!((local.x - 753.143).abs() < 1.0);
    assert!((local.y - 1111.9493).abs() < 0.0001);
    assert!(to_local(Point { lon: f64::NAN, lat: 0.0 }, origin).is_none());
}

#[test]
fn unreal_selection_preserves_source_geometry() {
    let xml = std::str::from_utf8(XML).unwrap()
        .replace(r#"k="building" v="yes""#, r#"k="building" v="construction""#)
        .replace(r#"<way id="32">"#, r#"<way id="32"><tag k="level" v="-1;0"/>"#);
    let result = parse_xml(xml.as_bytes()).unwrap();
    assert_eq!(result.features.len(), 4);
    let included: Vec<_> = result.features.iter()
        .filter(|feature| hirogeo_osm::unreal_compat::includes(&result, feature))
        .map(|feature| feature.id)
        .collect();
    assert_eq!(included, [40, 31]);
}
