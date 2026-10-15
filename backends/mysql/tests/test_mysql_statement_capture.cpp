// Unit test of S1 capture (no server): trigger capture calls decoded into the
// typed ColumnarBatches a statement hands to the runtime.
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/mysql/engine/mysql_statement_capture.hpp"

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " +     \
                               #cond);                                                             \
    }                                                                                              \
  } while (0)

namespace {

using namespace accelerator_codegen::aux_state;
using engine::mysql::CapturedRow;
using engine::mysql::DecodeCapturedRow;
using engine::mysql::StatementCapture;
using runtime::ColumnarBatch;

// A capture call's arguments; nullptr entries are SQL NULL.
struct Call {
  Call(std::string table, std::string kind, std::string types, std::vector<const char *> values)
      : texts{std::move(table), std::move(kind), std::move(types)} {
    for (const auto &text : texts) {
      args.push_back(const_cast<char *>(text.c_str()));
      lengths.push_back(text.size());
    }
    for (const char *value : values) {
      args.push_back(const_cast<char *>(value));
      lengths.push_back(value ? std::strlen(value) : 0);
    }
  }
  CapturedRow Decode() { return DecodeCapturedRow(args.size(), args.data(), lengths.data()); }
  std::vector<std::string> texts;
  std::vector<char *> args;
  std::vector<unsigned long> lengths;
};

// Types: int, string, decimal, date.
CapturedRow Insert(const char *key, const char *name, const char *price, const char *day) {
  return Call("5", "0", "isnd", {key, name, price, day}).Decode();
}

void InsertsShareOneBatch() {
  StatementCapture capture;
  capture.Add(Insert("1", "a", "1.50", "1970-01-02"), {});
  capture.Add(Insert("2", "bc", "2.25", "1970-01-03"), {});
  auto batches = capture.Take();
  CHECK(batches.size() == 1 && capture.Empty());
  const ColumnarBatch &batch = *batches[0];
  CHECK(batch.table == 5 && batch.kind == core::MutationKind::Insert && batch.row_count == 2);
  CHECK(batch.new_rows.attnums == std::vector<int32_t>({0, 1, 2, 3}));
  const auto &price = batch.new_rows.columns[2];
  CHECK(price.scale == 2 && price.Value(1).payload == core::AuxValue::Of(
                                core::AuxType::ScaledDecimal, core::ScaledDecimal{225, 2}).payload);
  CHECK(std::get<std::string>(batch.new_rows.columns[1].Value(1).payload) == "bc");
  CHECK(std::get<core::AuxDate>(batch.new_rows.columns[3].Value(0).payload).days_since_epoch == 1);
}

void WatchedColumnsOnly() {
  StatementCapture capture;
  capture.Add(Insert("7", "x", "3.00", "1970-01-01"), {0, 2});
  const auto batches = capture.Take();
  CHECK(batches[0]->new_rows.attnums == std::vector<int32_t>({0, 2}));
  CHECK(batches[0]->new_rows.columns.size() == 2);
}

void KindOrTableChangeStartsABatch() {
  StatementCapture capture;
  capture.Add(Insert("1", "a", "1.00", "1970-01-01"), {});
  capture.Add(Call("5", "1", "isnd", {"1", "a", "1.00", "1970-01-01"}).Decode(), {});
  capture.Add(Call("6", "1", "isnd", {"1", "a", "1.00", "1970-01-01"}).Decode(), {});
  const auto batches = capture.Take();
  CHECK(batches.size() == 3);
  CHECK(batches[1]->kind == core::MutationKind::Delete && batches[1]->old_rows.columns.size() == 4);
  CHECK(batches[2]->table == 6);
}

void NullFirstRow() {
  StatementCapture capture;
  capture.Add(Insert("1", nullptr, nullptr, nullptr), {});
  capture.Add(Insert("2", "b", "4.5", "1970-01-01"), {});
  const auto batches = capture.Take();
  const auto &set = batches[0]->new_rows;
  CHECK(set.columns[1].IsNull(0) && !set.columns[1].IsNull(1));
  CHECK(set.columns[2].IsNull(0) && set.columns[2].scale == 1); // the first value's scale
  CHECK(set.columns[3].IsNull(0));
}

void UpdateReportsChangedColumns() {
  StatementCapture capture;
  capture.Add(Call("5", "2", "isnd",
                   {"1", "a", "1.00", "1970-01-01", "1", "a", "2.00", "1970-01-01"}).Decode(), {});
  capture.Add(Call("5", "2", "isnd",
                   {"2", "b", "1.00", "1970-01-01", "2", "c", "1.00", "1970-01-01"}).Decode(), {});
  const auto batches = capture.Take();
  CHECK(batches.size() == 1 && batches[0]->row_count == 2);
  CHECK(batches[0]->updated_attnums == std::vector<int32_t>({1, 2})); // name and price
  CHECK(std::get<std::string>(batches[0]->old_rows.columns[1].Value(1).payload) == "b");
  CHECK(std::get<std::string>(batches[0]->new_rows.columns[1].Value(1).payload) == "c");
}

void MalformedCallsThrow() {
  auto throws = [](auto &&fn) {
    try {
      fn();
    } catch (const std::runtime_error &) {
      return true;
    }
    return false;
  };
  CHECK(throws([] { Call("5", "0", "is", {"1"}).Decode(); }));                 // too few values
  CHECK(throws([] { Call("5", "3", "is", {"1", "a"}).Decode(); }));            // unknown kind
  CHECK(throws([] { Call("x", "0", "is", {"1", "a"}).Decode(); }));            // table id
  StatementCapture capture;
  CHECK(throws([&] { capture.Add(Call("5", "0", "is", {"12x", "a"}).Decode(), {}); }));
  CHECK(throws([&] { capture.Add(Call("5", "0", "q", {"1"}).Decode(), {}); }));
  CHECK(throws([&] { capture.Add(Call("5", "0", "is", {"1", "a"}).Decode(), {0, 5}); }));
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, void (*)()>> tests = {
      {"inserts_share_one_batch", InsertsShareOneBatch},
      {"watched_columns_only", WatchedColumnsOnly},
      {"kind_or_table_change_starts_a_batch", KindOrTableChangeStartsABatch},
      {"null_first_row", NullFirstRow},
      {"update_reports_changed_columns", UpdateReportsChangedColumns},
      {"malformed_calls_throw", MalformedCallsThrow},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << "\n";
    } catch (const std::exception &e) {
      std::cout << "FAIL " << name << ": " << e.what() << "\n";
      failed++;
    }
  }
  return failed == 0 ? 0 : 1;
}
