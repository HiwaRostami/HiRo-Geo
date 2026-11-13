"""Encode the adjacent OSM fixture as OSM PBF 0.6 for format parity tests."""
from decimal import Decimal
from pathlib import Path
from xml.etree import ElementTree
import zlib


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def uint(field, value):
    return varint(field * 8) + varint(value)


def blob(field, data):
    return varint(field * 8 + 2) + varint(len(data)) + data


def zigzag(value):
    return value * 2 if value >= 0 else -value * 2 - 1


def packed(field, values):
    return blob(field, b"".join(map(varint, values)))


def deltas(values):
    previous = 0
    for value in values:
        yield zigzag(value - previous)
        previous = value


def block(kind, payload):
    body = uint(2, len(payload)) + blob(3, zlib.compress(payload))
    header = blob(1, kind) + uint(3, len(body))
    return len(header).to_bytes(4, "big") + header + body


def convert(source, destination):
    root = ElementTree.parse(source).getroot()
    strings = {"": 0}

    def intern(text):
        return strings.setdefault(text, len(strings))

    groups = {kind: bytearray() for kind in ("node", "way", "relation")}
    for element in root:
        kind = element.tag
        if kind not in ("node", "way", "relation"):
            continue
        tags = element.findall("tag")
        identifier = int(element.attrib["id"])
        message = uint(1, zigzag(identifier) if kind == "node" else identifier)
        message += packed(2, (intern(t.attrib["k"]) for t in tags))
        message += packed(3, (intern(t.attrib["v"]) for t in tags))
        if kind == "node":
            for field, name in ((8, "lat"), (9, "lon")):
                message += uint(field, zigzag(int(Decimal(element.attrib[name]) * 10_000_000)))
            groups[kind].extend(blob(1, message))
        elif kind == "way":
            message += packed(8, deltas(int(n.attrib["ref"]) for n in element.findall("nd")))
            groups[kind].extend(blob(3, message))
        else:
            members = element.findall("member")
            message += packed(8, (intern(m.attrib.get("role", "")) for m in members))
            message += packed(9, deltas(int(m.attrib["ref"]) for m in members))
            kinds = {"node": 0, "way": 1, "relation": 2}
            message += packed(10, (kinds[m.attrib["type"]] for m in members))
            groups[kind].extend(blob(4, message))
    table = b"".join(blob(1, text.encode()) for text in strings)
    destination.write_bytes(
        block(b"OSMHeader", blob(4, b"OsmSchema-V0.6"))
        + block(b"OSMData", blob(1, table) + b"".join(blob(2, group) for group in groups.values() if group))
    )


if __name__ == "__main__":
    source = Path(__file__).with_name("area.osm")
    convert(source, source.with_suffix(".osm.pbf"))
