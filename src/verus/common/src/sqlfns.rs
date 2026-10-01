// SQL scalar functions used by the generated specs.  These definitions are
// part of every spec that uses them (trusted): each states the SQL meaning.
use vstd::prelude::*;

verus! {

/// s LIKE p, p a pattern without ESCAPE: '%' matches any run of bytes, '_'
/// one byte, every other byte itself.  (Bytes = characters for ASCII text;
/// the host contract requires ASCII where '_' is used.)
pub open spec fn like_match(s: Seq<u8>, p: Seq<u8>) -> bool
    decreases p.len() + s.len(),
{
    if p.len() == 0 {
        s.len() == 0
    } else if p[0] == 37u8 {
        // '%'
        like_match(s, p.drop_first()) || (s.len() > 0 && like_match(s.drop_first(), p))
    } else if s.len() == 0 {
        false
    } else if p[0] == 95u8 || p[0] == s[0] {
        // '_' or the same byte
        like_match(s.drop_first(), p.drop_first())
    } else {
        false
    }
}

pub open spec fn clamp(x: int, lo: int, hi: int) -> int {
    if x < lo { lo } else if x > hi { hi } else { x }
}

/// SUBSTRING(s FROM start FOR len), len >= 0, positions counted from 1:
/// the bytes at positions [start, start + len) that exist.
pub open spec fn sql_substr(s: Seq<u8>, start: int, len: int) -> Seq<u8> {
    s.subrange(clamp(start - 1, 0, s.len() as int), clamp(start - 1 + len, clamp(start - 1, 0, s.len() as int), s.len() as int))
}

// Calendar (proleptic Gregorian) from a day number d = days since 1970-01-01
// (H. Hinnant's civil_from_days; / and % are floor division for positive
// divisors).
pub open spec fn civil_doe(d: int) -> int {
    (d + 719468) % 146097
}

pub open spec fn civil_yoe(d: int) -> int {
    let doe = civil_doe(d);
    (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365
}

pub open spec fn civil_doy(d: int) -> int {
    let yoe = civil_yoe(d);
    civil_doe(d) - (365 * yoe + yoe / 4 - yoe / 100)
}

pub open spec fn civil_mp(d: int) -> int {
    (5 * civil_doy(d) + 2) / 153
}

/// EXTRACT(MONTH FROM d), 1..12.
pub open spec fn civil_month(d: int) -> int {
    if civil_mp(d) < 10 { civil_mp(d) + 3 } else { civil_mp(d) - 9 }
}

/// EXTRACT(DAY FROM d), 1..31.
pub open spec fn civil_day(d: int) -> int {
    civil_doy(d) - (153 * civil_mp(d) + 2) / 5 + 1
}

/// EXTRACT(YEAR FROM d).
pub open spec fn civil_year(d: int) -> int {
    civil_yoe(d) + ((d + 719468) / 146097) * 400 + (if civil_month(d) <= 2 { 1int } else { 0 })
}

/// a/b < c/d for non-zero b and d (multiply through by (b d)^2 > 0).
pub open spec fn ratio_lt(a: int, b: int, c: int, d: int) -> bool {
    a * b * d * d < c * d * b * b
}

/// a/b = c/d for non-zero b and d.
pub open spec fn ratio_eq(a: int, b: int, c: int, d: int) -> bool {
    a * d == c * b
}

/// a / b truncated toward zero (SQL integer division), b != 0.
pub open spec fn trunc_div(a: int, b: int) -> int {
    let q = (if a >= 0 { a } else { -a }) / (if b >= 0 { b } else { -b });
    if (a >= 0) == (b >= 0) { q } else { -q }
}

/// a % b with the sign of a (SQL MOD), b != 0.
pub open spec fn trunc_rem(a: int, b: int) -> int {
    a - b * trunc_div(a, b)
}

/// v (an integer at scale 10^d more than the result) rounded half away from
/// zero to the result's scale: SQL ROUND on DECIMAL.
pub open spec fn round_half_away(v: int, d: nat) -> int {
    let p = pow10(d);
    let a = if v >= 0 { v } else { -v };
    let q = if 2 * (a % p) >= p { a / p + 1 } else { a / p };
    if v >= 0 { q } else { -q }
}

pub open spec fn pow10(d: nat) -> int
    decreases d,
{
    if d == 0 { 1 } else { 10 * pow10((d - 1) as nat) }
}

/// UPPER on ASCII text (other bytes unchanged).
pub open spec fn ascii_upper(s: Seq<u8>) -> Seq<u8> {
    s.map_values(|b: u8| if 97 <= b <= 122 { (b - 32) as u8 } else { b })
}

/// LOWER on ASCII text (other bytes unchanged).
pub open spec fn ascii_lower(s: Seq<u8>) -> Seq<u8> {
    s.map_values(|b: u8| if 65 <= b <= 90 { (b + 32) as u8 } else { b })
}

} // verus!
