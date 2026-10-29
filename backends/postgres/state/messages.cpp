// GENERATED BACKEND
#include "backends/postgres/state/messages.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>

#include "substrate/aux/runtime/wire.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

constexpr uint8_t kVersion = 1;

template <typename T>
void Put(std::string &out, T value) {
  out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <typename T>
T Get(std::string_view &in) {
  if (in.size() < sizeof(T)) {
    throw std::runtime_error("commit message truncated");
  }
  T value;
  std::memcpy(&value, in.data(), sizeof(T));
  in.remove_prefix(sizeof(T));
  return value;
}

} // namespace

// u8 version | u32 accelerator | u64 epoch | u8 empty | u8 invalidates
// | u64 n | batch[n]
void EncodeCommit(const CommitMessage &message, std::string &out) {
  Put<uint8_t>(out, kVersion);
  Put<uint32_t>(out, message.accelerator);
  Put<uint64_t>(out, message.epoch);
  Put<uint8_t>(out, message.empty);
  Put<uint8_t>(out, message.invalidates);
  Put<uint64_t>(out, message.batches.size());
  for (const auto &batch : message.batches) {
    runtime::EncodeBatch(*batch, out);
  }
}

CommitMessage DecodeCommit(std::string_view in) {
  if (Get<uint8_t>(in) != kVersion) {
    throw std::runtime_error("commit message: unknown version");
  }
  CommitMessage message;
  message.accelerator = Get<uint32_t>(in);
  message.epoch = Get<uint64_t>(in);
  message.empty = Get<uint8_t>(in) != 0;
  message.invalidates = Get<uint8_t>(in) != 0;
  const auto count = Get<uint64_t>(in);
  for (uint64_t i = 0; i < count; i++) {
    message.batches.push_back(
        std::make_shared<const runtime::ColumnarBatch>(runtime::DecodeBatch(in)));
  }
  if (!in.empty()) {
    throw std::runtime_error("commit message: trailing bytes");
  }
  return message;
}

// u8 version | u32 slot | u32 member | i64 deadline_ns | u64 epoch | u64 max_wait_us | arguments
void EncodeRequest(const ReadRequest &request, std::string &out) {
  Put<uint8_t>(out, kVersion);
  Put<uint32_t>(out, request.slot);
  Put<uint32_t>(out, request.member);
  Put<int64_t>(out, request.deadline_ns);
  Put<uint64_t>(out, request.epoch);
  Put<uint64_t>(out, request.max_wait_us);
  runtime::EncodeArguments(request.arguments, out);
}

ReadRequest DecodeRequest(std::string_view in) {
  if (Get<uint8_t>(in) != kVersion) {
    throw std::runtime_error("read request: unknown version");
  }
  ReadRequest request;
  request.slot = Get<uint32_t>(in);
  request.member = Get<uint32_t>(in);
  request.deadline_ns = Get<int64_t>(in);
  request.epoch = Get<uint64_t>(in);
  request.max_wait_us = Get<uint64_t>(in);
  request.arguments = runtime::DecodeArguments(in);
  if (!in.empty()) {
    throw std::runtime_error("read request: trailing bytes");
  }
  return request;
}

// u8 served | rows (only when served)
void EncodeResponse(const ReadResponse &response, std::string &out) {
  Put<uint8_t>(out, response.served);
  if (response.served) {
    runtime::EncodeRows(response.rows, out);
  }
}

ReadResponse DecodeResponse(std::string_view in) {
  ReadResponse response;
  response.served = Get<uint8_t>(in) != 0;
  if (response.served) {
    response.rows = runtime::DecodeRows(in);
  }
  if (!in.empty()) {
    throw std::runtime_error("read response: trailing bytes");
  }
  return response;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
