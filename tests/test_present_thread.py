"""The presenter thread (on every PC) keeps the presents in order, one out at most, and off the swapchain's remaking.

present_out hands a present to the presenter thread only from present_image (it alone sets the copy's fence; frame generation's
present_commit paces its presents itself), NVIDIA Reflex's present id with it. The presenter waits for the copy's fence before it
presents, then presents on the presents' own queue (the replay's, or the Present queue row's) with that queue's lock held for the
call only, under FIFO waits until the present is shown (VK_KHR_present_wait), and never without end. There is no second queue for
it. The game's thread waits while the frame before's present is still out; the swapchain is made again only with the presenter
idle, and the wait for that comes before the queue's lock (the presenter takes it). The presenter never makes a swapchain itself:
an out-of-date or suboptimal present asks the next present_image for one.

python -I -B tests/test_present_thread.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def main():
    vk = ROOT / "gpu" / "vk"
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    main_cpp = (vk / "vk_main.cpp").read_text(encoding="utf-8")

    out = block(replay, "static bool present_out(uint32_t index, std::unique_lock<decltype(dev.present_mu)>& qk, OutputTrialSource trial, bool preview_path) {")
    hand = out[: out.index("present_hand_off(")]
    need("const bool hand_off = present_copy_done && present_thread_on();" in hand and "if (hand_off) {" in hand, "a present is handed over from frame generation")
    # (NVIDIA Reflex: its present id goes with the present; the game's thread lets go of the present marker, the presenter
    # presents with that id and marks the present's end)
    need("static_cast<const VkPresentIdKHR*>(pi.pNext)->pPresentIds[0]" in hand and
         "if (reflex_id) reflex_present_handed_off();" in hand,
         "with NVIDIA Reflex the present's id is not handed over with it, or the game's thread keeps its present marker")
    need("present_hand_off(index, present_copy_done, reflex_id, frame);" in out, "the present is handed over without its Reflex id")
    need("qk.unlock();" in hand, "the queue's lock is held while the game's thread hands a present over")
    rest = out[out.index("present_hand_off("):]
    need("qk.unlock();\n        present_thread_drain();\n        qk.lock();" in rest and
         rest.index("present_thread_drain();") < rest.index("vkQueuePresentKHR"),
         "a present made directly may go out before the one handed over, or waits for it with the queue's lock held")

    image = block(replay, "void present_image_with_trial(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view, bool source_srgb, bool allow_scale, OutputTrialSource trial) {")
    need("present_copy_done = f.done;\n    present_out(index, qk, trial, preview_path);\n    present_copy_done = VK_NULL_HANDLE;" in image,
         "present_image does not let its present (and only it) be handed over")
    for site in ("reflex_swapchain_stale() || hdr_changed || vsync_changed) {", "if (res == VK_ERROR_OUT_OF_DATE_KHR) {"):
        at = image[image.index(site):]
        need(at.index("present_thread_drain();") < at.index("std::lock_guard qk(dev.present_mu);"),
             "present_image remakes the swapchain without first waiting for the presenter, outside the queue's lock")
    commit = block(replay, "bool present_commit(")
    need("present_out(p.index, qk);" in commit and "present_copy_done" not in commit,
         "frame generation's paced presents are handed over")

    give = block(replay, "void present_hand_off(uint32_t index, VkFence done, uint64_t reflex_id, OutputTrialFrame trial) {")
    idle = block(replay, "bool presenter_wait_idle(std::unique_lock<std::mutex>& lk, int limit_ms = 0) {")
    need("presenter.cv.wait(lk, idle);" in idle, "presenter_wait_idle does not wait for the presenter")
    need(give.index("presenter_wait_idle(lk);") < give.index("presenter.busy = true"),
         "a present is handed over before the one before is made")
    need("presenter_wait_idle(lk, 5000)" in block(replay, "bool present_thread_drain() {"), "present_thread_drain does not wait, or without a limit")

    loop = block(replay, "void presenter_loop() {")
    need("presenter.reflex_id = reflex_id;" in give and "const uint64_t reflex_id = presenter.reflex_id;" in loop,
         "the presenter does not take the Reflex id handed over with the present")
    need("const bool reflex = reflex_id > next_id;" in loop and "pi.pNext = dev.present_wait || reflex ? &pid : nullptr;" in loop,
         "the presenter does not present with NVIDIA Reflex's id (going up from the last), or ids are missing from some presents")
    need("if (reflex) reflex_present_end(id);" in loop and
         loop.index("vkQueuePresentKHR") < loop.index("if (reflex) reflex_present_end(id);"),
         "the presenter does not mark the end of a present made with Reflex's id after the present call")
    need(loop.index("presenter_wait_copy(done)") < loop.index("vkQueuePresentKHR"),
         "the presenter presents before the copy is done (the driver's wait comes back)")
    copy = block(replay, "bool presenter_wait_copy(VkFence done) {")
    need("UINT64_MAX" not in copy and "100000000ull" in copy and "VK_ERROR_DEVICE_LOST" in copy,
         "the presenter's wait for the copy has no limit, or misses a device lost")
    need("vkQueuePresentKHR(own ? dev.present_queue : dev.queue, &pi)" in loop and
         "std::unique_lock q(own ? dev.present_queue_mu : dev.present_mu);" in loop and
         "std::lock_guard pq(dev.presenter_queue_mu);" in loop,
         "the presenter does not present on the presents' own queue, under that queue's lock and then the swapchain's")
    need(loop.index("presenter_wait_copy(done)") < loop.index("std::unique_lock q(own"),
         "the presenter takes the queue's lock before the copy is done (the replay's submits would wait for the GPU)")
    need(loop.index("std::unique_lock q(own") < loop.index("std::lock_guard pq(dev.presenter_queue_mu);"),
         "the presenter takes the swapchain's lock before the queue's (device_wait_idle_locked's order is the queue's first)")
    need("wait_for_present(dev.device, sc, id, 20000000ull)" in loop and "dev.swap_mode == VK_PRESENT_MODE_FIFO_KHR" in loop,
         "under FIFO the presenter does not wait (with a limit) for its present to be shown")
    need("UINT64_MAX" not in loop, "a wait without end on the presenter")
    wait_idle = block(main_cpp, "") if False else replay[replay.index("void device_wait_idle_locked() {"):]
    wait_idle = wait_idle[:wait_idle.index("\n}") + 2]
    need("std::lock_guard pr(dev.presenter_queue_mu);" in wait_idle, "vkDeviceWaitIdle runs with the presenter's queue in use")
    need("presenter.busy = false" in loop and "notify_all" in loop, "the presenter does not say it is done")
    need("create_swapchain" not in loop and "reflex_presented" not in loop,
         "the presenter makes a swapchain itself, or sets Reflex's present marker")
    finish = block(replay, "void present_finish(VkResult res, bool on_presenter) {")
    need("on_presenter && (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)" in finish and "swap_resize" in finish,
         "an out-of-date present on the presenter does not ask for a new swapchain")

    need("on_presenter && res == VK_ERROR_SURFACE_LOST_KHR" in finish and "presenter_surface_lost.store(true" in finish,
         "a surface lost at the presenter's present is not handed to the next present_image")
    lost = image[image.index("if (presenter_surface_lost.exchange(false"):]
    need(lost.index("present_thread_drain();") < lost.index("std::lock_guard qk(dev.present_mu);") <
         lost.index("invalidate_lost_surface(VK_ERROR_SURFACE_LOST_KHR);") < lost.index("return;"),
         "a surface the presenter found lost is dropped without the presenter idle first, or outside the queue's lock")
    acq = image[image.index("    if (res == VK_ERROR_SURFACE_LOST_KHR) {"):]
    need(acq.index("present_thread_drain();") < acq.index("invalidate_lost_surface(res);"),
         "the acquire's lost surface is dropped with a present still out on the presenter")
    make = block(main_cpp, "bool create_swapchain(uint32_t width, uint32_t height) {")
    first = next(l.strip() for l in make[make.index("{") + 1:].splitlines()
                 if l.strip() and not l.strip().startswith("//"))   # (the comment lines name the why; the first statement is the pin)
    need(first == "if (!swapchain_made_ready()) return false;",
         "create_swapchain does not answer not-made while a present is in the making on the presenter thread")
    on = block(replay, "bool present_thread_on() {")
    need("getenv" not in on and "return true;" in on, "the presenter thread is not on on every PC")
    dev_make = block(main_cpp, "bool create_device() {")
    need("presenter_queue_pick" not in dev_make and "presenter_family" not in main_cpp,
         "a second queue is made for the presenter (it time-shared the GPU with the replay)")
    swap = block(main_cpp, "bool create_swapchain(uint32_t width, uint32_t height) {")
    need("presenter_family" not in swap, "the swapchain is shared with a presenter queue's family")
    print("PASS: the presenter thread waits for the copy, keeps the presents in order and off the swapchain's remaking")


if __name__ == "__main__":
    main()
