#include "common/config/filter_json.h"

#include "common/common/assert.h"
#include "common/common/utility.h"
#include "common/config/json_utility.h"
#include "common/config/protocol_json.h"
#include "common/config/rds_json.h"
#include "common/config/utility.h"
#include "common/json/config_schemas.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Config {

namespace {

void translateComparisonFilter(const Json::Object& config,
                               envoy::api::v2::filter::ComparisonFilter& filter) {
  const std::string op = config.getString("op");
  if (op == ">=") {
    filter.set_op(envoy::api::v2::filter::ComparisonFilter::GE);
  } else {
    ASSERT(op == "=");
    filter.set_op(envoy::api::v2::filter::ComparisonFilter::EQ);
  }

  auto* runtime = filter.mutable_value();
  runtime->set_default_value(config.getInteger("value"));
  runtime->set_runtime_key(config.getString("runtime_key", ""));
}

void translateStatusCodeFilter(const Json::Object& config,
                               envoy::api::v2::filter::StatusCodeFilter& filter) {
  translateComparisonFilter(config, *filter.mutable_comparison());
}

void translateDurationFilter(const Json::Object& config,
                             envoy::api::v2::filter::DurationFilter& filter) {
  translateComparisonFilter(config, *filter.mutable_comparison());
}

void translateRuntimeFilter(const Json::Object& config,
                            envoy::api::v2::filter::RuntimeFilter& filter) {
  filter.set_runtime_key(config.getString("key"));
}

void translateRepeatedFilter(
    const Json::Object& config,
    Protobuf::RepeatedPtrField<envoy::api::v2::filter::AccessLogFilter>& filters) {
  for (const auto& json_filter : config.getObjectArray("filters")) {
    FilterJson::translateAccessLogFilter(*json_filter, *filters.Add());
  }
}

void translateOrFilter(const Json::Object& config, envoy::api::v2::filter::OrFilter& filter) {
  translateRepeatedFilter(config, *filter.mutable_filters());
}

void translateAndFilter(const Json::Object& config, envoy::api::v2::filter::AndFilter& filter) {
  translateRepeatedFilter(config, *filter.mutable_filters());
}

} // namespace

void FilterJson::translateAccessLogFilter(
    const Json::Object& json_access_log_filter,
    envoy::api::v2::filter::AccessLogFilter& access_log_filter) {
  const std::string type = json_access_log_filter.getString("type");
  if (type == "status_code") {
    translateStatusCodeFilter(json_access_log_filter,
                              *access_log_filter.mutable_status_code_filter());
  } else if (type == "duration") {
    translateDurationFilter(json_access_log_filter, *access_log_filter.mutable_duration_filter());
  } else if (type == "runtime") {
    translateRuntimeFilter(json_access_log_filter, *access_log_filter.mutable_runtime_filter());
  } else if (type == "logical_or") {
    translateOrFilter(json_access_log_filter, *access_log_filter.mutable_or_filter());
  } else if (type == "logical_and") {
    translateAndFilter(json_access_log_filter, *access_log_filter.mutable_and_filter());
  } else if (type == "not_healthcheck") {
    access_log_filter.mutable_not_health_check_filter();
  } else {
    ASSERT(type == "traceable_request");
    access_log_filter.mutable_traceable_filter();
  }
}

void FilterJson::translateAccessLog(const Json::Object& json_access_log,
                                    envoy::api::v2::filter::AccessLog& access_log) {
  JSON_UTIL_SET_STRING(json_access_log, access_log, path);
  JSON_UTIL_SET_STRING(json_access_log, access_log, format);
  if (json_access_log.hasObject("filter")) {
    translateAccessLogFilter(*json_access_log.getObject("filter"), *access_log.mutable_filter());
  }
}

void FilterJson::translateHttpConnectionManager(
    const Json::Object& json_http_connection_manager,
    envoy::api::v2::filter::HttpConnectionManager& http_connection_manager) {
  json_http_connection_manager.validateSchema(Json::Schema::HTTP_CONN_NETWORK_FILTER_SCHEMA);

  envoy::api::v2::filter::HttpConnectionManager::CodecType codec_type{};
  envoy::api::v2::filter::HttpConnectionManager::CodecType_Parse(
      StringUtil::toUpper(json_http_connection_manager.getString("codec_type")), &codec_type);
  http_connection_manager.set_codec_type(codec_type);

  JSON_UTIL_SET_STRING(json_http_connection_manager, http_connection_manager, stat_prefix);

  if (json_http_connection_manager.hasObject("rds")) {
    Utility::translateRdsConfig(*json_http_connection_manager.getObject("rds"),
                                *http_connection_manager.mutable_rds());
  }
  if (json_http_connection_manager.hasObject("route_config")) {
    if (json_http_connection_manager.hasObject("rds")) {
      throw EnvoyException(
          "http connection manager must have either rds or route_config but not both");
    }
    RdsJson::translateRouteConfiguration(*json_http_connection_manager.getObject("route_config"),
                                         *http_connection_manager.mutable_route_config());
  }

  for (const auto& json_filter : json_http_connection_manager.getObjectArray("filters", true)) {
    auto* filter = http_connection_manager.mutable_http_filters()->Add();
    JSON_UTIL_SET_STRING(*json_filter, *filter, name);
    JSON_UTIL_SET_STRING(*json_filter, *filter->mutable_deprecated_v1(), type);

    const std::string json_config = "{\"deprecated_v1\": true, \"value\": " +
                                    json_filter->getObject("config")->asJsonString() + "}";

    const auto status = Protobuf::util::JsonStringToMessage(json_config, filter->mutable_config());
    // JSON schema has already validated that this is a valid JSON object.
    ASSERT(status.ok());
    UNREFERENCED_PARAMETER(status);
  }

  JSON_UTIL_SET_BOOL(json_http_connection_manager, http_connection_manager, add_user_agent);

  if (json_http_connection_manager.hasObject("tracing")) {
    const auto json_tracing = json_http_connection_manager.getObject("tracing");
    auto* tracing = http_connection_manager.mutable_tracing();

    envoy::api::v2::filter::HttpConnectionManager::Tracing::OperationName operation_name{};
    envoy::api::v2::filter::HttpConnectionManager::Tracing::OperationName_Parse(
        StringUtil::toUpper(json_tracing->getString("operation_name")), &operation_name);
    tracing->set_operation_name(operation_name);

    for (const std::string& header :
         json_tracing->getStringArray("request_headers_for_tags", true)) {
      tracing->add_request_headers_for_tags(header);
    }
  }

  if (json_http_connection_manager.hasObject("http1_settings")) {
    ProtocolJson::translateHttp1ProtocolOptions(
        *json_http_connection_manager.getObject("http1_settings"),
        *http_connection_manager.mutable_http_protocol_options());
  }

  if (json_http_connection_manager.hasObject("http2_settings")) {
    ProtocolJson::translateHttp2ProtocolOptions(
        *json_http_connection_manager.getObject("http2_settings"),
        *http_connection_manager.mutable_http2_protocol_options());
  }

  JSON_UTIL_SET_STRING(json_http_connection_manager, http_connection_manager, server_name);
  JSON_UTIL_SET_DURATION_SECONDS(json_http_connection_manager, http_connection_manager,
                                 idle_timeout);
  JSON_UTIL_SET_DURATION(json_http_connection_manager, http_connection_manager, drain_timeout);

  for (const auto& json_access_log :
       json_http_connection_manager.getObjectArray("access_log", true)) {
    auto* access_log = http_connection_manager.mutable_access_log()->Add();
    translateAccessLog(*json_access_log, *access_log);
  }

  JSON_UTIL_SET_BOOL(json_http_connection_manager, http_connection_manager, use_remote_address);
  JSON_UTIL_SET_BOOL(json_http_connection_manager, http_connection_manager, generate_request_id);

  envoy::api::v2::filter::HttpConnectionManager::ForwardClientCertDetails fcc_details{};
  envoy::api::v2::filter::HttpConnectionManager::ForwardClientCertDetails_Parse(
      StringUtil::toUpper(
          json_http_connection_manager.getString("forward_client_cert", "sanitize")),
      &fcc_details);
  http_connection_manager.set_forward_client_cert_details(fcc_details);

  for (const std::string& detail :
       json_http_connection_manager.getStringArray("set_current_client_cert_details", true)) {
    if (detail == "Subject") {
      http_connection_manager.mutable_set_current_client_cert_details()
          ->mutable_subject()
          ->set_value(true);
    } else {
      ASSERT(detail == "SAN");
      http_connection_manager.mutable_set_current_client_cert_details()->mutable_san()->set_value(
          true);
    }
  }
}

} // namespace Config
} // namespace Envoy
