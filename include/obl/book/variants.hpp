#pragma once
// The book variants under comparison. Add new combinations here and to
// `for_each_variant`; the benchmark and the differential tests pick them up.

#include "obl/book/l3_book.hpp"
#include "obl/book/levels_array.hpp"
#include "obl/book/levels_map.hpp"
#include "obl/book/levels_vector.hpp"

namespace obl::book {

template <Side S>
using LinearVectorLevels = SortedVectorLevels<S, VecSearch::Linear>;
template <Side S>
using BinaryVectorLevels = SortedVectorLevels<S, VecSearch::Binary>;
template <Side S>
using DenseArrayLevels = ArrayLevels<S>;

using MapStdBook = L3Book<MapLevels, StdOrderIndex>;
using MapOpenBook = L3Book<MapLevels, OpenAddressingOrderIndex>;
using VecLinearBook = L3Book<LinearVectorLevels, OpenAddressingOrderIndex>;
using VecBinaryBook = L3Book<BinaryVectorLevels, OpenAddressingOrderIndex>;
using ArrayStdBook = L3Book<DenseArrayLevels, StdOrderIndex>;
using ArrayOpenBook = L3Book<DenseArrayLevels, OpenAddressingOrderIndex>;

// Calls f.template operator()<Book>() for every variant.
template <class F>
void for_each_variant(F&& f) {
  f.template operator()<MapStdBook>();
  f.template operator()<MapOpenBook>();
  f.template operator()<VecLinearBook>();
  f.template operator()<VecBinaryBook>();
  f.template operator()<ArrayStdBook>();
  f.template operator()<ArrayOpenBook>();
}

}  // namespace obl::book
