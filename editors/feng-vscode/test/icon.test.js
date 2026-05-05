const assert = require('assert');
const fs = require('fs');
const path = require('path');

const packageJson = require('../package.json');

function findLanguage(id) {
    const languages = packageJson.contributes && Array.isArray(packageJson.contributes.languages)
        ? packageJson.contributes.languages
        : [];

    return languages.find(language => language.id === id);
}

function assertLanguageIcon(id, expectedExtensions, expectedIconPath) {
    const language = findLanguage(id);

    assert(language, `expected language contribution for ${id}`);
    assert.deepStrictEqual(language.extensions, expectedExtensions, `unexpected extensions for ${id}`);
    assert(language.icon, `expected icon contribution for ${id}`);
    assert.strictEqual(language.icon.light, expectedIconPath, `unexpected light icon for ${id}`);
    assert.strictEqual(language.icon.dark, expectedIconPath, `unexpected dark icon for ${id}`);
}

const extensionRoot = path.join(__dirname, '..');
const commands = packageJson.contributes && Array.isArray(packageJson.contributes.commands)
    ? packageJson.contributes.commands
    : [];
const restartCommand = commands.find(command => command.command === 'feng.restartLanguageServer');
const fengDefaults = packageJson.contributes && packageJson.contributes.configurationDefaults
    ? packageJson.contributes.configurationDefaults['[feng]']
    : null;

assertLanguageIcon('feng', ['.feng', '.ff'], './icons/feng-ff.svg');
assertLanguageIcon('feng-manifest', ['.fm'], './icons/feng-fm.svg');
assertLanguageIcon('feng-bundle', ['.fb'], './icons/feng-fb.svg');
assertLanguageIcon('feng-symbol-table', ['.ft'], './icons/feng-ft.svg');

for (const iconPath of [
    'icons/feng-ff.svg',
    'icons/feng-fm.svg',
    'icons/feng-fb.svg',
    'icons/feng-ft.svg'
]) {
    assert(fs.existsSync(path.join(extensionRoot, iconPath)), `expected icon asset ${iconPath}`);
}

assert(restartCommand, 'expected Feng language server restart command contribution');
assert.strictEqual(restartCommand.title, 'Feng: Restart Language Server');
assert(fengDefaults, 'expected Feng language configuration defaults');
assert.strictEqual(fengDefaults['editor.quickSuggestions'].other, true);
assert.strictEqual(fengDefaults['editor.suggestOnTriggerCharacters'], true);

console.log('icon metadata tests passed');
