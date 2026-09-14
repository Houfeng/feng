const assert = require('assert');
const fs = require('fs');
const textmate = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

const rawGrammar = require('../syntaxes/feng.tmLanguage.json');
const GENERIC_OPEN = 'punctuation.definition.generic.begin.feng';
const GENERIC_CLOSE = 'punctuation.definition.generic.end.feng';
const PARAM_OPEN = 'punctuation.definition.typeparams.begin.feng';
const PARAM_CLOSE = 'punctuation.definition.typeparams.end.feng';
const OPERATOR = 'keyword.operator.feng';

/* Check each angle bracket's actual TextMate scope and that the region closes. */
function runCase(grammar, name, source, expectedScopes) {
    let ruleStack = textmate.INITIAL;
    const scopes = [];

    for (const line of source.split('\n')) {
        const result = grammar.tokenizeLine(line, ruleStack);
        for (const token of result.tokens) {
            for (let index = token.startIndex; index < Math.min(token.endIndex, line.length); index += 1) {
                if (line[index] === '<' || line[index] === '>') {
                    scopes.push(token.scopes[token.scopes.length - 1]);
                }
            }
        }
        ruleStack = result.ruleStack;
    }

    assert.deepStrictEqual(scopes, expectedScopes, name);
    const followingLine = grammar.tokenizeLine('let following = 0;', ruleStack);
    assert(followingLine.tokens.every(token => !token.scopes.includes('meta.type.arguments.feng')),
        `${name}: generic highlighting must end before the following statement`);
}

/* Load the same TextMate and Oniguruma engines used by VS Code for scope tests. */
async function main() {
    const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm'));
    await oniguruma.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
    const registry = new textmate.Registry({
        onigLib: Promise.resolve({
            /* Construct the Oniguruma scanner requested by the grammar engine. */
            createOnigScanner(patterns) {
                return new oniguruma.OnigScanner(patterns);
            },
            /* Construct an Oniguruma string for each tokenized source line. */
            createOnigString(source) {
                return new oniguruma.OnigString(source);
            }
        }),
        /* Resolve Feng's grammar without loading unrelated language grammars. */
        async loadGrammar(scopeName) {
            return scopeName === rawGrammar.scopeName ? rawGrammar : null;
        }
    });

    try {
        const grammar = await registry.loadGrammar(rawGrammar.scopeName);
        assert(grammar, 'Feng grammar must load');

        runCase(grammar, 'generic fields before initializers',
            'type Future<T, E> {\n  seal var _value: Option<T> = none;\n  seal var _error: Option<E> = none;\n}',
            [PARAM_OPEN, PARAM_CLOSE, GENERIC_OPEN, GENERIC_CLOSE, GENERIC_OPEN, GENERIC_CLOSE]);
        runCase(grammar, 'pointer arguments in annotations and constructors',
            'seal let waiters: List<AsyncTaskContext*> = List<AsyncTaskContext*>();',
            [GENERIC_OPEN, GENERIC_CLOSE, GENERIC_OPEN, GENERIC_CLOSE]);
        runCase(grammar, 'nested generic arguments with qualified pointer arrays',
            'let items: Map<string, List<ns.Node*[]>> = make();',
            [GENERIC_OPEN, GENERIC_OPEN, GENERIC_CLOSE, GENERIC_CLOSE]);
        runCase(grammar, 'nested generic pointer suffix',
            'let item: Box<Inner<int>*> = make();',
            [GENERIC_OPEN, GENERIC_OPEN, GENERIC_CLOSE, GENERIC_CLOSE]);
        runCase(grammar, 'whitespace around generic brackets',
            'let item: option < int > = none;',
            [GENERIC_OPEN, GENERIC_CLOSE]);
        runCase(grammar, 'explicit generic call with nested arguments',
            'let value = identity<List<int>>(items);',
            [GENERIC_OPEN, GENERIC_OPEN, GENERIC_CLOSE, GENERIC_CLOSE]);
        runCase(grammar, 'generic constraints with nested arguments',
            'type Box<T: Comparable<List<T>>> {}',
            [PARAM_OPEN, GENERIC_OPEN, GENERIC_OPEN, GENERIC_CLOSE, GENERIC_CLOSE, PARAM_CLOSE]);
        runCase(grammar, 'comparisons in generic binding initializers',
            'let ok: Option<bool> = a<b && b>c;',
            [GENERIC_OPEN, GENERIC_CLOSE, OPERATOR, OPERATOR]);
        runCase(grammar, 'comparison sequence followed by an identifier',
            'let ok = a < b > c;',
            [OPERATOR, OPERATOR]);
        runCase(grammar, 'less-than and greater-or-equal comparisons',
            'let ok = a<b>=c;',
            [OPERATOR, OPERATOR]);
        runCase(grammar, 'shift expressions and assignments',
            'let bits = value<<1; bits>>=1;',
            [OPERATOR, OPERATOR, OPERATOR, OPERATOR]);
        runCase(grammar, 'generic-looking strings and comments',
            'let text = "List<Node*>"; // Option<T>',
            ['string.quoted.double.feng', 'string.quoted.double.feng',
                'comment.line.double-slash.feng', 'comment.line.double-slash.feng']);
    } finally {
        registry.dispose();
    }

    console.log('generic syntax scope tests passed');
}

/* Propagate asynchronous engine or assertion failures to the test command. */
main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
