module.exports = grammar({
  name: "feng",

  extras: ($) => [
    /\s/,
  ],

  rules: {
    source_file: ($) => repeat($._token),

    _token: ($) => choice(
      $.doc_comment,
      $.line_comment,
      $.block_comment,
      $.string,
      $.number,
      $.boolean,
      $.builtin_type,
      $.self_keyword,
      $.keyword,
      $.call_expression,
      $.member_expression,
      $.identifier,
      $.operator,
      $.l_paren,
      $.r_paren,
      $.l_brace,
      $.r_brace,
      $.l_bracket,
      $.r_bracket,
      $.dot,
      $.comma,
      $.colon,
      $.semicolon,
      $.unknown
    ),

    doc_comment: () => token(/\/\*\*[\s\S]*?\*\//),
    block_comment: () => token(/\/\*(?!\*)[\s\S]*?\*\//),
    line_comment: () => token(/\/\/[^\n]*/),

    string: () => seq(
      '"',
      repeat(choice(/[^"\\\n]+/, $.escape_sequence)),
      '"'
    ),
    escape_sequence: () => token(/\\./),

    number: () => token(choice(
      /0[xX][0-9a-fA-F]+/,
      /0[bB][01]+/,
      /0[oO][0-7]+/,
      /\d+(?:\.\d+)?/
    )),

    boolean: () => token(choice("true", "false")),

    builtin_type: () => token(choice(
      "bool",
      "string",
      "int",
      "long",
      "byte",
      "float",
      "double",
      "i8",
      "i16",
      "i32",
      "i64",
      "u8",
      "u16",
      "u32",
      "u64",
      "f32",
      "f64",
      "void"
    )),

    self_keyword: () => token("self"),

    keyword: () => token(choice(
      "module",
      "import",
      "as",
      "func",
      "type",
      "spec",
      "fit",
      "let",
      "var",
      "return",
      "if",
      "else",
      "for",
      "while",
      "break",
      "continue",
      "match",
      "case",
      "defer",
      "throw",
      "try",
      "catch",
      "finally",
      "extern",
      "pub",
      "use"
    )),

    call_expression: ($) => prec.left(seq(
      field("function", $.identifier),
      $.l_paren,
      optional(seq($.identifier, repeat(seq($.comma, $.identifier)))),
      $.r_paren
    )),

    member_expression: ($) => prec.left(seq(
      field("object", choice($.identifier, $.self_keyword)),
      $.dot,
      field("member", $.identifier)
    )),

    identifier: () => token(/[A-Za-z_][A-Za-z0-9_]*/),

    operator: () => token(choice(
      "==", "!=", "<=", ">=", "&&", "||", "->", "=>",
      "+", "-", "*", "/", "%",
      "=", "<", ">", "!", "&", "|", "^", "~"
    )),

    l_paren: () => "(",
    r_paren: () => ")",
    l_brace: () => "{",
    r_brace: () => "}",
    l_bracket: () => "[",
    r_bracket: () => "]",
    dot: () => ".",
    comma: () => ",",
    colon: () => ":",
    semicolon: () => ";",

    unknown: () => token(prec(-1, /./)),
  },
});
