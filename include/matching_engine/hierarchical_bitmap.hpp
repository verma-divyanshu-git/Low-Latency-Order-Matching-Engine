#ifndef MATCHING_ENGINE_HIERARCHICAL_BITMAP_HPP
#define MATCHING_ENGINE_HIERARCHICAL_BITMAP_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

namespace matching_engine {

class HierarchicalBitmap {
public:
  explicit HierarchicalBitmap(std::uint32_t bit_count)
      : HierarchicalBitmap{bit_count, make_layout(bit_count)} {}

  [[nodiscard]] bool set(std::uint32_t index) noexcept {
    if (index >= bit_count_) {
      return false;
    }

    std::uint32_t child_word = index / kWordBits;
    std::uint64_t& base_word = word(0U, child_word);
    const std::uint64_t bit = std::uint64_t{1} << (index % kWordBits);
    if ((base_word & bit) != 0U) {
      return true;
    }

    const bool was_empty = base_word == 0U;
    base_word |= bit;
    if (!was_empty) {
      return true;
    }

    for (std::uint32_t level = 1U; level < level_count_; ++level) {
      const std::uint32_t summary_word_index = child_word / kWordBits;
      std::uint64_t& summary_word = word(level, summary_word_index);
      const std::uint64_t summary_bit = std::uint64_t{1} << (child_word % kWordBits);
      const bool summary_was_empty = summary_word == 0U;
      summary_word |= summary_bit;
      if (!summary_was_empty) {
        break;
      }
      child_word = summary_word_index;
    }
    return true;
  }

  [[nodiscard]] bool clear(std::uint32_t index) noexcept {
    if (index >= bit_count_) {
      return false;
    }

    std::uint32_t child_word = index / kWordBits;
    std::uint64_t& base_word = word(0U, child_word);
    const std::uint64_t bit = std::uint64_t{1} << (index % kWordBits);
    if ((base_word & bit) == 0U) {
      return true;
    }

    base_word &= ~bit;
    if (base_word != 0U) {
      return true;
    }

    for (std::uint32_t level = 1U; level < level_count_; ++level) {
      const std::uint32_t summary_word_index = child_word / kWordBits;
      std::uint64_t& summary_word = word(level, summary_word_index);
      const std::uint64_t summary_bit = std::uint64_t{1} << (child_word % kWordBits);
      summary_word &= ~summary_bit;
      if (summary_word != 0U) {
        break;
      }
      child_word = summary_word_index;
    }
    return true;
  }

  [[nodiscard]] std::optional<bool> test(std::uint32_t index) const noexcept {
    if (index >= bit_count_) {
      return std::nullopt;
    }
    return (word(0U, index / kWordBits) & (std::uint64_t{1} << (index % kWordBits))) != 0U;
  }

  [[nodiscard]] std::optional<std::uint32_t> first_set() const noexcept {
    return find_at_or_after(0U, 0U);
  }

  [[nodiscard]] std::optional<std::uint32_t> last_set() const noexcept {
    return find_at_or_before(0U, bit_count_ - 1U);
  }

  [[nodiscard]] std::optional<std::uint32_t> next_set(std::uint32_t index) const noexcept {
    if (index >= bit_count_) {
      return std::nullopt;
    }
    return find_at_or_after(0U, index);
  }

  [[nodiscard]] std::optional<std::uint32_t> previous_set(std::uint32_t index) const noexcept {
    if (index >= bit_count_) {
      return std::nullopt;
    }
    return find_at_or_before(0U, index);
  }

private:
  static constexpr std::uint32_t kWordBits = 64U;
  static constexpr std::uint32_t kMaximumLevels = 6U;

  struct Layout {
    std::array<std::size_t, kMaximumLevels> offsets{};
    std::array<std::uint32_t, kMaximumLevels> word_counts{};
    std::uint32_t level_count{};
    std::size_t total_words{};
  };

  HierarchicalBitmap(std::uint32_t bit_count, const Layout& layout)
      : bit_count_{bit_count}, offsets_{layout.offsets}, word_counts_{layout.word_counts},
        level_count_{layout.level_count},
        words_{std::make_unique<std::uint64_t[]>(layout.total_words)} {}

  [[nodiscard]] static Layout make_layout(std::uint32_t bit_count) {
    if (bit_count == 0U) {
      throw std::invalid_argument{"HierarchicalBitmap bit count must be positive"};
    }

    Layout layout;
    std::uint64_t level_bits = bit_count;
    while (true) {
      if (layout.level_count == kMaximumLevels) {
        throw std::length_error{"HierarchicalBitmap hierarchy is too deep"};
      }

      const std::uint64_t level_words = (level_bits + (kWordBits - 1U)) / kWordBits;
      if (level_words > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error{"HierarchicalBitmap level exceeds index range"};
      }
      const auto word_count = static_cast<std::uint32_t>(level_words);
      if (layout.total_words >
          std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(word_count)) {
        throw std::length_error{"HierarchicalBitmap storage size overflows"};
      }

      layout.offsets[layout.level_count] = layout.total_words;
      layout.word_counts[layout.level_count] = word_count;
      layout.total_words += word_count;
      ++layout.level_count;
      if (word_count == 1U) {
        break;
      }
      level_bits = word_count;
    }

    if (layout.total_words > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) {
      throw std::length_error{"HierarchicalBitmap allocation size overflows"};
    }
    return layout;
  }

  [[nodiscard]] std::uint64_t& word(std::uint32_t level, std::uint32_t word_index) noexcept {
    return words_[offsets_[level] + word_index];
  }

  [[nodiscard]] const std::uint64_t& word(std::uint32_t level,
                                          std::uint32_t word_index) const noexcept {
    return words_[offsets_[level] + word_index];
  }

  [[nodiscard]] std::uint32_t level_bit_count(std::uint32_t level) const noexcept {
    return level == 0U ? bit_count_ : word_counts_[level - 1U];
  }

  [[nodiscard]] std::optional<std::uint32_t> find_at_or_after(std::uint32_t level,
                                                              std::uint32_t index) const noexcept {
    if (index >= level_bit_count(level)) {
      return std::nullopt;
    }

    const std::uint32_t word_index = index / kWordBits;
    const std::uint64_t candidates =
        word(level, word_index) &
        (std::numeric_limits<std::uint64_t>::max() << (index % kWordBits));
    if (candidates != 0U) {
      return word_index * kWordBits + static_cast<std::uint32_t>(std::countr_zero(candidates));
    }
    if (level + 1U >= level_count_) {
      return std::nullopt;
    }

    const auto next_word = find_at_or_after(level + 1U, word_index + 1U);
    if (!next_word.has_value()) {
      return std::nullopt;
    }
    const std::uint64_t populated_word = word(level, *next_word);
    return *next_word * kWordBits + static_cast<std::uint32_t>(std::countr_zero(populated_word));
  }

  [[nodiscard]] std::optional<std::uint32_t> find_at_or_before(std::uint32_t level,
                                                               std::uint32_t index) const noexcept {
    if (index >= level_bit_count(level)) {
      return std::nullopt;
    }

    const std::uint32_t word_index = index / kWordBits;
    const std::uint32_t bit_index = index % kWordBits;
    const std::uint64_t mask = bit_index == kWordBits - 1U
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : (std::uint64_t{1} << (bit_index + 1U)) - 1U;
    const std::uint64_t candidates = word(level, word_index) & mask;
    if (candidates != 0U) {
      return word_index * kWordBits + (kWordBits - 1U) -
             static_cast<std::uint32_t>(std::countl_zero(candidates));
    }
    if (level + 1U >= level_count_ || word_index == 0U) {
      return std::nullopt;
    }

    const auto previous_word = find_at_or_before(level + 1U, word_index - 1U);
    if (!previous_word.has_value()) {
      return std::nullopt;
    }
    const std::uint64_t populated_word = word(level, *previous_word);
    return *previous_word * kWordBits + (kWordBits - 1U) -
           static_cast<std::uint32_t>(std::countl_zero(populated_word));
  }

  std::uint32_t bit_count_;
  std::array<std::size_t, kMaximumLevels> offsets_;
  std::array<std::uint32_t, kMaximumLevels> word_counts_;
  std::uint32_t level_count_;
  std::unique_ptr<std::uint64_t[]> words_;
};

} // namespace matching_engine

#endif
