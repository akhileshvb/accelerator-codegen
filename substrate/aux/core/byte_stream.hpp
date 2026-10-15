// SCAFFOLDING
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Little-endian helpers for query-owned aux-state partition blobs at a
// serialized process/SHM boundary (and round-trip tests). Wire layout is per
// query; only the primitives are shared here.
class ByteWriter {
public:
	std::vector<uint8_t> bytes;
	// Set by WriteImageHeader: string lengths written after it are 64-bit.
	bool wide_lengths = false;

	void U8(uint8_t value) {
		bytes.push_back(value);
	}

	void U16(uint16_t value) {
		bytes.push_back(static_cast<uint8_t>(value));
		bytes.push_back(static_cast<uint8_t>(value >> 8));
	}

	void U32(uint32_t value) {
		for (int shift = 0; shift < 32; shift += 8) {
			bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
		}
	}

	void U64(uint64_t value) {
		for (int shift = 0; shift < 64; shift += 8) {
			bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
		}
	}

	void I32(int32_t value) {
		U32(static_cast<uint32_t>(value));
	}

	void I64(int64_t value) {
		U64(static_cast<uint64_t>(value));
	}

	void Double(double value) {
		uint64_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value));
		std::memcpy(&bits, &value, sizeof(bits));
		U64(bits);
	}

	void String(const std::string &value) {
		if (wide_lengths) {
			U64(static_cast<uint64_t>(value.size()));
		} else {
			U32(static_cast<uint32_t>(value.size()));
		}
		bytes.insert(bytes.end(), value.begin(), value.end());
	}
};

class ByteReader {
public:
	explicit ByteReader(const std::vector<uint8_t> &bytes_p) : bytes(bytes_p) {
	}

	const std::vector<uint8_t> &bytes;
	std::size_t offset = 0;
	// Set by ReadImageHeader from the header's format: string lengths are
	// 64-bit (wide header) or 32-bit (legacy header, no wide flag).
	bool wide_lengths = false;

	bool CanRead(std::size_t count) const {
		return offset <= bytes.size() && count <= bytes.size() - offset;
	}

	bool U8(uint8_t &value) {
		if (!CanRead(1)) {
			return false;
		}
		value = bytes[offset++];
		return true;
	}

	bool U16(uint16_t &value) {
		if (!CanRead(2)) {
			return false;
		}
		value = static_cast<uint16_t>(bytes[offset]) |
		        (static_cast<uint16_t>(bytes[offset + 1]) << 8);
		offset += 2;
		return true;
	}

	bool U32(uint32_t &value) {
		if (!CanRead(4)) {
			return false;
		}
		value = static_cast<uint32_t>(bytes[offset]) |
		        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
		        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
		        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
		offset += 4;
		return true;
	}

	bool U64(uint64_t &value) {
		if (!CanRead(8)) {
			return false;
		}
		value = 0;
		for (int shift = 0; shift < 64; shift += 8) {
			value |= static_cast<uint64_t>(bytes[offset++]) << shift;
		}
		return true;
	}

	bool I32(int32_t &value) {
		uint32_t bits = 0;
		if (!U32(bits)) {
			return false;
		}
		value = static_cast<int32_t>(bits);
		return true;
	}

	bool I64(int64_t &value) {
		uint64_t bits = 0;
		if (!U64(bits)) {
			return false;
		}
		value = static_cast<int64_t>(bits);
		return true;
	}

	bool Double(double &value) {
		uint64_t bits = 0;
		if (!U64(bits)) {
			return false;
		}
		std::memcpy(&value, &bits, sizeof(value));
		return true;
	}

	bool String(std::string &value) {
		uint64_t length = 0;
		if (wide_lengths) {
			if (!U64(length)) {
				return false;
			}
		} else {
			uint32_t narrow = 0;
			if (!U32(narrow)) {
				return false;
			}
			length = narrow;
		}
		if (!CanRead(static_cast<std::size_t>(length)) || length > bytes.size()) {
			return false;
		}
		value.assign(reinterpret_cast<const char *>(bytes.data() + offset), static_cast<std::size_t>(length));
		offset += length;
		return true;
	}

	bool FullyConsumed() const {
		return offset == bytes.size();
	}
};

// Image header: u32 magic, u32 version, count, where a wide header sets
// kWideImageFlag in the version word, stores a u64 count and switches string
// lengths to u64. Writers emit the wide form; readers also accept the legacy
// (u32 count, u32 string length) form. Images are never persisted: they live in
// process memory or in shared memory of one build, so the legacy path is only a
// safety net.
constexpr uint32_t kWideImageFlag = 0x80000000u;

inline void WriteImageHeader(ByteWriter &writer, uint32_t magic, uint32_t version, uint64_t count) {
	writer.U32(magic);
	writer.U32(version | kWideImageFlag);
	writer.U64(count);
	writer.wide_lengths = true;
}

inline bool ReadImageHeader(ByteReader &reader, uint32_t magic, uint32_t expected_version,
                          uint64_t &count) {
	uint32_t actual_magic = 0;
	uint32_t version = 0;
	if (!reader.U32(actual_magic) || !reader.U32(version) || actual_magic != magic) {
		return false;
	}
	const bool wide = (version & kWideImageFlag) != 0;
	version &= ~kWideImageFlag;
	if (version != expected_version) {
		return false;
	}
	if (wide) {
		if (!reader.U64(count)) {
			return false;
		}
	} else {
		uint32_t narrow = 0;
		if (!reader.U32(narrow)) {
			return false;
		}
		count = narrow;
	}
	reader.wide_lengths = wide;
	return true;
}

} // namespace accelerator_codegen::aux_state::core
