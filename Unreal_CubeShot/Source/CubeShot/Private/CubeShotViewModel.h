#pragma once

#include "CoreMinimal.h"
#include "CubeShotTypes.h"
#include "UObject/StrongObjectPtr.h"

class UStaticMesh;
class UTexture2D;

/**
 * Preview/display options and control-state validation. Workspace options and
 * batch admission belong to the injected controller.
 */
class FCubeShotViewModel final
{
public:
	enum class EChangeKind : uint8
	{
		None,
		CompositeOnly,
		Recapture
	};

	FCubeShotViewModel();

	const FCubeShotOptions& GetOptions() const { return Options; }
	FCubeShotOptions GetOptionsCopy() const;

	void SetStaticMesh(UStaticMesh* InStaticMesh);
	void SetBackgroundTexture(UTexture2D* InTexture);
	void SetBackgroundMode(ECubeShotBackgroundMode InMode);
	void SetSolidColor(const FLinearColor& InColor);
	void SetProjectionMode(ECubeShotProjectionMode InMode);
	void SetCameraDistance(double InDistance);
	void SetFieldOfView(double InFieldOfView);
	void SetOrthoWidth(double InOrthoWidth);
	void SetResolution(int32 InResolution);
	void SetAutoFit(bool bInAutoFit);
	void ApplyAutoFit(const FCubeShotAutoFitResult& Result);

	bool Validate();
	bool AreOptionsEnabled() const { return !bGenerating; }
	bool IsBackgroundTextureEnabled() const;
	bool IsFieldOfViewEnabled() const;
	bool IsOrthoWidthEnabled() const;
	bool IsCameraDistanceEnabled() const;
	bool IsGenerating() const { return bGenerating; }
	void SetGenerating(bool bInGenerating);

	const FText& GetValidationError() const { return ValidationError; }
	const FText& GetValidationWarning() const { return ValidationWarning; }

private:
	void RefreshObjectPointers();
	bool ValidateCachedBackgroundTextureSource(FText& OutError);
	void InvalidateBackgroundTextureSourceCache();

	FCubeShotOptions Options;
	TStrongObjectPtr<UStaticMesh> StaticMesh;
	TStrongObjectPtr<UTexture2D> BackgroundTexture;
	FText ValidationError;
	FText ValidationWarning;
	FText CachedBackgroundTextureSourceError;
	UTexture2D* CachedBackgroundTexture = nullptr;
	FGuid CachedBackgroundTextureSourceId;
	bool bGenerating = false;
	bool bHasBackgroundTextureSourceResult = false;
	bool bCachedBackgroundTextureSourceValid = false;
};
