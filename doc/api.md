# API

Two public headers, both fully documented with Doxygen comments:

- `include/dockmaster/dm.h`: producers and policy. `dm_submit()`,
  `dm_alloc()` / `dm_submit_block()` for zero copy, `dm_policy_apply()`,
  `dm_policy_revert()`, `dm_hint_register()`.
- `include/dockmaster/dm_link.h`: link drivers. `struct dm_link_ops`,
  `struct dm_link_caps`, `dm_link_register()` and the ISR-safe
  `dm_link_tx_done()`, `dm_link_probe_done()`, `dm_link_state()`.

Counters are in `include/dockmaster/dm_stats.h`, the tier-0 backend in
`include/dockmaster/dm_hw.h` and the test link in
`include/dockmaster/dm_link_fake.h`. Everything else is private under
`subsys/dockmaster/`.

The HTML reference is generated with `doxygen doc/Doxyfile`; CI runs it with
warnings as errors, so an undocumented public symbol fails the build.

## Producer example

```c
#include <dockmaster/dm.h>

void report_temperature(int16_t centi_c)
{
	struct dm_submit_opts o = {
		.coalesce_key = SENSOR_TEMP, /* newest value wins */
		.link_hint = DM_LINK_ANY,
	};

	/* Never blocks; a refusal is a result code, not a wait. */
	(void)dm_submit(DM_C1, &centi_c, sizeof(centi_c), DM_F_COALESCE, &o);
}
```

## Driver example

```c
static const struct dm_link_ops ops = {
	.tx = my_tx,                 /* start, return at once */
	.cancel = my_cancel,
	.cost_of_us = my_cost_of_us, /* pure airtime model */
	.probe = my_probe,
	.is_busy = my_cca,
};

static int my_init(const struct device *dev)
{
	const struct dm_link_caps caps = {
		.mtu = 127, .tier = DM_TIER_SW, .probe_period_ms = 30000,
	};

	return dm_link_register(dev, &ops, &caps, &my_link_index);
}

/* In the radio's done ISR: */
dm_link_tx_done(my_link_index, handle, status, 0 /* let Dockmaster time it */);
```
