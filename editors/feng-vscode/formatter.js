const KEYWORDS = new Set([
    'type',
    'enum',
    'spec',
    'fit',
    'extern',
    'func',
    'let',
    'var',
    'static',
    'open',
    'seal',
    'self',
    'module',
    'import',
    'as',
    'if',
    'else',
    'match',
    'while',
    'for',
    'in',
    'break',
    'continue',
    'defer',
    'try',
    'catch',
    'throw',
    'return',
    'void',
    'unknown'
]);

const CONTROL_PAREN_KEYWORDS = new Set(['if', 'while', 'for', 'match', 'catch', 'return', 'throw']);
const PREFIX_CONTEXT_KEYWORDS = new Set(['if', 'while', 'return', 'throw']);
const PREFIX_OPERATORS = new Set(['!', '-', '*', '+', '&']);
const THREE_CHAR_OPERATORS = new Set(['<<=', '>>=']);
const MULTI_CHAR_OPERATORS = new Set([
    '->', '&&', '||', '==', '!=', '<=', '>=',
    '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<', '>>'
]);
const SINGLE_CHAR_OPERATORS = new Set(['+', '-', '*', '/', '%', '=', '!', '<', '>', '&', '|', '^', '~']);
const DELIMITERS = new Set(['(', ')', '[', ']', '{', '}']);
const PUNCTUATION = new Set([',', ':', ';', '.', '...']);
const BUILTIN_TYPE_IDENTIFIERS = new Set([
    'byte', 'short', 'int', 'long',
    'i8', 'i16', 'i32', 'i64',
    'u8', 'u16', 'u32', 'u64',
    'float', 'double', 'bool', 'char', 'string',
    'usize', 'isize'
]);
const OPEN_TO_CLOSE = {
    '(': ')',
    '[': ']',
    '{': '}'
};
const CLOSE_TO_OPEN = {
    ')': '(',
    ']': '[',
    '}': '{'
};

function createIndentUnit(options) {
    const tabSize = Number.isInteger(options.tabSize) && options.tabSize > 0 ? options.tabSize : 4;

    if (options.insertSpaces === false) {
        return '\t';
    }

    return ' '.repeat(tabSize);
}

function isIdentifierStart(char) {
    return /[A-Za-z_]/.test(char);
}

function isIdentifierPart(char) {
    return /[A-Za-z0-9_]/.test(char);
}

function isDigit(char) {
    return /[0-9]/.test(char);
}

function classifyWord(value) {
    if (KEYWORDS.has(value)) {
        return { type: 'keyword', value };
    }
    if (value === 'true' || value === 'false') {
        return { type: 'boolean', value };
    }
    return { type: 'identifier', value };
}

function classifySymbol(value) {
    if (DELIMITERS.has(value)) {
        return { type: 'delimiter', value };
    }
    if (PUNCTUATION.has(value)) {
        return { type: 'punctuation', value };
    }
    return { type: 'operator', value };
}

function pushBlockComment(source, start, tokens) {
    let index = start + 2;
    let fragment = '/*';
    let isFirstLine = true;

    while (index < source.length) {
        const current = source[index];
        const next = index + 1 < source.length ? source[index + 1] : '';

        if (current === '\n') {
            if (isFirstLine) {
                tokens.push({ type: 'comment', value: fragment.replace(/[\t ]+$/g, '') });
            } else {
                const stripped = fragment.replace(/^[\t ]+/, '').replace(/[\t ]+$/g, '');
                tokens.push({ type: 'comment', value: stripped, commentRole: 'body' });
            }
            tokens.push({ type: 'newline' });
            fragment = '';
            isFirstLine = false;
            index += 1;
            continue;
        }

        if (current === '*' && next === '/') {
            fragment += '*/';
            if (isFirstLine) {
                tokens.push({ type: 'comment', value: fragment.replace(/[\t ]+$/g, '') });
            } else {
                const stripped = fragment.replace(/^[\t ]+/, '').replace(/[\t ]+$/g, '');
                tokens.push({ type: 'comment', value: stripped, commentRole: 'body' });
            }
            return index + 2;
        }

        fragment += current;
        index += 1;
    }

    if (fragment.length > 0) {
        if (isFirstLine) {
            tokens.push({ type: 'comment', value: fragment.replace(/[\t ]+$/g, '') });
        } else {
            const stripped = fragment.replace(/^[\t ]+/, '').replace(/[\t ]+$/g, '');
            tokens.push({ type: 'comment', value: stripped, commentRole: 'body' });
        }
    }

    return index;
}

function tokenize(source) {
    const tokens = [];
    let index = 0;

    while (index < source.length) {
        const current = source[index];
        const next = index + 1 < source.length ? source[index + 1] : '';
        const threeChars = source.slice(index, index + 3);
        const twoChars = source.slice(index, index + 2);

        if (current === ' ' || current === '\t' || current === '\r') {
            index += 1;
            continue;
        }

        if (current === '\n') {
            tokens.push({ type: 'newline' });
            index += 1;
            continue;
        }

        if (current === '/' && next === '/') {
            let end = index + 2;

            while (end < source.length && source[end] !== '\n') {
                end += 1;
            }

            tokens.push({ type: 'comment', value: source.slice(index, end).replace(/[\t ]+$/g, '') });
            index = end;
            continue;
        }

        if (current === '/' && next === '*') {
            index = pushBlockComment(source, index, tokens);
            continue;
        }

        if (current === '"') {
            let end = index + 1;
            let escaped = false;

            while (end < source.length) {
                const value = source[end];

                if (escaped) {
                    escaped = false;
                } else if (value === '\\') {
                    escaped = true;
                } else if (value === '"') {
                    end += 1;
                    break;
                }

                end += 1;
            }

            tokens.push({ type: 'string', value: source.slice(index, end) });
            index = end;
            continue;
        }

        if (current === '`') {
            let end = index + 1;

            while (end < source.length) {
                const value = source[end];

                if (value === '`') {
                    /* Check for escaped backtick `` */
                    if (end + 1 < source.length && source[end + 1] === '`') {
                        end += 2;
                        continue;
                    }
                    end += 1;
                    break;
                }

                end += 1;
            }

            tokens.push({ type: 'string', value: source.slice(index, end) });
            index = end;
            continue;
        }

        if (current === '@' && isIdentifierStart(next)) {
            let end = index + 2;

            while (end < source.length && isIdentifierPart(source[end])) {
                end += 1;
            }

            tokens.push({ type: 'annotation', value: source.slice(index, end) });
            index = end;
            continue;
        }

        if (current === '~' && isIdentifierStart(next)) {
            let end = index + 2;

            while (end < source.length && isIdentifierPart(source[end])) {
                end += 1;
            }

            tokens.push({ type: 'identifier', value: source.slice(index, end) });
            index = end;
            continue;
        }

        if (isIdentifierStart(current)) {
            let end = index + 1;

            while (end < source.length && isIdentifierPart(source[end])) {
                end += 1;
            }

            tokens.push(classifyWord(source.slice(index, end)));
            index = end;
            continue;
        }

        if (isDigit(current)) {
            let end = index + 1;

            if (current === '0' && end < source.length) {
                const prefix = source[end];
                if (prefix === 'x' || prefix === 'X') {
                    end += 1;
                    while (end < source.length && /[0-9A-Fa-f]/.test(source[end])) {
                        end += 1;
                    }
                    tokens.push({ type: 'number', value: source.slice(index, end) });
                    index = end;
                    continue;
                }
                if (prefix === 'b' || prefix === 'B') {
                    end += 1;
                    while (end < source.length && /[01]/.test(source[end])) {
                        end += 1;
                    }
                    tokens.push({ type: 'number', value: source.slice(index, end) });
                    index = end;
                    continue;
                }
                if (prefix === 'o' || prefix === 'O') {
                    end += 1;
                    while (end < source.length && /[0-7]/.test(source[end])) {
                        end += 1;
                    }
                    tokens.push({ type: 'number', value: source.slice(index, end) });
                    index = end;
                    continue;
                }
            }

            while (end < source.length && isDigit(source[end])) {
                end += 1;
            }
            if (source[end] === '.' && isDigit(source[end + 1])) {
                end += 1;
                while (end < source.length && isDigit(source[end])) {
                    end += 1;
                }
            }

            tokens.push({ type: 'number', value: source.slice(index, end) });
            index = end;
            continue;
        }

        /* Recognize multi-character punctuation before its single-character prefix. */
        if (PUNCTUATION.has(threeChars)) {
            tokens.push(classifySymbol(threeChars));
            index += 3;
            continue;
        }

        if (THREE_CHAR_OPERATORS.has(threeChars)) {
            tokens.push(classifySymbol(threeChars));
            index += 3;
            continue;
        }

        if (MULTI_CHAR_OPERATORS.has(twoChars)) {
            tokens.push(classifySymbol(twoChars));
            index += 2;
            continue;
        }

        if (DELIMITERS.has(current) || PUNCTUATION.has(current) || SINGLE_CHAR_OPERATORS.has(current)) {
            tokens.push(classifySymbol(current));
            index += 1;
            continue;
        }

        tokens.push({ type: 'text', value: current });
        index += 1;
    }

    return tokens;
}

function splitIntoLines(tokens) {
    const lines = [[]];

    for (const token of tokens) {
        if (token.type === 'newline') {
            lines.push([]);
            continue;
        }
        lines[lines.length - 1].push(token);
    }

    return lines;
}

function isAtomToken(token) {
    return token != null &&
        (token.type === 'identifier' ||
            token.type === 'keyword' ||
            token.type === 'annotation' ||
            token.type === 'number' ||
            token.type === 'string' ||
            token.type === 'boolean' ||
            token.type === 'text');
}

function isOpeningDelimiter(token, expected) {
    return token != null && token.type === 'delimiter' && expected.includes(token.value);
}

function isClosingDelimiter(token) {
    return token != null && token.type === 'delimiter' && (token.value === ')' || token.value === ']' || token.value === '}');
}

function isOperator(token) {
    return token != null && token.type === 'operator';
}

function isPrefixOperator(token, previousSignificantToken) {
    if (!isOperator(token) || !PREFIX_OPERATORS.has(token.value)) {
        return false;
    }

    if (previousSignificantToken == null) {
        return true;
    }

    if (isOperator(previousSignificantToken)) {
        return true;
    }

    if (previousSignificantToken.type === 'keyword' && PREFIX_CONTEXT_KEYWORDS.has(previousSignificantToken.value)) {
        return true;
    }

    return previousSignificantToken.type === 'punctuation' ||
        isOpeningDelimiter(previousSignificantToken, ['(', '[', '{']);
}

function shouldSpaceBeforeOpenParen(previousSignificantToken) {
    return previousSignificantToken != null &&
        previousSignificantToken.type === 'keyword' &&
        CONTROL_PAREN_KEYWORDS.has(previousSignificantToken.value);
}

function shouldSpaceBeforeOpenBrace(previousEmittedToken) {
    return previousEmittedToken != null &&
        previousEmittedToken.value !== '{' &&
        previousEmittedToken.value !== '(' &&
        previousEmittedToken.value !== '[' &&
        previousEmittedToken.value !== '.';
}

function isCastTypeIdentifier(token) {
    if (token == null) {
        return false;
    }

    if (token.type === 'keyword') {
        return true;
    }

    if (token.type !== 'identifier') {
        return false;
    }

    if (BUILTIN_TYPE_IDENTIFIERS.has(token.value)) {
        return true;
    }

    return /^[A-Z]/.test(token.value);
}

function isCastTypeToken(token) {
    if (isCastTypeIdentifier(token)) {
        return true;
    }

    if (token == null) {
        return false;
    }

    if (token.type === 'operator') {
        return token.value === '*' || token.value === '&' || token.value === '<' || token.value === '>';
    }

    if (token.type === 'punctuation') {
        return token.value === ',' || token.value === '.';
    }

    if (token.type === 'delimiter') {
        return token.value === '[' || token.value === ']';
    }

    return false;
}

function findMatchingOpenParen(tokens, closeIndex) {
    let depth = 0;

    for (let index = closeIndex - 1; index >= 0; index -= 1) {
        const token = tokens[index];

        if (token.type !== 'delimiter') {
            continue;
        }

        if (token.value === ')') {
            depth += 1;
            continue;
        }

        if (token.value !== '(') {
            continue;
        }

        if (depth === 0) {
            return index;
        }

        depth -= 1;
    }

    return -1;
}

function isLikelyCastClose(tokens, closeIndex) {
    if (closeIndex < 0 || closeIndex >= tokens.length) {
        return false;
    }

    const closeToken = tokens[closeIndex];

    if (closeToken.type !== 'delimiter' || closeToken.value !== ')') {
        return false;
    }

    const openIndex = findMatchingOpenParen(tokens, closeIndex);

    if (openIndex < 0 || openIndex + 1 >= closeIndex) {
        return false;
    }

    for (let index = openIndex + 1; index < closeIndex; index += 1) {
        if (!isCastTypeToken(tokens[index])) {
            return false;
        }
    }

    return true;
}

function needsSpaceBetween(previousEmittedToken,
                           previousSignificantToken,
                           tokenBeforePreviousSignificant,
                           currentToken) {
    if (previousEmittedToken == null) {
        return false;
    }

    if (previousEmittedToken.type === 'comment') {
        return true;
    }

    if (currentToken.type === 'comment') {
        return true;
    }

    if (currentToken.value === '.' || previousEmittedToken.value === '.') {
        return false;
    }

    if (currentToken.value === ',' || currentToken.value === ';' || currentToken.value === ':') {
        return false;
    }

    if (isOperator(previousSignificantToken) && previousEmittedToken === previousSignificantToken) {
        return !isPrefixOperator(previousSignificantToken, tokenBeforePreviousSignificant);
    }

    if (previousEmittedToken.value === ',') {
        return true;
    }

    if (previousEmittedToken.value === ':') {
        return tokenBeforePreviousSignificant == null || tokenBeforePreviousSignificant.value !== '[';
    }

    if (previousEmittedToken.value === ';') {
        return currentToken.value !== ')';
    }

    if (currentToken.type === 'operator') {
        if (!isPrefixOperator(currentToken, previousSignificantToken)) {
            return true;
        }

        return previousSignificantToken != null &&
            previousSignificantToken.type === 'keyword' &&
            PREFIX_CONTEXT_KEYWORDS.has(previousSignificantToken.value);
    }

    if (currentToken.value === '(') {
        return shouldSpaceBeforeOpenParen(previousSignificantToken);
    }

    if (currentToken.value === '[') {
        return false;
    }

    if (currentToken.value === '{') {
        return shouldSpaceBeforeOpenBrace(previousEmittedToken);
    }

    if (currentToken.value === ')' || currentToken.value === ']') {
        return false;
    }

    if (currentToken.value === '}') {
        return previousEmittedToken.value !== '{';
    }

    if (previousEmittedToken.value === '{') {
        return currentToken.value !== '}';
    }

    if (previousEmittedToken.value === '}' && isAtomToken(currentToken)) {
        return true;
    }

    if (previousEmittedToken.value === '(' || previousEmittedToken.value === '[') {
        return false;
    }

    return isAtomToken(previousEmittedToken) && isAtomToken(currentToken);
}

function countLeadingClosers(tokens) {
    let count = 0;

    for (const token of tokens) {
        if (token.type === 'comment') {
            break;
        }
        if (isClosingDelimiter(token)) {
            count += 1;
            continue;
        }
        break;
    }

    return count;
}

function updateDelimiterStack(delimiterStack, tokens) {
    /* Pre-scan: identify () pairs on this line where ) is followed by { or ->.
     * These are lambda parameter lists or func-call-before-block patterns.
     * Neither the ( nor the ) should affect the delimiter stack, because only
     * the { should contribute to indentation of subsequent lines. */
    const skipIndices = new Set();

    for (let i = 0; i < tokens.length; i += 1) {
        const token = tokens[i];
        if (token.type !== 'delimiter' || token.value !== '(') {
            continue;
        }

        /* Skip control-flow parens: if(...) for(...) while(...) catch(...) etc. */
        let prevSig = null;
        for (let p = i - 1; p >= 0; p -= 1) {
            if (tokens[p].type !== 'comment') {
                prevSig = tokens[p];
                break;
            }
        }
        if (prevSig != null && prevSig.type === 'keyword' && CONTROL_PAREN_KEYWORDS.has(prevSig.value)) {
            continue;
        }

        let depth = 1;
        let closeIdx = -1;
        let afterClose = null;

        for (let j = i + 1; j < tokens.length; j += 1) {
            const t = tokens[j];
            if (t.type === 'comment') {
                continue;
            }
            if (t.type === 'delimiter' && t.value === '(') {
                depth += 1;
            } else if (t.type === 'delimiter' && t.value === ')') {
                depth -= 1;
                if (depth === 0) {
                    closeIdx = j;
                    for (let k = j + 1; k < tokens.length; k += 1) {
                        const next = tokens[k];
                        if (next.type === 'comment') {
                            continue;
                        }
                        afterClose = next;
                        break;
                    }
                    break;
                }
            }
        }

        if (closeIdx >= 0 && afterClose != null &&
            ((afterClose.type === 'delimiter' && afterClose.value === '{') ||
             (afterClose.type === 'operator' && afterClose.value === '->'))) {
            skipIndices.add(i);
            skipIndices.add(closeIdx);
        }
    }

    for (let i = 0; i < tokens.length; i += 1) {
        const token = tokens[i];

        if (token.type === 'comment') {
            continue;
        }

        if (skipIndices.has(i)) {
            continue;
        }

        if (isOpeningDelimiter(token, ['(', '[', '{'])) {
            /* Mark function-call parens so they don't contribute to indentation.
             * A function-call ( is preceded by an identifier that is NOT a
             * declaration name (not preceded by func/type/spec/fit/extern). */
            let isFuncCall = false;
            if (token.value === '(') {
                let prevSig = null;
                let prevPrevSig = null;
                for (let p = i - 1; p >= 0; p -= 1) {
                    if (tokens[p].type === 'comment') { continue; }
                    if (prevSig == null) { prevSig = tokens[p]; }
                    else { prevPrevSig = tokens[p]; break; }
                }
                if (prevSig != null && prevSig.type === 'identifier') {
                    const isDecl = prevPrevSig != null && prevPrevSig.type === 'keyword' &&
                        (prevPrevSig.value === 'func' || prevPrevSig.value === 'type' ||
                         prevPrevSig.value === 'spec' || prevPrevSig.value === 'fit' ||
                         prevPrevSig.value === 'extern');
                    isFuncCall = !isDecl;
                }
            }
            delimiterStack.push({ value: token.value, isFuncCall: isFuncCall });
            continue;
        }

        if (isClosingDelimiter(token)) {
            const expected = CLOSE_TO_OPEN[token.value];

            if (delimiterStack.length > 0 && delimiterStack[delimiterStack.length - 1].value === expected) {
                delimiterStack.pop();
            } else if (delimiterStack.length > 0 && token.value === ')') {
                /* Fallback pop for ): only remove ( entries, never { or [.
                 * This handles unmatched func-call ) without corrupting
                 * block indentation. */
                for (let s = delimiterStack.length - 1; s >= 0; s -= 1) {
                    if (delimiterStack[s].value === '(') {
                        delimiterStack.splice(s, 1);
                        break;
                    }
                }
            }
        }
    }
}

/*
 * Returns true when tokens[index] is a postfix pointer star (T*).
 *
 * Rules (all must hold):
 *   1. tokens[index] is `*`.
 *   2. The previous significant token is:
 *      a. An identifier preceded by `:` or `(` — i.e. a type-annotation position, OR
 *      b. Another `*` that was itself a postfix pointer star (chained T**).
 *   3. The next token is NOT an identifier and NOT `(`.  When * is followed by a
 *      letter or `(` it is a dereference / multiplication operator, not a pointer
 *      type suffix.
 */
function isPostfixPointerStar(tokens, index, previousSignificantToken,
                              tokenBeforePreviousSignificant, lastEmittedWasPostfixPointer) {
    const token = tokens[index];
    if (token == null || token.type !== 'operator' || token.value !== '*') {
        return false;
    }

    /* Condition 2 */
    if (previousSignificantToken == null) {
        return false;
    }
    if (previousSignificantToken.type === 'identifier') {
        /* Must be in a type-annotation context: `:` or `(` before the identifier */
        if (tokenBeforePreviousSignificant == null) {
            return false;
        }
        const ctx = tokenBeforePreviousSignificant.value;
        if (ctx !== ':' && ctx !== '(') {
            return false;
        }
    } else if (previousSignificantToken.type === 'operator' &&
               previousSignificantToken.value === '*' &&
               lastEmittedWasPostfixPointer) {
        /* Chained pointer: previous * was already a postfix pointer star */
    } else {
        return false;
    }

    /* Condition 3: next token must not be an identifier or `(` */
    const next = tokens[index + 1];
    if (next != null && (next.type === 'identifier' || next.value === '(')) {
        return false;
    }

    return true;
}

function tokenCanFollowExplicitGenericTarget(token) {
    if (token == null) {
        return true;
    }

    return (token.type === 'delimiter' &&
          (token.value === '(' || token.value === '[' || token.value === '{' ||
           token.value === ')' || token.value === ']' || token.value === '}')) ||
         (token.type === 'punctuation' &&
          (token.value === ',' || token.value === ';' || token.value === ':')) ||
         (token.type === 'operator' && token.value === '>');
}

function countGenericCloseOperators(token) {
    if (token == null || token.type !== 'operator') {
        return 0;
    }

    if (token.value === '>') {
        return 1;
    }

    if (token.value === '>>') {
        return 2;
    }

    return 0;
}

function tokenAfterConsumingGenericClosers(tokens, index, consumedClosers) {
    const token = tokens[index];

    if (token != null && token.type === 'operator' && token.value === '>>' && consumedClosers === 1) {
        return { type: 'operator', value: '>' };
    }

    return tokens[index + 1];
}

function tokenCanAppearInGenericTypeArgs(token) {
    if (token == null || token.type === 'newline' || token.type === 'comment') {
        return false;
    }

    if (token.type === 'identifier') {
        return true;
    }

    if (token.type === 'punctuation') {
        return token.value === ',' || token.value === '.' || token.value === ':';
    }

    if (token.type === 'delimiter') {
        return token.value === '[' || token.value === ']';
    }

    return token.type === 'operator' &&
        (token.value === '<' || token.value === '>' || token.value === '!' || token.value === '*');
}

function looksLikeExplicitGenericOpen(tokens, index, previousSignificantToken) {
    let depth = 1;
    let sawTypeToken = false;

    if (tokens[index] == null || tokens[index].type !== 'operator' || tokens[index].value !== '<') {
        return false;
    }

    if (previousSignificantToken == null || previousSignificantToken.type !== 'identifier') {
        return false;
    }

    for (let lookahead = index + 1; lookahead < tokens.length; lookahead += 1) {
        const token = tokens[lookahead];
        const genericCloseCount = countGenericCloseOperators(token);

        if (token.type === 'operator' && token.value === '<') {
            depth += 1;
            continue;
        }

        if (genericCloseCount > 0) {
            for (let consumedClosers = 1; consumedClosers <= genericCloseCount; consumedClosers += 1) {
                depth -= 1;
                if (depth === 0) {
                    return sawTypeToken && tokenCanFollowExplicitGenericTarget(
                        tokenAfterConsumingGenericClosers(tokens, lookahead, consumedClosers)
                    );
                }
            }
            continue;
        }

        if (!tokenCanAppearInGenericTypeArgs(token)) {
            return false;
        }

        if (token.type === 'identifier') {
            sawTypeToken = true;
        }
    }

    return false;
}

function formatLineTokens(tokens, previousSignificantTokenBeforeLine) {
    let result = '';
    let previousEmittedToken = null;
    let previousSignificantToken = previousSignificantTokenBeforeLine;
    let tokenBeforePreviousSignificant = null;
    let lastLineSignificantToken = null;
    let genericDepth = 0;
    let lastEmittedWasGenericOpen = false;       /* just emitted a generic < */
    let lastEmittedWasGenericClose = false;      /* just emitted a generic > */
    let lastEmittedWasPostfixPointer = false;    /* just emitted a postfix pointer * */
    let lastEmittedWasCastPrefixOperator = false;/* just emitted a cast-following prefix op */

    for (let index = 0; index < tokens.length; index += 1) {
        const token = tokens[index];
        const isGenericOpen = looksLikeExplicitGenericOpen(tokens, index, previousSignificantToken);
        const isDoubleGenericClose = token.type === 'operator' && token.value === '>>' && genericDepth >= 2;
        const isGenericClose = token.type === 'operator' && token.value === '>' && genericDepth > 0;
        const isPostfixPtr = isPostfixPointerStar(
            tokens, index,
            previousSignificantToken, tokenBeforePreviousSignificant,
            lastEmittedWasPostfixPointer
        );
        const tightAfterCastClose = token.type === 'operator' &&
            PREFIX_OPERATORS.has(token.value) &&
            previousSignificantToken != null &&
            previousSignificantToken.type === 'delimiter' &&
            previousSignificantToken.value === ')' &&
            isLikelyCastClose(tokens, index - 1);

        if (isGenericOpen) {
            result += token.value;
            genericDepth += 1;
            lastEmittedWasGenericOpen = true;
            lastEmittedWasGenericClose = false;
            lastEmittedWasPostfixPointer = false;
            lastEmittedWasCastPrefixOperator = false;
        } else if (isDoubleGenericClose) {
            result += token.value;
            genericDepth -= 2;
            lastEmittedWasGenericOpen = false;
            lastEmittedWasGenericClose = true;
            lastEmittedWasPostfixPointer = false;
            lastEmittedWasCastPrefixOperator = false;
        } else if (isGenericClose) {
            result += token.value;
            genericDepth -= 1;
            lastEmittedWasGenericOpen = false;
            lastEmittedWasGenericClose = true;
            lastEmittedWasPostfixPointer = false;
            lastEmittedWasCastPrefixOperator = false;
        } else if (genericDepth > 0 &&
                   token.type === 'punctuation' && token.value === ',') {
            result += token.value;
            lastEmittedWasGenericOpen = false;
            lastEmittedWasGenericClose = false;
            lastEmittedWasPostfixPointer = false;
            lastEmittedWasCastPrefixOperator = false;
        } else {
            /* Suppress space:
             *  - after generic open <
             *  - suffix delimiter after generic close >
             *  - the postfix pointer * itself (no space between type and *)
             *  - tokens that attach tightly after a postfix pointer *:
             *    , ; ) ] > * [   — e.g. T*, T*) T*[] T** T*>
             *    but NOT before = { or other operators (those keep their space)
             */
            const tightAfterPostfixPointer = lastEmittedWasPostfixPointer && (
                token.value === ',' || token.value === ';' ||
                token.value === ')' || token.value === ']' ||
                token.value === '>' || token.value === '*' ||
                token.value === '['
            );
            const suppressSpace = lastEmittedWasGenericOpen ||
                (lastEmittedWasGenericClose && (token.value === '(' || token.value === '[' || token.value === ')')) ||
                tightAfterPostfixPointer ||
                lastEmittedWasCastPrefixOperator ||
                tightAfterCastClose ||
                isPostfixPtr;

            if (!suppressSpace && needsSpaceBetween(previousEmittedToken,
                previousSignificantToken,
                tokenBeforePreviousSignificant,
                token)) {
                result += ' ';
            }
            result += token.value;
            lastEmittedWasGenericOpen = false;
            lastEmittedWasGenericClose = false;
            lastEmittedWasPostfixPointer = isPostfixPtr;
            lastEmittedWasCastPrefixOperator = tightAfterCastClose;
        }

        previousEmittedToken = token;

        if (token.type !== 'comment') {
            tokenBeforePreviousSignificant = previousSignificantToken;
            previousSignificantToken = token;
            lastLineSignificantToken = token;
        }
    }

    return {
        text: result.replace(/[\t ]+$/g, ''),
        lastSignificantToken: lastLineSignificantToken
    };
}

function formatFengSource(source, options = {}) {
    const normalized = source.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    const indentUnit = createIndentUnit(options);
    const lines = splitIntoLines(tokenize(normalized));
    const formattedLines = [];
    const delimiterStack = [];
    let previousSignificantToken = null;

    for (const tokens of lines) {
        if (tokens.length === 0) {
            formattedLines.push('');
            continue;
        }

        const leadingClosers = countLeadingClosers(tokens);
        /* Count function-call parens in the stack that shouldn't add indent */
        let funcCallCount = 0;
        for (const entry of delimiterStack) {
            if (entry.isFuncCall) { funcCallCount += 1; }
        }
        /* Leading ) that close func-call parens shouldn't reduce indent either.
         * Count how many leading ) correspond to func-call ( in the stack. */
        let leadingFuncCallClosers = 0;
        let tempStack = delimiterStack.slice();
        for (const token of tokens) {
            if (token.type === 'comment') { break; }
            if (!isClosingDelimiter(token)) { break; }
            if (token.value === ')') {
                /* Check if this ) would close a func-call ( */
                for (let s = tempStack.length - 1; s >= 0; s -= 1) {
                    if (tempStack[s].value === '(' && tempStack[s].isFuncCall) {
                        leadingFuncCallClosers += 1;
                        tempStack.splice(s, 1);
                        break;
                    } else if (tempStack[s].value === '(') {
                        /* Non-func-call (, this ) closes it normally */
                        tempStack.splice(s, 1);
                        break;
                    }
                }
            } else {
                /* } or ], pop matching entry */
                const expected = CLOSE_TO_OPEN[token.value];
                for (let s = tempStack.length - 1; s >= 0; s -= 1) {
                    if (tempStack[s].value === expected) {
                        tempStack.splice(s, 1);
                        break;
                    }
                }
            }
        }
        const effectiveIndent = Math.max(delimiterStack.length - leadingClosers - funcCallCount + leadingFuncCallClosers, 0);
        const formattedLine = formatLineTokens(tokens, previousSignificantToken);

        const isBodyCommentLine = tokens.length === 1 &&
            tokens[0].type === 'comment' &&
            tokens[0].commentRole === 'body';
        const indentPrefix = isBodyCommentLine
            ? (formattedLine.text.length > 0 ? indentUnit.repeat(effectiveIndent) + ' ' : '')
            : indentUnit.repeat(effectiveIndent);

        formattedLines.push(indentPrefix + formattedLine.text);

        if (formattedLine.lastSignificantToken != null) {
            previousSignificantToken = formattedLine.lastSignificantToken;
        }

        updateDelimiterStack(delimiterStack, tokens);
    }

    return formattedLines.join('\n');
}

function normalizeText(source) {
    return source.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
}

function isManifestIdentifier(value) {
    return /^[A-Za-z_][A-Za-z0-9_.-]*$/.test(value);
}

function formatManifestComment(trimmedLine) {
    const content = trimmedLine.slice(1).trim();

    return content.length > 0 ? `# ${content}` : '#';
}

function parseManifestLine(line) {
    const trimmed = line.trim();
    const sectionMatch = trimmed.match(/^\[\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*\]$/);
    const entryMatch = trimmed.match(/^([A-Za-z_][A-Za-z0-9_.-]*)\s*:\s*("(?:[^"\\]|\\.)*")\s*$/);

    if (trimmed.length === 0) {
        return { type: 'blank' };
    }

    if (trimmed.startsWith('#')) {
        return {
            type: 'comment',
            text: formatManifestComment(trimmed)
        };
    }

    if (sectionMatch != null) {
        return {
            type: 'section',
            name: sectionMatch[1]
        };
    }

    if (entryMatch != null && isManifestIdentifier(entryMatch[1])) {
        return {
            type: 'entry',
            key: entryMatch[1],
            value: entryMatch[2]
        };
    }

    return {
        type: 'raw',
        text: line.replace(/[\t ]+$/g, '')
    };
}

function groupManifestBlocks(nodes) {
    const blocks = [{ header: null, nodes: [] }];

    for (const node of nodes) {
        if (node.type === 'section') {
            blocks.push({ header: node, nodes: [] });
            continue;
        }

        blocks[blocks.length - 1].nodes.push(node);
    }

    return blocks;
}

function formatManifestEntry(node, maxKeyLength) {
    return `${node.key}:${' '.repeat(Math.max(maxKeyLength - node.key.length + 1, 1))}${node.value}`;
}

function renderManifestBlock(block, outputLines) {
    let maxKeyLength = 0;

    if (block.header != null) {
        outputLines.push(`[${block.header.name}]`);
    }

    for (const node of block.nodes) {
        if (node.type === 'entry') {
            maxKeyLength = Math.max(maxKeyLength, node.key.length);
        }
    }

    for (const node of block.nodes) {
        if (node.type === 'blank') {
            outputLines.push('');
            continue;
        }

        if (node.type === 'comment') {
            outputLines.push(node.text);
            continue;
        }

        if (node.type === 'entry') {
            outputLines.push(formatManifestEntry(node, maxKeyLength));
            continue;
        }

        outputLines.push(node.text);
    }
}

function formatFengManifestSource(source) {
    const normalized = normalizeText(source);
    const hasTrailingNewline = normalized.endsWith('\n');
    const rawLines = hasTrailingNewline ? normalized.slice(0, -1).split('\n') : normalized.split('\n');
    const nodes = rawLines.map(parseManifestLine);
    const blocks = groupManifestBlocks(nodes);
    const outputLines = [];

    for (const block of blocks) {
        if (block.header == null && block.nodes.length === 0) {
            continue;
        }
        renderManifestBlock(block, outputLines);
    }

    const formatted = outputLines.join('\n');

    return hasTrailingNewline ? `${formatted}\n` : formatted;
}

module.exports = {
    formatFengSource,
    formatFengManifestSource
};
