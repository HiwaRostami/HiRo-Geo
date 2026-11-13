use hirogeo_osm::{parse_xml, unreal_compat, ElementKind, FeatureKind, Ring};
use std::{env, fs, io::{self, BufWriter, Write}};

fn write_ring(out: &mut impl Write, ring: &Ring) -> io::Result<()> {
    for point in &ring.points {
        write!(out, "{:.9},{:.9};", point.lon, point.lat)?;
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = env::args_os().skip(1);
    let input = args.next().ok_or("usage: dump_geometry <input.osm> <output.tsv>")?;
    let output = args.next().ok_or("missing output path")?;
    let unreal = match args.next() {
        None => false,
        Some(flag) if flag == "--unreal" && args.next().is_none() => true,
        _ => return Err("expected optional --unreal".into()),
    };
    let result = parse_xml(&fs::read(input)?)?;
    let mut out = BufWriter::new(fs::File::create(output)?);
    let mut excluded = 0;
    for feature in &result.features {
        if unreal && !unreal_compat::includes(&result, feature) {
            excluded += 1;
            continue;
        }
        let kind = match feature.kind {
            FeatureKind::Building => "building",
            FeatureKind::BuildingPart => "part",
            FeatureKind::Road => "road",
            FeatureKind::Water => "water",
        };
        let source = match feature.source {
            ElementKind::Node => "node",
            ElementKind::Way => "way",
            ElementKind::Relation => "relation",
        };
        for outer in feature.rings.iter().filter(|ring| !ring.inner) {
            write!(out, "{kind}\t{source}/{}\t", feature.id)?;
            write_ring(&mut out, outer)?;
            for hole in feature.rings.iter().filter(|ring| ring.inner && ring.polygon == outer.polygon) {
                write!(out, "|")?;
                write_ring(&mut out, hole)?;
            }
            writeln!(out)?;
        }
    }
    out.flush()?;
    if unreal {
        println!("Excluded by Unreal compatibility rules: {excluded}");
    }
    Ok(())
}
