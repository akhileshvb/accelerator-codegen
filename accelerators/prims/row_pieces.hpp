#pragma once
//
// RowPieces<Row>, InternedPieces<Row>, GroupRows, FillChunks
// PURPOSE    Partials of a partitioned scan that cost O(rows seen) to create and O(1)
//            to merge, and the parallel regroup of those rows afterwards. A bulk load
//            then runs per group on the pool instead of merging per-partition
//            structures sized by the key space one after another.
// COMPLEXITY RowPieces::Merge moves one vector per piece, never a row. GroupRows is a
//            stable counting sort of every piece (one piece per task): O(rows).
// MEMORY     GroupRows holds a second copy of each piece until the caller drops it.
// THREADS    Pieces are private to their scan partition until merged; GroupRows runs
//            one piece per task through `Parallelism`.
// USE WHEN   the scan feeds a structure keyed by a hash, id or range (sharded maps,
//            chunked arrays) whose rows must reach it in scan order.
//

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "prims/parallelism.hpp"

namespace accelerator_codegen::aux_state::core {

// The rows of one scan partition; merging concatenates the piece lists in partition order.
template <class Row> struct RowPieces {
  std::vector<std::vector<Row>> pieces = std::vector<std::vector<Row>>(1);

  void Add(Row row) { pieces.back().push_back(std::move(row)); }
  static void Merge(RowPieces &into, RowPieces &&from) {
    into.pieces.insert(into.pieces.end(), std::make_move_iterator(from.pieces.begin()),
                       std::make_move_iterator(from.pieces.end()));
    from.pieces.clear();
  }
};

// RowPieces whose rows carry a small-integer id of a string, numbered per piece in
// first-seen order. Unify renumbers them in piece order, which is the numbering
// the serial scan would have produced.
template <class Row> struct InternedPieces : RowPieces<Row> {
  std::vector<std::vector<std::string>> piece_names = std::vector<std::vector<std::string>>(1);
  std::unordered_map<std::string, int32_t> index; // of the scanning (last) piece

  int32_t Intern(const std::string &name) {
    const auto it = index.find(name);
    if (it != index.end())
      return it->second;
    auto &names = piece_names.back();
    const auto id = static_cast<int32_t>(names.size());
    names.push_back(name);
    index.emplace(name, id);
    return id;
  }
  static void Merge(InternedPieces &into, InternedPieces &&from) {
    RowPieces<Row>::Merge(into, std::move(from));
    into.piece_names.insert(into.piece_names.end(),
                            std::make_move_iterator(from.piece_names.begin()),
                            std::make_move_iterator(from.piece_names.end()));
    from.piece_names.clear();
  }
  // global_id(name) numbers a string; set_id(row, table) rewrites a row's local id
  // through table (local id -> global id).
  template <class GlobalId, class SetId>
  void Unify(const Parallelism &parallelism, GlobalId &&global_id, SetId &&set_id) {
    std::vector<std::vector<int32_t>> renumber(this->pieces.size());
    for (std::size_t p = 0; p < renumber.size(); ++p)
      for (const auto &name : piece_names[p])
        renumber[p].push_back(global_id(name));
    parallelism.For(this->pieces.size(), [&](std::size_t p) {
      for (Row &row : this->pieces[p])
        set_id(row, renumber[p]);
    });
  }
};

// One piece's rows ordered by group (scan order kept inside a group): the rows of
// group g are rows[begin[g], begin[g + 1]).
template <class Row> struct GroupedPiece {
  std::vector<Row> rows;
  std::vector<std::size_t> begin;
};

// Groups every piece by group_of(row) in [0, groups) and empties the input pieces.
template <class Row, class GroupOf>
std::vector<GroupedPiece<Row>> GroupRows(const Parallelism &parallelism,
                                         std::vector<std::vector<Row>> &pieces,
                                         std::size_t groups, GroupOf &&group_of) {
  std::vector<GroupedPiece<Row>> grouped(pieces.size());
  parallelism.For(pieces.size(), [&](std::size_t p) {
    auto &source = pieces[p];
    auto &out = grouped[p];
    out.begin.assign(groups + 1, 0);
    for (const Row &row : source)
      ++out.begin[group_of(row) + 1];
    for (std::size_t g = 0; g < groups; ++g)
      out.begin[g + 1] += out.begin[g];
    std::vector<std::size_t> next(out.begin.begin(), out.begin.end() - 1);
    out.rows.resize(source.size());
    for (Row &row : source)
      out.rows[next[group_of(row)]++] = std::move(row);
    std::vector<Row>().swap(source);
  });
  return grouped;
}

// Calls apply(array.Mutable(key_of(row)), row) for every row of `pieces` (keys >= 0),
// rows of one chunk in piece order. Chunks are dealt to groups that run concurrently
// on the pool, so `array` (a ChunkedCowArray) needs no lock. Empties `pieces`.
template <class Array, class Row, class KeyOf, class Apply>
void FillChunks(const Parallelism &parallelism, Array &array,
                std::vector<std::vector<Row>> &pieces, KeyOf &&key_of, Apply &&apply) {
  constexpr std::size_t kGroups = 1024;
  std::vector<int64_t> top(pieces.size(), -1);
  parallelism.For(pieces.size(), [&](std::size_t p) {
    for (const Row &row : pieces[p])
      top[p] = std::max<int64_t>(top[p], key_of(row));
  });
  const int64_t last = top.empty() ? -1 : *std::max_element(top.begin(), top.end());
  if (last < 0)
    return;
  array.Reserve(last);
  auto grouped = GroupRows(parallelism, pieces, kGroups, [&](const Row &row) {
    return static_cast<std::size_t>(key_of(row)) / Array::kChunkSize % kGroups;
  });
  parallelism.For(kGroups, [&](std::size_t g) {
    for (auto &piece : grouped)
      for (std::size_t i = piece.begin[g]; i < piece.begin[g + 1]; ++i)
        apply(array.Mutable(key_of(piece.rows[i])), piece.rows[i]);
  });
}

} // namespace accelerator_codegen::aux_state::core
