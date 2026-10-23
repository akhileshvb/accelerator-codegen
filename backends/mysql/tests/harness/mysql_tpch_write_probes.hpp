#pragma once

#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

namespace accel::mysql::tpch_harness {

using accel::tpch_params::WriteProbe;

inline WriteProbe ResolveWriteProbe(const Tools &tools, int query_num, int candidate_offset) {
  return accel::tpch_params::ResolveWriteProbe(
      query_num, candidate_offset, [&](const std::string &sql) { return QuerySQL(tools, sql); });
}

inline bool ProbeChangesBaseFingerprint(const Tools &tools,
                                        const std::string &base_sql,
                                        const std::string &update_sql,
                                        const std::string &restore_sql) {
  auto before = QuerySQL(tools, base_sql);
  Require(before.first, "probe before SQL failed");
  Require(RunSQL(tools, update_sql), "probe update failed: " + update_sql);
  auto after = QuerySQL(tools, base_sql);
  Require(after.first, "probe after SQL failed");
  RunSQL(tools, restore_sql);
  return TrimLines(before.second) != TrimLines(after.second);
}

inline WriteProbe PickInvalidatingWriteProbe(const Tools &tools, int query_num,
                                            const std::string &base_sql,
                                            int max_candidates = 64) {
  WriteProbe probe;
  for (int cand = 0; cand < max_candidates; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand);
    } catch (const std::exception &ex) {
      if (cand == 0) {
        throw std::runtime_error("write probe Q" + std::to_string(query_num) +
                                 " failed: " + ex.what());
      }
      break;
    }
    if (ProbeChangesBaseFingerprint(tools, base_sql, probe.update_sql,
                                    probe.restore_sql)) {
      return probe;
    }
  }
  throw std::runtime_error(
      "invalidating probe did not change base SQL fingerprint for Q" +
      std::to_string(query_num) +
      " — write must impact this query's observable result");
}

// Setup check for write-mix benches: the probe must change native SQL *and*
// the aux Execute answer, and must not be a maintenance NoOp.
inline WriteProbe RequireInvalidatingAuxWrite(const Tools &tools, int query_num,
                                              const std::string &base_sql,
                                              const std::string &aux_sql,
                                              int max_candidates = 64) {
  WriteProbe probe;
  for (int cand = 0; cand < max_candidates; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand);
    } catch (const std::exception &ex) {
      if (cand == 0) {
        throw std::runtime_error("write probe Q" + std::to_string(query_num) +
                                 " failed: " + ex.what());
      }
      break;
    }
    auto before_sql = QuerySQL(tools, base_sql);
    Require(before_sql.first, "probe before SQL failed");
    auto before_aux = QuerySQL(tools, aux_sql);
    Require(before_aux.first, "probe before aux failed: " + before_aux.second);
    Require(RunSQL(tools, probe.update_sql),
            "probe update failed: " + probe.update_sql);
    auto after_sql = QuerySQL(tools, base_sql);
    Require(after_sql.first, "probe after SQL failed");
    auto after_aux = QuerySQL(tools, aux_sql);
    Require(after_aux.first, "probe after aux failed: " + after_aux.second);
    auto action_q =
        QuerySQL(tools, "SELECT accel_get_last_maintenance_action()");
    const std::string action =
        action_q.first ? TrimLines(action_q.second) : "error";
    RunSQL(tools, probe.restore_sql);
    const bool sql_changed =
        TrimLines(before_sql.second) != TrimLines(after_sql.second);
    const bool aux_changed =
        TrimLines(before_aux.second) != TrimLines(after_aux.second);
    if (!sql_changed)
      continue;
    Require(aux_changed,
            "Q" + std::to_string(query_num) +
                " write changed native SQL but aux Execute was unchanged "
                "(action=" +
                action + ")");
    Require(action != "NoOp" && action != "None" && action != "error",
            "Q" + std::to_string(query_num) +
                " write changed results but maintenance action was " + action);
    return probe;
  }
  throw std::runtime_error(
      "no write probe changed native SQL and aux state for Q" +
      std::to_string(query_num));
}

} // namespace accel::mysql::tpch_harness
