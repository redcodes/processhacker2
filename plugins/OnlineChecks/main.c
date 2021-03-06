/*
 * Process Hacker Online Checks -
 *   Main Program
 *
 * Copyright (C) 2010-2013 wj32
 * Copyright (C) 2012-2016 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "onlnchk.h"
#include "db.h"

PPH_PLUGIN PluginInstance;
PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
PH_CALLBACK_REGISTRATION PluginShowOptionsCallbackRegistration;
PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
PH_CALLBACK_REGISTRATION MainMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ModuleMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION TreeNewMessageCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessTreeNewInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ModulesTreeNewInitializingCallbackRegistration;

BOOLEAN VirusTotalScanningEnabled = FALSE;
ULONG ProcessesUpdatedCount = 0;
LIST_ENTRY ProcessListHead = { &ProcessListHead, &ProcessListHead };
PH_QUEUED_LOCK ProcessesListLock = PH_QUEUED_LOCK_INIT;

VOID ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
#ifdef VIRUSTOTAL_API
    static ULONG ProcessesUpdatedCount = 0;
    PLIST_ENTRY listEntry;

    if (!VirusTotalScanningEnabled)
        return;

    ProcessesUpdatedCount++;

    if (ProcessesUpdatedCount < 2)
        return;

    listEntry = ProcessListHead.Flink;

    while (listEntry != &ProcessListHead)
    {
        PPROCESS_EXTENSION extension;

        extension = CONTAINING_RECORD(listEntry, PROCESS_EXTENSION, ListEntry);

        PPH_STRING filePath = NULL;
        PPH_PROCESS_ITEM processItem = extension->ProcessItem;
        PPH_MODULE_ITEM moduleItem = extension->ModuleItem;

        if (processItem)
        {
            filePath = processItem->FileName;
        }
        else if (moduleItem)
        {
            filePath = moduleItem->FileName;
        }

        if (!PhIsNullOrEmptyString(filePath)) // !PH_IS_FAKE_PROCESS_ID(processItem->ProcessId)
        {
            if (!extension->ResultValid)
            {
                PPROCESS_DB_OBJECT object;

                if (object = FindProcessDbObject(&filePath->sr))
                {
                    extension->Stage1 = TRUE;
                    extension->ResultValid = TRUE;

                    extension->Positives = object->Positives;
                    PhSwapReference(&extension->VirusTotalResult, PhDuplicateString(object->Result));
                }
            }

            if (!extension->Stage1)
            {
                if (!VirusTotalGetCachedResult(filePath))
                {
                    VirusTotalAddCacheResult(filePath, extension);
                }

                extension->Stage1 = TRUE;
            }
        }

        listEntry = listEntry->Flink;
    }
#endif
}

VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (VirusTotalScanningEnabled = !!PhGetIntegerSetting(SETTING_NAME_VIRUSTOTAL_SCAN_ENABLED))
    {
        InitializeProcessDb();
        InitializeVirusTotalProcessMonitor();
    }
}

VOID NTAPI ShowOptionsCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    NOTHING;
}

VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;
    PPH_STRING fileName;

    switch (menuItem->Id)
    {
    case ENABLE_SERVICE_VIRUSTOTAL:
        {
            ULONG scanningEnabled = !VirusTotalScanningEnabled;

            PhSetIntegerSetting(SETTING_NAME_VIRUSTOTAL_SCAN_ENABLED, scanningEnabled);

            if (VirusTotalScanningEnabled != scanningEnabled)
            {
                INT result = IDOK;
                TASKDIALOGCONFIG config;

                memset(&config, 0, sizeof(TASKDIALOGCONFIG));
                config.cbSize = sizeof(TASKDIALOGCONFIG);
                config.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
                config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
                config.hwndParent = menuItem->OwnerWindow;
                config.hMainIcon = PhLoadIcon(
                    NtCurrentPeb()->ImageBaseAddress,
                    MAKEINTRESOURCE(PHAPP_IDI_PROCESSHACKER),
                    PH_LOAD_ICON_SIZE_LARGE,
                    GetSystemMetrics(SM_CXICON),
                    GetSystemMetrics(SM_CYICON)
                    );
                config.cxWidth = 180;
                config.pszWindowTitle = L"Process Hacker - VirusTotal";
                config.pszMainInstruction = L"VirusTotal scanning requires a restart of Process Hacker.";
                config.pszContent = L"Do you want to restart Process Hacker now?";

                if (SUCCEEDED(TaskDialogIndirect(&config, &result, NULL, NULL)) && result == IDYES)
                {
                    ProcessHacker_PrepareForEarlyShutdown(PhMainWndHandle);
                    PhShellProcessHacker(
                        PhMainWndHandle,
                        L"-v",
                        SW_SHOW,
                        0,
                        PH_SHELL_APP_PROPAGATE_PARAMETERS | PH_SHELL_APP_PROPAGATE_PARAMETERS_IGNORE_VISIBILITY,
                        0,
                        NULL
                        );
                    ProcessHacker_Destroy(PhMainWndHandle);
                }

                DestroyIcon(config.hMainIcon);
            }
        }
        break;
    case ID_SENDTO_SERVICE1:
        fileName = menuItem->Context;
        UploadToOnlineService(fileName, UPLOAD_SERVICE_VIRUSTOTAL);
        break;
    case ID_SENDTO_SERVICE2:
        fileName = menuItem->Context;
        UploadToOnlineService(fileName, UPLOAD_SERVICE_JOTTI);
        break;
    }
}

VOID NTAPI MainMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_EMENU_ITEM menuItem;

    if (menuInfo->u.MainMenu.SubMenuIndex != PH_MENU_ITEM_LOCATION_VIEW)
        return;

    menuItem = PhPluginCreateEMenuItem(PluginInstance, 0, ENABLE_SERVICE_VIRUSTOTAL, L"Enable VirusTotal scanning", NULL);
    PhInsertEMenuItem(menuInfo->Menu, PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_SEPARATOR, 0, NULL, NULL), -1);
    PhInsertEMenuItem(menuInfo->Menu, menuItem, -1);

    if (VirusTotalScanningEnabled)
        menuItem->Flags |= PH_EMENU_CHECKED;
}

PPH_EMENU_ITEM CreateSendToMenu(
    _In_ PPH_EMENU_ITEM Parent,
    _In_ PWSTR InsertAfter,
    _In_ PPH_STRING FileName
    )
{
    PPH_EMENU_ITEM sendToMenu;
    PPH_EMENU_ITEM menuItem;
    ULONG insertIndex;

    // Create the Send To menu.
    sendToMenu = PhPluginCreateEMenuItem(PluginInstance, 0, 0, L"Send to", NULL);
    PhInsertEMenuItem(sendToMenu, PhPluginCreateEMenuItem(PluginInstance, 0, ID_SENDTO_SERVICE1, L"virustotal.com", FileName), -1);
    PhInsertEMenuItem(sendToMenu, PhPluginCreateEMenuItem(PluginInstance, 0, ID_SENDTO_SERVICE2, L"virusscan.jotti.org", FileName), -1);
    //PhInsertEMenuItem(sendToMenu, PhPluginCreateEMenuItem(PluginInstance, 0, ID_SENDTO_SERVICE3, L"camas.comodo.com", FileName), -1);

    menuItem = PhFindEMenuItem(Parent, PH_EMENU_FIND_STARTSWITH, InsertAfter, 0);

    if (menuItem)
        insertIndex = PhIndexOfEMenuItem(Parent, menuItem);
    else
        insertIndex = -1;

    PhInsertEMenuItem(Parent, sendToMenu, insertIndex + 1);

    return sendToMenu;
}

VOID NTAPI ProcessMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_PROCESS_ITEM processItem;
    PPH_EMENU_ITEM sendToMenu;

    if (menuInfo->u.Process.NumberOfProcesses == 1)
        processItem = menuInfo->u.Process.Processes[0];
    else
        processItem = NULL;

    sendToMenu = CreateSendToMenu(menuInfo->Menu, L"Search online", processItem ? processItem->FileName : NULL);

    // Only enable the Send To menu if there is exactly one process selected and it has a file name.
    if (!processItem || !processItem->FileName)
    {
        sendToMenu->Flags |= PH_EMENU_DISABLED;
    }
}

VOID NTAPI ModuleMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_MODULE_ITEM moduleItem;
    PPH_EMENU_ITEM sendToMenu;

    if (menuInfo->u.Module.NumberOfModules == 1)
        moduleItem = menuInfo->u.Module.Modules[0];
    else
        moduleItem = NULL;

    sendToMenu = CreateSendToMenu(menuInfo->Menu, L"Search online", moduleItem ? moduleItem->FileName : NULL);

    if (!moduleItem)
    {
        sendToMenu->Flags |= PH_EMENU_DISABLED;
    }
}

LONG NTAPI VirusTotalProcessNodeSortFunction(
    _In_ PVOID Node1,
    _In_ PVOID Node2,
    _In_ ULONG SubId,
    _In_ PVOID Context
    )
{
    PPH_PROCESS_NODE node1 = Node1;
    PPH_PROCESS_NODE node2 = Node2;
    PPROCESS_EXTENSION extension1 = PhPluginGetObjectExtension(PluginInstance, node1->ProcessItem, EmProcessItemType);
    PPROCESS_EXTENSION extension2 = PhPluginGetObjectExtension(PluginInstance, node2->ProcessItem, EmProcessItemType);

    return PhCompareStringWithNull(extension1->VirusTotalResult, extension2->VirusTotalResult, TRUE);
}

LONG NTAPI VirusTotalModuleNodeSortFunction(
    _In_ PVOID Node1,
    _In_ PVOID Node2,
    _In_ ULONG SubId,
    _In_ PVOID Context
    )
{
    PPH_MODULE_NODE node1 = Node1;
    PPH_MODULE_NODE node2 = Node2;
    PPROCESS_EXTENSION extension1 = PhPluginGetObjectExtension(PluginInstance, node1->ModuleItem, EmModuleItemType);
    PPROCESS_EXTENSION extension2 = PhPluginGetObjectExtension(PluginInstance, node2->ModuleItem, EmModuleItemType);

    return PhCompareStringWithNull(extension1->VirusTotalResult, extension2->VirusTotalResult, TRUE);
}

VOID NTAPI ProcessTreeNewInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_INFORMATION info = Parameter;
    PH_TREENEW_COLUMN column;

    //*(HWND*)Context = info->TreeNewHandle;

    memset(&column, 0, sizeof(PH_TREENEW_COLUMN));
    column.Text = L"VirusTotal";
    column.Width = 140;
    column.Alignment = PH_ALIGN_CENTER;
    column.CustomDraw = TRUE;
    PhPluginAddTreeNewColumn(PluginInstance, info->CmData, &column, NETWORK_COLUMN_ID_VIUSTOTAL, NULL, VirusTotalProcessNodeSortFunction);
}

VOID NTAPI ModuleTreeNewInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_INFORMATION info = Parameter;
    PH_TREENEW_COLUMN column;

    //*(HWND*)Context = info->TreeNewHandle;

    memset(&column, 0, sizeof(PH_TREENEW_COLUMN));
    column.Text = L"VirusTotal";
    column.Width = 140;
    column.Alignment = PH_ALIGN_CENTER;
    column.CustomDraw = TRUE;
    PhPluginAddTreeNewColumn(PluginInstance, info->CmData, &column, NETWORK_COLUMN_ID_VIUSTOTAL_MODULE, NULL, VirusTotalModuleNodeSortFunction);
}

VOID NTAPI TreeNewMessageCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_MESSAGE message = Parameter;

    switch (message->Message)
    {
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = message->Parameter1;

            switch (message->SubId)
            {
            case NETWORK_COLUMN_ID_VIUSTOTAL:
                {
                    PPH_PROCESS_NODE processNode = (PPH_PROCESS_NODE)getCellText->Node;
                    PPROCESS_EXTENSION extension = PhPluginGetObjectExtension(PluginInstance, processNode->ProcessItem, EmProcessItemType);

                    getCellText->Text = PhGetStringRef(extension->VirusTotalResult);
                }
                break;
            case NETWORK_COLUMN_ID_VIUSTOTAL_MODULE:
                {
                    PPH_MODULE_NODE processNode = (PPH_MODULE_NODE)getCellText->Node;
                    PPROCESS_EXTENSION extension = PhPluginGetObjectExtension(PluginInstance, processNode->ModuleItem, EmModuleItemType);

                    getCellText->Text = PhGetStringRef(extension->VirusTotalResult);
                }
                break;
            }
        }
        break;
    case TreeNewCustomDraw:
        {
            PPH_TREENEW_CUSTOM_DRAW customDraw = message->Parameter1;
            PPROCESS_EXTENSION extension = NULL;
            PH_STRINGREF text;
            SIZE textSize;

            if (!VirusTotalScanningEnabled)
            {
                static PH_STRINGREF disabledText = PH_STRINGREF_INIT(L"VirusTotal disabled");

                GetTextExtentPoint32(
                    customDraw->Dc,
                    disabledText.Buffer,
                    (ULONG)disabledText.Length / 2,
                    &textSize
                    );

                DrawText(
                    customDraw->Dc,
                    disabledText.Buffer,
                    (ULONG)disabledText.Length / 2,
                    &customDraw->CellRect,
                    DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE
                    );

                return;
            }

            switch (message->SubId)
            {
            case NETWORK_COLUMN_ID_VIUSTOTAL:
                {
                    PPH_PROCESS_NODE processNode = (PPH_PROCESS_NODE)customDraw->Node;
                    
                    extension = PhPluginGetObjectExtension(PluginInstance, processNode->ProcessItem, EmProcessItemType);
                }
                break;
            case NETWORK_COLUMN_ID_VIUSTOTAL_MODULE:
                {
                    PPH_MODULE_NODE processNode = (PPH_MODULE_NODE)customDraw->Node;

                    extension = PhPluginGetObjectExtension(PluginInstance, processNode->ModuleItem, EmModuleItemType);
                }
                break;
            }

            if (!extension)
                break;

            if (extension->Positives)
            {
                //fontHandle = CommonCreateFont(-14, FW_BOLD, NULL);  
                SetTextColor(customDraw->Dc, RGB(0xff, 0, 0));
            }
            else
            {
                //fontHandle = CommonDuplicateFont((HFONT)SendMessage(ProcessTreeNewHandle, WM_GETFONT, 0, 0));
                SetTextColor(customDraw->Dc, RGB(0x64, 0x64, 0x64));
            }

            // originalFont = SelectObject(customDraw->Dc, fontHandle);
            text = PhGetStringRef(extension->VirusTotalResult);

            GetTextExtentPoint32(
                customDraw->Dc,
                text.Buffer,
                (ULONG)text.Length / 2,
                &textSize
                );

            DrawText(
                customDraw->Dc,
                text.Buffer,
                (ULONG)text.Length / 2,
                &customDraw->CellRect,
                DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE
                );

            //SelectObject(customDraw->Dc, originalFont);
            //DeleteObject(fontHandle);
        }
        break;
    }
}

VOID NTAPI ProcessItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_PROCESS_ITEM processItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    memset(extension, 0, sizeof(PROCESS_EXTENSION));

    extension->ProcessItem = processItem;
    extension->VirusTotalResult = PhReferenceEmptyString();

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    InsertTailList(&ProcessListHead, &extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ProcessItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_PROCESS_ITEM processItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    PhClearReference(&extension->VirusTotalResult);

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    RemoveEntryList(&extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ModuleItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_MODULE_ITEM moduleItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    memset(extension, 0, sizeof(PROCESS_EXTENSION));

    extension->ModuleItem = moduleItem;
    extension->VirusTotalResult = PhReferenceEmptyString();

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    InsertTailList(&ProcessListHead, &extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ModuleItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_MODULE_ITEM processItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    PhClearReference(&extension->VirusTotalResult);

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    RemoveEntryList(&extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        {
            PPH_PLUGIN_INFORMATION info;
            PH_SETTING_CREATE settings[] =
            {
                { IntegerSettingType, SETTING_NAME_VIRUSTOTAL_SCAN_ENABLED, L"0" },
            };

            PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

            if (!PluginInstance)
                return FALSE;

            info->DisplayName = L"Online Checks";
            info->Author = L"dmex, wj32";
            info->Description = L"Allows files to be checked with online services.";
            info->Url = L"https://wj32.org/processhacker/forums/viewtopic.php?t=1118";
            info->HasOptions = FALSE;

            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackLoad),
                LoadCallback,
                NULL,
                &PluginLoadCallbackRegistration
                );
            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackShowOptions),
                ShowOptionsCallback,
                NULL,
                &PluginShowOptionsCallbackRegistration
                );
            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem),
                MenuItemCallback,
                NULL,
                &PluginMenuItemCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackMainMenuInitializing),
                MainMenuInitializingCallback,
                NULL,
                &MainMenuInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessMenuInitializing),
                ProcessMenuInitializingCallback,
                NULL,
                &ProcessMenuInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackModuleMenuInitializing),
                ModuleMenuInitializingCallback,
                NULL,
                &ModuleMenuInitializingCallbackRegistration
                );

            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
                ProcessesUpdatedCallback,
                NULL,
                &ProcessesUpdatedCallbackRegistration
                );

            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessTreeNewInitializing),
                ProcessTreeNewInitializingCallback,
                NULL,
                &ProcessTreeNewInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackModuleTreeNewInitializing),
                ModuleTreeNewInitializingCallback,
                NULL,
                &ModulesTreeNewInitializingCallbackRegistration
                );

            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackTreeNewMessage),
                TreeNewMessageCallback,
                NULL,
                &TreeNewMessageCallbackRegistration
                );

            PhPluginSetObjectExtension(
                PluginInstance,
                EmProcessItemType,
                sizeof(PROCESS_EXTENSION),
                ProcessItemCreateCallback,
                ProcessItemDeleteCallback
                );

            PhPluginSetObjectExtension(
                PluginInstance,
                EmModuleItemType,
                sizeof(PROCESS_EXTENSION),
                ModuleItemCreateCallback,
                ModuleItemDeleteCallback
                );

            PhAddSettings(settings, ARRAYSIZE(settings));
        }
        break;
    }

    return TRUE;
}