"""Compare the raw geometry dumps of FOSMLoader and the Rust XML loader."""
import argparse
from collections import Counter, defaultdict
import xml.etree.ElementTree as ET


def normalize(points, polygon):
    if not polygon or not points:
        return min(points, points[::-1])
    if len(points) > 1 and points[0] == points[-1]:
        points = points[:-1]
    first = min(points)
    return min(
        direction[index:] + direction[:index]
        for direction in (points, points[::-1])
        for index, point in enumerate(direction)
        if point == first
    )


def coordinate(lon, lat):
    return round(float(lon) * 1e9), round(float(lat) * 1e9)


def geometry(text, polygon):
    return normalize(
        tuple(coordinate(*point.split(",")) for point in text.split(";") if point),
        polygon,
    )


def tag_map(element):
    return {tag.attrib["k"]: tag.attrib["v"] for tag in element.findall("tag")}


def load_dump(path):
    rows = defaultdict(list)
    with open(path, encoding="utf-8-sig") as stream:
        for line in stream:
            kind, source, rings = line.rstrip("\r\n").split("\t")
            polygon = kind not in ("road", "water_line")
            outer, *holes = (geometry(ring, polygon) for ring in rings.split("|"))
            rows[kind, outer, tuple(sorted(holes))].append(source)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("osm")
    parser.add_argument("cpp_tsv")
    parser.add_argument("rust_tsv")
    args = parser.parse_args()
    root = ET.parse(args.osm).getroot()
    nodes = {n.attrib["id"]: coordinate(n.attrib["lon"], n.attrib["lat"]) for n in root.findall("node")}
    tags = {}
    ways = defaultdict(list)
    for way in root.findall("way"):
        source = f'way/{way.attrib["id"]}'
        tags[source] = tag_map(way)
        refs = [node.attrib["ref"] for node in way.findall("nd")]
        if refs and all(ref in nodes for ref in refs):
            points = tuple(nodes[ref] for ref in refs)
            for polygon in (False, True):
                ways[polygon, normalize(points, polygon)].append(source)
    for relation in root.findall("relation"):
        tags[f'relation/{relation.attrib["id"]}'] = tag_map(relation)

    cpp = load_dump(args.cpp_tsv)
    rust = load_dump(args.rust_tsv)
    matched = Counter()
    differences = Counter()
    for key in sorted(cpp.keys() | rust.keys()):
        kind, outer, holes = key
        cpp_rows, rust_rows = cpp.get(key, []), rust.get(key, [])
        shared = min(len(cpp_rows), len(rust_rows))
        matched[kind] += shared
        for side, rows in (("cpp_only", cpp_rows[shared:]), ("rust_only", rust_rows[shared:])):
            for source in rows:
                differences[side, kind] += 1
                candidates = [source] if source else ways[kind not in ("road", "water_line"), outer]
                details = [(candidate, tags[candidate]) for candidate in candidates]
                print(f"{side} {kind}: points={len(outer)} holes={len(holes)} first={outer[:1]} sources={details}")
    print("Matched:", dict(matched))
    print("Differences:", dict(differences))


if __name__ == "__main__":
    main()
