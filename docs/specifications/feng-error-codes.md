# Feng 编译器错误码索引

本文只维护错误码分类入口，不重复定义各类错误码。具体编号、用途、消息模板、归类和迁移规则均以
对应主规范为准，避免总表与分段规范分别维护后产生不一致。

## LE：词法错误

[LE 词法错误码规范](./feng-error-codes-le.md)

## SE：语法错误

[SE 语法错误码规范](./feng-error-codes-se.md)

## AE：语义错误

[AE 语义错误码规范](./feng-error-codes-ae.md)

## CE：发码错误

[CE 发码错误码规范](./feng-error-codes-ce.md)

## IE：基础错误

IE 当前只有以下两个基础错误码，暂在本索引中作为唯一规范定义；如后续扩展为独立分类，应迁入单独
主规范并在此仅保留链接。

| 错误码 | 用途 | 错误文案 |
|---|---|---|
| `IE0001` | 内存不足 | `out of memory` |
| `IE0002` | 编译器内部错误 | `internal compiler error` |
