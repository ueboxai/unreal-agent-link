#include "UAL_ContentBrowserExt.h"

#include "UAL_NetworkManager.h"

#include "ContentBrowserModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "FUAL_ContentBrowserExt"

void FUAL_ContentBrowserExt::Register()
{
	if (bRegistered)
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FContentBrowserMenuExtender_SelectedPaths>& Extenders = ContentBrowserModule.GetAllPathViewContextMenuExtenders();

	Extenders.Add(FContentBrowserMenuExtender_SelectedPaths::CreateRaw(this, &FUAL_ContentBrowserExt::OnExtendPathMenu));
	ExtenderHandle = Extenders.Last().GetHandle();
	bRegistered = true;
}

void FUAL_ContentBrowserExt::Unregister()
{
	if (!bRegistered)
	{
		return;
	}

	if (FModuleManager::Get().IsModuleLoaded("ContentBrowser"))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::GetModuleChecked<FContentBrowserModule>("ContentBrowser");
		TArray<FContentBrowserMenuExtender_SelectedPaths>& Extenders = ContentBrowserModule.GetAllPathViewContextMenuExtenders();
		Extenders.RemoveAll([Handle = ExtenderHandle](const FContentBrowserMenuExtender_SelectedPaths& Delegate)
		{
			return Delegate.GetHandle() == Handle;
		});
	}

	bRegistered = false;
}

TSharedRef<FExtender> FUAL_ContentBrowserExt::OnExtendPathMenu(const TArray<FString>& SelectedPaths)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	if (SelectedPaths.Num() > 0)
	{
		Extender->AddMenuExtension(
			"PathContextBulkOperations",
			EExtensionHook::After,
			nullptr,
			FMenuExtensionDelegate::CreateRaw(this, &FUAL_ContentBrowserExt::AddMenuEntry, SelectedPaths));
	}

	return Extender;
}

void FUAL_ContentBrowserExt::AddMenuEntry(FMenuBuilder& MenuBuilder, TArray<FString> SelectedPaths)
{
	MenuBuilder.BeginSection(NAME_None, LOCTEXT("UALSection", "虚幻盒子"));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("UALImportToAgent", "导入到虚幻盒子资产库"),
			LOCTEXT("UALImportToAgentTooltip", "将选中的文件夹及其内容导入到虚幻盒子中（虚幻盒子需要处于打开状态）"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
			FUIAction(FExecuteAction::CreateLambda([this, SelectedPaths]()
			{
				HandleImportToAgent(SelectedPaths);
			}))
		);
	}
	MenuBuilder.EndSection();
}

void FUAL_ContentBrowserExt::HandleImportToAgent(const TArray<FString>& SelectedPaths)
{
	// 构造发送的 JSON 报文
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (const FString& Path : SelectedPaths)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(Path));
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("paths"), PathsArray);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("evt"));
	Root->SetStringField(TEXT("method"), TEXT("content.import_folder"));
	Root->SetObjectField(TEXT("payload"), Payload);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
}

#undef LOCTEXT_NAMESPACE

