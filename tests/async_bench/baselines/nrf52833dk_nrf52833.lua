-- cycles/op on nrf52833dk/nrf52833, from scripts/bench_table.py --emit-baseline.
-- tests/async_bench fails a case slower than this + CONFIG_BENCH_MARGIN_PCT.
return {
	["yield/2"] = { inc = 1301, gen = 1294 },
	["yield/8"] = { inc = 1272, gen = 1275 },
	["yield/32"] = { inc = 1339, gen = 1365 },
	["sleep/16"] = { inc = 3482, gen = 3599 },
	["await_fanout/1"] = { inc = 12487, gen = 13504 },
	["await_fanout/4"] = { inc = 10566, gen = 10301 },
	["await_fanout/8"] = { inc = 10659, gen = 9864 },
	["await_multi/4"] = { inc = 13953, gen = 13740 },
	["pub_async"] = { inc = 8292, gen = 8753 },
	["read_async"] = { inc = 10194, gen = 9371 },
	["spawn_join"] = { inc = 25394, gen = 20753 },
}
