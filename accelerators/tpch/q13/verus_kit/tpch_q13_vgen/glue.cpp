#include "tpch_q13_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <malloc.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = kit::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q13 (catalog text), :comment_pattern ('%' || :word1 || '%' || :word2 || '%') as $1.
SELECT c_count, COUNT(*) AS custdist
FROM (SELECT c_custkey, COUNT(o_orderkey) AS c_count
      FROM customer LEFT OUTER JOIN orders
        ON c_custkey = o_custkey AND o_comment NOT LIKE $1
      GROUP BY c_custkey) AS c_orders
GROUP BY c_count
ORDER BY custdist DESC, c_count DESC
)SQL";

const char kCustomerScanSql[] = "SELECT c_custkey FROM customer;";
const char kOrderScanSql[] = "SELECT o_custkey, o_comment FROM orders;";

// A text column in the kernel's columnar form: text i is bytes[off[i], off[i + 1]).
struct TextColumn {
  std::vector<uint8_t> bytes;
  std::vector<std::size_t> off{0};

  void Push(const std::string &text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
    off.push_back(bytes.size());
  }
};

struct ScanInputs {
  std::vector<tpchq13vgen_customer_scan> customers;
  std::vector<int64_t> order_custkeys;
  TextColumn order_comments;

  explicit ScanInputs(kit::Host &host) {
    host.Query(kCustomerScanSql, [&](const core::AuxRow &row) {
      customers.push_back({kit::I64(row, 0, 0)});
    });
    host.Query(kOrderScanSql, [&](const core::AuxRow &row) {
      order_custkeys.push_back(kit::I64(row, 0, 0));
      order_comments.Push(kit::Text(row, 1));
    });
  }
};

struct DeltaInputs {
  std::vector<tpchq13vgen_customer_delta> customers;
  std::vector<int64_t> order_custkeys;
  TextColumn order_comments;
  std::vector<int64_t> order_signs;

  explicit DeltaInputs(const kit::Changes &changes) {
    const auto &customer_changes = changes.at(0);
    const auto &order_changes = changes.at(1);
    customers.reserve(customer_changes.size());
    order_custkeys.reserve(order_changes.size());
    order_signs.reserve(order_changes.size());
    for (const auto &change : customer_changes) {
      customers.push_back({{kit::I64(change.row, 0, 0)}, change.sign});
    }
    for (const auto &change : order_changes) {
      order_custkeys.push_back(kit::I64(change.row, 0, 0));
      order_comments.Push(kit::Text(change.row, 1));
      order_signs.push_back(change.sign);
    }
  }
};

void *Build(kit::Host &host) {
  void *state = nullptr;
  {
    const ScanInputs inputs(host);
    state = tpchq13vgen_build(inputs.customers.size(), inputs.customers.data(),
                              inputs.order_custkeys.size(), inputs.order_custkeys.data(),
                              inputs.order_comments.bytes.data(), inputs.order_comments.off.data());
  }
  // The scan copies (and the kernel's per-row buffers) are freed; give the arena back so the
  // resident set is the state, not the build's garbage.
  malloc_trim(0);
  return state;
}

void FreeState(void *state) {
  tpchq13vgen_free(static_cast<tpchq13vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &, int32_t *err) {
  const DeltaInputs inputs(changes);
  return tpchq13vgen_apply(static_cast<const tpchq13vgen_state *>(state),
                          inputs.customers.size(), inputs.customers.data(),
                          inputs.order_custkeys.size(), inputs.order_custkeys.data(),
                          inputs.order_comments.bytes.data(), inputs.order_comments.off.data(),
                          inputs.order_signs.data(), err);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  // The kernel answers only the patterns it tracks (the 16 TPC-H
  // '%word1%word2%' patterns); any other pattern declines (false: SQL answers).
  std::string pattern;
  if (!kit::ArgText(args, 0, pattern)) {
    return false;
  }
  const std::unique_ptr<tpchq13vgen_result, decltype(&tpchq13vgen_result_free)> result(
      call(reinterpret_cast<const uint8_t *>(pattern.data()), pattern.size()),
      tpchq13vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq13vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    int64_t c_count = 0;
    int64_t custdist = 0;
    const uint8_t has_count = tpchq13vgen_result_c0(result.get(), index, &c_count);
    const uint8_t has_dist = tpchq13vgen_result_c1(result.get(), index, &custdist);
    core::AuxRow row;
    row.values.reserve(2);
    row.values.push_back(has_count ? kit::OutI64(c_count) : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(has_dist ? kit::OutI64(custdist) : kit::OutNull(core::AuxType::Int64));
    out.push_back(std::move(row));
  }
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq13vgen_execute(static_cast<const tpchq13vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq13vgens_answer(static_cast<const tpchq13vgens_reading *>(state), x...);
  });
}

void *PrepareApply(const void *base, const kit::Changes &changes,
                   kit::Host &, int32_t *err) {
  const DeltaInputs inputs(changes);
  return tpchq13vgens_stage_apply(static_cast<const tpchq13vgens_base *>(base),
                                   inputs.customers.size(), inputs.customers.data(),
                                   inputs.order_custkeys.size(), inputs.order_custkeys.data(),
                                   inputs.order_comments.bytes.data(), inputs.order_comments.off.data(),
                                   inputs.order_signs.data(), err);
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  void *prepared = nullptr;
  {
    const ScanInputs inputs(host);
    prepared = tpchq13vgens_stage_build(static_cast<const tpchq13vgens_head *>(head_of()),
                                          inputs.customers.size(), inputs.customers.data(),
                                          inputs.order_custkeys.size(), inputs.order_custkeys.data(),
                                          inputs.order_comments.bytes.data(), inputs.order_comments.off.data());
  }
  malloc_trim(0);
  return prepared;
}

const kit::Glue kGlue = {
    "tpch_q13_vgen", "tpch", 13, kQuerySql,
    {{"customer", {"c_custkey"}}, {"orders", {"o_custkey", "o_comment"}}},
    {kit::Kind::Text},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq13vgens_new(); },
        [](void *store) { tpchq13vgens_free(static_cast<tpchq13vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq13vgens_newest_base(static_cast<const tpchq13vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq13vgens_base_state(static_cast<const tpchq13vgens_base *>(base)); },
        [](void *base) { tpchq13vgens_base_free(static_cast<tpchq13vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq13vgens_newest_head(static_cast<const tpchq13vgens_store *>(store)); },
        [](void *head) { tpchq13vgens_head_free(static_cast<tpchq13vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq13vgens_stage_invalid(static_cast<const tpchq13vgens_head *>(head)); },
        [](void *staged) { tpchq13vgens_staged_free(static_cast<tpchq13vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq13vgens_begin(static_cast<const tpchq13vgens_store *>(store), from); },
        [](const void *store) { tpchq13vgens_abort(static_cast<const tpchq13vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq13vgens_install(static_cast<const tpchq13vgens_store *>(store), static_cast<tpchq13vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq13vgens_read(static_cast<const tpchq13vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq13vgens_reading_free(const_cast<tpchq13vgens_reading *>(static_cast<const tpchq13vgens_reading *>(reading))); },
        [](const void *store) { tpchq13vgens_invalidate_all(static_cast<const tpchq13vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq13vgens_version_count(static_cast<const tpchq13vgens_store *>(store)); },
        []() -> std::size_t { return tpchq13vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq13vgens_heap_peak_bytes(); },
        []() { tpchq13vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq13vgens_stage_adopt(static_cast<const tpchq13vgens_head *>(head), static_cast<tpchq13vgen_state *>(state)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
