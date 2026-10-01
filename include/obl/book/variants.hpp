#pragma once
// The book variants under comparison. Add new combinations here and to
// `for_each_variant`; the benchmark and the differential tests pick them up.
//
// Matrix: every level container with the open-addressing index (isolates the
// level structure), and every index with the dense array (isolates the index).

#include "obl/book/l3_book.hpp"
#include "obl/book/levels_array.hpp"
#include "obl/book/levels_map.hpp"
#include "obl/book/levels_soa.hpp"
#include "obl/book/levels_vector.hpp"

namespace obl::book {

template <Side S>
using LinearVectorLevels = SortedVectorLevels<S, VecSearch::Linear>;
template <Side S>
using BinaryVectorLevels = SortedVectorLevels<S, VecSearch::Binary>;
template <Side S>
using DenseArrayLevels = ArrayLevels<S>;

// Hybrid: small dense window around the best + B-tree for far-away prices
// (std::map when built without abseil), with recentering.
#ifdef OBL_HAVE_ABSEIL
template <Side S>
using FarLevelMap = absl::btree_map<Price, Level, LevelCmp<S>>;
inline constexpr char kHybrid4kName[] = "hybrid(4K window+btree)";
inline constexpr char kHybrid1kName[] = "hybrid(1K window+btree)";
#else
template <Side S>
using FarLevelMap = std::map<Price, Level, LevelCmp<S>>;
inline constexpr char kHybrid4kName[] = "hybrid(4K window+map)";
inline constexpr char kHybrid1kName[] = "hybrid(1K window+map)";
#endif
template <Side S>
using Hybrid4kLevels = ArrayLevels<S, 4096, FarLevelMap<S>, kHybrid4kName>;
template <Side S>
using Hybrid1kLevels = ArrayLevels<S, 1024, FarLevelMap<S>, kHybrid1kName>;

using OA = OpenAddressingOrderIndex;

using MapStdBook = L3Book<MapLevels, StdOrderIndex>;
using MapOpenBook = L3Book<MapLevels, OA>;
using PoolMapOpenBook = L3Book<PoolMapLevels, OA>;
using VecLinearBook = L3Book<LinearVectorLevels, OA>;
using VecBinaryBook = L3Book<BinaryVectorLevels, OA>;
using SoaVecBook = L3Book<SoaVectorLevels, OA>;
using ArrayStdBook = L3Book<DenseArrayLevels, StdOrderIndex>;
using ArrayOpenBook = L3Book<DenseArrayLevels, OA>;
using ArrayCompactBook = L3Book<DenseArrayLevels, CompactOrderIndex>;
using VecLinearCompactBook = L3Book<LinearVectorLevels, CompactOrderIndex>;
using Hybrid4kOpenBook = L3Book<Hybrid4kLevels, OA>;
using Hybrid1kOpenBook = L3Book<Hybrid1kLevels, OA>;
// per-level vector queues instead of the intrusive list
using ArrayOpenVqBook = L3Book<DenseArrayLevels, OA, VectorQueues>;
using ArrayCompactVqBook = L3Book<DenseArrayLevels, CompactOrderIndex, VectorQueues>;
using VecLinearCompactVqBook = L3Book<LinearVectorLevels, CompactOrderIndex, VectorQueues>;
#ifdef OBL_HAVE_ABSEIL
using BTreeOpenBook = L3Book<BTreeLevels, OA>;
using ArrayAbslBook = L3Book<DenseArrayLevels, AbslOrderIndex>;
#endif
#ifdef OBL_HAVE_UNORDERED_DENSE
using ArrayDenseBook = L3Book<DenseArrayLevels, DenseOrderIndex>;
#endif

// Calls f.template operator()<Book>() for every variant.
template <class F>
void for_each_variant(F&& f) {
  // level containers (with open addressing)
  f.template operator()<MapStdBook>();
  f.template operator()<MapOpenBook>();
  f.template operator()<PoolMapOpenBook>();
#ifdef OBL_HAVE_ABSEIL
  f.template operator()<BTreeOpenBook>();
#endif
  f.template operator()<VecLinearBook>();
  f.template operator()<VecBinaryBook>();
  f.template operator()<SoaVecBook>();
  f.template operator()<ArrayOpenBook>();
  f.template operator()<Hybrid4kOpenBook>();
  f.template operator()<Hybrid1kOpenBook>();
  // order indexes (with the dense array)
  f.template operator()<ArrayStdBook>();
  f.template operator()<ArrayCompactBook>();
  f.template operator()<VecLinearCompactBook>();
  // FIFO queue policy
  f.template operator()<ArrayOpenVqBook>();
  f.template operator()<ArrayCompactVqBook>();
  f.template operator()<VecLinearCompactVqBook>();
#ifdef OBL_HAVE_UNORDERED_DENSE
  f.template operator()<ArrayDenseBook>();
#endif
#ifdef OBL_HAVE_ABSEIL
  f.template operator()<ArrayAbslBook>();
#endif
}

}  // namespace obl::book
