// Copyright Hiro. All Rights Reserved.

#include "SHiroGeoImporterWindow.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "HiroGeoJSBridge.h"
#include "HiroGeoActor.h"
#include "SWebBrowser.h"
#include "IWebBrowserSingleton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Styling/AppStyle.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/MessageDialog.h"
#include "Misc/ConfigCacheIni.h"
#include "Internationalization/Regex.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "HttpModule.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpResponse.h"
#include "Factories/Factory.h"
#include "AssetSelection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "DesktopPlatformModule.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/FileHelper.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "HiroGeoImporter"

// ================= ثابت‌های تبدیل جغرافیایی =================
namespace HiroGeoGeo
{
    constexpr double Pi = 3.14159265358979323846;
    constexpr double DegToRad = Pi / 180.0;
    constexpr double EarthRadiusMeters = 6378137.0; // WGS84
    constexpr double MetersPerDegLat = 111320.0;
}

// ================================================================
//                            Construct
// ================================================================
SHiroGeoImporterWindow::~SHiroGeoImporterWindow()
{
    if (DownloadRequest.IsValid())
    {
        DownloadRequest->OnProcessRequestComplete().Unbind();
        DownloadRequest->CancelRequest();
        DownloadRequest.Reset();
    }
    if (RetryTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(RetryTickerHandle);
        RetryTickerHandle.Reset();
    }
    if (JSBridge)
    {
        JSBridge->RemoveFromRoot();
        JSBridge = nullptr;
    }
}

void SHiroGeoImporterWindow::Construct(const FArguments& InArgs)
{
    JSBridge = NewObject<UHiroGeoJSBridge>();
    JSBridge->AddToRoot();
    JSBridge->OnBoundsSelected.AddSP(this, &SHiroGeoImporterWindow::OnMapBoundsReceived);

    ChildSlot
    [
        SNew(SBox)
        .WidthOverride(960)
        .HeightOverride(680)
        [
            // بدنه: پنل تنظیمات (چپ) + نقشه (راست)
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(10, 10, 10, 10)
            [
                SNew(SBox)
                .WidthOverride(230)
                [
                    BuildSettingsPanel()
                ]
            ]

            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .Padding(0, 10, 10, 10)
            [
                SAssignNew(ContainerBox, SVerticalBox)
            ]
        ]
    ];

    // ساخت اولیه مرورگر نقشه
    OnToggleModeClicked(ECheckBoxState::Unchecked);
    CheckForUpdates();
}

// ================================================================
//                       پنل تنظیمات سمت چپ
// ================================================================
void SHiroGeoImporterWindow::CheckForUpdates()
{
    if (bUpdateChecking) return;
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("HiroGeo"));
    if (!Plugin.IsValid()) { UpdateStatus = TEXT("Unable to determine installed version"); return; }
    const FString Version = Plugin->GetDescriptor().VersionName;
    if (Version.IsEmpty()) { UpdateStatus = TEXT("Unable to determine installed version"); return; }
    bUpdateChecking = true; bUpdateAvailable = false; UpdateStatus = TEXT("Checking for updates...");
    TWeakPtr<SHiroGeoImporterWindow> Weak = SharedThis(this);
    auto Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(TEXT("https://hiro-co.ir/api/v1/releases/latest/?app=hirogeo&current_version=") + FGenericPlatformHttp::UrlEncode(Version));
    Request->SetVerb(TEXT("GET")); Request->SetHeader(TEXT("Accept"), TEXT("application/json")); Request->SetTimeout(8.0f);
    Request->OnProcessRequestComplete().BindLambda([Weak, Version](FHttpRequestPtr Req, FHttpResponsePtr Response, bool bSuccess) {
        auto Self = Weak.Pin(); if (!Self.IsValid()) return;
        Self->bUpdateChecking = false; Self->UpdateStatus = TEXT("Update check failed");
        if (!bSuccess || !Response.IsValid()) return;
        if (Response->GetResponseCode() == 404) { Self->UpdateStatus = TEXT("No release published"); return; }
        if (Response->GetResponseCode() != 200 || Response->GetContent().Num() > 65536) return;
        TSharedPtr<FJsonObject> Data;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
        FString App, Current, Latest; bool Available = false;
        if (!FJsonSerializer::Deserialize(Reader, Data) || !Data.IsValid() ||
            !Data->TryGetStringField(TEXT("app"), App) || App != TEXT("hirogeo") ||
            !Data->TryGetStringField(TEXT("current_version"), Current) || Current != Version ||
            !Data->TryGetStringField(TEXT("version"), Latest) || Latest.IsEmpty() || Latest.Len() > 40 ||
            !Data->TryGetBoolField(TEXT("update_available"), Available)) return;
        Self->bUpdateAvailable = Available;
        Self->UpdateStatus = Available ? TEXT("Update available: ") + Latest : TEXT("You have the latest version");
    });
    if (!Request->ProcessRequest()) { bUpdateChecking = false; UpdateStatus = TEXT("Update check failed"); }
}

TSharedRef<SWidget> SHiroGeoImporterWindow::BuildSettingsPanel()
{
    return SNew(SVerticalBox)

    // ---- عنوان Settings ----
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
    [
        SNew(STextBlock)
        .Text(LOCTEXT("SettingsTitle", "Settings"))
        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
    ]

    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
    [
        SNew(SSeparator)
    ]

    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
    [ SNew(STextBlock).Text(LOCTEXT("UpdatesTitle", "Updates")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11)) ]
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
    [ SNew(STextBlock).Text_Lambda([this](){return FText::FromString(UpdateStatus);}).AutoWrapText(true) ]
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
    [ SNew(SButton).Text(LOCTEXT("CheckUpdates", "Check for updates")).IsEnabled_Lambda([this](){return !bUpdateChecking;}).OnClicked_Lambda([this](){CheckForUpdates();return FReply::Handled();}) ]
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
    [ SNew(SButton).Text(LOCTEXT("GetUpdate", "Get update")).Visibility_Lambda([this](){return bUpdateAvailable?EVisibility::Visible:EVisibility::Collapsed;}).OnClicked_Lambda([](){FPlatformProcess::LaunchURL(TEXT("https://hiro-co.ir/account/"),nullptr,nullptr);return FReply::Handled();}) ]

    // ---- Use Local File ----
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
    [
        SNew(SCheckBox)
        .IsChecked(this, &SHiroGeoImporterWindow::GetLocalModeCheckState)
        .OnCheckStateChanged(this, &SHiroGeoImporterWindow::OnToggleModeClicked)
        [
            SNew(STextBlock).Text(LOCTEXT("LocalMode", "Use Local File"))
        ]
    ]

    // ---- دکمه Select File (فقط وقتی Local Mode فعاله) ----
    + SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 12)
    [
        SNew(SBox)
        .Visibility(this, &SHiroGeoImporterWindow::GetLocalFileButtonVisibility)
        [
            SNew(SButton)
            .Text(LOCTEXT("SelectOSM", "Select .OSM File"))
            .HAlign(HAlign_Center)
            .OnClicked(FOnClicked::CreateSP(this, &SHiroGeoImporterWindow::OnSelectFileClicked))
        ]
    ]

    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
    [
        SNew(SSeparator)
        .Visibility(this, &SHiroGeoImporterWindow::GetLocalFileButtonVisibility)
    ]

    // ---- Map selection method (فقط در حالت آنلاین) ----
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
    [
        SNew(SBox)
        .Visibility(this, &SHiroGeoImporterWindow::GetOnlineModeVisibility)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("SelMethod", "Map selection method"))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
        ]
    ]

    // ---- Free ----
    + SVerticalBox::Slot().AutoHeight().Padding(8, 2)
    [
        SNew(SBox)
        .Visibility(this, &SHiroGeoImporterWindow::GetOnlineModeVisibility)
        [
            SNew(SCheckBox)
            .Style(FAppStyle::Get(), "RadioButton")
            .IsChecked(this, &SHiroGeoImporterWindow::IsModeChecked, EHiroGeoSelectionMode::Free)
            .OnCheckStateChanged(this, &SHiroGeoImporterWindow::OnSelectionModeChanged, EHiroGeoSelectionMode::Free)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).Text(LOCTEXT("FreeMode", "Free"))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("FreeDesc", "(To select small areas)"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
                ]
            ]
        ]
    ]

    // ---- Grid ----
    + SVerticalBox::Slot().AutoHeight().Padding(8, 2)
    [
        SNew(SBox)
        .Visibility(this, &SHiroGeoImporterWindow::GetOnlineModeVisibility)
        [
            SNew(SCheckBox)
            .Style(FAppStyle::Get(), "RadioButton")
            .IsChecked(this, &SHiroGeoImporterWindow::IsModeChecked, EHiroGeoSelectionMode::Grid)
            .OnCheckStateChanged(this, &SHiroGeoImporterWindow::OnSelectionModeChanged, EHiroGeoSelectionMode::Grid)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).Text(LOCTEXT("GridMode", "Grid"))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("GridDesc", "(To select multiple large areas)"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
                ]
            ]
        ]
    ]

    // ---- اسلایدر زوم گرید (فقط در حالت Grid دیده می‌شه) ----
    + SVerticalBox::Slot().AutoHeight().Padding(8, 10, 8, 0)
    [
        SNew(SBox)
        .Visibility(this, &SHiroGeoImporterWindow::GetGridSettingsVisibility)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock).Text(this, &SHiroGeoImporterWindow::GetGridZoomLabel)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
            [
                SNew(SSlider)
                .MinValue(1.0f)
                .MaxValue(10.0f)
                .StepSize(1.0f)
                .Value(this, &SHiroGeoImporterWindow::GetGridZoomValue)
                .OnValueChanged(this, &SHiroGeoImporterWindow::OnGridZoomChanged)
            ]
        ]
    ]

    // ---- وضعیت دانلود Overpass (retry / timeout باید اینجا دیده شود) ----
    + SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
    [
        SNew(STextBlock)
        .AutoWrapText(true)
        .ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.45f)))
        .Text_Lambda([this]() { return FText::FromString(DownloadStatus); })
        .Visibility_Lambda([this]()
        {
            return DownloadStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
        })
    ]

    // ---- فضای خالی برای پر کردن فضا ----
    + SVerticalBox::Slot().FillHeight(1.0f)

    // ---- دکمه Generate Map در وسط پایین ستون تنظیمات ----
    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10).HAlign(HAlign_Center)
    [
        SNew(SButton)
        .Text(LOCTEXT("Generate", "Generate Map"))
        .HAlign(HAlign_Center)
        .IsEnabled_Lambda([this]() { return !DownloadRequest.IsValid() && !RetryTickerHandle.IsValid(); })
        .OnClicked(this, &SHiroGeoImporterWindow::OnGenerateClicked)
    ];
}

// ================================================================
//                    هندلرهای حالت انتخاب / زوم گرید
// ================================================================
ECheckBoxState SHiroGeoImporterWindow::IsModeChecked(EHiroGeoSelectionMode Mode) const
{
    return (CurrentSelectionMode == Mode) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SHiroGeoImporterWindow::OnSelectionModeChanged(ECheckBoxState NewState, EHiroGeoSelectionMode Mode)
{
    if (NewState == ECheckBoxState::Checked)
    {
        CurrentSelectionMode = Mode;
        PushModeAndGridSizeToJS();
    }
}

EVisibility SHiroGeoImporterWindow::GetGridSettingsVisibility() const
{
    return (CurrentSelectionMode == EHiroGeoSelectionMode::Grid) ? EVisibility::Visible : EVisibility::Collapsed;
}

float SHiroGeoImporterWindow::GetGridZoomValue() const
{
    return (float)GridZoomLevel;
}

void SHiroGeoImporterWindow::OnGridZoomChanged(float NewValue)
{
    GridZoomLevel = FMath::Clamp(FMath::RoundToInt(NewValue), 1, 10);
    PushModeAndGridSizeToJS();
}

FText SHiroGeoImporterWindow::GetGridZoomLabel() const
{
    const int32 SizeMeters = FMath::RoundToInt(ComputeGridCellSizeMeters(GridZoomLevel));
    return FText::Format(
        LOCTEXT("GridZoomLabel", "Grid zoom: {0}   (cell size: {1} x {1} m)"),
        FText::AsNumber(GridZoomLevel), FText::AsNumber(SizeMeters));
}

float SHiroGeoImporterWindow::ComputeGridCellSizeMeters(int32 ZoomLevel)
{
    const int32 Clamped = FMath::Clamp(ZoomLevel, 1, 10);
    // zoom=1 -> 5000m ... zoom=10 -> 500m  (خطی)
    return 5000.f - (Clamped - 1) * 500.f;
}

void SHiroGeoImporterWindow::PushModeAndGridSizeToJS()
{
    if (bIsLocalMode || !WebBrowser.IsValid())
    {
        return;
    }

    const FString ModeStr = (CurrentSelectionMode == EHiroGeoSelectionMode::Grid) ? TEXT("grid") : TEXT("free");
    const float SizeMeters = ComputeGridCellSizeMeters(GridZoomLevel);

    WebBrowser->ExecuteJavascript(FString::Printf(
        TEXT("if(window.HiroGeoSetMode){ window.HiroGeoSetMode('%s'); } if(window.HiroGeoSetGridSize){ window.HiroGeoSetGridSize(%f); }"),
        *ModeStr, SizeMeters));
}

// ================================================================
//                  جابه‌جایی بین حالت آنلاین / فایل لوکال
// ================================================================
void SHiroGeoImporterWindow::OnToggleModeClicked(ECheckBoxState NewState)
{
    bIsLocalMode = (NewState == ECheckBoxState::Checked);

    // اگه نقشه هنوز ساخته نشده، یک‌بار می‌سازیمش و تا آخر نگه می‌داریم
    if (!WebBrowser.IsValid())
    {
        const FString DataUrl = FString::Printf(
            TEXT("data:text/html;charset=utf-8,%s"),
            *FGenericPlatformHttp::UrlEncode(GetMapHTML()));

        TSharedPtr<SWebBrowser> Browser = SNew(SWebBrowser)
            .InitialURL(DataUrl)
            .ShowControls(false)
            .OnLoadCompleted(FSimpleDelegate::CreateSP(this, &SHiroGeoImporterWindow::SyncUsedGridCellsToJS));

        WebBrowser = Browser;
        Browser->BindUObject(TEXT("HiroGeoJSBridge"), JSBridge, true);

        // ---- کادر جستجو در بالای نقشه ----
        ContainerBox->AddSlot()
        .AutoHeight()
        .Padding(0, 0, 0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SAssignNew(SearchBox, SEditableTextBox)
                .HintText(LOCTEXT("SearchHint", "Search location (e.g. Tehran, 35.68,51.38)"))
                .OnTextCommitted(this, &SHiroGeoImporterWindow::OnSearchTextCommitted)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4, 0, 0, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("SearchBtn", "Search"))
                .OnClicked(this, &SHiroGeoImporterWindow::OnSearchClicked)
            ]
        ];

        // ---- مرورگر نقشه ----
        ContainerBox->AddSlot()
        .FillHeight(1.0f)
        [
            Browser.ToSharedRef()
        ];
    }
}

// ================================================================
//                       جستجوی نقشه
// ================================================================
FReply SHiroGeoImporterWindow::OnSearchClicked()
{
    if (SearchBox.IsValid() && WebBrowser.IsValid())
    {
        FString Query = SearchBox->GetText().ToString();
        if (!Query.IsEmpty())
        {
            // ارسال کوئری جستجو به جاوااسکریپت نقشه
            FString EncodedQuery = FGenericPlatformHttp::UrlEncode(Query);
            WebBrowser->ExecuteJavascript(FString::Printf(
                TEXT("if(window.HiroGeoSearchLocation) window.HiroGeoSearchLocation('%s');"),
                *EncodedQuery));
        }
    }
    return FReply::Handled();
}

void SHiroGeoImporterWindow::OnSearchTextChanged(const FText& NewText)
{
    // هیچ عمل خاصی نیاز نیست - فقط برای تکمیل رابط
}

void SHiroGeoImporterWindow::OnSearchTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
    if (CommitType == ETextCommit::OnEnter)
    {
        OnSearchClicked();
    }
}

// ================================================================
//                          صفحه HTML نقشه
// ================================================================
FString SHiroGeoImporterWindow::GetMapHTML() const
{
    const FString InitialMode = (CurrentSelectionMode == EHiroGeoSelectionMode::Grid) ? TEXT("grid") : TEXT("free");
    const FString InitialGridSize = FString::SanitizeFloat(ComputeGridCellSizeMeters(GridZoomLevel));

    // بخش ۱: CSS و HTML
    FString Html = TEXT(R"HTML(
        <style>
            body { margin: 0; padding: 0; font-family: sans-serif; }
            #map { width: 100%; height: 100vh; }
            #info {
                position: absolute;
                bottom: 10px;
                left: 50%;
                transform: translateX(-50%);
                z-index: 1000;
                background: white;
                padding: 8px 14px;
                border-radius: 5px;
                box-shadow: 0 0 5px rgba(0,0,0,0.3);
                text-align: center;
                font-size: 13px;
            }
        </style>
        <link rel="stylesheet" href="https://unpkg.com/leaflet@1.7.1/dist/leaflet.css" />
        <script src="https://unpkg.com/leaflet@1.7.1/dist/leaflet.js"></script>
        <div id="info">Loading...</div>
        <div id="map"></div>
        <script>
            var map = L.map('map').setView([35.6892, 51.3890], 13);
            L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
                attribution: '&copy; OpenStreetMap contributors'
            }).addTo(map);
            var currentMode = '__INITIAL_MODE__';
            var gridSizeMeters = __INITIAL_GRIDSIZE__;
            var gridRefLat = null;
            var gridLayer = L.layerGroup().addTo(map);
            var usedCells = new Set();
            var selectedCellKey = null;
            var selectedCellBounds = null;
            var freePoints = [];
            var freeMarkers = [];
            var freePolygon = null;
            var infoBox = document.getElementById('info');
            function metersPerDegLat() { return 111320.0; }
            function metersPerDegLon(atLatDeg) { return 111320.0 * Math.cos(atLatDeg * Math.PI / 180.0); }
    )HTML");

    // بخش ۲: توابع انتخاب و Grid
    Html += TEXT(R"HTML(
            function clearFreeSelection() {
                freePoints = [];
                freeMarkers.forEach(function(m) { map.removeLayer(m); });
                freeMarkers = [];
                if (freePolygon) { map.removeLayer(freePolygon); freePolygon = null; }
                if (currentMode === 'free') infoBox.innerText = 'Click 4 points to select an area';
            }
            function clearGridSelection() {
                selectedCellKey = null;
                selectedCellBounds = null;
                if (currentMode === 'grid') infoBox.innerText = 'Click a grid cell to select it';
                redrawGrid();
            }
            function resetSelectionState() { clearFreeSelection(); clearGridSelection(); }
            function cellKeyFor(row, col) { return row + '_' + col + '_' + Math.round(gridSizeMeters); }
            function redrawGrid() {
                gridLayer.clearLayers();
                if (currentMode !== 'grid') return;
                if (gridRefLat === null) gridRefLat = map.getCenter().lat;
                var degLat = gridSizeMeters / metersPerDegLat();
                var degLon = gridSizeMeters / metersPerDegLon(gridRefLat);
                var bounds = map.getBounds();
                var rowMin = Math.floor(bounds.getSouth() / degLat) - 1;
                var rowMax = Math.ceil(bounds.getNorth() / degLat) + 1;
                var colMin = Math.floor(bounds.getWest() / degLon) - 1;
                var colMax = Math.ceil(bounds.getEast() / degLon) + 1;
                for (var r = rowMin; r <= rowMax; r++) {
                    for (var c = colMin; c <= colMax; c++) {
                        var key = cellKeyFor(r, c);
                        var south = r * degLat, north = (r + 1) * degLat;
                        var west = c * degLon, east = (c + 1) * degLon;
                        var isUsed = usedCells.has(key);
                        var isSelected = (key === selectedCellKey);
                        var color = isUsed ? '#888888' : (isSelected ? '#ff7800' : '#3388ff');
                        var fillOpacity = isUsed ? 0.35 : (isSelected ? 0.35 : 0.04);
                        var rect = L.rectangle([[south, west], [north, east]], { color: color, weight: 1, fillOpacity: fillOpacity });
                        if (!isUsed) {
                            (function(rr, cc, kk, s, n, w, e) {
                                rect.on('click', function() { onGridCellClicked(rr, cc, kk, s, n, w, e); });
                            })(r, c, key, south, north, west, east);
                        }
                        rect.addTo(gridLayer);
                    }
                }
            }
    )HTML");

    // بخش ۳: هندلر کلیک و ارسال به UE
    Html += TEXT(R"HTML(
            function onGridCellClicked(r, c, key, south, north, west, east) {
                selectedCellKey = key;
                selectedCellBounds = { minLat: south, maxLat: north, minLon: west, maxLon: east };
                infoBox.innerText = 'Cell selected (' + Math.round(gridSizeMeters) + 'm). Press Generate Map in Settings.';
                redrawGrid();
            }
            map.on('click', function(e) {
                if (currentMode !== 'free') return;
                if (freePoints.length >= 4) clearFreeSelection();
                freePoints.push(e.latlng);
                var marker = L.marker(e.latlng).addTo(map);
                freeMarkers.push(marker);
                if (freePoints.length < 4) {
                    infoBox.innerText = (4 - freePoints.length) + ' more point(s) to select area';
                } else {
                    var latlngs = freePoints.map(function(p) { return [p.lat, p.lng]; });
                    if (freePolygon) map.removeLayer(freePolygon);
                    freePolygon = L.polygon(latlngs, { color: '#ff7800' }).addTo(map);
                    infoBox.innerText = 'Area selected. Press Generate Map in Settings.';
                }
            });
            map.on('moveend', function() { if (currentMode === 'grid') redrawGrid(); });
            function sendCurrentSelection() {
                if (currentMode === 'grid') {
                    if (!selectedCellBounds) { alert('Select a grid cell first.'); return; }
                    sendBoundsToUE(selectedCellBounds.minLat, selectedCellBounds.minLon, selectedCellBounds.maxLat, selectedCellBounds.maxLon, selectedCellKey);
                } else {
                    if (freePoints.length < 4) { alert('Select 4 points first.'); return; }
                    var lats = freePoints.map(function(p) { return p.lat; });
                    var lons = freePoints.map(function(p) { return p.lng; });
                    sendBoundsToUE(Math.min.apply(null, lats), Math.min.apply(null, lons), Math.max.apply(null, lats), Math.max.apply(null, lons), '');
                }
            }
            function sendBoundsToUE(minLat, minLon, maxLat, maxLon, cellKey) {
                if (window.ue && window.ue.hirogeojsbridge) {
                    window.ue.hirogeojsbridge.onmapareaselected(minLat.toString(), minLon.toString(), maxLat.toString(), maxLon.toString(), cellKey || '');
                } else if (window.ue) {
                    alert('HiroGeoJSBridge not found. Keys: ' + Object.keys(window.ue).join(', '));
                } else {
                    alert('window.ue is undefined - BindUObject may have failed.');
                }
            }
    )HTML");

    // بخش ۴: توابع global و جستجو
    Html += TEXT(R"HTML(
            window.HiroGeoSetMode = function(mode) {
                currentMode = mode;
                resetSelectionState();
                redrawGrid();
            };
            window.HiroGeoSetGridSize = function(meters) {
                gridSizeMeters = meters;
                resetSelectionState();
            };
            window.HiroGeoMarkCellUsed = function(key) {
                usedCells.add(key);
                if (key === selectedCellKey) { selectedCellKey = null; selectedCellBounds = null; }
                redrawGrid();
            };
            window.HiroGeoSearchLocation = function(query) {
                var coordMatch = query.match(/^(-?\d+\.?\d*)\s*[,\s]\s*(-?\d+\.?\d*)$/);
                if (coordMatch) {
                    var lat = parseFloat(coordMatch[1]);
                    var lon = parseFloat(coordMatch[2]);
                    if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
                        map.setView([lat, lon], 15);
                        infoBox.innerText = 'Moved to ' + lat.toFixed(4) + ', ' + lon.toFixed(4);
                        return;
                    }
                }
                var url = 'https://nominatim.openstreetmap.org/search?format=json&q=' + encodeURIComponent(query);
                fetch(url)
                    .then(function(response) { return response.json(); })
                    .then(function(data) {
                        if (data && data.length > 0) {
                            map.setView([parseFloat(data[0].lat), parseFloat(data[0].lon)], 15);
                            infoBox.innerText = 'Found: ' + data[0].display_name.substring(0, 60);
                        } else {
                            infoBox.innerText = 'No results found for: ' + query;
                        }
                    })
                    .catch(function(err) { infoBox.innerText = 'Search error. Try again.'; });
            };
            resetSelectionState();
        </script>
    )HTML");

    Html = Html.Replace(TEXT("__INITIAL_MODE__"), *InitialMode);
    Html = Html.Replace(TEXT("__INITIAL_GRIDSIZE__"), *InitialGridSize);
    return Html;
}

// ================================================================
//                     دانلود / دکمه Generate Map
// ================================================================
void SHiroGeoImporterWindow::OnMapBoundsReceived(FString MinLat, FString MinLon, FString MaxLat, FString MaxLon, FString CellKey)
{
    // ---- نکته ۱: بررسی اینکه آیا این سلول گرید قبلاً ساخته شده ----
    if (!CellKey.IsEmpty() && IsGridCellAlreadyGenerated(CellKey))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(LOCTEXT("CellAlreadyUsed", "Grid cell '{0}' has already been generated. Skipping."),
                FText::FromString(CellKey)));
        return;
    }

    if (RetryTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(RetryTickerHandle);
        RetryTickerHandle.Reset();
    }
    DownloadRetryCount = 0;
    OverpassEndpointIndex = 0;
    LastDownloadHttpCode = 0;
    LastDownloadFailureSummary.Reset();
    SetDownloadStatus(TEXT("Preparing Overpass download…"));
    PendingMinLat = FCString::Atod(*MinLat);
    PendingMinLon = FCString::Atod(*MinLon);
    PendingMaxLat = FCString::Atod(*MaxLat);
    PendingMaxLon = FCString::Atod(*MaxLon);
    PendingCellKey = CellKey;

    DownloadOSMData(MinLat, MinLon, MaxLat, MaxLon);
}

// ================================================================
//       بررسی اینکه آیا سلول گرید قبلاً تولید شده یا نه
// ================================================================
bool SHiroGeoImporterWindow::IsGridCellAlreadyGenerated(const FString& CellKey) const
{
    if (CellKey.IsEmpty()) return false;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) return false;

    // بررسی تمام HiroGeoActor های موجود - اگر اکتوری با همین CellKey وجود داشت، یعنی قبلاً ساخته شده
    for (TActorIterator<AHiroGeoActor> It(World); It; ++It)
    {
        AHiroGeoActor* Actor = *It;
        if (Actor && Actor->GetGeneratedCellKey() == CellKey)
        {
            return true;
        }
    }
    return false;
}

// ================================================================
//   سینک کردن سلول‌های قبلاً ساخته‌شده با JS، بعد از باز شدن/لود شدن نقشه
//   (usedCells سمت جاوااسکریپت یک Set داخل همون SWebBrowser است و با هر بار
//    ساختن مرورگر جدید خالی می‌شود؛ این تابع آن را از روی اکتورهای واقعیِ
//    موجود در صحنه دوباره می‌سازد تا بعد از بستن/باز کردن پنجره هم گرید
//    به‌درستی سلول‌های قبلی را "استفاده‌شده" نشان دهد)
// ================================================================
void SHiroGeoImporterWindow::SyncUsedGridCellsToJS()
{
    if (!WebBrowser.IsValid())
    {
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return;
    }

    for (TActorIterator<AHiroGeoActor> It(World); It; ++It)
    {
        AHiroGeoActor* Actor = *It;
        const FString Key = Actor ? Actor->GetGeneratedCellKey() : FString();
        if (!Key.IsEmpty())
        {
            WebBrowser->ExecuteJavascript(FString::Printf(
                TEXT("if(window.HiroGeoMarkCellUsed) window.HiroGeoMarkCellUsed('%s');"), *Key));
        }
    }
}

int32 SHiroGeoImporterWindow::GetOverpassEndpointCount()
{
    return 3;
}

FString SHiroGeoImporterWindow::GetOverpassEndpointLabel(int32 EndpointIndex)
{
    static const TCHAR* Labels[] = {
        TEXT("overpass-api.de"),
        TEXT("lz4.overpass-api.de"),
        TEXT("overpass.kumi.systems"),
    };
    const int32 Count = UE_ARRAY_COUNT(Labels);
    const int32 SafeIndex = ((EndpointIndex % Count) + Count) % Count;
    return Labels[SafeIndex];
}

FString SHiroGeoImporterWindow::BuildOverpassMapUrl(int32 EndpointIndex, const FString& MinLat, const FString& MinLon, const FString& MaxLat, const FString& MaxLon)
{
    static const TCHAR* Hosts[] = {
        TEXT("https://overpass-api.de/api/map"),
        TEXT("https://lz4.overpass-api.de/api/map"),
        TEXT("https://overpass.kumi.systems/api/map"),
    };
    const int32 Count = UE_ARRAY_COUNT(Hosts);
    const int32 SafeIndex = ((EndpointIndex % Count) + Count) % Count;
    return FString::Printf(TEXT("%s?bbox=%s,%s,%s,%s"), Hosts[SafeIndex], *MinLon, *MinLat, *MaxLon, *MaxLat);
}

void SHiroGeoImporterWindow::SetDownloadStatus(const FString& NewStatus, bool bAlsoNotify)
{
    DownloadStatus = NewStatus;
    UE_LOG(LogTemp, Log, TEXT("HiroGeo: %s"), *DownloadStatus);

    if (bAlsoNotify && !NewStatus.IsEmpty())
    {
        FNotificationInfo Info(FText::FromString(NewStatus));
        Info.ExpireDuration = 4.0f;
        Info.bUseLargeFont = false;
        Info.bFireAndForget = true;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
}

void SHiroGeoImporterWindow::DownloadOSMData(FString MinLat, FString MinLon, FString MaxLat, FString MaxLon)
{
    // برای retry در صورت دانلود ناموفق/نامعتبر (OnDownloadComplete) ذخیره می‌شود
    PendingDownloadMinLat = MinLat;
    PendingDownloadMinLon = MinLon;
    PendingDownloadMaxLat = MaxLat;
    PendingDownloadMaxLon = MaxLon;

    if (DownloadRequest.IsValid())
    {
        DownloadRequest->OnProcessRequestComplete().Unbind();
        DownloadRequest->CancelRequest();
        DownloadRequest.Reset();
    }

    // Disk cache — UE equivalent of streets-gl tile reuse for Overpass extracts.
    // Only consult cache on the first attempt of a Generate click.
    if (DownloadRetryCount == 0)
    {
        const FString CacheDir = FPaths::ProjectSavedDir() / TEXT("HiroGeoOSMCache");
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        if (!PF.DirectoryExists(*CacheDir))
            PF.CreateDirectoryTree(*CacheDir);

        const FString CacheName = FString::Printf(
            TEXT("bbox_%.5f_%.5f_%.5f_%.5f.osm"),
            FCString::Atod(*MinLat), FCString::Atod(*MinLon),
            FCString::Atod(*MaxLat), FCString::Atod(*MaxLon));
        const FString CachePath = CacheDir / CacheName;
        if (PF.FileExists(*CachePath))
        {
            FString Cached;
            if (FFileHelper::LoadFileToString(Cached, *CachePath) &&
                Cached.Contains(TEXT("<osm")) && Cached.Contains(TEXT("</osm>")))
            {
                SetDownloadStatus(FString::Printf(TEXT("OSM cache hit — loading %s"), *CacheName), true);
                SpawnHiroGeoActorAndBuild(CachePath, PendingMinLat, PendingMinLon, PendingMaxLat, PendingMaxLon);
                return;
            }
        }
        PendingOSMCachePath = CachePath;
    }

    const FString Url = BuildOverpassMapUrl(OverpassEndpointIndex, MinLat, MinLon, MaxLat, MaxLon);
    const FString HostLabel = GetOverpassEndpointLabel(OverpassEndpointIndex);
    SetDownloadStatus(FString::Printf(
        TEXT("Downloading OSM from %s (attempt %d)…"),
        *HostLabel, DownloadRetryCount + 1),
        DownloadRetryCount == 0);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->OnProcessRequestComplete().BindSP(this, &SHiroGeoImporterWindow::OnDownloadComplete);
    Request->SetURL(Url);
    Request->SetVerb("GET");
    Request->SetHeader(TEXT("Accept"), TEXT("application/osm+xml,application/xml,text/xml,*/*"));
    Request->SetHeader(TEXT("User-Agent"), TEXT("HiroGeo/1.0 (UnrealEngine; OSM importer)"));
    // Cap per-request wait; combined with short retries this keeps total UX under ~2–3 minutes.
    Request->SetTimeout(45.0f);
    DownloadRequest = Request;
    if (!Request->ProcessRequest())
    {
        Request->OnProcessRequestComplete().Unbind();
        OnDownloadComplete(Request, nullptr, false);
    }
}

void SHiroGeoImporterWindow::OnDownloadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    if (DownloadRequest != Request)
        return;
    DownloadRequest.Reset();
    // یک فایل OSM واقعی همیشه تگ ریشه‌ی <osm دارد. وقتی Overpass به‌خاطر
    // درخواست‌های پشت‌سرهم (مخصوصاً حالت Grid که چند سلول پشت‌سرهم دانلود
    // می‌شوند) ریت‌لیمیت کند یا خطا بدهد، معمولاً یک پاسخ خالی/HTML خطا
    // برمی‌گرداند که parse آن دقیقاً همان "OSMLoader: Failed to parse OSM XML"
    // را تولید می‌کند که در لاگ دیدید — و چون اکتور قبل از این خطا از قبل
    // Spawn شده، در Outliner هست اما هیچ مشی ندارد (کاشی نامرئی).
    const int32 HttpCode = Response.IsValid() ? Response->GetResponseCode() : 0;
    constexpr int32 MaxOsmResponseBytes = 100 * 1024 * 1024;
    const bool bResponseUsable = bWasSuccessful && Response.IsValid() && HttpCode == 200 &&
        Response->GetContent().Num() > 0 && Response->GetContent().Num() <= MaxOsmResponseBytes;
    const FString Content = bResponseUsable ? Response->GetContentAsString() : FString();
    const bool bValidOsm = Content.Contains(TEXT("<osm")) && Content.Contains(TEXT("</osm>"));

    if (!bValidOsm)
    {
        FString ResponseHeaders = TEXT("<no response>");
        FString BodySnippet = TEXT("<no response>");
        if (Response.IsValid())
        {
            ResponseHeaders = FString::Join(Response->GetAllHeaders(), TEXT(" | "));
            BodySnippet = Response->GetContentAsString().Left(512);
            BodySnippet.ReplaceInline(TEXT("\r"), TEXT(" "));
            BodySnippet.ReplaceInline(TEXT("\n"), TEXT(" "));
        }
        LastDownloadHttpCode = HttpCode;
        const bool bNoHttpResponse = (HttpCode == 0);
        if (bNoHttpResponse)
        {
            LastDownloadFailureSummary = TEXT("connection failed / timeout (no HTTP response — often VPN/proxy or blocked Overpass)");
        }
        else
        {
            if (BodySnippet.Contains(TEXT("at capacity")) || BodySnippet.Contains(TEXT("queue is full")))
            {
                LastDownloadFailureSummary = FString::Printf(TEXT("HTTP %d — Overpass server at capacity / queue full"), HttpCode);
            }
            else if (HttpCode == 429)
            {
                LastDownloadFailureSummary = TEXT("HTTP 429 — rate limited");
            }
            else if (HttpCode == 504 || HttpCode == 502)
            {
                LastDownloadFailureSummary = FString::Printf(TEXT("HTTP %d — gateway/capacity error from Overpass"), HttpCode);
            }
            else
            {
                LastDownloadFailureSummary = FString::Printf(TEXT("HTTP %d — invalid or non-OSM response"), HttpCode);
            }
        }

        UE_LOG(LogTemp, Warning,
            TEXT("HiroGeo: Overpass failure url='%s' status=%d connected=%d bytes=%d summary='%s' headers='%s' body='%s'"),
            Request.IsValid() ? *Request->GetURL() : TEXT("<null request>"),
            HttpCode, bWasSuccessful ? 1 : 0,
            Response.IsValid() ? Response->GetContent().Num() : 0,
            *LastDownloadFailureSummary, *ResponseHeaders, *BodySnippet);

        SetDownloadStatus(FString::Printf(TEXT("Overpass failed: %s"), *LastDownloadFailureSummary), true);

        const int32 MaxRetries = GetOverpassEndpointCount() - 1;
        if (DownloadRetryCount < MaxRetries)
        {
            DownloadRetryCount++;
            OverpassEndpointIndex = (OverpassEndpointIndex + 1) % GetOverpassEndpointCount();
            // Cap wait: 5s, 10s, 12s — never the old 8/16/32/64 silent spiral.
            const float RetryDelaySeconds = FMath::Min(5.0f * static_cast<float>(DownloadRetryCount), 12.0f);
            const FString NextHost = GetOverpassEndpointLabel(OverpassEndpointIndex);
            SetDownloadStatus(FString::Printf(
                TEXT("Retry %d/%d in %.0fs → %s…"),
                DownloadRetryCount, MaxRetries, RetryDelaySeconds, *NextHost),
                true);
            UE_LOG(LogTemp, Warning,
                TEXT("HiroGeo: OSM download invalid/failed (HTTP %d, retry %d/%d, next endpoint=%d) - retrying in %.0fs..."),
                HttpCode, DownloadRetryCount, MaxRetries, OverpassEndpointIndex, RetryDelaySeconds);

            TWeakPtr<SHiroGeoImporterWindow> RetryWeakThis = StaticCastSharedRef<SHiroGeoImporterWindow>(AsShared()).ToWeakPtr();
            RetryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                [RetryWeakThis](float) -> bool
                {
                    if (TSharedPtr<SHiroGeoImporterWindow> Pinned = RetryWeakThis.Pin())
                    {
                        Pinned->RetryTickerHandle.Reset();
                        Pinned->DownloadOSMData(Pinned->PendingDownloadMinLat, Pinned->PendingDownloadMinLon,
                            Pinned->PendingDownloadMaxLat, Pinned->PendingDownloadMaxLon);
                    }
                    return false; // one-shot
                }), RetryDelaySeconds);
            return;
        }

        const FText FailMsg = FText::Format(
            LOCTEXT("DownloadFailDetailed",
                "Failed to download OSM from Overpass.\n\nLast error: {0}\n\nWhat to try:\n- Overpass may be busy — wait a bit and retry.\n- If you use VPN/proxy, try disabling it or allow overpass-api.de.\n- Enable Use Local File and import a .osm you already downloaded.\n- Use a smaller map area / grid cell."),
            FText::FromString(LastDownloadFailureSummary.IsEmpty()
                ? FString::Printf(TEXT("HTTP %d"), LastDownloadHttpCode)
                : LastDownloadFailureSummary));
        DownloadRetryCount = 0;
        OverpassEndpointIndex = 0;
        SetDownloadStatus(TEXT("Download failed — see dialog. Tip: Use Local File."), true);
        FMessageDialog::Open(EAppMsgType::Ok, FailMsg);
        return;
    }

    DownloadRetryCount = 0;
    OverpassEndpointIndex = 0;
    LastDownloadHttpCode = 200;
    LastDownloadFailureSummary.Reset();
    SetDownloadStatus(TEXT("OSM download OK — generating map…"), true);

    FString TempDir = FPaths::ProjectSavedDir() / TEXT("TempOSM");
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*TempDir))
    {
        PlatformFile.CreateDirectory(*TempDir);
    }

    // اسم فایل یونیک، چون حالا ممکنه چند ناحیه پشت‌سرهم وارد بشن (مخصوصا حالت Grid)
    FString FilePath = TempDir / FString::Printf(TEXT("Downloaded_%lld.osm"), (int64)FDateTime::Now().ToUnixTimestamp());

    if (FFileHelper::SaveStringToFile(Content, *FilePath))
    {
        // Mirror into persistent OSM cache for subsequent grid/free imports.
        if (!PendingOSMCachePath.IsEmpty())
        {
            FFileHelper::SaveStringToFile(Content, *PendingOSMCachePath);
            PendingOSMCachePath.Reset();
        }
        SpawnHiroGeoActorAndBuild(FilePath, PendingMinLat, PendingMinLon, PendingMaxLat, PendingMaxLon);
    }
    else
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SaveFail", "Failed to save OSM file."));
    }
}

FReply SHiroGeoImporterWindow::OnGenerateClicked()
{
    if (bIsLocalMode)
    {
        // در حالت Local، کلیک Generate همان کار Select File رو می‌کنه
        return OnSelectFileClicked();
    }

    if (WebBrowser.IsValid())
    {
        WebBrowser->ExecuteJavascript(TEXT("if(typeof sendCurrentSelection === 'function') sendCurrentSelection();"));
    }
    return FReply::Handled();
}

FReply SHiroGeoImporterWindow::OnSelectFileClicked()
{
    TArray<FString> OutFiles;
    if (FDesktopPlatformModule::Get()->OpenFileDialog(nullptr, TEXT("Select OSM File"), TEXT(""), TEXT(""), TEXT("OSM File|*.osm"), EFileDialogFlags::None, OutFiles))
    {
        if (OutFiles.Num() > 0)
        {
            SpawnHiroGeoActorAndBuild(OutFiles[0]);
        }
    }
    return FReply::Handled();
}

ECheckBoxState SHiroGeoImporterWindow::GetLocalModeCheckState() const
{
    return bIsLocalMode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

EVisibility SHiroGeoImporterWindow::GetLocalFileButtonVisibility() const
{
    return bIsLocalMode ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SHiroGeoImporterWindow::GetOnlineModeVisibility() const
{
    return bIsLocalMode ? EVisibility::Collapsed : EVisibility::Visible;
}

// ================================================================
//             استخراج bounds از خود فایل OSM (حالت لوکال)
// ================================================================
bool SHiroGeoImporterWindow::ExtractBoundsFromOSMFile(const FString& FilePath, double& OutMinLat, double& OutMinLon, double& OutMaxLat, double& OutMaxLon)
{
    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
    {
        return false;
    }

    const FRegexPattern Pattern(TEXT("<bounds[^>]*minlat=\"([\\-0-9\\.]+)\"[^>]*minlon=\"([\\-0-9\\.]+)\"[^>]*maxlat=\"([\\-0-9\\.]+)\"[^>]*maxlon=\"([\\-0-9\\.]+)\""));
    FRegexMatcher Matcher(Pattern, FileContent);
    if (Matcher.FindNext())
    {
        OutMinLat = FCString::Atod(*Matcher.GetCaptureGroup(1));
        OutMinLon = FCString::Atod(*Matcher.GetCaptureGroup(2));
        OutMaxLat = FCString::Atod(*Matcher.GetCaptureGroup(3));
        OutMaxLon = FCString::Atod(*Matcher.GetCaptureGroup(4));
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("HiroGeo: Could not find <bounds> tag in OSM file: %s"), *FilePath);
    return false;
}

// ================================================================
//          محاسبه مبدأ دنیا و آفست برای چینش کاشی‌های مجاور
// ================================================================
bool SHiroGeoImporterWindow::LoadWorldOrigin(UWorld* World, double& OutLat, double& OutLon)
{
    if (!World) return false;

    const FString Section = FString::Printf(TEXT("HiroGeoImporter.%s"), *World->GetMapName());
    FString Value;
    if (GConfig->GetString(*Section, TEXT("OriginLatLon"), Value, GEditorPerProjectIni))
    {
        TArray<FString> Parts;
        Value.ParseIntoArray(Parts, TEXT(","));
        if (Parts.Num() == 2)
        {
            OutLat = FCString::Atod(*Parts[0]);
            OutLon = FCString::Atod(*Parts[1]);
            return true;
        }
    }
    return false;
}

void SHiroGeoImporterWindow::SaveWorldOrigin(UWorld* World, double Lat, double Lon)
{
    if (!World) return;

    const FString Section = FString::Printf(TEXT("HiroGeoImporter.%s"), *World->GetMapName());
    const FString Value = FString::Printf(TEXT("%.8f,%.8f"), Lat, Lon);
    GConfig->SetString(*Section, TEXT("OriginLatLon"), *Value, GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

FVector SHiroGeoImporterWindow::ComputeWorldOffsetCm(double OriginLat, double OriginLon, double TargetLat, double TargetLon)
{
    using namespace HiroGeoGeo;

    const double DeltaLatMeters = (TargetLat - OriginLat) * DegToRad * EarthRadiusMeters;
    const double AvgLatRad = ((OriginLat + TargetLat) * 0.5) * DegToRad;
    const double DeltaLonMeters = (TargetLon - OriginLon) * DegToRad * EarthRadiusMeters * FMath::Cos(AvgLatRad);

    // X = شرق (Lon) بدون تغییر علامت — مطابق همه‌ی generatorها در GeoMeshGenerator.cpp
    // (UE.X = LocalX * ScaleFactor)
    //
    // Y = جنوب با علامت منفی، نه شمال! طبق کامنت صریح GeoMeshGenerator.cpp
    // (خط ~4727، "Y-axis mirror fix"): تمام مش‌ها (دیوار، جاده، آب، ریل، POI،
    // درخت، زمین) با UE.Y = -LocalY * ScaleFactor ساخته می‌شوند، و چون
    // OSMLoader::LatLonToLocal، LocalY مثبت را به‌ازای Lat افزایشی (شمال) برمی‌گرداند،
    // یعنی در UE نهایتاً +Y = جنوب است. پس دلتای Lat باید قبل از تبدیل به سانتی‌متر
    // علامتش منفی شود تا با همین قرارداد مش هم‌خوان باشد.
    return FVector(DeltaLonMeters * 100.0, -DeltaLatMeters * 100.0, 0.0);
}

// ================================================================
//                      اسپاون اکتور + چینش در دنیا
// ================================================================
void SHiroGeoImporterWindow::SpawnHiroGeoActorAndBuild(FString OSMFilePath, double MinLat, double MinLon, double MaxLat, double MaxLon)
{
    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        UE_LOG(LogTemp, Error, TEXT("HiroGeo: No valid editor world."));
        return;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();

    // اگه bounds واقعی نداریم (حالت فایل لوکال)، از تگ <bounds> داخل خود فایل استخراج می‌کنیم
    const bool bHasValidBounds = !(FMath::IsNearlyZero(MinLat) && FMath::IsNearlyZero(MinLon) && FMath::IsNearlyZero(MaxLat) && FMath::IsNearlyZero(MaxLon));
    if (!bHasValidBounds)
    {
        ExtractBoundsFromOSMFile(OSMFilePath, MinLat, MinLon, MaxLat, MaxLon);
    }

    // مرکز bbox درخواستی - فقط به‌عنوان fallback استفاده می‌شه (وقتی اکتور
    // مبدأ واقعی مش رو گزارش نکرده، مثلاً مسیر GeoJSON)
    const double RequestedCenterLat = (MinLat + MaxLat) * 0.5;
    const double RequestedCenterLon = (MinLon + MaxLon) * 0.5;

    // آیا قبلاً نقشه‌ای در این لول وارد شده؟
    bool bAnyExisting = false;
    for (TActorIterator<AHiroGeoActor> It(World); It; ++It)
    {
        bAnyExisting = true;
        break;
    }

    double OriginLat = RequestedCenterLat, OriginLon = RequestedCenterLon;
    const bool bHaveStoredOrigin = LoadWorldOrigin(World, OriginLat, OriginLon);

    AHiroGeoActor* NewActor = World->SpawnActor<AHiroGeoActor>(AHiroGeoActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);

    if (NewActor)
    {
        NewActor->OSMFilePath = OSMFilePath;
        NewActor->bUseGeoJSON = false;

        // ---- نکته ۱: ذخیره CellKey در اکتور برای جلوگیری از ساخت مجدد ----
        if (!PendingCellKey.IsEmpty())
        {
            NewActor->SetGeneratedCellKey(PendingCellKey);
        }

        // ---- نکته ۲: ذخیره bounds گرید در اکتور تا مش‌ها فقط به اندازه گرید ساخته بشن ----
        if (!PendingCellKey.IsEmpty())
        {
            // فقط در حالت Grid: تنظیم bounding box برای فیلتر کردن مش‌ها
            NewActor->SetGridBounds(MinLat, MinLon, MaxLat, MaxLon);
        }

        // مش رو می‌سازیم - این کار MeshOriginLatitude/Longitude رو هم پر می‌کنه
        // (نقطه‌ی جغرافیایی واقعی که محل (0,0,0) محلی این مش به آن اشاره می‌کند)
        NewActor->Generate();

        // ====================================================================
        // چینش صحیح در دنیا - باید بعد از Generate() انجام شود
        //
        // نکته مهم: مرکز bbox درخواستی (RequestedCenterLat/Lon) معمولاً دقیقاً
        // همان نقطه‌ی جغرافیایی نیست که مرکز محلی (0,0,0) مش به آن اشاره می‌کند؛
        // GeoMeshGenerator/HiroGeoActor مش را حول centroid واقعی دیتای
        // بارگذاری‌شده (بعد از کلیپ) سنتر می‌کند، نه حول مرکز bbox درخواستی.
        // اگر بر این اساس عمل نکنیم، هر کاشی به‌اندازه‌ی این اختلاف (که می‌تواند
        // صدها متر باشد) جابه‌جا می‌شود و کاشی‌ها به‌جای کنار هم چیده شدن،
        // پریشان/نامرتب می‌نشینند - دقیقاً همان مشکلی که قبلاً دیده می‌شد.
        // پس همیشه از مقدار واقعی گزارش‌شده توسط خود اکتور استفاده می‌کنیم،
        // نه از مرکز bbox که فقط یک تخمین اولیه‌ست.
        // ====================================================================
        double PlacementLat = RequestedCenterLat;
        double PlacementLon = RequestedCenterLon;
        if (NewActor->HasMeshOrigin())
        {
            PlacementLat = NewActor->GetMeshOriginLatitude();
            PlacementLon = NewActor->GetMeshOriginLongitude();
        }

        FVector SpawnLocation = FVector::ZeroVector;
        if (bAnyExisting && bHaveStoredOrigin)
        {
            // این اولین نقشه نیست -> نسبت به مبدأ ذخیره‌شده آفست می‌گیریم تا کنار نقشه‌های قبلی بچینه
            SpawnLocation = ComputeWorldOffsetCm(OriginLat, OriginLon, PlacementLat, PlacementLon);
        }
        else
        {
            // اولین نقشه -> همین نقطه (مبدأ واقعی مش) مبدأ دنیا (0,0,0) می‌شه
            SpawnLocation = FVector::ZeroVector;
            SaveWorldOrigin(World, PlacementLat, PlacementLon);
        }

        // نکته: AHiroGeoActor::PostActorCreated() به محض Spawn شدن در ادیتور،
        // ترنسفرم اکتور رو به Identity ریست می‌کنه (برای حالت drag-and-drop
        // دستی از Content Browser). اینجا - بعد از Generate() - موقعیت نهایی
        // و صحیح رو ست می‌کنیم تا هم آن ریست override بشه و هم از مبدأ واقعی
        // مش (نه فقط مرکز bbox) استفاده شده باشه.
        NewActor->SetActorLocation(SpawnLocation);

        // اگه از حالت Grid اومده، این سل رو "استفاده‌شده" علامت بزن تا دوباره قابل انتخاب نباشه
        if (!PendingCellKey.IsEmpty() && WebBrowser.IsValid())
        {
            WebBrowser->ExecuteJavascript(FString::Printf(
                TEXT("if(window.HiroGeoMarkCellUsed) window.HiroGeoMarkCellUsed('%s');"), *PendingCellKey));
        }
        PendingCellKey.Empty();

        // توجه: دیگه پنجره رو بعد از هر Generate نمی‌بندیم، چون در حالت Grid باید بشه پشت‌سرهم چند ناحیه وارد کرد
        // اگه می‌خواید دوباره مثل قبل ببنده، این خط رو از کامنت خارج کنید:
        // FSlateApplication::Get().FindWidgetWindow(AsShared())->RequestDestroyWindow();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to spawn HiroGeoActor"));
    }
}

#undef LOCTEXT_NAMESPACE
