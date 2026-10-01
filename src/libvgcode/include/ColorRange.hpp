///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_COLORRANGE_HPP
#define VGCODE_COLORRANGE_HPP

#include "../include/Types.hpp"

#include <cfloat>
#include <vector>

namespace libvgcode {

static const Palette DEFAULT_RANGES_COLORS{ {
    {  11,  44, 122 }, // bluish
    {  19,  89, 133 },
    {  28, 136, 145 },
    {   4, 214,  15 },
    { 170, 242,   0 },
    { 252, 249,   3 },
    { 245, 206,  10 },
    { 227, 136,  32 },
    { 209, 104,  48 },
    { 194,  82,  60 },
    { 148,  38,  22 }  // reddish
} };

class ColorRange
{
public:
    //
    // Constructor
    //
    explicit ColorRange(EColorRangeType type = EColorRangeType::Linear);
    //
    // Return the type of this ColorRange.
    //
    EColorRangeType get_type() const;
    //
    // Return the palette used by this ColorRange.
    // Default is DEFAULT_RANGES_COLORS
    //
    const Palette& get_palette() const;
    //
    // Set the palette to be used by this ColorRange.
    // The given palette must contain at least two colors.
    //
    void set_palette(const Palette& palette);
    //
    // Return the interpolated color at the given value.
    // Value is clamped to [get_range()[0]..get_range()[1]].
    //
    Color get_color_at(float value) const;
    //
    // Return the range of this ColorRange.
    // The range is detected during the call to Viewer::load().
    // [0] -> min
    // [1] -> max
    //
    const std::array<float, 2>& get_range() const;
    //
    // Return the values detected while setting up this ColorRange, in increasing order, so that a
    // legend built from them names the values the data really used (temperatures, speeds, ...)
    // instead of evenly spaced samples of [min, max] that the print never used.
    // The size of the returned vector is the number of distinct values detected, up to MAX_VALUES.
    // Past that, and when no value was detected at all, it is get_palette().size() evenly spaced
    // samples of the range instead.
    //
    std::vector<float> get_values() const;
    //
    // Return the size of the palette, in bytes
    //
    std::size_t size_in_bytes_cpu() const;

    static const ColorRange DUMMY_COLOR_RANGE;

private:
    EColorRangeType m_type{ EColorRangeType::Linear };
    //
    // The palette used by this ColorRange
    // 
    Palette m_palette;
    //
    // [0] = min
    // [1] = max
    //
    std::array<float, 2> m_range{ FLT_MAX, -FLT_MAX };
    //
    // The distinct values passed to update(), in increasing order. Callers pass already binned
    // values, so these are the values the data really used. Collection stops once more than
    // MAX_VALUES distinct values have been seen: past that a legend listing them would be
    // unreadable, and get_values() falls back to evenly spaced samples of the range.
    //
    std::vector<float> m_values;
    //
    // Largest number of distinct values a legend can list.
    //
    static constexpr std::size_t MAX_VALUES{ 16 };

    //
    // Use the passed value to update the range.
    //
    void update(float value);
    //
    // Reset the range
    // Call this method before reuse an instance of ColorRange.
    //
    void reset();

    friend class ViewerImpl;
};

} // namespace libvgcode

#endif // VGCODE_COLORRANGE_HPP
