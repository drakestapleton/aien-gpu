/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* parity_mq -- the GSP message queue as the module runs it (om/seal.om + om/mqput.om to send,
 * om/mqget.om to receive) against the oracle aien-dev/aienux c9678d7b native/kernel/dev/gb10_gsp_boot.c
 * (gsp_mq_send, gsp_rpc_rm_alloc/control/free, gsp_mq_recv).
 * Send: the whole 256 KiB command queue must be byte-identical after every message, including ring wrap,
 * multi-element messages and params placed past the RPC length (GSP_RM_ALLOC); refusals must agree.
 * Receive: messages the oracle writes as GSP would must be read back identically; corrupted ones must be
 * refused with the oracle's class. Prints PARITY-MQ PASS sends=N recvs=M refusals=K. Host only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omhost.h"
#include "gb10_gsp_boot.h"

#define QSZ GSP_MQ_QUEUE_SIZE
static uint8_t cq_c[QSZ], cq_o[QSZ], sq[QSZ], msg[65536], out_c[65544], out_o[65536];
static uint8_t pay[65536];

static uint32_t rng = 777u;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void init_queue(uint8_t *q)
{
	const uint32_t h[8] = { 0, QSZ, GSP_MQ_MSG_SIZE, GSP_MQ_MSG_COUNT, 0, GSP_MQ_FLAGS_SWAP_RX, GSP_MQ_RXHDR_OFF,
				GSP_MQ_ENTRY_OFF };
	memset(q, 0, QSZ);
	for (int i = 0; i < 8; i++)
		wr32(q + 4 * i, h[i]);
}

/* Omega send, as kmod/aien_gpu_rpc.c does it: payload at 80, seal, put, publish writePtr. */
static uint64_t om_send(uint8_t *cq, const gsp_seg *segs, uint32_t nsegs, uint32_t fn, uint32_t rpclen, uint32_t seq,
			uint32_t txseq, uint32_t gsprd)
{
	uint64_t st, placed = 0, elems, wr;
	for (uint32_t i = 0; i < nsegs; i++) {
		if (80 + placed + segs[i].len > sizeof(msg))
			return 2000000;
		if (segs[i].p)
			memcpy(msg + 80 + placed, segs[i].p, segs[i].len);
		else
			memset(msg + 80 + placed, 0, segs[i].len);
		placed += segs[i].len;
	}
	elems = om_call(agpu_om_seal, P(msg), fn, rpclen, placed, seq, txseq, &st);
	if (st)
		return 3000000 + st;
	if (elems >= 1000000)
		return elems;
	wr = om_call(agpu_om_mqput, P(cq), P(msg), elems, gsprd, 0, 0, &st);
	if (st)
		return 3000000 + st;
	if (wr >= 1000000)
		return wr + 100;
	wr32(cq + 16, (uint32_t)wr);
	return 0;
}

static int sends(unsigned *n, unsigned *refused)
{
	gsp_mq q;
	uint32_t txseq = 0, rpcseq = 0;

	init_queue(cq_c);
	init_queue(cq_o);
	init_queue(sq);
	q.cmdq = cq_c;
	q.statq = sq;
	q.tx_seq = 0;
	q.rx_seq = 0;
	q.rpc_seq = 0;
	for (unsigned t = 0; t < 600; t++) {
		const char *why = 0;
		uint32_t kind = rnd() % 4, len = rnd() % (t % 7 == 0 ? 20000 : 600), fn = 1 + rnd() % 120, seq;
		gsp_seg seg[2];
		int rc;
		uint64_t ro;

		for (uint32_t i = 0; i < len; i++)
			pay[i] = (uint8_t)rnd();
		/* GSP drains: move its read pointer (kept in the status queue, SWAP_RX) to a random place
		 * behind our write pointer, sometimes leaving the queue nearly full. */
		if (rnd() % 3 == 0)
			wr32(sq + GSP_MQ_RXHDR_OFF, rd32(cq_c + 16));
		uint32_t gsprd = rd32(sq + GSP_MQ_RXHDR_OFF);
		if (kind == 0) {
			/* GSP_RM_ALLOC: rpc_gsp_rm_alloc_v03_00 (hClient 0, hParent 4, hObject 8, hClass 12, status 16,
			 * paramsSize 20, flags 24, reserved 28), params after it, outside the RPC length. */
			uint32_t h[4] = { rnd(), rnd(), rnd(), rnd() };
			uint8_t fixed[GSP_RM_ALLOC_FIXED] = { 0 };
			for (int i = 0; i < 4; i++)
				wr32(fixed + 4 * i, h[i]);
			wr32(fixed + 20, len);
			rc = gsp_rpc_rm_alloc(&q, h[0], h[1], h[2], h[3], pay, len, &seq, &why);
			gsp_seg s2[2] = { { fixed, sizeof(fixed) }, { pay, len } };
			ro = om_send(cq_o, s2, 2, GSP_FN_GSP_RM_ALLOC, GSP_RM_ALLOC_FIXED, rpcseq, txseq, gsprd);
		} else {
			seg[0].p = pay;
			seg[0].len = len;
			rc = gsp_mq_send(&q, fn, seg, 1, len, kind & 1, &seq, &why);
			ro = om_send(cq_o, seg, 1, fn, len, (kind & 1) ? rpcseq : 0, txseq, gsprd);
		}
		if ((rc == 0) != (ro == 0)) {
			printf("PARITY-MQ FAIL send %u kind %u len %u: oracle %d (%s) omega %llu\n", t, kind, len, rc,
			       why ? why : "-", (unsigned long long)ro);
			return 1;
		}
		if (rc == 0) {
			txseq++;
			if (kind == 0 || (kind & 1))
				rpcseq++;
			(*n)++;
		} else {
			(*refused)++;
		}
		if (memcmp(cq_c, cq_o, QSZ)) {
			for (uint32_t i = 0; i < QSZ; i++)
				if (cq_c[i] != cq_o[i]) {
					printf("PARITY-MQ FAIL send %u kind %u len %u: byte %u C %02x Omega %02x\n", t, kind, len,
					       i, cq_c[i], cq_o[i]);
					break;
				}
			return 1;
		}
	}
	return 0;
}

/* a message refused by both with the same class */
static uint64_t class_of(const char *why)
{
	if (!why) return 0;
	if (!strcmp(why, "status queue header not the driver's shape")) return 1000001;
	if (!strcmp(why, "ring pointer corrupt")) return 1000002;
	if (!strcmp(why, "element count bad")) return 1000003;
	if (!strcmp(why, "rpc length bad")) return 1000004;
	if (!strcmp(why, "checksum not zero")) return 1000005;
	if (!strcmp(why, "bad sequence number")) return 1000006;
	if (!strcmp(why, "rpc signature bad")) return 1000007;
	return 1;
}

/* Re-zero the element checksum after a deliberate header change (so a later check is reached). */
static void fix_cksum(uint8_t *slot0, uint32_t elems)
{
	(void)elems;
	uint32_t mlen = GSP_ELEM_HDR_SIZE + rd32(slot0 + 56);
	if (mlen > 4096)
		return;
	wr32(slot0 + 32, 0);
	uint32_t up = (mlen + 7) & ~7u;
	uint8_t tmp[4096 + 8] = { 0 };
	memcpy(tmp, slot0, mlen);
	wr32(slot0 + 32, gsp_checksum32(tmp, up));
}

static int recvs(unsigned *n, unsigned *refused)
{
	for (unsigned t = 0; t < 400; t++) {
		gsp_mq gsp, q;
		const char *why = 0;
		uint32_t len = rnd() % (t % 5 == 0 ? 30000 : 900), fn_c = 0, res_c = 0;
		unsigned corrupt = t % 10;
		uint32_t start = rnd() % GSP_MQ_MSG_COUNT;

		init_queue(sq);
		init_queue(cq_c);
		wr32(sq + 16, start);			/* GSP's write pointer */
		wr32(cq_c + GSP_MQ_RXHDR_OFF, start);	/* our read pointer (SWAP_RX) */
		gsp.cmdq = sq; gsp.statq = cq_c; gsp.tx_seq = t; gsp.rx_seq = 0; gsp.rpc_seq = 0;
		for (uint32_t i = 0; i < len; i++)
			pay[i] = (uint8_t)rnd();
		gsp_seg s = { pay, len };
		if (gsp_mq_send(&gsp, 0x1000u + rnd() % 16, &s, 1, len, 0, 0, &why)) {
			printf("PARITY-MQ FAIL setup %s\n", why);
			return 1;
		}
		uint8_t *slot0 = sq + GSP_MQ_ENTRY_OFF + start * 4096;
		switch (corrupt) {
		case 1: slot0[100 % (48 + 32 + len)] ^= 0x40; break;			/* payload or header bit: checksum */
		case 2: wr32(slot0 + 52, 0x12345678u); fix_cksum(slot0, 1); break;	/* signature */
		case 3: wr32(sq + 8, 2048); break;						/* header shape */
		case 4: wr32(slot0 + 40, 0); break;						/* element count 0 */
		case 5: wr32(slot0 + 56, 16); fix_cksum(slot0, 1); break;		/* rpc length < 32 */
		case 6: wr32(sq + 16, 70); break;						/* write pointer past the ring */
		case 7: wr32(slot0 + 40, 17); break;					/* element count above 16 */
		case 9: wr32(sq + 24, 16); break;						/* rxHdrOff below 32 */
		default: break;
		}
		uint32_t rxseq = corrupt == 8 ? 1 : t;	/* 8: wrong sequence number */
		q.cmdq = cq_c; q.statq = sq; q.tx_seq = 0; q.rx_seq = rxseq; q.rpc_seq = 0;
		uint32_t our_rd = rd32(cq_c + GSP_MQ_RXHDR_OFF), gsp_wr = rd32(sq + 16);
		memset(out_c, 0, sizeof(out_c));
		memset(out_o, 0, sizeof(out_o));
		int rc = gsp_mq_recv(&q, out_c, sizeof(out_c), &fn_c, &res_c, &why);
		uint64_t st, ro = om_call(agpu_om_mqget, P(sq), P(out_o), our_rd, gsp_wr, rxseq, 0, &st);
		if (st) {
			printf("PARITY-MQ FAIL recv %u omega trap %llu\n", t, (unsigned long long)st);
			return 1;
		}
		if (rc == 1) {
			uint32_t mlen = GSP_ELEM_HDR_SIZE + rd32(out_c + 56), up = (mlen + 7) & ~7u;
			uint32_t elems = rd32(out_c + 40);
			if (ro != elems || memcmp(out_c, out_o, up) || rd32(out_o + 60) != fn_c || rd32(out_o + 64) != res_c) {
				printf("PARITY-MQ FAIL recv %u corrupt %u: oracle elems %u, omega %llu\n", t, corrupt, elems,
				       (unsigned long long)ro);
				return 1;
			}
			(*n)++;
		} else if (rc < 0) {
			if (ro != class_of(why)) {
				printf("PARITY-MQ FAIL recv %u corrupt %u: oracle (%s) omega %llu\n", t, corrupt, why,
				       (unsigned long long)ro);
				return 1;
			}
			(*refused)++;
		} else if (ro != 0) {
			printf("PARITY-MQ FAIL recv %u: oracle empty, omega %llu\n", t, (unsigned long long)ro);
			return 1;
		}
	}
	return 0;
}

int main(void)
{
	unsigned s = 0, r = 0, ref = 0;
	if (sends(&s, &ref) || recvs(&r, &ref))
		return 1;
	printf("PARITY-MQ PASS sends=%u recvs=%u refusals=%u\n", s, r, ref);
	return 0;
}
