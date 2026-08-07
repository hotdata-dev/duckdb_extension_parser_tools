#include "parse_tables.hpp"
#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_options.hpp"
#include <algorithm>
#include <cctype>
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"

namespace duckdb {

inline const char *ToString(TableContext context) {
    switch (context) {
        case TableContext::From: return "from";
        case TableContext::JoinLeft: return "join_left";
        case TableContext::JoinRight: return "join_right";
        case TableContext::FromCTE: return "from_cte";
        case TableContext::CTE: return "cte";
        case TableContext::Subquery: return "subquery";
        default: return "unknown";
    }
}

inline const TableContext FromString(const char *context) {
    if (strcmp(context, "from") == 0) return TableContext::From;
    if (strcmp(context, "join_left") == 0) return TableContext::JoinLeft;
    if (strcmp(context, "join_right") == 0) return TableContext::JoinRight;
    if (strcmp(context, "from_cte") == 0) return TableContext::FromCTE;
    if (strcmp(context, "cte") == 0) return TableContext::CTE;
    if (strcmp(context, "subquery") == 0) return TableContext::Subquery;
    throw InternalException("Unknown table context: %s", context);
}

struct ParseTablesState : public GlobalTableFunctionState {
    idx_t row = 0;
    vector<TableRefResult> results;
};

struct ParseTablesBindData : public TableFunctionData {
    string sql;
};

// BIND function: runs during query planning to decide output schema
static unique_ptr<FunctionData> ParseTablesBind(ClientContext &context, 
                                    TableFunctionBindInput &input, 
                                    vector<LogicalType> &return_types, 
                                    vector<string> &names) {
                                
    string sql_input = StringValue::Get(input.inputs[0]);
                                                    
    // always return the same columns:

    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
    // schema name, table name, usage context (from, join, cte, etc)
    names = {"schema", "table", "context"};
    
    // create a bind data object to hold the SQL input
    
    auto result = make_uniq<ParseTablesBindData>();
    result->sql = sql_input;

    return std::move(result);
}

// INIT function: runs before table function execution
static unique_ptr<GlobalTableFunctionState> ParseTablesInit(ClientContext &context,
    TableFunctionInitInput &input) {
    return make_uniq<ParseTablesState>();
}

// Forward declaration for mutual recursion
static void ExtractTablesFromExpression(
    const duckdb::ParsedExpression &expr,
    std::vector<TableRefResult> &results,
    const duckdb::CommonTableExpressionMap *cte_map
);

static void ExtractTablesFromRef(
    const duckdb::TableRef &ref,
    std::vector<TableRefResult> &results,
    const TableContext context = TableContext::From,
    bool is_top_level = false,
    const duckdb::CommonTableExpressionMap *cte_map = nullptr
) {
    using namespace duckdb;

    switch (ref.type) {
        case TableReferenceType::BASE_TABLE: {
            auto &base = (BaseTableRef &)ref;
            TableContext context_label = context;

            if (cte_map && cte_map->map.find(base.table_name) != cte_map->map.end()) {
                context_label = TableContext::FromCTE;
            } else if (is_top_level) {
                context_label = TableContext::From;
            }

            results.push_back(TableRefResult{
                base.schema_name.empty() ? "main" : base.schema_name,
                base.table_name,
                context_label
            });
            break;
        }
        case TableReferenceType::JOIN: {
            auto &join = (JoinRef &)ref;
            ExtractTablesFromRef(*join.left, results, TableContext::JoinLeft, is_top_level, cte_map);
            ExtractTablesFromRef(*join.right, results, TableContext::JoinRight, false, cte_map);
            // Process JOIN condition for subqueries
            if (join.condition) {
                ExtractTablesFromExpression(*join.condition, results, cte_map);
            }
            break;
        }
        case TableReferenceType::SUBQUERY: {
            auto &subquery = (SubqueryRef &)ref;
            if (subquery.subquery && subquery.subquery->node) {
                ExtractTablesFromQueryNode(*subquery.subquery->node, results, TableContext::Subquery, cte_map);
            }
            break;
        }
        default:
            break;
    }
}

// Extract tables from expressions that may contain subqueries (WHERE, HAVING, SELECT list, etc.)
static void ExtractTablesFromExpression(
    const duckdb::ParsedExpression &expr,
    std::vector<TableRefResult> &results,
    const duckdb::CommonTableExpressionMap *cte_map
) {
    using namespace duckdb;

    // Check if this is a subquery expression
    if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
        auto &subquery_expr = (const SubqueryExpression &)expr;
        if (subquery_expr.subquery && subquery_expr.subquery->node) {
            ExtractTablesFromQueryNode(*subquery_expr.subquery->node, results, TableContext::Subquery, cte_map);
        }
        // Also process the child expression (e.g., the left side of IN)
        if (subquery_expr.child) {
            ExtractTablesFromExpression(*subquery_expr.child, results, cte_map);
        }
        return;
    }

    // Recursively process child expressions
    ParsedExpressionIterator::EnumerateChildren(expr,
        [&](const ParsedExpression &child) {
            ExtractTablesFromExpression(child, results, cte_map);
        });
}

static void ExtractTablesFromQueryNode(
    const duckdb::QueryNode &node,
    std::vector<TableRefResult> &results,
    const TableContext context,
    const duckdb::CommonTableExpressionMap *cte_map
) {
    using namespace duckdb;

    if (node.type == QueryNodeType::SELECT_NODE) {
        auto &select_node = (SelectNode &)node;

        // Handle CTE definitions
        for (const auto &entry : select_node.cte_map.map) {
            results.push_back(TableRefResult{
                "", entry.first, TableContext::CTE
            });

            if (entry.second && entry.second->query && entry.second->query->node) {
                ExtractTablesFromQueryNode(*entry.second->query->node, results, TableContext::From, &select_node.cte_map);
            }
        }

        if (select_node.from_table) {
            ExtractTablesFromRef(*select_node.from_table, results, context, true, &select_node.cte_map);
        }

        // Extract tables from WHERE clause subqueries
        if (select_node.where_clause) {
            ExtractTablesFromExpression(*select_node.where_clause, results, &select_node.cte_map);
        }

        // Extract tables from SELECT list subqueries
        for (const auto &expr : select_node.select_list) {
            if (expr) {
                ExtractTablesFromExpression(*expr, results, &select_node.cte_map);
            }
        }

        // Extract tables from HAVING clause subqueries
        if (select_node.having) {
            ExtractTablesFromExpression(*select_node.having, results, &select_node.cte_map);
        }

        // Extract tables from QUALIFY clause subqueries
        if (select_node.qualify) {
            ExtractTablesFromExpression(*select_node.qualify, results, &select_node.cte_map);
        }

        // Extract tables from GROUP BY expressions
        for (const auto &expr : select_node.groups.group_expressions) {
            if (expr) {
                ExtractTablesFromExpression(*expr, results, &select_node.cte_map);
            }
        }
    }
    // additional step necessary for duckdb v1.4.0: unwrap CTE node
    else if (node.type == QueryNodeType::CTE_NODE) {
        auto &cte_node = (CTENode &)node;

        if (cte_node.child) {
            ExtractTablesFromQueryNode(*cte_node.child, results, context, cte_map);
        }
    }
    // Handle UNION/INTERSECT/EXCEPT (set operations)
    else if (node.type == QueryNodeType::SET_OPERATION_NODE) {
        auto &set_node = (SetOperationNode &)node;

        for (auto &child : set_node.children) {
            if (child) {
                ExtractTablesFromQueryNode(*child, results, context, cte_map);
            }
        }
    }

    // Process result modifiers (ORDER BY, LIMIT) for all node types
    for (const auto &modifier : node.modifiers) {
        if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
            auto &order_modifier = (OrderModifier &)*modifier;
            for (const auto &order : order_modifier.orders) {
                if (order.expression) {
                    ExtractTablesFromExpression(*order.expression, results, cte_map);
                }
            }
        } else if (modifier->type == ResultModifierType::LIMIT_MODIFIER) {
            auto &limit_modifier = (LimitModifier &)*modifier;
            if (limit_modifier.limit) {
                ExtractTablesFromExpression(*limit_modifier.limit, results, cte_map);
            }
            if (limit_modifier.offset) {
                ExtractTablesFromExpression(*limit_modifier.offset, results, cte_map);
            }
        }
    }
}

static void ExtractTablesFromSQL(const std::string &sql, std::vector<TableRefResult> &results) {
    Parser parser;

    try {
        parser.ParseQuery(sql);
    } catch (const ParserException &ex) {
        // swallow parser exceptions to make this function more robust. is_parsable can be used if needed
        return;
    }

    for (auto &stmt : parser.statements) {
        if (stmt->type == StatementType::SELECT_STATEMENT) {
            auto &select_stmt = (SelectStatement &)*stmt;
            if (select_stmt.node) {
                ExtractTablesFromQueryNode(*select_stmt.node, results);
            }
        }
    }
}

static void ExtractTablesFromSQL(const std::string & sql, std::vector<TableRefResult> &result, std::unordered_set<std::string> excluded_types) {
    std::vector<TableRefResult> temp_result;
    ExtractTablesFromSQL(sql, temp_result);
    std::unordered_set<TableContext> e_types;

    for (auto &type : excluded_types) {
        e_types.insert(FromString(type.c_str()));
    }

    for (auto &table : temp_result) {
        if (e_types.count(table.context) == 0) {
            result.push_back(table);
        }
    }
}

static void ParseTablesFunction(ClientContext &context,
                   TableFunctionInput &data,
                   DataChunk &output) {
    auto &state = (ParseTablesState &)*data.global_state;
    auto &bind_data = (ParseTablesBindData &)*data.bind_data;

    if (state.results.empty() && state.row == 0) {
        ExtractTablesFromSQL(bind_data.sql, state.results);
    }

    if (state.row >= state.results.size()) {
        return;
    }

    auto &ref = state.results[state.row];
    output.SetCardinality(1);
    output.SetValue(0, 0, Value(ref.schema));
    output.SetValue(1, 0, Value(ref.table));
    output.SetValue(2, 0, Value(ToString(ref.context)));

    state.row++;
}

static void ParseTablesScalarFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    Vector flag(LogicalType::BOOLEAN); 
    
    // Allow for the optional boolean argument. if not provided, default to true
    if (args.ColumnCount() == 1) {
        // create a default argument to pass below. we'll use a constant vector since all values are the same
        Vector c(LogicalType::BOOLEAN);
        c.Reference(Value::BOOLEAN(true));
        ConstantVector::Reference(flag, c, 0, args.size());
    } else if (args.ColumnCount() == 2) {
        flag.Reference(args.data[1]);
    } else {
        throw InvalidInputException("parse_tables() expects 1 or 2 arguments");
    }

    // Execute does the heavy lifting of iterating over the input data
    // and calling the provided lambda function for each input value.
    // The lambda function is responsible for parsing the SQL query and
    // extracting the table names.
    BinaryExecutor::Execute<string_t, bool, list_entry_t>(args.data[0], flag, result, args.size(), 
    [&result](string_t query, bool exclude_cte) -> list_entry_t {
        // Parse the SQL query and extract table names
        auto query_string = query.GetString();
        std::vector<TableRefResult> parsed_tables;
        if (exclude_cte) {
            std::unordered_set<std::string> excluded_types = {"cte", "from_cte"};
            ExtractTablesFromSQL(query_string, parsed_tables, excluded_types);
        } else {
            ExtractTablesFromSQL(query_string, parsed_tables);
        }
        
    
        auto current_size = ListVector::GetListSize(result);
        auto number_of_tables = parsed_tables.size();
        auto new_size = current_size + number_of_tables;

        // grow list if needed
        if (ListVector::GetListCapacity(result) < new_size) {
            ListVector::Reserve(result, new_size);
        }

        // Write the string into the child vector
        auto tables = FlatVector::GetData<string_t>(ListVector::GetEntry(result));
        for (size_t i = 0; i < parsed_tables.size(); i++) {
            auto &table = parsed_tables[i];
            tables[current_size + i] = StringVector::AddStringOrBlob(ListVector::GetEntry(result), table.table);
        }

        // Update size
        ListVector::SetListSize(result, new_size);

        return list_entry_t(current_size, number_of_tables); 
    });
}

static void ParseTablesScalarFunction_struct(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, list_entry_t>(args.data[0], result, args.size(),
    [&result](string_t query) -> list_entry_t {
        // Parse the SQL query and extract table names
        auto query_string = query.GetString();
        std::vector<TableRefResult> parsed_tables;
        ExtractTablesFromSQL(query_string, parsed_tables);    

        auto current_size = ListVector::GetListSize(result);
        auto number_of_tables = parsed_tables.size();
        auto new_size = current_size + number_of_tables;

        // Grow list vector if needed
        if (ListVector::GetListCapacity(result) < new_size) {
            ListVector::Reserve(result, new_size);
        }

        // Get the struct child vector of the list
        auto &struct_vector = ListVector::GetEntry(result);

        // Ensure list size is updated
        ListVector::SetListSize(result, new_size);

        // Get the fields in the STRUCT
        auto &entries = StructVector::GetEntries(struct_vector);
        auto &schema_entry = *entries[0]; // "schema" field
        auto &table_entry = *entries[1];  // "table" field
        auto &context_entry = *entries[2]; // "context" field

        auto schema_data = FlatVector::GetData<string_t>(schema_entry);
        auto table_data = FlatVector::GetData<string_t>(table_entry);
        auto context_data = FlatVector::GetData<string_t>(context_entry);


        for (size_t i = 0; i < number_of_tables; i++) {
            const auto &table = parsed_tables[i];
            auto idx = current_size + i;

            schema_data[idx] = StringVector::AddStringOrBlob(schema_entry, table.schema);
            table_data[idx] = StringVector::AddStringOrBlob(table_entry, table.table);
            context_data[idx] = StringVector::AddStringOrBlob(context_entry, ToString(table.context));
        }

        return list_entry_t(current_size, number_of_tables);
    });
}

static void IsParsableFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(),
    [](string_t query) -> bool {
        try {
            Parser parser;
            parser.ParseQuery(query.GetString());
            return true;
        } catch (const std::exception &) {
            return false;
        }
    });
}

// Extension scaffolding
// ---------------------------------------------------

void RegisterParseTablesFunction(ExtensionLoader &loader) {
    TableFunction tf("parse_tables", {LogicalType::VARCHAR}, ParseTablesFunction, ParseTablesBind, ParseTablesInit);
    loader.RegisterFunction(tf);
}

void RegisterParseTableScalarFunction(ExtensionLoader &loader) {
    // parse_table_names is overloaded, allowing for an optional boolean argument
    // that indicates whether to include CTEs in the result
    // usage: parse_tables(sql_query [, include_cte])
    ScalarFunctionSet set("parse_table_names");
    set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::VARCHAR), ParseTablesScalarFunction));
    set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::BOOLEAN}, LogicalType::LIST(LogicalType::VARCHAR), ParseTablesScalarFunction));
    loader.RegisterFunction(set);

    // parse_tables_struct is a scalar function that returns a list of structs
    auto return_type = LogicalType::LIST(LogicalType::STRUCT({
        {"schema", LogicalType::VARCHAR},
        {"table", LogicalType::VARCHAR},
        {"context", LogicalType::VARCHAR}
    }));
    ScalarFunction sf("parse_tables", {LogicalType::VARCHAR}, return_type, ParseTablesScalarFunction_struct);
    loader.RegisterFunction(sf);

    // is_parsable is a scalar function that returns a boolean indicating whether the SQL query is parsable (no parse errors)
    ScalarFunction is_parsable("is_parsable", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, IsParsableFunction);
    loader.RegisterFunction(is_parsable);
}

} // namespace duckdb
