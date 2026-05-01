# feng 依赖管理机制

## feng.fm 本地引用

依赖支持本地路径，目标路可以是绝对路径，或相对路径（相对于当前 `feng.fm`）

```fm
[dependencies]
xxx: "./local-path"
```

`[dependencies]` 的键仍然表示依赖包名；如果值是本地路径，则该键必须与目标包的 `package.name` 一致，工具需做一致性校验，不一致时报错。

本地路径必须以 `./`、`../` 或 `/` 开头，以此识别本地路径，其他均认为远程包版本号。

本地路径可以是如下三种情况：

- 一个 `fb` 格式的包文件，编译和构建时直接使用
- 一个包含 `feng.fm` 文件的目录，编译和构建时递归构建，并使用构建产物（可能过当前 fm 中的 name、out 等信息找到）
- 一个 `feng.fm` 文件路径，和上条处理逻辑一样

feng build 时，需要将本地路径的项目中非 `.fb` 文件的依赖递归构建，不可循环，出现循环时报错。

本地路径的方式，还可用于 monorepo 的开发方式，多个本地 project 间引用。

## feng.fm 远程引用

增加可选 registry 节，用于声明依赖的包仓库。

```fm
[registry]
url: "local-registry-url"
```

为什么 registry 是可选的？因为未来会建立「官方 registry」，在工程未配置 registry 时，使用「官方 registry」作为默认仓库。

注意：官方 registry 并无特殊之处，只是会在安装 feng 时，默认作为「全局 registry 配置」

## feng deps

## 本地路径依赖的管理

- feng deps add 时，如果依赖是本地路径，则直接添加 dependencies 信息
- feng deps remove 时，如果依赖是本地路径，则直接从 dependencies 移除
- feng deps install 时，本地路径全局跳过，不必做处理

## 远程包依赖的管理

- "~/.feng/cache" 作为包的「全局缓存目录」，格式为「包名-版本.fb」
- feng deps add 命令将包安装到「全局缓存目录」并将包信息添加当前工程 feng.fm 的 dependencies 中，如果已经有全局缓存则直接添加 dependencies 信息
- feng deps remove 命令仅将包信息从 dependencies 移除，不删全局缓存
- feng deps install 检查当前工程 dependencies 远程包是否都已在全局缓存，如果没有则安装到全局缓存

从远程 registry 安装包时的 url 格式：

```bash
${registry}/packages/${name}-${version}.fb
```

注意：feng build 时，会先触发 feng deps install，未安装到全局缓存的包，将会被安装到全局缓存。

## feng 全局配置

全局配置路径 `~/.feng/config.fm`

```fm
[registry]
url: "global-registry-url"
```

安装包之前，如果有局部 registry 配置，则使用局部 registry，如果没有局部 registry 配置，才使用全局 registry，如果都没有，则报错并提示。

## 编译及构建

无论本地路径依赖还是远程依赖，编译时 feng build 就都能根据 dependencies 找到依赖，并得到完整的 pkg 列表，通过 -pkg 传递给 feng 顶层编译命令了。
