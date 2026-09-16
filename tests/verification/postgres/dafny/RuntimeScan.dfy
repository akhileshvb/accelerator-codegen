// Structural twin of Postgres Q1 Runtime InitialScan + EnsureBuilt + Execute glue
// SOURCE: src/generated/postgres/runtime.cpp
//   ScanTpchQ1Lineitems (~295), TpchQ1Scan, Runtime::EnsureBuilt (~1809), Execute (~1825)
// SPI HeapTuple fetch = oracle; twin is row projection + control flow.

include "../../substrate/dafny/Store.dfy"

module PostgresRuntimeScan {
  import opened SubstrateStore

  // Projected SPI row → TpchQ1LineitemInput (runtime.cpp ScanTpchQ1Lineitems)
  datatype SpiLineitemRow = SpiLineitemRow(
    shipdate_days: int,
    quantity_raw: int,
    extended_price_raw: int,
    discount_raw: int,
    tax_raw: int,
    returnflag: int,
    linestatus: int
  )

  datatype LineitemInput = LineitemInput(
    shipdate_days: int,
    quantity_raw: int,
    quantity_scale: int,
    extended_price_raw: int,
    extended_price_scale: int,
    discount_raw: int,
    discount_scale: int,
    tax_raw: int,
    tax_scale: int,
    returnflag: int,
    linestatus: int
  )

  // Structural: scales fixed to 2 as in ScanTpchQ1Lineitems consumer.
  ghost function ProjectQ1Lineitem(row: SpiLineitemRow): LineitemInput {
    LineitemInput(
      row.shipdate_days,
      row.quantity_raw, 2,
      row.extended_price_raw, 2,
      row.discount_raw, 2,
      row.tax_raw, 2,
      row.returnflag,
      row.linestatus
    )
  }

  ghost function ScanTpchQ1Lineitems(rows: seq<SpiLineitemRow>): seq<LineitemInput>
    decreases |rows|
  {
    if |rows| == 0 then []
    else [ProjectQ1Lineitem(rows[0])] + ScanTpchQ1Lineitems(rows[1..])
  }

  lemma ScanPreservesLength(rows: seq<SpiLineitemRow>)
    ensures |ScanTpchQ1Lineitems(rows)| == |rows|
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      ScanPreservesLength(rows[1..]);
    }
  }

  lemma ProjectScaleIs2(row: SpiLineitemRow)
    ensures ProjectQ1Lineitem(row).quantity_scale == 2
    ensures ProjectQ1Lineitem(row).extended_price_scale == 2
    ensures ProjectQ1Lineitem(row).discount_scale == 2
    ensures ProjectQ1Lineitem(row).tax_scale == 2
  {
  }

  // Runtime::EnsureBuilt (runtime.cpp:~1809)
  datatype RuntimeState = RuntimeState(built: bool, store: Store)

  ghost function EnsureBuilt(r: RuntimeState, install_keys: seq<int>, install_payloads: seq<int>, epoch: nat): RuntimeState
    requires |install_keys| == |install_payloads|
    requires !r.built ==> |install_keys| >= 0
  {
    if r.built then
      r
    else
      RuntimeState(true, InstallAll(r.store, install_keys, install_payloads, epoch))
  }

  ghost function InstallAll(store: Store, keys: seq<int>, payloads: seq<int>, epoch: nat): Store
    requires |keys| == |payloads|
    decreases |keys|
  {
    if |keys| == 0 then store
    else InstallAll(PublishPartition(store, keys[0], payloads[0], epoch),
                    keys[1..], payloads[1..], epoch)
  }

  lemma EnsureBuiltIdempotent(r: RuntimeState, keys: seq<int>, payloads: seq<int>, epoch: nat)
    requires r.built
    requires |keys| == |payloads|
    ensures EnsureBuilt(r, keys, payloads, epoch) == r
  {
  }

  lemma EnsureBuiltInstalls(r: RuntimeState, key: int, payload: int, epoch: nat)
    requires !r.built
    requires payload != 0
    ensures var r2 := EnsureBuilt(r, [key], [payload], epoch);
            r2.built && GetReadablePayload(r2.store, key) == payload
  {
    var s1 := PublishPartition(r.store, key, payload, epoch);
    assert InstallAll(r.store, [key], [payload], epoch) == InstallAll(s1, [], [], epoch);
    assert InstallAll(s1, [], [], epoch) == s1;
    PublishThenReadable(r.store, key, payload, epoch);
  }

  // Execute control flow (runtime.cpp:~1825): EnsureBuilt then module.Execute
  // NeedsRebuild ⇒ clear built and EnsureBuilt again.
  ghost function ExecuteGlue(r: RuntimeState, needs_rebuild: bool,
                             keys: seq<int>, payloads: seq<int>, epoch: nat): RuntimeState
    requires |keys| == |payloads|
  {
    var r1 := EnsureBuilt(r, keys, payloads, epoch);
    if needs_rebuild then
      EnsureBuilt(RuntimeState(false, r1.store), keys, payloads, epoch)
    else
      r1
  }

  lemma ExecuteEnsuresBuilt(r: RuntimeState, keys: seq<int>, payloads: seq<int>, epoch: nat)
    requires |keys| == |payloads|
    ensures ExecuteGlue(r, false, keys, payloads, epoch).built
  {
    if r.built {
    } else if |keys| == 0 {
    } else {
    }
  }

  // Q1 arg decode: PG_GETARG_INT32(0) → QueryArguments Int64 (ExecutePostgresTpchQ1)
  ghost function DecodeQ1Arg(day_offset: int): int {
    day_offset
  }

  lemma DecodeQ1ArgId(x: int)
    ensures DecodeQ1Arg(x) == x
  {
  }
}
