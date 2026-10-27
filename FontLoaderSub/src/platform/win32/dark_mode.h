#pragma once

#include <Windows.h>
#include <CommCtrl.h>

bool DarkModeInitialize();
void DarkModeShutdown();

void DarkModeTaskDialogNotification(HWND hwnd, UINT notification);
bool DarkModeHandleDialogMessage(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT *result);

HRESULT AppTaskDialog(
    HWND owner,
    HINSTANCE instance,
    PCWSTR windowTitle,
    PCWSTR mainInstruction,
    PCWSTR content,
    TASKDIALOG_COMMON_BUTTON_FLAGS commonButtons,
    PCWSTR icon,
    int *button);
