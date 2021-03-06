/* Copyright (C) Extensible Service Proxy Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "src/api_manager/config_manager.h"

#include "src/api_manager/config.h"
#include "src/api_manager/context/global_context.h"
#include "src/api_manager/mock_api_manager_environment.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::Return;

using ::google::api_manager::utils::Status;

namespace google {
namespace api_manager {

namespace {

const char kServerConfigWithServiceName[] = R"(
{
  "google_authentication_secret": "{}",
  "metadata_server_config": {
    "enabled": true,
    "url": "http://localhost"
  },
  "service_control_config": {
    "report_aggregator_config": {
      "cache_entries": 10000,
      "flush_interval_ms": 1000001232
    },
    "quota_aggregator_config": {
      "cache_entries": 300000,
      "refresh_interval_ms": 1000
    }
  },
  "service_management_config": {
    "fetch_throttle_window_s": 300
  },
  "service_name": "service_name_from_server_config",
  "rollout_strategy": "managed"
}
)";

// The number of minutes to jump to next fetch throttle window.
// In order to trigger a new fetch timer.
// It should be bigger than "fetch_throttle_window_s" field
// in kServerConfigWithServiceName
const int kNextFetchWindowInS = 330;

const char kGceMetadataWithServiceNameAndConfigId[] = R"(
{
  "project": {
    "projectId": "test-project"
  },
  "instance": {
    "attributes":{
      "endpoints-service-name": "service_name_from_metadata",
      "endpoints-service-config-id":"2017-05-01r1"
    }
  }
}
)";

const char kServiceConfig1[] = R"(
{
  "name": "bookstore.test.appspot.com",
  "title": "Bookstore",
  "id": "2017-05-01r0"
}
)";

const char kServiceConfig2[] = R"(
{
  "name": "bookstore.test.appspot.com",
  "title": "Bookstore",
  "id": "2017-05-01r1"
}
)";

const char kServiceConfig3[] = R"(
{
  "name": "bookstore.test.appspot.com",
  "title": "Bookstore",
  "id": "2017-05-01r2"
}
)";

const char kRolloutsResponse1[] = R"(
{
  "rollouts": [
    {
      "rolloutId": "2017-05-01r0",
      "createTime": "2017-05-01T22:40:09.884Z",
      "createdBy": "test_user@google.com",
      "status": "SUCCESS",
      "trafficPercentStrategy": {
        "percentages": {
          "2017-05-01r0": 100
        }
      },
      "serviceName": "service_name_from_server_config"
    }
  ]
}
)";

const char kRolloutsResponse2[] = R"(
{
  "rollouts": [
    {
      "rolloutId": "2017-05-01r1",
      "createTime": "2017-05-01T22:40:09.884Z",
      "createdBy": "test_user@google.com",
      "status": "SUCCESS",
      "trafficPercentStrategy": {
        "percentages": {
          "2017-05-01r1": 100
        }
      },
      "serviceName": "service_name_from_server_config"
    }
  ]
}
)";

const char kRolloutsResponseMultipleServiceConfig[] = R"(
{
  "rollouts": [
    {
      "rolloutId": "2017-05-01r0",
      "createTime": "2017-05-01T22:40:09.884Z",
      "createdBy": "test_user@google.com",
      "status": "FAILED",
      "trafficPercentStrategy": {
        "percentages": {
          "2017-05-01r0": 80,
          "2017-05-01r1": 20
        }
      },
      "serviceName": "service_name_from_server_config"
    }
  ]
}
)";

class MockTimerApiManagerEnvironment : public MockApiManagerEnvironmentWithLog {
 public:
  MOCK_METHOD1(MakeTag, void*(std::function<void(bool)>));

  virtual std::unique_ptr<PeriodicTimer> StartPeriodicTimer(
      std::chrono::milliseconds interval, std::function<void()> continuation) {
    timer_count_++;
    timer_last_interval_ = interval;
    mock_periodic_timer_ = new MockPeriodicTimer(continuation);
    return std::unique_ptr<PeriodicTimer>(mock_periodic_timer_);
  }

  MOCK_METHOD1(DoRunHTTPRequest, void(HTTPRequest*));
  MOCK_METHOD1(DoRunGRPCRequest, void(GRPCRequest*));
  virtual void RunHTTPRequest(std::unique_ptr<HTTPRequest> req) {
    DoRunHTTPRequest(req.get());
  }
  virtual void RunGRPCRequest(std::unique_ptr<GRPCRequest> req) {
    DoRunGRPCRequest(req.get());
  }

  void RunTimer() { mock_periodic_timer_->Run(); }
  int timer_count() const { return timer_count_; }
  std::chrono::milliseconds timer_last_interval() const {
    return timer_last_interval_;
  }

 private:
  int timer_count_{};
  std::chrono::milliseconds timer_last_interval_;
  MockPeriodicTimer* mock_periodic_timer_{};
};

// Both service_name, config_id in server config
class ConfigManagerServiceNameConfigIdTest : public ::testing::Test {
 public:
  void SetUp() {
    env_.reset(new ::testing::NiceMock<MockTimerApiManagerEnvironment>());
    // save the raw pointer of env before calling std::move(env).
    raw_env_ = env_.get();

    global_context_ = std::make_shared<context::GlobalContext>(
        std::move(env_), kServerConfigWithServiceName);

    global_context_->set_service_name("service_name_from_metadata");
  }

  std::unique_ptr<MockTimerApiManagerEnvironment> env_;
  MockTimerApiManagerEnvironment* raw_env_;
  std::shared_ptr<context::GlobalContext> global_context_;
};

TEST_F(ConfigManagerServiceNameConfigIdTest, VerifyTimerIntervalDistribution) {
  ON_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillByDefault(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponse1);
      }));

  int sequence = 0;
  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status&,
                        const std::vector<std::pair<std::string, int>>&) {
        sequence++;
      },
      nullptr));
  config_manager->set_current_rollout_id("2017-05-01r0");

  // Default is 5 minute interval. Use 5 slot for each minute.
  int timer_dist[5];
  memset(timer_dist, 0, sizeof(timer_dist));

  for (int i = 0; i < 100; ++i) {
    config_manager->SetLatestRolloutId(
        "2017-05-01r111", std::chrono::system_clock::now() +
                              std::chrono::seconds(i * kNextFetchWindowInS));
    EXPECT_EQ(raw_env_->timer_count(), i + 1);
    raw_env_->RunTimer();
    EXPECT_EQ(0, sequence);

    // Slot index in minute
    int dx = raw_env_->timer_last_interval().count() / (1000 * 60);
    std::cout << "bucket index: " << dx << std::endl;
    if (dx >= 0 && dx < 5) {
      timer_dist[dx]++;
    }
  }

  // 10 requests should be distributed into 5 slots almost evently.
  // For each minute slot, the count should be bigger than 1.
  for (int i = 0; i < 5; ++i) {
    std::cout << "bucket index: " << i << ", count: " << timer_dist[i]
              << std::endl;
    EXPECT_TRUE(timer_dist[i] >= 1);
  }
}

TEST_F(ConfigManagerServiceNameConfigIdTest, RolloutSingleServiceConfig) {
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponse1);
      }))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/configs/2017-05-01r0",
            req->url());
        req->OnComplete(Status::OK, {}, kServiceConfig1);
      }));

  int sequence = 0;
  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        const std::vector<std::pair<std::string, int>>& list) {
        EXPECT_EQ(1, list.size());
        EXPECT_EQ(kServiceConfig1, list[0].first);
        EXPECT_EQ(100, list[0].second);
        sequence++;
      },
      nullptr));

  config_manager->SetLatestRolloutId("2017-05-01r0",
                                     std::chrono::system_clock::now());
  EXPECT_EQ(0, sequence);
  EXPECT_EQ(raw_env_->timer_count(), 1);
  raw_env_->RunTimer();
  EXPECT_EQ(1, sequence);

  config_manager->SetLatestRolloutId(
      "2017-05-01r0", std::chrono::system_clock::now() +
                          std::chrono::seconds(kNextFetchWindowInS));
  // Timer is not called.
  EXPECT_EQ(raw_env_->timer_count(), 1);
}

TEST_F(ConfigManagerServiceNameConfigIdTest, RolloutIDNotChanged) {
  int sequence = 0;
  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        const std::vector<std::pair<std::string, int>>& list) {
        sequence++;
      },
      nullptr));

  // set rollout_id to 2017-05-01r0 which is same as kRolloutsResponse1
  config_manager->set_current_rollout_id("2017-05-01r0");

  config_manager->SetLatestRolloutId("2017-05-01r0",
                                     std::chrono::system_clock::now());
  EXPECT_EQ(raw_env_->timer_count(), 0);
  EXPECT_EQ(0, sequence);
}

TEST_F(ConfigManagerServiceNameConfigIdTest, RepeatedTrigger) {
  int sequence = 0;
  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        const std::vector<std::pair<std::string, int>>& list) {
        EXPECT_EQ(1, list.size());
        EXPECT_EQ(kServiceConfig1, list[0].first);
        EXPECT_EQ(100, list[0].second);
        sequence++;
      },
      nullptr));
  config_manager->set_current_rollout_id("2017-05-01r0");

  auto now = std::chrono::system_clock::now();
  // Use a different ID to trigger
  config_manager->SetLatestRolloutId("2017-05-01r111", now);
  EXPECT_EQ(raw_env_->timer_count(), 1);

  // So no need to make rollout HTTP call.
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_)).Times(0);

  // Trigger it again, a new timer call should not be called.
  // Since last timer is not fired regardless now late it is now.
  config_manager->SetLatestRolloutId(
      "2017-05-01r111", now + std::chrono::seconds(kNextFetchWindowInS));
  EXPECT_EQ(raw_env_->timer_count(), 1);

  // But the replied rollout ID is the same as the old one.
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponse1);
      }));

  // Fire the first timer.
  raw_env_->RunTimer();
  // callback should not be called
  EXPECT_EQ(0, sequence);

  // Trigger it again, default window is 5 minute, not next window yet.
  // So not to fire the timer.
  config_manager->SetLatestRolloutId("2017-05-01r111",
                                     now + std::chrono::seconds(10));
  EXPECT_EQ(raw_env_->timer_count(), 1);
  // callback should not be called
  EXPECT_EQ(0, sequence);

  // Trigger it again, at next window. Timer should be called.
  config_manager->SetLatestRolloutId(
      "2017-05-01r111", std::chrono::system_clock::now() +
                            std::chrono::seconds(kNextFetchWindowInS));
  EXPECT_EQ(raw_env_->timer_count(), 2);
}

TEST_F(ConfigManagerServiceNameConfigIdTest, RolloutMultipleServiceConfig) {
  std::function<void(HTTPRequest * req)> handler = [this](HTTPRequest* req) {
    std::map<std::string, std::string> data = {
        {"https://servicemanagement.googleapis.com/v1/services/"
         "service_name_from_metadata/configs/2017-05-01r0",
         kServiceConfig1},
        {"https://servicemanagement.googleapis.com/v1/services/"
         "service_name_from_metadata/configs/2017-05-01r1",
         kServiceConfig2}};

    if (data.find(req->url()) != data.end()) {
      req->OnComplete(Status::OK, {}, std::move(data[req->url()]));
    } else {
      req->OnComplete(utils::Status(Code::NOT_FOUND, "Not Found"), {}, "");
    }
  };

  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponseMultipleServiceConfig);
      }))
      .WillOnce(Invoke(handler))
      .WillOnce(Invoke(handler));

  int sequence = 0;

  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        std::vector<std::pair<std::string, int>> list) {
        std::sort(list.begin(), list.end());

        EXPECT_EQ(2, list.size());
        EXPECT_EQ(kServiceConfig1, list[0].first);
        EXPECT_EQ(80, list[0].second);
        EXPECT_EQ(kServiceConfig2, list[1].first);
        EXPECT_EQ(20, list[1].second);
        sequence++;
      },
      nullptr));

  config_manager->SetLatestRolloutId("2017-05-01r0",
                                     std::chrono::system_clock::now());
  EXPECT_EQ(0, sequence);

  EXPECT_EQ(raw_env_->timer_count(), 1);
  raw_env_->RunTimer();
  EXPECT_EQ(1, sequence);
}

TEST_F(ConfigManagerServiceNameConfigIdTest,
       RolloutMultipleServiceConfigPartiallyFailedThenSucceededNextTimerEvent) {
  std::function<void(HTTPRequest * req)> first_handler =
      [this](HTTPRequest* req) {
        std::map<std::string, std::string> data = {
            {"https://servicemanagement.googleapis.com/v1/services/"
             "service_name_from_metadata/configs/2017-05-01r0",
             kServiceConfig1}};

        if (data.find(req->url()) != data.end()) {
          req->OnComplete(Status::OK, {}, std::move(data[req->url()]));
        } else {
          req->OnComplete(utils::Status(Code::NOT_FOUND, "Not Found"), {}, "");
        }
      };

  std::function<void(HTTPRequest * req)> second_handler =
      [this](HTTPRequest* req) {
        std::map<std::string, std::string> data = {
            {"https://servicemanagement.googleapis.com/v1/services/"
             "service_name_from_metadata/configs/2017-05-01r0",
             kServiceConfig1},
            {"https://servicemanagement.googleapis.com/v1/services/"
             "service_name_from_metadata/configs/2017-05-01r1",
             kServiceConfig2}};

        if (data.find(req->url()) != data.end()) {
          req->OnComplete(Status::OK, {}, std::move(data[req->url()]));
        } else {
          req->OnComplete(utils::Status(Code::NOT_FOUND, "Not Found"), {}, "");
        }
      };

  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponseMultipleServiceConfig);
      }))
      .WillOnce(Invoke(first_handler))
      .WillOnce(Invoke(first_handler))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponseMultipleServiceConfig);
      }))
      .WillOnce(Invoke(second_handler))
      .WillOnce(Invoke(second_handler));

  int sequence = 0;

  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        const std::vector<std::pair<std::string, int>>& list) {
        sequence++;
      },
      nullptr));

  config_manager->SetLatestRolloutId("2017-05-01r0",
                                     std::chrono::system_clock::now());
  EXPECT_EQ(0, sequence);
  EXPECT_EQ(raw_env_->timer_count(), 1);
  raw_env_->RunTimer();
  // One of ServiceConfig download was failed. The callback should not be
  // invoked
  EXPECT_EQ(0, sequence);

  // Succeeded on the next timer event. Invoke the callback function
  config_manager->SetLatestRolloutId(
      "2017-05-01r0", std::chrono::system_clock::now() +
                          std::chrono::seconds(kNextFetchWindowInS));
  EXPECT_EQ(raw_env_->timer_count(), 2);
  raw_env_->RunTimer();
  EXPECT_EQ(1, sequence);
}

TEST_F(ConfigManagerServiceNameConfigIdTest, RolloutSingleServiceConfigUpdate) {
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponse1);
      }))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/configs/2017-05-01r0",
            req->url());
        req->OnComplete(Status::OK, {}, kServiceConfig1);
      }))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponse2);
      }))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/configs/2017-05-01r1",
            req->url());
        req->OnComplete(Status::OK, {}, kServiceConfig2);
      }));

  int sequence = 0;

  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        const std::vector<std::pair<std::string, int>>& list) {
        EXPECT_EQ(1, list.size());

        // depends on sequence, different service_config will downloaded
        EXPECT_EQ(sequence == 0 ? kServiceConfig1 : kServiceConfig2,
                  list[0].first);

        EXPECT_EQ(100, list[0].second);

        sequence++;
      },
      nullptr));

  config_manager->SetLatestRolloutId("2017-05-01r0",
                                     std::chrono::system_clock::now());
  EXPECT_EQ(0, sequence);

  // run first periodic timer
  EXPECT_EQ(raw_env_->timer_count(), 1);
  raw_env_->RunTimer();
  EXPECT_EQ(1, sequence);

  config_manager->SetLatestRolloutId(
      "2017-05-01r1", std::chrono::system_clock::now() +
                          std::chrono::seconds(kNextFetchWindowInS));
  EXPECT_EQ(raw_env_->timer_count(), 2);
  raw_env_->RunTimer();
  EXPECT_EQ(2, sequence);
}

TEST_F(ConfigManagerServiceNameConfigIdTest,
       RolloutSingleServiceConfigNoupdate) {
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/rollouts?filter=status=SUCCESS",
            req->url());
        req->OnComplete(Status::OK, {}, kRolloutsResponse1);
      }))
      .WillOnce(Invoke([this](HTTPRequest* req) {
        EXPECT_EQ(
            "https://servicemanagement.googleapis.com/v1/services/"
            "service_name_from_metadata/configs/2017-05-01r0",
            req->url());
        req->OnComplete(Status::OK, {}, kServiceConfig1);
      }));

  int sequence = 0;
  std::shared_ptr<ConfigManager> config_manager(new ConfigManager(
      global_context_,
      [this, &sequence](const utils::Status& status,
                        const std::vector<std::pair<std::string, int>>& list) {
        EXPECT_EQ(1, list.size());
        EXPECT_EQ(kServiceConfig1, list[0].first);
        EXPECT_EQ(100, list[0].second);

        sequence++;
      },
      nullptr));

  config_manager->SetLatestRolloutId("2017-05-01r0",
                                     std::chrono::system_clock::now());
  EXPECT_EQ(0, sequence);

  // run first periodic timer
  EXPECT_EQ(raw_env_->timer_count(), 1);
  raw_env_->RunTimer();
  EXPECT_EQ(1, sequence);

  config_manager->SetLatestRolloutId(
      "2017-05-01r0", std::chrono::system_clock::now() +
                          std::chrono::seconds(kNextFetchWindowInS));
  // Same rollout_id, no update
  EXPECT_EQ(raw_env_->timer_count(), 1);
}

}  // namespace
}  // namespace api_manager
}  // namespace google
