/*
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "3rdparty/INIReader.h"
#include "examples/cpp/multi_gpu_gpt/gpt_example_utils.h"
#include "src/fastertransformer/models/multi_gpu_gpt/ParallelGpt.h"
#include "src/fastertransformer/utils/mpi_utils.h"
#include "src/fastertransformer/utils/nvtx_utils.h"

#include <cuda_profiler_api.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <vector>

#ifdef USE_NVTX
bool NVTX_ON = true;
#endif

using namespace fastertransformer;

template<typename T>
void multi_gpu_gpt_example(const INIReader reader);

int main(int argc, char* argv[])
{
    MPICHECK(MPI_Init(&argc, &argv));
    srand(0);

    std::string ini_name;
    if (argc == 2) {
        ini_name = std::string(argv[1]);
    }
    else {
        ini_name = "../examples/cpp/multi_gpu_gpt/gpt_config.ini";
    }

    INIReader reader = INIReader(ini_name);
    if (reader.ParseError() < 0) {
        std::cout << "[ERROR] Can't load '" << ini_name << "'\n";
        return -1;
    }
    const int is_half = reader.GetInteger("ft_instance_hyperparameter", "is_half");

    if (is_half == 0)
        multi_gpu_gpt_example<float>(reader);
    else if (is_half == 1)
        multi_gpu_gpt_example<half>(reader);
    else {
        printf("[ERROR] is_fp16 should be 0 (use float) or 1 (use half). \n");
        return -1;
    }
    MPI_Finalize();
    return 0;
}

template<typename T>
void multi_gpu_gpt_example(const INIReader reader)
{
    const std::string model_name = reader.Get("ft_instance_hyperparameter", "model_name");
    const size_t max_batch_size = (size_t)reader.GetInteger("ft_instance_hyperparameter", "max_batch_size");
    const size_t max_seq_len = (size_t)reader.GetInteger("ft_instance_hyperparameter", "max_seq_len");
    const size_t beam_width = (size_t)reader.GetInteger("ft_instance_hyperparameter", "beam_width");
    const int top_k = reader.GetInteger("ft_instance_hyperparameter", "top_k");
    const float top_p = reader.GetFloat("ft_instance_hyperparameter", "top_p");
    const float temperature = reader.GetFloat("ft_instance_hyperparameter", "temperature");
    const float repetition_penalty = reader.GetFloat("ft_instance_hyperparameter", "repetition_penalty");
    const std::string model_dir = std::string(reader.Get("ft_instance_hyperparameter", "model_dir"));

    const int tensor_para_size = reader.GetInteger("ft_instance_hyperparameter", "tensor_para_size");
    const int pipeline_para_size = reader.GetInteger("ft_instance_hyperparameter", "pipeline_para_size");

    const size_t head_num = (size_t)reader.GetInteger(model_name, "head_num");
    const size_t size_per_head = (size_t)reader.GetInteger(model_name, "size_per_head");
    const size_t vocab_size = (size_t)reader.GetInteger(model_name, "vocab_size");
    const size_t decoder_layers = (size_t)reader.GetInteger(model_name, "decoder_layers");
    const size_t hidden_units = head_num * size_per_head;
    const size_t inter_size = 4 * hidden_units;

    const size_t request_batch_size = reader.GetInteger("request", "request_batch_size");
    // The length of tokens we hope this model to generate
    const int request_output_len = reader.GetInteger("request", "request_output_len");

    const int start_id = 50256;
    const int end_id = 50256;

    FT_CHECK(head_num % tensor_para_size == 0);
    FT_CHECK(decoder_layers % pipeline_para_size == 0);

    // Prepare the parallelism parameters
    int rank, world_size, device, device_count;
    MPICHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
    MPICHECK(MPI_Comm_size(MPI_COMM_WORLD, &world_size));
    if (rank == 0)
        printf("Total ranks: %d.\n", world_size);
    check_cuda_error(cudaGetDeviceCount(&device_count));
    check_cuda_error(cudaSetDevice(rank % device_count));
    check_cuda_error(cudaGetDevice(&device));

    struct cudaDeviceProp prop;
    check_cuda_error(cudaGetDeviceProperties(&prop, device));
    printf("Device %s\n", prop.name);

    printf("P%d is runing with %d GPU.\n", rank, device);

    if (tensor_para_size * pipeline_para_size != world_size) {
        printf("[ERROR] tensor_para_size * pipeline_para_size should equal to world_size \n");
        exit(-1);
    }

    const int tensor_para_rank = rank % tensor_para_size;
    const int pipeline_para_rank = rank / tensor_para_size;
    const int layers_per_group = decoder_layers / pipeline_para_size;
    if (layers_per_group * pipeline_para_size != (int)decoder_layers) {
        printf("[ERROR] layers_per_group (%d) * pipeline_para_size (%d) should equal to decoder_layers (%ld) \n",
               layers_per_group,
               pipeline_para_size,
               decoder_layers);
        exit(-1);
    }

    // assume gpu_num = k * n,
    // tensor parallelism group size is n
    // pipeline parallelism group size is k

    // convert WORLD communicator into 2D grid (k * n) communicator
    // comms of the same row means they are in the same tensor parallel group
    // comms of the same col means they are in the same pipeline parallel group
    MPI_Comm grid_comm;
    int dims[2] = {pipeline_para_size, tensor_para_size};
    int periods[2] = {0, 0};
    MPI_Cart_create(MPI_COMM_WORLD, 2, dims, periods, 0, &grid_comm);

    MPI_Comm comm_tensor_parallel, comm_pipeline_parallel;

    int remain_dims_tensor_parallel[2] = {false, true};
    int remain_dims_pipeline_parallel[2] = {true, false};
    // split 2D communicator into rows and cols, each row = one tensor parallel group, each col = one pipeline parallel
    // group
    MPI_Cart_sub(grid_comm, remain_dims_tensor_parallel, &comm_tensor_parallel);
    MPI_Cart_sub(grid_comm, remain_dims_pipeline_parallel, &comm_pipeline_parallel);

    int rank_tensor_parallel, rank_pipeline_parallel;
    MPI_Comm_rank(comm_tensor_parallel, &rank_tensor_parallel);
    MPI_Comm_rank(comm_pipeline_parallel, &rank_pipeline_parallel);

    ncclUniqueId tensor_para_nccl_uid;
    ncclUniqueId pipeline_para_nccl_uid;
    // root of tensor parallel group and pipeline parallel group creates the nccl uid
    if (rank_tensor_parallel == 0) {
        NCCLCHECK(ncclGetUniqueId(&tensor_para_nccl_uid));
    }

    if (rank_pipeline_parallel == 0) {
        NCCLCHECK(ncclGetUniqueId(&pipeline_para_nccl_uid));
    }
    // broadcast nccl uid to the comms in the same tensor parallel group or pipeline parallel group
    MPI_Bcast(&tensor_para_nccl_uid, sizeof(tensor_para_nccl_uid), MPI_BYTE, 0, comm_tensor_parallel);
    MPI_Bcast(&pipeline_para_nccl_uid, sizeof(pipeline_para_nccl_uid), MPI_BYTE, 0, comm_pipeline_parallel);

    ncclComm_t tensor_para_nccl_comm, pipeline_para_nccl_comm;
    NCCLCHECK(ncclCommInitRank(&tensor_para_nccl_comm, tensor_para_size, tensor_para_nccl_uid, tensor_para_rank));
    NCCLCHECK(
        ncclCommInitRank(&pipeline_para_nccl_comm, pipeline_para_size, pipeline_para_nccl_uid, pipeline_para_rank));

    // Read ids of request from file.
    int max_input_len = -1;
    std::vector<int> v_start_lengths;
    std::vector<int> v_start_ids;
    read_start_ids(request_batch_size,
                   &v_start_lengths,
                   &v_start_ids,
                   max_input_len,
                   end_id,
                   beam_width,
                   "../examples/cpp/multi_gpu_gpt/start_ids.csv");

    int* d_input_ids;
    int* d_input_lengths;
    if (max_input_len == 0) {
        // unconditional case, no input ids, so do nothing.
        d_input_ids = nullptr;
        d_input_lengths = nullptr;
        max_input_len = 0;
    }
    else {
        // conditional case.
        deviceMalloc(&d_input_ids, request_batch_size * beam_width * max_input_len, false);
        deviceMalloc(&d_input_lengths, request_batch_size * beam_width, false);
        cudaH2Dcpy(d_input_ids, v_start_ids.data(), request_batch_size * beam_width * max_input_len);
        cudaH2Dcpy(d_input_lengths, v_start_lengths.data(), request_batch_size * beam_width);
    }

    const int total_output_len = max_input_len + request_output_len;
    if (total_output_len > (int)max_seq_len) {
        printf("[ERROR] total_output_len (%d) should be <= max_seq_len (%ld). \n", total_output_len, max_seq_len);
        exit(-1);
    }

    cudaStream_t stream;
    cublasHandle_t cublas_handle;
    cublasLtHandle_t cublaslt_handle;
    cudaStreamCreate(&stream);
    cublasCreate(&cublas_handle);
    cublasLtCreate(&cublaslt_handle);
    cublasSetStream(cublas_handle, stream);
    cublasAlgoMap* cublas_algo_map = new cublasAlgoMap("gemm_config.in");

    Allocator<AllocatorType::CUDA> allocator(getDevice());

    std::mutex* cublas_wrapper_mutex = new std::mutex();
    cublasMMWrapper cublas_wrapper =
        cublasMMWrapper(cublas_handle, cublaslt_handle, stream, cublas_algo_map, cublas_wrapper_mutex, &allocator);
    if (std::is_same<T, half>::value) {
        cublas_wrapper.setGemmConfig(CUDA_R_16F, CUDA_R_16F, CUDA_R_16F, CUDA_R_32F);
    }
    else if (std::is_same<T, float>::value) {
        cublas_wrapper.setFP32GemmConfig();
    }

    fastertransformer::ParallelGptWeight<T> gpt_weights(hidden_units,
                                                        inter_size,
                                                        vocab_size,
                                                        decoder_layers,
                                                        max_seq_len,
                                                        tensor_para_size,
                                                        tensor_para_rank,
                                                        pipeline_para_size,
                                                        pipeline_para_rank);
    gpt_weights.loadModel(model_dir);
    unsigned long long random_seed;
    if (rank == 0) {
        random_seed = (unsigned long long)(0);
    }
    if (world_size > 1) {
        MPICHECK(MPI_Bcast(&random_seed, 1, MPI_UNSIGNED_LONG_LONG, 0, MPI_COMM_WORLD));
    }
    ParallelGpt<T> gpt = ParallelGpt<T>(0,  // max_batch_size, FT will adjust the buffer automatically.
                                        0,  // max_seq_len, FT will adjust the buffer automatically.
                                        0,  // max_input_len, FT will adjust the buffer automatically.
                                        beam_width,
                                        head_num,
                                        size_per_head,
                                        inter_size,
                                        decoder_layers,
                                        vocab_size,
                                        start_id,
                                        end_id,
                                        0.0f,
                                        top_k,
                                        top_p,
                                        random_seed,
                                        temperature,
                                        1.0f,  // len_penalty,
                                        repetition_penalty,
                                        tensor_para_size,
                                        tensor_para_rank,
                                        tensor_para_nccl_comm,
                                        pipeline_para_size,
                                        pipeline_para_rank,
                                        pipeline_para_nccl_comm,
                                        stream,
                                        &cublas_wrapper,
                                        &allocator,
                                        false,
                                        &prop);

    int* d_output_ids;
    int* d_parent_ids;
    int* d_sequence_lengths;
    deviceMalloc(&d_output_ids, request_batch_size * beam_width * total_output_len, false);
    deviceMalloc(&d_parent_ids, request_batch_size * beam_width * total_output_len, false);
    deviceMalloc(&d_sequence_lengths, request_batch_size * beam_width, false);

    std::vector<Tensor> input_tensors = std::vector<Tensor>{
        Tensor{MEMORY_GPU,
               TYPE_INT32,
               std::vector<size_t>{request_batch_size * beam_width, (size_t)max_input_len},
               d_input_ids},
        Tensor{MEMORY_GPU, TYPE_INT32, std::vector<size_t>{request_batch_size * beam_width}, d_input_lengths},
        Tensor{MEMORY_CPU, TYPE_INT32, std::vector<size_t>{1}, &total_output_len}};

    std::vector<Tensor> output_tensors = std::vector<Tensor>{
        Tensor{MEMORY_GPU,
               TYPE_INT32,
               std::vector<size_t>{request_batch_size, beam_width, (size_t)total_output_len},
               d_output_ids},
        Tensor{MEMORY_GPU,
               TYPE_INT32,
               std::vector<size_t>{(size_t)total_output_len, request_batch_size, beam_width},
               d_parent_ids},
        Tensor{MEMORY_GPU, TYPE_INT32, std::vector<size_t>{request_batch_size, beam_width}, d_sequence_lengths},
        Tensor{MEMORY_GPU,
               TYPE_FP32,
               std::vector<size_t>{(size_t)request_output_len, request_batch_size, beam_width},
               nullptr}};

    print_mem_usage();

    int ite = 1;
    cudaDeviceSynchronize();
    MPI_Barrier(MPI_COMM_WORLD);

    cudaProfilerStart();
    // warm up
    ite = 1;
    nvtx::setScope("warmup_time");
    PUSH_RANGE("warmup time")
    for (int i = 0; i < ite; ++i) {
        gpt.forward(&output_tensors, &input_tensors, &gpt_weights);
    }
    cudaDeviceSynchronize();
    MPI_Barrier(MPI_COMM_WORLD);

    POP_RANGE;
    nvtx::resetScope();

    if (rank == 0) {

        std::string fName = "out";
        auto outFile = std::ofstream(fName, std::ios::out);
        if (!outFile.is_open()) {
            printf("[WARNING] Cannot write results into output file %s \n", fName.c_str());
        }
        else {
            size_t outCount = total_output_len * request_batch_size * beam_width;
            int* hBuf = new int[outCount];
            cudaD2Hcpy(hBuf, d_output_ids, outCount);

            {
                std::cout << "Writing " << outCount << " elements\n";
                int zeroCount = 0;
                for (size_t i = 0; i < outCount; i++) {
                    if (hBuf[i] == int(0))
                        zeroCount++;
                    outFile << hBuf[i] << " ";
                    if ((i + 1) % (total_output_len) == 0)
                        outFile << std::endl;

                    if (i < 10)
                        printf("%5d ", hBuf[i]);
                    if ((i + 1) % (total_output_len) == 0 && i < 10)
                        std::cout << std::endl;
                }
                std::cout << std::endl << "zeroCount = " << zeroCount << std::endl;
            }
            delete[] hBuf;
        }
    }

    // test time
    struct timeval start, end;
    MPI_Barrier(MPI_COMM_WORLD);
    cudaDeviceSynchronize();
    gettimeofday(&start, NULL);

    nvtx::setScope("total_time");
    PUSH_RANGE("total time")
    for (int i = 0; i < ite; ++i) {
        gpt.forward(&output_tensors, &input_tensors, &gpt_weights);
    }

    cudaDeviceSynchronize();
    MPI_Barrier(MPI_COMM_WORLD);

    POP_RANGE;
    nvtx::resetScope();
    gettimeofday(&end, NULL);

    cudaProfilerStop();

    printf("[INFO] request_batch_size %ld beam_width %ld head_num %ld size_per_head %ld total_output_len %d"
           " decoder_layers %ld vocab_size %ld FT-CPP-decoding-beamsearch-time %.2f ms\n",
           request_batch_size,
           beam_width,
           head_num,
           size_per_head,
           total_output_len,
           decoder_layers,
           vocab_size,
           ((end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) * 0.001) / ite);

    ncclCommDestroy(tensor_para_nccl_comm);
    ncclCommDestroy(pipeline_para_nccl_comm);

    delete cublas_algo_map;
    delete cublas_wrapper_mutex;
    return;
}