/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrTextureAdjuster.h"
#include "GrColorSpaceXform.h"
#include "GrContext.h"
#include "GrContextPriv.h"
#include "GrGpu.h"
#include "GrProxyProvider.h"
#include "SkGr.h"

GrTextureAdjuster::GrTextureAdjuster(GrContext* context, sk_sp<GrTextureProxy> original,
                                     SkAlphaType alphaType,
                                     uint32_t uniqueID,
                                     SkColorSpace* cs)
    : INHERITED(context, original->width(), original->height(),
                GrPixelConfigIsAlphaOnly(original->config()))
    , fOriginal(std::move(original))
    , fAlphaType(alphaType)
    , fColorSpace(cs)
    , fUniqueID(uniqueID) {}

void GrTextureAdjuster::makeCopyKey(const CopyParams& params, GrUniqueKey* copyKey) {
    // Destination color space is irrelevant - we already have a texture so we're just sub-setting
    GrUniqueKey baseKey;
    GrMakeKeyFromImageID(&baseKey, fUniqueID, SkIRect::MakeWH(this->width(), this->height()));
    MakeCopyKeyFromOrigKey(baseKey, params, copyKey);
}

void GrTextureAdjuster::didCacheCopy(const GrUniqueKey& copyKey, uint32_t contextUniqueID) {
    // We don't currently have a mechanism for notifications on Images!
}

sk_sp<GrTextureProxy> GrTextureAdjuster::refTextureProxyCopy(const CopyParams& copyParams,
                                                             bool willBeMipped) {
    GrProxyProvider* proxyProvider = fContext->contextPriv().proxyProvider();

    GrUniqueKey key;
    this->makeCopyKey(copyParams, &key);
    sk_sp<GrTextureProxy> cachedCopy;
    if (key.isValid()) {
        cachedCopy = proxyProvider->findOrCreateProxyByUniqueKey(key,
                                                                 this->originalProxy()->origin());
        if (cachedCopy && (!willBeMipped || GrMipMapped::kYes == cachedCopy->mipMapped())) {
            return cachedCopy;
        }
    }

    sk_sp<GrTextureProxy> proxy = this->originalProxyRef();

    sk_sp<GrTextureProxy> copy = CopyOnGpu(fContext, std::move(proxy), copyParams, willBeMipped);
    if (copy) {
        if (key.isValid()) {
            SkASSERT(copy->origin() == this->originalProxy()->origin());
            if (cachedCopy) {
                SkASSERT(GrMipMapped::kYes == copy->mipMapped() &&
                         GrMipMapped::kNo == cachedCopy->mipMapped());
                // If we had a cachedProxy, that means there already is a proxy in the cache which
                // matches the key, but it does not have mip levels and we require them. Thus we
                // must remove the unique key from that proxy.
                proxyProvider->removeUniqueKeyFromProxy(key, cachedCopy.get());
            }
            proxyProvider->assignUniqueKeyToProxy(key, copy.get());
            if (!proxyProvider->recordingDDL()) {
                // If we're recording a DDL we cannot add genID change listeners because
                // that process isn't thread safe
                this->didCacheCopy(key, proxyProvider->contextUniqueID());
            }
        }
    }
    return copy;
}

sk_sp<GrTextureProxy> GrTextureAdjuster::onRefTextureProxyForParams(
        const GrSamplerState& params,
        SkColorSpace* dstColorSpace,
        sk_sp<SkColorSpace>* texColorSpace,
        bool willBeMipped,
        SkScalar scaleAdjust[2]) {
    sk_sp<GrTextureProxy> proxy = this->originalProxyRef();
    CopyParams copyParams;

    if (!fContext) {
        // The texture was abandoned.
        return nullptr;
    }

    if (texColorSpace) {
        *texColorSpace = sk_ref_sp(fColorSpace);
    }
    SkASSERT(this->width() <= fContext->contextPriv().caps()->maxTextureSize() &&
             this->height() <= fContext->contextPriv().caps()->maxTextureSize());

    bool needsCopyForMipsOnly = false;
    if (!params.isRepeated() ||
        !GrGpu::IsACopyNeededForRepeatWrapMode(fContext->contextPriv().caps(), proxy.get(),
                                               proxy->width(), proxy->height(), params.filter(),
                                               &copyParams, scaleAdjust)) {
        needsCopyForMipsOnly = GrGpu::IsACopyNeededForMips(fContext->contextPriv().caps(),
                                                           proxy.get(), params.filter(),
                                                           &copyParams);
        if (!needsCopyForMipsOnly) {
            return proxy;
        }
    }

    sk_sp<GrTextureProxy> result = this->refTextureProxyCopy(copyParams, willBeMipped);
    if (!result && needsCopyForMipsOnly) {
        // If we were unable to make a copy and we only needed a copy for mips, then we will return
        // the source texture here and require that the GPU backend is able to fall back to using
        // bilerp if mips are required.
        return this->originalProxyRef();
    }
    return result;
}

std::unique_ptr<GrFragmentProcessor> GrTextureAdjuster::createFragmentProcessor(
        const SkMatrix& origTextureMatrix,
        const SkRect& constraintRect,
        FilterConstraint filterConstraint,
        bool coordsLimitedToConstraintRect,
        const GrSamplerState::Filter* filterOrNullForBicubic,
        SkColorSpace* dstColorSpace) {
    SkMatrix textureMatrix = origTextureMatrix;

    SkRect domain;
    GrSamplerState samplerState;
    if (filterOrNullForBicubic) {
        samplerState.setFilterMode(*filterOrNullForBicubic);
    }
    SkScalar scaleAdjust[2] = { 1.0f, 1.0f };
    sk_sp<GrTextureProxy> proxy(
            this->refTextureProxyForParams(samplerState, nullptr, nullptr, scaleAdjust));
    if (!proxy) {
        return nullptr;
    }
    // If we made a copy then we only copied the contentArea, in which case the new texture is all
    // content.
    if (proxy.get() != this->originalProxy()) {
        textureMatrix.postScale(scaleAdjust[0], scaleAdjust[1]);
    }

    DomainMode domainMode =
        DetermineDomainMode(constraintRect, filterConstraint, coordsLimitedToConstraintRect,
                            proxy.get(), filterOrNullForBicubic, &domain);
    if (kTightCopy_DomainMode == domainMode) {
        // TODO: Copy the texture and adjust the texture matrix (both parts need to consider
        // non-int constraint rect)
        // For now: treat as bilerp and ignore what goes on above level 0.

        // We only expect MIP maps to require a tight copy.
        SkASSERT(filterOrNullForBicubic &&
                 GrSamplerState::Filter::kMipMap == *filterOrNullForBicubic);
        static const GrSamplerState::Filter kBilerp = GrSamplerState::Filter::kBilerp;
        domainMode =
            DetermineDomainMode(constraintRect, filterConstraint, coordsLimitedToConstraintRect,
                                proxy.get(), &kBilerp, &domain);
        SkASSERT(kTightCopy_DomainMode != domainMode);
    }
    SkASSERT(kNoDomain_DomainMode == domainMode ||
             (domain.fLeft <= domain.fRight && domain.fTop <= domain.fBottom));
    auto fp = CreateFragmentProcessorForDomainAndFilter(std::move(proxy), textureMatrix,
                                                        domainMode, domain, filterOrNullForBicubic);
    return GrColorSpaceXformEffect::Make(std::move(fp), fColorSpace, fAlphaType, dstColorSpace);
}
