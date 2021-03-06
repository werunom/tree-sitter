#include "runtime/parser.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <stdbool.h>
#include "tree_sitter/runtime.h"
#include "runtime/tree.h"
#include "runtime/lexer.h"
#include "runtime/length.h"
#include "runtime/array.h"
#include "runtime/language.h"
#include "runtime/alloc.h"
#include "runtime/reduce_action.h"
#include "runtime/error_costs.h"

#define LOG(...)                                                                            \
  if (self->lexer.logger.log || self->print_debugging_graphs) {                             \
    snprintf(self->lexer.debug_buffer, TREE_SITTER_SERIALIZATION_BUFFER_SIZE, __VA_ARGS__); \
    parser__log(self);                                                                      \
  }

#define LOG_STACK()                                                \
  if (self->print_debugging_graphs) {                              \
    ts_stack_print_dot_graph(self->stack, self->language, stderr); \
    fputs("\n\n", stderr);                                         \
  }

#define LOG_TREE()                                                        \
  if (self->print_debugging_graphs) {                                     \
    ts_tree_print_dot_graph(self->finished_tree, self->language, stderr); \
    fputs("\n", stderr);                                                  \
  }

#define SYM_NAME(symbol) ts_language_symbol_name(self->language, symbol)

static const unsigned MAX_VERSION_COUNT = 6;
static const unsigned MAX_SUMMARY_DEPTH = 16;
static const unsigned MAX_COST_DIFFERENCE = 16 * ERROR_COST_PER_SKIPPED_TREE;

typedef struct {
  unsigned cost;
  unsigned node_count;
  int dynamic_precedence;
  bool is_in_error;
} ErrorStatus;

typedef enum {
  ErrorComparisonTakeLeft,
  ErrorComparisonPreferLeft,
  ErrorComparisonNone,
  ErrorComparisonPreferRight,
  ErrorComparisonTakeRight,
} ErrorComparison;

static void parser__log(Parser *self) {
  if (self->lexer.logger.log) {
    self->lexer.logger.log(
      self->lexer.logger.payload,
      TSLogTypeParse,
      self->lexer.debug_buffer
    );
  }

  if (self->print_debugging_graphs) {
    fprintf(stderr, "graph {\nlabel=\"");
    for (char *c = &self->lexer.debug_buffer[0]; *c != 0; c++) {
      if (*c == '"') fputc('\\', stderr);
      fputc(*c, stderr);
    }
    fprintf(stderr, "\"\n}\n\n");
  }
}

static bool parser__breakdown_top_of_stack(Parser *self, StackVersion version) {
  bool did_break_down = false;
  bool pending = false;

  do {
    StackSliceArray pop = ts_stack_pop_pending(self->stack, version);
    if (!pop.size) break;

    did_break_down = true;
    pending = false;
    for (uint32_t i = 0; i < pop.size; i++) {
      StackSlice slice = pop.contents[i];
      TSStateId state = ts_stack_state(self->stack, slice.version);
      Tree *parent = *array_front(&slice.trees);

      for (uint32_t j = 0; j < parent->children.size; j++) {
        Tree *child = parent->children.contents[j];
        pending = child->children.size > 0;

        if (child->symbol == ts_builtin_sym_error) {
          state = ERROR_STATE;
        } else if (!child->extra) {
          state = ts_language_next_state(self->language, state, child->symbol);
        }

        ts_tree_retain(child);
        ts_stack_push(self->stack, slice.version, child, pending, state);
      }

      for (uint32_t j = 1; j < slice.trees.size; j++) {
        Tree *tree = slice.trees.contents[j];
        ts_stack_push(self->stack, slice.version, tree, false, state);
      }

      ts_tree_release(&self->tree_pool, parent);
      array_delete(&slice.trees);

      LOG("breakdown_top_of_stack tree:%s", SYM_NAME(parent->symbol));
      LOG_STACK();
    }
  } while (pending);

  return did_break_down;
}

static void parser__breakdown_lookahead(Parser *self, Tree **lookahead,
                                        TSStateId state,
                                        ReusableNode *reusable_node) {
  bool did_break_down = false;
  while (reusable_node->tree->children.size > 0 && reusable_node->tree->parse_state != state) {
    LOG("state_mismatch sym:%s", SYM_NAME(reusable_node->tree->symbol));
    reusable_node_breakdown(reusable_node);
    did_break_down = true;
  }

  if (did_break_down) {
    ts_tree_release(&self->tree_pool, *lookahead);
    ts_tree_retain(*lookahead = reusable_node->tree);
  }
}

static ErrorComparison parser__compare_versions(Parser *self, ErrorStatus a, ErrorStatus b) {
  if (!a.is_in_error && b.is_in_error) {
    if (a.cost < b.cost) {
      return ErrorComparisonTakeLeft;
    } else {
      return ErrorComparisonPreferLeft;
    }
  }

  if (a.is_in_error && !b.is_in_error) {
    if (b.cost < a.cost) {
      return ErrorComparisonTakeRight;
    } else {
      return ErrorComparisonPreferRight;
    }
  }

  if (a.cost < b.cost) {
    if ((b.cost - a.cost) * (1 + a.node_count) > MAX_COST_DIFFERENCE) {
      return ErrorComparisonTakeLeft;
    } else {
      return ErrorComparisonPreferLeft;
    }
  }

  if (b.cost < a.cost) {
    if ((a.cost - b.cost) * (1 + b.node_count) > MAX_COST_DIFFERENCE) {
      return ErrorComparisonTakeRight;
    } else {
      return ErrorComparisonPreferRight;
    }
  }

  if (a.dynamic_precedence > b.dynamic_precedence) return ErrorComparisonPreferLeft;
  if (b.dynamic_precedence > a.dynamic_precedence) return ErrorComparisonPreferRight;
  return ErrorComparisonNone;
}

static ErrorStatus parser__version_status(Parser *self, StackVersion version) {
  unsigned cost = ts_stack_error_cost(self->stack, version);
  bool is_paused = ts_stack_is_paused(self->stack, version);
  if (is_paused) cost += ERROR_COST_PER_SKIPPED_TREE;
  return (ErrorStatus) {
    .cost = cost,
    .node_count = ts_stack_node_count_since_error(self->stack, version),
    .dynamic_precedence = ts_stack_dynamic_precedence(self->stack, version),
    .is_in_error = is_paused || ts_stack_state(self->stack, version) == ERROR_STATE
  };
}

static bool parser__better_version_exists(Parser *self, StackVersion version,
                                          bool is_in_error, unsigned cost) {
  if (self->finished_tree && self->finished_tree->error_cost <= cost) return true;

  Length position = ts_stack_position(self->stack, version);
  ErrorStatus status = {
    .cost = cost,
    .is_in_error = is_in_error,
    .dynamic_precedence = ts_stack_dynamic_precedence(self->stack, version),
    .node_count = ts_stack_node_count_since_error(self->stack, version),
  };

  for (StackVersion i = 0, n = ts_stack_version_count(self->stack); i < n; i++) {
    if (i == version ||
        !ts_stack_is_active(self->stack, i) ||
        ts_stack_position(self->stack, i).bytes < position.bytes) continue;
    ErrorStatus status_i = parser__version_status(self, i);
    switch (parser__compare_versions(self, status, status_i)) {
      case ErrorComparisonTakeRight:
        return true;
      case ErrorComparisonPreferRight:
        if (ts_stack_can_merge(self->stack, i, version)) return true;
      default:
        break;
    }
  }

  return false;
}

static void parser__restore_external_scanner(Parser *self, Tree *external_token) {
  if (external_token) {
    self->language->external_scanner.deserialize(
      self->external_scanner_payload,
      ts_external_token_state_data(&external_token->external_token_state),
      external_token->external_token_state.length
    );
  } else {
    self->language->external_scanner.deserialize(self->external_scanner_payload, NULL, 0);
  }
}

static Tree *parser__lex(Parser *self, StackVersion version, TSStateId parse_state) {
  Length start_position = ts_stack_position(self->stack, version);
  Tree *external_token = ts_stack_last_external_token(self->stack, version);
  TSLexMode lex_mode = self->language->lex_modes[parse_state];
  const bool *valid_external_tokens = ts_language_enabled_external_tokens(
    self->language,
    lex_mode.external_lex_state
  );

  bool found_external_token = false;
  bool error_mode = parse_state == ERROR_STATE;
  bool skipped_error = false;
  int32_t first_error_character = 0;
  Length error_start_position = length_zero();
  Length error_end_position = length_zero();
  uint32_t last_byte_scanned = start_position.bytes;
  ts_lexer_reset(&self->lexer, start_position);

  for (;;) {
    Length current_position = self->lexer.current_position;

    if (valid_external_tokens) {
      LOG(
        "lex_external state:%d, row:%u, column:%u",
        lex_mode.external_lex_state,
        current_position.extent.row,
        current_position.extent.column
      );
      ts_lexer_start(&self->lexer);
      parser__restore_external_scanner(self, external_token);
      if (self->language->external_scanner.scan(
        self->external_scanner_payload,
        &self->lexer.data,
        valid_external_tokens
      )) {
        if (length_is_undefined(self->lexer.token_end_position)) {
          self->lexer.token_end_position = self->lexer.current_position;
        }

        if (!error_mode || self->lexer.token_end_position.bytes > current_position.bytes) {
          found_external_token = true;
          break;
        }
      }

      if (self->lexer.current_position.bytes > last_byte_scanned) {
        last_byte_scanned = self->lexer.current_position.bytes;
      }
      ts_lexer_reset(&self->lexer, current_position);
    }

    LOG(
      "lex_internal state:%d, row:%u, column:%u",
      lex_mode.lex_state,
      current_position.extent.row,
      current_position.extent.column
    );
    ts_lexer_start(&self->lexer);
    if (self->language->lex_fn(&self->lexer.data, lex_mode.lex_state)) {
      break;
    }

    if (!error_mode) {
      error_mode = true;
      lex_mode = self->language->lex_modes[ERROR_STATE];
      valid_external_tokens = ts_language_enabled_external_tokens(
        self->language,
        lex_mode.external_lex_state
      );
      if (self->lexer.current_position.bytes > last_byte_scanned) {
        last_byte_scanned = self->lexer.current_position.bytes;
      }
      ts_lexer_reset(&self->lexer, start_position);
      continue;
    }

    if (!skipped_error) {
      LOG("skip_unrecognized_character");
      skipped_error = true;
      error_start_position = self->lexer.token_start_position;
      error_end_position = self->lexer.token_start_position;
      first_error_character = self->lexer.data.lookahead;
    }

    if (self->lexer.current_position.bytes == error_end_position.bytes) {
      if (self->lexer.data.lookahead == 0) {
        self->lexer.data.result_symbol = ts_builtin_sym_error;
        break;
      }
      self->lexer.data.advance(&self->lexer, false);
    }

    error_end_position = self->lexer.current_position;
  }

  if (self->lexer.current_position.bytes > last_byte_scanned) {
    last_byte_scanned = self->lexer.current_position.bytes;
  }

  Tree *result;
  if (skipped_error) {
    Length padding = length_sub(error_start_position, start_position);
    Length size = length_sub(error_end_position, error_start_position);
    result = ts_tree_make_error(&self->tree_pool, size, padding, first_error_character, self->language);
  } else {
    if (self->lexer.token_end_position.bytes < self->lexer.token_start_position.bytes) {
      self->lexer.token_start_position = self->lexer.token_end_position;
    }

    TSSymbol symbol = self->lexer.data.result_symbol;
    Length padding = length_sub(self->lexer.token_start_position, start_position);
    Length size = length_sub(self->lexer.token_end_position, self->lexer.token_start_position);

    if (found_external_token) {
      symbol = self->language->external_scanner.symbol_map[symbol];
    } else if (symbol == self->language->keyword_capture_token && symbol != 0) {
      uint32_t end_byte = self->lexer.token_end_position.bytes;
      ts_lexer_reset(&self->lexer, self->lexer.token_start_position);
      ts_lexer_start(&self->lexer);
      if (
        self->language->keyword_lex_fn(&self->lexer.data, 0) &&
        self->lexer.token_end_position.bytes == end_byte &&
        ts_language_has_actions(self->language, parse_state, self->lexer.data.result_symbol)
      ) {
        symbol = self->lexer.data.result_symbol;
      }
    }

    result = ts_tree_make_leaf(&self->tree_pool, symbol, padding, size, self->language);

    if (found_external_token) {
      result->has_external_tokens = true;
      unsigned length = self->language->external_scanner.serialize(
        self->external_scanner_payload,
        self->lexer.debug_buffer
      );
      ts_external_token_state_init(&result->external_token_state, self->lexer.debug_buffer, length);
    }
  }

  result->bytes_scanned = last_byte_scanned - start_position.bytes + 1;
  result->parse_state = parse_state;
  result->first_leaf.lex_mode = lex_mode;

  LOG("lexed_lookahead sym:%s, size:%u", SYM_NAME(result->symbol), result->size.bytes);
  return result;
}

static Tree *parser__get_cached_token(Parser *self, size_t byte_index, Tree *last_external_token) {
  TokenCache *cache = &self->token_cache;
  if (cache->token &&
      cache->byte_index == byte_index &&
      ts_tree_external_token_state_eq(cache->last_external_token, last_external_token)) {
    return cache->token;
  } else {
    return NULL;
  }
}

static void parser__set_cached_token(Parser *self, size_t byte_index, Tree *last_external_token,
                                     Tree *token) {
  TokenCache *cache = &self->token_cache;
  if (token) ts_tree_retain(token);
  if (last_external_token) ts_tree_retain(last_external_token);
  if (cache->token) ts_tree_release(&self->tree_pool, cache->token);
  if (cache->last_external_token) ts_tree_release(&self->tree_pool, cache->last_external_token);
  cache->token = token;
  cache->byte_index = byte_index;
  cache->last_external_token = last_external_token;
}

static bool parser__can_reuse_first_leaf(Parser *self, TSStateId state, Tree *tree,
                                         TableEntry *table_entry,
                                         ReusableNode *next_reusable_node) {
  TSLexMode current_lex_mode = self->language->lex_modes[state];

  // If the token was created in a state with the same set of lookaheads, it is reusable.
  if (tree->first_leaf.lex_mode.lex_state == current_lex_mode.lex_state &&
      tree->first_leaf.lex_mode.external_lex_state == current_lex_mode.external_lex_state &&
      (tree->first_leaf.symbol != self->language->keyword_capture_token ||
       tree->parse_state == state)) return true;

  // Empty tokens are not reusable in states with different lookaheads.
  if (tree->size.bytes == 0 && tree->symbol != ts_builtin_sym_end) return false;

  // If the current state allows external tokens or other tokens that conflict with this
  // token, this token is not reusable.
  return current_lex_mode.external_lex_state == 0 && table_entry->is_reusable;
}

static Tree *parser__get_lookahead(Parser *self, StackVersion version, TSStateId *state,
                                   ReusableNode *reusable_node, TableEntry *table_entry) {
  Length position = ts_stack_position(self->stack, version);
  Tree *last_external_token = ts_stack_last_external_token(self->stack, version);

  Tree *result;
  while ((result = reusable_node->tree)) {
    if (reusable_node->byte_index > position.bytes) {
      LOG("before_reusable_node symbol:%s", SYM_NAME(result->symbol));
      break;
    }

    if (reusable_node->byte_index < position.bytes) {
      LOG("past_reusable_node symbol:%s", SYM_NAME(result->symbol));
      reusable_node_pop(reusable_node);
      continue;
    }

    if (!ts_tree_external_token_state_eq(reusable_node->last_external_token, last_external_token)) {
      LOG("reusable_node_has_different_external_scanner_state symbol:%s", SYM_NAME(result->symbol));
      reusable_node_pop(reusable_node);
      continue;
    }

    const char *reason = NULL;
    if (result->has_changes) {
      reason = "has_changes";
    } else if (result->symbol == ts_builtin_sym_error) {
      reason = "is_error";
    } else if (result->is_missing) {
      reason = "is_missing";
    } else if (result->fragile_left || result->fragile_right) {
      reason = "is_fragile";
    } else if (self->in_ambiguity && result->children.size) {
      reason = "in_ambiguity";
    }

    if (reason) {
      LOG("cant_reuse_node_%s tree:%s", reason, SYM_NAME(result->symbol));
      if (!reusable_node_breakdown(reusable_node)) {
        reusable_node_pop(reusable_node);
        parser__breakdown_top_of_stack(self, version);
        *state = ts_stack_state(self->stack, version);
      }
      continue;
    }

    ts_language_table_entry(self->language, *state, result->first_leaf.symbol, table_entry);
    ReusableNode next_reusable_node = reusable_node_after_leaf(reusable_node);
    if (!parser__can_reuse_first_leaf(self, *state, result, table_entry, &next_reusable_node)) {
      LOG(
        "cant_reuse_node symbol:%s, first_leaf_symbol:%s",
        SYM_NAME(result->symbol),
        SYM_NAME(result->first_leaf.symbol)
      );
      *reusable_node = next_reusable_node;
      break;
    }

    LOG("reuse_node symbol:%s", SYM_NAME(result->symbol));
    ts_tree_retain(result);
    return result;
  }

  if ((result = parser__get_cached_token(self, position.bytes, last_external_token))) {
    ts_language_table_entry(self->language, *state, result->first_leaf.symbol, table_entry);
    if (parser__can_reuse_first_leaf(self, *state, result, table_entry, NULL)) {
      ts_tree_retain(result);
      return result;
    }
  }

  result = parser__lex(self, version, *state);
  parser__set_cached_token(self, position.bytes, last_external_token, result);
  ts_language_table_entry(self->language, *state, result->symbol, table_entry);
  return result;
}

static bool parser__select_tree(Parser *self, Tree *left, Tree *right) {
  if (!left) return true;
  if (!right) return false;

  if (right->error_cost < left->error_cost) {
    LOG("select_smaller_error symbol:%s, over_symbol:%s",
        SYM_NAME(right->symbol), SYM_NAME(left->symbol));
    return true;
  }

  if (left->error_cost < right->error_cost) {
    LOG("select_smaller_error symbol:%s, over_symbol:%s",
        SYM_NAME(left->symbol), SYM_NAME(right->symbol));
    return false;
  }

  if (right->dynamic_precedence > left->dynamic_precedence) {
    LOG("select_higher_precedence symbol:%s, prec:%u, over_symbol:%s, other_prec:%u",
        SYM_NAME(right->symbol), right->dynamic_precedence, SYM_NAME(left->symbol),
        left->dynamic_precedence);
    return true;
  }

  if (left->dynamic_precedence > right->dynamic_precedence) {
    LOG("select_higher_precedence symbol:%s, prec:%u, over_symbol:%s, other_prec:%u",
        SYM_NAME(left->symbol), left->dynamic_precedence, SYM_NAME(right->symbol),
        right->dynamic_precedence);
    return false;
  }

  if (left->error_cost > 0) return true;

  int comparison = ts_tree_compare(left, right);
  switch (comparison) {
    case -1:
      LOG("select_earlier symbol:%s, over_symbol:%s", SYM_NAME(left->symbol),
          SYM_NAME(right->symbol));
      return false;
      break;
    case 1:
      LOG("select_earlier symbol:%s, over_symbol:%s", SYM_NAME(right->symbol),
          SYM_NAME(left->symbol));
      return true;
    default:
      LOG("select_existing symbol:%s, over_symbol:%s", SYM_NAME(left->symbol),
          SYM_NAME(right->symbol));
      return false;
  }
}

static void parser__shift(Parser *self, StackVersion version, TSStateId state,
                          Tree *lookahead, bool extra) {
  if (extra != lookahead->extra) {
    if (ts_stack_version_count(self->stack) > 1) {
      lookahead = ts_tree_make_copy(&self->tree_pool, lookahead);
    } else {
      ts_tree_retain(lookahead);
    }
    lookahead->extra = extra;
  } else {
    ts_tree_retain(lookahead);
  }

  bool is_pending = lookahead->children.size > 0;
  ts_stack_push(self->stack, version, lookahead, is_pending, state);
  if (lookahead->has_external_tokens) {
    ts_stack_set_last_external_token(
      self->stack, version, ts_tree_last_external_token(lookahead)
    );
  }
}

static bool parser__replace_children(Parser *self, Tree *tree, TreeArray *children) {
  self->scratch_tree = *tree;
  self->scratch_tree.children.size = 0;
  ts_tree_set_children(&self->scratch_tree, children, self->language);
  if (parser__select_tree(self, tree, &self->scratch_tree)) {
    *tree = self->scratch_tree;
    return true;
  } else {
    return false;
  }
}

static StackSliceArray parser__reduce(Parser *self, StackVersion version, TSSymbol symbol,
                                     uint32_t count, int dynamic_precedence,
                                     uint16_t alias_sequence_id, bool fragile) {
  uint32_t initial_version_count = ts_stack_version_count(self->stack);

  StackSliceArray pop = ts_stack_pop_count(self->stack, version, count);

  for (uint32_t i = 0; i < pop.size; i++) {
    StackSlice slice = pop.contents[i];

    // Extra tokens on top of the stack should not be included in this new parent
    // node. They will be re-pushed onto the stack after the parent node is
    // created and pushed.
    TreeArray children = slice.trees;
    while (children.size > 0 && children.contents[children.size - 1]->extra) {
      children.size--;
    }

    Tree *parent = ts_tree_make_node(&self->tree_pool,
      symbol, &children, alias_sequence_id, self->language
    );

    // This pop operation may have caused multiple stack versions to collapse
    // into one, because they all diverged from a common state. In that case,
    // choose one of the arrays of trees to be the parent node's children, and
    // delete the rest of the tree arrays.
    while (i + 1 < pop.size) {
      StackSlice next_slice = pop.contents[i + 1];
      if (next_slice.version != slice.version) break;
      i++;

      TreeArray children = next_slice.trees;
      while (children.size > 0 && children.contents[children.size - 1]->extra) {
        children.size--;
      }

      if (parser__replace_children(self, parent, &children)) {
        ts_tree_array_delete(&self->tree_pool, &slice.trees);
        slice = next_slice;
      } else {
        ts_tree_array_delete(&self->tree_pool, &next_slice.trees);
      }
    }

    parent->dynamic_precedence += dynamic_precedence;
    parent->alias_sequence_id = alias_sequence_id;

    TSStateId state = ts_stack_state(self->stack, slice.version);
    TSStateId next_state = ts_language_next_state(self->language, state, symbol);
    if (fragile || self->in_ambiguity || pop.size > 1 || initial_version_count > 1) {
      parent->fragile_left = true;
      parent->fragile_right = true;
      parent->parse_state = TS_TREE_STATE_NONE;
    } else {
      parent->parse_state = state;
    }

    // Push the parent node onto the stack, along with any extra tokens that
    // were previously on top of the stack.
    ts_stack_push(self->stack, slice.version, parent, false, next_state);
    for (uint32_t j = parent->children.size; j < slice.trees.size; j++) {
      ts_stack_push(self->stack, slice.version, slice.trees.contents[j], false, next_state);
    }

    if (ts_stack_version_count(self->stack) > MAX_VERSION_COUNT) {
      i++;
      while (i < pop.size) {
        StackSlice slice = pop.contents[i];
        ts_tree_array_delete(&self->tree_pool, &slice.trees);
        ts_stack_halt(self->stack, slice.version);
        i++;
      }
      while (ts_stack_version_count(self->stack) > slice.version + 1) {
        ts_stack_remove_version(self->stack, slice.version + 1);
      }
      break;
    }
  }

  for (StackVersion i = initial_version_count; i < ts_stack_version_count(self->stack); i++) {
    for (StackVersion j = initial_version_count; j < i; j++) {
      if (ts_stack_merge(self->stack, j, i)) {
        i--;
        break;
      }
    }
  }

  return pop;
}

static void parser__start(Parser *self, TSInput input, Tree *previous_tree) {
  if (previous_tree) {
    LOG("parse_after_edit");
  } else {
    LOG("new_parse");
  }

  if (self->language->external_scanner.deserialize) {
    self->language->external_scanner.deserialize(self->external_scanner_payload, NULL, 0);
  }

  ts_lexer_set_input(&self->lexer, input);
  ts_stack_clear(self->stack);
  self->reusable_node = reusable_node_new(previous_tree);
  self->finished_tree = NULL;
  self->accept_count = 0;
  self->in_ambiguity = false;
}

static void parser__accept(Parser *self, StackVersion version, Tree *lookahead) {
  lookahead->extra = true;
  assert(lookahead->symbol == ts_builtin_sym_end);
  ts_tree_retain(lookahead);
  ts_stack_push(self->stack, version, lookahead, false, 1);

  StackSliceArray pop = ts_stack_pop_all(self->stack, version);
  for (uint32_t i = 0; i < pop.size; i++) {
    TreeArray trees = pop.contents[i].trees;

    Tree *root = NULL;
    for (uint32_t j = trees.size - 1; j + 1 > 0; j--) {
      Tree *child = trees.contents[j];
      if (!child->extra) {
        for (uint32_t k = 0; k < child->children.size; k++) {
          ts_tree_retain(child->children.contents[k]);
        }
        array_splice(&trees, j, 1, &child->children);
        root = ts_tree_make_node(
          &self->tree_pool, child->symbol, &trees,
          child->alias_sequence_id, self->language
        );
        ts_tree_release(&self->tree_pool, child);
        break;
      }
    }

    assert(root && root->ref_count > 0);
    self->accept_count++;

    if (self->finished_tree) {
      if (parser__select_tree(self, self->finished_tree, root)) {
        ts_tree_release(&self->tree_pool, self->finished_tree);
        self->finished_tree = root;
      } else {
        ts_tree_release(&self->tree_pool, root);
      }
    } else {
      self->finished_tree = root;
    }
  }

  ts_stack_remove_version(self->stack, pop.contents[0].version);
  ts_stack_halt(self->stack, version);
}

static bool parser__do_all_potential_reductions(Parser *self, StackVersion starting_version,
                                                TSSymbol lookahead_symbol) {
  uint32_t initial_version_count = ts_stack_version_count(self->stack);

  bool can_shift_lookahead_symbol = false;
  StackVersion version = starting_version;
  for (unsigned i = 0; true; i++) {
    uint32_t version_count = ts_stack_version_count(self->stack);
    if (version >= version_count) break;

    bool merged = false;
    for (StackVersion i = initial_version_count; i < version; i++) {
      if (ts_stack_merge(self->stack, i, version)) {
        merged = true;
        break;
      }
    }
    if (merged) continue;

    TSStateId state = ts_stack_state(self->stack, version);
    bool has_shift_action = false;
    array_clear(&self->reduce_actions);

    TSSymbol first_symbol, end_symbol;
    if (lookahead_symbol != 0) {
      first_symbol = lookahead_symbol;
      end_symbol = lookahead_symbol + 1;
    } else {
      first_symbol = 1;
      end_symbol = self->language->token_count;
    }

    for (TSSymbol symbol = first_symbol; symbol < end_symbol; symbol++) {
      TableEntry entry;
      ts_language_table_entry(self->language, state, symbol, &entry);
      for (uint32_t i = 0; i < entry.action_count; i++) {
        TSParseAction action = entry.actions[i];
        switch (action.type) {
          case TSParseActionTypeShift:
          case TSParseActionTypeRecover:
            if (!action.params.extra && !action.params.repetition) has_shift_action = true;
            break;
          case TSParseActionTypeReduce:
            if (action.params.child_count > 0)
              ts_reduce_action_set_add(&self->reduce_actions, (ReduceAction){
                .symbol = action.params.symbol,
                .count = action.params.child_count,
                .dynamic_precedence = action.params.dynamic_precedence,
                .alias_sequence_id = action.params.alias_sequence_id,
              });
          default:
            break;
        }
      }
    }

    for (uint32_t i = 0; i < self->reduce_actions.size; i++) {
      ReduceAction action = self->reduce_actions.contents[i];

      parser__reduce(
        self, version, action.symbol, action.count,
        action.dynamic_precedence, action.alias_sequence_id,
        true
      );
    }

    if (has_shift_action) {
      can_shift_lookahead_symbol = true;
    } else if (self->reduce_actions.size > 0 && i < MAX_VERSION_COUNT) {
      ts_stack_renumber_version(self->stack, version_count, version);
      continue;
    } else if (lookahead_symbol != 0) {
      ts_stack_remove_version(self->stack, version);
    }

    if (version == starting_version) {
      version = version_count;
    } else {
      version++;
    }
  }

  return can_shift_lookahead_symbol;
}

static void parser__handle_error(Parser *self, StackVersion version, TSSymbol lookahead_symbol) {
  // Perform any reductions that could have happened in this state, regardless of the lookahead.
  uint32_t previous_version_count = ts_stack_version_count(self->stack);
  parser__do_all_potential_reductions(self, version, 0);
  uint32_t version_count = ts_stack_version_count(self->stack);

  // Push a discontinuity onto the stack. Merge all of the stack versions that
  // were created in the previous step.
  bool did_insert_missing_token = false;
  for (StackVersion v = version; v < version_count;) {
    if (!did_insert_missing_token) {
      TSStateId state = ts_stack_state(self->stack, v);
      for (TSSymbol missing_symbol = 1;
           missing_symbol < self->language->token_count;
           missing_symbol++) {
        TSStateId state_after_missing_symbol = ts_language_next_state(
          self->language, state, missing_symbol
        );
        if (state_after_missing_symbol == 0) continue;

        if (ts_language_has_reduce_action(
          self->language,
          state_after_missing_symbol,
          lookahead_symbol
        )) {
          StackVersion version_with_missing_tree = ts_stack_copy_version(self->stack, v);
          Tree *missing_tree = ts_tree_make_missing_leaf(&self->tree_pool, missing_symbol, self->language);
          ts_stack_push(
            self->stack, version_with_missing_tree,
            missing_tree, false,
            state_after_missing_symbol
          );

          if (parser__do_all_potential_reductions(
            self, version_with_missing_tree,
            lookahead_symbol
          )) {
            LOG(
              "recover_with_missing symbol:%s, state:%u",
              SYM_NAME(missing_symbol),
              ts_stack_state(self->stack, version_with_missing_tree)
            );
            did_insert_missing_token = true;
            break;
          }
        }
      }
    }

    ts_stack_push(self->stack, v, NULL, false, ERROR_STATE);
    v = (v == version) ? previous_version_count : v + 1;
  }

  for (unsigned i = previous_version_count; i < version_count; i++) {
    assert(ts_stack_merge(self->stack, version, previous_version_count));
  }

  ts_stack_record_summary(self->stack, version, MAX_SUMMARY_DEPTH);
  LOG_STACK();
}

static void parser__halt_parse(Parser *self) {
  LOG("halting_parse");
  LOG_STACK();

  ts_lexer_advance_to_end(&self->lexer);
  Length remaining_length = length_sub(
    self->lexer.current_position,
    ts_stack_position(self->stack, 0)
  );

  Tree *filler_node = ts_tree_make_error(&self->tree_pool, remaining_length, length_zero(), 0, self->language);
  filler_node->visible = false;
  ts_stack_push(self->stack, 0, filler_node, false, 0);

  TreeArray children = array_new();
  Tree *root_error = ts_tree_make_error_node(&self->tree_pool, &children, self->language);
  ts_stack_push(self->stack, 0, root_error, false, 0);

  Tree *eof = ts_tree_make_leaf(&self->tree_pool, ts_builtin_sym_end, length_zero(), length_zero(), self->language);
  parser__accept(self, 0, eof);
  ts_tree_release(&self->tree_pool, eof);
}

static bool parser__recover_to_state(Parser *self, StackVersion version, unsigned depth,
                                     TSStateId goal_state) {
  StackSliceArray pop = ts_stack_pop_count(self->stack, version, depth);
  StackVersion previous_version = STACK_VERSION_NONE;

  for (unsigned i = 0; i < pop.size; i++) {
    StackSlice slice = pop.contents[i];

    if (slice.version == previous_version) {
      ts_tree_array_delete(&self->tree_pool, &slice.trees);
      array_erase(&pop, i--);
      continue;
    }

    if (ts_stack_state(self->stack, slice.version) != goal_state) {
      ts_stack_halt(self->stack, slice.version);
      ts_tree_array_delete(&self->tree_pool, &slice.trees);
      array_erase(&pop, i--);
      continue;
    }

    TreeArray error_trees = ts_stack_pop_error(self->stack, slice.version);
    if (error_trees.size > 0) {
      assert(error_trees.size == 1);
      array_splice(&slice.trees, 0, 0, &error_trees.contents[0]->children);
      for (unsigned j = 0; j < error_trees.contents[0]->children.size; j++) {
        ts_tree_retain(slice.trees.contents[j]);
      }
      ts_tree_array_delete(&self->tree_pool, &error_trees);
    }

    TreeArray trailing_extras = ts_tree_array_remove_trailing_extras(&slice.trees);

    if (slice.trees.size > 0) {
      Tree *error = ts_tree_make_error_node(&self->tree_pool, &slice.trees, self->language);
      error->extra = true;
      ts_stack_push(self->stack, slice.version, error, false, goal_state);
    } else {
      array_delete(&slice.trees);
    }

    for (unsigned j = 0; j < trailing_extras.size; j++) {
      Tree *tree = trailing_extras.contents[j];
      ts_stack_push(self->stack, slice.version, tree, false, goal_state);
    }

    previous_version = slice.version;
    array_delete(&trailing_extras);
  }

  return previous_version != STACK_VERSION_NONE;
}

static void parser__recover(Parser *self, StackVersion version, Tree *lookahead) {
  bool did_recover = false;
  unsigned previous_version_count = ts_stack_version_count(self->stack);
  Length position = ts_stack_position(self->stack, version);
  StackSummary *summary = ts_stack_get_summary(self->stack, version);
  unsigned node_count_since_error = ts_stack_node_count_since_error(self->stack, version);
  unsigned current_error_cost = ts_stack_error_cost(self->stack, version);

  if (summary && lookahead->symbol != ts_builtin_sym_error) {
    for (unsigned i = 0; i < summary->size; i++) {
      StackSummaryEntry entry = summary->contents[i];

      if (entry.state == ERROR_STATE) continue;
      if (entry.position.bytes == position.bytes) continue;
      unsigned depth = entry.depth;
      if (node_count_since_error > 0) depth++;

      bool would_merge = false;
      for (unsigned j = 0; j < previous_version_count; j++) {
        if (
          ts_stack_state(self->stack, j) == entry.state &&
          ts_stack_position(self->stack, j).bytes == position.bytes
        ) {
          would_merge = true;
          break;
        }
      }

      if (would_merge) continue;

      unsigned new_cost =
        current_error_cost +
        entry.depth * ERROR_COST_PER_SKIPPED_TREE +
        (position.bytes - entry.position.bytes) * ERROR_COST_PER_SKIPPED_CHAR +
        (position.extent.row - entry.position.extent.row) * ERROR_COST_PER_SKIPPED_LINE;
      if (parser__better_version_exists(self, version, false, new_cost)) break;

      if (ts_language_has_actions(self->language, entry.state, lookahead->symbol)) {
        if (parser__recover_to_state(self, version, depth, entry.state)) {
          did_recover = true;
          LOG("recover_to_previous state:%u, depth:%u", entry.state, depth);
          LOG_STACK();
          break;
        }
      }
    }
  }

  for (unsigned i = previous_version_count; i < ts_stack_version_count(self->stack); i++) {
    if (!ts_stack_is_active(self->stack, i)) {
      ts_stack_remove_version(self->stack, i--);
    }
  }

  if (did_recover && ts_stack_version_count(self->stack) > MAX_VERSION_COUNT) {
    ts_stack_halt(self->stack, version);
    return;
  }

  if (lookahead->symbol == ts_builtin_sym_end) {
    LOG("recover_eof");
    TreeArray children = array_new();
    Tree *parent = ts_tree_make_error_node(&self->tree_pool, &children, self->language);
    ts_stack_push(self->stack, version, parent, false, 1);
    parser__accept(self, version, lookahead);
    return;
  }

  unsigned new_cost =
    current_error_cost + ERROR_COST_PER_SKIPPED_TREE +
    ts_tree_total_bytes(lookahead) * ERROR_COST_PER_SKIPPED_CHAR +
    ts_tree_total_size(lookahead).extent.row * ERROR_COST_PER_SKIPPED_LINE;

  if (parser__better_version_exists(self, version, false, new_cost)) {
    ts_stack_halt(self->stack, version);
    return;
  }

  unsigned n;
  const TSParseAction *actions = ts_language_actions(self->language, 1, lookahead->symbol, &n);
  if (n > 0 && actions[n - 1].type == TSParseActionTypeShift && actions[n - 1].params.extra) {
    lookahead->extra = true;
  }

  LOG("skip_token symbol:%s", SYM_NAME(lookahead->symbol));
  ts_tree_retain(lookahead);
  TreeArray children = array_new();
  array_reserve(&children, 1);
  array_push(&children, lookahead);
  Tree *error_repeat = ts_tree_make_node(
    &self->tree_pool,
    ts_builtin_sym_error_repeat,
    &children,
    0,
    self->language
  );

  if (node_count_since_error > 0) {
    StackSliceArray pop = ts_stack_pop_count(self->stack, version, 1);
    assert(pop.size == 1);
    assert(pop.contents[0].trees.size == 1);
    ts_stack_renumber_version(self->stack, pop.contents[0].version, version);
    array_push(&pop.contents[0].trees, error_repeat);
    error_repeat = ts_tree_make_node(
      &self->tree_pool,
      ts_builtin_sym_error_repeat,
      &pop.contents[0].trees,
      0,
      self->language
    );
  }

  ts_stack_push(self->stack, version, error_repeat, false, ERROR_STATE);

  if (lookahead->has_external_tokens) {
    ts_stack_set_last_external_token(
      self->stack, version, ts_tree_last_external_token(lookahead)
    );
  }
}

static void parser__advance(Parser *self, StackVersion version, ReusableNode *reusable_node) {
  TSStateId state = ts_stack_state(self->stack, version);
  TableEntry table_entry;
  Tree *lookahead = parser__get_lookahead(self, version, &state, reusable_node, &table_entry);

  for (;;) {
    StackVersion last_reduction_version = STACK_VERSION_NONE;

    for (uint32_t i = 0; i < table_entry.action_count; i++) {
      TSParseAction action = table_entry.actions[i];

      switch (action.type) {
        case TSParseActionTypeShift: {
          if (action.params.repetition) break;
          TSStateId next_state;
          if (action.params.extra) {

            // TODO remove when TREE_SITTER_LANGUAGE_VERSION 9 is out.
            if (state == ERROR_STATE) continue;

            next_state = state;
            LOG("shift_extra");
          } else {
            next_state = action.params.state;
            LOG("shift state:%u", next_state);
          }

          if (lookahead->children.size > 0) {
            parser__breakdown_lookahead(self, &lookahead, state, reusable_node);
            next_state = ts_language_next_state(self->language, state, lookahead->symbol);
          }

          parser__shift(self, version, next_state, lookahead, action.params.extra);
          if (lookahead == reusable_node->tree) reusable_node_pop(reusable_node);
          ts_tree_release(&self->tree_pool, lookahead);
          return;
        }

        case TSParseActionTypeReduce: {
          bool is_fragile = table_entry.action_count > 1;
          LOG("reduce sym:%s, child_count:%u", SYM_NAME(action.params.symbol), action.params.child_count);
          StackSliceArray reduction = parser__reduce(
            self, version, action.params.symbol, action.params.child_count,
            action.params.dynamic_precedence, action.params.alias_sequence_id,
            is_fragile
          );
          StackSlice slice = *array_front(&reduction);
          last_reduction_version = slice.version;
          break;
        }

        case TSParseActionTypeAccept: {
          LOG("accept");
          parser__accept(self, version, lookahead);
          ts_tree_release(&self->tree_pool, lookahead);
          return;
        }

        case TSParseActionTypeRecover: {
          while (lookahead->children.size > 0) {
            parser__breakdown_lookahead(self, &lookahead, state, reusable_node);
          }
          parser__recover(self, version, lookahead);
          if (lookahead == reusable_node->tree) reusable_node_pop(reusable_node);
          ts_tree_release(&self->tree_pool, lookahead);
          return;
        }
      }
    }

    if (last_reduction_version != STACK_VERSION_NONE) {
      ts_stack_renumber_version(self->stack, last_reduction_version, version);
      LOG_STACK();
    } else if (state == ERROR_STATE) {
      parser__recover(self, version, lookahead);
      ts_tree_release(&self->tree_pool, lookahead);
      return;
    } else if (!parser__breakdown_top_of_stack(self, version)) {
      LOG("detect_error");
      ts_stack_pause(self->stack, version, lookahead->first_leaf.symbol);
      ts_tree_release(&self->tree_pool, lookahead);
      return;
    }

    state = ts_stack_state(self->stack, version);
    ts_language_table_entry(self->language, state, lookahead->first_leaf.symbol, &table_entry);
  }
}

static unsigned parser__condense_stack(Parser *self) {
  bool made_changes = false;
  unsigned min_error_cost = UINT_MAX;
  for (StackVersion i = 0; i < ts_stack_version_count(self->stack); i++) {
    if (ts_stack_is_halted(self->stack, i)) {
      ts_stack_remove_version(self->stack, i);
      i--;
      continue;
    }

    ErrorStatus status_i = parser__version_status(self, i);
    if (!status_i.is_in_error && status_i.cost < min_error_cost) {
      min_error_cost = status_i.cost;
    }

    for (StackVersion j = 0; j < i; j++) {
      ErrorStatus status_j = parser__version_status(self, j);

      switch (parser__compare_versions(self, status_j, status_i)) {
        case ErrorComparisonTakeLeft:
          made_changes = true;
          ts_stack_remove_version(self->stack, i);
          i--;
          j = i;
          break;
        case ErrorComparisonPreferLeft:
        case ErrorComparisonNone:
          if (ts_stack_merge(self->stack, j, i)) {
            made_changes = true;
            i--;
            j = i;
          }
          break;
        case ErrorComparisonPreferRight:
          made_changes = true;
          if (ts_stack_merge(self->stack, j, i)) {
            i--;
            j = i;
          } else {
            ts_stack_swap_versions(self->stack, i, j);
          }
          break;
        case ErrorComparisonTakeRight:
          made_changes = true;
          ts_stack_remove_version(self->stack, j);
          i--;
          j--;
          break;
      }
    }
  }

  while (ts_stack_version_count(self->stack) > MAX_VERSION_COUNT) {
    ts_stack_remove_version(self->stack, MAX_VERSION_COUNT);
    made_changes = true;
  }

  if (ts_stack_version_count(self->stack) > 0) {
    bool has_unpaused_version = false;
    for (StackVersion i = 0, n = ts_stack_version_count(self->stack); i < n; i++) {
      if (ts_stack_is_paused(self->stack, i)) {
        if (!has_unpaused_version && self->accept_count < MAX_VERSION_COUNT) {
          LOG("resume version:%u", i);
          min_error_cost = ts_stack_error_cost(self->stack, i);
          TSSymbol lookahead_symbol = ts_stack_resume(self->stack, i);
          parser__handle_error(self, i, lookahead_symbol);
          has_unpaused_version = true;
        } else {
          ts_stack_remove_version(self->stack, i);
          i--;
          n--;
        }
      } else {
        has_unpaused_version = true;
      }
    }
  }

  if (made_changes) {
    LOG("condense");
    LOG_STACK();
  }

  return min_error_cost;
}

bool parser_init(Parser *self) {
  ts_lexer_init(&self->lexer);
  array_init(&self->reduce_actions);
  array_reserve(&self->reduce_actions, 4);
  ts_tree_pool_init(&self->tree_pool);
  self->stack = ts_stack_new(&self->tree_pool);
  self->finished_tree = NULL;
  parser__set_cached_token(self, 0, NULL, NULL);
  return true;
}

void parser_set_language(Parser *self, const TSLanguage *language) {
  if (self->external_scanner_payload && self->language->external_scanner.destroy)
    self->language->external_scanner.destroy(self->external_scanner_payload);

  if (language && language->external_scanner.create)
    self->external_scanner_payload = language->external_scanner.create();
  else
    self->external_scanner_payload = NULL;

  self->language = language;
}

void parser_destroy(Parser *self) {
  if (self->stack)
    ts_stack_delete(self->stack);
  if (self->reduce_actions.contents)
    array_delete(&self->reduce_actions);
  ts_tree_pool_delete(&self->tree_pool);
  parser_set_language(self, NULL);
}

Tree *parser_parse(Parser *self, TSInput input, Tree *old_tree, bool halt_on_error) {
  parser__start(self, input, old_tree);

  StackVersion version = STACK_VERSION_NONE;
  uint32_t position = 0, last_position = 0;
  ReusableNode reusable_node;

  do {
    for (version = 0; version < ts_stack_version_count(self->stack); version++) {
      reusable_node = self->reusable_node;

      while (ts_stack_is_active(self->stack, version)) {
        LOG("process version:%d, version_count:%u, state:%d, row:%u, col:%u",
            version, ts_stack_version_count(self->stack),
            ts_stack_state(self->stack, version),
            ts_stack_position(self->stack, version).extent.row,
            ts_stack_position(self->stack, version).extent.column);

        parser__advance(self, version, &reusable_node);
        LOG_STACK();

        position = ts_stack_position(self->stack, version).bytes;
        if (position > last_position || (version > 0 && position == last_position)) {
          last_position = position;
          break;
        }
      }
    }

    self->reusable_node = reusable_node;

    unsigned min_error_cost = parser__condense_stack(self);
    if (self->finished_tree && self->finished_tree->error_cost < min_error_cost) {
      break;
    } else if (halt_on_error && min_error_cost > 0) {
      parser__halt_parse(self);
      break;
    }

    self->in_ambiguity = version > 1;
  } while (version != 0);

  ts_stack_clear(self->stack);
  parser__set_cached_token(self, 0, NULL, NULL);
  ts_tree_assign_parents(self->finished_tree, &self->tree_pool, self->language);

  LOG("done");
  LOG_TREE();
  return self->finished_tree;
}
