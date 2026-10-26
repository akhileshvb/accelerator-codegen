// sql2verus: SQL (+ DDL) -> trusted Verus reference spec (q_ref).
//
//   sql2verus --schema <ddl.sql> --query <query.sql> --out <spec.rs>
//             [--obligations <gen_obligations.rs> [--inputs <q.inputs>] [--kernel-stub <kernel.rs>]
//              [--world <gen_world.rs>] [--ffi <gen_ffi.rs> --ffi-header <q.h> --ffi-prefix <p>
//               [--host-glue <gen_glue.cpp> --accel-id <id> --workload <name> --query-id <n>]]]
//   (kernel obligations: see obligations.rs; the store adapter and C ABI
//   derived from them: see glue.rs)
//
// DataFusion parses the query and resolves names and types
// (`create_logical_plan`: no optimizer, so the plan keeps the SQL's shape).
// This program walks that plan and prints a Verus file that states the
// query's meaning.  Whatever it has no rule for, it refuses, naming the
// construct.
//
// Supported SQL
//   query blocks   SELECT [aggregates] FROM ... WHERE ... [GROUP BY ...
//                  [HAVING ...]], at the top level, as derived tables
//                  (FROM (SELECT ...) AS t, WITH t AS (...)), and as
//                  subqueries; ORDER BY and LIMIT at the top level
//   FROM           base tables and derived tables, comma / INNER JOIN ... ON,
//                  LEFT JOIN ... ON (the right side a single table, whose
//                  columns may then only be counted: COUNT(col))
//   expressions    comparisons, AND / OR / NOT, + - * /, unary -, CASE WHEN
//                  ... ELSE, [NOT] IN (list), [NOT] BETWEEN, [NOT] LIKE,
//                  EXTRACT(YEAR | MONTH | DAY FROM d), SUBSTRING(s FROM i
//                  FOR n), CAST, DATE / TIMESTAMP literals +- intervals,
//                  $n parameters, [NOT] EXISTS, [NOT] IN (subquery), scalar
//                  subqueries (possibly correlated, at any depth)
//   aggregates     COUNT(*), COUNT(col), COUNT(DISTINCT e), SUM, AVG, MIN, MAX
//
// Semantic rules applied (the decisions a reviewer signs off on once, here):
//   * Tables are bags (sequences; duplicates count).  Every column the query
//     reads must be NOT NULL; parameters are non-NULL by host contract.
//     The only NULLs are those SQL itself produces: an aggregate other than
//     COUNT over no rows (a scalar subquery or a query without GROUP BY),
//     modelled as Option, and the missing side of a LEFT JOIN.  A NULL
//     comparison is false; such comparisons are refused under NOT, where
//     SQL's three-valued logic would differ.
//   * A query block means: the FROM-clause tuples (bag product) that satisfy
//     WHERE (`tuples`); GROUP BY splits them into groups, one output row per
//     non-empty group that satisfies HAVING; without GROUP BY, aggregates
//     give exactly one row; otherwise each tuple gives one row.
//   * INTEGER / BIGINT -> int; DECIMAL(p,s) -> int (value * 10^s); DATE ->
//     int (days since 1970-01-01); TIMESTAMP -> int (nanoseconds since
//     1970-01-01 00:00); BOOLEAN -> bool; text -> Seq<u8>.  Arithmetic is
//     exact: + - * on scaled integers, and / and AVG give exact rationals
//     (Rat {num, den}); rounding to the engine's result type is host
//     formatting.  Division by a non-literal is allowed only in the SELECT
//     list, where a zero divisor leaves the query without an answer (SQL
//     raises an error).
//   * DOUBLE / REAL -> int (the exact value * 2^1074; finite by host
//     contract).  Comparisons, GROUP BY, MIN / MAX on float values are exact.
//     SUM / AVG of float expressions are rounded in an engine-chosen order:
//     the spec bounds the result (verus_common::rel::float_within, gamma_m
//     with u = 2^-53) instead of fixing it.  Float arithmetic elsewhere is
//     refused.
//   * Text: = is byte equality; < and ORDER BY are bytes_lt (C collation).
//     CHAR(n) follows the target engine (--engine): DuckDB has no padded
//     CHAR (it is VARCHAR), so CHAR compares as bytes, trailing spaces
//     significant; Postgres (bpchar) / MySQL (PAD SPACE) compare CHAR without
//     trailing spaces (literals are trimmed here; columns and parameters by
//     host contract).  LIKE and
//     SUBSTRING count bytes (= characters for ASCII text).
//   * ORDER BY: rows are sorted, ties in any order; LIMIT n takes the first n
//     rows of some such ordering.
//
// After the definitions the file carries facts about them, which Verus
// checks (so they add nothing to what is trusted): an answer exists, and it
// is unique as a bag of rows, or as a sequence when ORDER BY orders every
// pair of rows.
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fmt::Write as _;
use std::sync::Arc;

use datafusion::arrow::datatypes::{DataType, TimeUnit};
use datafusion::common::tree_node::{Transformed, TreeNode};
use datafusion::common::{Column, DFSchemaRef, ScalarValue};
use datafusion::logical_expr::{
    expr::{AggregateFunction, Alias, Between, Case, Exists, InList, InSubquery, Like, Placeholder, ScalarFunction},
    BinaryExpr, Cast, Distinct, Expr, FetchType, JoinType, LogicalPlan, Operator, SkipType, Subquery, Union,
};
use datafusion::prelude::*;
use datafusion::sql::sqlparser::{ast, dialect::GenericDialect, parser::Parser};

type Res<T> = Result<T, String>;

macro_rules! bail {
    ($($t:tt)*) => { return Err(format!($($t)*)) };
}

mod obligations;
mod glue;

const VERSION: &str = "sql2verus 0.2 (datafusion 55.1.0)";
const NS_PER_DAY: i128 = 86_400_000_000_000;

// ── types ─────────────────────────────────────────────────────────────────
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Ty {
    Int,
    Dec(u32),
    Rat,
    Flt(u32), // value * 2^(1074 p)
    Text,
    Date,
    Ts,
    Bool,
    Days,
    Months,
    Nanos,
}

fn ty_of(dt: &DataType) -> Option<Ty> {
    Some(match dt {
        DataType::Int8 | DataType::Int16 | DataType::Int32 | DataType::Int64 => Ty::Int,
        DataType::UInt8 | DataType::UInt16 | DataType::UInt32 | DataType::UInt64 => Ty::Int,
        DataType::Decimal128(_, s) if *s >= 0 => Ty::Dec(*s as u32),
        DataType::Float32 | DataType::Float64 => Ty::Flt(1),
        DataType::Utf8 | DataType::Utf8View | DataType::LargeUtf8 => Ty::Text,
        DataType::Date32 => Ty::Date,
        DataType::Timestamp(_, None) => Ty::Ts,
        DataType::Boolean => Ty::Bool,
        _ => return None,
    })
}

fn ty_doc(t: Ty) -> String {
    match t {
        Ty::Int => "integer".into(),
        Ty::Dec(s) => format!("DECIMAL, value * 10^{s}"),
        Ty::Rat => "exact rational num / den".into(),
        Ty::Flt(p) => format!("DOUBLE, value * 2^{}", 1074 * p),
        Ty::Text => "text bytes".into(),
        Ty::Date => "DATE, days since 1970-01-01".into(),
        Ty::Ts => "TIMESTAMP, nanoseconds since 1970-01-01 00:00".into(),
        Ty::Bool => "boolean".into(),
        Ty::Days => "days".into(),
        Ty::Months => "months".into(),
        Ty::Nanos => "nanoseconds".into(),
    }
}

fn verus_ty(t: Ty) -> &'static str {
    match t {
        Ty::Text => "Seq<u8>",
        Ty::Bool => "bool",
        Ty::Rat => "Rat",
        _ => "int",
    }
}

/// A GROUP BY key value as the key: Option<T> when it may be NULL.
fn opt_key(v: Val) -> Val {
    let Some(gd) = v.guard.clone() else { return v };
    let code = match v.code.strip_suffix("->0") {
        Some(c) if gd == format!("{c} is Some") => c.to_string(),
        _ => format!("if {gd} {{ Some({}) }} else {{ None }}", v.code),
    };
    Val { code, guard: None, opt: true, ..v }
}

/// The Verus type of a GROUP BY key.
fn key_ty(v: &Val) -> String {
    if v.opt { format!("Option<{}>", verus_ty(v.ty)) } else { verus_ty(v.ty).to_string() }
}

fn scale_of(t: Ty) -> Option<u32> {
    match t {
        Ty::Int => Some(0),
        Ty::Dec(s) => Some(s),
        _ => None,
    }
}

// ── an emitted expression ─────────────────────────────────────────────────
#[derive(Clone, Debug)]
struct Val {
    code: String,                     // the value; for Rat the numerator
    den: Option<String>,              // Rat denominator (float AVG: the count)
    ty: Ty,
    prec: u8,                         // 3 atom, 2 *, 1 + -, 0 comparison / logic
    int_lit: Option<i128>,
    str_lit: Option<String>,
    char_col: bool,
    param: Option<u32>,
    approx: Option<(String, String)>, // computed float: (abs code, rounded-operation count)
    guard: Option<String>,            // the value is NULL unless guard holds
    nullcmp: bool,                    // a boolean that reads NULL as false
    opt: bool,                        // code is an Option<..> (a GROUP BY key that may be NULL)
    nonlit_div: bool,                 // divides by a non-literal
    den_conds: Vec<String>,           // divisors that must be non-zero
}

impl Val {
    fn atom(code: String, ty: Ty) -> Val {
        Val {
            code,
            den: None,
            ty,
            prec: 3,
            int_lit: None,
            str_lit: None,
            char_col: false,
            param: None,
            approx: None,
            guard: None,
            nullcmp: false,
            opt: false,
            nonlit_div: false,
            den_conds: vec![],
        }
    }
    fn int(v: i128, ty: Ty) -> Val {
        Val { int_lit: Some(v), ..Val::atom(v.to_string(), ty) }
    }
    fn wrap(&self, min: u8) -> String {
        if self.prec >= min { self.code.clone() } else { format!("({})", self.code) }
    }
    fn den_s(&self) -> String {
        self.den.clone().unwrap_or("1".into())
    }
    /// Carry the operands' side conditions over to a result.
    fn with_meta(mut self, parts: &[&Val]) -> Val {
        for p in parts {
            self.guard = match (self.guard.take(), &p.guard) {
                (None, None) => None,
                (Some(a), None) => Some(a),
                (None, Some(b)) => Some(b.clone()),
                (Some(a), Some(b)) if a == *b => Some(a),
                (Some(a), Some(b)) => Some(format!("{a} && {b}")),
            };
            self.nullcmp |= p.nullcmp;
            self.nonlit_div |= p.nonlit_div;
            for c in &p.den_conds {
                if !self.den_conds.contains(c) {
                    self.den_conds.push(c.clone());
                }
            }
        }
        self
    }
}

fn pow10(n: u32) -> i128 {
    10i128.pow(n)
}

fn rescale(v: &Val, from: u32, to: u32) -> Val {
    if from == to {
        return v.clone();
    }
    let k = pow10(to - from);
    match v.int_lit {
        Some(x) => Val { int_lit: Some(x * k), code: (x * k).to_string(), prec: 3, ..v.clone() },
        None => Val { code: format!("{} * {}", v.wrap(2), k), prec: 2, int_lit: None, ..v.clone() },
    }
}

fn flt_rescale(code: &str, prec: u8, from: u32, to: u32) -> (String, u8) {
    if from == to {
        (code.to_string(), prec)
    } else {
        let c = if prec >= 2 { code.to_string() } else { format!("({code})") };
        (format!("{c} * pow2({})", 1074 * (to - from)), 2)
    }
}

/// scale of a numeric parameter used as a multiplier (`x * $n`): DECIMAL(.., 9), value * 10^9.
const MULT_PARAM_SCALE: u32 = 9;

fn bytes_lit(s: &str) -> String {
    if s.is_empty() {
        return "Seq::<u8>::empty()".into();
    }
    let parts: Vec<String> = s
        .bytes()
        .map(|b| match b {
            b'\'' => "b'\\''".into(),
            b'\\' => "b'\\\\'".into(),
            0x20..=0x7e => format!("b'{}'", b as char),
            _ => format!("{b}u8"),
        })
        .collect();
    format!("seq![{}]", parts.join(", "))
}

/// Replace the tuple variable `t` (the token, as in `t.x` or `f(t)`) by `new`.
fn subst_t(code: &str, new: &str) -> String {
    let b = code.as_bytes();
    let ident = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    let mut o = String::new();
    let mut i = 0;
    while i < b.len() {
        let prev_ok = i == 0 || !(ident(b[i - 1]) || b[i - 1] == b'.' || b[i - 1] == b'\'');
        let next_ok = i + 1 >= b.len() || !ident(b[i + 1]);
        if b[i] == b't' && prev_ok && next_ok {
            o.push_str(new);
        } else {
            o.push(b[i] as char);
        }
        i += 1;
    }
    o
}

// ── calendar ──────────────────────────────────────────────────────────────
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn days_in_month(y: i64, m: i64) -> i64 {
    days_from_civil(if m == 12 { y + 1 } else { y }, if m == 12 { 1 } else { m + 1 }, 1) - days_from_civil(y, m, 1)
}

/// d + n months, the day clamped to the month's end (PostgreSQL).
fn add_months(d: i64, n: i64) -> i64 {
    let (y, m, day) = civil_from_days(d);
    let t = y * 12 + (m - 1) + n;
    let (y2, m2) = (t.div_euclid(12), t.rem_euclid(12) + 1);
    days_from_civil(y2, m2, day.min(days_in_month(y2, m2)))
}

fn date_val(d: i64) -> Val {
    let (y, mo, da) = civil_from_days(d);
    Val { int_lit: Some(d as i128), ..Val::atom(format!("{d} /* DATE '{y:04}-{mo:02}-{da:02}' */"), Ty::Date) }
}

fn parse_date(s: &str) -> Res<i64> {
    let p: Vec<&str> = s.trim().split('-').collect();
    if p.len() != 3 {
        bail!("DATE literal '{s}' is not YYYY-MM-DD");
    }
    let n = |x: &str| x.trim().parse::<i64>().map_err(|_| format!("DATE literal '{s}' is not YYYY-MM-DD"));
    let (y, m, d) = (n(p[0])?, n(p[1])?, n(p[2])?);
    if !(1..=12).contains(&m) || d < 1 || d > days_in_month(y, m) {
        bail!("DATE literal '{s}' is out of range");
    }
    Ok(days_from_civil(y, m, d))
}

fn parse_ts(s: &str) -> Res<i128> {
    let s = s.trim();
    let (date, time) = match s.split_once([' ', 'T']) {
        Some((a, b)) => (a, b),
        None => (s, "00:00:00"),
    };
    let days = parse_date(date)? as i128;
    let tp: Vec<&str> = time.split(':').collect();
    if tp.len() != 3 {
        bail!("TIMESTAMP literal '{s}' is not YYYY-MM-DD HH:MM:SS[.f]");
    }
    let n = |x: &str| x.parse::<i128>().map_err(|_| format!("TIMESTAMP literal '{s}'"));
    let (h, mi) = (n(tp[0])?, n(tp[1])?);
    let (sec, frac) = match tp[2].split_once('.') {
        Some((a, f)) => {
            if f.len() > 9 {
                bail!("TIMESTAMP literal '{s}': more than nanosecond precision");
            }
            (n(a)?, n(f)? * pow10(9 - f.len() as u32))
        }
        None => (n(tp[2])?, 0),
    };
    if h > 23 || mi > 59 || sec > 59 {
        bail!("TIMESTAMP literal '{s}' is out of range");
    }
    Ok(days * NS_PER_DAY + ((h * 60 + mi) * 60 + sec) * 1_000_000_000 + frac)
}

/// The exact value of a finite f64, as Verus code at scale 2^1074.
fn f64_code(x: f64) -> Res<String> {
    if !x.is_finite() {
        bail!("non-finite float literal");
    }
    if x == 0.0 {
        return Ok("0".into());
    }
    let bits = x.to_bits();
    let sign = if bits >> 63 == 1 { "-" } else { "" };
    let exp = ((bits >> 52) & 0x7ff) as i64;
    let frac = bits & ((1u64 << 52) - 1);
    let (mut m, mut e) = if exp == 0 { (frac, -1074i64) } else { (frac | (1u64 << 52), exp - 1075) };
    while m % 2 == 0 {
        m /= 2;
        e += 1;
    }
    Ok(format!("{sign}{m} * pow2({})", e + 1074))
}

fn decimal_to_f64(v: i128, s: i8) -> f64 {
    format!("{v}e{}", -(s as i32)).parse::<f64>().unwrap()
}

const RESERVED: &[&str] = &[
    "db", "params", "t", "f", "r", "out", "full", "i", "j", "k", "m", "x", "y", "a", "b", "s", "e", "u", "v", "z", "self", "type", "fn",
    "let", "match", "use", "mod", "ref", "move", "in", "loop", "as", "where", "impl", "trait", "struct", "enum", "const", "static", "pub",
    "crate", "super", "true", "false", "if", "else", "while", "for", "return", "break", "continue", "dyn", "async", "await", "unsafe",
    "extern", "box", "yield", "macro", "try", "abstract", "final", "override", "priv", "typeof", "unsized", "virtual", "do", "become",
    "spec", "proof", "exec", "tracked", "ghost", "open", "closed", "requires", "ensures", "decreases", "invariant", "forall", "exists",
    "choose", "assert", "assume", "seq", "sum", "flat", "ind", "int", "nat", "bool", "char", "str", "verus", "is", "has", "old", "pow2",
    "abs", "dedup", "global", "default", "union", "auto", "t0", "y0",
];

/// A Verus identifier for an SQL name.
fn sanitize(s: &str) -> String {
    let mut o = String::new();
    for ch in s.to_lowercase().chars() {
        if ch.is_ascii_alphanumeric() {
            o.push(ch);
        } else if !o.ends_with('_') {
            o.push('_');
        }
    }
    let mut o = o.trim_matches('_').to_string();
    if o.is_empty() || o.chars().next().unwrap().is_ascii_digit() {
        o = format!("c_{o}");
    }
    if RESERVED.contains(&o.as_str()) || o.starts_with("g_") || o.starts_with("i_") {
        o.push('_');
    }
    o
}

fn camel(s: &str) -> String {
    s.split('_')
        .map(|w| {
            let mut c = w.chars();
            match c.next() {
                Some(f) => f.to_ascii_uppercase().to_string() + c.as_str(),
                None => String::new(),
            }
        })
        .collect()
}

fn split_and(e: &Expr) -> Vec<Expr> {
    match e {
        Expr::BinaryExpr(BinaryExpr { left, op: Operator::And, right }) => {
            let mut v = split_and(left);
            v.extend(split_and(right));
            v
        }
        other => vec![other.clone()],
    }
}

fn strip_alias(e: &Expr) -> &Expr {
    match e {
        Expr::Alias(Alias { expr, .. }) => strip_alias(expr),
        other => other,
    }
}

/// A one-line description of a subquery plan, for doc comments.
fn plan_doc(p: &LogicalPlan) -> String {
    let s = p.display_indent().to_string();
    let lines: Vec<&str> = s.lines().map(|l| l.trim()).collect();
    let mut o = lines.iter().rev().cloned().collect::<Vec<_>>().join(" <- ");
    if o.len() > 240 {
        let mut cut = 240;
        while !o.is_char_boundary(cut) {
            cut -= 1;
        }
        o.truncate(cut);
        o.push_str(" ...");
    }
    o
}

// ── schema ────────────────────────────────────────────────────────────────
struct Schema {
    ddl_cols: HashMap<String, Vec<String>>,
    /// CHAR(n) columns compared without trailing spaces (empty under byte semantics)
    char_cols: HashSet<(String, String)>,
    /// how the target engine compares CHAR(n): "bytes" or "padded" (and why)
    char_semantics: (&'static str, &'static str),
    col_ty: HashMap<(String, String), (Option<Ty>, bool)>,
    /// the inclusive value range of a numeric or date column's SQL type
    col_range: HashMap<(String, String), (i128, i128)>,
}

/// The values a column of this type can hold, as the model stores them
/// (decimals scaled, dates as day numbers); None: unbounded (text, booleans).
fn type_range(dt: &DataType) -> Option<(i128, i128)> {
    let bits = |b: u32| (-(1i128 << (b - 1)), (1i128 << (b - 1)) - 1);
    Some(match dt {
        DataType::Int8 => bits(8),
        DataType::Int16 => bits(16),
        DataType::Int32 | DataType::Date32 => bits(32),
        DataType::Int64 => bits(64),
        DataType::UInt8 => (0, u8::MAX as i128),
        DataType::UInt16 => (0, u16::MAX as i128),
        DataType::UInt32 => (0, u32::MAX as i128),
        DataType::UInt64 => (0, u64::MAX as i128),
        DataType::Decimal128(p, _) => {
            let m = 10i128.pow(*p as u32) - 1;
            (-m, m)
        }
        _ => return None,
    })
}

/// An identifier as DataFusion names it: quoted keeps its case, unquoted is
/// lower-cased.
fn norm_ident(id: &ast::Ident) -> String {
    if id.quote_style.is_some() { id.value.clone() } else { id.value.to_lowercase() }
}

async fn load_schema(ctx: &SessionContext, ddl: &str, char_semantics: (&'static str, &'static str)) -> Res<Schema> {
    let padded = char_semantics.0 == "padded";
    let stmts = Parser::parse_sql(&GenericDialect {}, ddl).map_err(|e| format!("schema: {e}"))?;
    let mut ddl_cols = HashMap::new();
    let mut char_cols = HashSet::new();
    let mut col_ty = HashMap::new();
    let mut col_range = HashMap::new();
    for st in stmts {
        match &st {
            ast::Statement::CreateTable(ct) => {
                let t = match ct.name.0.last() {
                    Some(ast::ObjectNamePart::Identifier(id)) => norm_ident(id),
                    _ => bail!("schema: table name {}", ct.name),
                };
                let mut cols = vec![];
                for c in &ct.columns {
                    let n = norm_ident(&c.name);
                    if padded && matches!(c.data_type, ast::DataType::Char(_) | ast::DataType::Character(_)) {
                        char_cols.insert((t.clone(), n.clone()));
                    }
                    cols.push(n);
                }
                let mut seen = HashSet::new();
                for c in &cols {
                    if !seen.insert(sanitize(c)) {
                        bail!("schema: columns of {t} collide as Verus names ({c})");
                    }
                }
                if ddl_cols.keys().any(|u: &String| sanitize(u) == sanitize(&t)) {
                    bail!("schema: table {t} defined twice, or collides with another as a Verus name");
                }
                ddl_cols.insert(t.clone(), cols);
                // Constraints do not change what a query means, except that a
                // PRIMARY KEY column is NOT NULL; register the bare table.
                let mut ct2 = ct.clone();
                let mut pk: HashSet<String> = HashSet::new();
                for c in &ct2.constraints {
                    if let ast::TableConstraint::PrimaryKey(p) = c {
                        for ic in &p.columns {
                            if let ast::Expr::Identifier(id) = &ic.column.expr {
                                pk.insert(norm_ident(id));
                            }
                        }
                    }
                }
                ct2.constraints.clear();
                for c in ct2.columns.iter_mut() {
                    let is_pk = pk.contains(&norm_ident(&c.name)) || c.options.iter().any(|o| matches!(o.option, ast::ColumnOption::PrimaryKey(_)));
                    c.options.retain(|o| matches!(o.option, ast::ColumnOption::NotNull | ast::ColumnOption::Null));
                    if is_pk && !c.options.iter().any(|o| matches!(o.option, ast::ColumnOption::NotNull)) {
                        c.options.push(ast::ColumnOptionDef { name: None, option: ast::ColumnOption::NotNull });
                    }
                }
                ctx.sql(&ast::Statement::CreateTable(ct2).to_string()).await.map_err(|e| format!("schema: {e}"))?;
                let prov = ctx.table_provider(datafusion::common::TableReference::bare(t.clone())).await.map_err(|e| format!("schema: {e}"))?;
                for f in prov.schema().fields() {
                    col_ty.insert((t.clone(), f.name().clone()), (ty_of(f.data_type()), f.is_nullable()));
                    if let Some(r) = type_range(f.data_type()) {
                        col_range.insert((t.clone(), f.name().clone()), r);
                    }
                }
            }
            // views are expanded where they are used (as derived tables)
            ast::Statement::CreateView { .. } => {
                ctx.sql(&st.to_string()).await.map_err(|e| format!("schema: {e}"))?;
            }
            // indexes and the like do not change what a query means
            ast::Statement::CreateIndex(_) | ast::Statement::AlterTable { .. } | ast::Statement::Comment { .. } => {}
            other => bail!("schema: statement not supported: {}", other.to_string().lines().next().unwrap_or("")),
        }
    }
    Ok(Schema { ddl_cols, char_cols, char_semantics, col_ty, col_range })
}

/// Verus names for a table: its Db field and its row struct.
fn table_field(t: &str) -> String {
    sanitize(t)
}

fn table_row(t: &str) -> String {
    format!("{}Row", camel(&sanitize(t)))
}

mod perm;

// ── relations, binders, frames ────────────────────────────────────────────
#[derive(Clone, Debug)]
struct RelInfo {
    func: String,                          // rel_x
    row: String,                           // XRow
    cols: Vec<(String, String, Ty, bool, bool)>, // (sql name, field, ty, char, nullable)
}

#[derive(Clone, Debug)]
enum Src {
    Base(String),
    Rel(RelInfo),
}

#[derive(Clone, Debug)]
struct Binder {
    alias: String,
    var: String,
    src: Src,
    optional: bool,
    on: Vec<String>, // LEFT JOIN conditions
}

impl Binder {
    fn row(&self) -> String {
        match &self.src {
            Src::Base(t) => table_row(t),
            Src::Rel(r) => r.row.clone(),
        }
    }
    fn seq(&self) -> String {
        match &self.src {
            Src::Base(t) => format!("db.{}", table_field(t)),
            Src::Rel(r) => format!("{}(db, params)", r.func),
        }
    }
    fn vty(&self) -> String {
        if self.optional { format!("Option<{}>", self.row()) } else { self.row() }
    }
}

#[derive(Default)]
struct Frame {
    binders: Vec<Binder>,
    outer: Vec<Binder>,
    uses_params: bool,
    calls: bool, // calls a subquery's functions (needs db)
}

/// How column references render.
#[derive(Clone)]
enum Ctx {
    /// per tuple: local binders are variables, or fields of tuple `t`
    Row { tuple: Option<String> },
    /// per group: aggregates and GROUP BY keys
    Group(Arc<GroupCtx>),
}

struct GroupCtx {
    prefix: String,
    tuple_ty: String,
    outer_args: String,
    keys: Vec<(String, Val)>, // (g var, key value in the Row{t} context)
    group_exprs: Vec<Expr>,
    aggr_exprs: Vec<Expr>,
    schema: DFSchemaRef,
    scalar: bool,
}

impl GroupCtx {
    fn call(&self, f: &str) -> String {
        let gargs: String = self.keys.iter().map(|(g, _)| format!(", {g}")).collect();
        format!("{}{f}(db, params{}{gargs}", self.prefix, self.outer_args)
    }
}

/// A translated query block.
struct Block {
    prefix: String,
    tuple: String,
    binders: Vec<Binder>,
    outer: Vec<Binder>,
    shape: Shape,
    outs: Vec<OutCol>,
    having: Vec<Val>,
    keys: Vec<(String, Val)>,
    distinct: bool,
    union_rows: Option<String>, // UNION: the rows, built from the inputs' rows
}

impl Block {
    fn outer_params(&self) -> String {
        self.outer.iter().map(|b| format!(", {}: {}", b.var, b.vty())).collect()
    }
    fn outer_args(&self) -> String {
        self.outer.iter().map(|b| format!(", {}", b.var)).collect()
    }
}

#[derive(Clone, PartialEq, Debug)]
enum Shape {
    Plain,
    Grouped,
    Scalar,
}

#[derive(Clone)]
struct OutCol {
    sql: String,
    field: String,
    val: Val,       // Plain: Row{t} context; Grouped / Scalar: Group context
    nullable: bool, // Option<..> (an aggregate over no rows)
    key: Option<usize>,
}

impl OutCol {
    fn vty(&self) -> String {
        let base = if self.val.approx.is_some() { "int" } else { verus_ty(self.val.ty) };
        if self.nullable { format!("Option<{base}>") } else { base.to_string() }
    }
}

// ── the translator ────────────────────────────────────────────────────────
struct Tr<'a> {
    sch: &'a Schema,
    frames: Vec<Frame>,
    used: BTreeMap<String, BTreeSet<String>>,
    params: BTreeMap<u32, (Ty, bool, bool)>,
    items: Vec<String>,
    vars: HashSet<String>,
    rels: HashMap<String, RelInfo>,
    counters: HashMap<&'static str, usize>,
    notes: BTreeSet<String>,
    pfx: String, // prefix of subquery helper names (obligations)
    perm: perm::Perm,
    nulls_first_default: [bool; 2], // NULLs first under [ASC, DESC] when ORDER BY does not say
    guarded_vals: HashSet<String>, // blocks (prefixes) whose aggregates skip NULL values
}

impl<'a> Tr<'a> {
    fn fresh(&mut self, kind: &'static str) -> usize {
        let c = self.counters.entry(kind).or_insert(0);
        *c += 1;
        *c
    }

    fn var_for(&mut self, alias: &str) -> String {
        let base = sanitize(alias);
        let mut v = base.clone();
        let mut n = 2;
        while self.vars.contains(&v) {
            v = format!("{base}_{n}");
            n += 1;
        }
        self.vars.insert(v.clone());
        v
    }

    fn frame(&mut self) -> &mut Frame {
        self.frames.last_mut().unwrap()
    }

    /// After a sub-block: its outer references are references of the
    /// enclosing frame too, unless they are the enclosing frame's own.
    fn absorb(&mut self, f: &Frame) {
        let nf = self.frames.len();
        for o in &f.outer {
            let local = self.frames[nf - 1].binders.iter().any(|x| x.var == o.var);
            if !local && !self.frames[nf - 1].outer.iter().any(|x| x.var == o.var) {
                self.frames[nf - 1].outer.push(o.clone());
            }
        }
        if f.uses_params {
            self.frame().uses_params = true;
        }
        self.frame().calls = true;
    }

    // ── FROM ──
    fn from_tree(&mut self, p: &LogicalPlan, out: &mut Vec<Binder>, on: &mut Vec<Expr>) -> Res<()> {
        match p {
            LogicalPlan::TableScan(ts) => {
                if !ts.filters.is_empty() || ts.fetch.is_some() {
                    bail!("table scan with pushed-down filters/fetch");
                }
                let t = ts.table_name.table().to_string();
                if let Some(vp) = ts.source.get_logical_plan() {
                    let vp = vp.into_owned();
                    out.push(self.rel_binder(&t, &vp)?);
                } else {
                    out.push(self.base_binder(&t, &t)?);
                }
            }
            LogicalPlan::SubqueryAlias(sa) => match sa.input.as_ref() {
                LogicalPlan::TableScan(ts) if ts.source.get_logical_plan().is_some() => {
                    let vp = ts.source.get_logical_plan().unwrap().into_owned();
                    out.push(self.rel_binder(sa.alias.table(), &vp)?);
                }
                LogicalPlan::TableScan(ts) => {
                    let t = ts.table_name.table().to_string();
                    out.push(self.base_binder(sa.alias.table(), &t)?);
                }
                inner => {
                    let b = self.rel_binder(sa.alias.table(), inner)?;
                    out.push(b);
                }
            },
            LogicalPlan::Join(j) => match j.join_type {
                JoinType::Inner => {
                    self.from_tree(&j.left, out, on)?;
                    self.from_tree(&j.right, out, on)?;
                    for (l, r) in &j.on {
                        on.push(l.clone().eq(r.clone()));
                    }
                    if let Some(f) = &j.filter {
                        on.push(f.clone());
                    }
                }
                JoinType::Left | JoinType::Right => {
                    // the preserved side, then the side that may be missing
                    let (keep, opt) = if j.join_type == JoinType::Left { (&j.left, &j.right) } else { (&j.right, &j.left) };
                    self.from_tree(keep, out, on)?;
                    let mut right = vec![];
                    let mut ron = vec![];
                    self.from_tree(opt, &mut right, &mut ron)?;
                    if right.len() != 1 || !ron.is_empty() {
                        bail!("outer join whose optional side is not a single table");
                    }
                    // the joined tables are visible to the ON conditions
                    let n0 = self.frame().binders.len();
                    let vis: Vec<Binder> = out.iter().cloned().chain(std::iter::once(right[0].clone())).collect();
                    self.frame().binders.extend(vis);
                    let mut conds: Vec<Expr> = j.on.iter().map(|(l, r)| l.clone().eq(r.clone())).collect();
                    if let Some(f) = &j.filter {
                        conds.extend(split_and(f));
                    }
                    let mut codes = vec![];
                    for c in &conds {
                        let v = self.expr(c, &Ctx::Row { tuple: None })?;
                        if v.ty != Ty::Bool || v.nonlit_div {
                            bail!("LEFT JOIN condition {c}");
                        }
                        codes.push(bool_code(&v));
                    }
                    self.frame().binders.truncate(n0);
                    let mut b = right.pop().unwrap();
                    b.optional = true;
                    b.on = codes;
                    out.push(b);
                }
                other => bail!("{other} join (only INNER, LEFT and RIGHT joins are supported)"),
            },
            other => bail!("unsupported FROM item: {}", other.display()),
        }
        Ok(())
    }

    fn rel_binder(&mut self, alias: &str, plan: &LogicalPlan) -> Res<Binder> {
        let rel = self.derived(alias, plan)?;
        let var = self.var_for(alias);
        self.frame().uses_params = true;
        Ok(Binder { alias: alias.into(), var, src: Src::Rel(rel), optional: false, on: vec![] })
    }

    fn base_binder(&mut self, alias: &str, table: &str) -> Res<Binder> {
        if !self.sch.ddl_cols.contains_key(table) {
            bail!("table {table} is not in the schema");
        }
        self.used.entry(table.into()).or_default();
        let var = self.var_for(alias);
        Ok(Binder { alias: alias.into(), var, src: Src::Base(table.into()), optional: false, on: vec![] })
    }

    // ── columns ──
    fn column(&mut self, c: &Column, outer_only: bool, ctx: &Ctx) -> Res<Val> {
        let alias = match &c.relation {
            Some(r) => r.table().to_string(),
            None => bail!("unqualified column {} (expected a resolved table column)", c.name),
        };
        let nf = self.frames.len();
        let start = if outer_only { nf - 1 } else { nf };
        let mut found = None;
        for d in (0..start).rev() {
            if let Some(b) = self.frames[d].binders.iter().find(|b| b.alias == alias) {
                found = Some((d, b.clone()));
                break;
            }
        }
        let Some((depth, b)) = found else { bail!("unknown table alias {alias}") };
        for d in depth + 1..nf {
            if !self.frames[d].outer.iter().any(|o| o.var == b.var) {
                self.frames[d].outer.push(b.clone());
            }
        }
        let local = depth + 1 == nf;
        let base = match ctx {
            Ctx::Row { tuple: Some(t) } if local => format!("{t}.{}", b.var),
            _ => b.var.clone(),
        };
        let (field, ty, ch, nullable) = match &b.src {
            Src::Base(t) => {
                let key = (t.clone(), c.name.clone());
                let (ty, nullable) = self.sch.col_ty.get(&key).cloned().ok_or_else(|| format!("column {t}.{} is not in the schema", c.name))?;
                let ty = ty.ok_or_else(|| format!("column {t}.{} has an unsupported type", c.name))?;
                self.used.get_mut(t).unwrap().insert(key.1.clone());
                (sanitize(&key.1), ty, self.sch.char_cols.contains(&key), nullable)
            }
            Src::Rel(r) => {
                let col = r.cols.iter().find(|x| x.0 == c.name).ok_or_else(|| format!("column {alias}.{} not found", c.name))?;
                (col.1.clone(), col.2, col.3, col.4)
            }
        };
        if nullable && ty == Ty::Rat {
            bail!("column {alias}.{} is a NULL-able fraction", c.name);
        }
        if b.optional || nullable {
            // NULL when the LEFT JOIN side is missing, or the column is NULL
            let guards: Vec<String> = [b.optional.then(|| format!("{base} is Some")), nullable.then(|| format!("{base}{}.{field} is Some", if b.optional { "->0" } else { "" }))].into_iter().flatten().collect();
            let opt = if b.optional { format!("{base}->0.{field}") } else { format!("{base}.{field}") };
            let code = if nullable { format!("{opt}->0") } else { opt };
            return Ok(Val { guard: Some(guards.join(" && ")), char_col: ch, ..Val::atom(code, ty) });
        }
        if ty == Ty::Rat {
            // a fraction column of a derived table (AVG, or division by a literal): den > 0
            return Ok(Val { den: Some(format!("{base}.{field}.den")), ..Val::atom(format!("{base}.{field}.num"), ty) });
        }
        Ok(Val { char_col: ch, ..Val::atom(format!("{base}.{field}"), ty) })
    }

    fn literal(&self, s: &ScalarValue) -> Res<Val> {
        Ok(match s {
            ScalarValue::Int8(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::Int16(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::Int32(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::Int64(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::UInt8(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::UInt16(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::UInt32(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::UInt64(Some(v)) => Val::int(*v as i128, Ty::Int),
            ScalarValue::Decimal128(Some(v), _, s) if *s >= 0 => Val::int(*v, Ty::Dec(*s as u32)),
            ScalarValue::Float64(Some(x)) => Val::atom(f64_code(*x)?, Ty::Flt(1)),
            ScalarValue::Float32(Some(x)) => Val::atom(f64_code(*x as f64)?, Ty::Flt(1)),
            ScalarValue::Utf8(Some(v)) | ScalarValue::Utf8View(Some(v)) | ScalarValue::LargeUtf8(Some(v)) => {
                Val { str_lit: Some(v.clone()), ..Val::atom(bytes_lit(v), Ty::Text) }
            }
            ScalarValue::Boolean(Some(b)) => Val::atom(b.to_string(), Ty::Bool),
            ScalarValue::Date32(Some(d)) => date_val(*d as i64),
            ScalarValue::TimestampNanosecond(Some(v), None) => Val::int(*v as i128, Ty::Ts),
            ScalarValue::TimestampMicrosecond(Some(v), None) => Val::int(*v as i128 * 1000, Ty::Ts),
            ScalarValue::TimestampMillisecond(Some(v), None) => Val::int(*v as i128 * 1_000_000, Ty::Ts),
            ScalarValue::TimestampSecond(Some(v), None) => Val::int(*v as i128 * 1_000_000_000, Ty::Ts),
            ScalarValue::IntervalMonthDayNano(Some(i)) => match (i.months, i.days, i.nanoseconds) {
                (m, 0, 0) if m != 0 => Val::int(m as i128, Ty::Months),
                (0, d, 0) => Val::int(d as i128, Ty::Days),
                (0, 0, n) => Val::int(n as i128, Ty::Nanos),
                _ => bail!("interval mixing months, days and time"),
            },
            ScalarValue::IntervalYearMonth(Some(m)) => Val::int(*m as i128, Ty::Months),
            ScalarValue::IntervalDayTime(Some(i)) => match (i.days, i.milliseconds) {
                (d, 0) => Val::int(d as i128, Ty::Days),
                (0, ms) => Val::int(ms as i128 * 1_000_000, Ty::Nanos),
                _ => bail!("interval mixing days and time"),
            },
            other if other.is_null() => bail!("NULL literal (three-valued logic is not modelled)"),
            other => bail!("unsupported literal {other:?}"),
        })
    }

    fn placeholder(&mut self, p: &Placeholder, hint: Option<Ty>) -> Res<Val> {
        let n: u32 = p
            .id
            .strip_prefix('$')
            .and_then(|x| x.parse().ok())
            .ok_or_else(|| format!("parameter {} (only $1, $2, ... are supported)", p.id))?;
        self.frame().uses_params = true;
        let ty = hint.ok_or_else(|| format!("cannot determine the type of parameter {}", p.id))?;
        if matches!(ty, Ty::Bool | Ty::Days | Ty::Months | Ty::Nanos | Ty::Rat) || matches!(ty, Ty::Flt(p) if p != 1) {
            bail!("parameter {} would have type {:?}", p.id, ty);
        }
        if let Some((t0, _, _)) = self.params.get(&n) {
            if *t0 != ty {
                bail!("parameter {} is used at two types", p.id);
            }
        } else {
            self.params.insert(n, (ty, false, false));
        }
        Ok(Val { param: Some(n), ..Val::atom(format!("params.p{n}"), ty) })
    }

    fn hint_for(op: Operator, other: &Val) -> Option<Ty> {
        match op {
            Operator::Multiply if matches!(other.ty, Ty::Days | Ty::Months | Ty::Nanos) => Some(Ty::Int),
            // a parameter multiplying a number is a DECIMAL with MULT_PARAM_SCALE places (exact for any value
            // with at most that many decimals: TPC-H Q11's 0.0001/SF down to SF 100000)
            Operator::Multiply if matches!(other.ty, Ty::Int | Ty::Dec(_)) => Some(Ty::Dec(MULT_PARAM_SCALE)),
            Operator::Multiply | Operator::Divide => None,
            Operator::Plus | Operator::Minus if matches!(other.ty, Ty::Date | Ty::Ts) => None,
            _ if other.ty == Ty::Rat => None,
            _ => Some(other.ty),
        }
    }

    // ── expressions ──
    fn expr(&mut self, e: &Expr, ctx: &Ctx) -> Res<Val> {
        if let Ctx::Group(g) = ctx {
            if let Some(v) = self.group_ref(e, g)? {
                return Ok(v);
            }
        }
        match e {
            Expr::Alias(Alias { expr, .. }) => self.expr(expr, ctx),
            Expr::Column(c) => match ctx {
                Ctx::Group(_) => bail!("column {c} is neither grouped nor aggregated"),
                _ => self.column(c, false, ctx),
            },
            Expr::OuterReferenceColumn(_, c) => self.column(c, true, ctx),
            Expr::Literal(s, _) => self.literal(s),
            Expr::Placeholder(p) => self.placeholder(p, None),
            Expr::Cast(Cast { expr, field }) => self.cast(expr, field.data_type(), ctx),
            Expr::Not(x) => {
                let v = self.expr(x, ctx)?;
                if v.ty != Ty::Bool {
                    bail!("NOT on a non-boolean ({e})");
                }
                if v.nullcmp || v.guard.is_some() {
                    bail!("NOT over a comparison with a possibly-NULL value ({e}): three-valued logic");
                }
                Ok(Val { code: format!("!{}", v.wrap(3)), prec: 3, ..v })
            }
            Expr::Negative(x) => {
                let v = self.expr(x, ctx)?;
                match v.ty {
                    Ty::Int | Ty::Dec(_) | Ty::Flt(_) | Ty::Rat => {
                        Ok(Val { code: format!("-{}", v.wrap(3)), prec: 3, int_lit: v.int_lit.map(|x| -x), ..v })
                    }
                    t => bail!("unary minus on {t:?}"),
                }
            }
            Expr::IsNotNull(x) | Expr::IsNull(x) => {
                let v = self.expr(x, ctx)?;
                let is = matches!(e, Expr::IsNotNull(_));
                let code = match &v.guard {
                    Some(g) => if is { g.clone() } else { format!("!({g})") },
                    None => is.to_string(),
                };
                Ok(Val::atom(format!("({code})"), Ty::Bool))
            }
            Expr::IsTrue(x) | Expr::IsNotFalse(x) => self.expr(x, ctx),
            Expr::IsFalse(x) | Expr::IsNotTrue(x) => self.expr(&Expr::Not(x.clone()), ctx),
            Expr::Between(Between { expr, negated, low, high }) => {
                let a = Expr::BinaryExpr(BinaryExpr::new(low.clone(), Operator::LtEq, expr.clone()));
                let b = Expr::BinaryExpr(BinaryExpr::new(expr.clone(), Operator::LtEq, high.clone()));
                let both = Expr::BinaryExpr(BinaryExpr::new(Box::new(a), Operator::And, Box::new(b)));
                if *negated { self.expr(&Expr::Not(Box::new(both)), ctx) } else { self.expr(&both, ctx) }
            }
            Expr::InList(InList { expr, list, negated }) => {
                if list.is_empty() {
                    bail!("empty IN list");
                }
                let mut acc: Option<Expr> = None;
                for x in list {
                    let eq = Expr::BinaryExpr(BinaryExpr::new(expr.clone(), Operator::Eq, Box::new(x.clone())));
                    acc = Some(match acc {
                        None => eq,
                        Some(a) => Expr::BinaryExpr(BinaryExpr::new(Box::new(a), Operator::Or, Box::new(eq))),
                    });
                }
                let ors = acc.unwrap();
                if *negated { self.expr(&Expr::Not(Box::new(ors)), ctx) } else { self.expr(&ors, ctx) }
            }
            Expr::Like(Like { negated, expr, pattern, escape_char, case_insensitive }) => {
                if escape_char.is_some() || *case_insensitive {
                    bail!("LIKE with ESCAPE / ILIKE");
                }
                let v = self.expr(expr, ctx)?;
                if v.ty != Ty::Text {
                    bail!("LIKE on {:?}", v.ty);
                }
                if let Expr::Placeholder(ph) = strip_alias(pattern) {
                    // a text parameter as the pattern: like_match(value, params.pN).  The parameter is
                    // matched byte for byte: a pattern for a CHAR(n) column would need its trailing
                    // spaces trimmed (as for a literal), which a parameter cannot express, so refuse it.
                    if v.char_col {
                        bail!("LIKE with a parameter pattern on a CHAR(n) column (the pattern's trailing spaces cannot be trimmed)");
                    }
                    let pv = self.placeholder(ph, Some(Ty::Text))?;
                    let code = format!("like_match({}, {})", v.code, pv.code);
                    let code = if *negated { format!("!{code}") } else { code };
                    let mut out = Val::atom(code, Ty::Bool).with_meta(&[&v]);
                    if out.guard.is_some() && *negated {
                        bail!("NOT LIKE on a possibly-NULL value");
                    }
                    out.nullcmp |= out.guard.is_some();
                    self.notes.insert("LIKE with a parameter pattern: the pattern uses '%' and '_' (ASCII text, bytes = characters).".into());
                    return Ok(out);
                }
                let Expr::Literal(ScalarValue::Utf8(Some(p)) | ScalarValue::Utf8View(Some(p)), _) = strip_alias(pattern) else {
                    bail!("LIKE with a non-literal, non-parameter pattern");
                };
                if p.contains('_') {
                    self.notes.insert("LIKE patterns use '_': the text they match is ASCII (bytes = characters).".into());
                }
                let p = if v.char_col { p.trim_end_matches(' ').to_string() } else { p.clone() };
                let code = format!("like_match({}, {})", v.code, bytes_lit(&p));
                let code = if *negated { format!("!{code}") } else { code };
                let mut out = Val::atom(code, Ty::Bool).with_meta(&[&v]);
                if out.guard.is_some() && *negated {
                    bail!("NOT LIKE on a possibly-NULL value");
                }
                out.nullcmp |= out.guard.is_some();
                Ok(out)
            }
            Expr::Case(Case { expr, when_then_expr, else_expr }) => {
                let Some(els) = else_expr else { bail!("CASE without ELSE (yields NULL)") };
                let mut branches = vec![];
                for (w, t) in when_then_expr {
                    let cond = match expr {
                        Some(base) => Expr::BinaryExpr(BinaryExpr::new(base.clone(), Operator::Eq, w.clone())),
                        None => (**w).clone(),
                    };
                    let c = self.expr(&cond, ctx)?;
                    if c.ty != Ty::Bool {
                        bail!("CASE WHEN on a non-boolean");
                    }
                    branches.push((c, self.expr(t, ctx)?));
                }
                let mut ev = self.expr(els, ctx)?;
                for (_, v) in &branches {
                    let (_, e2) = unify(v, &ev).map_err(|m| format!("CASE branches: {m}"))?;
                    ev = e2;
                }
                let mut vals = vec![];
                for (c, v) in branches {
                    let (v2, _) = unify(&v, &ev).map_err(|m| format!("CASE branches: {m}"))?;
                    vals.push((c, v2));
                }
                for v in vals.iter().map(|x| &x.1).chain(std::iter::once(&ev)) {
                    if v.approx.is_some() || v.ty == Ty::Rat || v.guard.is_some() {
                        bail!("CASE yielding a computed float / fraction / NULL-able value");
                    }
                }
                let mut code = ev.code.clone();
                let mut parts: Vec<Val> = vec![ev.clone()];
                for (c, v) in vals.iter().rev() {
                    code = format!("if {} {{ {} }} else {{ {} }}", bool_code(c), v.code, code);
                    parts.push(Val { guard: None, ..c.clone() });
                    parts.push(v.clone());
                }
                let refs: Vec<&Val> = parts.iter().collect();
                let mut out = Val { code: format!("({code})"), prec: 3, ..Val::atom(String::new(), ev.ty) }.with_meta(&refs);
                out.nullcmp = false;
                out.char_col = ev.char_col && vals.iter().all(|(_, v)| v.char_col);
                Ok(out)
            }
            Expr::ScalarFunction(ScalarFunction { func, args }) => self.function(&func.name().to_lowercase(), args, ctx),
            Expr::Exists(Exists { subquery, negated }) => {
                let call = self.exists_sub(subquery, ctx)?;
                Ok(Val::atom(if *negated { format!("!{call}") } else { call }, Ty::Bool))
            }
            Expr::InSubquery(InSubquery { expr, subquery, negated }) => {
                let x = self.expr(expr, ctx)?;
                if x.guard.is_some() {
                    bail!("IN with a NULL-able value");
                }
                let call = self.in_sub(subquery, &x, ctx)?;
                Ok(Val::atom(if *negated { format!("!{call}") } else { call }, Ty::Bool).with_meta(&[&x]))
            }
            Expr::ScalarSubquery(sq) => self.scalar_sub(sq, ctx),
            Expr::SetComparison(sc) => {
                let x = self.expr(&sc.expr, ctx)?;
                if x.guard.is_some() {
                    bail!("ALL / ANY with a NULL-able value");
                }
                let all = matches!(sc.quantifier, datafusion::logical_expr::expr::SetQuantifier::All);
                let call = self.quantified_sub(&sc.subquery, &x, sc.op, all, ctx)?;
                Ok(Val::atom(call, Ty::Bool).with_meta(&[&x]))
            }
            Expr::BinaryExpr(BinaryExpr { left, op, right }) => self.binary(e, left, *op, right, ctx),
            Expr::AggregateFunction(_) => bail!("aggregate {e} outside a grouped SELECT / HAVING"),
            other => bail!("unsupported expression: {other}"),
        }
    }

    fn cast(&mut self, x: &Expr, to: &DataType, ctx: &Ctx) -> Res<Val> {
        if let Expr::Literal(ScalarValue::Utf8(Some(s)) | ScalarValue::Utf8View(Some(s)), _) = strip_alias(x) {
            match to {
                DataType::Date32 => return Ok(date_val(parse_date(s)?)),
                DataType::Timestamp(TimeUnit::Nanosecond | TimeUnit::Microsecond | TimeUnit::Millisecond | TimeUnit::Second, None) => {
                    let n = parse_ts(s)?;
                    return Ok(Val { int_lit: Some(n), ..Val::atom(format!("{n} /* TIMESTAMP '{s}' */"), Ty::Ts) });
                }
                _ => {}
            }
        }
        let v = self.expr(x, ctx)?;
        let target = ty_of(to).ok_or_else(|| format!("CAST to {to}"))?;
        match (v.ty, target) {
            (a, b) if a == b => Ok(v),
            (Ty::Int, Ty::Dec(s)) => Ok(Val { ty: Ty::Dec(s), ..rescale(&v, 0, s) }),
            (Ty::Dec(a), Ty::Dec(b)) if b >= a => Ok(Val { ty: Ty::Dec(b), ..rescale(&v, a, b) }),
            (Ty::Int | Ty::Dec(_), Ty::Flt(_)) => to_flt(&v),
            (Ty::Date, Ty::Ts) => Ok(Val { code: format!("{} * {NS_PER_DAY}", v.wrap(2)), prec: 2, ty: Ty::Ts, int_lit: v.int_lit.map(|d| d * NS_PER_DAY), ..v }),
            (a, b) => bail!("CAST from {a:?} to {b:?}"),
        }
    }

    fn function(&mut self, name: &str, args: &[Expr], ctx: &Ctx) -> Res<Val> {
        match name {
            "date_part" | "datepart" | "extract" => {
                let Expr::Literal(ScalarValue::Utf8(Some(part)) | ScalarValue::Utf8View(Some(part)), _) = strip_alias(&args[0]) else {
                    bail!("EXTRACT with a non-literal field");
                };
                let v = self.expr(&args[1], ctx)?;
                let day = match v.ty {
                    Ty::Date => v.code.clone(),
                    Ty::Ts => format!("{} / {NS_PER_DAY}", v.wrap(2)),
                    t => bail!("EXTRACT from {t:?}"),
                };
                let f = match part.to_uppercase().as_str() {
                    "YEAR" => "civil_year",
                    "MONTH" => "civil_month",
                    "DAY" => "civil_day",
                    p => bail!("EXTRACT({p})"),
                };
                Ok(Val::atom(format!("{f}({day})"), Ty::Int).with_meta(&[&v]))
            }
            "substr" | "substring" => {
                let v = self.expr(&args[0], ctx)?;
                if v.ty != Ty::Text {
                    bail!("SUBSTRING of {:?}", v.ty);
                }
                let lit = |e: &Expr| -> Res<i128> {
                    match strip_alias(e) {
                        Expr::Literal(ScalarValue::Int64(Some(n)), _) => Ok(*n as i128),
                        other => bail!("SUBSTRING with a non-literal position ({other})"),
                    }
                };
                let start = lit(&args[1])?;
                if args.len() < 3 {
                    bail!("SUBSTRING without FOR");
                }
                let len = lit(&args[2])?;
                if len < 0 {
                    bail!("SUBSTRING with a negative length (an error in SQL)");
                }
                self.notes.insert("SUBSTRING is applied to ASCII text (bytes = characters).".into());
                Ok(Val::atom(format!("sql_substr({}, {start}, {len})", v.code), Ty::Text).with_meta(&[&v]))
            }
            "abs" => {
                let v = self.expr(&args[0], ctx)?;
                if !matches!(v.ty, Ty::Int | Ty::Dec(_)) {
                    bail!("ABS of {:?}", v.ty);
                }
                Ok(Val { code: format!("abs({})", v.code), prec: 3, int_lit: v.int_lit.map(|x| x.abs()), ..v })
            }
            "upper" | "lower" => {
                let v = self.expr(&args[0], ctx)?;
                if v.ty != Ty::Text {
                    bail!("{name} of {:?}", v.ty);
                }
                self.notes.insert("UPPER / LOWER change ASCII letters only (the text is ASCII, or the engine does the same).".into());
                let f = if name == "upper" { "ascii_upper" } else { "ascii_lower" };
                Ok(Val::atom(format!("{f}({})", v.code), Ty::Text).with_meta(&[&v]))
            }
            "length" | "char_length" | "character_length" | "octet_length" => {
                let v = self.expr(&args[0], ctx)?;
                if v.ty != Ty::Text {
                    bail!("{name} of {:?}", v.ty);
                }
                if name != "octet_length" {
                    self.notes.insert("LENGTH counts bytes (= characters for ASCII text).".into());
                }
                Ok(Val::atom(format!("{}.len() as int", v.wrap(3)), Ty::Int).with_meta(&[&v]))
            }
            "concat" => {
                let mut acc: Option<Val> = None;
                for a in args {
                    let v = self.expr(a, ctx)?;
                    if v.ty != Ty::Text || v.guard.is_some() {
                        bail!("CONCAT of a non-text / NULL-able value");
                    }
                    acc = Some(match acc {
                        None => v,
                        Some(x) => Val { prec: 1, ..Val::atom(format!("{} + {}", x.wrap(1), v.wrap(2)), Ty::Text) }.with_meta(&[&x, &v]),
                    });
                }
                acc.ok_or_else(|| "CONCAT()".to_string())
            }
            "coalesce" => {
                // the first argument that is not NULL (columns are never NULL)
                let mut vals = vec![];
                for a in args {
                    vals.push(self.expr(a, ctx)?);
                }
                let mut out = vals.pop().ok_or("COALESCE()")?;
                while let Some(v) = vals.pop() {
                    let (a, b) = unify(&v, &out).map_err(|m| format!("COALESCE: {m}"))?;
                    if a.ty == Ty::Rat || a.approx.is_some() {
                        bail!("COALESCE of a fraction / computed float");
                    }
                    out = match &a.guard {
                        None => Val { guard: None, ..a.clone() },
                        Some(g) => Val { code: format!("(if {g} {{ {} }} else {{ {} }})", a.code, b.code), prec: 3, guard: b.guard.clone(), ..a.clone() },
                    };
                }
                Ok(out)
            }
            "round" => {
                let v = self.expr(&args[0], ctx)?;
                let k: i128 = match args.get(1).map(strip_alias) {
                    None => 0,
                    Some(Expr::Literal(ScalarValue::Int64(Some(n)), _)) => *n as i128,
                    Some(other) => bail!("ROUND with a non-literal precision ({other})"),
                };
                match v.ty {
                    Ty::Int if k >= 0 => Ok(v),
                    Ty::Dec(s) if k >= 0 && (k as u32) <= s => {
                        let d = s - k as u32;
                        if d == 0 {
                            return Ok(v);
                        }
                        Ok(Val { code: format!("round_half_away({}, {d})", v.code), prec: 3, int_lit: None, ty: Ty::Dec(k as u32), ..v })
                    }
                    t => bail!("ROUND of {t:?} to {k} places"),
                }
            }
            other => bail!("function {other}"),
        }
    }

    fn binary(&mut self, whole: &Expr, l: &Expr, op: Operator, r: &Expr, ctx: &Ctx) -> Res<Val> {
        let (lv, rv) = match (strip_alias(l), strip_alias(r)) {
            (Expr::Placeholder(p), _) => {
                let rv = self.expr(r, ctx)?;
                (self.placeholder(p, Self::hint_for(op, &rv))?, rv)
            }
            (_, Expr::Placeholder(p)) => {
                let lv = self.expr(l, ctx)?;
                let h = Self::hint_for(op, &lv);
                (lv, self.placeholder(p, h)?)
            }
            _ => (self.expr(l, ctx)?, self.expr(r, ctx)?),
        };
        use Operator::*;
        match op {
            And | Or => {
                if lv.ty != Ty::Bool || rv.ty != Ty::Bool {
                    bail!("AND/OR on non-booleans ({whole})");
                }
                let s = if op == And { "&&" } else { "||" };
                let code = format!("({} {s} {})", bool_code(&lv), bool_code(&rv));
                let nullish = lv.nullcmp || rv.nullcmp || lv.guard.is_some() || rv.guard.is_some();
                let mut v = Val { prec: 3, ..Val::atom(code, Ty::Bool) }.with_meta(&[&lv, &rv]);
                v.nullcmp = nullish;
                v.guard = None;
                Ok(v)
            }
            Eq | NotEq | Lt | LtEq | Gt | GtEq => self.compare(&whole.to_string(), lv, op, rv),
            Divide if lv.ty == Ty::Int && rv.ty == Ty::Int => {
                bail!("integer / integer ({whole}): its meaning differs between engines (PostgreSQL truncates, DuckDB and MySQL do not); write CAST(.. AS DECIMAL) or use DIV")
            }
            Plus | Minus | Multiply | Divide => self.arith(whole, lv, op, rv, ctx),
            IntegerDivide | Modulo => {
                if lv.ty != Ty::Int || rv.ty != Ty::Int {
                    bail!("{op} on {:?} and {:?} ({whole})", lv.ty, rv.ty);
                }
                if rv.int_lit == Some(0) {
                    bail!("division by zero ({whole})");
                }
                let f = if op == Modulo { "trunc_rem" } else { "trunc_div" };
                let mut v = Val::atom(format!("{f}({}, {})", lv.code, rv.code), Ty::Int).with_meta(&[&lv, &rv]);
                if rv.int_lit.is_none() {
                    v.nonlit_div = true;
                    v.den_conds.push(format!("{} != 0", rv.wrap(1)));
                }
                Ok(v)
            }
            StringConcat => {
                if lv.ty != Ty::Text || rv.ty != Ty::Text {
                    bail!("|| on {:?} and {:?}", lv.ty, rv.ty);
                }
                Ok(Val { prec: 1, ..Val::atom(format!("{} + {}", lv.wrap(1), rv.wrap(2)), Ty::Text) }.with_meta(&[&lv, &rv]))
            }
            _ => bail!("unsupported operator {op} ({whole})"),
        }
    }

    fn compare(&mut self, whole: &str, mut lv: Val, op: Operator, mut rv: Val) -> Res<Val> {
        use Operator::*;
        for (p, other) in [(&lv, &rv), (&rv, &lv)] {
            if let (Some(n), true) = (p.param, other.char_col) {
                self.params.get_mut(&n).unwrap().1 = true;
            }
        }
        if lv.approx.is_some() || rv.approx.is_some() {
            bail!("comparison of a computed float (rounding decides it): {whole}");
        }
        if lv.nonlit_div || rv.nonlit_div {
            bail!("comparison involving division by a non-literal: {whole}");
        }
        // a text literal compared with a date / timestamp
        let (lt0, rt0) = (lv.ty, rv.ty);
        for (a, b) in [(&mut lv, rt0), (&mut rv, lt0)] {
            if let (Some(s), Ty::Date) = (a.str_lit.clone(), b) {
                *a = date_val(parse_date(&s)?);
            } else if let (Some(s), Ty::Ts) = (a.str_lit.clone(), b) {
                let n = parse_ts(&s)?;
                *a = Val { int_lit: Some(n), ..Val::atom(format!("{n} /* TIMESTAMP '{s}' */"), Ty::Ts) };
            }
        }
        let trim = |a: &mut Val, other_char: bool| {
            if let (Some(s), true) = (a.str_lit.clone(), other_char) {
                let t = s.trim_end_matches(' ').to_string();
                a.code = bytes_lit(&t);
                a.str_lit = Some(t);
            }
        };
        let (lc, rc) = (lv.char_col, rv.char_col);
        trim(&mut lv, rc);
        trim(&mut rv, lc);
        let sym = match op { Eq => "==", NotEq => "!=", Lt => "<", LtEq => "<=", Gt => ">", _ => ">=" };
        let code = if lv.ty == Ty::Rat || rv.ty == Ty::Rat {
            let a = as_ratio(&lv).map_err(|m| format!("{m} in {whole}"))?;
            let b = as_ratio(&rv).map_err(|m| format!("{m} in {whole}"))?;
            match op {
                Eq => format!("ratio_eq({}, {}, {}, {})", a.0, a.1, b.0, b.1),
                NotEq => format!("!ratio_eq({}, {}, {}, {})", a.0, a.1, b.0, b.1),
                Lt => format!("ratio_lt({}, {}, {}, {})", a.0, a.1, b.0, b.1),
                Gt => format!("ratio_lt({}, {}, {}, {})", b.0, b.1, a.0, a.1),
                LtEq => format!("!ratio_lt({}, {}, {}, {})", b.0, b.1, a.0, a.1),
                _ => format!("!ratio_lt({}, {}, {}, {})", a.0, a.1, b.0, b.1),
            }
        } else {
            let (a, b) = unify(&lv, &rv).map_err(|m| format!("{m} in {whole}"))?;
            match (a.ty, op) {
                (Ty::Text, Eq) => format!("{} == {}", a.code, b.code),
                (Ty::Text, NotEq) => format!("{} != {}", a.code, b.code),
                (Ty::Text, Lt) => format!("bytes_lt({}, {})", a.code, b.code),
                (Ty::Text, Gt) => format!("bytes_lt({}, {})", b.code, a.code),
                (Ty::Text, LtEq) => format!("!bytes_lt({}, {})", b.code, a.code),
                (Ty::Text, GtEq) => format!("!bytes_lt({}, {})", a.code, b.code),
                (Ty::Bool, Eq) => format!("{} == {}", a.wrap(1), b.wrap(1)),
                (Ty::Bool, NotEq) => format!("{} != {}", a.wrap(1), b.wrap(1)),
                (Ty::Bool, _) => format!("(if {} {{ 1int }} else {{ 0 }}) {sym} (if {} {{ 1int }} else {{ 0 }})", a.code, b.code),
                (Ty::Days | Ty::Months | Ty::Nanos, _) => bail!("comparison of intervals ({whole})"),
                _ => format!("{} {sym} {}", a.wrap(1), b.wrap(1)),
            }
        };
        let mut v = Val { prec: 0, ..Val::atom(code, Ty::Bool) }.with_meta(&[&lv, &rv]);
        v.nullcmp |= v.guard.is_some();
        Ok(v)
    }

    fn arith(&mut self, whole: &Expr, lv: Val, op: Operator, rv: Val, ctx: &Ctx) -> Res<Val> {
        use Operator::*;
        let sym = match op { Plus => "+", Minus => "-", Multiply => "*", _ => "/" };
        let meta = |v: Val| v.with_meta(&[&lv, &rv]);
        // dates, timestamps, intervals
        match (lv.ty, rv.ty, op) {
            (Ty::Date | Ty::Ts, Ty::Months, Plus | Minus) | (Ty::Months, Ty::Date | Ty::Ts, Plus) => {
                let (d, m) = if lv.ty == Ty::Months { (&rv, &lv) } else { (&lv, &rv) };
                let (Some(dv), Some(mv)) = (d.int_lit, m.int_lit) else {
                    bail!("month arithmetic on a non-literal date ({whole})");
                };
                let mv = if op == Minus { -mv } else { mv };
                return Ok(match d.ty {
                    Ty::Date => date_val(add_months(dv as i64, mv as i64)),
                    _ => {
                        let day = dv.div_euclid(NS_PER_DAY);
                        let rest = dv.rem_euclid(NS_PER_DAY);
                        Val::int(add_months(day as i64, mv as i64) as i128 * NS_PER_DAY + rest, Ty::Ts)
                    }
                });
            }
            (Ty::Date, Ty::Days | Ty::Int, Plus | Minus) | (Ty::Days | Ty::Int, Ty::Date, Plus) => {
                let (d, n) = if lv.ty == Ty::Date { (&lv, &rv) } else { (&rv, &lv) };
                if let (Some(a), Some(b)) = (d.int_lit, n.int_lit) {
                    return Ok(date_val((if op == Minus { a - b } else { a + b }) as i64));
                }
                return Ok(meta(Val { prec: 1, ..Val::atom(format!("{} {sym} {}", d.wrap(1), n.wrap(2)), Ty::Date) }));
            }
            (Ty::Date, Ty::Date, Minus) => {
                return Ok(meta(Val { prec: 1, ..Val::atom(format!("{} - {}", lv.wrap(1), rv.wrap(2)), Ty::Int) }));
            }
            (Ty::Ts, Ty::Days | Ty::Nanos, Plus | Minus) | (Ty::Days | Ty::Nanos, Ty::Ts, Plus) => {
                let (t, n) = if lv.ty == Ty::Ts { (&lv, &rv) } else { (&rv, &lv) };
                let ns = if n.ty == Ty::Days { rescale_k(n, NS_PER_DAY) } else { n.clone() };
                if let (Some(a), Some(b)) = (t.int_lit, ns.int_lit) {
                    return Ok(Val::int(if op == Minus { a - b } else { a + b }, Ty::Ts));
                }
                return Ok(meta(Val { prec: 1, ..Val::atom(format!("{} {sym} {}", t.wrap(1), ns.wrap(2)), Ty::Ts) }));
            }
            (Ty::Days | Ty::Months | Ty::Nanos, Ty::Int, Multiply) | (Ty::Int, Ty::Days | Ty::Months | Ty::Nanos, Multiply) => {
                let (d, k) = if matches!(lv.ty, Ty::Days | Ty::Months | Ty::Nanos) { (&lv, &rv) } else { (&rv, &lv) };
                if let Some(n) = k.param {
                    if d.ty == Ty::Days {
                        self.params.get_mut(&n).unwrap().2 = true;
                    }
                }
                if let (Some(a), Some(b)) = (d.int_lit, k.int_lit) {
                    return Ok(Val::int(a * b, d.ty));
                }
                if d.ty == Ty::Months {
                    bail!("a non-literal number of months ({whole})");
                }
                if d.int_lit == Some(1) {
                    return Ok(Val { ty: d.ty, ..k.clone() });
                }
                return Ok(meta(Val { prec: 2, ..Val::atom(format!("{} * {}", d.wrap(2), k.wrap(3)), d.ty) }));
            }
            _ => {}
        }
        // floats
        if matches!(lv.ty, Ty::Flt(_)) || matches!(rv.ty, Ty::Flt(_)) {
            if let Ctx::Group(_) = ctx {
                bail!("arithmetic on float aggregates ({whole})");
            }
            if op == Divide {
                bail!("float division ({whole})");
            }
            let (a, b) = (to_flt(&lv)?, to_flt(&rv)?);
            let (pa, pb) = (flt_p(a.ty), flt_p(b.ty));
            let absc = |v: &Val| -> String { v.approx.as_ref().map(|x| x.0.clone()).unwrap_or(format!("abs({})", v.code)) };
            let kc = |v: &Val| -> String { v.approx.as_ref().map(|x| x.1.clone()).unwrap_or("0".into()) };
            let k = simplify_k(&format!("{} + {} + 1", kc(&a), kc(&b)));
            let (code, abs, p) = if op == Multiply {
                (format!("{} * {}", a.wrap(2), b.wrap(3)), format!("({}) * ({})", absc(&a), absc(&b)), pa + pb)
            } else {
                let p = pa.max(pb);
                let (ca, pra) = flt_rescale(&a.code, a.prec, pa, p);
                let (cb, prb) = flt_rescale(&b.code, b.prec, pb, p);
                let (aa, _) = flt_rescale(&absc(&a), 0, pa, p);
                let (ab, _) = flt_rescale(&absc(&b), 0, pb, p);
                let ca = if pra >= 1 { ca } else { format!("({ca})") };
                let cb = if prb >= 2 { cb } else { format!("({cb})") };
                (format!("{ca} {sym} {cb}"), format!("{aa} + {ab}"), p)
            };
            let prec = if op == Multiply { 2 } else { 1 };
            return Ok(meta(Val { prec, approx: Some((abs, k)), ..Val::atom(code, Ty::Flt(p)) }));
        }
        // exact numbers
        let num = |t: Ty| matches!(t, Ty::Int | Ty::Dec(_) | Ty::Rat);
        if !num(lv.ty) || !num(rv.ty) {
            bail!("arithmetic {:?} {sym} {:?} is not supported ({whole})", lv.ty, rv.ty);
        }
        if op == Divide || lv.ty == Ty::Rat || rv.ty == Ty::Rat {
            let (an, ad) = as_ratio(&lv)?;
            let (bn, bd) = as_ratio(&rv)?;
            let (num, den) = match op {
                Plus | Minus => (format!("{an} * {bd} {sym} {bn} * {ad}"), format!("{ad} * {bd}")),
                Multiply => (format!("{an} * {bn}"), format!("{ad} * {bd}")),
                _ => {
                    if rv.int_lit == Some(0) {
                        bail!("division by zero ({whole})");
                    }
                    (format!("{an} * {bd}"), format!("{ad} * {bn}"))
                }
            };
            let mut v = meta(Val { den: Some(simp_mul(&den)), prec: 3, ..Val::atom(format!("({})", simp_mul(&num)), Ty::Rat) });
            if op == Divide && rv.int_lit.is_none() {
                v.nonlit_div = true;
                let c = format!("{} != 0", rv.wrap(1));
                if !v.den_conds.contains(&c) {
                    v.den_conds.push(c);
                }
            }
            return Ok(v);
        }
        let (sa, sb) = (scale_of(lv.ty).unwrap(), scale_of(rv.ty).unwrap());
        let (ty, a, b) = match op {
            Plus | Minus => {
                let s = sa.max(sb);
                let ty = if lv.ty == Ty::Int && rv.ty == Ty::Int { Ty::Int } else { Ty::Dec(s) };
                (ty, rescale(&lv, sa, s), rescale(&rv, sb, s))
            }
            _ => {
                let ty = if lv.ty == Ty::Int && rv.ty == Ty::Int { Ty::Int } else { Ty::Dec(sa + sb) };
                (ty, lv.clone(), rv.clone())
            }
        };
        if let (Some(x), Some(y)) = (a.int_lit, b.int_lit) {
            let v = match op { Plus => x + y, Minus => x - y, _ => x * y };
            return Ok(meta(Val::int(v, ty)));
        }
        let (pl, pr) = if op == Multiply { (2, 3) } else { (1, 2) };
        let pr = if op == Multiply && b.prec == 2 { 2 } else { pr };
        let code = format!("{} {sym} {}", a.wrap(pl), b.wrap(pr));
        Ok(meta(Val { prec: if op == Multiply { 2 } else { 1 }, ..Val::atom(code, ty) }))
    }

    // ── per-group references ──
    fn group_ref(&mut self, e: &Expr, g: &Arc<GroupCtx>) -> Res<Option<Val>> {
        let e = strip_alias(e);
        let key = |i: usize| -> Val {
            let (gv, kv) = &g.keys[i];
            if kv.opt {
                return Val { char_col: kv.char_col, guard: Some(format!("{gv} is Some")), ..Val::atom(format!("{gv}->0"), kv.ty) };
            }
            Val { char_col: kv.char_col, ..Val::atom(gv.clone(), kv.ty) }
        };
        for (i, ge) in g.group_exprs.iter().enumerate() {
            if strip_alias(ge) == e {
                return Ok(Some(key(i)));
            }
        }
        if let Expr::Column(c) = e {
            if let Ok(idx) = g.schema.index_of_column(c) {
                let nk = g.group_exprs.len();
                if idx < nk {
                    return Ok(Some(key(idx)));
                }
                let ae = g.aggr_exprs[idx - nk].clone();
                return Ok(Some(self.aggregate(&ae, g)?));
            }
        }
        if let Expr::AggregateFunction(_) = e {
            return Ok(Some(self.aggregate(e, g)?));
        }
        Ok(None)
    }

    fn aggregate(&mut self, ae: &Expr, g: &Arc<GroupCtx>) -> Res<Val> {
        let ae = strip_alias(ae);
        let Expr::AggregateFunction(AggregateFunction { func, params }) = ae else { bail!("aggregate {ae}") };
        if params.filter.is_some() || !params.order_by.is_empty() || params.args.len() != 1 {
            bail!("aggregate {ae}: FILTER / ORDER BY / several arguments");
        }
        let fname = func.name().to_lowercase();
        let arg = &params.args[0];
        let tctx = Ctx::Row { tuple: Some("t".into()) };
        let tt = g.tuple_ty.clone();
        let count = format!("{})", g.call("group_count"));
        let scalar = g.scalar;
        // the values the aggregate sees: those of tuples where the argument is not NULL
        let seen = |guard: &Option<String>| match guard {
            Some(gd) => format!("{}, |t: {tt}| if {gd} {{ 1int }} else {{ 0 }})", g.call("group_sum")),
            None => count.clone(),
        };
        let vals = |v: &Val| match &v.guard {
            Some(gd) => format!("{}, |t: {tt}| {gd}, |t: {tt}| {})", g.call("group_vals_where"), v.code),
            None => format!("{}, |t: {tt}| {})", g.call("group_vals"), v.code),
        };
        let nullable = |v: Val, arg: &Val| -> Val {
            if scalar || arg.guard.is_some() { Val { guard: Some(format!("{} > 0", seen(&arg.guard))), ..v } } else { v }
        };
        if params.distinct {
            if fname != "count" {
                bail!("{fname}(DISTINCT ...)");
            }
            let v = self.expr(arg, &tctx)?;
            if v.approx.is_some() || v.ty == Ty::Rat {
                bail!("COUNT(DISTINCT {arg})");
            }
            if v.guard.is_some() {
                self.guarded_vals.insert(g.prefix.clone());
            }
            return Ok(Val::atom(format!("distinct_count({})", vals(&v)), Ty::Int));
        }
        match fname.as_str() {
            "count" => {
                if let Expr::Literal(ScalarValue::Int64(Some(_)), _) = strip_alias(arg) {
                    return Ok(Val::atom(count, Ty::Int));
                }
                let v = self.expr(arg, &tctx)?;
                Ok(Val::atom(seen(&v.guard), Ty::Int))
            }
            "sum" | "avg" => {
                let v = self.expr(arg, &tctx)?;
                if v.nonlit_div || v.ty == Ty::Rat {
                    bail!("{fname} of a fraction");
                }
                let n = seen(&v.guard);
                let skip_null = |code: &str| match &v.guard {
                    Some(gd) => format!("if {gd} {{ {code} }} else {{ 0 }}"),
                    None => code.to_string(),
                };
                let sum = |code: &str| format!("{}, |t: {tt}| {})", g.call("group_sum"), skip_null(code));
                match v.ty {
                    Ty::Int | Ty::Dec(_) => {
                        if fname == "sum" {
                            Ok(nullable(Val::atom(sum(&v.code), v.ty), &v))
                        } else {
                            let s = scale_of(v.ty).unwrap();
                            let den = if s == 0 { n } else { format!("{n} * {}", pow10(s)) };
                            Ok(nullable(Val { den: Some(den), ..Val::atom(sum(&v.code), Ty::Rat) }, &v))
                        }
                    }
                    Ty::Flt(p) => {
                        let (abs, k) = v.approx.clone().unwrap_or((format!("abs({})", v.code), "0".into()));
                        // rounded operations on a path: n - 1 additions + k per tuple (+ 1 division for AVG, added in row_ok)
                        let m = simplify_k(&format!("{n} - 1 + {k}"));
                        let mut out = Val { approx: Some((sum(&abs), m)), ..Val::atom(sum(&v.code), Ty::Flt(p)) };
                        if fname == "avg" {
                            out.den = Some(n); // AVG: the value is about code / den
                        }
                        Ok(nullable(out, &v))
                    }
                    t => bail!("{fname} over {t:?}"),
                }
            }
            "min" | "max" => {
                let v = self.expr(arg, &tctx)?;
                if v.approx.is_some() || v.ty == Ty::Rat {
                    bail!("{fname}({arg})");
                }
                if !matches!(v.ty, Ty::Int | Ty::Dec(_) | Ty::Date | Ty::Ts | Ty::Flt(1) | Ty::Text) {
                    bail!("{fname} over {:?}", v.ty);
                }
                let f = match (fname == "min", v.ty == Ty::Text) {
                    (true, false) => "min_of",
                    (false, false) => "max_of",
                    (true, true) => "min_text",
                    (false, true) => "max_text",
                };
                if v.guard.is_some() {
                    self.guarded_vals.insert(g.prefix.clone());
                }
                Ok(nullable(Val::atom(format!("{f}({})", vals(&v)), v.ty), &v))
            }
            other => bail!("aggregate function {other}"),
        }
    }

    // ── subqueries ──
    fn sub_args(&self, outer: &[Binder], ctx: &Ctx) -> String {
        outer
            .iter()
            .map(|b| {
                let local = self.frames.last().unwrap().binders.iter().any(|x| x.var == b.var);
                match ctx {
                    Ctx::Row { tuple: Some(t) } if local => format!(", {t}.{}", b.var),
                    _ => format!(", {}", b.var),
                }
            })
            .collect()
    }

    fn sub_block(&mut self, plan: &LogicalPlan, prefix: String) -> Res<Block> {
        self.frames.push(Frame::default());
        let r = self.block(plan, prefix, false);
        let f = self.frames.pop().unwrap();
        let mut b = r?;
        b.outer = f.outer.clone();
        self.absorb(&f);
        Ok(b)
    }

    fn exists_sub(&mut self, sq: &Subquery, ctx: &Ctx) -> Res<String> {
        let k = self.fresh("exists");
        let name = format!("{}exists_{k}", self.pfx);
        let mut p = sq.subquery.as_ref();
        if let LogicalPlan::Projection(pr) = p {
            p = &pr.input; // EXISTS ignores the SELECT list
        }
        if matches!(p, LogicalPlan::Aggregate(_) | LogicalPlan::Limit(_) | LogicalPlan::Distinct(_) | LogicalPlan::Sort(_))
            || matches!(p, LogicalPlan::Filter(f) if matches!(f.input.as_ref(), LogicalPlan::Aggregate(_)))
        {
            let b = self.sub_block(&sq.subquery, format!("{name}_"))?;
            let args = self.sub_args(&b.outer, ctx);
            return Ok(format!("{name}_rows(db, params{args}).len() > 0"));
        }
        // EXISTS over a join of FROM items: an index for each
        self.frames.push(Frame::default());
        let r: Res<(Vec<Binder>, Vec<String>)> = (|| {
            let mut preds = vec![];
            if let LogicalPlan::Filter(f) = p {
                preds = split_and(&f.predicate);
                p = &f.input;
            }
            let mut bs = vec![];
            let mut on = vec![];
            self.from_tree(p, &mut bs, &mut on)?;
            if bs.iter().any(|b| b.optional) {
                bail!("LEFT JOIN inside EXISTS");
            }
            preds.extend(on);
            self.frame().binders = bs.clone();
            let mut conds = vec![];
            for e in &preds {
                let v = self.expr(e, &Ctx::Row { tuple: None })?;
                if v.ty != Ty::Bool || v.nonlit_div {
                    bail!("WHERE conjunct {e}");
                }
                conds.push(bool_code(&v));
            }
            Ok((bs, conds))
        })();
        let f = self.frames.pop().unwrap();
        let (bs, conds) = r?;
        self.absorb(&f);
        let wdb = f.calls || bs.iter().any(|b| matches!(b.src, Src::Rel(_)));
        let up = f.uses_params || bs.iter().any(|b| matches!(b.src, Src::Rel(_)));
        let bargs = |bs: &[Binder]| -> String { bs.iter().map(|b| format!(", {}: {}", b.var, b.vty())).collect() };
        let pre = match (wdb, up) {
            (true, true) => "db: Db, params: Params".to_string(),
            (true, false) => "db: Db".into(),
            (false, true) => "params: Params".into(),
            (false, false) => String::new(),
        };
        let wargs = format!("{pre}{}{}", bargs(&f.outer), bargs(&bs));
        let doc = plan_doc(&sq.subquery);
        let mut w = String::new();
        writeln!(w, "/// The WHERE clause of EXISTS ({doc})").unwrap();
        writeln!(w, "pub open spec fn {name}_where({}) -> bool {{", wargs.trim_start_matches(", ")).unwrap();
        if conds.is_empty() {
            writeln!(w, "    true").unwrap();
        }
        for c in &conds {
            writeln!(w, "    &&& {c}").unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        let pa = if up { ", params: Params" } else { "" };
        writeln!(w, "/// EXISTS ({doc})").unwrap();
        writeln!(w, "pub open spec fn {name}(db: Db{pa}{}) -> bool {{", bargs(&f.outer)).unwrap();
        let idx: Vec<String> = bs.iter().map(|b| format!("i_{}: int", b.var)).collect();
        let trig: Vec<String> = bs.iter().map(|b| format!("{}[i_{}]", b.seq(), b.var)).collect();
        let ranges: Vec<String> = bs.iter().map(|b| format!("0 <= i_{} < {}.len()", b.var, b.seq())).collect();
        let mut args: Vec<String> = vec![];
        if wdb {
            args.push("db".into());
        }
        if up {
            args.push("params".into());
        }
        args.extend(f.outer.iter().map(|b| b.var.clone()));
        args.extend(bs.iter().map(|b| format!("{}[i_{}]", b.seq(), b.var)));
        writeln!(w, "    exists|{}| #![trigger {}]", idx.join(", "), trig.join(", ")).unwrap();
        writeln!(w, "        {} && {name}_where({})", ranges.join(" && "), args.join(", ")).unwrap();
        writeln!(w, "}}\n").unwrap();
        self.items.push(w);
        self.perm_exists(&name, &f.outer, &bs, wdb, up, &conds);
        let pa = if up { ", params" } else { "" };
        Ok(format!("{name}(db{pa}{})", self.sub_args(&f.outer, ctx)))
    }

    fn in_sub(&mut self, sq: &Subquery, x: &Val, ctx: &Ctx) -> Res<String> {
        if x.ty == Ty::Rat || x.approx.is_some() {
            bail!("IN with a fractional / computed-float value");
        }
        let k = self.fresh("in");
        let name = format!("{}in_{k}", self.pfx);
        let b = self.sub_block(&sq.subquery, format!("{name}_"))?;
        if b.outs.len() != 1 {
            bail!("IN (subquery) with {} columns", b.outs.len());
        }
        let col = b.outs[0].clone();
        if col.nullable || col.val.ty == Ty::Rat || col.val.approx.is_some() {
            bail!("IN (subquery) over a NULL-able / fractional / computed-float column");
        }
        let rows = format!("{name}_rows(db, params{})", b.outer_args());
        let rowv = Val { char_col: col.val.char_col, ..Val::atom(format!("{rows}[i].{}", col.field), col.val.ty) };
        let xv = Val { code: "x".into(), prec: 3, param: None, guard: None, ..x.clone() };
        let cmp = self.compare("IN (subquery)", xv, Operator::Eq, rowv)?;
        let mut w = String::new();
        writeln!(w, "/// x IN ({})", plan_doc(&sq.subquery)).unwrap();
        writeln!(w, "pub open spec fn {name}(db: Db, params: Params{}, x: {}) -> bool {{", b.outer_params(), verus_ty(x.ty)).unwrap();
        writeln!(w, "    exists|i: int| #![trigger {rows}[i]]").unwrap();
        writeln!(w, "        0 <= i < {rows}.len() && {}", cmp.code).unwrap();
        writeln!(w, "}}\n").unwrap();
        self.items.push(w);
        self.perm_in(&name, &b, verus_ty(x.ty), &cmp.code);
        self.frame().uses_params = true;
        Ok(format!("{name}(db, params{}, {})", self.sub_args(&b.outer, ctx), x.code))
    }

    /// x op ALL (subquery) / x op ANY (subquery) (columns are never NULL).
    fn quantified_sub(&mut self, sq: &Subquery, x: &Val, op: Operator, all: bool, ctx: &Ctx) -> Res<String> {
        if x.ty == Ty::Rat || x.approx.is_some() {
            bail!("ALL / ANY with a fractional / computed-float value");
        }
        self.perm.unsupported("ALL / ANY subquery");
        let k = self.fresh("quant");
        let name = format!("{}quant_{k}", self.pfx);
        let b = self.sub_block(&sq.subquery, format!("{name}_"))?;
        if b.outs.len() != 1 {
            bail!("ALL / ANY (subquery) with {} columns", b.outs.len());
        }
        let col = b.outs[0].clone();
        if col.nullable || col.val.ty == Ty::Rat || col.val.approx.is_some() {
            bail!("ALL / ANY over a NULL-able / fractional / computed-float column");
        }
        let rows = format!("{name}_rows(db, params{})", b.outer_args());
        let rowv = Val { char_col: col.val.char_col, ..Val::atom(format!("{rows}[i].{}", col.field), col.val.ty) };
        let xv = Val { code: "x".into(), prec: 3, param: None, guard: None, ..x.clone() };
        let cmp = self.compare("ALL / ANY (subquery)", xv, op, rowv)?;
        let mut w = String::new();
        writeln!(w, "/// x {op} {} ({})", if all { "ALL" } else { "ANY" }, plan_doc(&sq.subquery)).unwrap();
        writeln!(w, "pub open spec fn {name}(db: Db, params: Params{}, x: {}) -> bool {{", b.outer_params(), verus_ty(x.ty)).unwrap();
        if all {
            writeln!(w, "    forall|i: int| #![trigger {rows}[i]] 0 <= i < {rows}.len() ==> {}", cmp.code).unwrap();
        } else {
            writeln!(w, "    exists|i: int| #![trigger {rows}[i]] 0 <= i < {rows}.len() && {}", cmp.code).unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        self.items.push(w);
        self.frame().uses_params = true;
        Ok(format!("{name}(db, params{}, {})", self.sub_args(&b.outer, ctx), x.code))
    }

    fn scalar_sub(&mut self, sq: &Subquery, ctx: &Ctx) -> Res<Val> {
        let k = self.fresh("scalar");
        let name = format!("{}scalar_{k}", self.pfx);
        let b = self.sub_block(&sq.subquery, format!("{name}_"))?;
        if b.shape != Shape::Scalar || b.outs.len() != 1 {
            bail!("scalar subquery that is not a single aggregate (it may return several rows)");
        }
        if !b.having.is_empty() {
            bail!("scalar subquery with HAVING (NULL when HAVING fails)");
        }
        let col = b.outs[0].clone();
        if col.val.approx.is_some() {
            bail!("scalar subquery yielding a computed float");
        }
        if col.val.nonlit_div {
            bail!("scalar subquery dividing by a non-literal");
        }
        self.frame().uses_params = true;
        let call = format!("{name}_value(db, params{})", self.sub_args(&b.outer, ctx));
        let mut w = String::new();
        writeln!(w, "/// The value of ({}){}", plan_doc(&sq.subquery), if col.nullable { "; None when it is NULL (no rows)" } else { "" }).unwrap();
        writeln!(w, "pub open spec fn {name}_value(db: Db, params: Params{}) -> {} {{", b.outer_params(), col.vty()).unwrap();
        writeln!(w, "    {name}_row_of(db, params{}).{}", b.outer_args(), col.field).unwrap();
        writeln!(w, "}}\n").unwrap();
        self.items.push(w);
        {
            let outer: perm::Args = b.outer.iter().map(|x| (x.var.clone(), x.vty())).collect();
            let call = format!("{name}_value(db, params{})", b.outer_args());
            self.perm.lemma(&format!("{name}_value"), "", &outer, perm::Kind::Eq, &call, &format!("{name}_row_of("), "");
        }
        let ty = col.val.ty;
        let (code, den, guard) = match (col.nullable, ty == Ty::Rat) {
            (true, true) => (format!("{call}->0.num"), Some(format!("{call}->0.den")), Some(format!("{call} is Some"))),
            (true, false) => (format!("{call}->0"), None, Some(format!("{call} is Some"))),
            (false, true) => (format!("{call}.num"), Some(format!("{call}.den")), None),
            (false, false) => (call.clone(), None, None),
        };
        Ok(Val { den, guard, char_col: col.val.char_col, ..Val::atom(code, ty) })
    }

    /// Permutation lemma for EXISTS over one FROM item.
    fn perm_exists(&mut self, name: &str, outer: &[Binder], bs: &[Binder], wdb: bool, up: bool, conds: &[String]) {
        if bs.len() != 1 {
            self.perm.unsupported("EXISTS over a join of several FROM items");
            return;
        }
        let o: perm::Args = outer.iter().map(|b| (b.var.clone(), b.vty())).collect();
        let ov = perm::vals(&o);
        let b0 = &bs[0];
        let r = b0.row();
        let s1 = b0.seq();
        let s2 = perm::to_db2(&s1);
        let sep = |db: &str| -> String {
            let mut v = vec![];
            if wdb {
                v.push(db.to_string());
            }
            if up {
                v.push("params".to_string());
            }
            if v.is_empty() { String::new() } else { format!("{}, ", v.join(", ")) }
        };
        let wh = |db: &str, x: &str| format!("{name}_where({}{}{x})", sep(db), o.iter().map(|(n, _)| format!("{n}, ")).collect::<String>());
        let ex = |db: &str| format!("{name}({db}{}{ov})", if up { ", params" } else { "" });
        if wdb {
            let mut a = o.clone();
            a.push((b0.var.clone(), r.clone()));
            let call = wh("db", &b0.var);
            self.perm.lemma(&format!("{name}_where"), "", &a, perm::Kind::Eq, &call, &conds.join(" "), "");
        }
        let (ex1, ex2) = (ex("db"), ex("db2"));
        let mut body = String::new();
        use std::fmt::Write;
        writeln!(body, "        if {ex1} {{").unwrap();
        writeln!(body, "            let i = choose|i: int| 0 <= i < {s1}.len() && {};", wh("db", &format!("{s1}[i]"))).unwrap();
        writeln!(body, "            assert({s1}.contains({s1}[i]));\n            lemma_bag_eq_contains({s1}, {s2}, {s1}[i]);").unwrap();
        writeln!(body, "            let j = choose|j: int| 0 <= j < {s2}.len() && {s2}[j] == {s1}[i];").unwrap();
        writeln!(body, "            assert(0 <= j < {s2}.len() && {});\n            assert({ex2});\n        }}", wh("db2", &format!("{s2}[j]"))).unwrap();
        let rev = perm::swap(&body);
        body.push_str(&rev);
        self.perm.lemma(name, "", &o, perm::Kind::Eq, &ex1, &format!("{name}_where( {s1}"), &body);
    }

    /// Permutation lemma for x IN (subquery).
    fn perm_in(&mut self, name: &str, b: &Block, xty: &str, cmp: &str) {
        let mut a: perm::Args = b.outer.iter().map(|x| (x.var.clone(), x.vty())).collect();
        a.push(("x".into(), xty.to_string()));
        let oa = b.outer_args();
        let r1 = format!("{name}_rows(db, params{oa})");
        let r2 = perm::to_db2(&r1);
        let c1 = cmp.to_string();
        let c2 = perm::to_db2(cmp).replace("[i]", "[j]");
        let call = format!("{name}(db, params{oa}, x)");
        let mut body = String::new();
        use std::fmt::Write;
        writeln!(body, "        if {call} {{").unwrap();
        writeln!(body, "            let i = choose|i: int| 0 <= i < {r1}.len() && {c1};").unwrap();
        writeln!(body, "            assert({r1}.contains({r1}[i]));\n            lemma_bag_eq_contains({r1}, {r2}, {r1}[i]);").unwrap();
        writeln!(body, "            let j = choose|j: int| 0 <= j < {r2}.len() && {r2}[j] == {r1}[i];").unwrap();
        writeln!(body, "            assert(0 <= j < {r2}.len() && {c2});\n            assert({});\n        }}", perm::to_db2(&call)).unwrap();
        let rev = perm::swap(&body);
        body.push_str(&rev);
        self.perm.lemma(name, "", &a, perm::Kind::Eq, &call, &format!("{name}_rows( {cmp}"), &body);
    }

    /// A derived table (FROM (SELECT ...) AS alias, or a CTE): its rows.
    fn derived(&mut self, alias: &str, plan: &LogicalPlan) -> Res<RelInfo> {
        let key = format!("{alias}\n{}", plan.display_indent());
        if let Some(r) = self.rels.get(&key) {
            return Ok(r.clone());
        }
        let base = sanitize(alias);
        self.frames.push(Frame::default());
        let r = self.block(plan, format!("{base}_"), false);
        let f = self.frames.pop().unwrap();
        let b = r?;
        if !f.outer.is_empty() {
            bail!("derived table {alias} refers to an enclosing query (LATERAL)");
        }
        for o in &b.outs {
            if o.nullable || o.val.approx.is_some() || o.val.nonlit_div {
                bail!("derived table {alias}: column {} is NULL-able / a computed float / divides by a non-literal", o.sql);
            }
        }
        let info = RelInfo {
            func: format!("rel_{base}"),
            row: format!("{}Row", camel(&base)),
            cols: b.outs.iter().map(|o| (o.sql.clone(), o.field.clone(), o.val.ty, o.val.char_col, o.nullable)).collect(),
        };
        let mut w = String::new();
        writeln!(w, "/// The rows of derived table {alias} (a bag).").unwrap();
        writeln!(w, "pub open spec fn {}(db: Db, params: Params) -> Seq<{}> {{\n    {base}_rows(db, params)\n}}\n", info.func, info.row).unwrap();
        self.items.push(w);
        self.perm.lemma(&info.func, "", &vec![], perm::Kind::Bag, &format!("{}(db, params)", info.func), &format!("{base}_rows("), "");
        self.rels.insert(key, info.clone());
        Ok(info)
    }

    // ── query blocks ──
    /// Translates one SELECT (without top-level ORDER BY / LIMIT) and emits
    /// its definitions; `prefix` names them ("" at the top level).
    fn block(&mut self, plan: &LogicalPlan, prefix: String, top: bool) -> Res<Block> {
        let mut p = plan;
        let mut distinct = false;
        match p {
            LogicalPlan::Distinct(Distinct::All(inner)) => {
                distinct = true;
                p = inner;
            }
            LogicalPlan::Distinct(Distinct::On(_)) => bail!("DISTINCT ON"),
            _ => {}
        }
        if let LogicalPlan::Union(u) = p {
            return self.union_block(u, prefix, top, distinct);
        }
        let mut projs: Vec<(Vec<Expr>, DFSchemaRef)> = vec![];
        while let LogicalPlan::Projection(pr) = p {
            projs.push((pr.expr.clone(), pr.schema.clone()));
            p = &pr.input;
        }
        if projs.is_empty() {
            bail!("a query block without a SELECT list ({})", p.display());
        }
        let (mut outs_e, out_schema) = projs[0].clone();
        for (inner, ischema) in projs.iter().skip(1) {
            outs_e = outs_e.into_iter().map(|e| inline_proj(e, inner, ischema)).collect::<Res<Vec<_>>>()?;
        }
        let mut having = None;
        if let LogicalPlan::Filter(f) = p {
            if let LogicalPlan::Aggregate(_) = f.input.as_ref() {
                having = Some(f.predicate.clone());
                p = &f.input;
            }
        }
        let mut agg = None;
        if let LogicalPlan::Aggregate(a) = p {
            agg = Some(a.clone());
            p = &a.input;
        }
        let mut preds = vec![];
        if let LogicalPlan::Filter(f) = p {
            preds = split_and(&f.predicate);
            p = &f.input;
        }
        match p {
            LogicalPlan::Distinct(_) => bail!("SELECT DISTINCT"),
            LogicalPlan::Union(_) => bail!("UNION"),
            LogicalPlan::Window(_) => bail!("window functions"),
            LogicalPlan::EmptyRelation(_) => bail!("SELECT without FROM"),
            LogicalPlan::Sort(_) | LogicalPlan::Limit(_) => bail!("ORDER BY / LIMIT inside a subquery or derived table"),
            _ => {}
        }
        let mut binders = vec![];
        let mut on = vec![];
        self.from_tree(p, &mut binders, &mut on)?;
        preds.extend(on);
        self.frame().binders = binders.clone();
        let mut where_ = vec![];
        for e in &preds {
            let v = self.expr(e, &Ctx::Row { tuple: None })?;
            if v.ty != Ty::Bool {
                bail!("WHERE conjunct is not boolean: {e}");
            }
            if v.nonlit_div {
                bail!("WHERE divides by a non-literal: {e}");
            }
            if binders.iter().any(|b| b.optional && v.code.contains(&format!("{}->0", b.var))) {
                bail!("WHERE reads the missing side of a LEFT JOIN ({e})");
            }
            where_.push(bool_code(&v));
        }
        let tuple = if prefix.is_empty() { "Tuple".to_string() } else { format!("{}Tuple", camel(prefix.trim_end_matches('_'))) };
        let outer0 = self.frames.last().unwrap().outer.clone();
        let outer_args: String = outer0.iter().map(|b| format!(", {}", b.var)).collect();
        let shape = match &agg {
            Some(a) if a.group_expr.is_empty() => Shape::Scalar,
            Some(_) => Shape::Grouped,
            None => Shape::Plain,
        };
        let mut outs: Vec<OutCol> = vec![];
        let mut fields: HashSet<String> = HashSet::new();
        let field_for = |sql: &str, fields: &mut HashSet<String>| -> String {
            let base = sanitize(sql);
            let mut f = base.clone();
            let mut n = 2;
            while fields.contains(&f) {
                f = format!("{base}_{n}");
                n += 1;
            }
            fields.insert(f.clone());
            f
        };
        let mut keys: Vec<(String, Val)> = vec![];
        let mut hv = vec![];
        let tctx = Ctx::Row { tuple: Some("t".into()) };
        match &agg {
            None => {
                for (i, e) in outs_e.iter().enumerate() {
                    let sql = out_schema.field(i).name().clone();
                    let v = self.expr(e, &tctx)?;
                    if v.approx.is_some() {
                        bail!("computed float {e} in a SELECT list without aggregation");
                    }
                    if v.guard.is_none() && v.code.contains("->0") {
                        bail!("NULL-able value {e} in the SELECT list");
                    }
                    let field = field_for(&sql, &mut fields);
                    let nullable = v.guard.is_some();
                    outs.push(OutCol { sql, field, val: v, nullable, key: None });
                }
            }
            Some(a) => {
                for (i, ge) in a.group_expr.iter().enumerate() {
                    let v = self.expr(ge, &tctx)?;
                    if v.approx.is_some() || v.ty == Ty::Rat || v.nonlit_div {
                        bail!("GROUP BY {ge}: not an exact value");
                    }
                    let v = opt_key(v);
                    let gname = match strip_alias(ge) {
                        Expr::Column(c) => format!("g_{}", sanitize(&c.name).trim_end_matches('_')),
                        _ => format!("g_{}", i + 1),
                    };
                    let gname = if keys.iter().any(|(g, _)| *g == gname) { format!("{gname}_{}", i + 1) } else { gname };
                    // an expression key gets a name, so proofs can refer to it
                    let simple = v.code.strip_prefix("t.").map_or(false, |r| r.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.'));
                    let v = if simple {
                        v
                    } else {
                        let kf = format!("{prefix}key_{}", i + 1);
                        let op: String = outer0.iter().map(|b| format!(", {}: {}", b.var, b.vty())).collect();
                        self.items.push(format!(
                            "/// GROUP BY key {}: {}\npub open spec fn {kf}(db: Db, params: Params{op}, t: {tuple}) -> {} {{\n    {}\n}}\n\n",
                            i + 1,
                            ge.to_string().replace('\n', " "),
                            key_ty(&v),
                            v.code
                        ));
                        {
                            let mut a: perm::Args = outer0.iter().map(|b| (b.var.clone(), b.vty())).collect();
                            a.push(("t".into(), tuple.clone()));
                            self.perm.lemma(&kf, "", &a, perm::Kind::Eq, &format!("{kf}(db, params{outer_args}, t)"), &v.code, "");
                        }
                        Val { code: format!("{kf}(db, params{outer_args}, t)"), ..v }
                    };
                    keys.push((gname, v));
                }
                let gctx = Arc::new(GroupCtx {
                    prefix: prefix.clone(),
                    tuple_ty: tuple.clone(),
                    outer_args: outer_args.clone(),
                    keys: keys.clone(),
                    group_exprs: a.group_expr.clone(),
                    aggr_exprs: a.aggr_expr.clone(),
                    schema: a.schema.clone(),
                    scalar: shape == Shape::Scalar,
                });
                let gc = Ctx::Group(gctx);
                for (i, e) in outs_e.iter().enumerate() {
                    let sql = out_schema.field(i).name().clone();
                    let v = self.expr(e, &gc)?;
                    let key = keys.iter().position(|(g, k)| *g == v.code || (k.opt && format!("{g}->0") == v.code));
                    let nullable = v.guard.is_some();
                    let field = field_for(&sql, &mut fields);
                    outs.push(OutCol { sql, field, val: v, nullable, key });
                }
                if let Some(h) = &having {
                    for c in split_and(h) {
                        let v = self.expr(&c, &gc)?;
                        if v.ty != Ty::Bool || v.nonlit_div || v.approx.is_some() {
                            bail!("HAVING {c}");
                        }
                        hv.push(v);
                    }
                }
            }
        }
        let outer = self.frames.last().unwrap().outer.clone();
        if outer.len() != outer0.len() {
            bail!("a correlated subquery whose SELECT list / HAVING refers to the enclosing query");
        }
        let blk = Block { prefix: prefix.clone(), tuple, binders, outer, shape, outs, having: hv, keys: keys.clone(), distinct, union_rows: None };
        if top && as_bag(&blk) && blk.outs.iter().any(|o| o.val.approx.is_some()) {
            bail!("a rounded float aggregate in a query whose GROUP BY columns are not all selected");
        }
        self.emit_block(&blk, &where_, top);
        Ok(blk)
    }

    /// UNION ALL of query blocks (UNION = DISTINCT over it): the bag of all
    /// their rows, column by column.
    fn union_block(&mut self, u: &Union, prefix: String, top: bool, distinct: bool) -> Res<Block> {
        let mut parts = vec![];
        for (i, inp) in u.inputs.iter().enumerate() {
            parts.push(self.sub_block(inp, format!("{prefix}u{}_", i + 1))?);
        }
        let n = u.schema.fields().len();
        let mut outs = vec![];
        let mut fields: HashSet<String> = HashSet::new();
        for j in 0..n {
            let mut ty: Option<Ty> = None;
            for b in &parts {
                let c = b.outs.get(j).ok_or("UNION inputs with different numbers of columns")?;
                if c.nullable || c.val.approx.is_some() || c.val.nonlit_div {
                    bail!("UNION column {} is NULL-able / a computed float / divides by a non-literal", c.sql);
                }
                ty = Some(match (ty, c.val.ty) {
                    (None, t) => t,
                    (Some(a), b2) if a == b2 => a,
                    (Some(a), b2) if scale_of(a).is_some() && scale_of(b2).is_some() => Ty::Dec(scale_of(a).unwrap().max(scale_of(b2).unwrap())),
                    (Some(a), b2) => bail!("UNION column {j}: {a:?} and {b2:?}"),
                });
            }
            let sql = u.schema.field(j).name().clone();
            let mut f = sanitize(&sql);
            while fields.contains(&f) {
                f.push('_');
            }
            fields.insert(f.clone());
            let ch = parts.iter().all(|b| b.outs[j].val.char_col);
            outs.push(OutCol { sql, field: f, val: Val { char_col: ch, ..Val::atom(String::new(), ty.unwrap()) }, nullable: false, key: None });
        }
        let row = if top { "OutRow".to_string() } else { format!("{}Row", camel(prefix.trim_end_matches('_'))) };
        let mut segs = vec![];
        for b in &parts {
            let prow = format!("{}Row", camel(b.prefix.trim_end_matches('_')));
            let fs: Vec<String> = outs
                .iter()
                .zip(&b.outs)
                .map(|(o, c)| {
                    let v = Val::atom(format!("r.{}", c.field), c.val.ty);
                    let v = match (scale_of(c.val.ty), scale_of(o.val.ty)) {
                        (Some(a), Some(z)) if a != z => rescale(&v, a, z),
                        _ => v,
                    };
                    format!("{}: {}", o.field, v.code)
                })
                .collect();
            segs.push(format!("{}rows(db, params{}).map_values(|r: {prow}| {row} {{ {} }})", b.prefix, b.outer_args(), fs.join(", ")));
        }
        let body = segs.join("\n        + ");
        let outer = self.frames.last().unwrap().outer.clone();
        let blk = Block {
            prefix: prefix.clone(),
            tuple: String::new(),
            binders: vec![],
            outer,
            shape: Shape::Plain,
            outs,
            having: vec![],
            keys: vec![],
            distinct,
            union_rows: Some(body),
        };
        self.emit_block(&blk, &[], top);
        Ok(blk)
    }

    fn emit_block(&mut self, b: &Block, where_: &[String], top: bool) {
        let guarded = self.guarded_vals.contains(&b.prefix);
        self.perm.block(b, where_, top);
        if let Some(body) = &b.union_rows {
            let p = &b.prefix;
            let row = if top { "OutRow".to_string() } else { format!("{}Row", camel(p.trim_end_matches('_'))) };
            let mut w = String::new();
            writeln!(w, "// ── {} ──\n", if p.is_empty() { "The query (UNION)".to_string() } else { format!("Block {} (UNION)", p.trim_end_matches('_')) }).unwrap();
            writeln!(w, "/// One output row.").unwrap();
            writeln!(w, "pub struct {row} {{").unwrap();
            for c in &b.outs {
                writeln!(w, "    /// {} ({})", c.sql, ty_doc(c.val.ty)).unwrap();
                writeln!(w, "    pub {}: {},", c.field, c.vty()).unwrap();
            }
            writeln!(w, "}}\n").unwrap();
            writeln!(w, "/// The rows: every input's rows ({}).", if b.distinct { "UNION: without repeats" } else { "UNION ALL" }).unwrap();
            writeln!(w, "pub open spec fn {p}rows(db: Db, params: Params{}) -> Seq<{row}> {{", b.outer_params()).unwrap();
            if b.distinct {
                writeln!(w, "    dedup({body})").unwrap();
            } else {
                writeln!(w, "    {body}").unwrap();
            }
            writeln!(w, "}}\n").unwrap();
            self.items.push(w);
            return;
        }
        let mut w = String::new();
        let p = &b.prefix;
        let tt = &b.tuple;
        let op = b.outer_params();
        let oa = b.outer_args();
        let keys = &b.keys;
        let row = if top { "OutRow".to_string() } else { format!("{}Row", camel(p.trim_end_matches('_'))) };
        let what = if p.is_empty() { "the query".to_string() } else { format!("block {}", p.trim_end_matches('_')) };
        writeln!(w, "// ── {} ──\n", if p.is_empty() { "The query".to_string() } else { format!("Block {}", p.trim_end_matches('_')) }).unwrap();
        writeln!(w, "/// One tuple of the FROM clause of {what}.").unwrap();
        writeln!(w, "pub struct {tt} {{").unwrap();
        for bd in &b.binders {
            writeln!(w, "    pub {}: {},", bd.var, bd.vty()).unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        let bargs: String = b.binders.iter().map(|bd| format!(", {}: {}", bd.var, bd.vty())).collect();
        writeln!(w, "/// The WHERE clause of {what}, conjunct by conjunct in SQL order.").unwrap();
        writeln!(w, "pub open spec fn {p}where_clause(db: Db, params: Params{op}{bargs}) -> bool {{").unwrap();
        if where_.is_empty() {
            writeln!(w, "    true").unwrap();
        }
        for c in where_ {
            writeln!(w, "    &&& {c}").unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        for (i, bd) in b.binders.iter().enumerate() {
            if bd.optional {
                let prev: String = b.binders[..i].iter().map(|x| format!(", {}: {}", x.var, x.vty())).collect();
                writeln!(w, "/// The ON condition of LEFT JOIN {}.", bd.alias).unwrap();
                writeln!(w, "pub open spec fn {p}on_{}(db: Db, params: Params{op}{prev}, {}: {}) -> bool {{", bd.var, bd.var, bd.row()).unwrap();
                if bd.on.is_empty() {
                    writeln!(w, "    true").unwrap();
                }
                for c in &bd.on {
                    writeln!(w, "    &&& {c}").unwrap();
                }
                writeln!(w, "}}\n").unwrap();
            }
        }
        writeln!(w, "/// The FROM-clause tuples of {what} (bag product) that satisfy WHERE.").unwrap();
        writeln!(w, "pub open spec fn {p}tuples(db: Db, params: Params{op}) -> Seq<{tt}> {{").unwrap();
        writeln!(w, "    {}", nest(b, 0)).unwrap();
        writeln!(w, "}}\n").unwrap();
        for i in 1..=b.binders.len() {
            let prev: String = b.binders[..i].iter().map(|x| format!(", {}: {}", x.var, x.vty())).collect();
            let names: Vec<String> = b.binders[..i].iter().map(|x| x.var.clone()).collect();
            writeln!(w, "/// The tuples extending ({}).", names.join(", ")).unwrap();
            writeln!(w, "pub open spec fn {p}tuples_{i}(db: Db, params: Params{op}{prev}) -> Seq<{tt}> {{").unwrap();
            writeln!(w, "    {}", nest(b, i)).unwrap();
            writeln!(w, "}}\n").unwrap();
        }
        let key_params: String = keys.iter().map(|(g, v)| format!(", {g}: {}", key_ty(v))).collect();
        let key_args: String = keys.iter().map(|(g, _)| format!(", {g}")).collect();
        let cond = keys.iter().map(|(g, v)| format!("{} == {g}", v.code)).collect::<Vec<_>>().join(" && ");
        if b.shape != Shape::Plain {
            let grp = if keys.is_empty() { "all tuples".to_string() } else { format!("the tuples of group ({})", keys.iter().map(|k| k.0.trim_start_matches("g_").to_string()).collect::<Vec<_>>().join(", ")) };
            writeln!(w, "/// Σ f(t) over {grp}.").unwrap();
            writeln!(w, "pub open spec fn {p}group_sum(db: Db, params: Params{op}{key_params}, f: spec_fn({tt}) -> int) -> int {{").unwrap();
            if cond.is_empty() {
                writeln!(w, "    sum({p}tuples(db, params{oa}), f)").unwrap();
            } else {
                writeln!(w, "    sum({p}tuples(db, params{oa}), |t: {tt}| if {cond} {{ f(t) }} else {{ 0 }})").unwrap();
            }
            writeln!(w, "}}\n").unwrap();
            writeln!(w, "/// COUNT(*) over {grp}.").unwrap();
            writeln!(w, "pub open spec fn {p}group_count(db: Db, params: Params{op}{key_params}) -> int {{").unwrap();
            if cond.is_empty() {
                writeln!(w, "    {p}tuples(db, params{oa}).len() as int").unwrap();
            } else {
                writeln!(w, "    sum({p}tuples(db, params{oa}), |t: {tt}| if {cond} {{ 1int }} else {{ 0 }})").unwrap();
            }
            writeln!(w, "}}\n").unwrap();
            writeln!(w, "/// The values of f over {grp}.").unwrap();
            writeln!(w, "pub open spec fn {p}group_vals<V>(db: Db, params: Params{op}{key_params}, f: spec_fn({tt}) -> V) -> Seq<V> {{").unwrap();
            if cond.is_empty() {
                writeln!(w, "    {p}tuples(db, params{oa}).map_values(f)").unwrap();
            } else {
                writeln!(w, "    map_where({p}tuples(db, params{oa}), |t: {tt}| {cond}, f)").unwrap();
            }
            writeln!(w, "}}\n").unwrap();
            if guarded {
                writeln!(w, "/// The values of f over {grp} where keep holds (the non-NULL ones).").unwrap();
                writeln!(w, "pub open spec fn {p}group_vals_where<V>(db: Db, params: Params{op}{key_params}, keep: spec_fn({tt}) -> bool, f: spec_fn({tt}) -> V) -> Seq<V> {{").unwrap();
                if cond.is_empty() {
                    writeln!(w, "    map_where({p}tuples(db, params{oa}), keep, f)").unwrap();
                } else {
                    writeln!(w, "    map_where({p}tuples(db, params{oa}), |t: {tt}| {cond} && keep(t), f)").unwrap();
                }
                writeln!(w, "}}\n").unwrap();
            }
        }
        let _ = key_args;
        writeln!(w, "/// One output row of {what}.").unwrap();
        writeln!(w, "pub struct {row} {{").unwrap();
        for c in &b.outs {
            let doc = if c.val.approx.is_some() { "DOUBLE, value * 2^1074; a rounded aggregate".to_string() } else { ty_doc(c.val.ty) };
            writeln!(w, "    /// {} ({doc}{})", c.sql, if c.nullable { "; None = NULL" } else { "" }).unwrap();
            writeln!(w, "    pub {}: {},", c.field, c.vty()).unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        if b.shape != Shape::Plain {
            writeln!(w, "/// The output row of {}{}.", if keys.is_empty() { "the aggregate".to_string() } else { "a group: its keys and its aggregates".to_string() },
                if b.outs.iter().any(|c| c.val.approx.is_some()) { " (float aggregates at their exact value)" } else { "" }).unwrap();
            writeln!(w, "pub open spec fn {p}row_of(db: Db, params: Params{op}{key_params}) -> {row} {{").unwrap();
            writeln!(w, "    {row} {{").unwrap();
            for c in &b.outs {
                writeln!(w, "        {}: {},", c.field, row_expr(c)).unwrap();
            }
            writeln!(w, "    }}\n}}\n").unwrap();
        }
        if b.shape == Shape::Grouped || !b.having.is_empty() {
            let hv: Vec<String> = b.having.iter().map(bool_code).collect();
            writeln!(w, "/// HAVING (true when absent).").unwrap();
            writeln!(w, "pub open spec fn {p}having(db: Db, params: Params{op}{key_params}) -> bool {{").unwrap();
            writeln!(w, "    {}", if hv.is_empty() { "true".to_string() } else { hv.join(" && ") }).unwrap();
            writeln!(w, "}}\n").unwrap();
        }
        writeln!(w, "/// The result rows of {what}, as a bag (in no particular order).").unwrap();
        writeln!(w, "pub open spec fn {p}rows(db: Db, params: Params{op}) -> Seq<{row}> {{").unwrap();
        match b.shape {
            Shape::Plain => {
                let fs: Vec<String> = b.outs.iter().map(|c| format!("{}: {}", c.field, row_expr(c))).collect();
                let r = format!("{p}tuples(db, params{oa}).map_values(|t: {tt}| {row} {{ {} }})", fs.join(", "));
                writeln!(w, "    {}", if b.distinct { format!("dedup({r})") } else { r }).unwrap();
            }
            Shape::Scalar if b.having.is_empty() => writeln!(w, "    seq![{p}row_of(db, params{oa})]").unwrap(),
            Shape::Scalar => writeln!(w, "    if {p}having(db, params{oa}) {{ seq![{p}row_of(db, params{oa})] }} else {{ Seq::empty() }}").unwrap(),
            Shape::Grouped => {
                let tkeys: String = keys.iter().map(|(_, v)| format!(", {}", v.code)).collect();
                let r = format!(
                    "dedup(map_where({p}tuples(db, params{oa}), |t: {tt}| {p}having(db, params{oa}{tkeys}), |t: {tt}| {}))\n        .map_values(|k: {}| {p}row_of(db, params{oa}{}))",
                    key_tuple_val(keys),
                    key_tuple_ty(keys),
                    key_tuple_args(keys, "k")
                );
                // DISTINCT: groups whose rows coincide (keys not all selected) give one row
                writeln!(w, "    {}", if b.distinct { format!("dedup({r})") } else { r }).unwrap();
            }
        }
        writeln!(w, "}}\n").unwrap();
        self.items.push(w);
    }
}

fn bool_code(v: &Val) -> String {
    match &v.guard {
        Some(g) => format!("({g} && {})", v.code),
        None => v.code.clone(),
    }
}

/// The value of an output column of a grouped / aggregate block.
fn row_expr(c: &OutCol) -> String {
    let v = &c.val;
    let base = match (v.ty, &v.approx) {
        (Ty::Flt(pp), Some(_)) => {
            let e = if pp > 1 { format!("({}) / pow2({})", v.code, 1074 * (pp - 1)) } else { v.code.clone() };
            match &v.den { Some(d) => format!("({e}) / ({d})"), None => e }
        }
        (Ty::Rat, _) => format!("Rat {{ num: ({}) as int, den: ({}) as int }}", v.code, v.den_s()),
        _ => v.code.clone(),
    };
    if c.nullable {
        format!("if {} {{ Some({base}) }} else {{ None }}", v.guard.clone().unwrap())
    } else {
        base
    }
}

/// The tuples extending the first `i` FROM items (level `i` of the nest):
/// iterate over FROM item i, continuing at level i + 1.
fn nest(b: &Block, i: usize) -> String {
    let p = &b.prefix;
    let oa = b.outer_args();
    let vars: Vec<String> = b.binders.iter().map(|x| x.var.clone()).collect();
    if i == b.binders.len() {
        return format!(
            "if {p}where_clause(db, params{oa}{}) {{ seq![{} {{ {} }}] }} else {{ Seq::empty() }}",
            vars.iter().map(|v| format!(", {v}")).collect::<String>(),
            b.tuple,
            vars.join(", ")
        );
    }
    let bd = &b.binders[i];
    let upto: String = vars[..=i].iter().map(|v| format!(", {v}")).collect();
    let next = format!("{p}tuples_{}(db, params{oa}{upto})", i + 1);
    if bd.optional {
        let prev: String = vars[..i].iter().map(|v| format!(", {v}")).collect();
        format!(
            "{{ let m = flat({}.map_values(|{v}: {r}| if {p}on_{v}(db, params{oa}{prev}, {v}) {{ seq![Some({v})] }} else {{ Seq::empty() }})); flat((if m.len() > 0 {{ m }} else {{ seq![None::<{r}>] }}).map_values(|{v}: Option<{r}>| {next})) }}",
            bd.seq(),
            v = bd.var,
            r = bd.row()
        )
    } else {
        format!("flat({}.map_values(|{}: {}| {next}))", bd.seq(), bd.var, bd.row())
    }
}

fn key_tuple_ty(keys: &[(String, Val)]) -> String {
    if keys.len() == 1 {
        key_ty(&keys[0].1)
    } else {
        format!("({})", keys.iter().map(|(_, v)| key_ty(v)).collect::<Vec<_>>().join(", "))
    }
}

fn key_tuple_val(keys: &[(String, Val)]) -> String {
    if keys.len() == 1 {
        keys[0].1.code.clone()
    } else {
        format!("({})", keys.iter().map(|(_, v)| v.code.clone()).collect::<Vec<_>>().join(", "))
    }
}

fn key_tuple_args(keys: &[(String, Val)], k: &str) -> String {
    if keys.len() == 1 {
        format!(", {k}")
    } else {
        (0..keys.len()).map(|i| format!(", {k}.{i}")).collect()
    }
}

fn simp_mul(s: &str) -> String {
    let mut s = s.to_string();
    for (a, b) in [(" * 1 ", " "), ("1 * ", "")] {
        s = s.replace(a, b);
    }
    if let Some(x) = s.strip_suffix(" * 1") {
        s = x.to_string();
    }
    s
}

fn simplify_k(s: &str) -> String {
    let mut lit = 0i64;
    let mut rest = vec![];
    for p in s.split(" + ") {
        match p.trim().parse::<i64>() {
            Ok(n) => lit += n,
            Err(_) => rest.push(p.trim().to_string()),
        }
    }
    if lit != 0 || rest.is_empty() {
        rest.push(lit.to_string());
    }
    rest.join(" + ")
}

fn rescale_k(v: &Val, k: i128) -> Val {
    match v.int_lit {
        Some(x) => Val::int(x * k, v.ty),
        None => Val { code: format!("{} * {k}", v.wrap(2)), prec: 2, ..v.clone() },
    }
}

fn flt_p(t: Ty) -> u32 {
    if let Ty::Flt(p) = t { p } else { 1 }
}

/// An operand of float arithmetic, as a float (SQL converts it to DOUBLE).
fn to_flt(v: &Val) -> Res<Val> {
    match v.ty {
        Ty::Flt(_) => Ok(v.clone()),
        Ty::Int => match v.int_lit {
            Some(n) if n.abs() < (1i128 << 53) => Ok(Val::atom(f64_code(n as f64)?, Ty::Flt(1))),
            Some(_) => bail!("integer literal not exactly representable as a DOUBLE"),
            None => Ok(Val {
                code: format!("{} * pow2(1074)", v.wrap(2)),
                prec: 2,
                ty: Ty::Flt(1),
                approx: Some((format!("abs({}) * pow2(1074)", v.code), "1".into())),
                ..v.clone()
            }),
        },
        Ty::Dec(s) => match v.int_lit {
            Some(n) => Ok(Val::atom(f64_code(decimal_to_f64(n, s as i8))?, Ty::Flt(1))),
            None => bail!("DECIMAL column converted to DOUBLE (rounds; not modelled)"),
        },
        t => bail!("{t:?} in float arithmetic"),
    }
}

/// An exact number as numerator / denominator codes.
fn as_ratio(v: &Val) -> Result<(String, String), String> {
    match v.ty {
        Ty::Rat => Ok((v.wrap(2), format!("({})", v.den_s()))),
        Ty::Int => Ok((v.wrap(2), "1".into())),
        Ty::Dec(s) => Ok((v.wrap(2), pow10(s).to_string())),
        t => Err(format!("{t:?} where a number is expected")),
    }
}

fn unify(a: &Val, b: &Val) -> Result<(Val, Val), String> {
    match (scale_of(a.ty), scale_of(b.ty)) {
        (Some(x), Some(y)) => {
            let s = x.max(y);
            let ty = if s == 0 && a.ty == Ty::Int && b.ty == Ty::Int { Ty::Int } else { Ty::Dec(s) };
            let (mut a2, mut b2) = (rescale(a, x, s), rescale(b, y, s));
            a2.ty = ty;
            b2.ty = ty;
            Ok((a2, b2))
        }
        _ => match (a.ty, b.ty) {
            (Ty::Flt(p), Ty::Flt(q)) => {
                let m = p.max(q);
                let (ca, pa) = flt_rescale(&a.code, a.prec, p, m);
                let (cb, pb) = flt_rescale(&b.code, b.prec, q, m);
                Ok((Val { code: ca, prec: pa, ty: Ty::Flt(m), ..a.clone() }, Val { code: cb, prec: pb, ty: Ty::Flt(m), ..b.clone() }))
            }
            (Ty::Flt(_), Ty::Int | Ty::Dec(_)) => unify(a, &to_flt(b)?),
            (Ty::Int | Ty::Dec(_), Ty::Flt(_)) => unify(&to_flt(a)?, b),
            (x, y) if x == y => Ok((a.clone(), b.clone())),
            (x, y) => Err(format!("comparison of {x:?} with {y:?}")),
        },
    }
}

/// Replace references to an inner projection's outputs by their expressions.
fn inline_proj(e: Expr, inner: &[Expr], ischema: &DFSchemaRef) -> Res<Expr> {
    e.transform_up(|x| {
        if let Expr::Column(c) = &x {
            if let Ok(i) = ischema.index_of_column(c) {
                return Ok(Transformed::yes(strip_alias(&inner[i]).clone()));
            }
        }
        Ok(Transformed::no(x))
    })
    .map(|t| t.data)
    .map_err(|e| e.to_string())
}

// ── the top level ─────────────────────────────────────────────────────────
/// One ORDER BY key: an output column, its direction, and where its NULLs go.
struct SortKey {
    col: usize,
    desc: bool,
    nulls_first: bool,
}

struct Top {
    block: Block,
    order: Vec<SortKey>,
    limit: Option<usize>,
    offset: usize,
}

/// LIMIT / OFFSET applied to a full answer `a`.
fn cut(top: &Top, a: &str) -> String {
    let a = if top.offset > 0 { format!("skip_rows({a}, {})", top.offset) } else { a.to_string() };
    match top.limit {
        Some(n) => format!("limit_rows({a}, {n})"),
        None => a,
    }
}

fn translate(tr: &mut Tr, plan: &LogicalPlan, explicit_nulls: &[Option<bool>]) -> Res<Top> {
    let mut p = plan;
    let mut limit = None;
    let mut offset = 0;
    if let LogicalPlan::Limit(l) = p {
        match l.get_skip_type().map_err(|e| e.to_string())? {
            SkipType::Literal(k) => offset = k,
            _ => bail!("OFFSET must be a constant"),
        }
        match l.get_fetch_type().map_err(|e| e.to_string())? {
            FetchType::Literal(n) => limit = n,
            _ => bail!("LIMIT must be a constant"),
        }
        p = &l.input;
    }
    let mut sort = None;
    if let LogicalPlan::Sort(s) = p {
        if s.fetch.is_some() {
            bail!("sort with fetch");
        }
        sort = Some(s.expr.clone());
        p = &s.input;
    }
    match p {
        LogicalPlan::Projection(pr) if matches!(pr.input.as_ref(), LogicalPlan::Sort(_)) => {
            bail!("ORDER BY an expression that is not in the SELECT list")
        }
        LogicalPlan::Projection(_) | LogicalPlan::Distinct(Distinct::All(_)) | LogicalPlan::Union(_) => {}
        _ => bail!("ORDER BY on an expression that is not selected ({})", p.display()),
    }
    let schema = p.schema().clone();
    tr.frames.push(Frame::default());
    let block = tr.block(p, String::new(), true)?;
    let mut order = vec![];
    let sort = sort.unwrap_or_default();
    let nkeys = sort.len();
    for (pos, s) in sort.into_iter().enumerate() {
        let Expr::Column(c) = strip_alias(&s.expr) else { bail!("ORDER BY {}: only output columns", s.expr) };
        let i = schema.index_of_column(c).map_err(|_| format!("ORDER BY {c}: not an output column"))?;
        let col = &block.outs[i];
        if col.val.ty == Ty::Rat {
            bail!("ORDER BY on a fraction column ({})", col.sql);
        }
        let desc = !s.asc;
        let nulls_first = match explicit_nulls.get(pos).copied().flatten() {
            _ if !col.nullable => false,
            Some(nf) => nf,
            None if explicit_nulls.len() == nkeys => tr.nulls_first_default[desc as usize],
            None => bail!("ORDER BY {} on a NULL-able column: cannot tell whether NULLS FIRST / LAST was written", col.sql),
        };
        order.push(SortKey { col: i, desc, nulls_first });
    }
    Ok(Top { block, order, limit, offset })
}

/// NULLS FIRST / LAST as written on the query's ORDER BY keys (None: not written).
fn explicit_nulls(sql: &str) -> Res<Vec<Option<bool>>> {
    let stmts = Parser::parse_sql(&GenericDialect {}, sql).map_err(|e| format!("{e}"))?;
    Ok(match stmts.first() {
        Some(ast::Statement::Query(q)) => match q.order_by.as_ref().map(|o| &o.kind) {
            Some(ast::OrderByKind::Expressions(es)) => es.iter().map(|e| e.options.nulls_first).collect(),
            _ => vec![],
        },
        _ => vec![],
    })
}

// ── printing ──────────────────────────────────────────────────────────────
fn print(tr: &Tr, top: &Top, sql: &str, src_name: &str) -> String {
    let mut o = String::new();
    let w = &mut o;
    let b = &top.block;
    writeln!(w, "// GENERATED by {VERSION} from {src_name}.  DO NOT EDIT: regenerate.").unwrap();
    writeln!(w, "//").unwrap();
    writeln!(w, "// The reference meaning (q_ref) of this SQL.  The definitions are TRUSTED:").unwrap();
    writeln!(w, "// they are what every proof in the crate is about.  They are produced").unwrap();
    writeln!(w, "// mechanically; the rules applied are documented in sql2verus/src/main.rs.").unwrap();
    writeln!(w, "// The \"Facts\" section at the end is proved here (checked, not trusted).").unwrap();
    writeln!(w, "//").unwrap();
    for l in sql.trim().lines() {
        writeln!(w, "//   {}", l.trim_end()).unwrap();
    }
    writeln!(w, "//").unwrap();
    writeln!(w, "// Query-specific notes (host contract):").unwrap();
    writeln!(w, "//  * CHAR(n) text: {} ({}).", if tr.sch.char_semantics.0 == "bytes" {
        "compared as bytes like VARCHAR, trailing spaces significant; the host delivers text unchanged"
    } else {
        "compared without trailing spaces"
    }, tr.sch.char_semantics.1).unwrap();
    let mut chars: Vec<String> = tr
        .used
        .iter()
        .flat_map(|(t, cs)| cs.iter().filter(|c| tr.sch.char_cols.contains(&(t.clone(), (*c).clone()))).cloned().collect::<Vec<_>>())
        .collect();
    chars.sort();
    if !chars.is_empty() {
        writeln!(w, "//  * CHAR columns {}: the host delivers them without trailing spaces.", chars.join(", ")).unwrap();
    }
    for (n, (t, ch, days)) in &tr.params {
        let extra = if *ch { "; compared with a CHAR column, so the host passes it without trailing spaces" } else { "" };
        let what = if *days { " (a number of days: `$n * INTERVAL '1' DAY`)" } else { "" };
        writeln!(w, "//  * ${n} is params.p{n}: {}{what}{extra}.", ty_doc(*t)).unwrap();
    }
    let used_tys: BTreeSet<String> = tr
        .used
        .iter()
        .flat_map(|(t, cs)| cs.iter().map(|c| format!("{:?}", tr.sch.col_ty[&(t.clone(), c.clone())].0.unwrap())).collect::<Vec<_>>())
        .collect();
    if used_tys.iter().any(|t| t.starts_with("Flt")) {
        writeln!(w, "//  * DOUBLE columns are finite (no NaN / infinity); the host delivers the exact value.").unwrap();
    }
    if used_tys.contains("Ts") {
        writeln!(w, "//  * TIMESTAMP columns are delivered as nanoseconds since 1970-01-01 00:00.").unwrap();
    }
    writeln!(w, "//  * Text order is bytes_lt: the host runs only under a C (byte) collation.").unwrap();
    for n in &tr.notes {
        writeln!(w, "//  * {n}").unwrap();
    }
    writeln!(w, "#![allow(unused_imports)]").unwrap();
    writeln!(w, "use vstd::prelude::*;").unwrap();
    for m in ["bytes", "order", "perm", "rat", "rel", "sqlfns", "sums"] {
        writeln!(w, "use verus_common::{m}::*;").unwrap();
    }
    writeln!(w, "\nverus! {{\n").unwrap();
    for (t, cs) in &tr.used {
        writeln!(w, "/// A row of `{t}`, projected onto the columns the query reads.").unwrap();
        writeln!(w, "pub struct {} {{", table_row(t)).unwrap();
        for c in &tr.sch.ddl_cols[t] {
            if cs.contains(c) {
                let (ty, nullable) = tr.sch.col_ty[&(t.clone(), c.clone())];
                let ty = ty.unwrap();
                let ch = if tr.sch.char_cols.contains(&(t.clone(), c.clone())) { ", CHAR" } else { "" };
                let (null_doc, vty) = if nullable { ("; None = NULL", format!("Option<{}>", verus_ty(ty))) } else { ("", verus_ty(ty).to_string()) };
                writeln!(w, "    /// {}{ch}{null_doc}", ty_doc(ty)).unwrap();
                writeln!(w, "    pub {}: {vty},", sanitize(c)).unwrap();
            }
        }
        writeln!(w, "}}\n").unwrap();
    }
    writeln!(w, "/// The database: each table is a bag (a sequence; duplicates count).").unwrap();
    writeln!(w, "pub struct Db {{").unwrap();
    for t in tr.used.keys() {
        writeln!(w, "    pub {}: Seq<{}>,", table_field(t), table_row(t)).unwrap();
    }
    writeln!(w, "}}\n").unwrap();
    writeln!(w, "/// The query parameters.").unwrap();
    writeln!(w, "pub struct Params {{").unwrap();
    for (n, (t, _, days)) in &tr.params {
        writeln!(w, "    /// ${n}: {}{}", ty_doc(*t), if *days { ", a number of days" } else { "" }).unwrap();
        writeln!(w, "    pub p{n}: {},", verus_ty(*t)).unwrap();
    }
    writeln!(w, "}}\n").unwrap();
    for it in &tr.items {
        w.push_str(it);
    }

    // ── the answer ──
    writeln!(w, "// ── The answer ──\n").unwrap();
    let keys = &b.keys;
    let approx = b.outs.iter().any(|c| c.val.approx.is_some());
    let row_ok_body = |w: &mut String, r: &str| {
        for c in &b.outs {
            let v = &c.val;
            let f = format!("{r}.{}", c.field);
            match &v.approx {
                Some((abs, m)) => {
                    let pp = flt_p(v.ty);
                    let x = if c.nullable { format!("{f}->0") } else { f.clone() };
                    let lhs = if pp > 1 { format!("{x} * pow2({})", 1074 * (pp - 1)) } else { x };
                    let lhs = match &v.den { Some(d) => format!("{lhs} * ({d})"), None => lhs };
                    let m = if v.den.is_some() { simplify_k(&format!("{m} + 1")) } else { m.clone() };
                    let within = format!("float_within({lhs}, {}, {abs}, {m})", v.code);
                    if c.nullable {
                        let g = v.guard.clone().unwrap();
                        writeln!(w, "    &&& ({f} is Some <==> {g}) && ({f} is Some ==> {within})").unwrap();
                    } else {
                        writeln!(w, "    &&& {within}").unwrap();
                    }
                }
                None if c.key.is_none() => writeln!(w, "    &&& {f} == {}", row_expr(c)).unwrap(),
                None => {}
            }
            for e in den_exprs(c) {
                writeln!(w, "    &&& {e}").unwrap();
            }
        }
    };
    let ka: String = keys.iter().map(|(g, _)| format!(", {g}")).collect();
    match b.shape {
        Shape::Grouped if !as_bag(b) => {
            writeln!(w, "/// Row r is a group's row: the group is non-empty and passes HAVING, and").unwrap();
            writeln!(w, "/// the other columns are its aggregates.").unwrap();
            writeln!(w, "pub open spec fn row_ok(db: Db, params: Params, r: OutRow) -> bool {{").unwrap();
            for c in &b.outs {
                if let Some(i) = c.key {
                    writeln!(w, "    let {} = r.{};", keys[i].0, c.field).unwrap();
                }
            }
            writeln!(w, "    &&& group_count(db, params{ka}) > 0").unwrap();
            writeln!(w, "    &&& having(db, params{ka})").unwrap();
            row_ok_body(w, "r");
            writeln!(w, "}}\n").unwrap();
            writeln!(w, "/// Rows a and b are for the same group.").unwrap();
            writeln!(w, "pub open spec fn same_group(a: OutRow, b: OutRow) -> bool {{").unwrap();
            let sg: Vec<String> = key_cols(b).iter().map(|c| format!("a.{f} == b.{f}", f = c.field)).collect();
            writeln!(w, "    {}", sg.join(" && ")).unwrap();
            writeln!(w, "}}\n").unwrap();
        }
        Shape::Scalar => {
            writeln!(w, "/// Row r is the aggregate row.").unwrap();
            writeln!(w, "pub open spec fn row_ok(db: Db, params: Params, r: OutRow) -> bool {{").unwrap();
            writeln!(w, "    &&& true").unwrap();
            row_ok_body(w, "r");
            writeln!(w, "}}\n").unwrap();
        }
        _ => {}
    }
    let ob: Vec<String> = top
        .order
        .iter()
        .map(|k| {
            let c = &b.outs[k.col];
            let nulls = if c.nullable { if k.nulls_first { " NULLS FIRST" } else { " NULLS LAST" } } else { "" };
            format!("{} {}{nulls}", c.sql, if k.desc { "DESC" } else { "ASC" })
        })
        .collect();
    writeln!(w, "/// ORDER BY {}: a sorts strictly before b.", if ob.is_empty() { "(none)".into() } else { ob.join(", ") }).unwrap();
    writeln!(w, "pub open spec fn row_lt(a: OutRow, b: OutRow) -> bool {{").unwrap();
    writeln!(w, "    {}", lex_lt(b, &top.order)).unwrap();
    writeln!(w, "}}\n").unwrap();
    let defined: Vec<String> = b.outs.iter().flat_map(|c| c.val.den_conds.clone()).collect();
    writeln!(w, "/// `out` is the full answer (before any LIMIT), sorted by ORDER BY (ties in any order).").unwrap();
    writeln!(w, "pub open spec fn full_result(db: Db, params: Params, out: Seq<OutRow>) -> bool {{").unwrap();
    match b.shape {
        Shape::Grouped if !as_bag(b) => {
            writeln!(w, "    &&& forall|i: int| 0 <= i < out.len() ==> row_ok(db, params, #[trigger] out[i])").unwrap();
            writeln!(w, "    &&& forall|i: int, j: int| 0 <= i < j < out.len() ==>").unwrap();
            writeln!(w, "        !same_group(#[trigger] out[i], #[trigger] out[j]) && !row_lt(out[j], out[i])").unwrap();
            let gq: Vec<String> = keys.iter().map(|(g, v)| format!("{g}: {}", key_ty(v))).collect();
            let gm: Vec<String> = key_cols(b)
                .iter()
                .enumerate()
                .map(|(n, c)| {
                    let g = &keys[c.key.unwrap()].0;
                    if n == 0 { format!("(#[trigger] out[i]).{} == {g}", c.field) } else { format!("out[i].{} == {g}", c.field) }
                })
                .collect();
            writeln!(w, "    &&& forall|{}| #[trigger] group_count(db, params{ka}) > 0 && having(db, params{ka}) ==>", gq.join(", ")).unwrap();
            writeln!(w, "        exists|i: int| 0 <= i < out.len() && {}", gm.join(" && ")).unwrap();
        }
        Shape::Plain | Shape::Grouped => {
            writeln!(w, "    &&& out.to_multiset() == rows(db, params).to_multiset()").unwrap();
            writeln!(w, "    &&& forall|i: int, j: int| 0 <= i < j < out.len() ==> !row_lt(#[trigger] out[j], #[trigger] out[i])").unwrap();
            if !defined.is_empty() {
                writeln!(w, "    &&& {}", tuple_defined(b, &defined)).unwrap();
            }
        }
        Shape::Scalar => {
            writeln!(w, "    &&& out.len() == rows(db, params).len()").unwrap();
            writeln!(w, "    &&& forall|i: int| 0 <= i < out.len() ==> row_ok(db, params, #[trigger] out[i])").unwrap();
        }
    }
    writeln!(w, "}}\n").unwrap();
    if top.offset > 0 {
        writeln!(w, "/// OFFSET k: the rows after the first k.").unwrap();
        writeln!(w, "pub open spec fn skip_rows(rows: Seq<OutRow>, k: int) -> Seq<OutRow> {{").unwrap();
        writeln!(w, "    if rows.len() <= k {{ Seq::empty() }} else {{ rows.skip(k) }}").unwrap();
        writeln!(w, "}}\n").unwrap();
    }
    if top.limit.is_some() {
        writeln!(w, "/// LIMIT n: the first n rows, or all rows if there are fewer.").unwrap();
        writeln!(w, "pub open spec fn limit_rows(rows: Seq<OutRow>, n: int) -> Seq<OutRow> {{").unwrap();
        writeln!(w, "    if rows.len() <= n {{ rows }} else {{ rows.take(n) }}").unwrap();
        writeln!(w, "}}\n").unwrap();
    }
    if top.limit.is_some() || top.offset > 0 {
        let what: Vec<String> = top.limit.iter().map(|n| format!("LIMIT {n}")).chain((top.offset > 0).then(|| format!("OFFSET {}", top.offset))).collect();
        writeln!(w, "/// `out` is the answer to the query ({}).", what.join(" ")).unwrap();
        writeln!(w, "pub open spec fn query_result(db: Db, params: Params, out: Seq<OutRow>) -> bool {{").unwrap();
        writeln!(w, "    exists|full: Seq<OutRow>| #[trigger] full_result(db, params, full) && out == {}", cut(top, "full")).unwrap();
        writeln!(w, "}}\n").unwrap();
    } else {
        writeln!(w, "/// `out` is the answer to the query.").unwrap();
        writeln!(w, "pub open spec fn query_result(db: Db, params: Params, out: Seq<OutRow>) -> bool {{").unwrap();
        writeln!(w, "    full_result(db, params, out)").unwrap();
        writeln!(w, "}}\n").unwrap();
    }
    let avail = print_facts(w, top, approx, &defined);
    print_defined(w, top, avail, &defined);
    let tables: Vec<String> = tr.used.keys().map(|t| table_field(t)).collect();
    tr.perm.print(w, &tables, top, &defined);
    writeln!(w, "}} // verus!").unwrap();
    o
}

/// The divisor guards of one output column, as row_ok states them.
fn den_exprs(c: &OutCol) -> Vec<String> {
    c.val
        .den_conds
        .iter()
        .map(|d| match &c.val.guard {
            Some(g) if c.nullable => format!("{g} ==> {d}"),
            _ => d.clone(),
        })
        .collect()
}

/// Every tuple's divisors are non-zero; for a grouped query, those of each group
/// (keyed by a tuple of it) that passes HAVING.
fn tuple_defined(b: &Block, defined: &[String]) -> String {
    let lets: String = b.keys.iter().map(|(g, v)| format!(" let {g} = {};", v.code)).collect();
    let ka: String = b.keys.iter().map(|(g, _)| format!(", {g}")).collect();
    let cond = if b.shape == Shape::Grouped { format!("having(db, params{ka}) ==> ({})", defined.join(" && ")) } else { defined.join(" && ") };
    format!("forall|i: int| 0 <= i < tuples(db, params).len() ==> {{ let t = #[trigger] tuples(db, params)[i];{lets} {cond} }}")
}

/// The answer is stated as a bag of rows (plain SELECT, or GROUP BY whose
/// columns are not all selected: two groups may then give equal rows).
fn as_bag(b: &Block) -> bool {
    b.shape == Shape::Plain || (b.shape == Shape::Grouped && (0..b.keys.len()).any(|i| !b.outs.iter().any(|o| o.key == Some(i))))
}

fn key_cols(b: &Block) -> Vec<&OutCol> {
    let mut v: Vec<&OutCol> = b.outs.iter().filter(|c| c.key.is_some()).collect();
    v.sort_by_key(|c| c.key);
    v.dedup_by_key(|c| c.key);
    v
}

/// An ORDER BY column: `nulls_first` is Some for a NULL-able column.
struct OrdCol {
    field: String,
    ty: Ty,
    desc: bool,
    nulls_first: Option<bool>,
}

fn ord_col(c: &OutCol, desc: bool, nulls_first: bool) -> OrdCol {
    OrdCol { field: c.field.clone(), ty: c.val.ty, desc, nulls_first: c.nullable.then_some(nulls_first) }
}

/// One column comparison: (strictly before, equal).
fn col_cmp(c: &OrdCol) -> (String, String) {
    let f = &c.field;
    let (x, y) = if c.desc { ("b", "a") } else { ("a", "b") };
    let val = |r: &str| if c.nulls_first.is_some() { format!("{r}.{f}->0") } else { format!("{r}.{f}") };
    let (xv, yv) = (val(x), val(y));
    let vlt = match c.ty {
        Ty::Text => format!("bytes_lt({xv}, {yv})"),
        Ty::Bool => format!("(!{xv} && {yv})"),
        _ => format!("{xv} < {yv}"),
    };
    let lt = match c.nulls_first {
        None => vlt,
        Some(first) => {
            let (n, m) = if first { ("a", "b") } else { ("b", "a") };
            format!("(({n}.{f} is None && {m}.{f} is Some) || (a.{f} is Some && b.{f} is Some && {vlt}))")
        }
    };
    (lt, format!("a.{f} == b.{f}"))
}

fn lex_lt_cols(cols: &[OrdCol]) -> String {
    let mut expr = "false".to_string();
    for c in cols.iter().rev() {
        let (lt, eq) = col_cmp(c);
        expr = if expr == "false" { lt } else { format!("{lt} || ({eq} && {})", if expr.contains("||") { format!("({expr})") } else { expr }) };
    }
    expr
}

fn lex_lt(b: &Block, order: &[SortKey]) -> String {
    let cols: Vec<OrdCol> = order.iter().map(|k| ord_col(&b.outs[k.col], k.desc, k.nulls_first)).collect();
    lex_lt_cols(&cols)
}

/// Which existence facts `print_facts` generated (what `lemma_defined_exists` builds on).
#[derive(Clone, Copy, PartialEq)]
enum Facts {
    /// None (a rounded float aggregate, or a column with no canonical order).
    None,
    /// `lemma_query_result_iff` for the single aggregate row.
    Scalar,
    /// `lemma_query_result_iff` for a bag-shaped answer.
    Plain,
    /// A bag-shaped grouped answer with divisors: existence is not stated.
    BagNoExistence,
    /// `lemma_query_result_iff` for a grouped answer; with divisors it requires the
    /// `defined` that `print_grouped_facts` declares.
    Grouped,
}

/// Facts: existence and uniqueness of the answer.
fn print_facts(w: &mut String, top: &Top, approx: bool, defined: &[String]) -> Facts {
    let b = &top.block;
    writeln!(w, "// ── Facts about this spec (verified by Verus, not trusted) ─────────────────").unwrap();
    if approx {
        writeln!(w, "// No facts: a rounded float aggregate has many acceptable values (the bound").unwrap();
        writeln!(w, "// admits the correctly rounded result, but that is not proved here).\n").unwrap();
        return Facts::None;
    }
    if b.shape == Shape::Scalar {
        writeln!(w, "// The answer is the single aggregate row.\n").unwrap();
        let pre = if defined.is_empty() { String::new() } else { "    requires\n        row_ok(db, params, row_of(db, params)),\n".to_string() };
        let c = cut(top, "rows(db, params)");
        writeln!(w, "/// The query has exactly one answer.").unwrap();
        writeln!(w, "pub proof fn lemma_query_result_iff(db: Db, params: Params, out: Seq<OutRow>)\n{pre}    ensures").unwrap();
        writeln!(w, "        query_result(db, params, {c}),\n        query_result(db, params, out) <==> out == {c},\n{{").unwrap();
        writeln!(w, "    let r = rows(db, params);\n    assert(r.len() <= 1 && (r.len() == 1 ==> r[0] == row_of(db, params)));").unwrap();
        writeln!(w, "    assert forall|o: Seq<OutRow>| #[trigger] full_result(db, params, o) <==> o == r by {{").unwrap();
        writeln!(w, "        if full_result(db, params, o) {{\n            if o.len() == 1 {{\n                assert(row_ok(db, params, o[0]));\n                assert(o[0] == row_of(db, params));\n            }}\n            assert(o =~= r);\n        }}").unwrap();
        writeln!(w, "        if o == r && r.len() == 1 {{\n            assert(row_ok(db, params, r[0]));\n        }}\n    }}").unwrap();
        writeln!(w, "    assert(full_result(db, params, r));").unwrap();
        if top.limit.is_some() || top.offset > 0 {
            writeln!(w, "    if query_result(db, params, out) {{").unwrap();
            writeln!(w, "        let full = choose|full: Seq<OutRow>| #[trigger] full_result(db, params, full) && out == {};", cut(top, "full")).unwrap();
            writeln!(w, "    }}").unwrap();
        }
        writeln!(w, "}}\n").unwrap();
        return Facts::Scalar;
    }
    // ORDER BY, extended so that it orders every pair of different rows
    let ordered: Vec<usize> = top.order.iter().map(|k| k.col).collect();
    let mut ext: Vec<OrdCol> = top.order.iter().map(|k| ord_col(&b.outs[k.col], k.desc, k.nulls_first)).collect();
    let tiebreak: Vec<&OutCol> = if as_bag(b) { b.outs.iter().collect() } else { key_cols(b) };
    let mut total = true;
    for t in &tiebreak {
        let i = b.outs.iter().position(|c| c.field == t.field).unwrap();
        if !ordered.contains(&i) {
            total = false;
            if t.val.ty == Ty::Rat {
                writeln!(w, "// No facts: column {} has no canonical order.\n", t.sql).unwrap();
                return Facts::None;
            }
            ext.push(ord_col(t, false, true));
        }
    }
    if total {
        writeln!(w, "// ORDER BY orders every pair of different rows, so the answer is unique.\n").unwrap();
    } else {
        writeln!(w, "// ORDER BY leaves ties, so the answer is unique only up to the order of tied").unwrap();
        writeln!(w, "// rows (as a bag; with LIMIT, which tied rows make the cut is not determined).\n").unwrap();
    }
    // a text column's value: `x.n`, or `x.n->0` when it may be NULL
    let text_cols: Vec<(String, bool)> = ext.iter().filter(|c| c.ty == Ty::Text).map(|c| (c.field.clone(), c.nulls_first.is_some())).collect();
    let tv = |r: &str, n: &(String, bool)| if n.1 { format!("{r}.{}->0", n.0) } else { format!("{r}.{}", n.0) };
    writeln!(w, "/// ORDER BY, extended by {} so that it orders every pair of different rows.", if !as_bag(b) { "the GROUP BY columns" } else { "the other columns" }).unwrap();
    writeln!(w, "pub open spec fn ext_lt(a: OutRow, b: OutRow) -> bool {{\n    {}\n}}\n", lex_lt_cols(&ext)).unwrap();
    writeln!(w, "pub open spec fn ext_order() -> spec_fn(OutRow, OutRow) -> bool {{\n    |a: OutRow, b: OutRow| ext_lt(a, b)\n}}\n").unwrap();
    writeln!(w, "pub proof fn lemma_ext_order()\n    ensures\n        strict_order(ext_order()),\n{{").unwrap();
    writeln!(w, "    assert forall|x: OutRow| !#[trigger] ext_order()(x, x) by {{").unwrap();
    for n in &text_cols {
        writeln!(w, "        lemma_bytes_lt_irrefl({});", tv("x", n)).unwrap();
    }
    writeln!(w, "    }}").unwrap();
    writeln!(w, "    assert forall|x: OutRow, y: OutRow, z: OutRow| #[trigger] ext_order()(x, y) && #[trigger] ext_order()(y, z) implies ext_order()(x, z) by {{").unwrap();
    for n in &text_cols {
        let (x, y, z) = (tv("x", n), tv("y", n), tv("z", n));
        writeln!(w, "        if bytes_lt({x}, {y}) && bytes_lt({y}, {z}) {{ lemma_bytes_lt_trans({x}, {y}, {z}); }}").unwrap();
        writeln!(w, "        if bytes_lt({z}, {y}) && bytes_lt({y}, {x}) {{ lemma_bytes_lt_trans({z}, {y}, {x}); }}").unwrap();
    }
    writeln!(w, "    }}\n}}\n").unwrap();
    let differ: Vec<String> = tiebreak.iter().map(|c| format!("a.{f} != b.{f}", f = c.field)).collect();
    writeln!(w, "/// Rows that differ in {} are ordered one way or the other.", if !as_bag(b) { "a GROUP BY column" } else { "some column" }).unwrap();
    writeln!(w, "pub proof fn lemma_ext_total(a: OutRow, b: OutRow)\n    requires\n        {},\n    ensures\n        ext_order()(a, b) || ext_order()(b, a),\n{{", differ.join(" || ")).unwrap();
    for n in &text_cols {
        let (a, b) = (tv("a", n), tv("b", n));
        let both = if n.1 { format!("a.{0} is Some && b.{0} is Some && ", n.0) } else { String::new() };
        writeln!(w, "    if {both}{a} != {b} {{ lemma_bytes_lt_total({a}, {b}); }}").unwrap();
    }
    writeln!(w, "}}\n").unwrap();
    writeln!(w, "/// ORDER BY implies the extended order{}.", if total { " (they are the same)" } else { "" }).unwrap();
    writeln!(w, "pub proof fn lemma_row_lt_ext(a: OutRow, b: OutRow)\n    ensures\n        row_lt(a, b) ==> ext_order()(a, b),{}\n{{\n}}\n", if total { "\n        row_lt(a, b) == ext_order()(a, b)," } else { "" }).unwrap();
    if as_bag(b) {
        print_plain_facts(w, top, total, defined)
    } else {
        print_grouped_facts(w, top, total, defined);
        Facts::Grouped
    }
}

/// `defined`: the query's divisors are non-zero, so that it has an answer; with
/// the lemmas that make `defined <==> exists an answer` a verified fact.
fn print_defined(w: &mut String, top: &Top, facts: Facts, defined: &[String]) {
    let b = &top.block;
    let own = facts == Facts::Grouped && !defined.is_empty() && !as_bag(b);
    writeln!(w, "// ── The answer exists exactly when `defined` (verified by Verus, not trusted) ──\n").unwrap();
    if !own {
        writeln!(w, "/// No divisor of the query is zero{}.", if defined.is_empty() { " (it has none)" } else { "" }).unwrap();
        writeln!(w, "pub open spec fn defined(db: Db, params: Params) -> bool {{").unwrap();
        if defined.is_empty() {
            writeln!(w, "    true").unwrap();
        } else if b.shape == Shape::Scalar {
            let conj: Vec<String> = b.outs.iter().flat_map(den_exprs).collect();
            if b.having.is_empty() {
                for e in &conj {
                    writeln!(w, "    &&& {e}").unwrap();
                }
            } else {
                let all: Vec<String> = conj.iter().map(|e| format!("({e})")).collect();
                writeln!(w, "    having(db, params) ==> ({})", all.join(" && ")).unwrap();
            }
        } else {
            writeln!(w, "    {}", tuple_defined(b, defined)).unwrap();
        }
        writeln!(w, "}}\n").unwrap();
    }
    let keys = &b.keys;
    let kcols = key_cols(b);
    let tk = |t: &str| -> String { keys.iter().map(|(_, v)| format!(", {}", subst_t(&v.code, t))).collect() };
    let cut_ans = |a: &str| cut(top, a);
    // ── defined ==> an answer exists ──
    let scalar_having = b.shape == Shape::Scalar && !b.having.is_empty() && !defined.is_empty();
    let forward = match facts {
        Facts::None | Facts::BagNoExistence => false,
        Facts::Scalar => !scalar_having,
        Facts::Plain | Facts::Grouped => true,
    };
    if forward {
        writeln!(w, "/// With non-zero divisors the query has an answer.").unwrap();
        writeln!(w, "pub proof fn lemma_defined_exists(db: Db, params: Params)\n    requires\n        defined(db, params),\n    ensures\n        exists|o: Seq<OutRow>| #[trigger] query_result(db, params, o),\n{{").unwrap();
        let a = if facts == Facts::Scalar { "rows(db, params)" } else { "answer(db, params)" };
        if facts == Facts::Scalar && !defined.is_empty() {
            writeln!(w, "    assert(row_ok(db, params, row_of(db, params)));").unwrap();
        }
        if facts == Facts::Plain && !defined.is_empty() {
            writeln!(w, "    assert forall|i: int| 0 <= i < tuples(db, params).len() implies #[trigger] full_result_defined(tuples(db, params)[i]) by {{\n        assert(full_result_defined(tuples(db, params)[i]));\n    }}").unwrap();
        }
        writeln!(w, "    lemma_query_result_iff(db, params, {a});\n    assert(query_result(db, params, {}));\n}}\n", cut_ans(a)).unwrap();
    }
    // ── an answer exists ==> defined ──
    let converse = defined.is_empty() || facts != Facts::None;
    if !converse {
        writeln!(w, "// (No existence lemmas: see the facts above.)\n").unwrap();
        return;
    }
    writeln!(w, "/// An answer exists only if the divisors are non-zero.").unwrap();
    writeln!(w, "pub proof fn lemma_exists_defined(db: Db, params: Params)\n    ensures\n        (exists|o: Seq<OutRow>| #[trigger] query_result(db, params, o)) ==> defined(db, params),\n{{").unwrap();
    if !defined.is_empty() {
        writeln!(w, "    if exists|o: Seq<OutRow>| #[trigger] query_result(db, params, o) {{").unwrap();
        writeln!(w, "        let o = choose|o: Seq<OutRow>| #[trigger] query_result(db, params, o);").unwrap();
        if top.limit.is_some() || top.offset > 0 {
            writeln!(w, "        let full = choose|full: Seq<OutRow>| #[trigger] full_result(db, params, full) && o == {};", cut(top, "full")).unwrap();
        } else {
            writeln!(w, "        let full = o;").unwrap();
        }
        writeln!(w, "        assert(full_result(db, params, full));").unwrap();
        if b.shape == Shape::Scalar {
            writeln!(w, "        assert(full.len() == rows(db, params).len());\n        if full.len() > 0 {{\n            assert(row_ok(db, params, full[0]));\n        }}").unwrap();
        } else if own {
            writeln!(w, "        assert forall|i: int| 0 <= i < tuples(db, params).len() implies #[trigger] row_ok(db, params, present_row(db, params)(tuples(db, params)[i])) || !present_keep(db, params)(tuples(db, params)[i]) by {{").unwrap();
            writeln!(w, "            let t = tuples(db, params)[i];\n            if present_keep(db, params)(t) {{\n                lemma_tuple_group(db, params, i);").unwrap();
            let om: Vec<String> = kcols
                .iter()
                .enumerate()
                .map(|(n, c)| {
                    let kv = subst_t(&keys[c.key.unwrap()].1.code, "t");
                    if n == 0 { format!("(#[trigger] full[j]).{} == {kv}", c.field) } else { format!("full[j].{} == {kv}", c.field) }
                })
                .collect();
            writeln!(w, "                assert(group_count(db, params{}) > 0 && having(db, params{}));", tk("t"), tk("t")).unwrap();
            writeln!(w, "                let j = choose|j: int| 0 <= j < full.len() && {};", om.join(" && ")).unwrap();
            writeln!(w, "                assert(row_ok(db, params, full[j]));\n                assert(row_ok(db, params, present_row(db, params)(t)));\n            }}\n        }}").unwrap();
        }
        writeln!(w, "        assert(defined(db, params));\n    }}").unwrap();
    }
    writeln!(w, "}}\n").unwrap();
}

fn lim(top: &Top, a: &str) -> String {
    cut(top, a)
}

fn print_iff(w: &mut String, top: &Top, total: bool, pre: &str) {
    writeln!(w, "/// The query has an answer{}.", if total { ", and only one" } else { "" }).unwrap();
    writeln!(w, "pub proof fn lemma_query_result_iff(db: Db, params: Params, out: Seq<OutRow>)\n{pre}    ensures\n        query_result(db, params, {}),", lim(top, "answer(db, params)")).unwrap();
    if total {
        writeln!(w, "        query_result(db, params, out) <==> out == {},", lim(top, "answer(db, params)")).unwrap();
    }
    writeln!(w, "{{\n    lemma_answer_ok(db, params);").unwrap();
    if total {
        if top.limit.is_some() || top.offset > 0 {
            writeln!(w, "    if query_result(db, params, out) {{\n        let full = choose|full: Seq<OutRow>| #[trigger] full_result(db, params, full) && out == {};\n        lemma_full_result_unique(db, params, full);\n    }}", cut(top, "full")).unwrap();
        } else {
            writeln!(w, "    if query_result(db, params, out) {{\n        lemma_full_result_unique(db, params, out);\n    }}").unwrap();
        }
    }
    writeln!(w, "}}\n").unwrap();
}

fn print_plain_facts(w: &mut String, top: &Top, total: bool, defined: &[String]) -> Facts {
    writeln!(w, "/// The answer before any LIMIT: the rows, sorted by the extended order.").unwrap();
    writeln!(w, "pub open spec fn answer(db: Db, params: Params) -> Seq<OutRow> {{\n    sort_dups(rows(db, params), ext_order())\n}}\n").unwrap();
    writeln!(w, "proof fn lemma_answer(db: Db, params: Params)\n    ensures\n        strict_order(ext_order()),\n        total_on(ext_order(), rows(db, params)),").unwrap();
    writeln!(w, "        nondesc(answer(db, params), ext_order()),\n        answer(db, params).to_multiset() =~= rows(db, params).to_multiset(),\n{{").unwrap();
    writeln!(w, "    lemma_ext_order();").unwrap();
    writeln!(w, "    assert forall|x: OutRow, y: OutRow| rows(db, params).contains(x) && rows(db, params).contains(y) && x != y implies #[trigger] ext_order()(x, y) || #[trigger] ext_order()(y, x) by {{").unwrap();
    writeln!(w, "        lemma_ext_total(x, y);\n    }}").unwrap();
    writeln!(w, "    lemma_sort_dups(rows(db, params), ext_order());\n}}\n").unwrap();
    if !defined.is_empty() && top.block.shape != Shape::Plain {
        writeln!(w, "// (Existence needs non-zero divisors in every group; not stated here.)\n").unwrap();
        return Facts::BagNoExistence;
    }
    let pre = if defined.is_empty() {
        String::new()
    } else {
        "    requires\n        forall|i: int| 0 <= i < tuples(db, params).len() ==> #[trigger] full_result_defined(tuples(db, params)[i]),\n".to_string()
    };
    if !defined.is_empty() {
        writeln!(w, "pub open spec fn full_result_defined(t: Tuple) -> bool {{\n    {}\n}}\n", defined.join(" && ")).unwrap();
    }
    writeln!(w, "/// The answer satisfies full_result.").unwrap();
    writeln!(w, "pub proof fn lemma_answer_ok(db: Db, params: Params)\n{pre}    ensures\n        full_result(db, params, answer(db, params)),\n{{").unwrap();
    writeln!(w, "    lemma_answer(db, params);\n    let ans = answer(db, params);").unwrap();
    writeln!(w, "    assert forall|i: int, j: int| 0 <= i < j < ans.len() implies !row_lt(#[trigger] ans[j], #[trigger] ans[i]) by {{").unwrap();
    writeln!(w, "        lemma_row_lt_ext(ans[j], ans[i]);\n    }}").unwrap();
    if !defined.is_empty() {
        writeln!(w, "    assert forall|i: int| 0 <= i < tuples(db, params).len() implies {{ let t = #[trigger] tuples(db, params)[i]; {} }} by {{\n        assert(full_result_defined(tuples(db, params)[i]));\n    }}", defined.join(" && ")).unwrap();
    }
    writeln!(w, "}}\n").unwrap();
    writeln!(w, "/// Every full answer is the answer reordered (the same bag).").unwrap();
    writeln!(w, "pub proof fn lemma_full_result_bag(db: Db, params: Params, out: Seq<OutRow>)\n    requires\n        full_result(db, params, out),\n    ensures\n        out.to_multiset() == answer(db, params).to_multiset(),\n{{\n    lemma_answer(db, params);\n}}\n").unwrap();
    if total {
        writeln!(w, "/// full_result has no other solution.").unwrap();
        writeln!(w, "pub proof fn lemma_full_result_unique(db: Db, params: Params, out: Seq<OutRow>)\n    requires\n        full_result(db, params, out),\n    ensures\n        out == answer(db, params),\n{{").unwrap();
        writeln!(w, "    broadcast use vstd::seq_lib::group_to_multiset_ensures;").unwrap();
        writeln!(w, "    lemma_answer(db, params);\n    let ans = answer(db, params);").unwrap();
        writeln!(w, "    assert forall|x: OutRow, y: OutRow| ans.contains(x) && ans.contains(y) && x != y implies #[trigger] ext_order()(x, y) || #[trigger] ext_order()(y, x) by {{").unwrap();
        writeln!(w, "        lemma_ext_total(x, y);\n    }}").unwrap();
        writeln!(w, "    assert forall|i: int, j: int| 0 <= i < j < out.len() implies !ext_order()(#[trigger] out[j], #[trigger] out[i]) by {{").unwrap();
        writeln!(w, "        lemma_row_lt_ext(out[j], out[i]);\n    }}").unwrap();
        writeln!(w, "    lemma_nondesc_unique(ans, out, ext_order());\n}}\n").unwrap();
    }
    print_iff(w, top, total, &pre);
    Facts::Plain
}

fn print_grouped_facts(w: &mut String, top: &Top, total: bool, defined: &[String]) {
    let b = &top.block;
    let keys = &b.keys;
    let kcols = key_cols(b);
    let tk = |t: &str| -> String { keys.iter().map(|(_, v)| format!(", {}", subst_t(&v.code, t))).collect() };
    writeln!(w, "pub open spec fn present_keep(db: Db, params: Params) -> spec_fn(Tuple) -> bool {{").unwrap();
    writeln!(w, "    |t: Tuple| having(db, params{})\n}}\n", tk("t")).unwrap();
    writeln!(w, "pub open spec fn present_row(db: Db, params: Params) -> spec_fn(Tuple) -> OutRow {{").unwrap();
    writeln!(w, "    |t: Tuple| row_of(db, params{})\n}}\n", tk("t")).unwrap();
    writeln!(w, "/// The rows of the non-empty groups that pass HAVING (with repeats).").unwrap();
    writeln!(w, "pub open spec fn present(db: Db, params: Params) -> Seq<OutRow> {{").unwrap();
    writeln!(w, "    map_where(tuples(db, params), present_keep(db, params), present_row(db, params))\n}}\n").unwrap();
    writeln!(w, "/// The answer before any LIMIT: those rows, without repeats, in the extended order.").unwrap();
    writeln!(w, "pub open spec fn answer(db: Db, params: Params) -> Seq<OutRow> {{").unwrap();
    writeln!(w, "    sort_by(present(db, params), ext_order())\n}}\n").unwrap();
    let pre = if defined.is_empty() {
        String::new()
    } else {
        writeln!(w, "/// No division by zero in the rows of the groups (else SQL raises an error).").unwrap();
        writeln!(w, "pub open spec fn defined(db: Db, params: Params) -> bool {{").unwrap();
        writeln!(w, "    forall|i: int| 0 <= i < tuples(db, params).len() ==> #[trigger] row_ok(db, params, present_row(db, params)(tuples(db, params)[i])) || !present_keep(db, params)(tuples(db, params)[i])").unwrap();
        writeln!(w, "}}\n").unwrap();
        "    requires\n        defined(db, params),\n".to_string()
    };
    let pick = |v: &str| format!("choose|k: int| 0 <= k < tuples(db, params).len() && present_keep(db, params)(tuples(db, params)[k]) && present_row(db, params)(tuples(db, params)[k]) == {v}");
    writeln!(w, "proof fn lemma_present(db: Db, params: Params)\n    ensures\n        strict_order(ext_order()),").unwrap();
    writeln!(w, "        sorted_by(answer(db, params), ext_order()),").unwrap();
    writeln!(w, "        forall|y: OutRow| #[trigger] answer(db, params).contains(y) <==> present(db, params).contains(y),").unwrap();
    writeln!(w, "        forall|y: OutRow| #[trigger] present(db, params).contains(y) <==> exists|k: int|").unwrap();
    writeln!(w, "            0 <= k < tuples(db, params).len() && present_keep(db, params)(tuples(db, params)[k]) && present_row(db, params)(tuples(db, params)[k]) == y,\n{{").unwrap();
    writeln!(w, "    lemma_ext_order();\n    lemma_map_where(tuples(db, params), present_keep(db, params), present_row(db, params));").unwrap();
    writeln!(w, "    let xs = present(db, params);").unwrap();
    writeln!(w, "    assert forall|x: OutRow, y: OutRow| xs.contains(x) && xs.contains(y) && x != y implies #[trigger] ext_order()(x, y) || #[trigger] ext_order()(y, x) by {{").unwrap();
    writeln!(w, "        let i = {};\n        let j = {};", pick("x"), pick("y")).unwrap();
    let differ: Vec<String> = kcols.iter().map(|c| format!("x.{f} != y.{f}", f = c.field)).collect();
    writeln!(w, "        if {} {{ lemma_ext_total(x, y); }}\n    }}", differ.join(" || ")).unwrap();
    writeln!(w, "    lemma_sort_by(xs, ext_order());\n}}\n").unwrap();
    // a tuple's group is non-empty / a non-empty group has a tuple
    let cond_t0: Vec<String> = keys.iter().map(|(_, v)| format!("{} == {}", v.code, subst_t(&v.code, "t0"))).collect();
    writeln!(w, "/// A tuple's group is non-empty.").unwrap();
    writeln!(w, "proof fn lemma_tuple_group(db: Db, params: Params, k: int)\n    requires\n        0 <= k < tuples(db, params).len(),\n    ensures").unwrap();
    writeln!(w, "        group_count(db, params{}) > 0,\n{{", tk("tuples(db, params)[k]")).unwrap();
    writeln!(w, "    let t0 = tuples(db, params)[k];").unwrap();
    writeln!(w, "    let f = |t: Tuple| if {} {{ 1int }} else {{ 0 }};", cond_t0.join(" && ")).unwrap();
    writeln!(w, "    assert(group_count(db, params{}) == sum(tuples(db, params), f));", tk("t0")).unwrap();
    writeln!(w, "    lemma_sum_pos(tuples(db, params), f, k);\n}}\n").unwrap();
    let gq: String = keys.iter().map(|(g, v)| format!(", {g}: {}", key_ty(v))).collect();
    let ga: String = keys.iter().map(|(g, _)| format!(", {g}")).collect();
    writeln!(w, "/// A non-empty group has a tuple.").unwrap();
    writeln!(w, "proof fn lemma_group_tuple(db: Db, params: Params{gq}) -> (k: int)\n    requires\n        group_count(db, params{ga}) > 0,\n    ensures\n        0 <= k < tuples(db, params).len(),").unwrap();
    for (g, v) in keys {
        writeln!(w, "        {} == {g},", subst_t(&v.code, "tuples(db, params)[k]")).unwrap();
    }
    writeln!(w, "{{").unwrap();
    let kc: Vec<String> = keys.iter().map(|(g, v)| format!("{} == {g}", v.code)).collect();
    writeln!(w, "    lemma_sum_nonzero_witness(tuples(db, params), |t: Tuple| if {} {{ 1int }} else {{ 0 }})\n}}\n", kc.join(" && ")).unwrap();
    // the answer is an answer: one lemma per conjunct of full_result, combined
    // with `tuples` hidden (unfolding the FROM product there is costly)
    let a = "answer(db, params)";
    let gq2: Vec<String> = keys.iter().map(|(g, v)| format!("{g}: {}", key_ty(v))).collect();
    let gm = |ans: &str| -> String {
        kcols
            .iter()
            .enumerate()
            .map(|(n, c)| {
                let g = &keys[c.key.unwrap()].0;
                if n == 0 { format!("(#[trigger] {ans}[i]).{} == {g}", c.field) } else { format!("{ans}[i].{} == {g}", c.field) }
            })
            .collect::<Vec<_>>()
            .join(" && ")
    };
    writeln!(w, "/// Every row of the answer is a group's row.").unwrap();
    writeln!(w, "proof fn lemma_answer_rows(db: Db, params: Params)\n{pre}    ensures\n        forall|i: int| 0 <= i < {a}.len() ==> row_ok(db, params, #[trigger] {a}[i]),\n{{").unwrap();
    writeln!(w, "    lemma_present(db, params);\n    let ans = answer(db, params);").unwrap();
    writeln!(w, "    assert forall|i: int| 0 <= i < ans.len() implies row_ok(db, params, #[trigger] ans[i]) by {{").unwrap();
    if defined.is_empty() {
        writeln!(w, "        assert(ans.contains(ans[i]));\n        let k = {};\n        lemma_tuple_group(db, params, k);\n    }}\n}}\n", pick("ans[i]")).unwrap();
    } else {
        // `defined` gives row_ok of every present group's row directly
        writeln!(w, "        assert(ans.contains(ans[i]));\n        let k = {};", pick("ans[i]")).unwrap();
        writeln!(w, "        assert(row_ok(db, params, present_row(db, params)(tuples(db, params)[k])));\n    }}\n}}\n").unwrap();
    }
    writeln!(w, "/// The answer's rows are of distinct groups, in ORDER BY order.").unwrap();
    writeln!(w, "proof fn lemma_answer_order(db: Db, params: Params)\n    ensures\n        forall|i: int, j: int| 0 <= i < j < {a}.len() ==> !same_group(#[trigger] {a}[i], #[trigger] {a}[j]) && !row_lt({a}[j], {a}[i]),\n{{").unwrap();
    writeln!(w, "    lemma_present(db, params);\n    let ans = answer(db, params);").unwrap();
    writeln!(w, "    assert forall|i: int, j: int| 0 <= i < j < ans.len() implies !same_group(#[trigger] ans[i], #[trigger] ans[j]) && !row_lt(ans[j], ans[i]) by {{").unwrap();
    writeln!(w, "        assert(ext_order()(ans[i], ans[j]));\n        lemma_row_lt_ext(ans[j], ans[i]);").unwrap();
    writeln!(w, "        assert(ans.contains(ans[i]) && ans.contains(ans[j]));").unwrap();
    writeln!(w, "        let ki = {};\n        let kj = {};\n    }}\n}}\n", pick("ans[i]"), pick("ans[j]")).unwrap();
    writeln!(w, "/// Every non-empty group that passes HAVING has a row in the answer.").unwrap();
    writeln!(w, "proof fn lemma_answer_groups(db: Db, params: Params)\n    ensures\n        forall|{}| #[trigger] group_count(db, params{ga}) > 0 && having(db, params{ga}) ==> exists|i: int| 0 <= i < {a}.len() && {},\n{{", gq2.join(", "), gm(a)).unwrap();
    writeln!(w, "    lemma_present(db, params);\n    let ans = answer(db, params);").unwrap();
    writeln!(w, "    assert forall|{}| #[trigger] group_count(db, params{ga}) > 0 && having(db, params{ga}) implies exists|i: int| 0 <= i < ans.len() && {} by {{", gq2.join(", "), gm("ans")).unwrap();
    writeln!(w, "        let k = lemma_group_tuple(db, params{ga});").unwrap();
    writeln!(w, "        let y = row_of(db, params{ga});").unwrap();
    writeln!(w, "        assert(present_keep(db, params)(tuples(db, params)[k]) && present_row(db, params)(tuples(db, params)[k]) == y);").unwrap();
    writeln!(w, "        assert(answer(db, params).contains(y));").unwrap();
    writeln!(w, "        let i = choose|i: int| 0 <= i < ans.len() && ans[i] == y;\n        assert(ans[i] == y);\n    }}\n}}\n").unwrap();
    writeln!(w, "/// The answer satisfies full_result.").unwrap();
    writeln!(w, "pub proof fn lemma_answer_ok(db: Db, params: Params)\n{pre}    ensures\n        full_result(db, params, answer(db, params)),\n{{").unwrap();
    writeln!(w, "    hide(tuples);\n    lemma_answer_rows(db, params);\n    lemma_answer_order(db, params);\n    lemma_answer_groups(db, params);\n}}\n").unwrap();
    // every full answer has the answer's rows
    let rkey: String = kcols.iter().map(|c| format!(", y.{}", c.field)).collect();
    writeln!(w, "/// Every full answer has exactly the answer's rows, without repeats.").unwrap();
    writeln!(w, "proof fn lemma_full_result_rows(db: Db, params: Params, out: Seq<OutRow>)\n    requires\n        full_result(db, params, out),\n    ensures").unwrap();
    writeln!(w, "        forall|y: OutRow| out.contains(y) <==> answer(db, params).contains(y),\n        forall|i: int, j: int| 0 <= i < j < out.len() ==> out[i] != out[j],\n{{").unwrap();
    writeln!(w, "    lemma_present(db, params);\n    let ans = answer(db, params);").unwrap();
    writeln!(w, "    assert forall|y: OutRow| out.contains(y) <==> ans.contains(y) by {{").unwrap();
    writeln!(w, "        if out.contains(y) {{").unwrap();
    writeln!(w, "            let i = choose|i: int| 0 <= i < out.len() && out[i] == y;").unwrap();
    writeln!(w, "            assert(row_ok(db, params, out[i]));").unwrap();
    writeln!(w, "            assert(y == row_of(db, params{rkey}));").unwrap();
    writeln!(w, "            let k = lemma_group_tuple(db, params{rkey});").unwrap();
    writeln!(w, "            assert(present_keep(db, params)(tuples(db, params)[k]) && present_row(db, params)(tuples(db, params)[k]) == y);\n        }}").unwrap();
    writeln!(w, "        if ans.contains(y) {{").unwrap();
    writeln!(w, "            let k = {};", pick("y")).unwrap();
    writeln!(w, "            lemma_tuple_group(db, params, k);").unwrap();
    writeln!(w, "            let t0 = tuples(db, params)[k];").unwrap();
    let om: Vec<String> = kcols
        .iter()
        .enumerate()
        .map(|(n, c)| {
            let kv = subst_t(&keys[c.key.unwrap()].1.code, "t0");
            if n == 0 { format!("(#[trigger] out[i]).{} == {kv}", c.field) } else { format!("out[i].{} == {kv}", c.field) }
        })
        .collect();
    writeln!(w, "            assert(group_count(db, params{}) > 0 && having(db, params{}));", tk("t0"), tk("t0")).unwrap();
    writeln!(w, "            let i = choose|i: int| 0 <= i < out.len() && {};", om.join(" && ")).unwrap();
    writeln!(w, "            assert(row_ok(db, params, out[i]));").unwrap();
    writeln!(w, "            assert(out[i] == y);\n        }}\n    }}").unwrap();
    writeln!(w, "    assert forall|i: int, j: int| 0 <= i < j < out.len() implies out[i] != out[j] by {{\n        assert(!same_group(out[i], out[j]));\n    }}\n}}\n").unwrap();
    writeln!(w, "/// Every full answer is the answer reordered (the same bag).").unwrap();
    writeln!(w, "pub proof fn lemma_full_result_bag(db: Db, params: Params, out: Seq<OutRow>)\n    requires\n        full_result(db, params, out),\n    ensures\n        out.to_multiset() == answer(db, params).to_multiset(),\n{{").unwrap();
    writeln!(w, "    lemma_present(db, params);\n    lemma_full_result_rows(db, params, out);\n    let ans = answer(db, params);").unwrap();
    writeln!(w, "    assert forall|i: int, j: int| 0 <= i < j < ans.len() implies ans[i] != ans[j] by {{\n        assert(ext_order()(ans[i], ans[j]));\n    }}").unwrap();
    writeln!(w, "    lemma_same_set_multiset(out, ans);\n}}\n").unwrap();
    if total {
        writeln!(w, "/// full_result has no other solution.").unwrap();
        writeln!(w, "pub proof fn lemma_full_result_unique(db: Db, params: Params, out: Seq<OutRow>)\n    requires\n        full_result(db, params, out),\n    ensures\n        out == answer(db, params),\n{{").unwrap();
        writeln!(w, "    lemma_present(db, params);\n    lemma_full_result_rows(db, params, out);").unwrap();
        writeln!(w, "    assert forall|i: int, j: int| 0 <= i < j < out.len() implies ext_order()(#[trigger] out[i], #[trigger] out[j]) by {{").unwrap();
        let differ: Vec<String> = kcols.iter().map(|c| format!("out[i].{f} != out[j].{f}", f = c.field)).collect();
        writeln!(w, "        assert(!same_group(out[i], out[j]));\n        assert({});", differ.join(" || ")).unwrap();
        writeln!(w, "        lemma_ext_total(out[i], out[j]);\n        lemma_row_lt_ext(out[j], out[i]);\n    }}").unwrap();
        writeln!(w, "    lemma_sorted_unique(out, answer(db, params), ext_order());\n}}\n").unwrap();
    }
    print_iff(w, top, total, &pre);
}

fn strip_sql_comments(s: &str) -> String {
    s.lines().filter(|l| !l.trim_start().starts_with("--")).collect::<Vec<_>>().join("\n").trim().trim_end_matches(';').to_string()
}

fn opt0(args: &[String], k: &str) -> Option<String> {
    args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).cloned()
}

async fn run() -> Res<()> {
    let args: Vec<String> = std::env::args().collect();
    let get = |k: &str| -> Res<String> {
        args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).cloned().ok_or_else(|| format!("missing {k}"))
    };
    let (schema_path, query_path, out_path) = (get("--schema")?, get("--query")?, get("--out")?);
    let ddl = std::fs::read_to_string(&schema_path).map_err(|e| format!("{schema_path}: {e}"))?;
    let sql = strip_sql_comments(&std::fs::read_to_string(&query_path).map_err(|e| format!("{query_path}: {e}"))?);
    // SQL numeric literals are exact: parse 0.06 as DECIMAL, not DOUBLE
    let cfg = SessionConfig::new().set_bool("datafusion.sql_parser.parse_float_as_decimal", true);
    let ctx = SessionContext::new_with_config(cfg);
    // CHAR(n) semantics of the target engine (the spec models the engine the accelerator runs on)
    let engine = opt0(&args, "--engine").unwrap_or_else(|| "duckdb".to_string());
    let char_semantics: (&'static str, &'static str) = match opt0(&args, "--char-semantics").as_deref() {
        Some("bytes") => ("bytes", "set by --char-semantics"),
        Some("padded") => ("padded", "set by --char-semantics"),
        Some(o) => return Err(format!("--char-semantics: bytes or padded, not {o}")),
        None => match engine.as_str() {
            "duckdb" => ("bytes", "DuckDB: CHAR(n) is an alias of VARCHAR"),
            "postgres" => ("padded", "Postgres bpchar; a parameter compared with it must bind as bpchar, not text"),
            "mysql" => ("padded", "MySQL CHAR under a PAD SPACE collation"),
            e => return Err(format!("--engine: duckdb, postgres or mysql, not {e}")),
        },
    };
    // NULLs sort as the largest value in Postgres, as the smallest in MySQL, and last in DuckDB
    let nulls_first_default = match engine.as_str() {
        "postgres" => [false, true],
        "mysql" => [true, false],
        _ => [false, false],
    };
    let sch = load_schema(&ctx, &ddl, char_semantics).await?;
    // Unoptimized: SQL -> LogicalPlan, name/type resolution only.
    let plan = ctx.state().create_logical_plan(&sql).await.map_err(|e| format!("DataFusion: {e}"))?;
    let mut tr = Tr {
        sch: &sch,
        frames: vec![],
        used: BTreeMap::new(),
        params: BTreeMap::new(),
        items: vec![],
        vars: HashSet::new(),
        rels: HashMap::new(),
        counters: HashMap::new(),
        notes: BTreeSet::new(),
        pfx: String::new(),
        perm: perm::Perm::default(),
        guarded_vals: HashSet::new(),
        nulls_first_default,
    };
    let top = translate(&mut tr, &plan, &explicit_nulls(&sql)?)?;
    let src_name = std::path::Path::new(&query_path).file_name().unwrap().to_string_lossy().to_string();
    let text = print(&tr, &top, &sql, &src_name);
    std::fs::write(&out_path, &text).map_err(|e| format!("{out_path}: {e}"))?;
    let opt = |k: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).cloned();
    if let Some(ob_path) = opt("--obligations") {
        let inputs = match opt("--inputs") {
            Some(p) => {
                let t = std::fs::read_to_string(&p).map_err(|e| format!("{p}: {e}"))?;
                let n = std::path::Path::new(&p).file_name().unwrap().to_string_lossy().to_string();
                Some((n, t))
            }
            None => None,
        };
        let inputs = inputs.as_ref().map(|(n, t)| (n.as_str(), t.clone()));
        let (ob, stub, model) = obligations::obligations(&mut tr, &top, &ctx, &text, inputs, &src_name, &sql).await.map_err(|e| format!("obligations: {e}"))?;
        std::fs::write(&ob_path, ob).map_err(|e| format!("{ob_path}: {e}"))?;
        if let Some(sp) = opt("--kernel-stub") {
            std::fs::write(&sp, stub).map_err(|e| format!("{sp}: {e}"))?;
        }
        if let Some(wp) = opt("--world") {
            std::fs::write(&wp, glue::world(&model)).map_err(|e| format!("{wp}: {e}"))?;
        }
        if let Some(fp) = opt("--ffi") {
            let hp = get("--ffi-header")?;
            let prefix = get("--ffi-prefix")?;
            let hname = std::path::Path::new(&hp).file_name().unwrap().to_string_lossy().to_string();
            let (rs, h) = glue::ffi(&model, &prefix, &hname).map_err(|e| format!("ffi: {e}"))?;
            std::fs::write(&fp, rs).map_err(|e| format!("{fp}: {e}"))?;
            std::fs::write(&hp, h).map_err(|e| format!("{hp}: {e}"))?;
            if let Some(gp) = opt("--host-glue") {
                let qid: u32 = get("--query-id")?.parse().map_err(|_| "--query-id: a number".to_string())?;
                let cpp = glue::kit(&model, &prefix, &hname, &get("--accel-id")?, &get("--workload")?, qid).map_err(|e| format!("host glue: {e}"))?;
                std::fs::write(&gp, cpp).map_err(|e| format!("{gp}: {e}"))?;
            }
        }
    }
    Ok(())
}

#[tokio::main]
async fn main() {
    if let Err(e) = run().await {
        eprintln!("sql2verus: refused: {e}");
        std::process::exit(1);
    }
}
