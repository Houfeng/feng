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
    'fn run():void {\nvar total:float=(float)7.8;\ntotal%=(float)3.2;\nvar mask:i32=8;\nmask>>=1;\nmask&=3;\n}\n',
    'fn run(): void {\n    var total: float = (float)7.8;\n    total %= (float)3.2;\n    var mask: i32 = 8;\n    mask >>= 1;\n    mask &= 3;\n}\n'
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

console.log('formatter tests passed');