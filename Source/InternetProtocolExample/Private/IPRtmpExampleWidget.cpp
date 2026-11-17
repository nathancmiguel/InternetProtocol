/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPRtmpExampleWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Font.h"
#include "IPRtmpExampleActor.h"

namespace {
	const FLinearColor Accent(0.25f, 0.6f, 1.0f);
	const FLinearColor Muted(0.7f, 0.7f, 0.75f);
}

TSharedRef<SWidget> UIPRtmpExampleWidget::RebuildWidget() {
	if (WidgetTree && !WidgetTree->RootWidget) {
		BuildTree();
	}
	return Super::RebuildWidget();
}

UTextBlock* UIPRtmpExampleWidget::AddText(UPanelWidget* Parent, const FString& Text, int32 Size, FLinearColor Color) {
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>();
	Block->SetText(FText::FromString(Text));
	FSlateFontInfo FontInfo = Block->GetFont();
	if (Font) {
		FontInfo.FontObject = Font;
	}
	FontInfo.Size = Size;
	Block->SetFont(FontInfo);
	Block->SetColorAndOpacity(FSlateColor(Color));
	Block->SetAutoWrapText(true);
	Parent->AddChild(Block);
	return Block;
}

UVerticalBox* UIPRtmpExampleWidget::AddSection(UPanelWidget* Parent, const FString& Title) {
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>();
	Frame->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.85f));
	Frame->SetPadding(FMargin(12.0f));
	Parent->AddChild(Frame);
	if (UVerticalBoxSlot* PanelSlot = Cast<UVerticalBoxSlot>(Frame->Slot)) {
		PanelSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	}
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	Frame->SetContent(Box);
	AddText(Box, Title, 15, Accent);
	return Box;
}

UEditableTextBox* UIPRtmpExampleWidget::AddField(UPanelWidget* Parent, const FString& Label, const FString& Default, bool bPassword) {
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Parent->AddChild(Row);
	UTextBlock* Caption = AddText(Row, Label, 11, Muted);
	if (UHorizontalBoxSlot* PanelSlot = Cast<UHorizontalBoxSlot>(Caption->Slot)) {
		PanelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PanelSlot->SetVerticalAlignment(VAlign_Center);
	}
	UEditableTextBox* Field = WidgetTree->ConstructWidget<UEditableTextBox>();
	Field->SetText(FText::FromString(Default));
	Field->SetIsPassword(bPassword);
	Field->SetForegroundColor(FLinearColor::Black);
	FSlateFontInfo FieldFont = Field->WidgetStyle.TextStyle.Font;
	if (Font) {
		FieldFont.FontObject = Font;
	}
	FieldFont.Size = 11;
	Field->WidgetStyle.SetFont(FieldFont);
	Field->SetMinDesiredWidth(260.0f);
	Row->AddChild(Field);
	if (UHorizontalBoxSlot* PanelSlot = Cast<UHorizontalBoxSlot>(Field->Slot)) {
		PanelSlot->SetPadding(FMargin(0.0f, 2.0f));
	}
	return Field;
}

UComboBoxString* UIPRtmpExampleWidget::AddCombo(UPanelWidget* Parent, const FString& Label, const TArray<FString>& Options, int32 Selected) {
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Parent->AddChild(Row);
	UTextBlock* Caption = AddText(Row, Label, 11, Muted);
	if (UHorizontalBoxSlot* PanelSlot = Cast<UHorizontalBoxSlot>(Caption->Slot)) {
		PanelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PanelSlot->SetVerticalAlignment(VAlign_Center);
	}
	UComboBoxString* Combo = WidgetTree->ConstructWidget<UComboBoxString>();
	for (const FString& Option : Options) {
		Combo->AddOption(Option);
	}
	Combo->SetSelectedIndex(Selected);
	Row->AddChild(Combo);
	if (UHorizontalBoxSlot* PanelSlot = Cast<UHorizontalBoxSlot>(Combo->Slot)) {
		PanelSlot->SetPadding(FMargin(0.0f, 2.0f));
	}
	return Combo;
}

UButton* UIPRtmpExampleWidget::AddButton(UPanelWidget* Parent, const FString& Label) {
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Parent->AddChild(Button);
	if (UHorizontalBoxSlot* PanelSlot = Cast<UHorizontalBoxSlot>(Button->Slot)) {
		PanelSlot->SetPadding(FMargin(0.0f, 6.0f, 8.0f, 4.0f));
	}
	UTextBlock* Text = AddText(Button, Label, 12, FLinearColor::Black);
	Text->SetAutoWrapText(false);
	return Button;
}

void UIPRtmpExampleWidget::BuildTree() {
	// Same font as the plugin's other examples (falls back to the default font if it is missing).
	Font = LoadObject<UObject>(nullptr, TEXT("/InternetProtocol/Assets/Fonts/Montserrat-Medium_Font.Montserrat-Medium_Font"), nullptr, LOAD_NoWarn | LOAD_Quiet);

	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Canvas;

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	Canvas->AddChild(Scroll);
	if (UCanvasPanelSlot* PanelSlot = Cast<UCanvasPanelSlot>(Scroll->Slot)) {
		PanelSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 1.0f));
		PanelSlot->SetOffsets(FMargin(16.0f, 16.0f, 460.0f, 16.0f));
	}
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	Scroll->AddChild(Column);

	AddText(Column, TEXT("InternetProtocol RTMP example  -  Tab: UI / move (WASD + mouse)"), 11, Muted);

	// ---- Receive ----
	UVerticalBox* Receive = AddSection(Column, TEXT("Receive (OBS / ffmpeg -> screen)"));
	ReceivePort = AddField(Receive, TEXT("Port"), TEXT("1935"));
	ReceiveApp = AddField(Receive, TEXT("App"), TEXT("live"));
	ReceiveKey = AddField(Receive, TEXT("Stream key"), TEXT("demo"), true);
	ReceiveMode = AddCombo(Receive, TEXT("Latency mode"), {TEXT("LowLatency"), TEXT("Smooth")}, 0);
	ReceiveLatency = AddField(Receive, TEXT("Target latency (ms, 0 = default)"), TEXT("0"));
	{
		UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
		Receive->AddChild(Buttons);
		AddButton(Buttons, TEXT("Start server + receiver"))->OnClicked.AddDynamic(this, &UIPRtmpExampleWidget::OnStartReceive);
		AddButton(Buttons, TEXT("Stop"))->OnClicked.AddDynamic(this, &UIPRtmpExampleWidget::OnStopReceive);
	}
	ReceiveStatus = AddText(Receive, TEXT("Idle"), 12);
	ReceiveStats = AddText(Receive, TEXT(""), 10, Muted);
	ReceiveHint = AddText(Receive, TEXT(""), 10, Muted);

	// ---- Publish ----
	UVerticalBox* Publish = AddSection(Column, TEXT("Publish (Unreal -> RTMP server)"));
	PublishUrl = AddField(Publish, TEXT("Server URL"), TEXT("rtmp://127.0.0.1:1935/live"));
	PublishKey = AddField(Publish, TEXT("Stream key"), TEXT("unreal"), true);
	PublishSource = AddCombo(Publish, TEXT("Video source"), {TEXT("Viewport"), TEXT("Camera (SceneCapture)"), TEXT("Camera (RenderTarget)")}, 0);
	PublishSize = AddField(Publish, TEXT("Size (WxH)"), TEXT("1280x720"));
	PublishFps = AddField(Publish, TEXT("Frame rate"), TEXT("30"));
	PublishBitrate = AddField(Publish, TEXT("Video bitrate (kbit/s)"), TEXT("4500"));
	PublishAudio = AddCombo(Publish, TEXT("Audio"), {TEXT("Game audio (main submix)"), TEXT("None")}, 0);
	{
		UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
		Publish->AddChild(Buttons);
		AddButton(Buttons, TEXT("Publish"))->OnClicked.AddDynamic(this, &UIPRtmpExampleWidget::OnStartPublish);
		AddButton(Buttons, TEXT("Stop"))->OnClicked.AddDynamic(this, &UIPRtmpExampleWidget::OnStopPublish);
	}
	PublishStatus = AddText(Publish, TEXT("Idle"), 12);
	PublishStats = AddText(Publish, TEXT(""), 10, Muted);
	AddText(Publish, TEXT("Tip: publish to rtmp://127.0.0.1:<receive port>/<app> with the receive key to see yourself on the screen (loopback)."), 10, Muted);
}

int32 UIPRtmpExampleWidget::IntField(UEditableTextBox* Field, int32 Fallback) const {
	const FString Text = Field ? Field->GetText().ToString().TrimStartAndEnd() : FString();
	return Text.IsNumeric() ? FCString::Atoi(*Text) : Fallback;
}

void UIPRtmpExampleWidget::OnStartReceive() {
	if (!Example) {
		return;
	}
	const int32 Port = IntField(ReceivePort, 1935);
	const FString App = ReceiveApp->GetText().ToString().TrimStartAndEnd();
	const EIPRtmpLatencyMode Mode = ReceiveMode->GetSelectedIndex() == 1 ? EIPRtmpLatencyMode::Smooth : EIPRtmpLatencyMode::LowLatency;
	if (Example->StartReceiving(Port, App, ReceiveKey->GetText().ToString().TrimStartAndEnd(), Mode, IntField(ReceiveLatency, 0))) {
		// The key is shown masked; type it yourself in OBS/ffmpeg.
		ReceiveHint->SetText(FText::FromString(FString::Printf(
			TEXT("OBS: Server rtmp://<this machine>:%d/%s , Stream Key = the key above\nffmpeg: ffmpeg -re -i video.mp4 -c:v libx264 -c:a aac -f flv rtmp://<this machine>:%d/%s/<key>"),
			Port, *App, Port, *App)));
	}
}

void UIPRtmpExampleWidget::OnStopReceive() {
	if (Example) {
		Example->StopReceiving();
	}
	ReceiveHint->SetText(FText::GetEmpty());
}

void UIPRtmpExampleWidget::OnStartPublish() {
	if (!Example) {
		return;
	}
	int32 Width = 1280, Height = 720;
	FString Left, Right;
	if (PublishSize->GetText().ToString().Split(TEXT("x"), &Left, &Right)) {
		Width = Left.IsNumeric() ? FCString::Atoi(*Left) : Width;
		Height = Right.IsNumeric() ? FCString::Atoi(*Right) : Height;
	}
	EIPPublishVideoSource Source = EIPPublishVideoSource::Viewport;
	if (PublishSource->GetSelectedIndex() == 1) {
		Source = EIPPublishVideoSource::SceneCapture;
	} else if (PublishSource->GetSelectedIndex() == 2) {
		Source = EIPPublishVideoSource::RenderTarget;
	}
	Example->StartPublishing(PublishUrl->GetText().ToString().TrimStartAndEnd(), PublishKey->GetText().ToString().TrimStartAndEnd(), Source,
		Width, Height, IntField(PublishFps, 30), IntField(PublishBitrate, 4500), PublishAudio->GetSelectedIndex() == 0);
}

void UIPRtmpExampleWidget::OnStopPublish() {
	if (Example) {
		Example->StopPublishing();
	}
}

void UIPRtmpExampleWidget::NativeTick(const FGeometry& Geometry, float DeltaTime) {
	Super::NativeTick(Geometry, DeltaTime);
	StatsTimer -= DeltaTime;
	if (!Example || StatsTimer > 0.0f) {
		return;
	}
	StatsTimer = 0.5f;
	ReceiveStatus->SetText(FText::FromString(Example->GetReceiverStatus()));
	PublishStatus->SetText(FText::FromString(Example->GetPublisherStatus()));

	const FIPRtmpReceiverStats Rx = Example->GetReceiverStats();
	ReceiveStats->SetText(Rx.VideoDecoder.IsEmpty() && Rx.AudioDecoder.IsEmpty() ? FText::GetEmpty() : FText::FromString(FString::Printf(
		TEXT("%dx%d  %.1f fps  %.0f kbit/s  buffer %.0f ms\ndropped video %lld  audio %lld  rebuffers %d\n%s | %s  %d Hz %d ch"),
		Rx.Width, Rx.Height, Rx.Fps, Rx.BitrateKbps, Rx.BufferedMs, Rx.DroppedVideoFrames, Rx.DroppedAudioFrames, Rx.Rebuffers,
		*Rx.VideoDecoder, *Rx.AudioDecoder, Rx.AudioSampleRate, Rx.AudioChannels)));

	const FIPRtmpPublisherStats Tx = Example->GetPublisherStats();
	PublishStats->SetText(Tx.State == EIPRtmpPublisherState::Idle ? FText::GetEmpty() : FText::FromString(FString::Printf(
		TEXT("%.1f fps  %.0f kbit/s sent  queue %lld B  rtt %.0f ms\ndropped video %lld  audio %lld  reconnects %d\n%s | %s"),
		Tx.Fps, Tx.SentKbps, Tx.QueuedBytes, Tx.RttMs, Tx.DroppedVideoFrames, Tx.DroppedAudioFrames, Tx.ReconnectAttempts,
		*Tx.VideoEncoder, *Tx.AudioEncoder)));
}
