/*************************************************************************
 * Copyright (c) 2015-2021, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#ifndef NCCL_DEVICE_H_
#define NCCL_DEVICE_H_

#include "nccl.h"
#include "rccl_bfloat16.h"
#include "align.h"
#include <stdint.h>
// [RCCL] Support for clique-based kernels
#include "clique/CliqueCommon.h"
// [/RCCL]

// Convert volatile access to atomic
#if defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
#define LOAD(VAR) __atomic_load_n((VAR), __ATOMIC_SEQ_CST)
#define STORE(DST, SRC) __atomic_store_n((DST), (SRC), __ATOMIC_SEQ_CST)
#else
#define LOAD(VAR) *(VAR)
#define STORE(DST, SRC) *(DST) = (SRC)
#endif


#define NCCL_NUM_FUNCTIONS 5 // SendRecv not included for now
typedef enum { ncclFuncBroadcast, ncclFuncReduce, ncclFuncAllGather, ncclFuncReduceScatter, ncclFuncAllReduce, ncclFuncSendRecv, ncclNumFuncs} ncclFunc_t;
extern const char* ncclFuncStr[NCCL_NUM_FUNCTIONS+1];

#define NCCL_NUM_ALGORITHMS 3 // Tree/Ring/CollNet
#define NCCL_ALGO_TREE 0
#define NCCL_ALGO_RING 1
#define NCCL_ALGO_COLLNET 2
extern const char* ncclAlgoStr[NCCL_NUM_ALGORITHMS];

#define NCCL_NUM_PROTOCOLS 3 // Simple/LL/LL128
#define NCCL_PROTO_LL 0
#define NCCL_PROTO_LL128 1
#define NCCL_PROTO_CLIQUE 1  // [RCCL] Clique takes up same protocol as unused LL128
#define NCCL_PROTO_SIMPLE 2
extern const char* ncclProtoStr[NCCL_NUM_PROTOCOLS];

#define NCCL_MAX_OPS 2048
#define NCCL_STEPS 8

union ncclLLFifoLine {
  /* Flags have to be *after* data, because otherwise, an incomplete receive
     from the network may receive the flag but not the data.
     Note this is assuming that either we receive contiguous chunks of data
     (sockets) or data is written with an atomicity of 8 bytes (IB/RDMA). */
  struct {
    uint32_t data1;
    uint32_t flag1;
    uint32_t data2;
    uint32_t flag2;
  };
  uint64_t v[2];
  int4 i4;
};

#if defined(__gfx1030__)
#define WARP_SIZE 32
#else
#define WARP_SIZE 64
#endif
#define MAXCHANNELS 32
#define NCCL_MAX_NTHREADS 256
#define NCCL_SIMPLE_MAX_NTHREADS NCCL_MAX_NTHREADS
#define NCCL_LL_MAX_NTHREADS NCCL_MAX_NTHREADS
#define NCCL_LL_LINES_PER_THREAD 8
#ifdef TEST_LL_CLEANUP
#define NCCL_LL_CLEAN_MASK 0x078 // Set to 0x100 to disable cleanup
#define NCCL_LL_FLAG_MAX   0x100
#define NCCL_LL_FLAG(a) ((uint32_t)((a) % NCCL_LL_FLAG_MAX))
#else
#define NCCL_LL_CLEAN_MASK 0x7ffffff8
#define NCCL_LL_FLAG(a) ((uint32_t)(a))
#endif
// Make sure the clean mask will last for at least NCCL_NSTEPS
static_assert(NCCL_LL_CLEAN_MASK % NCCL_STEPS == 0, "Invalid NCCL_LL_CLEAN_MASK value");

#define NCCL_LL128_LINESIZE 128
#define NCCL_LL128_LINEELEMS (NCCL_LL128_LINESIZE/sizeof(uint64_t))
#define NCCL_LL128_DATAELEMS (NCCL_LL128_LINEELEMS-1)

#define NCCL_LL128_MAX_NTHREADS 256
#define NCCL_LL128_ELEMS_PER_THREAD 120

// Receiving from up to 3 sources is more compute intensive than sending
// to 3 dests. Use 70% for reduce and 30% for bcast.
#define NCCL_LL128_SPLIT(nt) ((nt*7/(10*32))*32)

#define NCCL_LL128_SHMEM_ELEMS_PER_THREAD 2
#define NCCL_LL128_SHMEM_SIZE (NCCL_LL128_SHMEM_ELEMS_PER_THREAD*NCCL_LL128_MAX_NTHREADS)

#define NCCL_DIRECT_WRITE 0x01
#define NCCL_DIRECT_READ  0x02
#define NCCL_DIRECT_NIC   0x04
#define NCCL_IPC_WRITE    0x08
#define NCCL_IPC_READ     0x10

struct ncclConnInfo {
  // Regular comm mechanism
  char *buffs[NCCL_NUM_PROTOCOLS]; // Local for recv, remote for send
  uint64_t *tail;     // Local for recv, remote for send
  uint64_t *head;     // Local for send, remote for recv

  int direct;         // Direct communication
  int shared;         // Buffers are shared
  void **ptrExchange; // Pointer exchange for direct communication
  uint64_t* redOpArgExchange; // PreOp scaler exchange for direct pull case

  int *sizesFifo;     // Sizes fifo from GPU to proxy
  void* *ptrsFifo;      // Buffer fifo from proxy to GPU

  uint64_t step;      // Keep where we are
  uint64_t llLastCleaning;

  // GPU's HDP_MEM_FLUSH_ADDR: HDP Memory Coherency Flush Control. This register
  // allows software to explicitly initiate a flush read to HDP memory. See more
  // descriptions in primitives.h.
  uint32_t* next_hdp_reg;  // Next GPU in ring (for p2p transport use only)
  uint32_t* curr_hdp_reg;  // Current GPU's HDP register
};

struct ncclConnector {
  int connected;
  struct ncclProxyArgs *proxyAppend;
  struct ncclProxyArgs **proxyAppendPtr;
  struct ncclTransportComm* transportComm;
  void* transportResources;
  struct ncclConnInfo conn;
  struct ncclComm *comm;
};

struct ncclRing {
  // Shortcuts for userRanks[1] and userRanks[n-1]
  int prev;
  int next;

  // Maps an internal nccl index to user-specified rank order. This is necessary
  // since we need to know how the user expects data to be ordered across
  // devices. Ordered from current device.
  int* userRanks;
  int* devUserRanks;

  int index; // This rank's index in the ring
};


#define NCCL_MAX_TREE_ARITY 3
struct ncclTree {
  int depth;
  int up;
  int down[NCCL_MAX_TREE_ARITY];
};

#define NCCL_MAX_DIRECT_ARITY 7
struct ncclDirect {
  int depth;
  int out;
  int nHeads;
  int headRank;
  int shift;
  int up[NCCL_MAX_DIRECT_ARITY];
  int down[NCCL_MAX_DIRECT_ARITY];
};

#define NCCL_CONN_IDX_P2P_NET 2
#define NCCL_MAX_CONNS 3
struct ncclPeer {
  struct ncclConnector send[NCCL_MAX_CONNS];
  struct ncclConnector recv[NCCL_MAX_CONNS];
};

struct ncclDevComm;

#pragma pack(push)  /* push current alignment to stack */
#pragma pack(8)     /* set alignment to 8 bytes boundary */
#define NCCL_MAX_WORK_ELEMENTS 1
#define NCCL_MAX_GROUPS (NCCL_MAX_NTHREADS/WARP_SIZE)

/* ncclWork is to be a power of two, currently 8x64 bytes, */
/* to make sure reads to host from the CUDA kernel are aligned. */
/* Make sure to adjust padding at the end of ncclWorkElem. */
struct ncclWorkElem {
  // Header
  struct ncclDevComm* comm;
  uint16_t nThreads;
  uint16_t funcIndex;
  uint8_t regUsed;
  uint8_t direct;
  uint8_t active, redOpArgIsPtr;

  const void * sendbuff;
  void * recvbuff;

  // Op-specific fields.
  union {
    struct {
      size_t count;
      size_t lastChunkSize;
      uint64_t redOpArg;
      uint16_t root;
      uint8_t bid;
      uint8_t nChannels;
      uint16_t connIndex;
      uint16_t opCount;
    } coll;
    struct {
      size_t sendCount;
      size_t recvCount;
      int sendChunkSize;
      int recvChunkSize;
      int32_t delta;
      union {
        struct {
          uint16_t nThreads:12;
          uint16_t sendIdx:2;
          uint16_t recvIdx:2;
        };
        uint16_t padding;
      };
      uint16_t opCount;
    } p2p;
    // [RCCL] Clique-based arguments
    //        NOTE: Follows same field structure as coll
    //              because nChannels is accessed from "coll" struct.
    struct {
      size_t count;
      cliqueDevicePtrs_t* ptrs;
      uint64_t unused_1;
      uint16_t unused_2;
      uint8_t bid;
      uint8_t nChannels;
    } clique;
    // [/RCCL]
    uint64_t align[4];
  };
};
static_assert(sizeof(struct ncclWorkElem) == (0x10*sizeof(int)), "ncclWorkElem must have a pow2 size");

struct ncclWorkRegElem {
  struct ncclWorkElem elem;
  void* dnInputs[NCCL_MAX_DIRECT_ARITY+1];
  void* dnOutputs[NCCL_MAX_DIRECT_ARITY+1];
  void* upOutputs[NCCL_MAX_DIRECT_ARITY+1];
};
#define NCCL_REG_ELEM_FACTOR 4
static_assert(sizeof(struct ncclWorkRegElem) == (NCCL_REG_ELEM_FACTOR*sizeof(struct ncclWorkElem)), "ncclWorkRegElem size must be pow2 times ncclWorkElem size");

struct ncclWork {
  union {
    struct ncclWorkElem elems[NCCL_MAX_WORK_ELEMENTS];
    struct ncclWorkRegElem regElems[NCCL_MAX_WORK_ELEMENTS/NCCL_REG_ELEM_FACTOR];
  };
};

struct ncclChannel {
  union {
    struct {
      struct ncclRing ring;
      struct ncclTree tree;
      struct ncclDirect collTree;

      int id;

      // Communication structures
      struct ncclPeer* peers;
      struct ncclPeer* devPeers;

      // Operation list for aggregation
      struct ncclWork* workFifo;
      int workCount;
      size_t totalSize;
      uint64_t workFifoTail; // Only used by CPU

#ifdef ENABLE_PROFILING
      struct timeval tvs;
      uint64_t sizes;
      int active_req;
      uint64_t send_byte;
      uint64_t recv_byte;
      float bw_cumulative;
      int bw_count;
#endif
      uint16_t index;        // Only used by GPU

      // GDRCOPY support
      struct ncclWork* workFifoGdr;
      struct ncclWork* workFifoDev;
      void* gdrMemDesc;
    };
    int data[0x80];
  };
};
static_assert(sizeof(struct ncclChannel) == 0x80*sizeof(int), "ncclChannel must have a pow2 size");
#pragma pack(pop)   /* restore original alignment from stack */

#ifdef ENABLE_PROFILING
struct ncclProfElem {
  union {
    struct {
      uint64_t total_cycle;
      uint64_t wait_cycle;      // total wait cycle
      uint64_t wait_send_cycle;
      uint64_t wait_recv_cycle;
      // primtive cycles
      uint64_t send_cycle;
      uint64_t directSend_cycle;
      uint64_t recv_cycle;
      uint64_t directRecv_cycle;
      uint64_t copySend_cycle;
      uint64_t directCopySend_cycle;
      uint64_t recvCopySend_cycle;
      uint64_t directRecvCopySend_cycle;
      uint64_t recvReduceCopy_cycle;
      uint64_t recvReduceSend_cycle;
      uint64_t recvReduceCopySend_cycle;
      uint64_t directRecvReduceCopySend_cycle;
      // primitive bytes
      uint64_t send_byte;
      uint64_t directSend_byte;
      uint64_t recv_byte;
      uint64_t directRecv_byte;
      uint64_t copySend_byte;
      uint64_t directCopySend_byte;
      uint64_t recvCopySend_byte;
      uint64_t directRecvCopySend_byte;
      uint64_t recvReduceCopy_byte;
      uint64_t recvReduceSend_byte;
      uint64_t recvReduceCopySend_byte;
      uint64_t directRecvReduceCopySend_byte;
    };
    int data[0x80];
  };
};

struct ncclProf {
  struct ncclProfElem elems[MAXCHANNELS];
};
#endif

#ifdef ENABLE_COLLTRACE
typedef enum {
  ncclCollTraceNotReady,
  ncclCollTraceKernelLaunchType,
  ncclCollTraceKernelEndType,
  ncclCollTraceCollEndType,
  ncclCollTraceAbortType,
  ncclCollTraceDataType
} ncclCollTraceDataType_t;

struct ncclCollTrace {
  uint8_t type;
  uint8_t bid;
  int16_t funcIndex;
  uint32_t data_0;
  uint64_t timeStamp;
  uint64_t opCount;
  union {
    uint64_t data_1;
    struct {
      uint16_t nThreads;
      uint8_t bid;
      uint8_t nChannels;
    } coll;
    struct {
      uint16_t nThreads;
      uint16_t delta;
    } p2p;
  };
};
static_assert(sizeof(struct ncclCollTrace) == 8*sizeof(int), "ncclCollTrace must have a pow2 size");

#define COLLTRACE_NUM_ITEMS 8192
#endif

struct ncclDevComm {
  int rank;
  int nRanks;
  int buffSizes[NCCL_NUM_PROTOCOLS];

  // Flag to ask NCCL kernels to abort
  volatile uint32_t *abortFlag;

  // Channels, device side
  struct ncclChannel* channels;

#ifdef ENABLE_PROFILING
  // Profiling counters
  struct ncclProf* devProf;
#endif

#ifdef ENABLE_COLLTRACE
  struct ncclCollTrace* collTrace;
  uint32_t collTraceHead, *collTraceTail;
  pthread_t collTraceThread;
  bool collTraceExit;
#endif
};

struct ncclDevCommAndChannels {
  ncclDevComm comm;
  ncclChannel channels[MAXCHANNELS];
};

#endif
