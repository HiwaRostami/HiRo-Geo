# HiRo Geo Rust OSM core

The standalone crate decodes OSM XML and PBF into the same node, way and relation model and extracts building outlines, building parts, road centerlines and polygonal water. It builds as an MSVC static library with ABI version 1. Unreal continues to use its existing loaders and geometry generators.

## C++ data contract

The contract below comes from `HiroGeo/Public/GeoJSONLoader.h`, `HiroGeo/Public/OSMLoader.h` and the corresponding loader implementations.

| C++ output | Data consumed by the existing pipeline |
|---|---|
| `FGeoBuilding` | Outer footprint and courtyard rings in WGS84 lon/lat and local metres. Also centre, total and minimum heights, levels and minimum level, roof height/levels/shape/angle/direction/orientation/material, name and building type, facade material, wall and roof colours and explicit-colour flags, building-part ID, tower flag, outline and roof suppression flags. |
| `FGeoRoad` | Ordered WGS84 and local-metre centreline, width in metres, name, highway category and classification, layer, sidewalk and cycleway side relative to way direction, road and sidewalk colours. |
| `FGeoWater` | WGS84 and local-metre geometry and holes, name, closed/linear flag and width for waterways. |
| Coordinates | Both loaders use an equirectangular local projection with radius 6,371,000 m and cosine of origin latitude for eastward distance. North is positive Y. The OSM loader takes the midpoint of its geographic bounds as origin. Both loaders narrow local coordinates to float before constructing the vector. The Rust ABI accepts the origin explicitly and returns those rounded metre values in double fields. Unreal's later centimetre scale and Y inversion belong to mesh generation. |

`FOSMLoader` indexes nodes and ways before resolving relation members. It rejects ways with missing node references, stitches outer way segments for multipolygons, assigns inner rings by containment, converts positions to local metres, then applies building-part grouping and suppression. It also derives visual defaults from tags. `FGeoJSONLoader` obtains the topology directly from the geometry arrays and converts it to the same structures. Its GeoJSON feature interpretation stays in C++.

The ABI supplies WGS84 points, original node IDs, source IDs, tags and relation membership. It groups multiple polygons under their relation ID with a polygon index for each outer and its holes. The C++ adapter must emit one output entry per outer ring, remove the repeated closing coordinate where C++ expects an open ring array, and apply the existing tag defaults and building-part suppression. The Rust extractor stitches both outer and inner way segments and skips a malformed relation geometry as a unit. Nested relation members remain available as references. Polygon assembly from nested relations, linear waterways, vegetation, railway, elevation samples and object placement need their own compatibility work. Raw tags preserve inputs for the adapter, including roof, material, colour, bridge and tunnel tags.

## ABI ownership

- Include `include/hirogeo_osm.h`. The returned opaque handle owns all result storage. Parse functions take a caller-owned byte buffer readable for the duration of the call and require an output slot containing no live handle. On return the slot contains a new handle or null.
- Result access returns borrowed read-only views valid until release. Ring arrays contain `count` points and the same number of node IDs. Strings are UTF-8 byte views with explicit lengths and may contain embedded zero bytes.
- Release with `hiro_osm_release(&handle)`. It clears the caller's slot and accepts a slot that already contains null. A copied handle remains an alias and becomes invalid when the owner releases it.
- Status 0 is success, 1 is invalid argument, 2 is a decode error, 3 is a caught panic and 4 is an absent item or out-of-range index. Get parse error text on the same thread using `hiro_osm_error(NULL, 0)` for the required size, including the terminator. A subsequent parse on that thread replaces the error. Truncated error text may end inside a UTF-8 character.
- Independent parses and concurrent reads of the same result are supported. The caller must exclude release while any reader or borrowed view is active. Pointers must be valid, correctly aligned and sufficiently sized. Arbitrary or freed pointers and overlapping output slots violate the contract. Rust panic unwinding is caught at the ABI boundary. Process-level aborts such as allocator exhaustion are outside this guarantee.

## Unreal feature selection

`hiro_osm_unreal_includes` takes the same zero-based index as `hiro_osm_feature`. On success it writes 1 for an included feature and 0 for an excluded feature. This is an optional selection over the immutable result. It excludes roads marked as tunnels or underground, roads whose first numeric level is negative, and building outlines marked as construction, proposed or demolished. The raw result remains available to other consumers.

The selection keeps independent tagged inner ways, deduplicates same-kind outer ways already represented by a relation, and retains valid multipolygon building parts and their holes. Incomplete relations still require closed rings with all referenced nodes before the extractor can emit geometry.

Use `cargo run --release --example dump_geometry -- input.osm rust.tsv --unreal` to export the selected geometry for `python examples/compare_geometry.py input.osm cpp.tsv rust.tsv`.

## Build and validation

Direct dependencies are `quick-xml 0.39.1` (MIT) and `osmpbf 0.3.8` (MIT OR Apache-2.0, Rust 1.88 minimum). The PBF decoder uses its default Rust zlib backend. Review resolved transitive licences after Cargo can produce the lockfile.

```powershell
cargo test
cargo build --release --target x86_64-pc-windows-msvc
cargo run --release --example measure -- xml tests/fixtures/area.osm 100
cargo run --release --example measure -- pbf tests/fixtures/area.osm.pbf 100
```

The test fixture represents two water polygons, one water hole, one outline, one elevated building part, one valid road and one invalid road with a missing node. XML and PBF fixtures describe the same dataset. Regenerate the compressed PBF fixture with `python tests/fixtures/make_pbf.py`. Use the fixture for correctness and a larger equivalent XML/PBF extract for meaningful performance measurements.

The Rust benchmark reads input before timing, performs one warm-up parse and reports median, nearest-rank p95 and feature counts. To measure C++ XML, run `FOSMLoader::LoadFromFile` in a separate development test harness with identical XML input. Record file I/O separately or include it on both sides. Compare source IDs, ring membership and local coordinates and normalize relation polygon counts before comparing totals. Use separate processes and record peak working set with the same measurement method. C++ XML versus Rust XML measures the difference between implementations and the amount of work performed. Rust XML versus Rust PBF measures format and decoder differences. No runtime or memory baseline has been measured yet.
