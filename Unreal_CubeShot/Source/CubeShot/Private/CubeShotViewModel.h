#pragma once

#include "CoreMinimal.h"
#include "CubeShotTypes.h"
#include "UObject/StrongObjectPtr.h"

class UStaticMesh;
class UTexture2D;

/**
 * UI state kept independent of Slate so validation and control-state rules can be
 * exercised by automation tests without opening the CubeShot tab.
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
	void RequestPreviewRecapture();

	bool Validate();
	bool CanGenerate() const { return bOptionsValid && !bGenerating; }
	bool AreOptionsEnabled() const { return !bGenerating; }
	bool IsBackgroundTextureEnabled() const;
	bool IsFieldOfViewEnabled() const;
	bool IsOrthoWidthEnabled() const;
	bool IsCameraDistanceEnabled() const;
	bool IsGenerating() const { return bGenerating; }
	void SetGenerating(bool bInGenerating);

	const FText& GetValidationError() const { return ValidationError; }
	const FText& GetValidationWarning() const { return ValidationWarning; }
	uint64 GetRevision() const { return Revision; }
	EChangeKind GetLastChangeKind() const { return LastChangeKind; }

private:
	void MarkChanged(EChangeKind ChangeKind);
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
	uint64 Revision = 0;
	EChangeKind LastChangeKind = EChangeKind::Recapture;
	bool bOptionsValid = false;
	bool bGenerating = false;
	bool bHasBackgroundTextureSourceResult = false;
	bool bCachedBackgroundTextureSourceValid = false;
};
