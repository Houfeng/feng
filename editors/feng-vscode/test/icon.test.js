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

console.log('icon metadata tests passed');