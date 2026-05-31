[
  "module"
  "import"
  "as"
  "func"
  "type"
  "spec"
  "fit"
  "let"
  "var"
  "return"
  "if"
  "else"
  "for"
  "while"
  "break"
  "continue"
  "match"
  "case"
  "defer"
  "throw"
  "try"
  "catch"
  "finally"
  "extern"
  "pub"
  "use"
] @keyword

(self_keyword) @variable.special

[
  (boolean)
] @boolean

[
  (number)
] @number

[
  (string)
] @string

(escape_sequence) @string.escape

[
  (line_comment)
  (block_comment)
] @comment

(doc_comment) @comment.doc

[
  (builtin_type)
] @type.builtin

(identifier) @variable

((identifier) @constant
 (#match? @constant "^[A-Z][A-Z0-9_]*$"))

(call_expression
  function: (identifier) @function)

(member_expression
  member: (identifier) @property)

[
  (dot)
  (comma)
  (colon)
  (semicolon)
] @punctuation.delimiter

[
  (l_paren)
  (r_paren)
  (l_brace)
  (r_brace)
  (l_bracket)
  (r_bracket)
] @punctuation.bracket

(operator) @operator
