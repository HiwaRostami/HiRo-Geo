use hirogeo_osm::{parse_pbf, parse_xml, Extract, FeatureKind};
use std::{env, fs, hint::black_box, time::Instant};

fn measure(parser: fn(&[u8]) -> Result<Extract, String>, data: &[u8], runs: usize) {
    let baseline = parser(data).expect("warm-up parse");
    let mut counts = [0; 4];
    let mut water_polygons = 0;
    let mut water_holes = 0;
    for feature in &baseline.features {
        counts[feature.kind as usize - 1] += 1;
        if feature.kind == FeatureKind::Water {
            for ring in &feature.rings {
                if ring.inner {
                    water_holes += 1;
                } else {
                    water_polygons += 1;
                }
            }
        }
    }
    let skipped = baseline.skipped_geometry;
    drop(baseline);

    let mut timings = Vec::with_capacity(runs);
    for _ in 0..runs {
        let start = Instant::now();
        let result = parser(black_box(data)).expect("measured parse");
        timings.push(start.elapsed().as_secs_f64() * 1_000.0);
        black_box(&result);
    }
    timings.sort_by(f64::total_cmp);
    let median = (timings[(runs - 1) / 2] + timings[runs / 2]) / 2.0;
    let p95 = timings[(runs * 95).div_ceil(100) - 1];
    println!(
        "runs={runs} bytes={} median_ms={median:.3} p95_ms={p95:.3} buildings={} parts={} roads={} water={} water_polygons={water_polygons} water_holes={water_holes} skipped={skipped}",
        data.len(), counts[0], counts[1], counts[2], counts[3]
    );
}

fn main() {
    let args: Vec<_> = env::args().collect();
    if args.len() != 4 {
        eprintln!("usage: measure <xml|pbf> <file> <runs: 1..100000>");
        std::process::exit(2);
    }
    let runs = args[3].parse::<usize>().expect("run count");
    assert!((1..=100_000).contains(&runs));
    let input = fs::read(&args[2]).expect("read input file");
    match args[1].as_str() {
        "xml" => measure(parse_xml, &input, runs),
        "pbf" => measure(parse_pbf, &input, runs),
        _ => panic!("expected xml or pbf"),
    }
}
