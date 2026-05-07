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

// --- generic syntax tests ---

function findGenericTypeParams() {
    return grammar.repository && grammar.repository.genericTypeParams;
}

function findExplicitGenericCall() {
    return grammar.repository && grammar.repository.explicitGenericCall;
}

function findFunctionDefinitionPatterns() {
    const fd = grammar.repository && grammar.repository.functionDefinitions;
    return (fd && Array.isArray(fd.patterns)) ? fd.patterns : [];
}

function findTypeDefinitionPatterns() {
    const td = grammar.repository && grammar.repository.typeDefinitions;
    return (td && Array.isArray(td.patterns)) ? td.patterns : [];
}

function findSpecDefinitionPatterns() {
    const sd = grammar.repository && grammar.repository.specDefinitions;
    return (sd && Array.isArray(sd.patterns)) ? sd.patterns : [];
}

// 1. genericTypeParams rule exists and has type parameter name pattern
const genericTypeParams = findGenericTypeParams();
assert(genericTypeParams, 'expected genericTypeParams rule in grammar repository');
const genericPatterns = Array.isArray(genericTypeParams.patterns) ? genericTypeParams.patterns : [];
const typeParamNamePattern = genericPatterns.find(
    p => p.name === 'entity.name.type.parameter.feng'
);
assert(typeParamNamePattern, 'expected entity.name.type.parameter.feng pattern in genericTypeParams');

// 2. explicitGenericCall rule exists for :< syntax
const explicitGenericCall = findExplicitGenericCall();
assert(explicitGenericCall, 'expected explicitGenericCall rule in grammar repository');
const explicitPatterns = Array.isArray(explicitGenericCall.patterns) ? explicitGenericCall.patterns : [];
const colonAnglePattern = explicitPatterns.find(
    p => p.name === 'punctuation.definition.generic.begin.feng'
);
assert(colonAnglePattern, 'expected punctuation.definition.generic.begin.feng for :< syntax');
const colonAngleRegex = new RegExp(colonAnglePattern.match);
assert(colonAngleRegex.test(':<'), 'explicit generic call pattern should match :<');

// 3. functionDefinitions has a begin/end pattern with type-param support
const fnPatterns = findFunctionDefinitionPatterns();
const fnGenericPattern = fnPatterns.find(p => p.begin && p.begin.includes('<'));
assert(fnGenericPattern, 'expected a begin/end pattern in functionDefinitions for generic functions');
assert(fnGenericPattern.beginCaptures && fnGenericPattern.beginCaptures['1'], 'fn generic pattern should capture group 1 (fn keyword)');
assert.strictEqual(
    fnGenericPattern.beginCaptures['1'].name,
    'keyword.declaration.function.feng',
    'fn keyword should have scope keyword.declaration.function.feng'
);
assert.strictEqual(
    fnGenericPattern.beginCaptures['3'].name,
    'entity.name.function.feng',
    'fn name should have scope entity.name.function.feng'
);

// 4. typeDefinitions has a begin/end pattern with type-param support
const typePatterns = findTypeDefinitionPatterns();
const typeGenericPattern = typePatterns.find(p => p.begin && p.begin.includes('<'));
assert(typeGenericPattern, 'expected a begin/end pattern in typeDefinitions for generic types');
assert.strictEqual(
    typeGenericPattern.beginCaptures['1'].name,
    'keyword.declaration.type.feng',
    'type keyword should have scope keyword.declaration.type.feng'
);

// 5. specDefinitions has a begin/end pattern with type-param support
const specPatterns = findSpecDefinitionPatterns();
const specGenericPattern = specPatterns.find(p => p.begin && p.begin.includes('<'));
assert(specGenericPattern, 'expected a begin/end pattern in specDefinitions for generic specs');
assert.strictEqual(
    specGenericPattern.beginCaptures['1'].name,
    'keyword.declaration.spec.feng',
    'spec keyword should have scope keyword.declaration.spec.feng'
);

// 6. Top-level patterns include explicitGenericCall
const topPatterns = grammar.patterns;
assert(Array.isArray(topPatterns), 'grammar should have top-level patterns array');
const hasExplicitGenericCallInclude = topPatterns.some(
    p => p.include === '#explicitGenericCall'
);
assert(hasExplicitGenericCallInclude, 'top-level patterns should include #explicitGenericCall');

console.log('syntax tests passed');