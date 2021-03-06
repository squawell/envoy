#include <chrono>
#include <cstdint>
#include <string>

#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/ratelimit/ratelimit_impl.h"

#include "test/mocks/grpc/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AtLeast;
using testing::Invoke;
using testing::Ref;
using testing::Return;
using testing::WithArg;
using testing::_;

namespace Envoy {
namespace RateLimit {

class MockRequestCallbacks : public RequestCallbacks {
public:
  MOCK_METHOD1(complete, void(LimitStatus status));
};

class RateLimitGrpcClientTest : public testing::Test {
public:
  RateLimitGrpcClientTest()
      : async_client_(new Grpc::MockAsyncClient<pb::lyft::ratelimit::RateLimitRequest,
                                                pb::lyft::ratelimit::RateLimitResponse>()),
        client_(RateLimitAsyncClientPtr{async_client_}, Optional<std::chrono::milliseconds>()) {}

  Grpc::MockAsyncClient<pb::lyft::ratelimit::RateLimitRequest,
                        pb::lyft::ratelimit::RateLimitResponse>* async_client_;
  Grpc::MockAsyncRequest async_request_;
  GrpcClientImpl client_;
  MockRequestCallbacks request_callbacks_;
  Tracing::MockSpan span_;
};

TEST_F(RateLimitGrpcClientTest, Basic) {
  std::unique_ptr<pb::lyft::ratelimit::RateLimitResponse> response;

  {
    pb::lyft::ratelimit::RateLimitRequest request;
    Http::HeaderMapImpl headers;
    GrpcClientImpl::createRequest(request, "foo", {{{{"foo", "bar"}}}});
    EXPECT_CALL(*async_client_, send(_, ProtoEq(request), Ref(client_), _, _))
        .WillOnce(Invoke([this](
                             const Protobuf::MethodDescriptor& service_method,
                             const pb::lyft::ratelimit::RateLimitRequest&,
                             Grpc::AsyncRequestCallbacks<pb::lyft::ratelimit::RateLimitResponse>&,
                             Tracing::Span&,
                             const Optional<std::chrono::milliseconds>&) -> Grpc::AsyncRequest* {
          EXPECT_EQ("pb.lyft.ratelimit.RateLimitService", service_method.service()->full_name());
          EXPECT_EQ("ShouldRateLimit", service_method.name());
          return &async_request_;
        }));

    client_.limit(request_callbacks_, "foo", {{{{"foo", "bar"}}}}, Tracing::NullSpan::instance());

    client_.onCreateInitialMetadata(headers);
    EXPECT_EQ(nullptr, headers.RequestId());

    response.reset(new pb::lyft::ratelimit::RateLimitResponse());
    response->set_overall_code(pb::lyft::ratelimit::RateLimitResponse_Code_OVER_LIMIT);
    EXPECT_CALL(span_, setTag("ratelimit_status", "over_limit"));
    EXPECT_CALL(request_callbacks_, complete(LimitStatus::OverLimit));
    client_.onSuccess(std::move(response), span_);
  }

  {
    pb::lyft::ratelimit::RateLimitRequest request;
    Http::HeaderMapImpl headers;
    GrpcClientImpl::createRequest(request, "foo", {{{{"foo", "bar"}, {"bar", "baz"}}}});
    EXPECT_CALL(*async_client_, send(_, ProtoEq(request), _, _, _))
        .WillOnce(Return(&async_request_));

    client_.limit(request_callbacks_, "foo", {{{{"foo", "bar"}, {"bar", "baz"}}}},
                  Tracing::NullSpan::instance());

    client_.onCreateInitialMetadata(headers);

    response.reset(new pb::lyft::ratelimit::RateLimitResponse());
    response->set_overall_code(pb::lyft::ratelimit::RateLimitResponse_Code_OK);
    EXPECT_CALL(span_, setTag("ratelimit_status", "ok"));
    EXPECT_CALL(request_callbacks_, complete(LimitStatus::OK));
    client_.onSuccess(std::move(response), span_);
  }

  {
    pb::lyft::ratelimit::RateLimitRequest request;
    GrpcClientImpl::createRequest(
        request, "foo",
        {{{{"foo", "bar"}, {"bar", "baz"}}}, {{{"foo2", "bar2"}, {"bar2", "baz2"}}}});
    EXPECT_CALL(*async_client_, send(_, ProtoEq(request), _, _, _))
        .WillOnce(Return(&async_request_));

    client_.limit(request_callbacks_, "foo",
                  {{{{"foo", "bar"}, {"bar", "baz"}}}, {{{"foo2", "bar2"}, {"bar2", "baz2"}}}},
                  Tracing::NullSpan::instance());

    response.reset(new pb::lyft::ratelimit::RateLimitResponse());
    EXPECT_CALL(request_callbacks_, complete(LimitStatus::Error));
    client_.onFailure(Grpc::Status::Unknown, "", span_);
  }
}

TEST_F(RateLimitGrpcClientTest, Cancel) {
  std::unique_ptr<pb::lyft::ratelimit::RateLimitResponse> response;

  EXPECT_CALL(*async_client_, send(_, _, _, _, _)).WillOnce(Return(&async_request_));

  client_.limit(request_callbacks_, "foo", {{{{"foo", "bar"}}}}, Tracing::NullSpan::instance());

  EXPECT_CALL(async_request_, cancel());
  client_.cancel();
}

TEST(RateLimitGrpcFactoryTest, NoCluster) {
  envoy::api::v2::RateLimitServiceConfig config;
  config.set_cluster_name("foo");
  Upstream::MockClusterManager cm;

  EXPECT_CALL(cm, get("foo")).WillOnce(Return(nullptr));
  EXPECT_THROW(GrpcFactoryImpl(config, cm), EnvoyException);
}

TEST(RateLimitGrpcFactoryTest, Create) {
  envoy::api::v2::RateLimitServiceConfig config;
  config.set_cluster_name("foo");
  Upstream::MockClusterManager cm;

  EXPECT_CALL(cm, get("foo")).Times(AtLeast(1));
  GrpcFactoryImpl factory(config, cm);
  factory.create(Optional<std::chrono::milliseconds>());
}

TEST(RateLimitNullFactoryTest, Basic) {
  NullFactoryImpl factory;
  ClientPtr client = factory.create(Optional<std::chrono::milliseconds>());
  MockRequestCallbacks request_callbacks;
  EXPECT_CALL(request_callbacks, complete(LimitStatus::OK));
  client->limit(request_callbacks, "foo", {{{{"foo", "bar"}}}}, Tracing::NullSpan::instance());
  client->cancel();
}

} // namespace RateLimit
} // namespace Envoy
