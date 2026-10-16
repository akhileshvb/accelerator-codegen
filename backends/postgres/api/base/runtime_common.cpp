// Base PostgreSQL accelerators: PostgreSQL-free parts of the runtime
// (kernel/task registries, ParallelFor, date helpers).
#include "backends/postgres/api/base/pg_base.hpp"
#include "runtime_internal.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace accelerator_codegen::pg_base {

// Registries are filled by static registrars at library load (immutable after).
std::deque<KernelEntry> &KernelRegistry() {
	static std::deque<KernelEntry> entries;
	return entries;
}

std::deque<std::pair<const char *, TaskFn>> &TaskRegistry() {
	static std::deque<std::pair<const char *, TaskFn>> entries;
	return entries;
}

void RegisterKernel(const KernelEntry &entry) {
	KernelRegistry().push_back(entry);
}

void RegisterTask(const char *name, TaskFn fn) {
	TaskRegistry().emplace_back(name, fn);
}

const KernelEntry *FindKernel(int query_id, bool parallel) {
	for (const auto &e : KernelRegistry()) {
		if (e.query_id == query_id && e.parallel == parallel) {
			return &e;
		}
	}
	return nullptr;
}

TaskFn FindTask(const char *name) {
	for (const auto &e : TaskRegistry()) {
		if (std::strcmp(e.first, name) == 0) {
			return e.second;
		}
	}
	return nullptr;
}

void ParallelFor(std::size_t tasks, int threads, const std::function<void(std::size_t task, int thread)> &fn) {
	if (threads < 1) {
		threads = 1;
	}
	if (static_cast<std::size_t>(threads) > tasks) {
		threads = static_cast<int>(tasks == 0 ? 1 : tasks);
	}
	std::atomic<std::size_t> next {0};
	std::exception_ptr error;
	std::mutex error_lock;
	auto body = [&](int thread) {
		try {
			for (;;) {
				const auto t = next.fetch_add(1, std::memory_order_relaxed);
				if (t >= tasks) {
					return;
				}
				fn(t, thread);
			}
		} catch (...) {
			std::lock_guard<std::mutex> guard(error_lock);
			if (!error) {
				error = std::current_exception();
			}
			next.store(tasks, std::memory_order_relaxed);
		}
	};
	std::vector<std::thread> pool;
	pool.reserve(static_cast<std::size_t>(threads - 1));
	for (int i = 1; i < threads; i++) {
		pool.emplace_back(body, i);
	}
	body(0);
	for (auto &t : pool) {
		t.join();
	}
	if (error) {
		std::rethrow_exception(error);
	}
}

// Howard Hinnant's civil-from-days / days-from-civil.
int32_t DaysFromCivil(int y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int>(doe) - 719468;
}

void CivilFromDays(int32_t z, int &y, unsigned &m, unsigned &d) {
	z += 719468;
	const int era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int yy = static_cast<int>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y = yy + (m <= 2);
}

std::string EncodeBatch(const ScanSpec &spec, const Batch &batch) {
	ByteWriter w;
	w.Put<uint32_t>(static_cast<uint32_t>(batch.count));
	for (std::size_t c = 0; c < spec.columns.size(); c++) {
		switch (spec.columns[c].type) {
		case ColType::Int32:
		case ColType::Date:
			w.buf.append(static_cast<const char *>(batch.cols[c]), batch.count * sizeof(int32_t));
			break;
		case ColType::Int64:
		case ColType::Decimal:
		case ColType::Double:
			w.buf.append(static_cast<const char *>(batch.cols[c]), batch.count * sizeof(int64_t));
			break;
		case ColType::Text: {
			const StringRef *strings = batch.Str(static_cast<int>(c));
			for (std::size_t r = 0; r < batch.count; r++) {
				w.Put<uint32_t>(strings[r].len);
			}
			for (std::size_t r = 0; r < batch.count; r++) {
				w.buf.append(strings[r].ptr, strings[r].len);
			}
			break;
		}
		}
		if (spec.columns[c].nullable) {
			w.buf.append(reinterpret_cast<const char *>(batch.nulls[c]), batch.count);
		}
	}
	return w.Take();
}

BatchDecoder::BatchDecoder(const ScanSpec &spec_p) : spec(spec_p), words(spec_p.columns.size()), refs(spec_p.columns.size()), masks(spec_p.columns.size()) {
}

const Batch &BatchDecoder::Decode(std::string_view message) {
	const char *pos = message.data();
	const char *end = pos + message.size();
	const auto need = [&](std::size_t n) {
		if (static_cast<std::size_t>(end - pos) < n) {
			throw std::runtime_error("base accelerator: truncated batch message");
		}
	};
	need(sizeof(uint32_t));
	uint32_t count;
	std::memcpy(&count, pos, sizeof(count));
	pos += sizeof(count);
	batch.count = count;
	for (std::size_t c = 0; c < spec.columns.size(); c++) {
		if (spec.columns[c].type == ColType::Text) {
			need(count * sizeof(uint32_t));
			const char *lens = pos;
			pos += count * sizeof(uint32_t);
			refs[c].resize(count);
			for (std::size_t row = 0; row < count; row++) {
				uint32_t len;
				std::memcpy(&len, lens + row * sizeof(uint32_t), sizeof(len));
				need(len);
				refs[c][row] = StringRef {pos, len, 0};
				pos += len;
			}
			batch.cols[c] = refs[c].data();
		} else {
			const std::size_t width =
			    (spec.columns[c].type == ColType::Int32 || spec.columns[c].type == ColType::Date) ? 4 : 8;
			need(count * width);
			words[c].resize((count * width + 7) / 8);
			std::memcpy(words[c].data(), pos, count * width);
			pos += count * width;
			batch.cols[c] = words[c].data();
		}
		if (spec.columns[c].nullable) {
			need(count);
			masks[c].assign(pos, pos + count);
			pos += count;
			batch.nulls[c] = masks[c].data();
		}
	}
	return batch;
}

} // namespace accelerator_codegen::pg_base
