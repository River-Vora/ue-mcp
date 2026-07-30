// Landscape sculpting and weight painting (#742).
//
// The landscape category has always advertised "sculpting, painting, heightmap
// import", but no such action existed - the description promised capability the
// surface did not have, which is worse than a gap because a caller plans around
// it. These are the missing writes, done through FLandscapeEditDataInterface,
// the same path the landscape editor tools use.
//
// Translation-unit partition of FLandscapeHandlers; registrations live in
// LandscapeHandlers.cpp.

#include "LandscapeHandlers.h"

#include "HandlerUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "ScopedTransaction.h"

namespace
{
	// Landscape heights are uint16 with 32768 as "zero". One unit of height is
	// LANDSCAPE_ZSCALE (1/128) cm before the actor's own Z scale.
	constexpr uint16 LandscapeMidHeight = 32768;

	/** The landscape to edit: by actor label, else the only one in the world. */
	ALandscape* ResolveLandscape(UWorld* World, const FString& ActorLabel, FString& OutError)
	{
		TArray<ALandscape*> Found;
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			ALandscape* Candidate = *It;
			if (!Candidate) continue;
			if (!ActorLabel.IsEmpty() && !Candidate->GetActorLabel().Equals(ActorLabel, ESearchCase::IgnoreCase)) continue;
			Found.Add(Candidate);
		}
		if (Found.Num() == 0)
		{
			OutError = ActorLabel.IsEmpty()
				? TEXT("No Landscape actor in the current level. Create one with landscape(create).")
				: FString::Printf(TEXT("No Landscape actor labelled '%s'"), *ActorLabel);
			return nullptr;
		}
		if (Found.Num() > 1 && ActorLabel.IsEmpty())
		{
			TArray<FString> Labels;
			for (ALandscape* L : Found) Labels.Add(L->GetActorLabel());
			OutError = FString::Printf(
				TEXT("%d Landscape actors in the level; pass actorLabel to choose. Available: [%s]"),
				Found.Num(), *FString::Join(Labels, TEXT(", ")));
			return nullptr;
		}
		return Found[0];
	}

	/**
	 * Convert a world-space XY position and radius into the landscape's own
	 * quad grid, clamped to the loaded extent. Working in world units is what a
	 * caller has; working in quads is what the edit interface needs.
	 */
	bool WorldToLandscapeRect(
		ALandscape* Landscape,
		ULandscapeInfo* Info,
		const FVector2D& Center,
		double Radius,
		int32& OutX1, int32& OutY1, int32& OutX2, int32& OutY2,
		FString& OutError)
	{
		const FTransform ActorToWorld = Landscape->ActorToWorld();
		const FVector LocalCenter = ActorToWorld.InverseTransformPosition(FVector(Center.X, Center.Y, 0.0));
		const FVector Scale = ActorToWorld.GetScale3D();
		if (FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y))
		{
			OutError = TEXT("Landscape has a zero XY scale");
			return false;
		}
		const double RadiusQuadsX = FMath::Abs(Radius / Scale.X);
		const double RadiusQuadsY = FMath::Abs(Radius / Scale.Y);

		int32 MinX, MinY, MaxX, MaxY;
		if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
		{
			OutError = TEXT("Could not read the landscape extent (are its components loaded?)");
			return false;
		}

		OutX1 = FMath::Clamp(FMath::FloorToInt(LocalCenter.X - RadiusQuadsX), MinX, MaxX);
		OutY1 = FMath::Clamp(FMath::FloorToInt(LocalCenter.Y - RadiusQuadsY), MinY, MaxY);
		OutX2 = FMath::Clamp(FMath::CeilToInt(LocalCenter.X + RadiusQuadsX), MinX, MaxX);
		OutY2 = FMath::Clamp(FMath::CeilToInt(LocalCenter.Y + RadiusQuadsY), MinY, MaxY);

		if (OutX2 < OutX1 || OutY2 < OutY1)
		{
			OutError = TEXT("The requested area does not overlap the landscape");
			return false;
		}
		return true;
	}

	/** Smooth 0..1 brush weight for a point, given a falloff fraction. */
	double BrushWeight(double DistanceFraction, double Falloff)
	{
		if (DistanceFraction >= 1.0) return 0.0;
		const double Inner = FMath::Clamp(1.0 - Falloff, 0.0, 1.0);
		if (DistanceFraction <= Inner) return 1.0;
		const double T = (DistanceFraction - Inner) / FMath::Max(1.0 - Inner, KINDA_SMALL_NUMBER);
		// Smoothstep so a brush edge does not leave a visible ring.
		return 1.0 - (T * T * (3.0 - 2.0 * T));
	}
}

// landscape(sculpt): raise, lower or flatten a circular brush footprint.
TSharedPtr<FJsonValue> FLandscapeHandlers::Sculpt(const TSharedPtr<FJsonObject>& Params)
{
	REQUIRE_EDITOR_WORLD(World);

	FString ResolveError;
	ALandscape* Landscape = ResolveLandscape(World, OptionalString(Params, TEXT("actorLabel")), ResolveError);
	if (!Landscape) return MCPError(ResolveError);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info) return MCPError(TEXT("Landscape has no LandscapeInfo (not registered yet)"));

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MCPError(TEXT("Missing 'center' ({x, y} in world space)"));
	}
	FVector2D Center(0.0, 0.0);
	(*CenterObj)->TryGetNumberField(TEXT("x"), Center.X);
	(*CenterObj)->TryGetNumberField(TEXT("y"), Center.Y);

	const double Radius = OptionalNumber(Params, TEXT("radius"), 500.0);
	if (Radius <= 0.0) return MCPError(TEXT("'radius' must be positive"));
	const double Falloff = FMath::Clamp(OptionalNumber(Params, TEXT("falloff"), 0.5), 0.0, 1.0);
	const FString Mode = OptionalString(Params, TEXT("mode"), TEXT("raise")).ToLower();
	if (Mode != TEXT("raise") && Mode != TEXT("lower") && Mode != TEXT("flatten"))
	{
		return MCPError(TEXT("'mode' must be raise, lower or flatten"));
	}
	// World-space centimetres to move the surface at full brush strength.
	const double Amount = OptionalNumber(Params, TEXT("amount"), 100.0);

	int32 X1, Y1, X2, Y2;
	FString RectError;
	if (!WorldToLandscapeRect(Landscape, Info, Center, Radius, X1, Y1, X2, Y2, RectError))
	{
		return MCPError(RectError);
	}

	const int32 Width = X2 - X1 + 1;
	const int32 Height = Y2 - Y1 + 1;
	TArray<uint16> Heights;
	Heights.SetNumZeroed(Width * Height);

	FLandscapeEditDataInterface EditData(Info);
	EditData.GetHeightData(X1, Y1, X2, Y2, Heights.GetData(), 0);

	const FVector Scale = Landscape->ActorToWorld().GetScale3D();
	// uint16 height units per world centimetre.
	const double ZScale = FMath::IsNearlyZero(Scale.Z) ? 0.0 : (1.0 / (LANDSCAPE_ZSCALE * Scale.Z));
	if (ZScale == 0.0) return MCPError(TEXT("Landscape has a zero Z scale; heights cannot be written"));

	const FVector LocalCenter = Landscape->ActorToWorld().InverseTransformPosition(FVector(Center.X, Center.Y, 0.0));
	const double RadiusQuadsX = FMath::Abs(Radius / Scale.X);
	const double RadiusQuadsY = FMath::Abs(Radius / Scale.Y);

	// Flatten needs a target: the height under the brush centre before editing.
	double FlattenTarget = 0.0;
	if (Mode == TEXT("flatten"))
	{
		const int32 CX = FMath::Clamp(FMath::RoundToInt(LocalCenter.X), X1, X2);
		const int32 CY = FMath::Clamp(FMath::RoundToInt(LocalCenter.Y), Y1, Y2);
		FlattenTarget = (double)Heights[(CY - Y1) * Width + (CX - X1)];
	}

	int32 Touched = 0;
	for (int32 Y = Y1; Y <= Y2; ++Y)
	{
		for (int32 X = X1; X <= X2; ++X)
		{
			const double DX = (X - LocalCenter.X) / FMath::Max(RadiusQuadsX, KINDA_SMALL_NUMBER);
			const double DY = (Y - LocalCenter.Y) / FMath::Max(RadiusQuadsY, KINDA_SMALL_NUMBER);
			const double Fraction = FMath::Sqrt(DX * DX + DY * DY);
			const double Weight = BrushWeight(Fraction, Falloff);
			if (Weight <= 0.0) continue;

			const int32 Index = (Y - Y1) * Width + (X - X1);
			const double Current = (double)Heights[Index];
			double Next = Current;
			if (Mode == TEXT("raise"))       Next = Current + Amount * ZScale * Weight;
			else if (Mode == TEXT("lower"))  Next = Current - Amount * ZScale * Weight;
			else                             Next = FMath::Lerp(Current, FlattenTarget, Weight);

			Heights[Index] = (uint16)FMath::Clamp(FMath::RoundToInt(Next), 0, 65535);
			++Touched;
		}
	}

	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("MCP landscape sculpt")));
		Landscape->Modify();
		EditData.SetHeightData(X1, Y1, X2, Y2, Heights.GetData(), 0, /*InCalcNormals=*/true);
		EditData.Flush();
	}
	Landscape->PostEditChange();

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("landscape"), Landscape->GetActorLabel());
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetNumberField(TEXT("radius"), Radius);
	Result->SetNumberField(TEXT("amount"), Amount);
	Result->SetNumberField(TEXT("verticesTouched"), Touched);
	Result->SetNumberField(TEXT("rectX1"), X1);
	Result->SetNumberField(TEXT("rectY1"), Y1);
	Result->SetNumberField(TEXT("rectX2"), X2);
	Result->SetNumberField(TEXT("rectY2"), Y2);
	Result->SetStringField(TEXT("note"), TEXT("The level is left dirty and unsaved; save it when ready."));
	return MCPResult(Result);
}

// landscape(paint_layer): paint a weight layer over a circular footprint.
TSharedPtr<FJsonValue> FLandscapeHandlers::PaintLayer(const TSharedPtr<FJsonObject>& Params)
{
	REQUIRE_EDITOR_WORLD(World);

	FString ResolveError;
	ALandscape* Landscape = ResolveLandscape(World, OptionalString(Params, TEXT("actorLabel")), ResolveError);
	if (!Landscape) return MCPError(ResolveError);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info) return MCPError(TEXT("Landscape has no LandscapeInfo (not registered yet)"));

	FString LayerName;
	if (auto Err = RequireString(Params, TEXT("layerName"), LayerName)) return Err;

	// Resolve the layer against the landscape's registered target layers, so a
	// typo names the layers that DO exist instead of silently painting nothing.
	ULandscapeLayerInfoObject* LayerInfo = nullptr;
	TArray<FString> KnownLayers;
	for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
	{
		KnownLayers.Add(Layer.GetLayerName().ToString());
		if (Layer.GetLayerName().ToString().Equals(LayerName, ESearchCase::IgnoreCase))
		{
			LayerInfo = Layer.LayerInfoObj;
		}
	}
	if (!LayerInfo)
	{
		return MCPError(FString::Printf(
			TEXT("Paint layer '%s' has no LayerInfo on this landscape. Registered layers: [%s]. Use landscape(add_layer_info) first."),
			*LayerName, *FString::Join(KnownLayers, TEXT(", "))));
	}

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MCPError(TEXT("Missing 'center' ({x, y} in world space)"));
	}
	FVector2D Center(0.0, 0.0);
	(*CenterObj)->TryGetNumberField(TEXT("x"), Center.X);
	(*CenterObj)->TryGetNumberField(TEXT("y"), Center.Y);

	const double Radius = OptionalNumber(Params, TEXT("radius"), 500.0);
	if (Radius <= 0.0) return MCPError(TEXT("'radius' must be positive"));
	const double Falloff = FMath::Clamp(OptionalNumber(Params, TEXT("falloff"), 0.5), 0.0, 1.0);
	// 0..1 target weight at full brush strength.
	const double Strength = FMath::Clamp(OptionalNumber(Params, TEXT("strength"), 1.0), 0.0, 1.0);

	int32 X1, Y1, X2, Y2;
	FString RectError;
	if (!WorldToLandscapeRect(Landscape, Info, Center, Radius, X1, Y1, X2, Y2, RectError))
	{
		return MCPError(RectError);
	}

	const int32 Width = X2 - X1 + 1;
	const int32 Height = Y2 - Y1 + 1;
	TArray<uint8> Weights;
	Weights.SetNumZeroed(Width * Height);

	FLandscapeEditDataInterface EditData(Info);
	EditData.GetWeightData(LayerInfo, X1, Y1, X2, Y2, Weights.GetData(), 0);

	const FVector Scale = Landscape->ActorToWorld().GetScale3D();
	const FVector LocalCenter = Landscape->ActorToWorld().InverseTransformPosition(FVector(Center.X, Center.Y, 0.0));
	const double RadiusQuadsX = FMath::Abs(Radius / Scale.X);
	const double RadiusQuadsY = FMath::Abs(Radius / Scale.Y);
	const double TargetWeight = Strength * 255.0;

	int32 Touched = 0;
	for (int32 Y = Y1; Y <= Y2; ++Y)
	{
		for (int32 X = X1; X <= X2; ++X)
		{
			const double DX = (X - LocalCenter.X) / FMath::Max(RadiusQuadsX, KINDA_SMALL_NUMBER);
			const double DY = (Y - LocalCenter.Y) / FMath::Max(RadiusQuadsY, KINDA_SMALL_NUMBER);
			const double Weight = BrushWeight(FMath::Sqrt(DX * DX + DY * DY), Falloff);
			if (Weight <= 0.0) continue;

			const int32 Index = (Y - Y1) * Width + (X - X1);
			const double Blended = FMath::Lerp((double)Weights[Index], TargetWeight, Weight);
			Weights[Index] = (uint8)FMath::Clamp(FMath::RoundToInt(Blended), 0, 255);
			++Touched;
		}
	}

	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("MCP landscape paint")));
		Landscape->Modify();
		// bWeightAdjust normalises the other layers so the weights still sum to
		// 1; without it a painted layer silently fights whatever was there.
		EditData.SetAlphaData(LayerInfo, X1, Y1, X2, Y2, Weights.GetData(), 0,
			ELandscapeLayerPaintingRestriction::None, /*bWeightAdjust=*/true, /*bTotalWeightAdjust=*/false);
		EditData.Flush();
	}
	Landscape->PostEditChange();

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("landscape"), Landscape->GetActorLabel());
	Result->SetStringField(TEXT("layerName"), LayerName);
	Result->SetNumberField(TEXT("strength"), Strength);
	Result->SetNumberField(TEXT("radius"), Radius);
	Result->SetNumberField(TEXT("verticesTouched"), Touched);
	Result->SetStringField(TEXT("note"), TEXT("Other layer weights were renormalised. The level is left dirty and unsaved."));
	return MCPResult(Result);
}
