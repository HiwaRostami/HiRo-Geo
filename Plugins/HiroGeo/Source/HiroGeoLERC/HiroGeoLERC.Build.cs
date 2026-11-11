// Copyright Hiro. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

// ============================================================================
// HiroGeoLERC
//
// Bridge module that wraps Esri's open-source LERC (Limited Error Raster
// Compression) decoder so HiroGeo can decode the elevation tiles served by
// elevation3d.arcgis.com (the same World Elevation 3D dataset streets.gl
// uses). Mirrors the existing HiroGeoCGAL bridge-module pattern: HiroGeo
// depends on this module, this module owns/builds the third-party code.
//
// The vendored LERC source lives under Private/ThirdParty/Lerc. It is
// Esri's official decoder (Apache License 2.0, see ThirdParty/Lerc/LICENSE
// and NOTICE), unmodified, compiled directly into this module rather than
// linked as a prebuilt static/shared library — LERC has zero external
// dependencies (no CGAL/Boost-style burden here), so this is the simplest
// and most portable option across all UE target platforms.
// ============================================================================
public class HiroGeoLERC : ModuleRules
{
	public HiroGeoLERC(ReadOnlyTargetRules Target) : base(Target)
	{
		// NoPCHs (نه Shared/Explicit PCH): سورس خام Lerc فرض می‌کنه هدر خودش
		// (BitMask.h / Lerc.h / BitStuffer.h / ...) literally اولین هدریه که
		// توی هر translation unit پردازش می‌شه. اگه از Shared/Explicit PCH
		// استفاده کنیم، UBT قبل از اولین #include خود فایل، CoreMinimal.h (و
		// در نتیجه Templates/UnrealTemplate.h) رو زورکی inject می‌کنه؛ اون
		// هدر یه تابع global به اسم BitMask() داره که با LercNS::BitMask
		// تصادم اسم پیدا می‌کنه (error C2872: ambiguous symbol) و باعث
		// می‌شه چک داخلی Lerc هم («Expected X.h to be first header included»)
		// fail بشه. غیرفعال کردن PCH برای این ماژول هر دو خطا رو حل می‌کنه،
		// چون دیگه هیچ هدر یونریلی قبل از هدرهای خود Lerc وارد نمی‌شه.
		PCHUsage = PCHUsageMode.NoPCHs;

		// به همین دلیل Unity Build هم برای این ماژول خاموشه: نمی‌خوایم چند
		// فایل Lerc (یا فایل‌های دیگه‌ی ماژول) توی یه translation unit واحد
		// merge بشن و سر همین مشکلات header-order با هم تداخل کنن.
		bUseUnity = false;

		// LercNS::lerc_assert() throws on internal sanity-check failures.
		// UE modules default to exceptions disabled; enable them for this
		// module only so that code path is well-defined instead of just a
		// compiler warning (MSVC C4530) or UB.
		bEnableExceptions = true;

		// سورس خام Esri Lerc چند تا وارنینگ کاملاً بی‌خطر MSVC داره (مثل
		// shadowed local variable توی BitStuffer.cpp/CntZImage.cpp) که توی
		// این پروژه به ارور تبدیل می‌شن (Warnings-as-Errors). به‌جای دستکاری
		// فایل‌های وندور (که طبق توضیح بالا باید دست‌نخورده بمونن)، فقط سطح
		// همین وارنینگ خاص رو برای این ماژول پایین می‌آریم.
		ShadowVariableWarningLevel = WarningLevel.Off;

		// نکته: اگه بعد از این هنوز خطاهای C4800 (implicit conversion به
		// bool، توی Lerc.cpp/Lerc2.cpp) یا C4702 (unreachable code، توی
		// fpl_Predictor.cpp) مونده بودن، یه خط پراگما درست بعد از اولین
		// #include خود همون فایل‌ها (نه قبلش، که چک header-order خراب نشه)
		// قابل‌اعتمادترین راهه و هیچ منطقی از Lerc رو تغییر نمی‌ده:
		//   #pragma warning(disable: 4800 4702)

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		// Tells Lerc_c_api.h to NOT apply __declspec(dllimport)/(dllexport).
		// We are compiling the LERC sources as ordinary internal translation
		// units of this module, not linking against a separate LERC DLL.
		PublicDefinitions.Add("LERC_STATIC=1");

		string ThirdPartyLercDir = Path.Combine(ModuleDirectory, "Private", "ThirdParty", "Lerc");

		PrivateIncludePaths.AddRange(new string[]
		{
			ThirdPartyLercDir,
			Path.Combine(ThirdPartyLercDir, "include"),
			Path.Combine(ThirdPartyLercDir, "Lerc1Decode")
		});
	}
}