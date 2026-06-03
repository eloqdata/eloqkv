# Namespace

## Use Cases
- Tenant Isolation: The system supports a scale of millions or tens of millions of namespaces. Namespaces restrict data access, providing isolation between tenants.

## Usage Workflow

### 1. Create a Namespace
Run the command:
```
eloqkv ❯ NAMESPACE ADD tenant1
"bXluZXduYW1lc3BhY2V0b2"
```

### 2. Authenticate
Authenticate using the token:
```
eloqkv ❯ AUTH bXluZXduYW1lc3BhY2V0b2
OK
```

### 3. Read and Write Data
Write and read data:
```
eloqkv ❯ SET key1 value1
OK
eloqkv ❯ GET key1
"value1"
```

## Key Isolation
- During `AUTH` authentication, the system uses tokens to distinguish namespaces.
- A connection to a namespace only accesses keys of that namespace.
- Tenants of a namespace do not support the `SELECT` command.

## Authentication Method
To access a namespace:
1. Connect to the server.
2. Run `AUTH <token>`.
The connection shifts to the namespace associated with the token.

## Administration
To manage namespaces, authenticate with the password (`requirepass`). Namespace management is disabled when `requirepass` contains zero bytes.

### Commands

Except for `NAMESPACE CURRENT`, only connections authenticated with the `requirepass` password (the administrator user) can execute these commands.

#### NAMESPACE ADD <namespace_name>
- Creates a namespace.
- Returns the Token.
- Example response: `"bXluZXduYW1lc3BhY2V0b2"`

#### NAMESPACE GET <namespace_name>
- Returns the Token of the namespace.
- Example response: `"bXluZXduYW1lc3BhY2V0b2"`

#### NAMESPACE GET *
- Returns a list of namespaces and tokens.
- Example response:
  ```
  1) "tenant1"
  2) "bXluZXduYW1lc3BhY2V0b2"
  3) "tenant2"
  4) "YW5vdGhlcm5hbWVzcGFjZXRv"
  ```

#### NAMESPACE REFRESH <namespace_name>
- Replaces the token of the namespace.
- Returns the Token.
- Example response: `"YW5vdGhlcm5hbWVzcGFjZXRv"`

#### NAMESPACE DEL <namespace_name>
- Deletes the namespace.
- Example response: `"OK"`

#### NAMESPACE CURRENT
- Returns the namespace name of the connection.
- Example response: `"tenant1"`
