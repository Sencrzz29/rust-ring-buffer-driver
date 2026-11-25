// SPDX-License-Identifier: GPL-2.0
//! Hand-written edge cases for the ring buffer.

use ringbuf_core::{check_capacity, RingBuf, RingError, MAX_CAPACITY, MIN_CAPACITY};

fn ring(cap: usize) -> RingBuf<Vec<u8>> {
    RingBuf::new(vec![0u8; cap])
}

fn contents(r: &RingBuf<Vec<u8>>) -> Vec<u8> {
    let (a, b) = r.readable().expect("readable");
    [a, b].concat()
}

#[test]
fn starts_empty() {
    let r = ring(64);
    assert_eq!(r.capacity(), 64);
    assert_eq!(r.len(), 0);
    assert_eq!(r.free(), 64);
    assert!(r.is_empty());
}

#[test]
fn push_pop_roundtrip() {
    let mut r = ring(64);
    assert_eq!(r.push(b"hello"), Ok(5));
    let mut out = [0u8; 16];
    assert_eq!(r.pop(&mut out), Ok(5));
    assert_eq!(&out[..5], b"hello");
    assert!(r.is_empty());
}

#[test]
fn pop_on_empty_returns_zero() {
    let mut r = ring(64);
    let mut out = [0u8; 8];
    assert_eq!(r.pop(&mut out), Ok(0));
}

#[test]
fn partial_push_then_full() {
    let mut r = ring(64);
    assert_eq!(r.push(&[7u8; 100]), Ok(64));
    assert_eq!(r.free(), 0);
    assert_eq!(r.push(b"x"), Err(RingError::Full));
    // an empty write is never an error
    assert_eq!(r.push(b""), Ok(0));
}

#[test]
fn wraps_around_and_preserves_order() {
    let mut r = ring(64);
    let first: Vec<u8> = (0..48).collect();
    assert_eq!(r.push(&first), Ok(48));
    let mut sink = [0u8; 40];
    assert_eq!(r.pop(&mut sink), Ok(40)); // head is now at 40
    let second: Vec<u8> = (100..150).collect();
    assert_eq!(r.push(&second), Ok(50)); // 24 bytes to the end, 26 wrap to 0

    let (a, b) = r.readable().unwrap();
    assert!(!b.is_empty(), "data should be split across the wrap point");
    let mut expected: Vec<u8> = (40..48).collect();
    expected.extend(100..150);
    assert_eq!([a, b].concat(), expected);
}

#[test]
fn drained_ring_restarts_at_zero() {
    let mut r = ring(64);
    r.push(&[1u8; 60]).unwrap();
    r.pop(&mut [0u8; 60]).unwrap();
    r.push(&[2u8; 30]).unwrap();
    let (a, b) = r.readable().unwrap();
    assert_eq!(a.len(), 30);
    assert!(b.is_empty(), "after draining, new data should be contiguous");
}

#[test]
fn peek_does_not_consume() {
    let mut r = ring(64);
    r.push(b"abcdefgh").unwrap();
    let mut out = [0u8; 3];
    assert_eq!(r.peek(2, &mut out), Ok(3));
    assert_eq!(&out, b"cde");
    assert_eq!(r.len(), 8);
}

#[test]
fn peek_slices_rejects_hostile_ranges() {
    let mut r = ring(64);
    r.push(&[9u8; 10]).unwrap();
    // exactly the stored range is fine
    assert!(r.peek_slices(0, 10).is_ok());
    assert!(r.peek_slices(10, 0).is_ok());
    // past the end
    assert_eq!(r.peek_slices(0, 11), Err(RingError::OutOfRange));
    assert_eq!(r.peek_slices(11, 0), Err(RingError::OutOfRange));
    assert_eq!(r.peek_slices(5, 6), Err(RingError::OutOfRange));
    // offset + count overflows usize: must be an error, not a wrap to a small number
    assert_eq!(r.peek_slices(usize::MAX, 2), Err(RingError::OutOfRange));
    assert_eq!(r.peek_slices(2, usize::MAX), Err(RingError::OutOfRange));
    assert_eq!(r.peek_slices(usize::MAX, usize::MAX), Err(RingError::OutOfRange));
    // peek() with an offset past the data
    assert_eq!(r.peek(11, &mut [0u8; 1]), Err(RingError::OutOfRange));
}

#[test]
fn consume_and_commit_bounds() {
    let mut r = ring(64);
    r.push(&[1u8; 10]).unwrap();
    assert_eq!(r.consume(11), Err(RingError::OutOfRange));
    assert_eq!(r.consume(usize::MAX), Err(RingError::OutOfRange));
    assert_eq!(r.commit(55), Err(RingError::OutOfRange)); // only 54 free
    assert_eq!(r.commit(usize::MAX), Err(RingError::OutOfRange));
    assert_eq!(r.len(), 10, "failed calls must not change state");
    assert_eq!(r.commit(54), Ok(()));
    assert_eq!(r.free(), 0);
}

#[test]
fn zero_capacity_storage_never_panics() {
    let mut r = ring(0);
    assert_eq!(r.push(b"x"), Err(RingError::Full));
    assert_eq!(r.pop(&mut [0u8; 4]), Ok(0));
    assert_eq!(r.peek_slices(0, 0).map(|(a, b)| a.len() + b.len()), Ok(0));
    assert_eq!(r.peek_slices(0, 1), Err(RingError::OutOfRange));
    let (a, b) = r.writable_mut().unwrap();
    assert!(a.is_empty() && b.is_empty());
    assert_eq!(r.commit(1), Err(RingError::OutOfRange));
}

#[test]
fn writable_then_commit_matches_push() {
    let mut r = ring(64);
    r.push(&[0u8; 50]).unwrap();
    r.pop(&mut [0u8; 30]).unwrap(); // head=30, len=20, tail=50
    {
        let (a, b) = r.writable_mut().unwrap();
        assert_eq!(a.len() + b.len(), 44);
        assert_eq!(a.len(), 14); // 50..64
        a.fill(0xAA);
        b[..5].fill(0xBB);
    }
    r.commit(19).unwrap();
    let c = contents(&r);
    assert_eq!(c.len(), 39);
    assert!(c[20..34].iter().all(|&x| x == 0xAA));
    assert!(c[34..39].iter().all(|&x| x == 0xBB));
}

#[test]
fn capacity_validation() {
    assert_eq!(check_capacity(0), Err(RingError::BadCapacity));
    assert_eq!(check_capacity(MIN_CAPACITY as u64 - 1), Err(RingError::BadCapacity));
    assert_eq!(check_capacity(MIN_CAPACITY as u64), Ok(MIN_CAPACITY));
    assert_eq!(check_capacity(MAX_CAPACITY as u64), Ok(MAX_CAPACITY));
    assert_eq!(check_capacity(MAX_CAPACITY as u64 + 1), Err(RingError::BadCapacity));
    assert_eq!(check_capacity(1 << 60), Err(RingError::BadCapacity));
    assert_eq!(check_capacity(u64::MAX), Err(RingError::BadCapacity));
}

#[test]
fn clear_resets() {
    let mut r = ring(64);
    r.push(&[1u8; 40]).unwrap();
    r.clear();
    assert!(r.is_empty());
    assert_eq!(r.free(), 64);
    assert_eq!(r.push(&[2u8; 64]), Ok(64));
}
