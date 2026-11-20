-- cycles/op on nrf52833dk/nrf52833, from scripts/bench_table.py --emit-baseline.
-- tests/async_bench fails a case slower than this + CONFIG_BENCH_MARGIN_PCT.
return {
	["yield/2"] = { inc = 1289, gen = 1290 },
	["yield/8"] = { inc = 1264, gen = 1265 },
	["yield/32"] = { inc = 1418, gen = 1373 },
	["sleep/16"] = { inc = 3789, gen = 3854 },
	["await_fanout/1"] = { inc = 13530, gen = 13577 },
	["await_fanout/4"] = { inc = 10722, gen = 10364 },
	["await_fanout/8"] = { inc = 10214, gen = 9987 },
	["await_multi/4"] = { inc = 13918, gen = 13711 },
	["pub_async"] = { inc = 8761, gen = 8801 },
	["read_async"] = { inc = 9718, gen = 9408 },
	["spawn_join"] = { inc = 16229, gen = 16508 },
}
