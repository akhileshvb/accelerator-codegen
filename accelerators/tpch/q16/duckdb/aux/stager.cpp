#include "tpch/q16/duckdb/aux/stager.hpp"

#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace accelerator_codegen::aux_state::query {

using namespace q16_pages;

namespace {

// Pages per slice when copying them for writing.
constexpr std::size_t kCopyPageGrain = 4;
// Events per slice of the group-event sort.
constexpr std::size_t kEventGrain = 16384;
// Size-list changes below which the lists are re-sorted on one thread.
constexpr std::size_t kListChangeOpThreshold = 1024;

} // namespace

bool TpchQ16Stager::Open() {
  meta_pub = Fetch<TpchQ16Meta>(Layout::kMeta);
  return meta_pub != nullptr;
}

void TpchQ16Stager::Apply(const TpchQ16Op &op) {
  switch (op.kind) {
  case TpchQ16Op::Kind::PartInsert:
    PartInsert(op);
    break;
  case TpchQ16Op::Kind::PartDelete:
    PartDelete(op.key);
    break;
  case TpchQ16Op::Kind::PartsuppInsert:
    PartsuppInsert(op.key, op.key2);
    break;
  case TpchQ16Op::Kind::PartsuppDelete:
    PartsuppDelete(op.key, op.key2);
    break;
  case TpchQ16Op::Kind::SupplierSet:
    SupplierSet(op.key, op.eligible);
    break;
  }
}

bool TpchQ16Stager::ApplyBatch(const std::vector<TpchQ16Op> &ops) {
  batching = true;
  for (const auto &op : ops)
    Apply(op);
  batching = false;
  AddFlipEvents();
  std::vector<Membership> changes;
  return ApplyGroupEvents(changes) && ApplySupplierMemberships(std::move(changes));
}

void TpchQ16Stager::Finish(std::vector<core::StateImage> &out) {
  ResortLists();
  if (meta_mut)
    out.push_back(MakeImage<TpchQ16Meta>(
        Layout::kMeta, std::shared_ptr<const TpchQ16Meta>(std::move(meta_mut))));
  Collect(lists, out);
  Collect(groups, out);
  Collect(parts, out);
  Collect(ps, out);
  Collect(supp, out);
}

template <class T>
std::shared_ptr<const T> TpchQ16Stager::Fetch(core::PartitionId id) {
  const auto snapshot = services.StateStore().ReadPartitions(visibility, {id});
  for (const auto &image : snapshot.partitions)
    if (image.partition == id)
      return std::static_pointer_cast<const T>(image.payload);
  return nullptr;
}

template <class T>
typename TpchQ16Stager::PageSet<T>::Entry &TpchQ16Stager::EntryOf(PageSet<T> &set,
                                                                  core::PartitionId id) {
  auto it = set.entries.find(id);
  if (it == set.entries.end()) {
    typename PageSet<T>::Entry entry;
    entry.published = Fetch<T>(id);
    it = set.entries.emplace(id, std::move(entry)).first;
  }
  return it->second;
}

template <class T> const T *TpchQ16Stager::Peek(PageSet<T> &set, core::PartitionId id) {
  auto &entry = EntryOf(set, id);
  return entry.mutated ? entry.mutated.get() : entry.published.get();
}

template <class T, class Init>
T &TpchQ16Stager::Mut(PageSet<T> &set, core::PartitionId id, Init init) {
  auto &entry = EntryOf(set, id);
  if (!entry.mutated)
    entry.mutated = entry.published ? std::make_shared<T>(*entry.published)
                                    : init();
  return *entry.mutated;
}

template <class T>
void TpchQ16Stager::Collect(PageSet<T> &set, std::vector<core::StateImage> &out) {
  for (auto &[id, entry] : set.entries)
    if (entry.mutated)
      out.push_back(MakeImage<T>(
          id, std::shared_ptr<const T>(std::move(entry.mutated))));
}

TpchQ16Meta &TpchQ16Stager::MutableMeta() {
  if (!meta_mut)
    meta_mut = std::make_shared<TpchQ16Meta>(*meta_pub);
  return *meta_mut;
}

uint16_t TpchQ16Stager::Brand(const std::string &name) {
  const auto &m = Meta();
  const auto it = m.brand_index.find(name);
  if (it != m.brand_index.end())
    return it->second;
  auto &mm = MutableMeta();
  const auto id = InternName(mm.brands, mm.brand_index, name, Layout::kBrandCap);
  RecomputeRanks(mm);
  return id;
}

uint16_t TpchQ16Stager::Type(const std::string &name) {
  const auto &m = Meta();
  const auto it = m.type_index.find(name);
  if (it != m.type_index.end())
    return it->second;
  auto &mm = MutableMeta();
  const auto id = InternName(mm.types, mm.type_index, name, kTypeCap);
  RecomputeRanks(mm);
  return id;
}

TpchQ16PartInfo TpchQ16Stager::InfoOf(int64_t partkey) {
  const auto *page = Peek(parts, Layout::PartPartition(partkey));
  if (!page)
    return {};
  return page->info[static_cast<std::size_t>(partkey) & (kPartPageSize - 1)];
}

void TpchQ16Stager::SetInfo(int64_t partkey, const TpchQ16PartInfo &info) {
  auto &page = Mut<TpchQ16PartPage>(parts, Layout::PartPartition(partkey), [] {
    auto p = std::make_shared<TpchQ16PartPage>();
    p->Init();
    return p;
  });
  page.info[static_cast<std::size_t>(partkey) & (kPartPageSize - 1)] = info;
}

bool TpchQ16Stager::Eligible(int64_t suppkey) {
  const auto *page = Peek(supp, Layout::SuppPartition(suppkey));
  if (!page)
    return true;
  return page->eligible[static_cast<std::size_t>(suppkey) &
                        (kSuppPageSize - 1)] != 0;
}

TpchQ16SupplierPage &TpchQ16Stager::MutSupp(int64_t suppkey) {
  return Mut<TpchQ16SupplierPage>(supp, Layout::SuppPartition(suppkey), [] {
    auto p = std::make_shared<TpchQ16SupplierPage>();
    p->Init();
    return p;
  });
}

TpchQ16PartsuppPage &TpchQ16Stager::MutPs(int64_t partkey) {
  return Mut<TpchQ16PartsuppPage>(ps, Layout::PsPartition(partkey), [] {
    auto p = std::make_shared<TpchQ16PartsuppPage>();
    p->Init();
    return p;
  });
}

const TpchQ16GroupMembers *TpchQ16Stager::PeekGroup(uint32_t code) {
  const auto *page = Peek(
      groups, Layout::GroupPartition(CodeSize(code), CodeBrand(code)));
  if (!page)
    return nullptr;
  const auto type = CodeType(code);
  const auto it = std::lower_bound(
      page->groups.begin(), page->groups.end(), type,
      [](const TpchQ16GroupMembers &g, uint16_t t) { return g.type_id < t; });
  return it != page->groups.end() && it->type_id == type ? &*it : nullptr;
}

TpchQ16GroupMembers *TpchQ16Stager::FindGroup(uint32_t code, bool create) {
  if (!create && !PeekGroup(code))
    return nullptr;
  auto &page = Mut<TpchQ16GroupPage>(
      groups, Layout::GroupPartition(CodeSize(code), CodeBrand(code)),
      [] { return std::make_shared<TpchQ16GroupPage>(); });
  const auto type = CodeType(code);
  auto it = std::lower_bound(
      page.groups.begin(), page.groups.end(), type,
      [](const TpchQ16GroupMembers &g, uint16_t t) { return g.type_id < t; });
  if (it == page.groups.end() || it->type_id != type) {
    TpchQ16GroupMembers fresh;
    fresh.type_id = type;
    it = page.groups.insert(it, std::move(fresh));
  }
  return &*it;
}

void TpchQ16Stager::Touch(uint32_t code, const TpchQ16GroupMembers &gm) {
  touched.emplace(code, gm.eligible_count);
}

void TpchQ16Stager::GroupAdd(uint32_t code, int64_t suppkey, int delta) {
  if (batching) {
    events.push_back({code, suppkey, delta});
    return;
  }
  auto *gm = FindGroup(code, delta > 0);
  if (!gm)
    return;
  const auto pos =
      std::lower_bound(gm->suppkeys.begin(), gm->suppkeys.end(), suppkey);
  const auto idx = static_cast<std::ptrdiff_t>(pos - gm->suppkeys.begin());
  const bool found = pos != gm->suppkeys.end() && *pos == suppkey;
  if (delta > 0) {
    Touch(code, *gm);
    if (found) {
      ++gm->ref_counts[static_cast<std::size_t>(idx)];
    } else {
      gm->suppkeys.insert(pos, suppkey);
      gm->ref_counts.insert(gm->ref_counts.begin() + idx, 1);
      auto &sp = MutSupp(suppkey);
      sp.groups.Insert(static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1),
                       code);
      if (Eligible(suppkey))
        ++gm->eligible_count;
    }
  } else {
    if (!found)
      return;
    Touch(code, *gm);
    if (--gm->ref_counts[static_cast<std::size_t>(idx)] == 0) {
      gm->suppkeys.erase(gm->suppkeys.begin() + idx);
      gm->ref_counts.erase(gm->ref_counts.begin() + idx);
      auto &sp = MutSupp(suppkey);
      sp.groups.Erase(static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1),
                      code);
      if (Eligible(suppkey))
        --gm->eligible_count;
    }
  }
}

bool TpchQ16Stager::InGroup(const TpchQ16PartInfo &info) {
  return info.brand_id != kNoBrand && SizeInRange(info.size);
}

uint32_t TpchQ16Stager::CodeOf(const TpchQ16PartInfo &info) {
  return GroupCode(info.size, info.brand_id, info.type_id);
}

void TpchQ16Stager::RemovePart(int64_t partkey, const TpchQ16PartInfo &info) {
  if (InGroup(info)) {
    if (const auto *page = Peek(ps, Layout::PsPartition(partkey))) {
      const auto row = static_cast<std::size_t>(partkey) & (kPsPageSize - 1);
      const auto code = CodeOf(info);
      for (const auto *p = page->rows.Begin(row); p != page->rows.End(row); ++p)
        GroupAdd(code, *p, -1);
    }
  }
  SetInfo(partkey, TpchQ16PartInfo{});
}

void TpchQ16Stager::PartInsert(const TpchQ16Op &op) {
  CheckKey(op.key, "partkey");
  const auto current = InfoOf(op.key);
  if (current.brand_id != kNoBrand)
    RemovePart(op.key, current);
  TpchQ16PartInfo info;
  info.brand_id = Brand(op.brand);
  info.type_id = Type(op.type);
  info.size = op.size;
  SetInfo(op.key, info);
  if (!InGroup(info))
    return;
  if (const auto *page = Peek(ps, Layout::PsPartition(op.key))) {
    const auto row = static_cast<std::size_t>(op.key) & (kPsPageSize - 1);
    const auto code = CodeOf(info);
    for (const auto *p = page->rows.Begin(row); p != page->rows.End(row); ++p)
      GroupAdd(code, *p, +1);
  }
}

void TpchQ16Stager::PartDelete(int64_t partkey) {
  if (!Layout::KeyInRange(partkey))
    return;
  const auto current = InfoOf(partkey);
  if (current.brand_id == kNoBrand)
    return;
  RemovePart(partkey, current);
}

void TpchQ16Stager::PartsuppInsert(int64_t partkey, int64_t suppkey) {
  CheckKey(partkey, "partkey");
  CheckKey(suppkey, "suppkey");
  MutPs(partkey).rows.Insert(
      static_cast<std::size_t>(partkey) & (kPsPageSize - 1), suppkey);
  const auto info = InfoOf(partkey);
  if (InGroup(info))
    GroupAdd(CodeOf(info), suppkey, +1);
}

void TpchQ16Stager::PartsuppDelete(int64_t partkey, int64_t suppkey) {
  if (!Layout::KeyInRange(partkey) || !Layout::KeyInRange(suppkey))
    return;
  const auto *page = Peek(ps, Layout::PsPartition(partkey));
  if (!page)
    return;
  const auto row = static_cast<std::size_t>(partkey) & (kPsPageSize - 1);
  if (std::find(page->rows.Begin(row), page->rows.End(row), suppkey) ==
      page->rows.End(row))
    return;
  MutPs(partkey).rows.Erase(row, suppkey);
  const auto info = InfoOf(partkey);
  if (InGroup(info))
    GroupAdd(CodeOf(info), suppkey, -1);
}

void TpchQ16Stager::SupplierSet(int64_t suppkey, bool eligible) {
  CheckKey(suppkey, "suppkey");
  if (Eligible(suppkey) == eligible)
    return;
  auto &page = MutSupp(suppkey);
  const auto row = static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1);
  page.eligible[row] = eligible ? 1 : 0;
  if (batching) {
    flipped.push_back(suppkey);
    return;
  }
  for (const auto *p = page.groups.Begin(row); p != page.groups.End(row); ++p) {
    auto *gm = FindGroup(*p, false);
    if (!gm)
      continue;
    Touch(*p, *gm);
    gm->eligible_count += eligible ? 1 : -1;
  }
}

template <class T, class Init>
void TpchQ16Stager::MutAll(PageSet<T> &set, const std::vector<core::PartitionId> &ids, Init init) {
  std::vector<typename PageSet<T>::Entry *> entries;
  for (const auto id : ids)
    entries.push_back(&EntryOf(set, id));
  core::ParallelEach(services, entries.size(), kCopyPageGrain, [&](std::size_t i) {
    auto &entry = *entries[i];
    if (!entry.mutated)
      entry.mutated = entry.published ? std::make_shared<T>(*entry.published) : init();
  });
}

template <class T> T &TpchQ16Stager::OpenedPage(PageSet<T> &set, core::PartitionId id) {
  return *set.entries.at(id).mutated;
}

bool TpchQ16Stager::EligibleIn(const std::shared_ptr<const TpchQ16SupplierPage> &page,
                int64_t suppkey) const {
  return !page || page->eligible[static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1)] != 0;
}

bool TpchQ16Stager::EligiblePublished(int64_t suppkey) const {
  return EligibleIn(supp.entries.at(Layout::SuppPartition(suppkey)).published, suppkey);
}

bool TpchQ16Stager::EligibleNow(int64_t suppkey) const {
  const auto &entry = supp.entries.at(Layout::SuppPartition(suppkey));
  return entry.mutated ? entry.mutated->eligible[static_cast<std::size_t>(suppkey) &
                                                 (kSuppPageSize - 1)] != 0
                       : EligibleIn(entry.published, suppkey);
}

void TpchQ16Stager::AddFlipEvents() {
  std::sort(flipped.begin(), flipped.end());
  flipped.erase(std::unique(flipped.begin(), flipped.end()), flipped.end());
  for (const auto suppkey : flipped) {
    if (EligiblePublished(suppkey) == EligibleNow(suppkey))
      continue;
    const auto &published = supp.entries.at(Layout::SuppPartition(suppkey)).published;
    if (!published)
      continue;
    const auto row = static_cast<std::size_t>(suppkey) & (kSuppPageSize - 1);
    for (const auto *code = published->groups.Begin(row); code != published->groups.End(row);
         ++code)
      events.push_back({*code, suppkey, 0});
  }
}

bool TpchQ16Stager::MergeGroup(TpchQ16GroupMembers &gm, uint32_t code, const MemberEvent *first,
                const MemberEvent *last, std::vector<Membership> &changes) const {
  std::vector<int64_t> keys;
  std::vector<int32_t> refs;
  keys.reserve(gm.suppkeys.size() + static_cast<std::size_t>(last - first));
  refs.reserve(keys.capacity());
  const auto keep_until = [&](std::size_t &at, int64_t bound) {
    for (; at < gm.suppkeys.size() && gm.suppkeys[at] < bound; ++at) {
      keys.push_back(gm.suppkeys[at]);
      refs.push_back(gm.ref_counts[at]);
    }
  };
  std::size_t at = 0;
  int32_t eligible = gm.eligible_count;
  for (const auto *event = first; event != last; ++event) {
    keep_until(at, event->suppkey);
    int32_t before = 0;
    if (at < gm.suppkeys.size() && gm.suppkeys[at] == event->suppkey)
      before = gm.ref_counts[at++];
    const int32_t after = before + event->delta;
    if (after < 0)
      return false;
    eligible += (after > 0 && EligibleNow(event->suppkey)) -
                (before > 0 && EligiblePublished(event->suppkey));
    if (after > 0) {
      keys.push_back(event->suppkey);
      refs.push_back(after);
    }
    if ((before == 0) != (after == 0))
      changes.push_back({event->suppkey, code, static_cast<int8_t>(after > 0 ? 1 : -1)});
  }
  keep_until(at, std::numeric_limits<int64_t>::max());
  gm.suppkeys = std::move(keys);
  gm.ref_counts = std::move(refs);
  gm.eligible_count = eligible;
  return true;
}

bool TpchQ16Stager::ApplyGroupEvents(std::vector<Membership> &changes) {
  core::ParallelStableSort(
      services, events,
      [](const MemberEvent &a, const MemberEvent &b) {
        return a.code != b.code ? a.code < b.code : a.suppkey < b.suppkey;
      },
      kEventGrain);
  std::size_t net = 0; // sum equal (code, supplier) events in place
  for (const auto &event : events) {
    if (net && events[net - 1].code == event.code && events[net - 1].suppkey == event.suppkey)
      events[net - 1].delta += event.delta;
    else
      events[net++] = event;
  }
  events.resize(net);

  struct PageEvents {
    core::PartitionId page;
    std::size_t begin, end;
    bool consistent = true;
    std::vector<Membership> changes;
    std::vector<std::pair<uint32_t, int32_t>> touched_before;
  };
  const auto page_of = [](uint32_t code) {
    return Layout::GroupPartition(CodeSize(code), CodeBrand(code));
  };
  std::vector<PageEvents> pages;
  for (std::size_t i = 0; i < events.size(); ++i)
    if (pages.empty() || pages.back().page != page_of(events[i].code))
      pages.push_back({page_of(events[i].code), i, i + 1});
    else
      pages.back().end = i + 1;

  std::vector<core::PartitionId> group_ids, supplier_ids;
  for (const auto &page : pages)
    group_ids.push_back(page.page);
  for (const auto &event : events)
    supplier_ids.push_back(Layout::SuppPartition(event.suppkey));
  supplier_ids.erase(std::unique(supplier_ids.begin(), supplier_ids.end()), supplier_ids.end());
  for (const auto id : supplier_ids)
    EntryOf(supp, id);
  MutAll(groups, group_ids, [] { return std::make_shared<TpchQ16GroupPage>(); });

  core::ParallelEach(services, pages.size(), 1, [&](std::size_t i) {
    auto &work = pages[i];
    auto &page = OpenedPage(groups, work.page);
    for (std::size_t first = work.begin; first < work.end;) {
      const uint32_t code = events[first].code;
      std::size_t last = first;
      while (last < work.end && events[last].code == code)
        ++last;
      auto it = std::lower_bound(
          page.groups.begin(), page.groups.end(), CodeType(code),
          [](const TpchQ16GroupMembers &g, uint16_t t) { return g.type_id < t; });
      const bool exists = it != page.groups.end() && it->type_id == CodeType(code);
      TpchQ16GroupMembers fresh;
      fresh.type_id = CodeType(code);
      auto &gm = exists ? *it : fresh;
      work.touched_before.emplace_back(code, gm.eligible_count);
      if (!MergeGroup(gm, code, &events[first], &events[last], work.changes)) {
        work.consistent = false;
        return;
      }
      if (!exists && !gm.suppkeys.empty())
        page.groups.insert(it, std::move(fresh));
      first = last;
    }
  });
  for (auto &work : pages) {
    if (!work.consistent)
      return false;
    changes.insert(changes.end(), work.changes.begin(), work.changes.end());
    for (const auto &[code, before] : work.touched_before)
      touched.emplace(code, before);
  }
  events.clear();
  return true;
}

bool TpchQ16Stager::ApplySupplierMemberships(std::vector<Membership> changes) {
  std::sort(changes.begin(), changes.end(), [](const Membership &a, const Membership &b) {
    return a.suppkey != b.suppkey ? a.suppkey < b.suppkey : a.code < b.code;
  });
  struct PageChanges {
    core::PartitionId page;
    std::size_t begin, end;
  };
  std::vector<PageChanges> pages;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const auto page = Layout::SuppPartition(changes[i].suppkey);
    if (pages.empty() || pages.back().page != page)
      pages.push_back({page, i, i + 1});
    else
      pages.back().end = i + 1;
  }
  std::vector<core::PartitionId> ids;
  for (const auto &page : pages)
    ids.push_back(page.page);
  MutAll(supp, ids, [] {
    auto page = std::make_shared<TpchQ16SupplierPage>();
    page->Init();
    return page;
  });
  core::ParallelEach(services, pages.size(), 1, [&](std::size_t i) {
    auto &page = OpenedPage(supp, pages[i].page);
    for (std::size_t c = pages[i].begin; c < pages[i].end; ++c) {
      const auto row = static_cast<std::size_t>(changes[c].suppkey) & (kSuppPageSize - 1);
      if (changes[c].delta > 0)
        page.groups.Insert(row, changes[c].code);
      else
        page.groups.Erase(row, changes[c].code);
    }
  });
  return true;
}

void TpchQ16Stager::ResortLists() {
  struct Change {
    uint32_t code;
    int32_t before, after;
  };
  std::map<int32_t, std::vector<Change>> by_size;
  std::size_t change_count = 0;
  for (const auto &[code, before] : touched) {
    const auto *gm = PeekGroup(code);
    const int32_t after = gm ? gm->eligible_count : 0;
    if (after == before)
      continue;
    by_size[CodeSize(code)].push_back({code, before, after});
    ++change_count;
  }
  std::vector<core::PartitionId> ids;
  for (const auto &entry : by_size)
    ids.push_back(Layout::SizePartition(entry.first));
  MutAll(lists, ids, [] { return std::make_shared<TpchQ16SizeList>(); });
  std::vector<std::pair<int32_t, const std::vector<Change> *>> sizes;
  for (const auto &entry : by_size)
    sizes.emplace_back(entry.first, &entry.second);
  const RowLess less{Meta()};
  core::ParallelEachWhen(
      services, sizes.size(), change_count, kListChangeOpThreshold,
      [&](std::size_t i) {
        const int32_t size = sizes[i].first;
        auto &list = OpenedPage(lists, Layout::SizePartition(size));
        for (const auto &change : *sizes[i].second) {
          const auto brand = static_cast<uint16_t>(CodeBrand(change.code));
          const auto type = CodeType(change.code);
          if (change.before > 0) {
            const Q16PrecomputedRow old_row{brand, type, size, change.before};
            auto it = std::lower_bound(list.rows.begin(), list.rows.end(), old_row, less);
            if (it == list.rows.end() || it->brand_id != brand || it->type_id != type) {
              it = std::find_if(list.rows.begin(), list.rows.end(),
                                [&](const Q16PrecomputedRow &r) {
                                  return r.brand_id == brand && r.type_id == type;
                                });
            }
            if (it != list.rows.end())
              list.rows.erase(it);
          }
          if (change.after > 0) {
            const Q16PrecomputedRow new_row{brand, type, size, change.after};
            list.rows.insert(
                std::lower_bound(list.rows.begin(), list.rows.end(), new_row, less), new_row);
          }
        }
      });
}

} // namespace accelerator_codegen::aux_state::query
