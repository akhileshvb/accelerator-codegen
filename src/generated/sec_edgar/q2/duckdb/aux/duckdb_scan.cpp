#include "sec_edgar/q2/duckdb/aux/duckdb_scan.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
#include <unordered_set>

namespace accelerator_codegen::aux_state::query {
namespace scan = ::accelerator_codegen::aux_state::engine::duckdb::scan;
namespace {

using ::duckdb::DataChunk;
using ::duckdb::idx_t;
using ::duckdb::string_t;
using ::duckdb::UnifiedVectorFormat;

std::string Quote(const std::string &s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string QuoteList(const std::vector<std::string> &values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += Quote(values[i]);
  }
  return out;
}

// Streams the result of `sql` chunk by chunk.
template <typename Fn>
void Stream(::duckdb::Connection &connection, const std::string &sql, Fn &&fn) {
  auto result = scan::ScanConnection(connection).SendQuery(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error("sec_edgar q2 scan failed: " +
                             (result ? result->GetError() : std::string("null")));
  }
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    fn(*chunk);
  }
  if (result->HasError()) {
    throw std::runtime_error("sec_edgar q2 scan failed: " + result->GetError());
  }
}

// Typed, selection-aware access to one chunk column.
template <typename T> struct Col {
  UnifiedVectorFormat format;
  const T *data = nullptr;
  void Init(DataChunk &chunk, idx_t column) {
    chunk.data[column].ToUnifiedFormat(chunk.size(), format);
    data = UnifiedVectorFormat::GetData<T>(format);
  }
  idx_t Index(idx_t row) const { return format.sel->get_index(row); }
  bool Valid(idx_t row) const { return format.validity.RowIsValid(Index(row)); }
  const T &Get(idx_t row) const { return data[Index(row)]; }
};

std::string_view View(const string_t &s) {
  return std::string_view(s.GetData(), s.GetSize());
}

} // namespace

void SecEdgarQ2DuckdbScan::ScanNum(
    const std::function<void(const SecEdgarQ2NumInput &)> &consumer) {
  Stream(connection,
         "SELECT adsh, tag, uom, value FROM num WHERE adsh IS NOT NULL AND "
         "tag IS NOT NULL AND uom IS NOT NULL",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, tag, uom;
           Col<double> value;
           adsh.Init(chunk, 0);
           tag.Init(chunk, 1);
           uom.Init(chunk, 2);
           value.Init(chunk, 3);
           SecEdgarQ2NumInput in;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             in.adsh.assign(View(adsh.Get(r)));
             in.tag.assign(View(tag.Get(r)));
             in.uom.assign(View(uom.Get(r)));
             in.value_is_null = !value.Valid(r);
             in.value = in.value_is_null ? 0.0 : value.Get(r);
             consumer(in);
           }
         });
}

void SecEdgarQ2DuckdbScan::ScanSub(
    const std::function<void(const SecEdgarQ2SubInput &)> &consumer) {
  Stream(connection, "SELECT adsh, name, fy FROM sub", [&](DataChunk &chunk) {
    Col<string_t> adsh, name;
    Col<int32_t> fy;
    adsh.Init(chunk, 0);
    name.Init(chunk, 1);
    fy.Init(chunk, 2);
    SecEdgarQ2SubInput in;
    for (idx_t r = 0; r < chunk.size(); ++r) {
      in.adsh.assign(View(adsh.Get(r)));
      in.name_is_null = !name.Valid(r);
      if (in.name_is_null) {
        in.name.clear();
      } else {
        in.name.assign(View(name.Get(r)));
      }
      in.fy_is_null = !fy.Valid(r);
      in.fy = in.fy_is_null ? 0 : fy.Get(r);
      consumer(in);
    }
  });
}

void SecEdgarQ2DuckdbScan::ScanUomRowCounts(
    const std::function<void(const std::string &, uint64_t)> &consumer) {
  Stream(connection,
         "SELECT uom, count(*) FROM num WHERE value IS NOT NULL AND uom IS NOT "
         "NULL AND adsh IS NOT NULL AND tag IS NOT NULL GROUP BY uom",
         [&](DataChunk &chunk) {
           Col<string_t> uom;
           Col<int64_t> rows;
           uom.Init(chunk, 0);
           rows.Init(chunk, 1);
           std::string name;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             name.assign(View(uom.Get(r)));
             consumer(name, static_cast<uint64_t>(rows.Get(r)));
           }
         });
}

void SecEdgarQ2DuckdbScan::ScanNumValueCounts(
    const std::vector<std::string> &uoms,
    const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) {
  if (uoms.empty()) {
    return;
  }
  Stream(connection,
         "SELECT adsh, tag, uom, value, count(*) FROM num WHERE value IS NOT "
         "NULL AND adsh IS NOT NULL AND tag IS NOT NULL AND uom IN (" +
             QuoteList(uoms) + ") GROUP BY adsh, tag, uom, value",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, tag, uom;
           Col<double> value;
           Col<int64_t> rows;
           adsh.Init(chunk, 0);
           tag.Init(chunk, 1);
           uom.Init(chunk, 2);
           value.Init(chunk, 3);
           rows.Init(chunk, 4);
           for (idx_t r = 0; r < chunk.size(); ++r) {
             consumer({View(adsh.Get(r)), View(tag.Get(r)), View(uom.Get(r)),
                       value.Get(r), static_cast<uint64_t>(rows.Get(r))});
           }
         });
}

bool SecEdgarQ2DuckdbScan::ScanNumKeys(
    const std::vector<std::string> &uoms,
    const std::function<void(std::string_view)> &adsh_fn,
    const std::function<void(std::string_view)> &tag_fn) {
  if (uoms.empty()) {
    return true;
  }
  const std::string where = " FROM num WHERE value IS NOT NULL AND adsh IS NOT NULL AND "
                            "tag IS NOT NULL AND uom IN (" +
                            QuoteList(uoms) + ")";
  for (int which = 0; which < 2; ++which) {
    const auto &fn = which == 0 ? adsh_fn : tag_fn;
    Stream(connection, std::string("SELECT DISTINCT ") + (which == 0 ? "adsh" : "tag") + where,
           [&](DataChunk &chunk) {
             Col<string_t> col;
             col.Init(chunk, 0);
             for (idx_t r = 0; r < chunk.size(); ++r) {
               fn(View(col.Get(r)));
             }
           });
  }
  return true;
}

bool SecEdgarQ2DuckdbScan::ScanNumValueCountsParallel(
    const std::vector<std::string> &uoms, unsigned threads,
    const std::function<void(unsigned, const SecEdgarQ2ValueCountRow *, size_t)> &batch) {
  if (uoms.empty()) {
    return true;
  }
  auto result = scan::ScanConnection(connection).Query(
      "SELECT adsh, tag, uom, value, count(*) FROM num WHERE value IS NOT "
      "NULL AND adsh IS NOT NULL AND tag IS NOT NULL AND uom IN (" +
      QuoteList(uoms) + ") GROUP BY adsh, tag, uom, value");
  if (!result || result->HasError()) {
    throw std::runtime_error("sec_edgar q2 scan failed: " +
                             (result ? result->GetError() : std::string("null")));
  }
  using ChunkPtr = ::duckdb::unique_ptr<DataChunk>;
  std::mutex mu;
  std::condition_variable cv_pop, cv_push;
  std::deque<ChunkPtr> queue;
  bool closed = false;
  std::exception_ptr error;
  const size_t max_queue = static_cast<size_t>(threads) * 2;
  std::vector<std::thread> workers;
  for (unsigned w = 0; w < threads; ++w) {
    workers.emplace_back([&, w] {
      std::vector<SecEdgarQ2ValueCountRow> rows;
      while (true) {
        ChunkPtr chunk;
        {
          std::unique_lock<std::mutex> lock(mu);
          cv_pop.wait(lock, [&] { return closed || !queue.empty(); });
          if (queue.empty()) {
            return;
          }
          chunk = std::move(queue.front());
          queue.pop_front();
        }
        cv_push.notify_one();
        try {
          Col<string_t> adsh, tag, uom;
          Col<double> value;
          Col<int64_t> counts;
          adsh.Init(*chunk, 0);
          tag.Init(*chunk, 1);
          uom.Init(*chunk, 2);
          value.Init(*chunk, 3);
          counts.Init(*chunk, 4);
          rows.clear();
          for (idx_t r = 0; r < chunk->size(); ++r) {
            rows.push_back({View(adsh.Get(r)), View(tag.Get(r)), View(uom.Get(r)),
                            value.Get(r), static_cast<uint64_t>(counts.Get(r))});
          }
          batch(w, rows.data(), rows.size());
        } catch (...) {
          std::lock_guard<std::mutex> lock(mu);
          if (!error) {
            error = std::current_exception();
          }
        }
      }
    });
  }
  std::exception_ptr fetch_error;
  try {
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) {
        break;
      }
      std::unique_lock<std::mutex> lock(mu);
      cv_push.wait(lock, [&] { return queue.size() < max_queue || error; });
      if (error) {
        break;
      }
      queue.push_back(std::move(chunk));
      lock.unlock();
      cv_pop.notify_one();
    }
  } catch (...) {
    fetch_error = std::current_exception();
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    closed = true;
  }
  cv_pop.notify_all();
  for (auto &t : workers) {
    t.join();
  }
  if (fetch_error) {
    std::rethrow_exception(fetch_error);
  }
  if (error) {
    std::rethrow_exception(error);
  }
  if (result->HasError()) {
    throw std::runtime_error("sec_edgar q2 scan failed: " + result->GetError());
  }
  return true;
}

void SecEdgarQ2DuckdbScan::LookupNumValueCounts(
    const std::string &uom,
    const std::vector<std::pair<std::string, std::string>> &groups,
    const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) {
  if (groups.empty()) {
    return;
  }
  std::vector<std::string> adshs, tags;
  std::unordered_set<std::string> exact;
  for (const auto &g : groups) {
    adshs.push_back(g.first);
    tags.push_back(g.second);
    exact.insert(g.first + std::string(1, '\0') + g.second);
  }
  Stream(connection,
         "SELECT adsh, tag, value, count(*) FROM num WHERE uom = " + Quote(uom) +
             " AND value IS NOT NULL AND adsh IN (" + QuoteList(adshs) +
             ") AND tag IN (" + QuoteList(tags) + ") GROUP BY adsh, tag, value",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, tag;
           Col<double> value;
           Col<int64_t> rows;
           adsh.Init(chunk, 0);
           tag.Init(chunk, 1);
           value.Init(chunk, 2);
           rows.Init(chunk, 3);
           std::string key;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             key.assign(View(adsh.Get(r)));
             key.push_back('\0');
             key.append(View(tag.Get(r)));
             if (exact.count(key) == 0) {
               continue;
             }
             consumer({View(adsh.Get(r)), View(tag.Get(r)), uom, value.Get(r),
                       static_cast<uint64_t>(rows.Get(r))});
           }
         });
}

void SecEdgarQ2DuckdbScan::LookupSub(
    const std::vector<std::string> &adshs,
    const std::function<void(const SecEdgarQ2SubInput &)> &consumer) {
  if (adshs.empty()) {
    return;
  }
  Stream(connection,
         "SELECT adsh, name, fy FROM sub WHERE adsh IN (" + QuoteList(adshs) + ")",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, name;
           Col<int32_t> fy;
           adsh.Init(chunk, 0);
           name.Init(chunk, 1);
           fy.Init(chunk, 2);
           SecEdgarQ2SubInput in;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             in.adsh.assign(View(adsh.Get(r)));
             in.name_is_null = !name.Valid(r);
             in.name.assign(in.name_is_null ? std::string_view() : View(name.Get(r)));
             in.fy_is_null = !fy.Valid(r);
             in.fy = in.fy_is_null ? 0 : fy.Get(r);
             consumer(in);
           }
         });
}

void SecEdgarQ2DuckdbScan::ExecuteDirect(
    int64_t fy, const std::string &uom,
    const std::function<void(const SecEdgarQ2DirectRow &)> &consumer) {
  const std::string u = Quote(uom);
  Stream(connection,
         "SELECT s.name, n.tag, n.value FROM num n "
         "JOIN sub s ON n.adsh = s.adsh "
         "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num "
         "WHERE uom = " +
             u +
             " AND value IS NOT NULL GROUP BY adsh, tag) m "
             "ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
             "WHERE n.uom = " +
             u + " AND s.fy = " + std::to_string(fy) +
             " AND n.value IS NOT NULL "
             "ORDER BY n.value DESC, s.name, n.tag LIMIT " +
             std::to_string(kSecEdgarQ2ResultLimit),
         [&](DataChunk &chunk) {
           Col<string_t> name, tag;
           Col<double> value;
           name.Init(chunk, 0);
           tag.Init(chunk, 1);
           value.Init(chunk, 2);
           for (idx_t r = 0; r < chunk.size(); ++r) {
             const bool null_name = !name.Valid(r);
             consumer({null_name ? std::string_view() : View(name.Get(r)),
                       null_name, View(tag.Get(r)), value.Get(r)});
           }
         });
}

} // namespace accelerator_codegen::aux_state::query
