const KEYWORDS = new Set([
    'type',
    'spec',
    'fit',
    'extern',
    'fn',
    'let',
    'var',
    'pu',
    'pr',
    'self',
    'mod',
    'use',
    'as',
    'if',
    'else',
    'while',
    'for',
    'break',
    'continue',
    'try',
    'catch',
    'finally',
    'throw',
    'return',
    'void'
]);

const CONTROL_PAREN_KEYWORDS = new Set(['if', 'while', 'for', 'catch', 'return', 'throw']);
const PREFIX_CONTEXT_KEYWORDS = new Set(['if', 'while', 'return', 'throw']);
const PREFIX_OPERATORS = new Set(['!', '-', '*', '+']);
const THREE_CHAR_OPERATORS = new Set(['<<=', '>>=']);
const MULTI_CHAR_OPERATORS = new Set([
    '->', '&&', '||', '==', '!=', '<=', '>=',
    '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<', '>>'
]);
const SINGLE_CHAR_OPERATORS = new Set(['+', '-', '*', '/', '%', '=', '!', '<', '>', '&', '|', '^', '~']);
const DELIMITERS = new Set(['(', ')', '[', ']', '{', '}']);
const PUNCTUATION = new Set([',', ':', ';', '.']);
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

    while (index < source.length) {
        const current = source[index];
        const next = index + 1 < source.length ? source[index + 1] : '';

        if (current === '\n') {
            tokens.push({ type: 'comment', value: fragment.replace(/[\t ]+$/g, '') });
            tokens.push({ type: 'newline' });
            fragment = '';
            index += 1;
            continue;
        }

        if (current === '*' && next === '/') {
            fragment += '*/';
            tokens.push({ type: 'comment', value: fragment.replace(/[\t ]+$/g, '') });
            return index + 2;
        }

        fragment += current;
        index += 1;
    }

    if (fragment.length > 0) {
        tokens.push({ type: 'comment', value: fragment.replace(/[\t ]+$/g, '') });
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

    if (isOperator(previousSignificantToken) && previousEmittedToken === previousSignificantToken) {
        return !isPrefixOperator(previousSignificantToken, tokenBeforePreviousSignificant);
    }

    if (currentToken.value === '.' || previousEmittedToken.value === '.') {
        return false;
    }

    if (currentToken.value === ',' || currentToken.value === ';' || currentToken.value === ':') {
        return false;
    }

    if (previousEmittedToken.value === ',' || previousEmittedToken.value === ':') {
        return true;
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
    for (const token of tokens) {
        if (token.type === 'comment') {
            continue;
        }

        if (isOpeningDelimiter(token, ['(', '[', '{'])) {
            delimiterStack.push(token.value);
            continue;
        }

        if (isClosingDelimiter(token)) {
            const expected = CLOSE_TO_OPEN[token.value];

            if (delimiterStack.length > 0 && delimiterStack[delimiterStack.length - 1] === expected) {
                delimiterStack.pop();
            } else if (delimiterStack.length > 0) {
                delimiterStack.pop();
            }
        }
    }
}

function formatLineTokens(tokens, previousSignificantTokenBeforeLine) {
    let result = '';
    let previousEmittedToken = null;
    let previousSignificantToken = previousSignificantTokenBeforeLine;
    let tokenBeforePreviousSignificant = null;
    let lastLineSignificantToken = null;

    for (const token of tokens) {
        if (needsSpaceBetween(previousEmittedToken,
            previousSignificantToken,
            tokenBeforePreviousSignificant,
            token)) {
            result += ' ';
        }

        result += token.value;
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
        const effectiveIndent = Math.max(delimiterStack.length - leadingClosers, 0);
        const formattedLine = formatLineTokens(tokens, previousSignificantToken);

        formattedLines.push(indentUnit.repeat(effectiveIndent) + formattedLine.text);

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