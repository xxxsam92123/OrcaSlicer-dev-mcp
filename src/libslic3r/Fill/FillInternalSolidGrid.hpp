#pragma once

#include "FillRectilinear.hpp"

namespace Slic3r {

class FillInternalSolidGrid : public FillRectilinear {
public:
    Fill *clone() const override { return new FillInternalSolidGrid(*this); }
    Polylines fill_surface(const Surface *surface, const FillParams &params) override;
};

} // namespace Slic3r
