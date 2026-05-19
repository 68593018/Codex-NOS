# AGENTS.md

本文件定义 Codex-NOS 工程中所有 AI agent 都必须遵守的通用工程规则。

## Project Scope

Codex-NOS 是一个用于网络操作系统平台软件分析、验证和原型开发的 C 语言工程。

工程目标：

- 以 C 语言为主体实现平台核心逻辑、组件模型、调度、服务和 IPC。
- 使用 Python 编写辅助脚本、生成工具、测试工具和数据处理工具。
- 代码按工业级实现要求处理性能、资源开销、并发安全和生命周期管理。

## Repository Contents

应提交到 Git 的内容：

- `Makefile`
- `conf/` 下的 YAML 配置
- `doc/` 下的设计文档
- `include/` 下的手写头文件
- `scripts/` 下的辅助脚本
- `src/` 下的手写 C 源码和私有头文件
- agent 通用规则文档，如 `AGENTS.md`

不应提交到 Git 的内容：

- `*.o`
- `*.so`
- `nos_Proc*`
- `include/nos_ids.h`
- `src/core/manifest_*.c`
- `.agents/`
- `.codex/`
- Python 缓存、编辑器缓存、日志和 core dump

## Generated Files

`conf/*.yaml` 是节点、组件、服务 ID、buffer profile 和 manifest 的源数据。

生成路径：

- `scripts/gen_manifest.py` 从 `conf/` 生成 `include/nos_ids.h`
- `scripts/gen_manifest.py` 从 `conf/` 生成 `src/core/manifest_*.c`
- `make` 生成 `*.o`、`*.so` 和 `nos_Proc*`

规则：

- 不要手工修改生成文件。
- 需要修改节点、组件、服务、ID 或 buffer profile 时，优先修改 `conf/`。
- 生成文件不得作为源码提交。

## Build And Validation

代码修改后，应至少执行：

```sh
make test
make clean
make
```

如果只修改文档或规则文件，可以不执行构建，但提交说明或回复中必须明确说明原因。

构建失败时不要提交代码，除非任务目标就是记录或暴露当前失败状态，并且必须说明失败命令和失败原因。

## C Code Rules

通用规则：

- 代码保持简单清晰，不做过度设计。
- 不随意引入复杂框架或外部依赖。
- 新增抽象必须服务于真实复杂度、重复消除或既有架构一致性。
- 不确定内容不要写成确定结论。

组件开发规则：

- 组件严禁使用 `static` 全局或局部状态。
- 同一个 `.so` 被多实例加载时，实例之间必须保持状态隔离。
- 所有运行时状态必须存储在 `self->priv` 私有上下文或外部 KV 数据库中。
- 组件回调函数，如 `on_msg`，必须保证线程安全和可重入。
- 组件 `stop` 阶段必须显式释放所有自持资源，如定时器、内存和外部句柄。
- 禁止产生悬挂指针和生命周期不明确的资源引用。

## Embedded Service Rules

嵌入式服务包括 Log、Timer、DB 等平台服务。

规则：

- 服务必须支持多线程并发调用。
- 禁止使用 `__thread` 或 TLS 等隐式上下文。
- API 调用必须显式传递 `self`、句柄或等价上下文。
- 服务初始化必须优先于所有业务组件加载。
- 服务停止和释放必须考虑仍在运行的组件、回调和 pending 任务。

## Documentation Rules

文档采用工程汇报风格：

- 优先使用“一句话结论 + 表格 + 简要说明”。
- 明确区分事实、假设和建议。
- 不把推测写成确定结论。
- 修改架构、运行模型、配置模型或约束时，同步更新相关文档。

## Git Rules

每次有效代码修改并通过验证后，必须及时提交并推送到远程仓库。

规则：

- 提交前检查 `git status --short --ignored`。
- 不提交构建产物、生成文件、本地缓存或 agent 私有目录。
- commit message 使用简洁的工程语义。
- 不要回退用户已有修改，除非用户明确要求。
- 推送目标默认使用当前分支的 upstream。

## Review Rules

每次代码修改完成后，必须回头检查影响范围：

- 调用链是否同步更新。
- 资源依赖是否完整。
- 是否引入竞态、泄露、悬挂指针或边界错误。
- 是否需要同步更新配置、文档、生成脚本或构建规则。
