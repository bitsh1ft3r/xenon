/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <limits>
#include <type_traits>

namespace Base {
  namespace MathUtils {

    // https://locklessinc.com/articles/sat_arithmetic/
    template <typename T>
    inline T SaturateAdd(T a, T b) {
      using TU = typename std::make_unsigned<T>::type;
      TU result = TU(a) + TU(b);
      if (std::is_unsigned<T>::value) {
        result |= TU(-static_cast<typename std::make_signed<T>::type>(result < TU(a)));
      } else {
        TU overflowed = (TU(a) >> (sizeof(T) * 8 - 1)) + std::numeric_limits<T>::max();
        if (T((overflowed ^ TU(b)) | ~(TU(b) ^ result)) >= 0) { result = overflowed; }
      }
      return T(result);
    }

    template <typename T>
    inline T SaturateSub(T a, T b) {
      using TU = typename std::make_unsigned<T>::type;
      TU result = TU(a) - TU(b);
      if (std::is_unsigned<T>::value) {
        result &= TU(-static_cast<typename std::make_signed<T>::type>(result <= TU(a)));
      } else {
        TU overflowed = (TU(a) >> (sizeof(T) * 8 - 1)) + std::numeric_limits<T>::max();  
        if (T((overflowed ^ TU(b)) & (overflowed ^ result)) < 0) { result = overflowed; }
      }
      return T(result);
    }

    // Rounds the given number up to the next highest multiple.
    template <typename T, typename V>
    constexpr T RoundUp(T value, V multiple, bool force_non_zero = true) {
      if (force_non_zero && !value) {
        return multiple;
      }
      return (value + multiple - 1) / multiple * multiple;
    }
  }
}