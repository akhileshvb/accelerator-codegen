// The canary suite's accelerators: one module that mirrors tables as multisets of rows. Its answer
// is the rows themselves, so any lost, duplicated, stale or corrupted row shows against the SQL.
// It is written against the substrate only; the scan comes from the backend (ScanFn).
#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "canary_engine.hpp"

namespace accelerator_codegen::canary {

struct RowOrder {
  bool operator()(const Row &a, const Row &b) const { return a < b; }
};
using RowSet = std::multiset<Row, RowOrder>;

struct TableShape {
  std::string table;
  std::vector<CanaryColumn> columns;
  std::vector<int> output; // columns of the answer, in order; empty: all
};

struct MirrorOptions {
  std::size_t partitions = 1; // scan partitions of a build
  bool incremental = true;    // maintain writes; else every write rebuilds
  bool decline = false;       // the state never serves: every read is answered from SQL
  bool tagged = false;        // the answer's first column is the table's position
};

inline Cell ToCell(const core::AuxValue &value, const CanaryColumn &column) {
  if (value.is_null) {
    return std::monostate{};
  }
  if (column.is_text) {
    return std::get<std::string>(value.payload);
  }
  if (const auto *i = std::get_if<int64_t>(&value.payload)) {
    return *i;
  }
  return static_cast<int64_t>(std::get<uint64_t>(value.payload));
}

class MirrorModule final : public core::AuxQueryModule {
public:
  MirrorModule(std::string id, std::vector<TableShape> shapes, ScanFn scan, MirrorOptions options)
      : id_(std::move(id)), shapes_(std::move(shapes)), scan_(std::move(scan)), options_(options) {}

  core::AcceleratorId Id() const override { return id_; }

  std::vector<core::TableDependency> Dependencies() const override {
    std::vector<core::TableDependency> out;
    for (std::size_t t = 0; t < shapes_.size(); ++t) {
      core::TableDependency dep;
      dep.table = t;
      for (std::size_t c = 0; c < shapes_[t].columns.size(); ++c) {
        dep.watched_columns.push_back(static_cast<core::ColumnId>(c));
        dep.mutation_attnums.push_back(static_cast<int32_t>(c));
      }
      out.push_back(std::move(dep));
    }
    return out;
  }

  struct Ops final : core::AuxTxnBuffer {
    struct Op {
      bool add;
      Row row;
    };
    std::map<std::size_t, std::vector<Op>> by_table;
    bool rebuild = false;
    bool HasPendingChanges() const override { return rebuild || !by_table.empty(); }
    void Clear() override {
      by_table.clear();
      rebuild = false;
    }
  };

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override { return std::make_unique<Ops>(); }

  void Build(core::AuxEngineServices &services, core::StateBuilder &builder) override {
    for (std::size_t t = 0; t < shapes_.size(); ++t) {
      const auto &shape = shapes_[t];
      std::vector<std::vector<Row>> partials(std::max<std::size_t>(options_.partitions, 1));
      scan_(services, shape.table, shape.columns, options_.partitions, [&](std::size_t p, const ScanBatch &batch) {
        ReadBatch(batch, shape, partials.at(p));
      });
      auto state = std::make_shared<RowSet>();
      for (auto &partial : partials) {
        state->insert(std::make_move_iterator(partial.begin()), std::make_move_iterator(partial.end()));
      }
      builder.Write(core::StateImage::Make<RowSet>(id_, t, 0, std::shared_ptr<const RowSet>(std::move(state))));
    }
    builder.Finish();
  }

  core::InvalidationPlan InterpretMutation(const core::MutationBatch &mutation) const override {
    core::InvalidationPlan plan;
    plan.scope = aux_state::InvalidationScope::PartitionSet;
    plan.partitions.insert(mutation.table);
    return plan;
  }

  void AccumulateMutation(const core::MutationBatch &mutation, core::AuxEngineServices &,
                          core::AuxTxnBuffer &base) override {
    auto &ops = static_cast<Ops &>(base);
    if (!options_.incremental || mutation.table >= shapes_.size()) {
      ops.rebuild = true;
      return;
    }
    const auto &columns = shapes_[mutation.table].columns;
    const auto to_row = [&](const core::AuxRow &row) {
      Row out;
      for (std::size_t c = 0; c < columns.size(); ++c) {
        out.push_back(ToCell(row.values.at(c), columns[c]));
      }
      return out;
    };
    auto &list = ops.by_table[mutation.table];
    if (mutation.kind == core::MutationKind::Update && mutation.old_rows.size() != mutation.new_rows.size()) {
      ops.rebuild = true;
      return;
    }
    for (const auto &row : mutation.old_rows) {
      if (mutation.kind != core::MutationKind::Insert) {
        list.push_back({false, to_row(row)});
      }
    }
    for (const auto &row : mutation.new_rows) {
      if (mutation.kind != core::MutationKind::Delete) {
        list.push_back({true, to_row(row)});
      }
    }
  }

  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &services, const core::InvalidationPlan &,
                                                  core::AuxTxnBuffer &base) override {
    auto &ops = static_cast<Ops &>(base);
    core::StagedMaintenanceOutcome outcome;
    if (ops.rebuild) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    // The runtime's maintenance policy decides, as for any accelerator: a small change to a state
    // this size is applied; the table row counts it is given are the engine's (TableRows).
    std::size_t changed_rows = 0;
    for (const auto &entry : ops.by_table) {
      changed_rows += entry.second.size();
    }
    const auto decision = services.ChooseMaintenance(core::MaintenanceRequest::IncrementalPartitionedNoPartitionRebuild(
        changed_rows, ops.by_table.size(), services.ExpectedQueriesBeforeNextWrite()));
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      core::FinishRebuildAll(outcome, decision);
      return outcome;
    }
    const auto current = services.StateStore().Read(services.CurrentVisibility());
    for (const auto &[table, list] : ops.by_table) {
      const auto *image = FindImage(current, table);
      if (image == nullptr) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      auto next = std::make_shared<RowSet>(*image->As<RowSet>());
      for (const auto &op : list) {
        if (op.add) {
          next->insert(op.row);
        } else if (const auto it = next->find(op.row); it != next->end()) {
          next->erase(it);
        } else {
          core::FinishRebuildAll(outcome); // the state lost a row the database still had
          return outcome;
        }
      }
      outcome.updates.push_back(core::StateImage::Make<RowSet>(id_, table, 0, std::shared_ptr<const RowSet>(next)));
    }
    outcome.action = core::MaintenanceAction::ApplyDelta;
    return outcome;
  }

  bool CheckValidity(core::AuxEngineServices &services, const core::QueryArguments &) const override {
    return !options_.decline &&
           services.StateStore().CheckValidity({id_, {}}, services.CurrentVisibility()).IsValid();
  }

  void Execute(core::AuxEngineServices &services, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    const auto snapshot = services.StateStore().Read(services.CurrentVisibility());
    std::vector<const core::StateImage *> images;
    for (std::size_t t = 0; t < shapes_.size(); ++t) {
      images.push_back(FindImage(snapshot, t));
      if (images.back() == nullptr) {
        ExecuteFallback(services, arguments, sink);
        return;
      }
    }
    core::OutputSchema schema;
    if (options_.tagged) {
      schema.columns.push_back({"tag", core::AuxType::Int64, false});
    }
    for (const int c : Output(shapes_.front())) {
      const auto &column = shapes_.front().columns[c];
      schema.columns.push_back({column.name, column.is_text ? core::AuxType::String : core::AuxType::Int64, column.nullable});
    }
    sink.Begin(schema);
    for (std::size_t t = 0; t < shapes_.size(); ++t) {
      for (const auto &row : *images[t]->As<RowSet>()) {
        core::AuxRow out;
        if (options_.tagged) {
          out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(t)));
        }
        for (const int c : Output(shapes_[t])) {
          out.values.push_back(ToValue(row[c], shapes_[t].columns[c]));
        }
        sink.WriteRow(out);
      }
    }
    sink.End();
  }

private:
  static std::vector<int> Output(const TableShape &shape) {
    if (!shape.output.empty()) {
      return shape.output;
    }
    std::vector<int> all(shape.columns.size());
    for (std::size_t c = 0; c < all.size(); ++c) {
      all[c] = static_cast<int>(c);
    }
    return all;
  }

  static const core::StateImage *FindImage(const core::StateSnapshot &snapshot, std::size_t table) {
    for (const auto &image : snapshot.partitions) {
      if (image.partition == table) {
        return &image;
      }
    }
    return nullptr;
  }

  static core::AuxValue ToValue(const Cell &cell, const CanaryColumn &column) {
    const auto type = column.is_text ? core::AuxType::String : core::AuxType::Int64;
    if (std::holds_alternative<std::monostate>(cell)) {
      return core::AuxValue::Null(type);
    }
    return column.is_text ? core::AuxValue::Of(type, std::get<std::string>(cell))
                          : core::AuxValue::Of(type, std::get<int64_t>(cell));
  }

  // The rows of one batch. Every text view of the batch is taken first and read last: the
  // contract keeps them valid together until the sink returns, so a view into a temporary (an
  // inlined short string copied by the accessor) or into a buffer reused for the next value shows
  // here, and under the address sanitizer as a stack or heap use after scope.
  static void ReadBatch(const ScanBatch &batch, const TableShape &shape, std::vector<Row> &out) {
    const std::size_t n = batch.size();
    const std::size_t width = shape.columns.size();
    std::vector<std::vector<std::string_view>> views(width);
    for (std::size_t c = 0; c < width; ++c) {
      if (!shape.columns[c].is_text) {
        continue;
      }
      for (std::size_t r = 0; r < n; ++r) {
        views[c].push_back(batch.IsNull(r, static_cast<int>(c)) ? std::string_view() : batch.TextView(r, static_cast<int>(c)));
      }
    }
    for (std::size_t r = 0; r < n; ++r) {
      Row row;
      for (std::size_t c = 0; c < width; ++c) {
        const int col = static_cast<int>(c);
        if (batch.IsNull(r, col)) {
          if (!shape.columns[c].nullable) {
            throw std::runtime_error("NULL in non-nullable scan column " + shape.columns[c].name);
          }
          row.emplace_back(std::monostate{});
        } else if (shape.columns[c].is_text) {
          if (views[c][r] != batch.TextView(r, col)) {
            throw std::runtime_error("text view of row " + std::to_string(r) + " of " + shape.table + "." +
                                     shape.columns[c].name + " changed while its batch was alive");
          }
          row.emplace_back(std::string(views[c][r]));
        } else {
          row.emplace_back(batch.Int(r, col));
        }
      }
      out.push_back(std::move(row));
    }
  }

  std::string id_;
  std::vector<TableShape> shapes_;
  ScanFn scan_;
  MirrorOptions options_;
};

} // namespace accelerator_codegen::canary
