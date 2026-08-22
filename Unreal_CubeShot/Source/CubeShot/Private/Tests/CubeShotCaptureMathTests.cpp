#include "CubeShotCaptureService.h"

#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"

namespace
{
    TArray<FVector> MakeCorners(const FBoxSphereBounds& Bounds)
    {
        TArray<FVector> Corners;
        for (int32 XSign = -1; XSign <= 1; XSign += 2)
        {
            for (int32 YSign = -1; YSign <= 1; YSign += 2)
            {
                for (int32 ZSign = -1; ZSign <= 1; ZSign += 2)
                {
                    Corners.Add(Bounds.Origin + FVector(
                        Bounds.BoxExtent.X * XSign,
                        Bounds.BoxExtent.Y * YSign,
                        Bounds.BoxExtent.Z * ZSign));
                }
            }
        }
        return Corners;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotCameraBasisTest,
    "CubeShot.Editor.Capture.CameraBasis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotCameraBasisTest::RunTest(const FString& Parameters)
{
    const FBoxSphereBounds Bounds(FVector(10.0, 20.0, 30.0), FVector(5.0, 6.0, 7.0), 10.0);
    for (const FCubeShotDirectionInfo& DirectionInfo : CubeShot::GetDirectionInfos())
    {
        const FTransform CameraTransform = FCubeShotCaptureService::MakeCameraTransform(Bounds, DirectionInfo.Direction, 100.0);
        const FVector ExpectedLocation = Bounds.Origin + DirectionInfo.CameraAxis * 100.0;
        TestTrue(
            *FString::Printf(TEXT("%s camera location"), DirectionInfo.Label),
            CameraTransform.GetLocation().Equals(ExpectedLocation, 0.001));
        TestTrue(
            *FString::Printf(TEXT("%s camera forward"), DirectionInfo.Label),
            CameraTransform.GetUnitAxis(EAxis::X).Equals(DirectionInfo.ViewDirection, 0.001));
        TestTrue(
            *FString::Printf(TEXT("%s camera up/roll"), DirectionInfo.Label),
            CameraTransform.GetUnitAxis(EAxis::Z).Equals(DirectionInfo.UpVector, 0.001));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotAutoFitTest,
    "CubeShot.Editor.Capture.AutoFitEightCorners",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotAutoFitTest::RunTest(const FString& Parameters)
{
    const FBoxSphereBounds Bounds(FVector(13.0, -7.0, 21.0), FVector(80.0, 35.0, 12.0), 90.0);
    FCubeShotAutoFitResult AutoFit;
    FText Error;
    TestTrue(
        TEXT("Auto Fit succeeds for asymmetric bounds"),
        FCubeShotCaptureService::CalculateAutoFitForBounds(Bounds, 45.0, 0.9, AutoFit, Error));

    const double Tangent = FMath::Tan(FMath::DegreesToRadians(22.5));
    const TArray<FVector> Corners = MakeCorners(Bounds);
    for (const FCubeShotDirectionInfo& DirectionInfo : CubeShot::GetDirectionInfos())
    {
        const FVector Forward = DirectionInfo.ViewDirection;
        const FVector Right = FVector::CrossProduct(DirectionInfo.UpVector, Forward).GetSafeNormal();
        const FVector Up = FVector::CrossProduct(Forward, Right).GetSafeNormal();
        double MinimumDepth = TNumericLimits<double>::Max();
        double MaximumDepth = TNumericLimits<double>::Lowest();

        for (const FVector& Corner : Corners)
        {
            const FVector Offset = Corner - Bounds.Origin;
            const double Depth = AutoFit.CameraDistance + FVector::DotProduct(Offset, Forward);
            const double ScreenExtent = FMath::Max(
                FMath::Abs(FVector::DotProduct(Offset, Right)),
                FMath::Abs(FVector::DotProduct(Offset, Up)));
            TestTrue(
                *FString::Printf(TEXT("%s perspective corner fits 90%% frame"), DirectionInfo.Label),
                ScreenExtent <= Depth * Tangent * 0.9 + 0.001);
            TestTrue(
                *FString::Printf(TEXT("%s orthographic corner fits 90%% frame"), DirectionInfo.Label),
                ScreenExtent <= AutoFit.OrthoWidth * 0.5 * 0.9 + 0.001);
            MinimumDepth = FMath::Min(MinimumDepth, Depth);
            MaximumDepth = FMath::Max(MaximumDepth, Depth);
        }

        TestTrue(
            *FString::Printf(TEXT("%s near plane has clipping margin"), DirectionInfo.Label),
            AutoFit.NearPlane < MinimumDepth);
        TestTrue(
            *FString::Printf(TEXT("%s far plane has clipping margin"), DirectionInfo.Label),
            AutoFit.FarPlane > MaximumDepth);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotSupersamplePolicyTest,
    "CubeShot.Editor.Capture.SupersamplePolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotSupersamplePolicyTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("512 captures at 2x"), FCubeShotCaptureService::GetCaptureResolution(512), 1024);
    TestEqual(TEXT("2048 captures at 2x"), FCubeShotCaptureService::GetCaptureResolution(2048), 4096);
    TestEqual(TEXT("4096 captures at 1x"), FCubeShotCaptureService::GetCaptureResolution(4096), 4096);
    return true;
}
