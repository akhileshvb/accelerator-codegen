#pragma once
//
// mysql_collation (FoldAscii, EqualsCi, LessCi)
// PURPOSE    ASCII model of MySQL's default case-insensitive collations
//            (utf8mb4_0900_ai_ci): = and ORDER BY ignore ASCII case; the
//            collation is NO PAD, so trailing blanks stay significant. Accent
//            folding of non-ASCII text is not modelled (bytes >= 0x80 compare
//            as bytes). LIKE has its own matcher: sql_like.hpp
//            (case_insensitive = true).
// COMPLEXITY O(length).
// MEMORY     FoldAscii allocates one string; the comparisons do not allocate.
// COW/THREADS Pure functions; thread-safe.
// USE WHEN   an accelerator compares or orders text columns/parameters as the
//            engine's default collation does. Keep the stored spelling for
//            output (MySQL prints the column's own text, not the parameter).
//

#include <cstddef>
#include <string>
#include <string_view>

namespace accelerator_codegen::mysql_collation {

inline char FoldChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string FoldAscii(std::string_view s) {
  std::string out(s);
  for (auto &c : out)
    c = FoldChar(c);
  return out;
}

inline bool EqualsCi(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (FoldChar(a[i]) != FoldChar(b[i]))
      return false;
  return true;
}

// Three-way: <0, 0, >0 by folded bytes (shorter prefix sorts first: NO PAD).
inline int CompareCi(std::string_view a, std::string_view b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char x = static_cast<unsigned char>(FoldChar(a[i]));
    const unsigned char y = static_cast<unsigned char>(FoldChar(b[i]));
    if (x != y)
      return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

inline bool LessCi(std::string_view a, std::string_view b) { return CompareCi(a, b) < 0; }

} // namespace accelerator_codegen::mysql_collation
