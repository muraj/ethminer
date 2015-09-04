/*
This file is part of cpp-ethereum.

cpp-ethereum is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

cpp-ethereum is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file EthashCUDAMiner.cpp
* @author Gav Wood <i@gavwood.com>
* @date 2014
*
* Determines the PoW algorithm.
*/

#if ETH_ETHASHCUDA || !ETH_TRUE

#if defined(WIN32)
#include <Windows.h>
#endif

#include "EthashCUDAMiner.h"
#include <thread>
#include <chrono>
#include <libethash-cuda/ethash_cuda_miner.h>
using namespace std;
using namespace dev;
using namespace eth;

namespace dev
{
namespace eth
{
	class EthashCUDAHook : public ethash_cuda_miner::search_hook
	{
	public:
		EthashCUDAHook(EthashCUDAMiner* _owner) : m_owner(_owner) {}
		EthashCUDAHook(EthashCUDAHook const&) = delete;

		void abort()
		{
			{
				UniqueGuard l(x_all);
				if (m_aborted)
					return;
				//		cdebug << "Attempting to abort";

				m_abort = true;
			}
			// m_abort is true so now searched()/found() will return true to abort the search.
			// we hang around on this thread waiting for them to point out that they have aborted since
			// otherwise we may end up deleting this object prior to searched()/found() being called.
			m_aborted.wait(true);
			//		for (unsigned timeout = 0; timeout < 100 && !m_aborted; ++timeout)
			//			std::this_thread::sleep_for(chrono::milliseconds(30));
			//		if (!m_aborted)
			//			cwarn << "Couldn't abort. Abandoning OpenCL process.";
		}

		void reset()
		{
			UniqueGuard l(x_all);
			m_aborted = m_abort = false;
		}

	protected:
		virtual bool found(uint64_t const* _nonces, uint32_t _count) override
		{
			//		dev::operator <<(std::cerr << "Found nonces: ", vector<uint64_t>(_nonces, _nonces + _count)) << std::endl;
			for (uint32_t i = 0; i < _count; ++i)
				if (m_owner->report(_nonces[i]))
					return (m_aborted = true);
			return m_owner->shouldStop();
		}

		virtual bool searched(uint64_t _startNonce, uint32_t _count) override
		{
			UniqueGuard l(x_all);
			//		std::cerr << "Searched " << _count << " from " << _startNonce << std::endl;
			m_owner->accumulateHashes(_count);
			m_last = _startNonce + _count;
			if (m_abort || m_owner->shouldStop())
				return (m_aborted = true);
			return false;
		}

	private:
		Mutex x_all;
		uint64_t m_last;
		bool m_abort = false;
		Notified<bool> m_aborted = { true };
		EthashCUDAMiner* m_owner = nullptr;
	};
}
}

unsigned EthashCUDAMiner::s_platformId = 0;
unsigned EthashCUDAMiner::s_deviceId = 0;
unsigned EthashCUDAMiner::s_numInstances = 0;
int EthashCUDAMiner::s_devices[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

EthashCUDAMiner::EthashCUDAMiner(ConstructionInfo const& _ci) :
	GenericMiner<EthashProofOfWork>(_ci),
	Worker("cudaminer" + toString(index())),
m_hook( new EthashCUDAHook(this))
{
/*
#if defined(WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	int num_cpus = sysinfo.dwNumberOfProcessors;
	SetThreadAffinityMask(GetCurrentThread(), 1 << (index() % num_cpus));
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
*/
}
EthashCUDAMiner::~EthashCUDAMiner()
{
	pause();
	delete m_miner;
	delete m_hook;
}

bool EthashCUDAMiner::report(uint64_t _nonce)
{
	Nonce n = (Nonce)(u64)_nonce;
	EthashProofOfWork::Result r = EthashAux::eval(work().seedHash, work().headerHash, n);
	if (r.value < work().boundary)
		return submitProof(Solution{ n, r.mixHash });
	return false;
}

void EthashCUDAMiner::kickOff()
{
	m_hook->reset();
	startWorking();
}

void EthashCUDAMiner::workLoop()
{
	// take local copy of work since it may end up being overwritten by kickOff/pause.
	try {
		WorkPackage w = work();
		cnote << "workLoop" << !!m_miner << m_minerSeed << w.seedHash;
		if (!m_miner || m_minerSeed != w.seedHash)
		{
			cnote << "Initialising miner...";
			m_minerSeed = w.seedHash;

			delete m_miner;
			m_miner = new ethash_cuda_miner;

			unsigned device = s_devices[index()] > -1 ? s_devices[index()] : index();

			EthashAux::FullType dag;
			while (true)
			{
				if ((dag = EthashAux::full(w.seedHash, true)))
					break;
				if (shouldStop())
				{
					delete m_miner;
					m_miner = nullptr;
					return;
				}
				cnote << "Awaiting DAG";
				this_thread::sleep_for(chrono::milliseconds(500));
			}
			bytesConstRef dagData = dag->data();
			m_miner->init(dagData.data(), dagData.size(), device);
		}

		uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)w.boundary >> 192);
		m_miner->search(w.headerHash.data(), upper64OfBoundary, *m_hook);
	}
	catch (std::runtime_error const& _e)
	{
		delete m_miner;
		m_miner = nullptr;
		cwarn << "Error CUDA mining: " << _e.what();
	}
}

void EthashCUDAMiner::pause()
{
	m_hook->abort();
	stopWorking();
}

std::string EthashCUDAMiner::platformInfo()
{
	return ethash_cuda_miner::platform_info(s_deviceId);
}

unsigned EthashCUDAMiner::getNumDevices()
{
	return ethash_cuda_miner::getNumDevices();
}

void EthashCUDAMiner::listDevices()
{
	return ethash_cuda_miner::listDevices();
}

bool EthashCUDAMiner::configureGPU(
	unsigned _blockSize,
	unsigned _gridSize,
	unsigned _numStreams,
	unsigned _extraGPUMemory,
	bool _highcpu,
	uint64_t _currentBlock
	)
{
	if (_blockSize != 32 && _blockSize != 64 && _blockSize != 128)
	{
		cout << "Given localWorkSize of " << toString(_blockSize) << "is invalid. Must be either 32,64 or 128" << endl;
		return false;
	}
	if (!ethash_cuda_miner::configureGPU(
		s_devices,
		_blockSize,
		_gridSize,
		_numStreams,
		_extraGPUMemory,
		_highcpu,
		_currentBlock)
		)
	{
		cout << "No CUDA device with sufficient memory was found. Can't CUDA mine. Remove the -U argument" << endl;
		return false;
	}
	return true;
}

#endif