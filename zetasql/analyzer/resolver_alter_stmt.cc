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

// This file contains the implementation of ALTER related resolver
// methods from resolver.h.
#include <map>
#include <memory>
#include <utility>

#include "zetasql/base/logging.h"
#include "zetasql/analyzer/name_scope.h"
#include "zetasql/analyzer/resolver.h"
#include "zetasql/parser/ast_node_kind.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parse_tree_errors.h"
#include "zetasql/public/id_string.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/strings.h"
#include "zetasql/public/types/type.h"
#include "zetasql/public/types/type_parameters.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "zetasql/base/statusor.h"
#include "absl/strings/string_view.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

namespace {
bool OptionsPresent(const ResolvedColumnAnnotations* annotations) {
  if (annotations != nullptr) {
    if (annotations->option_list_size() > 0) {
      return true;
    }
    for (int i = 0; i < annotations->child_list_size(); ++i) {
      if (OptionsPresent(annotations->child_list(i))) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace

absl::Status Resolver::ResolveAlterActions(
    const ASTAlterStatementBase* ast_statement,
    absl::string_view alter_statement_kind,
    std::unique_ptr<ResolvedStatement>* output,
    bool* has_only_set_options_action,
    std::vector<std::unique_ptr<const ResolvedAlterAction>>* alter_actions) {
  ZETASQL_RET_CHECK(ast_statement->path() != nullptr);
  const IdString table_name_id_string =
      MakeIdString(ast_statement->path()->ToIdentifierPathString());

  IdStringSetCase new_columns, column_to_drop;
  const Table* altered_table = nullptr;
  bool existing_rename_to_action = false;
  // Some engines do not add all the referenced tables into the catalog. Thus,
  // if the lookup here fails it does not necessarily mean that the table does
  // not exist.
  absl::Status table_status = FindTable(ast_statement->path(), &altered_table);

  *has_only_set_options_action = true;
  bool already_added_primary_key = false;
  const ASTAlterActionList* action_list = ast_statement->action_list();
  for (const ASTAlterAction* const action : action_list->actions()) {
    if (action->node_kind() != AST_SET_OPTIONS_ACTION) {
      *has_only_set_options_action = false;
    }
    switch (action->node_kind()) {
      case AST_SET_OPTIONS_ACTION: {
        std::vector<std::unique_ptr<const ResolvedOption>> resolved_options;
        ZETASQL_RETURN_IF_ERROR(ResolveOptionsList(
            action->GetAsOrDie<ASTSetOptionsAction>()->options_list(),
            &resolved_options));
        alter_actions->push_back(
            MakeResolvedSetOptionsAction(std::move(resolved_options)));
      } break;
      case AST_ADD_CONSTRAINT_ACTION:
      case AST_DROP_CONSTRAINT_ACTION: {
        if (!ast_statement->is_if_exists()) {
          ZETASQL_RETURN_IF_ERROR(table_status);
        }
        if (action->node_kind() == AST_ADD_CONSTRAINT_ACTION) {
          const auto* constraint = action->GetAsOrDie<ASTAddConstraintAction>();
          auto constraint_kind = constraint->constraint()->node_kind();
          if (constraint_kind == AST_PRIMARY_KEY) {
            if (already_added_primary_key) {
              return MakeSqlErrorAt(action)
                     << "ALTER TABLE only supports one ADD PRIMARY KEY action";
            }
            already_added_primary_key = true;
          }

          std::unique_ptr<const ResolvedAddConstraintAction>
              resolved_alter_action;
          ZETASQL_RETURN_IF_ERROR(ResolveAddConstraintAction(altered_table,
                                                     ast_statement, constraint,
                                                     &resolved_alter_action));
          alter_actions->push_back(std::move(resolved_alter_action));
        } else {
          const auto* constraint =
              action->GetAsOrDie<ASTDropConstraintAction>();
          alter_actions->push_back(MakeResolvedDropConstraintAction(
              constraint->is_if_exists(),
              constraint->constraint_name()->GetAsString()));
        }
      } break;
      case AST_DROP_PRIMARY_KEY_ACTION: {
        if (!ast_statement->is_if_exists()) {
          ZETASQL_RETURN_IF_ERROR(table_status);
        }
        alter_actions->push_back(MakeResolvedDropPrimaryKeyAction(
            action->GetAsOrDie<ASTDropPrimaryKeyAction>()->is_if_exists()));
      } break;
      case AST_ALTER_CONSTRAINT_ENFORCEMENT_ACTION:
        return MakeSqlErrorAt(action)
               << "ALTER CONSTRAINT ENFORCED/NOT ENFORCED is not supported";
      case AST_ALTER_CONSTRAINT_SET_OPTIONS_ACTION:
        return MakeSqlErrorAt(action)
               << "ALTER CONSTRAINT SET OPTIONS is not supported";
      case AST_ADD_COLUMN_ACTION:
      case AST_DROP_COLUMN_ACTION:
      case AST_ALTER_COLUMN_TYPE_ACTION: {
        if (ast_statement->node_kind() != AST_ALTER_TABLE_STATEMENT) {
          // Views, models, etc don't support ADD/DROP/SET DATA TYPE columns.
          return MakeSqlErrorAt(action)
                 << "ALTER " << alter_statement_kind << " does not support "
                 << action->GetSQLForAlterAction();
        }
        if (!ast_statement->is_if_exists()) {
          ZETASQL_RETURN_IF_ERROR(table_status);
        }
        std::unique_ptr<const ResolvedAlterAction> resolved_action;
        if (action->node_kind() == AST_ADD_COLUMN_ACTION) {
          ZETASQL_RETURN_IF_ERROR(ResolveAddColumnAction(
              table_name_id_string, altered_table,
              action->GetAsOrDie<ASTAddColumnAction>(), &new_columns,
              &column_to_drop, &resolved_action));
        } else if (action->node_kind() == AST_DROP_COLUMN_ACTION) {
          ZETASQL_RETURN_IF_ERROR(ResolveDropColumnAction(
              table_name_id_string, altered_table,
              action->GetAsOrDie<ASTDropColumnAction>(), &new_columns,
              &column_to_drop, &resolved_action));
        } else if (language().LanguageFeatureEnabled(
                       FEATURE_ALTER_COLUMN_SET_DATA_TYPE)) {
          ZETASQL_RETURN_IF_ERROR(ResolveAlterColumnTypeAction(
              table_name_id_string, altered_table,
              action->GetAsOrDie<ASTAlterColumnTypeAction>(),
              &resolved_action));
        } else {
          return MakeSqlErrorAt(action)
                 << "ALTER " << alter_statement_kind << " does not support "
                 << action->GetSQLForAlterAction();
        }
        alter_actions->push_back(std::move(resolved_action));
      } break;
      case AST_SET_AS_ACTION: {
        if (ast_statement->node_kind() != AST_ALTER_ENTITY_STATEMENT) {
          return MakeSqlErrorAt(action)
                 << "ALTER " << alter_statement_kind << " does not support "
                 << action->GetSQLForAlterAction();
        }
        const auto* set_as_action = action->GetAsOrDie<ASTSetAsAction>();
        std::string entity_body_json;
        if (set_as_action->json_body() != nullptr) {
          // TODO: Use ResolveExpr() after JSON type goes public.
          ZETASQL_ASSIGN_OR_RETURN(auto json_literal,
                           ResolveJsonLiteral(set_as_action->json_body()));
          entity_body_json = json_literal->value().json_string();
        }
        std::string entity_body_text;
        if (set_as_action->text_body() != nullptr) {
          entity_body_text = set_as_action->text_body()->string_value();
        }
        if (entity_body_json.empty() && entity_body_text.empty()) {
          return MakeSqlErrorAt(action)
                 << "ALTER SET AS requires JSON or TEXT body literal";
        }
        if (!entity_body_text.empty() && !entity_body_json.empty()) {
          return MakeSqlErrorAt(ast_statement)
                 << "ALTER SET AS should have exactly one JSON or TEXT body "
                    "literal";
        }
        std::unique_ptr<const ResolvedAlterAction> resolved_action =
            MakeResolvedSetAsAction(entity_body_json, entity_body_text);
        alter_actions->push_back(std::move(resolved_action));
      } break;
      case AST_RENAME_TO_CLAUSE: {
        if (ast_statement->node_kind() != AST_ALTER_TABLE_STATEMENT) {
          // only rename table is supported
          return MakeSqlErrorAt(action)
                 << "ALTER " << alter_statement_kind << " does not support "
                 << action->GetSQLForAlterAction();
        }
        if (existing_rename_to_action) {
          return MakeSqlErrorAt(action)
                 << "Multiple RENAME TO actions are not supported";
        }
        existing_rename_to_action = true;
        auto* rename_to = action->GetAsOrDie<ASTRenameToClause>();
        std::unique_ptr<const ResolvedAlterAction> resolved_action =
            MakeResolvedRenameToAction(
                rename_to->new_name()->ToIdentifierVector());
        alter_actions->push_back(std::move(resolved_action));
        break;
      }
      case AST_ALTER_COLUMN_OPTIONS_ACTION:
      case AST_ALTER_COLUMN_DROP_NOT_NULL_ACTION: {
        if (ast_statement->node_kind() != AST_ALTER_TABLE_STATEMENT) {
          // Views, models, etc don't support ALTER COLUMN ... SET OPTIONS/DROP
          // NOT NULL ...
          return MakeSqlErrorAt(action)
                 << "ALTER " << alter_statement_kind << " does not support "
                 << action->GetSQLForAlterAction();
        }
        if (!ast_statement->is_if_exists()) {
          ZETASQL_RETURN_IF_ERROR(table_status);
        }
        std::unique_ptr<const ResolvedAlterAction> resolved_action;
        if (action->node_kind() == AST_ALTER_COLUMN_OPTIONS_ACTION) {
          ZETASQL_RETURN_IF_ERROR(ResolveAlterColumnOptionsAction(
              table_name_id_string, altered_table,
              action->GetAsOrDie<ASTAlterColumnOptionsAction>(),
              &resolved_action));
        } else if (action->node_kind() ==
                   AST_ALTER_COLUMN_DROP_NOT_NULL_ACTION) {
          ZETASQL_RETURN_IF_ERROR(ResolveAlterColumnDropNotNullAction(
              table_name_id_string, altered_table,
              action->GetAsOrDie<ASTAlterColumnDropNotNullAction>(),
              &resolved_action));
        }
        alter_actions->push_back(std::move(resolved_action));
      } break;
      case AST_SET_COLLATE_CLAUSE: {
        return MakeSqlErrorAt(action)
               << "ALTER " << alter_statement_kind << " does not support "
               << action->GetSQLForAlterAction() << " action.";
      }
      default:
        return MakeSqlErrorAt(action)
               << "ALTER " << alter_statement_kind << " does not support "
               << action->GetSQLForAlterAction();
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterDatabaseStatement(
    const ASTAlterDatabaseStatement* ast_statement,
    std::unique_ptr<ResolvedStatement>* output) {
  bool has_only_set_options_action = true;
  std::vector<std::unique_ptr<const ResolvedAlterAction>>
      resolved_alter_actions;
  ZETASQL_RETURN_IF_ERROR(ResolveAlterActions(ast_statement, "DATABASE", output,
                                      &has_only_set_options_action,
                                      &resolved_alter_actions));
  *output = MakeResolvedAlterDatabaseStmt(
      ast_statement->path()->ToIdentifierVector(),
      std::move(resolved_alter_actions), ast_statement->is_if_exists());
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterSchemaStatement(
    const ASTAlterSchemaStatement* ast_statement,
    std::unique_ptr<ResolvedStatement>* output) {
  bool has_only_set_options_action = true;
  std::vector<std::unique_ptr<const ResolvedAlterAction>>
      resolved_alter_actions;
  if (ast_statement->collate() != nullptr) {
      return MakeSqlErrorAt(ast_statement->collate())
          << "COLLATE is unsupported";
  }
  ZETASQL_RETURN_IF_ERROR(ResolveAlterActions(ast_statement, "SCHEMA", output,
                                      &has_only_set_options_action,
                                      &resolved_alter_actions));
  *output = MakeResolvedAlterSchemaStmt(
      ast_statement->path()->ToIdentifierVector(),
      std::move(resolved_alter_actions), ast_statement->is_if_exists());
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterTableStatement(
    const ASTAlterTableStatement* ast_statement,
    std::unique_ptr<ResolvedStatement>* output) {
  bool has_only_set_options_action = true;
  std::vector<std::unique_ptr<const ResolvedAlterAction>>
      resolved_alter_actions;
  if (ast_statement->collate() != nullptr) {
      return MakeSqlErrorAt(ast_statement->collate())
          << "COLLATE is unsupported";
  }
  ZETASQL_RETURN_IF_ERROR(ResolveAlterActions(ast_statement, "TABLE", output,
                                      &has_only_set_options_action,
                                      &resolved_alter_actions));
  std::unique_ptr<ResolvedAlterTableStmt> alter_statement =
      MakeResolvedAlterTableStmt(ast_statement->path()->ToIdentifierVector(),
                                 std::move(resolved_alter_actions),
                                 ast_statement->is_if_exists());

  // TODO: deprecate ResolvedAlterTableSetOptionsStmt
  // To support legacy code, form ResolvedAlterTableSetOptionsStmt here
  // if RESOLVED_ALTER_TABLE_SET_OPTIONS_STMT is enabled
  const bool legacy_support =
      language().SupportsStatementKind(RESOLVED_ALTER_TABLE_SET_OPTIONS_STMT);
  const bool alter_support =
      language().SupportsStatementKind(RESOLVED_ALTER_TABLE_STMT);
  if (has_only_set_options_action && legacy_support) {
    // Converts the action list with potentially multiple SET OPTIONS actions
    // to a single list of options.
    std::vector<std::unique_ptr<const ResolvedOption>> resolved_options;
    const ASTAlterActionList* action_list = ast_statement->action_list();
    for (const ASTAlterAction* const action : action_list->actions()) {
      ZETASQL_RETURN_IF_ERROR(ResolveOptionsList(
          action->GetAsOrDie<ASTSetOptionsAction>()->options_list(),
          &resolved_options));
    }
    *output = MakeResolvedAlterTableSetOptionsStmt(
        alter_statement->name_path(), std::move(resolved_options),
        ast_statement->is_if_exists());
  } else if (!has_only_set_options_action && legacy_support && !alter_support) {
    return MakeSqlErrorAt(ast_statement)
           << "ALTER TABLE supports only the SET OPTIONS action";
  } else if (!alter_support) {
    return MakeSqlErrorAt(ast_statement) << "ALTER TABLE is not supported";
  } else {
    *output = std::move(alter_statement);
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAddColumnAction(
    IdString table_name_id_string, const Table* table,
    const ASTAddColumnAction* action, IdStringSetCase* new_columns,
    IdStringSetCase* columns_to_drop,
    std::unique_ptr<const ResolvedAlterAction>* alter_action) {
  ZETASQL_DCHECK(*alter_action == nullptr);

  const ASTColumnDefinition* column = action->column_definition();

  const IdString column_name = column->name()->GetAsIdString();
  if (!new_columns->insert(column_name).second) {
    return MakeSqlErrorAt(action->column_definition()->name())
           << "Duplicate column name " << column_name
           << " in ALTER TABLE ADD COLUMN";
  }

  // Check that ASTAddColumnAction does not contain various fields for which we
  // don't have corresponding properties in ResolvedAlterAction yet.
  // TODO: add corresponding properties and support.
  if (action->fill_expression() != nullptr) {
    return MakeSqlErrorAt(action->fill_expression())
           << "ALTER TABLE ADD COLUMN with FILL USING is not supported yet";
  }
  if (column->schema()->generated_column_info() != nullptr) {
    return MakeSqlErrorAt(action->column_definition()->name())
           << "ALTER TABLE ADD COLUMN does not support generated columns yet";
  }
  if (column->schema()->ContainsAttribute(AST_PRIMARY_KEY_COLUMN_ATTRIBUTE)) {
    return MakeSqlErrorAt(action->column_definition()->name())
           << "ALTER TABLE ADD COLUMN does not support primary key attribute"
           << " (column: " << column_name << ")";
  }
  if (column->schema()->ContainsAttribute(AST_FOREIGN_KEY_COLUMN_ATTRIBUTE)) {
    return MakeSqlErrorAt(action->column_definition()->name())
           << "ALTER TABLE ADD COLUMN does not support foreign key attribute"
           << " (column: " << column_name << ")";
  }
  if (action->column_position() != nullptr) {
    return MakeSqlErrorAt(action->column_position())
           << "ALTER TABLE ADD COLUMN with column position is not supported"
           << " (column: " << column_name << ")";
  }
  // Check the column does not exist, unless it was just deleted by DROP COLUMN.
  if (table != nullptr && !action->is_if_not_exists() &&
      columns_to_drop->find(column_name) == columns_to_drop->end()) {
    if (table->FindColumnByName(column_name.ToString()) != nullptr) {
      return MakeSqlErrorAt(action->column_definition()->name())
             << "Column already exists: " << column_name;
    }
  }

  NameList column_name_list;
  // We don't support fill expression, so can use cheaper method
  // ResolveColumnDefinitionNoCache to resolve columns.
  ZETASQL_ASSIGN_OR_RETURN(
      std::unique_ptr<const ResolvedColumnDefinition> column_definition,
      ResolveColumnDefinitionNoCache(column, table_name_id_string,
                                     &column_name_list));

  *alter_action = MakeResolvedAddColumnAction(action->is_if_not_exists(),
                                              std::move(column_definition));
  return absl::OkStatus();
}

absl::Status Resolver::ResolveDropColumnAction(
    IdString table_name_id_string, const Table* table,
    const ASTDropColumnAction* action, IdStringSetCase* new_columns,
    IdStringSetCase* columns_to_drop,
    std::unique_ptr<const ResolvedAlterAction>* alter_action) {
  ZETASQL_DCHECK(*alter_action == nullptr);

  const IdString column_name = action->column_name()->GetAsIdString();
  if (!columns_to_drop->insert(column_name).second) {
    return MakeSqlErrorAt(action->column_name())
           << "ALTER TABLE DROP COLUMN cannot drop column " << column_name
           << " multiple times";
  }
  if (new_columns->find(column_name) != new_columns->end()) {
    return MakeSqlErrorAt(action->column_name())
           << "Column " << column_name
           << " cannot be added and dropped by the same ALTER TABLE statement";
  }

  // If the table is present, verify that the column exists and can be dropped.
  if (table != nullptr) {
    const Column* column = table->FindColumnByName(column_name.ToString());
    if (column == nullptr && !action->is_if_exists()) {
      return MakeSqlErrorAt(action->column_name())
             << "Column not found: " << column_name;
    }
    if (column != nullptr && column->IsPseudoColumn()) {
      return MakeSqlErrorAt(action->column_name())
             << "ALTER TABLE DROP COLUMN cannot drop pseudo-column "
             << column_name;
    }
  }

  *alter_action = MakeResolvedDropColumnAction(action->is_if_exists(),
                                               column_name.ToString());
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterColumnTypeAction(
    IdString table_name_id_string, const Table* table,
    const ASTAlterColumnTypeAction* action,
    std::unique_ptr<const ResolvedAlterAction>* alter_action) {
  ZETASQL_RET_CHECK(*alter_action == nullptr);

  const IdString column_name = action->column_name()->GetAsIdString();

  std::unique_ptr<ResolvedColumnRef> column_reference;
  const Type* resolved_type = nullptr;
  TypeParameters type_parameters;

  if (table != nullptr) {
    const Column* column = table->FindColumnByName(column_name.ToString());
    if (column == nullptr && !action->is_if_exists()) {
      return MakeSqlErrorAt(action) << "Column not found: " << column_name;
    }
    if (column != nullptr && column->IsPseudoColumn()) {
      return MakeSqlErrorAt(action->column_name())
             << "ALTER TABLE ALTER COLUMN SET DATA TYPE cannot change the type "
             << "of pseudo-column " << column_name;
    }
    if (column != nullptr) {
      const ResolvedColumn resolved_column(AllocateColumnId(),
                                           table_name_id_string, column_name,
                                           column->GetType());
      column_reference = MakeColumnRef(resolved_column);
    }

    NameList column_name_list;
    std::unique_ptr<const ResolvedColumnAnnotations> annotations;
    std::unique_ptr<ResolvedGeneratedColumnInfo> generated_column_info;
    std::unique_ptr<const ResolvedExpr> column_default_expr;

    ZETASQL_RETURN_IF_ERROR(ResolveColumnSchema(
        action->schema(), column_name_list, &resolved_type, &annotations,
        &generated_column_info, &column_default_expr));

    ZETASQL_RET_CHECK(generated_column_info == nullptr);
    ZETASQL_RET_CHECK(column_default_expr == nullptr);

    if (annotations != nullptr) {
      // OPTIONS not allowed.
      if (OptionsPresent(annotations.get())) {
        return MakeSqlErrorAt(action->schema())
               << "For ALTER TABLE ALTER COLUMN SET DATA TYPE, the updated "
               << "data type cannot contain OPTIONS";
      }

      ZETASQL_ASSIGN_OR_RETURN(type_parameters,
                       annotations->GetFullTypeParameters(resolved_type));
    }
  }

  SignatureMatchResult result;
  if (column_reference != nullptr) {
    const Type* existing_type = column_reference->column().type();

    // TODO: Check CONVERT USING expression when parser adds it.
    // Note that we cannot check that a NUMERIC(P,S) column can't increase S by
    // more than P was increased because the type parameters aren't in the
    // catalog.
    if (!coercer_.AssignableTo(InputArgumentType(existing_type), resolved_type,
                               /*is_explicit=*/false, &result)) {
      return MakeSqlErrorAt(action)
             << "ALTER TABLE ALTER COLUMN SET DATA TYPE "
             << "requires that the existing column type ("
             << column_reference->column().type()->TypeName(
                    language().product_mode())
             << ") is assignable to the new type ("
             << resolved_type->ShortTypeName(language().product_mode()) << ")";
    }
  }

  *alter_action = MakeResolvedAlterColumnSetDataTypeAction(
      action->is_if_exists(), column_name.ToString(), resolved_type,
      type_parameters);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterColumnOptionsAction(
    IdString table_name_id_string, const Table* table,
    const ASTAlterColumnOptionsAction* action,
    std::unique_ptr<const ResolvedAlterAction>* alter_action) {
  ZETASQL_RET_CHECK(*alter_action == nullptr);
  const IdString column_name = action->column_name()->GetAsIdString();
  // If the table is present, verify that the column exists and can be modified.
  if (table != nullptr) {
    const Column* column = table->FindColumnByName(column_name.ToString());
    if (column == nullptr) {
      if (action->is_if_exists()) {
        // Silently ignore the NOT FOUND error since this is a ALTER COLUMN IF
        // EXISTS action.
      } else {
        return MakeSqlErrorAt(action->column_name())
               << "Column not found: " << column_name;
      }
    } else if (column->IsPseudoColumn()) {
      return MakeSqlErrorAt(action->column_name())
             << "ALTER COLUMN SET OPTIONS not supported "
             << "for pseudo-column " << column_name;
    }
  }
  std::vector<std::unique_ptr<const ResolvedOption>> resolved_options;
  ZETASQL_RETURN_IF_ERROR(
      ResolveOptionsList(action->options_list(), &resolved_options));
  *alter_action = MakeResolvedAlterColumnOptionsAction(
      action->is_if_exists(), column_name.ToString(),
      std::move(resolved_options));
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterColumnDropNotNullAction(
    IdString table_name_id_string, const Table* table,
    const ASTAlterColumnDropNotNullAction* action,
    std::unique_ptr<const ResolvedAlterAction>* alter_action) {
  ZETASQL_RET_CHECK(*alter_action == nullptr);
  const IdString column_name = action->column_name()->GetAsIdString();
  std::unique_ptr<ResolvedColumnRef> column_reference;
  // If the table is present, verify that the column exists and can be modified.
  if (table != nullptr) {
    const Column* column = table->FindColumnByName(column_name.ToString());
    if (column == nullptr) {
      if (action->is_if_exists()) {
        // Silently ignore the NOT FOUND error since this is a ALTER COLUMN IF
        // EXISTS action.
      } else {
        return MakeSqlErrorAt(action->column_name())
               << "Column not found: " << column_name;
      }
    } else if (column->IsPseudoColumn()) {
      return MakeSqlErrorAt(action->column_name())
             << "ALTER COLUMN DROP NOT NULL not supported for pseudo-column "
             << column_name;
    }
  }
  *alter_action = MakeResolvedAlterColumnDropNotNullAction(
      action->is_if_exists(), column_name.ToString());
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAlterEntityStatement(
    const ASTAlterEntityStatement* ast_statement,
    std::unique_ptr<ResolvedStatement>* output) {
  bool has_only_set_options_action = true;
  std::vector<std::unique_ptr<const ResolvedAlterAction>>
      resolved_alter_actions;
  ZETASQL_RETURN_IF_ERROR(ResolveAlterActions(
      ast_statement, ast_statement->type()->GetAsString(), output,
      &has_only_set_options_action, &resolved_alter_actions));
  *output = MakeResolvedAlterEntityStmt(
      ast_statement->path()->ToIdentifierVector(),
      std::move(resolved_alter_actions), ast_statement->is_if_exists(),
      ast_statement->type()->GetAsString());
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAddForeignKey(
    const Table* referencing_table, const ASTAlterStatementBase* alter_stmt,
    const ASTAddConstraintAction* alter_action,
    std::unique_ptr<const ResolvedAddConstraintAction>* resolved_alter_action) {
  if (!language().LanguageFeatureEnabled(FEATURE_FOREIGN_KEYS)) {
    return MakeSqlErrorAt(alter_action) << "FOREIGN KEY is not supported";
  }

  // <referencing_table> may be null if the target table does not exist. In
  // that case, we return an error for ALTER TABLE and optimistically assume
  // schemas match for ALTER TABLE IF EXISTS.

  // The caller should have already verified this for us.
  ZETASQL_RET_CHECK(referencing_table != nullptr || alter_stmt->is_if_exists());

  const ASTForeignKey* foreign_key =
      alter_action->constraint()->GetAsOrDie<ASTForeignKey>();

  ColumnIndexMap column_indexes;
  std::vector<const Type*> column_types;
  if (referencing_table != nullptr) {
    for (int i = 0; i < referencing_table->NumColumns(); i++) {
      const Column* column = referencing_table->GetColumn(i);
      ZETASQL_RET_CHECK(column != nullptr);
      column_indexes[id_string_pool_->Make(column->Name())] = i;
      column_types.push_back(column->GetType());
    }
  } else {
    // If the referencing table does not exist, then we use the referenced
    // columns' types. We also include the referencing columns' names in the
    // resolved node so that SQL builders can reconstruct the original SQL.
    const Table* referenced_table;
    ZETASQL_RETURN_IF_ERROR(
        FindTable(foreign_key->reference()->table_name(), &referenced_table));
    for (const ASTIdentifier* column_name :
         foreign_key->reference()->column_list()->identifiers()) {
      const Column* column =
          referenced_table->FindColumnByName(column_name->GetAsString());
      if (column == nullptr) {
        return MakeSqlErrorAt(column_name)
               << "Column " << column_name->GetAsString()
               << " not found in table " << referenced_table->Name();
      }
      column_types.push_back(column->GetType());
    }

    // Column indexes for referencing columns are fake and assigned based on
    // their appearance in the constraint DDL.
    for (int i = 0; i < foreign_key->column_list()->identifiers().size(); i++) {
      const ASTIdentifier* referencing_column =
          foreign_key->column_list()->identifiers().at(i);
      column_indexes.insert({referencing_column->GetAsIdString(), i});
    }
  }

  std::vector<std::unique_ptr<ResolvedForeignKey>> foreign_keys;
  ZETASQL_RETURN_IF_ERROR(ResolveForeignKeyTableConstraint(column_indexes, column_types,
                                                   foreign_key, &foreign_keys));
  ZETASQL_RET_CHECK(foreign_keys.size() == 1);
  *resolved_alter_action = MakeResolvedAddConstraintAction(
      alter_action->is_if_not_exists(), std::move(foreign_keys[0]),
      referencing_table);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAddPrimaryKey(
    const Table* target_table, const ASTAlterStatementBase* alter_stmt,
    const ASTAddConstraintAction* alter_action,
    std::unique_ptr<const ResolvedAddConstraintAction>* resolved_alter_action) {
  // The caller should have already verified this for us. We either have a
  // table or the action uses IF EXISTS.
  ZETASQL_RET_CHECK(target_table != nullptr || alter_stmt->is_if_exists());

  const ASTPrimaryKey* ast_primary_key =
      alter_action->constraint()->GetAsOrDie<ASTPrimaryKey>();

  ColumnIndexMap column_indexes;
  if (target_table != nullptr) {
    for (int i = 0; i < target_table->NumColumns(); i++) {
      const Column* column = target_table->GetColumn(i);
      ZETASQL_RET_CHECK(column != nullptr);
      if (!column->IsPseudoColumn()) {
        column_indexes[id_string_pool_->Make(column->Name())] = i;
      }
    }
  }

  std::unique_ptr<ResolvedPrimaryKey> primary_key;
  ZETASQL_RETURN_IF_ERROR(
      ResolvePrimaryKey(column_indexes, ast_primary_key, &primary_key));

  *resolved_alter_action = MakeResolvedAddConstraintAction(
      alter_action->is_if_not_exists(), std::move(primary_key), target_table);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveAddConstraintAction(
    const Table* referencing_table, const ASTAlterStatementBase* alter_stmt,
    const ASTAddConstraintAction* alter_action,
    std::unique_ptr<const ResolvedAddConstraintAction>* resolved_alter_action) {
  auto constraint_kind = alter_action->constraint()->node_kind();
  if (constraint_kind == AST_CHECK_CONSTRAINT &&
      !language().LanguageFeatureEnabled(FEATURE_CHECK_CONSTRAINT)) {
    return MakeSqlErrorAt(alter_action) << "CHECK CONSTRAINT is not supported";
  } else if (constraint_kind == AST_FOREIGN_KEY) {
    return ResolveAddForeignKey(referencing_table, alter_stmt, alter_action,
                                resolved_alter_action);
  } else if (constraint_kind == AST_PRIMARY_KEY) {
    return ResolveAddPrimaryKey(referencing_table, alter_stmt, alter_action,
                                resolved_alter_action);
  }

  return MakeSqlErrorAt(alter_action)
         << "ALTER TABLE ADD CONSTRAINT is not implemented";
}
}  // namespace zetasql
