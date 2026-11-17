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
	class ShaderOverrideManager
	{
	public:
		static ShaderOverrideManager& getInstance()
		{
			static ShaderOverrideManager instance;
			return instance;
		}

		void init(const std::filesystem::path& basePath);
		void reloadOverrides();

		bool isEnabled() const { return _enabled; }
		void setEnabled(bool enabled) { _enabled = enabled; }

		// Lookup by 32-bit CRC or 64-bit FNV-1 / FNV-1a
		const std::vector<uint8_t>* findOverride(uint32_t crc32, uint64_t fnv1, uint64_t fnv1a);
		bool hasOverride(uint32_t crc32, uint64_t fnv1, uint64_t fnv1a);
		void addOverride(uint32_t crc32, const std::vector<uint8_t>& bytecode);
		void removeOverride(uint32_t crc32);

		// Dynamic compilation
		bool compileHLSL(const std::string& hlslCode, const std::string& sourceName, const std::string& targetProfile,
		                 const std::string& entryPoint, std::vector<uint8_t>& outBytecode, std::string& outErrors);

		// 1-Click Auto-patch outline
		bool autoPatchOutline(uint32_t crc32Hash, int shaderType, std::string& outMsg);

		size_t getOverrideCount();
		const std::filesystem::path& getBasePath() const { return _basePath; }
		const std::filesystem::path& getFixesPath() const { return _fixesPath; }

	private:
		ShaderOverrideManager();
		~ShaderOverrideManager();

		void initCompiler();
		void loadFileOverride(const std::filesystem::path& filePath);

		std::filesystem::path _basePath;
		std::filesystem::path _fixesPath;
		bool _initialized = false;
		bool _enabled = true;

		std::unordered_map<uint32_t, std::vector<uint8_t>> _overridesByCrc32;
		std::unordered_map<uint64_t, std::vector<uint8_t>> _overridesByFnv64;
		std::unordered_map<uint32_t, std::string> _overrideSources;
		std::shared_mutex _overrideMutex;

		HMODULE _hD3DCompiler = nullptr;
		void* _pfnD3DCompile = nullptr;
	};

	uint64_t compute_fnv1_64(const uint8_t* data, size_t size);
	uint64_t compute_fnv1a_64(const uint8_t* data, size_t size);
}
