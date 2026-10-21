#pragma once
//
// RowPieces<Row>, GroupRows
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
#include <iterator>
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

} // namespace accelerator_codegen::aux_state::core
