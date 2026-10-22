// Wide types: the 128-bit decimal cell, 64-bit image headers, 64-bit PartitionKey.
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

#include "backends/duckdb/engine/capture.hpp"
#include "backends/duckdb/api/decimal128.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "duckdb.hpp"

using namespace accelerator_codegen::aux_state;
namespace engine_duckdb = accelerator_codegen::aux_state::engine::duckdb;

static_assert(std::is_same_v<PartitionKey, int64_t>);
static_assert(static_cast<int>(core::AuxType::ScaledDecimal128) == 7);
static_assert(std::is_same_v<std::variant_alternative_t<7, core::AuxPayload>, core::ScaledDecimal128>);

static void Check(bool ok, const std::string &what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << "\n";
    std::exit(1);
  }
}

static void TestDecimalText() {
  const auto text = [](unsigned __int128 mag, bool neg, int scale) {
    return core::ScaledDecimal128ToString(neg ? -static_cast<__int128>(mag) : static_cast<__int128>(mag), scale);
  };
  Check(text(0, false, 0) == "0", "zero");
  Check(text(5, true, 3) == "-0.005", "small negative");
  Check(text(12345, true, 2) == "-123.45", "negative");
  Check(text(12, false, -2) == "1200", "negative scale");
  const unsigned __int128 big = static_cast<unsigned __int128>(1) << 100; // 1267650600228229401496703205376
  Check(text(big, false, 0) == "1267650600228229401496703205376", "2^100");
  Check(text(big, true, 6) == "-1267650600228229401496703.205376", "-2^100 scaled");
  const __int128 min = static_cast<__int128>(static_cast<unsigned __int128>(1) << 127);
  Check(core::ScaledDecimal128ToString(min, 0) == "-170141183460469231731687303715884105728", "int128 min");
}

static void TestDuckdbRoundTrip(duckdb::Connection &con, const std::string &literal, int scale) {
  const std::string type = "DECIMAL(38," + std::to_string(scale) + ")";
  auto result = con.Query("SELECT CAST('" + literal + "' AS " + type + ")");
  Check(!result->HasError(), result->GetError());
  const auto original = result->GetValue(0, 0);

  const auto cell = engine_duckdb::CapturedValue(original);
  const auto *wide = std::get_if<core::ScaledDecimal128>(&cell.payload);
  Check(cell.type == core::AuxType::ScaledDecimal128 && wide && wide->scale == scale, "captured as 128-bit " + literal);
  Check(core::ScaledDecimal128ToString(wide->value, wide->scale) == literal, "text " + literal);

  const auto back = engine_duckdb::Decimal128ToValue(*wide);
  Check(back.type() == original.type() && back == original, "value " + literal);
}

static void TestImageHeader() {
  core::ByteWriter writer;
  const uint64_t count = (uint64_t(1) << 33) + 7;
  core::WriteImageHeader(writer, 0xABCD, 3, count);
  writer.String("wide");
  core::ByteReader reader(writer.bytes);
  uint64_t got = 0;
  std::string text;
  Check(core::ReadImageHeader(reader, 0xABCD, 3, got) && got == count, "64-bit count");
  Check(reader.String(text) && text == "wide" && reader.FullyConsumed(), "64-bit string length");

  core::ByteReader wrong(writer.bytes);
  Check(!core::ReadImageHeader(wrong, 0xABCD, 4, got) && !core::ReadImageHeader(wrong, 0x1, 3, got), "mismatch rejected");

  core::ByteWriter legacy; // the former header: u32 count, u32 string lengths
  legacy.U32(0xABCD);
  legacy.U32(3);
  legacy.U32(5);
  legacy.String("old");
  core::ByteReader old(legacy.bytes);
  Check(core::ReadImageHeader(old, 0xABCD, 3, got) && got == 5, "legacy count");
  Check(old.String(text) && text == "old" && old.FullyConsumed(), "legacy string length");
}

int main() {
  TestDecimalText();
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);
  TestDuckdbRoundTrip(con, "123456789012345678901234567890.12", 2);
  TestDuckdbRoundTrip(con, "-99999999999999999999999999999999999999", 0);
  TestDuckdbRoundTrip(con, "-0.12345678901234567890123456789012345678", 38);
  std::cout << "wide types OK\n";
}
