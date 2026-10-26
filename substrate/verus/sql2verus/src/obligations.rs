// Kernel obligations (gen_obligations.rs): the theorem an aux-state kernel
// must prove about the query, and the contracts its host glue must meet.
//
//   sql2verus ... --obligations <gen_obligations.rs> [--inputs <q.inputs>]
//                 [--kernel-stub <kernel.rs>]
//
// The kernel's inputs are declared (by analysis) in an inputs file; without
// one, every table of the spec's Db is scanned whole and its changes are
// captured whole.  Inputs file: statements ending in `;`, `--` line comments.
//
//   scan <name> [AS <Type>]: SELECT <columns> FROM <table> [<alias>] [WHERE ...];
//   rescan <name> AS <Type> KEYS <column>: SELECT <columns> FROM <table> [<alias>] [WHERE ...];
//   delta <table> [AS <Type>] [OF <RowType>]: SELECT <expressions> FROM <table>;
//
// Rules (fixed; reviewed once):
//   * A scan reads one base table: plain columns (named by their SELECT
//     names), filtered by plain predicates and [NOT] EXISTS subqueries; no
//     query parameters, no aggregation, no joins.  It holds when its rows
//     equal the declared query's result as a bag: for every weight f, the
//     sum of f over the rows is the sum of f over the query's result.
//   * A rescan is a scan restricted to the rows whose KEYS column is one of
//     the given keys, evaluated on the database after the transaction.
//   * Every table of Db has one delta: the transaction's changes to it, as
//     signed rows.  A delta that selects exactly the columns the spec reads
//     (lossless) is read through its view (`OF` names its row type; `AS`
//     makes it a struct {row, sign}, else it is a (row, i64) pair).  A lossy
//     delta (fewer columns, or computed ones) is a struct `AS` its fields +
//     sign, and the full changed rows come as a ghost argument with a
//     projection relation.  The full deltas satisfy bag_step per table.
//   * Runtime types: integer -> i64, DECIMAL -> i64 (value * 10^s), DATE ->
//     i32 (days since 1970-01-01), TIMESTAMP -> i64 (ns), text -> Vec<u8>,
//     boolean -> u8 (nonzero = true), DOUBLE -> u64 (IEEE-754 bits, finite;
//     the spec value is verus_common::rel::f64_val).  Results: integer ->
//     i64, DECIMAL -> i128, AVG / fractions -> (i128, i128) as (num, den),
//     DOUBLE -> u64 bits, NULL-able -> Option.  Parameters: text -> &[u8],
//     else as for rows.
//   * A NULL-able column is a row field plus a u8 flag `<field>__null`
//     (nonzero: NULL, the value is then unspecified); its spec view is an
//     Option and NULL is inside the envelope.  A scan of one NULL-able
//     column, a NULL-able KEYS column, and a lossy delta over NULL-able
//     values are refused.
//   * A row type without text is #[repr(C)] + Copy and passed as a slice;
//     one with text is passed as an owned Vec (build) or &Vec (apply).
//   * The kernel provides crate::kernel::{State, build, execute, apply,
//     rescan_<name>_keys} with State::db(), and a spec fn rescan_<name>_wanted
//     (State, delta views) -> Seq<key>: the keys it asks for.  The host passes
//     back exactly those keys; it calls only the ob_* functions, whose
//     contracts are fixed here.
//   * `Model` describes the ob_* functions for the generated glue (glue.rs):
//     the store adapter and the C ABI are derived from it, never written.
use super::*;

#[derive(Clone, Copy, PartialEq, Debug)]
enum Kind {
    Scan,
    Rescan,
    Delta,
}

#[derive(Clone, Debug)]
struct Decl {
    kind: Kind,
    name: String,
    ty: Option<String>,
    of: Option<String>,
    keys: Option<String>,
    sql: String,
}

fn parse_inputs(text: &str) -> Res<Vec<Decl>> {
    let body: String = text.lines().filter(|l| !l.trim_start().starts_with("--")).collect::<Vec<_>>().join("\n");
    let mut out = vec![];
    for stmt in body.split(';') {
        let stmt = stmt.trim();
        if stmt.is_empty() {
            continue;
        }
        let (head, sql) = stmt.split_once(':').ok_or_else(|| format!("inputs: missing ':' in `{}`", stmt.lines().next().unwrap_or("")))?;
        let toks: Vec<&str> = head.split_whitespace().collect();
        let kind = match toks.first().copied() {
            Some("scan") => Kind::Scan,
            Some("rescan") => Kind::Rescan,
            Some("delta") => Kind::Delta,
            _ => bail!("inputs: `{head}`: expected scan / rescan / delta"),
        };
        let mut name = toks.get(1).ok_or_else(|| format!("inputs: `{head}`: missing name"))?.to_string();
        if kind == Kind::Delta {
            // a table name, as SQL names it: quoted keeps its case, unquoted is lower-cased
            name = match name.strip_prefix('"').and_then(|x| x.strip_suffix('"')) {
                Some(x) => x.replace("\"\"", "\""),
                None => name.to_lowercase(),
            };
        }
        let (mut ty, mut of, mut keys) = (None, None, None);
        let mut i = 2;
        while i < toks.len() {
            let v = toks.get(i + 1).ok_or_else(|| format!("inputs: `{head}`: {} needs a value", toks[i]))?.to_string();
            match toks[i].to_ascii_uppercase().as_str() {
                "AS" => ty = Some(v),
                "OF" => of = Some(v),
                "KEYS" => keys = Some(v),
                other => bail!("inputs: `{head}`: unexpected {other}"),
            }
            i += 2;
        }
        let ok_ident = |s: &str| s.chars().next().map_or(false, |c| c.is_ascii_alphabetic()) && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        let checked = if kind == Kind::Delta { None } else { Some(&name) };
        for s in [checked, ty.as_ref(), of.as_ref()].into_iter().flatten() {
            if !ok_ident(s) {
                bail!("inputs: `{head}`: {s} is not an identifier");
            }
        }
        match kind {
            Kind::Scan if of.is_some() || keys.is_some() => bail!("inputs: scan {name}: OF / KEYS are for delta / rescan"),
            Kind::Rescan if ty.is_none() || keys.is_none() || of.is_some() => bail!("inputs: rescan {name}: needs AS <Type> KEYS <column>"),
            Kind::Delta if keys.is_some() => bail!("inputs: delta {name}: KEYS is for rescan"),
            _ => {}
        }
        out.push(Decl { kind, name, ty, of, keys, sql: sql.trim().to_string() });
    }
    Ok(out)
}

/// The inputs used when analysis declares none: every table whole.
fn default_inputs(tr: &Tr) -> Res<String> {
    let mut w = String::new();
    writeln!(w, "-- default: every table of Db scanned whole, its changes captured whole").unwrap();
    for (t, cs) in &tr.used {
        let cols: Vec<&String> = tr.sch.ddl_cols[t].iter().filter(|c| cs.contains(*c)).collect();
        if cols.is_empty() {
            bail!("table {t}: the query reads none of its columns (an empty row type)");
        }
        // identifiers quoted as the schema names them (DataFusion folds unquoted ones)
        let q = |x: &str| if x.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_') { x.to_string() } else { format!("\"{}\"", x.replace('"', "\"\"")) };
        let list = cols.iter().map(|c| format!("{} AS {}", q(c), sanitize(c))).collect::<Vec<_>>().join(", ");
        let base = camel(&sanitize(t));
        writeln!(w, "scan {} AS {base}Rt: SELECT {list} FROM {};", sanitize(t), q(t)).unwrap();
        writeln!(w, "delta {} AS {base}Delta OF {base}Rt: SELECT {list} FROM {};", q(t), q(t)).unwrap();
    }
    Ok(w)
}

pub(crate) fn snake(s: &str) -> String {
    let mut o = String::new();
    for (i, c) in s.chars().enumerate() {
        if c.is_ascii_uppercase() {
            if i > 0 && !o.ends_with('_') {
                o.push('_');
            }
            o.push(c.to_ascii_lowercase());
        } else {
            o.push(c);
        }
    }
    o
}

fn rt_ty(t: Ty) -> Res<&'static str> {
    Ok(match t {
        Ty::Int | Ty::Dec(_) | Ty::Ts => "i64",
        Ty::Date => "i32",
        Ty::Text => "Vec<u8>",
        Ty::Bool => "u8",
        Ty::Flt(1) => "u64",
        other => bail!("no runtime type for {other:?} in a scan / delta row"),
    })
}

/// The spec value of runtime value `x` of type `t`.
fn rt_view(x: &str, t: Ty) -> String {
    match t {
        Ty::Text => format!("{x}@"),
        Ty::Bool => format!("({x} != 0)"),
        Ty::Flt(_) => format!("f64_val({x})"),
        _ => format!("{x} as int"),
    }
}

fn copyable(fields: &[RtField]) -> bool {
    fields.iter().all(|f| f.ty != Ty::Text)
}

#[derive(Clone, Debug, PartialEq)]
struct RtField {
    name: String,
    col: String, // SQL column ("" when computed)
    ty: Ty,
    null: bool, // NULL-able: a flag field (null_flag) says whether it is NULL
}

/// The flag field of NULL-able field `f`: nonzero when the value is NULL.
pub(crate) fn null_flag(f: &str) -> String {
    format!("{f}__null")
}

impl RtField {
    /// The spec value of this field on row `r`.
    fn view(&self, r: &str) -> String {
        let x = format!("{r}.{}", self.name);
        let v = rt_view(&x, self.ty);
        if self.null { format!("(if {r}.{} != 0 {{ None }} else {{ Some({v}) }})", null_flag(&self.name)) } else { v }
    }

    fn spec_ty(&self) -> String {
        if self.null { format!("Option<{}>", verus_ty(self.ty)) } else { verus_ty(self.ty).to_string() }
    }
}

/// A runtime row struct and what it is a view of.
#[derive(Clone, Debug)]
struct RtStruct {
    table: String,
    fields: Vec<RtField>,
    used_by: Vec<String>,
}

/// One translated scan / delta SELECT.
struct Sel {
    table: String,
    var: String,
    conds: Vec<String>,
    outs: Vec<(String, Option<String>, Val)>, // (SELECT name, column if plain, value)
    exprs: Vec<Expr>,                          // the SELECT expressions, aliases stripped
    doc: String,
}

/// A row of a scan / delta argument, as the glue marshals it.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Elem {
    /// A runtime primitive: i64 / i32 / u8 / Vec<u8>.
    Prim(&'static str),
    /// A row struct (or a delta struct).
    Struct(String),
    /// A (row struct, sign) pair.
    Pair(String),
}

/// What an obligation function's parameter is, for the glue.
#[derive(Clone, Debug)]
pub(crate) enum Arg {
    /// The kernel state (&State).
    State,
    /// A ghost argument (erased).
    Ghost,
    /// A sequence of rows: a slice (&[T]), an owned Vec (build) or a &Vec.
    Rows(Elem),
    /// A text query parameter (&[u8]).
    Text,
    /// A scalar query parameter (i64 / i32).
    Scalar(&'static str),
}

/// A parameter of an obligation function.
pub(crate) struct P {
    pub(crate) name: String,
    pub(crate) ty: String,
    pub(crate) ghost: bool,
    pub(crate) arg: Arg,
}

/// Database placeholders in an obligation's requires: the state's database,
/// the database a build is for, and the database after a transaction.
pub(crate) const SDB: &str = "$SDB";
pub(crate) const DB: &str = "$DB";
pub(crate) const DB2: &str = "$DB2";
pub(crate) const ST: &str = "$S";

pub(crate) struct ObFn {
    pub(crate) doc: String,
    pub(crate) name: String,
    pub(crate) params: Vec<P>,
    pub(crate) ret: String,
    pub(crate) requires: Vec<String>,
    pub(crate) ensures: Vec<String>,
}

/// A runtime struct of the C ABI: fields in order, each a primitive or a
/// struct; `copy`: #[repr(C)] (no text), passed as an array.
pub(crate) struct CStruct {
    pub(crate) name: String,
    pub(crate) fields: Vec<(String, Elem)>,
    /// The SQL type of each primitive field (a struct field: unused).
    pub(crate) tys: Vec<Ty>,
    pub(crate) copy: bool,
}

impl CStruct {
    pub(crate) fn is_flag(&self, f: &str) -> bool {
        self.fields.iter().any(|(g, _)| null_flag(g) == f)
    }

    /// The SELECT column of field `f` (of its NULL flag, if `f` is one), and whether it is NULL-able.
    pub(crate) fn cell_of(&self, f: &str) -> Option<(usize, bool)> {
        let value = if self.is_flag(f) { f.strip_suffix("__null")? } else { f };
        let cell = self.fields.iter().filter(|(g, _)| !self.is_flag(g)).position(|(g, _)| g == value)?;
        Some((cell, self.fields.iter().any(|(g, _)| *g == null_flag(value))))
    }
}

/// A declared scan or rescan, as the host glue runs it.
pub(crate) struct ScanDecl {
    pub(crate) name: String,
    pub(crate) sql: String,
    pub(crate) el: Elem,
    /// A primitive row's SQL type.
    pub(crate) ty: Ty,
    /// Rescan: the SELECT name of the KEYS column.
    pub(crate) key_out: Option<String>,
}

/// Where a delta field's value comes from, on a captured row of its table.
pub(crate) enum FieldSrc {
    /// A captured column (index into `DeltaDecl::cols`), and whether it may be NULL.
    Col(usize, bool),
    /// Whether a captured column is NULL (the flag of a NULL-able `Col`).
    NullOf(usize),
    /// A computed (lossy) field: its SQL expression over the captured columns.
    Expr(Expr),
}

/// One table's delta: the captured columns and how each delta field reads them.
pub(crate) struct DeltaDecl {
    pub(crate) table: String,
    /// Captured columns (SQL names, as the schema names them) and their types.
    pub(crate) cols: Vec<(String, Ty)>,
    /// The fields of the delta's row (or lossy struct), in declaration order.
    pub(crate) fields: Vec<(String, FieldSrc, Ty)>,
}

/// The generated obligations, as the glue (glue.rs) needs them.
pub(crate) struct Model {
    pub(crate) fns: Vec<ObFn>,
    /// Definition order: a struct's struct fields come before it.
    pub(crate) structs: Vec<CStruct>,
    /// Result columns: runtime type, nullable, SQL type.
    pub(crate) outs: Vec<(&'static str, bool, Ty)>,
    /// Query parameters ($n) and their SQL types.
    pub(crate) params: Vec<(u32, Ty)>,
    pub(crate) scans: Vec<ScanDecl>,
    pub(crate) deltas: Vec<DeltaDecl>,
    /// The query's SQL (comments stripped), for the host's fallback.
    pub(crate) query_sql: String,
    pub(crate) src_name: String,
    pub(crate) inputs_name: String,
}

/// `requires` with its database placeholders instantiated.
pub(crate) fn inst(r: &str, st: &str, sdb: &str, db: &str, db2: &str) -> String {
    r.replace(SDB, sdb).replace(DB2, db2).replace(DB, db).replace(ST, st)
}

impl ObFn {
    pub(crate) fn sig(&self, prefix: &str) -> String {
        let ps: Vec<String> = self
            .params
            .iter()
            .map(|p| if p.ghost { format!("Ghost({}): Ghost<{}>", p.name, p.ty) } else { format!("{}: {}", p.name, p.ty) })
            .collect();
        let mut w = String::new();
        writeln!(w, "pub fn {prefix}{}(\n    {},\n) -> {}", self.name, ps.join(",\n    "), self.ret).unwrap();
        if !self.requires.is_empty() {
            writeln!(w, "    requires").unwrap();
            for r in &self.requires {
                writeln!(w, "        {},", inst(r, "*s", "s.db()", "db", "db2")).unwrap();
            }
        }
        if !self.ensures.is_empty() {
            writeln!(w, "    ensures").unwrap();
            for e in &self.ensures {
                writeln!(w, "        {},", inst(e, "*s", "s.db()", "db", "db2")).unwrap();
            }
        }
        w
    }
    pub(crate) fn args(&self) -> String {
        self.params.iter().map(|p| if p.ghost { format!("Ghost({})", p.name) } else { p.name.clone() }).collect::<Vec<_>>().join(", ")
    }
}

struct Ob<'t, 'a> {
    tr: &'t mut Tr<'a>,
    structs: BTreeMap<String, RtStruct>,
    order: Vec<String>, // struct definition order
    items: Vec<String>,
    prim_views: BTreeSet<&'static str>,
}

impl<'t, 'a> Ob<'t, 'a> {
    /// Translate `SELECT ... FROM <table> [alias] [WHERE ...]`.
    async fn select(&mut self, ctx: &SessionContext, what: &str, sql: &str, allow_computed: bool) -> Res<Sel> {
        let plan = ctx.state().create_logical_plan(sql).await.map_err(|e| format!("{what}: DataFusion: {e}"))?;
        let LogicalPlan::Projection(pr) = &plan else { bail!("{what}: expected SELECT <columns> FROM <table> [WHERE ...]") };
        let mut p = pr.input.as_ref();
        let mut preds = vec![];
        if let LogicalPlan::Filter(f) = p {
            preds = split_and(&f.predicate);
            p = &f.input;
        }
        let used0 = self.tr.used.clone();
        let params0 = self.tr.params.clone();
        self.tr.frames.clear();
        self.tr.frames.push(Frame::default());
        // each scan / delta is its own scope: variable names restart
        self.tr.vars.clear();
        let r: Res<Sel> = (|| {
            let mut bs = vec![];
            let mut on = vec![];
            self.tr.from_tree(p, &mut bs, &mut on)?;
            if bs.len() != 1 || !on.is_empty() || bs[0].optional {
                bail!("{what}: a scan / delta reads exactly one table (no joins)");
            }
            let Src::Base(table) = bs[0].src.clone() else { bail!("{what}: reads a view or derived table; scans read base tables") };
            self.tr.frame().binders = bs.clone();
            let mut conds = vec![];
            for e in &preds {
                let v = self.tr.expr(e, &Ctx::Row { tuple: None })?;
                if v.ty != Ty::Bool || v.nonlit_div || v.guard.is_some() {
                    bail!("{what}: WHERE conjunct {e}");
                }
                conds.push(bool_code(&v));
            }
            let mut outs = vec![];
            for (i, e) in pr.expr.iter().enumerate() {
                let sql_name = pr.schema.field(i).name().clone();
                let col = match strip_alias(e) {
                    Expr::Column(c) => Some(c.name.clone()),
                    _ if allow_computed => None,
                    _ => bail!("{what}: SELECT {e}: a scan returns columns; computing is the kernel's job"),
                };
                let v = self.tr.expr(e, &Ctx::Row { tuple: None })?;
                if v.ty == Ty::Rat || v.approx.is_some() || (v.guard.is_some() && col.is_none()) || v.nonlit_div || !v.den_conds.is_empty() {
                    bail!("{what}: SELECT {e}: not an exact row value");
                }
                outs.push((sql_name, col, v));
            }
            let exprs = pr.expr.iter().map(|e| strip_alias(e).clone()).collect();
            Ok(Sel { table, var: bs[0].var.clone(), conds, outs, exprs, doc: sql.split_whitespace().collect::<Vec<_>>().join(" ") })
        })();
        let f = self.tr.frames.pop().unwrap();
        let sel = r?;
        if !f.outer.is_empty() || f.uses_params || self.tr.params != params0 || sel.conds.iter().any(|c| c.contains("params")) {
            bail!("{what}: uses query parameters or an unsupported subquery (scan filters allow plain predicates and [NOT] EXISTS)");
        }
        for (t, cs) in &self.tr.used {
            let before = used0.get(t);
            for c in cs {
                if !before.map_or(false, |b| b.contains(c)) {
                    bail!("{what}: reads {t}.{c}, which the query does not read (not in Db)");
                }
            }
            if before.is_none() {
                bail!("{what}: reads table {t}, which the query does not read (not in Db)");
            }
        }
        Ok(sel)
    }

    /// The Db columns of a table, in Db order.
    fn db_cols(&self, t: &str) -> Vec<String> {
        self.tr.sch.ddl_cols[t].iter().filter(|c| self.tr.used[t].contains(*c)).cloned().collect()
    }

    /// The fields of a struct from a SELECT of plain columns.
    fn fields_of(&self, what: &str, sel: &Sel) -> Res<Vec<RtField>> {
        let mut fs: Vec<RtField> = vec![];
        for (n, col, v) in &sel.outs {
            let Some(col) = col else { bail!("{what}: computed column {n} in a row type") };
            let name = sanitize(n);
            if fs.iter().any(|f| f.name == name) {
                bail!("{what}: two columns named {name}");
            }
            rt_ty(v.ty).map_err(|e| format!("{what}: {e}"))?;
            fs.push(RtField { name, col: col.clone(), ty: v.ty, null: v.guard.is_some() });
        }
        Ok(fs)
    }

    fn register(&mut self, what: &str, name: &str, table: &str, fields: Vec<RtField>) -> Res<()> {
        match self.structs.get_mut(name) {
            Some(s) => {
                if s.table != table || s.fields != fields {
                    bail!("{what}: {name} was declared with different columns by {}", s.used_by.join(", "));
                }
                s.used_by.push(what.into());
            }
            None => {
                self.structs.insert(name.into(), RtStruct { table: table.into(), fields, used_by: vec![what.into()] });
                self.order.push(name.into());
            }
        }
        Ok(())
    }

    /// Does struct `name` cover exactly the table's Db columns?
    fn full(&self, name: &str) -> bool {
        let s = &self.structs[name];
        let mut a: Vec<&String> = s.fields.iter().map(|f| &f.col).collect();
        let mut b = self.db_cols(&s.table);
        a.sort();
        b.sort();
        a.len() == b.len() && a.iter().zip(&b).all(|(x, y)| *x == y)
    }

    /// The spec type a struct is a view of.
    fn spec_of(&self, name: &str) -> String {
        let s = &self.structs[name];
        if self.full(name) {
            table_row(&s.table)
        } else if s.fields.len() == 1 {
            s.fields[0].spec_ty()
        } else {
            format!("({})", s.fields.iter().map(|f| f.spec_ty()).collect::<Vec<_>>().join(", "))
        }
    }

    /// The spec value of a table row `v` projected as struct `name` is.
    fn proj_of(&self, name: &str, v: &str) -> String {
        let s = &self.structs[name];
        if self.full(name) {
            v.to_string()
        } else if s.fields.len() == 1 {
            format!("{v}.{}", sanitize(&s.fields[0].col))
        } else {
            format!("({})", s.fields.iter().map(|f| format!("{v}.{}", sanitize(&f.col))).collect::<Vec<_>>().join(", "))
        }
    }

    fn view(&self, name: &str) -> String {
        format!("{}_view", snake(name))
    }

    fn seq_view(&self, name: &str) -> String {
        format!("{}_seq_view", snake(name))
    }

    fn prim_seq_view(&mut self, t: Ty) -> Res<String> {
        let rt = rt_ty(t)?;
        self.prim_views.insert(rt);
        Ok(format!("seq_view_{}", prim_tag(rt)))
    }
}

/// Rows a table can hold: the DBMS numbers rows with 64-bit row ids.
const MAX_ROWS: i128 = 1 << 63;

/// Changed rows a state may absorb after its build before apply may decline.
const MAX_HISTORY: i128 = 1 << 62;

fn range_of(x: &str, lo: i128, hi: i128) -> String {
    format!("{lo} <= {x} <= {hi}")
}

fn prim_tag(rt: &str) -> &'static str {
    match rt {
        "i64" => "i64",
        "i32" => "i32",
        "u8" => "u8",
        "u64" => "u64",
        _ => "bytes",
    }
}

fn prim_spec(rt: &str) -> &'static str {
    match rt {
        "Vec<u8>" => "Seq<u8>",
        "u8" => "bool",
        _ => "int",
    }
}

fn prim_ty_of(rt: &str) -> Ty {
    match rt {
        "Vec<u8>" => Ty::Text,
        "u8" => Ty::Bool,
        "u64" => Ty::Flt(1),
        _ => Ty::Int,
    }
}

/// How a build / apply argument of type `elem` is passed, and its sequence.
fn pass(elem: &str, copy: bool, build: bool) -> (String, String) {
    if copy {
        (format!("&[{elem}]"), "@".into())
    } else if build {
        (format!("Vec<{elem}>"), "@".into())
    } else {
        (format!("&Vec<{elem}>"), "@".into())
    }
}

/// gen_obligations.rs (and the kernel stub) for a translated query.
pub(crate) async fn obligations(
    tr: &mut Tr<'_>,
    top: &Top,
    ctx: &SessionContext,
    spec_text: &str,
    inputs: Option<(&str, String)>,
    src_name: &str,
    query_sql: &str,
) -> Res<(String, String, Model)> {
    let query_sql = query_sql.to_string();
    let (inputs_name, inputs_text) = match inputs {
        Some((n, t)) => (n.to_string(), t),
        None => ("(default)".to_string(), default_inputs(tr)?),
    };
    let decls = parse_inputs(&inputs_text)?;
    let tables: Vec<String> = tr.used.keys().cloned().collect();
    let mut ob = Ob { tr, structs: BTreeMap::new(), order: vec![], items: vec![], prim_views: BTreeSet::new() };

    // ── scans and rescans ──
    struct ScanOut {
        name: String,
        rescan: bool,
        contract: String,
        elem: String,
        el: Elem,
        copy: bool,
        key_rt: Option<&'static str>,
    }
    let mut scans: Vec<ScanOut> = vec![];
    let mut scan_decls: Vec<ScanDecl> = vec![];
    let mut seen_names: BTreeSet<String> = BTreeSet::new();
    for d in decls.iter().filter(|d| d.kind != Kind::Delta) {
        let rescan = d.kind == Kind::Rescan;
        let what = format!("{} {}", if rescan { "rescan" } else { "scan" }, d.name);
        let nm = sanitize(&d.name);
        if !seen_names.insert(format!("{}{nm}", if rescan { "r_" } else { "s_" })) {
            bail!("{what}: declared twice");
        }
        ob.tr.pfx = format!("{}_{nm}_", if rescan { "rescan" } else { "scan" });
        ob.tr.counters.clear();
        let items0 = ob.tr.items.len();
        let sel = ob.select(ctx, &what, &d.sql, false).await?;
        ob.items.extend(ob.tr.items.drain(items0..));
        let (elem, copy, spec_ty, proj) = match &d.ty {
            Some(t) => {
                let fs = ob.fields_of(&what, &sel)?;
                ob.register(&what, t, &sel.table, fs)?;
                let copy = copyable(&ob.structs[t].fields);
                (t.clone(), copy, ob.spec_of(t), ob.proj_of(t, &sel.var))
            }
            None => {
                if sel.outs.len() != 1 {
                    bail!("{what}: several columns need AS <Type>");
                }
                let v = &sel.outs[0].2;
                if sel.outs[0].1.is_none() {
                    bail!("{what}: computed column");
                }
                if v.guard.is_some() {
                    bail!("{what}: a NULL-able column needs AS <Type> (the row carries its NULL flag)");
                }
                let rt = rt_ty(v.ty).map_err(|e| format!("{what}: {e}"))?;
                (rt.to_string(), rt != "Vec<u8>", verus_ty(v.ty).to_string(), v.code.clone())
            }
        };
        let sv = if d.ty.is_some() { ob.seq_view(&elem) } else { ob.prim_seq_view(sel.outs[0].2.ty)? };
        let mut conds = sel.conds.clone();
        let mut key_rt = None;
        if let Some(k) = &d.keys {
            // the key column, read on the scanned table's row
            let ty = ob.tr.sch.col_ty.get(&(sel.table.clone(), k.clone())).and_then(|x| x.0).ok_or_else(|| format!("{what}: KEYS {k} is not a column of {}", sel.table))?;
            if !ob.tr.used[&sel.table].contains(k) {
                bail!("{what}: KEYS {k} is not read by the query (not in Db)");
            }
            if ty == Ty::Text {
                bail!("{what}: KEYS {k}: text keys are not supported");
            }
            if ob.tr.sch.col_ty[&(sel.table.clone(), k.clone())].1 {
                bail!("{what}: KEYS {k} is NULL-able");
            }
            let rt = rt_ty(ty)?;
            key_rt = Some(rt);
            let kv = ob.prim_seq_view(ty)?;
            conds.push(format!("{kv}(keys).contains({}.{})", sel.var, sanitize(k)));
        }
        let trow = table_row(&sel.table);
        let rhs = if conds.is_empty() && proj == sel.var {
            format!("sum(db.{}, f)", table_field(&sel.table))
        } else {
            let c = if conds.is_empty() {
                "true".to_string()
            } else if conds.len() == 1 {
                conds[0].clone()
            } else {
                conds.iter().map(|c| format!("({c})")).collect::<Vec<_>>().join(" && ")
            };
            format!("sum(\n        db.{},\n        |{}: {trow}| if {c} {{ f({proj}) }} else {{ 0 }},\n    )", table_field(&sel.table), sel.var)
        };
        let fname = format!("{}_{nm}_ok", if rescan { "rescan" } else { "scan" });
        let kp = match key_rt {
            Some(rt) => format!(", keys: Seq<{rt}>"),
            None => String::new(),
        };
        let mut w = String::new();
        writeln!(w, "/// {what}: {}", sel.doc).unwrap();
        if rescan {
            writeln!(w, "///   restricted to the rows whose {} is one of `keys`.", d.keys.as_ref().unwrap()).unwrap();
        }
        writeln!(w, "pub open spec fn {fname}(db: Db{kp}, rows: Seq<{elem}>) -> bool {{").unwrap();
        writeln!(w, "    forall|f: spec_fn({spec_ty}) -> int| #[trigger] sum({sv}(rows), f) == {rhs}").unwrap();
        writeln!(w, "}}\n").unwrap();
        ob.items.push(w);
        let el = if d.ty.is_some() { Elem::Struct(elem.clone()) } else { Elem::Prim(rt_ty(sel.outs[0].2.ty)?) };
        let key_out = match &d.keys {
            Some(k) => Some(
                sel.outs
                    .iter()
                    .find(|o| o.1.as_deref() == Some(k.as_str()))
                    .map(|o| o.0.clone())
                    .ok_or_else(|| format!("{what}: the KEYS column {k} must be one of the SELECT columns"))?,
            ),
            None => None,
        };
        scan_decls.push(ScanDecl { name: nm.clone(), sql: d.sql.clone(), el: el.clone(), ty: sel.outs[0].2.ty, key_out });
        scans.push(ScanOut { name: nm, rescan, contract: fname, elem, el, copy, key_rt });
    }
    ob.tr.pfx.clear();

    // ── deltas ──
    struct DeltaOut {
        table: String,
        elem: String,
        copy: bool,
        lossy: bool,
        view: String, // lossless: seq view to Seq<(TRow, int)>
        el: Elem,
    }
    let mut deltas: BTreeMap<String, DeltaOut> = BTreeMap::new();
    let mut delta_structs: Vec<CStruct> = vec![];
    let mut delta_decls: BTreeMap<String, DeltaDecl> = BTreeMap::new();
    for d in decls.iter().filter(|d| d.kind == Kind::Delta) {
        let what = format!("delta {}", d.name);
        if !ob.tr.used.contains_key(&d.name) {
            bail!("{what}: {} is not a table the query reads", d.name);
        }
        if deltas.contains_key(&d.name) {
            bail!("{what}: declared twice");
        }
        let sel = ob.select(ctx, &what, &d.sql, true).await?;
        if sel.table != d.name {
            bail!("{what}: SELECTs from {}", sel.table);
        }
        if !sel.conds.is_empty() {
            bail!("{what}: a delta has no WHERE (every change is captured)");
        }
        let plain = sel.outs.iter().all(|o| o.1.is_some());
        let mut cols: Vec<String> = sel.outs.iter().filter_map(|o| o.1.clone()).collect();
        cols.sort();
        let mut dbc = ob.db_cols(&d.name);
        dbc.sort();
        let lossless = plain && cols == dbc;
        let trow = table_row(&d.name);
        if lossless {
            let r = d.of.clone().ok_or_else(|| format!("{what}: a lossless delta names its row type: OF <RowType>"))?;
            let fs = ob.fields_of(&what, &sel)?;
            ob.register(&what, &r, &d.name, fs)?;
            let copy = copyable(&ob.structs[&r].fields);
            let rv = ob.view(&r);
            let (elem, view) = match &d.ty {
                Some(dt) => {
                    let sv = format!("{}_seq_view", snake(dt));
                    let mut w = String::new();
                    writeln!(w, "/// {what}: a changed `{}` row and its sign (+1 insert, -1 delete).", d.name).unwrap();
                    if copy {
                        writeln!(w, "#[repr(C)]\n#[derive(Clone, Copy)]").unwrap();
                    }
                    writeln!(w, "pub struct {dt} {{\n    pub row: {r},\n    pub sign: i64,\n}}\n").unwrap();
                    writeln!(w, "pub open spec fn {sv}(s: Seq<{dt}>) -> Seq<({trow}, int)> {{").unwrap();
                    writeln!(w, "    s.map_values(|d: {dt}| ({rv}(d.row), d.sign as int))").unwrap();
                    writeln!(w, "}}\n").unwrap();
                    ob.items.push(w);
                    delta_structs.push(CStruct {
                        name: dt.clone(),
                        fields: vec![("row".into(), Elem::Struct(r.clone())), ("sign".into(), Elem::Prim("i64"))],
                        tys: vec![Ty::Int, Ty::Int],
                        copy,
                    });
                    (dt.clone(), sv)
                }
                None => {
                    let sv = format!("{}_delta_seq_view", snake(&r));
                    let mut w = String::new();
                    writeln!(w, "/// {what}: (changed `{}` row, sign +1 insert / -1 delete).", d.name).unwrap();
                    writeln!(w, "pub open spec fn {sv}(s: Seq<({r}, i64)>) -> Seq<({trow}, int)> {{").unwrap();
                    writeln!(w, "    s.map_values(|d: ({r}, i64)| ({rv}(d.0), d.1 as int))").unwrap();
                    writeln!(w, "}}\n").unwrap();
                    ob.items.push(w);
                    (format!("({r}, i64)"), sv)
                }
            };
            let el = if d.ty.is_some() { Elem::Struct(elem.clone()) } else { Elem::Pair(r.clone()) };
            let rs = &ob.structs[&r];
            delta_decls.insert(
                d.name.clone(),
                DeltaDecl {
                    table: d.name.clone(),
                    cols: rs.fields.iter().map(|f| (f.col.clone(), f.ty)).collect(),
                    fields: rs
                        .fields
                        .iter()
                        .enumerate()
                        .flat_map(|(i, f)| {
                            let flag = f.null.then(|| (null_flag(&f.name), FieldSrc::NullOf(i), Ty::Bool));
                            [(f.name.clone(), FieldSrc::Col(i, f.null), f.ty)].into_iter().chain(flag)
                        })
                        .collect(),
                },
            );
            deltas.insert(d.name.clone(), DeltaOut { table: d.name.clone(), elem, copy, lossy: false, view, el });
        } else {
            if d.of.is_some() {
                bail!("{what}: OF is for a lossless delta (exactly the columns the spec reads)");
            }
            let dt = d.ty.clone().ok_or_else(|| format!("{what}: a lossy delta needs AS <Type>"))?;
            let mut fields = vec![];
            let mut rel = vec![];
            for (n, col, v) in &sel.outs {
                let f = sanitize(n);
                if f == "sign" || fields.iter().any(|(x, _): &(String, &str)| *x == f) {
                    bail!("{what}: field name {f}");
                }
                if v.guard.is_some() {
                    bail!("{what}: field {f} is NULL-able; a lossy delta carries NOT NULL values");
                }
                let rt = rt_ty(v.ty).map_err(|e| format!("{what}: {e}"))?;
                let lhs = rt_view(&format!("d[i].{f}"), v.ty);
                let _ = col;
                let rhs = v.wrap(1);
                rel.push(format!("{lhs} == {rhs}"));
                fields.push((f, rt));
            }
            let copy = fields.iter().all(|(_, rt)| *rt != "Vec<u8>");
            if ob.structs.contains_key(&dt) {
                bail!("{what}: {dt} is also a row type");
            }
            let mut w = String::new();
            writeln!(w, "/// {what} (lossy): {}", sel.doc).unwrap();
            if copy {
                writeln!(w, "#[repr(C)]\n#[derive(Clone, Copy)]").unwrap();
            }
            writeln!(w, "pub struct {dt} {{").unwrap();
            for (f, rt) in &fields {
                writeln!(w, "    pub {f}: {rt},").unwrap();
            }
            writeln!(w, "    pub sign: i64,\n}}\n").unwrap();
            writeln!(w, "/// {what}: each runtime delta is the projection of the full changed row.").unwrap();
            writeln!(w, "pub open spec fn {}_delta_ok(d: Seq<{dt}>, full: Seq<({trow}, int)>) -> bool {{", table_field(&d.name)).unwrap();
            writeln!(w, "    &&& d.len() == full.len()").unwrap();
            writeln!(w, "    &&& forall|i: int| #![trigger d[i]] 0 <= i < d.len() ==> {{").unwrap();
            writeln!(w, "        let {} = full[i].0;", sel.var).unwrap();
            for r in &rel {
                writeln!(w, "        &&& {r}").unwrap();
            }
            writeln!(w, "        &&& d[i].sign as int == full[i].1").unwrap();
            writeln!(w, "    }}\n}}\n").unwrap();
            ob.items.push(w);
            let mut cf: Vec<(String, Elem)> = fields.iter().map(|(f, rt)| (f.clone(), Elem::Prim(*rt))).collect();
            cf.push(("sign".into(), Elem::Prim("i64")));
            let mut tys: Vec<Ty> = sel.outs.iter().map(|o| o.2.ty).collect();
            tys.push(Ty::Int);
            delta_structs.push(CStruct { name: dt.clone(), fields: cf, tys, copy });
            // the captured columns: every column the fields read, by name
            let mut cols: Vec<String> = vec![];
            for e in &sel.exprs {
                let mut refs: Vec<String> = e.column_refs().into_iter().map(|c| c.name.clone()).collect();
                refs.sort();
                for c in refs {
                    if !cols.contains(&c) {
                        cols.push(c);
                    }
                }
            }
            let mut ctys = vec![];
            for c in &cols {
                let ty = ob.tr.sch.col_ty.get(&(d.name.clone(), c.clone())).and_then(|x| x.0).ok_or_else(|| format!("{what}: column {c} has no type"))?;
                ctys.push((c.clone(), ty));
            }
            let dfields = fields
                .iter()
                .zip(&sel.exprs)
                .zip(&sel.outs)
                .map(|(((f, _), e), o)| match strip_alias(e) {
                    Expr::Column(c) => (f.clone(), FieldSrc::Col(cols.iter().position(|x| *x == c.name).unwrap(), false), o.2.ty),
                    other => (f.clone(), FieldSrc::Expr(other.clone()), o.2.ty),
                })
                .collect();
            delta_decls.insert(d.name.clone(), DeltaDecl { table: d.name.clone(), cols: ctys, fields: dfields });
            deltas.insert(d.name.clone(), DeltaOut { table: d.name.clone(), elem: dt.clone(), copy, lossy: true, view: String::new(), el: Elem::Struct(dt) });
        }
    }
    for t in &tables {
        if !deltas.contains_key(t) {
            bail!("table {t}: no delta declared (every table of Db needs one)");
        }
    }

    // ── names must not collide with the spec's ──
    for n in ob.order.iter().chain(deltas.values().filter(|d| d.lossy || !d.elem.starts_with('(')).map(|d| &d.elem)) {
        if spec_text.contains(&format!("pub struct {n} ")) {
            bail!("type name {n} is already defined by the spec");
        }
    }

    // ── row structs and views ──
    let mut defs = String::new();
    for n in ob.order.clone() {
        let s = ob.structs[&n].clone();
        let copy = copyable(&s.fields);
        writeln!(defs, "/// Runtime row: {}.", s.used_by.join("; ")).unwrap();
        if copy {
            writeln!(defs, "#[repr(C)]\n#[derive(Clone, Copy)]").unwrap();
        }
        writeln!(defs, "pub struct {n} {{").unwrap();
        for f in &s.fields {
            writeln!(defs, "    /// {}.{} ({})", s.table, f.col, ty_doc(f.ty)).unwrap();
            writeln!(defs, "    pub {}: {},", f.name, rt_ty(f.ty)?).unwrap();
            if f.null {
                writeln!(defs, "    /// Nonzero: {} is NULL and the value above is unspecified.", f.name).unwrap();
                writeln!(defs, "    pub {}: u8,", null_flag(&f.name)).unwrap();
            }
        }
        writeln!(defs, "}}\n").unwrap();
        let spec = ob.spec_of(&n);
        let body = if ob.full(&n) {
            let fs: Vec<String> = ob
                .db_cols(&s.table)
                .iter()
                .map(|c| {
                    let f = s.fields.iter().find(|f| &f.col == c).unwrap();
                    format!("        {}: {},", sanitize(c), f.view("r"))
                })
                .collect();
            format!("{spec} {{\n{}\n    }}", fs.join("\n"))
        } else if s.fields.len() == 1 {
            s.fields[0].view("r")
        } else {
            format!("({})", s.fields.iter().map(|f| f.view("r")).collect::<Vec<_>>().join(", "))
        };
        writeln!(defs, "pub open spec fn {}(r: {n}) -> {spec} {{\n    {body}\n}}\n", ob.view(&n)).unwrap();
        writeln!(defs, "pub open spec fn {}(s: Seq<{n}>) -> Seq<{spec}> {{\n    s.map_values(|r: {n}| {}(r))\n}}\n", ob.seq_view(&n), ob.view(&n)).unwrap();
    }
    for rt in ob.prim_views.clone() {
        let tag = prim_tag(rt);
        writeln!(defs, "pub open spec fn seq_view_{tag}(s: Seq<{rt}>) -> Seq<{}> {{", prim_spec(rt)).unwrap();
        writeln!(defs, "    s.map_values(|x: {rt}| {})\n}}\n", rt_view("x", prim_ty_of(rt))).unwrap();
    }

    // ── parameters and result ──
    let b = &top.block;
    let mut pdefs = vec![];
    let mut pinit = vec![];
    let mut exec_params = vec![P { name: "s".into(), ty: "&State".into(), ghost: false, arg: Arg::State }];
    let mut pargs = vec![];
    for (n, (t, _, _)) in &ob.tr.params {
        let (rt, spec) = match t {
            Ty::Text => ("&[u8]", format!("p{n}@")),
            Ty::Date => ("i32", format!("p{n} as int")),
            Ty::Int | Ty::Dec(_) | Ty::Ts => ("i64", format!("p{n} as int")),
            Ty::Flt(_) => bail!("parameter ${n}: DOUBLE parameters have no runtime type"),
            other => bail!("parameter ${n}: {other:?}"),
        };
        pdefs.push(format!("p{n}: {}", verus_ty(*t)));
        pinit.push(format!("p{n}"));
        let arg = if rt == "&[u8]" { Arg::Text } else { Arg::Scalar(rt) };
        exec_params.push(P { name: format!("p{n}"), ty: rt.into(), ghost: false, arg });
        pargs.push(spec);
    }
    let mut out_tys = vec![];
    let mut out_views = vec![];
    let mut outs_rt: Vec<(&'static str, bool, Ty)> = vec![];
    let mut out_fits = vec![];
    for (k, c) in b.outs.iter().enumerate() {
        let (rt, view): (&str, Box<dyn Fn(&str) -> String>) = match c.val.ty {
            Ty::Text => ("Vec<u8>", Box::new(|x: &str| format!("{x}@"))),
            Ty::Bool => ("bool", Box::new(|x: &str| x.to_string())),
            Ty::Rat => ("(i128, i128)", Box::new(|x: &str| format!("Rat {{ num: {x}.0 as int, den: {x}.1 as int }}"))),
            Ty::Dec(_) => ("i128", Box::new(|x: &str| format!("{x} as int"))),
            Ty::Date => ("i32", Box::new(|x: &str| format!("{x} as int"))),
            Ty::Int | Ty::Ts => ("i64", Box::new(|x: &str| format!("{x} as int"))),
            Ty::Flt(1) => ("u64", Box::new(|x: &str| format!("f64_val({x})"))),
            other => bail!("result column {}: {other:?}", c.sql),
        };
        outs_rt.push((rt, c.nullable, c.val.ty));
        let fits = |x: &str| match rt {
            "i128" => Some(range_of(x, i128::MIN, i128::MAX)),
            "i64" => Some(range_of(x, i64::MIN as i128, i64::MAX as i128)),
            "i32" => Some(range_of(x, i32::MIN as i128, i32::MAX as i128)),
            "u64" => Some(format!("f64_in_range({x})")),
            "(i128, i128)" => Some(format!("{} && {}", range_of(&format!("{x}.num"), i128::MIN, i128::MAX),
                                           range_of(&format!("{x}.den"), i128::MIN, i128::MAX))),
            _ => None,
        };
        if let Some(f) = fits("v") {
            out_fits.push(if c.nullable {
                format!("match r.{} {{ Some(v) => {f}, None => true }}", c.field)
            } else {
                fits(&format!("r.{}", c.field)).unwrap()
            });
        }
        let acc = if b.outs.len() == 1 { "r".to_string() } else { format!("r.{k}") };
        if c.nullable {
            out_tys.push(format!("Option<{rt}>"));
            out_views.push(format!("{}: match {acc} {{ Some(v) => Some({}), None => None }}", c.field, view("v")));
        } else {
            out_tys.push(rt.to_string());
            out_views.push(format!("{}: {}", c.field, view(&acc)));
        }
    }
    let out_ty = if out_tys.len() == 1 { out_tys[0].clone() } else { format!("({})", out_tys.join(", ")) };

    // ── the obligation functions ──
    let mut fns: Vec<ObFn> = vec![];
    {
        let mut ps = vec![];
        let mut req = vec![];
        for s in scans.iter().filter(|s| !s.rescan) {
            let (ty, _) = pass(&s.elem, s.copy, true);
            ps.push(P { name: s.name.clone(), ty, ghost: false, arg: Arg::Rows(s.el.clone()) });
            req.push(format!("{}({DB}, {}@)", s.contract, s.name));
        }
        ps.push(P { name: "db".into(), ty: "Db".into(), ghost: true, arg: Arg::Ghost });
        fns.push(ObFn {
            doc: "Build: a state for db; None only outside the envelope.".into(),
            name: "build".into(),
            params: ps,
            ret: "(r: Option<State>)".into(),
            requires: req,
            ensures: vec![
                "r matches Some(s) ==> s.db() == db".into(),
                "r matches Some(s) ==> crate::kernel::history(s) == 0".into(),
                "envelope(db) ==> r is Some".into(),
            ],
        });
    }
    fns.push(ObFn {
        doc: "Execute: the query's answer on s.db(); None (the host then answers with SQL) only\n/// outside the envelope, or when no correct answer exists or one overflows the result's types.".into(),
        name: "execute".into(),
        params: exec_params,
        ret: format!("(r: Option<Vec<{out_ty}>>)"),
        requires: vec![],
        ensures: vec![
            format!("r matches Some(out) ==> query_result(s.db(), params({}), out_view(out@))", pargs.join(", ")),
            format!("envelope(s.db()) && answer_fits(s.db(), params({})) ==> r is Some", pargs.join(", ")),
        ],
    });
    if outs_rt.iter().any(|o| o.0 == "u64") {
        // a rounded float is only bounded by the spec: the bits it returns must still be finite
        fns.last_mut().unwrap().ensures.push("r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i])".into());
    }
    let delta_params = |deltas: &BTreeMap<String, DeltaOut>| -> Vec<P> {
        deltas
            .values()
            .map(|d| {
                let (ty, _) = pass(&d.elem, d.copy, false);
                P { name: format!("{}_delta", table_field(&d.table)), ty, ghost: false, arg: Arg::Rows(d.el.clone()) }
            })
            .collect()
    };
    let wanted = |s: &ScanOut| {
        let views: Vec<String> = deltas.values().map(|d| format!("{}_delta@", table_field(&d.table))).collect();
        format!("crate::kernel::rescan_{}_wanted({ST}, {})", s.name, views.join(", "))
    };
    let changes = deltas.values().map(|d| format!("{}_delta@.len()", table_field(&d.table))).collect::<Vec<_>>().join(" + ");
    let changes = if changes.is_empty() { "0".to_string() } else { changes };
    let mut wanted_decls: Vec<String> = vec!["/// What the state has absorbed since its build, in changed rows at most (e.g. ids it\n/// never frees).\npub open spec fn history(s: State) -> nat {\n    0\n}\n".to_string()];
    for s in scans.iter().filter(|s| s.rescan) {
        let tys: Vec<String> = deltas.values().map(|d| format!("{}_delta: Seq<{}>", table_field(&d.table), d.elem)).collect();
        wanted_decls.push(format!("/// The keys the kernel asks the host to rescan ({}); the host passes back exactly these.\npub open spec fn rescan_{}_wanted(s: State, {}) -> Seq<{}> {{\n    Seq::empty()\n}}\n", s.name, s.name, tys.join(", "), s.key_rt.unwrap()));
        let mut ps = vec![P { name: "s".into(), ty: "&State".into(), ghost: false, arg: Arg::State }];
        ps.extend(delta_params(&deltas));
        fns.push(ObFn {
            doc: format!("The keys whose rows the host rescans (rescan {}) for apply: the kernel's\n/// requested keys, which the host passes back to apply.", s.name),
            name: format!("rescan_{}_keys", s.name),
            params: ps,
            ret: format!("(r: Vec<{}>)", s.key_rt.unwrap()),
            requires: vec![],
            ensures: vec![format!("r@ == {}", wanted(s))],
        });
    }
    {
        let mut ps = vec![P { name: "s".into(), ty: "&State".into(), ghost: false, arg: Arg::State }];
        ps.extend(delta_params(&deltas));
        let mut req = vec![];
        for s in scans.iter().filter(|s| s.rescan) {
            ps.push(P { name: format!("{}_keys", s.name), ty: format!("&[{}]", s.key_rt.unwrap()), ghost: false, arg: Arg::Rows(Elem::Prim(s.key_rt.unwrap())) });
            let (ty, _) = pass(&s.elem, s.copy, false);
            ps.push(P { name: format!("{}_rows", s.name), ty, ghost: false, arg: Arg::Rows(s.el.clone()) });
        }
        let mut cap = vec![];
        for d in deltas.values() {
            let f = table_field(&d.table);
            if d.lossy {
                ps.push(P { name: format!("{f}_full"), ty: format!("Seq<({}, int)>", table_row(&d.table)), ghost: true, arg: Arg::Ghost });
                req.push(format!("{f}_delta_ok({f}_delta@, {f}_full)"));
                cap.push(format!("{f}_full"));
            } else {
                cap.push(format!("{}({f}_delta@)", d.view));
            }
        }
        req.push(format!("capture_ok({SDB}, {DB2}, {})", cap.join(", ")));
        for s in scans.iter().filter(|s| s.rescan) {
            req.push(format!("{n}_keys@ == {}", wanted(s), n = s.name));
            req.push(format!("{}({DB2}, {n}_keys@, {n}_rows@)", s.contract, n = s.name));
        }
        ps.push(P { name: "db2".into(), ty: "Db".into(), ghost: true, arg: Arg::Ghost });
        fns.push(ObFn {
            doc: "Apply one transaction: a state for db2; Err (the host then rebuilds) only outside\n/// the envelope or after max_history() changed rows since the build.".into(),
            name: "apply".into(),
            params: ps,
            ret: "(r: Result<State, u32>)".into(),
            requires: req,
            ensures: vec![
                "r matches Ok(s2) ==> s2.db() == db2".into(),
                format!("r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + {changes}"),
                format!("envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + {changes} <= max_history() ==> r is Ok"),
            ],
        });
    }

    // ── print ──
    let mut w = String::new();
    writeln!(w, "// GENERATED by {VERSION} from {src_name} and inputs {inputs_name}.  DO NOT EDIT: regenerate.").unwrap();
    writeln!(w, "//").unwrap();
    writeln!(w, "// Kernel obligations: the theorem the kernel proves about the query (q_ref in").unwrap();
    writeln!(w, "// gen_spec.rs), and the contracts the host glue must meet.  TRUSTED: the").unwrap();
    writeln!(w, "// scan / capture contracts are what the host is assumed to deliver; the").unwrap();
    writeln!(w, "// rules are documented in sql2verus/src/obligations.rs.  The kernel lives").unwrap();
    writeln!(w, "// behind crate::kernel; the host calls only the ob_* functions below.").unwrap();
    writeln!(w, "//").unwrap();
    writeln!(w, "// Kernel inputs:").unwrap();
    for l in inputs_text.trim().lines() {
        writeln!(w, "//   {}", l.trim_end()).unwrap();
    }
    writeln!(w, "#![allow(unused_imports)]").unwrap();
    writeln!(w, "use vstd::prelude::*;").unwrap();
    for m in ["bytes", "order", "rat", "rel", "sqlfns", "sums"] {
        writeln!(w, "use verus_common::{m}::*;").unwrap();
    }
    writeln!(w, "use crate::gen_spec::*;\nuse crate::kernel;\nuse crate::kernel::State;").unwrap();
    writeln!(w, "\nverus! {{\n").unwrap();
    writeln!(w, "// ── Runtime rows and their views ──\n").unwrap();
    w.push_str(&defs);
    writeln!(w, "// ── Parameters and result ──\n").unwrap();
    writeln!(w, "pub open spec fn params({}) -> Params {{\n    Params {{ {} }}\n}}\n", pdefs.join(", "), pinit.join(", ")).unwrap();
    writeln!(w, "pub open spec fn out_view(o: Seq<{out_ty}>) -> Seq<OutRow> {{").unwrap();
    writeln!(w, "    o.map_values(|r: {out_ty}| OutRow {{ {} }})\n}}\n", out_views.join(", ")).unwrap();
    writeln!(w, "// ── Scan and capture contracts ──\n").unwrap();
    for it in &ob.items {
        w.push_str(it);
    }
    let cps: Vec<String> = tables.iter().map(|t| format!("{}: Seq<({}, int)>", table_field(t), table_row(t))).collect();
    writeln!(w, "/// Each change is one row, inserted (+1) or deleted (-1).").unwrap();
    writeln!(w, "pub open spec fn unit_weights<A>(d: Seq<(A, int)>) -> bool {{").unwrap();
    writeln!(w, "    forall|i: int| 0 <= i < d.len() ==> (#[trigger] d[i]).1 == 1 || d[i].1 == -1\n}}\n").unwrap();
    writeln!(w, "/// One transaction's changes: every table of db2 is that of db plus its deltas (bags).").unwrap();
    writeln!(w, "pub open spec fn capture_ok(db: Db, db2: Db, {}) -> bool {{", cps.join(", ")).unwrap();
    for t in &tables {
        writeln!(w, "    &&& bag_step(db.{f}, db2.{f}, {f})", f = table_field(t)).unwrap();
        writeln!(w, "    &&& unit_weights({})", table_field(t)).unwrap();
    }
    writeln!(w, "}}\n").unwrap();
    writeln!(w, "// ── The envelope: where the kernel may not decline ──\n").unwrap();
    writeln!(w, "/// Most rows a table can hold (the DBMS's row ids are 64-bit).\npub open spec fn max_rows() -> int {{\n    {MAX_ROWS}\n}}\n").unwrap();
    writeln!(w, "/// Most changed rows a state may absorb after its build before apply may decline\n/// (the host then rebuilds).\npub open spec fn max_history() -> int {{\n    {MAX_HISTORY}\n}}\n").unwrap();
    let mut env = vec![];
    for t in &tables {
        let f = table_field(t);
        env.push(format!("db.{f}.len() <= max_rows()"));
        let bounds: Vec<String> = ob
            .db_cols(t)
            .iter()
            .filter_map(|c| {
                let x = format!("r.{}", sanitize(c));
                let (ty, null) = ob.tr.sch.col_ty[&(t.clone(), c.clone())];
                let v = if null { format!("{x}->0") } else { x.clone() };
                let bound = match ty {
                    Some(Ty::Flt(_)) => Some(format!("f64_in_range({v})")),
                    _ => ob.tr.sch.col_range.get(&(t.clone(), c.clone())).map(|&(lo, hi)| range_of(&v, lo, hi)),
                };
                bound.map(|b| if null { format!("({x} is Some ==> {b})") } else { b })
            })
            .collect();
        if !bounds.is_empty() {
            writeln!(w, "/// Each value of {t} lies in the range of its column's SQL type.").unwrap();
            writeln!(w, "pub open spec fn {f}_in_type(r: {}) -> bool {{\n    {}\n}}\n", table_row(t), bounds.join("\n    && ")).unwrap();
            env.push(format!("forall|i: int| 0 <= i < db.{f}.len() ==> {f}_in_type(#[trigger] db.{f}[i])"));
        }
    }
    writeln!(w, "/// The databases the DBMS can represent: each table within its row-id space, each").unwrap();
    writeln!(w, "/// value within its column's SQL type.").unwrap();
    writeln!(w, "pub open spec fn envelope(db: Db) -> bool {{").unwrap();
    for e in &env {
        writeln!(w, "    &&& {e}").unwrap();
    }
    writeln!(w, "}}\n").unwrap();
    writeln!(w, "/// An answer row whose values fit the result's runtime types.").unwrap();
    writeln!(w, "pub open spec fn out_fits(r: OutRow) -> bool {{\n    {}\n}}\n", if out_fits.is_empty() { "true".to_string() } else { out_fits.iter().map(|f| if out_fits.len() > 1 && f.starts_with("match") { format!("({f})") } else { f.clone() }).collect::<Vec<_>>().join("\n    && ") }).unwrap();
    writeln!(w, "/// A correct answer exists (`defined`: no divisor is zero; gen_spec.rs proves").unwrap();
    writeln!(w, "/// `defined <==> exists an answer`) and every correct answer fits: at most max_rows()").unwrap();
    writeln!(w, "/// rows, each in the result's runtime types.  A kernel that declines because no answer").unwrap();
    writeln!(w, "/// exists must prove").unwrap();
    writeln!(w, "/// `!defined`.").unwrap();
    writeln!(w, "pub open spec fn answer_fits(db: Db, params: Params) -> bool {{").unwrap();
    writeln!(w, "    &&& defined(db, params)").unwrap();
    writeln!(w, "    &&& forall|o: Seq<OutRow>| #[trigger] query_result(db, params, o) ==> o.len() <= max_rows() && forall|i: int| 0 <= i < o.len() ==> out_fits(#[trigger] o[i])\n}}\n").unwrap();
    writeln!(w, "// ── Obligations: the host calls only these ──\n").unwrap();
    let mut stub = String::new();
    writeln!(stub, "// Kernel skeleton GENERATED by {VERSION} for {src_name}: the entry points").unwrap();
    writeln!(stub, "// gen_obligations.rs calls, with its contracts.  Replace the bodies (and State).").unwrap();
    writeln!(stub, "#![allow(unused_imports)]\nuse vstd::prelude::*;\nuse verus_common::rat::*;\nuse crate::gen_spec::*;\nuse crate::gen_obligations::*;\n\nverus! {{\n").unwrap();
    writeln!(stub, "pub struct State {{\n    db: Ghost<Db>,\n}}\n\nimpl State {{\n    pub closed spec fn db(self) -> Db {{\n        self.db@\n    }}\n}}\n").unwrap();
    for d in &wanted_decls {
        writeln!(stub, "{d}").unwrap();
    }
    for f in &fns {
        writeln!(w, "/// {}", f.doc).unwrap();
        w.push_str(&f.sig("ob_"));
        writeln!(w, "{{\n    kernel::{}({})\n}}\n", f.name, f.args()).unwrap();
        writeln!(stub, "#[verifier::external_body]").unwrap();
        stub.push_str(&f.sig(""));
        writeln!(stub, "{{\n    unimplemented!()\n}}\n").unwrap();
    }
    writeln!(w, "}} // verus!").unwrap();
    writeln!(stub, "}} // verus!").unwrap();
    let mut structs: Vec<CStruct> = vec![];
    for n in &ob.order {
        let s = &ob.structs[n];
        let mut fields = vec![];
        let mut tys = vec![];
        for f in &s.fields {
            fields.push((f.name.clone(), Elem::Prim(rt_ty(f.ty)?)));
            tys.push(f.ty);
            if f.null {
                fields.push((null_flag(&f.name), Elem::Prim("u8")));
                tys.push(Ty::Bool);
            }
        }
        structs.push(CStruct { name: n.clone(), fields, tys, copy: copyable(&s.fields) });
    }
    structs.extend(delta_structs);
    let params = ob.tr.params.iter().map(|(n, (t, _, _))| (*n, *t)).collect();
    let model = Model {
        fns,
        structs,
        outs: outs_rt,
        params,
        scans: scan_decls,
        deltas: delta_decls.into_values().collect(),
        query_sql,
        src_name: src_name.to_string(),
        inputs_name,
    };
    Ok((w, stub, model))
}
