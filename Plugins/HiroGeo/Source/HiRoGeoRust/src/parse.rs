use crate::{ElementKind, Member, Node, Osm, Point, Relation, Tags, Way};
use osmpbf::{Element, ElementReader, RelMemberType};
use quick_xml::{events::Event, Reader};

fn attributes(element: &quick_xml::events::BytesStart<'_>) -> Result<Tags, String> {
    element.attributes().map(|attribute| {
        let attribute = attribute.map_err(|e| e.to_string())?;
        let name = std::str::from_utf8(attribute.key.as_ref()).map_err(|e| e.to_string())?;
        let value = attribute.unescape_value().map_err(|e| e.to_string())?;
        Ok((name.to_owned(), value.into_owned()))
    }).collect()
}

fn required<'a>(tags: &'a Tags, name: &str) -> Result<&'a str, String> {
    tags.get(name).map(String::as_str).ok_or_else(|| format!("missing {name}"))
}

fn number<T: std::str::FromStr>(tags: &Tags, name: &str) -> Result<T, String> {
    required(tags, name)?.parse().map_err(|_| format!("invalid {name}"))
}

pub fn xml(input: &[u8]) -> Result<Osm, String> {
    std::str::from_utf8(input).map_err(|e| e.to_string())?;
    let mut reader = Reader::from_reader(input);
    reader.config_mut().expand_empty_elements = true;
    let mut data = Osm::default();
    let mut depth = 0usize;
    let mut root_seen = false;
    let mut current = None;
    loop {
        match reader.read_event().map_err(|e| e.to_string())? {
            Event::Start(element) => {
                match (depth, element.name().as_ref()) {
                    (0, b"osm") if !root_seen => root_seen = true,
                    (0, _) => return Err("expected one osm root".into()),
                    (1, name @ (b"node" | b"way" | b"relation")) => {
                        let attrs = attributes(&element)?;
                        let id = number(&attrs, "id")?;
                        current = Some(match name {
                            b"node" => {
                                let point = Point { lon: number(&attrs, "lon")?, lat: number(&attrs, "lat")? };
                                if !point.valid() { return Err("invalid node coordinates".into()); }
                                data.nodes.insert(id, Node { point, tags: Tags::new() });
                                (ElementKind::Node, id)
                            }
                            b"way" => {
                                data.ways.insert(id, Way::default());
                                (ElementKind::Way, id)
                            }
                            _ => {
                                data.relations.insert(id, Relation::default());
                                (ElementKind::Relation, id)
                            }
                        });
                    }
                    (2, b"tag") if current.is_some() => {
                        let attrs = attributes(&element)?;
                        let (kind, id) = current.unwrap();
                        let tags = match kind {
                            ElementKind::Node => &mut data.nodes.get_mut(&id).unwrap().tags,
                            ElementKind::Way => &mut data.ways.get_mut(&id).unwrap().tags,
                            ElementKind::Relation => &mut data.relations.get_mut(&id).unwrap().tags,
                        };
                        tags.insert(required(&attrs, "k")?.to_owned(), required(&attrs, "v")?.to_owned());
                    }
                    (2, b"nd") => {
                        if let Some((ElementKind::Way, id)) = current {
                            data.ways.get_mut(&id).unwrap().refs.push(number(&attributes(&element)?, "ref")?);
                        }
                    }
                    (2, b"member") => {
                        if let Some((ElementKind::Relation, id)) = current {
                            let attrs = attributes(&element)?;
                            let kind = match required(&attrs, "type")? {
                                "node" => ElementKind::Node,
                                "way" => ElementKind::Way,
                                "relation" => ElementKind::Relation,
                                _ => return Err("invalid relation member type".into()),
                            };
                            data.relations.get_mut(&id).unwrap().members.push(Member {
                                kind, id: number(&attrs, "ref")?, role: attrs.get("role").cloned().unwrap_or_default(),
                            });
                        }
                    }
                    _ => {}
                }
                depth += 1;
            }
            Event::End(_) => {
                depth = depth.checked_sub(1).ok_or("unexpected closing element")?;
                if depth == 1 { current = None; }
            }
            Event::Text(text) if depth == 0 && !text.iter().all(u8::is_ascii_whitespace) => {
                return Err("text outside osm root".into());
            }
            Event::DocType(_) => return Err("DOCTYPE is unsupported".into()),
            Event::Eof if root_seen && depth == 0 => return Ok(data),
            Event::Eof => return Err("missing or incomplete osm root".into()),
            _ => {}
        }
    }
}

pub fn pbf(input: &[u8]) -> Result<Osm, String> {
    // The first blob header must advertise the standard OSMHeader block.
    let size = input.get(..4).ok_or("missing PBF header")?;
    let size = u32::from_be_bytes(size.try_into().unwrap()) as usize;
    if size == 0 || size > 64 * 1024 ||
        !input.get(4..4 + size).is_some_and(|header| header.windows(9).any(|bytes| bytes == b"OSMHeader")) {
        return Err("invalid OSM PBF header".into());
    }
    let mut data = Osm::default();
    let mut invalid = false;
    ElementReader::new(input).for_each(|element| {
        let mut insert_node = |id, lon, lat, tags| {
            let point = Point { lon, lat };
            invalid |= !point.valid();
            data.nodes.insert(id, Node { point, tags });
        };
        match element {
            Element::Node(node) => insert_node(node.id(), node.lon(), node.lat(), copy_tags(node.tags())),
            Element::DenseNode(node) => insert_node(node.id(), node.lon(), node.lat(), copy_tags(node.tags())),
            Element::Way(way) => {
                data.ways.insert(way.id(), Way { refs: way.refs().collect(), tags: copy_tags(way.tags()) });
            }
            Element::Relation(relation) => {
                let members = relation.members().map(|member| {
                    let role = member.role().map(str::to_owned).unwrap_or_else(|_| {
                        invalid = true;
                        String::new()
                    });
                    let kind = match member.member_type {
                        RelMemberType::Node => ElementKind::Node,
                        RelMemberType::Way => ElementKind::Way,
                        RelMemberType::Relation => ElementKind::Relation,
                    };
                    Member { kind, id: member.member_id, role }
                }).collect();
                data.relations.insert(relation.id(), Relation { members, tags: copy_tags(relation.tags()) });
            }
        }
    }).map_err(|e| e.to_string())?;
    if invalid { return Err("invalid PBF node coordinates or member role".into()); }
    Ok(data)
}

fn copy_tags<'a>(tags: impl Iterator<Item = (&'a str, &'a str)>) -> Tags {
    tags.map(|(k, v)| (k.to_owned(), v.to_owned())).collect()
}
