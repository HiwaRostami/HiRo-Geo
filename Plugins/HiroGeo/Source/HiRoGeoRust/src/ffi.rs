use crate::{Extract, LocalPoint, Point, Tags};
use std::{cell::RefCell, panic::{catch_unwind, AssertUnwindSafe}, ptr, slice};

thread_local! {
    static ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

const INVALID: u32 = 1;
const PARSE: u32 = 2;
const PANIC: u32 = 3;
const RANGE: u32 = 4;

#[repr(C)]
pub struct Bytes {
    ptr: *const u8,
    len: u64,
}

impl From<&str> for Bytes {
    fn from(value: &str) -> Self {
        Self { ptr: value.as_ptr(), len: value.len() as u64 }
    }
}

#[repr(C)]
pub struct FeatureInfo {
    id: i64,
    ring_count: u64,
    kind: u32,
    source: u32,
}

#[repr(C)]
pub struct RingView {
    points: *const Point,
    node_ids: *const i64,
    count: u64,
    polygon: u32,
    inner: u32,
}

#[repr(C)]
pub struct MemberView {
    role: Bytes,
    id: i64,
    kind: u32,
}

fn status(action: impl FnOnce() -> Result<(), u32>) -> u32 {
    match catch_unwind(AssertUnwindSafe(action)) {
        Ok(Ok(())) => 0,
        Ok(Err(code)) => code,
        Err(_) => PANIC,
    }
}

unsafe fn parse(data: *const u8, size: u64, out: *mut *mut Extract,
    parser: fn(&[u8]) -> Result<Extract, String>) -> u32 {
    status(|| {
        ERROR.with(|error| error.borrow_mut().clear());
        if out.is_null() { return Err(INVALID); }
        out.write(ptr::null_mut());
        if data.is_null() || size == 0 || size > isize::MAX as u64 { return Err(INVALID); }
        match parser(slice::from_raw_parts(data, size as usize)) {
            Ok(result) => { out.write(Box::into_raw(Box::new(result))); Ok(()) }
            Err(message) => { ERROR.with(|error| *error.borrow_mut() = message); Err(PARSE) }
        }
    })
}

#[no_mangle]
pub extern "C" fn hiro_osm_abi_version() -> u32 { 1 }

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_parse_xml(data: *const u8, size: u64, out: *mut *mut Extract) -> u32 {
    parse(data, size, out, crate::parse_xml)
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_parse_pbf(data: *const u8, size: u64, out: *mut *mut Extract) -> u32 {
    parse(data, size, out, crate::parse_pbf)
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_release(handle: *mut *mut Extract) -> u32 {
    status(|| {
        let handle = handle.as_mut().ok_or(INVALID)?;
        let old = std::mem::replace(handle, ptr::null_mut());
        if !old.is_null() { drop(Box::from_raw(old)); }
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_error(out: *mut u8, capacity: u64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| ERROR.with(|error| {
        let error = error.borrow();
        if !out.is_null() && capacity > 0 {
            let length = (capacity - 1).min(error.len() as u64) as usize;
            ptr::copy_nonoverlapping(error.as_ptr(), out, length);
            out.add(length).write(0);
        }
        error.len() as u64 + 1
    }))).unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_to_local(point: Point, origin: Point, out: *mut LocalPoint) -> u32 {
    status(|| {
        if out.is_null() { return Err(INVALID); }
        out.write(crate::to_local(point, origin).ok_or(INVALID)?);
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_counts(result: *const Extract, features: *mut u64, skipped: *mut u64) -> u32 {
    status(|| {
        let result = result.as_ref().ok_or(INVALID)?;
        if features.is_null() || skipped.is_null() { return Err(INVALID); }
        features.write(result.features.len() as u64);
        skipped.write(result.skipped_geometry);
        Ok(())
    })
}

fn index(value: u64) -> Result<usize, u32> { usize::try_from(value).map_err(|_| RANGE) }

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_unreal_includes(
    result: *const Extract, feature: u64, included: *mut u32,
) -> u32 {
    status(|| {
        if included.is_null() { return Err(INVALID); }
        let result = result.as_ref().ok_or(INVALID)?;
        let feature = result.features.get(index(feature)?).ok_or(RANGE)?;
        included.write(u32::from(crate::unreal_compat::includes(result, feature)));
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_feature(result: *const Extract, feature: u64, out: *mut FeatureInfo) -> u32 {
    status(|| {
        if out.is_null() { return Err(INVALID); }
        let feature = result.as_ref().ok_or(INVALID)?.features.get(index(feature)?).ok_or(RANGE)?;
        out.write(FeatureInfo { id: feature.id, ring_count: feature.rings.len() as u64,
            kind: feature.kind as u32, source: feature.source as u32 });
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_ring(result: *const Extract, feature: u64, ring: u64, out: *mut RingView) -> u32 {
    status(|| {
        if out.is_null() { return Err(INVALID); }
        let feature = result.as_ref().ok_or(INVALID)?.features.get(index(feature)?).ok_or(RANGE)?;
        let ring = feature.rings.get(index(ring)?).ok_or(RANGE)?;
        out.write(RingView { points: ring.points.as_ptr(), node_ids: ring.node_ids.as_ptr(),
            count: ring.points.len() as u64, polygon: ring.polygon, inner: u32::from(ring.inner) });
        Ok(())
    })
}

fn tags(result: &Extract, source: u32, id: i64) -> Option<&Tags> {
    match source {
        1 => result.osm.nodes.get(&id).map(|node| &node.tags),
        2 => result.osm.ways.get(&id).map(|way| &way.tags),
        3 => result.osm.relations.get(&id).map(|relation| &relation.tags),
        _ => None,
    }
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_tag(result: *const Extract, source: u32, id: i64, tag: u64,
    key: *mut Bytes, value: *mut Bytes) -> u32 {
    status(|| {
        if key.is_null() || value.is_null() { return Err(INVALID); }
        let tags = tags(result.as_ref().ok_or(INVALID)?, source, id).ok_or(RANGE)?;
        let (k, v) = tags.iter().nth(index(tag)?).ok_or(RANGE)?;
        key.write(k.as_str().into());
        value.write(v.as_str().into());
        Ok(())
    })
}

#[no_mangle]
pub unsafe extern "C" fn hiro_osm_member(result: *const Extract, relation: i64, member: u64, out: *mut MemberView) -> u32 {
    status(|| {
        if out.is_null() { return Err(INVALID); }
        let relation = result.as_ref().ok_or(INVALID)?.osm.relations.get(&relation).ok_or(RANGE)?;
        let member = relation.members.get(index(member)?).ok_or(RANGE)?;
        out.write(MemberView { role: member.role.as_str().into(), id: member.id,
            kind: member.kind as u32 });
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_views_and_release() {
        let mut handle = ptr::null_mut();
        let data = include_bytes!("../tests/fixtures/area.osm");
        unsafe {
            assert_eq!(hiro_osm_parse_xml(data.as_ptr(), data.len() as u64, &mut handle), 0);
            let (mut count, mut skipped) = (0, 0);
            assert_eq!(hiro_osm_counts(handle, &mut count, &mut skipped), 0);
            assert_eq!((count, skipped), (4, 1));
            let mut view = std::mem::MaybeUninit::uninit();
            assert_eq!(hiro_osm_ring(handle, 0, 0, view.as_mut_ptr()), 0);
            let view = view.assume_init();
            assert!(view.count >= 4);
            assert_eq!(*view.node_ids, *view.node_ids.add(view.count as usize - 1));
            assert_eq!(hiro_osm_release(&mut handle), 0);
            assert!(handle.is_null());
            assert_eq!(hiro_osm_release(&mut handle), 0);
        }
    }

    #[test]
    fn parse_failure_reports_error_and_null_handle() {
        let data = b"<html/>";
        let mut handle = ptr::null_mut();
        unsafe {
            assert_eq!(hiro_osm_parse_xml(data.as_ptr(), data.len() as u64, &mut handle), PARSE);
            assert!(handle.is_null());
            let size = hiro_osm_error(ptr::null_mut(), 0);
            assert!(size > 1);
            let mut message = vec![0; size as usize];
            assert_eq!(hiro_osm_error(message.as_mut_ptr(), size), size);
            assert_ne!(message[0], 0);
            assert_eq!(message.last(), Some(&0));
        }
    }
}
