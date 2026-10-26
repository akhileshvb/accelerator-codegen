// Permutation invariance of the generated spec.
//
// A SQL table is a bag, but the spec holds it as a `Seq`.  For the spec to
// mean "SQL over bags" it must not depend on the order of any table's rows.
// This module emits, next to the spec, a proof that it does not:
//
//   lemma_query_perm(db, db2, params, out)
//     requires db_perm(db, db2)       (each table of db2 is a reordering of db's)
//     ensures  query_result(db, params, out) <==> query_result(db2, params, out)
//
// The proof is a chain of one lemma per generated definition F that mentions
// the database: `F(db, ..) == F(db2, ..)` for a scalar / bool / row, or
// `bag_eq(F(db, ..), F(db2, ..))` for a sequence (a bag).  A lemma calls the
// lemmas of the definitions it mentions; the generic steps (map, filter-map,
// join = flattened map, dedup, sums) are `verus_common::perm` and
// `verus_common::sums`.  The lemmas are Facts: checked by Verus, not trusted.
//
// A construct without a lemma here (e.g. a join of several indexes inside
// EXISTS) makes `skip` non-empty: the spec is then emitted without the
// section, with a comment saying why.
use crate::{as_bag, bool_code, camel, cut, key_cols, key_tuple_args, key_tuple_ty, key_tuple_val, key_ty, row_expr, table_field, Block, Shape, Top};
use std::fmt::Write;

const HIDE: &str = "    /*HIDE*/\n";

#[derive(Default, Clone)]
pub struct Perm {
    /// (lemma text with a HIDE marker, the definition it is about, definitions it must keep unfolded)
    pub lemmas: Vec<(String, String, Vec<String>)>,
    /// every generated definition that has a lemma (hidden inside the other lemmas)
    pub fns: Vec<String>,
    /// definitions the next lemma must see through
    pub keep: Vec<String>,
    /// (function name, lemma proving its invariance)
    pub reg: Vec<(String, String)>,
    pub skip: Option<String>,
}

pub type Args = Vec<(String, String)>;

/// Replace the identifier `db` by `db2` (not field accesses `.db`).
pub fn to_db2(s: &str) -> String {
    swap_ident(s, &[("db", "db2")])
}

/// Exchange `db` and `db2`.
pub fn swap(s: &str) -> String {
    swap_ident(s, &[("db", "db2"), ("db2", "db")])
}

fn swap_ident(s: &str, map: &[(&str, &str)]) -> String {
    let cs: Vec<char> = s.chars().collect();
    let mut o = String::new();
    let mut i = 0;
    while i < cs.len() {
        let c = cs[i];
        if c.is_alphanumeric() || c == '_' {
            let st = i;
            while i < cs.len() && (cs[i].is_alphanumeric() || cs[i] == '_') {
                i += 1;
            }
            let w: String = cs[st..i].iter().collect();
            let field = st > 0 && cs[st - 1] == '.';
            match map.iter().find(|(a, _)| *a == w) {
                Some((_, b)) if !field => o.push_str(b),
                _ => o.push_str(&w),
            }
        } else {
            o.push(c);
            i += 1;
        }
    }
    o
}

/// `name` occurs in `text` as a whole identifier.
pub fn mentions(text: &str, name: &str) -> bool {
    let mut from = 0;
    while let Some(k) = text[from..].find(name) {
        let st = from + k;
        let en = st + name.len();
        let before_ok = st == 0 || !{
            let c = text[..st].chars().last().unwrap();
            c.is_alphanumeric() || c == '_' || c == '.'
        };
        let after_ok = name.ends_with('(') || en >= text.len() || !{
            let c = text[en..].chars().next().unwrap();
            c.is_alphanumeric() || c == '_'
        };
        if before_ok && after_ok {
            return true;
        }
        from = st + 1;
    }
    false
}

pub fn decl(a: &Args) -> String {
    a.iter().map(|(n, t)| format!("{n}: {t}")).collect::<Vec<_>>().join(", ")
}

pub fn vals(a: &Args) -> String {
    a.iter().map(|(n, _)| format!(", {n}")).collect()
}

#[derive(Clone, Copy, PartialEq)]
pub enum Kind {
    /// F(db, ..) == F(db2, ..)
    Eq,
    /// bag_eq(F(db, ..), F(db2, ..))
    Bag,
    /// F(db, ..) <==> F(db2, ..), proved by the body
    Iff,
}

impl Perm {
    /// The lemma texts, each hiding every other generated definition: a lemma
    /// unfolds only its own definition and gets the rest from the lemmas it calls.
    pub fn texts(&self) -> String {
        let mut o = String::new();
        for (text, me, keep) in &self.lemmas {
            let hides: String = self.fns.iter().filter(|f| *f != me && !keep.contains(f)).map(|f| format!("    hide({f});\n")).collect();
            o.push_str(&text.replace(HIDE, &hides));
        }
        o
    }

    pub fn unsupported(&mut self, why: &str) {
        if self.skip.is_none() {
            self.skip = Some(why.to_string());
        }
    }

    /// The lemma calls for every registered definition mentioned in `text`.
    fn calls(&self, text: &str) -> String {
        let mut seen = std::collections::BTreeSet::new();
        let mut s = String::new();
        for (f, l) in &self.reg {
            if mentions(text, f) && seen.insert(l.clone()) {
                writeln!(s, "    {l}(db, db2, params);").unwrap();
            }
        }
        s
    }

    /// Emit the lemma for the definition `call` (its call text with `db`,
    /// e.g. "where_clause(db, params, a, b)"), universally quantified over
    /// `args`.  `deps` is the text of the definition (to find the lemmas it
    /// needs); `body` proves the claim for the quantified arguments.
    pub fn lemma(&mut self, fname: &str, generics: &str, args: &Args, kind: Kind, call: &str, deps: &str, body: &str) {
        let lname = format!("lemma_perm_{fname}");
        let lhs = call.to_string();
        let rhs = to_db2(call);
        let claim = |trig: bool| -> String {
            let t = if trig { "#[trigger] " } else { "" };
            match kind {
                Kind::Eq => format!("{t}{lhs} == {rhs}"),
                Kind::Bag => format!("{t}bag_eq({lhs}, {rhs})"),
                Kind::Iff => format!("{t}{lhs} == {rhs}"),
            }
        };
        let calls = self.calls(deps);
        let mut w = String::new();
        writeln!(w, "proof fn {lname}{generics}(db: Db, db2: Db, params: Params)\n    requires\n        db_perm(db, db2),\n    ensures").unwrap();
        if args.is_empty() {
            writeln!(w, "        {},\n{{\n{HIDE}{calls}{body}}}\n", claim(false)).unwrap();
        } else {
            writeln!(w, "        forall|{}| {},\n{{\n{HIDE}", decl(args), claim(true)).unwrap();
            w.push_str(&calls);
            writeln!(w, "    assert forall|{}| {} by {{\n{body}    }}\n}}\n", decl(args), claim(true)).unwrap();
        }
        let keep = std::mem::take(&mut self.keep);
        self.lemmas.push((w, fname.to_string(), keep));
        self.fns.push(fname.to_string());
        self.reg.push((fname.to_string(), lname));
    }
}

impl Perm {
    /// The lemmas for one translated query block (everything `emit_block` defines).
    pub fn block(&mut self, b: &Block, where_: &[String], top: bool) {
        if b.union_rows.is_some() {
            self.unsupported("UNION");
            return;
        }
        let p = b.prefix.clone();
        let tt = b.tuple.clone();
        let oa = b.outer_args();
        let outer: Args = b.outer.iter().map(|x| (x.var.clone(), x.vty())).collect();
        let n = b.binders.len();
        let row = if top { "OutRow".to_string() } else { format!("{}Row", camel(p.trim_end_matches('_'))) };
        let bargs: Args = b.binders.iter().map(|x| (x.var.clone(), x.vty())).collect();
        let cat = |a: &Args, c: &Args| -> Args { a.iter().chain(c.iter()).cloned().collect() };
        // texts the definitions are built from (to find the lemmas they need)
        let rtext: String = b.outs.iter().map(row_expr).collect::<Vec<_>>().join(" ");
        let htext: String = b.having.iter().map(bool_code).collect::<Vec<_>>().join(" ");

        // WHERE
        let wtext = where_.join(" ");
        self.lemma(&format!("{p}where_clause"), "", &cat(&outer, &bargs), Kind::Eq, &format!("{p}where_clause(db, params{oa}{})", vals(&bargs)), &wtext, "");
        // LEFT JOIN ON conditions
        for (i, bd) in b.binders.iter().enumerate() {
            if bd.optional {
                let prev: Args = bargs[..i].to_vec();
                let mut a = cat(&outer, &prev);
                a.push((bd.var.clone(), bd.row()));
                self.lemma(&format!("{p}on_{}", bd.var), "", &a, Kind::Eq, &format!("{p}on_{}(db, params{oa}{}, {})", bd.var, vals(&prev), bd.var), &bd.on.join(" "), "");
            }
        }
        // the join nest, innermost first
        for i in (0..=n).rev() {
            let upto: Args = bargs[..i].to_vec();
            let (fname, call) = if i == 0 {
                (format!("{p}tuples"), format!("{p}tuples(db, params{oa})"))
            } else {
                (format!("{p}tuples_{i}"), format!("{p}tuples_{i}(db, params{oa}{})", vals(&upto)))
            };
            let a = cat(&outer, &upto);
            if i == n {
                let body = format!("        assert({call} == {});\n", to_db2(&call));
                self.lemma(&fname, "", &a, Kind::Bag, &call, &format!("{p}where_clause("), &body);
                continue;
            }
            let bd = &b.binders[i];
            let v = &bd.var;
            let r = bd.row();
            let t1 = bd.seq();
            let t2 = to_db2(&t1);
            let up1: String = bargs[..=i].iter().map(|(x, _)| format!(", {x}")).collect();
            let next1 = format!("{p}tuples_{}(db, params{oa}{up1})", i + 1);
            let next2 = to_db2(&next1);
            let mut body = String::new();
            let deps;
            if !bd.optional {
                deps = format!("{next1} {t1}");
                writeln!(body, "        let g1 = |{v}: {r}| {next1};\n        let g2 = |{v}: {r}| {next2};").unwrap();
                writeln!(body, "        assert forall|a: {r}| #[trigger] bag_eq(g1(a), g2(a)) by {{}}").unwrap();
                writeln!(body, "        lemma_bag_eq_flat_map({t1}, {t2}, g1, g2);").unwrap();
            } else {
                let prev: String = bargs[..i].iter().map(|(x, _)| format!(", {x}")).collect();
                let on1 = format!("{p}on_{v}(db, params{oa}{prev}, {v})");
                let on2 = to_db2(&on1);
                deps = format!("{next1} {t1} {on1}");
                writeln!(body, "        let o1 = |{v}: {r}| if {on1} {{ seq![Some({v})] }} else {{ Seq::empty() }};").unwrap();
                writeln!(body, "        let o2 = |{v}: {r}| if {on2} {{ seq![Some({v})] }} else {{ Seq::empty() }};").unwrap();
                writeln!(body, "        assert forall|a: {r}| #[trigger] bag_eq(o1(a), o2(a)) by {{ assert(o1(a) == o2(a)); }}").unwrap();
                writeln!(body, "        lemma_bag_eq_flat_map({t1}, {t2}, o1, o2);").unwrap();
                writeln!(body, "        let m1 = flat({t1}.map_values(o1));\n        let m2 = flat({t2}.map_values(o2));").unwrap();
                writeln!(body, "        lemma_bag_eq_len(m1, m2);").unwrap();
                writeln!(body, "        let w1 = if m1.len() > 0 {{ m1 }} else {{ seq![None::<{r}>] }};\n        let w2 = if m2.len() > 0 {{ m2 }} else {{ seq![None::<{r}>] }};").unwrap();
                writeln!(body, "        assert(bag_eq(w1, w2));").unwrap();
                writeln!(body, "        let g1 = |{v}: Option<{r}>| {next1};\n        let g2 = |{v}: Option<{r}>| {next2};").unwrap();
                writeln!(body, "        assert forall|a: Option<{r}>| #[trigger] bag_eq(g1(a), g2(a)) by {{}}").unwrap();
                writeln!(body, "        lemma_bag_eq_flat_map(w1, w2, g1, g2);").unwrap();
            }
            self.lemma(&fname, "", &a, Kind::Bag, &call, &deps, &body);
        }
        let keys = &b.keys;
        let kargs: Args = keys.iter().map(|(g, v)| (g.clone(), key_ty(v))).collect();
        let kv = vals(&kargs);
        let cond = keys.iter().map(|(g, v)| format!("{} == {g}", v.code)).collect::<Vec<_>>().join(" && ");
        let t1 = format!("{p}tuples(db, params{oa})");
        let t2 = to_db2(&t1);
        if b.shape != Shape::Plain {
            let ak = cat(&outer, &kargs);
            let mut af = ak.clone();
            af.push(("f".into(), format!("spec_fn({tt}) -> int")));
            let sum_clo = |inner: &str, other: &str| format!("|t: {tt}| if {cond} {{ {inner} }} else {{ {other} }}");
            // the closure may mention db (a GROUP BY expression key): sum over db2 uses its own copy
            let sum_body = |clo: &str| -> String {
                format!(
                    "        assert(bag_eq({t1}, {t2}));\n        let c1 = {clo};\n        let c2 = {};\n        assert forall|a: {tt}| #[trigger] c1(a) == c2(a) by {{}}\n        lemma_sum_perm({t1}, {t2}, c1);\n        lemma_sum_ext_all({t2}, c1, c2);\n",
                    to_db2(clo)
                )
            };
            // Σ
            let (call, body) = (format!("{p}group_sum(db, params{oa}{kv}, f)"), if cond.is_empty() {
                format!("        assert(bag_eq({t1}, {t2}));\n        lemma_sum_perm({t1}, {t2}, f);\n")
            } else {
                sum_body(&sum_clo("f(t)", "0"))
            });
            self.lemma(&format!("{p}group_sum"), "", &af, Kind::Eq, &call, &format!("{p}tuples( {cond}"), &body);
            // COUNT(*)
            let call = format!("{p}group_count(db, params{oa}{kv})");
            let body = if cond.is_empty() {
                format!("        assert(bag_eq({t1}, {t2}));\n        lemma_bag_eq_len({t1}, {t2});\n")
            } else {
                sum_body(&sum_clo("1int", "0"))
            };
            self.lemma(&format!("{p}group_count"), "", &ak, Kind::Eq, &call, &format!("{p}tuples( {cond}"), &body);
            // MIN / MAX / COUNT(DISTINCT): over the group's values
            let vals_bag = if cond.is_empty() {
                format!("        assert(bag_eq({t1}, {t2}));\n        lemma_bag_eq_map({t1}, {t2}, f, f);\n")
            } else {
                let keep = format!("|t: {tt}| {cond}");
                format!(
                    "        assert(bag_eq({t1}, {t2}));\n        let k1 = {keep};\n        let k2 = {};\n        assert forall|a: {tt}| #[trigger] k1(a) == k2(a) by {{}}\n        lemma_bag_eq_map_where({t1}, {t2}, k1, k2, f, f);\n",
                    to_db2(&keep)
                )
            };
            // the same over the tuples where `keep` holds (the non-NULL ones)
            let vals_where_bag = {
                let keep = if cond.is_empty() { "keep".to_string() } else { format!("|t: {tt}| {cond} && keep(t)") };
                format!(
                    "        assert(bag_eq({t1}, {t2}));\n        let k1 = {keep};\n        let k2 = {};\n        assert forall|a: {tt}| #[trigger] k1(a) == k2(a) by {{}}\n        lemma_bag_eq_map_where({t1}, {t2}, k1, k2, f, f);\n",
                    to_db2(&keep)
                )
            };
            let all = format!("{rtext} {htext}");
            for (gvf, bag, extra) in [("group_vals", &vals_bag, ""), ("group_vals_where", &vals_where_bag, ", keep")] {
                let mut af = ak.clone();
                if gvf == "group_vals_where" {
                    af.push(("keep".into(), format!("spec_fn({tt}) -> bool")));
                }
                af.push(("f".into(), format!("spec_fn({tt}) -> int")));
                let gv = format!("{p}{gvf}(db, params{oa}{kv}{extra}, f)");
                let gv2 = to_db2(&gv);
                for (fun, lem) in [("min_of", "lemma_min_of_perm"), ("max_of", "lemma_max_of_perm")] {
                    if mentions(&all, &format!("{fun}({p}{gvf}(")) {
                        let call = format!("{fun}({gv})");
                        let body = format!("{bag}        {lem}({gv}, {gv2});\n");
                        self.keep = vec![format!("{p}{gvf}")];
                        self.lemma(&format!("{p}{}_{fun}", gvf.replace("group_vals", "group")), "", &af, Kind::Eq, &call, &format!("{p}tuples( {cond}"), &body);
                        self.fns.pop();
                        let last = self.reg.pop().unwrap();
                        self.reg.push((format!("{fun}({p}{gvf}("), last.1));
                    }
                }
                if mentions(&all, "min_text") || mentions(&all, "max_text") {
                    self.unsupported("MIN / MAX over text");
                }
                if mentions(&all, &format!("distinct_count({p}{gvf}(")) {
                    for (suffix, ty) in [("int", "int"), ("text", "Seq<u8>")] {
                        let mut ad = af.clone();
                        *ad.last_mut().unwrap() = ("f".into(), format!("spec_fn({tt}) -> {ty}"));
                        let call = format!("distinct_count({gv})");
                        let body = format!("{bag}        lemma_distinct_count_perm({gv}, {gv2});\n");
                        self.keep = vec![format!("{p}{gvf}")];
                        self.lemma(&format!("{p}{}_distinct_{suffix}", gvf.replace("group_vals", "group")), "", &ad, Kind::Eq, &call, &format!("{p}tuples( {cond}"), &body);
                        self.fns.pop();
                        let last = self.reg.pop().unwrap();
                        self.reg.push((format!("distinct_count({p}{gvf}("), last.1));
                    }
                }
            }
            // output row
            self.lemma(&format!("{p}row_of"), "", &ak, Kind::Eq, &format!("{p}row_of(db, params{oa}{kv})"), &format!("{rtext} {htext}"), "");
        }
        if b.shape == Shape::Grouped || !b.having.is_empty() {
            let ak = cat(&outer, &kargs);
            self.lemma(&format!("{p}having"), "", &ak, Kind::Eq, &format!("{p}having(db, params{oa}{kv})"), &htext, "");
        }
        // the rows
        let call = format!("{p}rows(db, params{oa})");
        let mut body = String::new();
        let deps;
        match b.shape {
            Shape::Plain => {
                let fs: Vec<String> = b.outs.iter().map(|c| format!("{}: {}", c.field, row_expr(c))).collect();
                let f1 = format!("|t: {tt}| {row} {{ {} }}", fs.join(", "));
                let f2 = to_db2(&f1);
                deps = format!("{rtext} {p}tuples(");
                writeln!(body, "        assert(bag_eq({t1}, {t2}));\n        let f1 = {f1};\n        let f2 = {f2};").unwrap();
                writeln!(body, "        assert forall|a: {tt}| #[trigger] f1(a) == f2(a) by {{}}").unwrap();
                writeln!(body, "        lemma_bag_eq_map({t1}, {t2}, f1, f2);").unwrap();
                if b.distinct {
                    writeln!(body, "        lemma_bag_eq_dedup({t1}.map_values(f1), {t2}.map_values(f2));").unwrap();
                }
            }
            Shape::Scalar => {
                deps = format!("{p}row_of( {p}having(");
                writeln!(body, "        assert({call} == {});", to_db2(&call)).unwrap();
            }
            Shape::Grouped => {
                let tkeys: String = keys.iter().map(|(_, v)| format!(", {}", v.code)).collect();
                let keep1 = format!("|t: {tt}| {p}having(db, params{oa}{tkeys})");
                let kf1 = format!("|t: {tt}| {}", key_tuple_val(keys));
                let kty = key_tuple_ty(keys);
                let rf1 = format!("|k: {kty}| {p}row_of(db, params{oa}{})", key_tuple_args(keys, "k"));
                deps = format!("{p}row_of( {p}rows_keys");
                // step 1: the distinct keys of the groups that pass HAVING
                let kcall = format!("dedup(map_where({t1}, {keep1}, {kf1}))");
                let mut kb = String::new();
                writeln!(kb, "        assert(bag_eq({t1}, {t2}));\n        let keep1 = {keep1};\n        let keep2 = {};", to_db2(&keep1)).unwrap();
                writeln!(kb, "        let kf1 = {kf1};\n        let kf2 = {};", to_db2(&kf1)).unwrap();
                writeln!(kb, "        assert forall|a: {tt}| #[trigger] keep1(a) == keep2(a) by {{}}").unwrap();
                writeln!(kb, "        assert forall|a: {tt}| #[trigger] kf1(a) == kf2(a) by {{}}").unwrap();
                writeln!(kb, "        lemma_bag_eq_map_where({t1}, {t2}, keep1, keep2, kf1, kf2);").unwrap();
                writeln!(kb, "        lemma_bag_eq_dedup(map_where({t1}, keep1, kf1), map_where({t2}, keep2, kf2));").unwrap();
                self.lemma(&format!("{p}rows_keys"), "", &outer, Kind::Bag, &kcall, &format!("{p}having( {p}tuples( {}", keys.iter().map(|(_, v)| v.code.clone()).collect::<Vec<_>>().join(" ")), &kb);
                self.fns.pop();
                // step 2: one row per key
                let k1 = kcall.clone();
                let k2 = to_db2(&k1);
                writeln!(body, "        let rf1 = {rf1};\n        let rf2 = {};", to_db2(&rf1)).unwrap();
                writeln!(body, "        assert forall|k: {kty}| #[trigger] rf1(k) == rf2(k) by {{}}").unwrap();
                writeln!(body, "        assert(bag_eq({k1}, {k2}));").unwrap();
                writeln!(body, "        lemma_bag_eq_map({k1}, {k2}, rf1, rf2);").unwrap();
                if b.distinct {
                    writeln!(body, "        lemma_bag_eq_dedup({k1}.map_values(rf1), {k2}.map_values(rf2));").unwrap();
                }
            }
        }
        self.lemma(&format!("{p}rows"), "", &outer, Kind::Bag, &call, &deps, &body);
    }

    /// The final lemmas: the query's answers do not depend on the table order.
    pub fn print(&self, w: &mut String, tables: &[String], top: &Top, defined: &[String]) {
        let mut me = self.clone();
        let me = &mut me;
        writeln!(w, "// ── Permutation invariance (verified by Verus, not trusted) ──────────────────").unwrap();
        if let Some(why) = &self.skip {
            writeln!(w, "// Not stated for this query: {why}.\n").unwrap();
            return;
        }
        writeln!(w, "// A table is a bag: the answers do not depend on the order of any table's rows.\n").unwrap();
        writeln!(w, "/// db2 holds the same bag of rows as db in every table.").unwrap();
        writeln!(w, "pub open spec fn db_perm(db: Db, db2: Db) -> bool {{").unwrap();
        for t in tables {
            writeln!(w, "    &&& bag_eq(db.{t}, db2.{t})").unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        let b = &top.block;
        let keys = &b.keys;
        {
            let rtext: String = b.outs.iter().map(row_expr).collect::<Vec<_>>().join(" ");
            let htext: String = b.having.iter().map(bool_code).collect::<Vec<_>>().join(" ");
            if matches!(b.shape, Shape::Scalar) || (matches!(b.shape, Shape::Grouped) && !as_bag(b)) {
                me.lemma("row_ok", "", &vec![("r".to_string(), "OutRow".to_string())], Kind::Eq, "row_ok(db, params, r)", &format!("{rtext} {htext} group_count( having("), "");
            }
        }
        w.push_str(&me.texts());
        let rtext: String = b.outs.iter().map(row_expr).collect::<Vec<_>>().join(" ");
        let htext: String = b.having.iter().map(bool_code).collect::<Vec<_>>().join(" ");
        let ktext: String = keys.iter().map(|(_, v)| v.code.clone()).collect::<Vec<_>>().join(" ");
        let need = match b.shape {
            Shape::Grouped if !as_bag(b) => "row_ok( group_count( having(".to_string(),
            Shape::Plain | Shape::Grouped => format!("rows( tuples( {} {htext} {ktext}", defined.join(" ")),
            Shape::Scalar => "rows( row_ok(".to_string(),
        };
        let _ = (&rtext, &htext);
        let hides: String = me.fns.iter().map(|f| format!("    hide({f});\n")).collect();
        let all_calls = format!("{hides}{}", me.calls(&need));
        let ka: String = keys.iter().map(|(g, _)| format!(", {g}")).collect();
        let mut body = String::new();
        match b.shape {
            Shape::Grouped if !as_bag(b) => {
                let gq: Vec<String> = keys.iter().map(|(g, v)| format!("{g}: {}", key_ty(v))).collect();
                let gm: Vec<String> = key_cols(b)
                    .iter()
                    .enumerate()
                    .map(|(n, c)| {
                        let g = &keys[c.key.unwrap()].0;
                        if n == 0 { format!("(#[trigger] out[i]).{} == {g}", c.field) } else { format!("out[i].{} == {g}", c.field) }
                    })
                    .collect();
                writeln!(body, "    assert forall|i: int| 0 <= i < out.len() implies row_ok(db2, params, #[trigger] out[i]) by {{\n        assert(row_ok(db, params, out[i]) == row_ok(db2, params, out[i]));\n    }}").unwrap();
                writeln!(body, "    assert forall|{}| #[trigger] group_count(db2, params{ka}) > 0 && having(db2, params{ka}) implies exists|i: int| 0 <= i < out.len() && {} by {{", gq.join(", "), gm.join(" && ")).unwrap();
                writeln!(body, "        assert(group_count(db, params{ka}) == group_count(db2, params{ka}));\n        assert(having(db, params{ka}) == having(db2, params{ka}));\n    }}").unwrap();
            }
            Shape::Plain | Shape::Grouped => {
                writeln!(body, "    assert(bag_eq(rows(db, params), rows(db2, params)));").unwrap();
                if !defined.is_empty() {
                    let lets: String = keys.iter().map(|(g, v)| format!(" let {g} = {};", v.code)).collect();
                    let cond = if b.shape == Shape::Grouped {
                        format!("having(db, params{ka}) ==> ({})", defined.join(" && "))
                    } else {
                        defined.join(" && ")
                    };
                    let (lets2, cond2) = (to_db2(&lets), to_db2(&cond));
                    writeln!(body, "    assert forall|i: int| 0 <= i < tuples(db2, params).len() implies {{ let t = #[trigger] tuples(db2, params)[i];{lets2} {cond2} }} by {{").unwrap();
                    writeln!(body, "        let t0 = tuples(db2, params)[i];\n        assert(tuples(db2, params).contains(t0));").unwrap();
                    writeln!(body, "        lemma_bag_eq_contains(tuples(db2, params), tuples(db, params), t0);").unwrap();
                    writeln!(body, "        let k = choose|k: int| 0 <= k < tuples(db, params).len() && tuples(db, params)[k] == t0;").unwrap();
                    writeln!(body, "        assert(tuples(db, params)[k] == t0);\n    }}").unwrap();
                }
            }
            Shape::Scalar => {
                writeln!(body, "    lemma_bag_eq_len(rows(db, params), rows(db2, params));").unwrap();
                writeln!(body, "    assert forall|i: int| 0 <= i < out.len() implies row_ok(db2, params, #[trigger] out[i]) by {{\n        assert(row_ok(db, params, out[i]) == row_ok(db2, params, out[i]));\n    }}").unwrap();
            }
        }
        writeln!(w, "proof fn lemma_full_result_perm(db: Db, db2: Db, params: Params, out: Seq<OutRow>)\n    requires\n        db_perm(db, db2),\n        full_result(db, params, out),\n    ensures\n        full_result(db2, params, out),\n{{\n{all_calls}{body}}}\n").unwrap();
        let limited = top.limit.is_some() || top.offset > 0;
        let dir = |a: &str, b2: &str| -> String {
            let mut s = String::new();
            writeln!(s, "    if query_result({a}, params, out) {{").unwrap();
            if limited {
                writeln!(s, "        let full = choose|full: Seq<OutRow>| #[trigger] full_result({a}, params, full) && out == {};", cut(top, "full")).unwrap();
                writeln!(s, "        lemma_full_result_perm({a}, {b2}, params, full);\n        assert(full_result({b2}, params, full));").unwrap();
            } else {
                writeln!(s, "        lemma_full_result_perm({a}, {b2}, params, out);").unwrap();
            }
            writeln!(s, "    }}").unwrap();
            s
        };
        writeln!(w, "/// The query accepts the same answers for any reordering of the tables.").unwrap();
        writeln!(w, "pub proof fn lemma_query_perm(db: Db, db2: Db, params: Params, out: Seq<OutRow>)\n    requires\n        db_perm(db, db2),\n    ensures\n        query_result(db, params, out) <==> query_result(db2, params, out),\n{{").unwrap();
        writeln!(w, "    assert(db_perm(db2, db));").unwrap();
        w.push_str(&dir("db", "db2"));
        w.push_str(&dir("db2", "db"));
        writeln!(w, "}}\n").unwrap();
    }
}
