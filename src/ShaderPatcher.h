#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <memory>

namespace ShaderToggler
{
	enum class PatchStrategy
	{
		AutoDetect = 0,
		CelOutline_MadBlend,        // Neutralizes mad inverse cel-blend instruction
		MRT_ZeroTargetO1,           // Neutralizes writes to o1 output register (MRT outline/mask)
		InvertedHull_VertexNormal,  // Neutralizes vertex normal extrusion in vertex shaders
		Blur_PassThrough,           // Fullscreen pass-through routing t0 to o0 (kills blur/DoF)
		Vignette_PassThrough        // Fullscreen pass-through routing t0 to o0 (kills vignette)
	};

	class ShaderPatcher
	{
	public:
		static ShaderPatcher& getInstance()
		{
			static ShaderPatcher instance;
			return instance;
		}

		/// <summary>
		/// Analyzes and patches DXBC bytecode directly in memory.
		/// Recomputes the Microsoft container checksum using CalculateDXBCChecksum.
		/// </summary>
		bool patchDXBCBytecode(const std::vector<uint8_t>& originalBytecode, PatchStrategy strategy,
		                       std::vector<uint8_t>& outPatchedBytecode, std::string& outDetails);

		/// <summary>
		/// High-level 1-click auto patcher. Analyzes the shader with ShaderAnalyzer,
		/// selects the optimal strategy (outline mad NOP, MRT o1 NOP, or blur pass-through),
		/// saves the patch to <GameDir>/ShaderFixes/<hash>.cso, and activates it immediately.
		/// </summary>
		bool autoPatchShader(uint32_t shaderHash, int shaderType, PatchStrategy strategy,
		                     const std::filesystem::path& basePath, std::string& outMsg);

		/// <summary>
		/// Reverts an active patch for the specified shader hash, deleting the fix file
		/// and unregistering it from the override manager.
		/// </summary>
		bool revertPatch(uint32_t shaderHash, const std::filesystem::path& basePath, std::string& outMsg);

		/// <summary>
		/// Checks whether a patch file or active override exists for this shader hash.
		/// </summary>
		bool isPatchActive(uint32_t shaderHash, const std::filesystem::path& basePath);

	private:
		ShaderPatcher() = default;
		~ShaderPatcher() = default;

		bool patchCelOutlineInstruction(uint8_t* pBytecode, size_t size, std::string& outDetails);
		bool patchMRTO1Target(uint8_t* pBytecode, size_t size, std::string& outDetails);
		bool patchInvertedHullExtrusion(uint8_t* pBytecode, size_t size, std::string& outDetails);
		bool generatePassThroughBytecode(const std::vector<uint8_t>& origBytecode,
		                                 std::vector<uint8_t>& outPatchedBytecode, std::string& outDetails);
	};
}
