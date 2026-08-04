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
 * Longest single listen before we come up for air to service the serial
 * bridge. The radio sits in receive for the whole slice, so it is deaf only
 * during its own transmissions; a shorter slice just makes a queued serial
 * line go out sooner. This is as close to full-duplex as one radio allows.
 */
#define LISTEN_SLICE_MS		250

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

/* Sequence number for packets this node originates (beacon and serial) */
static uint16_t tx_seq;

/*
 * Serial bridge line assembly. Filled by polling the console UART from the
 * main loop (uart_poll_in), so there is no ISR and no shared-state locking,
 * and nothing touches the console's interrupt/TX path.
 */
static uint8_t line_buf[MAX_PAYLOAD_LEN];
static uint8_t line_len;

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

	ret = radio_set_tx(true);
	if (ret < 0) {
		return ret;
	}

	/* CAD is enabled, so a send can bounce off a busy channel. */
	for (int i = 0; i <= TX_BUSY_RETRIES; i++) {
		ret = lora_send(lora_dev, frame, sizeof(*hdr) + payload_len);
		if (ret != -EBUSY) {
			return ret;
		}
		LOG_DBG("Channel busy, retry %d", i + 1);
		k_sleep(K_MSEC(TX_BUSY_BACKOFF_MS));
	}

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
	}

	tx_seq++;
}

/*
 * Drain any characters typed on the console (non-blocking) and, on a newline,
 * originate the assembled line over LoRa. Polled from the main loop so it
 * never touches the console's interrupt or TX path.
 */
static void serial_poll(void)
{
	uint8_t c;

	while (uart_poll_in(uart_dev, &c) == 0) {
		if (c == '\r' || c == '\n') {
			if (line_len > 0) {
				originate(line_buf, line_len);
				line_len = 0;
			}
		} else if (line_len < sizeof(line_buf)) {
			line_buf[line_len++] = c;
		}
		/* Chars past MAX_PAYLOAD_LEN are dropped until the next line */
	}
}

int main(void)
{
	uint8_t rx_buf[MAX_DATA_LEN];
	int64_t next_originate;
	int16_t rssi;
	int8_t snr;
	int ret, len;

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

	LOG_INF("Mesh node %u started, TTL %u", NODE_ID, MESH_TTL);

	next_originate = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();
		int64_t listen_ms;

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

		ret = radio_set_tx(false);
		if (ret < 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		/*
		 * Listen until the next beacon is due, but never for longer than
		 * one slice, so a line typed on the console is picked up promptly.
		 */
		listen_ms = MIN(next_originate - now, (int64_t)LISTEN_SLICE_MS);
		len = lora_recv(lora_dev, rx_buf, sizeof(rx_buf),
				K_MSEC(listen_ms), &rssi, &snr);
		if (len == -EAGAIN) {
			continue;	/* Slice/beacon timeout, re-evaluate */
		}
		if (len < 0) {
			LOG_ERR("LoRa receive failed (%d)", len);
			k_sleep(K_MSEC(100));
			continue;
		}

		handle_rx(rx_buf, len, rssi, snr);
	}

	return 0;
}
