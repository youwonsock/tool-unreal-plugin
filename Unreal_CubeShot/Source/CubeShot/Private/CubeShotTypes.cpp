#include "CubeShotTypes.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "StaticMeshResources.h"

#define LOCTEXT_NAMESPACE "CubeShotTypes"

namespace
{
    const TArray<FCubeShotDirectionInfo> GDirectionInfos =
    {
        {ECubeShotDirection::Front,  TEXT("Front"),  TEXT("Front"),  FVector::XAxisVector,  -FVector::XAxisVector, FVector::ZAxisVector},
        {ECubeShotDirection::Back,   TEXT("Back"),   TEXT("Back"),   -FVector::XAxisVector, FVector::XAxisVector,  FVector::ZAxisVector},
        {ECubeShotDirection::Left,   TEXT("Left"),   TEXT("Left"),   -FVector::YAxisVector, FVector::YAxisVector,  FVector::ZAxisVector},
        {ECubeShotDirection::Right,  TEXT("Right"),  TEXT("Right"),  FVector::YAxisVector,  -FVector::YAxisVector, FVector::ZAxisVector},
        {ECubeShotDirection::Top,    TEXT("Top"),    TEXT("Top"),    FVector::ZAxisVector,  -FVector::ZAxisVector, FVector::XAxisVector},
        {ECubeShotDirection::Bottom, TEXT("Bottom"), TEXT("Bottom"), -FVector::ZAxisVector, FVector::ZAxisVector,  FVector::XAxisVector}
    };
}

const TArray<FCubeShotDirectionInfo>& CubeShot::GetDirectionInfos()
{
    return GDirectionInfos;
}

const FCubeShotDirectionInfo& CubeShot::GetDirectionInfo(const ECubeShotDirection Direction)
{
    const int32 Index = static_cast<int32>(Direction);
    return GDirectionInfos.IsValidIndex(Index) ? GDirectionInfos[Index] : GDirectionInfos[0];
}

bool CubeShot::IsPowerOfTwoResolution(const int32 Resolution)
{
    return Resolution > 0 && FMath::IsPowerOfTwo(Resolution);
}

bool CubeShot::ValidateBackgroundTextureSource(
    UTexture2D* Texture,
    const bool bProbeMip0,
    FText& OutError)
{
    OutError = FText::GetEmpty();
    if (!IsValid(Texture))
    {
        OutError = LOCTEXT("NoBackgroundTexture", "Select a Texture2D for the texture background.");
        return false;
    }

    FTextureSource& Source = Texture->Source;
    if (!Source.IsValid())
    {
        OutError = LOCTEXT(
            "InvalidBackgroundTextureSource",
            "The selected background texture has no valid editor source data.");
        return false;
    }

    if (Source.GetSizeX() <= 0
        || Source.GetSizeY() <= 0
        || Source.GetNumMips() <= 0
        || Source.GetNumLayers() <= 0
        || Source.GetNumBlocks() <= 0
        || Source.GetFormat(0) == TSF_Invalid
        || Source.CalcMipSize(0) <= 0)
    {
        OutError = LOCTEXT(
            "InvalidBackgroundTextureDimensions",
            "The selected background texture has invalid source dimensions or no readable mip 0.");
        return false;
    }

    if (bProbeMip0)
    {
        FTextureSource::FMipLock MipLock(FTextureSource::ELockState::ReadOnly, &Source, 0);
        if (!MipLock.IsValid() || MipLock.GetDataSize() <= 0)
        {
            OutError = LOCTEXT(
                "UnreadableBackgroundTextureMip",
                "The selected background texture's mip 0 source data could not be read.");
            return false;
        }
    }

    return true;
}

bool CubeShot::ValidateOptions(const FCubeShotOptions& Options, FText& OutError, FText& OutWarning)
{
    OutError = FText::GetEmpty();
    OutWarning = FText::GetEmpty();

    if (!IsValid(Options.StaticMesh))
    {
        OutError = LOCTEXT("NoMesh", "Select a Static Mesh.");
        return false;
    }

    const FStaticMeshRenderData* RenderData = Options.StaticMesh->GetRenderData();
    if (RenderData == nullptr || RenderData->LODResources.IsEmpty() || RenderData->LODResources[0].GetNumVertices() == 0)
    {
        OutError = LOCTEXT("InvalidMeshRenderData", "The mesh has no valid LOD0 render data.");
        return false;
    }

    const FBoxSphereBounds Bounds = Options.StaticMesh->GetBounds();
    const bool bFiniteBounds = FMath::IsFinite(Bounds.Origin.X)
        && FMath::IsFinite(Bounds.Origin.Y)
        && FMath::IsFinite(Bounds.Origin.Z)
        && FMath::IsFinite(Bounds.BoxExtent.X)
        && FMath::IsFinite(Bounds.BoxExtent.Y)
        && FMath::IsFinite(Bounds.BoxExtent.Z);
    if (!bFiniteBounds || Bounds.BoxExtent.GetMax() <= UE_SMALL_NUMBER)
    {
        OutError = LOCTEXT("InvalidMeshBounds", "The mesh has invalid or empty bounds.");
        return false;
    }

    if (Options.BackgroundMode == ECubeShotBackgroundMode::Texture
        && !ValidateBackgroundTextureSource(Options.BackgroundTexture, false, OutError))
    {
        return false;
    }

    if (Options.Resolution < MinResolution || Options.Resolution > MaxResolution)
    {
        OutError = FText::Format(LOCTEXT("ResolutionRange", "Resolution must be between {0} and {1}."), MinResolution, MaxResolution);
        return false;
    }

    if (!FMath::IsFinite(Options.CameraDistance) || Options.CameraDistance <= 0.0)
    {
        OutError = LOCTEXT("InvalidDistance", "Camera Distance must be greater than zero.");
        return false;
    }

    if (!FMath::IsFinite(Options.FieldOfView) || Options.FieldOfView < 1.0 || Options.FieldOfView > 170.0)
    {
        OutError = LOCTEXT("InvalidFov", "FOV must be between 1 and 170 degrees.");
        return false;
    }

    if (!FMath::IsFinite(Options.OrthoWidth) || Options.OrthoWidth <= 0.0)
    {
        OutError = LOCTEXT("InvalidOrthoWidth", "Ortho Width must be greater than zero.");
        return false;
    }

    if (!IsPowerOfTwoResolution(Options.Resolution))
    {
        OutWarning = LOCTEXT("NoMipWarning", "Non-power-of-two output keeps its exact size and is saved without mipmaps.");
    }

    return true;
}

#undef LOCTEXT_NAMESPACE
