// TextShaperTests.cpp
//
// Gate for ITextShaper (agente 2 — item `harfbuzz`): the Unicode
// text-shaping core (grapheme clusters, per-cluster advances, bidi). Proves
// the REAL adapter (src/engine/sdk/TextShaper.cpp):
//   - clusters: "a" + U+0301 (combining acute) is ONE grapheme cluster;
//     ZWJ emoji sequences stay one cluster; plain "abc" is three;
//   - shaping: marks carry zero advance; clusters map 1:1 to glyphs;
//   - bidi: Hebrew "אבג" is detected RTL; reorder_bidi mirrors the run;
//     mixed "ab" + Hebrew reorders to visual [LTR run][mirrored RTL run];
//   - validation: invalid UTF-8 is refused with a message; empty text is a
//     valid empty shape.
//
// Deterministic and headless. Self-contained (std + engine/ui only).

#include <engine/ui/ITextShaper.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

void test_clusters() {
    std::printf("[shaper] clusters…\n");
    auto shaper = engine::ui::create_text_shaper();
    check(shaper != nullptr, "factory create_text_shaper() returns non-null");

    std::vector<std::string> clusters;
    std::string err;

    // "a" + combining acute (U+0301) = one grapheme.
    const std::string aAcute = "a\xCC\x81";
    check(shaper->segment_clusters(aAcute, clusters, err),
          "segment(a+U+0301) succeeds");
    check(clusters.size() == 1, "a+combining-acute is ONE cluster");
    if (clusters.size() == 1) {
        check(clusters[0].size() == 3, "cluster keeps base+mark bytes");
    }

    // Plain "abc" = three clusters.
    check(shaper->segment_clusters("abc", clusters, err),
          "segment(abc) succeeds");
    check(clusters.size() == 3, "abc is three clusters");

    // ZWJ family emoji: man ZWJ woman ZWJ girl — one cluster.
    const std::string family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    check(shaper->segment_clusters(family, clusters, err),
          "segment(ZWJ family) succeeds");
    check(clusters.size() == 1, "ZWJ emoji sequence is ONE cluster");

    // Hebrew "אבג" is three separate clusters (no combining marks).
    const std::string hebrew = "\xD7\x90\xD7\x91\xD7\x92";
    check(shaper->segment_clusters(hebrew, clusters, err),
          "segment(hebrew) succeeds");
    check(clusters.size() == 3, "hebrew abc is three clusters");
    std::printf("[shaper] clusters OK\n");
}

void test_shaping_advances() {
    std::printf("[shaper] shaping/advances…\n");
    auto shaper = engine::ui::create_text_shaper();
    std::vector<engine::ui::ShapedGlyph> glyphs;
    bool hasRtl = false;
    std::vector<engine::ui::BidiRun> runs;
    std::string err;

    check(shaper->shape("abc", 10.0f, glyphs, hasRtl, runs, err),
          "shape(abc) succeeds");
    check(glyphs.size() == 3, "three glyphs for abc");
    check(!hasRtl, "abc is LTR (hasRtl=false)");
    if (glyphs.size() == 3) {
        check(glyphs[0].xAdvance == 10.0f && glyphs[1].xAdvance == 10.0f &&
              glyphs[2].xAdvance == 10.0f,
              "each glyph advance = default (monospaced)");
        check(glyphs[0].cluster == 0 && glyphs[1].cluster == 1 &&
              glyphs[2].cluster == 2,
              "cluster indices map 1:1");
    }
    check(runs.size() == 1 && !runs[0].rtl, "single LTR run");

    // "a" + U+0301: the mark rides the base with zero advance.
    const std::string aAcute = "a\xCC\x81";
    check(shaper->shape(aAcute, 8.0f, glyphs, hasRtl, runs, err),
          "shape(a+U+0301) succeeds");
    check(glyphs.size() == 2, "two glyphs (base + mark)");
    if (glyphs.size() == 2) {
        check(glyphs[0].xAdvance == 8.0f && !glyphs[0].isMark,
              "base has full advance");
        check(glyphs[1].xAdvance == 0.0f && glyphs[1].isMark,
              "mark has zero advance");
        check(glyphs[0].cluster == 0 && glyphs[1].cluster == 0,
              "base and mark share the same cluster");
    }
    std::printf("[shaper] shaping OK\n");
}

void test_bidi() {
    std::printf("[shaper] bidi…\n");
    auto shaper = engine::ui::create_text_shaper();
    std::vector<engine::ui::ShapedGlyph> glyphs;
    std::vector<engine::ui::ShapedGlyph> visual;
    bool hasRtl = false;
    std::vector<engine::ui::BidiRun> runs;
    std::string err;

    // Pure Hebrew "אבג" — RTL, mirrored visually.
    const std::string hebrew = "\xD7\x90\xD7\x91\xD7\x92"; // alef bet gimel
    check(shaper->shape(hebrew, 5.0f, glyphs, hasRtl, runs, err),
          "shape(hebrew) succeeds");
    check(hasRtl, "hebrew detected as RTL");
    check(runs.size() == 1 && runs[0].rtl, "single RTL run");
    check(glyphs.size() == 3, "three hebrew glyphs");
    check(shaper->reorder_bidi(glyphs, runs, visual, err),
          "reorder_bidi(hebrew) succeeds");
    check(visual.size() == 3, "visual keeps three glyphs");
    if (visual.size() == 3 && glyphs.size() == 3) {
        // Logical [alef, bet, gimel] -> visual [gimel, bet, alef] (mirrored).
        check(visual[0].codepoint == glyphs[2].codepoint,
              "visual[0] = logical[2] (mirrored run)");
        check(visual[2].codepoint == glyphs[0].codepoint,
              "visual[2] = logical[0]");
    }

    // Mixed "ab" + Hebrew: two runs, visual = [LTR ab][mirrored RTL].
    const std::string mixed = "ab\xD7\x90\xD7\x91\xD7\x92";
    check(shaper->shape(mixed, 5.0f, glyphs, hasRtl, runs, err),
          "shape(mixed) succeeds");
    check(hasRtl, "mixed text has RTL");
    check(runs.size() == 2, "two directional runs");
    if (runs.size() == 2) {
        check(!runs[0].rtl && runs[1].rtl,
              "run order = [LTR, RTL]");
    }
    check(shaper->reorder_bidi(glyphs, runs, visual, err),
          "reorder_bidi(mixed) succeeds");
    check(visual.size() == 5, "visual keeps five glyphs");
    if (visual.size() == 5 && glyphs.size() == 5) {
        // Visual order: a, b, then the RTL run mirrored: gimel, bet, alef.
        check(visual[0].codepoint == static_cast<std::uint32_t>('a'),
              "visual[0]=a (LTR run first)");
        check(visual[1].codepoint == static_cast<std::uint32_t>('b'),
              "visual[1]=b");
        check(visual[2].codepoint == glyphs[4].codepoint,
              "visual[2]=mirrored first RTL glyph (logical last)");
        check(visual[4].codepoint == glyphs[2].codepoint,
              "visual[4]=mirrored last RTL glyph (logical first)");
    }
    std::printf("[shaper] bidi OK\n");
}

void test_validation() {
    std::printf("[shaper] validation…\n");
    auto shaper = engine::ui::create_text_shaper();

    std::vector<std::string> clusters;
    std::string err;

    // Empty text -> valid empty.
    check(shaper->segment_clusters("", clusters, err),
          "segment(empty) succeeds");
    check(clusters.empty(), "empty yields zero clusters");

    // Invalid UTF-8: 0xFF lead byte.
    const std::string bad = "\xFF\xFE";
    check(!shaper->segment_clusters(bad, clusters, err),
          "invalid UTF-8 refused");
    check(!err.empty(), "refusal reports a message");

    // Invalid continuation: 0xC3 0x41.
    const std::string bad2 = "\xC3\x41";
    check(!shaper->segment_clusters(bad2, clusters, err),
          "bad continuation refused");

    // Truncated: 0xE2 0x82 (needs 3 bytes).
    const std::string bad3 = "\xE2\x82";
    check(!shaper->segment_clusters(bad3, clusters, err),
          "truncated UTF-8 refused");

    std::printf("[shaper] validation OK\n");
}

}  // namespace

int main() {
    test_clusters();
    test_shaping_advances();
    test_bidi();
    test_validation();
    if (g_failures == 0) {
        std::printf("[shaper] ALL PASSED\n");
        return 0;
    }
    std::printf("[shaper] %d FAILURE(S)\n", g_failures);
    return 1;
}
