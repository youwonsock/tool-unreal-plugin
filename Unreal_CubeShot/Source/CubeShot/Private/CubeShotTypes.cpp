#include "CubeShotTypes.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"
#include "Editor.h"
#include "Misc/App.h"
#include "RHIGlobals.h"

#define LOCTEXT_NAMESPACE "CubeShotTypes"

bool CubeShot::CanExecute(FText& OutError)
{
    return EvaluateExecutionPolicy(GEditor != nullptr, GEditor && GEditor->PlayWorld,
        GEditor && GEditor->bIsSimulatingInEditor, FApp::CanEverRender(), GUsingNullRHI,
        GDynamicRHI != nullptr, OutError);
}
bool CubeShot::EvaluateExecutionPolicy(bool bEditor, bool bPIE, bool bSIE, bool bCanRender,
    bool bNullRhi, bool bRhiPresent, FText& OutError)
{
    if (!bEditor) OutError = LOCTEXT("NoEditor", "CubeShot requires an Editor world.");
    else if (bPIE || bSIE) OutError = LOCTEXT("PlayBlocked", "Stop PIE or SIE before CubeShot capture.");
    else if (!bCanRender || bNullRhi || !bRhiPresent) OutError = LOCTEXT("NoRendering", "CubeShot requires a rendering Editor (no -nullrhi).");
    else { OutError = FText::GetEmpty(); return true; }
    return false;
}

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

bool CubeShot::ValidateBounds(const FBoxSphereBounds& Bounds, FText& OutError)
{
    const bool Finite=FMath::IsFinite(Bounds.Origin.X)&&FMath::IsFinite(Bounds.Origin.Y)&&FMath::IsFinite(Bounds.Origin.Z)
        &&FMath::IsFinite(Bounds.BoxExtent.X)&&FMath::IsFinite(Bounds.BoxExtent.Y)&&FMath::IsFinite(Bounds.BoxExtent.Z)
        &&FMath::IsFinite(Bounds.SphereRadius);
    if(!Finite||Bounds.BoxExtent.X<0||Bounds.BoxExtent.Y<0||Bounds.BoxExtent.Z<0
        ||Bounds.SphereRadius<0||Bounds.BoxExtent.GetMax()<=UE_SMALL_NUMBER)
    {OutError=LOCTEXT("InvalidMeshBounds","The mesh has invalid or empty bounds.");return false;}
    OutError=FText::GetEmpty();return true;
}

bool CubeShot::ValidateBoundsAndCamera(const FBoxSphereBounds& Bounds,const FCubeShotOptions& Options,FText& OutError)
{
    if(!ValidateBounds(Bounds,OutError))return false;
    if(!FMath::IsFinite(Options.CameraDistance)||Options.CameraDistance<=0)
    {OutError=LOCTEXT("InvalidDistance","Camera Distance must be greater than zero.");return false;}
    if(!FMath::IsFinite(Options.FieldOfView)||Options.FieldOfView<1||Options.FieldOfView>170)
    {OutError=LOCTEXT("InvalidFov","FOV must be between 1 and 170 degrees.");return false;}
    if(!FMath::IsFinite(Options.OrthoWidth)||Options.OrthoWidth<=0)
    {OutError=LOCTEXT("InvalidOrthoWidth","Ortho Width must be greater than zero.");return false;}
    const double MinimumDistance=Bounds.BoxExtent.GetMax()+MinimumNearPlane;
    if(!Options.bAutoFit&&Options.CameraDistance<=MinimumDistance)
    {
        OutError=FText::Format(LOCTEXT("ManualCameraInsideBounds",
            "Camera Distance must be greater than {0} cm so every view remains in front of the near plane."),FText::AsNumber(MinimumDistance));
        return false;
    }
    OutError=FText::GetEmpty();return true;
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
    if(!ValidateBoundsAndCamera(Bounds,Options,OutError))return false;

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

    if (!IsPowerOfTwoResolution(Options.Resolution))
    {
        OutWarning = LOCTEXT("NoMipWarning", "Non-power-of-two output keeps its exact size and is saved without mipmaps.");
    }

    if (Options.StaticMesh->GetNaniteSettings().bEnabled)
    {
        const FText NaniteWarning = LOCTEXT("NaniteFallbackWarning",
            "CubeShot disables Nanite and captures the fallback LOD0 mesh. Its silhouette may differ from the Nanite view.");
        OutWarning = OutWarning.IsEmpty() ? NaniteWarning
            : FText::Format(LOCTEXT("CombinedNaniteWarning", "{0}\n{1}"), OutWarning, NaniteWarning);
    }

    // Keep the panel, asset writer and MCP preflight on the same material policy.
    for (const FStaticMaterial& Slot : Options.StaticMesh->GetStaticMaterials())
    {
        const UMaterialInterface* Material = Slot.MaterialInterface;
        if (Material && Material->GetBlendMode() != BLEND_Opaque && Material->GetBlendMode() != BLEND_Masked)
        {
            const FText MaterialWarning = LOCTEXT("BestEffortMaterialWarning",
                "Translucent, additive, and refractive materials are captured on a best-effort basis. Inspect silhouette and alpha before using the result as an icon.");
            OutWarning = OutWarning.IsEmpty() ? MaterialWarning
                : FText::Format(LOCTEXT("CombinedWarning", "{0}\n{1}"), OutWarning, MaterialWarning);
            break;
        }
    }

    return true;
}

#undef LOCTEXT_NAMESPACE
