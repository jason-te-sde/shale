// The reference model the simulator compares the database with. If it is wrong, every invariant
// that uses it is wrong, so it is tested on its own.

#include "testkit/model_db.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "shale/write_batch.h"

namespace shale {
namespace {

TEST(ModelDBTest, PutDeleteApply) {
  ModelDB model;
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Delete("a");
  batch.Put("b", "2");
  model.Apply(batch);

  EXPECT_EQ(model.state(), (ModelDB::State{{"b", "2"}}));
  EXPECT_EQ(model.Get("a"), std::nullopt);
  EXPECT_EQ(model.Get("b"), std::optional<std::string>("2"));
}

TEST(ModelDBTest, LaterOperationOnAKeySupersedesTheEarlier) {
  ModelDB model;
  WriteBatch batch;
  batch.Put("k", "old");
  batch.Put("k", "new");
  batch.Put("gone", "x");
  batch.Delete("gone");
  batch.Put("gone", "back");
  model.Apply(batch);

  EXPECT_EQ(model.state(), (ModelDB::State{{"k", "new"}, {"gone", "back"}}));
}

TEST(ModelDBTest, PutOverwritesAndDeleteOfAnAbsentKeyDoesNothing) {
  ModelDB model;
  model.Put("a", "1");
  model.Put("a", "2");
  model.Delete("never-written");
  EXPECT_EQ(model.state(), (ModelDB::State{{"a", "2"}}));

  model.Delete("a");
  EXPECT_TRUE(model.state().empty());
}

TEST(ModelDBTest, EmptyKeyAndEmptyValueAreOrdinary) {
  ModelDB model;
  model.Put("", "");
  model.Put("k", "");
  EXPECT_EQ(model.Get(""), std::optional<std::string>(""));
  EXPECT_EQ(model.Get("k"), std::optional<std::string>(""));
  model.Delete("");
  EXPECT_EQ(model.Get(""), std::nullopt);
}

TEST(ModelDBTest, EmptyBatchChangesNothing) {
  ModelDB model;
  model.Put("a", "1");
  model.Apply(WriteBatch());
  EXPECT_EQ(model.state(), (ModelDB::State{{"a", "1"}}));
}

TEST(ModelDBTest, ResetReplacesTheState) {
  ModelDB model;
  model.Put("a", "1");
  model.Reset({{"x", "9"}});
  EXPECT_EQ(model.state(), (ModelDB::State{{"x", "9"}}));
  EXPECT_EQ(model.Get("a"), std::nullopt);
}

TEST(ModelDBTest, KeysAreHeldInBytewiseOrder) {
  // A key with a byte above 0x7f sorts after one below it, as the database orders them, whatever
  // the signedness of char.
  ModelDB model;
  model.Put("\xff", "high");
  model.Put("a", "low");
  model.Put(std::string("a\0b", 3), "nul");
  std::string order;
  for (const auto& entry : model.state()) {
    order += entry.second + " ";
  }
  EXPECT_EQ(order, "low nul high ");
}

TEST(ModelDBTest, DigestIsOrderAndContentSensitive) {
  EXPECT_NE(ModelDB::Digest({{"a", "1"}, {"b", "2"}}), ModelDB::Digest({{"a", "2"}, {"b", "1"}}));
  EXPECT_NE(ModelDB::Digest({{"ab", ""}}), ModelDB::Digest({{"a", "b"}}));
  EXPECT_EQ(ModelDB::Digest({{"a", "1"}, {"b", "2"}}), ModelDB::Digest({{"b", "2"}, {"a", "1"}}))
      << "equal maps are the same map, whatever order they were built in";
}

TEST(ModelDBTest, DigestSeesWhereAKeyEndsAndAValueBegins) {
  // The lengths are part of what is hashed, so moving a byte across the key-value boundary, or
  // from one entry to the next, changes the digest.
  EXPECT_NE(ModelDB::Digest({{"a", "bc"}}), ModelDB::Digest({{"ab", "c"}}));
  EXPECT_NE(ModelDB::Digest({{"a", ""}, {"b", ""}}), ModelDB::Digest({{"a", "b"}}));
  EXPECT_NE(ModelDB::Digest({{"a", "x"}, {"b", "y"}}), ModelDB::Digest({{"a", "xb"}, {"", "y"}}));
}

TEST(ModelDBTest, DigestSeesEveryEntryAndEveryByte) {
  const ModelDB::State base{{"a", "1"}, {"b", "2"}, {"c", "3"}};
  ModelDB::State fewer = base;
  fewer.erase("c");
  ModelDB::State more = base;
  more["d"] = "4";
  ModelDB::State changed = base;
  changed["b"] = "3";
  EXPECT_NE(ModelDB::Digest(base), ModelDB::Digest(fewer));
  EXPECT_NE(ModelDB::Digest(base), ModelDB::Digest(more));
  EXPECT_NE(ModelDB::Digest(base), ModelDB::Digest(changed));
  EXPECT_NE(ModelDB::Digest({}), ModelDB::Digest({{"", ""}})) << "an empty entry is an entry";
  EXPECT_NE(ModelDB::Digest({{std::string(1, '\0'), ""}}), ModelDB::Digest({{"", ""}}));
}

TEST(ModelDBTest, DigestFillsBothHalvesOfTheWord) {
  // The digest is two 32-bit hash chains from different seeds, one in each half of the word. A
  // 32-bit hash widened to 64 bits would leave the high half empty and gain nothing, so both halves
  // must carry a hash of the contents and the two must not be the same hash.
  const uint64_t digest = ModelDB::Digest({{"a", "1"}});
  EXPECT_NE(digest >> 32, uint64_t{0});
  EXPECT_NE(digest & 0xffffffffu, uint64_t{0});
  EXPECT_NE(digest >> 32, digest & 0xffffffffu);
}

TEST(ModelDBTest, DigestOfTheModelEqualsDigestOfItsState) {
  ModelDB model;
  model.Put("a", "1");
  model.Put("b", "2");
  EXPECT_EQ(ModelDB::Digest(model.state()), ModelDB::Digest({{"a", "1"}, {"b", "2"}}));
}

}  // namespace
}  // namespace shale
