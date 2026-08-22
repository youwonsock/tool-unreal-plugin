#if WITH_DEV_AUTOMATION_TESTS

#include "CubeShotViewModel.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeShotViewModelDefaultsTest,
	"CubeShot.Editor.ViewModel.DefaultsAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotViewModelDefaultsTest::RunTest(const FString& Parameters)
{
	FCubeShotViewModel ViewModel;
	const FCubeShotOptions& Defaults = ViewModel.GetOptions();
	TestEqual(TEXT("Default resolution"), Defaults.Resolution, 1024);
	TestEqual(TEXT("Default FOV"), Defaults.FieldOfView, 45.0);
	TestEqual(TEXT("Default background"), Defaults.BackgroundMode, ECubeShotBackgroundMode::Transparent);
	TestEqual(TEXT("Default projection"), Defaults.ProjectionMode, ECubeShotProjectionMode::Perspective);
	TestTrue(TEXT("Auto Fit defaults on"), Defaults.bAutoFit);
	TestFalse(TEXT("Generate is disabled without a mesh"), ViewModel.CanGenerate());
	TestFalse(TEXT("Auto Fit locks camera distance"), ViewModel.IsCameraDistanceEnabled());
	TestTrue(TEXT("Perspective enables FOV"), ViewModel.IsFieldOfViewEnabled());
	TestFalse(TEXT("Perspective disables Ortho Width"), ViewModel.IsOrthoWidthEnabled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeShotViewModelControlStateTest,
	"CubeShot.Editor.ViewModel.ControlStateAndInvalidInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotViewModelControlStateTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine test cube"), Cube))
	{
		return false;
	}

	FCubeShotViewModel ViewModel;
	ViewModel.SetStaticMesh(Cube);
	TestTrue(TEXT("A valid mesh enables generation"), ViewModel.CanGenerate());

	ViewModel.SetProjectionMode(ECubeShotProjectionMode::Orthographic);
	ViewModel.SetAutoFit(false);
	TestFalse(TEXT("Orthographic disables FOV"), ViewModel.IsFieldOfViewEnabled());
	TestTrue(TEXT("Manual orthographic enables width"), ViewModel.IsOrthoWidthEnabled());
	TestTrue(TEXT("Manual fit enables distance"), ViewModel.IsCameraDistanceEnabled());

	ViewModel.SetResolution(255);
	TestFalse(TEXT("Out-of-range resolution disables generation"), ViewModel.CanGenerate());
	TestFalse(TEXT("Invalid input exposes an error"), ViewModel.GetValidationError().IsEmpty());

	ViewModel.SetResolution(1000);
	TestTrue(TEXT("In-range NPOT is valid"), ViewModel.CanGenerate());
	TestFalse(TEXT("NPOT exposes a mip warning"), ViewModel.GetValidationWarning().IsEmpty());

	ViewModel.SetBackgroundMode(ECubeShotBackgroundMode::Texture);
	TestFalse(TEXT("Texture mode requires a texture"), ViewModel.CanGenerate());
	TestTrue(TEXT("Texture picker is enabled in Texture mode"), ViewModel.IsBackgroundTextureEnabled());

	UTexture2D* InvalidSourceTexture = NewObject<UTexture2D>();
	ViewModel.SetBackgroundTexture(InvalidSourceTexture);
	TestFalse(TEXT("Texture mode rejects a texture without source art"), ViewModel.CanGenerate());
	TestTrue(
		TEXT("Unreadable source art is reported inline"),
		ViewModel.GetValidationError().ToString().Contains(TEXT("source"), ESearchCase::IgnoreCase));

	UTexture2D* ReadableSourceTexture = NewObject<UTexture2D>();
	const TArray<FColor> SourcePixels =
	{
		FColor::Red, FColor::Green,
		FColor::Blue, FColor::White
	};
	ReadableSourceTexture->Source.Init(
		2,
		2,
		1,
		1,
		TSF_BGRA8,
		reinterpret_cast<const uint8*>(SourcePixels.GetData()));
	ViewModel.SetBackgroundTexture(ReadableSourceTexture);
	TestTrue(TEXT("Texture mode accepts readable mip 0 source art"), ViewModel.CanGenerate());
	ViewModel.SetCameraDistance(250.0);
	TestTrue(TEXT("Unrelated numeric changes retain the cached source validation"), ViewModel.CanGenerate());

	ViewModel.SetBackgroundMode(ECubeShotBackgroundMode::SolidColor);
	ViewModel.SetGenerating(true);
	TestFalse(TEXT("Generation locks all options"), ViewModel.AreOptionsEnabled());
	TestFalse(TEXT("Generation disables Generate"), ViewModel.CanGenerate());
	return true;
}

#endif
