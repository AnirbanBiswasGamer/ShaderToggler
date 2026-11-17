#include "ShaderPatcher.h"
#include "ShaderAnalyzer.h"
#include "ShaderOverrideManager.h"
#include "DXBCChecksum.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace ShaderToggler
{
	// DXBC Opcode constants
	static constexpr DWORD D3D10_OPCODE_MAD = 50;  // 0x32
	static constexpr DWORD D3D10_OPCODE_MOV = 54;  // 0x36
	static constexpr DWORD D3D10_OPCODE_MUL = 56;  // 0x38
	static constexpr DWORD D3D10_OPCODE_NOP = 57;  // 0x39
	static constexpr DWORD D3D10_OPCODE_RET = 62;  // 0x3E

	// DXBC Operand Types
	static constexpr DWORD D3D10_OPERAND_TYPE_TEMP   = 0;
	static constexpr DWORD D3D10_OPERAND_TYPE_INPUT  = 1;
	static constexpr DWORD D3D10_OPERAND_TYPE_OUTPUT = 2; // Output in some SM4 versions
	static constexpr DWORD D3D10_OPERAND_TYPE_OUTPUT2 = 3; // Output register o0, o1...

	static BYTE* findChunk(BYTE* pBytecode, DWORD totalSize, const char fourCC[4], DWORD& outChunkLen)
	{
		if(totalSize < 32) return nullptr;
		DWORD numChunks = *reinterpret_cast<DWORD*>(pBytecode + 28);
		if(32 + numChunks * 4 > totalSize) return nullptr;

		for(DWORD i = 0; i < numChunks; ++i)
		{
			DWORD offset = *reinterpret_cast<DWORD*>(pBytecode + 32 + i * 4);
			if(offset + 8 <= totalSize)
			{
				if(memcmp(pBytecode + offset, fourCC, 4) == 0)
				{
					outChunkLen = *reinterpret_cast<DWORD*>(pBytecode + offset + 4);
					return pBytecode + offset;
				}
			}
		}
		return nullptr;
	}

	struct DXBCOperand
	{
		DWORD type = 0;
		DWORD regIndex = 0;
		DWORD dim = 0;
		DWORD nextOffset = 0;
	};

	static DXBCOperand parseDXBCOperand(const DWORD* tokens, DWORD opIdx, DWORD maxTokens)
	{
		DXBCOperand op;
		if(opIdx >= maxTokens) return op;

		DWORD opTok = tokens[opIdx];
		op.type = (opTok >> 12) & 0xFF;
		op.dim = (opTok >> 20) & 0x3;
		DWORD cur = opIdx + 1;

		// Extended operand modifier (e.g. negate, abs)
		if(opTok & 0x80000000)
		{
			cur++;
		}

		if(op.dim == 0)
		{
			op.regIndex = 0;
		}
		else if(op.dim == 1 && cur < maxTokens)
		{
			op.regIndex = tokens[cur++];
		}
		else if(op.dim == 2 && cur + 1 < maxTokens)
		{
			op.regIndex = tokens[cur + 1];
			cur += 2;
		}
		else if(op.dim == 3 && cur + 2 < maxTokens)
		{
			cur += 3;
		}

		op.nextOffset = cur;
		return op;
	}

	bool ShaderPatcher::patchCelOutlineInstruction(uint8_t* pBytecode, size_t size, std::string& outDetails)
	{
		DWORD shexLen = 0;
		BYTE* pShex = findChunk(pBytecode, static_cast<DWORD>(size), "SHEX", shexLen);
		if(pShex == nullptr)
		{
			pShex = findChunk(pBytecode, static_cast<DWORD>(size), "SHDR", shexLen);
		}
		if(pShex == nullptr)
		{
			outDetails = "Shader bytecode has no SHEX/SHDR instruction chunk";
			return false;
		}

		DWORD* tokens = reinterpret_cast<DWORD*>(pShex + 8);
		DWORD totalTokens = shexLen / 4;
		if(totalTokens <= 2)
		{
			outDetails = "Invalid instruction token count";
			return false;
		}

		struct ParsedInst
		{
			DWORD tokIdx = 0;
			DWORD opcode = 0;
			DWORD len = 0;
			DXBCOperand op0; // dest
			DXBCOperand op1; // src0
			DXBCOperand op2; // src1
			DXBCOperand op3; // src2
			bool hasImmOne = false;
		};

		std::vector<ParsedInst> insts;
		DWORD idx = 2; // skip version and length tokens

		while(idx < totalTokens)
		{
			DWORD opcodeToken = tokens[idx];
			DWORD opcode = opcodeToken & 0x7FF;
			DWORD len = (opcodeToken >> 24) & 0x7F;
			if(len == 0 || idx + len > totalTokens) break;

			ParsedInst info;
			info.tokIdx = idx;
			info.opcode = opcode;
			info.len = len;

			DWORD cur = idx + 1;
			if(opcodeToken & 0x80000000)
			{
				cur++; // skip extended opcode token
			}

			// Check for immediate 1.0f constant (0x3F800000)
			for(DWORD k = 0; k < len; ++k)
			{
				if(tokens[idx + k] == 0x3F800000)
				{
					info.hasImmOne = true;
					break;
				}
			}

			// Parse up to 4 operands
			if(cur < idx + len)
			{
				info.op0 = parseDXBCOperand(tokens, cur, idx + len);
				cur = info.op0.nextOffset;
			}
			if(cur < idx + len)
			{
				info.op1 = parseDXBCOperand(tokens, cur, idx + len);
				cur = info.op1.nextOffset;
			}
			if(cur < idx + len)
			{
				info.op2 = parseDXBCOperand(tokens, cur, idx + len);
				cur = info.op2.nextOffset;
			}
			if(cur < idx + len)
			{
				info.op3 = parseDXBCOperand(tokens, cur, idx + len);
				cur = info.op3.nextOffset;
			}

			insts.push_back(info);
			idx += len;
		}

		int targetTokIdx = -1;
		DWORD targetLen = 0;
		DWORD targetDestReg = 0;

		// Tier 1: Exact Cel-Outline Inverse Blend Pattern
		// In anime shaders: (1 - SceneColor) * LineColor + SceneColor
		// Represented as:
		//   add rInv, -rScene, 1.0f
		//   mad rScene, rInv, rLine, rScene (or mad rScene, rLine, rInv, rScene)
		for(size_t i = 0; i < insts.size(); ++i)
		{
			const auto& inst = insts[i];
			if(inst.opcode == D3D10_OPCODE_MAD)
			{
				// Check if destination temp register matches accumulator temp register (src2)
				if(inst.op0.type == D3D10_OPERAND_TYPE_TEMP &&
				   inst.op3.type == D3D10_OPERAND_TYPE_TEMP &&
				   inst.op0.regIndex == inst.op3.regIndex)
				{
					// Check preceding 1..4 instructions for an ADD with 1.0f
					for(int p = 1; p <= 4 && (static_cast<int>(i) - p) >= 0; ++p)
					{
						const auto& prev = insts[i - p];
						if(prev.opcode == 0 && prev.hasImmOne) // opcode 0 == ADD
						{
							targetTokIdx = static_cast<int>(inst.tokIdx);
							targetLen = inst.len;
							targetDestReg = inst.op0.regIndex;
							break;
						}
					}
					if(targetTokIdx >= 0) break;
				}
			}
		}

		// Tier 2: Direct Scene Accumulator MAD (mad rScene, rLine, factor, rScene)
		if(targetTokIdx < 0)
		{
			for(size_t i = 0; i < insts.size(); ++i)
			{
				const auto& inst = insts[i];
				if(inst.opcode == D3D10_OPCODE_MAD)
				{
					if(inst.op0.type == D3D10_OPERAND_TYPE_TEMP &&
					   inst.op3.type == D3D10_OPERAND_TYPE_TEMP &&
					   inst.op0.regIndex == inst.op3.regIndex)
					{
						targetTokIdx = static_cast<int>(inst.tokIdx);
						targetLen = inst.len;
						targetDestReg = inst.op0.regIndex;
					}
				}
			}
		}

		// Tier 3: Pre-output fallback (last MAD before color transform/ret)
		if(targetTokIdx < 0)
		{
			for(const auto& inst : insts)
			{
				if(inst.opcode == D3D10_OPCODE_MAD || inst.opcode == D3D10_OPCODE_MUL)
				{
					targetTokIdx = static_cast<int>(inst.tokIdx);
					targetLen = inst.len;
					targetDestReg = inst.op0.regIndex;
				}
			}
		}

		if(targetTokIdx >= 0 && targetLen > 0)
		{
			// NOP out the cel-outline blend instruction
			tokens[targetTokIdx] = (targetLen << 24) | D3D10_OPCODE_NOP;
			for(DWORD j = 1; j < targetLen; ++j)
			{
				tokens[targetTokIdx + j] = 0;
			}

			char buf[160];
			sprintf_s(buf, "Neutralized cel-outline blend 'mad r%u, ... + r%u' (token %d). Water & scene intact!",
			          targetDestReg, targetDestReg, targetTokIdx);
			outDetails = buf;
			return true;
		}

		outDetails = "No suitable cel-outline blend instruction found";
		return false;
	}

	bool ShaderPatcher::patchMRTO1Target(uint8_t* pBytecode, size_t size, std::string& outDetails)
	{
		DWORD shexLen = 0;
		BYTE* pShex = findChunk(pBytecode, static_cast<DWORD>(size), "SHEX", shexLen);
		if(pShex == nullptr)
		{
			pShex = findChunk(pBytecode, static_cast<DWORD>(size), "SHDR", shexLen);
		}
		if(pShex == nullptr)
		{
			outDetails = "Shader bytecode has no SHEX/SHDR instruction chunk";
			return false;
		}

		DWORD* tokens = reinterpret_cast<DWORD*>(pShex + 8);
		DWORD totalTokens = shexLen / 4;
		DWORD idx = 2;
		int nopCount = 0;

		while(idx < totalTokens)
		{
			DWORD opcodeToken = tokens[idx];
			DWORD len = (opcodeToken >> 24) & 0x7F;
			if(len == 0 || idx + len > totalTokens) break;

			// Check first destination operand
			if(len >= 3)
			{
				DWORD op0 = tokens[idx + 1];
				DWORD opType = (op0 >> 12) & 0xFF;
				DWORD dim = (op0 >> 20) & 0x3;

				// Output register o1 check
				if((opType == D3D10_OPERAND_TYPE_OUTPUT || opType == D3D10_OPERAND_TYPE_OUTPUT2) && dim >= 1)
				{
					DWORD regIdx = tokens[idx + 2];
					if(regIdx == 1) // o1
					{
						tokens[idx] = (len << 24) | D3D10_OPCODE_NOP;
						for(DWORD j = 1; j < len; ++j)
						{
							tokens[idx + j] = 0;
						}
						nopCount++;
					}
				}
			}
			idx += len;
		}

		if(nopCount > 0)
		{
			char buf[128];
			sprintf_s(buf, "Neutralized %d write(s) to MRT o1 outline target", nopCount);
			outDetails = buf;
			return true;
		}

		outDetails = "No writes to o1 outline target found in bytecode";
		return false;
	}

	bool ShaderPatcher::patchInvertedHullExtrusion(uint8_t* pBytecode, size_t size, std::string& outDetails)
	{
		DWORD shexLen = 0;
		BYTE* pShex = findChunk(pBytecode, static_cast<DWORD>(size), "SHEX", shexLen);
		if(pShex == nullptr)
		{
			pShex = findChunk(pBytecode, static_cast<DWORD>(size), "SHDR", shexLen);
		}
		if(pShex == nullptr)
		{
			outDetails = "Vertex shader has no SHEX/SHDR instruction chunk";
			return false;
		}

		DWORD* tokens = reinterpret_cast<DWORD*>(pShex + 8);
		DWORD totalTokens = shexLen / 4;
		DWORD idx = 2;
		int patchedIdx = -1;

		while(idx < totalTokens)
		{
			DWORD opcodeToken = tokens[idx];
			DWORD opcode = opcodeToken & 0x7FF;
			DWORD len = (opcodeToken >> 24) & 0x7F;
			if(len == 0 || idx + len > totalTokens) break;

			// Extrusion along normal is typically a MAD or MUL instruction
			if(opcode == D3D10_OPCODE_MAD)
			{
				// NOP this MAD instruction to prevent vertex offset
				tokens[idx] = (len << 24) | D3D10_OPCODE_NOP;
				for(DWORD j = 1; j < len; ++j)
				{
					tokens[idx + j] = 0;
				}
				patchedIdx = static_cast<int>(idx);
				break;
			}
			idx += len;
		}

		if(patchedIdx >= 0)
		{
			char buf[128];
			sprintf_s(buf, "Neutralized vertex normal extrusion (collapsed inverted hull at token %d)", patchedIdx);
			outDetails = buf;
			return true;
		}

		outDetails = "No vertex extrusion instruction found to neutralize";
		return false;
	}

	bool ShaderPatcher::generatePassThroughBytecode(const std::vector<uint8_t>& origBytecode,
	                                                std::vector<uint8_t>& outPatchedBytecode, std::string& outDetails)
	{
		// Clean fullscreen pass-through pixel shader: routes scene texture t0 directly to o0,
		// eliminating blur, DoF, fog, and vignette with maximum performance and zero artifacts.
		const char* passThroughHLSL =
			"Texture2D t0 : register(t0);\n"
			"SamplerState s0 : register(s0);\n"
			"struct PS_INPUT {\n"
			"    float4 pos : SV_Position;\n"
			"    float2 uv : TEXCOORD0;\n"
			"};\n"
			"float4 main(PS_INPUT input) : SV_Target\n"
			"{\n"
			"    return t0.Sample(s0, input.uv);\n"
			"}\n";

		std::string errors;
		bool ok = ShaderOverrideManager::getInstance().compileHLSL(
			passThroughHLSL, "AutoPassThrough.hlsl", "ps_5_0", "main", outPatchedBytecode, errors);

		if(!ok)
		{
			ok = ShaderOverrideManager::getInstance().compileHLSL(
				passThroughHLSL, "AutoPassThrough.hlsl", "ps_4_0", "main", outPatchedBytecode, errors);
		}

		if(ok && !outPatchedBytecode.empty())
		{
			outDetails = "Compiled universal crystal-clear pass-through shader (bypasses blur/vignette)";
			return true;
		}

		outDetails = "Failed to compile pass-through shader: " + errors;
		return false;
	}

	bool ShaderPatcher::patchDXBCBytecode(const std::vector<uint8_t>& originalBytecode, PatchStrategy strategy,
	                                      std::vector<uint8_t>& outPatchedBytecode, std::string& outDetails)
	{
		if(originalBytecode.size() < 32 || memcmp(originalBytecode.data(), "DXBC", 4) != 0)
		{
			outDetails = "Invalid DXBC shader container";
			return false;
		}

		if(strategy == PatchStrategy::Blur_PassThrough || strategy == PatchStrategy::Vignette_PassThrough)
		{
			return generatePassThroughBytecode(originalBytecode, outPatchedBytecode, outDetails);
		}

		outPatchedBytecode = originalBytecode;
		bool success = false;

		switch(strategy)
		{
			case PatchStrategy::MRT_ZeroTargetO1:
				success = patchMRTO1Target(outPatchedBytecode.data(), outPatchedBytecode.size(), outDetails);
				break;
			case PatchStrategy::InvertedHull_VertexNormal:
				success = patchInvertedHullExtrusion(outPatchedBytecode.data(), outPatchedBytecode.size(), outDetails);
				break;
			case PatchStrategy::CelOutline_MadBlend:
			default:
				// First try cel outline blend instruction
				success = patchCelOutlineInstruction(outPatchedBytecode.data(), outPatchedBytecode.size(), outDetails);
				if(!success)
				{
					// Fallback to checking MRT o1
					success = patchMRTO1Target(outPatchedBytecode.data(), outPatchedBytecode.size(), outDetails);
				}
				break;
		}

		if(!success)
		{
			outPatchedBytecode.clear();
			return false;
		}

		// Recompute Microsoft DXBC container checksum
		DWORD newHash[4] = { 0 };
		if(CalculateDXBCChecksum(outPatchedBytecode.data(), static_cast<DWORD>(outPatchedBytecode.size()), newHash))
		{
			memcpy(outPatchedBytecode.data() + 4, newHash, 16);
			return true;
		}

		outDetails = "Checksum recalculation failed";
		outPatchedBytecode.clear();
		return false;
	}

	bool ShaderPatcher::autoPatchShader(uint32_t shaderHash, int shaderType, PatchStrategy strategy,
	                                    const std::filesystem::path& basePath, std::string& outMsg)
	{
		if(shaderHash == 0)
		{
			outMsg = "Invalid shader hash";
			return false;
		}

		const auto* origBytecode = ShaderAnalyzer::getInstance().getBytecode(shaderHash);
		if(origBytecode == nullptr || origBytecode->empty())
		{
			outMsg = "No bytecode cached for shader 0x" + std::to_string(shaderHash);
			return false;
		}

		// Auto-detect optimal strategy if requested
		if(strategy == PatchStrategy::AutoDetect)
		{
			uint32_t flags = ShaderAnalyzer::getInstance().getCategoryFlags(shaderHash, shaderType);
			std::string disasm = ShaderAnalyzer::getInstance().disassembleShader(shaderHash);

			if(shaderType == 1) // Vertex shader
			{
				strategy = PatchStrategy::InvertedHull_VertexNormal;
			}
			else if((flags & Category_OutlineEdge) != 0)
			{
				if(disasm.find("dcl_output o1") != std::string::npos)
				{
					strategy = PatchStrategy::MRT_ZeroTargetO1;
				}
				else
				{
					strategy = PatchStrategy::CelOutline_MadBlend;
				}
			}
			else if((flags & Category_BloomBlur) != 0 || (flags & Category_FogAtmospheric) != 0 || (flags & Category_Vignette) != 0)
			{
				strategy = PatchStrategy::Blur_PassThrough;
			}
			else
			{
				strategy = PatchStrategy::CelOutline_MadBlend;
			}
		}

		std::vector<uint8_t> patchedBytecode;
		std::string details;
		if(!patchDXBCBytecode(*origBytecode, strategy, patchedBytecode, details))
		{
			outMsg = "Patch failed: " + details;
			return false;
		}

		// Save patched .cso to <GameDir>/ShaderFixes/
		std::filesystem::path fixesDir = basePath / "ShaderFixes";
		std::error_code ec;
		std::filesystem::create_directories(fixesDir, ec);

		char hexBuf[32];
		sprintf_s(hexBuf, "0x%08X", shaderHash);
		std::filesystem::path csoPath = fixesDir / (std::string(hexBuf) + ".cso");

		std::ofstream csoFile(csoPath, std::ios::binary);
		if(!csoFile.is_open())
		{
			outMsg = "Failed to write patch file to " + csoPath.filename().string();
			return false;
		}

		csoFile.write(reinterpret_cast<const char*>(patchedBytecode.data()), patchedBytecode.size());
		csoFile.close();

		// Register in ShaderOverrideManager for immediate live effect
		ShaderOverrideManager::getInstance().addOverride(shaderHash, patchedBytecode);

		outMsg = "Patched! (" + details + " -> ShaderFixes/" + std::string(hexBuf) + ".cso)";
		return true;
	}

	bool ShaderPatcher::revertPatch(uint32_t shaderHash, const std::filesystem::path& basePath, std::string& outMsg)
	{
		if(shaderHash == 0)
		{
			outMsg = "Invalid shader hash";
			return false;
		}

		char hexBuf[32];
		sprintf_s(hexBuf, "0x%08X", shaderHash);
		std::filesystem::path fixesDir = basePath / "ShaderFixes";

		std::error_code ec;
		std::filesystem::path csoPath = fixesDir / (std::string(hexBuf) + ".cso");
		std::filesystem::path hlslPath = fixesDir / (std::string(hexBuf) + ".hlsl");

		bool removed = false;
		if(std::filesystem::exists(csoPath, ec))
		{
			std::filesystem::remove(csoPath, ec);
			removed = true;
		}
		if(std::filesystem::exists(hlslPath, ec))
		{
			std::filesystem::remove(hlslPath, ec);
			removed = true;
		}

		ShaderOverrideManager::getInstance().removeOverride(shaderHash);

		outMsg = removed ? "Reverted patch and removed fix file for " + std::string(hexBuf)
		                 : "Removed in-memory override for " + std::string(hexBuf);
		return true;
	}

	bool ShaderPatcher::isPatchActive(uint32_t shaderHash, const std::filesystem::path& basePath)
	{
		if(shaderHash == 0) return false;

		char hexBuf[32];
		sprintf_s(hexBuf, "0x%08X", shaderHash);
		std::filesystem::path fixesDir = basePath / "ShaderFixes";

		std::error_code ec;
		std::filesystem::path csoPath = fixesDir / (std::string(hexBuf) + ".cso");
		std::filesystem::path hlslPath = fixesDir / (std::string(hexBuf) + ".hlsl");

		if(std::filesystem::exists(csoPath, ec) || std::filesystem::exists(hlslPath, ec))
		{
			return true;
		}

		return ShaderOverrideManager::getInstance().hasOverride(shaderHash, 0, 0);
	}
}
