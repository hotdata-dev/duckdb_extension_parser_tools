#include "parse_functions.hpp"
#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"


namespace duckdb {

enum class FunctionContext {
	Select,
	Where,
	Having,
	OrderBy,
	GroupBy,
	Join,
	WindowFunction,
	Nested
};

inline const char *ToString(FunctionContext context) {
	switch (context) {
		case FunctionContext::Select: return "select";
		case FunctionContext::Where: return "where";
		case FunctionContext::Having: return "having";
		case FunctionContext::OrderBy: return "order_by";
		case FunctionContext::GroupBy: return "group_by";
		case FunctionContext::Join: return "join";
		case FunctionContext::WindowFunction: return "window";
		case FunctionContext::Nested: return "nested";
		default: return "unknown";
	}
}

struct ParseFunctionsState : public GlobalTableFunctionState {
	idx_t row = 0;
	vector<FunctionResult> results;
};

struct ParseFunctionsBindData : public TableFunctionData {
	string sql;
};

// BIND function: runs during query planning to decide output schema
static unique_ptr<FunctionData> ParseFunctionsBind(ClientContext &context,
													TableFunctionBindInput &input,
													vector<LogicalType> &return_types,
													vector<string> &names) {

	string sql_input = StringValue::Get(input.inputs[0]);

	// always return the same columns:
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	// function name, schema name, usage context
	names = {"function_name", "schema", "context"};

	// create a bind data object to hold the SQL input
	auto result = make_uniq<ParseFunctionsBindData>();
	result->sql = sql_input;

	return std::move(result);
}

// INIT function: runs before table function execution
static unique_ptr<GlobalTableFunctionState> ParseFunctionsInit(ClientContext &context,
																														TableFunctionInitInput &input) {
	return make_uniq<ParseFunctionsState>();
}

static void ExtractFunctionsFromQueryNode(const QueryNode &node, std::vector<FunctionResult> &results);

class FunctionExtractor {
public:
	static void ExtractFromExpression(const ParsedExpression &expr, 
																					std::vector<FunctionResult> &results,
																					FunctionContext context = FunctionContext::Select) {
		if (expr.expression_class == ExpressionClass::FUNCTION) {
			auto &func = (FunctionExpression &)expr;
			results.push_back(FunctionResult{
				func.function_name,
				func.schema.empty() ? "main" : func.schema,
				ToString(context)
			});
			
			// For nested function calls within this function, mark as nested
			ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
				ExtractFromExpression(child, results, FunctionContext::Nested);
			});
		} else if (expr.expression_class == ExpressionClass::WINDOW) {
			auto &window_expr = (WindowExpression &)expr;
			results.push_back(FunctionResult{
				window_expr.function_name,
				window_expr.schema.empty() ? "main" : window_expr.schema,
				ToString(context)
			});
			
			// Extract functions from window function arguments
			for (const auto &child : window_expr.children) {
				if (child) {
					ExtractFromExpression(*child, results, FunctionContext::Nested);
				}
			}
			
			// Extract functions from PARTITION BY expressions
			for (const auto &partition : window_expr.partitions) {
				if (partition) {
					ExtractFromExpression(*partition, results, FunctionContext::Nested);
				}
			}
			
			// Extract functions from ORDER BY expressions
			for (const auto &order : window_expr.orders) {
				if (order.expression) {
					ExtractFromExpression(*order.expression, results, FunctionContext::Nested);
				}
			}
			
			// Extract functions from argument ordering expressions
			for (const auto &arg_order : window_expr.arg_orders) {
				if (arg_order.expression) {
					ExtractFromExpression(*arg_order.expression, results, FunctionContext::Nested);
				}
			}
			
			// Extract functions from frame expressions
			if (window_expr.start_expr) {
				ExtractFromExpression(*window_expr.start_expr, results, FunctionContext::Nested);
			}
			if (window_expr.end_expr) {
				ExtractFromExpression(*window_expr.end_expr, results, FunctionContext::Nested);
			}
			if (window_expr.offset_expr) {
				ExtractFromExpression(*window_expr.offset_expr, results, FunctionContext::Nested);
			}
			if (window_expr.default_expr) {
				ExtractFromExpression(*window_expr.default_expr, results, FunctionContext::Nested);
			}
			
			// Extract functions from filter expression
			if (window_expr.filter_expr) {
				ExtractFromExpression(*window_expr.filter_expr, results, FunctionContext::Nested);
			}
		} else if (expr.expression_class == ExpressionClass::SUBQUERY) {
			auto &subquery = (SubqueryExpression &)expr;
			if (subquery.child) {
				ExtractFromExpression(*subquery.child, results, context);
			}
			if (subquery.subquery && subquery.subquery->node) {
				ExtractFunctionsFromQueryNode(*subquery.subquery->node, results);
			}
		} else {
			// For non-function expressions, preserve the current context
			ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
				ExtractFromExpression(child, results, context);
			});
		}
	}

	static void ExtractFromExpressionList(const vector<unique_ptr<ParsedExpression>> &expressions,
																								std::vector<FunctionResult> &results,
																								FunctionContext context) {
		for (const auto &expr : expressions) {
			if (expr) {
				ExtractFromExpression(*expr, results, context);
			}
		}
	}
};

static void ExtractFunctionsFromRef(const TableRef &ref, std::vector<FunctionResult> &results) {
	switch (ref.type) {
	case TableReferenceType::JOIN: {
		auto &join = (JoinRef &)ref;
		if (join.left) {
			ExtractFunctionsFromRef(*join.left, results);
		}
		if (join.right) {
			ExtractFunctionsFromRef(*join.right, results);
		}
		if (join.condition) {
			FunctionExtractor::ExtractFromExpression(*join.condition, results, FunctionContext::Join);
		}
		break;
	}
	case TableReferenceType::SUBQUERY: {
		auto &subquery = (SubqueryRef &)ref;
		if (subquery.subquery && subquery.subquery->node) {
			ExtractFunctionsFromQueryNode(*subquery.subquery->node, results);
		}
		break;
	}
	case TableReferenceType::TABLE_FUNCTION: {
		auto &table_function = (TableFunctionRef &)ref;
		if (table_function.function) {
			ParsedExpressionIterator::EnumerateChildren(*table_function.function, [&](const ParsedExpression &child) {
				FunctionExtractor::ExtractFromExpression(child, results, FunctionContext::Nested);
			});
		}
		if (table_function.subquery && table_function.subquery->node) {
			ExtractFunctionsFromQueryNode(*table_function.subquery->node, results);
		}
		break;
	}
	default:
		break;
	}
}

static void ExtractFunctionsFromQueryNode(const QueryNode &node, std::vector<FunctionResult> &results) {
	if (node.type == QueryNodeType::SELECT_NODE) {
		auto &select_node = (SelectNode &)node;

		// Extract from CTEs first (to match expected order in tests)
		for (const auto &cte : select_node.cte_map.map) {
			if (cte.second && cte.second->query && cte.second->query->node) {
				ExtractFunctionsFromQueryNode(*cte.second->query->node, results);
			}
		}

		// Extract from SELECT list
		FunctionExtractor::ExtractFromExpressionList(select_node.select_list, results, FunctionContext::Select);

		// Extract from WHERE clause
		if (select_node.where_clause) {
			FunctionExtractor::ExtractFromExpression(*select_node.where_clause, results, FunctionContext::Where);
		}

		// Extract from GROUP BY clause
		FunctionExtractor::ExtractFromExpressionList(select_node.groups.group_expressions, results, FunctionContext::GroupBy);

		// Extract from HAVING clause
		if (select_node.having) {
			FunctionExtractor::ExtractFromExpression(*select_node.having, results, FunctionContext::Having);
		}

		// Extract from ORDER BY clause
		for (const auto &modifier : select_node.modifiers) {
			if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
				auto &order_modifier = (OrderModifier &)*modifier;
				for (const auto &order : order_modifier.orders) {
					if (order.expression) {
						FunctionExtractor::ExtractFromExpression(*order.expression, results, FunctionContext::OrderBy);
					}
				}
			}
		}

		if (select_node.from_table) {
			ExtractFunctionsFromRef(*select_node.from_table, results);
		}
	// additional step necessary for duckdb v1.4.0: unwrap CTE node
	}  else if (node.type == QueryNodeType::CTE_NODE) {
        auto &cte_node = (CTENode &)node;

        if (cte_node.child) {
            ExtractFunctionsFromQueryNode(*cte_node.child, results);
        }
    }
}

static void ExtractFunctionsFromSQL(const std::string &sql, std::vector<FunctionResult> &results) {
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
				ExtractFunctionsFromQueryNode(*select_stmt.node, results);
			}
		}
	}
}

static void ParseFunctionsFunction(ClientContext &context,
																				TableFunctionInput &data,
																				DataChunk &output) {
	auto &state = (ParseFunctionsState &)*data.global_state;
	auto &bind_data = (ParseFunctionsBindData &)*data.bind_data;

	if (state.results.empty() && state.row == 0) {
		ExtractFunctionsFromSQL(bind_data.sql, state.results);
	}

	if (state.row >= state.results.size()) {
		return;
	}

	auto &func = state.results[state.row];
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(func.function_name));
	output.SetValue(1, 0, Value(func.schema));
	output.SetValue(2, 0, Value(func.context));

	state.row++;
}

static void ParseFunctionNamesScalarFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, list_entry_t>(args.data[0], result, args.size(),
	[&result](string_t query) -> list_entry_t {
		// Parse the SQL query and extract function names
		auto query_string = query.GetString();
		std::vector<FunctionResult> parsed_functions;
		ExtractFunctionsFromSQL(query_string, parsed_functions);

		auto current_size = ListVector::GetListSize(result);
		auto number_of_functions = parsed_functions.size();
		auto new_size = current_size + number_of_functions;

		// grow list if needed
		if (ListVector::GetListCapacity(result) < new_size) {
			ListVector::Reserve(result, new_size);
		}

		// Write the function names into the child vector
		auto functions = FlatVector::GetData<string_t>(ListVector::GetEntry(result));
		for (size_t i = 0; i < parsed_functions.size(); i++) {
			auto &func = parsed_functions[i];
			functions[current_size + i] = StringVector::AddStringOrBlob(ListVector::GetEntry(result), func.function_name);
		}

		// Update size
		ListVector::SetListSize(result, new_size);

		return list_entry_t(current_size, number_of_functions);
	});
}

static void ParseFunctionsScalarFunction_struct(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, list_entry_t>(args.data[0], result, args.size(),
	[&result](string_t query) -> list_entry_t {
		// Parse the SQL query and extract function names
		auto query_string = query.GetString();
		std::vector<FunctionResult> parsed_functions;
		ExtractFunctionsFromSQL(query_string, parsed_functions);

		auto current_size = ListVector::GetListSize(result);
		auto number_of_functions = parsed_functions.size();
		auto new_size = current_size + number_of_functions;

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
		auto &function_name_entry = *entries[0]; // "function_name" field
		auto &schema_entry = *entries[1];  // "schema" field
		auto &context_entry = *entries[2]; // "context" field

		auto function_name_data = FlatVector::GetData<string_t>(function_name_entry);
		auto schema_data = FlatVector::GetData<string_t>(schema_entry);
		auto context_data = FlatVector::GetData<string_t>(context_entry);

		for (size_t i = 0; i < number_of_functions; i++) {
			const auto &func = parsed_functions[i];
			auto idx = current_size + i;

			function_name_data[idx] = StringVector::AddStringOrBlob(function_name_entry, func.function_name);
			schema_data[idx] = StringVector::AddStringOrBlob(schema_entry, func.schema);
			context_data[idx] = StringVector::AddStringOrBlob(context_entry, func.context);
		}

		return list_entry_t(current_size, number_of_functions);
	});
}

// Extension scaffolding
// ---------------------------------------------------

void RegisterParseFunctionsFunction(ExtensionLoader &loader) {
	TableFunction tf("parse_functions", {LogicalType::VARCHAR}, ParseFunctionsFunction, ParseFunctionsBind, ParseFunctionsInit);
	loader.RegisterFunction(tf);
}

void RegisterParseFunctionScalarFunction(ExtensionLoader &loader) {
	// parse_function_names is a scalar function that returns a list of function names
	ScalarFunction sf("parse_function_names", {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::VARCHAR), ParseFunctionNamesScalarFunction);
	loader.RegisterFunction(sf);

	// parse_functions_struct is a scalar function that returns a list of structs
	auto return_type = LogicalType::LIST(LogicalType::STRUCT({
		{"function_name", LogicalType::VARCHAR},
		{"schema", LogicalType::VARCHAR},
		{"context", LogicalType::VARCHAR}
	}));
	ScalarFunction sf_struct("parse_functions", {LogicalType::VARCHAR}, return_type, ParseFunctionsScalarFunction_struct);
	loader.RegisterFunction(sf_struct);
}



} // namespace duckdb
