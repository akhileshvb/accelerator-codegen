use vstd::prelude::*;
use verus_common::sums::sum;
use verus_prooflib::ordered::{KeyView, Index};
use crate::gen_spec::*;

verus! {
#[derive(Clone, Copy)]
pub struct Day { pub shipdate: i32, pub count: u64, pub revenue: i128 }
pub struct BucketProfile { pub count: u64, pub revenue: i128, pub days: Vec<Day> }
pub struct Variant {
    pub name: Vec<u8>, pub address: Vec<u8>, pub phone: Vec<u8>, pub count: u64,
}
pub type Facts = Index<Index<BucketProfile>>;
pub type Dimensions = Index<Index<Variant>>;
pub open spec fn bucket(date: int) -> int { date / 256 }
pub open spec fn weight(row: LineitemRow) -> int { row.l_extendedprice * (100 - row.l_discount) }
pub open spec fn number_key(number: int) -> KeyView { (number, Seq::empty()) }
pub open spec fn bucket_count(rows: Seq<LineitemRow>, bin: int, supplier: int) -> int {
    sum(rows, |row: LineitemRow| if bucket(row.l_shipdate) == bin && row.l_suppkey == supplier { 1int } else { 0int })
}
pub open spec fn bucket_revenue(rows: Seq<LineitemRow>, bin: int, supplier: int) -> int {
    sum(rows, |row: LineitemRow| if bucket(row.l_shipdate) == bin && row.l_suppkey == supplier { weight(row) } else { 0int })
}
pub open spec fn day_count(rows: Seq<LineitemRow>, supplier: int, date: int) -> int {
    sum(rows, |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int })
}
pub open spec fn day_revenue(rows: Seq<LineitemRow>, supplier: int, date: int) -> int {
    sum(rows, |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int })
}
pub open spec fn profile_at(facts: Facts, bin: int, supplier: int) -> Option<BucketProfile> {
    match facts.at(number_key(bin)) { Some(inner) => inner.at(number_key(supplier)), None => None }
}
pub open spec fn stored_count(facts: Facts, bin: int, supplier: int) -> int {
    match profile_at(facts, bin, supplier) { Some(profile) => profile.count as int, None => 0 }
}
pub open spec fn stored_revenue(facts: Facts, bin: int, supplier: int) -> int {
    match profile_at(facts, bin, supplier) { Some(profile) => profile.revenue as int, None => 0 }
}
pub open spec fn stored_day_count(facts: Facts, supplier: int, date: int) -> int {
    match profile_at(facts, bucket(date), supplier) {
        Some(profile) => days_count(profile.days@, date),
        None => 0,
    }
}
pub open spec fn stored_day_revenue(facts: Facts, supplier: int, date: int) -> int {
    match profile_at(facts, bucket(date), supplier) {
        Some(profile) => days_revenue(profile.days@, date),
        None => 0,
    }
}
pub open spec fn days_count(days: Seq<Day>, date: int) -> int {
    sum(days, |day: Day| if day.shipdate as int == date { day.count as int } else { 0int })
}
pub open spec fn days_revenue(days: Seq<Day>, date: int) -> int {
    sum(days, |day: Day| if day.shipdate as int == date { day.revenue as int } else { 0int })
}
pub open spec fn days_shape(days: Seq<Day>, bin: int) -> bool {
    &&& days.len() <= 256
    &&& forall|position: int| 0 <= position < days.len() ==> (#[trigger] days[position]).count > 0 && bucket(days[position].shipdate as int) == bin
    &&& forall|left: int, right: int| #![trigger days[left], days[right]]
        0 <= left < right < days.len() ==> days[left].shipdate < days[right].shipdate
}
pub open spec fn profile_matches(profile: BucketProfile, rows: Seq<LineitemRow>, bin: int, supplier: int) -> bool {
    &&& 0 < profile.count
    &&& profile.days.len() <= 256
    &&& profile.count as int == bucket_count(rows, bin, supplier)
    &&& profile.revenue as int == bucket_revenue(rows, bin, supplier)
    &&& profile.count as int == sum(profile.days@, |day: Day| day.count as int)
    &&& profile.revenue as int == sum(profile.days@, |day: Day| day.revenue as int)
    &&& forall|position: int| 0 <= position < profile.days.len() ==> (
            #[trigger] profile.days@[position].count > 0 &&
            bucket(profile.days@[position].shipdate as int) == bin &&
            profile.days@[position].count as int == day_count(rows, supplier, profile.days@[position].shipdate as int) &&
            profile.days@[position].revenue as int == day_revenue(rows, supplier, profile.days@[position].shipdate as int))
    &&& forall|left: int, right: int| #![trigger profile.days@[left], profile.days@[right]]
        0 <= left < right < profile.days.len() ==> profile.days@[left].shipdate < profile.days@[right].shipdate
    &&& forall|date: int| #[trigger] day_count(rows, supplier, date) > 0 && bucket(date) == bin ==>
        exists|position: int| 0 <= position < profile.days.len() && (#[trigger] profile.days@[position]).shipdate as int == date
}
pub open spec fn profile_shape(profile: BucketProfile, bin: int) -> bool {
    &&& profile.count > 0
    &&& profile.days.len() <= 256
    &&& profile.count as int == sum(profile.days@, |day: Day| day.count as int)
    &&& profile.revenue as int == sum(profile.days@, |day: Day| day.revenue as int)
    &&& forall|position: int| 0 <= position < profile.days.len() ==>
        (#[trigger] profile.days@[position]).count > 0 && bucket(profile.days@[position].shipdate as int) == bin
    &&& forall|left: int, right: int| #![trigger profile.days@[left], profile.days@[right]]
        0 <= left < right < profile.days.len() ==> profile.days@[left].shipdate < profile.days@[right].shipdate
}
pub open spec fn facts_shape(facts: Facts) -> bool {
    &&& forall|bin: int, supplier: int| #[trigger] profile_at(facts, bin, supplier) matches Some(profile) ==>
        profile_shape(profile, bin)
    &&& forall|key: KeyView| #[trigger] facts.at(key) matches Some(inner) ==> (
        key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX && inner.wf() && inner.entries().len() > 0 &&
        forall|supplier: KeyView| #[trigger] inner.at(supplier) is Some ==> supplier.1 == Seq::<u8>::empty() && i64::MIN <= supplier.0 <= i64::MAX)
}
pub open spec fn facts_match(facts: Facts, rows: Seq<LineitemRow>) -> bool {
    &&& facts_shape(facts)
    &&& forall|supplier: int, date: int| #[trigger] stored_day_count(facts, supplier, date) == day_count(rows, supplier, date)
    &&& forall|supplier: int, date: int| #[trigger] stored_day_revenue(facts, supplier, date) == day_revenue(rows, supplier, date)
}
pub open spec fn encode_part(bytes: Seq<u8>) -> Seq<u8>
{
    Seq::new(2 * bytes.len() + 1, |position: int|
        if position == 2 * bytes.len() { 0u8 }
        else if position % 2 == 0 { 1u8 } else { bytes[position / 2] })
}
pub open spec fn variant_key(name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>) -> KeyView {
    (0int, encode_part(name) + encode_part(address) + encode_part(phone))
}
pub open spec fn supplier_count(rows: Seq<SupplierRow>, supplier: int, key: KeyView) -> int {
    sum(rows, |row: SupplierRow| if row.s_suppkey == supplier &&
        variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int })
}
pub open spec fn variant_at(dimensions: Dimensions, supplier: int, key: KeyView) -> Option<Variant> {
    match dimensions.at(number_key(supplier)) { Some(inner) => inner.at(key), None => None }
}
pub open spec fn stored_supplier_count(dimensions: Dimensions, supplier: int, key: KeyView) -> int {
    match variant_at(dimensions, supplier, key) { Some(value) => value.count as int, None => 0 }
}
pub open spec fn dimensions_shape(dimensions: Dimensions) -> bool {
    &&& forall|supplier: int, key: KeyView| #[trigger] variant_at(dimensions, supplier, key) matches Some(value) ==>
        value.count > 0 && key == variant_key(value.name@, value.address@, value.phone@)
    &&& forall|key: KeyView| #[trigger] dimensions.at(key) matches Some(inner) ==>
        key.1 == Seq::<u8>::empty() && inner.wf() && inner.entries().len() > 0
}
pub open spec fn dimensions_match(dimensions: Dimensions, rows: Seq<SupplierRow>) -> bool {
    dimensions_shape(dimensions) && forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(dimensions, supplier, key)
        == supplier_count(rows, supplier, key)
}
}
