/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "IPRtmpExampleWidget.generated.h"

class AIPRtmpExampleActor;
class UButton;
class UComboBoxString;
class UEditableTextBox;
class UPanelWidget;
class UTextBlock;
class UVerticalBox;

/**
 * Control panel of the RTMP example. The layout is built in C++ (no Widget Blueprint needed); subclass it in a
 * Widget Blueprint if you want to restyle it.
 */
UCLASS()
class INTERNETPROTOCOLEXAMPLE_API UIPRtmpExampleWidget : public UUserWidget {
	GENERATED_BODY()

public:
	void SetExample(AIPRtmpExampleActor* InExample) { Example = InExample; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;

private:
	void BuildTree();
	UVerticalBox* AddSection(UPanelWidget* Parent, const FString& Title);
	UTextBlock* AddText(UPanelWidget* Parent, const FString& Text, int32 Size = 11, FLinearColor Color = FLinearColor::White);
	UEditableTextBox* AddField(UPanelWidget* Parent, const FString& Label, const FString& Default, bool bPassword = false);
	UComboBoxString* AddCombo(UPanelWidget* Parent, const FString& Label, const TArray<FString>& Options, int32 Selected);
	UButton* AddButton(UPanelWidget* Parent, const FString& Label);

	UFUNCTION()
	void OnStartReceive();
	UFUNCTION()
	void OnStopReceive();
	UFUNCTION()
	void OnStartPull();
	UFUNCTION()
	void OnStartPublish();
	UFUNCTION()
	void OnStopPublish();

	int32 IntField(UEditableTextBox* Field, int32 Fallback) const;

	UPROPERTY(Transient)
	TObjectPtr<AIPRtmpExampleActor> Example;

	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ReceivePort;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ReceiveApp;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ReceiveKey;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> ReceiveMode;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ReceiveLatency;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReceiveStatus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReceiveStats;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReceiveHint;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ReceiveViewKey;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PullUrl;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PullKey;

	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PublishUrl;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PublishKey;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> PublishSource;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PublishSize;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PublishFps;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PublishBitrate;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> PublishAudio;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PublishStatus;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PublishStats;

	UPROPERTY(Transient) TObjectPtr<UObject> Font;

	float StatsTimer = 0.0f;
};
