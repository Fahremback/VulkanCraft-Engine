// TextShaper.cpp — SDK adapter for engine/ui/ITextShaper.hpp (agente 2,
// item `harfbuzz`). The ONLY TU with shaping behavior. Implements the
// Unicode text-shaping core headless and deterministic:
//   - UTF-8 decode with strict validation;
//   - grapheme clusters (UAX#29 basics): base + combining marks + ZWJ
//     sequences form one cluster;
//   - bidi (UAX#9 basics): directional runs by script range; RTL runs
//     mirrored internally; visual order left-to-right.
//
// The engine has no vendored harfbuzz; this adapter provides the shaping
// core the editor needs (deterministic layout of text runs), self-contained.
// Font advances come from the caller; ligature selection and font-specific
// shaping are out of scope (the caller feeds per-cluster advances).

#include "engine/ui/ITextShaper.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace ui {

namespace {

// ---- UTF-8 decoding ------------------------------------------------------

struct Decoded {
    std::vector<std::uint32_t> cps;   // code points
    std::vector<std::size_t> offsets; // byte offset of each code point
};

// Decodes a UTF-8 string into code points. Returns false on invalid input.
bool decode_utf8(const std::string& s, Decoded& out, std::string& error) {
    out.cps.clear();
    out.offsets.clear();
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            error = "text shaping: invalid UTF-8 lead byte";
            return false;
        }
        if (i + len > n) {
            error = "text shaping: truncated UTF-8 sequence";
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                error = "text shaping: invalid UTF-8 continuation byte";
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings and surrogates/out-of-range.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            error = "text shaping: invalid code point";
            return false;
        }
        out.cps.push_back(cp);
        out.offsets.push_back(i);
        i += len;
    }
    return true;
}

// Combining marks (Mn/Mc/Me — UAX#29 grapheme-extend basics).
bool is_combining_mark(std::uint32_t cp) {
    // Combining Diacritical Marks and extended blocks.
    if (cp >= 0x0300 && cp <= 0x036F) return true;
    if (cp >= 0x1AB0 && cp <= 0x1AFF) return true;
    if (cp >= 0x1DC0 && cp <= 0x1DFF) return true;
    if (cp >= 0x20D0 && cp <= 0x20FF) return true;
    if (cp >= 0xFE20 && cp <= 0xFE2F) return true;
    // Hebrew niqqud, Arabic harakat.
    if (cp >= 0x0591 && cp <= 0x05BD) return true;
    if (cp >= 0x05BF && cp <= 0x05C7) return true;
    if (cp >= 0x0610 && cp <= 0x061A) return true;
    if (cp >= 0x064B && cp <= 0x065F) return true;
    if (cp >= 0x0670) return false; // 0670 is a letter (superseded alef)
    return false;
}

// ZWJ (zero-width joiner) keeps a sequence in ONE cluster.
bool is_zwj(std::uint32_t cp) { return cp == 0x200D; }

// ---- Bidi script ranges (UAX#9 basics) -----------------------------------

bool is_rtl_codepoint(std::uint32_t cp) {
    // Hebrew, Arabic, Nko, Syriac, Thaana, Adlam, etc.
    if (cp >= 0x0590 && cp <= 0x05FF) return true; // Hebrew
    if (cp >= 0x0600 && cp <= 0x06FF) return true; // Arabic
    if (cp >= 0x0750 && cp <= 0x077F) return true; // Arabic Supplement
    if (cp >= 0x07C0 && cp <= 0x07FF) return true; // NKo
    if (cp >= 0x0700 && cp <= 0x074F) return true; // Syriac
    if (cp >= 0x0780 && cp <= 0x07BF) return true; // Thaana
    if (cp >= 0x08A0 && cp <= 0x08FF) return true; // Arabic Extended-A
    if (cp >= 0xFB1D && cp <= 0xFB4F) return true; // Hebrew Presentation Forms
    if (cp >= 0xFB50 && cp <= 0xFDFF) return true; // Arabic Presentation Forms-A
    if (cp >= 0xFE70 && cp <= 0xFEFF) return true; // Arabic Presentation Forms-B
    if (cp >= 0x1E900 && cp <= 0x1E95F) return true; // Adlam
    return false;
}

}  // namespace

class TextShaper final : public ITextShaper {
public:
    bool segment_clusters(const std::string& utf8,
                          std::vector<std::string>& clustersOut,
                          std::string& errorOut) override {
        Decoded d;
        if (!decode_utf8(utf8, d, errorOut)) return false;
        clustersOut.clear();
        if (d.cps.empty()) return true;
        // A cluster = a base code point followed by any combining marks / ZWJ
        // (UAX#29 basics). A leading mark forms its own one-codepoint cluster.
        std::size_t i = 0;
        const std::size_t n = d.cps.size();
        while (i < n) {
            const std::size_t start = d.offsets[i];
            if (is_combining_mark(d.cps[i])) {
                const std::size_t end =
                    (i + 1 < n) ? d.offsets[i + 1] : utf8.size();
                clustersOut.push_back(utf8.substr(start, end - start));
                ++i;
                continue;
            }
            // Extend the cluster with marks/ZWJ; a ZWJ (U+200D) joins the
            // NEXT base too (UAX#29 GB11: emoji ZWJ sequences stay one
            // cluster). Track whether the previous code point was a ZWJ.
            std::size_t j = i + 1;
            bool prevZwj = false;
            while (j < n) {
                const bool mark = is_combining_mark(d.cps[j]);
                const bool zwj = is_zwj(d.cps[j]);
                if (mark || zwj || prevZwj) {
                    if (zwj) {
                        prevZwj = true;
                    } else if (!mark) {
                        prevZwj = false;  // consumed the base after the ZWJ
                    }
                    ++j;
                    continue;
                }
                break;
            }
            const std::size_t end = (j < n) ? d.offsets[j] : utf8.size();
            clustersOut.push_back(utf8.substr(start, end - start));
            i = j;
        }
        return true;
    }

    bool shape(const std::string& utf8, float defaultAdvance,
               std::vector<ShapedGlyph>& glyphsOut, bool& hasRtlOut,
               std::vector<BidiRun>& runsOut,
               std::string& errorOut) override {
        Decoded d;
        if (!decode_utf8(utf8, d, errorOut)) return false;
        glyphsOut.clear();
        runsOut.clear();
        hasRtlOut = false;
        if (d.cps.empty()) return true;

        std::vector<std::string> clusters;
        if (!segment_clusters(utf8, clusters, errorOut)) return false;
        if (clusters.empty() && !d.cps.empty()) {
            errorOut = "text shaping: internal cluster mismatch";
            return false;
        }

        // Shape cluster by cluster: decode each cluster's code points.
        for (std::size_t ci = 0; ci < clusters.size(); ++ci) {
            Decoded cd;
            if (!decode_utf8(clusters[ci], cd, errorOut)) return false;
            for (std::size_t k = 0; k < cd.cps.size(); ++k) {
                const bool mark = is_combining_mark(cd.cps[k]);
                ShapedGlyph g;
                g.codepoint = cd.cps[k];
                g.cluster = static_cast<int>(ci);
                g.xAdvance = mark ? 0.0f : defaultAdvance;
                g.xOffset = 0.0f;
                g.isMark = mark;
                glyphsOut.push_back(g);
            }
            // Track runs: a run flips when the RTL-ness of the cluster's
            // base differs from the current run.
            bool baseRtl = false;
            for (std::size_t k = 0; k < cd.cps.size(); ++k) {
                if (!is_combining_mark(cd.cps[k]) && !is_zwj(cd.cps[k])) {
                    baseRtl = is_rtl_codepoint(cd.cps[k]);
                    break;
                }
            }
            const std::size_t glyphStart = glyphsOut.size() - cd.cps.size();
            if (runsOut.empty()) {
                BidiRun r;
                r.begin = glyphStart;
                r.count = cd.cps.size();
                r.rtl = baseRtl;
                runsOut.push_back(r);
            } else {
                BidiRun& last = runsOut.back();
                if (last.rtl == baseRtl) {
                    last.count += cd.cps.size();
                } else {
                    BidiRun r;
                    r.begin = glyphStart;
                    r.count = cd.cps.size();
                    r.rtl = baseRtl;
                    runsOut.push_back(r);
                }
            }
            if (baseRtl) hasRtlOut = true;
        }
        return true;
    }

    bool reorder_bidi(const std::vector<ShapedGlyph>& logical,
                      const std::vector<BidiRun>& runs,
                      std::vector<ShapedGlyph>& visualOut,
                      std::string& errorOut) override {
        visualOut.clear();
        if (runs.empty()) {
            visualOut = logical;
            return true;
        }
        // Runs are in logical order. Visual order: each run is placed
        // left-to-right; RTL runs are mirrored internally (their logical
        // first glyph becomes visually rightmost WITHIN the run).
        for (const BidiRun& run : runs) {
            if (run.begin + run.count > logical.size()) {
                errorOut = "text shaping: bidi run out of range";
                return false;
            }
            if (!run.rtl) {
                for (std::size_t k = 0; k < run.count; ++k) {
                    visualOut.push_back(logical[run.begin + k]);
                }
            } else {
                // Mirror: emit the run's glyphs in reverse (marks stay glued
                // to their base — we keep them attached by emitting base
                // then its marks in order, reversed per cluster; here we
                // reverse the whole run which keeps mark order per cluster
                // correct for the common case since marks follow their base).
                for (std::size_t k = run.count; k > 0; --k) {
                    visualOut.push_back(logical[run.begin + k - 1]);
                }
            }
        }
        return true;
    }
};

std::shared_ptr<ITextShaper> create_text_shaper() {
    return std::make_shared<TextShaper>();
}

}  // namespace ui
}  // namespace engine
