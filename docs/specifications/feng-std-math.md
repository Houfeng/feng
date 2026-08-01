# Feng 标准库 Math 规范

本文档定义标准库模块 `std.math` 当前提供的双精度标量数学 API。底层实现固定基于 `libm`；标准库层只负责收敛公开命名与签名，不在包装层额外引入裁剪、归一化或特判逻辑。

## 1 职责

- 定义 `std.math` 中公开类型 `Math` 的稳定 API 面，统一承载双精度 `f64` 数学函数。
- 约束 `Math` 的公开方法与底层 `libm` 符号之间的一一对应关系，避免标准库层重新发明一套独立数值语义。
- 明确当前版本的支持边界：只公开 `f64` 标量函数，不在本规范中引入 `f32`、向量数学或数学常量对象。

## 2 公开 API

使用方通过 `import std.math;` 后可直接使用 `Math`。

### 2.1 基础值与比较

| 方法 | 签名 | 底层符号 | 说明 |
| --- | --- | --- | --- |
| `Math.abs` | `open static func abs(x: f64): f64` | `fabs` | 返回 `x` 的绝对值 |
| `Math.min` | `open static func min(x: f64, y: f64): f64` | `fmin` | 返回两个值中的较小值，按 `fmin` 语义处理 NaN |
| `Math.max` | `open static func max(x: f64, y: f64): f64` | `fmax` | 返回两个值中的较大值，按 `fmax` 语义处理 NaN |
| `Math.copySign` | `open static func copySign(x: f64, sign: f64): f64` | `copysign` | 返回保留 `x` 的绝对值且符号位取自 `sign` 的结果 |

### 2.2 三角与双曲函数

| 方法 | 签名 | 底层符号 | 说明 |
| --- | --- | --- | --- |
| `Math.sin` | `open static func sin(x: f64): f64` | `sin` | 正弦 |
| `Math.cos` | `open static func cos(x: f64): f64` | `cos` | 余弦 |
| `Math.tan` | `open static func tan(x: f64): f64` | `tan` | 正切 |
| `Math.asin` | `open static func asin(x: f64): f64` | `asin` | 反正弦 |
| `Math.acos` | `open static func acos(x: f64): f64` | `acos` | 反余弦 |
| `Math.atan` | `open static func atan(x: f64): f64` | `atan` | 反正切 |
| `Math.atan2` | `open static func atan2(y: f64, x: f64): f64` | `atan2` | 返回点 `(x, y)` 相对 x 轴的极角 |
| `Math.sinh` | `open static func sinh(x: f64): f64` | `sinh` | 双曲正弦 |
| `Math.cosh` | `open static func cosh(x: f64): f64` | `cosh` | 双曲余弦 |
| `Math.tanh` | `open static func tanh(x: f64): f64` | `tanh` | 双曲正切 |

### 2.3 指数、对数与幂

| 方法 | 签名 | 底层符号 | 说明 |
| --- | --- | --- | --- |
| `Math.exp` | `open static func exp(x: f64): f64` | `exp` | 返回 $e^x$ |
| `Math.exp2` | `open static func exp2(x: f64): f64` | `exp2` | 返回 $2^x$ |
| `Math.expm1` | `open static func expm1(x: f64): f64` | `expm1` | 返回 $e^x - 1$ |
| `Math.log` | `open static func log(x: f64): f64` | `log` | 自然对数 |
| `Math.log2` | `open static func log2(x: f64): f64` | `log2` | 以 2 为底的对数 |
| `Math.log10` | `open static func log10(x: f64): f64` | `log10` | 以 10 为底的对数 |
| `Math.log1p` | `open static func log1p(x: f64): f64` | `log1p` | 返回 `log(1 + x)` |
| `Math.pow` | `open static func pow(x: f64, y: f64): f64` | `pow` | 返回 $x^y$ |
| `Math.sqrt` | `open static func sqrt(x: f64): f64` | `sqrt` | 平方根 |
| `Math.cbrt` | `open static func cbrt(x: f64): f64` | `cbrt` | 立方根 |
| `Math.hypot` | `open static func hypot(x: f64, y: f64): f64` | `hypot` | 返回 $\sqrt{x^2 + y^2}$ |

### 2.4 取整与余数

| 方法 | 签名 | 底层符号 | 说明 |
| --- | --- | --- | --- |
| `Math.ceil` | `open static func ceil(x: f64): f64` | `ceil` | 向上取整 |
| `Math.floor` | `open static func floor(x: f64): f64` | `floor` | 向下取整 |
| `Math.round` | `open static func round(x: f64): f64` | `round` | 按 `libm round` 语义取整；半值远离零 |
| `Math.trunc` | `open static func trunc(x: f64): f64` | `trunc` | 向零截断 |
| `Math.fmod` | `open static func fmod(x: f64, y: f64): f64` | `fmod` | 浮点取模，语义与 `fmod` 一致 |
| `Math.remainder` | `open static func remainder(x: f64, y: f64): f64` | `remainder` | IEEE 风格余数，语义与 `remainder` 一致 |

## 3 语义

- `Math` 的公开方法全部是纯函数式静态方法；它们不依赖实例状态，也不读写标准库内部全局状态。
- 当前版本 `Math` 的所有参数位与返回位都固定为 `f64`。标准库不得把整数、`f32` 或其他数值类型隐式扩展成这一组 API。
- `Math` 的每个公开方法都必须直接委托到对应的 `libm` 符号，不得在包装层额外插入边界裁剪、特殊值替换或错误吞并逻辑。
- `Math.min` / `Math.max` 的语义固定对齐 `fmin` / `fmax`，不是通过普通 `<` / `>` 比较手写实现。
- `Math.fmod` 与 `Math.remainder` 是两个不同运算：前者对齐 `fmod`，后者对齐 `remainder`；标准库不得把二者混用成同一实现。
- `Math.round` 固定采用 `libm round` 的取整规则；当结果正好落在两个整数中点时，必须向远离零的方向取整。
- 标准库当前不拦截 `libm` 的 domain/range 行为，也不为 `errno` 或浮点环境标志定义额外 Feng 语义；调用结果中的 `NaN`、`+/-inf`、有符号零与舍入行为都遵循宿主 `libm`。

```feng
import std.math;

let angle = Math.atan2(4.0, 3.0);
let length = Math.hypot(3.0, 4.0);
let root = Math.sqrt(81.0);
let scaled = Math.copySign(2.0, -1.0);
```

## 4 规则

- [必须] `Math` 定义在模块 `std.math` 中，且当前版本只通过 `open static func` 公开数学能力。
- [必须] `Math` 当前公开的所有方法都以 `f64` 作为参数类型与返回类型；若未来引入 `f32` 版本，必须显式新增而不是改写现有签名。
- [必须] 每个公开方法都与一个稳定的 `libm` 符号一一对应，不得让多个公开名字指向不同平台上的不同语义实现。
- [必须] `Math.min` / `Math.max` 必须分别映射到 `fmin` / `fmax`，不得在标准库层自行实现 NaN 分支。
- [必须] `Math.fmod` 必须映射到 `fmod`；`Math.remainder` 必须映射到 `remainder`。
- [必须] `Math.copySign` 必须保留第一个参数的绝对值，并把第二个参数的符号位复制到结果中。
- [禁止] 在 `Math` 包装层对输入做隐式角度制转换、单位换算或精度裁剪。
- [禁止] 在 `Math` 包装层把 `libm` 的 `NaN`、`+/-inf` 或有符号零改写成其他 Feng 值。

## 5 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-builtin-type.md](./feng-builtin-type.md): `f64` 的类型语义。
- [feng-interop.md](./feng-interop.md): `extern func` 与 C ABI 导入规则。