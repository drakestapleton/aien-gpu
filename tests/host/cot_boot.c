// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* cot_boot -- the real SEC2 packet against the start-up, on the host: takes the cot_packet.bin that
 * aien-gpu-boot --plan-only built from the real GB20B package and runs om/boot.om with it on the
 * simulated GB20B SEC2 + GSP (tests/host/gb20b_sim.c). The simulated SEC2 accepts only what the
 * driver's GB20B HALs send (COT version 2, size 860, FRTS fields zero), so this closes the loop
 * between the package builder and the start-up without the chip.
 *   cot_boot PACKET  -> COT-BOOT PASS ..., exit 0, only if the start-up finishes with stage 0 and the
 *                       simulator saw no violation; the same packet with COT version 1 or with the
 *                       FRTS size set (what a GB10B package sends) must stop at stage 11.
 * Host only: the simulator is not the chip. */
#include <stdio.h>
#include <string.h>
#include "omhost.h"
#include "gb20b_sim.h"

#define ARGS_PA 0x0000000123456000ull

static struct g2sim sim;

static int run(const uint8_t *pkt, uint32_t len, uint32_t *stage, uint32_t *err)
{
	uint64_t st[32] = { 0 }, act[4], obs = 0, status;
	struct g2sim_cfg c;
	struct ck_gpu_regs regs;
	static uint8_t buf[1024];
	unsigned steps = 0;

	memset(&c, 0, sizeof(c));
	c.secboot_reads = 2;
	c.reply_reads = 3;
	c.mask_reads = 4;
	c.args_reads = 2;
	c.lockdown_reads = 5;
	c.fmc_args_pa = ARGS_PA;
	g2sim_init(&sim, &c);
	regs = g2sim_regs(&sim);
	memset(buf, 0, sizeof(buf));
	memcpy(buf, pkt, len);
	st[1] = len;
	st[2] = ARGS_PA;
	st[3] = 0;
	st[4] = st[5] = st[6] = st[7] = 100;
	for (;;) {
		uint64_t v = om_call(agpu_om_boot, P(st), P(buf), obs, P(act), 0, 0, &status);

		if (status || (v != 0 && v != 1) || ++steps > 100000)
			return -1;
		if (v == 1)
			break;
		obs = 0;
		if (act[0] == 1)
			obs = regs.rd(regs.ctx, (uint32_t)act[1]);
		else if (act[0] == 2)
			regs.wr(regs.ctx, (uint32_t)act[1], (uint32_t)act[2]);
		else if (act[0] != 3)
			return -1;
	}
	*stage = (uint32_t)st[12];
	*err = (uint32_t)st[19];
	return 0;
}

int main(int argc, char **argv)
{
	static uint8_t pkt[1024], bad[1024];
	uint32_t stage = ~0u, err = ~0u;
	size_t n;
	FILE *f;

	if (argc != 2 || !(f = fopen(argv[1], "rb"))) {
		printf("COT-BOOT FAIL usage: cot_boot PACKET\n");
		return 1;
	}
	n = fread(pkt, 1, sizeof(pkt), f);
	fclose(f);
	if (n != 868) {
		printf("COT-BOOT FAIL packet is %zu bytes, want 868\n", n);
		return 1;
	}
	if (run(pkt, 868, &stage, &err) || stage != 0 || sim.violations || sim.phase != 4) {
		printf("COT-BOOT FAIL real packet: stage %u err %#x violations %u phase %u\n", stage, err,
		       sim.violations, sim.phase);
		return 1;
	}
	/* controls: what a GB10B package would send */
	memcpy(bad, pkt, sizeof(bad));
	bad[8] = 1; /* COT version 1 */
	if (run(bad, 868, &stage, &err) || stage != 11 || err == 0) {
		printf("COT-BOOT FAIL control version 1: stage %u err %#x, want stage 11\n", stage, err);
		return 1;
	}
	memcpy(bad, pkt, sizeof(bad));
	bad[28] = 0x48; bad[29] = 0xfd; bad[30] = 0x02; /* frtsSysmemSize = 195912 */
	if (run(bad, 868, &stage, &err) || stage != 11 || err == 0) {
		printf("COT-BOOT FAIL control FRTS size: stage %u err %#x, want stage 11\n", stage, err);
		return 1;
	}
	printf("COT-BOOT PASS version=%u size=%u controls=2 caught=2\n", pkt[8] | pkt[9] << 8, pkt[10] | pkt[11] << 8);
	return 0;
}
