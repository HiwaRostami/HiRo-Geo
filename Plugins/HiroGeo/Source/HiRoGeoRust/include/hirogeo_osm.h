#ifndef HIROGEO_OSM_H
#define HIROGEO_OSM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct HiroOsmResult HiroOsmResult;
typedef struct { double lon, lat; } HiroOsmPoint;
typedef struct { double x, y; } HiroOsmLocalPoint;
typedef struct { const uint8_t *ptr; uint64_t len; } HiroOsmBytes;
typedef struct { int64_t id; uint64_t ring_count; uint32_t kind, source; } HiroOsmFeature;
typedef struct {
    const HiroOsmPoint *points;
    const int64_t *node_ids;
    uint64_t count;
    uint32_t polygon, inner;
} HiroOsmRing;
typedef struct { HiroOsmBytes role; int64_t id; uint32_t kind; } HiroOsmMember;

enum { HIRO_OSM_OK, HIRO_OSM_INVALID_ARGUMENT, HIRO_OSM_PARSE_ERROR,
       HIRO_OSM_PANIC, HIRO_OSM_OUT_OF_RANGE };
enum { HIRO_OSM_NODE = 1, HIRO_OSM_WAY, HIRO_OSM_RELATION };
enum { HIRO_OSM_BUILDING = 1, HIRO_OSM_BUILDING_PART, HIRO_OSM_ROAD, HIRO_OSM_WATER };

uint32_t hiro_osm_abi_version(void);
uint32_t hiro_osm_parse_xml(const uint8_t *data, uint64_t size, HiroOsmResult **out);
uint32_t hiro_osm_parse_pbf(const uint8_t *data, uint64_t size, HiroOsmResult **out);
uint32_t hiro_osm_release(HiroOsmResult **handle);
uint64_t hiro_osm_error(uint8_t *out, uint64_t capacity);
uint32_t hiro_osm_to_local(HiroOsmPoint point, HiroOsmPoint origin, HiroOsmLocalPoint *out);
uint32_t hiro_osm_counts(const HiroOsmResult *result, uint64_t *features, uint64_t *skipped);
uint32_t hiro_osm_feature(const HiroOsmResult *result, uint64_t feature, HiroOsmFeature *out);
/* Writes 1 when the feature passes Unreal compatibility filters, or 0 otherwise. */
uint32_t hiro_osm_unreal_includes(const HiroOsmResult *result, uint64_t feature, uint32_t *included);
uint32_t hiro_osm_ring(const HiroOsmResult *result, uint64_t feature, uint64_t ring, HiroOsmRing *out);
uint32_t hiro_osm_tag(const HiroOsmResult *result, uint32_t source, int64_t id, uint64_t tag,
                      HiroOsmBytes *key, HiroOsmBytes *value);
uint32_t hiro_osm_member(const HiroOsmResult *result, int64_t relation, uint64_t member,
                         HiroOsmMember *out);

#ifdef __cplusplus
}
#endif
#endif
