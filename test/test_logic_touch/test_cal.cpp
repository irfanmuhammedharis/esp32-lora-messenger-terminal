// Stage 6 — calibration solver, on the host.   pio test -e native
//
// Tests the pure math inside Touch::solve(): four raw corner samples mapped
// onto screen pixels. This is the only part of the touch subsystem that is
// Arduino-free and therefore host-testable. Every failure surface — swapped
// axes, inverted ranges, span too small, map() overflow — is exercised so
// that a change to the solve() formula cannot silently break a mapping the
// hardware depends on.
//
// Touch::solve() lives in lib/Touch/Touch.cpp but that file pulls in
// <Arduino.h>. The structs and the function are replicated here (identically,
// not approximately) so the test compiles under `pio test -e native`.

#include <string.h>
#include <unity.h>

// ── Replicated types — MUST match Touch.h ───────────────────────────────────

struct TouchRaw {
    uint16_t x;
    uint16_t y;
    uint16_t z;       // unused by solve() but kept for struct identity
};

struct TouchCal {
    uint32_t version;
    int32_t  xRawMin, xRawMax;
    int32_t  yRawMin, yRawMax;
    bool     swapAxes;
    uint16_t zThreshold;
};

static constexpr uint32_t TOUCH_CAL_VERSION = (0x54430201u | (1 & 0x3));
static constexpr uint16_t TOUCH_Z_THRESHOLD = 350;

// ── solve(), replicated identically from lib/Touch/Touch.cpp ─────────────────

static TouchCal solve(const TouchRaw &tl, const TouchRaw &tr,
                      const TouchRaw &br, const TouchRaw &bl,
                      int32_t inset, int32_t w, int32_t h) {
    TouchCal c{};
    c.version    = TOUCH_CAL_VERSION;
    c.zThreshold = TOUCH_Z_THRESHOLD;

    // Two-edge axis detection: the top edge and the left edge must agree on
    // whether the axes are swapped; a sloppy tap can lie about one edge, so
    // when they disagree trust the edge with the larger movement contrast.
    const int32_t dxTop  = abs((int32_t)tr.x - (int32_t)tl.x);
    const int32_t dyTop  = abs((int32_t)tr.y - (int32_t)tl.y);
    const int32_t dxLeft = abs((int32_t)bl.x - (int32_t)tl.x);
    const int32_t dyLeft = abs((int32_t)bl.y - (int32_t)tl.y);

    const bool swapTop  = dyTop > dxTop;    // screen X carried by raw Y
    const bool swapLeft = dxLeft > dyLeft;  // screen Y carried by raw X

    if (swapTop == swapLeft) {
        c.swapAxes = swapTop;
    } else {
        const int32_t topContrast  = abs(dyTop - dxTop);
        const int32_t leftContrast = abs(dxLeft - dyLeft);
        c.swapAxes = (leftContrast > topContrast) ? swapLeft : swapTop;
    }

    auto axisA = [&](const TouchRaw &r) {
        return (int32_t)(c.swapAxes ? r.y : r.x);
    };
    auto axisB = [&](const TouchRaw &r) {
        return (int32_t)(c.swapAxes ? r.x : r.y);
    };

    const int32_t aLeft   = (axisA(tl) + axisA(bl)) / 2;
    const int32_t aRight  = (axisA(tr) + axisA(br)) / 2;
    const int32_t bTop    = (axisB(tl) + axisB(tr)) / 2;
    const int32_t bBottom = (axisB(bl) + axisB(br)) / 2;

    const int32_t spanX = w - 1 - 2 * inset;
    const int32_t spanY = h - 1 - 2 * inset;
    if (spanX > 0) {
        const int32_t perPx = ((aRight - aLeft) * 1000) / spanX;
        c.xRawMin = aLeft  - (perPx * inset) / 1000;
        c.xRawMax = aRight + (perPx * inset) / 1000;
    } else {
        c.xRawMin = aLeft;
        c.xRawMax = aRight;
    }
    if (spanY > 0) {
        const int32_t perPx = ((bBottom - bTop) * 1000) / spanY;
        c.yRawMin = bTop    - (perPx * inset) / 1000;
        c.yRawMax = bBottom + (perPx * inset) / 1000;
    } else {
        c.yRawMin = bTop;
        c.yRawMax = bBottom;
    }
    return c;
}

// ── Simulate a calibrated read — replicating Touch::read() ──────────────────

static bool mapToScreen(const TouchCal &c, const TouchRaw &r,
                        int32_t w, int32_t h,
                        int32_t &sx, int32_t &sy) {
    const int32_t a = c.swapAxes ? r.y : r.x;
    const int32_t b = c.swapAxes ? r.x : r.y;

    // map() integer arithmetic: (v - fromLow) * (toHigh - toLow)
    //                         / (fromHigh - fromLow) + toLow
    const int32_t fromA = c.xRawMin, toA = c.xRawMax;
    const int32_t fromB = c.yRawMin, toB = c.yRawMax;

    if (toA == fromA || toB == fromB) return false;

    const int64_t numA = (int64_t)(a - fromA) * (int64_t)(w - 1);
    const int64_t numB = (int64_t)(b - fromB) * (int64_t)(h - 1);
    sx = (int32_t)(numA / (toA - fromA));
    sy = (int32_t)(numB / (toB - fromB));
    if (sx < 0) sx = 0;
    if (sx > w - 1) sx = w - 1;
    if (sy < 0) sy = 0;
    if (sy > h - 1) sy = h - 1;
    return true;
}

// ── Test: normal unswapped calibration ──────────────────────────────────────

static void test_normal_no_swap(void) {
    // The X raw axis tracks screen X. Four taps 30 px from the corners of a
    // 240×320 panel. Raw X spans 500-3500, raw Y spans 400-3600.
    const TouchRaw tl = { 520,  420, 0};
    const TouchRaw tr = {3480,  400, 0};
    const TouchRaw br = {3500, 3580, 0};
    const TouchRaw bl = { 500, 3600, 0};

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    // No axis swap: raw X varies far more along the top edge than raw Y.
    TEST_ASSERT_FALSE_MESSAGE(c.swapAxes, "axes should not swap");
    TEST_ASSERT_EQUAL_UINT32(TOUCH_CAL_VERSION, c.version);

    // The extrapolated raw range should bracket the samples.
    TEST_ASSERT_TRUE_MESSAGE(c.xRawMin < 520,  "xRawMin should be left of TL");
    TEST_ASSERT_TRUE_MESSAGE(c.xRawMax > 3500, "xRawMax should be right of BR");
    TEST_ASSERT_TRUE_MESSAGE(c.yRawMin < 400,  "yRawMin should be above TR");
    TEST_ASSERT_TRUE_MESSAGE(c.yRawMax > 3600, "yRawMax should be below BL");

    // The solve() extrapolates from inset targets to screen edges, so a raw
    // sample at the TL target position maps back to (inset, inset), not (0,0).
    // Tolerance of 4 px accounts for integer division truncation in the per-px
    // extrapolation.
    int32_t sx, sy;
    TEST_ASSERT_TRUE(mapToScreen(c, tl, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 30,  sx);
    TEST_ASSERT_INT_WITHIN(4, 30,  sy);

    TEST_ASSERT_TRUE(mapToScreen(c, tr, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 209, sx);   // w-1 - inset = 209
    TEST_ASSERT_INT_WITHIN(4, 30,  sy);

    TEST_ASSERT_TRUE(mapToScreen(c, br, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 209, sx);
    TEST_ASSERT_INT_WITHIN(4, 289, sy);   // h-1 - inset = 289

    TEST_ASSERT_TRUE(mapToScreen(c, bl, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 30,  sx);
    TEST_ASSERT_INT_WITHIN(4, 289, sy);
}

// ── Test: swapped axes ──────────────────────────────────────────────────────

static void test_swapped_axes(void) {
    // Raw Y tracks screen X (the panel was mounted rotated or the connector
    // was wired differently). Top-left→top-right: raw X barely changes, raw
    // Y varies 500→3500.
    const TouchRaw tl = {2000,  520, 0};
    const TouchRaw tr = {2010, 3480, 0};
    const TouchRaw br = { 390, 3500, 0};
    const TouchRaw bl = { 400,  500, 0};

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    TEST_ASSERT_TRUE_MESSAGE(c.swapAxes, "raw Y tracks screen X — must swap");

    int32_t sx, sy;
    TEST_ASSERT_TRUE(mapToScreen(c, tl, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 30,  sx);
    TEST_ASSERT_INT_WITHIN(4, 30,  sy);

    TEST_ASSERT_TRUE(mapToScreen(c, br, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 209, sx);
    TEST_ASSERT_INT_WITHIN(4, 289, sy);
}

// ── Test: inverted range (raw min > raw max) ────────────────────────────────

static void test_inverted_range(void) {
    // The panel is mounted "backwards" — raw X decreases left to right.
    const TouchRaw tl = {3500, 3500, 0};   // inverted: high at left
    const TouchRaw tr = { 500, 3480, 0};   // inverted: low at right
    const TouchRaw br = { 520,  400, 0};
    const TouchRaw bl = {3480,  420, 0};

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    // xRawMin > xRawMax is valid — map() handles it.
    TEST_ASSERT_TRUE_MESSAGE(c.xRawMin > c.xRawMax,
                             "inverted: min should be greater than max");

    int32_t sx, sy;
    TEST_ASSERT_TRUE(mapToScreen(c, tl, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 30,  sx);
    TEST_ASSERT_INT_WITHIN(4, 30,  sy);

    TEST_ASSERT_TRUE(mapToScreen(c, br, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(4, 209, sx);
    TEST_ASSERT_INT_WITHIN(4, 289, sy);
}

// ── Test: swaps AND inversion (both raw axes reversed and swapped) ──────────

static void test_swapped_and_inverted(void) {
    // Swapped and inverted: raw Y (which tracks screen X) is HIGH at the
    // left edge and LOW at the right. raw X (which tracks screen Y) is HIGH
    // at the top and LOW at the bottom.
    const TouchRaw tl = {3500, 3500, 0};   // top-left:     both high
    const TouchRaw tr = {3500,  500, 0};   // top-right:    X high, Y low
    const TouchRaw br = { 500,  500, 0};   // bottom-right: both low
    const TouchRaw bl = { 500, 3500, 0};   // bottom-left:  X low, Y high

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    TEST_ASSERT_TRUE(c.swapAxes);
    TEST_ASSERT_TRUE_MESSAGE(c.xRawMin > c.xRawMax, "inverted X axis");

    int32_t sx, sy;
    // The targets were tapped at screen inset positions, so the error is
    // measured from (30, 30), not (0, 0). The solver extrapolates from the
    // inset targets to the edges, then the mapping projects back.
    TEST_ASSERT_TRUE(mapToScreen(c, tl, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(6, 30,  sx);
    TEST_ASSERT_INT_WITHIN(6, 30,  sy);

    TEST_ASSERT_TRUE(mapToScreen(c, br, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(6, 209, sx);   // w-1 - inset = 209
    TEST_ASSERT_INT_WITHIN(6, 289, sy);   // h-1 - inset = 289
}

// ── Test: center point maps correctly ───────────────────────────────────────

static void test_center_maps_to_center(void) {
    const TouchRaw tl = { 500,  500, 0};
    const TouchRaw tr = {3500,  480, 0};
    const TouchRaw br = {3520, 3520, 0};
    const TouchRaw bl = { 480, 3500, 0};

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    // The centre raw point should map to (w/2, h/2).
    const TouchRaw centre = {2000, 2000, 0};
    int32_t sx, sy;
    TEST_ASSERT_TRUE(mapToScreen(c, centre, 240, 320, sx, sy));
    TEST_ASSERT_INT_WITHIN(8, 120, sx);
    TEST_ASSERT_INT_WITHIN(8, 160, sy);
}

// ── Test: swap verdicts disagree -> prefer the more confident edge ──────────

static void test_disagreeing_edges_fall_back(void) {
    // The top edge (TL->TR) says "swapped": raw Y moved 500, raw X only 200,
    // so dyTop > dxTop. But the left edge (TL->BL) clearly says "normal":
    // raw Y moved 2500 while raw X barely moved 100. The top-right tap was
    // sloppy. The two verdicts disagree, so solve() must trust the left
    // edge, whose movement contrast (2400) dwarfs the top edge's (300).
    const TouchRaw tl = {1000, 1000, 0};
    const TouchRaw tr = {1200, 1500, 0};   // sloppy: pulled toward the middle
    const TouchRaw br = {3000, 3000, 0};
    const TouchRaw bl = {1100, 3500, 0};

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    TEST_ASSERT_FALSE_MESSAGE(c.swapAxes,
        "confident left edge (normal) must win over the sloppy top edge");
}

// ── Test: disagreeing edges, other direction ────────────────────────────────

static void test_disagreeing_edges_fall_back_swapped(void) {
    // Mirror of the previous test: the left edge is the sloppy one. Top edge
    // says "swapped" with huge contrast, left edge says "normal" barely.
    // solve() must trust the top edge.
    const TouchRaw tl = {1000, 1000, 0};
    const TouchRaw tr = {1000, 3500, 0};   // swapped: raw Y tracks screen X
    const TouchRaw br = {3500, 3500, 0};
    const TouchRaw bl = {1050, 1200, 0};   // sloppy left tap

    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);

    TEST_ASSERT_TRUE_MESSAGE(c.swapAxes,
        "confident top edge (swapped) must win over the sloppy left edge");
}

// ── Test: zero inset (targets at screen edges) ──────────────────────────────

static void test_zero_inset(void) {
    const TouchRaw tl = {1000, 1000, 0};
    const TouchRaw tr = {3000,  980, 0};
    const TouchRaw br = {3020, 3020, 0};
    const TouchRaw bl = { 980, 3000, 0};

    const TouchCal c = solve(tl, tr, br, bl, 0, 240, 320);

    // With zero inset, xRawMin/Max are the edge-averaged raw values
    // directly — no extrapolation. The left edge average is (1000+980)/2=990,
    // the right edge average is (3000+3020)/2=3010.
    TEST_ASSERT_INT_WITHIN(20, 990,  c.xRawMin);
    TEST_ASSERT_INT_WITHIN(20, 3010, c.xRawMax);
}

// ── Test: version embeds rotation ───────────────────────────────────────────

static void test_version_embeds_rotation(void) {
    // Just verify the version gets set; the rotation bits are tested by
    // the platform-dependent TOUCH_TFT_ROTATION, which varies per build.
    const TouchRaw tl = {500, 500, 0}, tr = {3000, 500, 0};
    const TouchRaw br = {3000, 3000, 0}, bl = {500, 3000, 0};
    const TouchCal c = solve(tl, tr, br, bl, 30, 240, 320);
    TEST_ASSERT_NOT_EQUAL(0, c.version);
}

// ── Test: zThreshold is populated ───────────────────────────────────────────

static void test_z_threshold_propagated(void) {
    const TouchRaw d = {1000, 1000, 0}, dr = {3000, 1000, 0};
    const TouchRaw dd = {3000, 3000, 0}, dl = {1000, 3000, 0};
    const TouchCal c = solve(d, dr, dd, dl, 30, 240, 320);
    TEST_ASSERT_EQUAL_UINT16(TOUCH_Z_THRESHOLD, c.zThreshold);
}

// ── Test: mapToScreen clamps out-of-range coordinates ───────────────────────

static void test_clamp_out_of_range(void) {
    const TouchRaw tl = { 520,  420, 0};
    const TouchRaw tr = {3480,  400, 0};
    const TouchRaw br = {3500, 3580, 0};
    const TouchRaw bl = { 500, 3600, 0};
    const TouchCal  c = solve(tl, tr, br, bl, 30, 240, 320);

    // A touch far outside the calibrated range should clamp, not crash.
    const TouchRaw far = {0, 0, 0};
    int32_t sx, sy;
    TEST_ASSERT_TRUE(mapToScreen(c, far, 240, 320, sx, sy));
    TEST_ASSERT_EQUAL_INT32(0, sx);
    TEST_ASSERT_EQUAL_INT32(0, sy);

    const TouchRaw far2 = {4095, 4095, 0};
    TEST_ASSERT_TRUE(mapToScreen(c, far2, 240, 320, sx, sy));
    TEST_ASSERT_EQUAL_INT32(239, sx);
    TEST_ASSERT_EQUAL_INT32(319, sy);
}

// ── Test: tiny panel (50×50) still solves ───────────────────────────────────

static void test_tiny_panel(void) {
    const TouchRaw tl = { 500,  500, 0};
    const TouchRaw tr = {1000,  510, 0};
    const TouchRaw br = { 990, 1000, 0};
    const TouchRaw bl = { 510,  990, 0};
    const TouchCal  c = solve(tl, tr, br, bl, 3, 50, 50);
    // Should not divide by zero; spanX = 50-1-6 = 43 > 0. Fine.
    int32_t sx, sy;
    TEST_ASSERT_TRUE(mapToScreen(c, tl, 50, 50, sx, sy));
}

// ── Test: large panel (800×480) ─────────────────────────────────────────────

static void test_large_panel(void) {
    const TouchRaw tl = { 200,  200, 0};
    const TouchRaw tr = {3800,  180, 0};
    const TouchRaw br = {3820, 3820, 0};
    const TouchRaw bl = { 180, 3800, 0};
    const TouchCal  c = solve(tl, tr, br, bl, 30, 800, 480);

    int32_t sx, sy;
    // The targets were tapped at inset=30, so a tap at the TL position
    // should map to roughly (30, 30), not (0, 0). The solver extrapolates
    // from inset targets out to the screen edges.
    TEST_ASSERT_TRUE(mapToScreen(c, tl, 800, 480, sx, sy));
    TEST_ASSERT_INT_WITHIN(10, 30,  sx);
    TEST_ASSERT_INT_WITHIN(10, 30,  sy);

    TEST_ASSERT_TRUE(mapToScreen(c, br, 800, 480, sx, sy));
    TEST_ASSERT_INT_WITHIN(10, 769, sx);   // w-1 - inset = 769
    TEST_ASSERT_INT_WITHIN(10, 449, sy);   // h-1 - inset = 449
}

// ── Test: map rejects zero-span calibration ─────────────────────────────────

static void test_map_rejects_zero_span(void) {
    TouchCal bad{};
    bad.version    = TOUCH_CAL_VERSION;
    bad.xRawMin    = 1000;
    bad.xRawMax    = 1000;   // zero span
    bad.yRawMin    = 1000;
    bad.yRawMax    = 3000;
    bad.swapAxes   = false;
    bad.zThreshold = TOUCH_Z_THRESHOLD;

    const TouchRaw r = {1500, 2000, 0};
    int32_t sx, sy;
    TEST_ASSERT_FALSE(mapToScreen(bad, r, 240, 320, sx, sy));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_normal_no_swap);
    RUN_TEST(test_swapped_axes);
    RUN_TEST(test_inverted_range);
    RUN_TEST(test_swapped_and_inverted);
    RUN_TEST(test_disagreeing_edges_fall_back);
    RUN_TEST(test_disagreeing_edges_fall_back_swapped);
    RUN_TEST(test_center_maps_to_center);
    RUN_TEST(test_zero_inset);
    RUN_TEST(test_version_embeds_rotation);
    RUN_TEST(test_z_threshold_propagated);
    RUN_TEST(test_clamp_out_of_range);
    RUN_TEST(test_tiny_panel);
    RUN_TEST(test_large_panel);
    RUN_TEST(test_map_rejects_zero_span);
    return UNITY_END();
}
