// Copyright 2016-2020 NVIDIA
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//        http://www.apache.org/licenses/LICENSE-2.0
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.


#include <iostream>
#include <string>
#include <algorithm>
#include <chrono>

#include <Core/Cuda/Types.h>
#include <Interpreters/Cuda/CudaStringsAggregator.h>
#include <AggregateFunctions/Cuda/ICudaAggregateFunction.h>
#include <AggregateFunctions/Cuda/createAggregateFunction.h>

//#include "hash_sort_alg_magic_numbers.h"
#include "StringGenerator/dealer.h"

using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

using namespace DB;

CudaAggregateFunctionPtr    createAggregateFunction(const String &name)
{
    if (name == "COUNT")
        return createCudaAggregateFunctionCount();
    else if (name == "uniqHLL12") 
        return createCudaAggregateFunctionUniq();
    else 
        throw std::runtime_error("createAggregateFunction: unknown function name " + name);
}

bool compare_results(CudaAggregateFunctionPtr    agg_function,
                     const std::unordered_map<std::string,CudaAggregateDataPtr> &res1,
                     const std::unordered_map<std::string,CudaAggregateDataPtr> &res2)
{
    if (res1.size() != res2.size()) return false;
    for (auto it : res1) 
    {
        auto it2 = res2.find(it.first);
        if (it2 == res2.end()) return false;
        if (agg_function->getResult(it.second) != agg_function->getResult(it2->second)) {
            printf("differ: %s %s %d %d\n", it.first.c_str(), it2->first.c_str(), 
                agg_function->getResult(it.second), agg_function->getResult(it2->second));
            return false;
        }
    }
    return true;
}

int main(int argc, char const *argv[])
{
    try {

    if (argc != 12) {
        std::cerr << std::endl << "USAGE: " << argv[0] << " <dev_number> <chunks_num> <buffer_max_str_num> <buffer_max_size> " << 
        "<hash_table_max_size> <hash_table_str_buffer_max_size> <memcpy_threads_num> <function_name> <strings_file_name>" << std::endl;
        std::cerr << "WHERE:" << std::endl;
        std::cerr << "        <dev_number> - cuda device number (as passed to cudaSetDevice, not bus id)" << std::endl;
        std::cerr << "        <chunks_num> - number of cuda streams (in fact, the only thing matters is whether <chunks_num> > 1)" << std::endl;
        std::cerr << "        <buffer_max_str_num> - maximum number of strings in one 'packet' of strings passed to gpu" << std::endl;
        std::cerr << "        <buffer_max_size> - maximum total length of strings in one 'packet' of strings passed to gpu" << std::endl;
        std::cerr << "        <hash_table_max_size> - gpu hash table size; if exeeded (i.e. number of uniq keys is greater then this parameter) " << std::endl <<
                     "        causes program to fail" << std::endl;
        std::cerr << "        <hash_table_str_buffer_max_size> - gpu hash table maximim total length of all keys; if exeeded (i.e. total length of uniq " << std::endl <<
                     "        keys is greater then this parameter) causes program to fail" << std::endl;
        std::cerr << "        <memcpy_threads_num> - number of threads used when performing memcpy from one host buffer to another" << std::endl;
        std::cerr << "        <function_name> - name of aggreagte function: COUNT or uniqHLL12" << std::endl;
        std::cerr << "        <strings_file_name> - file with strings generated by generator" << std::endl;
        std::cerr << "        <results_file_name> - file to output results (string_key value); use none to ommit output" << std::endl;
        std::cerr << "        <perform_naive_check> - whether to make naive aggregation on cpu to check results (1 - to do, 0 - not to do)" << std::endl;
        std::cerr << std::endl << "Example: " << argv[0] << " 0 2 2097152 134217728 16384 262144 1 uniqHLL12 test_save.dat" << std::endl << std::endl;
        return 2; 
    }
    int             dev_number = std::stoi(argv[1]); 
    size_t          chunks_num = std::stoi(argv[2]);
    UInt32          buffer_max_str_num = std::stoi(argv[3]), 
                    buffer_max_size = std::stoi(argv[4]);
    UInt32          hash_table_max_size = std::stoi(argv[5]), 
                    hash_table_str_buffer_max_size = std::stoi(argv[6]);
    size_t          memcpy_threads_num = std::stoi(argv[7]);
    std::string     function_name(argv[8]);
    std::string     fn(argv[9]);
    std::string     res_fn(argv[10]);
    bool            perform_naive_check = std::stoi(argv[11]);

    if ((function_name != "COUNT")&&(function_name != "uniqHLL12")) 
        throw std::runtime_error("unknown function name: " + function_name);

    size_t                  str_num = 0;
    char                    *buffer_keys, *buffer_texts;
    unsigned int            *buffer_keys_length, *buffer_texts_length; 
    unsigned long int       *buffer_keys_position, *buffer_texts_position;
    unsigned long int       *buffer_permutation_indexes;

    std::vector<UInt32>     lens, offsets, lens_vals, offsets_vals;
    std::vector<UInt64>     offsets64, offsets64_vals;

    std::cout << "reading buffers from file " << fn << "..." << std::endl;
    dealer                  gen(fn.c_str(), buffer_keys, buffer_keys_length, 
                                buffer_keys_position, buffer_texts, buffer_texts_length, 
                                buffer_texts_position, buffer_permutation_indexes);
    size_t                  buffer_permutation_size = buffer_permutation_indexes[0];
    std::cout << "done" << std::endl;

    std::cout << "calculate lengths and offsets..." << std::endl;
    char                    *buffer_keys_curr = buffer_keys,
                            *buffer_texts_curr = buffer_texts;
    while (buffer_keys_curr != buffer_keys + gen.get_mem_keys()) 
    {
        UInt32   offset = buffer_keys_curr - buffer_keys,
                 offset_val = buffer_texts_curr - buffer_texts;
        if (offsets.size() > 0) 
        {
            lens.push_back( offset - offsets.back() );
            lens_vals.push_back( offset_val - offsets_vals.back() );
        }
        offsets.push_back( offset );
        offsets_vals.push_back( offset_val );
        str_num++;

        buffer_keys_curr = strchr(buffer_keys_curr, '\0') + 1;
        buffer_texts_curr = strchr(buffer_texts_curr, '\0') + 1;
    }
    UInt32  offset = buffer_keys_curr - buffer_keys,
            offset_val = buffer_texts_curr - buffer_texts;
    if (offsets.size() > 0) 
    {
        lens.push_back( offset - offsets.back() );
        lens_vals.push_back( offset_val - offsets_vals.back() );
    }
    offsets.push_back( offset );  //append offset of 'end' (non-existing) string
    offsets_vals.push_back( offset_val );  //append offset of 'end' (non-existing) string

    std::cout << "basic buffer strings number(keys) = " << lens.size() << " total_buf_sz(keys) = " << offsets.back() << std::endl;
    std::cout << "basic buffer strings number(vals) = " << lens_vals.size() << " total_buf_sz(vals) = " << offsets_vals.back() << std::endl;
    if (buffer_permutation_size == 0) 
    {
        buffer_permutation_size = 1;
        delete []buffer_permutation_indexes;
        buffer_permutation_indexes = new unsigned long int[3];
        buffer_permutation_indexes[0] = buffer_permutation_size;
        buffer_permutation_indexes[1] = 0;
        buffer_permutation_indexes[2] = lens.size()-1;
    }
    size_t  total_str_num = 0;
    for (size_t  buffer_permutation_i = 1;buffer_permutation_i < 2*buffer_permutation_size+1;buffer_permutation_i += 2)
        total_str_num += (buffer_permutation_indexes[buffer_permutation_i+1] - buffer_permutation_indexes[buffer_permutation_i] + 1);
    std::cout << "total test rows number(using sampling)= " << total_str_num << std::endl;
    std::cout << "done" << std::endl;

    std::cout << "prepare offsets64 " << std::endl;
    offsets64.resize(offsets.size());
    offsets64_vals.resize(offsets_vals.size());
    for (size_t  buffer_permutation_i = 1;buffer_permutation_i < 2*buffer_permutation_size+1;buffer_permutation_i += 2)
    {
        std::cout << "buffer_permutation_i = " << buffer_permutation_i << std::endl;

        size_t  start = buffer_permutation_indexes[buffer_permutation_i],
                end = buffer_permutation_indexes[buffer_permutation_i+1];
        size_t                  block_str_num = std::min(size_t(buffer_max_str_num/8), size_t((end-start+1)/8));
        for (size_t i = start;i <= end;i += block_str_num) 
        {
            size_t next_i = std::min(i + block_str_num, end+1);

            for (size_t ii = i;ii < next_i;ii++) 
            {
                offsets64[ii] = offsets[ii] - offsets[i];
                offsets64_vals[ii] = offsets_vals[ii] - offsets_vals[i];
            }
        }
    }

    std::cout << "creating aggregation function..." << std::endl;
    CudaAggregateFunctionPtr    agg_function = createAggregateFunction(function_name);
    std::cout << "done" << std::endl;

    //std::unordered_map<std::string,CudaAggregateDataPtr>    agg_result;
    /*if (perform_naive_check) 
    {
        std::cout << "start naive aggregation..." << std::endl;
        auto                                                    host_e1 = steady_clock::now();
        for (size_t  buffer_permutation_i = 1;buffer_permutation_i < 2*buffer_permutation_size+1;buffer_permutation_i += 2)
        {
            size_t  start = buffer_permutation_indexes[buffer_permutation_i],
                    end = buffer_permutation_indexes[buffer_permutation_i+1];
            for (size_t i = start;i <= end;++i) 
            {
                UInt32      offset = offsets[i],
                            offset_val = offsets_vals[i];
                std::string key_str(&(buffer_keys[offset]), strlen(&(buffer_keys[offset]))),
                            val_str(&(buffer_texts[offset_val]), strlen(&(buffer_texts[offset_val])));

                auto it = agg_result.find(key_str);
                if (it == agg_result.end())
                {
                    agg_result[key_str] = (CudaAggregateDataPtr)malloc(agg_function->cudaSizeOfData());
                    agg_function->initAggreagteData( agg_result[key_str] );
                    agg_function->add( agg_result[key_str], val_str );
                }
                else
                {
                    agg_function->add( agg_result[key_str], val_str );
                }
            }
        }
        std::cout << "uniq keys num = " << agg_result.size() << std::endl;
        std::cout << "done" << std::endl;   
        auto                                        host_e2 = steady_clock::now();
        auto                                        host_t = duration_cast<milliseconds>(host_e2 - host_e1);
        std::cout << "naive aggregation time " << host_t.count() << "ms" << std::endl;
    }*/

    std::cout << "creating aggregator class..." << std::endl;
    CudaStringsAggregatorPtr   cuda_aggregator = CudaStringsAggregatorPtr(new CudaStringsAggregator(dev_number, chunks_num, 
        hash_table_max_size, hash_table_str_buffer_max_size, buffer_max_str_num, buffer_max_size, agg_function));
    std::cout << "done" << std::endl;

    std::cout << "start aggregation on GPU..." << std::endl;
    auto                                        cuda_e1 = steady_clock::now();
    cuda_aggregator->startProcessing();
    for (size_t  buffer_permutation_i = 1;buffer_permutation_i < 2*buffer_permutation_size+1;buffer_permutation_i += 2)
    {
        size_t  start = buffer_permutation_indexes[buffer_permutation_i],
                end = buffer_permutation_indexes[buffer_permutation_i+1];
        size_t                  block_str_num = std::min(size_t(buffer_max_str_num/8), size_t((end-start+1)/8));
        for (size_t i = start;i <= end;i += block_str_num) 
        {
            size_t next_i = std::min(i + block_str_num, end+1);
            cuda_aggregator->queueData(next_i - i, 
                offsets[next_i] - offsets[i], &(buffer_keys[offsets[i]]), &(offsets64[i]), 
                offsets_vals[next_i] - offsets_vals[i], &(buffer_texts[offsets_vals[i]]), &(offsets64_vals[i]),
                memcpy_threads_num);
        }
    }
    cuda_aggregator->waitProcessed();
    std::cout << "uniq keys num = " << cuda_aggregator->getResult().size() << std::endl;
    std::cout << "done" << std::endl;
    auto                                        cuda_e2 = steady_clock::now();
    auto                                        cuda_t = duration_cast<milliseconds>(cuda_e2 - cuda_e1);
    std::cout << "cuda aggregation time " << cuda_t.count() << "ms" << std::endl;

    /*if (perform_naive_check)
    {
        if ( compare_results(agg_function, cuda_aggregator->getResult(), agg_result) ) 
        {
            std::cout << "results of naive aggregation and cuda aggregation are equal" << std::endl;
        } 
        else 
        {
            std::cout << "ERROR: results of naive aggregation and cuda aggregation DIFFER!" << std::endl;
        }
    }*/

    if (res_fn != "none") 
    {
        std::ofstream    fout(res_fn.c_str());
        for (const auto &elem : cuda_aggregator->getResult() ) 
        {
            fout << elem.first << " " << agg_function->getResult(elem.second) << std::endl;
        }
    }

    return 0;

    } catch(std::exception &e) {

    std::cout << "ERROR: " << e.what() << std::endl;
    return 1;

    }
}