/**
 * Tree-sitter grammar for sw — the language driving the SwarmRT runtime.
 *
 * Goals:
 *   - Accurate enough to highlight every construct the actual lexer
 *     emits (modules, functions, case, receive, try/catch, f-strings,
 *     pattern matches, pipes, maps, lists, atoms, tuples, send/spawn).
 *   - Tolerant enough to keep highlighting half-typed code (most rules
 *     accept partial input gracefully).
 *
 * To build:  npm i ; npx tree-sitter generate ; npx tree-sitter test
 * Editor wiring lives under queries/highlights.scm (separate file).
 */

module.exports = grammar({
  name: 'sw',

  extras: $ => [
    /\s/,
    $.line_comment,
  ],

  word: $ => $.identifier,

  rules: {
    source_file: $ => seq(
      optional($.module_decl),
      repeat(choice($.import_decl, $.export_decl, $.function_definition)),
    ),

    line_comment: $ => token(seq('#', /[^\n]*/)),

    module_decl: $ => seq('module', field('name', $.module_name)),
    import_decl: $ => seq('import', field('name', $.module_name)),
    export_decl: $ => seq('export', '[', sepBy(',', $.identifier), ']'),

    module_name: $ => /[A-Z][A-Za-z0-9_]*/,

    function_definition: $ => seq(
      'fun',
      field('name', $.identifier),
      '(',
      optional(sepBy(',', $.parameter)),
      ')',
      field('body', $.block),
    ),

    parameter: $ => seq(
      field('name', $.identifier),
      optional(seq('=', field('default', $._expression))),
    ),

    block: $ => seq('{', repeat(seq($._statement, optional(';'))), '}'),

    _statement: $ => choice(
      $.assignment,
      $._expression,
    ),

    assignment: $ => prec.right(seq(
      field('name', $.identifier),
      '=',
      field('value', $._expression),
    )),

    _expression: $ => choice(
      $.binary,
      $.unary,
      $.pipe,
      $.call,
      $.spawn_expression,
      $.send_expression,
      $.if_expression,
      $.case_expression,
      $.receive_expression,
      $.try_expression,
      $.anonymous_fun,
      $.tuple,
      $.list,
      $.list_cons,
      $.map,
      $.range,
      $.member_access,
      $.module_call,
      $._primary,
    ),

    binary: $ => choice(
      ...[
        ['+',  10], ['-', 10],
        ['*',  11], ['/', 11], ['%', 11],
        ['++', 9],
        ['==', 5], ['!=', 5], ['<', 5], ['>', 5], ['<=', 5], ['>=', 5],
        ['&&', 4], ['||', 3],
      ].map(([op, p]) =>
        prec.left(p, seq(
          field('left', $._expression),
          field('op', op),
          field('right', $._expression),
        ))
      )
    ),

    unary: $ => prec(12, seq(field('op', '-'), field('operand', $._expression))),

    pipe: $ => prec.left(2, seq(
      field('value', $._expression),
      '|>',
      field('func', $._expression),
    )),

    call: $ => prec(15, seq(
      field('callee', choice($.identifier, $.member_access)),
      '(',
      optional(sepBy(',', $._expression)),
      ')',
    )),

    /* Module.func — for cross-module calls */
    module_call: $ => prec(16, seq(
      field('module', $.module_name),
      '.',
      field('name', $.identifier),
      '(',
      optional(sepBy(',', $._expression)),
      ')',
    )),

    member_access: $ => prec(16, seq(
      field('object', $._expression),
      '.',
      field('field', $.identifier),
    )),

    spawn_expression: $ => seq('spawn', '(', $._expression, ')'),
    send_expression: $ => seq('send', '(',
      field('to', $._expression), ',', field('msg', $._expression), ')'),

    if_expression: $ => prec.right(seq(
      'if', '(', field('condition', $._expression), ')',
      field('then', $.block),
      optional(seq('else', choice($.block, $.if_expression))),
    )),

    case_expression: $ => seq(
      'case', field('subject', $._expression),
      '{',
      repeat(seq($.case_clause, optional(';'))),
      '}',
    ),

    case_clause: $ => seq(
      field('pattern', $._pattern),
      optional(seq('when', field('guard', $._expression))),
      '->',
      field('body', $._statement),
    ),

    receive_expression: $ => seq(
      'receive',
      '{',
      repeat(seq($.case_clause, optional(';'))),
      optional($.after_clause),
      '}',
    ),

    after_clause: $ => seq('after', field('timeout', $._expression),
                          '{', field('body', $._statement), '}'),

    try_expression: $ => seq(
      'try', $.block,
      'catch', field('error', $.identifier),
      $.block,
    ),

    anonymous_fun: $ => seq(
      'fun', '(',
      optional(sepBy(',', $.parameter)),
      ')',
      $.block,
    ),

    /* Patterns reuse most of the expression surface; an identifier in
     * a pattern position is a binding (the grammar can't distinguish
     * — that's a semantic check). _is_ a syntactic catchall. */
    _pattern: $ => choice(
      $.integer,
      $.float,
      $.string,
      $.fstring,
      $.atom,
      $.identifier,
      $.tuple,
      $.list,
      $.list_cons,
      $.map,
      $.wildcard,
    ),

    wildcard: $ => '_',

    tuple: $ => seq('{', optional(sepBy(',', $._expression)), '}'),

    list: $ => seq('[', optional(sepBy(',', $._expression)), ']'),

    list_cons: $ => seq('[',
      field('head', $._expression), '|', field('tail', $._expression),
      ']'),

    map: $ => seq('%{',
      optional(sepBy(',', $.map_entry)),
    '}'),

    map_entry: $ => seq(
      field('key', choice($.atom_shorthand_key, $._expression)),
      choice(':', '=>'),
      field('value', $._expression),
    ),

    /* `name: val` shorthand inside %{} — `name` becomes the atom 'name'. */
    atom_shorthand_key: $ => /[a-z_][A-Za-z0-9_]*/,

    range: $ => seq($._expression, '..', $._expression),

    _primary: $ => choice(
      $.integer,
      $.float,
      $.string,
      $.fstring,
      $.atom,
      $.boolean,
      $.nil,
      $.self_expression,
      $.identifier,
      seq('(', $._expression, ')'),
    ),

    integer: $ => /-?\d[\d_]*/,
    float: $ => /-?\d[\d_]*\.\d+([eE][+-]?\d+)?/,
    string: $ => seq('"', repeat(choice(
      $.string_escape,
      $.string_interp,
      /[^"\\#]+/,
      /#/,
    )), '"'),
    fstring: $ => seq('f"', repeat(choice(
      $.string_escape,
      $.fstring_interp,
      /[^"\\{]+/,
    )), '"'),
    string_escape: $ => /\\./,
    string_interp: $ => seq('#{', $._expression, '}'),
    fstring_interp: $ => seq('{', $._expression, '}'),

    atom: $ => /'[A-Za-z_][A-Za-z0-9_]*'/,
    boolean: $ => choice('true', 'false'),
    nil: $ => 'nil',
    self_expression: $ => seq('self', '(', ')'),

    identifier: $ => /[a-z_][A-Za-z0-9_]*/,
  },
});

function sepBy(sep, rule) {
  return seq(rule, repeat(seq(sep, rule)), optional(sep));
}
