// MIT License © 2026 Binary Dice Games
/// @file sq_query_guard.hpp
/// @brief Best-effort "read-only SQL" check applied before any query is run.
///
/// The sq module is a *querying* frontend: it must never insert, modify or
/// delete data. `sq sql` has no read-only mode, so this lexical guard rejects
/// anything that is not a single SELECT / WITH / VALUES / SHOW / DESCRIBE /
/// EXPLAIN statement, or that contains a data- or schema-changing keyword
/// outside string literals, quoted identifiers and comments.
///
/// It is a safety net against accidents, NOT a security boundary (a database
/// function with side effects still passes). For untrusted use, connect with
/// a read-only database account.
#pragma once

#include <optional>
#include <string>

namespace bdg::wish::sq {

/// @brief Returns nullopt when @p sql looks read-only, otherwise a short
/// human-readable reason it was rejected.
std::optional<std::string> check_read_only_sql(const std::string& sql);

} // namespace bdg::wish::sq
