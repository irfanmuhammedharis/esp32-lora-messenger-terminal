/*
 * Copyright (c) hexcodeplus 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/uart.h>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_USB_DEVICE_STACK
#include <zephyr/usb/usb_device.h>
#endif

#define DEFAULT_RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(DEFAULT_RADIO_NODE),
	     "No default LoRa radio specified in DT");

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lora_mesh);

/* Unique per board, set via CONFIG_MESH_NODE_ID (see Kconfig) */
#define NODE_ID CONFIG_MESH_NODE_ID

#define MESH_MAGIC	0xA5	/* Rejects foreign traffic on the same channel */
#define MESH_TTL	3	/* Max forwards before a packet is dropped */
/* Also the max length of a line typed on the console for the serial bridge */
#define MAX_PAYLOAD_LEN	32
#define MAX_DATA_LEN	255

/* Send one of our own beacon packets this often */
#define ORIGINATE_INTERVAL_MS	10000
/*
 * How often the main loop comes up for air to service the serial bridge.
 *
 * This is NOT a radio listen window. The radio receives continuously through
 * lora_recv_async() (see rx_start()) and is deaf only during its own
 * transmissions; received frames queue up in rx_q until the loop takes them.
 *
 * It used to be one: a blocking lora_recv() per 250 ms slice. That can never
 * receive anything here - every frame is longer on air than the slice (a
 * 12-byte beacon is ~289 ms at SF10/125 kHz), and on each timeout the driver
 * puts the radio to sleep, discarding the frame in flight. Two nodes running
 * that build transmitted fine and heard each other 0% of the time.
 */
#define POLL_PERIOD_MS		50

/*
 * LBT (listen-before-talk) is our collision avoidance for forwards. A relay
 * transmits the moment it decides to, and lora_send() backs off if the
 * channel is busy (see TX_BUSY_* below) rather than blocking in a deaf sleep
 * beforehand. This keeps the node listening right up to the instant it talks,
 * which matters for a star-mesh where one hub relays for several leaves.
 */
/* LBT: how many times to retry a send that found the channel busy */
#define TX_BUSY_RETRIES		5
#define TX_BUSY_BACKOFF_MS	200

/* How many (src, seq) pairs we remember to suppress duplicates */
#define SEEN_CACHE_LEN	32

struct mesh_hdr {
	uint8_t magic;
	uint8_t src;	/* Device ID of the originator, never rewritten */
	uint8_t ttl;
	uint16_t seq;	/* Per-originator message number */
} __packed;

struct seen_entry {
	uint8_t src;
	uint16_t seq;
};

static struct seen_entry seen[SEEN_CACHE_LEN];
static uint8_t seen_next;

static const struct device *const lora_dev = DEVICE_DT_GET(DEFAULT_RADIO_NODE);
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static struct lora_modem_config config;
static bool radio_is_tx;
static bool rx_on;

/*
 * A received frame, handed from the radio's RX callback (system workqueue)
 * to the main loop, which does the parsing, printing and forwarding.
 */
struct rx_frame {
	int16_t rssi;
	int8_t snr;
	uint8_t len;
	uint8_t data[MAX_DATA_LEN];
};

K_MSGQ_DEFINE(rx_q, sizeof(struct rx_frame), 4, 4);

/* Sequence number for packets this node originates (beacon and serial) */
static uint16_t tx_seq;

/*
 * Serial bridge input. On the production UART0 console the RX interrupt
 * moves every byte into ser_q, and the main loop parses from there.
 *
 * Polling the UARTE directly (uart_poll_in) is not enough: between polls it
 * holds one byte plus the peripheral's small RX FIFO, and the main loop only
 * polls every POLL_PERIOD_MS - never while a packet is on air. The ESP32
 * sends a whole "+SEND,..." line in one burst, so each line was cut to its
 * first few characters, lost its '\n', and nothing ever went on air.
 *
 * The diagnostic build's console is USB CDC ACM, which buffers input itself
 * and whose output stopped when the app installed an IRQ callback on it, so
 * that build keeps polling.
 */
#define SERIAL_RX_IRQ (IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN) && \
	!DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart))

#if SERIAL_RX_IRQ
/* Holds the ESP32's whole TX queue (8 lines, up to 39 bytes each) while a
 * packet is on air and the main loop is not draining it.
 */
K_MSGQ_DEFINE(ser_q, sizeof(uint8_t), 512, 1);

static void serial_isr(const struct device *dev, void *user_data)
{
	uint8_t c;

	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
		return;
	}

	while (uart_fifo_read(dev, &c, 1) == 1) {
		/* On overflow the byte is dropped; the line logic resyncs on '\n' */
		(void)k_msgq_put(&ser_q, &c, K_NO_WAIT);
	}
}
#endif

static bool serial_getc(uint8_t *c)
{
#if SERIAL_RX_IRQ
	return k_msgq_get(&ser_q, c, K_NO_WAIT) == 0;
#else
	return uart_poll_in(uart_dev, c) == 0;
#endif
}

/*
 * Serial bridge line assembly, done in the main loop only, so the line state
 * below needs no locking.
 *
 * Only lines that start with SEND_PREFIX go on air, with the prefix stripped.
 * It was added while the ESP32 drove this wire from its TX0 pin, which also
 * carries its ROM boot log and startup banner - without the prefix, every one
 * of those lines went out as a mesh message. The ESP32 side has since moved
 * off TX0; the prefix stays so nothing but a deliberate send (not line
 * noise, not a loose wire) goes on air.
 */
#define SEND_PREFIX	"+SEND,"
#define SEND_PREFIX_LEN	(sizeof(SEND_PREFIX) - 1)

static uint8_t line_buf[MAX_PAYLOAD_LEN];
static uint8_t line_len;
static uint8_t prefix_len;	/* chars of SEND_PREFIX matched on this line */
static bool line_rejected;	/* line did not start with SEND_PREFIX */

/*
 * Returns true if (src, seq) was already handled. Otherwise records it and
 * returns false, so each distinct message is forwarded exactly once.
 */
static bool seen_check_and_add(uint8_t src, uint16_t seq)
{
	for (int i = 0; i < SEEN_CACHE_LEN; i++) {
		if (seen[i].src == src && seen[i].seq == seq) {
			return true;
		}
	}

	seen[seen_next].src = src;
	seen[seen_next].seq = seq;
	seen_next = (seen_next + 1) % SEEN_CACHE_LEN;

	return false;
}

/* The radio is half duplex, so the direction has to be reconfigured. */
static int radio_set_tx(bool tx)
{
	int ret;

	if (radio_is_tx == tx) {
		return 0;
	}

	config.tx = tx;
	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("LoRa config failed (%d)", ret);
		return ret;
	}

	radio_is_tx = tx;
	return 0;
}

/* Runs on the system workqueue for every frame received; the radio is
 * already back in receive when it is called.
 */
static void rx_cb(const struct device *dev, uint8_t *data, uint16_t size,
		  int16_t rssi, int8_t snr, void *user_data)
{
	/* Callbacks are serialised, so one static frame keeps this off the stack */
	static struct rx_frame f;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	f.len = MIN(size, sizeof(f.data));
	memcpy(f.data, data, f.len);
	f.rssi = rssi;
	f.snr = snr;

	if (k_msgq_put(&rx_q, &f, K_NO_WAIT) != 0) {
		LOG_WRN("RX queue full, frame dropped");
	}
}

/* Receive continuously until rx_stop(); frames arrive through rx_cb(). */
static int rx_start(void)
{
	int ret;

	ret = radio_set_tx(false);
	if (ret < 0) {
		return ret;
	}

	ret = lora_recv_async(lora_dev, rx_cb, NULL);
	if (ret < 0) {
		LOG_ERR("LoRa receive start failed (%d)", ret);
		return ret;
	}

	rx_on = true;
	return 0;
}

static void rx_stop(void)
{
	/* The radio must be idle before it can be reconfigured for TX */
	(void)lora_recv_async(lora_dev, NULL, NULL);
	rx_on = false;
}

static int mesh_tx(uint8_t src, uint16_t seq, uint8_t ttl,
		   const uint8_t *payload, uint8_t payload_len)
{
	uint8_t frame[sizeof(struct mesh_hdr) + MAX_PAYLOAD_LEN];
	struct mesh_hdr *hdr = (struct mesh_hdr *)frame;
	int ret;

	if (payload_len > MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}

	hdr->magic = MESH_MAGIC;
	hdr->src = src;
	hdr->ttl = ttl;
	hdr->seq = seq;
	memcpy(frame + sizeof(*hdr), payload, payload_len);

	rx_stop();

	ret = radio_set_tx(true);
	if (ret < 0) {
		(void)rx_start();
		return ret;
	}

	/* CAD is enabled, so a send can bounce off a busy channel. */
	for (int i = 0; i <= TX_BUSY_RETRIES; i++) {
		ret = lora_send(lora_dev, frame, sizeof(*hdr) + payload_len);
		if (ret != -EBUSY) {
			break;
		}
		LOG_DBG("Channel busy, retry %d", i + 1);
		k_sleep(K_MSEC(TX_BUSY_BACKOFF_MS));
	}

	/* Straight back to listening; the main loop retries if this fails */
	(void)rx_start();

	return ret;
}

static void handle_rx(uint8_t *frame, uint16_t len, int16_t rssi, int8_t snr)
{
	struct mesh_hdr *hdr = (struct mesh_hdr *)frame;
	uint8_t *payload = frame + sizeof(*hdr);
	uint8_t payload_len = len - sizeof(*hdr);

	if (len < sizeof(*hdr) || hdr->magic != MESH_MAGIC) {
		LOG_DBG("Dropping non-mesh frame (%u bytes)", len);
		return;
	}

	if (hdr->src == NODE_ID) {
		/* Our own packet came back to us */
		return;
	}

	if (seen_check_and_add(hdr->src, hdr->seq)) {
		LOG_INF("Duplicate from node %u seq %u, not forwarding",
			hdr->src, hdr->seq);
		return;
	}

	/* Print the payload as readable text so the console is a two-way link */
	char msg[MAX_PAYLOAD_LEN + 1];
	size_t n = MIN(payload_len, MAX_PAYLOAD_LEN);

	memcpy(msg, payload, n);
	msg[n] = '\0';

	LOG_INF("RX from node %u seq %u ttl %u (RSSI %d dBm, SNR %d dB): %s",
		hdr->src, hdr->seq, hdr->ttl, rssi, snr, msg);

	/*
	 * Machine-readable twin of the line above, for the ESP32 front-end.
	 * It survives log reformatting, colour and level changes; the front-end
	 * ignores everything that does not start with '+'.
	 */
	printk("+RX,%u,%u,%d,%d,%s\n", hdr->src, hdr->seq, rssi, snr, msg);

	if (hdr->ttl <= 1) {
		LOG_INF("TTL expired, not forwarding");
		return;
	}

	/* Forward immediately; LBT in mesh_tx() avoids talking over the channel */
	if (mesh_tx(hdr->src, hdr->seq, hdr->ttl - 1, payload, payload_len) < 0) {
		LOG_ERR("Forward failed");
		return;
	}

	LOG_INF("Forwarded node %u seq %u", hdr->src, hdr->seq);
}

/* Originate a packet of our own onto the mesh (beacon or serial-sourced). */
static void originate(const uint8_t *payload, uint8_t payload_len)
{
	char text[MAX_PAYLOAD_LEN + 1];
	size_t n = MIN(payload_len, MAX_PAYLOAD_LEN);

	/* Null-terminated copy so the sender can echo what it put on air */
	memcpy(text, payload, n);
	text[n] = '\0';

	/* Mark it seen first so our own forwarded echo isn't re-transmitted */
	seen_check_and_add(NODE_ID, tx_seq);

	if (mesh_tx(NODE_ID, tx_seq, MESH_TTL, payload, payload_len) < 0) {
		LOG_ERR("LoRa send failed");
	} else {
		LOG_INF("TX own seq %u: %s", tx_seq, text);
		/* Confirms TRANSMISSION only - the mesh is fire-and-forget. */
		printk("+TX,%u,%s\n", tx_seq, text);
	}

	tx_seq++;
}

/*
 * Drain any characters received on the console (non-blocking) and, on a
 * newline, originate the assembled line over LoRa if it was a SEND_PREFIX
 * command. Called from the main loop.
 */
static void serial_poll(void)
{
	uint8_t c;

	while (serial_getc(&c)) {
		if (c == '\r' || c == '\n') {
			if (line_rejected) {
				LOG_WRN("Ignored serial line without " SEND_PREFIX
					" prefix");
			} else if (line_len > 0) {
				originate(line_buf, line_len);
			}
			line_len = 0;
			prefix_len = 0;
			line_rejected = false;
		} else if (line_rejected) {
			/* Not a send command - discard until the next line */
		} else if (prefix_len < SEND_PREFIX_LEN) {
			if (c == SEND_PREFIX[prefix_len]) {
				prefix_len++;
			} else {
				line_rejected = true;
			}
		} else if (line_len < sizeof(line_buf)) {
			line_buf[line_len++] = c;
		}
		/* Chars past MAX_PAYLOAD_LEN are dropped until the next line */
	}
}

int main(void)
{
	/* Static: main's stack is 1 KB and a frame is 260 bytes */
	static struct rx_frame f;
	int64_t next_originate;
	int ret;

#ifdef CONFIG_USB_DEVICE_STACK
	/* Console runs over USB CDC ACM on this board */
	(void)usb_enable(NULL);
#endif

	/*
	 * Keep complaining rather than returning: a node wedged silently on a
	 * transient init failure is indistinguishable from a radio fault, and
	 * there is no console attached at boot to catch a one-shot message.
	 */
	while (!device_is_ready(lora_dev)) {
		LOG_ERR("%s not ready", lora_dev->name);
		k_sleep(K_SECONDS(2));
	}

	config.frequency = 865100000;
	config.bandwidth = BW_125_KHZ;
	config.datarate = SF_10;
	config.preamble_len = 8;
	config.coding_rate = CR_4_5;
	config.iq_inverted = false;
	config.public_network = false;
	config.tx_power = 14;
	config.tx = false;
	/* Listen before talk: lora_send() returns -EBUSY on a busy channel */
	config.cad.mode = LORA_CAD_MODE_LBT;

	while ((ret = lora_config(lora_dev, &config)) < 0) {
		LOG_ERR("LoRa config failed (%d), retrying", ret);
		k_sleep(K_SECONDS(2));
	}
	radio_is_tx = false;

	if (!device_is_ready(uart_dev)) {
		LOG_WRN("%s not ready, serial bridge disabled", uart_dev->name);
	}
#if SERIAL_RX_IRQ
	else {
		uart_irq_callback_user_data_set(uart_dev, serial_isr, NULL);
		uart_irq_rx_enable(uart_dev);
	}
#endif

	LOG_INF("Mesh node %u started, TTL %u", NODE_ID, MESH_TTL);

	next_originate = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();
		int64_t wait_ms;

		/* A failed restart after TX would otherwise leave us deaf for good */
		if (!rx_on && rx_start() < 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		/* Serial bridge: send any line typed on the console */
		serial_poll();

		if (now >= next_originate) {
			char payload[MAX_PAYLOAD_LEN];
			int n;

			n = snprintk(payload, sizeof(payload), "hello %u", tx_seq);
			originate(payload, MIN((size_t)n, sizeof(payload)));

			next_originate = k_uptime_get() + ORIGINATE_INTERVAL_MS;
			continue;
		}

		/*
		 * Wait for a received frame until the next beacon is due, but come
		 * up for air every POLL_PERIOD_MS so a typed line goes out promptly.
		 * The radio keeps listening throughout.
		 */
		wait_ms = MIN(next_originate - now, (int64_t)POLL_PERIOD_MS);
		if (k_msgq_get(&rx_q, &f, K_MSEC(wait_ms)) == 0) {
			handle_rx(f.data, f.len, f.rssi, f.snr);
		}
	}

	return 0;
}
