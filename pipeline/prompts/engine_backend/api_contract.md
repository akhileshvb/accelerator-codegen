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

## 1. Engine idiom, found through the manifest

There is no engine-neutral C++ surface. Accelerators for an engine use that engine's own
idiom (table functions, SQL functions, UDFs, extension or plugin entry points) and find
every name through the manifest (§6), which the accelerator pipeline renders into its
prompts. Roles (§2) fix what must exist; the names are the backend's, except the fixed
names of §3 and §4.

## 2. Roles

Every backend provides each role or declares it `n/a` with a reason in its manifest.
"Fixed names" are required exactly; the rest is named by the backend and listed in the
manifest.

| role id | the backend provides | fixed names |
|---|---|---|
| `registration` | The static registrars through which accelerators plug in without the backend naming any query: the engine's own registries (catalog entry, SQL entry point, executor, table function or UDF shim). | none |
| `runtime` | The host/runtime handle accelerators and harnesses use: finding or creating the backend's runtime host for a database/server, registering an accelerator module, running a read, applying a write, forcing a build. | none |
| `scan` | The engine-local typed scan helpers accelerator modules use in their initial scan: rows or batches of a table, a partitioned parallel scan, typed column decoding, inside the build snapshot. | none |
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

Harnesses call these by name and never test which engine they run on. On a server engine they
are callable from the engine's SQL client (`host.sh sql`; the conformance gate calls them there);
on an embedded engine they exist on the backend's own connections.

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
| registration | `api/register.hpp`, `api/...` | `QueryRegistrar`, `...` |
| value_semantics | | |
````

- Empty headers and symbols declare the role n/a; say why in the prose after the table.
- Every listed header exists; the rows together list **every** `.hpp` under `api/`.
- Every listed symbol occurs in one of its row's headers. Exception, role
  `introspection`: the SQL function names of §3 may occur in any file under `api/`
  (the source that defines them).
- `pipeline/gates/check_api_manifest.py <repo> <engine>` checks all of this.

## 7. Conformance

The gate `engine_api_conformance` (layer `api`): the manifest matches `api/`
(`check_api_manifest.py`); nothing outside the backend includes a non-`api/` header of
it (`scripts/check_layer_includes.py`); and every header under `api/` compiles in a
translation unit of its own with the flags of the code that includes it (target
`api_conformance_<engine>`, generated by `cmake/Backend.cmake`, built through
`host.sh build`), so each header includes what it uses. The first accelerator runs on
the backend exercise `api/` end to end.
