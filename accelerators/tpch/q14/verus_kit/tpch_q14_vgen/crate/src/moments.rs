use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::sum;
use verus_common::sqlfns::like_match;
use verus_common::rel::pow2;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use verus_prooflib::wide::Wide;

verus! {

pub struct Moments {
    pub count: Wide,
    pub total: Wide,
    pub promo: Wide,
}

pub struct MomentsView {
    pub count: int,
    pub total: int,
    pub promo: int,
}

pub open spec fn zero() -> MomentsView { MomentsView { count: 0, total: 0, promo: 0 } }

pub open spec fn moments_view(value: Moments) -> MomentsView {
    MomentsView { count: value.count@, total: value.total@, promo: value.promo@ }
}

pub open spec fn is_zero(value: Moments) -> bool {
    value.count@ == 0 && value.total@ == 0 && value.promo@ == 0
}

impl Moments {
    pub fn dup(&self) -> (out: Moments)
        ensures out == *self,
    { Moments { count: self.count.dup(), total: self.total.dup(), promo: self.promo.dup() } }

    pub fn is_empty(&self) -> (out: bool)
        ensures out == is_zero(*self),
    { self.count.is_zero() && self.total.is_zero() && self.promo.is_zero() }
}

pub fn empty() -> (out: Moments)
    ensures moments_view(out) == zero(),
{ Moments { count: Wide::zero(), total: Wide::zero(), promo: Wide::zero() } }

pub open spec fn part_count(rows: Seq<PartRow>, key: int) -> int {
    sum(rows, |part: PartRow| if part.p_partkey == key { 1 } else { 0 })
}

pub open spec fn promo_count(rows: Seq<PartRow>, key: int) -> int {
    sum(rows, |part: PartRow| if part.p_partkey == key &&
        like_match(part.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%']) { 1 } else { 0 })
}

pub open spec fn part_change(rows: Seq<PartDelta>, key: int) -> int {
    sum(rows, |part: PartDelta| if part.p_partkey as int == key { part.sign as int } else { 0 })
}

pub open spec fn promo_change(rows: Seq<PartDelta>, key: int) -> int {
    sum(rows, |part: PartDelta| if part.p_partkey as int == key && part.is_promo != 0 {
        part.sign as int
    } else { 0 })
}

pub open spec fn block_id(date: int, level: nat) -> int {
    pow2((32 - level) as nat) + (date + 2147483648) / pow2(level)
}

pub open spec fn in_block(id: u64, date: int) -> bool {
    in_block_prefix(id, date, 33)
}

pub open spec fn in_block_prefix(id: u64, date: int, levels: nat) -> bool {
    -2147483648 <= date < 2147483648 &&
        exists|level: nat| #[trigger] block_id(date, level) == id as int && level < levels && level <= 32
}

pub open spec fn weight(line: LineitemRow) -> int {
    line.l_extendedprice * (100 - line.l_discount)
}

pub open spec fn block_count(db: Db, id: u64) -> int {
    joined(db, |line: LineitemRow| in_block(id, line.l_shipdate), false, false)
}

pub open spec fn block_total(db: Db, id: u64) -> int {
    joined(db, |line: LineitemRow| in_block(id, line.l_shipdate), true, false)
}

pub open spec fn block_promo(db: Db, id: u64) -> int {
    joined(db, |line: LineitemRow| in_block(id, line.l_shipdate), true, true)
}

pub open spec fn scanned_count(parts: Seq<PartInput>, lines: Seq<LineInput>, id: u64) -> int {
    block_count(Db { part: part_input_seq_view(parts), lineitem: line_input_seq_view(lines) }, id)
}

pub open spec fn scanned_total(parts: Seq<PartInput>, lines: Seq<LineInput>, id: u64) -> int {
    block_total(Db { part: part_input_seq_view(parts), lineitem: line_input_seq_view(lines) }, id)
}

pub open spec fn scanned_promo(parts: Seq<PartInput>, lines: Seq<LineInput>, id: u64) -> int {
    block_promo(Db { part: part_input_seq_view(parts), lineitem: line_input_seq_view(lines) }, id)
}

pub open spec fn key_value(map: Map<i64, Moments>, key: i64) -> MomentsView {
    if map.dom().contains(key) { moments_view(map[key]) } else { zero() }
}

pub open spec fn date_key_spec(date: i32) -> int { date as int + 2147483648 }

pub open spec fn interval_count(db: Db, start: int, end: int) -> int {
    joined(db, |line: LineitemRow| start <= line.l_shipdate < end, false, false)
}

pub open spec fn interval_total(db: Db, start: int, end: int) -> int {
    joined(db, |line: LineitemRow| start <= line.l_shipdate < end, true, false)
}

pub open spec fn interval_promo(db: Db, start: int, end: int) -> int {
    joined(db, |line: LineitemRow| start <= line.l_shipdate < end, true, true)
}

pub open spec fn captured_count(db: Db, rows: Seq<LineDelta>, id: u64) -> int {
    captured(db, rows, |line: LineitemRow| in_block(id, line.l_shipdate), false, false)
}

pub open spec fn captured_total(db: Db, rows: Seq<LineDelta>, id: u64) -> int {
    captured(db, rows, |line: LineitemRow| in_block(id, line.l_shipdate), true, false)
}

pub open spec fn captured_promo(db: Db, rows: Seq<LineDelta>, id: u64) -> int {
    captured(db, rows, |line: LineitemRow| in_block(id, line.l_shipdate), true, true)
}

pub open spec fn rescanned_count(parts: Seq<PartDelta>, rows: Seq<LineInput>, id: u64) -> int {
    rescanned(parts, rows, |line: LineitemRow| in_block(id, line.l_shipdate), false, false)
}

pub open spec fn rescanned_total(parts: Seq<PartDelta>, rows: Seq<LineInput>, id: u64) -> int {
    rescanned(parts, rows, |line: LineitemRow| in_block(id, line.l_shipdate), true, false)
}

pub open spec fn rescanned_promo(parts: Seq<PartDelta>, rows: Seq<LineInput>, id: u64) -> int {
    rescanned(parts, rows, |line: LineitemRow| in_block(id, line.l_shipdate), true, true)
}

pub open spec fn catalog(parts: Seq<PartRow>, key: int, promotional: bool) -> int {
    if promotional { promo_count(parts, key) } else { part_count(parts, key) }
}

pub open spec fn changed(parts: Seq<PartDelta>, key: int, promotional: bool) -> int {
    if promotional { promo_change(parts, key) } else { part_change(parts, key) }
}

pub open spec fn part_indicator(part: PartRow, key: int, promotional: bool) -> int {
    if part.p_partkey == key && (!promotional ||
        like_match(part.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%'])) { 1 } else { 0 }
}

pub open spec fn contribution(parts: Seq<PartRow>, line: LineitemRow,
    selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool) -> int {
    if selected(line) { catalog(parts, line.l_partkey, promotional) * if weighted { weight(line) } else { 1 } } else { 0 }
}

pub open spec fn delta_contribution(parts: Seq<PartDelta>, line: LineitemRow,
    selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool) -> int {
    if selected(line) { changed(parts, line.l_partkey, promotional) * if weighted { weight(line) } else { 1 } } else { 0 }
}

pub open spec fn joined(db: Db, selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool) -> int {
    sum(db.lineitem, |line: LineitemRow| contribution(db.part, line, selected, weighted, promotional))
}

pub open spec fn captured(db: Db, rows: Seq<LineDelta>, selected: spec_fn(LineitemRow) -> bool,
    weighted: bool, promotional: bool) -> int {
    sum(rows, |delta: LineDelta| delta.sign as int * contribution(db.part, line_input_view(delta.row), selected, weighted, promotional))
}

pub open spec fn rescanned(parts: Seq<PartDelta>, rows: Seq<LineInput>, selected: spec_fn(LineitemRow) -> bool,
    weighted: bool, promotional: bool) -> int {
    sum(rows, |line: LineInput| delta_contribution(parts, line_input_view(line), selected, weighted, promotional))
}

pub open spec fn line_key_support(rows: Seq<LineDelta>, key: i64) -> bool {
    exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).row.l_partkey == key
}

pub open spec fn part_key_support(rows: Seq<PartDelta>, key: i64) -> bool {
    exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).p_partkey == key
}

pub open spec fn block_value(map: Map<u64, Moments>, key: u64) -> MomentsView {
    if map.dom().contains(key) { moments_view(map[key]) } else { zero() }
}

pub fn get_key(map: &HashMap<i64, Moments>, key: i64) -> (out: Moments)
    ensures moments_view(out) == key_value(map@, key),
{ match map.get(&key) { Some(value) => value.dup(), None => empty() } }

pub fn get_block(map: &HashMap<u64, Moments>, key: u64) -> (out: Moments)
    ensures moments_view(out) == block_value(map@, key),
{ match map.get(&key) { Some(value) => value.dup(), None => empty() } }

pub fn add(left: &Moments, right: &Moments) -> (out: Moments)
    ensures
        out.count@ == left.count@ + right.count@,
        out.total@ == left.total@ + right.total@,
        out.promo@ == left.promo@ + right.promo@,
{
    Moments { count: left.count.add(&right.count), total: left.total.add(&right.total), promo: left.promo.add(&right.promo) }
}

pub fn subtract(left: &Moments, right: &Moments) -> (out: Moments)
    ensures
        out.count@ == left.count@ - right.count@,
        out.total@ == left.total@ - right.total@,
        out.promo@ == left.promo@ - right.promo@,
{
    Moments { count: left.count.sub(&right.count), total: left.total.sub(&right.total), promo: left.promo.sub(&right.promo) }
}

pub fn scale(value: &Moments, factor: i64) -> (out: Moments)
    ensures
        out.count@ == factor as int * value.count@,
        out.total@ == factor as int * value.total@,
        out.promo@ == factor as int * value.promo@,
{
    let factor = Wide::from_i128(factor as i128);
    Moments { count: factor.mul(&value.count), total: factor.mul(&value.total), promo: factor.mul(&value.promo) }
}

pub fn line_moments(counts: &Moments, line: LineInput) -> (out: Moments)
    ensures
        out.count@ == counts.count@,
        out.total@ == counts.count@ * weight(line_input_view(line)),
        out.promo@ == counts.promo@ * weight(line_input_view(line)),
{
    let revenue = Wide::from_i128(line.l_extendedprice as i128).mul(&Wide::from_i128(100i128 - line.l_discount as i128));
    Moments { count: counts.count.dup(), total: counts.count.mul(&revenue), promo: counts.promo.mul(&revenue) }
}

pub fn is_promo(text: &Vec<u8>) -> (out: bool)
    ensures out == like_match(text@, seq![b'P', b'R', b'O', b'M', b'O', b'%']),
{
    proof {
        let prefix = seq![b'P', b'R', b'O', b'M', b'O'];
        lemma_like_prefix(text@, prefix);
        assert(prefix + seq![b'%'] =~= seq![b'P', b'R', b'O', b'M', b'O', b'%']);
        reveal_with_fuel(begins, 6);
    }
    text.len() >= 5 && text[0] == b'P' && text[1] == b'R' && text[2] == b'O'
        && text[3] == b'M' && text[4] == b'O'
}

pub proof fn lemma_like_percent(text: Seq<u8>)
    ensures like_match(text, seq![b'%']),
    decreases text.len(),
{
    if text.len() > 0 { lemma_like_percent(text.drop_first()); }
    reveal_with_fuel(like_match, 2);
}

pub open spec fn begins(text: Seq<u8>, prefix: Seq<u8>) -> bool
    decreases prefix.len(),
{
    prefix.len() == 0 || text.len() > 0 && text[0] == prefix[0] && begins(text.drop_first(), prefix.drop_first())
}

pub proof fn lemma_like_prefix(text: Seq<u8>, prefix: Seq<u8>)
    requires forall|index: int| 0 <= index < prefix.len() ==> (#[trigger] prefix[index]) != b'%' && prefix[index] != b'_',
    ensures like_match(text, prefix + seq![b'%']) == begins(text, prefix),
    decreases prefix.len(),
{
    if prefix.len() == 0 {
        assert(prefix + seq![b'%'] =~= seq![b'%']);
        lemma_like_percent(text);
    } else {
        let pattern = prefix + seq![b'%'];
        assert(pattern.drop_first() =~= prefix.drop_first() + seq![b'%']);
        if text.len() > 0 {
            assert forall|index: int| 0 <= index < prefix.drop_first().len() implies
                (#[trigger] prefix.drop_first()[index]) != b'%' && prefix.drop_first()[index] != b'_' by {
                assert(prefix.drop_first()[index] == prefix[index + 1]);
            }
            lemma_like_prefix(text.drop_first(), prefix.drop_first());
        }
        assert(prefix[0] != b'%' && prefix[0] != b'_');
    }
}

pub fn date_key(date: i32) -> (out: u64)
    ensures out as int == date as int + 2147483648,
{ (date as i64 + 2147483648i64) as u64 }

}
