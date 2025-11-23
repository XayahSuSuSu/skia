/*
 * Copyright 2020 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm/gm.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSize.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#include "tools/Resources.h"

class KawaseBlurImageFilterTest : public skiagm::GM {
public:
    KawaseBlurImageFilterTest() {}
    SkString getName() const override { return SkString("newgmtest"); }
    SkISize getISize() override { return {300, 300}; }

    void onOnceBeforeDraw() override { fMandrill = GetResourceAsImage("images/desktop.jpg"); }

    void onDraw(SkCanvas* canvas) override {
        float sigma = 80.0f;
        sk_sp<SkImageFilter> blurFilter =
                SkImageFilters::Blur(sigma, sigma, SkTileMode::kClamp, nullptr);
        sk_sp<SkImageFilter> imageFilter = SkImageFilters::KawaseBlur(80.0f, nullptr);
        SkPaint blurPaint;
        blurPaint.setImageFilter(blurFilter);
        canvas->drawImage(fMandrill, 0, 0, SkSamplingOptions(), &blurPaint);
    }

private:
    sk_sp<SkImage> fMandrill;
};
DEF_GM(return new KawaseBlurImageFilterTest;)
