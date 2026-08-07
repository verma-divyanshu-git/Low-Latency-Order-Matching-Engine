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

  [[nodiscard]] bool hierarchy_consistent() const noexcept {
    if (bit_count_ == 0U || level_count_ == 0U || level_count_ > kMaximumLevels ||
        total_words_ == 0U || words_ == nullptr) {
      return false;
    }

    std::uint64_t represented_bits = bit_count_;
    std::size_t expected_offset = 0U;
    for (std::uint32_t level = 0U; level < level_count_; ++level) {
      const std::uint64_t expected_words = ((represented_bits - 1U) / kWordBits) + 1U;
      if (expected_words > std::numeric_limits<std::uint32_t>::max() ||
          word_counts_[level] != expected_words || offsets_[level] != expected_offset ||
          expected_offset > total_words_ ||
          static_cast<std::size_t>(word_counts_[level]) > total_words_ - expected_offset) {
        return false;
      }
      expected_offset += word_counts_[level];

      const bool is_top = word_counts_[level] == 1U;
      if (is_top != (level + 1U == level_count_)) {
        return false;
      }
      represented_bits = word_counts_[level];
    }
    if (expected_offset != total_words_) {
      return false;
    }

    represented_bits = bit_count_;
    for (std::uint32_t level = 0U; level < level_count_; ++level) {
      const std::uint32_t valid_bits_in_last_word =
          static_cast<std::uint32_t>(represented_bits % kWordBits);
      if (valid_bits_in_last_word != 0U) {
        const std::uint64_t padding_mask = std::numeric_limits<std::uint64_t>::max()
                                           << valid_bits_in_last_word;
        const std::uint64_t* const last_word = checked_word(level, word_counts_[level] - 1U);
        if (last_word == nullptr || (*last_word & padding_mask) != 0U) {
          return false;
        }
      }

      if (level != 0U) {
        const std::uint32_t lower_word_count = word_counts_[level - 1U];
        for (std::uint32_t lower_index = 0U; lower_index < lower_word_count; ++lower_index) {
          const std::uint64_t* const lower_word = checked_word(level - 1U, lower_index);
          const std::uint64_t* const summary_word = checked_word(level, lower_index / kWordBits);
          if (lower_word == nullptr || summary_word == nullptr) {
            return false;
          }
          const bool summary_set =
              (*summary_word & (std::uint64_t{1} << (lower_index % kWordBits))) != 0U;
          if (summary_set != (*lower_word != 0U)) {
            return false;
          }
        }
      }
      represented_bits = word_counts_[level];
    }
    return word_counts_[level_count_ - 1U] == 1U;
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
        level_count_{layout.level_count}, total_words_{layout.total_words},
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

  [[nodiscard]] const std::uint64_t* checked_word(std::uint32_t level,
                                                  std::uint32_t word_index) const noexcept {
    if (words_ == nullptr || level >= level_count_ || level >= kMaximumLevels ||
        word_index >= word_counts_[level] || offsets_[level] > total_words_ ||
        static_cast<std::size_t>(word_index) >= total_words_ - offsets_[level]) {
      return nullptr;
    }
    return &words_[offsets_[level] + word_index];
  }

  [[nodiscard]] std::uint32_t level_bit_count(std::uint32_t level) const noexcept {
    if (level >= level_count_ || level >= kMaximumLevels) {
      return 0U;
    }
    return level == 0U ? bit_count_ : word_counts_[level - 1U];
  }

  [[nodiscard]] std::optional<std::uint32_t> find_at_or_after(std::uint32_t level,
                                                              std::uint32_t index) const noexcept {
    const std::uint32_t represented_bits = level_bit_count(level);
    if (represented_bits == 0U || index >= represented_bits) {
      return std::nullopt;
    }

    const std::uint32_t word_index = index / kWordBits;
    const std::uint64_t* const current_word = checked_word(level, word_index);
    if (current_word == nullptr) {
      return std::nullopt;
    }
    const std::uint64_t candidates =
        *current_word & (std::numeric_limits<std::uint64_t>::max() << (index % kWordBits));
    if (candidates != 0U) {
      const std::uint32_t result =
          word_index * kWordBits + static_cast<std::uint32_t>(std::countr_zero(candidates));
      return result < represented_bits ? std::optional{result} : std::nullopt;
    }
    if (level >= level_count_ - 1U) {
      return std::nullopt;
    }

    const auto next_word = find_at_or_after(level + 1U, word_index + 1U);
    if (!next_word.has_value() || *next_word >= word_counts_[level]) {
      return std::nullopt;
    }
    const std::uint64_t* const populated_word = checked_word(level, *next_word);
    if (populated_word == nullptr || *populated_word == 0U) {
      return std::nullopt;
    }
    const std::uint32_t result =
        *next_word * kWordBits + static_cast<std::uint32_t>(std::countr_zero(*populated_word));
    return result < represented_bits ? std::optional{result} : std::nullopt;
  }

  [[nodiscard]] std::optional<std::uint32_t> find_at_or_before(std::uint32_t level,
                                                               std::uint32_t index) const noexcept {
    const std::uint32_t represented_bits = level_bit_count(level);
    if (represented_bits == 0U || index >= represented_bits) {
      return std::nullopt;
    }

    const std::uint32_t word_index = index / kWordBits;
    const std::uint64_t* const current_word = checked_word(level, word_index);
    if (current_word == nullptr) {
      return std::nullopt;
    }
    const std::uint32_t bit_index = index % kWordBits;
    const std::uint64_t mask = bit_index == kWordBits - 1U
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : (std::uint64_t{1} << (bit_index + 1U)) - 1U;
    const std::uint64_t candidates = *current_word & mask;
    if (candidates != 0U) {
      return word_index * kWordBits + (kWordBits - 1U) -
             static_cast<std::uint32_t>(std::countl_zero(candidates));
    }
    if (level >= level_count_ - 1U || word_index == 0U) {
      return std::nullopt;
    }

    const auto previous_word = find_at_or_before(level + 1U, word_index - 1U);
    if (!previous_word.has_value() || *previous_word >= word_counts_[level]) {
      return std::nullopt;
    }
    const std::uint64_t* const populated_word = checked_word(level, *previous_word);
    if (populated_word == nullptr || *populated_word == 0U) {
      return std::nullopt;
    }
    const std::uint32_t result = *previous_word * kWordBits + (kWordBits - 1U) -
                                 static_cast<std::uint32_t>(std::countl_zero(*populated_word));
    return result < represented_bits ? std::optional{result} : std::nullopt;
  }

  std::uint32_t bit_count_;
  std::array<std::size_t, kMaximumLevels> offsets_;
  std::array<std::uint32_t, kMaximumLevels> word_counts_;
  std::uint32_t level_count_;
  std::size_t total_words_;
  std::unique_ptr<std::uint64_t[]> words_;
};

} // namespace matching_engine

#endif
