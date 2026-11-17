#include "ShaderAnalyzer.h"
#include <windows.h>
#include <d3dcommon.h>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <map>

namespace ShaderToggler
{
	typedef HRESULT (WINAPI *PFN_D3DDisassemble)(
		LPCVOID pSrcData,
		SIZE_T SrcDataSize,
		UINT Flags,
		LPCSTR szComments,
		ID3DBlob** ppDisassembly
	);

	ShaderAnalyzer::ShaderAnalyzer()
	{
	}

	ShaderAnalyzer::~ShaderAnalyzer()
	{
		if(_hD3DCompiler != nullptr)
		{
			FreeLibrary(_hD3DCompiler);
			_hD3DCompiler = nullptr;
		}
	}

	void ShaderAnalyzer::initD3DCompiler()
	{
		if(_compilerInitialized)
		{
			return;
		}
		_compilerInitialized = true;
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
			_pfnD3DDisassemble = reinterpret_cast<void*>(GetProcAddress(_hD3DCompiler, "D3DDisassemble"));
		}
	}

	void ShaderAnalyzer::storeBytecode(uint32_t hash, const void* code, size_t codeSize)
	{
		if(hash == 0 || code == nullptr || codeSize == 0)
		{
			return;
		}

		std::unique_lock lock(_cacheMutex);
		if(_bytecodeCache.find(hash) == _bytecodeCache.end())
		{
			auto& vec = _bytecodeCache[hash];
			vec.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + codeSize);
		}
	}

	bool ShaderAnalyzer::hasBytecode(uint32_t hash)
	{
		if(hash == 0)
		{
			return false;
		}
		std::shared_lock lock(_cacheMutex);
		return _bytecodeCache.find(hash) != _bytecodeCache.end();
	}

	const std::vector<uint8_t>* ShaderAnalyzer::getBytecode(uint32_t hash)
	{
		if(hash == 0)
		{
			return nullptr;
		}
		std::shared_lock lock(_cacheMutex);
		const auto it = _bytecodeCache.find(hash);
		if(it != _bytecodeCache.end())
		{
			return &it->second;
		}
		return nullptr;
	}

	std::string ShaderAnalyzer::disassembleShader(uint32_t hash)
	{
		if(hash == 0)
		{
			return "";
		}

		{
			std::shared_lock lock(_cacheMutex);
			const auto it = _disasmCache.find(hash);
			if(it != _disasmCache.end())
			{
				return it->second;
			}
		}

		initD3DCompiler();
		if(_pfnD3DDisassemble == nullptr)
		{
			return "";
		}

		std::vector<uint8_t> bytecodeCopy;
		{
			std::shared_lock lock(_cacheMutex);
			const auto it = _bytecodeCache.find(hash);
			if(it == _bytecodeCache.end() || it->second.empty())
			{
				return "";
			}
			bytecodeCopy = it->second;
		}

		ID3DBlob* pDisasmBlob = nullptr;
		PFN_D3DDisassemble pfn = reinterpret_cast<PFN_D3DDisassemble>(_pfnD3DDisassemble);
		HRESULT hr = pfn(bytecodeCopy.data(), bytecodeCopy.size(), 0, nullptr, &pDisasmBlob);
		if(SUCCEEDED(hr) && pDisasmBlob != nullptr)
		{
			std::string disasm(static_cast<const char*>(pDisasmBlob->GetBufferPointer()), pDisasmBlob->GetBufferSize());
			pDisasmBlob->Release();

			std::unique_lock lock(_cacheMutex);
			_disasmCache[hash] = disasm;
			return disasm;
		}

		return "";
	}

	ShaderAnalysisResult ShaderAnalyzer::analyzeShader(uint32_t hash, int shaderType)
	{
		if(hash == 0)
		{
			ShaderAnalysisResult res;
			res.categorySummary = "Unknown";
			return res;
		}

		{
			std::shared_lock lock(_cacheMutex);
			const auto it = _analysisCache.find(hash);
			if(it != _analysisCache.end() && it->second.isAnalyzed)
			{
				return it->second;
			}
		}

		ShaderAnalysisResult result;
		result.hash = hash;
		result.categoryFlags = Category_All;
		result.isAnalyzed = true;

		std::string disasm = disassembleShader(hash);
		if(!disasm.empty())
		{
			std::string lowerDisasm = disasm;
			std::transform(lowerDisasm.begin(), lowerDisasm.end(), lowerDisasm.begin(), [](unsigned char c) { return std::tolower(c); });

			// Heuristic 1: Outline / Edge Detection
			if(shaderType == 0) // Pixel shader
			{
				// Count texture samples per texture register (t0, t1, t2...)
				std::map<std::string, int> textureSamples;
				std::istringstream stream(lowerDisasm);
				std::string line;
				int totalSamples = 0;
				bool hasSub = lowerDisasm.find("sub ") != std::string::npos;
				bool hasAbs = lowerDisasm.find("abs ") != std::string::npos;
				bool hasDeriv = lowerDisasm.find("deriv_rtx") != std::string::npos || lowerDisasm.find("deriv_rty") != std::string::npos;
				bool hasDot = lowerDisasm.find("dp2 ") != std::string::npos || lowerDisasm.find("dp3 ") != std::string::npos || lowerDisasm.find("dp4 ") != std::string::npos;
				bool hasMinMax = lowerDisasm.find("min ") != std::string::npos || lowerDisasm.find("max ") != std::string::npos;

				while(std::getline(stream, line))
				{
					if(line.find("sample") != std::string::npos)
					{
						totalSamples++;
						// Find texture register name e.g. t0, t1, t2
						size_t tPos = line.find(" t");
						if(tPos == std::string::npos) tPos = line.find(", t");
						if(tPos != std::string::npos)
						{
							size_t start = (line[tPos] == ' ') ? tPos + 1 : tPos + 2;
							size_t end = line.find_first_of(" ,.\r\n", start);
							if(end != std::string::npos)
							{
								std::string tName = line.substr(start, end - start);
								textureSamples[tName]++;
							}
						}
					}
				}

				int maxSingleTextureSamples = 0;
				for(const auto& pair : textureSamples)
				{
					if(pair.second > maxSingleTextureSamples)
					{
						maxSingleTextureSamples = pair.second;
					}
				}

				// Edge detection filters sample the same texture 3-8 times and take differences
				if((maxSingleTextureSamples >= 4 && (hasSub && (hasAbs || hasDot || hasMinMax))) || hasDeriv)
				{
					result.categoryFlags |= Category_OutlineEdge;
					result.categorySummary = "Outline / Edge Filter";
				}
				else if(maxSingleTextureSamples >= 5 && !hasAbs && (lowerDisasm.find("add ") != std::string::npos || lowerDisasm.find("mad ") != std::string::npos))
				{
					result.categoryFlags |= Category_BloomBlur;
					result.categorySummary = "Bloom / Blur Filter";
				}

				// Heuristic 2: Vignette / Lens Falloff
				if((lowerDisasm.find("0.500000") != std::string::npos || lowerDisasm.find("2.000000") != std::string::npos) &&
				   (lowerDisasm.find("dp2 ") != std::string::npos || lowerDisasm.find("sqrt ") != std::string::npos) &&
				   (lowerDisasm.find("mul o0") != std::string::npos || lowerDisasm.find("mad o0") != std::string::npos))
				{
					result.categoryFlags |= Category_Vignette;
					if(result.categorySummary.empty()) result.categorySummary = "Vignette / Lens Falloff";
					else result.categorySummary += ", Vignette";
				}

				// Heuristic 3: Fog / Atmospheric Depth
				if((lowerDisasm.find("exp ") != std::string::npos || lowerDisasm.find("exp2 ") != std::string::npos) &&
				   (lowerDisasm.find("lerp ") != std::string::npos || lowerDisasm.find("mad ") != std::string::npos || lowerDisasm.find("add ") != std::string::npos))
				{
					result.categoryFlags |= Category_FogAtmospheric;
					if(result.categorySummary.empty()) result.categorySummary = "Fog / Depth Effect";
					else result.categorySummary += ", Fog";
				}

				// Heuristic 5: UI / HUD
				if(lowerDisasm.find("discard_nz") != std::string::npos || lowerDisasm.find("discard_z") != std::string::npos)
				{
					result.categoryFlags |= Category_UI_HUD;
					if(result.categorySummary.empty()) result.categorySummary = "UI / Alpha Mask";
					else result.categorySummary += ", UI";
				}
			}
			else if(shaderType == 1) // Vertex shader
			{
				// Inverted hull outline check in vertex shader: normal multiplied by width and added to position
				if(lowerDisasm.find("normal") != std::string::npos &&
				   lowerDisasm.find("mul ") != std::string::npos &&
				   lowerDisasm.find("add ") != std::string::npos)
				{
					result.categoryFlags |= Category_OutlineEdge;
					result.categorySummary = "Inverted-Hull Outline";
				}

				// UI orthographic vertex check
				if(lowerDisasm.find("0.000000") != std::string::npos && lowerDisasm.find("1.000000") != std::string::npos)
				{
					result.categoryFlags |= Category_UI_HUD;
					if(result.categorySummary.empty()) result.categorySummary = "UI / 2D Transform";
				}
			}
		}
		else
		{
			// Binary fallback heuristic when D3DCompiler is unavailable
			const auto* pBytecode = getBytecode(hash);
			if(pBytecode != nullptr && !pBytecode->empty())
			{
				const uint8_t* bytes = pBytecode->data();
				size_t len = pBytecode->size();

				// Check for DXBC magic "DXBC"
				if(len > 4 && bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C')
				{
					int sampleCount = 0;
					bool hasDeriv = false;
					bool hasExp = false;
					for(size_t i = 0; i < len - 4; ++i)
					{
						uint32_t token = *reinterpret_cast<const uint32_t*>(bytes + i);
						uint32_t opcode = token & 0x7FF;
						if(opcode == 69 || opcode == 70) sampleCount++; // SAMPLE or SAMPLE_L
						if(opcode == 26 || opcode == 27) hasDeriv = true; // DERIV_RTX or DERIV_RTY
						if(opcode == 25) hasExp = true; // EXP
					}

					if(sampleCount >= 4 || hasDeriv)
					{
						result.categoryFlags |= Category_OutlineEdge;
						result.categorySummary = "Outline / Edge (Binary Heuristic)";
					}
					if(hasExp)
					{
						result.categoryFlags |= Category_FogAtmospheric;
						if(result.categorySummary.empty()) result.categorySummary = "Fog / Depth (Binary Heuristic)";
					}
				}
			}
		}

		if(result.categorySummary.empty())
		{
			result.categorySummary = "Standard Shader";
		}

		std::unique_lock lock(_cacheMutex);
		_analysisCache[hash] = result;
		return result;
	}

	uint32_t ShaderAnalyzer::getCategoryFlags(uint32_t hash, int shaderType)
	{
		return analyzeShader(hash, shaderType).categoryFlags;
	}

	std::string ShaderAnalyzer::getCategorySummary(uint32_t hash, int shaderType)
	{
		return analyzeShader(hash, shaderType).categorySummary;
	}

	bool ShaderAnalyzer::dumpShader(uint32_t hash, const std::filesystem::path& basePath, std::string& outCsoName, std::string& outAsmName)
	{
		if(hash == 0)
		{
			return false;
		}

		std::vector<uint8_t> bytecode;
		{
			std::shared_lock lock(_cacheMutex);
			const auto it = _bytecodeCache.find(hash);
			if(it == _bytecodeCache.end() || it->second.empty())
			{
				return false;
			}
			bytecode = it->second;
		}

		std::filesystem::path dumpDir = basePath / "ShaderDumps";
		std::error_code ec;
		std::filesystem::create_directories(dumpDir, ec);

		char hexBuf[32];
		sprintf_s(hexBuf, "0x%08X", hash);
		outCsoName = std::string(hexBuf) + ".cso";
		outAsmName = std::string(hexBuf) + ".asm";

		std::filesystem::path csoPath = dumpDir / outCsoName;
		std::filesystem::path asmPath = dumpDir / outAsmName;

		// Write binary bytecode
		std::ofstream csoFile(csoPath, std::ios::binary);
		if(csoFile.is_open())
		{
			csoFile.write(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
			csoFile.close();
		}

		// Write disassembly
		std::string disasm = disassembleShader(hash);
		if(!disasm.empty())
		{
			std::ofstream asmFile(asmPath);
			if(asmFile.is_open())
			{
				asmFile << "// ShaderToggler Disassembly Dump\n";
				asmFile << "// Hash: " << hexBuf << "\n";
				asmFile << "// Bytecode size: " << bytecode.size() << " bytes\n\n";
				asmFile << disasm;
				asmFile.close();
			}
		}

		return true;
	}
}
