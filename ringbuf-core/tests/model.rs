// SPDX-License-Identifier: GPL-2.0
//! Property-based "model" test.
//!
//! proptest generates thousands of random operation sequences -- including
//! hostile values such as `usize::MAX` offsets -- and runs each one against
//! both the real `RingBuf` and a trivially-correct model (`VecDeque`). After
//! every operation the two must agree on the result *and* on the contents.
//! Any panic inside `RingBuf` also fails the test.

use std::collections::VecDeque;

use proptest::prelude::*;
use ringbuf_core::{RingBuf, RingError};

#[derive(Debug, Clone)]
enum Op {
    Push(Vec<u8>),
    Pop(usize),
    Peek(usize, usize),
    PeekSlices(usize, usize),
    /// Fill `fill` bytes via `writable_mut`, then `commit(commit)`.
    WriteCommit { fill: usize, commit: usize },
    Consume(usize),
    Clear,
}

/// Mostly small numbers, sometimes huge or boundary values.
fn size() -> impl Strategy<Value = usize> {
    prop_oneof![
        8 => 0usize..300,
        1 => Just(usize::MAX),
        1 => Just(usize::MAX - 1),
        1 => any::<usize>(),
    ]
}

fn op() -> impl Strategy<Value = Op> {
    prop_oneof![
        4 => proptest::collection::vec(any::<u8>(), 0..200).prop_map(Op::Push),
        3 => size().prop_map(Op::Pop),
        2 => (size(), size()).prop_map(|(o, n)| Op::Peek(o, n.min(4096))),
        2 => (size(), size()).prop_map(|(o, n)| Op::PeekSlices(o, n)),
        2 => (0usize..300, size()).prop_map(|(fill, commit)| Op::WriteCommit { fill, commit }),
        1 => size().prop_map(Op::Consume),
        1 => Just(Op::Clear),
    ]
}

fn capacity() -> impl Strategy<Value = usize> {
    prop_oneof![Just(0usize), Just(1usize), 2usize..300]
}

fn contents(r: &RingBuf<Vec<u8>>) -> Vec<u8> {
    let (a, b) = r.readable().expect("readable() must not fail on a consistent ring");
    [a, b].concat()
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(2000))]

    #[test]
    fn ring_matches_vecdeque_model(cap in capacity(), ops in proptest::collection::vec(op(), 0..120)) {
        let mut ring = RingBuf::new(vec![0u8; cap]);
        let mut model: VecDeque<u8> = VecDeque::new();
        let mut marker: u8 = 0;

        for op in ops {
            match op {
                Op::Push(data) => {
                    let free = cap - model.len();
                    let expected = if data.is_empty() {
                        Ok(0)
                    } else if free == 0 {
                        Err(RingError::Full)
                    } else {
                        Ok(free.min(data.len()))
                    };
                    let got = ring.push(&data);
                    prop_assert_eq!(got, expected);
                    if let Ok(n) = got {
                        model.extend(&data[..n]);
                    }
                }
                Op::Pop(want) => {
                    let want = want.min(4096);
                    let mut buf = vec![0u8; want];
                    let n = ring.pop(&mut buf).expect("pop never fails");
                    prop_assert_eq!(n, want.min(model.len()));
                    let expected: Vec<u8> = model.drain(..n).collect();
                    prop_assert_eq!(&buf[..n], &expected[..]);
                }
                Op::Peek(off, want) => {
                    let mut buf = vec![0u8; want];
                    let got = ring.peek(off, &mut buf);
                    if off > model.len() {
                        prop_assert_eq!(got, Err(RingError::OutOfRange));
                    } else {
                        let n = want.min(model.len() - off);
                        prop_assert_eq!(got, Ok(n));
                        let expected: Vec<u8> = model.range(off..off + n).copied().collect();
                        prop_assert_eq!(&buf[..n], &expected[..]);
                    }
                }
                Op::PeekSlices(off, cnt) => {
                    let got = ring.peek_slices(off, cnt);
                    match off.checked_add(cnt) {
                        Some(end) if end <= model.len() => {
                            let (a, b) = got.expect("in-range peek must succeed");
                            let expected: Vec<u8> = model.range(off..end).copied().collect();
                            prop_assert_eq!([a, b].concat(), expected);
                        }
                        _ => prop_assert_eq!(got.err(), Some(RingError::OutOfRange)),
                    }
                }
                Op::WriteCommit { fill, commit } => {
                    let free = cap - model.len();
                    let fill = fill.min(free);
                    {
                        let (a, b) = ring.writable_mut().expect("writable_mut never fails");
                        prop_assert_eq!(a.len() + b.len(), free);
                        for byte in a.iter_mut().chain(b.iter_mut()).take(fill) {
                            marker = marker.wrapping_add(1);
                            *byte = marker;
                        }
                    }
                    let got = ring.commit(commit);
                    if commit <= free {
                        prop_assert_eq!(got, Ok(()));
                        // replay the same marker sequence into the model
                        let mut m = marker.wrapping_sub(fill as u8);
                        for i in 0..commit {
                            if i < fill {
                                m = m.wrapping_add(1);
                                model.push_back(m);
                            } else {
                                // bytes committed but not freshly written: take
                                // whatever the ring holds (stale storage)
                                model.push_back(0);
                            }
                        }
                        if commit > fill {
                            // re-sync stale bytes from the ring itself; their
                            // value is unspecified, only their count matters
                            let actual = contents(&ring);
                            let start = model.len() - (commit - fill);
                            for (i, slot) in model.iter_mut().enumerate().skip(start) {
                                *slot = actual[i];
                            }
                        }
                    } else {
                        prop_assert_eq!(got, Err(RingError::OutOfRange));
                    }
                }
                Op::Consume(n) => {
                    let got = ring.consume(n);
                    if n <= model.len() {
                        prop_assert_eq!(got, Ok(()));
                        model.drain(..n);
                    } else {
                        prop_assert_eq!(got, Err(RingError::OutOfRange));
                    }
                }
                Op::Clear => {
                    ring.clear();
                    model.clear();
                }
            }

            // invariants after every single operation
            prop_assert_eq!(ring.len(), model.len());
            prop_assert!(ring.len() <= ring.capacity());
            prop_assert_eq!(ring.free(), cap - model.len());
            prop_assert_eq!(contents(&ring), model.iter().copied().collect::<Vec<u8>>());
        }
    }
}
