---@meta

--- zbus channel userdata (returned by zbus.channel_declare).
---@class zbus_channel
local zbus_channel = {}

--- Publish a message table to this channel. Blocks the thread for up to timeout;
--- inside zephyr.async tasks use pub_async.
---@param data table # Message table matching the channel descriptor.
---@param timeout integer # zephyr.K_FOREVER, zephyr.K_NO_WAIT or milliseconds.
---@return integer err # 0 on success, negative errno on failure.
function zbus_channel:pub(data, timeout) end

--- Read the current message from this channel. Blocks the thread for up to
--- timeout; inside zephyr.async tasks use read_async.
---@param timeout integer # zephyr.K_FOREVER, zephyr.K_NO_WAIT or milliseconds.
---@return integer err # 0 on success, negative errno on failure.
---@return table|nil data # Message table, or nil on error.
function zbus_channel:read(timeout) end

--- Wait for the next message (requires CONFIG_LUA_ASYNC; async threads only).
--- Parks the calling task, not the thread. Raises if it would park from inside
--- a coroutine.wrap/resume.
---@param timeout? integer # zephyr.K_FOREVER (or nil), zephyr.K_NO_WAIT or milliseconds.
---@return table|nil data # Message table, or nil on timeout.
---@return integer|nil err # -zephyr.EAGAIN on timeout.
function zbus_channel:await(timeout) end

--- Publish without blocking the other tasks: a busy channel lock parks the
--- calling task until it is free (CONFIG_LUA_ASYNC; tasks only).
---@param data table # Message table matching the channel descriptor.
---@param timeout? integer # zephyr.K_FOREVER (default), zephyr.K_NO_WAIT or milliseconds.
---@return true|nil ok
---@return integer|nil err # -EBUSY (K_NO_WAIT) or -EAGAIN: nothing published; other values may mean a partial delivery.
function zbus_channel:pub_async(data, timeout) end

--- Read the current message, waiting for a busy channel lock as a task
--- (CONFIG_LUA_ASYNC; tasks only).
---@param timeout? integer # zephyr.K_FOREVER (default), zephyr.K_NO_WAIT or milliseconds.
---@return table|nil data
---@return integer|nil err # -EBUSY (K_NO_WAIT) or -EAGAIN.
function zbus_channel:read_async(timeout) end

--- Get or set how many recent messages are kept for await (CONFIG_LUA_ASYNC).
---@param depth? integer # New depth, >= 1.
---@return integer depth
function zbus_channel:queue_depth(depth) end

--- Messages the calling task missed on this channel because it fell behind.
---@return integer count
function zbus_channel:dropped() end

--- zbus observer userdata (returned by zbus.observer_declare).
---@class zbus_observer
local zbus_observer = {}

--- Wait for a message on any subscribed channel.
---@param timeout integer # zephyr.K_FOREVER, zephyr.K_NO_WAIT or milliseconds.
---@return integer err # 0 on success, negative errno on failure.
---@return zbus_channel|nil channel # Source channel, or nil on error.
---@return table|nil data # Message table, or nil on error.
function zbus_observer:wait_msg(timeout) end

--- zbus pub/sub namespace (requires CONFIG_LUA_LIB_ZBUS).
---@class zbus
local zbus = {}

--- Declare (look up) a zbus channel by name.
--- Mirrors Zephyr's ZBUS_CHAN_DECLARE — makes the channel available to Lua.
---@param name string # Channel name (e.g. "chan_acc_data").
---@return zbus_channel channel # Channel userdata.
function zbus.channel_declare(name) end

--- Declare (look up) a zbus observer by name.
--- Mirrors Zephyr's ZBUS_OBS_DECLARE — makes the observer available to Lua.
---@param name string # Observer name (e.g. "msub_acc_consumed").
---@return zbus_observer observer # Observer userdata.
function zbus.observer_declare(name) end

--- Filesystem API (requires CONFIG_LUA_FS).
---@class fs
local fs = {}

--- Load and execute a Lua script from the filesystem.
---@param path string # File path (relative to mount point or absolute).
---@return any ... # Values returned by the script.
function fs.dofile(path) end

--- Load a Lua script from the filesystem without executing it.
---@param path string # File path (relative to mount point or absolute).
---@return function|nil chunk # Compiled chunk, or nil on error.
---@return string|nil err # Error message on failure.
function fs.loadfile(path) end

--- List files in a directory.
---@param path? string # Directory path (defaults to mount point).
---@return table entries # Array of {name: string, size: integer, type: string}.
function fs.list(path) end

--- Task handle (returned by async.spawn).
---@class async_task
local async_task = {}

--- Wait for the task to finish. Raises if it would park from inside a
--- coroutine.wrap/resume.
---@param timeout? integer # zephyr.K_FOREVER (or nil), zephyr.K_NO_WAIT or milliseconds.
---@return boolean|nil ok # true: returned, false: failed or cancelled, nil: timeout.
---@return any ... # Return values, the error, -zephyr.ECANCELED or -zephyr.EAGAIN.
function async_task:join(timeout) end

--- Stop the task now; its __close handlers run.
---@return boolean # false if it had already finished.
function async_task:cancel() end

---@return "running"|"ready"|"sleeping"|"waiting"|"joining"|"locking"|"done"|"failed"|"cancelled"
function async_task:status() end

--- Cooperative tasks: local async = require("zephyr.async")
--- (requires CONFIG_LUA_ASYNC and CONFIG_<NAME>_LUA_THREAD_ASYNC=y).
--- sleep, yield, chan:await, task:join, and chan:pub_async/read_async when they
--- wait must be called from the task itself;
--- inside a coroutine.wrap/resume they raise "cannot park inside a coroutine".
---@class async
local async = {}

--- Start fn(...) as a new task. It first runs after the caller yields.
---@param fn function
---@return async_task
function async.spawn(fn, ...) end

--- Park the calling task for at least ms milliseconds (not zephyr.K_FOREVER).
---@param ms integer
function async.sleep(ms) end

--- Let the other ready tasks run.
function async.yield() end

--- Zephyr kernel API bindings.
--- Access via: local zephyr = require("zephyr")
---@class zephyr
---@field zbus zbus # zbus pub/sub namespace (requires CONFIG_LUA_LIB_ZBUS).
---@field fs fs # Filesystem API (requires CONFIG_LUA_FS).
---@field K_NO_WAIT integer # 0: don't wait.
---@field K_FOREVER integer # -1: wait indefinitely (SYS_FOREVER_MS).
---@field EAGAIN integer # errno values; bindings return them negated.
---@field EBUSY integer
---@field ECANCELED integer
---@field EFAULT integer
---@field EILSEQ integer
---@field EINVAL integer
---@field ENOMEM integer
---@field ENOMSG integer
local zephyr = {}

--- Sleep for the specified number of milliseconds. Blocks the whole thread.
---@param ms integer # Duration in milliseconds, >= 0.
function zephyr.msleep(ms) end

--- Print a message via Zephyr printk.
---@param message string # Message to print.
function zephyr.printk(message) end

--- Log a message at INFO level.
---@param message string # Message to log.
function zephyr.log_inf(message) end

--- Log a message at WARNING level.
---@param message string # Message to log.
function zephyr.log_wrn(message) end

--- Log a message at DEBUG level.
---@param message string # Message to log.
function zephyr.log_dbg(message) end

--- Log a message at ERROR level.
---@param message string # Message to log.
function zephyr.log_err(message) end

return zephyr
