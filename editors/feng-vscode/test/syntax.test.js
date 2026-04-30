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

console.log('syntax tests passed');