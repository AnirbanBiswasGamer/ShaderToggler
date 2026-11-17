#include "ShaderOverrideManager.h"
#include "ShaderAnalyzer.h"
#include <unknwn.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <regex>

namespace ShaderToggler
{
	struct ID3DBlob : public IUnknown
	{
		virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) = 0;
		virtual SIZE_T STDMETHODCALLTYPE GetBufferSize(void) = 0;
	};

	typedef HRESULT (WINAPI *PFN_D3DCompile)(
		LPCVOID pSrcData,
		SIZE_T SrcDataSize,
		LPCSTR pSourceName,
		const void* pDefines,
		void* pInclude,
		LPCSTR pEntrypoint,
		LPCSTR pTarget,
		UINT Flags1,
		UINT Flags2,
		ID3DBlob** ppCode,
		ID3DBlob** ppErrorMsgs
	);

	uint64_t compute_fnv1_64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 0xcbf29ce484222325ULL;
		for (size_t i = 0; i < size; ++i)
		{
			hash = (hash * 0x100000001b3ULL) ^ data[i];
		}
		return hash;
	}

	uint64_t compute_fnv1a_64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 0xcbf29ce484222325ULL;
		for (size_t i = 0; i < size; ++i)
		{
			hash = (hash ^ data[i]) * 0x100000001b3ULL;
		}
		return hash;
	}

	ShaderOverrideManager::ShaderOverrideManager()
	{
	}

	ShaderOverrideManager::~ShaderOverrideManager()
	{
		if(_hD3DCompiler != nullptr)
		{
			FreeLibrary(_hD3DCompiler);
			_hD3DCompiler = nullptr;
		}
	}

	void ShaderOverrideManager::initCompiler()
	{
		if(_pfnD3DCompile != nullptr)
		{
			return;
		}

		_hD3DCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
		if(_hD3DCompiler == nullptr)
		{
			_hD3DCompiler = LoadLibraryW(L"d3dcompiler_46.dll");
		}
		if(_hD3DCompiler == nullptr)
		{
			_hD3DCompiler = LoadLibraryW(L"d3dcompiler_43.dll");
		}

		if(_hD3DCompiler != nullptr)
		{
			_pfnD3DCompile = GetProcAddress(_hD3DCompiler, "D3DCompile");
		}
	}

	void ShaderOverrideManager::init(const std::filesystem::path& basePath)
	{
		_basePath = basePath;
		_fixesPath = basePath / "ShaderFixes";

		std::error_code ec;
		if(!std::filesystem::exists(_fixesPath, ec))
		{
			std::filesystem::create_directories(_fixesPath, ec);
		}

		initCompiler();
		scanDesktopForFixes();
		reloadOverrides();
		_initialized = true;
	}

	void ShaderOverrideManager::scanDesktopForFixes()
	{
		WCHAR userProfile[MAX_PATH];
		if(GetEnvironmentVariableW(L"USERPROFILE", userProfile, ARRAYSIZE(userProfile)) > 0)
		{
			std::filesystem::path desktopPath = std::filesystem::path(userProfile) / "Desktop";
			std::error_code ec;
			if(std::filesystem::exists(desktopPath, ec))
			{
				for(const auto& entry : std::filesystem::directory_iterator(desktopPath, ec))
				{
					if(entry.is_regular_file())
					{
						std::string filename = entry.path().filename().string();
						if(filename.find("-ps_replace.txt") != std::string::npos ||
						   filename.find("-vs_replace.txt") != std::string::npos ||
						   filename.find("_replace.txt") != std::string::npos)
						{
							std::filesystem::path dest = _fixesPath / filename;
							if(!std::filesystem::exists(dest, ec))
							{
								std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing, ec);
							}
						}
					}
				}
			}
		}
	}

	void ShaderOverrideManager::reloadOverrides()
	{
		std::unique_lock lock(_overrideMutex);
		_overridesByCrc32.clear();
		_overridesByFnv64.clear();
		_overrideSources.clear();

		std::error_code ec;
		if(!std::filesystem::exists(_fixesPath, ec))
		{
			return;
		}

		for(const auto& entry : std::filesystem::directory_iterator(_fixesPath, ec))
		{
			if(entry.is_regular_file())
			{
				loadFileOverride(entry.path());
			}
		}
	}

	bool ShaderOverrideManager::compileHLSL(const std::string& hlslCode, const std::string& sourceName,
	                                       const std::string& targetProfile, const std::string& entryPoint,
	                                       std::vector<uint8_t>& outBytecode, std::string& outErrors)
	{
		initCompiler();
		if(_pfnD3DCompile == nullptr)
		{
			outErrors = "d3dcompiler_47.dll not loaded or D3DCompile not found";
			return false;
		}

		PFN_D3DCompile pfn = reinterpret_cast<PFN_D3DCompile>(_pfnD3DCompile);

		ID3DBlob* pCode = nullptr;
		ID3DBlob* pErrorBlob = nullptr;

		std::vector<std::string> profilesToTry;
		if(!targetProfile.empty())
		{
			profilesToTry.push_back(targetProfile);
		}
		else
		{
			if(sourceName.find("-vs") != std::string::npos || sourceName.find("vs_") != std::string::npos)
			{
				profilesToTry.push_back("vs_5_0");
				profilesToTry.push_back("vs_4_0");
			}
			else if(sourceName.find("-cs") != std::string::npos || sourceName.find("cs_") != std::string::npos)
			{
				profilesToTry.push_back("cs_5_0");
				profilesToTry.push_back("cs_4_0");
			}
			else
			{
				profilesToTry.push_back("ps_5_0");
				profilesToTry.push_back("ps_4_0");
			}
		}

		HRESULT hr = E_FAIL;
		for(const auto& profile : profilesToTry)
		{
			hr = pfn(
				hlslCode.data(),
				hlslCode.size(),
				sourceName.c_str(),
				nullptr,
				nullptr,
				entryPoint.c_str(),
				profile.c_str(),
				1 << 15, // D3DCOMPILE_OPTIMIZATION_LEVEL3
				0,
				&pCode,
				&pErrorBlob
			);

			if(SUCCEEDED(hr) && pCode != nullptr)
			{
				break;
			}

			if(pErrorBlob != nullptr)
			{
				pErrorBlob->Release();
				pErrorBlob = nullptr;
			}
		}

		if(SUCCEEDED(hr) && pCode != nullptr)
		{
			const uint8_t* pBytes = static_cast<const uint8_t*>(pCode->GetBufferPointer());
			outBytecode.assign(pBytes, pBytes + pCode->GetBufferSize());
			pCode->Release();
			return true;
		}

		if(pErrorBlob != nullptr)
		{
			outErrors.assign(static_cast<const char*>(pErrorBlob->GetBufferPointer()), pErrorBlob->GetBufferSize());
			pErrorBlob->Release();
		}
		else
		{
			outErrors = "D3DCompile failed with HRESULT 0x" + std::to_string(hr);
		}

		return false;
	}

	void ShaderOverrideManager::loadFileOverride(const std::filesystem::path& filePath)
	{
		std::string ext = filePath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		std::string stem = filePath.stem().string();
		std::string filename = filePath.filename().string();

		// Parse hash from filename (e.g. ab6655f0beae8f43-ps_replace.txt, 0xB1A1710A.cso, B1A1710A.hlsl)
		uint64_t fnvHash = 0;
		uint32_t crcHash = 0;

		std::regex hex64Regex(R"(([0-9a-fA-F]{16}))");
		std::regex hex32Regex(R"((?:0x)?([0-9a-fA-F]{8}))");
		std::smatch match;

		if(std::regex_search(filename, match, hex64Regex))
		{
			try { fnvHash = std::stoull(match[1].str(), nullptr, 16); } catch(...) {}
		}
		else if(std::regex_search(filename, match, hex32Regex))
		{
			try { crcHash = std::stoul(match[1].str(), nullptr, 16); } catch(...) {}
		}

		if(fnvHash == 0 && crcHash == 0)
		{
			return;
		}

		std::vector<uint8_t> bytecode;

		if(ext == ".cso" || ext == ".bin")
		{
			std::ifstream in(filePath, std::ios::binary);
			if(in.is_open())
			{
				in.seekg(0, std::ios::end);
				size_t sz = static_cast<size_t>(in.tellg());
				in.seekg(0, std::ios::beg);
				bytecode.resize(sz);
				in.read(reinterpret_cast<char*>(bytecode.data()), sz);
			}
		}
		else if(ext == ".hlsl" || ext == ".txt")
		{
			std::ifstream in(filePath);
			if(in.is_open())
			{
				std::stringstream ss;
				ss << in.rdbuf();
				std::string hlsl = ss.str();

				// Auto-patch outline blend if detected:
				// Pattern 1: DBXV2 style inverse multiply blend: r0.xyz = r2.xyz * r1.xyz + r0.xyz;
				size_t blendPos = hlsl.find("r0.xyz = r2.xyz * r1.xyz + r0.xyz;");
				if(blendPos != std::string::npos && hlsl.find("r1.xyz = float3(0, 0, 0);") == std::string::npos)
				{
					hlsl.insert(blendPos, "r1.xyz = float3(0, 0, 0); // Auto-neutralized outline\n  ");
				}

				std::string errors;
				std::string profile = "";
				if(filename.find("-ps") != std::string::npos) profile = "ps_4_0";
				else if(filename.find("-vs") != std::string::npos) profile = "vs_4_0";

				if(compileHLSL(hlsl, filename, profile, "main", bytecode, errors))
				{
					if(crcHash != 0) _overrideSources[crcHash] = hlsl;
				}
			}
		}

		if(!bytecode.empty())
		{
			if(fnvHash != 0)
			{
				_overridesByFnv64[fnvHash] = bytecode;
			}
			if(crcHash != 0)
			{
				_overridesByCrc32[crcHash] = bytecode;
			}
		}
	}

	const std::vector<uint8_t>* ShaderOverrideManager::findOverride(uint32_t crc32, uint64_t fnv1, uint64_t fnv1a)
	{
		std::shared_lock lock(_overrideMutex);

		if(crc32 != 0)
		{
			auto it = _overridesByCrc32.find(crc32);
			if(it != _overridesByCrc32.end())
			{
				return &it->second;
			}
		}

		if(fnv1 != 0)
		{
			auto it = _overridesByFnv64.find(fnv1);
			if(it != _overridesByFnv64.end())
			{
				return &it->second;
			}
		}

		if(fnv1a != 0)
		{
			auto it = _overridesByFnv64.find(fnv1a);
			if(it != _overridesByFnv64.end())
			{
				return &it->second;
			}
		}

		return nullptr;
	}

	bool ShaderOverrideManager::hasOverride(uint32_t crc32, uint64_t fnv1, uint64_t fnv1a)
	{
		return findOverride(crc32, fnv1, fnv1a) != nullptr;
	}

	size_t ShaderOverrideManager::getOverrideCount()
	{
		std::shared_lock lock(_overrideMutex);
		return _overridesByCrc32.size() + _overridesByFnv64.size();
	}

	bool ShaderOverrideManager::autoPatchOutline(uint32_t crc32Hash, int shaderType, std::string& outMsg)
	{
		if(crc32Hash == 0)
		{
			outMsg = "Invalid shader hash";
			return false;
		}

		// Check if we have source in _overrideSources
		std::string hlsl;
		{
			std::shared_lock lock(_overrideMutex);
			auto it = _overrideSources.find(crc32Hash);
			if(it != _overrideSources.end())
			{
				hlsl = it->second;
			}
		}

		// If no cached source, check ShaderFixes or Desktop for matching files
		if(hlsl.empty())
		{
			std::error_code ec;
			for(const auto& entry : std::filesystem::directory_iterator(_fixesPath, ec))
			{
				if(entry.is_regular_file())
				{
					std::string ext = entry.path().extension().string();
					if(ext == ".hlsl" || ext == ".txt")
					{
						std::ifstream in(entry.path());
						if(in.is_open())
						{
							std::stringstream ss;
							ss << in.rdbuf();
							hlsl = ss.str();
							break;
						}
					}
				}
			}
		}

		if(hlsl.empty())
		{
			outMsg = "No HLSL source available to patch for this shader";
			return false;
		}

		// Neutralize outline blend in HLSL
		size_t blendPos = hlsl.find("r0.xyz = r2.xyz * r1.xyz + r0.xyz;");
		if(blendPos != std::string::npos)
		{
			if(hlsl.find("r1.xyz = float3(0, 0, 0);") == std::string::npos)
			{
				hlsl.insert(blendPos, "r1.xyz = float3(0, 0, 0); // Auto-neutralized outline\n  ");
			}
		}
		else
		{
			// Check for MRT o1 outline target
			size_t o1Pos = hlsl.find("o1.xyzw = ");
			if(o1Pos != std::string::npos)
			{
				size_t semi = hlsl.find(";", o1Pos);
				if(semi != std::string::npos)
				{
					hlsl.replace(o1Pos, semi - o1Pos + 1, "o1.xyzw = float4(0, 0, 0, 0);");
				}
			}
		}

		std::vector<uint8_t> compiledBytecode;
		std::string errors;
		std::string profile = (shaderType == 1) ? "vs_4_0" : "ps_4_0";
		char hexBuf[32];
		sprintf_s(hexBuf, "0x%08X", crc32Hash);

		if(!compileHLSL(hlsl, hexBuf, profile, "main", compiledBytecode, errors))
		{
			outMsg = "Compilation failed: " + errors;
			return false;
		}

		// Save patched files
		char csoName[64], hlslName[64];
		sprintf_s(csoName, "0x%08X.cso", crc32Hash);
		sprintf_s(hlslName, "0x%08X.hlsl", crc32Hash);

		std::ofstream outCso(_fixesPath / csoName, std::ios::binary);
		if(outCso.is_open())
		{
			outCso.write(reinterpret_cast<const char*>(compiledBytecode.data()), compiledBytecode.size());
		}

		std::ofstream outHlsl(_fixesPath / hlslName);
		if(outHlsl.is_open())
		{
			outHlsl << hlsl;
		}

		{
			std::unique_lock lock(_overrideMutex);
			_overridesByCrc32[crc32Hash] = compiledBytecode;
			_overrideSources[crc32Hash] = hlsl;
		}

		outMsg = "Outline patched & saved to ShaderFixes/" + std::string(csoName);
		return true;
	}
}
