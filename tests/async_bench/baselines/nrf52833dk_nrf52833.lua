-- cycles/op on nrf52833dk/nrf52833, from scripts/bench_table.py --emit-baseline.
-- tests/async_bench fails a case slower than this + CONFIG_BENCH_MARGIN_PCT.
return {
	["yield/2"] = { inc = 1298, gen = 1294 },
	["yield/8"] = { inc = 1271, gen = 1273 },
	["yield/32"] = { inc = 1335, gen = 1357 },
	["sleep/16"] = { inc = 3560, gen = 3671 },
	["await_fanout/1"] = { inc = 13140, gen = 14239 },
	["await_fanout/4"] = { inc = 11014, gen = 11006 },
	["await_fanout/8"] = { inc = 10972, gen = 10600 },
	["await_multi/4"] = { inc = 14037, gen = 14320 },
	["pub_async"] = { inc = 8314, gen = 8841 },
	["read_async"] = { inc = 9840, gen = 9426 },
	["spawn_join"] = { inc = 25286, gen = 20852 },
}
