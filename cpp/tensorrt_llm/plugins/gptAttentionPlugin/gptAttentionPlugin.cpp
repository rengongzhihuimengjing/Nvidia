/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2022 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "gptAttentionPlugin.h"
#include "tensorrt_llm/kernels/decoderMaskedMultiheadAttention.h"
#include "tensorrt_llm/kernels/gptKernels.h"
#include "tensorrt_llm/kernels/unfusedAttentionKernels.h"
#include "tensorrt_llm/plugins/common/checkMacrosPlugin.h"
#include "tensorrt_llm/plugins/common/plugin.h"
#include "tensorrt_llm/plugins/gptAttentionCommon/gptAttentionCommon.h"
#include "tensorrt_llm/plugins/gptAttentionCommon/gptAttentionCommonImpl.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>

using namespace nvinfer1;
using namespace tensorrt_llm::kernels;
using tensorrt_llm::plugins::GPTAttentionPluginCreator;
using tensorrt_llm::plugins::GPTAttentionPlugin;

static const char* GPT_ATTENTION_PLUGIN_VERSION{"1"};
static const char* GPT_ATTENTION_PLUGIN_NAME{"GPTAttention"};

GPTAttentionPlugin::GPTAttentionPlugin(int num_heads, int num_kv_heads, int unidirectional, float q_scaling,
    tensorrt_llm::kernels::PositionEmbeddingType position_embedding_type,
    int rotary_embedding_dim, // for RoPE. 0 for non-RoPE
    float rotary_embedding_base, tensorrt_llm::kernels::RotaryScalingType rotary_embedding_scale_type,
    float rotary_embedding_scale, int rotary_embedding_max_positions, int tp_size, int tp_rank, // for ALiBi
    tensorrt_llm::kernels::ContextFMHAType context_fmha_type, bool multi_block_mode, int kv_cache_quant_mode,
    bool remove_input_padding, tensorrt_llm::kernels::AttentionMaskType mask_type, bool paged_kv_cache,
    nvinfer1::DataType type, int32_t max_context_length, bool qkv_bias_enabled)
    : GPTAttentionPluginCommon(num_heads, num_kv_heads, unidirectional, q_scaling, position_embedding_type,
        rotary_embedding_dim, rotary_embedding_base, rotary_embedding_scale_type, rotary_embedding_scale,
        rotary_embedding_max_positions, tp_size, tp_rank, context_fmha_type, multi_block_mode, kv_cache_quant_mode,
        remove_input_padding, mask_type, paged_kv_cache, type, max_context_length, qkv_bias_enabled)
{
}

GPTAttentionPlugin::GPTAttentionPlugin(const void* data, size_t length)
    : GPTAttentionPluginCommon(data, length)
{
}

// IPluginV2DynamicExt Methods
GPTAttentionPlugin* GPTAttentionPlugin::clone() const noexcept
{
    return dynamic_cast<GPTAttentionPlugin*>(this->cloneImpl<GPTAttentionPlugin>());
}

// outputs
//     output_tensor [batch_size, seq_len, local_hidden_size]
//     present_key_value_pool [blocks, 2, local_num_kv_heads, tokens_per_block, head_size] if paged_kv_attention
//                         or [batch_size, 2, local_num_kv_heads, max_seq_len, head_size]
nvinfer1::DimsExprs GPTAttentionPlugin::getOutputDimensions(
    int outputIndex, const nvinfer1::DimsExprs* inputs, int nbInputs, nvinfer1::IExprBuilder& exprBuilder) noexcept
{
    TLLM_CHECK(outputIndex == 0 || outputIndex == 1);
    if (outputIndex == 0)
    {
        auto ret = inputs[getInputTensorIdx()];
        ret.d[2] = exprBuilder.operation(
            DimensionOperation::kPROD, *inputs[getPastKeyValueIdx()].d[4], *exprBuilder.constant(mNumHeads));
        return ret;
    }
    return inputs[getPastKeyValueIdx()];
}

bool GPTAttentionPlugin::supportsFormatCombination(
    int pos, const nvinfer1::PluginTensorDesc* inOut, int nbInputs, int nbOutputs) noexcept
{
    if (pos == getSequenceLengthIdx() || pos == getHostPastKeyValueLengthsIdx() || pos == getContextLengthsIdx()
        || pos == getCacheIndirIdx() || pos == getRequestTypesIdx())
    {
        return inOut[pos].type == nvinfer1::DataType::kINT32;
    }
    else if (mKVCacheQuantMode.hasKvCacheQuant()
        && (pos == getKVCacheDequantizationScaleIdx() || pos == getKVCacheQuantizationScaleIdx()))
    {
        // kv_scale for mType->int8/fp8 and int8/fp8->mType conversion
        return inOut[pos].type == nvinfer1::DataType::kFLOAT && inOut[pos].format == TensorFormat::kLINEAR;
    }
    else if (mPagedKVCache && pos == getKVCacheBlockPointersIdx())
    {
        // pointers to kv cache blocks
        return inOut[pos].type == nvinfer1::DataType::kINT32 && inOut[pos].format == TensorFormat::kLINEAR;
    }
    else if (mKVCacheQuantMode.hasInt8KvCache() && (pos == getPastKeyValueIdx() || pos == nbInputs + 1))
    {
        // If use Int8 K/V cache we require I/O KV values to int8
        return (inOut[pos].type == nvinfer1::DataType::kINT8) && (inOut[pos].format == TensorFormat::kLINEAR);
    }
    else if (mRemovePadding && (pos == getHostContextLengthsIdx()))
    {
        return inOut[pos].type == nvinfer1::DataType::kINT32 && inOut[pos].format == TensorFormat::kLINEAR;
    }
    else
    {
        return (inOut[pos].type == mType) && (inOut[pos].format == TensorFormat::kLINEAR);
    }
    return false;
}

void GPTAttentionPlugin::configurePlugin(const nvinfer1::DynamicPluginTensorDesc* in, int nbInputs,
    const nvinfer1::DynamicPluginTensorDesc* out, int nbOutputs) noexcept
{
    mHeadSize = in[getPastKeyValueIdx()].desc.dims.d[4];
    TLLM_CHECK(mHeadSize > 0);

    // pre-check whether FMHA is supported in order to save memory allocation
    mEnableContextFMHA = mEnableContextFMHA && MHARunner::fmha_supported(getHeadSize(), mSM);
}

size_t GPTAttentionPlugin::getWorkspaceSize(const nvinfer1::PluginTensorDesc* inputs, int nbInputs,
    const nvinfer1::PluginTensorDesc* outputs, int nbOutputs) const noexcept
{
    const int max_context_length = mMaxContextLength;
    const int nbReq = inputs[getSequenceLengthIdx()].dims.d[0];
    auto const type = inputs[getInputTensorIdx()].type;
    size_t const context_workspace_size = getWorkspaceSizeForContext(type, nbReq, max_context_length);

    const int total_num_seq = inputs[getSequenceLengthIdx()].dims.d[0];
    size_t const generation_workspace_size = getWorkspaceSizeForGeneration(type, total_num_seq);

    return std::max(context_workspace_size, generation_workspace_size);
}

static int32_t getStride(nvinfer1::Dims const& dims, int n)
{
    TLLM_CHECK(n >= 0 && n < dims.nbDims);
    return std::accumulate(dims.d + n + 1, dims.d + dims.nbDims, 1, std::multiplies<int32_t>{});
}

template <typename T, typename KVCacheBuffer>
int GPTAttentionPlugin::enqueueImpl(const nvinfer1::PluginTensorDesc* inputDesc,
    const nvinfer1::PluginTensorDesc* outputDesc, const void* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream)
{
    int32_t const nbSeq = inputDesc[getContextLengthsIdx()].dims.d[0];
    int32_t const beam_width = inputDesc[getCacheIndirIdx()].dims.d[1];
    RequestType const* reqTypes = static_cast<RequestType const*>(inputs[getRequestTypesIdx()]);

    int32_t nbContextRequests = 0;
    int32_t contextTokenIdxEnd = 0;
    // count context requests
    for (int32_t seqIdx = 0; seqIdx < nbSeq; seqIdx++)
    {
        if (reqTypes[seqIdx] != RequestType::kCONTEXT)
        {
            break;
        }
        ++nbContextRequests;
        contextTokenIdxEnd += mRemovePadding ? static_cast<int32_t const*>(inputs[getHostContextLengthsIdx()])[seqIdx]
                                             : inputDesc[getInputTensorIdx()].dims.d[1];
    }
    for (int32_t seqIdx = nbContextRequests; seqIdx < nbSeq; seqIdx++)
    {
        TLLM_CHECK(reqTypes[seqIdx] == RequestType::kGENERATION);
    }

    // mixed requests require mRemovePadding and mPagedKVCache
    if (nbContextRequests != 0 && nbContextRequests != nbSeq)
    {
        TLLM_CHECK(mRemovePadding && mPagedKVCache);
    }

    if (nbContextRequests > 0)
    {
        auto seqIdxBeg = 0;
        auto tokenIdxBeg = 0;
        auto localNbTokens = contextTokenIdxEnd;
        enqueueSome<T, KVCacheBuffer>(seqIdxBeg, nbContextRequests, tokenIdxBeg, localNbTokens, inputDesc, outputDesc,
            inputs, outputs, workspace, stream);
    }

    if (auto nbGenerationSeq = nbSeq - nbContextRequests; nbGenerationSeq > 0)
    {
        auto seqIdxBeg = nbContextRequests;
        auto tokenIdxBeg = contextTokenIdxEnd;
        auto localNbTokens = nbGenerationSeq;
        enqueueSome<T, KVCacheBuffer>(seqIdxBeg, nbGenerationSeq, tokenIdxBeg, localNbTokens, inputDesc, outputDesc,
            inputs, outputs, workspace, stream);
    }

    return 0;
}

template <typename T, typename KVCacheBuffer>
int GPTAttentionPlugin::enqueueSome(int32_t seqIdxBeg, int32_t localNbSeq, int32_t tokenIdxBeg, int32_t localNbTokens,
    const nvinfer1::PluginTensorDesc* inputDesc, const nvinfer1::PluginTensorDesc* outputDesc,
    const void* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream)
{
    const int beamWidth = inputDesc[getCacheIndirIdx()].dims.d[1];
    const int maxSeqLen = inputDesc[getCacheIndirIdx()].dims.d[2];

    const T* attention_input
        = static_cast<const T*>(inputs[getInputTensorIdx()]) + inputDesc[getInputTensorIdx()].dims.d[2] * tokenIdxBeg;
    const int* sequence_length = static_cast<const int*>(inputs[getSequenceLengthIdx()]) + seqIdxBeg;
    const T* qkv_bias = nullptr;
    if (mQKVBiasEnabled)
    {
        qkv_bias = reinterpret_cast<const T*>(inputs[getQKVBiasTensorIdx()]);
    }

    auto const reqTypeInBatchPtr = static_cast<RequestType const*>(inputs[getRequestTypesIdx()]) + seqIdxBeg;
    bool const is_context = (reqTypeInBatchPtr[0] == RequestType::kCONTEXT);

    const int* context_lengths = reinterpret_cast<const int*>(inputs[getContextLengthsIdx()]) + seqIdxBeg;
    // Note we still need context length during generation for MMHA optimziation.
    int32_t const max_context_len = [&]()
    {
        if (!mRemovePadding)
        {
            return inputDesc[getInputTensorIdx()].dims.d[1];
        }
        auto const host_context_lengths = static_cast<int32_t const*>(inputs[getHostContextLengthsIdx()]) + seqIdxBeg;
        return *std::max_element(host_context_lengths, host_context_lengths + localNbSeq);
    }();
    TLLM_CHECK(max_context_len <= mMaxContextLength);

    const float* kv_scale_orig_quant = nullptr;
    const float* kv_scale_quant_orig = nullptr;
    if (mKVCacheQuantMode.hasKvCacheQuant())
    {
        assert(inputDesc[getKVCacheQuantizationScaleIdx()].type == DataType::kFLOAT);
        assert(inputDesc[getKVCacheDequantizationScaleIdx()].type == DataType::kFLOAT);
        kv_scale_orig_quant = reinterpret_cast<const float*>(inputs[getKVCacheQuantizationScaleIdx()]);
        kv_scale_quant_orig = reinterpret_cast<const float*>(inputs[getKVCacheDequantizationScaleIdx()]);
    }

    int max_blocks_per_sequence = 0;
    int tokens_per_block = 0;
    void* block_pointers = nullptr;
    if (mPagedKVCache)
    {
        auto& kvCacheBlockPointers = inputDesc[getKVCacheBlockPointersIdx()];
        auto& kvCacheBlockPointersShape = inputDesc[getKVCacheBlockPointersIdx()].dims;
        // Div by 2 because we reinterpret int32 input as int64
        max_blocks_per_sequence = kvCacheBlockPointersShape.d[kvCacheBlockPointersShape.nbDims - 1] / 2;
        tokens_per_block = inputDesc[getPastKeyValueIdx()].dims.d[3];
        // Div by 2 because we reinterpret int32 input as int64
        auto offset = getStride(kvCacheBlockPointersShape, 0) / 2 * seqIdxBeg;
        auto const typed_block_pointers = static_cast<void* const*>(inputs[getKVCacheBlockPointersIdx()]) + offset;
        block_pointers = const_cast<void*>(static_cast<void const*>(typed_block_pointers));
    }

    T* context_buf_ = (T*) (outputs[0]) + outputDesc[0].dims.d[2] * tokenIdxBeg;
    void* key_value_cache = nullptr;
    if (!mPagedKVCache)
    {
        auto const cacheElemSize = (mKVCacheQuantMode.hasKvCacheQuant() ? 1 : sizeof(T));
        key_value_cache
            = static_cast<std::byte*>(outputs[1]) + cacheElemSize * getStride(outputDesc[1].dims, 0) * seqIdxBeg;
    }

    const T* alibi_slopes = isALiBi() ? static_cast<const T*>(inputs[getAlibiSlopesIdx()]) : nullptr;
    if (is_context) // context stage
    {
        const int batch_size = localNbSeq;
        enqueueContext<T, KVCacheBuffer>(
            EnqueueContextParams<T, KVCacheBuffer>{attention_input, qkv_bias, max_context_len, maxSeqLen,
                context_lengths, kv_scale_orig_quant, kv_scale_quant_orig, alibi_slopes, context_buf_, key_value_cache,
                block_pointers, batch_size, localNbTokens, tokens_per_block, max_blocks_per_sequence, workspace},
            stream);
    }
    else // generation stage; input_seq_len == 1
    {
        int batch_beam = localNbSeq;

        TLLM_CHECK(batch_beam % beamWidth == 0);
        int32_t const num_requests = batch_beam / beamWidth;

        const int* cache_indir = beamWidth == 1 ? nullptr : reinterpret_cast<const int*>(inputs[getCacheIndirIdx()]);

        int32_t const* past_kv_len_list = static_cast<const int*>(inputs[getHostPastKeyValueLengthsIdx()]) + seqIdxBeg;
        int32_t const past_kv_len = *std::max_element(past_kv_len_list, past_kv_len_list + localNbSeq);
        enqueueGeneration<T, KVCacheBuffer>(
            EnqueueGenerationParams<T, KVCacheBuffer>{attention_input, qkv_bias, sequence_length, past_kv_len,
                beamWidth, context_lengths, kv_scale_orig_quant, kv_scale_quant_orig, alibi_slopes, context_buf_,
                key_value_cache, block_pointers, maxSeqLen, num_requests, tokens_per_block, max_blocks_per_sequence,
                cache_indir, workspace},
            stream);
    }

    return 0;
}

template <typename T>
int GPTAttentionPlugin::enqueueDispatchKVCacheType(const nvinfer1::PluginTensorDesc* inputDesc,
    const nvinfer1::PluginTensorDesc* outputDesc, const void* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream)
{
    if (mPagedKVCache)
    {
        return enqueueImpl<T, KVBlockArray>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
    else
    {
        return enqueueImpl<T, KVLinearBuffer>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
    return 0;
}

int GPTAttentionPlugin::enqueue(const nvinfer1::PluginTensorDesc* inputDesc,
    const nvinfer1::PluginTensorDesc* outputDesc, const void* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    if (mType == DataType::kHALF)
    {
        return enqueueDispatchKVCacheType<half>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
    else if (mType == DataType::kFLOAT)
    {
        return enqueueDispatchKVCacheType<float>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
#ifdef ENABLE_BF16
    else if (mType == DataType::kBF16)
    {
        return enqueueDispatchKVCacheType<__nv_bfloat16>(inputDesc, outputDesc, inputs, outputs, workspace, stream);
    }
#endif
    return 0;
}

// IPluginV2Ext Methods
nvinfer1::DataType GPTAttentionPlugin::getOutputDataType(
    int index, const nvinfer1::DataType* inputTypes, int nbInputs) const noexcept
{
    TLLM_CHECK(index == 0 || index == 1);
    return inputTypes[index];
}

// IPluginV2 Methods

const char* GPTAttentionPlugin::getPluginType() const noexcept
{
    return GPT_ATTENTION_PLUGIN_NAME;
}

const char* GPTAttentionPlugin::getPluginVersion() const noexcept
{
    return GPT_ATTENTION_PLUGIN_VERSION;
}

int GPTAttentionPlugin::getNbOutputs() const noexcept
{
    return 2;
}

size_t GPTAttentionPlugin::getSerializationSize() const noexcept
{
    return GPTAttentionPluginCommon::getCommonSerializationSize();
}

void GPTAttentionPlugin::serialize(void* buffer) const noexcept
{
    GPTAttentionPluginCommon::serializeCommon(buffer);
}

///////////////

GPTAttentionPluginCreator::GPTAttentionPluginCreator()
    : GPTAttentionPluginCreatorCommon()
{

    mPluginAttributes.emplace_back(PluginField("remove_input_padding", nullptr, PluginFieldType::kINT8, 0));
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

const char* GPTAttentionPluginCreator::getPluginName() const noexcept
{
    return GPT_ATTENTION_PLUGIN_NAME;
}

const char* GPTAttentionPluginCreator::getPluginVersion() const noexcept
{
    return GPT_ATTENTION_PLUGIN_VERSION;
}

const PluginFieldCollection* GPTAttentionPluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2* GPTAttentionPluginCreator::createPlugin(const char* name, const PluginFieldCollection* fc) noexcept
{
    PluginFieldParser p{fc->nbFields, fc->fields};

    try
    {
        auto* obj = new GPTAttentionPlugin(p.getScalar<int32_t>("num_heads").value(),
            p.getScalar<int32_t>("num_kv_heads").value(), p.getScalar<int32_t>("unidirectional").value(),
            p.getScalar<float>("q_scaling").value(),
            static_cast<PositionEmbeddingType>(p.getScalar<int8_t>("position_embedding_type").value()),
            p.getScalar<int32_t>("rotary_embedding_dim").value(), p.getScalar<float>("rotary_embedding_base").value(),
            static_cast<RotaryScalingType>(p.getScalar<int8_t>("rotary_embedding_scale_type").value()),
            p.getScalar<float>("rotary_embedding_scale").value(),
            p.getScalar<int32_t>("rotary_embedding_max_positions").value(),
            static_cast<int32_t>(p.getScalar<int32_t>("tp_size").value()),
            static_cast<int32_t>(p.getScalar<int32_t>("tp_rank").value()),
            static_cast<ContextFMHAType>(p.getScalar<int8_t>("context_fmha_type").value()),
            static_cast<bool>(p.getScalar<int8_t>("multi_block_mode").value()),
            p.getScalar<int32_t>("kv_cache_quant_mode").value(),
            static_cast<bool>(p.getScalar<int8_t>("remove_input_padding").value()),
            static_cast<AttentionMaskType>(p.getScalar<int32_t>("mask_type").value()),
            static_cast<bool>(p.getScalar<int32_t>("paged_kv_cache").value()),
            static_cast<nvinfer1::DataType>(p.getScalar<int32_t>("type_id").value()),
            p.getScalar<int32_t>("max_context_length").value(),
            static_cast<bool>(p.getScalar<int8_t>("qkv_bias_enabled").value()));
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (const std::exception& e)
    {
        caughtError(e);
    }
    return nullptr;
}

IPluginV2* GPTAttentionPluginCreator::deserializePlugin(
    const char* name, const void* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call GPTAttentionPlugin::destroy()
    try
    {
        auto* obj = new GPTAttentionPlugin(serialData, serialLength);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (const std::exception& e)
    {
        caughtError(e);
    }
    return nullptr;
}