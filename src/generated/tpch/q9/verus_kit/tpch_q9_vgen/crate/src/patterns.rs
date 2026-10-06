// The tracked patterns: '%color%' for the 92 TPC-H colors.
use vstd::prelude::*;

verus! {

pub fn tracked_patterns() -> (pats: Vec<Vec<u8>>)
    ensures pats.len() == 92,
{
    let mut pats: Vec<Vec<u8>> = Vec::new();
    pats.push(vec![b'%', b'a', b'l', b'm', b'o', b'n', b'd', b'%']);
    pats.push(vec![b'%', b'a', b'n', b't', b'i', b'q', b'u', b'e', b'%']);
    pats.push(vec![b'%', b'a', b'q', b'u', b'a', b'm', b'a', b'r', b'i', b'n', b'e', b'%']);
    pats.push(vec![b'%', b'a', b'z', b'u', b'r', b'e', b'%']);
    pats.push(vec![b'%', b'b', b'e', b'i', b'g', b'e', b'%']);
    pats.push(vec![b'%', b'b', b'i', b's', b'q', b'u', b'e', b'%']);
    pats.push(vec![b'%', b'b', b'l', b'a', b'c', b'k', b'%']);
    pats.push(vec![b'%', b'b', b'l', b'a', b'n', b'c', b'h', b'e', b'd', b'%']);
    pats.push(vec![b'%', b'b', b'l', b'u', b'e', b'%']);
    pats.push(vec![b'%', b'b', b'l', b'u', b's', b'h', b'%']);
    pats.push(vec![b'%', b'b', b'r', b'o', b'w', b'n', b'%']);
    pats.push(vec![b'%', b'b', b'u', b'r', b'l', b'y', b'w', b'o', b'o', b'd', b'%']);
    pats.push(vec![b'%', b'b', b'u', b'r', b'n', b'i', b's', b'h', b'e', b'd', b'%']);
    pats.push(vec![b'%', b'c', b'h', b'a', b'r', b't', b'r', b'e', b'u', b's', b'e', b'%']);
    pats.push(vec![b'%', b'c', b'h', b'i', b'f', b'f', b'o', b'n', b'%']);
    pats.push(vec![b'%', b'c', b'h', b'o', b'c', b'o', b'l', b'a', b't', b'e', b'%']);
    pats.push(vec![b'%', b'c', b'o', b'r', b'a', b'l', b'%']);
    pats.push(vec![b'%', b'c', b'o', b'r', b'n', b'f', b'l', b'o', b'w', b'e', b'r', b'%']);
    pats.push(vec![b'%', b'c', b'o', b'r', b'n', b's', b'i', b'l', b'k', b'%']);
    pats.push(vec![b'%', b'c', b'r', b'e', b'a', b'm', b'%']);
    pats.push(vec![b'%', b'c', b'y', b'a', b'n', b'%']);
    pats.push(vec![b'%', b'd', b'a', b'r', b'k', b'%']);
    pats.push(vec![b'%', b'd', b'e', b'e', b'p', b'%']);
    pats.push(vec![b'%', b'd', b'i', b'm', b'%']);
    pats.push(vec![b'%', b'd', b'o', b'd', b'g', b'e', b'r', b'%']);
    pats.push(vec![b'%', b'd', b'r', b'a', b'b', b'%']);
    pats.push(vec![b'%', b'f', b'i', b'r', b'e', b'b', b'r', b'i', b'c', b'k', b'%']);
    pats.push(vec![b'%', b'f', b'l', b'o', b'r', b'a', b'l', b'%']);
    pats.push(vec![b'%', b'f', b'o', b'r', b'e', b's', b't', b'%']);
    pats.push(vec![b'%', b'f', b'r', b'o', b's', b't', b'e', b'd', b'%']);
    pats.push(vec![b'%', b'g', b'a', b'i', b'n', b's', b'b', b'o', b'r', b'o', b'%']);
    pats.push(vec![b'%', b'g', b'h', b'o', b's', b't', b'%']);
    pats.push(vec![b'%', b'g', b'o', b'l', b'd', b'e', b'n', b'r', b'o', b'd', b'%']);
    pats.push(vec![b'%', b'g', b'r', b'e', b'e', b'n', b'%']);
    pats.push(vec![b'%', b'g', b'r', b'e', b'y', b'%']);
    pats.push(vec![b'%', b'h', b'o', b'n', b'e', b'y', b'd', b'e', b'w', b'%']);
    pats.push(vec![b'%', b'h', b'o', b't', b'%']);
    pats.push(vec![b'%', b'i', b'n', b'd', b'i', b'a', b'n', b'%']);
    pats.push(vec![b'%', b'i', b'v', b'o', b'r', b'y', b'%']);
    pats.push(vec![b'%', b'k', b'h', b'a', b'k', b'i', b'%']);
    pats.push(vec![b'%', b'l', b'a', b'c', b'e', b'%']);
    pats.push(vec![b'%', b'l', b'a', b'v', b'e', b'n', b'd', b'e', b'r', b'%']);
    pats.push(vec![b'%', b'l', b'a', b'w', b'n', b'%']);
    pats.push(vec![b'%', b'l', b'e', b'm', b'o', b'n', b'%']);
    pats.push(vec![b'%', b'l', b'i', b'g', b'h', b't', b'%']);
    pats.push(vec![b'%', b'l', b'i', b'm', b'e', b'%']);
    pats.push(vec![b'%', b'l', b'i', b'n', b'e', b'n', b'%']);
    pats.push(vec![b'%', b'm', b'a', b'g', b'e', b'n', b't', b'a', b'%']);
    pats.push(vec![b'%', b'm', b'a', b'r', b'o', b'o', b'n', b'%']);
    pats.push(vec![b'%', b'm', b'e', b'd', b'i', b'u', b'm', b'%']);
    pats.push(vec![b'%', b'm', b'e', b't', b'a', b'l', b'l', b'i', b'c', b'%']);
    pats.push(vec![b'%', b'm', b'i', b'd', b'n', b'i', b'g', b'h', b't', b'%']);
    pats.push(vec![b'%', b'm', b'i', b'n', b't', b'%']);
    pats.push(vec![b'%', b'm', b'i', b's', b't', b'y', b'%']);
    pats.push(vec![b'%', b'm', b'o', b'c', b'c', b'a', b's', b'i', b'n', b'%']);
    pats.push(vec![b'%', b'n', b'a', b'v', b'a', b'j', b'o', b'%']);
    pats.push(vec![b'%', b'n', b'a', b'v', b'y', b'%']);
    pats.push(vec![b'%', b'o', b'l', b'i', b'v', b'e', b'%']);
    pats.push(vec![b'%', b'o', b'r', b'a', b'n', b'g', b'e', b'%']);
    pats.push(vec![b'%', b'o', b'r', b'c', b'h', b'i', b'd', b'%']);
    pats.push(vec![b'%', b'p', b'a', b'l', b'e', b'%']);
    pats.push(vec![b'%', b'p', b'a', b'p', b'a', b'y', b'a', b'%']);
    pats.push(vec![b'%', b'p', b'e', b'a', b'c', b'h', b'%']);
    pats.push(vec![b'%', b'p', b'e', b'r', b'u', b'%']);
    pats.push(vec![b'%', b'p', b'i', b'n', b'k', b'%']);
    pats.push(vec![b'%', b'p', b'l', b'u', b'm', b'%']);
    pats.push(vec![b'%', b'p', b'o', b'w', b'd', b'e', b'r', b'%']);
    pats.push(vec![b'%', b'p', b'u', b'f', b'f', b'%']);
    pats.push(vec![b'%', b'p', b'u', b'r', b'p', b'l', b'e', b'%']);
    pats.push(vec![b'%', b'r', b'e', b'd', b'%']);
    pats.push(vec![b'%', b'r', b'o', b's', b'e', b'%']);
    pats.push(vec![b'%', b'r', b'o', b's', b'y', b'%']);
    pats.push(vec![b'%', b'r', b'o', b'y', b'a', b'l', b'%']);
    pats.push(vec![b'%', b's', b'a', b'd', b'd', b'l', b'e', b'%']);
    pats.push(vec![b'%', b's', b'a', b'l', b'm', b'o', b'n', b'%']);
    pats.push(vec![b'%', b's', b'a', b'n', b'd', b'y', b'%']);
    pats.push(vec![b'%', b's', b'e', b'a', b's', b'h', b'e', b'l', b'l', b'%']);
    pats.push(vec![b'%', b's', b'i', b'e', b'n', b'n', b'a', b'%']);
    pats.push(vec![b'%', b's', b'k', b'y', b'%']);
    pats.push(vec![b'%', b's', b'l', b'a', b't', b'e', b'%']);
    pats.push(vec![b'%', b's', b'm', b'o', b'k', b'e', b'%']);
    pats.push(vec![b'%', b's', b'n', b'o', b'w', b'%']);
    pats.push(vec![b'%', b's', b'p', b'r', b'i', b'n', b'g', b'%']);
    pats.push(vec![b'%', b's', b't', b'e', b'e', b'l', b'%']);
    pats.push(vec![b'%', b't', b'a', b'n', b'%']);
    pats.push(vec![b'%', b't', b'h', b'i', b's', b't', b'l', b'e', b'%']);
    pats.push(vec![b'%', b't', b'o', b'm', b'a', b't', b'o', b'%']);
    pats.push(vec![b'%', b't', b'u', b'r', b'q', b'u', b'o', b'i', b's', b'e', b'%']);
    pats.push(vec![b'%', b'v', b'i', b'o', b'l', b'e', b't', b'%']);
    pats.push(vec![b'%', b'w', b'h', b'e', b'a', b't', b'%']);
    pats.push(vec![b'%', b'w', b'h', b'i', b't', b'e', b'%']);
    pats.push(vec![b'%', b'y', b'e', b'l', b'l', b'o', b'w', b'%']);
    pats
}

}
