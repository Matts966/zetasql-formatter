//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/local_service/local_service.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "zetasql/base/logging.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor.h"
#include "zetasql/common/errors.h"
#include "zetasql/common/proto_helper.h"
#include "zetasql/local_service/local_service.pb.h"
#include "zetasql/local_service/state.h"
#include "zetasql/proto/simple_catalog.pb.h"
#include "zetasql/public/builtin_function.h"
#include "zetasql/public/evaluator.h"
#include "zetasql/public/function.h"
#include "zetasql/public/id_string.h"
#include "zetasql/public/language_options.h"
#include "zetasql/public/simple_catalog.h"
#include "zetasql/public/sql_formatter.h"
#include "zetasql/public/table_from_proto.h"
#include "zetasql/public/type.h"
#include "zetasql/public/type.pb.h"
#include "zetasql/public/value.h"
#include "zetasql/public/value.pb.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/sql_builder.h"
#include "absl/base/thread_annotations.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "zetasql/base/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/optional.h"
#include "zetasql/base/source_location.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {
namespace local_service {

using google::protobuf::RepeatedPtrField;

namespace {

absl::Status RepeatedParametersToMap(
    const RepeatedPtrField<EvaluateRequest::Parameter>& params,
    const QueryParametersMap& types, ParameterValueMap* map) {
  for (const auto& param : params) {
    std::string name = absl::AsciiStrToLower(param.name());
    const Type* type = zetasql_base::FindPtrOrNull(types, name);
    ZETASQL_RET_CHECK(type != nullptr) << "Type not found for '" << name << "'";
    auto result = Value::Deserialize(param.value(), type);
    ZETASQL_RETURN_IF_ERROR(result.status());
    (*map)[name] = result.value();
  }

  return absl::OkStatus();
}

// Populate the existing pools into the map with existing indices, to make sure
// the serialized type will use the same indices.
void PopulateExistingPoolsToFileDescriptorSetMap(
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    FileDescriptorSetMap* file_descriptor_set_map) {
  file_descriptor_set_map->clear();

  for (int i = 0; i < pools.size(); ++i) {
    std::unique_ptr<Type::FileDescriptorEntry>& entry =
        (*file_descriptor_set_map)[pools[i]];
    ZETASQL_CHECK_EQ(entry.get(), nullptr);
    entry = absl::make_unique<Type::FileDescriptorEntry>();
    entry->descriptor_set_index = i;
  }

  ZETASQL_CHECK_EQ(pools.size(), file_descriptor_set_map->size());
}

absl::Status SerializeTypeUsingExistingPools(
    const Type* type, const std::vector<const google::protobuf::DescriptorPool*>& pools,
    TypeProto* type_proto) {
  FileDescriptorSetMap file_descriptor_set_map;
  PopulateExistingPoolsToFileDescriptorSetMap(pools, &file_descriptor_set_map);

  ZETASQL_RETURN_IF_ERROR(type->SerializeToProtoAndDistinctFileDescriptors(
      type_proto, &file_descriptor_set_map));

  ZETASQL_RET_CHECK_EQ(pools.size(), file_descriptor_set_map.size())
      << type->DebugString(true)
      << " uses unknown DescriptorPool, this shouldn't happen.";
  return absl::OkStatus();
}

}  // namespace

class RegisteredDescriptorPoolState : public GenericState {
 public:
  RegisteredDescriptorPoolState() {}
  absl::Status Init(const google::protobuf::FileDescriptorSet& fdset) {
    ZETASQL_RET_CHECK(!pool_.has_value() && !is_builtin_);
    pool_.emplace();
    ZETASQL_RETURN_IF_ERROR(AddFileDescriptorSetToPool(&fdset, &(pool_.value())));
    return absl::OkStatus();
  }

  const google::protobuf::DescriptorPool* pool() {
    if (is_builtin_) {
      return google::protobuf::DescriptorPool::generated_pool();
    } else if (pool_.has_value()) {
      return &(pool_.value());
    }
    // This shouldn't happen.
    return nullptr;
  }

 private:
  friend class RegisteredDescriptorPoolPool;
  class builtin_descriptor_pool_t {};
  explicit RegisteredDescriptorPoolState(builtin_descriptor_pool_t)
      : is_builtin_(true) {}

  absl::optional<google::protobuf::DescriptorPool> pool_ = absl::nullopt;
  const bool is_builtin_ = false;
};

class RegisteredDescriptorPoolPool
    : public SharedStatePool<RegisteredDescriptorPoolState> {
 public:
  RegisteredDescriptorPoolPool() {
    int64_t id = Register(new RegisteredDescriptorPoolState(
        RegisteredDescriptorPoolState::builtin_descriptor_pool_t()));
    ZETASQL_CHECK_NE(id, -1);
    builtin_pool_ = Get(id);
  }

  std::shared_ptr<RegisteredDescriptorPoolState>
  GetBuiltinDescriptorPoolState() {
    return builtin_pool_;
  }

 private:
  std::shared_ptr<RegisteredDescriptorPoolState> builtin_pool_;
};

class PreparedExpressionState : public GenericState {
 public:
  PreparedExpressionState() {}
  PreparedExpressionState(const PreparedExpressionState&) = delete;
  PreparedExpressionState& operator=(const PreparedExpressionState&) = delete;

  absl::Status InitAndDeserializeOptionsWithPools(
      const std::string& sql, const AnalyzerOptionsProto& proto,
      std::vector<const google::protobuf::DescriptorPool*> pools,
      absl::flat_hash_set<int64_t> owned_descriptor_pool_ids = {},
      absl::optional<int64_t> owned_catalog_id = absl::nullopt) {
    absl::MutexLock lock(&mutex_);
    ZETASQL_RETURN_IF_ERROR(
        AnalyzerOptions::Deserialize(proto, pools, &factory_, &options_));
    zetasql::EvaluatorOptions evaluator_options;
    evaluator_options.type_factory = &factory_;
    evaluator_options.default_time_zone = options_.default_time_zone();
    exp_ = absl::make_unique<PreparedExpression>(sql, evaluator_options);
    owned_descriptor_pool_ids_ = std::move(owned_descriptor_pool_ids);
    owned_catalog_id_ = owned_catalog_id;
    initialized_ = true;
    return absl::OkStatus();
  }

  PreparedExpression* GetPreparedExpression() {
    absl::MutexLock lock(&mutex_);
    ZETASQL_CHECK(initialized_);

    return exp_.get();
  }

  const AnalyzerOptions& GetAnalyzerOptions() {
    absl::MutexLock lock(&mutex_);
    ZETASQL_CHECK(initialized_);

    return options_;
  }

  absl::flat_hash_set<int64_t> owned_descriptor_pool_ids() {
    absl::MutexLock lock(&mutex_);
    ZETASQL_CHECK(initialized_);
    return owned_descriptor_pool_ids_;
  }

  absl::optional<int64_t> owned_catalog_id() {
    absl::MutexLock lock(&mutex_);
    ZETASQL_CHECK(initialized_);
    return owned_catalog_id_;
  }

 private:
  absl::Mutex mutex_;

  bool initialized_ ABSL_GUARDED_BY(mutex_);

  TypeFactory factory_ ABSL_GUARDED_BY(mutex_);
  // TODO: Restructure API so we can drop the mutexes.
  std::unique_ptr<PreparedExpression> exp_ ABSL_GUARDED_BY(mutex_);
  AnalyzerOptions options_ ABSL_GUARDED_BY(mutex_);
  // Descriptor pools that are owned by this PreparedExpression, and should
  // be deleted when this object is deleted.
  absl::flat_hash_set<int64_t> owned_descriptor_pool_ids_
      ABSL_GUARDED_BY(mutex_);
  absl::optional<int64_t> owned_catalog_id_ ABSL_GUARDED_BY(mutex_);
};

class PreparedExpressionPool : public SharedStatePool<PreparedExpressionState> {
};

class RegisteredCatalogState : public GenericState {
 public:
  RegisteredCatalogState() {}
  RegisteredCatalogState(const RegisteredCatalogState&) = delete;
  RegisteredCatalogState& operator=(const RegisteredCatalogState&) = delete;

  absl::Status InitWithPools(
      const SimpleCatalogProto& proto,
      const std::vector<const google::protobuf::DescriptorPool*>& pools,
      absl::flat_hash_set<int64_t> owned_descriptor_pool_ids = {}) {
    absl::MutexLock lock(&mutex_);
    ZETASQL_RETURN_IF_ERROR(SimpleCatalog::Deserialize(proto, pools, &catalog_));
    owned_descriptor_pool_ids_ = std::move(owned_descriptor_pool_ids);
    initialized_ = true;
    return absl::OkStatus();
  }

  SimpleCatalog* GetCatalog() {
    absl::MutexLock lock(&mutex_);
    ZETASQL_CHECK(initialized_);
    return catalog_.get();
  }

  const absl::flat_hash_set<int64_t>& owned_descriptor_pool_ids() {
    return owned_descriptor_pool_ids_;
  }

 private:
  absl::Mutex mutex_;

  bool initialized_ ABSL_GUARDED_BY(mutex_);

  std::unique_ptr<SimpleCatalog> catalog_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_set<int64_t> owned_descriptor_pool_ids_;
};

class RegisteredCatalogPool : public SharedStatePool<RegisteredCatalogState> {};

ZetaSqlLocalServiceImpl::ZetaSqlLocalServiceImpl()
    : registered_descriptor_pools_(new RegisteredDescriptorPoolPool()),
      registered_catalogs_(new RegisteredCatalogPool()),
      prepared_expressions_(new PreparedExpressionPool()) {}

ZetaSqlLocalServiceImpl::~ZetaSqlLocalServiceImpl() {}

void ZetaSqlLocalServiceImpl::CleanupCatalog(
    absl::optional<int64_t>* catalog_id) {
  if (catalog_id->has_value()) {
    registered_catalogs_->Delete(**catalog_id);
  }
}

void ZetaSqlLocalServiceImpl::CleanupDescriptorPools(
    absl::flat_hash_set<int64_t>* descriptor_pool_ids) {
  for (int64_t pool_id : *descriptor_pool_ids) {
    registered_descriptor_pools_->Delete(pool_id);
  }
}

absl::Status ZetaSqlLocalServiceImpl::RegisterNewDescriptorPools(
    std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>&
        descriptor_pool_states,
    absl::flat_hash_set<int64_t>& registered_descriptor_pool_ids,
    DescriptorPoolIdList& descriptor_pool_id_list) {
  registered_descriptor_pool_ids.clear();
  descriptor_pool_id_list.Clear();
  for (std::shared_ptr<RegisteredDescriptorPoolState>& pool_state :
       descriptor_pool_states) {
    if (!pool_state->IsRegistered()) {
      // Not registered, so we registered it, and own it.
      int64_t pool_id = registered_descriptor_pools_->Register(pool_state);
      ZETASQL_RET_CHECK_NE(-1, pool_id)
          << "Failed to register descriptor pool, this shouldn't happen";
      registered_descriptor_pool_ids.insert(pool_id);
    }
    descriptor_pool_id_list.add_registered_ids(pool_state->GetId());
  }
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::Prepare(const PrepareRequest& request,
                                                PrepareResponse* response) {
  std::shared_ptr<PreparedExpressionState> state =
      std::make_shared<PreparedExpressionState>();
  std::shared_ptr<RegisteredCatalogState> catalog_state;
  std::vector<const google::protobuf::DescriptorPool*> pools;

  std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>
      descriptor_pool_states;
  ZETASQL_RETURN_IF_ERROR(GetDescriptorPools(request.descriptor_pool_list(),
                                     descriptor_pool_states, pools));

  absl::flat_hash_set<int64_t> owned_descriptor_pool_ids;
  // On error, make sure we don't leak any registered descriptor pools.
  auto descriptor_pool_cleanup = absl::MakeCleanup(
      absl::bind_front(&ZetaSqlLocalServiceImpl::CleanupDescriptorPools, this,
                       &owned_descriptor_pool_ids));
  ZETASQL_RETURN_IF_ERROR(RegisterNewDescriptorPools(
      descriptor_pool_states, owned_descriptor_pool_ids,
      *(response->mutable_prepared()->mutable_descriptor_pool_id_list())));

  ZETASQL_RETURN_IF_ERROR(GetCatalogState(request, pools, catalog_state));
  std::optional<int64_t> owned_catalog_id;
  auto catalog_cleanup = absl::MakeCleanup(absl::bind_front(
      &ZetaSqlLocalServiceImpl::CleanupCatalog, this, &owned_catalog_id));

  if (catalog_state != nullptr && !catalog_state->IsRegistered()) {
    owned_catalog_id = registered_catalogs_->Register(catalog_state);
    ZETASQL_RET_CHECK_NE(-1, owned_catalog_id.value())
        << "Failed to register catalog, this shouldn't happen";
  }
  ZETASQL_RETURN_IF_ERROR(state->InitAndDeserializeOptionsWithPools(
      request.sql(), request.options(), pools, owned_descriptor_pool_ids,
      owned_catalog_id));
  ZETASQL_RETURN_IF_ERROR(state->GetPreparedExpression()->Prepare(
      state->GetAnalyzerOptions(),
      catalog_state != nullptr ? catalog_state->GetCatalog() : nullptr));
  ZETASQL_RETURN_IF_ERROR(RegisterPrepared(state, pools, response->mutable_prepared()));

  // No errors, caller is now responsible for the prepared expression and
  // therefore any owned descriptor pools.
  std::move(catalog_cleanup).Cancel();
  std::move(descriptor_pool_cleanup).Cancel();
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::RegisterPrepared(
    std::shared_ptr<PreparedExpressionState>& state,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    PreparedState* response) {
  PreparedExpression* exp = state->GetPreparedExpression();

  ZETASQL_RETURN_IF_ERROR(SerializeTypeUsingExistingPools(
      exp->output_type(), pools, response->mutable_output_type()));

  ZETASQL_ASSIGN_OR_RETURN(auto columns, exp->GetReferencedColumns());
  for (const std::string& column_name : columns) {
    response->add_referenced_columns(column_name);
  }

  ZETASQL_ASSIGN_OR_RETURN(auto parameters, exp->GetReferencedParameters());
  for (const std::string& parameter_name : parameters) {
    response->add_referenced_parameters(parameter_name);
  }

  ZETASQL_ASSIGN_OR_RETURN(auto parameter_count, exp->GetPositionalParameterCount());
  response->set_positional_parameter_count(parameter_count);

  int64_t id = prepared_expressions_->Register(state);
  ZETASQL_RET_CHECK_NE(-1, id)
      << "Failed to register prepared state, this shouldn't happen.";

  response->set_prepared_expression_id(id);
  if (response->descriptor_pool_id_list().registered_ids_size() == 0) {
    response->clear_descriptor_pool_id_list();
  }

  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::Unprepare(int64_t id) {
  std::shared_ptr<PreparedExpressionState> state =
      prepared_expressions_->Get(id);
  if (state == nullptr) {
    return MakeSqlError() << "Unknown prepared expression ID: " << id;
  }

  // This will only capture the 'last' error we encounter, but since any error
  // would indicate some sort of horrible internal state error, that's
  // probably okay.
  absl::Status status;
  for (int64_t pool_id : state->owned_descriptor_pool_ids()) {
    if (!registered_descriptor_pools_->Delete(pool_id)) {
      status = MakeSqlError() << "Unknown descriptor pool ID: " << pool_id;
    }
  }
  if (state->owned_catalog_id().has_value()) {
    int64_t owned_catalog_id = state->owned_catalog_id().value();
    if (!registered_catalogs_->Delete(owned_catalog_id)) {
      status = MakeSqlError() << "Unknown catalog ID: " << owned_catalog_id;
    }
  }

  if (!prepared_expressions_->Delete(id)) {
    status = MakeSqlError() << "Unknown prepared expression ID: " << id;
  }
  return status;
}

absl::Status ZetaSqlLocalServiceImpl::Evaluate(const EvaluateRequest& request,
                                                 EvaluateResponse* response) {
  bool prepared = request.has_prepared_expression_id();
  std::shared_ptr<PreparedExpressionState> state;
  std::vector<const google::protobuf::DescriptorPool*> pools;
  std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>
      descriptor_pool_states;
  absl::flat_hash_set<int64_t> owned_descriptor_pool_ids;
  // On error, make sure we don't leak any registered descriptor pools.
  auto descriptor_pool_cleanup = absl::MakeCleanup(
      absl::bind_front(&ZetaSqlLocalServiceImpl::CleanupDescriptorPools, this,
                       &owned_descriptor_pool_ids));
  if (prepared) {
    // Descriptor pools should only be transmitted during prepare (or the
    // the first call to evaluate, which is implicitly a Prepare).
    ZETASQL_RET_CHECK_EQ(request.descriptor_pool_list().definitions_size(), 0);
    int64_t id = request.prepared_expression_id();
    state = prepared_expressions_->Get(id);
    if (state == nullptr) {
      return MakeSqlError() << "Prepared expression " << id << " unknown.";
    }
  } else {
    ZETASQL_RETURN_IF_ERROR(GetDescriptorPools(request.descriptor_pool_list(),
                                       descriptor_pool_states, pools));

    ZETASQL_RETURN_IF_ERROR(RegisterNewDescriptorPools(
        descriptor_pool_states, owned_descriptor_pool_ids,
        *(response->mutable_prepared()->mutable_descriptor_pool_id_list())));
    state = std::make_shared<PreparedExpressionState>();
    ZETASQL_RETURN_IF_ERROR(state->InitAndDeserializeOptionsWithPools(
        request.sql(), request.options(), pools, owned_descriptor_pool_ids,
        /*owned_catalog_id=*/std::nullopt));
    if (request.has_options()) {
      // PreparedExpression::Prepare must be invoked if we need to supply
      // analyzer options.
      ZETASQL_RETURN_IF_ERROR(state->GetPreparedExpression()->Prepare(
          state->GetAnalyzerOptions(), nullptr));
    }
  }
  ZETASQL_RETURN_IF_ERROR(EvaluateImpl(request, state.get(), response));

  if (!prepared) {
    ZETASQL_RETURN_IF_ERROR(
        RegisterPrepared(state, pools, response->mutable_prepared()));
  }
  // No errors, caller is now responsible for the prepared expression and
  // therefore any owned descriptor pools.
  std::move(descriptor_pool_cleanup).Cancel();
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::EvaluateImpl(
    const EvaluateRequest& request, PreparedExpressionState* state,
    EvaluateResponse* response) {
  const AnalyzerOptions& analyzer_options = state->GetAnalyzerOptions();

  ParameterValueMap columns, params;
  ZETASQL_RETURN_IF_ERROR(RepeatedParametersToMap(
      request.columns(), analyzer_options.expression_columns(), &columns));
  ZETASQL_RETURN_IF_ERROR(RepeatedParametersToMap(
      request.params(), analyzer_options.query_parameters(), &params));

  auto result = state->GetPreparedExpression()->Execute(columns, params);
  ZETASQL_RETURN_IF_ERROR(result.status());

  const Value& value = result.value();
  ZETASQL_RETURN_IF_ERROR(value.Serialize(response->mutable_value()));

  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::GetTableFromProto(
    const TableFromProtoRequest& request, SimpleTableProto* response) {
  TypeFactory factory;
  google::protobuf::DescriptorPool pool;
  ZETASQL_RETURN_IF_ERROR(
      AddFileDescriptorSetToPool(&request.file_descriptor_set(), &pool));
  const google::protobuf::Descriptor* proto_descr =
      pool.FindMessageTypeByName(request.proto().proto_name());
  if (proto_descr == nullptr) {
    return ::zetasql_base::UnknownErrorBuilder()
           << "Proto type name not found: " << request.proto().proto_name();
  }
  if (proto_descr->file()->name() != request.proto().proto_file_name()) {
    return ::zetasql_base::UnknownErrorBuilder()
           << "Proto " << request.proto().proto_name() << " found in "
           << proto_descr->file()->name() << ", not "
           << request.proto().proto_file_name() << " as specified.";
  }
  TableFromProto table(proto_descr->name());
  ZETASQL_RETURN_IF_ERROR(table.Init(proto_descr, &factory));
  FileDescriptorSetMap file_descriptor_set_map;
  ZETASQL_RETURN_IF_ERROR(table.Serialize(&file_descriptor_set_map, response));
  if (!file_descriptor_set_map.empty()) {
    ZETASQL_RET_CHECK_EQ(1, file_descriptor_set_map.size())
        << "Table from proto " << proto_descr->full_name()
        << " uses unknown DescriptorPool, this shouldn't happen.";
    ZETASQL_RET_CHECK_EQ(0, file_descriptor_set_map.at(&pool)->descriptor_set_index)
        << "Table from proto " << proto_descr->full_name()
        << " uses unknown DescriptorPool, this shouldn't happen.";
  }
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::GetDescriptorPools(
    const DescriptorPoolListProto& descriptor_pool_list,
    std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>&
        descriptor_pool_states,
    std::vector<const google::protobuf::DescriptorPool*>& descriptor_pools) {
  using Definition = DescriptorPoolListProto::Definition;
  descriptor_pool_states.clear();
  descriptor_pools.clear();
  for (const Definition& definition : descriptor_pool_list.definitions()) {
    std::shared_ptr<RegisteredDescriptorPoolState> state;
    switch (definition.definition_case()) {
      case Definition::kFileDescriptorSet: {
        state = std::make_shared<RegisteredDescriptorPoolState>();
        ZETASQL_RETURN_IF_ERROR(state->Init(definition.file_descriptor_set()));
        break;
      }
      case Definition::kRegisteredId: {
        state = registered_descriptor_pools_->Get(definition.registered_id());
        if (state == nullptr) {
          return absl::Status(
              absl::StatusCode::kInvalidArgument,
              absl::StrCat("Invalid DescriptorPoolList::Definition: unknown "
                           "registered_id",
                           definition.DebugString()));
        }
        break;
      }
      case Definition::kBuiltin: {
        state = registered_descriptor_pools_->GetBuiltinDescriptorPoolState();
        break;
      }
      default:
        return absl::Status(
            absl::StatusCode::kInvalidArgument,
            absl::StrCat(
                "Invalid DescriptorPoolList::Definition contains unknown "
                "definition type",
                definition.DebugString()));
    }
    descriptor_pool_states.push_back(state);
    ZETASQL_RET_CHECK_NE(state->pool(), nullptr);
    descriptor_pools.push_back(state->pool());
  }

  return absl::OkStatus();
}

template <typename RequestProto>
absl::Status ZetaSqlLocalServiceImpl::GetCatalogState(
    const RequestProto& request,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    std::shared_ptr<RegisteredCatalogState>& state) {
  if (request.has_registered_catalog_id()) {
    int64_t id = request.registered_catalog_id();
    state = registered_catalogs_->Get(id);
    if (state == nullptr) {
      return MakeSqlError() << "Registered catalog " << id << " unknown.";
    }
  } else {
    state = std::make_shared<RegisteredCatalogState>();
    ZETASQL_RETURN_IF_ERROR(state->InitWithPools(request.simple_catalog(), pools));
  }
  return absl::OkStatus();
}

zetasql_base::StatusOr<std::vector<const google::protobuf::DescriptorPool*>>
ToDescriptorPoolVector(
    const std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>& states) {
  std::vector<const google::protobuf::DescriptorPool*> pools;
  pools.reserve(states.size());
  for (const auto& state : states) {
    pools.push_back(state->pool());
    ZETASQL_RET_CHECK_NE(state->pool(), nullptr);
  }
  return pools;
}

absl::Status ZetaSqlLocalServiceImpl::Analyze(const AnalyzeRequest& request,
                                                AnalyzeResponse* response) {
  std::shared_ptr<RegisteredCatalogState> catalog_state;
  std::vector<const google::protobuf::DescriptorPool*> pools;
  std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>
      descriptor_pool_states;

  ZETASQL_RETURN_IF_ERROR(GetDescriptorPools(request.descriptor_pool_list(),
                                     descriptor_pool_states, pools));
  ZETASQL_RETURN_IF_ERROR(GetCatalogState(request, pools, catalog_state));
  if (request.has_sql_expression()) {
    return AnalyzeExpressionImpl(request, pools, catalog_state->GetCatalog(),
                                 response);
  } else {
    return AnalyzeImpl(request, pools, catalog_state->GetCatalog(), response);
  }
}

absl::Status ZetaSqlLocalServiceImpl::AnalyzeImpl(
    const AnalyzeRequest& request,
    const std::vector<const google::protobuf::DescriptorPool*>& pools, Catalog* catalog,
    AnalyzeResponse* response) {
  AnalyzerOptions options;
  TypeFactory factory;
  ZETASQL_RETURN_IF_ERROR(AnalyzerOptions::Deserialize(request.options(), pools,
                                               &factory, &options));

  if (!(request.has_sql_statement() || request.has_parse_resume_location())) {
    return ::zetasql_base::UnknownErrorBuilder()
           << "Unrecognized AnalyzeRequest target " << request.target_case();
  }
  std::unique_ptr<const AnalyzerOutput> output;

  if (request.has_sql_statement()) {
    const std::string& sql = request.sql_statement();

    ZETASQL_RETURN_IF_ERROR(
        zetasql::AnalyzeStatement(sql, options, catalog, &factory, &output));

    ZETASQL_RETURN_IF_ERROR(
        SerializeResolvedOutput(output.get(), pools, sql, response));
  } else if (request.has_parse_resume_location()) {
    bool at_end_of_input;
    ParseResumeLocation location =
        ParseResumeLocation::FromProto(request.parse_resume_location());
    ZETASQL_RETURN_IF_ERROR(zetasql::AnalyzeNextStatement(
        &location, options, catalog, &factory, &output, &at_end_of_input));

    ZETASQL_RETURN_IF_ERROR(SerializeResolvedOutput(output.get(), pools,
                                            location.input(), response));
    response->set_resume_byte_position(location.byte_position());
  }
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::AnalyzeExpressionImpl(
    const AnalyzeRequest& request,
    const std::vector<const google::protobuf::DescriptorPool*>& pools, Catalog* catalog,
    AnalyzeResponse* response) {
  AnalyzerOptions options;
  TypeFactory factory;
  ZETASQL_RETURN_IF_ERROR(AnalyzerOptions::Deserialize(request.options(), pools,
                                               &factory, &options));

  if (request.has_sql_expression()) {
    std::unique_ptr<const AnalyzerOutput> output;
    TypeFactory factory;

    const std::string& sql = request.sql_expression();

    ZETASQL_RETURN_IF_ERROR(
        zetasql::AnalyzeExpression(sql, options, catalog, &factory, &output));

    ZETASQL_RETURN_IF_ERROR(
        SerializeResolvedOutput(output.get(), pools, sql, response));
  }
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::BuildSql(const BuildSqlRequest& request,
                                                 BuildSqlResponse* response) {
  std::shared_ptr<RegisteredCatalogState> catalog_state;
  std::vector<const google::protobuf::DescriptorPool*> pools;

  std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>
      descriptor_pool_states;

  ZETASQL_RETURN_IF_ERROR(GetDescriptorPools(request.descriptor_pool_list(),
                                     descriptor_pool_states, pools));
  ZETASQL_RETURN_IF_ERROR(GetCatalogState(request, pools, catalog_state));
  IdStringPool string_pool;
  ResolvedNode::RestoreParams restore_params(
      pools, catalog_state->GetCatalog(),
      catalog_state->GetCatalog()->type_factory(), &string_pool);

  std::unique_ptr<ResolvedNode> ast;
  if (request.has_resolved_statement()) {
    ast = std::move(ResolvedStatement::RestoreFrom(request.resolved_statement(),
                                                   restore_params)
                        .value());
  } else if (request.has_resolved_expression()) {
    ast = std::move(
        ResolvedExpr::RestoreFrom(request.resolved_expression(), restore_params)
            .value());
  } else {
    return absl::OkStatus();
  }

  zetasql::SQLBuilder sql_builder;
  ZETASQL_CHECK_OK(ast->Accept(&sql_builder));
  response->set_sql(sql_builder.sql());
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::ExtractTableNamesFromStatement(
    const ExtractTableNamesFromStatementRequest& request,
    ExtractTableNamesFromStatementResponse* response) {
  LanguageOptions language_options = request.has_options()
                                         ? LanguageOptions(request.options())
                                         : LanguageOptions();

  zetasql::TableNamesSet table_names;
  if (request.allow_script()) {
    ZETASQL_RETURN_IF_ERROR(zetasql::ExtractTableNamesFromScript(
        request.sql_statement(), zetasql::AnalyzerOptions(language_options),
        &table_names));
  } else {
    ZETASQL_RETURN_IF_ERROR(zetasql::ExtractTableNamesFromStatement(
        request.sql_statement(), zetasql::AnalyzerOptions(language_options),
        &table_names));
  }
  for (const std::vector<std::string>& table_name : table_names) {
    ExtractTableNamesFromStatementResponse_TableName* table_name_field =
        response->add_table_name();
    for (const std::string& name_segment : table_name) {
      table_name_field->add_table_name_segment(name_segment);
    }
  }
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::ExtractTableNamesFromNextStatement(
    const ExtractTableNamesFromNextStatementRequest& request,
    ExtractTableNamesFromNextStatementResponse* response) {
  ParseResumeLocation location =
      ParseResumeLocation::FromProto(request.parse_resume_location());

  LanguageOptions language_options = request.has_options() ?
      LanguageOptions(request.options()) :
      LanguageOptions();

  bool at_end_of_input;
  zetasql::TableNamesSet table_names;
  ZETASQL_RETURN_IF_ERROR(zetasql::ExtractTableNamesFromNextStatement(
      &location, zetasql::AnalyzerOptions(language_options), &table_names,
      &at_end_of_input));

  for (const std::vector<std::string>& table_name : table_names) {
    ExtractTableNamesFromNextStatementResponse_TableName* table_name_field =
        response->add_table_name();
    for (const std::string& name_segment : table_name) {
      table_name_field->add_table_name_segment(name_segment);
    }
  }

  response->set_resume_byte_position(location.byte_position());

  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::SerializeResolvedOutput(
    const AnalyzerOutput* output,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    absl::string_view statement, AnalyzeResponse* response) {
  FileDescriptorSetMap file_descriptor_set_map;
  PopulateExistingPoolsToFileDescriptorSetMap(pools, &file_descriptor_set_map);

  if (output->resolved_statement() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(output->resolved_statement()->SaveTo(
        &file_descriptor_set_map, response->mutable_resolved_statement()));
  } else {
    ZETASQL_RETURN_IF_ERROR(output->resolved_expr()->SaveTo(
        &file_descriptor_set_map, response->mutable_resolved_expression()));
  }

  // If the file_descriptor_set_map contains more descriptor pools than those
  // passed in the request, the additonal one must be the generated descriptor
  // pool. The reason is that some built-in functions use the DatetimePart
  // enum whose descriptor comes from the generated pool.
  // TODO: Describe the descriptor pool passing contract in detail
  // with a doc, and put a link here.
  if (file_descriptor_set_map.size() != pools.size()) {
    ZETASQL_RET_CHECK_EQ(file_descriptor_set_map.size(), pools.size() + 1)
        << "Analyzer result of " << statement
        << " uses unknown DescriptorPool, this shouldn't happen.";
    const auto& entry =
        file_descriptor_set_map.at(google::protobuf::DescriptorPool::generated_pool());
    ZETASQL_RET_CHECK_NE(entry.get(), nullptr)
        << "Analyzer result of " << statement
        << " uses unknown DescriptorPool, this shouldn't happen.";
    ZETASQL_RET_CHECK_EQ(entry->descriptor_set_index, pools.size())
        << "Analyzer result of " << statement
        << " uses unknown DescriptorPool, this shouldn't happen.";
  }

  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::FormatSql(
    const FormatSqlRequest& request, FormatSqlResponse* response) {
  return ::zetasql::FormatSql(request.sql(), response->mutable_sql());
}

absl::Status ZetaSqlLocalServiceImpl::RegisterCatalog(
    const RegisterCatalogRequest& request, RegisterResponse* response) {
  std::unique_ptr<RegisteredCatalogState> state(new RegisteredCatalogState());

  std::vector<std::shared_ptr<RegisteredDescriptorPoolState>>
      descriptor_pool_states;
  std::vector<const google::protobuf::DescriptorPool*> pools;

  ZETASQL_RETURN_IF_ERROR(GetDescriptorPools(request.descriptor_pool_list(),
                                     descriptor_pool_states, pools));

  absl::flat_hash_set<int64_t> owned_descriptor_pool_ids;
  // On error, make sure we don't leak any registered descriptor pools.
  auto descriptor_pool_cleanup = absl::MakeCleanup(
      absl::bind_front(&ZetaSqlLocalServiceImpl::CleanupDescriptorPools, this,
                       &owned_descriptor_pool_ids));
  for (std::shared_ptr<RegisteredDescriptorPoolState>& pool_state :
       descriptor_pool_states) {
    if (!pool_state->IsRegistered()) {
      // Not registered, so we registered it, and own it.
      int64_t pool_id = registered_descriptor_pools_->Register(pool_state);
      ZETASQL_RET_CHECK_NE(-1, pool_id)
          << "Failed to register descriptor pool, this shouldn't happen";
      owned_descriptor_pool_ids.insert(pool_id);
    }
    response->mutable_descriptor_pool_id_list()->add_registered_ids(
        pool_state->GetId());
  }

  ZETASQL_RETURN_IF_ERROR(state->InitWithPools(request.simple_catalog(), pools,
                                       owned_descriptor_pool_ids));
  int64_t id = registered_catalogs_->Register(std::move(state));
  ZETASQL_RET_CHECK_NE(-1, id) << "Failed to register catalog, this shouldn't happen.";

  response->set_registered_id(id);
  // No errors, caller is now responsible for the prepared expression and
  // therefore any owned descriptor pools.
  std::move(descriptor_pool_cleanup).Cancel();

  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::UnregisterCatalog(int64_t id) {
  std::shared_ptr<RegisteredCatalogState> state = registered_catalogs_->Get(id);
  if (state == nullptr) {
    return MakeSqlError() << "Unknown catalog ID: " << id;
  }

  absl::Status status;
  for (int64_t pool_id : state->owned_descriptor_pool_ids()) {
    if (!registered_descriptor_pools_->Delete(pool_id)) {
      status = MakeSqlError() << "Unknown descriptor pool ID: " << pool_id;
    }
  }
  if (!registered_catalogs_->Delete(id)) {
    status = MakeSqlError() << "Failed to fully delete catalog ID: " << id;
  }
  return status;
}

absl::Status ZetaSqlLocalServiceImpl::GetBuiltinFunctions(
    const ZetaSQLBuiltinFunctionOptionsProto& proto,
    GetBuiltinFunctionsResponse* resp) {
  TypeFactory factory;
  std::map<std::string, std::unique_ptr<Function>> functions;
  ZetaSQLBuiltinFunctionOptions options(proto);

  zetasql::GetZetaSQLFunctions(&factory, options, &functions);

  FileDescriptorSetMap map;
  for (const auto& function : functions) {
    ZETASQL_RETURN_IF_ERROR(function.second->Serialize(&map, resp->add_function()));
  }

  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::GetLanguageOptions(
    const LanguageOptionsRequest& request, LanguageOptionsProto* response) {
  zetasql::LanguageOptions options;
  if (request.has_maximum_features() && request.maximum_features()) {
    options.EnableMaximumLanguageFeatures();
  }
  if (request.has_language_version()) {
    options.SetLanguageVersion(request.language_version());
  }
  options.Serialize(response);
  return absl::OkStatus();
}

absl::Status ZetaSqlLocalServiceImpl::GetAnalyzerOptions(
    const AnalyzerOptionsRequest& request, AnalyzerOptionsProto* response) {
  zetasql::AnalyzerOptions options;
  FileDescriptorSetMap unused_map;
  return options.Serialize(&unused_map, response);
}

size_t ZetaSqlLocalServiceImpl::NumRegisteredDescriptorPools() const {
  return registered_descriptor_pools_->NumSavedStates();
}

size_t ZetaSqlLocalServiceImpl::NumRegisteredCatalogs() const {
  return registered_catalogs_->NumSavedStates();
}

size_t ZetaSqlLocalServiceImpl::NumSavedPreparedExpression() const {
  return prepared_expressions_->NumSavedStates();
}

}  // namespace local_service
}  // namespace zetasql
