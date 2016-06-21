#ifndef COMPILER_PARSE_TABLE_H_
#define COMPILER_PARSE_TABLE_H_

#include <map>
#include <set>
#include <utility>
#include <vector>
#include "compiler/lex_table.h"
#include "compiler/rules/symbol.h"
#include "compiler/rules/metadata.h"
#include "compiler/precedence_range.h"
#include "compiler/syntax_grammar.h"

namespace tree_sitter {

typedef uint64_t ParseStateId;

enum ParseActionType {
  ParseActionTypeError,
  ParseActionTypeShift,
  ParseActionTypeReduce,
  ParseActionTypeAccept,
};

class ParseAction {
  ParseAction(ParseActionType type, ParseStateId state_index,
              rules::Symbol symbol, size_t consumed_symbol_count,
              PrecedenceRange range, rules::Associativity, const Production *);

 public:
  ParseAction();
  static ParseAction Accept();
  static ParseAction Error();
  static ParseAction Shift(ParseStateId state_index, PrecedenceRange precedence);
  static ParseAction Reduce(rules::Symbol symbol, size_t consumed_symbol_count,
                            int precedence, rules::Associativity,
                            const Production &);
  static ParseAction ShiftExtra();
  static ParseAction ReduceExtra(rules::Symbol symbol);
  bool operator==(const ParseAction &) const;
  bool operator<(const ParseAction &) const;

  ParseActionType type;
  bool extra;
  bool fragile;
  rules::Symbol symbol;
  ParseStateId state_index;
  size_t consumed_symbol_count;
  PrecedenceRange precedence_range;
  rules::Associativity associativity;
  const Production *production;
};

struct ParseTableEntry {
  std::vector<ParseAction> actions;
  bool reusable;
  bool depends_on_lookahead;

  ParseTableEntry();
  ParseTableEntry(const std::vector<ParseAction> &, bool, bool);
  bool operator==(const ParseTableEntry &other) const;
};

class ParseState {
 public:
  ParseState();
  std::set<rules::Symbol> expected_inputs() const;
  bool operator==(const ParseState &) const;
  void each_advance_action(std::function<void(ParseAction *)>);

  std::map<rules::Symbol, ParseTableEntry> entries;
  LexStateId lex_state_id;
};

struct ParseTableSymbolMetadata {
  bool extra;
  bool structural;
};

class ParseTable {
 public:
  std::set<rules::Symbol> all_symbols() const;
  ParseStateId add_state();
  ParseAction &set_action(ParseStateId state_id, rules::Symbol symbol,
                          ParseAction action);
  ParseAction &add_action(ParseStateId state_id, rules::Symbol symbol,
                          ParseAction action);

  std::vector<ParseState> states;
  ParseState error_state;
  std::map<rules::Symbol, ParseTableSymbolMetadata> symbols;
};

}  // namespace tree_sitter

#endif  // COMPILER_PARSE_TABLE_H_
