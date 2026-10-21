use crate::packed::{self, Packed, PackedCursor};
use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::{sum, lemma_sum_take};
use verus_prooflib::ordered::{Index, KeyView};
use crate::construction::{moment_measure, link_count};
use crate::inventory::moment_at;
use crate::model::*;
use crate::maintenance::profile_cell;
use crate::values::{scale, Moment, Mom};

verus! {

pub proof fn lemma_part_finish(groups: Packed, target: i64, nation: i64, part: i64, value: bool, factor: int, selected: spec_fn((KeyView, Moment)) -> int)
    requires groups.wf(), forall|row: (KeyView, Moment)| #[trigger] selected(row)
        == if nation == target && row.0 == numeric_key(part as int) { moment_measure(row.1, value) * factor } else { 0 },
    ensures sum(groups.entries(), selected) == if nation == target { moment_measure(moment_at(groups, part as int), value) * factor } else { 0 },
{
    if nation == target {
        packed::lemma_selected_weight(&groups, numeric_key(part as int), |row: (KeyView, Moment)| moment_measure(row.1, value), factor, selected);
    } else { verus_common::sums::lemma_sum_zero(groups.entries(), selected); }
}

pub fn weighted_parts(target: &mut Profiles, nation: i64, groups: &Packed, factor: &Wide)
    requires profile_layout(*old(target)), crate::construction::profile_keys_ok(*groups),
    ensures profile_layout(*final(target)),
        forall|query: i64, part: i64, value: bool| #[trigger] profile_cell(*final(target), query, part, value)
            == profile_cell(*old(target), query, part, value)
                + if query == nation { moment_measure(moment_at(*groups, part as int), value) * factor@ } else { 0 },
{
    let ghost initial = *target;
    let mut processed = Ghost(0int);
    let mut cursor = groups.cursor();
    loop
        invariant profile_layout(*target), initial == *old(target), crate::construction::profile_keys_ok(*groups),
            0 <= processed@ <= groups.entries().len(), cursor.remaining() == groups.entries().skip(processed@),
            forall|query: i64, part: i64, value: bool| #[trigger] profile_cell(*target, query, part, value)
                == profile_cell(initial, query, part, value) + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                    if query == nation && row.0 == numeric_key(part as int) { moment_measure(row.1, value) * factor@ } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (part, moment) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == groups.entries().len()); assert(groups.entries().take(processed@) =~= groups.entries());
                    assert forall|query: i64, part: i64, value: bool| #[trigger] profile_cell(*target, query, part, value)
                        == profile_cell(initial, query, part, value)
                            + if query == nation { moment_measure(moment_at(*groups, part as int), value) * factor@ } else { 0 } by {
                        let selected = |row: (KeyView, Moment)| if query == nation && row.0 == numeric_key(part as int) { moment_measure(row.1, value) * factor@ } else { 0 };
                        lemma_part_finish(*groups, nation, query, part, value, factor@, selected);
                        assert(profile_cell(*target, query, part, value) == profile_cell(initial, query, part, value) + sum(groups.entries(), selected));
                    }
                }
                return;
            },
        };
        proof { groups.model(); assert(numeric(part.view())); }
        let weighted = scale(&*moment, factor);
        let ghost previous = *target;
        add_profile(target, nation, part.number, &weighted, 8);
        proof {
            assert forall|query: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*target, query, selected_part, value)
                == profile_cell(initial, query, selected_part, value) + sum(groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                    if query == nation && row.0 == numeric_key(selected_part as int) { moment_measure(row.1, value) * factor@ } else { 0 }) by {
                let weight = |row: (KeyView, Moment)| if query == nation && row.0 == numeric_key(selected_part as int) { moment_measure(row.1, value) * factor@ } else { 0 };
                lemma_sum_take(groups.entries(), processed@, weight);
                assert(groups.entries()[processed@] == (part.view(), (*moment)@));
                assert(profile_cell(previous, query, selected_part, value) == profile_cell(initial, query, selected_part, value) + sum(groups.entries().take(processed@), weight));
                if query == nation && selected_part == part.number {
                    assert(profile_cell(*target, query, selected_part, value) == profile_cell(previous, query, selected_part, value) + moment_measure(weighted@, value));
                } else { assert(profile_cell(*target, query, selected_part, value) == profile_cell(previous, query, selected_part, value)); }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub proof fn lemma_link_finish(links: Index<Wide>, nation: i64, factor: int, selected: spec_fn((KeyView, Wide)) -> int)
    requires links.wf(), forall|row: (KeyView, Wide)| #[trigger] selected(row)
        == if row.0 == numeric_key(nation as int) { row.1@ * factor } else { 0 },
    ensures sum(links.entries(), selected) == link_count(links, nation) * factor,
{
    verus_prooflib::lemma_ordered_selected_weight(&links, numeric_key(nation as int), |row: (KeyView, Wide)| row.1@, factor, selected);
}

pub fn linked_part(target: &mut Profiles, part: i64, change: &Mom, links: &Index<Wide>)
    requires profile_layout(*old(target)), links.wf(),
        forall|key: KeyView| (#[trigger] links.at(key)).is_some() ==> numeric(key),
    ensures profile_layout(*final(target)),
        forall|nation: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*final(target), nation, selected_part, value)
            == profile_cell(*old(target), nation, selected_part, value)
                + if selected_part == part { link_count(*links, nation) * moment_measure(change@, value) } else { 0 },
{
    let ghost initial = *target;
    let mut processed = Ghost(0int);
    let mut cursor = links.cursor();
    loop
        invariant profile_layout(*target), initial == *old(target), links.wf(),
            forall|key: KeyView| (#[trigger] links.at(key)).is_some() ==> numeric(key),
            0 <= processed@ <= links.entries().len(), cursor.remaining() == links.entries().skip(processed@),
            forall|nation: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*target, nation, selected_part, value)
                == profile_cell(initial, nation, selected_part, value) + sum(links.entries().take(processed@), |row: (KeyView, Wide)|
                    if selected_part == part && row.0 == numeric_key(nation as int) { row.1@ * moment_measure(change@, value) } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (nation, multiplicity) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == links.entries().len()); assert(links.entries().take(processed@) =~= links.entries());
                    assert forall|nation: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*target, nation, selected_part, value)
                        == profile_cell(initial, nation, selected_part, value)
                            + if selected_part == part { link_count(*links, nation) * moment_measure(change@, value) } else { 0 } by {
                        let selected = |row: (KeyView, Wide)| if selected_part == part && row.0 == numeric_key(nation as int) { row.1@ * moment_measure(change@, value) } else { 0 };
                        if selected_part == part { lemma_link_finish(*links, nation, moment_measure(change@, value), selected); }
                        else { verus_common::sums::lemma_sum_zero(links.entries(), selected); }
                        assert(profile_cell(*target, nation, selected_part, value) == profile_cell(initial, nation, selected_part, value) + sum(links.entries(), selected));
                    }
                }
                return;
            },
        };
        proof { links.model(); assert(numeric(nation.view())); }
        let weighted = scale(change, &*multiplicity);
        let ghost previous = *target;
        add_profile(target, nation.number, part, &weighted, 8);
        proof {
            assert forall|query: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*target, query, selected_part, value)
                == profile_cell(initial, query, selected_part, value) + sum(links.entries().take(processed@ + 1), |row: (KeyView, Wide)|
                    if selected_part == part && row.0 == numeric_key(query as int) { row.1@ * moment_measure(change@, value) } else { 0 }) by {
                let weight = |row: (KeyView, Wide)| if selected_part == part && row.0 == numeric_key(query as int) { row.1@ * moment_measure(change@, value) } else { 0 };
                lemma_sum_take(links.entries(), processed@, weight);
                assert(links.entries()[processed@] == (nation.view(), *multiplicity));
                assert(profile_cell(previous, query, selected_part, value) == profile_cell(initial, query, selected_part, value) + sum(links.entries().take(processed@), weight));
                if query == nation.number && selected_part == part {
                    assert(moment_measure(weighted@, value) == (*multiplicity)@ * moment_measure(change@, value)) by (nonlinear_arith)
                        requires weighted.count@ == change.count@ * (*multiplicity)@, weighted.value@ == change.value@ * (*multiplicity)@;
                } else { assert(profile_cell(*target, query, selected_part, value) == profile_cell(previous, query, selected_part, value)); }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn supplier_group(target: &mut Profiles, groups: &Packed, links: &Index<Wide>)
    requires profile_layout(*old(target)), crate::construction::profile_keys_ok(*groups), links.wf(),
        forall|key: KeyView| (#[trigger] links.at(key)).is_some() ==> numeric(key),
    ensures profile_layout(*final(target)),
        forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*final(target), nation, part, value)
            == profile_cell(*old(target), nation, part, value)
                + moment_measure(moment_at(*groups, part as int), value) * link_count(*links, nation),
{
    let ghost initial = *target;
    let mut processed = Ghost(0int);
    let mut cursor = links.cursor();
    loop
        invariant profile_layout(*target), initial == *old(target), crate::construction::profile_keys_ok(*groups), links.wf(),
            forall|key: KeyView| (#[trigger] links.at(key)).is_some() ==> numeric(key),
            0 <= processed@ <= links.entries().len(), cursor.remaining() == links.entries().skip(processed@),
            forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*target, nation, part, value)
                == profile_cell(initial, nation, part, value) + sum(links.entries().take(processed@), |row: (KeyView, Wide)|
                    if row.0 == numeric_key(nation as int) { row.1@ * moment_measure(moment_at(*groups, part as int), value) } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (nation, multiplicity) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == links.entries().len()); assert(links.entries().take(processed@) =~= links.entries());
                    assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*target, nation, part, value)
                        == profile_cell(initial, nation, part, value) + moment_measure(moment_at(*groups, part as int), value) * link_count(*links, nation) by {
                        let selected = |row: (KeyView, Wide)| if row.0 == numeric_key(nation as int) { row.1@ * moment_measure(moment_at(*groups, part as int), value) } else { 0 };
                        let measure = moment_measure(moment_at(*groups, part as int), value);
                        lemma_link_finish(*links, nation, measure, selected);
                        assert(profile_cell(*target, nation, part, value) == profile_cell(initial, nation, part, value) + sum(links.entries(), selected));
                        assert(measure * link_count(*links, nation) == link_count(*links, nation) * measure) by (nonlinear_arith);
                    }
                }
                return;
            },
        };
        proof { links.model(); assert(numeric(nation.view())); }
        let ghost previous = *target;
        weighted_parts(target, nation.number, groups, &*multiplicity);
        proof {
            assert forall|query: i64, part: i64, value: bool| #[trigger] profile_cell(*target, query, part, value)
                == profile_cell(initial, query, part, value) + sum(links.entries().take(processed@ + 1), |row: (KeyView, Wide)|
                    if row.0 == numeric_key(query as int) { row.1@ * moment_measure(moment_at(*groups, part as int), value) } else { 0 }) by {
                let measure = moment_measure(moment_at(*groups, part as int), value);
                let weight = |row: (KeyView, Wide)| if row.0 == numeric_key(query as int) { row.1@ * measure } else { 0 };
                lemma_sum_take(links.entries(), processed@, weight);
                assert(links.entries()[processed@] == (nation.view(), *multiplicity));
                assert(profile_cell(previous, query, part, value) == profile_cell(initial, query, part, value) + sum(links.entries().take(processed@), weight));
                assert(measure * (*multiplicity)@ == (*multiplicity)@ * measure) by (nonlinear_arith);
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

}
