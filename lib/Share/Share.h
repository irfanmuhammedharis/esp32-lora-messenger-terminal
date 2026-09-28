#pragma once
//
// The text of the SHARE LOC + VITALS preset: two messages, built at the
// moment of sending from the current GPS fix and the current vitals.
//
//     LOC 12.34567,76.27112          live fix
//     LAST 12.34567,76.27112 4m      fix lost - last known position, and how
//                                    long ago it was taken
//     LOC NO GPS FIX                 no position has ever been received
//
//     HR 72 SPO2 98% T 36.6C         "--" for any value the health gates
//                                    rejected, same as the Vitals screen
//
// Two messages because one cannot hold both: MSG_MAX_LEN is 32, fixed by the
// nRF firmware (app_config.h), and a worst-case LAST line is 31 characters on
// its own. Each message stands alone, so losing one on air never garbles the
// other, and each reads as plain text on any receiver.
//
// Contains NO Arduino calls: test/test_logic_gps proves every worst case fits
// MSG_MAX_LEN on the host.

#include <stddef.h>
#include <stdint.h>

#include "Gps.h"

// `out` should hold MSG_MAX_LEN + 1 bytes.
void formatLocationMsg(char *out, size_t n, const GpsFix &fix, uint32_t nowMs);

// `out` should hold MSG_MAX_LEN + 1 bytes. Values are clamped to their field
// widths, so even a nonsense reading cannot push the message past the cap.
void formatVitalsMsg(char *out, size_t n,
                     bool hrOk, int hr,
                     bool spo2Ok, int spo2,
                     bool tempOk, int32_t tempMilliC);
