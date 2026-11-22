"""RX_VK_PASS_TIMING times the GPU's ASTC decode and BC re-encode, by whether the upload budget can put a texture off.

At an area's entry the GPU decodes dozens of ASTC textures in one frame, and re-encodes the big ones to BC3 (or BC7),
in the recordings' upload command buffers. That work showed in no span: the GPU sat at 100% for ~450 ms at Hyrule
Field's entry on an RX 9070 XT while the log's GPU time line showed 20 ms (#1720). Each texture's decode is now a span
of kind "astc", named by whether it was re-encoded and whether upload_defer could put it off. The span opens after the
staging prefix (test_texture_staging_publication compiles that prefix without the profiler) and adds no return path
(test_bc_upload_retry counts them). upload_defer and the labels share one rule, upload_never_put_off.

python -I -B tests/test_astc_pass_timing.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"
DEV = ROOT / "docs" / "dev" / "reference" / "profiling.md"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    tex = TEX.read_text(encoding="utf-8")
    upload = block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {")
    label = "const char* const span_label = kSpans[tex.bc ? 1 : 0][upload_never_put_off(tex) ? 1 : 0];"
    span = "span.emplace(cmd, span_label);"
    need(label in upload and span in upload, "the ASTC decode has no RX_VK_PASS_TIMING span of its own")
    at = upload.index(span)
    need(upload.index("const uint64_t limit =") < at, "the span opens inside the staging prefix the staging test compiles")
    need(at < upload.index("vkCmdDispatch("), "a decode's dispatch runs before its span opens")
    need(at < upload.index("bc_scratch_layout(cmd"), "the BC scratch's layout change runs before the span opens")
    # (an upload chunk's flush ends the command buffer and waits on the CPU: the span closes before it, opens after)
    need("span.reset();" in upload and "replay_flush_upload(cmd)" in upload, "the span is not closed around an upload chunk's flush")
    flush = upload.index("replay_flush_upload(cmd)")
    need(upload.index("span.reset();") < flush < upload.index(span, flush), "a span runs across an upload chunk's flush")
    labels = block(upload, "static const char* const kSpans[2][2] = {")
    need(labels.count('"astc ') == 4, "a label of the ASTC spans does not start with the kind 'astc'")
    need(labels.count("BC re-encode") == 2 and labels.count("never put off") == 2, "the four labels are not the four cases")

    rule = block(tex, "bool upload_never_put_off(const Texture& tex) {")
    need("tex.layers > 1" in rule and "tex.width > 1024 && tex.height > 1024" in rule, "the never-put-off rule changed")
    defer = block(tex, "bool upload_defer(const Texture& tex) {")
    need("if (upload_never_put_off(tex)) return false;" in defer, "upload_defer has a rule of its own, apart from the labels'")
    need(defer.count("tex.layers > 1") == 0, "upload_defer keeps a second copy of the terrain rule")

    dev = DEV.read_text(encoding="utf-8")
    row = next((line for line in dev.splitlines() if line.startswith("| `RX_VK_PASS_TIMING=1` or `2`")), "")
    need("ASTC" in row, "docs/dev/reference/profiling.md's RX_VK_PASS_TIMING row doesn't say it times the ASTC decode")
    print("PASS: RX_VK_PASS_TIMING times the ASTC decode and re-encode, split by what the upload budget can put off")


if __name__ == "__main__":
    main()
