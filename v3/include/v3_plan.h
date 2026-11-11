// ECHO v3 plan loader (round / query JSON -> IR).
#pragma once

#include <string>

#include "v3_ir.h"
#include "v3_json.h"

namespace v3 {

bool load_round(const std::string& path, Round* out, std::string* err);
bool load_query_file(const std::string& path, Query* out, std::string* err);
bool parse_query(const json::J& j, Query* out, std::string* err);
Value parse_value(const json::J& j);

}  // namespace v3
