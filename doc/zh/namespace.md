# 名字空间

## 使用场景
- 租户隔离：系统支持百万级或千万级数量的名字空间。名字空间限制数据访问范围，实现租户间的数据隔离。

## 使用流程

### 1. 创建名字空间
执行命令：
```text
eloqkv ❯ NAMESPACE ADD tenant1
"bXluZXduYW1lc3BhY2V0b2"
```

### 2. 认证
使用生成的 Token 进行认证：
```text
eloqkv ❯ AUTH bXluZXduYW1lc3BhY2V0b2
OK
```

### 3. 数据读写
写入和读取数据：
```text
eloqkv ❯ SET key1 value1
OK
eloqkv ❯ GET key1
"value1"
```

## 键隔离
- `AUTH`认证时，通过不同的令牌区分不同的名字空间。
- 连接仅访问自身名字空间中的键。
- 名字空间的租户不支持 `SELECT` 命令。

## 认证方式
访问名字空间的步骤：
1. 连接服务器。
2. 执行 `AUTH <token>`。
连接切换到与该 Token 关联的名字空间。

## 管理
除 `NAMESPACE CURRENT` 外，管理名字空间需要连接处于 `default` 名字空间（若配置了密码 `requirepass`，则连接还必须通过认证）。

### 命令

#### NAMESPACE ADD <namespace_name>
- 创建名字空间。
- 返回生成的 Token。
- 返回值示例：`"bXluZXduYW1lc3BhY2V0b2"`

#### NAMESPACE GET <namespace_name>
- 返回该名字空间的 Token。
- 返回值示例：`"bXluZXduYW1lc3BhY2V0b2"`

#### NAMESPACE GET *
- 返回名字空间与 Token 的列表。
- 返回值示例：
  ```text
  1) "tenant1"
  2) "bXluZXduYW1lc3BhY2V0b2"
  3) "tenant2"
  4) "YW5vdGhlcm5hbWVzcGFjZXRv"
  ```

#### NAMESPACE REFRESH <namespace_name>
- 更新名字空间的 Token。
- 返回 Token。
- 返回值示例：`"YW5vdGhlcm5hbWVzcGFjZXRv"`

#### NAMESPACE DEL <namespace_name>
- 删除名字空间。
- 返回值示例：`"OK"`

#### NAMESPACE CURRENT
- 返回连接的名字空间名称。
- 返回值示例：`"tenant1"`
