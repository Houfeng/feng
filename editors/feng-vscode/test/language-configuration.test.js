const assert = require('assert');

const configuration = require('../language-configuration.json');

const autoClosingPairs = Array.isArray(configuration.autoClosingPairs)
    ? configuration.autoClosingPairs
    : [];
let documentationCommentPair = null;
let hasRegularBlockCommentPair = false;

for (const pair of autoClosingPairs) {
    if (pair.open === '/**') {
        documentationCommentPair = pair;
    } else if (pair.open === '/*') {
        hasRegularBlockCommentPair = true;
    }
}

assert(documentationCommentPair, 'expected an auto-closing pair for documentation comments');
assert.strictEqual(
    documentationCommentPair.close,
    ' */',
    'documentation comments should insert a closing delimiter with a leading space'
);
assert(
    !hasRegularBlockCommentPair,
    'regular block-comment auto-closing must not intercept the documentation-comment pair'
);

const onEnterRules = Array.isArray(configuration.onEnterRules)
    ? configuration.onEnterRules
    : [];
let documentationCommentEnterRule = null;

for (const rule of onEnterRules) {
    if (
        rule.action
        && rule.action.indent === 'indentOutdent'
        && rule.action.appendText === ' * '
    ) {
        documentationCommentEnterRule = rule;
        break;
    }
}

assert(documentationCommentEnterRule, 'expected an indent-outdent rule for documentation comments');
assert(
    new RegExp(documentationCommentEnterRule.beforeText).test('/**'),
    'documentation-comment Enter rule should match the opening delimiter'
);
assert(
    new RegExp(documentationCommentEnterRule.afterText).test(documentationCommentPair.close),
    'documentation-comment Enter rule should match the auto-inserted closing delimiter'
);

console.log('language configuration tests passed');
