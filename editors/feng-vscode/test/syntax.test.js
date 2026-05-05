const assert = require('assert');

const grammar = require('../syntaxes/feng.tmLanguage.json');

function findOperatorPattern() {
    const operators = grammar.repository && grammar.repository.operators;
    const patterns = operators && Array.isArray(operators.patterns) ? operators.patterns : [];

    return patterns.find(pattern => pattern.name === 'keyword.operator.feng');
}

const operatorPattern = findOperatorPattern();

assert(operatorPattern, 'expected keyword.operator.feng pattern in TextMate grammar');

const operatorRegex = new RegExp(`^(?:${operatorPattern.match})$`);
const compoundOperators = ['+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='];

for (const operator of compoundOperators) {
    assert(operatorRegex.test(operator), `expected grammar to highlight ${operator}`);
}

// --- doc comment grammar tests ---

function findCommentPatterns() {
    const comments = grammar.repository && grammar.repository.comments;
    return (comments && Array.isArray(comments.patterns)) ? comments.patterns : [];
}

const commentPatterns = findCommentPatterns();

// 1. doc comment scope should exist and come before regular block comment
const docCommentPattern = commentPatterns.find(
    p => p.name === 'comment.block.documentation.feng'
);
assert(docCommentPattern, 'expected comment.block.documentation.feng pattern in comments');
assert.strictEqual(docCommentPattern.begin, '/\\*\\*', 'doc comment begin should be /\\*\\*');
assert.strictEqual(docCommentPattern.end, '\\*/', 'doc comment end should be \\*/');

const docIdx = commentPatterns.findIndex(p => p.name === 'comment.block.documentation.feng');
const blockIdx = commentPatterns.findIndex(p => p.name === 'comment.block.feng');
assert(docIdx < blockIdx, 'doc comment pattern must appear before regular block comment pattern');

// 2. doc comment should contain inner patterns for tags
const innerPatterns = Array.isArray(docCommentPattern.patterns) ? docCommentPattern.patterns : [];
assert(innerPatterns.length >= 2, 'doc comment should have at least 2 inner patterns');

// 3. @param + param-name capture pattern
const paramPattern = innerPatterns.find(p => p.captures && p.captures['2']);
assert(paramPattern, 'expected a pattern with capture group 2 for parameter name');
assert.strictEqual(
    paramPattern.captures['1'].name,
    'keyword.other.documentation.tag.feng',
    '@param tag should have scope keyword.other.documentation.tag.feng'
);
assert.strictEqual(
    paramPattern.captures['2'].name,
    'variable.other.documentation.parameter.feng',
    'param name should have scope variable.other.documentation.parameter.feng'
);

// verify @param regex actually matches @param followed by an identifier
const paramRegex = new RegExp(paramPattern.match);
assert(paramRegex.test('@param foo'), 'param pattern should match "@param foo"');
assert(paramRegex.test('@param myParam'), 'param pattern should match "@param myParam"');

// 4. generic tag pattern
const tagPattern = innerPatterns.find(
    p => p.name === 'keyword.other.documentation.tag.feng'
);
assert(tagPattern, 'expected a generic @tag pattern with scope keyword.other.documentation.tag.feng');
const tagRegex = new RegExp(tagPattern.match);
assert(tagRegex.test('@return'), 'tag pattern should match @return');
assert(tagRegex.test('@throws'), 'tag pattern should match @throws');
assert(tagRegex.test('@deprecated'), 'tag pattern should match @deprecated');

console.log('syntax tests passed');