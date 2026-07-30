#ifndef __TEST_MACROS_H__
#define __TEST_MACROS_H__

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace testing_internal {

inline absl::Status to_status(const absl::Status& status) { return status; }

template <typename T>
absl::Status to_status(const absl::StatusOr<T>& status_or) {
  return status_or.status();
}

}  // namespace testing_internal

#define ASSERT_OK(expr)                                          \
  do {                                                           \
    const absl::Status _s = ::testing_internal::to_status(expr); \
    ASSERT_TRUE(_s.ok()) << _s;                                  \
  } while (0)

#define EXPECT_OK(expr)                                          \
  do {                                                           \
    const absl::Status _s = ::testing_internal::to_status(expr); \
    EXPECT_TRUE(_s.ok()) << _s;                                  \
  } while (0)

#endif  // __TEST_MACROS_H__
