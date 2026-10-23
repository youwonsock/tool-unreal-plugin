#include "CubeShotViewModel.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"

FCubeShotViewModel::FCubeShotViewModel()
{
	Validate();
}

FCubeShotOptions FCubeShotViewModel::GetOptionsCopy() const
{
	FCubeShotOptions Copy = Options;
	Copy.StaticMesh = StaticMesh.Get();
	Copy.BackgroundTexture = BackgroundTexture.Get();
	return Copy;
}

void FCubeShotViewModel::SetStaticMesh(UStaticMesh* InStaticMesh)
{
	if (StaticMesh.Get() == InStaticMesh)
	{
		return;
	}
	StaticMesh.Reset(InStaticMesh);
	RefreshObjectPointers();
	Validate();
}

void FCubeShotViewModel::SetBackgroundTexture(UTexture2D* InTexture)
{
	if (BackgroundTexture.Get() == InTexture)
	{
		if (IsValid(InTexture)
			&& bHasBackgroundTextureSourceResult
			&& CachedBackgroundTextureSourceId != InTexture->Source.GetId())
		{
			InvalidateBackgroundTextureSourceCache();
			Validate();
		}
		return;
	}
	BackgroundTexture.Reset(InTexture);
	InvalidateBackgroundTextureSourceCache();
	RefreshObjectPointers();
	Validate();
}

void FCubeShotViewModel::SetBackgroundMode(const ECubeShotBackgroundMode InMode)
{
	if (Options.BackgroundMode != InMode)
	{
		Options.BackgroundMode = InMode;
		Validate();
	}
}

void FCubeShotViewModel::SetSolidColor(const FLinearColor& InColor)
{
	if (!Options.SolidColor.Equals(InColor))
	{
		Options.SolidColor = InColor;
		Validate();
	}
}

void FCubeShotViewModel::SetProjectionMode(const ECubeShotProjectionMode InMode)
{
	if (Options.ProjectionMode != InMode)
	{
		Options.ProjectionMode = InMode;
		Validate();
	}
}

void FCubeShotViewModel::SetCameraDistance(const double InDistance)
{
	if (!FMath::IsNearlyEqual(Options.CameraDistance, InDistance))
	{
		Options.CameraDistance = InDistance;
		Validate();
	}
}

void FCubeShotViewModel::SetFieldOfView(const double InFieldOfView)
{
	if (!FMath::IsNearlyEqual(Options.FieldOfView, InFieldOfView))
	{
		Options.FieldOfView = InFieldOfView;
		Validate();
	}
}

void FCubeShotViewModel::SetOrthoWidth(const double InOrthoWidth)
{
	if (!FMath::IsNearlyEqual(Options.OrthoWidth, InOrthoWidth))
	{
		Options.OrthoWidth = InOrthoWidth;
		Validate();
	}
}

void FCubeShotViewModel::SetResolution(const int32 InResolution)
{
	if (Options.Resolution != InResolution)
	{
		Options.Resolution = InResolution;
		Validate();
	}
}

void FCubeShotViewModel::SetAutoFit(const bool bInAutoFit)
{
	if (Options.bAutoFit != bInAutoFit)
	{
		Options.bAutoFit = bInAutoFit;
		Validate();
	}
}

void FCubeShotViewModel::ApplyAutoFit(const FCubeShotAutoFitResult& Result)
{
	if (FMath::IsNearlyEqual(Options.CameraDistance, Result.CameraDistance)
		&& FMath::IsNearlyEqual(Options.OrthoWidth, Result.OrthoWidth))
	{
		return;
	}
	Options.CameraDistance = Result.CameraDistance;
	Options.OrthoWidth = Result.OrthoWidth;
	Validate();
}

bool FCubeShotViewModel::Validate()
{
	RefreshObjectPointers();
	FText BackgroundTextureSourceError;
	const bool bBackgroundTextureSourceValid = Options.BackgroundMode != ECubeShotBackgroundMode::Texture
		|| !IsValid(Options.BackgroundTexture)
		|| ValidateCachedBackgroundTextureSource(BackgroundTextureSourceError);
	bool bOptionsValid = CubeShot::ValidateOptions(Options, ValidationError, ValidationWarning);
	if (!bBackgroundTextureSourceValid)
	{
		bOptionsValid = false;
		ValidationError = BackgroundTextureSourceError;
		ValidationWarning = FText::GetEmpty();
	}
	return bOptionsValid;
}

bool FCubeShotViewModel::IsBackgroundTextureEnabled() const
{
	return AreOptionsEnabled() && Options.BackgroundMode == ECubeShotBackgroundMode::Texture;
}

bool FCubeShotViewModel::IsFieldOfViewEnabled() const
{
	return AreOptionsEnabled() && Options.ProjectionMode == ECubeShotProjectionMode::Perspective;
}

bool FCubeShotViewModel::IsOrthoWidthEnabled() const
{
	return AreOptionsEnabled() && Options.ProjectionMode == ECubeShotProjectionMode::Orthographic && !Options.bAutoFit;
}

bool FCubeShotViewModel::IsCameraDistanceEnabled() const
{
	return AreOptionsEnabled() && !Options.bAutoFit;
}

void FCubeShotViewModel::SetGenerating(const bool bInGenerating)
{
	bGenerating = bInGenerating;
}

void FCubeShotViewModel::RefreshObjectPointers()
{
	Options.StaticMesh = StaticMesh.Get();
	Options.BackgroundTexture = BackgroundTexture.Get();
}

bool FCubeShotViewModel::ValidateCachedBackgroundTextureSource(FText& OutError)
{
	UTexture2D* Texture = BackgroundTexture.Get();
	if (!IsValid(Texture))
	{
		OutError = FText::GetEmpty();
		return false;
	}

	const FGuid SourceId = Texture->Source.GetId();
	if (!bHasBackgroundTextureSourceResult
		|| CachedBackgroundTexture != Texture
		|| CachedBackgroundTextureSourceId != SourceId)
	{
		CachedBackgroundTexture = Texture;
		CachedBackgroundTextureSourceId = SourceId;
		bCachedBackgroundTextureSourceValid = CubeShot::ValidateBackgroundTextureSource(
			Texture,
			true,
			CachedBackgroundTextureSourceError);
		bHasBackgroundTextureSourceResult = true;
	}

	OutError = CachedBackgroundTextureSourceError;
	return bCachedBackgroundTextureSourceValid;
}

void FCubeShotViewModel::InvalidateBackgroundTextureSourceCache()
{
	CachedBackgroundTexture = nullptr;
	CachedBackgroundTextureSourceId.Invalidate();
	CachedBackgroundTextureSourceError = FText::GetEmpty();
	bHasBackgroundTextureSourceResult = false;
	bCachedBackgroundTextureSourceValid = false;
}
