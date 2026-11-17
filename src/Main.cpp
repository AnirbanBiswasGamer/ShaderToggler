///////////////////////////////////////////////////////////////////////
//
// Part of ShaderToggler, a shader toggler add on for Reshade 5+ which allows you
// to define groups of shaders to toggle them on/off with one key press
// 
// (c) Frans 'Otis_Inf' Bouma.
//
// All rights reserved.
// https://github.com/FransBouma/ShaderToggler
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/////////////////////////////////////////////////////////////////////////

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID unsigned long long // Change ImGui texture ID type to that of a 'reshade::api::resource_view' handle

#include <imgui.h>
#include <reshade.hpp>
#include "crc32_hash.hpp"
#include "ShaderManager.h"
#include "CDataFile.h"
#include "ToggleGroup.h"
#include <vector>
#include <filesystem>
#include <chrono>
#include <windows.h>
#include <Xinput.h>
#include <shared_mutex>
#include <sstream>

using namespace reshade::api;
using namespace ShaderToggler;

extern "C" __declspec(dllexport) const char *NAME = "Shader Toggler";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Add-on which allows you to define groups of game shaders to toggle on/off with one key press.";

struct __declspec(uuid("038B03AA-4C75-443B-A695-752D80797037")) CommandListDataContainer {
    uint64_t activePixelShaderPipeline = 0;
    uint64_t activeVertexShaderPipeline = 0;
	uint64_t activeComputeShaderPipeline = 0;
};

#define FRAMECOUNT_COLLECTION_PHASE_DEFAULT 250;
#define HASH_FILE_NAME	"ShaderToggler.ini"

static const char* s_huntingKeyLabels[] = {
	"Pixel shader: previous",
	"Pixel shader: next",
	"Pixel shader: mark/unmark",
	"Vertex shader: previous",
	"Vertex shader: next",
	"Vertex shader: mark/unmark",
	"Compute shader: previous",
	"Compute shader: next",
	"Compute shader: mark/unmark"
};

static const char* s_huntingKeyIniNames[] = {
	"PixelPrevKey",
	"PixelNextKey",
	"PixelMarkKey",
	"VertexPrevKey",
	"VertexNextKey",
	"VertexMarkKey",
	"ComputePrevKey",
	"ComputeNextKey",
	"ComputeMarkKey"
};

static KeyData g_huntingKeys[9];
static KeyData g_masterToggleKey;
static bool g_isMasterKeyEditing = false;
static int g_huntingKeyEditingIndex = -1;

static std::string g_defaultIniFileName = "";
static std::string g_gameSpecificIniFileName = "";

struct KeyRepeatTracker
{
	std::chrono::steady_clock::time_point pressStartTime;
	std::chrono::steady_clock::time_point lastRepeatTime;
	bool isHeld = false;

	bool check(bool isDown, bool isInitialPress)
	{
		auto now = std::chrono::steady_clock::now();
		if (isInitialPress)
		{
			isHeld = true;
			pressStartTime = now;
			lastRepeatTime = now;
			return true;
		}
		if (isDown && isHeld)
		{
			auto holdMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - pressStartTime).count();
			if (holdMs >= 350)
			{
				auto repeatMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRepeatTime).count();
				if (repeatMs >= 60)
				{
					lastRepeatTime = now;
					return true;
				}
			}
		}
		else
		{
			isHeld = false;
		}
		return false;
	}
};
static KeyRepeatTracker g_repeatTrackers[6];

static bool g_allGroupsDisabledByMaster = false;
static std::vector<bool> g_savedGroupStates;

static void initHuntingKeysDefaults()
{
	g_huntingKeys[0].setKey(VK_NUMPAD1);
	g_huntingKeys[1].setKey(VK_NUMPAD2);
	g_huntingKeys[2].setKey(VK_NUMPAD3);
	g_huntingKeys[3].setKey(VK_NUMPAD4);
	g_huntingKeys[4].setKey(VK_NUMPAD5);
	g_huntingKeys[5].setKey(VK_NUMPAD6);
	g_huntingKeys[6].setKey(VK_NUMPAD7);
	g_huntingKeys[7].setKey(VK_NUMPAD8);
	g_huntingKeys[8].setKey(VK_NUMPAD9);
}

static ShaderToggler::ShaderManager g_pixelShaderManager;
static ShaderToggler::ShaderManager g_vertexShaderManager;
static ShaderToggler::ShaderManager g_computeShaderManager;
static KeyData g_keyCollector;
static atomic_uint32_t g_activeCollectorFrameCounter = 0;
static std::vector<ToggleGroup> g_toggleGroups;
static atomic_int g_toggleGroupIdKeyBindingEditing = -1;
static atomic_int g_toggleGroupIdShaderEditing = -1;
static float g_overlayOpacity = 1.0f;
static int g_startValueFramecountCollectionPhase = FRAMECOUNT_COLLECTION_PHASE_DEFAULT;
static std::string g_iniFileName = "";

// XInput dynamic loader and controller helpers
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, XINPUT_STATE* pState);
static HMODULE g_hXInputDll = nullptr;
static PFN_XInputGetState g_pfnXInputGetState = nullptr;
static bool g_xinputInitialized = false;
static bool g_enableGamepad = false;
static WORD g_prevGamepadButtons = 0;

static void initXInput()
{
	if(g_xinputInitialized)
	{
		return;
	}
	g_xinputInitialized = true;
	g_hXInputDll = LoadLibraryW(L"xinput1_4.dll");
	if(g_hXInputDll == nullptr)
	{
		g_hXInputDll = LoadLibraryW(L"xinput1_3.dll");
	}
	if(g_hXInputDll != nullptr)
	{
		g_pfnXInputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(g_hXInputDll, "XInputGetState"));
	}
}

static void copyTextToClipboard(const std::string& text)
{
	if(OpenClipboard(nullptr))
	{
		EmptyClipboard();
		HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
		if(hGlob)
		{
			void* pBuf = GlobalLock(hGlob);
			if(pBuf)
			{
				memcpy(pBuf, text.c_str(), text.size() + 1);
				GlobalUnlock(hGlob);
				SetClipboardData(CF_TEXT, hGlob);
			}
			else
			{
				GlobalFree(hGlob);
			}
		}
		CloseClipboard();
	}
}

// Flat hash set cache for high-throughput draw call blocking
static std::unordered_set<uint32_t> g_activeBlockedPixelHashes;
static std::unordered_set<uint32_t> g_activeBlockedVertexHashes;
static std::unordered_set<uint32_t> g_activeBlockedComputeHashes;
static std::vector<const ToggleGroup*> g_activeInvertedGroups;
static std::shared_mutex g_activeBlockedHashesMutex;
static std::atomic<bool> g_hasActiveBlockedShaders = false;

static void rebuildActiveBlockedShaderHashesCache()
{
	std::unique_lock lock(g_activeBlockedHashesMutex);
	g_activeBlockedPixelHashes.clear();
	g_activeBlockedVertexHashes.clear();
	g_activeBlockedComputeHashes.clear();
	g_activeInvertedGroups.clear();

	for(const auto& group : g_toggleGroups)
	{
		if(!group.isActive())
		{
			continue;
		}
		if(group.isInverted())
		{
			g_activeInvertedGroups.push_back(&group);
		}
		else
		{
			for(uint32_t hash : group.getPixelShaderHashes())
			{
				g_activeBlockedPixelHashes.emplace(hash);
			}
			for(uint32_t hash : group.getVertexShaderHashes())
			{
				g_activeBlockedVertexHashes.emplace(hash);
			}
			for(uint32_t hash : group.getComputeShaderHashes())
			{
				g_activeBlockedComputeHashes.emplace(hash);
			}
		}
	}
	g_hasActiveBlockedShaders = !g_activeBlockedPixelHashes.empty() ||
	                            !g_activeBlockedVertexHashes.empty() ||
	                            !g_activeBlockedComputeHashes.empty() ||
	                            !g_activeInvertedGroups.empty();
}

static void toggleAllGroups()
{
	bool anyActive = false;
	for (const auto& group : g_toggleGroups)
	{
		if (group.isActive())
		{
			anyActive = true;
			break;
		}
	}

	if (anyActive)
	{
		g_savedGroupStates.clear();
		for (auto& group : g_toggleGroups)
		{
			g_savedGroupStates.push_back(group.isActive());
			if (group.isActive())
			{
				group.toggleActive();
			}
		}
		g_allGroupsDisabledByMaster = true;
	}
	else
	{
		if (g_allGroupsDisabledByMaster && g_savedGroupStates.size() == g_toggleGroups.size())
		{
			for (size_t i = 0; i < g_toggleGroups.size(); ++i)
			{
				if (g_savedGroupStates[i] && !g_toggleGroups[i].isActive())
				{
					g_toggleGroups[i].toggleActive();
				}
			}
		}
		else
		{
			for (auto& group : g_toggleGroups)
			{
				if (!group.isActive())
				{
					group.toggleActive();
				}
			}
		}
		g_allGroupsDisabledByMaster = false;
	}
	rebuildActiveBlockedShaderHashesCache();
}

/// <summary>
/// Calculates a crc32 hash from the passed in shader bytecode. The hash is used to identity the shader in future runs.
/// </summary>
/// <param name="shaderData"></param>
/// <returns></returns>
static uint32_t calculateShaderHash(void* shaderData)
{
	if(nullptr==shaderData)
	{
		return 0;
	}

	const auto shaderDesc = *static_cast<shader_desc *>(shaderData);
	return compute_crc32(static_cast<const uint8_t *>(shaderDesc.code), shaderDesc.code_size);
}


/// <summary>
/// Adds a default group with VK_CAPITAL as toggle key. Only used if there aren't any groups defined in the ini file.
/// </summary>
void addDefaultGroup()
{
	ToggleGroup toAdd("Default", ToggleGroup::getNewGroupId());
	toAdd.setToggleKey(VK_CAPITAL, false, false, false);
	g_toggleGroups.push_back(toAdd);
	rebuildActiveBlockedShaderHashesCache();
}


/// <summary>
/// Loads the defined hashes and groups from the shaderToggler.ini file.
/// </summary>
void loadShaderTogglerIniFile()
{
	initHuntingKeysDefaults();
	g_toggleGroups.clear();

	CDataFile iniFile;
	if(!iniFile.Load(g_iniFileName))
	{
		addDefaultGroup();
		return;
	}

	// Load general settings
	g_enableGamepad = iniFile.GetBool("EnableGamepad", "General");

	// Load master toggle key
	const uint32_t masterKeyVal = iniFile.GetUInt("MasterToggleKey", "General");
	if(masterKeyVal != UINT_MAX && masterKeyVal != 0)
	{
		g_masterToggleKey.setKeyFromIniFile(masterKeyVal);
	}
	else
	{
		g_masterToggleKey.clear();
	}

	// Load hunting keys
	for(int i = 0; i < 9; ++i)
	{
		const uint32_t keyVal = iniFile.GetUInt(s_huntingKeyIniNames[i], "HuntingKeys");
		if(keyVal != UINT_MAX && keyVal != 0)
		{
			g_huntingKeys[i].setKeyFromIniFile(keyVal);
		}
	}

	int groupCounter = 0;
	const int numberOfGroups = iniFile.GetInt("AmountGroups", "General");
	if(numberOfGroups==INT_MIN)
	{
		// old format file?
		addDefaultGroup();
		groupCounter=-1;	// enforce old format read for pre 1.0 ini file.
	}
	else
	{
		for(int i=0;i<numberOfGroups;i++)
		{
			g_toggleGroups.push_back(ToggleGroup("", ToggleGroup::getNewGroupId()));
		}
	}
	for(auto& group: g_toggleGroups)
	{
		group.loadState(iniFile, groupCounter);		// groupCounter is normally 0 or greater. For when the old format is detected, it's -1 (and there's 1 group).
		groupCounter++;
	}
	rebuildActiveBlockedShaderHashesCache();
}


/// <summary>
/// Saves the currently known toggle groups with their shader hashes to the shadertoggler.ini file
/// </summary>
void saveShaderTogglerIniFile()
{
	// format: first section with # of groups, then per group a section with pixel and vertex shaders, as well as their name and key value.
	// groups are stored with "Group" + group counter, starting with 0.
	CDataFile iniFile;
	iniFile.SetInt("AmountGroups", static_cast<int>(g_toggleGroups.size()), "",  "General");
	iniFile.SetBool("EnableGamepad", g_enableGamepad, "", "General");
	iniFile.SetUInt("MasterToggleKey", g_masterToggleKey.getKeyForIniFile(), "", "General");

	for(int i = 0; i < 9; ++i)
	{
		iniFile.SetUInt(s_huntingKeyIniNames[i], g_huntingKeys[i].getKeyForIniFile(), "", "HuntingKeys");
	}

	int groupCounter = 0;
	for(const auto& group: g_toggleGroups)
	{
		group.saveState(iniFile, groupCounter);
		groupCounter++;
	}
	iniFile.SetFileName(g_iniFileName);
	iniFile.Save();
}


static void onInitCommandList(command_list *commandList)
{
	commandList->create_private_data<CommandListDataContainer>();
}


static void onDestroyCommandList(command_list *commandList)
{
	commandList->destroy_private_data<CommandListDataContainer>();
}

static void onResetCommandList(command_list *commandList)
{
	CommandListDataContainer &commandListData = commandList->get_private_data<CommandListDataContainer>();
	commandListData.activePixelShaderPipeline = 0;
	commandListData.activeVertexShaderPipeline = 0;
	commandListData.activeComputeShaderPipeline = 0;
}


static void onInitPipeline(device *device, pipeline_layout, uint32_t subobjectCount, const pipeline_subobject *subobjects, pipeline pipelineHandle)
{
	// shader has been created, we will now create a hash and store it with the handle we got.
	for (uint32_t i = 0; i < subobjectCount; ++i)
	{
		switch (subobjects[i].type)
		{
			case pipeline_subobject_type::vertex_shader:
				g_vertexShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
				break;
			case pipeline_subobject_type::pixel_shader:
				g_pixelShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
				break;
			case pipeline_subobject_type::compute_shader:
				g_computeShaderManager.addHashHandlePair(calculateShaderHash(subobjects[i].data), pipelineHandle.handle);
				break;
		}
	}
}


static void onDestroyPipeline(device *device, pipeline pipelineHandle)
{
	g_pixelShaderManager.removeHandle(pipelineHandle.handle);
	g_vertexShaderManager.removeHandle(pipelineHandle.handle);
	g_computeShaderManager.removeHandle(pipelineHandle.handle);
}


static void displayIsPartOfToggleGroup()
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
	ImGui::SameLine();
	ImGui::Text(" Shader is part of this toggle group.");
	ImGui::PopStyleColor();
}


static void displayShaderManagerInfo(ShaderManager& toDisplay, const char* shaderType)
{
	if(toDisplay.isInHuntingMode())
	{
		ImGui::Text("# of %s shaders active: %d. # of %s shaders in group: %d", shaderType, toDisplay.getAmountShaderHashesCollected(), shaderType, toDisplay.getMarkedShaderCount());
		const int total = toDisplay.getAmountShaderHashesCollected();
		const int idx = (total > 0) ? (toDisplay.getActiveHuntedShaderIndex() + 1) : 0;
		ImGui::Text("Current selected %s shader: %d / %d (Hash: 0x%08X)", shaderType, idx, total, toDisplay.getActiveHuntedShaderHash());
		if(toDisplay.isHuntedShaderMarked())
		{
			displayIsPartOfToggleGroup();
		}
	}
}

static void displayShaderManagerStats(ShaderManager& toDisplay, const char* shaderType)
{
	ImGui::Text("# of pipelines with %s shaders: %d. # of different %s shaders gathered: %d.", shaderType, toDisplay.getPipelineCount(), shaderType, toDisplay.getShaderCount());
}


static void onReshadeOverlay(reshade::api::effect_runtime *runtime)
{
	if(g_toggleGroupIdShaderEditing>=0)
	{
		ImGui::SetNextWindowBgAlpha(g_overlayOpacity);
		ImGui::SetNextWindowPos(ImVec2(10, 10));
		if (!ImGui::Begin("ShaderTogglerInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | 
														ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::End();
			return;
		}
		string editingGroupName = "";
		for(auto& group:g_toggleGroups)
		{
			if(group.getId()==g_toggleGroupIdShaderEditing)
			{
				editingGroupName = group.getName();
				break;
			}
		}
		
		displayShaderManagerStats(g_vertexShaderManager, "vertex");
		displayShaderManagerStats(g_pixelShaderManager, "pixel");
		displayShaderManagerStats(g_computeShaderManager, "compute");

		if(g_activeCollectorFrameCounter > 0)
		{
			const uint32_t counterValue = g_activeCollectorFrameCounter;
			ImGui::Text("Collecting active shaders... frames to go: %d", counterValue);
		}
		else
		{
			if(g_vertexShaderManager.isInHuntingMode() || g_pixelShaderManager.isInHuntingMode() || g_computeShaderManager.isInHuntingMode())
			{
				ImGui::Text("Editing the shaders for group: %s", editingGroupName.c_str());
			}
			displayShaderManagerInfo(g_vertexShaderManager, "vertex");
			displayShaderManagerInfo(g_pixelShaderManager, "pixel");
			displayShaderManagerInfo(g_computeShaderManager, "compute");
		}
		ImGui::End();
	}
}


static void onBindPipeline(command_list* commandList, pipeline_stage stages, pipeline pipelineHandle)
{
	if(nullptr != commandList)
	{
		CommandListDataContainer& commandListData = commandList->get_private_data<CommandListDataContainer>();

		if(pipelineHandle.handle == 0)
		{
			if((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader)
			{
				commandListData.activePixelShaderPipeline = 0;
			}
			if((stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader)
			{
				commandListData.activeVertexShaderPipeline = 0;
			}
			if((stages & pipeline_stage::compute_shader) == pipeline_stage::compute_shader)
			{
				commandListData.activeComputeShaderPipeline = 0;
			}
			return;
		}

		const bool handleHasPixelShaderAttached = g_pixelShaderManager.isKnownHandle(pipelineHandle.handle);
		const bool handleHasVertexShaderAttached = g_vertexShaderManager.isKnownHandle(pipelineHandle.handle);
		const bool handleHasComputeShaderAttached = g_computeShaderManager.isKnownHandle(pipelineHandle.handle);
		if(!handleHasPixelShaderAttached && !handleHasVertexShaderAttached && !handleHasComputeShaderAttached)
		{
			// draw call with unknown handle, don't collect it
			return;
		}
		// always do the following code as that has to run for every bind on a pipeline:
		if(g_activeCollectorFrameCounter > 0)
		{
			// in collection mode
			if(handleHasPixelShaderAttached)
			{
				g_pixelShaderManager.addActivePipelineHandle(pipelineHandle.handle);
			}
			if(handleHasVertexShaderAttached)
			{
				g_vertexShaderManager.addActivePipelineHandle(pipelineHandle.handle);
			}
			if(handleHasComputeShaderAttached)
			{
				g_computeShaderManager.addActivePipelineHandle(pipelineHandle.handle);
			}
		}
		else
		{
			commandListData.activePixelShaderPipeline = handleHasPixelShaderAttached ? pipelineHandle.handle : commandListData.activePixelShaderPipeline;
			commandListData.activeVertexShaderPipeline = handleHasVertexShaderAttached ? pipelineHandle.handle : commandListData.activeVertexShaderPipeline;
			commandListData.activeComputeShaderPipeline = handleHasComputeShaderAttached ? pipelineHandle.handle : commandListData.activeComputeShaderPipeline;
		}
		if((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader)
		{
			if(handleHasPixelShaderAttached)
			{
				if(g_activeCollectorFrameCounter > 0)
				{
					// in collection mode
					g_pixelShaderManager.addActivePipelineHandle(pipelineHandle.handle);
				}
				commandListData.activePixelShaderPipeline = pipelineHandle.handle;
			}
		}
		if((stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader)
		{
			if(handleHasVertexShaderAttached)
			{
				if(g_activeCollectorFrameCounter > 0)
				{
					// in collection mode
					g_vertexShaderManager.addActivePipelineHandle(pipelineHandle.handle);
				}
				commandListData.activeVertexShaderPipeline = pipelineHandle.handle;
			}
		}
		if((stages & pipeline_stage::compute_shader) == pipeline_stage::compute_shader)
		{
			if(handleHasComputeShaderAttached)
			{
				if(g_activeCollectorFrameCounter > 0)
				{
					// in collection mode
					g_computeShaderManager.addActivePipelineHandle(pipelineHandle.handle);
				}
				commandListData.activeComputeShaderPipeline = pipelineHandle.handle;
			}
		}
	}
}


/// <summary>
/// This function will return true if the command list specified has one or more shader hashes which are currently marked to be hidden. Otherwise false.
/// Uses aggregated flat hash set cache for maximum draw call throughput.
/// </summary>
/// <param name="commandList"></param>
/// <returns>true if the draw call has to be blocked</returns>
bool blockDrawCallForCommandList(command_list* commandList)
{
	if(nullptr == commandList)
	{
		return false;
	}

	const bool huntingActive = g_pixelShaderManager.isInHuntingMode() || 
	                           g_vertexShaderManager.isInHuntingMode() || 
	                           g_computeShaderManager.isInHuntingMode();

	if(!huntingActive && !g_hasActiveBlockedShaders.load(std::memory_order_relaxed))
	{
		return false;
	}

	const CommandListDataContainer &commandListData = commandList->get_private_data<CommandListDataContainer>();

	std::shared_lock lock(g_activeBlockedHashesMutex);

	// Check pixel shader
	if(commandListData.activePixelShaderPipeline != 0)
	{
		const uint32_t shaderHash = g_pixelShaderManager.getShaderHash(commandListData.activePixelShaderPipeline);
		if(shaderHash != 0)
		{
			if(g_pixelShaderManager.isBlockedShader(shaderHash) || g_activeBlockedPixelHashes.count(shaderHash) > 0)
			{
				return true;
			}
			for(const auto* invGroup : g_activeInvertedGroups)
			{
				if(invGroup->isBlockedPixelShader(shaderHash))
				{
					return true;
				}
			}
		}
	}

	// Check vertex shader
	if(commandListData.activeVertexShaderPipeline != 0)
	{
		const uint32_t shaderHash = g_vertexShaderManager.getShaderHash(commandListData.activeVertexShaderPipeline);
		if(shaderHash != 0)
		{
			if(g_vertexShaderManager.isBlockedShader(shaderHash) || g_activeBlockedVertexHashes.count(shaderHash) > 0)
			{
				return true;
			}
			for(const auto* invGroup : g_activeInvertedGroups)
			{
				if(invGroup->isBlockedVertexShader(shaderHash))
				{
					return true;
				}
			}
		}
	}

	// Check compute shader
	if(commandListData.activeComputeShaderPipeline != 0)
	{
		const uint32_t shaderHash = g_computeShaderManager.getShaderHash(commandListData.activeComputeShaderPipeline);
		if(shaderHash != 0)
		{
			if(g_computeShaderManager.isBlockedShader(shaderHash) || g_activeBlockedComputeHashes.count(shaderHash) > 0)
			{
				return true;
			}
			for(const auto* invGroup : g_activeInvertedGroups)
			{
				if(invGroup->isBlockedComputeShader(shaderHash))
				{
					return true;
				}
			}
		}
	}

	return false;
}


static bool onDraw(command_list* commandList, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
	// check if for this command list the active shader handles are part of the blocked set. If so, return true
	return blockDrawCallForCommandList(commandList);
}


static bool onDrawIndexed(command_list* commandList, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	// same as onDraw
	return blockDrawCallForCommandList(commandList);
}


static bool onDrawOrDispatchIndirect(command_list* commandList, indirect_command type, resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride)
{
	switch(type)
	{
		case indirect_command::unknown:
		case indirect_command::draw:
		case indirect_command::draw_indexed:
		case indirect_command::dispatch:
			// same as OnDraw
			return blockDrawCallForCommandList(commandList);
		// the rest aren't blocked
	}
	return false;
}


static bool onDispatch(command_list* commandList, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
	return blockDrawCallForCommandList(commandList);
}


static void onReshadePresent(effect_runtime* runtime)
{
	if(g_activeCollectorFrameCounter>0)
	{
		--g_activeCollectorFrameCounter;
	}

	bool cacheNeedsRebuild = false;

	// Check auto-hide expiration
	for(auto& group : g_toggleGroups)
	{
		if(group.checkAutoHideExpired())
		{
			cacheNeedsRebuild = true;
		}
	}

	// Check master toggle key
	if(g_masterToggleKey.isValid() && g_masterToggleKey.isKeyPressed(runtime))
	{
		toggleAllGroups();
		cacheNeedsRebuild = true;
	}

	// Check toggle keys for each group (supporting Toggle, Hold, and AutoHide modes)
	for(auto& group : g_toggleGroups)
	{
		if(group.getToggleMode() == ToggleMode::Hold)
		{
			const bool isDown = group.isToggleKeyDown(runtime);
			if(group.isActive() != isDown)
			{
				group.setActive(isDown);
				cacheNeedsRebuild = true;
				if(group.getId() == g_toggleGroupIdShaderEditing)
				{
					g_vertexShaderManager.toggleHideMarkedShaders();
					g_pixelShaderManager.toggleHideMarkedShaders();
					g_computeShaderManager.toggleHideMarkedShaders();
				}
			}
		}
		else
		{
			if(group.isToggleKeyPressed(runtime))
			{
				group.toggleActive();
				cacheNeedsRebuild = true;
				// if the group's shaders are being edited, it should toggle the ones currently marked.
				if(group.getId() == g_toggleGroupIdShaderEditing)
				{
					g_vertexShaderManager.toggleHideMarkedShaders();
					g_pixelShaderManager.toggleHideMarkedShaders();
					g_computeShaderManager.toggleHideMarkedShaders();
				}
			}
		}
	}

	// Gamepad (XInput) support
	if(g_enableGamepad && g_pfnXInputGetState != nullptr)
	{
		XINPUT_STATE padState;
		ZeroMemory(&padState, sizeof(XINPUT_STATE));
		if(g_pfnXInputGetState(0, &padState) == ERROR_SUCCESS)
		{
			const WORD currentButtons = padState.Gamepad.wButtons;
			const WORD pressedButtons = currentButtons & ~g_prevGamepadButtons;

			// Master Toggle combo: LB + RB + D-Pad Down
			const WORD masterCombo = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_DOWN;
			if((currentButtons & masterCombo) == masterCombo && (pressedButtons & masterCombo))
			{
				toggleAllGroups();
				cacheNeedsRebuild = true;
			}

			// In hunting mode: D-Pad Left/Right to hunt prev/next, D-Pad Up to mark/unmark
			if(g_toggleGroupIdShaderEditing >= 0)
			{
				const int padStep = ((currentButtons & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) != 0) ? 10 : 1;
				ShaderManager* activeManager = nullptr;
				if(g_pixelShaderManager.isInHuntingMode()) activeManager = &g_pixelShaderManager;
				else if(g_vertexShaderManager.isInHuntingMode()) activeManager = &g_vertexShaderManager;
				else if(g_computeShaderManager.isInHuntingMode()) activeManager = &g_computeShaderManager;

				if(activeManager != nullptr)
				{
					if(pressedButtons & XINPUT_GAMEPAD_DPAD_LEFT)
					{
						activeManager->huntPreviousShader(false, padStep);
					}
					if(pressedButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
					{
						activeManager->huntNextShader(false, padStep);
					}
					if(pressedButtons & XINPUT_GAMEPAD_DPAD_UP)
					{
						activeManager->toggleMarkOnHuntedShader();
					}
				}
			}

			g_prevGamepadButtons = currentButtons;
		}
		else
		{
			g_prevGamepadButtons = 0;
		}
	}

	if(cacheNeedsRebuild)
	{
		rebuildActiveBlockedShaderHashesCache();
	}

	// Hunting keys (configurable, defaults to Numpad 1-9).
	// Next/Previous keys support holding down for continuous scrolling.
	// If Ctrl is pressed, Next/Previous steps to the next marked shader.
	// If Shift is pressed, steps by 10 shaders for fast navigation.
	const bool ctrlPressed = runtime->is_key_down(VK_CONTROL);
	const int stepCount = runtime->is_key_down(VK_SHIFT) ? 10 : 1;

	// Pixel shaders
	if(g_repeatTrackers[0].check(g_huntingKeys[0].isKeyDown(runtime), g_huntingKeys[0].isKeyPressed(runtime)))
	{
		g_pixelShaderManager.huntPreviousShader(ctrlPressed, stepCount);
	}
	if(g_repeatTrackers[1].check(g_huntingKeys[1].isKeyDown(runtime), g_huntingKeys[1].isKeyPressed(runtime)))
	{
		g_pixelShaderManager.huntNextShader(ctrlPressed, stepCount);
	}
	if(g_huntingKeys[2].isKeyPressed(runtime))
	{
		g_pixelShaderManager.toggleMarkOnHuntedShader();
	}

	// Vertex shaders
	if(g_repeatTrackers[2].check(g_huntingKeys[3].isKeyDown(runtime), g_huntingKeys[3].isKeyPressed(runtime)))
	{
		g_vertexShaderManager.huntPreviousShader(ctrlPressed, stepCount);
	}
	if(g_repeatTrackers[3].check(g_huntingKeys[4].isKeyDown(runtime), g_huntingKeys[4].isKeyPressed(runtime)))
	{
		g_vertexShaderManager.huntNextShader(ctrlPressed, stepCount);
	}
	if(g_huntingKeys[5].isKeyPressed(runtime))
	{
		g_vertexShaderManager.toggleMarkOnHuntedShader();
	}

	// Compute shaders
	if(g_repeatTrackers[4].check(g_huntingKeys[6].isKeyDown(runtime), g_huntingKeys[6].isKeyPressed(runtime)))
	{
		g_computeShaderManager.huntPreviousShader(ctrlPressed, stepCount);
	}
	if(g_repeatTrackers[5].check(g_huntingKeys[7].isKeyDown(runtime), g_huntingKeys[7].isKeyPressed(runtime)))
	{
		g_computeShaderManager.huntNextShader(ctrlPressed, stepCount);
	}
	if(g_huntingKeys[8].isKeyPressed(runtime))
	{
		g_computeShaderManager.toggleMarkOnHuntedShader();
	}
}


/// <summary>
/// Function which marks the end of a keybinding editing cycle
/// </summary>
/// <param name="acceptCollectedBinding"></param>
/// <param name="groupEditing"></param>
void endKeyBindingEditing(bool acceptCollectedBinding, ToggleGroup& groupEditing)
{
	if (acceptCollectedBinding && g_toggleGroupIdKeyBindingEditing == groupEditing.getId() && g_keyCollector.isValid())
	{
		groupEditing.setToggleKey(g_keyCollector);
	}
	g_toggleGroupIdKeyBindingEditing = -1;
	g_keyCollector.clear();
}


/// <summary>
/// Function which marks the start of a keybinding editing cycle for the passed in toggle group
/// </summary>
/// <param name="groupEditing"></param>
void startKeyBindingEditing(ToggleGroup& groupEditing)
{
	if (g_toggleGroupIdKeyBindingEditing == groupEditing.getId())
	{
		return;
	}
	if (g_toggleGroupIdKeyBindingEditing >= 0)
	{
		endKeyBindingEditing(false, groupEditing);
	}
	g_toggleGroupIdKeyBindingEditing = groupEditing.getId();
}


/// <summary>
/// Function which marks the end of a shader editing cycle for a given toggle group
/// </summary>
/// <param name="acceptCollectedShaderHashes"></param>
/// <param name="groupEditing"></param>
void endShaderEditing(bool acceptCollectedShaderHashes, ToggleGroup& groupEditing)
{
	if(acceptCollectedShaderHashes && g_toggleGroupIdShaderEditing == groupEditing.getId())
	{
		groupEditing.storeCollectedHashes(g_pixelShaderManager.getMarkedShaderHashes(), g_vertexShaderManager.getMarkedShaderHashes(), g_computeShaderManager.getMarkedShaderHashes());
		g_pixelShaderManager.stopHuntingMode();
		g_vertexShaderManager.stopHuntingMode();
		g_computeShaderManager.stopHuntingMode();
		rebuildActiveBlockedShaderHashesCache();
	}
	g_toggleGroupIdShaderEditing = -1;
}


/// <summary>
/// Function which marks the start of a shader editing cycle for a given toggle group.
/// </summary>
/// <param name="groupEditing"></param>
void startShaderEditing(ToggleGroup& groupEditing)
{
	if(g_toggleGroupIdShaderEditing==groupEditing.getId())
	{
		return;
	}
	if(g_toggleGroupIdShaderEditing >= 0)
	{
		endShaderEditing(false, groupEditing);
	}
	g_toggleGroupIdShaderEditing = groupEditing.getId();
	g_activeCollectorFrameCounter = g_startValueFramecountCollectionPhase;
	g_pixelShaderManager.startHuntingMode(groupEditing.getPixelShaderHashes());
	g_vertexShaderManager.startHuntingMode(groupEditing.getVertexShaderHashes());
	g_computeShaderManager.startHuntingMode(groupEditing.getComputeShaderHashes());

	// after copying them to the managers, we can now clear the group's shader.
	groupEditing.clearHashes();
	rebuildActiveBlockedShaderHashesCache();
}


static void showHelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(450.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}


static void displaySettings(reshade::api::effect_runtime* runtime)
{
	if(g_toggleGroupIdKeyBindingEditing >= 0 || g_isMasterKeyEditing || g_huntingKeyEditingIndex >= 0)
	{
		// a keybinding is being edited. Read current pressed keys into the collector, cumulatively;
		g_keyCollector.collectKeysPressed(runtime);
	}

	if(ImGui::CollapsingHeader("General info and help"))
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("The Shader Toggler allows you to create one or more groups with shaders to toggle on/off. You can assign a keyboard shortcut (including using keys like Shift, Alt and Control) to each group, including a handy name. Each group can have one or more vertex, pixel or compute shaders assigned to it. When you press the assigned keyboard shortcut, any draw calls using these shaders will be disabled, effectively hiding the elements in the 3D scene.");
		ImGui::TextUnformatted("\nYou can customize the hunting keys below to fit your keyboard (including laptops and Tenkeyless/TKL keyboards). Holding down the previous/next keys will scroll continuously through shaders.");
		ImGui::TextUnformatted("* Previous/Next shader keys: step through all collected shaders");
		ImGui::TextUnformatted("* Shift + Previous/Next shader keys: jump by 10 shaders for fast navigation");
		ImGui::TextUnformatted("* Ctrl + Previous/Next shader keys: step through ONLY the marked shaders in the group");
		ImGui::TextUnformatted("* Mark/Unmark shader keys: mark or unmark the current shader as part of the group");
		ImGui::TextUnformatted("* Controller / Gamepad: LB + RB + D-Pad Down toggles all groups; D-Pad Left/Right/Up hunts and marks shaders");
		ImGui::TextUnformatted("\nWhen you step through the shaders, the current shader is disabled in the 3D scene so you can see if that's the shader you were looking for.");
		ImGui::TextUnformatted("When you're done, make sure you click 'Save all toggle groups' to preserve your settings in the INI file.");
		ImGui::PopTextWrapPos();
	}

	ImGui::AlignTextToFramePadding();
	if(ImGui::CollapsingHeader("Shader selection parameters", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::AlignTextToFramePadding();
		ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.5f);
		ImGui::SliderFloat("Overlay opacity", &g_overlayOpacity, 0.2f, 1.0f);
		ImGui::AlignTextToFramePadding();
		ImGui::SliderInt("# of frames to collect", &g_startValueFramecountCollectionPhase, 10, 1000);
		ImGui::SameLine();
		showHelpMarker("This is the number of frames the addon will collect active shaders. Set this to a high number if the shader you want to mark is only used occasionally. Only shaders that are used in the frames collected can be marked.");
		ImGui::AlignTextToFramePadding();
		ImGui::Checkbox("Enable Controller / Gamepad Support (XInput)", &g_enableGamepad);
		ImGui::SameLine();
		showHelpMarker("Enables Xbox / XInput gamepad shortcuts:\n- LB + RB + D-Pad Down: Master Toggle (all groups)\n- In Hunting Mode: D-Pad Left/Right steps shaders (hold LB/RB to step by 10), D-Pad Up marks/unmarks");
		ImGui::PopItemWidth();
	}
	ImGui::Separator();

	if(ImGui::CollapsingHeader("Hunting keyboard shortcuts"))
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("Configure shortcuts used to step through and mark shaders while in 'Change shaders' hunting mode. Holding previous/next keys will scroll continuously through shaders.");
		ImGui::PopTextWrapPos();
		ImGui::Separator();

		for(int i = 0; i < 9; ++i)
		{
			ImGui::PushID(2000 + i);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%-30s", s_huntingKeyLabels[i]);
			ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
			ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.35f);

			std::string keyText = (g_huntingKeyEditingIndex == i) ? g_keyCollector.getKeyAsString() : g_huntingKeys[i].getKeyAsString();
			ImGui::InputText("##HuntingKey", (char*)keyText.c_str(), keyText.size(), ImGuiInputTextFlags_ReadOnly);
			if(ImGui::IsItemClicked())
			{
				g_toggleGroupIdKeyBindingEditing = -1;
				g_isMasterKeyEditing = false;
				g_huntingKeyEditingIndex = i;
				g_keyCollector.clear();
			}
			ImGui::PopItemWidth();

			if(g_huntingKeyEditingIndex == i)
			{
				ImGui::SameLine();
				if(ImGui::Button("OK"))
				{
					if(g_keyCollector.isValid())
					{
						g_huntingKeys[i] = g_keyCollector;
					}
					g_huntingKeyEditingIndex = -1;
					g_keyCollector.clear();
				}
				ImGui::SameLine();
				if(ImGui::Button("Cancel"))
				{
					g_huntingKeyEditingIndex = -1;
					g_keyCollector.clear();
				}
			}
			ImGui::PopID();
		}

		if(ImGui::Button("Reset Hunting Keys to Defaults (Numpad 1-9)"))
		{
			initHuntingKeysDefaults();
			g_huntingKeyEditingIndex = -1;
			g_keyCollector.clear();
		}
	}
	ImGui::Separator();

	if(ImGui::CollapsingHeader("List of Toggle Groups", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Master toggle key
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Master Toggle Key");
		ImGui::SameLine();
		showHelpMarker("Pressing this key toggles all groups off or restores their states. Leave empty if not needed.");
		ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
		ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.35f);

		std::string masterKeyText = g_isMasterKeyEditing ? g_keyCollector.getKeyAsString() : g_masterToggleKey.getKeyAsString();
		ImGui::InputText("##MasterToggleKey", (char*)masterKeyText.c_str(), masterKeyText.size(), ImGuiInputTextFlags_ReadOnly);
		if(ImGui::IsItemClicked())
		{
			g_toggleGroupIdKeyBindingEditing = -1;
			g_huntingKeyEditingIndex = -1;
			g_isMasterKeyEditing = true;
			g_keyCollector.clear();
		}
		ImGui::PopItemWidth();

		if(g_isMasterKeyEditing)
		{
			ImGui::SameLine();
			if(ImGui::Button("OK##Master"))
			{
				if(g_keyCollector.isValid())
				{
					g_masterToggleKey = g_keyCollector;
				}
				g_isMasterKeyEditing = false;
				g_keyCollector.clear();
			}
			ImGui::SameLine();
			if(ImGui::Button("Cancel##Master"))
			{
				g_isMasterKeyEditing = false;
				g_keyCollector.clear();
			}
			ImGui::SameLine();
			if(ImGui::Button("Clear##Master"))
			{
				g_masterToggleKey.clear();
				g_isMasterKeyEditing = false;
				g_keyCollector.clear();
			}
		}
		else if(g_masterToggleKey.isValid())
		{
			ImGui::SameLine();
			if(ImGui::Button("Clear##Master"))
			{
				g_masterToggleKey.clear();
			}
		}

		ImGui::SameLine();
		if(ImGui::Button("Toggle All Groups Now"))
		{
			toggleAllGroups();
		}

		ImGui::Separator();

		if(ImGui::Button(" New "))
		{
			addDefaultGroup();
		}
		ImGui::Separator();

		std::vector<ToggleGroup> toRemove;
		for(auto& group : g_toggleGroups)
		{
			ImGui::PushID(group.getId());
			ImGui::AlignTextToFramePadding();
			if(ImGui::Button("X"))
			{
				toRemove.push_back(group);
			}
			ImGui::SameLine();
			ImGui::Text(" %d ", group.getId());
			ImGui::SameLine();
			if(ImGui::Button("Edit"))
			{
				group.setEditing(true);
			}

			ImGui::SameLine();
			if(g_toggleGroupIdShaderEditing >= 0)
			{
				if(g_toggleGroupIdShaderEditing == group.getId())
				{
					if(ImGui::Button(" Done "))
					{
						endShaderEditing(true, group);
					}
				}
				else
				{
					ImGui::BeginDisabled(true);
					ImGui::Button("      ");
					ImGui::EndDisabled();
				}
			}
			else
			{
				if(ImGui::Button("Change shaders"))
				{
					ImGui::SameLine();
					startShaderEditing(group);
				}
			}

			ImGui::SameLine();
			bool groupIsActive = group.isActive();
			if(ImGui::Checkbox("##GroupActiveToggle", &groupIsActive))
			{
				group.toggleActive();
				rebuildActiveBlockedShaderHashesCache();
			}
			ImGui::SameLine();
			const char* modeSuffix = (group.getToggleMode() == ToggleMode::Hold) ? ", hold" : ((group.getToggleMode() == ToggleMode::AutoHide) ? ", auto-hide" : "");
			ImGui::Text("%s (%s%s%s)", group.getName().c_str(), group.getToggleKeyAsString().c_str(), group.isInverted() ? ", inverted" : "", modeSuffix);
			if(group.isActiveAtStartup())
			{
				ImGui::SameLine();
				ImGui::Text("(Active at startup)");
			}
			if(group.isEditing())
			{
				ImGui::Separator();
				ImGui::Text("Edit group %d", group.getId());

				// Name of group
				char tmpBuffer[150];
				const string& name = group.getName();
				strncpy_s(tmpBuffer, 150, name.c_str(), name.size());
				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.7f);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Name");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				ImGui::InputText("##Name", tmpBuffer, 149);
				group.setName(tmpBuffer);
				ImGui::PopItemWidth();

				// Key binding of group
				bool isKeyEditing = false;
				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.5f);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Key shortcut");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				string textBoxContents = (g_toggleGroupIdKeyBindingEditing == group.getId()) ? g_keyCollector.getKeyAsString() : group.getToggleKeyAsString();	// The 'press a key' is inside keycollector
				string toggleKeyName = group.getToggleKeyAsString();
				ImGui::InputText("##Key shortcut", (char*)textBoxContents.c_str(), textBoxContents.size(), ImGuiInputTextFlags_ReadOnly);
				if(ImGui::IsItemClicked())
				{
					startKeyBindingEditing(group);
				}
				if(g_toggleGroupIdKeyBindingEditing == group.getId())
				{
					isKeyEditing = true;
					ImGui::SameLine();
					if(ImGui::Button("OK"))
					{
						endKeyBindingEditing(true, group);
					}
					ImGui::SameLine();
					if(ImGui::Button("Cancel"))
					{
						endKeyBindingEditing(false, group);
					}
				}
				ImGui::PopItemWidth();

				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.7f);
				ImGui::Text(" ");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				bool isDefaultActive = group.isActiveAtStartup();
				ImGui::Checkbox("Is active at startup", &isDefaultActive);
				group.setIsActiveAtStartup(isDefaultActive);
				ImGui::PopItemWidth();

				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.7f);
				ImGui::Text(" ");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				bool isInverted = group.isInverted();
				ImGui::Checkbox("Invert selection (hide all OTHER shaders)", &isInverted);
				group.setIsInverted(isInverted);
				ImGui::PopItemWidth();

				// Toggle mode (Toggle, Hold, AutoHide)
				const char* modeNames[] = { "Standard Toggle (Press key to toggle on/off)", "Momentary Hold (Active only while holding key)", "Auto-Hide Timer (Turns off after delay)" };
				int currentMode = static_cast<int>(group.getToggleMode());
				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.7f);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Toggle mode");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				if (ImGui::Combo("##ToggleMode", &currentMode, modeNames, 3))
				{
					group.setToggleMode(static_cast<ToggleMode>(currentMode));
				}
				ImGui::PopItemWidth();

				if (group.getToggleMode() == ToggleMode::AutoHide)
				{
					float delay = group.getAutoHideDelay();
					ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.4f);
					ImGui::AlignTextToFramePadding();
					ImGui::Text("Auto-hide delay");
					ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
					if (ImGui::SliderFloat("##AutoHideDelay", &delay, 0.5f, 30.0f, "%.1f s"))
					{
						group.setAutoHideDelay(delay);
					}
					ImGui::PopItemWidth();
				}

				// Shader Hashes Clipboard and Manual Addition
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Shader hashes");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				if (ImGui::Button("Copy Hashes to Clipboard"))
				{
					std::string hashExport = "# Toggle Group: " + group.getName() + "\n";
					const auto& px = group.getPixelShaderHashes();
					if(!px.empty())
					{
						hashExport += "[PixelShaders]\n";
						for(uint32_t h : px)
						{
							char buf[32];
							sprintf_s(buf, "0x%08X\n", h);
							hashExport += buf;
						}
					}
					const auto& vx = group.getVertexShaderHashes();
					if(!vx.empty())
					{
						hashExport += "[VertexShaders]\n";
						for(uint32_t h : vx)
						{
							char buf[32];
							sprintf_s(buf, "0x%08X\n", h);
							hashExport += buf;
						}
					}
					const auto& cs = group.getComputeShaderHashes();
					if(!cs.empty())
					{
						hashExport += "[ComputeShaders]\n";
						for(uint32_t h : cs)
						{
							char buf[32];
							sprintf_s(buf, "0x%08X\n", h);
							hashExport += buf;
						}
					}
					copyTextToClipboard(hashExport);
				}
				ImGui::SameLine();
				showHelpMarker("Copies all shader hashes in this group to the Windows clipboard for sharing or manual backup.");

				// Add manual hash
				static char s_manualHashInput[128] = "";
				static int s_manualShaderType = 0;
				const char* shaderTypeOptions[] = { "Pixel", "Vertex", "Compute" };

				ImGui::AlignTextToFramePadding();
				ImGui::Text("Add hash manually");
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.18f);
				ImGui::Combo("##ManualShaderType", &s_manualShaderType, shaderTypeOptions, 3);
				ImGui::PopItemWidth();

				ImGui::SameLine();
				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.25f);
				ImGui::InputTextWithHint("##ManualHashInput", "e.g. 0xA1B2C3D4", s_manualHashInput, sizeof(s_manualHashInput));
				ImGui::PopItemWidth();

				ImGui::SameLine();
				if(ImGui::Button("Add"))
				{
					std::string inputStr(s_manualHashInput);
					std::stringstream ss(inputStr);
					std::string token;
					bool added = false;
					while(std::getline(ss, token, ','))
					{
						std::stringstream tokenStream(token);
						std::string singleHash;
						while(tokenStream >> singleHash)
						{
							try
							{
								uint32_t hash = static_cast<uint32_t>(std::stoul(singleHash, nullptr, 16));
								if(hash != 0)
								{
									group.addShaderHash(hash, s_manualShaderType);
									added = true;
								}
							}
							catch(...) {}
						}
					}
					if(added)
					{
						rebuildActiveBlockedShaderHashesCache();
						s_manualHashInput[0] = '\0';
					}
				}

				if(!isKeyEditing)
				{
					if(ImGui::Button("OK"))
					{
						group.setEditing(false);
						g_toggleGroupIdKeyBindingEditing = -1;
						g_keyCollector.clear();
					}
				}
				ImGui::Separator();
			}

			ImGui::PopID();
		}
		if(toRemove.size() > 0)
		{
			// switch off keybinding editing or shader editing, if in progress
			g_toggleGroupIdKeyBindingEditing = -1;
			g_isMasterKeyEditing = false;
			g_huntingKeyEditingIndex = -1;
			g_keyCollector.clear();
			g_toggleGroupIdShaderEditing = -1;
			g_pixelShaderManager.stopHuntingMode();
			g_vertexShaderManager.stopHuntingMode();
		}
		for(const auto& group : toRemove)
		{
			std::erase(g_toggleGroups, group);
		}
		if(!toRemove.empty())
		{
			rebuildActiveBlockedShaderHashesCache();
		}

		ImGui::Separator();
		std::string iniNameOnly = std::filesystem::path(g_iniFileName).filename().string();
		ImGui::Text("Active INI Profile: %s", iniNameOnly.c_str());
		if(g_iniFileName == g_defaultIniFileName && !g_gameSpecificIniFileName.empty())
		{
			ImGui::SameLine();
			if(ImGui::Button("Switch to Game-Specific INI"))
			{
				g_iniFileName = g_gameSpecificIniFileName;
				loadShaderTogglerIniFile();
			}
		}
		else if(g_iniFileName == g_gameSpecificIniFileName)
		{
			ImGui::SameLine();
			if(ImGui::Button("Switch to Default INI"))
			{
				g_iniFileName = g_defaultIniFileName;
				loadShaderTogglerIniFile();
			}
		}

		if(g_toggleGroups.size() > 0)
		{
			if(ImGui::Button("Save all Toggle Groups"))
			{
				saveShaderTogglerIniFile();
			}
		}
	}
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			if(!reshade::register_addon(hModule))
			{
				return FALSE;
			}

			// We'll pass a nullptr for the module handle so we get the containing process' executable + path. We can't use the reshade's api as we don't have the runtime
			// and we can't use reshade's handle because under vulkan reshade is stored in a central space and therefore it won't get the folder of the exe (where the reshade dll is located as well).
			WCHAR buf[MAX_PATH];
			const std::filesystem::path dllPath = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf)) ? buf : std::filesystem::path();
			const std::filesystem::path basePath = dllPath.parent_path();
			const std::string exeStem = dllPath.stem().string();

			g_defaultIniFileName = (basePath / HASH_FILE_NAME).string();
			g_gameSpecificIniFileName = (basePath / ("ShaderToggler." + exeStem + ".ini")).string();

			if(std::filesystem::exists(g_gameSpecificIniFileName))
			{
				g_iniFileName = g_gameSpecificIniFileName;
			}
			else
			{
				g_iniFileName = g_defaultIniFileName;
			}

			reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
			reshade::register_event<reshade::addon_event::init_command_list>(onInitCommandList);
			reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
			reshade::register_event<reshade::addon_event::reset_command_list>(onResetCommandList);
			reshade::register_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
			reshade::register_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
			reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
			reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
			reshade::register_event<reshade::addon_event::draw>(onDraw);
			reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
			reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(onDrawOrDispatchIndirect);
			reshade::register_event<reshade::addon_event::dispatch>(onDispatch);
			reshade::register_overlay(nullptr, &displaySettings);
			initXInput();
			loadShaderTogglerIniFile();
		}
		break;
	case DLL_PROCESS_DETACH:
		if(g_hXInputDll != nullptr)
		{
			FreeLibrary(g_hXInputDll);
			g_hXInputDll = nullptr;
		}
		reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresent);
		reshade::unregister_event<reshade::addon_event::destroy_pipeline>(onDestroyPipeline);
		reshade::unregister_event<reshade::addon_event::init_pipeline>(onInitPipeline);
		reshade::unregister_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
		reshade::unregister_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
		reshade::unregister_event<reshade::addon_event::draw>(onDraw);
		reshade::unregister_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
		reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(onDrawOrDispatchIndirect);
		reshade::unregister_event<reshade::addon_event::dispatch>(onDispatch);
		reshade::unregister_event<reshade::addon_event::init_command_list>(onInitCommandList);
		reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
		reshade::unregister_event<reshade::addon_event::reset_command_list>(onResetCommandList);
		reshade::unregister_overlay(nullptr, &displaySettings);
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
