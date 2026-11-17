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

#include "ShaderManager.h"
#include "ShaderAnalyzer.h"
#include <algorithm>

using namespace reshade::api;

namespace ShaderToggler
{
	ShaderManager::ShaderManager(): _activeHuntedShaderHash(0), _heuristicFilter(0), _shaderType(0)
	{
	}


	void ShaderManager::addHashHandlePair(uint32_t shaderHash, uint64_t pipelineHandle)
	{
		if(pipelineHandle>0 && shaderHash > 0)
		{
			std::unique_lock lock(_hashHandlesMutex);
			_handleToShaderHash[pipelineHandle] = shaderHash;
			_shaderHashes.emplace(shaderHash);
		}
	}


	void ShaderManager::removeHandle(uint64_t handle)
	{
		std::unique_lock ulock(_hashHandlesMutex);
		if(_handleToShaderHash.count(handle)==1)
		{
			const auto it = _handleToShaderHash.find(handle);
			const auto shaderHash = it->second;
			_handleToShaderHash.erase(handle);
			_collectedActiveShaderHashes.erase(shaderHash);
			_shaderHashes.erase(shaderHash);
		}
	}


	void ShaderManager::startHuntingMode(const std::unordered_set<uint32_t>& currentMarkedHashes)
	{
		// copy the currently marked hashes (from the active group) to the set of marked hashes.
		{
			std::unique_lock lock(_markedShaderHashMutex);
			_markedShaderHashes.clear();
			for(const auto hash : currentMarkedHashes)
			{
				_markedShaderHashes.emplace(hash);
			}
		}

		// switch on hunting mode
		_isInHuntingMode = true;
		_activeHuntedShaderIndex = -1;
		_activeHuntedShaderHash = 0;
		_heuristicFilter = 0;
		{
			std::unique_lock lock(_collectedActiveHandlesMutex);
			_collectedActiveShaderHashes.clear();			// clear it so we start with a clean slate
			_filteredHuntedShaderHashes.clear();
		}
	}


	void ShaderManager::stopHuntingMode()
	{
		_isInHuntingMode = false;
		_activeHuntedShaderIndex = -1;
		_activeHuntedShaderHash = 0;
		_heuristicFilter = 0;
		{
			std::unique_lock lock(_collectedActiveHandlesMutex);
			_filteredHuntedShaderHashes.clear();
		}
		{
			std::unique_lock lock(_markedShaderHashMutex);
			_markedShaderHashes.clear();
		}
	}


	uint32_t ShaderManager::getAmountShaderHashesCollected()
	{
		std::shared_lock lock(_collectedActiveHandlesMutex);
		return static_cast<uint32_t>(_filteredHuntedShaderHashes.size());
	}


	uint32_t ShaderManager::getTotalRawHashesCollected()
	{
		std::shared_lock lock(_collectedActiveHandlesMutex);
		return static_cast<uint32_t>(_collectedActiveShaderHashes.size());
	}


	void ShaderManager::setHeuristicFilter(int categoryFlag)
	{
		_heuristicFilter = categoryFlag;
		rebuildFilteredHashes();
	}


	int ShaderManager::getHeuristicCandidateCount(int categoryFlag)
	{
		std::shared_lock lock(_collectedActiveHandlesMutex);
		if(categoryFlag == 0)
		{
			return static_cast<int>(_collectedActiveShaderHashes.size());
		}
		int count = 0;
		for(const auto hash : _collectedActiveShaderHashes)
		{
			if(ShaderAnalyzer::getInstance().getCategoryFlags(hash, _shaderType) & categoryFlag)
			{
				count++;
			}
		}
		return count;
	}


	void ShaderManager::rebuildFilteredHashes()
	{
		std::unique_lock lock(_collectedActiveHandlesMutex);
		_filteredHuntedShaderHashes.clear();

		for(const auto hash : _collectedActiveShaderHashes)
		{
			if(_heuristicFilter == 0)
			{
				_filteredHuntedShaderHashes.push_back(hash);
			}
			else
			{
				if(ShaderAnalyzer::getInstance().getCategoryFlags(hash, _shaderType) & _heuristicFilter)
				{
					_filteredHuntedShaderHashes.push_back(hash);
				}
			}
		}

		if(_filteredHuntedShaderHashes.empty())
		{
			_activeHuntedShaderIndex = -1;
			_activeHuntedShaderHash = 0;
		}
		else
		{
			auto it = std::find(_filteredHuntedShaderHashes.begin(), _filteredHuntedShaderHashes.end(), _activeHuntedShaderHash);
			if(it != _filteredHuntedShaderHashes.end())
			{
				_activeHuntedShaderIndex = static_cast<int>(std::distance(_filteredHuntedShaderHashes.begin(), it));
			}
			else
			{
				_activeHuntedShaderIndex = 0;
				_activeHuntedShaderHash = _filteredHuntedShaderHashes[0];
			}
		}
	}


	void ShaderManager::setActiveHuntedShaderHandle()
	{
		if(_activeHuntedShaderIndex < 0 || _filteredHuntedShaderHashes.empty() || static_cast<size_t>(_activeHuntedShaderIndex) >= _filteredHuntedShaderHashes.size())
		{
			_activeHuntedShaderHash = 0;
			return;
		}

		_activeHuntedShaderHash = _filteredHuntedShaderHashes[_activeHuntedShaderIndex];
	}


	void ShaderManager::huntNextShader(bool ctrlPressed, int stepCount)
	{
		if(!_isInHuntingMode)
		{
			return;
		}
		if(_filteredHuntedShaderHashes.empty())
		{
			return;
		}
		if(ctrlPressed)
		{
			if(_markedShaderHashes.empty() || (_markedShaderHashes.size() == 1 && _markedShaderHashes.count(_activeHuntedShaderHash)==1))
			{
				return;
			}

			const int total = static_cast<int>(_filteredHuntedShaderHashes.size());
			int index = (_activeHuntedShaderIndex + 1) % total;
			while(index != _activeHuntedShaderIndex)
			{
				uint32_t hash = _filteredHuntedShaderHashes[index];
				if(_markedShaderHashes.count(hash) == 1)
				{
					_activeHuntedShaderIndex = index;
					_activeHuntedShaderHash = hash;
					break;
				}
				index = (index + 1) % total;
			}
			return;
		}

		const int total = static_cast<int>(_filteredHuntedShaderHashes.size());
		_activeHuntedShaderIndex = (_activeHuntedShaderIndex + stepCount) % total;
		if(_activeHuntedShaderIndex < 0)
		{
			_activeHuntedShaderIndex += total;
		}
		setActiveHuntedShaderHandle();
	}


	void ShaderManager::huntPreviousShader(bool ctrlPressed, int stepCount)
	{
		if(!_isInHuntingMode)
		{
			return;
		}
		if(_filteredHuntedShaderHashes.empty())
		{
			return;
		}
		if(ctrlPressed)
		{
			if(_markedShaderHashes.empty() || (_markedShaderHashes.size() == 1 && _markedShaderHashes.count(_activeHuntedShaderHash) == 1))
			{
				return;
			}

			const int total = static_cast<int>(_filteredHuntedShaderHashes.size());
			int index = (_activeHuntedShaderIndex - 1 + total) % total;
			while(index != _activeHuntedShaderIndex)
			{
				uint32_t hash = _filteredHuntedShaderHashes[index];
				if(_markedShaderHashes.count(hash) == 1)
				{
					_activeHuntedShaderIndex = index;
					_activeHuntedShaderHash = hash;
					break;
				}
				index = (index - 1 + total) % total;
			}
			return;
		}

		const int total = static_cast<int>(_filteredHuntedShaderHashes.size());
		_activeHuntedShaderIndex = (_activeHuntedShaderIndex - stepCount) % total;
		if(_activeHuntedShaderIndex < 0)
		{
			_activeHuntedShaderIndex += total;
		}
		setActiveHuntedShaderHandle();
	}


	bool ShaderManager::isBlockedShader(uint32_t shaderHash)
	{
		if(shaderHash == 0)
		{
			return false;
		}

		bool toReturn = false;
		if(_isInHuntingMode)
		{
			// get the shader hash bound to this pipeline handle
			toReturn |= (_activeHuntedShaderHash == shaderHash);
		}
		if(_hideMarkedShaders)
		{
			// check if the shader hash is part of the toggle group
			std::shared_lock lock(_markedShaderHashMutex);
			toReturn |= (_markedShaderHashes.count(shaderHash) == 1);
		}

		return toReturn;
	}


	void ShaderManager::addActivePipelineHandle(uint64_t handle)
	{
		// get the shader hash bound to this pipeline handle
		const auto shaderHash = getShaderHash(handle);
		if(shaderHash>0)
		{
			std::unique_lock lock(_collectedActiveHandlesMutex);
			_collectedActiveShaderHashes.emplace(shaderHash);
		}
	}


	void ShaderManager::toggleMarkOnHuntedShader()
	{
		if(_activeHuntedShaderHash<=0)
		{
			return;
		}
		std::unique_lock lock(_markedShaderHashMutex);
		if(_markedShaderHashes.count(_activeHuntedShaderHash)==1)
		{
			// remove it
			_markedShaderHashes.erase(_activeHuntedShaderHash);
		}
		else
		{
			// add it
			_markedShaderHashes.emplace(_activeHuntedShaderHash);
		}
	}


	uint32_t ShaderManager::getShaderHash(uint64_t handle)
	{
		if(handle == 0)
		{
			return 0;
		}

		std::shared_lock lock(_hashHandlesMutex);
		const auto it = _handleToShaderHash.find(handle);
		if(it != _handleToShaderHash.end())
		{
			return it->second;
		}
		return 0;
	}
}
