/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/effects/SkImageFilters.h"

#include "include/core/SkFlattenable.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkM44.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkSize.h"
#include "include/core/SkString.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/private/base/SkAssert.h"
#include "include/private/base/SkFloatingPoint.h"
#include "src/core/SkImageFilterTypes.h"
#include "src/core/SkImageFilter_Base.h"
#include "src/core/SkPicturePriv.h"
#include "src/core/SkReadBuffer.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkWriteBuffer.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <chrono>
#include <iostream>

namespace {
// Constants from kawase_blur_rt.cpp
static constexpr float kInputScale = 0.25f;
static constexpr float kInverseInputScale = 1.0f / kInputScale;
static constexpr uint32_t kMaxPasses = 4;
static constexpr float kMaxCrossFadeRadius = 30.0f;

class SkKawaseBlurImageFilter final : public SkImageFilter_Base {
public:
    SkKawaseBlurImageFilter(SkScalar blurRadius, sk_sp<SkImageFilter> input)
            : SkImageFilter_Base(&input, 1), fBlurRadius(blurRadius) {
        SkASSERT(blurRadius >= 0.f);
    }

    SkRect computeFastBounds(const SkRect& src) const override;

protected:
    void flatten(SkWriteBuffer&) const override;

private:
    friend void ::SkRegisterKawaseBlurImageFilterFlattenable();
    SK_FLATTENABLE_HOOKS(SkKawaseBlurImageFilter)

    MatrixCapability onGetCTMCapability() const override { return MatrixCapability::kComplex; }

    skif::FilterResult onFilterImage(const skif::Context&) const override;

    skif::LayerSpace<SkIRect> onGetInputLayerBounds(
            const skif::Mapping& mapping,
            const skif::LayerSpace<SkIRect>& desiredOutput,
            std::optional<skif::LayerSpace<SkIRect>> contentBounds) const override;

    std::optional<skif::LayerSpace<SkIRect>> onGetOutputLayerBounds(
            const skif::Mapping& mapping,
            std::optional<skif::LayerSpace<SkIRect>> contentBounds) const override;

    skif::ParameterSpace<SkScalar> fBlurRadius;
};

}  // end namespace

sk_sp<SkImageFilter> SkImageFilters::KawaseBlur(SkScalar blurRadius,
                                                sk_sp<SkImageFilter> input,
                                                const CropRect& cropRect) {
    if (!SkScalarIsFinite(blurRadius) || blurRadius < 0.f) {
        return nullptr;
    }

    sk_sp<SkImageFilter> filter = sk_make_sp<SkKawaseBlurImageFilter>(blurRadius, std::move(input));
    if (cropRect) {
        filter = SkImageFilters::Crop(*cropRect, SkTileMode::kDecal, std::move(filter));
    }
    return filter;
}

void SkRegisterKawaseBlurImageFilterFlattenable() {
    SK_REGISTER_FLATTENABLE(SkKawaseBlurImageFilter);
    SkFlattenable::Register("SkKawaseBlurImageFilterImpl", SkKawaseBlurImageFilter::CreateProc);
}

sk_sp<SkFlattenable> SkKawaseBlurImageFilter::CreateProc(SkReadBuffer& buffer) {
    SK_IMAGEFILTER_UNFLATTEN_COMMON(common, 1);
    SkScalar blurRadius = buffer.readScalar();

    return SkImageFilters::KawaseBlur(blurRadius, common.getInput(0));
}

void SkKawaseBlurImageFilter::flatten(SkWriteBuffer& buffer) const {
    SkImageFilter_Base::flatten(buffer);
    buffer.writeScalar(SkScalar(fBlurRadius));
}

///////////////////////////////////////////////////////////////////////////////////////////////////

skif::FilterResult SkKawaseBlurImageFilter::onFilterImage(const skif::Context& ctx) const {
    skif::FilterResult input = this->getChildOutput(0, ctx);
    if (!input) {
        return {};
    }

    // Map blur radius from parameter space to layer space
    SkScalar blurRadius = SkScalar(skif::ParameterSpace<SkScalar>(fBlurRadius));
    skif::Context inputCtx = ctx.withNewDesiredOutput(input.layerBounds());
    skif::FilterResult::Builder blurBuilder{inputCtx};
    blurBuilder.add(input);
    return blurBuilder.kawaseBlur(blurRadius);
}

skif::LayerSpace<SkIRect> SkKawaseBlurImageFilter::onGetInputLayerBounds(
        const skif::Mapping& mapping,
        const skif::LayerSpace<SkIRect>& desiredOutput,
        std::optional<skif::LayerSpace<SkIRect>> contentBounds) const {
    // Kawase blur needs input that extends beyond the desired output
    // The blur radius affects how much extra input is needed
    SkScalar layerRadius = SkScalar(skif::ParameterSpace<SkScalar>(fBlurRadius));

    // Estimate required input bounds (similar to Gaussian blur's 3σ rule)
    skif::LayerSpace<SkIRect> requiredInput = desiredOutput;
    skif::LayerSpace<SkISize> radiusOutset =
            skif::LayerSpace<SkSize>({3.f * layerRadius, 3.f * layerRadius}).ceil();
    requiredInput.outset(radiusOutset);

    return this->getChildInputLayerBounds(0, mapping, requiredInput, contentBounds);
}

std::optional<skif::LayerSpace<SkIRect>> SkKawaseBlurImageFilter::onGetOutputLayerBounds(
        const skif::Mapping& mapping,
        std::optional<skif::LayerSpace<SkIRect>> contentBounds) const {
    auto childOutput = this->getChildOutputLayerBounds(0, mapping, contentBounds);
    if (childOutput) {
        // Output bounds are the same as input (blur doesn't expand output in Kawase)
        // However, due to downsampling, the actual output might be smaller
        // For now, return the child output bounds
        return childOutput;
    } else {
        return skif::LayerSpace<SkIRect>::Unbounded();
    }
}

SkRect SkKawaseBlurImageFilter::computeFastBounds(const SkRect& src) const {
    SkRect bounds = this->getInput(0) ? this->getInput(0)->computeFastBounds(src) : src;
    // Kawase blur doesn't significantly expand bounds like Gaussian blur
    // The downsampling actually reduces the effective output size
    return bounds;
}
