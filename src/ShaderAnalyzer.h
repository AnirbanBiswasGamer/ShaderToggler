#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <filesystem>

namespace ShaderToggler
{
	enum ShaderCategory : uint32_t
	{
		Category_All = 0,
		Category_OutlineEdge = 1 << 0,
		Category_Vignette = 1 << 1,
		Category_FogAtmospheric = 1 << 2,
		Category_BloomBlur = 1 << 3,
		Category_UI_HUD = 1 << 4
	};

	struct ShaderAnalysisResult
	{
		uint32_t hash = 0;
		uint32_t categoryFlags = Category_All;
		std::string categorySummary;
		bool isAnalyzed = false;
	};

	class ShaderAnalyzer
	{
	public:
		static ShaderAnalyzer& getInstance()
		{
			static ShaderAnalyzer instance;
			return instance;
		}

		void storeBytecode(uint32_t hash, const void* code, size_t codeSize);
		bool hasBytecode(uint32_t hash);
		const std::vector<uint8_t>* getBytecode(uint32_t hash);

		ShaderAnalysisResult analyzeShader(uint32_t hash, int shaderType);
		uint32_t getCategoryFlags(uint32_t hash, int shaderType);
		std::string getCategorySummary(uint32_t hash, int shaderType);

		std::string disassembleShader(uint32_t hash);
		bool dumpShader(uint32_t hash, const std::filesystem::path& basePath, std::string& outCsoName, std::string& outAsmName);

	private:
		ShaderAnalyzer();
		~ShaderAnalyzer();

		void initD3DCompiler();

		std::unordered_map<uint32_t, std::vector<uint8_t>> _bytecodeCache;
		std::unordered_map<uint32_t, ShaderAnalysisResult> _analysisCache;
		std::unordered_map<uint32_t, std::string> _disasmCache;
		std::shared_mutex _cacheMutex;

		HMODULE _hD3DCompiler = nullptr;
		void* _pfnD3DDisassemble = nullptr;
		bool _compilerInitialized = false;
	};
}
