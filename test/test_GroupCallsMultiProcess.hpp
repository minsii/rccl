/*************************************************************************
 * Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#ifndef TEST_GROUPCALLS_MULTI_PROCESS_HPP
#define TEST_GROUPCALLS_MULTI_PROCESS_HPP

#include "CorrectnessTest.hpp"
#include "test_AllGatherMultiProcess.hpp"
#include "test_AllReduceMultiProcess.hpp"
#include "test_BroadcastMultiProcess.hpp"
#include "test_ReduceMultiProcess.hpp"
#include "test_ReduceScatterMultiProcess.hpp"

#include <string>

namespace CorrectnessTests
{
    class GroupCallsMultiProcessCorrectnessTest : public MultiProcessCorrectnessTest
    {
    public:
        void TestGroupCalls(int process, std::vector<int> const& ranks, std::vector<Dataset*>& datasets, std::vector<ncclFunc_t> const& funcs, bool& pass)
        {
            ncclGroupStart();
            for (int i = 0; i < ranks.size(); i++)
            {
                SetUpPerProcess(ranks[i], funcs, comms[ranks[i]], streams[ranks[i]], datasets);
                if (numDevices > numDevicesAvailable)
                {
                    break;
                }
            }
            ncclGroupEnd();

            if (numDevices > numDevicesAvailable)
            {
                pass = true;
                return;
            }

            int numProcesses = numDevices / ranks.size();
            Barrier barrier(process, numProcesses, StripPortNumberFromCommId(std::string(getenv("NCCL_COMM_ID"))));

            for (int i = 0; i < ranks.size(); i++)
            {
                for (int j = 0; j < datasets.size(); j++)
                {
                    FillDatasetWithPattern(*datasets[j], ranks[i]);
                }
            }

            int const root = 0;

            AllGatherMultiProcessCorrectnessTest::ComputeExpectedResults(*datasets[0], barrier, numDevices, ranks);
            AllReduceMultiProcessCorrectnessTest::ComputeExpectedResults(*datasets[1], barrier, op, ranks);
            BroadcastMultiProcessCorrectnessTest::ComputeExpectedResults(*datasets[2], root, ranks);
            ReduceMultiProcessCorrectnessTest::ComputeExpectedResults(*datasets[3], barrier, op, root, ranks);
            ReduceScatterMultiProcessCorrectnessTest::ComputeExpectedResults(*datasets[4], barrier, op, ranks);
            barrier.Wait();

            size_t const byteCount = datasets[0]->NumBytes() / numDevices;
            size_t const elemCount = numElements / numDevices;

            ncclGroupStart();
            // AllGather
            for (int i = 0; i < ranks.size(); i++)
            {
                int rank = ranks[i];
                ncclAllGather((int8_t *)datasets[0]->inputs[rank] + (rank * byteCount),
                              datasets[0]->outputs[rank], elemCount,
                              dataType, comms[rank], streams[rank]);
            }

            // AllReduce
            for (int i = 0; i < ranks.size(); i++)
            {
                int rank = ranks[i];
                ncclAllReduce(datasets[1]->inputs[rank], datasets[1]->outputs[rank],
                              numElements, dataType, op, comms[rank], streams[rank]);
            }

            // Broadcast
            for (int i = 0; i < ranks.size(); i++)
            {
                int rank = ranks[i];
                ncclBroadcast(datasets[2]->inputs[rank],
                              datasets[2]->outputs[rank],
                              numElements, dataType,
                              root, comms[rank], streams[rank]);
            }

            // Reduce
            for (int i = 0; i < ranks.size(); i++)
            {
                int rank = ranks[i];
                ncclReduce(datasets[3]->inputs[rank],
                           datasets[3]->outputs[rank],
                           numElements, dataType, op,
                           root, comms[rank], streams[rank]);
            }

            // ReduceScatter
            for (int i = 0; i < ranks.size(); i++)
            {
                int rank = ranks[i];
                ncclReduceScatter(datasets[4]->inputs[rank],
                                  (int8_t *)datasets[4]->outputs[rank] + (rank * byteCount),
                                  elemCount, dataType, op,
                                  comms[rank], streams[rank]);
            }

            // Signal end of group call
            ncclGroupEnd();

            for (int i = 0; i < ranks.size(); i++)
            {
                HIP_CALL(hipSetDevice(ranks[i]));
                HIP_CALL(hipStreamSynchronize(streams[ranks[i]]));
            }

            for (int i = 0; i < funcs.size(); i++)
            {
                for (int j = 0; j < ranks.size(); j++)
                {
                    pass = ValidateResults(*datasets[i], ranks[j], root);
                    if (!pass)
                    {
                        break;
                    }
                }
                barrier.Wait();
                for (int j = 0; j < ranks.size(); j++)
                {
                    datasets[i]->Release(ranks[j]);
                }
            }

            for (int i = 0; i < ranks.size(); i++)
            {
                TearDownPerProcess(comms[ranks[i]], streams[ranks[i]]);
            }
        }
    };
}

#endif
