const assert = require('assert');

const { formatFengSource, formatFengManifestSource } = require('../formatter');

function runCase(name, input, expected, options) {
    assert.strictEqual(formatFengSource(input, options), expected, name);
}

function runManifestCase(name, input, expected) {
    assert.strictEqual(formatFengManifestSource(input), expected, name);
}

runCase(
    'normalizes operators and trailing comments',
    'fn calc(a:int,b:int):int {\nlet total=a+b*2; //sum\nreturn total==0||!done;\n}\n',
    'fn calc(a: int, b: int): int {\n    let total = a + b * 2; //sum\n    return total == 0 || !done;\n}\n'
);

runCase(
    'normalizes multi-line object literals',
    'fn main(args:string[]):void {\nlet user=User{\nname:"Houfeng",\nage:18\n};\n}\n',
    'fn main(args: string[]): void {\n    let user = User {\n        name: "Houfeng",\n        age: 18\n    };\n}\n'
);

runCase(
    'normalizes pointer and array parameter types',
    'extern fn map(cb:Handler,ptr:*Point,items:string[]):void;\n',
    'extern fn map(cb: Handler, ptr: *Point, items: string[]): void;\n'
);

runCase(
    'indents multi-line parameter lists and for headers',
    'fn main(\nargs:string[],\nlimit:int\n):void {\nfor(i=0;i<limit;i=i+1){\nprint(args[i]);\n}\n}\n',
    'fn main(\n    args: string[],\n    limit: int\n): void {\n    for (i = 0; i < limit; i = i + 1) {\n        print(args[i]);\n    }\n}\n'
);

runCase(
    'preserves tab indentation when requested',
    'fn main(args:string[]):void {\nlet value=-1;\nif !ready {\nreturn value;\n}\n}\n',
    'fn main(args: string[]): void {\n\tlet value = -1;\n\tif !ready {\n\t\treturn value;\n\t}\n}\n',
    { insertSpaces: false, tabSize: 4 }
);

runCase(
    'normalizes compound and bitwise operators',
    'fn run():void {\nvar count:i32=1;\ncount+=2;\ncount-=3;\ncount*=4;\ncount/=5;\nvar total:float=(float)7.8;\ntotal%=(float)3.2;\nvar mask:i32=8;\nmask&=3;\nmask|=4;\nmask^=1;\nmask<<=2;\nmask>>=1;\n}\n',
    'fn run(): void {\n    var count: i32 = 1;\n    count += 2;\n    count -= 3;\n    count *= 4;\n    count /= 5;\n    var total: float = (float)7.8;\n    total %= (float)3.2;\n    var mask: i32 = 8;\n    mask &= 3;\n    mask |= 4;\n    mask ^= 1;\n    mask <<= 2;\n    mask >>= 1;\n}\n'
);

runManifestCase(
    'formats manifest sections comments and aligned values',
    '#package\n [package] \nname:"examples"\nversion:  "0.1.0"\nout:"build/"\n\n# deps\n[dependencies]\ndemo:"0.1.0"\nbase.core:"1.2.3"\n',
    '# package\n[package]\nname:    "examples"\nversion: "0.1.0"\nout:     "build/"\n\n# deps\n[dependencies]\ndemo:      "0.1.0"\nbase.core: "1.2.3"\n'
);

runManifestCase(
    'preserves unknown manifest lines while normalizing known ones',
    '[package]\nname:"demo"\ninvalid line\n#note\n',
    '[package]\nname: "demo"\ninvalid line\n# note\n'
);

runCase(
    'preserves single-line block comment unchanged',
    'fn test(): void {\n    let x = /* value */ 1;\n}\n',
    'fn test(): void {\n    let x = /* value */ 1;\n}\n'
);

runCase(
    'formats top-level doc comment with standard alignment',
    '/**\n* Description\n* @param foo bar\n*/\nfn test(): void {}\n',
    '/**\n * Description\n * @param foo bar\n */\nfn test(): void {}\n'
);

runCase(
    'formats doc comment inside function body',
    'fn outer(): void {\n/**\n* Does something\n* @param x the value\n*/\nlet y = 1;\n}\n',
    'fn outer(): void {\n    /**\n     * Does something\n     * @param x the value\n     */\n    let y = 1;\n}\n'
);

runCase(
    'formats doc comment with closing delimiter on its own line',
    '/**\n* Summary line\n*\n* @return result\n*/\nfn get(): int {}\n',
    '/**\n * Summary line\n *\n * @return result\n */\nfn get(): int {}\n'
);

// --- generic syntax formatting tests ---

runCase(
    'preserves angle brackets in generic function declaration without spaces',
    'fn foo<T>(x:T):T {\nreturn x;\n}\n',
    'fn foo<T>(x: T): T {\n    return x;\n}\n'
);

runCase(
    'preserves angle brackets in generic type with two params without spaces',
    'type Map<K,V> {\nlet key:K;\nlet value:V;\n}\n',
    'type Map<K, V> {\n    let key: K;\n    let value: V;\n}\n'
);

runCase(
    'formats explicit generic call :< without extra spaces',
    'fn main():void {\nlet x=identity:<int>(42);\n}\n',
    'fn main(): void {\n    let x = identity:<int>(42);\n}\n'
);

runCase(
    'formats generic type references in variable declarations',
    'fn main():void {\nlet m:Map<string,int>;\n}\n',
    'fn main(): void {\n    let m: Map<string, int>;\n}\n'
);

runCase(
    'preserves comparison operators with spaces (not confused with generics)',
    'fn check(n:int):bool {\nreturn n<10;\n}\n',
    'fn check(n: int): bool {\n    return n < 10;\n}\n'
);

console.log('formatter tests passed');