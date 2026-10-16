# Lua for Zephyr RTOS

> **Alpha** — API may change. Feedback welcome!

A [Zephyr RTOS](https://zephyrproject.org/) module that integrates
[Lua 5.5.0](https://www.lua.org/) as a first-class scripting engine for
embedded systems. Lua scripts run in **dedicated threads** with **isolated
heaps**; inter-thread communication happens through **zbus channels**, keeping
scripting sandboxed and deterministic.

![Architecture](https://github.com/user-attachments/assets/c6bc2fc3-6ba3-45a1-98c0-98e4dd28f1bf)

Each Lua thread gets its own **sys_heap**, **stack**, and **allocator** — fully
isolated from every other thread. Scripts talk to the rest of the firmware
exclusively through **zbus**, so adding or removing a Lua script never
destabilises the system.

## Features

**Script loading** — three ways to get Lua code onto a target:

- **Embedded source** — compiled into the firmware image as C strings
- **Precompiled bytecode** — strip the parser at build time, saving ~15-20 KB flash
- **Filesystem** — load scripts from LittleFS (or any Zephyr FS) at runtime

**System integration:**

- **zbus bindings** — publish, read, and subscribe to channels directly from Lua
- **Message descriptors** — automatic Lua table ↔ C struct conversion (manual or nanopb-generated)
- **Kernel API** — `msleep`, `printk`, structured logging (`log_inf`, `log_wrn`, `log_dbg`, `log_err`)
- **Interactive REPL** — Lua shell over the Zephyr console
- **Selective library loading** — enable only the standard Lua libraries you need via Kconfig

## Table of Contents

- [Quick Start](#quick-start)
- [Samples](#samples)
- [How It Works](#how-it-works)
- [CMake API](#cmake-api)
- [Lua API](#lua-api)
- [Message Descriptors](#message-descriptors)
- [Configuration](#configuration)
- [License](#license)

## Quick Start

### Prerequisites

- A [Zephyr workspace](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) (west, SDK, toolchain)
- [`just`](https://github.com/casey/just) command runner (optional but recommended)

### Optional: create a local workspace

Instead of using an existing Zephyr workspace, run this from the repository root
with `west` installed and available in your active Python environment:

```sh
just workspace
```

Without `just`, the equivalent commands are:

```sh
mkdir -p ws/manifest
ln -sfn ../../west.yml ws/manifest/west.yml
if [ ! -d ws/.west ]; then west init -l ws/manifest; fi
(cd ws && west update --narrow --fetch-opt=--depth=1)
```

This creates a Git-ignored `ws/` workspace containing Zephyr **v4.4.2** and only
three imported modules: `cmsis_6` for the default `mps2/an385` Cortex-M board,
`littlefs` for the filesystem sample, and `nanopb` for the producer/consumer
sample. Other boards or features may need additional modules in `west.yml`'s
allowlist. Downloads use shallow Git history.

The tracked `west.yml` is symlinked from `ws/manifest/`, so there is only one
manifest to edit. This keeps `.west/` inside `ws/`: `west init -l` creates it
next to the manifest directory, not inside it. Run `just workspace` again
after changing the manifest to update the dependencies.

Install Python dependencies through west in your active virtual environment.
This module's Python helpers use only the standard library, so no additional
requirements are needed. Zephyr and nanopb declare their own requirements,
which `west packages pip` discovers automatically. From the repository root:

```sh
(cd ws && west packages pip --install)
```

Run west commands inside `ws/` (or a subdirectory) so west can discover the
workspace and Zephyr without environment-variable overrides. The sample
applications register this Lua module in their `CMakeLists.txt`.

A compatible Zephyr SDK/toolchain is still required; the manifest does not
install it. This repository is outside the nested workspace and is not a
west-managed project, so west's project-level `submodules: true` option cannot
initialize its Lua submodule. Initialize it if needed:

```sh
git submodule update --init
```

For example, build and run a sample from inside the workspace:

```sh
cd ws
west build -b mps2/an385 ../samples/hello_world
west build -t run
```

This uses `ws/build/` for build output; the sample sources stay outside the
workspace.

### 1. Add the module

Clone (or add as a west module) next to your Zephyr workspace, then point your
application's `CMakeLists.txt` at it:

```cmake
cmake_minimum_required(VERSION 3.20.0)
set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../")

# Include luaz.cmake and define threads BEFORE find_package(Zephyr)
include(${ZEPHYR_EXTRA_MODULES}/luaz.cmake)
luaz_define_source_thread(src/my_script.lua)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)

# Embed a Lua script as a C string header (post-Zephyr)
luaz_add_file("src/helper.lua")

# Generate all defined threads
luaz_generate_threads()

target_sources(app PRIVATE src/main.c)
```

### 2. Write a Lua script

```lua
-- src/my_script.lua
local z = require("zephyr")

z.printk("Hello from Lua!")
z.msleep(1000)
z.printk("Done.")
```

### 3. Use zbus from Lua (optional)

Channels and observers defined in C are declared directly from Lua — no C
setup hook required:

```lua
-- src/my_script.lua
local zephyr = require("zephyr")
local zbus = zephyr.zbus

local my_channel = zbus.channel_declare("my_channel")
local my_observer = zbus.observer_declare("my_observer")

-- Publish a table to a channel
my_channel:pub({ x = 1, y = 2 }, 200)

-- Read the current value
local err, msg = my_channel:read(200)

-- Wait for a message on an observer
err, _, msg = my_observer:wait_msg(500)
```

> **Note:** `luaz_openlibs()` is called automatically by generated threads,
> which registers `require()` and preloads all Kconfig-enabled libraries.

### 4. Enable Lua and build

```
# prj.conf
CONFIG_LUA=y
```

```sh
just build          # west build -b mps2/an385 app
just run            # west build -t run
```

## Samples

Every sample includes a `sample.yaml` for automated testing with
[twister](https://docs.zephyrproject.org/latest/develop/test/twister.html).

| Sample                                                 | Description                              | Key Features                                                |
| ------------------------------------------------------ | ---------------------------------------- | ----------------------------------------------------------- |
| [`hello_world`](samples/hello_world)                   | Basic Lua thread + embedded script       | `luaz_define_source_thread`, `luaz_add_file`, REPL, logging |
| [`hello_world_bytecode`](samples/hello_world_bytecode) | Bytecode-only variant                    | `luaz_define_bytecode_thread`, parser stripped              |
| [`producer_consumer`](samples/producer_consumer)       | zbus pub/sub between Lua and C           | nanopb descriptors, nested structs, bytecode                |
| [`littlefs`](samples/littlefs)                         | Scripts loaded from LittleFS at runtime  | `luaz_define_fs_thread`, `luaz_add_fs_file`, `zephyr.fs`    |
| [`heavy`](samples/heavy)                               | Stress test with dynamic code generation | Heap usage tracking, recursion, string ops                  |

```sh
# Run a single sample
just build project_dir=samples/hello_world
just run

# Run the full test suite
just test           # west twister -p mps2/an385 -T samples
```

## How It Works

### Thread model

Each generated thread is a self-contained Zephyr thread with:

- A dedicated **sys_heap** (`CONFIG_LUA_THREAD_HEAP_SIZE`, default 32 KB)
- A dedicated **thread stack** (`CONFIG_LUA_THREAD_STACK_SIZE`, default 2 KB)
- A custom **Lua allocator** backed by that heap
- **`luaz_openlibs()`** called automatically — registers `require()` and preloads all Kconfig-enabled libraries
- A weak **setup hook** (`<script>_lua_setup`) for registering zbus channels/observers

Per-thread Kconfig overrides are generated automatically:
`CONFIG_<SCRIPT>_LUA_THREAD_STACK_SIZE`, `_HEAP_SIZE`, and `_PRIORITY` default
to the global values but can be tuned individually.

There are three thread flavours:

| Variant        | CMake macro                   | Script location                      |
| -------------- | ----------------------------- | ------------------------------------ |
| **Source**     | `luaz_define_source_thread`   | Embedded as C string, parsed at boot |
| **Bytecode**   | `luaz_define_bytecode_thread` | Precompiled, parser can be stripped  |
| **Filesystem** | `luaz_define_fs_thread`       | Loaded from FS path at runtime       |

#### Source vs bytecode: memory comparison

Measured on the [`heavy`](samples/heavy) sample (mps2/an385, 32 KB heap, 4 KB stack):

| Resource   |  Source Thread | Bytecode Thread |            Saving |
| ---------- | -------------: | --------------: | ----------------: |
| Heap peak  | 23 856 B (72%) |  22 168 B (67%) |  -1 688 B (-7.1%) |
| Stack peak |  2 744 B (66%) |   1 080 B (26%) | -1 664 B (-60.6%) |

Bytecode threads skip the parser's recursive-descent call chain at runtime,
which accounts for the large stack reduction.

### zbus integration

Scripts interact with the rest of the system exclusively through
[zbus](https://docs.zephyrproject.org/latest/services/zbus/index.html)
channels. Conversion between Lua tables and C structs is handled automatically
via **message descriptors** stored in `zbus_chan_user_data()` — see
[Message Descriptors](#message-descriptors) below.

### Module structure

| Path         | Contents                                                                    |
| ------------ | --------------------------------------------------------------------------- |
| `lua/`       | Lua 5.5.0 core (git submodule — **do not modify**)                          |
| `src/`       | Zephyr integration: allocator, kernel bindings, zbus, REPL, FS, descriptors |
| `include/`   | Public headers (`luaz_utils.h`, `luaz_zbus.h`, `luaz_fs.h`, …)              |
| `templates/` | `.c.in` / `.h.in` files used by the CMake code-gen functions                |
| `scripts/`   | Python helpers (`luaz_gen.py`)                                              |
| `samples/`   | Ready-to-build example applications                                         |

---

## CMake API

All functions are provided by `luaz.cmake`. This file is included **twice**
during a build: once by the application (pre-Zephyr) and once by the module
(post-Zephyr).

### Pre-Zephyr: thread definitions

Call these **before** `find_package(Zephyr)` to define threads and generate
per-thread Kconfig fragments:

| Macro                               | Description                                             |
| ----------------------------------- | ------------------------------------------------------- |
| `luaz_define_source_thread(path)`   | Define a source-embedded Lua thread                     |
| `luaz_define_bytecode_thread(path)` | Define a bytecode Lua thread (`CONFIG_LUA_PRECOMPILE`)  |
| `luaz_define_fs_thread(fs_path)`    | Define a filesystem-backed Lua thread (`CONFIG_LUA_FS`) |

### Post-Zephyr: code generation

Call these **after** `find_package(Zephyr)`:

| Function                       | Description                                                             |
| ------------------------------ | ----------------------------------------------------------------------- |
| `luaz_generate_threads()`      | Generate all threads defined by `luaz_define_*_thread()` above          |
| `luaz_add_file(path)`          | Embed a `.lua` file as a C `const char[]` header                        |
| `luaz_add_bytecode_file(path)` | Embed precompiled bytecode as a C `uint8_t[]` header                    |
| `luaz_add_fs_file(src [name])` | Register a Lua file for embedding and writing to the filesystem at boot |

## Lua API

### `zephyr` library

Loaded with `require("zephyr")`. Automatically preloaded by `luaz_openlibs()`.

| Function              | Description                 |
| --------------------- | --------------------------- |
| `zephyr.msleep(ms)`   | Sleep for `ms` milliseconds |
| `zephyr.printk(msg)`  | Kernel print                |
| `zephyr.log_inf(msg)` | Log at INFO level           |
| `zephyr.log_wrn(msg)` | Log at WARNING level        |
| `zephyr.log_dbg(msg)` | Log at DEBUG level          |
| `zephyr.log_err(msg)` | Log at ERROR level          |

### `zephyr.zbus` — zbus bindings

Nested inside the `zephyr` table when `CONFIG_LUA_LIB_ZBUS=y`. Channels and
observers defined in C are declared from Lua with `channel_declare` /
`observer_declare`:

| Function / Method             | Description                                               |
| ----------------------------- | --------------------------------------------------------- |
| `zbus.channel_declare(name)`  | Get a channel userdata by name                            |
| `zbus.observer_declare(name)` | Get an observer userdata by name                          |
| `chan:pub(table, timeout_ms)` | Publish a Lua table to a zbus channel                     |
| `chan:read(timeout_ms)`       | Read the current channel value as a Lua table             |
| `obs:wait_msg(timeout_ms)`    | Block until a message arrives; returns `err, chan, table` |

### `zephyr.fs` — filesystem bindings

Nested inside the `zephyr` table when `CONFIG_LUA_FS=y`.

Loading this library also replaces the global `dofile` and `loadfile` with
filesystem-backed versions, so scripts can use them transparently.

| Function                   | Description                                          |
| -------------------------- | ---------------------------------------------------- |
| `zephyr.fs.dofile(path)`   | Load and execute a Lua script from the filesystem    |
| `zephyr.fs.loadfile(path)` | Load a script without executing (returns a function) |
| `zephyr.fs.list([path])`   | List files in a directory                            |

### Standard Lua libraries

Individual standard libraries are controlled via Kconfig (see
[Configuration](#configuration)). When enabled, they are preloaded and
available via `require()`:

```lua
local string = require("string")
local math   = require("math")
```

## Message Descriptors

The descriptor system provides **automatic Lua table ↔ C struct conversion**
for zbus messages. Descriptors are stored as zbus channel `user_data` for O(1)
lookup — the conversion functions in `luaz_zbus.c` use them automatically.

### Manual descriptors

Define field arrays and pass them inline to `ZBUS_CHAN_DEFINE`:

```c
#include <luaz_msg_descr.h>

struct sensor_data {
        int32_t x, y, z;
};

static const struct lua_msg_field_descr sensor_fields[] = {
        LUA_MSG_FIELD(struct sensor_data, x, LUA_MSG_TYPE_INT),
        LUA_MSG_FIELD(struct sensor_data, y, LUA_MSG_TYPE_INT),
        LUA_MSG_FIELD(struct sensor_data, z, LUA_MSG_TYPE_INT),
};

ZBUS_CHAN_DEFINE(chan_sensor, struct sensor_data, NULL,
                LUA_ZBUS_MSG_DESCR(struct sensor_data, sensor_fields),
                ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
```

For nested structs use `LUA_MSG_FIELD_OBJECT`:

```c
LUA_MSG_FIELD_OBJECT(struct parent, child_field, child_fields),
```

Supported field types: `LUA_MSG_TYPE_INT`, `LUA_MSG_TYPE_UINT`,
`LUA_MSG_TYPE_NUMBER`, `LUA_MSG_TYPE_STRING`, `LUA_MSG_TYPE_STRING_BUF`,
`LUA_MSG_TYPE_BOOL`, `LUA_MSG_TYPE_OBJECT`.

### nanopb descriptor bridge

When using [nanopb](https://jpa.kapsi.fi/nanopb/), `luaz_msg_descr_pb.h`
auto-generates descriptors from nanopb `FIELDLIST` X-macros — the `.proto`
file becomes the single source of truth:

```c
#include <luaz_msg_descr_pb.h>
#include "channels.pb.h"

/* Child descriptors must be defined before parents (leaf-first) */
LUA_PB_DESCR_DEFINE(msg_acc_data);
ZBUS_CHAN_DEFINE(chan_acc_data, struct msg_acc_data, NULL,
                LUA_PB_DESCR_REF(msg_acc_data),
                ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(.x = 0));
```

Nested MESSAGE fields are resolved automatically via nanopb's
`<parent_t>_<field>_MSGTYPE` macros. See the [`producer_consumer`](samples/producer_consumer)
sample for a complete example.

## Configuration

All options live under `Kconfig.luaz`.

| Option                           | Default  | Description                                                          |
| -------------------------------- | -------- | -------------------------------------------------------------------- |
| `CONFIG_LUA`                     | —        | Enable Lua support (selects zbus)                                    |
| `CONFIG_LUA_REPL`                | `n`      | Enable interactive Lua shell (selects Zephyr shell)                  |
| `CONFIG_LUA_REPL_LINE_SIZE`      | `256`    | Maximum REPL input line length                                       |
| `CONFIG_LUA_THREAD_STACK_SIZE`   | `2048`   | Default stack size (bytes) for generated Lua threads                 |
| `CONFIG_LUA_THREAD_HEAP_SIZE`    | `32768`  | Default heap size (bytes) for generated Lua threads                  |
| `CONFIG_LUA_THREAD_PRIORITY`     | `7`      | Default cooperative priority of generated Lua threads                |
| `CONFIG_LUA_LIBS_ALL`            | `n`      | Preload all standard Lua libraries + zbus                            |
| `CONFIG_LUA_LIB_STRING`          | if ALL   | Lua string library                                                   |
| `CONFIG_LUA_LIB_TABLE`           | if ALL   | Lua table library                                                    |
| `CONFIG_LUA_LIB_MATH`            | if ALL   | Lua math library                                                     |
| `CONFIG_LUA_LIB_COROUTINE`       | if ALL   | Lua coroutine library                                                |
| `CONFIG_LUA_LIB_UTF8`            | if ALL   | Lua utf8 library                                                     |
| `CONFIG_LUA_LIB_DEBUG`           | if ALL   | Lua debug library                                                    |
| `CONFIG_LUA_LIB_ZBUS`            | if ALL   | Include zbus bindings as `zephyr.zbus` subtable                      |
| `CONFIG_LUA_PRECOMPILE`          | `n`      | Precompile Lua scripts to bytecode at build time                     |
| `CONFIG_LUA_PRECOMPILE_ONLY`     | `n`      | Exclude the Lua parser from the target (saves ~15-20 KB)             |
| `CONFIG_LUA_EXTRA_OPTIMIZATIONS` | `n`      | Reduce internal data structure sizes (experimental, bytecode-only)   |
| `CONFIG_LUA_FS`                  | `n`      | Enable filesystem support for Lua scripts                            |
| `CONFIG_LUA_FS_MOUNT_POINT`      | `"/lfs"` | Filesystem mount point prefix                                        |
| `CONFIG_LUA_FS_MAX_FILE_SIZE`    | `4096`   | Maximum Lua script file size (bytes)                                 |
| `CONFIG_LUA_FS_SHELL`            | `n`      | Enable `lua_fs` shell commands (list, cat, write, delete, run, stat) |

Per-thread overrides: each `luaz_define_*_thread()` generates
`CONFIG_<SCRIPT>_LUA_THREAD_STACK_SIZE`, `_HEAP_SIZE`, and `_PRIORITY` options
that default to the global values above.

## License

Copyright 2026 Rodrigo Peixoto and Victor Miranda.

Licensed under the [Apache License, Version 2.0](LICENSE).
