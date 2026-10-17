#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <luaz_msg_descr.h>

struct msg_tick {
	int32_t seq;
};

static const struct lua_msg_field_descr tick_fields[] = {
	LUA_MSG_FIELD(struct msg_tick, seq, LUA_MSG_TYPE_INT),
};

ZBUS_CHAN_DEFINE(chan_tick, struct msg_tick, NULL, LUA_ZBUS_MSG_DESCR(struct msg_tick, tick_fields),
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Never published: exercises await timeouts. */
ZBUS_CHAN_DEFINE(chan_idle, struct msg_tick, NULL, LUA_ZBUS_MSG_DESCR(struct msg_tick, tick_fields),
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* No descriptor: chan:await must refuse it. */
ZBUS_CHAN_DEFINE(chan_raw, struct msg_tick, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

/* Four spaced messages, then a burst of eight. */
static void publisher(void *p1, void *p2, void *p3)
{
	struct msg_tick msg;

	k_msleep(300);
	for (msg.seq = 1; msg.seq <= 12; msg.seq++) {
		zbus_chan_pub(&chan_tick, &msg, K_MSEC(100));
		if (msg.seq <= 4) {
			k_msleep(50);
		}
	}
}

K_THREAD_DEFINE(publisher_id, 1024, publisher, NULL, NULL, NULL, 5, 0, 0);
