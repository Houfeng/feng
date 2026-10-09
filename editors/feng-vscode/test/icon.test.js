const assert = require('assert');
const fs = require('fs');
const path = require('path');

const packageJson = require('../package.json');

/** Find the language contribution whose file icon metadata is under test. */
function findLanguage(id) {
    const languages = packageJson.contributes && Array.isArray(packageJson.contributes.languages)
        ? packageJson.contributes.languages
        : [];

    return languages.find(language => language.id === id);
}

/** Check file extensions and the independent light and dark icon paths. */
function assertLanguageIcon(id, expectedExtensions, expectedIconPaths) {
    const language = findLanguage(id);

    assert(language, `expected language contribution for ${id}`);
    assert.deepStrictEqual(language.extensions, expectedExtensions, `unexpected extensions for ${id}`);
    assert(language.icon, `expected icon contribution for ${id}`);
    assert.strictEqual(language.icon.light, expectedIconPaths.light, `unexpected light icon for ${id}`);
    assert.strictEqual(language.icon.dark, expectedIconPaths.dark, `unexpected dark icon for ${id}`);
}

const extensionRoot = path.join(__dirname, '..');
const extensionIconPath = packageJson.icon;
const commands = packageJson.contributes && Array.isArray(packageJson.contributes.commands)
    ? packageJson.contributes.commands
    : [];
const restartCommand = commands.find(command => command.command === 'feng.restartLanguageServer');
const fengDefaults = packageJson.contributes && packageJson.contributes.configurationDefaults
    ? packageJson.contributes.configurationDefaults['[feng]']
    : null;

assertLanguageIcon('feng', ['.feng', '.ff'], {
    light: './icons/feng-ff-light.svg',
    dark: './icons/feng-ff-dark.svg'
});
assertLanguageIcon('feng-manifest', ['.fm'], {
    light: './icons/feng-fm-light.svg',
    dark: './icons/feng-fm-dark.svg'
});
assertLanguageIcon('feng-bundle', ['.fb'], {
    light: './icons/feng-fb-light.svg',
    dark: './icons/feng-fb-dark.svg'
});
assertLanguageIcon('feng-symbol-table', ['.ft'], {
    light: './icons/feng-ft-light.svg',
    dark: './icons/feng-ft-dark.svg'
});
assert.strictEqual(extensionIconPath, 'icons/feng-logo.png');

for (const iconPath of [
    'icons/feng-logo.png',
    'icons/feng-ff-light.svg',
    'icons/feng-ff-dark.svg',
    'icons/feng-fm-light.svg',
    'icons/feng-fm-dark.svg',
    'icons/feng-fb-light.svg',
    'icons/feng-fb-dark.svg',
    'icons/feng-ft-light.svg',
    'icons/feng-ft-dark.svg'
]) {
    assert(fs.existsSync(path.join(extensionRoot, iconPath)), `expected icon asset ${iconPath}`);
}

assert(restartCommand, 'expected Feng language server restart command contribution');
assert.strictEqual(restartCommand.title, 'Feng: Restart Language Server');
assert(fengDefaults, 'expected Feng language configuration defaults');
assert.strictEqual(fengDefaults['editor.quickSuggestions'].other, true);
assert.strictEqual(fengDefaults['editor.suggestOnTriggerCharacters'], true);

console.log('icon metadata tests passed');
