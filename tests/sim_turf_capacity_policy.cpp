// SPDX-License-Identifier: GPL-3.0-or-later
// The simulated-turf capacity policy without Vulkan: demand grows only the saturated list, allocation failure leaves the
// installed cap alone, and sustained lower demand observes the configured floor.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "sim_turf_capacity.h"

using mkwvk::sim_turf_capacity::Policy;
using mkwvk::sim_turf_capacity::mirror_span;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    // The stock-compatible copy keeps forward lists at their start and reverse lists aligned at their occupied end.
    CHECK((mirror_span(200, 100, 8, false) == mkwvk::sim_turf_capacity::MirrorSpan{0, 0, 800}));
    CHECK((mirror_span(200, 100, 8, true) == mkwvk::sim_turf_capacity::MirrorSpan{800, 0, 800}));
    CHECK((mirror_span(50, 100, 8, true) == mkwvk::sim_turf_capacity::MirrorSpan{0, 400, 400}));
    CHECK((mirror_span(0, 100, 8, true) == mkwvk::sim_turf_capacity::MirrorSpan{}));

    Policy blades{143360, 64, 1048576, 143360};
    Policy pool{138240, 32, 786432, 138240};

    CHECK(blades.capacity() == 143360 && blades.target() == 143360);
    CHECK(pool.capacity() == 138240 && pool.target() == 138240);
    CHECK(!blades.observe(90000, 1));
    CHECK(!pool.observe(80000, 1));

    // Only the saturated blade list asks to grow. Its target includes 25% headroom and is group-aligned.
    CHECK(blades.observe(143360, 2));
    CHECK(blades.target() == 179200);
    CHECK(pool.target() == 138240);

    // A failed allocation does not lie to the shader about the installed capacity; the next sample can retry.
    blades.install_failed();
    CHECK(blades.capacity() == 143360);

    // The adaptive floor still starts safely at stock. Sustained demand shrinks to its aligned target with headroom;
    // zero demand can release every record, and the next reservation grows it again.
    Policy lean{143360, 64, 1048576, 0};
    CHECK(lean.capacity() == 143360);
    CHECK(!lean.observe(20000, 1));
    CHECK(lean.observe(20000, 602));
    CHECK(lean.target() == 25024);
    lean.installed();
    CHECK(!lean.observe(0, 700));
    CHECK(lean.observe(0, 1301));
    CHECK(lean.target() == 0);
    lean.installed();
    CHECK(lean.observe(1, 1302));
    CHECK(lean.target() == 64);

    // Shrinking uses the highest demand in the whole quiet window, not an unusually low final sample.
    Policy variable{143360, 64, 1048576, 0};
    CHECK(!variable.observe(20000, 1));
    CHECK(!variable.observe(30000, 300));
    CHECK(variable.observe(10000, 602));
    CHECK(variable.target() == 37504);

    CHECK(blades.observe(143360, 3));
    blades.installed();
    CHECK(blades.capacity() == 179200);
    CHECK(!blades.observe(150000, 4));

    // A transient low sample does not shrink. Sustained quiet can, but never below the game's stock cap.
    CHECK(!blades.observe(20000, 300));
    CHECK(blades.observe(20000, 901));
    CHECK(blades.target() == 143360);
    blades.installed();
    CHECK(blades.capacity() == 143360);

    // A pathological counter is bounded by what the device/buffer representation permits.
    CHECK(pool.observe(UINT32_MAX, 700));
    CHECK(pool.target() == 786432);

    std::puts("PASS: adaptive simulated-turf capacity policy");
}
