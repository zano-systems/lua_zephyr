-- zephyr.async is only available in async threads.
local z = require("zephyr")
local ok = pcall(require, "zephyr.async")
z.printk("plain require ok=" .. tostring(ok))
