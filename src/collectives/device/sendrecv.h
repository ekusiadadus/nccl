/*************************************************************************
 * Copyright (c) 2015-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "devcomm.h"
#include "collectives.h"
#include "primitives.h"

template<typename T, typename RedOp>
struct RunWork<ncclFuncSendRecv, T, RedOp, NCCL_ALGO_RING, NCCL_PROTO_SIMPLE> {
  __device__ __forceinline__ void runSend(const int tid, const int nthreads, const int group, struct ncclWorkElemP2p* args) {
    void* buff = reinterpret_cast<void*>(uintptr_t(args->buffHi32)<<32 | args->buffLo32);
    size_t count = reinterpret_cast<size_t>(size_t(args->countHi32)<<32 | args->countLo32);
    if (args->peer == ncclShmem.comm.rank) {
      struct ncclWorkElemP2p* recvArgs = args-1;
      void* recvBuff = reinterpret_cast<void*>(uintptr_t(recvArgs->buffHi32)<<32 | recvArgs->buffLo32);
      if (buff != recvBuff) {
        ReduceOrCopyMulti<COLL_UNROLL, RedOp, T, 1, 1, 1, 1, 0>(tid, nthreads, nullptr, false, 1, (const T**)&buff, 1, (T**)&recvBuff, count);
      }
    } else {
      using Proto = ProtoSimple<1, 1>;
      int const chunkSize = args->chunkSize/sizeof(T);
      int const peer = args->peer;
      Primitives<T, RedOp, FanAsymmetric<0, 1>, 1, Proto, 1> prims
        (tid, nthreads, nullptr, &peer, buff, nullptr, /*redOpArg(ignored)=*/0, group);
      size_t offset = 0;
      do {
        int nelem = min(size_t(chunkSize), count-offset);
        prims.directSend(offset, offset, nelem);
        offset += nelem;
      } while(offset < count);
    }
  }

  __device__ __forceinline__ void runRecv(const int tid, const int nthreads, const int group, struct ncclWorkElemP2p* args) {
    if (args->peer != ncclShmem.comm.rank) {
      using Proto = ProtoSimple<1, 1>;
      void* buff = reinterpret_cast<void*>(uintptr_t(args->buffHi32)<<32 | args->buffLo32);
      ssize_t count = reinterpret_cast<size_t>(size_t(args->countHi32)<<32 | args->countLo32);
      int const chunkSize = args->chunkSize/sizeof(T);
      int const peer = args->peer;
      Primitives<T, RedOp, FanAsymmetric<1, 0>, 1, Proto, 1> prims
        (tid, nthreads, &peer, nullptr, nullptr, buff, /*redOpArg(ignored)=*/0, group);
      size_t offset = 0;
      do {
        int nelem = min(size_t(chunkSize), count-offset);
        prims.directRecv(offset, nelem);
        offset += nelem;
      } while(offset < count);
    }
  }

  __device__ __forceinline__ void run(ncclWork *work) {
    struct ncclWorkElemP2p* args = work->p2pElems;
    int ngroups = args->ngroups;
    int tid = threadIdx.x;
    int wid = tid / WARP_SIZE;
    // This has to work even for groups of 2.5 warps (which is 8 groups, and means 3
    // warps for send, 2 warps for recv).
    // warpStarts were rounded thanks to int division, but for group number we need to round the other way around
    // So we mirror wid then mirror again the group.
    #define NWARPS (NCCL_MAX_NTHREADS/WARP_SIZE)
    int group = ngroups-1- (NWARPS-1-wid) * ngroups / NWARPS;
    args += group;
    tid -= args->warpStart * WARP_SIZE;
    int nthreads = args->nWarps * WARP_SIZE;
    group |= 1<<16; // Used to select connIndex 1

    if (args->p2pType == ncclWorkP2pTypeUnused) return;
    if (tid >= nthreads || args->peer == -1) return;
    if ((group%2) == 0) {
      runRecv(tid, nthreads, group, args);
    } else {
      runSend(tid, nthreads, group, args);
    }
  }
};
