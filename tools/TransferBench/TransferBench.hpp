/*
Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <vector>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <set>
#include <unistd.h>
#include <map>
#include <iostream>
#include <sstream>
#include <hip/hip_runtime.h>
#include <hip/hip_ext.h>
#include <hsa/hsa_ext_amd.h>

#include "EnvVars.hpp"

// Helper macro for catching HIP errors
#define HIP_CALL(cmd)                                                   \
    do {                                                                \
        hipError_t error = (cmd);                                       \
        if (error != hipSuccess)                                        \
        {                                                               \
            std::cerr << "Encountered HIP error (" << hipGetErrorString(error) << ") at line " \
                      << __LINE__ << " in file " << __FILE__ << "\n";   \
            exit(-1);                                                   \
        }                                                               \
    } while (0)

// Different src/dst memory types supported
typedef enum
{
  MEM_CPU      = 0,    // Pinned CPU memory
  MEM_GPU      = 1,    // Coarse-grained global GPU memory
  MEM_GPU_FINE = 2     // Fine-grained global GPU memory
} MemType;

char const MemTypeStr[4] = "CGF";

typedef enum
{
  MODE_FILL  = 0,         // Fill data with pattern
  MODE_CHECK = 1          // Check data against pattern
} ModeType;

// Each threadblock copies N floats from src to dst
struct BlockParam
{
    int N;
    float* src;
    float* dst;
};

// Each Link is a uni-direction operation from a src memory to dst memory executed by a specific GPU
struct Link
{
  // Link config
  MemType exeMemType;      // Link executor type (CPU or GPU)
  int     exeIndex;        // Executor index (NUMA node for CPU / device ID for GPU)
  MemType srcMemType;      // Source memory type
  int     srcIndex;        // Source device index
  MemType dstMemType;      // Destination memory type
  int     dstIndex;        // Destination device index
  int     numBlocksToUse;  // Number of threadblocks to use for this Link

  // Link implementation
  float*      srcMem;      // Source memory
  float*      dstMem;      // Destination memory

  hipEvent_t  startEvent;
  hipEvent_t  stopEvent;
  hipStream_t stream;
  BlockParam* blockParam;

  double totalTime;
};

void DisplayUsage(char const* cmdName);                      // Display usage instructions
void GenerateConfigFile(char const* cfgFile, int numBlocks); // Generate a sample config file
void DisplayTopology();                                      // Display GPU topology
void PopulateTestSizes(size_t const numBytesPerLink, int const samplingFactor, std::vector<size_t>& valuesofN);
void ParseMemType(std::string const& token, int const numCpus, int const numGpus, MemType* memType, int* memIndex);
void ParseLinks(char* line, int numCpus, int numGpus, std::vector<Link>& links);       // Parse Link information
void EnablePeerAccess(int const deviceId, int const peerDeviceId);
void AllocateMemory(MemType memType, int devIndex, size_t numBytes, float** memPtr);
void DeallocateMemory(MemType memType, float* memPtr);
void CheckPages(char* byteArray, size_t numBytes, int targetId);
void CheckOrFill(ModeType mode, int N, bool isMemset, bool isHipCall, std::vector<float> const& fillPattern, float* ptr);
void RunLink(EnvVars const& ev, size_t const N, int const iteration, Link& link);
void RunPeerToPeerBenchmarks(EnvVars const& ev, size_t N, int numBlocksToUse, int readMode, int skipCpu);
double GetPeakBandwidth(EnvVars const& ev, size_t N, int isBidirectional,
                        MemType srcMemType, int srcIndex,
                        MemType dstMemType, int dstIndex,
                        int readMode);

std::string GetLinkTypeDesc(uint32_t linkType, uint32_t hopCount);
std::string GetDesc(MemType srcMemType, int srcIndex,
                    MemType dstMemType, int dstIndex);
std::string GetLinkDesc(Link const& link);
int RemappedIndex(int const origIdx, MemType const memType);
