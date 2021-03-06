#include <iostream>
#include <vector>
#include "windows.h"
#include "Constants.h"
#include "MainModule.h"
#include "Input/Mouse.h"
#include "Input/Keyboard.h"
#include "Components/EmulatorComponent.h"
#include "Components/Input/InputEmulator.h"
#include "Components/Input/TouchPanelEmulator.h"
#include "Components/SysTimer.h"
#include "Components/PlayerDataManager.h"
#include "Components/FrameRateManager.h"
#include "Components/CameraController.h"
#include "Components/FastLoader.h"
#include "Components/DebugComponent.h"
#include "Utilities/Stopwatch.h"
#include "FileSystem/ConfigFile.h"

using namespace DivaHook::Components;
using namespace DivaHook::Utilities;
using namespace DivaHook::FileSystem;

namespace DivaHook
{
	const std::string COMPONENTS_CONFIG_FILE_NAME = "components.ini";

	const DWORD JMP_HOOK_SIZE = 0x5;

	const DWORD HookByteLength = 0x5;
	const void* UpdateReturnAddress = (void*)(ENGINE_UPDATE_HOOK_TARGET_ADDRESS + HookByteLength);
	const void* TouchReactionReturnAddress = (void*)(TOUCH_REACT_HOOK_TARGET_ADDRESS + HookByteLength);

	const float DefaultResolutionWidth = 1280.0f;
	const float DefaultResolutionHeight = 720.0f;

	void* originalTouchReactAetFunc = (void*)0x408510;

	int* resolutionWidthPtr = (int*)RESOLUTION_WIDTH_ADDRESS;
	int* resolutionHeightPtr = (int*)RESOLUTION_HEIGHT_ADDRESS;

	float renderWidth, renderHeight;

	float elpasedTime;
	bool hasWindowFocus, hadWindowFocus;
	bool firstUpdateTick = true;

	Stopwatch updateStopwatch;
	std::vector<EmulatorComponent*> components;

	void InstallHook(BYTE *address, DWORD overrideAddress, DWORD length)
	{
		DWORD oldProtect, dwBkup, dwRelAddr;

		VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &oldProtect);

		// calculate the distance between our address and our target location
		// and subtract the 5bytes, which is the size of the jmp
		// (0xE9 0xAA 0xBB 0xCC 0xDD) = 5 bytes
		dwRelAddr = (DWORD)(overrideAddress - (DWORD)address) - JMP_HOOK_SIZE;

		*address = JMP_OPCODE;

		// overwrite the next 4 bytes (which is the size of a DWORD)
		// with the dwRelAddr
		*((DWORD *)(address + 1)) = dwRelAddr;

		// overwrite the remaining bytes with the NOP opcode (0x90)
		for (DWORD x = JMP_HOOK_SIZE; x < length; x++)
			*(address + x) = NOP_OPCODE;

		VirtualProtect(address, length, oldProtect, &dwBkup);
	}

	void AddComponents()
	{
		EmulatorComponent *allComponents[]
		{
			new InputEmulator(),
			new TouchPanelEmulator(),
			new SysTimer(),
			new PlayerDataManager(),
			new FrameRateManager(),
			new CameraController(),
			new FastLoader(),
			new DebugComponent(),
		};

		ConfigFile componentsConfig(MainModule::GetModuleDirectory(), COMPONENTS_CONFIG_FILE_NAME);
		bool success = componentsConfig.OpenRead();

		if (!success)
		{
			printf("AddComponents(): Unable to parse %s\n", COMPONENTS_CONFIG_FILE_NAME.c_str());
			return;
		}

		int componentCount = sizeof(allComponents) / sizeof(EmulatorComponent*);
		components.reserve(componentCount);

		std::string trueString = "true";
		std::string falseString = "false";

		for (int i = 0; i < componentCount; i++)
		{
			std::string *value;

			auto name = allComponents[i]->GetDisplayName();
			//printf("AddComponents(): searching name: %s\n", name);

			if (componentsConfig.TryGetValue(name, value))
			{
				//printf("AddComponents(): %s found\n", name);

				if (*value == trueString)
				{
					//printf("AddComponents(): enabling %s...\n", name);
					components.push_back(allComponents[i]);
				}
				else if (*value == falseString)
				{
					//printf("AddComponents(): disabling %s...\n", name);
				}
				else
				{
					//printf("AddComponents(): invalid value %s for component %s\n", value, name);
				}

				delete value;
			}
			else
			{
				//printf("AddComponents(): component %s not found\n", name);
				delete allComponents[i];
			}
		}
	}

	void InitializeTick()
	{
		AddComponents();

		MainModule::DivaWindowHandle = FindWindow(0, MainModule::DivaWindowName);

		if (MainModule::DivaWindowHandle == NULL)
			MainModule::DivaWindowHandle = FindWindow(0, MainModule::GlutDefaultName);

		updateStopwatch.Start();
	}

	void UpdateTick()
	{
		if (firstUpdateTick)
		{
			InitializeTick();
			firstUpdateTick = false;

			for (auto& component : components)
				component->Initialize();
		}

		elpasedTime = updateStopwatch.Restart();

		for (auto& component : components)
		{
			component->SetElapsedTime(elpasedTime);
			component->Update();
		}

		hadWindowFocus = hasWindowFocus;
		hasWindowFocus = MainModule::DivaWindowHandle == NULL || GetForegroundWindow() == MainModule::DivaWindowHandle;

		if (hasWindowFocus)
		{
			Input::Keyboard::GetInstance()->PollInput();
			Input::Mouse::GetInstance()->PollInput();

			for (auto& component : components)
				component->UpdateInput();
		}

		if (hasWindowFocus && !hadWindowFocus)
		{
			for (auto& component : components)
				component->OnFocusGain();
		}

		if (!hasWindowFocus && hadWindowFocus)
		{
			for (auto& component : components)
				component->OnFocusLost();
		}
	}

	void _declspec(naked) UpdateFunctionHook()
	{
		_asm
		{
			call UpdateTick
			jmp[UpdateReturnAddress]
		}
	}

	void _declspec(naked) TouchReactionAetFunctionHook()
	{
		_asm
		{
			// this is the function we replaced with the hook jump
			call originalTouchReactAetFunc

			// move X/Y into registers
			movss xmm1, [esp + 0x154 + 0x4]
			movss xmm2, [esp + 0x154 + 0x8]

			// calculate X scale factor
			movss xmm3, DefaultResolutionWidth
			divss xmm3, renderWidth

			// calculate Y scale factor
			movss xmm4, DefaultResolutionHeight
			divss xmm4, renderHeight

			// multiply positions by scale factors
			mulss xmm1, xmm3
			mulss xmm2, xmm4

			// move X/Y back into stack variables
			movss[esp + 0x154 + 0x4], xmm1
			movss[esp + 0x154 + 0x8], xmm2

			// return to original function
			jmp[TouchReactionReturnAddress]
		}
	}

	void InitializeHooks()
	{
		renderWidth = (float)*resolutionWidthPtr;
		renderHeight = (float)*resolutionHeightPtr;

		InstallHook((BYTE*)ENGINE_UPDATE_HOOK_TARGET_ADDRESS, (DWORD)UpdateFunctionHook, HookByteLength);

		if (renderWidth != DefaultResolutionWidth || renderHeight != DefaultResolutionHeight)
			InstallHook((BYTE*)TOUCH_REACT_HOOK_TARGET_ADDRESS, (DWORD)TouchReactionAetFunctionHook, HookByteLength);
	}

	void Dispose()
	{
		for (auto& component : components)
			delete component;
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		DivaHook::InitializeHooks();
		DivaHook::MainModule::Module = hModule;
		break;

	case DLL_PROCESS_DETACH:
		DivaHook::Dispose();
		break;
	}

	return TRUE;
}