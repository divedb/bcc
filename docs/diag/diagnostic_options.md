## DiagnosticOptions类设计文档

#### 1 概述

`DiagnosticOptions`是BCC编译器诊断系统中的运行时配置组件，用于控制诊断信息（Diagnostic）的最终严重级别（Severity）以及是否输出。该类通过对诊断项或诊断组进行动态配置，实现诊断信息的启用、禁用以及严重级别调整。

在编译过程中，诊断信息通常具有默认严重级别（如Note、Warning、Error等）。`DiagnosticOptions`允许用户在运行时对这些默认行为进行覆盖，从而满足不同场景下的诊断需求，例如：

* 将某些警告提升为错误；
* 将某些错误降级为警告；
* 禁用特定诊断信息；
* 批量控制某类诊断信息的输出行为。

该类由`DiagnosticsEngine`使用，在诊断信息发送至消费者（`DiagnosticConsumer`）之前应用相应策略。

#### 2 功能特点

##### 2.1 诊断级别覆盖

支持针对单个诊断类型修改其严重级别。

例如：

* 将未使用变量警告提升为错误；
* 将兼容性错误降级为警告。

##### 2.2 诊断抑制

支持完全忽略某类诊断，使其不再输出。

例如：

* 忽略所有风格检查警告；
* 忽略特定实验性特性产生的提示信息。

##### 2.3 分组管理

支持以诊断组（Group）为单位统一管理诊断行为。

例如：

* 禁用整个`"unused"`组；
* 将整个`"compatibility"`组提升为Error。

##### 2.4 运行时动态配置

无需修改诊断定义即可动态调整诊断策略，提升系统灵活性。

#### 3 优先级规则

当某个诊断同时受到多种配置影响时，系统按照以下优先级确定最终结果：

| 优先级   | 配置来源                      |
| ----- | ------------------------- |
| 1（最高） | 单个诊断覆盖（Per-kind Override） |
| 2     | 诊断组覆盖（Group Override）     |
| 3（最低） | 诊断定义中的默认严重级别              |

即：

```text
Per-kind Override
        ↓
 Group Override
        ↓
 Default Severity
```

若高优先级规则存在，则低优先级规则将被忽略。

#### 4 类结构

```cpp
class DiagnosticOptions
```

该类维护两类覆盖规则：

##### 4.1 单诊断覆盖表

```cpp
std::unordered_map<diag::DiagKind, Override> kind_overrides_;
```

用于记录针对具体诊断类型的配置。

示例：

```cpp
unused_variable => Error
unused_function => Ignore
```

##### 4.2 诊断组覆盖表

```cpp
std::unordered_map<std::string, Override> group_overrides_;
```

用于记录针对诊断组的配置。

示例：

```cpp
"unused" => Warning
"style"  => Ignore
```

#### 5. Override数据结构

**定义**

```cpp
struct Override {
  bool suppressed;
  DiagSeverity severity;
};
```

**成员说明**

| 成员         | 类型           | 说明       |
| ---------- | ------------ | -------- |
| suppressed | bool         | 是否抑制该诊断  |
| severity   | DiagSeverity | 覆盖后的严重级别 |

**逻辑说明**

当：

```cpp
suppressed == true
```

表示该诊断被完全忽略。否则：

```cpp
severity
```

表示最终采用的严重级别。

#### 6 公共接口说明

##### 6.1 SetSeverity

```cpp
void SetSeverity(diag::DiagKind kind, DiagSeverity sev);
```

**功能**

为指定诊断类型设置新的严重级别。

#### 参数

| 参数   | 说明     |
| ---- | ------ |
| kind | 诊断类型   |
| sev  | 新的严重级别 |

**示例**

```cpp
options.SetSeverity(diag::warn_unused_variable, DiagSeverity::Error);
```

效果：

```text
Warning => Error
```

##### 6.2 Ignore

```cpp
void Ignore(diag::DiagKind kind);
```

**功能**

忽略指定诊断类型。

**示例**

```cpp
options.Ignore(diag::warn_unused_variable);
```

效果：

```text
该诊断不再输出
```

##### 6.3 SetGroupSeverity

```cpp
void SetGroupSeverity(std::string_view group, DiagSeverity sev);
```

**功能**

修改指定诊断组内所有诊断的严重级别。

**参数**

| 参数    | 说明     |
| ----- | ------ |
| group | 诊断组名称  |
| sev   | 目标严重级别 |

**示例**

```cpp
options.SetGroupSeverity("unused", DiagSeverity::Error);
```

**效果**：

```text
unused组中的所有Warning => Error
```

##### 6.4 IgnoreGroup

```cpp
void IgnoreGroup(std::string_view group);
```

**功能**

忽略整个诊断组。

**示例**

```cpp
options.IgnoreGroup("unused");
```

效果：

```text
unused组中的所有诊断均被抑制
```

##### 6.5 GetEffectiveSeverity

```cpp
std::optional<DiagSeverity> GetEffectiveSeverity(diag::DiagKind kind) const noexcept;
```

**功能**

计算指定诊断的最终严重级别。

该函数会综合：

1. 单诊断覆盖规则；
2. 诊断组覆盖规则；
3. 默认严重级别；

得到最终结果。

**返回值**

**诊断有效返回：**

```cpp
std::optional<DiagSeverity>
```

包含最终严重级别。

例如：

```cpp
Error
Warning
Note
```

##### 诊断被抑制返回：

```cpp
std::nullopt
```

表示诊断不应被输出。

#### 7 典型使用场景

##### 场景一：实现`-Werror`

```cpp
options.SetGroupSeverity("warning", DiagSeverity::Error);
```

##### 场景二：实现`-Wno-unused-variable`

```cpp
options.Ignore(diag::warn_unused_variable);
```

##### 场景三：实现`-Wno-unused`

```cpp
options.IgnoreGroup("unused");
```

#### 8 总结

`DiagnosticOptions`是诊断系统中的策略控制模块，负责在运行时动态决定诊断信息的最终行为。其核心职责包括：

* 单诊断级别覆盖；
* 单诊断抑制；
* 诊断组级别覆盖；
* 诊断组抑制；
* 计算最终有效严重级别。

通过分层覆盖机制和明确的优先级策略，该类能够实现类似GCC、Clang中`-Werror`、`-Wno-*`等编译选项的功能，为编译器诊断系统提供灵活且可扩展的配置能力。
