#include <string>

#include <gtest/gtest.h>

#include "veclet/storage/v1/record.pb.h"
#include "veclet/v1/data.grpc.pb.h"
#include "veclet/v1/query.grpc.pb.h"

namespace {

TEST(GeneratedProtoContract, RoundTripsCanonicalStoredRecord) {
  veclet::storage::v1::StoredRecord stored;
  stored.set_local_index_id(7);
  stored.mutable_record()->set_vector_id("customer-42");
  stored.mutable_record()->set_version(1);
  stored.mutable_record()->add_embedding(0.25F);
  stored.mutable_record()->add_embedding(-0.5F);
  stored.mutable_record()->set_payload_data("opaque payload");

  std::string encoded;
  ASSERT_TRUE(stored.SerializeToString(&encoded));

  veclet::storage::v1::StoredRecord decoded;
  ASSERT_TRUE(decoded.ParseFromString(encoded));
  EXPECT_EQ(decoded.local_index_id(), 7);
  EXPECT_EQ(decoded.record().vector_id(), "customer-42");
  EXPECT_EQ(decoded.record().embedding_size(), 2);
}

TEST(GeneratedProtoContract, ExposesExpectedServices) {
  EXPECT_EQ(veclet::v1::QueryService::service_full_name(),
            "veclet.v1.QueryService");
  EXPECT_EQ(veclet::v1::DataService::service_full_name(),
            "veclet.v1.DataService");
}

}  // namespace
