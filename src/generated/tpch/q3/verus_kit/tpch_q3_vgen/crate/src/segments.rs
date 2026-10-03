use vstd::prelude::*;
use std::sync::Arc;
use crate::maps::*;

verus! {

pub struct Name {
    pub id: i64,
    pub text: Arc<Vec<u8>>,
}

pub open spec fn text_hash(text: Seq<u8>) -> i64
    decreases text.len(),
{
    if text.len() == 0 { 0i64 }
    else {
        let value = text_hash(text.drop_last()) as int * 257 + text.last() as int + 1;
        ((value - i64::MIN as int) % 18446744073709551616int + i64::MIN as int) as i64
    }
}

pub open spec fn has_text(texts: Index<Vec<u8>>, text: Seq<u8>) -> bool {
    exists|segment: i64| (#[trigger] texts.at(segment)) matches Some(bytes) && bytes@ == text
}

pub open spec fn dictionary_ok(names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64) -> bool {
    names.valid() && texts.valid() && dictionary_model_ok(|key: i64| names.at(key), |key: i64| texts.at(key), next)
}

pub open spec fn dictionary_overlay_ok(names: Index<Vec<Name>>, names_delta: Run<Vec<Name>>, texts: Index<Vec<u8>>, texts_delta: Run<Vec<u8>>, next: i64) -> bool {
    dictionary_model_ok(|key: i64| overlay_at(names, names_delta, key), |key: i64| overlay_at(texts, texts_delta, key), next)
}

pub open spec fn dictionary_model_ok(names: spec_fn(i64) -> Option<Arc<Vec<Name>>>, texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>, next: i64) -> bool {
    &&& next >= 0
    &&& forall|segment: i64| (#[trigger] texts(segment)).is_some() <==> 0 <= segment < next
    &&& forall|first: i64, second: i64| first != second &&
        (#[trigger] texts(first)).is_some() && (#[trigger] texts(second)).is_some()
        ==> texts(first).unwrap()@ != texts(second).unwrap()@
    &&& forall|hash: i64| (#[trigger] names(hash)).is_some() ==>
        name_bucket_ok(names(hash).unwrap()@, texts, hash)
    &&& forall|segment: i64| (#[trigger] texts(segment)).is_some() ==>
        names(text_hash(texts(segment).unwrap()@)).is_some() &&
        exists|position: int| 0 <= position < names(text_hash(texts(segment).unwrap()@)).unwrap()@.len() &&
            (#[trigger] names(text_hash(texts(segment).unwrap()@)).unwrap()@[position]).id == segment
}

pub open spec fn name_bucket_ok(bucket: Seq<Name>, texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>, hash: i64) -> bool {
    &&& bucket.len() > 0
    &&& forall|position: int| 0 <= position < bucket.len() ==>
        texts((#[trigger] bucket[position]).id) == Some(bucket[position].text)
        && text_hash(bucket[position].text@) == hash
    &&& forall|first: int, second: int| 0 <= first < second < bucket.len() ==>
        (#[trigger] bucket[first]).id != (#[trigger] bucket[second]).id
}

pub proof fn lemma_dictionary_finished(base_names: Index<Vec<Name>>, name_delta: Run<Vec<Name>>, base_texts: Index<Vec<u8>>, text_delta: Run<Vec<u8>>, next: i64, names: Index<Vec<Name>>, texts: Index<Vec<u8>>)
    requires dictionary_overlay_ok(base_names, name_delta, base_texts, text_delta, next), names.valid(), texts.valid(),
        forall|key: i64| #[trigger] names.at(key) == overlay_at(base_names, name_delta, key),
        forall|key: i64| #[trigger] texts.at(key) == overlay_at(base_texts, text_delta, key),
    ensures dictionary_ok(names, texts, next),
{
    let old_names = |key: i64| overlay_at(base_names, name_delta, key);
    let old_texts = |key: i64| overlay_at(base_texts, text_delta, key);
    let new_names = |key: i64| names.at(key);
    let new_texts = |key: i64| texts.at(key);
    assert(old_names =~= new_names);
    assert(old_texts =~= new_texts);
    assert(dictionary_model_ok(new_names, new_texts, next));
}

pub proof fn lemma_dictionary_overlay_empty(names: Index<Vec<Name>>, names_delta: Run<Vec<Name>>, texts: Index<Vec<u8>>, texts_delta: Run<Vec<u8>>, next: i64)
    requires dictionary_ok(names, texts, next),
        names_delta.model() == Map::<i64, Option<Arc<Vec<Name>>>>::empty(),
        texts_delta.model() == Map::<i64, Option<Arc<Vec<u8>>>>::empty(),
    ensures dictionary_overlay_ok(names, names_delta, texts, texts_delta, next),
{
    assert((|key: i64| overlay_at(names, names_delta, key)) =~= (|key: i64| names.at(key)));
    assert((|key: i64| overlay_at(texts, texts_delta, key)) =~= (|key: i64| texts.at(key)));
}

pub open spec fn name_match(names: Option<Arc<Vec<Name>>>, text: Seq<u8>, id: i64) -> bool {
    names.is_some() && exists|position: int| 0 <= position < names.unwrap()@.len() &&
        (#[trigger] names.unwrap()@[position]).id == id && names.unwrap()@[position].text@ == text
}

pub proof fn lemma_name_match(bucket: Arc<Vec<Name>>, text: Seq<u8>, position: int)
    requires 0 <= position < bucket@.len(), bucket@[position].text@ == text,
    ensures name_match(Some(bucket), text, bucket@[position].id),
{
    assert(Some(bucket).unwrap() == bucket);
    assert(name_match(Some(bucket), text, bucket@[position].id) ==
        (exists|entry: int| 0 <= entry < bucket@.len() &&
            (#[trigger] bucket@[entry]).id == bucket@[position].id && bucket@[entry].text@ == text));
    assert(exists|entry: int| 0 <= entry < bucket@.len() &&
        (#[trigger] bucket@[entry]).id == bucket@[position].id && bucket@[entry].text@ == text);
}

pub proof fn lemma_dictionary_bucket(names: Index<Vec<Name>>, names_delta: Run<Vec<Name>>, texts: Index<Vec<u8>>, texts_delta: Run<Vec<u8>>, next: i64, hash: i64)
    requires dictionary_overlay_ok(names, names_delta, texts, texts_delta, next),
        overlay_at(names, names_delta, hash).is_some(),
    ensures name_bucket_ok(overlay_at(names, names_delta, hash).unwrap()@,
        |id: i64| overlay_at(texts, texts_delta, id), hash),
{
    let name_lookup = |key: i64| overlay_at(names, names_delta, key);
    let text_lookup = |key: i64| overlay_at(texts, texts_delta, key);
    assert(dictionary_model_ok(name_lookup, text_lookup, next));
    assert(name_lookup(hash).is_some());
}

pub proof fn lemma_dictionary_next(names: Index<Vec<Name>>, names_delta: Run<Vec<Name>>, texts: Index<Vec<u8>>, texts_delta: Run<Vec<u8>>, next: i64)
    requires dictionary_overlay_ok(names, names_delta, texts, texts_delta, next),
    ensures overlay_at(texts, texts_delta, next).is_none(),
{
    let name_lookup = |key: i64| overlay_at(names, names_delta, key);
    let text_lookup = |key: i64| overlay_at(texts, texts_delta, key);
    assert(dictionary_model_ok(name_lookup, text_lookup, next));
    assert(!text_lookup(next).is_some());
}

pub proof fn lemma_dictionary_unique(names: Index<Vec<Name>>, names_delta: Run<Vec<Name>>, texts: Index<Vec<u8>>, texts_delta: Run<Vec<u8>>, next: i64)
    requires dictionary_overlay_ok(names, names_delta, texts, texts_delta, next),
    ensures forall|first: i64, second: i64| first != second &&
        (#[trigger] overlay_at(texts, texts_delta, first)).is_some() &&
        (#[trigger] overlay_at(texts, texts_delta, second)).is_some() ==>
        overlay_at(texts, texts_delta, first).unwrap()@ != overlay_at(texts, texts_delta, second).unwrap()@,
{
    let name_lookup = |key: i64| overlay_at(names, names_delta, key);
    let text_lookup = |key: i64| overlay_at(texts, texts_delta, key);
    assert(dictionary_model_ok(name_lookup, text_lookup, next));
    assert forall|first: i64, second: i64| first != second &&
        (#[trigger] overlay_at(texts, texts_delta, first)).is_some() &&
        (#[trigger] overlay_at(texts, texts_delta, second)).is_some() implies
        overlay_at(texts, texts_delta, first).unwrap()@ != overlay_at(texts, texts_delta, second).unwrap()@ by {
        assert(text_lookup(first).is_some());
        assert(text_lookup(second).is_some());
        assert(text_lookup(first).unwrap()@ != text_lookup(second).unwrap()@);
    }
}

pub proof fn lemma_dictionary_text_match(names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64, text: Seq<u8>, segment: i64)
    requires dictionary_ok(names, texts, next), texts.at(segment).is_some(), texts.at(segment).unwrap()@ == text,
    ensures name_match(names.at(text_hash(text)), text, segment),
{
    let name_lookup = |key: i64| names.at(key);
    let text_lookup = |key: i64| texts.at(key);
    assert(dictionary_model_ok(name_lookup, text_lookup, next));
    assert(text_lookup(segment).is_some());
    let bucket = name_lookup(text_hash(text)).unwrap();
    let position = choose|position: int| 0 <= position < bucket@.len() && (#[trigger] bucket@[position]).id == segment;
    assert(name_bucket_ok(bucket@, text_lookup, text_hash(text)));
    assert(text_lookup(bucket@[position].id) == Some(bucket@[position].text));
    lemma_name_match(bucket, text, position);
}

pub proof fn lemma_dictionary_named_text(names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64, text: Seq<u8>, segment: i64)
    requires dictionary_ok(names, texts, next), name_match(names.at(text_hash(text)), text, segment),
    ensures texts.at(segment).is_some(), texts.at(segment).unwrap()@ == text,
{
    let name_lookup = |key: i64| names.at(key);
    let text_lookup = |key: i64| texts.at(key);
    assert(dictionary_model_ok(name_lookup, text_lookup, next));
    assert(name_lookup(text_hash(text)).is_some());
    let bucket = name_lookup(text_hash(text)).unwrap();
    let position = choose|position: int| 0 <= position < bucket@.len() && (#[trigger] bucket@[position]).id == segment && bucket@[position].text@ == text;
    assert(name_bucket_ok(bucket@, text_lookup, text_hash(text)));
    assert(text_lookup(segment) == Some(bucket@[position].text));
}

pub proof fn lemma_dictionary_extend(
    names: spec_fn(i64) -> Option<Arc<Vec<Name>>>,
    texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>, next: i64,
    shared: Arc<Vec<u8>>, bucket: Arc<Vec<Name>>,
)
    requires
        dictionary_model_ok(names, texts, next), next < i64::MAX,
        bucket@ == (match names(text_hash(shared@)) { Some(old) => old@, None => Seq::empty() })
            .push(Name { id: next, text: shared }),
        forall|position: int| 0 <= position < bucket.len() - 1 ==>
            (#[trigger] bucket@[position]).text@ != shared@,
    ensures dictionary_model_ok(
        |hash: i64| if hash == text_hash(shared@) { Some(bucket) } else { names(hash) },
        |id: i64| if id == next { Some(shared) } else { texts(id) },
        (next + 1) as i64),
{
    let hash = text_hash(shared@);
    let new_names = |key: i64| if key == hash { Some(bucket) } else { names(key) };
    let new_texts = |id: i64| if id == next { Some(shared) } else { texts(id) };
    assert forall|id: i64| (#[trigger] texts(id)).is_some() implies texts(id).unwrap()@ != shared@ by {
        let bytes = texts(id).unwrap();
        if bytes@ == shared@ {
            let old = names(hash).unwrap();
            let position = choose|position: int| 0 <= position < old.len() && (#[trigger] old@[position]).id == id;
            assert(texts(old@[position].id) == Some(old@[position].text));
            assert(bucket@[position] == old@[position]);
        }
    }
    assert forall|key: i64| (#[trigger] new_names(key)).is_some() implies
        name_bucket_ok(new_names(key).unwrap()@, new_texts, key) by {
        let entries = new_names(key).unwrap();
        if key == hash {
            assert forall|position: int| 0 <= position < bucket.len() implies
                new_texts((#[trigger] bucket@[position]).id) == Some(bucket@[position].text)
                && text_hash(bucket@[position].text@) == key by {
                if position < bucket.len() - 1 {
                    let old = names(hash).unwrap();
                    assert(bucket@[position] == old@[position]);
                    assert(0 <= old@[position].id < next);
                }
            }
            assert forall|first: int, second: int| 0 <= first < second < bucket.len() implies
                (#[trigger] bucket@[first]).id != (#[trigger] bucket@[second]).id by {
                let old = names(hash).unwrap();
                assert(bucket@[first] == old@[first]);
                if second < bucket.len() - 1 { assert(bucket@[second] == old@[second]); }
                else { assert(0 <= bucket@[first].id < next); }
            }
        } else {
            assert forall|position: int| 0 <= position < entries.len() implies
                new_texts((#[trigger] entries@[position]).id) == Some(entries@[position].text)
                && text_hash(entries@[position].text@) == key by {
                assert(0 <= entries@[position].id < next);
            }
        }
    }
    assert forall|id: i64| (#[trigger] new_texts(id)).is_some() implies
        new_names(text_hash(new_texts(id).unwrap()@)).is_some() &&
        exists|position: int| 0 <= position < new_names(text_hash(new_texts(id).unwrap()@)).unwrap().len() &&
            (#[trigger] new_names(text_hash(new_texts(id).unwrap()@)).unwrap()@[position]).id == id by {
        let bytes = new_texts(id).unwrap();
        if id == next {
            assert(bucket@[bucket.len() as int - 1].id == id);
        } else {
            let old = names(text_hash(bytes@)).unwrap();
            let position = choose|position: int| 0 <= position < old.len() && (#[trigger] old@[position]).id == id;
            if text_hash(bytes@) == hash { assert(bucket@[position] == old@[position]); }
            else { assert(new_names(text_hash(bytes@)).unwrap()@[position].id == id); }
        }
    }
}

pub fn equal(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut position = 0;
    while position < left.len()
        invariant position <= left.len(), left.len() == right.len(),
            left@.take(position as int) == right@.take(position as int),
        decreases left.len() - position,
    {
        if left[position] != right[position] { return false; }
        proof {
            assert(left@.take(position as int + 1) =~= left@.take(position as int).push(left@[position as int]));
            assert(right@.take(position as int + 1) =~= right@.take(position as int).push(right@[position as int]));
        }
        position += 1;
    }
    proof { assert(left@.take(left@.len() as int) =~= left@); assert(right@.take(right@.len() as int) =~= right@); }
    true
}

pub fn fingerprint(text: &[u8]) -> (result: i64)
    ensures result == text_hash(text@),
{
    let mut hash = 0i64;
    let mut position = 0;
    while position < text.len()
        invariant position <= text.len(), hash == text_hash(text@.take(position as int)),
        decreases text.len() - position,
    {
        let ghost prior = hash as int;
        let multiplied = hash.wrapping_mul(257);
        hash = multiplied.wrapping_add(text[position] as i64 + 1);
        proof {
            let modulus = 18446744073709551616int;
            let minimum = i64::MIN as int;
            let byte = text@[position as int] as int + 1;
            assert(multiplied as int == (prior * 257 - minimum) % modulus + minimum);
            assert(hash as int == (multiplied as int + byte - minimum) % modulus + minimum);
            assert(((prior * 257 - minimum) % modulus + byte) % modulus == (prior * 257 + byte - minimum) % modulus);
            let prefix = text@.take(position as int + 1);
            assert(prefix.drop_last() =~= text@.take(position as int));
            assert(prefix.last() == text@[position as int]);
        }
        position += 1;
    }
    proof { assert(text@.take(text.len() as int) =~= text@); }
    hash
}

pub fn find(names: &Index<Vec<Name>>, text: &[u8]) -> (result: Option<i64>)
    ensures
        result matches Some(id) ==> name_match(names.at(text_hash(text@)), text@, id),
        result.is_none() ==> forall|id: i64| !#[trigger] name_match(names.at(text_hash(text@)), text@, id),
{
    if let Some(bucket) = names.get(fingerprint(text)) {
        let mut position = 0;
        while position < bucket.len()
            invariant position <= bucket.len(), names.at(text_hash(text@)) == Some(bucket),
                forall|prior: int| 0 <= prior < position ==> (#[trigger] bucket@[prior]).text@ != text@,
            decreases bucket.len() - position,
        {
            if equal(text, bucket[position].text.as_slice()) {
                proof {
                    reveal(name_match);
                    assert(bucket@[position as int].text@ == text@);
                    assert(exists|entry: int| 0 <= entry < bucket.len() &&
                        (#[trigger] bucket@[entry]).id == bucket@[position as int].id && bucket@[entry].text@ == text@);
                    lemma_name_match(bucket, text@, position as int);
                }
                return Some(bucket[position].id);
            }
            position += 1;
        }
    }
    None
}

pub fn intern(base: &Index<Vec<Name>>, base_texts: &Index<Vec<u8>>, delta: &mut Run<Vec<Name>>, texts: &mut Run<Vec<u8>>, next: &mut i64, text: &[u8]) -> (result: Option<i64>)
    requires run_ok(*old(delta)), run_ok(*old(texts)),
        dictionary_overlay_ok(*base, *old(delta), *base_texts, *old(texts), *old(next)),
    ensures
        run_ok(*final(delta)), run_ok(*final(texts)),
        result matches Some(id) ==> dictionary_overlay_ok(*base, *final(delta), *base_texts, *final(texts), *final(next))
            && overlay_at(*base_texts, *final(texts), id).is_some()
            && overlay_at(*base_texts, *final(texts), id).unwrap()@ == text@,
        result.is_some() ==> forall|id: i64| (#[trigger] overlay_at(*base_texts, *old(texts), id)).is_some() ==>
            overlay_at(*base_texts, *final(texts), id) == overlay_at(*base_texts, *old(texts), id),
        result.is_some() ==> forall|id: i64| (#[trigger] overlay_at(*base_texts, *final(texts), id)).is_some()
            && overlay_at(*base_texts, *old(texts), id).is_none() ==> overlay_at(*base_texts, *final(texts), id).unwrap()@ == text@,
{
    let ghost original_names = *delta;
    let ghost original_texts = *texts;
    let ghost original_next = *next;
    let hash = fingerprint(text);
    let existing = current(base, delta, hash);
    let mut names = Vec::new();
    if let Some(bucket) = existing {
        let mut position = 0;
        while position < bucket.len()
            invariant position <= bucket.len(),
                run_ok(*delta), run_ok(*texts),
                *delta == original_names, *texts == original_texts, *next == original_next,
                original_names == *old(delta), original_texts == *old(texts), original_next == *old(next),
                dictionary_overlay_ok(*base, original_names, *base_texts, original_texts, original_next),
                overlay_at(*base, original_names, hash) == Some(bucket), hash == text_hash(text@),
                names@ == bucket@.take(position as int),
                forall|prior: int| 0 <= prior < position ==> (#[trigger] bucket@[prior]).text@ != text@,
            decreases bucket.len() - position,
        {
            if equal(text, bucket[position].text.as_slice()) {
                proof {
                    reveal(dictionary_overlay_ok);
                    reveal(dictionary_model_ok);
                    reveal(name_bucket_ok);
                    lemma_dictionary_bucket(*base, original_names, *base_texts, original_texts, original_next, hash);
                    assert(overlay_at(*base_texts, *texts, bucket@[position as int].id) == Some(bucket@[position as int].text));
                }
                return Some(bucket[position].id);
            }
            names.push(Name { id: bucket[position].id, text: Arc::clone(&bucket[position].text) });
            position += 1;
        }
        proof { assert(bucket@.take(position as int) =~= bucket@); }
    }
    let id = *next;
    *next = match id.checked_add(1) { Some(value) => value, None => return None };
    let mut bytes = Vec::new();
    let ghost previous_names = names@;
    let mut position = 0;
    while position < text.len()
        invariant position <= text.len(), bytes@ == text@.take(position as int),
            run_ok(*delta), run_ok(*texts),
            *delta == original_names, *texts == original_texts,
            original_names == *old(delta), original_texts == *old(texts), original_next == *old(next),
            dictionary_overlay_ok(*base, original_names, *base_texts, original_texts, original_next),
            *next == id + 1, id == original_next, id < i64::MAX, hash == text_hash(text@),
            names@ == previous_names,
            previous_names == match overlay_at(*base, original_names, hash) { Some(bucket) => bucket@, None => Seq::empty() },
            forall|prior: int| 0 <= prior < previous_names.len() ==> (#[trigger] previous_names[prior]).text@ != text@,
        decreases text.len() - position,
    {
        bytes.push(text[position]);
        position += 1;
    }
    proof { assert(text@.take(position as int) =~= text@); }
    let shared = Arc::new(bytes);
    texts.put(id, Some(Arc::clone(&shared)));
    names.push(Name { id, text: shared });
    let bucket = Arc::new(names);
    proof {
        lemma_dictionary_extend(
            |key: i64| overlay_at(*base, original_names, key),
            |key: i64| overlay_at(*base_texts, original_texts, key), id, shared, bucket);
    }
    delta.put(hash, Some(bucket));
    proof {
        let original_name_lookup = |key: i64| overlay_at(*base, original_names, key);
        let original_text_lookup = |key: i64| overlay_at(*base_texts, original_texts, key);
        let new_name_lookup = |key: i64| if key == hash { Some(bucket) } else { original_name_lookup(key) };
        let new_text_lookup = |key: i64| if key == id { Some(shared) } else { original_text_lookup(key) };
        assert((|key: i64| overlay_at(*base, *delta, key)) =~= new_name_lookup);
        assert((|key: i64| overlay_at(*base_texts, *texts, key)) =~= new_text_lookup);
        lemma_dictionary_next(*base, original_names, *base_texts, original_texts, id);
        assert(dictionary_overlay_ok(*base, *delta, *base_texts, *texts, *next));
    }
    Some(id)
}

}
