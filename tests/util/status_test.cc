#include "shale/status.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

namespace shale {
namespace {

// Containers move elements on reallocation and fall back to copying when the move may throw, so
// the noexcept promises in the interface are part of its behaviour.
static_assert(std::is_nothrow_default_constructible_v<Status>);
static_assert(std::is_nothrow_move_constructible_v<Status>);
static_assert(std::is_nothrow_move_assignable_v<Status>);

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk) << s.ToString();
  EXPECT_EQ(s.ToString(), "OK");
  EXPECT_TRUE(Status::OK().ok());
  EXPECT_EQ(Status::OK().ToString(), "OK");
}

TEST(StatusTest, ToStringNamesCodeMessageAndDetail) {
  EXPECT_EQ(Status::NotFound("k").ToString(), "NotFound: k");
  EXPECT_EQ(Status::Corruption("bad block", "000007.sst").ToString(),
            "Corruption: bad block: 000007.sst");
  EXPECT_EQ(Status::InvalidArgument("x").ToString(), "Invalid argument: x");
  EXPECT_EQ(Status::IOError("x").ToString(), "IO error: x");
  EXPECT_EQ(Status::NotSupported("x").ToString(), "Not supported: x");
}

TEST(StatusTest, ExactlyOnePredicateHolds) {
  using Predicate = bool (Status::*)() const noexcept;
  const Predicate all_predicates[] = {&Status::IsNotFound, &Status::IsCorruption,
                                      &Status::IsInvalidArgument, &Status::IsIOError,
                                      &Status::IsNotSupported};
  struct Case {
    Status status;
    Status::Code code;
    Predicate expected;
  };
  const Case cases[] = {
      {Status::NotFound("x"), Status::Code::kNotFound, &Status::IsNotFound},
      {Status::Corruption("x"), Status::Code::kCorruption, &Status::IsCorruption},
      {Status::InvalidArgument("x"), Status::Code::kInvalidArgument, &Status::IsInvalidArgument},
      {Status::IOError("x"), Status::Code::kIOError, &Status::IsIOError},
      {Status::NotSupported("x"), Status::Code::kNotSupported, &Status::IsNotSupported},
  };
  for (const Case& c : cases) {
    EXPECT_FALSE(c.status.ok()) << c.status.ToString();
    EXPECT_EQ(c.status.code(), c.code) << c.status.ToString();
    for (Predicate p : all_predicates) {
      EXPECT_EQ((c.status.*p)(), p == c.expected) << c.status.ToString();
    }
  }
}

TEST(StatusTest, CopyAndMovePreserveCodeAndMessage) {
  Status a = Status::IOError("disk", "full");
  Status b = a;
  Status c = std::move(a);
  EXPECT_EQ(b.ToString(), "IO error: disk: full");
  EXPECT_EQ(c.ToString(), "IO error: disk: full");
  EXPECT_TRUE(b.IsIOError());
  EXPECT_TRUE(c.IsIOError());
}

}  // namespace
}  // namespace shale
