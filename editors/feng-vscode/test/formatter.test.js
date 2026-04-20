const assert = require('assert');

const { formatFengSource } = require('../formatter');

function runCase(name, input, expected, options) {
    assert.strictEqual(formatFengSource(input, options), expected, name);
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

console.log('formatter tests passed');