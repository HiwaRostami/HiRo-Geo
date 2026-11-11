#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Dom/JsonObject.h"
#include "Runtime/Online/HTTP/Public/Http.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Containers/Ticker.h"

class SWebBrowser;
class UHiroGeoJSBridge;

// ---- خارج از کلاس تعریف می‌شه تا operator== بدون مشکل کار کنه ----
enum class EHiroGeoSelectionMode : uint8 { Free, Grid };

class SHiroGeoImporterWindow : public SCompoundWidget
{
public:
    SLATE_USER_ARGS(SHiroGeoImporterWindow) {}
    SLATE_END_ARGS()

    /** Constructs this widget with InArgs */
    void Construct(const FArguments& InArgs);
    virtual ~SHiroGeoImporterWindow() override;

private:
    // ویجت مرورگر
    TSharedPtr<SWebBrowser> WebBrowser;
    // شیء ارتباط با JS
    UHiroGeoJSBridge* JSBridge;
    // درخواست HTTP
    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> DownloadRequest;

    // توابع رابط کاربری
    TSharedPtr<SVerticalBox> ContainerBox;
    bool bIsLocalMode = false;

    // ---- کادر جستجوی نقشه ----
    TSharedPtr<SEditableTextBox> SearchBox;

    // ---- ساخت پنل تنظیمات ----
    TSharedRef<SWidget> BuildSettingsPanel();
    void CheckForUpdates();
    FString UpdateStatus = TEXT("Not checked");
    bool bUpdateChecking = false;
    bool bUpdateAvailable = false;

    // هندلر دکمه‌ها
    void OnToggleModeClicked(ECheckBoxState NewState);
    FReply OnGenerateClicked();

    // ---- جستجوی نقشه ----
    FReply OnSearchClicked();
    void OnSearchTextChanged(const FText& NewText);
    void OnSearchTextCommitted(const FText& NewText, ETextCommit::Type CommitType);

    // منطق نقشه و دانلود
    void OnMapBoundsReceived(FString MinLat, FString MinLon, FString MaxLat, FString MaxLon, FString CellKey);
    void DownloadOSMData(FString MinLat, FString MinLon, FString MaxLat, FString MaxLon);
    void OnDownloadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

    // تابع نهایی ساخت اکتور
    void SpawnHiroGeoActorAndBuild(FString OSMFilePath, double MinLat = 0.0, double MinLon = 0.0, double MaxLat = 0.0, double MaxLon = 0.0);

    // ---- بررسی اینکه آیا سلول گرید قبلاً ساخته شده ----
    bool IsGridCellAlreadyGenerated(const FString& CellKey) const;

    // ---- بعد از باز شدن/لود شدن نقشه، سلول‌های قبلاً ساخته‌شده در صحنه را
    //      دوباره به JS اعلام می‌کند تا روی گرید به‌صورت "استفاده‌شده" نشان داده شوند ----
    void SyncUsedGridCellsToJS();

    // HTML و JS برای نمایش نقشه
    FString GetMapHTML() const;

    // ---- متغیرهای موقت برای ذخیره bounds و cellKey ----
    double PendingMinLat = 0.0, PendingMinLon = 0.0, PendingMaxLat = 0.0, PendingMaxLon = 0.0;
    FString PendingCellKey;

    // ---- retry برای دانلودهای ناموفق/نامعتبر OSM (مثلاً rate-limit از Overpass
    //      هنگام درخواست‌های پشت‌سرهم در حالت Grid) ----
    FString PendingDownloadMinLat, PendingDownloadMinLon, PendingDownloadMaxLat, PendingDownloadMaxLon;
    int32 DownloadRetryCount = 0;
    /** Rotates across public Overpass mirrors when the primary returns 502/timeout. */
    int32 OverpassEndpointIndex = 0;
    /** Consecutive status=0 / no-response failures (VPN/proxy / connect refused). */
    int32 LastDownloadHttpCode = 0;
    FString LastDownloadFailureSummary;
    FTSTicker::FDelegateHandle RetryTickerHandle;
    /** Path under Saved/HiroGeoOSMCache for the in-flight Overpass download. */
    FString PendingOSMCachePath;

    static FString BuildOverpassMapUrl(int32 EndpointIndex, const FString& MinLat, const FString& MinLon, const FString& MaxLat, const FString& MaxLon);
    static int32 GetOverpassEndpointCount();
    static FString GetOverpassEndpointLabel(int32 EndpointIndex);
    void SetDownloadStatus(const FString& NewStatus, bool bAlsoNotify = false);

    // ---- متغیرهای حالت انتخاب ----
    EHiroGeoSelectionMode CurrentSelectionMode = EHiroGeoSelectionMode::Free;
    int32 GridZoomLevel = 5;
    FString DownloadStatus;

    // ---- visibility getters ----
    ECheckBoxState GetLocalModeCheckState() const;
    EVisibility GetLocalFileButtonVisibility() const;
    EVisibility GetOnlineModeVisibility() const;
    EVisibility GetGridSettingsVisibility() const;

    // ---- selection mode / grid zoom ----
    ECheckBoxState IsModeChecked(EHiroGeoSelectionMode Mode) const;
    void OnSelectionModeChanged(ECheckBoxState NewState, EHiroGeoSelectionMode Mode);
    float GetGridZoomValue() const;
    void OnGridZoomChanged(float NewValue);
    FText GetGridZoomLabel() const;
    static float ComputeGridCellSizeMeters(int32 ZoomLevel);
    void PushModeAndGridSizeToJS();

    // ---- فایل لوکال ----
    FReply OnSelectFileClicked();

    // ---- world origin ----
    static bool LoadWorldOrigin(UWorld* World, double& OutLat, double& OutLon);
    static void SaveWorldOrigin(UWorld* World, double Lat, double Lon);
    static FVector ComputeWorldOffsetCm(double OriginLat, double OriginLon, double TargetLat, double TargetLon);

    // ---- استخراج bounds از فایل OSM ----
    static bool ExtractBoundsFromOSMFile(const FString& FilePath, double& OutMinLat, double& OutMinLon, double& OutMaxLat, double& OutMaxLon);
};
