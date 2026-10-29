// Glue derived from the kernel obligations (obligations.rs `Model`); nothing
// here is written per query.
//
//   sql2verus ... --obligations <gen_obligations.rs> [--world <gen_world.rs>]
//                 [--ffi <gen_ffi.rs> --ffi-header <q.h> --ffi-prefix <p>]
//
// gen_world.rs (VERIFIED): the kernel's state on the verified aux store
// (verus_substrate).  `State` answers for `db` when `s.db() == db`.  Over the
// ob_* functions, with their contracts restated against a version's database:
//   stage_apply(base, <apply's inputs>)   the next version's slot from the
//     newest version's state (base: with the proof that it answers for that
//     version); requires apply's contracts with s.db() := version e, db2 :=
//     e + 1 (and the kernel state := the base's);
//   stage_build(head, <build's inputs>)   requires build's contracts with
//     db := version e + 1;
//   stage_invalid(head);
//   answer(reading, <execute's inputs>)   THEOREM 5.4: every answer is the
//     query on the database the read version answers for and, if the host's
//     contract holds for the read, on the database the snapshot sees.
// A staged slot becomes a version of a store of its own (`into_prepared`) or
// of the benchmark's world (scripts/gen_verus_world.py: taken only if staged
// on the batch's base).  The stage_* requires are the host's contract
// (TRUSTED, unchecked by the C ABI).
//
// gen_ffi.rs (TRUSTED GLUE, not verified) and its C header: pointers in,
// slices / rows out, the ob_* or stage_* / answer functions called, nothing else.
//   * <p>_build / _free / _apply / _rescan_<n>_keys / _execute / _result_*:
//     the kernel's states owned by the host (a host store keeps them);
//   * <p>s_*: the Verus store (new / free / newest_base / base_state /
//     base_free / newest_head / head_free / stage_apply / stage_build /
//     stage_invalid / staged_free / begin / abort / install / read /
//     reading_free / answer / invalidate_all / version_count); the world's
//     base_k / head_k / read_k hand out the same bases, heads and readings.
// Marshalling (fixed):
//   * a NULL flag `<field>__null` is one more u8 field (column) after its value;
//   * rows (`x`): `size_t x__n`, then
//       - a primitive without text, or a struct without text: one array
//         `x` (#[repr(C)] structs; the header declares them as
//         <p>_<snake name>);
//       - otherwise column arrays, one per field in declaration order:
//         `x__<field>` (nested: `x__<field>__<field>`); a text column is the
//         concatenated bytes `x__<f>` plus n + 1 offsets `x__<f>__off`;
//       - a (row, sign) pair: the row as above under `x__row`, and
//         `int64_t *x__sign`;
//   * rescan keys written out by <p>_rescan_<n>_keys: an integer-like key as
//     `T *out, size_t cap` (up to cap keys written; returns the count, call
//     again with more room); a text key as a text column `uint8_t *out,
//     size_t out_cap, size_t *off, size_t off_cap, size_t *out_len`: returns
//     the count, sets *out_len to the bytes needed, and writes bytes and
//     count + 1 offsets only if both fit.  apply takes the keys back as rows;
//   * text parameter `p`: `const uint8_t *p, size_t p__len`; scalars by value;
//   * results: <p>_result_len, and per column k <p>_result_c<k>(r, i, out..)
//     returning 0 for NULL, else 1 and the value in out: text as
//     (const uint8_t **, size_t *), bool as uint8_t, i32 / i64 / u64 as is (a
//     DOUBLE is its u64 bits), i128 as
//     (int64_t hi, uint64_t lo), a fraction as two i128 (num, den).
use super::*;
use crate::obligations::*;

/// C / C++ keywords that are not Rust ones: a C name gets a trailing '_'.
const C_RESERVED: &[&str] = &[
    "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "case", "catch", "char", "class", "compl", "const_cast",
    "default", "delete", "double", "dynamic_cast", "explicit", "export", "float", "friend", "goto", "inline", "int",
    "long", "mutable", "namespace", "new", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
    "protected", "public", "register", "reinterpret_cast", "restrict", "short", "signed", "sizeof", "static_assert",
    "static_cast", "switch", "template", "this", "thread_local", "throw", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "xor", "xor_eq", "const", "enum", "extern",
    "for", "if", "else", "return", "static", "struct", "while", "do", "break", "continue", "true", "false",
];

fn c_ident(s: &str) -> String {
    if C_RESERVED.contains(&s) { format!("{s}_") } else { s.to_string() }
}

fn ob<'m>(m: &'m Model, name: &str) -> &'m ObFn {
    m.fns.iter().find(|f| f.name == name).expect("obligation function")
}

fn strukt<'m>(m: &'m Model, name: &str) -> &'m CStruct {
    m.structs.iter().find(|s| s.name == name).expect("runtime struct")
}

/// `want`, or `want_`, `want__`, ... not in `taken`.
fn fresh(want: &str, taken: &[&str]) -> String {
    let mut s = want.to_string();
    while taken.contains(&s.as_str()) {
        s.push('_');
    }
    s
}

fn param_decl(p: &P) -> String {
    if p.ghost { format!("Ghost({}): Ghost<{}>", p.name, p.ty) } else { format!("{}: {}", p.name, p.ty) }
}

// ───────────────────────────── gen_world.rs ─────────────────────────────

pub(crate) fn world(m: &Model) -> String {
    let (build, apply, exec) = (ob(m, "build"), ob(m, "apply"), ob(m, "execute"));
    let taken: Vec<&str> = build.params.iter().chain(&apply.params).chain(&exec.params).map(|p| p.name.as_str()).collect();
    let (bs, hd, rd) = (fresh("base", &taken), fresh("head", &taken), fresh("reading", &taken));
    let s = fresh("state", &taken);
    let mut w = String::new();
    writeln!(w, "// GENERATED by {VERSION} from {} and inputs {}.  DO NOT EDIT: regenerate.", m.src_name, m.inputs_name).unwrap();
    writeln!(w, "//").unwrap();
    writeln!(w, "// The kernel's state on the verified aux store (verus_substrate), over the").unwrap();
    writeln!(w, "// ob_* functions with their contracts restated against a version's database").unwrap();
    writeln!(w, "// (rules: sql2verus/src/glue.rs).  stage_* make the next version's slot from").unwrap();
    writeln!(w, "// the newest version (a store's, or a world's slot of it); answer is the query").unwrap();
    writeln!(w, "// on a reading.  The stage_* requires are the host's contract (TRUSTED): the").unwrap();
    writeln!(w, "// inputs of version e + 1 are those of the commit after version e (or of a").unwrap();
    writeln!(w, "// build as of e + 1), installed in commit order.").unwrap();
    writeln!(w, "#![allow(unused_imports)]\nuse vstd::prelude::*;").unwrap();
    for md in ["bytes", "order", "rat", "rel", "sqlfns", "sums"] {
        writeln!(w, "use verus_common::{md}::*;").unwrap();
    }
    writeln!(w, "use verus_substrate::store::*;\nuse verus_substrate::world::*;").unwrap();
    writeln!(w, "use crate::gen_spec::*;\nuse crate::gen_obligations::*;\nuse crate::kernel::State;").unwrap();
    writeln!(w, "\nverus! {{\n").unwrap();
    writeln!(w, "impl AccelState<Db> for State {{\n    open spec fn answers(self, db: Db) -> bool {{\n        self.db() == db\n    }}\n}}\n").unwrap();
    writeln!(w, "pub type QStore = Store<Db, Slot<State>>;\n").unwrap();
    writeln!(w, "pub type QBase = Base<Db, State>;\n").unwrap();
    writeln!(w, "pub type QHead = Head<Db>;\n").unwrap();
    writeln!(w, "pub type QStaged = Staged<Db, State>;\n").unwrap();
    writeln!(w, "pub type QReading = Reading<Db, Slot<State>>;\n").unwrap();

    // stage_apply
    let sdb = format!("({bs}.timeline())({bs}.epoch() as nat)");
    let db2 = format!("({bs}.timeline())(({bs}.epoch() + 1) as nat)");
    let mut ps = vec![format!("{bs}: &QBase")];
    let mut args = vec![];
    for p in &apply.params {
        match (&p.arg, p.name.as_str()) {
            (Arg::State, _) => args.push(s.clone()),
            (Arg::Ghost, "db2") => args.push(format!("Ghost({db2})")),
            _ => {
                ps.push(param_decl(p));
                args.push(if p.ghost { format!("Ghost({})", p.name) } else { p.name.clone() });
            }
        }
    }
    writeln!(w, "/// Version e + 1's slot from the newest version's state (e = base's version) and the").unwrap();
    writeln!(w, "/// commit's inputs.  Err: the kernel could not apply them (the host rebuilds).").unwrap();
    writeln!(w, "pub fn stage_apply(\n    {},\n) -> (r: Result<QStaged, u32>)", ps.join(",\n    ")).unwrap();
    writeln!(w, "    requires").unwrap();
    for r in &apply.requires {
        writeln!(w, "        {},", inst(r, &format!("{bs}.state_spec()"), &sdb, "", &db2)).unwrap();
    }
    writeln!(w, "    ensures\n        r matches Ok(p) ==> p.epoch() == {bs}.epoch() && p.timeline() == {bs}.timeline(),").unwrap();
    writeln!(w, "{{\n    let {s} = {bs}.state();").unwrap();
    writeln!(w, "    match ob_apply({}) {{", args.join(", ")).unwrap();
    writeln!(w, "        Ok(s2) => Ok({bs}.head().stage(Some(s2))),\n        Err(code) => Err(code),\n    }}\n}}\n").unwrap();

    // stage_build
    let db = format!("{hd}.next_db()");
    let mut ps = vec![format!("{hd}: &QHead")];
    let mut args = vec![];
    for p in &build.params {
        if p.ghost && p.name == "db" {
            args.push(format!("Ghost({db})"));
        } else {
            ps.push(param_decl(p));
            args.push(if p.ghost { format!("Ghost({})", p.name) } else { p.name.clone() });
        }
    }
    writeln!(w, "/// Version e + 1's slot built from the inputs of the database as of e + 1 (an").unwrap();
    writeln!(w, "/// invalid slot if the kernel cannot build it).").unwrap();
    writeln!(w, "pub fn stage_build(\n    {},\n) -> (p: QStaged)", ps.join(",\n    ")).unwrap();
    writeln!(w, "    requires").unwrap();
    for r in &build.requires {
        writeln!(w, "        {},", inst(r, "", "", &db, "")).unwrap();
    }
    writeln!(w, "    ensures\n        p.epoch() == {hd}.epoch(),\n        p.timeline() == {hd}.timeline(),").unwrap();
    writeln!(w, "{{\n    let {s} = ob_build({});\n    {hd}.stage({s})\n}}\n", args.join(", ")).unwrap();

    // stage_invalid
    writeln!(w, "/// Version e + 1's slot, invalid.").unwrap();
    writeln!(w, "pub fn stage_invalid({hd}: &QHead) -> (p: QStaged)\n    ensures\n        p.epoch() == {hd}.epoch(),\n        p.timeline() == {hd}.timeline(),").unwrap();
    writeln!(w, "{{\n    {hd}.stage(None)\n}}\n").unwrap();

    // answer: Theorem 5.4
    let mut ps = vec![format!("{rd}: &QReading")];
    let mut args = vec![];
    for p in &exec.params {
        if matches!(p.arg, Arg::State) {
            args.push(s.clone());
        } else {
            ps.push(param_decl(p));
            args.push(if p.ghost { format!("Ghost({})", p.name) } else { p.name.clone() });
        }
    }
    let out_ens: Vec<&String> = exec.ensures.iter().filter(|x| x.starts_with("r matches Some(out)")).collect();
    let row_ty = exec.ret.strip_prefix("(r: Option<Vec<").and_then(|x| x.strip_suffix(">>)")).expect("execute returns Option<Vec<row>>").to_string();
    writeln!(w, "/// The query on a reading (THEOREM 5.4): every answer is the query on the database the").unwrap();
    writeln!(w, "/// version answers for, and, if the host's contract holds for the read, on the").unwrap();
    writeln!(w, "/// database the snapshot sees.  None: the host answers with SQL.").unwrap();
    writeln!(w, "pub fn answer(\n    {},\n) -> (r: Option<Vec<{row_ty}>>)", ps.join(",\n    ")).unwrap();
    writeln!(w, "    ensures").unwrap();
    for x in &out_ens {
        writeln!(w, "        {},", x.replace("s.db()", &format!("{rd}.db()"))).unwrap();
    }
    for x in out_ens.iter().filter(|x| x.contains("s.db()")) {
        writeln!(w, "        {rd}.contract() ==> ({}),", x.replace("s.db()", &format!("{rd}.snapshot_db()"))).unwrap();
    }
    writeln!(w, "{{\n    match {rd}.state() {{\n        Some({s}) => ob_execute({}),\n        None => None,\n    }}\n}}\n", args.join(", ")).unwrap();
    writeln!(w, "}} // verus!").unwrap();
    w
}

// ───────────────────────────── gen_ffi.rs + header ─────────────────────────────

/// One C-ABI function: Rust parameters, C parameters, body.
struct FfiFn {
    doc: Vec<String>,
    name: String,
    rust_params: Vec<String>,
    c_params: Vec<String>,
    rust_ret: String,
    c_ret: String,
    body: String,
}

fn c_prim(rt: &str) -> &'static str {
    match rt {
        "i64" => "int64_t",
        "i32" => "int32_t",
        "u8" => "uint8_t",
        "u64" => "uint64_t",
        "usize" => "size_t",
        _ => panic!("no C type for {rt}"),
    }
}

/// Is an element passed as one array (no text anywhere)?
fn flat(m: &Model, el: &Elem) -> bool {
    match el {
        Elem::Prim(rt) => *rt != "Vec<u8>",
        Elem::Struct(n) => strukt(m, n).copy,
        Elem::Pair(_) => false,
    }
}

/// One C argument of a rows parameter, in ABI order (shared by the C ABI and
/// the C++ host glue, so the two cannot disagree on positions).
#[derive(Clone, Debug)]
enum SlotKind {
    Count,
    Prim(&'static str),
    TextBytes,
    TextOff,
    CopyStruct(String),
    Sign,
}

#[derive(Clone, Debug)]
struct Slot {
    name: String,
    kind: SlotKind,
    /// Field path from the parameter's row to this argument's value.
    path: Vec<String>,
}

struct Ffi<'m> {
    m: &'m Model,
    p: String,
}

impl<'m> Ffi<'m> {
    fn c_struct(&self, n: &str) -> String {
        format!("{}_{}", self.p, snake(n))
    }

    fn elem_rust(&self, el: &Elem) -> String {
        match el {
            Elem::Prim(rt) => rt.to_string(),
            Elem::Struct(n) => n.clone(),
            Elem::Pair(n) => format!("({n}, i64)"),
        }
    }

    /// Column parameters of element `el` under name `pre` (n rows), and the
    /// Rust expression for row `i`; `slots` records them in ABI order.
    fn columns(&self, el: &Elem, pre: &str, path: &[String], rp: &mut Vec<String>, cp: &mut Vec<String>, slots: &mut Vec<Slot>) -> String {
        let slot = |slots: &mut Vec<Slot>, name: String, kind: SlotKind| slots.push(Slot { name, kind, path: path.to_vec() });
        match el {
            Elem::Prim("Vec<u8>") => {
                rp.push(format!("{pre}: *const u8"));
                rp.push(format!("{pre}__off: *const usize"));
                cp.push(format!("const uint8_t *{}", c_ident(pre)));
                cp.push(format!("const size_t *{pre}__off"));
                slot(slots, pre.to_string(), SlotKind::TextBytes);
                slot(slots, format!("{pre}__off"), SlotKind::TextOff);
                format!("text({pre}, {pre}__off, i)")
            }
            Elem::Prim(rt) => {
                rp.push(format!("{pre}: *const {rt}"));
                cp.push(format!("const {} *{}", c_prim(rt), c_ident(pre)));
                slot(slots, pre.to_string(), SlotKind::Prim(rt));
                format!("*{pre}.add(i)")
            }
            Elem::Struct(n) if strukt(self.m, n).copy => {
                rp.push(format!("{pre}: *const {n}"));
                cp.push(format!("const {} *{}", self.c_struct(n), c_ident(pre)));
                slot(slots, pre.to_string(), SlotKind::CopyStruct(n.clone()));
                format!("*{pre}.add(i)")
            }
            Elem::Struct(n) => {
                let s = strukt(self.m, n);
                let fs: Vec<String> = s
                    .fields
                    .iter()
                    .map(|(f, fe)| {
                        let mut p2 = path.to_vec();
                        p2.push(f.clone());
                        format!("{f}: {}", self.columns(fe, &format!("{pre}__{f}"), &p2, rp, cp, slots))
                    })
                    .collect();
                format!("{n} {{ {} }}", fs.join(", "))
            }
            Elem::Pair(n) => {
                let mut prow = path.to_vec();
                prow.push("row".into());
                let row = self.columns(&Elem::Struct(n.clone()), &format!("{pre}__row"), &prow, rp, cp, slots);
                rp.push(format!("{pre}__sign: *const i64"));
                cp.push(format!("const int64_t *{pre}__sign"));
                let mut ps = path.to_vec();
                ps.push("sign".into());
                slots.push(Slot { name: format!("{pre}__sign"), kind: SlotKind::Sign, path: ps });
                format!("({row}, *{pre}__sign.add(i))")
            }
        }
    }

    /// The ABI slots of a rows parameter (count first).
    fn rows_slots(&self, p: &P) -> Vec<Slot> {
        let Arg::Rows(el) = &p.arg else { return vec![] };
        let (mut rp, mut cp, mut slots) = (vec![], vec![], vec![]);
        slots.push(Slot { name: format!("{}__n", p.name), kind: SlotKind::Count, path: vec![] });
        self.columns(el, &p.name, &[], &mut rp, &mut cp, &mut slots);
        slots
    }

    /// The C parameters of obligation parameters `ps` (ghosts erased), the
    /// statements building the Rust values, and the call arguments.
    fn marshal(&self, ps: &[&P], rp: &mut Vec<String>, cp: &mut Vec<String>) -> (String, Vec<String>) {
        let mut pre = String::new();
        let mut args = vec![];
        for p in ps {
            let x = &p.name;
            match &p.arg {
                Arg::Ghost => args.push("Ghost::assume_new()".into()),
                Arg::State => {
                    rp.push(format!("{x}: *const State"));
                    cp.push(format!("const {}_state *{}", self.p, c_ident(x)));
                    args.push(format!("&*{x}"));
                }
                Arg::Text => {
                    rp.push(format!("{x}: *const u8"));
                    rp.push(format!("{x}__len: usize"));
                    cp.push(format!("const uint8_t *{}", c_ident(x)));
                    cp.push(format!("size_t {x}__len"));
                    args.push(format!("slice({x}, {x}__len)"));
                }
                Arg::Scalar(rt) => {
                    rp.push(format!("{x}: {rt}"));
                    cp.push(format!("{} {}", c_prim(rt), c_ident(x)));
                    args.push(x.clone());
                }
                Arg::NullText => {
                    rp.push(format!("{x}: *const u8"));
                    rp.push(format!("{x}__len: usize"));
                    cp.push(format!("const uint8_t *{}", c_ident(x)));
                    cp.push(format!("size_t {x}__len"));
                    args.push(format!("if {x}.is_null() {{ None }} else {{ Some(slice({x}, {x}__len)) }}"));
                }
                Arg::NullScalar(rt) => {
                    rp.push(format!("{x}: {rt}"));
                    rp.push(format!("{x}__null: u8"));
                    cp.push(format!("{} {}", c_prim(rt), c_ident(x)));
                    cp.push(format!("uint8_t {x}__null"));
                    args.push(format!("if {x}__null != 0 {{ None }} else {{ Some({x}) }}"));
                }
                Arg::Rows(el) => {
                    rp.push(format!("{x}__n: usize"));
                    cp.push(format!("size_t {x}__n"));
                    let mut slots = vec![];
                    if flat(self.m, el) && p.ty.starts_with("&[") {
                        self.columns(el, x, &[], rp, cp, &mut slots);
                        args.push(format!("slice({x}, {x}__n)"));
                    } else {
                        let row = self.columns(el, x, &[], rp, cp, &mut slots);
                        writeln!(pre, "    let {x}__v: Vec<{}> = (0..{x}__n).map(|i| {row}).collect();", self.elem_rust(el)).unwrap();
                        args.push(if p.ty.starts_with('&') { format!("&{x}__v") } else { format!("{x}__v") });
                    }
                }
            }
        }
        (pre, args)
    }

    /// A field's record tag: its SQL type (dec<s> for decimals).
    fn rec_tag(t: Ty) -> String {
        match t {
            Ty::Dec(s) => format!("dec{s}"),
            Ty::Date => "date".into(),
            Ty::Ts => "ts".into(),
            Ty::Bool => "bool".into(),
            Ty::Text => "text".into(),
            Ty::Flt(_) => "f64".into(),
            _ => "int".into(),
        }
    }

    /// Record-mode description of a row element: field `name:tag`s and the
    /// Rust statements writing row `r`'s values (flattened) as JSON.
    fn rec_row(&self, el: &Elem, prim: Ty, r: &str, prefix: &str, fields: &mut Vec<String>, stmts: &mut Vec<String>) {
        // `null`: the NULL flag of a NULL-able value, logged as JSON null
        let val = |ty: Ty, rt: &str, expr: String, null: Option<String>, fields: &mut Vec<String>, stmts: &mut Vec<String>, name: String| {
            fields.push(format!("{name}:{}", Self::rec_tag(ty)));
            let write = if rt == "Vec<u8>" { format!("rec_text(&{expr}, o);") } else { format!("o.push_str(&({expr}).to_string());") };
            stmts.push(match null {
                Some(flag) => format!("if {flag} != 0 {{ o.push_str(\"null\"); }} else {{ {write} }}"),
                None => write,
            });
        };
        match el {
            // a Vec<u8> is logged as text whatever the scan's type (text rescan keys)
            Elem::Prim(rt) => val(if *rt == "Vec<u8>" { Ty::Text } else { prim }, rt, format!("*{r}"), None, fields, stmts, if prefix.is_empty() { "value".into() } else { prefix.trim_end_matches('.').into() }),
            Elem::Struct(n) => {
                let st = strukt(self.m, n);
                for (i, (f, fe)) in st.fields.iter().enumerate() {
                    match fe {
                        Elem::Struct(_) => self.rec_row(fe, Ty::Int, &format!("{r}.{f}"), &format!("{prefix}{f}."), fields, stmts),
                        Elem::Prim(_) if st.is_flag(f) => {}
                        Elem::Prim(rt) => {
                            let null = st.cell_of(f).is_some_and(|c| c.1).then(|| format!("{r}.{}", null_flag(f)));
                            val(st.tys[i], rt, format!("{r}.{f}"), null, fields, stmts, format!("{prefix}{f}"))
                        }
                        Elem::Pair(_) => unreachable!(),
                    }
                }
            }
            Elem::Pair(n) => {
                self.rec_row(&Elem::Struct(n.clone()), Ty::Int, &format!("{r}.0"), &format!("{prefix}row."), fields, stmts);
                val(Ty::Int, "i64", format!("{r}.1"), None, fields, stmts, format!("{prefix}sign"));
            }
        }
    }

    /// The record-mode block of an entry point: log every rows argument, then
    /// return `ret` without calling the kernel.
    fn rec_block(&self, ps: &[&P], label: &str, ret: &str) -> String {
        let mut w = String::from("    if rec_on() {\n");
        for p in ps {
            let Arg::Rows(el) = &p.arg else { continue };
            let x = &p.name;
            let rows = if flat(self.m, el) && p.ty.starts_with("&[") { format!("slice({x}, {x}__n)") } else { format!("&{x}__v") };
            let prim = self.m.scans.iter().find(|s| *x == s.name || *x == format!("{}_rows", s.name)).map(|s| s.ty).unwrap_or(Ty::Int);
            let (mut fields, mut stmts) = (vec![], vec![]);
            self.rec_row(el, prim, "r", "", &mut fields, &mut stmts);
            let fl = fields.iter().map(|f| format!("\"{f}\"")).collect::<Vec<_>>().join(", ");
            let body = stmts.join(" o.push(',');\n            ");
            let ty = self.elem_rust(el);
            w.push_str(&format!("        rec_rows(\"{label}\", \"{x}\", &[{fl}], {rows}, |r: &{ty}, o: &mut String| {{\n            {body}\n        }});\n"));
        }
        w.push_str(&format!("        return {ret};\n    }}\n"));
        w
    }

    fn out_ty(&self) -> String {
        let cols: Vec<String> =
            self.m.outs.iter().map(|(rt, null, _)| if *null { format!("Option<{rt}>") } else { rt.to_string() }).collect();
        if cols.len() == 1 { cols[0].clone() } else { format!("({})", cols.join(", ")) }
    }

    fn fns(&self) -> Vec<FfiFn> {
        let p = &self.p;
        let ps = format!("{p}s");
        let mut out = vec![];
        let f = |doc: &[&str], name: String, rust_params: Vec<String>, c_params: Vec<String>, ret: (&str, String), body: String| FfiFn {
            doc: doc.iter().map(|s| s.to_string()).collect(),
            name,
            rust_params,
            c_params,
            rust_ret: ret.0.to_string(),
            c_ret: ret.1,
            body,
        };
        let state_c = format!("{p}_state");
        let build = ob(self.m, "build");
        let apply = ob(self.m, "apply");
        let exec = ob(self.m, "execute");

        // ── the kernel's states, owned by the host ──
        {
            let (mut rp, mut cp) = (vec![], vec![]);
            let ps: Vec<&P> = build.params.iter().collect();
            let (pre, args) = self.marshal(&ps, &mut rp, &mut cp);
            let pre = pre + &self.rec_block(&ps, "build", "ptr::null_mut()");
            out.push(f(
                &["Build from the scans: a state, or NULL (the host then answers with SQL)."],
                format!("{p}_build"),
                rp,
                cp,
                ("*mut State", format!("{state_c} *")),
                format!("{pre}    match ob_build({}) {{\n        Some(s) => Box::into_raw(Box::new(s)),\n        None => ptr::null_mut(),\n    }}\n", args.join(", ")),
            ));
        }
        out.push(f(
            &["Free a state from build / apply."],
            format!("{p}_free"),
            vec!["s: *mut State".into()],
            vec![format!("{state_c} *s")],
            ("", "void".into()),
            "    if !s.is_null() {\n        drop(Box::from_raw(s));\n    }\n".into(),
        ));
        for r in self.m.fns.iter().filter(|x| x.name.starts_with("rescan_")) {
            let (mut rp, mut cp) = (vec![], vec![]);
            let ps: Vec<&P> = r.params.iter().collect();
            let (pre, args) = self.marshal(&ps, &mut rp, &mut cp);
            let key = r.ret.trim_start_matches("(r: Vec<").trim_end_matches(">)").to_string();
            let text = key == "Vec<u8>";
            let ret = if text {
                "put_text_keys(&rec_text_keys(), out, out_cap, off, off_cap, out_len)".to_string()
            } else {
                format!("{{\n            let ks = rec_keys();\n            for (i, k) in ks.iter().take(cap).enumerate() {{\n                *out.add(i) = *k as {key};\n            }}\n            ks.len()\n        }}")
            };
            let pre = pre + &self.rec_block(&ps, "rescan_keys", &ret);
            let call = format!("ob_{}({})", r.name, args.join(", "));
            let (doc, body): (&[&str], String) = if text {
                for (rx, cx) in [("out: *mut u8", "uint8_t *out"), ("out_cap: usize", "size_t out_cap"), ("off: *mut usize", "size_t *off"), ("off_cap: usize", "size_t off_cap"), ("out_len: *mut usize", "size_t *out_len")] {
                    rp.push(rx.into());
                    cp.push(cx.into());
                }
                (
                    &["The keys to rescan for apply, as a text column: returns how many there are, sets *out_len to", "their total bytes, and writes the bytes into out and count + 1 offsets into off only if", "both fit (count + 1 <= off_cap, *out_len <= out_cap); else call again with more room."],
                    format!("{pre}    put_text_keys(&{call}, out, out_cap, off, off_cap, out_len)\n"),
                )
            } else {
                rp.push(format!("out: *mut {key}"));
                rp.push("cap: usize".into());
                cp.push(format!("{} *out", c_prim(&key)));
                cp.push("size_t cap".into());
                (
                    &["The keys to rescan for apply: writes up to cap of them into out and", "returns how many there are (call again with more room if that is more)."],
                    format!("{pre}    let keys = {call};\n    for (i, k) in keys.iter().take(cap).enumerate() {{\n        *out.add(i) = *k;\n    }}\n    keys.len()\n"),
                )
            };
            out.push(f(doc, format!("{p}_{}", r.name), rp, cp, ("usize", "size_t".into()), body));
        }
        {
            let (mut rp, mut cp) = (vec![], vec![]);
            let ps: Vec<&P> = apply.params.iter().collect();
            let (pre, args) = self.marshal(&ps, &mut rp, &mut cp);
            let pre = pre + &self.rec_block(&ps, "apply", "{\n            if !err.is_null() {\n                *err = 99;\n            }\n            ptr::null_mut()\n        }");
            rp.push("err: *mut i32".into());
            cp.push("int32_t *err".into());
            out.push(f(
                &["Apply one transaction: the new state (the input is unchanged), or NULL", "with *err set (the host then rebuilds)."],
                format!("{p}_apply"),
                rp,
                cp,
                ("*mut State", format!("{state_c} *")),
                format!("{pre}    match ob_apply({}) {{\n        Ok(s2) => {{\n            if !err.is_null() {{\n                *err = 0;\n            }}\n            Box::into_raw(Box::new(s2))\n        }}\n        Err(e) => {{\n            if !err.is_null() {{\n                *err = e as i32;\n            }}\n            ptr::null_mut()\n        }}\n    }}\n", args.join(", ")),
            ));
        }
        {
            let (mut rp, mut cp) = (vec![], vec![]);
            let (pre, args) = self.marshal(&exec.params.iter().collect::<Vec<_>>(), &mut rp, &mut cp);
            out.push(f(
                &["The query's answer on the state, or NULL (the host then answers with SQL)."],
                format!("{p}_execute"),
                rp,
                cp,
                ("*mut FfiResult", format!("{p}_result *")),
                format!("{pre}    match ob_execute({}) {{\n        Some(rows) => Box::into_raw(Box::new(FfiResult {{ rows }})),\n        None => ptr::null_mut(),\n    }}\n", args.join(", ")),
            ));
        }
        out.push(f(
            &["How many rows the answer has."],
            format!("{p}_result_len"),
            vec!["r: *const FfiResult".into()],
            vec![format!("const {p}_result *r")],
            ("usize", "size_t".into()),
            "    let rows = &(*r).rows;\n    rows.len()\n".into(),
        ));
        let multi = self.m.outs.len() > 1;
        for (k, (rt, null, _)) in self.m.outs.iter().enumerate() {
            let (mut rp, mut cp) = (vec!["r: *const FfiResult".to_string(), "i: usize".into()], vec![format!("const {p}_result *r"), "size_t i".into()]);
            let set = match *rt {
                "Vec<u8>" => {
                    rp.extend(["out: *mut *const u8".into(), "out_len: *mut usize".into()]);
                    cp.extend(["const uint8_t **out".into(), "size_t *out_len".into()]);
                    "*out = x.as_ptr();\n            *out_len = x.len();"
                }
                "bool" => {
                    rp.push("out: *mut u8".into());
                    cp.push("uint8_t *out".into());
                    "*out = *x as u8;"
                }
                "i32" | "i64" | "u64" => {
                    rp.push(format!("out: *mut {rt}"));
                    cp.push(format!("{} *out", c_prim(rt)));
                    "*out = *x;"
                }
                "i128" => {
                    rp.extend(["hi: *mut i64".into(), "lo: *mut u64".into()]);
                    cp.extend(["int64_t *hi".into(), "uint64_t *lo".into()]);
                    "*hi = (*x >> 64) as i64;\n            *lo = *x as u64;"
                }
                "(i128, i128)" => {
                    for n in ["num", "den"] {
                        rp.extend([format!("{n}_hi: *mut i64"), format!("{n}_lo: *mut u64")]);
                        cp.extend([format!("int64_t *{n}_hi"), format!("uint64_t *{n}_lo")]);
                    }
                    "*num_hi = (x.0 >> 64) as i64;\n            *num_lo = x.0 as u64;\n            *den_hi = (x.1 >> 64) as i64;\n            *den_lo = x.1 as u64;"
                }
                other => panic!("result type {other}"),
            };
            let v = if multi { format!("&rows[i].{k}") } else { "&rows[i]".into() };
            let body = if *null {
                format!("    let rows = &(*r).rows;\n    match {v} {{\n        Some(x) => {{\n            {set}\n            1\n        }}\n        None => 0,\n    }}\n")
            } else {
                format!("    let rows = &(*r).rows;\n    let x = {v};\n    {{\n            {set}\n    }}\n    1\n")
            };
            out.push(f(
                &[if *null { "Column k of row i: 0 if NULL, else 1 and the value in out." } else { "Column k of row i (never NULL: returns 1), the value in out." }],
                format!("{p}_result_c{k}"),
                rp,
                cp,
                ("u8", "uint8_t".into()),
                body,
            ));
        }
        out.push(f(
            &["Free an answer; its text pointers become invalid."],
            format!("{p}_result_free"),
            vec!["r: *mut FfiResult".into()],
            vec![format!("{p}_result *r")],
            ("", "void".into()),
            "    if !r.is_null() {\n        drop(Box::from_raw(r));\n    }\n".into(),
        ));

        // ── the Verus store ──
        let sc = format!("{ps}_store");
        let st = |x: &str| format!("{x}: *const QStore");
        let stc = |x: &str| format!("const {sc} *{x}");
        let ptr_free = |doc: &str, name: &str, ty: &str, cty: &str| {
            f(&[doc], format!("{ps}_{name}"), vec![format!("x: *mut {ty}")], vec![format!("{ps}_{cty} *x")], ("", "void".into()),
                "    if !x.is_null() {\n        drop(Box::from_raw(x));\n    }\n".into())
        };
        out.push(f(&["A store holding one invalid version, current for every snapshot."], format!("{ps}_new"), vec![], vec![], ("*mut QStore", format!("{sc} *")),
            "    Box::into_raw(Box::new(Store::new(0, Slot::Invalid, 0, Ghost::assume_new(), Ghost::assume_new())))\n".into()));
        out.push(f(&["Free a store."], format!("{ps}_free"), vec!["st: *mut QStore".into()], vec![format!("{sc} *st")], ("", "void".into()),
            "    if !st.is_null() {\n        drop(Box::from_raw(st));\n    }\n".into()));
        out.push(f(&["The newest version's state, to stage the next version from; NULL if", "it is invalid."], format!("{ps}_newest_base"), vec![st("st")], vec![stc("st")],
            ("*mut QBase", format!("{ps}_base *")),
            "    match base(&*st) {\n        Some(b) => Box::into_raw(Box::new(b)),\n        None => ptr::null_mut(),\n    }\n".into()));
        out.push(f(&["The base's state (valid while the base is)."], format!("{ps}_base_state"), vec!["b: *const QBase".into()], vec![format!("const {ps}_base *b")],
            ("*const State", format!("const {state_c} *")), "    (*b).state() as *const State\n".into()));
        out.push(ptr_free("Free a base.", "base_free", "QBase", "base"));
        out.push(f(&["The newest version, to stage a build on; NULL if no version can follow it."], format!("{ps}_newest_head"), vec![st("st")], vec![stc("st")],
            ("*mut QHead", format!("{ps}_head *")),
            "    match head(&*st) {\n        Some(h) => Box::into_raw(Box::new(h)),\n        None => ptr::null_mut(),\n    }\n".into()));
        out.push(ptr_free("Free a head.", "head_free", "QHead", "head"));
        {
            let mut rp = vec!["b: *const QBase".to_string()];
            let mut cp = vec![format!("const {ps}_base *b")];
            let ps_: Vec<&P> = apply.params.iter().filter(|x| !matches!(x.arg, Arg::State) && x.name != "db2").collect();
            let (pre, args) = self.marshal(&ps_, &mut rp, &mut cp);
            let pre = pre + &self.rec_block(&ps_, "apply", "{\n            if !err.is_null() {\n                *err = 99;\n            }\n            ptr::null_mut()\n        }");
            rp.push("err: *mut i32".into());
            cp.push("int32_t *err".into());
            out.push(f(&["The next version's slot from the base and one commit's inputs (as for apply);", "NULL with *err set if the kernel cannot apply them (the host rebuilds)."],
                format!("{ps}_stage_apply"), rp, cp, ("*mut QStaged", format!("{ps}_staged *")),
                format!("{pre}    match stage_apply(&*b, {}) {{\n        Ok(p) => {{\n            if !err.is_null() {{\n                *err = 0;\n            }}\n            Box::into_raw(Box::new(p))\n        }}\n        Err(e) => {{\n            if !err.is_null() {{\n                *err = e as i32;\n            }}\n            ptr::null_mut()\n        }}\n    }}\n", args.join(", "))));
        }
        {
            let mut rp = vec!["h: *const QHead".to_string()];
            let mut cp = vec![format!("const {ps}_head *h")];
            let ps_: Vec<&P> = build.params.iter().filter(|x| x.name != "db").collect();
            let (pre, args) = self.marshal(&ps_, &mut rp, &mut cp);
            let pre = pre + &self.rec_block(&ps_, "build", "ptr::null_mut()");
            out.push(f(&["The next version's slot built from scans (as for build) of the database as of", "that version; invalid if the kernel cannot build it."],
                format!("{ps}_stage_build"), rp, cp, ("*mut QStaged", format!("{ps}_staged *")),
                format!("{pre}    Box::into_raw(Box::new(stage_build(&*h, {})))\n", args.join(", "))));
        }
        out.push(f(&["The next version's slot holding a state built by apply (HOST CONTRACT: its database", "is that of the version, as for stage_build); consumes the state."], format!("{ps}_stage_adopt"),
            vec!["h: *const QHead".into(), "s: *mut State".into()], vec![format!("const {ps}_head *h"), format!("{state_c} *s")], ("*mut QStaged", format!("{ps}_staged *")),
            "    Box::into_raw(Box::new((*h).stage(Some(*Box::from_raw(s)))))\n".into()));
        out.push(f(&["The next version's slot, invalid."], format!("{ps}_stage_invalid"), vec!["h: *const QHead".into()], vec![format!("const {ps}_head *h")],
            ("*mut QStaged", format!("{ps}_staged *")), "    Box::into_raw(Box::new(stage_invalid(&*h)))\n".into()));
        out.push(ptr_free("Free a staged slot.", "staged_free", "QStaged", "staged"));
        out.push(f(&["Mark a commit in flight, visible from engine epoch `from`: 1, or 0 if one", "is already in flight."], format!("{ps}_begin"), vec![st("st"), "from: u64".into()], vec![stc("st"), "uint64_t from".into()],
            ("i32", "int32_t".into()), "    (*st).begin(from) as i32\n".into()));
        out.push(f(&["The commit in flight ended without a new version."], format!("{ps}_abort"), vec![st("st")], vec![stc("st")], ("", "void".into()), "    (*st).abort()\n".into()));
        out.push(f(&["Append the staged slot (consumed), current from engine epoch `from`; keep the", "versions snapshots at or after `floor` select; end_commit != 0 also ends the", "commit in flight.  0: appended; 1: stale (the store now holds one invalid version,", "or, for a build during a commit, is unchanged).  HOST CONTRACT: staged on this store."],
            format!("{ps}_install"), vec![st("st"), "p: *mut QStaged".into(), "from: u64".into(), "floor: u64".into(), "end_commit: i32".into()],
            vec![stc("st"), format!("{ps}_staged *p"), "uint64_t from".into(), "uint64_t floor".into(), "int32_t end_commit".into()], ("i32", "int32_t".into()),
            "    let p = (*Box::from_raw(p)).into_prepared(&*st);\n    let r = if end_commit != 0 { (*st).commit(p, from, floor) } else { (*st).install(p, from, floor) };\n    match r {\n        Install::Applied => 0,\n        Install::Stale => 1,\n    }\n".into()));
        out.push(f(&["The reading of a snapshot at engine epoch `at` (0: the newest), waiting up", "to `tries` checks for a pending commit.  *status: 0 a reading (returned;", "free it), 1 invalid, 2 still pending, 3 not visible."],
            format!("{ps}_read"), vec![st("st"), "at: u64".into(), "tries: u64".into(), "status: *mut i32".into()], vec![stc("st"), "uint64_t at".into(), "uint64_t tries".into(), "int32_t *status".into()],
            ("*mut QReading", format!("{ps}_reading *")),
            "    let (code, r) = match (*st).read(at, tries) {\n        Read::Found(r) if r.state().is_some() => (0, Box::into_raw(Box::new(r))),\n        Read::Found(_) => (1, ptr::null_mut()),\n        Read::Pending => (2, ptr::null_mut()),\n        Read::NotVisible => (3, ptr::null_mut()),\n    };\n    if !status.is_null() {\n        *status = code;\n    }\n    r\n".into()));
        out.push(ptr_free("Free a reading.", "reading_free", "QReading", "reading"));
        {
            let (mut rp, mut cp) = (vec!["r: *const QReading".to_string()], vec![format!("const {ps}_reading *r")]);
            let ps_: Vec<&P> = exec.params.iter().filter(|x| !matches!(x.arg, Arg::State)).collect();
            let (pre, args) = self.marshal(&ps_, &mut rp, &mut cp);
            out.push(f(&["The query's answer on the reading (THEOREM 5.4, gen_world.rs), or NULL (the host then", "answers with SQL)."],
                format!("{ps}_answer"), rp, cp, ("*mut FfiResult", format!("{p}_result *")),
                format!("{pre}    match answer(&*r, {}) {{\n        Some(rows) => Box::into_raw(Box::new(FfiResult {{ rows }})),\n        None => ptr::null_mut(),\n    }}\n", args.join(", "))));
        }
        out.push(f(&["Every kept version's state becomes invalid."], format!("{ps}_invalidate_all"), vec![st("st")], vec![stc("st")], ("", "void".into()), "    (*st).invalidate_all()\n".into()));
        out.push(f(&["How many versions are kept."], format!("{ps}_version_count"), vec![st("st")], vec![stc("st")], ("usize", "size_t".into()), "    (*st).version_count()\n".into()));
        out.push(f(&["Bytes the library's Rust code holds live (a counting global allocator,", "substrate/verus/heap): every kept version, plus transients in flight."], format!("{ps}_heap_bytes"), vec![], vec![], ("usize", "size_t".into()), "    verus_heap::live_bytes()\n".into()));
        out.push(f(&["The most live bytes since the start or the last reset."], format!("{ps}_heap_peak_bytes"), vec![], vec![], ("usize", "size_t".into()), "    verus_heap::peak_bytes()\n".into()));
        out.push(f(&["Start a new peak measurement from the current live bytes."], format!("{ps}_heap_reset_peak"), vec![], vec![], ("", "void".into()), "    verus_heap::reset_peak()\n".into()));
        out
    }
}

/// The C type a Rust parameter type of the ABI has (prefix p).
fn c_of_rust(g: &Ffi, t: &str) -> String {
    let t = t.trim();
    let ptr = |inner: &str, konst: bool| {
        let c = c_of_rust(g, inner);
        let sep = if c.ends_with('*') { "" } else { " " };
        if konst { format!("const {c}{sep}*") } else { format!("{c}{sep}*") }
    };
    if let Some(x) = t.strip_prefix("*const ") {
        return ptr(x, true);
    }
    if let Some(x) = t.strip_prefix("*mut ") {
        return ptr(x, false);
    }
    let p = &g.p;
    match t {
        "i64" | "i32" | "u8" | "u64" | "usize" => c_prim(t).to_string(),
        "State" => format!("{p}_state"),
        "FfiResult" => format!("{p}_result"),
        "QStore" => format!("{p}s_store"),
        "QBase" => format!("{p}s_base"),
        "QHead" => format!("{p}s_head"),
        "QStaged" => format!("{p}s_staged"),
        "QReading" => format!("{p}s_reading"),
        n => g.c_struct(n),
    }
}

/// The C type of a C parameter declaration (its name dropped).
fn c_decl_ty(d: &str) -> String {
    let d = d.trim();
    let cut = d.rfind(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).map_or(0, |i| i + 1);
    d[..cut].trim().to_string()
}

/// Every Rust parameter (and result) of the ABI has the C type its
/// declaration says, in the same order.
fn check(g: &Ffi, fns: &[FfiFn]) -> Res<()> {
    let norm = |s: &str| s.replace(' ', "");
    for f in fns {
        if f.rust_params.len() != f.c_params.len() {
            bail!("ffi {}: {} Rust parameters, {} C ones", f.name, f.rust_params.len(), f.c_params.len());
        }
        for (r, c) in f.rust_params.iter().zip(&f.c_params) {
            let rt = r.split_once(':').map(|x| x.1).unwrap_or("");
            if norm(&c_of_rust(g, rt)) != norm(&c_decl_ty(c)) {
                bail!("ffi {}: Rust `{r}` is not C `{c}`", f.name);
            }
        }
        let want = if f.rust_ret.is_empty() { "void".to_string() } else { c_of_rust(g, &f.rust_ret) };
        if norm(&want) != norm(&f.c_ret) {
            bail!("ffi {}: returns Rust {} but C {}", f.name, f.rust_ret, f.c_ret);
        }
    }
    Ok(())
}

/// gen_ffi.rs and its C header.
pub(crate) fn ffi(m: &Model, prefix: &str, header_name: &str) -> Res<(String, String)> {
    if !prefix.chars().next().map_or(false, |c| c.is_ascii_lowercase()) || !prefix.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_') {
        bail!("--ffi-prefix {prefix}: a lower-case C identifier");
    }
    let g = Ffi { m, p: prefix.to_string() };
    let fns = g.fns();
    check(&g, &fns)?;
    let out_ty = g.out_ty();

    let mut r = String::new();
    writeln!(r, "// GENERATED by {VERSION} from {} and inputs {}.  DO NOT EDIT: regenerate.", m.src_name, m.inputs_name).unwrap();
    writeln!(r, "//").unwrap();
    writeln!(r, "// C ABI of the kernel (TRUSTED GLUE, not verified; declared in {header_name}).").unwrap();
    writeln!(r, "// It only turns pointers into slices and rows and calls the ob_* functions").unwrap();
    writeln!(r, "// (gen_obligations.rs) or the store's (gen_world.rs); ghost arguments are erased,").unwrap();
    writeln!(r, "// and their preconditions are the host's to meet.  Rules: sql2verus/src/glue.rs.").unwrap();
    writeln!(r, "#![allow(unused_imports, unused_variables, unused_unsafe, non_snake_case, clippy::all)]").unwrap();
    writeln!(r, "use std::ptr;\nuse std::sync::Arc;\nuse vstd::prelude::Ghost;").unwrap();
    writeln!(r, "use verus_substrate::store::{{Install, Read, Slot, Store}};\nuse verus_substrate::world::{{base, head}};").unwrap();
    writeln!(r, "use crate::gen_obligations::*;\nuse crate::gen_world::*;\nuse crate::kernel::State;\n").unwrap();
    writeln!(r, "// A C array as a slice (NULL or empty: an empty slice).").unwrap();
    writeln!(r, "unsafe fn slice<'a, T>(p: *const T, n: usize) -> &'a [T] {{\n    if n == 0 || p.is_null() {{\n        &[]\n    }} else {{\n        std::slice::from_raw_parts(p, n)\n    }}\n}}\n").unwrap();
    writeln!(r, "// Text i of a text column: bytes [off[i], off[i + 1]).").unwrap();
    writeln!(r, "unsafe fn text(b: *const u8, off: *const usize, i: usize) -> Vec<u8> {{\n    let (s, e) = (*off.add(i), *off.add(i + 1));\n    slice(b.wrapping_add(s), e - s).to_vec()\n}}\n").unwrap();
    writeln!(r, "// Record mode (glue contract checks): with ACCEL_VERUS_RECORD_DIR set, every").unwrap();
    writeln!(r, "// entry point appends the rows it was given to <dir>/records.jsonl and returns").unwrap();
    writeln!(r, "// without calling the kernel (rescan keys come from ACCEL_VERUS_RECORD_KEYS).").unwrap();
    writeln!(r, "fn rec_on() -> bool {{\n    std::env::var_os(\"ACCEL_VERUS_RECORD_DIR\").is_some()\n}}\n").unwrap();
    writeln!(r, "fn rec_text(b: &[u8], o: &mut String) {{\n    o.push('\"');\n    for x in b {{\n        o.push_str(&format!(\"{{:02x}}\", x));\n    }}\n    o.push('\"');\n}}\n").unwrap();
    writeln!(r, "fn rec_rows<T>(f: &str, param: &str, fields: &[&str], rows: &[T], w: fn(&T, &mut String)) {{").unwrap();
    writeln!(r, "    use std::io::Write;\n    let Some(d) = std::env::var_os(\"ACCEL_VERUS_RECORD_DIR\") else {{ return }};").unwrap();
    writeln!(r, "    let mut o = format!(\"{{{{\\\"fn\\\":\\\"{{f}}\\\",\\\"param\\\":\\\"{{param}}\\\",\\\"fields\\\":[\");").unwrap();
    writeln!(r, "    o.push_str(&fields.iter().map(|x| format!(\"\\\"{{x}}\\\"\")).collect::<Vec<_>>().join(\",\"));").unwrap();
    writeln!(r, "    o.push_str(\"],\\\"rows\\\":[\");").unwrap();
    writeln!(r, "    for (i, row) in rows.iter().enumerate() {{\n        if i > 0 {{\n            o.push(',');\n        }}\n        o.push('[');\n        w(row, &mut o);\n        o.push(']');\n    }}").unwrap();
    writeln!(r, "    o.push_str(\"]}}\\n\");").unwrap();
    writeln!(r, "    let path = std::path::Path::new(&d).join(\"records.jsonl\");").unwrap();
    writeln!(r, "    if let Ok(mut fh) = std::fs::OpenOptions::new().create(true).append(true).open(path) {{\n        let _ = fh.write_all(o.as_bytes());\n    }}\n}}\n").unwrap();
    writeln!(r, "fn rec_keys() -> Vec<i64> {{").unwrap();
    writeln!(r, "    let Some(p) = std::env::var_os(\"ACCEL_VERUS_RECORD_KEYS\") else {{ return vec![] }};").unwrap();
    writeln!(r, "    std::fs::read_to_string(p).unwrap_or_default().split(|c: char| c == ',' || c.is_whitespace()).filter_map(|x| x.trim().parse().ok()).collect()\n}}\n").unwrap();
    if m.fns.iter().any(|f| f.name.starts_with("rescan_") && f.ret.contains("Vec<Vec<u8>>")) {
        writeln!(r, "// Text keys as a text column: the count, *out_len = their bytes; written only if both fit.").unwrap();
        writeln!(r, "unsafe fn put_text_keys(keys: &[Vec<u8>], out: *mut u8, out_cap: usize, off: *mut usize, off_cap: usize, out_len: *mut usize) -> usize {{").unwrap();
        writeln!(r, "    let need: usize = keys.iter().map(|k| k.len()).sum();\n    *out_len = need;\n    if keys.len() + 1 <= off_cap && need <= out_cap {{\n        let mut at = 0;\n        for (i, k) in keys.iter().enumerate() {{\n            *off.add(i) = at;\n            if !k.is_empty() {{\n                ptr::copy_nonoverlapping(k.as_ptr(), out.add(at), k.len());\n            }}\n            at += k.len();\n        }}\n        *off.add(keys.len()) = at;\n    }}\n    keys.len()\n}}\n").unwrap();
        writeln!(r, "// Text rescan keys: the lines of ACCEL_VERUS_RECORD_KEYS, bytes as written.").unwrap();
        writeln!(r, "fn rec_text_keys() -> Vec<Vec<u8>> {{").unwrap();
        writeln!(r, "    let Some(p) = std::env::var_os(\"ACCEL_VERUS_RECORD_KEYS\") else {{ return vec![] }};").unwrap();
        writeln!(r, "    let b = std::fs::read(p).unwrap_or_default();\n    let mut ks: Vec<Vec<u8>> = b.split(|c| *c == b'\\n').map(|k| k.to_vec()).collect();\n    if ks.last().is_some_and(|k| k.is_empty()) {{\n        ks.pop();\n    }}\n    ks\n}}\n").unwrap();
    }
    writeln!(r, "/// An answer handed to the host (read through the result accessors).").unwrap();
    writeln!(r, "pub struct FfiResult {{\n    pub rows: Vec<{out_ty}>,\n}}\n").unwrap();
    for f in &fns {
        for d in &f.doc {
            writeln!(r, "/// {d}").unwrap();
        }
        let ret = if f.rust_ret.is_empty() { String::new() } else { format!(" -> {}", f.rust_ret) };
        let unsafe_ = if f.rust_params.iter().any(|p| p.contains('*')) { "unsafe " } else { "" };
        writeln!(r, "#[no_mangle]\npub {unsafe_}extern \"C\" fn {}({}){ret} {{\n{}}}\n", f.name, f.rust_params.join(", "), f.body).unwrap();
    }

    let mut h = String::new();
    writeln!(h, "/* GENERATED by {VERSION} from {} and inputs {}.  DO NOT EDIT: regenerate.", m.src_name, m.inputs_name).unwrap();
    writeln!(h, " *").unwrap();
    writeln!(h, " * C interface of the Verus-verified kernel (gen_ffi.rs).  The kernel is proved").unwrap();
    writeln!(h, " * against the specification generated from the query (gen_spec.rs); the").unwrap();
    writeln!(h, " * caller must meet the scan / capture contracts of gen_obligations.rs, and for").unwrap();
    writeln!(h, " * the Verus store ({prefix}s_*) those of gen_world.rs.  Marshalling:").unwrap();
    writeln!(h, " * sql2verus/src/glue.rs. */").unwrap();
    writeln!(h, "#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n#ifdef __cplusplus\nextern \"C\" {{\n#endif\n").unwrap();
    for t in [format!("{prefix}_state"), format!("{prefix}_result"), format!("{prefix}s_store"), format!("{prefix}s_base"), format!("{prefix}s_head"), format!("{prefix}s_staged"), format!("{prefix}s_reading")] {
        writeln!(h, "typedef struct {t} {t};").unwrap();
    }
    writeln!(h).unwrap();
    for s in m.structs.iter().filter(|s| s.copy) {
        writeln!(h, "typedef struct {{").unwrap();
        for (f, fe) in &s.fields {
            let ty = match fe {
                Elem::Prim(rt) => c_prim(rt).to_string(),
                Elem::Struct(n) => g.c_struct(n),
                Elem::Pair(_) => unreachable!(),
            };
            writeln!(h, "  {ty} {};", c_ident(f)).unwrap();
        }
        writeln!(h, "}} {};\n", g.c_struct(&s.name)).unwrap();
    }
    for f in &fns {
        writeln!(h, "/* {} */", f.doc.join("\n   ")).unwrap();
        let ret = &f.c_ret;
        let sep = if ret.ends_with('*') { "" } else { " " };
        let ps = if f.c_params.is_empty() { "void".to_string() } else { f.c_params.join(", ") };
        writeln!(h, "{ret}{sep}{}({ps});\n", f.name).unwrap();
    }
    writeln!(h, "#ifdef __cplusplus\n}}\n#endif").unwrap();
    Ok((r, h))
}

// ───────────────────────────── host glue (C++) ─────────────────────────────
//
// gen_glue.cpp (TRUSTED GLUE): the query's side of the generic host kit.
// Not used by the Verus pipeline (its glue is written by an agent and held to
// the declared inputs by the record-mode contract check); kept as the
// reference glue for the kit's regression tests (accelerators/tpch/q21/verus_kit/q21)
// and the corpus compile check (scripts/test_sql2verus.sh)
// (substrate/verus/kit/verus_kit.hpp).  It runs each declared
// scan / rescan SQL through the engine's kit::Host, reads captured delta rows,
// fills the C ABI's arrays in the slot order above, and registers the query
// (kit::Glue) for the engine runtimes.  Rules (fixed):
//   * a scan or rescan runs its declared SQL as written; a rescan wraps it:
//     SELECT * FROM (<sql>) WHERE <KEYS output> IN (<keys>), in chunks;
//   * a delta row is the captured row of its table's captured columns (by
//     name; the engine resolves them); a lossy field is computed from it
//     only for comparisons of columns and literals (anything else: refused);
//   * cells convert exactly (decimals rescaled without loss, dates as days,
//     DOUBLE as its IEEE-754 bits); a NULL cell of a NULL-able field sets its
//     flag; any other NULL, a mismatched or a non-finite cell declines (the
//     module then rebuilds or answers with SQL);
//   * results: text, integers, dates and booleans as such, decimals as exact
//     text, fractions as doubles, DOUBLE bits as doubles.

/// A Rust/SQL type's kit cell reader on row `row`, column `i`.
fn kit_conv(ty: Ty, row: &str, i: usize) -> Res<String> {
    Ok(match ty {
        Ty::Int => format!("kit::I64({row}, {i}, 0)"),
        Ty::Ts => format!("kit::Ts({row}, {i})"),
        Ty::Dec(s) => format!("kit::I64({row}, {i}, {s})"),
        Ty::Date => format!("kit::Date({row}, {i})"),
        Ty::Bool => format!("kit::Bool({row}, {i})"),
        Ty::Text => format!("kit::Text({row}, {i})"),
        Ty::Flt(1) => format!("kit::F64({row}, {i})"),
        other => bail!("host glue: no kit reader for {other:?} columns"),
    })
}

/// A struct field's value from cell `i` of `row`: the cell, the NULL-flag of a NULL-able
/// cell, or (for a NULL-able cell) the type's zero in place of a NULL.
fn kit_field(ty: Ty, row: &str, i: usize, nullable: bool, flag: bool) -> Res<String> {
    if flag {
        return Ok(format!("static_cast<uint8_t>(kit::IsNull({row}, {i}))"));
    }
    let v = kit_conv(ty, row, i)?;
    if !nullable {
        return Ok(v);
    }
    let zero = match ty {
        Ty::Text => "kit::kNoText",
        Ty::Date => "int32_t{0}",
        Ty::Bool => "uint8_t{0}",
        Ty::Flt(_) => "uint64_t{0}",
        _ => "int64_t{0}",
    };
    Ok(format!("(kit::IsNull({row}, {i}) ? {zero} : {v})"))
}

/// A C++ string literal of exact bytes.
fn cpp_bytes(s: &str) -> String {
    let mut o = String::from("std::string(\"");
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || b == b' ' || b == b'_' {
            o.push(b as char);
        } else {
            o.push_str(&format!("\\{:03o}", b));
        }
    }
    o.push_str(&format!("\", {})", s.len()));
    o
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum CK {
    Int,
    Text,
    Date,
    Bool,
}

/// A lossy delta field's expression in C++ over captured row `row`.
fn kit_expr(e: &Expr, cols: &[(String, Ty)], row: &str) -> Res<(String, CK)> {
    let bad = || format!("host glue: lossy delta expression `{e}`: only comparisons of columns and literals (AND / OR / NOT) are computed by the glue");
    Ok(match e {
        Expr::Column(c) => {
            let i = cols.iter().position(|x| x.0 == c.name).ok_or_else(bad)?;
            match cols[i].1 {
                Ty::Int => (format!("kit::I64({row}, {i}, 0)"), CK::Int),
                Ty::Text => (format!("kit::Text({row}, {i})"), CK::Text),
                Ty::Date => (format!("kit::Date({row}, {i})"), CK::Date),
                Ty::Bool => (format!("(kit::Bool({row}, {i}) != 0)"), CK::Bool),
                _ => return Err(bad()),
            }
        }
        Expr::Literal(v, _) => match v {
            ScalarValue::Utf8(Some(s)) | ScalarValue::Utf8View(Some(s)) | ScalarValue::LargeUtf8(Some(s)) => (cpp_bytes(s), CK::Text),
            ScalarValue::Int64(Some(x)) => (format!("int64_t{{{x}}}"), CK::Int),
            ScalarValue::Int32(Some(x)) => (format!("int64_t{{{x}}}"), CK::Int),
            ScalarValue::Int16(Some(x)) => (format!("int64_t{{{x}}}"), CK::Int),
            ScalarValue::Int8(Some(x)) => (format!("int64_t{{{x}}}"), CK::Int),
            ScalarValue::Boolean(Some(b)) => (format!("{b}"), CK::Bool),
            ScalarValue::Date32(Some(d)) => (format!("int32_t{{{d}}}"), CK::Date),
            _ => return Err(bad()),
        },
        Expr::Cast(c) => {
            let (code, k) = kit_expr(&c.expr, cols, row)?;
            match (&*c.expr, c.field.data_type()) {
                (Expr::Literal(ScalarValue::Utf8(Some(s)) | ScalarValue::Utf8View(Some(s)), _), DataType::Date32) => {
                    (format!("int32_t{{{}}}", parse_date(s)?), CK::Date)
                }
                (_, DataType::Utf8 | DataType::Utf8View | DataType::LargeUtf8) if k == CK::Text => (code, k),
                (_, DataType::Int64 | DataType::Int32 | DataType::Int16 | DataType::Int8) if k == CK::Int => (code, k),
                (_, DataType::Date32) if k == CK::Date => (code, k),
                (_, DataType::Boolean) if k == CK::Bool => (code, k),
                _ => return Err(bad()),
            }
        }
        Expr::Not(x) => {
            let (code, k) = kit_expr(x, cols, row)?;
            if k != CK::Bool {
                return Err(bad());
            }
            (format!("!{code}"), CK::Bool)
        }
        Expr::BinaryExpr(b) => {
            let (l, lk) = kit_expr(&b.left, cols, row)?;
            let (r, rk) = kit_expr(&b.right, cols, row)?;
            let op = match b.op {
                Operator::Eq => "==",
                Operator::NotEq => "!=",
                Operator::Lt => "<",
                Operator::LtEq => "<=",
                Operator::Gt => ">",
                Operator::GtEq => ">=",
                Operator::And => "&&",
                Operator::Or => "||",
                _ => return Err(bad()),
            };
            let logic = matches!(b.op, Operator::And | Operator::Or);
            if lk != rk || (logic && lk != CK::Bool) {
                return Err(bad());
            }
            (format!("({l} {op} {r})"), CK::Bool)
        }
        _ => return Err(bad()),
    })
}

impl<'m> Ffi<'m> {
    /// The C++ value of the field at `path` of a struct `n` row (initializer
    /// for a struct field), given a leaf resolver.
    fn kit_init(&self, n: &str, path: &[String], leaf: &dyn Fn(&[String]) -> Res<String>) -> Res<String> {
        let s = strukt(self.m, n);
        let mut vs = vec![];
        for (f, fe) in &s.fields {
            let mut p2 = path.to_vec();
            p2.push(f.clone());
            vs.push(match fe {
                Elem::Struct(m) => self.kit_init(m, &p2, leaf)?,
                _ => leaf(&p2)?,
            });
        }
        Ok(format!("{}{{{}}}", self.c_struct(n), vs.join(", ")))
    }

    /// Members holding a rows parameter's arrays, and the statements adding
    /// one row (leaf: the C++ value at a field path).
    fn kit_rows(&self, slots: &[Slot], leaf: &dyn Fn(&[String]) -> Res<String>) -> Res<(Vec<String>, Vec<String>)> {
        let (mut mem, mut push) = (vec![], vec![]);
        for sl in slots {
            let n = &sl.name;
            match &sl.kind {
                SlotKind::Count => {
                    mem.push(format!("size_t {n} = 0;"));
                    push.push(format!("++{n};"));
                }
                SlotKind::Prim(rt) => {
                    mem.push(format!("std::vector<{}> {n};", c_prim(rt)));
                    push.push(format!("{n}.push_back({});", leaf(&sl.path)?));
                }
                SlotKind::TextBytes => {
                    mem.push(format!("kit::TextColumn {n};"));
                    push.push(format!("{n}.Push({});", leaf(&sl.path)?));
                }
                SlotKind::TextOff => {}
                SlotKind::CopyStruct(s) => {
                    mem.push(format!("std::vector<{}> {n};", self.c_struct(s)));
                    push.push(format!("{n}.push_back({});", self.kit_init(s, &sl.path, leaf)?));
                }
                SlotKind::Sign => {
                    mem.push(format!("std::vector<int64_t> {n};"));
                    push.push(format!("{n}.push_back({});", leaf(&sl.path)?));
                }
            }
        }
        Ok((mem, push))
    }

    /// The C++ arguments of a rows parameter held in object `obj`.
    fn kit_args(slots: &[Slot], obj: &str) -> Vec<String> {
        slots
            .iter()
            .map(|sl| match &sl.kind {
                SlotKind::Count => format!("{obj}.{}", sl.name),
                SlotKind::TextBytes => format!("{obj}.{}.bytes.data()", sl.name),
                SlotKind::TextOff => format!("{obj}.{}.off.data()", sl.name.trim_end_matches("__off")),
                _ => format!("{obj}.{}.data()", sl.name),
            })
            .collect()
    }

    /// Leaf resolver for a scan / rescan row `r` of element `el`.
    fn scan_leaf<'a>(&'a self, sd: &'a ScanDecl) -> impl Fn(&[String]) -> Res<String> + 'a {
        move |path: &[String]| match (&sd.el, path) {
            (Elem::Prim(_), []) => kit_conv(sd.ty, "r", 0),
            (Elem::Struct(n), [f]) => {
                let s = strukt(self.m, n);
                let i = s.fields.iter().position(|x| &x.0 == f).ok_or_else(|| format!("host glue: {n} has no field {f}"))?;
                let (cell, nullable) = s.cell_of(f).unwrap();
                kit_field(s.tys[i], "r", cell, nullable, s.is_flag(f))
            }
            _ => bail!("host glue: scan {}: unexpected field path {path:?}", sd.name),
        }
    }

    /// Leaf resolver for a captured change `c` of delta `dd`.
    fn delta_leaf<'a>(&'a self, dd: &'a DeltaDecl) -> impl Fn(&[String]) -> Res<String> + 'a {
        move |path: &[String]| {
            let p: Vec<&String> = path.iter().filter(|x| x.as_str() != "row").collect();
            match p.as_slice() {
                [f] if f.as_str() == "sign" => Ok("c.sign".to_string()),
                [f] => {
                    let (_, src, ty) = dd.fields.iter().find(|x| &x.0 == *f).ok_or_else(|| format!("host glue: delta {}: no field {f}", dd.table))?;
                    match src {
                        FieldSrc::Col(i, nullable) => kit_field(*ty, "c.row", *i, *nullable, false),
                        FieldSrc::NullOf(i) => kit_field(*ty, "c.row", *i, true, true),
                        FieldSrc::Expr(e) => {
                            let (code, k) = kit_expr(e, &dd.cols, "c.row")?;
                            match (ty, k) {
                                (Ty::Bool, CK::Bool) => Ok(format!("static_cast<uint8_t>({code})")),
                                (Ty::Int, CK::Int) => Ok(code),
                                _ => bail!("host glue: delta {} field {f}: expression type {k:?} for a {ty:?} field", dd.table),
                            }
                        }
                    }
                }
                _ => bail!("host glue: delta {}: unexpected field path {path:?}", dd.table),
            }
        }
    }
}

fn raw_sql(sql: &str) -> Res<String> {
    if sql.contains(")vsql\"") {
        bail!("host glue: SQL contains the raw-string delimiter");
    }
    Ok(format!("R\"vsql({sql})vsql\""))
}

/// gen_glue.cpp: the query's host glue for the generic kit.
pub(crate) fn kit(m: &Model, prefix: &str, header: &str, accel_id: &str, workload: &str, query_id: u32) -> Res<String> {
    let g = Ffi { m, p: prefix.to_string() };
    let p = prefix;
    let ps = format!("{p}s");
    let (build, apply, exec) = (ob(m, "build"), ob(m, "apply"), ob(m, "execute"));
    let mut w = String::new();
    writeln!(w, "// GENERATED by {VERSION} from {} and inputs {}.  DO NOT EDIT: regenerate.", m.src_name, m.inputs_name).unwrap();
    writeln!(w, "//").unwrap();
    writeln!(w, "// Host glue of accelerator {accel_id} (TRUSTED GLUE): runs the declared scans,").unwrap();
    writeln!(w, "// reads captured deltas, and calls the kernel's C ABI ({header}) through the").unwrap();
    writeln!(w, "// generic kit (substrate/verus/kit/verus_kit.hpp).  Rules: sql2verus/src/glue.rs.").unwrap();
    writeln!(w, "#include <cstdint>\n#include <string>\n#include <vector>\n").unwrap();
    writeln!(w, "#include \"substrate/verus/kit/verus_kit.hpp\"\n#include \"{header}\"\n").unwrap();
    writeln!(w, "namespace {{\n\nnamespace kit = accelerator_codegen::verus_kit;\nnamespace core = accelerator_codegen::aux_state::core;\n").unwrap();

    // ── SQL texts ──
    for sd in &m.scans {
        writeln!(w, "// {} {}", if sd.key_out.is_some() { "rescan" } else { "scan" }, sd.name).unwrap();
        writeln!(w, "const char *const k{}Sql_{} = {};\n", if sd.key_out.is_some() { "Rescan" } else { "Scan" }, sd.name, raw_sql(&sd.sql)?).unwrap();
    }
    writeln!(w, "// the query itself (fallback)\nconst char *const kQuerySql = {};\n", raw_sql(&m.query_sql)?).unwrap();

    // ── build inputs ──
    let scan_of = |name: &str, rescan: bool| m.scans.iter().find(|s| s.name == name && s.key_out.is_some() == rescan);
    let build_rows: Vec<&P> = build.params.iter().filter(|x| matches!(x.arg, Arg::Rows(_))).collect();
    writeln!(w, "// The build's scans, in the C ABI's arrays.\nstruct BuildIn {{").unwrap();
    let mut fill = String::new();
    for prm in &build_rows {
        let sd = scan_of(&prm.name, false).ok_or_else(|| format!("host glue: no scan {}", prm.name))?;
        let slots = g.rows_slots(prm);
        let (mem, push) = g.kit_rows(&slots, &g.scan_leaf(sd))?;
        for x in mem {
            writeln!(w, "  {x}").unwrap();
        }
        writeln!(fill, "    h.Query(kScanSql_{}, [&](const core::AuxRow &r) {{", sd.name).unwrap();
        for x in push {
            writeln!(fill, "      {x}").unwrap();
        }
        writeln!(fill, "    }});").unwrap();
    }
    writeln!(w, "  void Fill(kit::Host &h) {{\n{fill}  }}\n}};\n").unwrap();
    let build_args = |obj: &str| -> Vec<String> { build_rows.iter().flat_map(|prm| Ffi::kit_args(&g.rows_slots(prm), obj)).collect() };

    // ── deltas ──
    let delta_rows: Vec<&P> = apply.params.iter().filter(|x| matches!(x.arg, Arg::Rows(_)) && x.name.ends_with("_delta")).collect();
    writeln!(w, "// One transaction's captured changes, in the C ABI's arrays.\nstruct DeltaIn {{").unwrap();
    let mut fill = String::new();
    for prm in &delta_rows {
        let ti = m.deltas.iter().position(|d| format!("{}_delta", table_field(&d.table)) == prm.name).ok_or_else(|| format!("host glue: no delta for {}", prm.name))?;
        let dd = &m.deltas[ti];
        let slots = g.rows_slots(prm);
        let (mem, push) = g.kit_rows(&slots, &g.delta_leaf(dd))?;
        for x in mem {
            writeln!(w, "  {x}").unwrap();
        }
        writeln!(fill, "    for (const auto &c : ch[{ti}]) {{").unwrap();
        for x in push {
            writeln!(fill, "      {x}").unwrap();
        }
        writeln!(fill, "    }}").unwrap();
    }
    writeln!(w, "  void Fill(const kit::Changes &ch) {{\n{fill}  }}\n}};\n").unwrap();
    let delta_args = |obj: &str| -> Vec<String> { delta_rows.iter().flat_map(|prm| Ffi::kit_args(&g.rows_slots(prm), obj)).collect() };

    // ── rescans ──
    let rescans: Vec<&ObFn> = m.fns.iter().filter(|f| f.name.starts_with("rescan_")).collect();
    let mut apply_pre = String::new();
    let mut rescan_args: Vec<String> = vec![];
    for rf in &rescans {
        let n = rf.name["rescan_".len()..rf.name.len() - "_keys".len()].to_string();
        let sd = scan_of(&n, true).ok_or_else(|| format!("host glue: no rescan {n}"))?;
        let key = rf.ret.trim_start_matches("(r: Vec<").trim_end_matches(">)").to_string();
        let text = key == "Vec<u8>";
        let rows_p = apply.params.iter().find(|x| x.name == format!("{n}_rows")).ok_or_else(|| format!("host glue: apply has no {n}_rows"))?;
        let slots = g.rows_slots(rows_p);
        let (mem, push) = g.kit_rows(&slots, &g.scan_leaf(sd))?;
        let keys_mem = if text { "std::vector<uint8_t> key_bytes;\n  std::vector<size_t> key_off;\n  size_t key_n = 0;".to_string() } else { format!("std::vector<{}> keys;", c_prim(&key)) };
        writeln!(w, "// Rescan {n}: the rows of the given keys, on the database after the transaction.\nstruct Rescan_{n} {{\n  {keys_mem}").unwrap();
        for x in mem {
            writeln!(w, "  {x}").unwrap();
        }
        writeln!(w, "  void Fill(kit::Host &h) {{").unwrap();
        if text {
            writeln!(w, "    std::vector<std::string> ks;\n    for (size_t i = 0; i < key_n; ++i)\n      ks.emplace_back(reinterpret_cast<const char *>(key_bytes.data()) + key_off[i], key_off[i + 1] - key_off[i]);").unwrap();
        } else {
            writeln!(w, "    std::vector<int64_t> ks(keys.begin(), keys.end());").unwrap();
        }
        writeln!(w, "    kit::QueryKeys(h, kRescanSql_{n}, \"{}\", ks, [&](const core::AuxRow &r) {{", sd.key_out.as_ref().unwrap()).unwrap();
        for x in push {
            writeln!(w, "      {x}").unwrap();
        }
        writeln!(w, "    }});\n  }}\n}};\n").unwrap();
        // the keys from the kernel, then the rows from the host
        let kargs = {
            let mut a = vec!["state".to_string()];
            a.extend(delta_args("d"));
            a.join(", ")
        };
        writeln!(apply_pre, "  Rescan_{n} r_{n};").unwrap();
        if text {
            let t = format!("r_{n}.key_bytes.data(), r_{n}.key_bytes.size(), r_{n}.key_off.data(), r_{n}.key_off.size(), &need");
            writeln!(apply_pre, "  r_{n}.key_bytes.resize(4096);\n  r_{n}.key_off.resize(1025);").unwrap();
            writeln!(apply_pre, "  {{\n    size_t need = 0;\n    const size_t k = {p}_{}({kargs}, {t});", rf.name).unwrap();
            writeln!(apply_pre, "    if (k + 1 > r_{n}.key_off.size() || need > r_{n}.key_bytes.size()) {{\n      r_{n}.key_bytes.resize(need);\n      r_{n}.key_off.resize(k + 1);\n      {p}_{}({kargs}, {t});\n    }}", rf.name).unwrap();
            writeln!(apply_pre, "    r_{n}.key_n = k;\n  }}\n  r_{n}.Fill(h);").unwrap();
            rescan_args.push(format!("r_{n}.key_n"));
            rescan_args.push(format!("r_{n}.key_bytes.data()"));
            rescan_args.push(format!("r_{n}.key_off.data()"));
        } else {
            writeln!(apply_pre, "  r_{n}.keys.resize(1024);").unwrap();
            writeln!(apply_pre, "  {{\n    const size_t k = {p}_{}({kargs}, r_{n}.keys.data(), r_{n}.keys.size());", rf.name).unwrap();
            writeln!(apply_pre, "    if (k > r_{n}.keys.size()) {{\n      r_{n}.keys.resize(k);\n      {p}_{}({kargs}, r_{n}.keys.data(), r_{n}.keys.size());\n    }}", rf.name).unwrap();
            writeln!(apply_pre, "    r_{n}.keys.resize(k);\n  }}\n  r_{n}.Fill(h);").unwrap();
            rescan_args.push(format!("r_{n}.keys.size()"));
            rescan_args.push(format!("r_{n}.keys.data()"));
        }
        rescan_args.extend(Ffi::kit_args(&slots, &format!("r_{n}")));
    }
    // apply's arguments in order: state, deltas, then per rescan (keys, rows)
    let apply_args = |state: &str| -> Res<Vec<String>> {
        let mut a = vec![];
        let mut ri = 0;
        for prm in &apply.params {
            match &prm.arg {
                Arg::State => a.push(state.to_string()),
                Arg::Ghost => {}
                Arg::Rows(_) if prm.name.ends_with("_delta") => a.extend(Ffi::kit_args(&g.rows_slots(prm), "d")),
                Arg::Rows(_) => {
                    let k = g.rows_slots(prm).len();
                    a.extend(rescan_args[ri..ri + k].iter().cloned());
                    ri += k;
                }
                _ => bail!("host glue: apply parameter {}", prm.name),
            }
        }
        Ok(a)
    };

    // ── kernel entry points ──
    let state_t = format!("{p}_state");
    writeln!(w, "void *Build(kit::Host &h) {{\n  BuildIn in;\n  in.Fill(h);\n  return {p}_build({});\n}}\n", build_args("in").join(", ")).unwrap();
    writeln!(w, "void FreeState(void *s) {{ {p}_free(static_cast<{state_t} *>(s)); }}\n").unwrap();
    let a_host = apply_args("state")?;
    let mut a_stage = vec![format!("static_cast<const {ps}_base *>(b)")];
    a_stage.extend(a_host.iter().filter(|x| x.as_str() != "state").cloned());
    writeln!(w, "void *Apply(const void *s, const kit::Changes &ch, kit::Host &h, int32_t *err) {{").unwrap();
    writeln!(w, "  const auto *state = static_cast<const {state_t} *>(s);\n  DeltaIn d;\n  d.Fill(ch);\n{apply_pre}  return {p}_apply({}, err);\n}}\n", a_host.join(", ")).unwrap();
    writeln!(w, "void *PrepareApply(const void *b, const kit::Changes &ch, kit::Host &h, int32_t *err) {{").unwrap();
    writeln!(w, "  [[maybe_unused]] const auto *state = {ps}_base_state(static_cast<const {ps}_base *>(b));\n  DeltaIn d;\n  d.Fill(ch);\n{apply_pre}  return {ps}_stage_apply({}, err);\n}}\n", a_stage.join(", ")).unwrap();
    let mut pb = vec![format!("static_cast<const {ps}_head *>(head_of())")];
    pb.extend(build_args("in"));
    writeln!(w, "void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &h) {{\n  BuildIn in;\n  in.Fill(h);\n  return {ps}_stage_build({});\n}}\n", pb.join(", ")).unwrap();

    // execute
    writeln!(w, "template <class Call>\nbool Run(const core::QueryArguments &a, std::vector<core::AuxRow> &out, Call call) {{").unwrap();
    let mut eargs: Vec<String> = vec![];
    for prm in &exec.params {
        let n: u32 = match prm.name.strip_prefix('p').and_then(|x| x.parse().ok()) {
            Some(n) => n,
            None => continue,
        };
        let ty = m.params.iter().find(|x| x.0 == n).map(|x| x.1).ok_or_else(|| format!("host glue: parameter ${n} has no type"))?;
        let k = n - 1;
        match (&prm.arg, ty) {
            (Arg::Text, _) => {
                writeln!(w, "  std::string p{n};\n  if (!kit::ArgText(a, {k}, p{n}))\n    return false;").unwrap();
                eargs.push(format!("reinterpret_cast<const uint8_t *>(p{n}.data())"));
                eargs.push(format!("p{n}.size()"));
            }
            (Arg::NullText, _) => {
                writeln!(w, "  std::string p{n};\n  const bool p{n}_null = kit::ArgIsNull(a, {k});\n  if (!p{n}_null && !kit::ArgText(a, {k}, p{n}))\n    return false;").unwrap();
                eargs.push(format!("p{n}_null ? nullptr : reinterpret_cast<const uint8_t *>(p{n}.data())"));
                eargs.push(format!("p{n}.size()"));
            }
            (Arg::NullScalar(rt), t) => {
                let (decl, conv) = match (*rt, t) {
                    ("i32", Ty::Date) => ("int32_t", format!("kit::ArgDate(a, {k}, p{n})")),
                    ("i64", Ty::Ts) => bail!("host glue: TIMESTAMP parameter ${n}"),
                    ("i64", t) => ("int64_t", format!("kit::ArgI64(a, {k}, {}, p{n})", if let Ty::Dec(s) = t { s } else { 0 })),
                    _ => bail!("host glue: parameter ${n} of type {ty:?}"),
                };
                writeln!(w, "  {decl} p{n} = 0;\n  const bool p{n}_null = kit::ArgIsNull(a, {k});\n  if (!p{n}_null && !{conv})\n    return false;").unwrap();
                eargs.push(format!("p{n}"));
                eargs.push(format!("static_cast<uint8_t>(p{n}_null)"));
            }
            (Arg::Scalar("i32"), Ty::Date) => {
                writeln!(w, "  int32_t p{n} = 0;\n  if (!kit::ArgDate(a, {k}, p{n}))\n    return false;").unwrap();
                eargs.push(format!("p{n}"));
            }
            (Arg::Scalar("i64"), t) => {
                let sc = if let Ty::Dec(s) = t { s } else { 0 };
                if t == Ty::Ts {
                    bail!("host glue: TIMESTAMP parameter ${n}");
                }
                writeln!(w, "  int64_t p{n} = 0;\n  if (!kit::ArgI64(a, {k}, {sc}, p{n}))\n    return false;").unwrap();
                eargs.push(format!("p{n}"));
            }
            _ => bail!("host glue: parameter ${n} of type {ty:?}"),
        }
    }
    writeln!(w, "  {p}_result *r = call({});\n  if (!r)\n    return false;", eargs.join(", ")).unwrap();
    writeln!(w, "  const size_t n = {p}_result_len(r);\n  out.reserve(out.size() + n);\n  for (size_t i = 0; i < n; ++i) {{\n    core::AuxRow row;").unwrap();
    for (k, (rt, _, ty)) in m.outs.iter().take(m.visible).enumerate() {
        let acc = format!("{p}_result_c{k}");
        let line = match *rt {
            "Vec<u8>" => format!("const uint8_t *b{k} = nullptr;\n      size_t l{k} = 0;\n      row.values.push_back({acc}(r, i, &b{k}, &l{k}) ? kit::OutText(b{k}, l{k}) : kit::OutNull(core::AuxType::String));"),
            "i64" if *ty == Ty::Ts => format!("int64_t v{k} = 0;\n      row.values.push_back({acc}(r, i, &v{k}) ? kit::OutTs(v{k}) : kit::OutNull(core::AuxType::String));"),
            "i64" => format!("int64_t v{k} = 0;\n      row.values.push_back({acc}(r, i, &v{k}) ? kit::OutI64(v{k}) : kit::OutNull(core::AuxType::Int64));"),
            "i32" => format!("int32_t v{k} = 0;\n      row.values.push_back({acc}(r, i, &v{k}) ? kit::OutDate(v{k}) : kit::OutNull(core::AuxType::Date));"),
            "u64" => format!("uint64_t v{k} = 0;\n      row.values.push_back({acc}(r, i, &v{k}) ? kit::OutF64(v{k}) : kit::OutNull(core::AuxType::Double));"),
            "bool" => format!("uint8_t v{k} = 0;\n      row.values.push_back({acc}(r, i, &v{k}) ? kit::OutBool(v{k}) : kit::OutNull(core::AuxType::Boolean));"),
            "i128" => {
                let sc = if let Ty::Dec(s) = ty { *s } else { 0 };
                format!("int64_t h{k} = 0;\n      uint64_t l{k} = 0;\n      row.values.push_back({acc}(r, i, &h{k}, &l{k}) ? kit::OutDec(h{k}, l{k}, {sc}) : kit::OutNull(core::AuxType::String));")
            }
            "(i128, i128)" => format!("int64_t nh{k} = 0, dh{k} = 0;\n      uint64_t nl{k} = 0, dl{k} = 0;\n      row.values.push_back({acc}(r, i, &nh{k}, &nl{k}, &dh{k}, &dl{k}) ? kit::OutRat(nh{k}, nl{k}, dh{k}, dl{k}) : kit::OutNull(core::AuxType::Double));"),
            other => bail!("host glue: result type {other}"),
        };
        writeln!(w, "    {{\n      {line}\n    }}").unwrap();
    }
    writeln!(w, "    out.push_back(std::move(row));\n  }}\n  {p}_result_free(r);\n  return true;\n}}\n").unwrap();
    writeln!(w, "bool Execute(const void *s, const core::QueryArguments &a, std::vector<core::AuxRow> &out) {{\n  return Run(a, out, [&](auto... x) {{ return {p}_execute(static_cast<const {state_t} *>(s), x...); }});\n}}\n").unwrap();
    writeln!(w, "bool Answer(const void *s, const core::QueryArguments &a, std::vector<core::AuxRow> &out) {{\n  return Run(a, out, [&](auto... x) {{ return {ps}_answer(static_cast<const {ps}_reading *>(s), x...); }});\n}}\n").unwrap();

    // ── registration ──
    let kind = |t: Ty| -> Res<&'static str> {
        Ok(match t {
            Ty::Text => "kit::Kind::Text",
            Ty::Date => "kit::Kind::Date",
            Ty::Int => "kit::Kind::Int",
            Ty::Dec(_) => "kit::Kind::Dec",
            Ty::Bool => "kit::Kind::Bool",
            other => bail!("host glue: parameter type {other:?}"),
        })
    };
    let tables: Vec<String> = m
        .deltas
        .iter()
        .map(|d| format!("{{{}, {{{}}}}}", cpp_str(&d.table), d.cols.iter().map(|c| cpp_str(&c.0)).collect::<Vec<_>>().join(", ")))
        .collect();
    let params: Vec<String> = m.params.iter().map(|(_, t)| kind(*t).map(|s| s.to_string())).collect::<Res<_>>()?;
    writeln!(w, "const kit::Glue kGlue = {{").unwrap();
    writeln!(w, "    {}, {}, {query_id}, kQuerySql,", cpp_str(accel_id), cpp_str(workload)).unwrap();
    writeln!(w, "    {{{}}},", tables.join(", ")).unwrap();
    writeln!(w, "    {{{}}},", params.join(", ")).unwrap();
    writeln!(w, "    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,").unwrap();
    writeln!(w, "    kit::StoreOps{{").unwrap();
    for (name, lam) in [
        ("new", format!("[]() -> void * {{ return {ps}_new(); }}")),
        ("free", format!("[](void *st) {{ {ps}_free(static_cast<{ps}_store *>(st)); }}")),
        ("newest_base", format!("[](const void *st) -> void * {{ return {ps}_newest_base(static_cast<const {ps}_store *>(st)); }}")),
        ("base_state", format!("[](const void *b) -> const void * {{ return {ps}_base_state(static_cast<const {ps}_base *>(b)); }}")),
        ("base_free", format!("[](void *b) {{ {ps}_base_free(static_cast<{ps}_base *>(b)); }}")),
        ("newest_head", format!("[](const void *st) -> void * {{ return {ps}_newest_head(static_cast<const {ps}_store *>(st)); }}")),
        ("head_free", format!("[](void *hd) {{ {ps}_head_free(static_cast<{ps}_head *>(hd)); }}")),
        ("stage_invalid", format!("[](const void *hd) -> void * {{ return {ps}_stage_invalid(static_cast<const {ps}_head *>(hd)); }}")),
        ("staged_free", format!("[](void *sg) {{ {ps}_staged_free(static_cast<{ps}_staged *>(sg)); }}")),
        ("begin", format!("[](const void *st, uint64_t from) -> int32_t {{ return {ps}_begin(static_cast<const {ps}_store *>(st), from); }}")),
        ("abort", format!("[](const void *st) {{ {ps}_abort(static_cast<const {ps}_store *>(st)); }}")),
        ("install", format!("[](const void *st, void *pr, uint64_t from, uint64_t floor, int32_t end) -> int32_t {{ return {ps}_install(static_cast<const {ps}_store *>(st), static_cast<{ps}_staged *>(pr), from, floor, end); }}")),
        ("read", format!("[](const void *st, uint64_t at, uint64_t tries, int32_t *status) -> const void * {{ return {ps}_read(static_cast<const {ps}_store *>(st), at, tries, status); }}")),
        ("reading_free", format!("[](const void *r) {{ {ps}_reading_free(const_cast<{ps}_reading *>(static_cast<const {ps}_reading *>(r))); }}")),
        ("invalidate_all", format!("[](const void *st) {{ {ps}_invalidate_all(static_cast<const {ps}_store *>(st)); }}")),
        ("version_count", format!("[](const void *st) -> size_t {{ return {ps}_version_count(static_cast<const {ps}_store *>(st)); }}")),
        ("heap_bytes", format!("[]() -> size_t {{ return {ps}_heap_bytes(); }}")),
        ("heap_peak_bytes", format!("[]() -> size_t {{ return {ps}_heap_peak_bytes(); }}")),
        ("heap_reset_peak", format!("[]() {{ {ps}_heap_reset_peak(); }}")),
        ("stage_adopt", format!("[](const void *hd, void *st) -> void * {{ return {ps}_stage_adopt(static_cast<const {ps}_head *>(hd), static_cast<{state_t} *>(st)); }}")),
    ] {
        writeln!(w, "        /* {name} */ {lam},").unwrap();
    }
    writeln!(w, "    }},\n}};\n\nconst kit::Registrar kRegistrar(&kGlue);\n\n}} // namespace").unwrap();
    Ok(w)
}

fn cpp_str(s: &str) -> String {
    format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""))
}
