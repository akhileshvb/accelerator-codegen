# Backend API contract (`backends/<engine>/api/`)

Normative and engine-agnostic. Every backend, whatever the engine and however it is
packaged (embedded library, server plugin, server extension), exposes the same
**roles** through `backends/<engine>/api/`, and nothing else of the backend is
visible outside `backends/<engine>/`. Accelerators, the substrate, `eval/`, scripts
and templates include only `backends/<engine>/api/...` headers of a backend
(`scripts/check_layer_includes.py` fails otherwise). `engine/`, `state/`, `tests/`
and any other directory are the backend's own.

A backend run writes `api/` and `api/README.md` (the manifest, below) in the layer
`api` (stage `impl_api`), after the slot layers it builds on. The engine profile gives
the facts (packaging, entry points, thread knobs, hazards); this file gives the
interface. Operational verbs (provision, build, load data, run SQL) are the separate
`host_contract.md`, which also fixes the manifest's table format.

## 1. The neutral surface: `api/accelerator_api.hpp`

Engine-neutral code (the conformance toy accelerator, harnesses that must run on any
engine) uses only the names in this header, namespace `accelerator_codegen::api`. All
other API names are engine-specific and are found through the manifest.

```cpp
// Scan access: run `sql` where a module's build or fallback scan runs (the build
// snapshot, or the reader's snapshot) and visit the rows. `types` gives the type of
// each result column; cells are converted to core::AuxValue exactly (NULL as
// AuxValue::Null). Supported: Int64, Double, String, Boolean, Date, ScaledDecimal128
// (the decimal as the engine prints it, at its own scale). An unsupported type
// throws; it never converts lossily.
class RowScanner {
public:
  virtual ~RowScanner() = default;
  virtual void Scan(const std::string &sql, const std::vector<core::AuxType> &types,
                    const std::function<void(const core::AuxRow &)> &on_row) const = 0;
};

struct QueryColumn { std::string name; core::AuxType type; };

// Query registration: one SQL-visible entry point answered by an accelerator.
struct QuerySpec {
  std::string function_name;                 // the SQL-visible name
  std::vector<core::AuxType> argument_types; // SQL arguments, in order
  std::vector<QueryColumn> result_columns;
  std::string fallback_sql;                  // the query for any arguments; $n = n-th argument
  std::vector<std::pair<std::string, core::TableId>> tables; // relation -> module table id
  core::AuxCostModelLocation cost_model;
  // The module, built once, over the engine's scan access.
  std::function<std::shared_ptr<core::AuxQueryModule>(const RowScanner &)> make_module;
};
void RegisterQuery(QuerySpec spec);               // callable from static initializers
struct QueryRegistrar { explicit QueryRegistrar(QuerySpec spec) { RegisterQuery(std::move(spec)); } };

// Where the engine needs a per-function exported symbol (server plugins/extensions
// resolve SQL functions by symbol name), the entry point of `function_name`;
// expands to nothing where it does not.
#define ACCEL_API_QUERY_ENTRY(function_name)
```

After registration (and the engine's load step) the SQL-visible call of a query is the
engine's own function-call syntax; harnesses on an engine know it. Registration makes
the engine capture the `tables`, build the module's state from `make_module`'s module
through the substrate's runtime, answer reads from state when it can serve them and
from `fallback_sql` otherwise (§3, `sql_fallback`), and report the execute path to
`core::RecordAuxExecute`.

## 2. Roles

Every backend provides each role or declares it `n/a` with a reason in its manifest.
"Fixed names" are required exactly; the rest is named by the backend and listed in the
manifest.

| role id | the backend provides | fixed names |
|---|---|---|
| `registration` | The static registrars through which accelerators plug in without the backend naming any query: the neutral surface of §1, plus the engine's own registries (catalog entry, SQL entry point, executor, table function or UDF shim). | §1 |
| `runtime` | The host/runtime handle accelerators and harnesses use: finding or creating the backend's runtime host for a database/server, registering an accelerator module, running a read, applying a write, forcing a build. | none |
| `scan` | The engine-local typed scan helpers accelerator modules use in their initial scan: rows or batches of a table, a partitioned parallel scan, typed column decoding, inside the build snapshot. | `RowScanner` (§1) |
| `sql_fallback` | Answering a registered query from SQL in the reader's own transaction/snapshot (the query's `fallback_sql` with the call's arguments as parameters or literals), rows converted to `core::AuxRow`. | none |
| `cost_model` | Resolving the maintenance cost-model file for a query's `core::AuxCostModelLocation` by engine and scale (the substrate's `AuxCostModelLookup`; the backend names the engine and the search directories, including mounts it runs in). | none |
| `maintenance` | What the calibration and benchmark code needs from the runtime: the expected queries before the next write, calibration writes, rows in the accelerator's tables, forcing a maintenance action, invalidating all state. | none |
| `introspection` | The test hooks of §3. | §3 |
| `thread_knobs` | The engine-side settings for the thread budget and the read, build and maintenance caps. | §4 |
| `value_semantics` | The engine-exact value helpers of §5. | none |

## 3. Introspection hooks (test and harness)

SQL-visible functions, one set for every engine that has a SQL client (server and
embedded engines alike; an embedded engine registers them on each connection with its
other functions). Names are exact; no engine prefix. All return text unless noted.

| function | returns / effect | required |
|---|---|---|
| `accel_get_execute_counts()` | How accelerator queries were answered, `core::FormatAuxExecuteCounts` (`hit=H rebuilt_on_read=R fallback=F`). | yes |
| `accel_get_last_maintenance_action()` | Name of the last maintenance action taken (`None` if none). | yes |
| `accel_get_last_maintenance_decision()` | The last decision as `core::FormatLastMaintenanceDecision` prints it; SQL NULL if none. | yes |
| `accel_set_force_maintenance_action(text)` | Forces every later maintenance decision to the action; NULL or empty: the policy chooses. | yes |
| `accel_invalidate_all()` | Every accelerator's state is rebuilt before its next read. | where the backend keeps state it can drop; else `n/a` in the manifest |
| `accel_stop_shard_workers()` | Stops the engine's accelerator worker processes (boolean); they restart on demand. | only where the profile's `process_model` has workers; else `n/a` in the manifest |
| `accel_shard_stats()` | Counters of the running workers (`key=value` pairs). | with workers |

Harnesses call these by name and never test which engine they run on.

## 4. Thread knobs

The settings named by the profile's `thread_knobs` carry these meanings and these
base names: `threads` (the budget), `read_threads`, `build_threads`, `maint_threads`
(phase caps within the budget, 0: the budget; maintenance default 1). The engine's
settings namespace decides the spelling: `accel.<knob>` where dots are allowed,
`accel_<knob>` where they are not. They feed the substrate's `thread_budget` as the
engine setting; the environment variables of `thread_budget.hpp` win over them.

## 5. Value semantics (engine-exact)

Where the engine's answer for a value depends on rules that accelerators must
reproduce, the backend ships them in `api/` and accelerators for this engine use them;
nothing engine-exact lives in the shared primitives. Cover, as far as the engine
differs from plain exact arithmetic and byte order:

- numeric/decimal division and AVG: the result scale and the rounding;
- the text of numeric results the engine prints;
- text collation: equality, ordering and case behavior of the default collation, and
  whether the engine can be told to compare bytewise;
- anything else the profile lists as a hazard of value representation.

Engine-neutral helpers (fixed-scale decimal text in and out, overflow-checked 128-bit
accumulation) stay in the shared primitives and the substrate
(`core::ScaledDecimal128ToString`). A backend's headers include only the standard
library, the engine SDK and the substrate.

## 6. Manifest: `api/README.md`

The index format is fixed by `host_contract.md` (`## Contract roles`, a table
`| role | headers | symbols |`, backticked comma-separated entries). The rows are the
nine role ids of §2, each once, in any order:

````
## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/accelerator_api.hpp`, `api/...` | `RegisterQuery`, `QueryRegistrar` |
| value_semantics | | |
````

- Empty headers and symbols declare the role n/a; say why in the prose after the table.
- Every listed header exists; the rows together list **every** `.hpp` under `api/`.
- Every listed symbol occurs in one of its row's headers. Exception, role
  `introspection`: the SQL function names of §3 may occur in any file under `api/`
  (the source that defines them).
- `pipeline/gates/check_api_manifest.py <repo> <engine>` checks all of this.

## 7. Conformance

The gate `engine_api_conformance` (layer `api`): the manifest matches `api/`; nothing
outside the backend includes a non-`api/` header of it; the neutral toy accelerator
(`pipeline/templates/engine_backend/api_conformance/`, written once against §1 only)
builds against this backend's `api/` and answers a trivial parameterised query
exactly as its SQL does, before and after a write, with `accel_get_execute_counts()`
showing the accelerator served it. The backend's harness
`backends/<engine>/tests/test_<engine>_api.cpp` (target `test_<engine>_api`) drives
that; the gate builds and runs it through `host.sh build` and `host.sh run`.
