#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace stuntmaster::presentation {

struct LicenseDocument {
    std::string title;
    std::string body;
};

// Portable, testable text model for the host-rendered license viewer (the
// hybrid P5(B) approach: a menu entry opens this pageable/scrollable host
// overlay; native HUD ownership is deferred). It holds the license documents,
// word-wraps and uppercase-folds the current one to the viewport width, and
// exposes the rows to draw now. It owns no GL or platform state -- the presenter
// rasterizes visibleRows() through the shared 5x7 overlay font and routes input
// into the navigation methods.
class LicenseOverlay {
public:
    LicenseOverlay() = default;
    explicit LicenseOverlay(std::vector<LicenseDocument> documents);

    void setDocuments(std::vector<LicenseDocument> documents);
    [[nodiscard]] bool empty() const noexcept { return documents_.empty(); }

    // Text viewport measured in glyph cells (columns wide, rows tall including
    // the one header row). Re-wraps the current document when the width changes.
    void setViewport(std::size_t columns, std::size_t rows);

    void scrollLines(int delta);
    void scrollPages(int delta);
    void nextDocument();
    void previousDocument();
    void home();

    [[nodiscard]] std::size_t documentCount() const noexcept {
        return documents_.size();
    }
    [[nodiscard]] std::size_t documentIndex() const noexcept { return index_; }
    [[nodiscard]] std::size_t topLine() const noexcept { return top_line_; }
    [[nodiscard]] std::size_t lineCount() const noexcept {
        return wrapped_.size();
    }

    // The rows to rasterize now: row 0 is a header (title, document N/of M,
    // visible line range), rows 1.. are the visible slice of the wrapped body.
    // Exactly `rows` entries when a viewport and at least one document exist.
    [[nodiscard]] std::vector<std::string> visibleRows() const;

private:
    void rewrap();
    void clampScroll();
    [[nodiscard]] std::size_t bodyRows() const noexcept;

    std::vector<LicenseDocument> documents_;
    std::vector<std::string> wrapped_;
    std::size_t index_{};
    std::size_t top_line_{};
    std::size_t columns_{40U};
    std::size_t rows_{20U};
};

// Word-wrap and uppercase-fold `text` to `columns` cells for the overlay font.
// Splits on newlines (preserving blank lines), expands tabs to spaces, drops
// carriage returns, collapses runs of spaces, breaks between words, and
// hard-splits any single word longer than a line. `columns == 0` yields no rows.
[[nodiscard]] std::vector<std::string> wrapLicenseText(
    std::string_view text, std::size_t columns);

} // namespace stuntmaster::presentation
