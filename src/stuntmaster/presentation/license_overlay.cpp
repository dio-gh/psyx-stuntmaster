#include "stuntmaster/presentation/license_overlay.hpp"

#include <algorithm>
#include <cctype>

namespace stuntmaster::presentation {

namespace {

char foldUpper(char character) noexcept {
    const auto value = static_cast<unsigned char>(character);
    if (value >= 'a' && value <= 'z') {
        return static_cast<char>(value - 'a' + 'A');
    }
    return character;
}

} // namespace

std::vector<std::string> wrapLicenseText(
    std::string_view text, std::size_t columns) {
    std::vector<std::string> lines;
    if (columns == 0U) {
        return lines;
    }

    // Split into logical source lines, normalising as we go: drop carriage
    // returns, turn tabs into spaces, and uppercase-fold for the overlay font.
    std::vector<std::string> source_lines;
    std::string current;
    for (const char raw : text) {
        if (raw == '\r') {
            continue;
        }
        if (raw == '\n') {
            source_lines.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(raw == '\t' ? ' ' : foldUpper(raw));
    }
    source_lines.push_back(std::move(current));

    for (const auto& source : source_lines) {
        // Tokenise on spaces so runs of whitespace collapse; an all-space or
        // empty source line is preserved as a single blank row (paragraph gap).
        std::vector<std::string> words;
        std::size_t position = 0U;
        while (position < source.size()) {
            while (position < source.size() && source[position] == ' ') {
                ++position;
            }
            const auto start = position;
            while (position < source.size() && source[position] != ' ') {
                ++position;
            }
            if (position > start) {
                words.push_back(source.substr(start, position - start));
            }
        }
        if (words.empty()) {
            lines.emplace_back();
            continue;
        }

        std::string line;
        for (auto& original : words) {
            std::string word = original;
            // A single word longer than a line is hard-split across rows.
            while (word.size() > columns) {
                if (!line.empty()) {
                    lines.push_back(std::move(line));
                    line.clear();
                }
                lines.push_back(word.substr(0U, columns));
                word.erase(0U, columns);
            }
            if (word.empty()) {
                continue;
            }
            if (line.empty()) {
                line = std::move(word);
            } else if (line.size() + 1U + word.size() <= columns) {
                line.push_back(' ');
                line += word;
            } else {
                lines.push_back(std::move(line));
                line = std::move(word);
            }
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }

    return lines;
}

LicenseOverlay::LicenseOverlay(std::vector<LicenseDocument> documents) {
    setDocuments(std::move(documents));
}

void LicenseOverlay::setDocuments(std::vector<LicenseDocument> documents) {
    documents_ = std::move(documents);
    index_ = 0U;
    top_line_ = 0U;
    rewrap();
}

void LicenseOverlay::setViewport(std::size_t columns, std::size_t rows) {
    columns = std::max<std::size_t>(columns, 1U);
    rows = std::max<std::size_t>(rows, 1U);
    const bool width_changed = columns != columns_;
    columns_ = columns;
    rows_ = rows;
    if (width_changed) {
        rewrap();
    }
    clampScroll();
}

std::size_t LicenseOverlay::bodyRows() const noexcept {
    return rows_ > 1U ? rows_ - 1U : rows_;
}

void LicenseOverlay::rewrap() {
    if (documents_.empty()) {
        wrapped_.clear();
        return;
    }
    index_ = std::min(index_, documents_.size() - 1U);
    wrapped_ = wrapLicenseText(documents_[index_].body, columns_);
    clampScroll();
}

void LicenseOverlay::clampScroll() {
    const auto visible = bodyRows();
    const auto max_top =
        wrapped_.size() > visible ? wrapped_.size() - visible : 0U;
    top_line_ = std::min(top_line_, max_top);
}

void LicenseOverlay::scrollLines(int delta) {
    if (delta < 0) {
        const auto up = static_cast<std::size_t>(-delta);
        top_line_ = up >= top_line_ ? 0U : top_line_ - up;
    } else {
        top_line_ += static_cast<std::size_t>(delta);
    }
    clampScroll();
}

void LicenseOverlay::scrollPages(int delta) {
    // Overlap by one row so a line is not skipped across a page turn.
    const auto step = bodyRows() > 1U ? bodyRows() - 1U : 1U;
    scrollLines(delta * static_cast<int>(step));
}

void LicenseOverlay::nextDocument() {
    if (documents_.size() < 2U) {
        return;
    }
    index_ = (index_ + 1U) % documents_.size();
    top_line_ = 0U;
    rewrap();
}

void LicenseOverlay::previousDocument() {
    if (documents_.size() < 2U) {
        return;
    }
    index_ = (index_ + documents_.size() - 1U) % documents_.size();
    top_line_ = 0U;
    rewrap();
}

void LicenseOverlay::home() {
    top_line_ = 0U;
}

std::vector<std::string> LicenseOverlay::visibleRows() const {
    std::vector<std::string> rows;
    if (documents_.empty() || rows_ == 0U) {
        return rows;
    }
    const auto visible = bodyRows();
    const auto last_line = std::min(top_line_ + visible, wrapped_.size());

    // Every row is normalised to exactly `columns_` cells (padded with spaces,
    // truncated if longer) so the rasterised panel is a constant size regardless
    // of which lines are on screen -- otherwise it would grow and shrink as the
    // longest visible line changes while scrolling.
    const auto normalize = [&](std::string row) {
        if (row.size() > columns_) {
            row.resize(columns_);
        } else {
            row.append(columns_ - row.size(), ' ');
        }
        return row;
    };

    std::string header = documents_[index_].title;
    for (auto& character : header) {
        character = foldUpper(character);
    }
    if (documents_.size() > 1U) {
        header += "  DOC " + std::to_string(index_ + 1U) + "/" +
            std::to_string(documents_.size());
    }
    if (!wrapped_.empty()) {
        header += "  LINE " + std::to_string(top_line_ + 1U) + "-" +
            std::to_string(last_line) + "/" +
            std::to_string(wrapped_.size());
    }
    rows.push_back(normalize(std::move(header)));

    for (std::size_t offset = 0U; offset < visible; ++offset) {
        const auto line = top_line_ + offset;
        rows.push_back(normalize(
            line < wrapped_.size() ? wrapped_[line] : std::string{}));
    }
    return rows;
}

} // namespace stuntmaster::presentation
