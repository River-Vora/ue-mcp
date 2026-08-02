#include "AssetHandlers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HandlerUtils.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	constexpr int32 DefaultRenderTargetSize = 512;
	constexpr int32 MaxRenderTargetSize = 16384;

	bool TryParseRenderTargetFormat(const FString& Value, ETextureRenderTargetFormat& OutFormat, FString& OutCanonical)
	{
		struct FFormatEntry
		{
			const TCHAR* Name;
			ETextureRenderTargetFormat Format;
		};

		static const FFormatEntry Formats[] = {
			{TEXT("R8"), RTF_R8},
			{TEXT("RG8"), RTF_RG8},
			{TEXT("RGBA8"), RTF_RGBA8},
			{TEXT("RGBA8_SRGB"), RTF_RGBA8_SRGB},
			{TEXT("R16F"), RTF_R16f},
			{TEXT("RG16F"), RTF_RG16f},
			{TEXT("RGBA16F"), RTF_RGBA16f},
			{TEXT("R32F"), RTF_R32f},
			{TEXT("RG32F"), RTF_RG32f},
			{TEXT("RGBA32F"), RTF_RGBA32f},
			{TEXT("RGB10A2"), RTF_RGB10A2},
		};

		for (const FFormatEntry& Entry : Formats)
		{
			if (Value.Equals(Entry.Name, ESearchCase::IgnoreCase))
			{
				OutFormat = Entry.Format;
				OutCanonical = Entry.Name;
				return true;
			}
		}
		return false;
	}

	FString RenderTargetFormatToString(ETextureRenderTargetFormat Format)
	{
		FString Canonical;
		ETextureRenderTargetFormat Parsed = RTF_RGBA8_SRGB;
		const TCHAR* Candidates[] = {
			TEXT("R8"), TEXT("RG8"), TEXT("RGBA8"), TEXT("RGBA8_SRGB"),
			TEXT("R16F"), TEXT("RG16F"), TEXT("RGBA16F"), TEXT("R32F"),
			TEXT("RG32F"), TEXT("RGBA32F"), TEXT("RGB10A2")
		};
		for (const TCHAR* Candidate : Candidates)
		{
			if (TryParseRenderTargetFormat(Candidate, Parsed, Canonical) && Parsed == Format)
			{
				return Canonical;
			}
		}
		return TEXT("UNKNOWN");
	}

	void SetColorResult(TSharedPtr<FJsonObject> Result, const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> ColorObject = MakeShared<FJsonObject>();
		ColorObject->SetNumberField(TEXT("r"), Color.R);
		ColorObject->SetNumberField(TEXT("g"), Color.G);
		ColorObject->SetNumberField(TEXT("b"), Color.B);
		ColorObject->SetNumberField(TEXT("a"), Color.A);
		Result->SetObjectField(TEXT("clearColor"), ColorObject);
	}

	TSharedPtr<FJsonValue> MakeRenderTargetResult(UTextureRenderTarget2D* RenderTarget, bool bCreated)
	{
		auto Result = MCPSuccess();
		if (bCreated)
		{
			MCPSetCreated(Result);
			MCPSetDeleteAssetRollback(Result, RenderTarget->GetPathName());
		}
		else
		{
			MCPSetExisted(Result);
		}
		Result->SetStringField(TEXT("assetPath"), RenderTarget->GetPathName());
		Result->SetStringField(TEXT("path"), RenderTarget->GetPathName());
		Result->SetStringField(TEXT("name"), RenderTarget->GetName());
		Result->SetStringField(TEXT("packagePath"), FPackageName::GetLongPackagePath(RenderTarget->GetOutermost()->GetName()));
		Result->SetNumberField(TEXT("width"), RenderTarget->SizeX);
		Result->SetNumberField(TEXT("height"), RenderTarget->SizeY);
		Result->SetStringField(TEXT("format"), RenderTargetFormatToString(RenderTarget->RenderTargetFormat));
		SetColorResult(Result, RenderTarget->ClearColor);
		Result->SetBoolField(TEXT("generateMips"), RenderTarget->bAutoGenerateMips);
		Result->SetNumberField(TEXT("targetGamma"), RenderTarget->TargetGamma);
		return MCPResult(Result);
	}

	bool TryReadColor(const TSharedPtr<FJsonObject>& Params, FLinearColor& OutColor, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		if (!Params->HasField(TEXT("clearColor")))
		{
			return true;
		}
		if (!Params->TryGetObjectField(TEXT("clearColor"), ColorObject) || !ColorObject || !ColorObject->IsValid())
		{
			OutError = TEXT("clearColor must be an object with numeric r, g, b, and a channels");
			return false;
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 0.0;
		(*ColorObject)->TryGetNumberField(TEXT("r"), R);
		(*ColorObject)->TryGetNumberField(TEXT("g"), G);
		(*ColorObject)->TryGetNumberField(TEXT("b"), B);
		(*ColorObject)->TryGetNumberField(TEXT("a"), A);
		if (!FMath::IsFinite(R) || !FMath::IsFinite(G) || !FMath::IsFinite(B) || !FMath::IsFinite(A))
		{
			OutError = TEXT("clearColor channels must be finite numbers");
			return false;
		}
		OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
		return true;
	}
}

TSharedPtr<FJsonValue> FAssetHandlers::CreateRenderTarget2D(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Error = RequireString(Params, TEXT("name"), Name)) return Error;
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")))
	{
		return MCPError(TEXT("name must be a non-empty Unreal asset name without '/' or '.'"));
	}

	FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game"));
	PackagePath.TrimStartAndEndInline();
	while (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);
	if (!FPackageName::IsValidLongPackageName(PackagePath, true))
	{
		return MCPError(FString::Printf(TEXT("Invalid packagePath: %s"), *PackagePath));
	}
	const FString LowerPackagePath = PackagePath.ToLower();
	if (LowerPackagePath == TEXT("/engine") || LowerPackagePath.StartsWith(TEXT("/engine/")) ||
		LowerPackagePath == TEXT("/script") || LowerPackagePath.StartsWith(TEXT("/script/")) ||
		LowerPackagePath == TEXT("/memory") || LowerPackagePath.StartsWith(TEXT("/memory/")) ||
		LowerPackagePath == TEXT("/temp") || LowerPackagePath.StartsWith(TEXT("/temp/")))
	{
		return MCPError(FString::Printf(TEXT("Refusing to create an asset in protected mount: %s"), *PackagePath));
	}

	const int32 Width = OptionalInt(Params, TEXT("width"), DefaultRenderTargetSize);
	const int32 Height = OptionalInt(Params, TEXT("height"), DefaultRenderTargetSize);
	if (Width < 1 || Width > MaxRenderTargetSize || Height < 1 || Height > MaxRenderTargetSize)
	{
		return MCPError(FString::Printf(TEXT("width and height must be between 1 and %d"), MaxRenderTargetSize));
	}

	FString OnConflict = OptionalString(Params, TEXT("onConflict"), TEXT("skip"));
	OnConflict.ToLowerInline();
	if (OnConflict != TEXT("skip") && OnConflict != TEXT("error"))
	{
		return MCPError(TEXT("onConflict must be 'skip' or 'error'"));
	}

	ETextureRenderTargetFormat RenderTargetFormat = RTF_RGBA8_SRGB;
	FString CanonicalFormat;
	const FString Format = OptionalString(Params, TEXT("format"), TEXT("RGBA8_SRGB"));
	if (!TryParseRenderTargetFormat(Format, RenderTargetFormat, CanonicalFormat))
	{
		return MCPError(TEXT("format must be one of R8, RG8, RGBA8, RGBA8_SRGB, R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F, RGB10A2"));
	}

	FLinearColor ClearColor = FLinearColor::Transparent;
	FString ColorError;
	if (!TryReadColor(Params, ClearColor, ColorError)) return MCPError(ColorError);
	const bool bGenerateMips = OptionalBool(Params, TEXT("generateMips"), false);
	const double TargetGamma = OptionalNumber(Params, TEXT("targetGamma"), 0.0);
	if (!FMath::IsFinite(TargetGamma) || TargetGamma < 0.0)
	{
		return MCPError(TEXT("targetGamma must be a finite number greater than or equal to 0"));
	}

	const FString PackageName = PackagePath + TEXT("/") + Name;
	if (!FPackageName::IsValidLongPackageName(PackageName, true))
	{
		return MCPError(FString::Printf(TEXT("Invalid render target package name: %s"), *PackageName));
	}
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	if (UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath))
	{
		if (OnConflict == TEXT("error"))
		{
			return MCPError(FString::Printf(TEXT("TextureRenderTarget2D '%s' already exists"), *ObjectPath));
		}
		UTextureRenderTarget2D* ExistingRenderTarget = Cast<UTextureRenderTarget2D>(ExistingObject);
		if (!ExistingRenderTarget)
		{
			return MCPError(FString::Printf(TEXT("Asset '%s' exists but is not a TextureRenderTarget2D"), *ObjectPath));
		}
		return MakeRenderTargetResult(ExistingRenderTarget, false);
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MCPError(FString::Printf(TEXT("Failed to create package '%s'"), *PackageName));
	}

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(
		Package, UTextureRenderTarget2D::StaticClass(), *Name, RF_Public | RF_Standalone);
	if (!RenderTarget)
	{
		return MCPError(FString::Printf(TEXT("Failed to construct TextureRenderTarget2D '%s'"), *Name));
	}

	RenderTarget->RenderTargetFormat = RenderTargetFormat;
	RenderTarget->ClearColor = ClearColor;
	RenderTarget->bAutoGenerateMips = bGenerateMips;
	RenderTarget->TargetGamma = static_cast<float>(TargetGamma);
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);
	FAssetRegistryModule::AssetCreated(RenderTarget);
	RenderTarget->MarkPackageDirty();

	if (!SaveAssetPackage(RenderTarget))
	{
		return MCPError(FString::Printf(TEXT("Created TextureRenderTarget2D '%s' but failed to save its package"), *ObjectPath));
	}

	return MakeRenderTargetResult(RenderTarget, true);
}
