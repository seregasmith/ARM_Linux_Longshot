// (longshot) A capture's late pages, decided at the replay.
//
// A capture copies the pages the CPU wrote, on the game's thread, at a submit. A page the GPU also writes may be missing
// the GPU's words in that copy: the submission that writes them back was not done when the copy was made. Such a page is
// late, and its copy is not used as it is (vk_memory.cpp: the replay waits for the write-back, or a pass on the GPU puts
// the write-back's words into the copy, "the late pages' fix"). capture_make finds the late pages by the write-backs
// recorded so far. With the replay on a thread of its own the game makes the captures of the next submissions before
// the replay has recorded the ones before them: the write-back that makes a page late is then not recorded yet, the
// page is not found late, and the mirror sync copies the CPU's older words over what the GPU wrote (RX_VK_MEM_CHECK's
// sync_over_gpu: a capture made with the GPU four submissions behind, every frame, on the same pages).
//
// The rule kept here: WHEN A CAPTURE IS TAKEN, EVERY PAGE OF IT THAT A MIRROR'S WRITE-BACK WROTE AFTER THE GPU'S PROGRESS
// AT THE CAPTURE IS LATE. The take is on the replay's thread, in the order of the submits: every submission before the
// capture's own is recorded by then, so its write-backs are all in the table the question is put to. Which words such a
// page takes from the GPU is the late pages' own business; this only completes the list.
//
// Not a page whose newest write is one the GPU made in place (no mirror: nothing is synced over it). Pure: no Vulkan.
// Tests: tests/test_late_replay.py.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace late_replay {

/// Completes a capture's late list at its take. pages: the capture's pages, ascending; done: the GPU's progress when
/// the capture was made; late (indices into pages, ascending) and late_pv (each one's newest write-back): the list
/// capture_make made, completed in place; added: the indices this call added (ascending); sidx: the capture's staging
/// slots (empty: none), where an added page loses its slot (-1: read from the buffer, as every late page).
/// written_back(page): the newest write-back of the page recorded (0: none); in_place(page): the newest one made in
/// place. Returns the newest write-back any late page has (the capture's late_value; 0: none late).
template <class WrittenBack, class InPlace>
uint64_t complete(const std::vector<uint32_t>& pages, uint64_t done, std::vector<uint32_t>& late, std::vector<uint64_t>& late_pv,
                  std::vector<uint32_t>& added, std::vector<int32_t>& sidx, WrittenBack&& written_back, InPlace&& in_place) {
    std::vector<uint32_t> all;
    std::vector<uint64_t> all_pv;
    added.clear();
    uint64_t newest = 0;
    size_t k = 0;   // (the next of the old list)
    const bool pv_kept = late_pv.size() == late.size();
    for (size_t i = 0; i < pages.size(); i++) {
        const bool was = k < late.size() && late[k] == (uint32_t)i;
        const uint64_t pv = written_back(pages[i]);
        if (was) {   // (found late at the capture: it stays, with the newest write-back there is now)
            const uint64_t old = pv_kept ? late_pv[k] : 0;
            k++;
            all.push_back((uint32_t)i), all_pv.push_back(pv > old ? pv : old);
        } else if (pv > done && in_place(pages[i]) < pv) {   // (written back from a mirror since the capture's view of the GPU)
            all.push_back((uint32_t)i), all_pv.push_back(pv);
            added.push_back((uint32_t)i);
            if (i < sidx.size()) sidx[i] = -1;
        } else {
            continue;
        }
        if (all_pv.back() > newest) newest = all_pv.back();
    }
    late.swap(all), late_pv.swap(all_pv);
    return newest;
}

} // namespace late_replay
