// Compiled structural twin of src/generated/tpch/q21/postgres/aux/module.cpp
// (post-fix, 2026-09-29).  Ordinary (compiled) functions; lemmas are ghost.
//
// C++ unordered_map iteration order is unspecified: every loop over a hash map
// takes an explicit enumeration `en` of the map's keys (IsEnum).  Refinement
// lemmas (Q21TwinAux.dfy) are proved for ALL enumerations.  Compiled callers
// supply IntKeys (ascending) enumerations; string-keyed maps are enumerated
// by the caller (C++ hash order).
//
// Arithmetic: Dafny `int` (unbounded).  C++ uses int64 counts / int32 keys;
// the verified-domain premise bounds them (see Q21Aux.DomainOk).
//
// SOURCE MAP (module.cpp line numbers; update together with the C++):
//   TpchQ21ComputeOrderContributions   :15    -> ComputeOrderContributions
//   BetterOutputRow                    :103   -> Better
//   MergeByNameAndSort                 :112   -> MergeByNameAndSort
//   MakeStateImage                     :123   -> MakeStateImage / NationRows
//   AsQ21Image                         :149   -> AsQ21Image
//   NationPartition                    :162   -> NationPartition
//   NationNamePartition                :169   -> NationNamePartition
//   ApplyContributionDeltas            :240   -> ApplyContributionDeltas
//   TpchQ21TxnBuffer::*                :484   -> TxnBuffer / MarkRebuild / Add*
//   Build                              :545   -> BuildImages
//   AccumulateMutation                 :595   -> AccumulateMutation
//   StageMaintenance                   :689   -> StageMaintenance
//   Execute                            :1025  -> Execute
//   ExecuteFallback                    :1150  -> ExecuteFallback
//   BuildState                         :1175  -> BuildState
//   LoadStateForNations                :1236  -> LoadStateForNations
//   EmitResult                         :1273  -> EmitResult
//   TpchQ21State::FindOrder/MutableOrder (module.hpp) -> FindOrder / MutableOrder
// Not twinned (not on the Postgres runtime path): Encode*/Decode* byte images,
// BuildOrderSummary (unused helper).
//
// Substrate oracles (inputs, not modeled here): StateStore reads (snapshot
// maps), ChooseMaintenance decisions, TpchQ21InitialScan (typed table scans),
// row decoding (Read*/…Cell parsers -> typed rows or parse failure).

include "Q21Types.dfy"

module Q21ModuleTwin {
  import opened Q21Types

  // ===================== Enumerations =====================

  ghost predicate IsEnum<K(!new)>(en: seq<K>, s: set<K>)
  {
    (forall i, j :: 0 <= i < j < |en| ==> en[i] != en[j]) &&
    (forall x :: x in en <==> x in s)
  }

  lemma IntMinExists(s: set<int>)
    requires s != {}
    ensures exists m :: m in s && forall y :: y in s ==> m <= y
    decreases |s|
  {
    var x :| x in s;
    if s - {x} == {} {
      forall y | y in s ensures x <= y { if y != x { assert y in s - {x}; } }
    } else {
      IntMinExists(s - {x});
      var m :| m in s - {x} && forall y :: y in s - {x} ==> m <= y;
      if x < m {
        forall y | y in s ensures x <= y { if y != x { assert y in s - {x}; } }
      } else {
        forall y | y in s ensures m <= y { if y != x { assert y in s - {x}; } }
      }
    }
  }

  function IntKeys(s: set<int>): seq<int>
    decreases |s|
  {
    if s == {} then []
    else
      IntMinExists(s);
      var m :| m in s && forall y :: y in s ==> m <= y;
      [m] + IntKeys(s - {m})
  }

  lemma IntKeys_enum(s: set<int>)
    ensures IsEnum(IntKeys(s), s)
    decreases |s|
  {
    if s != {} {
      IntMinExists(s);
      var m :| m in s && forall y :: y in s ==> m <= y;
      IntKeys_enum(s - {m});
      assert IntKeys(s) == [m] + IntKeys(s - {m});
    }
  }

  // Byte-lexicographic string order (C++ std::string operator<, which compares
  // as unsigned char; Dafny chars carry byte values 0..255).
  predicate StrLt(a: string, b: string)
    decreases |a|
  {
    if |b| == 0 then false
    else if |a| == 0 then true
    else if a[0] != b[0] then a[0] < b[0]
    else StrLt(a[1..], b[1..])
  }

  lemma StrLt_irrefl(a: string)
    ensures !StrLt(a, a)
  { if |a| > 0 { StrLt_irrefl(a[1..]); } }

  lemma StrLt_trans(a: string, b: string, c: string)
    requires StrLt(a, b) && StrLt(b, c)
    ensures StrLt(a, c)
    decreases |a|
  {
    if |a| > 0 && |b| > 0 && |c| > 0 && a[0] == b[0] && b[0] == c[0] {
      StrLt_trans(a[1..], b[1..], c[1..]);
    }
  }

  lemma StrLt_total(a: string, b: string)
    requires a != b
    ensures StrLt(a, b) || StrLt(b, a)
    decreases |a|
  {
    if |a| > 0 && |b| > 0 && a[0] == b[0] {
      assert a[1..] != b[1..] by {
        if a[1..] == b[1..] { assert a == [a[0]] + a[1..] && b == [b[0]] + b[1..]; }
      }
      StrLt_total(a[1..], b[1..]);
    }
  }


  // ----- String enumeration (compiled; byte-lexicographic ascending) -----

  predicate IsStrMin(s: set<string>, m: string)
  { m in s && forall y :: y in s && y != m ==> StrLt(m, y) }

  lemma StrMinExists(s: set<string>)
    requires s != {}
    ensures exists m :: IsStrMin(s, m)
    decreases |s|
  {
    var x :| x in s;
    if s - {x} == {} {
      assert IsStrMin(s, x) by {
        forall y | y in s && y != x ensures StrLt(x, y) { assert y in s - {x}; }
      }
    } else {
      StrMinExists(s - {x});
      var m :| IsStrMin(s - {x}, m);
      StrLt_total(x, m);
      if StrLt(x, m) {
        forall y | y in s && y != x ensures StrLt(x, y) {
          if y != m { assert y in s - {x}; StrLt_trans(x, m, y); }
        }
        assert IsStrMin(s, x);
      } else {
        forall y | y in s && y != m ensures StrLt(m, y) { if y != x { assert y in s - {x}; } }
        assert IsStrMin(s, m);
      }
    }
  }

  lemma StrMinUnique(s: set<string>)
    ensures forall m1, m2 :: IsStrMin(s, m1) && IsStrMin(s, m2) ==> m1 == m2
  {
    forall m1, m2 | IsStrMin(s, m1) && IsStrMin(s, m2) ensures m1 == m2 {
      if m1 != m2 { assert StrLt(m1, m2) && StrLt(m2, m1); StrLt_trans(m1, m2, m1); StrLt_irrefl(m1); }
    }
  }

  function StrKeys(s: set<string>): seq<string>
    decreases |s|
  {
    if s == {} then []
    else
      StrMinExists(s); StrMinUnique(s);
      var m :| m in s && IsStrMin(s, m);
      [m] + StrKeys(s - {m})
  }

  lemma StrKeys_enum(s: set<string>)
    ensures IsEnum(StrKeys(s), s)
    decreases |s|
  {
    if s != {} {
      StrMinExists(s); StrMinUnique(s);
      var m :| m in s && IsStrMin(s, m);
      StrKeys_enum(s - {m});
      assert StrKeys(s) == [m] + StrKeys(s - {m});
    }
  }

  // Iteration-order oracles: any enumeration of each key set.
  ghost predicate EnumOracle(ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
  { (forall s :: IsEnum(ei(s), s)) && (forall s :: IsEnum(es(s), s)) }

  lemma SortedOracle()
    ensures EnumOracle(IntKeys, StrKeys)
  {
    forall s: set<int> ensures IsEnum(IntKeys(s), s) { IntKeys_enum(s); }
    forall s: set<string> ensures IsEnum(StrKeys(s), s) { StrKeys_enum(s); }
  }

  // ===================== State (module.hpp) =====================

  datatype OrderSummary = OrderSummary(orderstatus: char, suppliers: map<int, SOI>)

  datatype TState = TState(
    supplier_names: map<int, string>,
    supplier_nations: map<int, int>,
    nation_name_to_keys: map<string, seq<int>>,
    order_summaries: map<int, OrderSummary>,   // shared base map
    order_overlay: map<int, OrderSummary>,     // per-image overlay
    nation_numwait: map<int, map<int, int>>)

  function EmptyState(): TState { TState(map[], map[], map[], map[], map[], map[]) }

  datatype OptSummary = NoSummary | SomeSummary(s: OrderSummary)

  function FindOrder(st: TState, ok: int): OptSummary
  {
    if ok in st.order_overlay then SomeSummary(st.order_overlay[ok])
    else if ok in st.order_summaries then SomeSummary(st.order_summaries[ok])
    else NoSummary
  }

  // MutableOrder: copy the base summary into the overlay (or create a default
  // one) and return the overlay value.
  function MutableOrderGet(st: TState, ok: int): OrderSummary
  {
    match FindOrder(st, ok)
    case SomeSummary(s) => s
    case NoSummary => OrderSummary(0 as char, map[])
  }

  function PutOverlay(st: TState, ok: int, s: OrderSummary): TState
  { st.(order_overlay := st.order_overlay[ok := s]) }

  datatype OutRow = OutRow(name: string, numwait: int)

  // ===================== Contributions (:15) =====================

  function ContribLoop(sup: map<int, SOI>, en: seq<int>, sole_sk: int, sole_cnt: int, late_suppliers: int): map<int, int>
    requires forall x :: x in en ==> x in sup
    decreases |en|
  {
    if |en| == 0 then
      (if late_suppliers == 1 then map[sole_sk := sole_cnt] else map[])
    else
      var info := sup[en[0]];
      if info.total <= 0 || info.late <= 0 then ContribLoop(sup, en[1..], sole_sk, sole_cnt, late_suppliers)
      else if late_suppliers + 1 > 1 then map[]  // NOT EXISTS fails for every candidate
      else ContribLoop(sup, en[1..], en[0], info.late, late_suppliers + 1)
  }

  function ComputeOrderContributions(order: OrderSummary, en: seq<int>): map<int, int>
    requires forall x :: x in en ==> x in order.suppliers
  {
    if order.orderstatus != 'F' || |order.suppliers| <= 1 then map[]
    else ContribLoop(order.suppliers, en, 0, 0, 0)
  }

  // ===================== Output rows (:103, :112, :123) =====================

  predicate Better(a: OutRow, b: OutRow)
  { if a.numwait != b.numwait then a.numwait > b.numwait else StrLt(a.name, b.name) }

  // std::map<std::string,int64_t> by_name; by_name[row.name] += row.numwait.
  // Modeled as an association list (the pre-sort order of by_name is not
  // observable: std::sort with BetterOutputRow totally orders distinct names).
  function AddByName(acc: seq<OutRow>, r: OutRow): seq<OutRow>
    decreases |acc|
  {
    if |acc| == 0 then [r]
    else if acc[0].name == r.name then [OutRow(r.name, acc[0].numwait + r.numwait)] + acc[1..]
    else [acc[0]] + AddByName(acc[1..], r)
  }

  function MergeRows(acc: seq<OutRow>, rows: seq<OutRow>): seq<OutRow>
    decreases |rows|
  { if |rows| == 0 then acc else MergeRows(AddByName(acc, rows[0]), rows[1..]) }

  // std::sort with BetterOutputRow (strict weak order; names are distinct after
  // the merge, so every correct sort produces the same sequence).
  function InsertBetter(x: OutRow, xs: seq<OutRow>): seq<OutRow>
  { if |xs| == 0 then [x] else if Better(xs[0], x) then [xs[0]] + InsertBetter(x, xs[1..]) else [x] + xs }

  function SortBetter(xs: seq<OutRow>): seq<OutRow>
  { if |xs| == 0 then [] else InsertBetter(xs[0], SortBetter(xs[1..])) }

  function MergeByNameAndSort(rows: seq<OutRow>): seq<OutRow>
  { SortBetter(MergeRows([], rows)) }

  // Per-supplier rows of one nation map, iterated in enumeration `en`.
  function NationRowsLoop(nw: map<int, int>, names: map<int, string>, en: seq<int>): seq<OutRow>
    requires forall x :: x in en ==> x in nw
    decreases |en|
  {
    if |en| == 0 then []
    else
      var sk := en[0];
      var rest := NationRowsLoop(nw, names, en[1..]);
      if nw[sk] <= 0 then rest
      else if sk in names then [OutRow(names[sk], nw[sk])] + rest
      else rest
  }

  // MakeStateImage(kind == Nation): the pre-sorted output_rows of a nation.
  function NationRows(st: TState, nk: int, en: seq<int>): seq<OutRow>
    requires nk in st.nation_numwait ==> forall x :: x in en ==> x in st.nation_numwait[nk]
  {
    if nk !in st.nation_numwait then []
    else MergeByNameAndSort(NationRowsLoop(st.nation_numwait[nk], st.supplier_names, en))
  }

  // ===================== Images / layout =====================

  datatype ImageKind = SupplierMetadata | OrderSummaries | NationNameIndex | NationK

  datatype TImage = TImage(kind: ImageKind, nationkey: int, nation_name: string,
                           state: TState, output_rows: seq<OutRow>)

  const kSupplierMetadata: int := 0
  const kOrderSummaries: int := 1
  const kNationNameIndex: int := 2
  const kNationBase: int := 1000
  const kNationNameBase: int := 2000000
  const kNationNameSlots: int := 1048576

  function NationPartition(nk: int): int { kNationBase + nk }

  // FNV-1a over the name's bytes (uint64 wrap-around), bucketed.
  function Fnv1a(name: string, h: int): int
    requires 0 <= h < 0x1_0000_0000_0000_0000
    ensures 0 <= Fnv1a(name, h) < 0x1_0000_0000_0000_0000
  {
    if |name| == 0 then h
    else
      var x := ((h as bv64) ^ ((name[0] as int % 256) as bv64)) as int;
      Fnv1a(name[1..], (x * 1099511628211) % 0x1_0000_0000_0000_0000)
  }

  function NationNamePartition(name: string): int
  { kNationNameBase + Fnv1a(name, 1469598103934665603) % kNationNameSlots }

  datatype OptImage = NoImage | SomeImage(img: TImage)

  // AsQ21Image: payload present with the expected kind (state non-null is
  // implicit: images always carry a state here).
  function AsQ21Image(snap: map<int, TImage>, pid: int, kind: ImageKind): OptImage
  {
    if pid in snap && snap[pid].kind == kind then SomeImage(snap[pid]) else NoImage
  }

  function MakeImage(kind: ImageKind, nk: int, st: TState, name: string, en: seq<int>): TImage
    requires kind == NationK && nk in st.nation_numwait ==> forall x :: x in en ==> x in st.nation_numwait[nk]
  {
    TImage(kind, nk, name, st, if kind == NationK then NationRows(st, nk, en) else [])
  }

  // ===================== BuildState (:1175) =====================

  function ScanSuppliers(st: TState, S: seq<Sup>): TState
    decreases |S|
  {
    if |S| == 0 then st
    else ScanSuppliers(st.(supplier_names := st.supplier_names[S[0].sk := S[0].name],
                           supplier_nations := st.supplier_nations[S[0].sk := S[0].nk]), S[1..])
  }

  function ScanNations(st: TState, N: seq<Nat>): TState
    decreases |N|
  {
    if |N| == 0 then st
    else
      var nm := N[0].name;
      var ks := if nm in st.nation_name_to_keys then st.nation_name_to_keys[nm] else [];
      ScanNations(st.(nation_name_to_keys := st.nation_name_to_keys[nm := ks + [N[0].nk]]), N[1..])
  }

  function ScanOrders(st: TState, O: seq<Ord>): TState
    decreases |O|
  {
    if |O| == 0 then st
    else
      var ok := O[0].ok;
      var base := if ok in st.order_summaries then st.order_summaries[ok] else OrderSummary(0 as char, map[]);
      ScanOrders(st.(order_summaries := st.order_summaries[ok := base.(orderstatus := O[0].st)]), O[1..])
  }

  function AddLineitem(sup: map<int, SOI>, l: Li): map<int, SOI>
  {
    var info := if l.sk in sup then sup[l.sk] else SOI(0, 0);
    sup[l.sk := SOI(info.total + 1, if l.rd > l.cd then info.late + 1 else info.late)]
  }

  function ScanLineitems(st: TState, L: seq<Li>): TState
    decreases |L|
  {
    if |L| == 0 then st
    else
      var ok := L[0].ok;
      if ok !in st.order_summaries || st.order_summaries[ok].orderstatus != 'F' then ScanLineitems(st, L[1..])
      else
        var s := st.order_summaries[ok];
        ScanLineitems(st.(order_summaries := st.order_summaries[ok := s.(suppliers := AddLineitem(s.suppliers, L[0]))]), L[1..])
  }

  function AddNumwait(nw: map<int, map<int, int>>, nk: int, sk: int, count: int): map<int, map<int, int>>
  {
    var inner := if nk in nw then nw[nk] else map[];
    nw[nk := inner[sk := (if sk in inner then inner[sk] else 0) + count]]
  }

  // for (sk, count) in contribs  (enumeration cen)
  function AddContribs(st: TState, contribs: map<int, int>, cen: seq<int>): TState
    requires forall x :: x in cen ==> x in contribs
    ensures AddContribs(st, contribs, cen).order_summaries == st.order_summaries
    ensures AddContribs(st, contribs, cen).supplier_nations == st.supplier_nations
    ensures AddContribs(st, contribs, cen).supplier_names == st.supplier_names
    ensures AddContribs(st, contribs, cen).nation_name_to_keys == st.nation_name_to_keys
    ensures AddContribs(st, contribs, cen).order_overlay == st.order_overlay
    decreases |cen|
  {
    if |cen| == 0 then st
    else
      var sk := cen[0];
      var count := contribs[sk];
      if count == 0 || sk !in st.supplier_nations then AddContribs(st, contribs, cen[1..])
      else AddContribs(st.(nation_numwait := AddNumwait(st.nation_numwait, st.supplier_nations[sk], sk, count)), contribs, cen[1..])
  }

  // for (orderkey, summary) in *order_summaries  (enumeration oen); the inner
  // enumerations (supplier maps, contribution maps) are given per order key.
  function BuildContribs(st: TState, oen: seq<int>,
                         sen: map<int, seq<int>>, cen: map<int, seq<int>>): TState
    requires forall x :: x in oen ==> x in st.order_summaries && x in sen && x in cen
    requires forall x :: x in oen ==> forall y :: y in sen[x] ==> y in st.order_summaries[x].suppliers
    requires forall x :: x in oen ==> forall y :: y in cen[x] ==>
               y in ComputeOrderContributions(st.order_summaries[x], sen[x])
    ensures BuildContribs(st, oen, sen, cen).order_summaries == st.order_summaries
    ensures BuildContribs(st, oen, sen, cen).supplier_names == st.supplier_names
    ensures BuildContribs(st, oen, sen, cen).supplier_nations == st.supplier_nations
    ensures BuildContribs(st, oen, sen, cen).nation_name_to_keys == st.nation_name_to_keys
    ensures BuildContribs(st, oen, sen, cen).order_overlay == st.order_overlay
    decreases |oen|
  {
    if |oen| == 0 then st
    else
      var s := st.order_summaries[oen[0]];
      if s.orderstatus != 'F' then BuildContribs(st, oen[1..], sen, cen)
      else
        var c := ComputeOrderContributions(s, sen[oen[0]]);
        var st2 := AddContribs(st, c, cen[oen[0]]);
        BuildContribs(st2, oen[1..], sen, cen)
  }

  // Erase summaries that are not F, have <= 1 supplier, or contribute nothing.
  function KeepSummary(s: OrderSummary, en: seq<int>): bool
    requires forall x :: x in en ==> x in s.suppliers
  { !(s.orderstatus != 'F' || |s.suppliers| <= 1 || |ComputeOrderContributions(s, en)| == 0) }

  function PruneSummaries(m: map<int, OrderSummary>, sen: map<int, seq<int>>): map<int, OrderSummary>
    requires forall x :: x in m ==> x in sen && forall y :: y in sen[x] ==> y in m[x].suppliers
  { map k | k in m && KeepSummary(m[k], sen[k]) :: m[k] }

  // ===================== TxnBuffer (:484) / Deltas =====================

  datatype LineitemDelta = LineitemDelta(orderkey: int, suppkey: int, commitdate: int, receiptdate: int, sign: int)
  datatype OrderDelta = OrderDelta(orderkey: int, old_status: char, new_status: char)

  datatype TxnBuffer = TxnBuffer(pending: bool, requires_rebuild: bool,
                                 lineitem_deltas: seq<LineitemDelta>, order_deltas: seq<OrderDelta>,
                                 supplier_delta_count: nat, nation_delta_count: nat)

  function EmptyBuffer(): TxnBuffer { TxnBuffer(false, false, [], [], 0, 0) }

  function MarkRebuild(b: TxnBuffer): TxnBuffer
  { TxnBuffer(true, true, [], [], 0, 0) }

  function AddLineitemDelta(b: TxnBuffer, d: LineitemDelta): TxnBuffer
  { b.(pending := true, lineitem_deltas := b.lineitem_deltas + [d]) }

  function AddOrderDelta(b: TxnBuffer, d: OrderDelta): TxnBuffer
  { b.(pending := true, order_deltas := b.order_deltas + [d]) }

  // ===================== AccumulateMutation (:595) =====================
  //
  // Decoded mutation rows: a typed row or a parse failure (C++ Read* false).

  datatype OptLi = LiBad | LiOk(l: Li)
  datatype OptOrd = OrdBad | OrdOk(o: Ord)
  datatype MutKind = Insert | Delete | Update
  datatype TableKind = TSupplier | TOrders | TLineitem | TNation | TOther

  datatype Mutation =
    | LiMut(kind: MutKind, li_old: seq<OptLi>, li_new: seq<OptLi>)
    | OrdMut(kind: MutKind, ord_old: seq<OptOrd>, ord_new: seq<OptOrd>)
    | SupMut
    | NatMut
    | OtherMut

  function AddLiRows(b: TxnBuffer, rows: seq<OptLi>, sign: int): TxnBuffer
    decreases |rows|
  {
    if |rows| == 0 then b
    else
      var b2 := match rows[0]
        case LiBad => MarkRebuild(b)
        case LiOk(l) => AddLineitemDelta(b, LineitemDelta(l.ok, l.sk, l.cd, l.rd, sign));
      AddLiRows(b2, rows[1..], sign)
  }

  // Update: for each i: add(old[i], -1); add(new[i], +1)   (continues after a
  // parse failure exactly like the C++ loop: MarkRebuild clears, later rows re-add).
  function AddLiPairs(b: TxnBuffer, olds: seq<OptLi>, news: seq<OptLi>): TxnBuffer
    requires |olds| == |news|
    decreases |olds|
  {
    if |olds| == 0 then b
    else AddLiPairs(AddLiRows(AddLiRows(b, [olds[0]], -1), [news[0]], 1), olds[1..], news[1..])
  }

  function AddOrdRows(b: TxnBuffer, rows: seq<OptOrd>, sign: int): TxnBuffer
    decreases |rows|
  {
    if |rows| == 0 then b
    else
      var b2 := match rows[0]
        case OrdBad => MarkRebuild(b)
        case OrdOk(o) =>
          AddOrderDelta(b, OrderDelta(o.ok, if sign > 0 then 0 as char else o.st, if sign > 0 then o.st else 0 as char));
      AddOrdRows(b2, rows[1..], sign)
  }

  // Order Update: a parse failure or key change marks rebuild and RETURNS.
  function AddOrdPairs(b: TxnBuffer, olds: seq<OptOrd>, news: seq<OptOrd>): TxnBuffer
    requires |olds| == |news|
    decreases |olds|
  {
    if |olds| == 0 then b
    else
      match (olds[0], news[0])
      case (OrdOk(o1), OrdOk(o2)) =>
        if o1.ok != o2.ok then MarkRebuild(b)
        else AddOrdPairs(AddOrderDelta(b, OrderDelta(o1.ok, o1.st, o2.st)), olds[1..], news[1..])
      case _ => MarkRebuild(b)
  }

  function AccumulateMutation(m: Mutation, b: TxnBuffer): TxnBuffer
  {
    match m
    case SupMut => MarkRebuild(b)
    case NatMut => MarkRebuild(b)
    case OtherMut => b
    case OrdMut(kind, olds, news) =>
      (match kind
       case Insert => AddOrdRows(b, news, 1)
       case Delete => AddOrdRows(b, olds, -1)
       case Update => if |olds| != |news| then MarkRebuild(b) else AddOrdPairs(b, olds, news))
    case LiMut(kind, olds, news) =>
      (match kind
       case Insert => AddLiRows(b, news, 1)
       case Delete => AddLiRows(b, olds, -1)
       case Update => if |olds| != |news| then MarkRebuild(b) else AddLiPairs(b, olds, news))
  }

  // ===================== Maintenance outcome =====================

  datatype Action = NoOp | ApplyDelta | RebuildPartition | RebuildAll | InvalidateAndFallback

  // invalidate_all: outcome.invalidations == All (pending rebuild) ; otherwise
  // the coordinator early-invalidates exactly the partitions in `updates`.
  datatype Outcome = Outcome(action: Action, invalidate_all: bool, updates: seq<(int, TImage)>)

  function RebuildAllOutcome(): Outcome { Outcome(RebuildAll, true, []) }

  // ===================== ApplyContributionDeltas (:240) =====================

  function ApplyOneDelta(st: TState, sk: int, delta: int): TState
  {
    if delta == 0 || sk !in st.supplier_nations then st
    else
      var nk := st.supplier_nations[sk];
      var nm := if nk in st.nation_numwait then st.nation_numwait[nk] else map[];
      var v := (if sk in nm then nm[sk] else 0) + delta;
      var nm2 := if v == 0 then nm - {sk} else nm[sk := v];
      st.(nation_numwait := if |nm2| == 0 then st.nation_numwait - {nk} else st.nation_numwait[nk := nm2])
  }

  function ApplyDeltasLoop(st: TState, oldc: map<int, int>, newc: map<int, int>, ks: seq<int>): TState
    decreases |ks|
  {
    if |ks| == 0 then st
    else
      var sk := ks[0];
      var o := if sk in oldc then oldc[sk] else 0;
      var n := if sk in newc then newc[sk] else 0;
      ApplyDeltasLoop(ApplyOneDelta(st, sk, n - o), oldc, newc, ks[1..])
  }

  function ApplyContributionDeltas(st: TState, oldc: map<int, int>, newc: map<int, int>): TState
  {
    // std::set<int64_t> affected_suppkeys: ascending order.
    ApplyDeltasLoop(st, oldc, newc, IntKeys(oldc.Keys + newc.Keys))
  }

  // ===================== StageMaintenance (:689) =====================

  // Per-order lineitem delta application (summary.suppliers[...] updates).
  function ApplyLiDeltas(sup: map<int, SOI>, ds: seq<LineitemDelta>): map<int, SOI>
    decreases |ds|
  {
    if |ds| == 0 then sup
    else
      var d := ds[0];
      var info := if d.suppkey in sup then sup[d.suppkey] else SOI(0, 0);
      var info2 := SOI(info.total + d.sign, if d.receiptdate > d.commitdate then info.late + d.sign else info.late);
      var sup2 := if info2.total <= 0 && info2.late <= 0 then sup - {d.suppkey} else sup[d.suppkey := info2];
      ApplyLiDeltas(sup2, ds[1..])
  }

  function DeltasFor(ds: seq<LineitemDelta>, ok: int): seq<LineitemDelta>
  {
    if |ds| == 0 then []
    else (if ds[0].orderkey == ok then [ds[0]] else []) + DeltasFor(ds[1..], ok)
  }

  // order_status_changes[orderkey] = last (old, new) with old != new.
  datatype OptChange = NoChange | Change(old_status: char, new_status: char)

  function StatusChange(ods: seq<OrderDelta>, ok: int): OptChange
  {
    if |ods| == 0 then NoChange
    else
      var rest := StatusChange(ods[1..], ok);
      if rest.Change? then rest
      else if ods[0].orderkey == ok && ods[0].old_status != ods[0].new_status then
        Change(ods[0].old_status, ods[0].new_status)
      else NoChange
  }

  // first_old_status (unordered_map::emplace keeps the FIRST insertion).
  datatype OptChar = NoChar | SomeChar(c: char)
  function FirstOldStatus(ods: seq<OrderDelta>, ok: int): OptChar
  {
    if |ods| == 0 then NoChar
    else if ods[0].orderkey == ok then SomeChar(ods[0].old_status)
    else FirstOldStatus(ods[1..], ok)
  }

  function AffectedOrderKeys(b: TxnBuffer): set<int>
  {
    (set d | d in b.order_deltas :: d.orderkey) + (set d | d in b.lineitem_deltas :: d.orderkey)
  }

  function NationsOf(st: TState, sks: set<int>): set<int>
  { set sk | sk in sks && sk in st.supplier_nations :: st.supplier_nations[sk] }

  // needed_nationkeys computed before the nation partitions are read.
  function NeededBeforeLoad(st: TState, b: TxnBuffer): set<int>
  {
    var ks := AffectedOrderKeys(b);
    var summarySupps := set ok, sk | ok in ks && FindOrder(st, ok).SomeSummary? &&
                                     sk in FindOrder(st, ok).s.suppliers :: sk;
    var deltaSupps := set d | d in b.lineitem_deltas :: d.suppkey;
    NationsOf(st, summarySupps + deltaSupps)
  }

  // Load: for each needed nk whose NationPartition image is present.
  // Returns None when an image is mistyped / has the wrong nationkey (RebuildAll).
  datatype OptState = NoState | SomeState(st: TState)

  function LoadNations(st: TState, snap: map<int, TImage>, nks: seq<int>): OptState
    decreases |nks|
  {
    if |nks| == 0 then SomeState(st)
    else
      var nk := nks[0];
      var pid := NationPartition(nk);
      if pid !in snap then LoadNations(st, snap, nks[1..])
      else
        var img := snap[pid];
        if img.kind != NationK || img.nationkey != nk then NoState
        else
          var st2 := if nk in img.state.nation_numwait
                     then st.(nation_numwait := st.nation_numwait[nk := img.state.nation_numwait[nk]])
                     else st;
          LoadNations(st2, snap, nks[1..])
  }

  // One affected order.  Returns NoState for the fail-closed RebuildAll arm.
  // `sen0`/`sen1`: enumerations of the old / new supplier maps (for
  // ComputeOrderContributions).
  function ProcessOrder(st: TState, b: TxnBuffer, ok: int,
                        sen0: seq<int>, sen1: seq<int>): OptState
    requires var s0 := MutableOrderGet(st, ok); forall x :: x in sen0 ==> x in s0.suppliers
    requires var s0 := MutableOrderGet(st, ok);
             var chg := StatusChange(b.order_deltas, ok);
             var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
             forall x :: x in sen1 ==> x in ApplyLiDeltas(s1.suppliers, DeltasFor(b.lineitem_deltas, ok))
  {
    var chg := StatusChange(b.order_deltas, ok);
    var first := FirstOldStatus(b.order_deltas, ok);
    if FindOrder(st, ok).NoSummary? && chg.Change? && chg.new_status == 0 as char then
      SomeState(st)  // deleted by this batch: skip
    else if FindOrder(st, ok).NoSummary? &&
            (first.NoChar? || first.c != 0 as char || chg.NoChange?) then
      NoState        // untracked pre-existing order: RebuildAll
    else
      // (created summary for a missing order has orderstatus = new status,
      //  then status_it applies the same new status again)
      var s0 := MutableOrderGet(st, ok);
      var old_contribs := ComputeOrderContributions(s0, sen0);
      var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
      var s2 := s1.(suppliers := ApplyLiDeltas(s1.suppliers, DeltasFor(b.lineitem_deltas, ok)));
      var new_contribs := ComputeOrderContributions(s2, sen1);
      var st2 := PutOverlay(st, ok, s2);
      SomeState(ApplyContributionDeltas(st2, old_contribs, new_contribs))
  }

  // Needed nations after the loop (published set): before-load set plus the
  // nations of delta suppliers and of old/new contributions.
  // nk_to_name[nk] = the first name (in nation_name_to_keys iteration order,
  // enumeration `names`) whose key vector contains nk.
  datatype OptName = NoName | SomeName(nm: string)

  function FirstNameWith(m: map<string, seq<int>>, names: seq<string>, nk: int): OptName
    requires forall x :: x in names ==> x in m
    ensures FirstNameWith(m, names, nk).SomeName? ==>
              FirstNameWith(m, names, nk).nm in names && nk in m[FirstNameWith(m, names, nk).nm]
    decreases |names|
  {
    if |names| == 0 then NoName
    else if nk in m[names[0]] then SomeName(names[0])
    else FirstNameWith(m, names[1..], nk)
  }

  function NameImages(st: TState, nk: int, en: seq<int>, names: seq<string>): seq<(int, TImage)>
    requires nk in st.nation_numwait ==> forall x :: x in en ==> x in st.nation_numwait[nk]
    requires forall x :: x in names ==> x in st.nation_name_to_keys
  {
    match FirstNameWith(st.nation_name_to_keys, names, nk)
    case NoName => []
    case SomeName(nm) =>
      if |st.nation_name_to_keys[nm]| == 1
      then [(NationNamePartition(nm), MakeImage(NationK, nk, st, nm, en))]
      else []
  }

  function PublishImages(st: TState, published: seq<int>, nen: map<int, seq<int>>, names: seq<string>): seq<(int, TImage)>
    requires forall nk :: nk in published ==> nk in nen
    requires forall nk :: nk in published && nk in st.nation_numwait ==>
               forall x :: x in nen[nk] ==> x in st.nation_numwait[nk]
    requires forall x :: x in names ==> x in st.nation_name_to_keys
    decreases |published|
  {
    if |published| == 0 then []
    else
      var nk := published[0];
      [(NationPartition(nk), MakeImage(NationK, nk, st, "", nen[nk]))]
        + NameImages(st, nk, nen[nk], names)
        + PublishImages(st, published[1..], nen, names)
  }

  // ===================== Top level: BuildState (:1175) =====================

  function ScanAll(T: TDB): TState
  { ScanLineitems(ScanOrders(ScanNations(ScanSuppliers(EmptyState(), T.sup), T.nats), T.ords), T.li) }

  function SupEnums(m: map<int, OrderSummary>, ei: set<int> -> seq<int>): map<int, seq<int>>
  { map ok | ok in m :: ei(m[ok].suppliers.Keys) }

  function ContribEnums(m: map<int, OrderSummary>, sen: map<int, seq<int>>, ei: set<int> -> seq<int>): map<int, seq<int>>
    requires forall ok :: ok in m ==> ok in sen && forall y :: y in sen[ok] ==> y in m[ok].suppliers
  { map ok | ok in m :: ei(ComputeOrderContributions(m[ok], sen[ok]).Keys) }

  function BuildState(T: TDB, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): TState
    requires EnumOracle(ei, es)
  {
    var st4 := ScanAll(T);
    var m := st4.order_summaries;
    var sen := SupEnums(m, ei);
    var cen := ContribEnums(m, sen, ei);
    var st5 := BuildContribs(st4, ei(m.Keys), sen, cen);
    st5.(order_summaries := PruneSummaries(st5.order_summaries, sen))
  }

  // ===================== Build (:545): published images =====================

  function NationEnums(st: TState, nks: seq<int>, ei: set<int> -> seq<int>): map<int, seq<int>>
  { map nk | nk in nks :: if nk in st.nation_numwait then ei(st.nation_numwait[nk].Keys) else [] }

  function NationImgs(st: TState, nks: seq<int>, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): seq<(int, TImage)>
    requires EnumOracle(ei, es)
    decreases |nks|
  {
    if |nks| == 0 then []
    else
      var nk := nks[0];
      var en := if nk in st.nation_numwait then ei(st.nation_numwait[nk].Keys) else [];
      [(NationPartition(nk), MakeImage(NationK, nk, st, "", en))] + NationImgs(st, nks[1..], ei, es)
  }

  function NameIdxImgs(st: TState, names: seq<string>, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): seq<(int, TImage)>
    requires EnumOracle(ei, es)
    requires forall x :: x in names ==> x in st.nation_name_to_keys
    decreases |names|
  {
    if |names| == 0 then []
    else
      var nm := names[0];
      var ks := st.nation_name_to_keys[nm];
      var rest := NameIdxImgs(st, names[1..], ei, es);
      if |ks| != 1 then rest
      else
        var nk := ks[0];
        var en := if nk in st.nation_numwait then ei(st.nation_numwait[nk].Keys) else [];
        [(NationNamePartition(nm), MakeImage(NationK, nk, st, nm, en))] + rest
  }

  function BuildImages(T: TDB, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): seq<(int, TImage)>
    requires EnumOracle(ei, es)
  {
    var st := BuildState(T, ei, es);
    [(kSupplierMetadata, MakeImage(SupplierMetadata, 0, st, "", [])),
     (kOrderSummaries, MakeImage(OrderSummaries, 0, st, "", [])),
     (kNationNameIndex, MakeImage(NationNameIndex, 0, st, "", []))]
    + NationImgs(st, ei(st.nation_numwait.Keys), ei, es)
    + NameIdxImgs(st, es(st.nation_name_to_keys.Keys), ei, es)
  }

  // ===================== StageMaintenance (:689) =====================

  datatype OptStateSet = NoStateSet | SomeStateSet(st: TState, contrib_supps: set<int>)

  function ProcessOrders(st: TState, b: TxnBuffer, ks: seq<int>, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): OptStateSet
    requires EnumOracle(ei, es)
    decreases |ks|
  {
    if |ks| == 0 then SomeStateSet(st, {})
    else
      var ok := ks[0];
      var s0 := MutableOrderGet(st, ok);
      var chg := StatusChange(b.order_deltas, ok);
      var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
      var sup2 := ApplyLiDeltas(s1.suppliers, DeltasFor(b.lineitem_deltas, ok));
      var sen0 := ei(s0.suppliers.Keys);
      var sen1 := ei(sup2.Keys);
      var r := ProcessOrder(st, b, ok, sen0, sen1);
      if r.NoState? then NoStateSet
      else
        var supps := (ComputeOrderContributions(s0, sen0).Keys) +
                     (ComputeOrderContributions(s1.(suppliers := sup2), sen1).Keys);
        var rest := ProcessOrders(r.st, b, ks[1..], ei, es);
        if rest.NoStateSet? then NoStateSet
        else SomeStateSet(rest.st, supps + rest.contrib_supps)
  }

  function LoadSide(side: map<int, TImage>): OptState
  {
    if (kSupplierMetadata in side && side[kSupplierMetadata].kind != SupplierMetadata) ||
       (kOrderSummaries in side && side[kOrderSummaries].kind != OrderSummaries) ||
       (kNationNameIndex in side && side[kNationNameIndex].kind != NationNameIndex) ||
       kSupplierMetadata !in side || kOrderSummaries !in side || kNationNameIndex !in side
    then NoState
    else
      var s0 := side[kSupplierMetadata].state;
      var s1 := side[kOrderSummaries].state;
      var s2 := side[kNationNameIndex].state;
      SomeState(TState(s0.supplier_names, s0.supplier_nations, s2.nation_name_to_keys,
                       s1.order_summaries, s1.order_overlay, map[]))
  }

  function StageMaintenance(b: TxnBuffer, dec1: Action, dec2: Action,
                            side: map<int, TImage>, natsnap: map<int, TImage>,
                            ei: set<int> -> seq<int>, es: set<string> -> seq<string>): Outcome
    requires EnumOracle(ei, es)
  {
    if b.requires_rebuild then
      Outcome(if dec1 == ApplyDelta then RebuildAll else dec1, true, [])
    else if |b.lineitem_deltas| == 0 && |b.order_deltas| == 0 &&
            b.supplier_delta_count == 0 && b.nation_delta_count == 0 then
      Outcome(NoOp, false, [])
    else if dec2 == RebuildPartition then RebuildAllOutcome()
    else if dec2 != ApplyDelta then Outcome(dec2, true, [])
    else
      var ls := LoadSide(side);
      if ls.NoState? then RebuildAllOutcome()
      else
        var st := ls.st;
        var needed0 := NeededBeforeLoad(st, b);
        var ln := LoadNations(st, natsnap, IntKeys(needed0));
        if ln.NoState? then RebuildAllOutcome()
        else
          var po := ProcessOrders(ln.st, b, IntKeys(AffectedOrderKeys(b)), ei, es);
          if po.NoStateSet? then RebuildAllOutcome()
          else
            var st2 := po.st;
            var deltaSupps := set d | d in b.lineitem_deltas :: d.suppkey;
            var published := needed0 + NationsOf(st2, deltaSupps) + NationsOf(st2, po.contrib_supps);
            var pub := IntKeys(published);
            var nen := NationEnums(st2, pub, ei);
            Outcome(ApplyDelta, false,
              [(kSupplierMetadata, MakeImage(SupplierMetadata, 0, st2, "", [])),
               (kOrderSummaries, MakeImage(OrderSummaries, 0, st2, "", [])),
               (kNationNameIndex, MakeImage(NationNameIndex, 0, st2, "", []))]
              + PublishImages(st2, pub, nen, es(st2.nation_name_to_keys.Keys)))
  }

  // ===================== Execute (:1025), LoadStateForNations, EmitResult =====================

  function EmitRows(nw: map<int, map<int, int>>, names: map<int, string>, nks: seq<int>, ei: set<int> -> seq<int>): seq<OutRow>
    requires forall s :: IsEnum(ei(s), s)
    decreases |nks|
  {
    if |nks| == 0 then []
    else
      var nk := nks[0];
      (if nk in nw then (assert IsEnum(ei(nw[nk].Keys), nw[nk].Keys);
                         NationRowsLoop(nw[nk], names, ei(nw[nk].Keys))) else [])
        + EmitRows(nw, names, nks[1..], ei)
  }

  // EmitResult(state): all nations of state.nation_numwait, merged and sorted.
  function EmitResult(st: TState, ei: set<int> -> seq<int>): seq<OutRow>
    requires forall s :: IsEnum(ei(s), s)
  { MergeByNameAndSort(EmitRows(st.nation_numwait, st.supplier_names, ei(st.nation_numwait.Keys), ei)) }

  // LoadStateForNations: None == `return false` (caller falls back).
  function LoadStateForNations(snap: map<int, TImage>, nks: seq<int>): OptState
  {
    if kSupplierMetadata !in snap || snap[kSupplierMetadata].kind != SupplierMetadata then NoState
    else
      var names := snap[kSupplierMetadata].state.supplier_names;
      var ln := LoadNations(EmptyState().(supplier_names := names), snap, nks);
      ln
  }

  datatype Arg = ArgNull | ArgText(s: string)
  datatype ExecResult = Emitted(rows: seq<OutRow>) | Fallback

  function Execute(arg: Arg, dirty: bool, valid: bool, snap: map<int, TImage>, ei: set<int> -> seq<int>): ExecResult
    requires forall s :: IsEnum(ei(s), s)
  {
    if arg.ArgNull? || dirty || !valid then Fallback
    else
      var nn := arg.s;
      var fpid := NationNamePartition(nn);
      if fpid in snap && snap[fpid].kind == NationK && snap[fpid].nation_name == nn &&
         |snap[fpid].output_rows| > 0
      then Emitted(snap[fpid].output_rows)
      else if kNationNameIndex !in snap || snap[kNationNameIndex].kind != NationNameIndex then Fallback
      else
        var idx := snap[kNationNameIndex].state.nation_name_to_keys;
        if nn !in idx || |idx[nn]| == 0 then Emitted([])
        else if |idx[nn]| != 1 then
          var ls := LoadStateForNations(snap, idx[nn]);
          if ls.NoState? then Fallback else Emitted(EmitResult(ls.st, ei))
        else
          var nk := idx[nn][0];
          var pid := NationPartition(nk);
          if pid in snap && snap[pid].kind == NationK && snap[pid].nationkey == nk
          then Emitted(snap[pid].output_rows)
          else Emitted([])
  }

  // ExecuteFallback (:1150): full BuildState at the caller's snapshot, restricted
  // to the parameter's nations.  ArgNull looks up the empty name.
  function ExecuteFallback(T: TDB, arg: Arg, ei: set<int> -> seq<int>, es: set<string> -> seq<string>): seq<OutRow>
    requires EnumOracle(ei, es)
  {
    var nn := if arg.ArgText? then arg.s else "";
    var full := BuildState(T, ei, es);
    var ks := if nn in full.nation_name_to_keys then full.nation_name_to_keys[nn] else [];
    var nw := map nk | nk in ks && nk in full.nation_numwait :: full.nation_numwait[nk];
    EmitResult(EmptyState().(nation_numwait := nw, supplier_names := full.supplier_names), ei)
  }
}
