#pragma once

// ITextShaper (agente 2 — item `harfbuzz`): PUBLIC text-shaping contract —
// Unicode text shaping core: grapheme-cluster segmentation, per-cluster
// advances, and basic bidirectional (bidi) reordering. The editor consumes
// it to lay out text runs (menus, labels, panels) deterministically and
// headless — no GPU, no font rasterizer, no window. The caller supplies the
// font advance (monospaced default or per-cluster measured widths); this
// contract decides the UNICODE structure: which code points form one visual
// cluster, in what visual order runs appear (bidi), and the advance of each
// glyph.
//
// What is implemented here (the shaping ALGORITHM, pure and deterministic):
//   - UTF-8 decoding (rejects invalid sequences);
//   - grapheme clustering per UAX#29 basics: a base code point followed by
//     combining marks (Mn/Mc/Me ranges) and ZWJ sequences group into ONE
//     cluster (so "a" + U+0301 is a single visual unit, emoji ZWJ families
//     stay one unit);
//   - bidi (UAX#9 basics): text is split into directional runs (LTR vs RTL
//     via the Hebrew/Arabic/etc. code-point ranges); RTL runs are reordered
//     so their logical first glyph is the visual rightmost — i.e. the run's
//     internal order is mirrored, and runs are emitted left-to-right in
//     visual order.
//
// Self-contained (std only). Deterministic: same UTF-8 + advances ->
// identical glyphs, bit-exact. The SDK adapter (src/engine/sdk/TextShaper.cpp)
// is the ONLY TU with behavior.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ui {

// One shaped glyph in VISUAL order (after bidi reordering).
struct ShapedGlyph {
    std::uint32_t codepoint{ 0 };  // decoded code point
    int cluster{ 0 };              // 0-based grapheme-cluster index
    float xAdvance{ 0.0f };        // horizontal advance for this glyph
    float xOffset{ 0.0f };         // relative pen offset (ligatures/marks)
    bool isMark{ false };          // combining mark (no standalone advance)
};

// A directional run found by bidi analysis (LOGICAL order).
struct BidiRun {
    std::size_t begin{ 0 };        // cluster index of the first glyph
    std::size_t count{ 0 };        // number of glyphs in the run
    bool rtl{ false };             // true = right-to-left script
};

class ITextShaper {
public:
    virtual ~ITextShaper() = default;

    // Decodes UTF-8 and splits the text into grapheme clusters (UAX#29
    // basics: base + combining marks + ZWJ sequences = one cluster).
    // Returns false with a message on invalid UTF-8. Empty input yields an
    // empty cluster list (success).
    virtual bool segment_clusters(const std::string& utf8,
                                  std::vector<std::string>& clustersOut,
                                  std::string& errorOut) = 0;

    // Shapes a text: decodes clusters, assigns each cluster's codepoints to
    // glyphs with `defaultAdvance` per non-mark glyph (marks get xAdvance 0
    // and ride on their base), and tags runs. Output glyphs are in LOGICAL
    // order; `hasRtl` reports whether any RTL run exists. `runsOut` describes
    // the directional runs (logical order) so the caller can apply bidi
    // (or call reorder_bidi below).
    virtual bool shape(const std::string& utf8, float defaultAdvance,
                       std::vector<ShapedGlyph>& glyphsOut, bool& hasRtlOut,
                       std::vector<BidiRun>& runsOut,
                       std::string& errorOut) = 0;

    // Reorders LOGICAL glyphs into VISUAL order per bidi (UAX#9 basics):
    // LTR runs keep their order; RTL runs are mirrored internally; runs are
    // emitted left-to-right in visual order. Deterministic and headless.
    virtual bool reorder_bidi(const std::vector<ShapedGlyph>& logical,
                              const std::vector<BidiRun>& runs,
                              std::vector<ShapedGlyph>& visualOut,
                              std::string& errorOut) = 0;
};

// Factory (implemented by the SDK adapter — the only TU with behavior).
std::shared_ptr<ITextShaper> create_text_shaper();

}  // namespace ui
}  // namespace engine
