/*
 * wakes-sp1 — the eMMC report (M6, #43; CONFIG_SP1_STORAGE_TEST).
 *
 * Decoded console lines, every one starting "EMMC": what came back during init (CID,
 * OCR, size), the card's state and EXT_CSD, block 0 (MBR / GPT / a bare FAT volume /
 * sp1-tape-looper's index) with the boot sector of each partition, and the driver's
 * error counters. Reads only. Storage thread only (sp1_store.c); paced for CDC ACM.
 */
#ifndef SP1_EMMC_REPORT_H
#define SP1_EMMC_REPORT_H

#include <stdbool.h>
#include "sp1_emmc.h"

void sp1_emmc_report_init(bool ok, const struct sp1_emmc_ident *id);
void sp1_emmc_report_card(void);
void sp1_emmc_report_stats(void);

#endif /* SP1_EMMC_REPORT_H */
