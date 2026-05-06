#include <fstream>
#include <iostream>
#include <numeric>
#include <cstdlib>
#include <unistd.h>

#include "cuda_assert.h"
#include "model.h"

#if (NV_TENSORRT_MAJOR > 8) || (NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR >= 4)
#define TRT_HAS_MEMORY_POOL_LIMIT 1
#else
#define TRT_HAS_MEMORY_POOL_LIMIT 0
#endif

#if NV_TENSORRT_MAJOR >= 10
#define TRT_HAS_IO_TENSOR_API 1
#else
#define TRT_HAS_IO_TENSOR_API 0
#endif

namespace {

size_t GetWorkspaceBytes() {
    const char *env = std::getenv("GPU_TRANSPARENT_UP_TRT_WORKSPACE_MB");
    long workspace_mb = 1024;
    if (env != nullptr && env[0] != '\0') {
        char *end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && parsed > 0) {
            workspace_mb = parsed;
        }
    }
    return static_cast<size_t>(workspace_mb) << 20;
}

} // namespace

void TRTLogger::log(Severity severity, const char *msg) noexcept {
    switch (severity) {
    case Severity::kINTERNAL_ERROR:
    case Severity::kERROR:
        ERRO("[TRT] %s", msg);
        break;
    case Severity::kWARNING:
        WARN("[TRT] %s", msg);
        break;
    default:
        break;
    }
}

TRTModel::TRTModel(const std::string &onnx, const std::string &engine, const size_t batch_size) : batch_size_(batch_size) {
    if (access(engine.c_str(), F_OK) != -1) {
        LoadEngine(engine);
    } else {
        BuildEngine(onnx);
        SaveEngine(engine);
    }
    InitContext();
    FLUSH_XLOG();
}

void TRTModel::CopyInput(cudaStream_t stream) {
    CopyInputAsync(stream);
    CUDART_ASSERT(cudaStreamSynchronize(stream));
}

void TRTModel::CopyOutput(cudaStream_t stream) {
    CopyOutputAsync(stream);
    CUDART_ASSERT(cudaStreamSynchronize(stream));
}

void TRTModel::CopyInputAsync(cudaStream_t stream) {
    for (auto tensor : input_tensors_) {
        tensor.second->CopyToDeviceAsync(stream);
    }
}

void TRTModel::CopyOutputAsync(cudaStream_t stream) {
    for (auto tensor : output_tensors_) {
        tensor.second->CopyToHostAsync(stream);
    }
}

void TRTModel::Infer(cudaStream_t stream) {
    InferAsync(stream);
    CUDART_ASSERT(cudaStreamSynchronize(stream));
}

void TRTModel::InferAsync(cudaStream_t stream) {
    Enqueue(stream);
}

void TRTModel::InferWithCopy(cudaStream_t stream) {
    CopyInputAsync(stream);
    InferAsync(stream);
    CopyOutputAsync(stream);
    CUDART_ASSERT(cudaStreamSynchronize(stream));
}

bool TRTModel::CheckOutput() {
    bool pass = true;
    for (auto out : output_tensors_) {
        if (out.second->CheckCorrect()) {
            continue;
        }
        pass = false;
        WARN("[RESULT] [FAIL] output tensor %s does not match", out.first.c_str());
    }
    return pass;
}

void TRTModel::ClearOutput(cudaStream_t stream) {
    for (auto out : output_tensors_) {
        out.second->Clear(stream);
    }
    CUDART_ASSERT(cudaStreamSynchronize(stream));
}

std::map<std::string, std::shared_ptr<Tensor>> &TRTModel::InputTensors() {
    return input_tensors_;
}

std::map<std::string, std::shared_ptr<Tensor>> &TRTModel::OutputTensors() {
    return output_tensors_;
}

void TRTModel::BuildEngine(const std::string &onnx) {
    auto builder = nvinfer1::createInferBuilder(logger_);
    ASSERT(builder, "Failed to create InferBuilder");

    // TensorRT 10 uses explicit batch semantics by default, so the deprecated
    // kEXPLICIT_BATCH flag is only needed for older releases.
#if NV_TENSORRT_MAJOR >= 10
    auto network = builder->createNetworkV2(0U);
#else
    const auto explicit_batch = 1U << (uint32_t)nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH;
    auto network = builder->createNetworkV2(explicit_batch);
#endif
    ASSERT(network, "Failed to create NetworkDefinition");

    auto parser = nvonnxparser::createParser(*network, logger_);
    ASSERT(parser, "Failed to create Parser");
    ASSERT(parser->parseFromFile(onnx.c_str(), false), "Failed to parse ONNX file");

    auto config = builder->createBuilderConfig();
    ASSERT(config, "Failed to create BuilderConfig");
    const size_t workspace_bytes = GetWorkspaceBytes();
#if TRT_HAS_MEMORY_POOL_LIMIT
    config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, workspace_bytes);
#else
    config->setMaxWorkspaceSize(workspace_bytes);
#endif

    auto profile = builder->createOptimizationProfile();
    ASSERT(profile, "Failed to create OptimizationProfile");

    for (int32_t i = 0; i < network->getNbInputs(); ++i) {
        nvinfer1::ITensor *input_tensor = network->getInput(i);
        nvinfer1::Dims dims = input_tensor->getDimensions();

        bool dynamic_shape = false;
        if (dims.d[0] == -1) {
            dims.d[0] = batch_size_;
            dynamic_shape = true;
        }
        for (int32_t j = 1; j < dims.nbDims; ++j) {
            if (dims.d[j] == -1) {
                dims.d[j] = default_dim_size_;
                dynamic_shape = true;
            }
        }

        if (dynamic_shape) {
            profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kMIN, dims);
            profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kOPT, dims);
            profile->setDimensions(input_tensor->getName(), nvinfer1::OptProfileSelector::kMAX, dims);
        }
    }

    config->addOptimizationProfile(profile);

#if NV_TENSORRT_MAJOR == 10 && NV_TENSORRT_MINOR <= 4
    auto serialized = builder->buildSerializedNetwork(*network, *config);
    ASSERT(serialized, "Failed to build serialized TensorRT engine from model %s", onnx.c_str());

    auto runtime = nvinfer1::createInferRuntime(logger_);
    ASSERT(runtime, "Failed to create InferRuntime while building model %s", onnx.c_str());
    engine_ = runtime->deserializeCudaEngine(serialized->data(), serialized->size());
    ASSERT(engine_, "Failed to deserialize TensorRT engine built from model %s", onnx.c_str());
#else
    engine_ = builder->buildEngineWithConfig(*network, *config);
    ASSERT(engine_, "Failed to build TensorRT engine from model %s", onnx.c_str());
#endif
    INFO("[MODEL] engine built with model %s", onnx.c_str());
}

void TRTModel::LoadEngine(const std::string &engine) {
    std::ifstream engine_file(engine, std::ios::binary | std::ios::ate);
    std::streamsize size = engine_file.tellg();
    engine_file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    ASSERT(engine_file.read(buffer.data(), size), "Failed to read engine file");

    auto runtime = nvinfer1::createInferRuntime(logger_);
    ASSERT(runtime, "Failed to create InferRuntime");

    bool init_plugins = initLibNvInferPlugins(&logger_, "");
    (void)init_plugins;
    engine_ = runtime->deserializeCudaEngine(buffer.data(), size);
    ASSERT(engine_, "Failed to deserialize CUDA engine");

    engine_file.close();
    INFO("[MODEL] engine %s loaded", engine.c_str());
}

void TRTModel::SaveEngine(const std::string &engine) {
    auto mem = engine_->serialize();
    ASSERT(mem, "Failed to serialize TensorRT engine %s", engine.c_str());

    std::ofstream engine_file(engine, std::ios::binary | std::ios::trunc);
    ASSERT(engine_file.good(), "Failed to open engine file %s for writing", engine.c_str());
    engine_file.write((const char *)mem->data(), mem->size());
    ASSERT(engine_file.good(), "Failed to write engine file %s", engine.c_str());
    engine_file.close();
    INFO("[MODEL] engine %s saved", engine.c_str());
}

void TRTModel::InitContext() {
    ctx_ = engine_->createExecutionContext();
    ASSERT(ctx_, "Failed to create IExecutionContext");

#if TRT_HAS_IO_TENSOR_API
    for (int32_t i = 0; i < engine_->getNbIOTensors(); ++i) {
        bool dynamic_shape = false;
        char const* tensor_name_c = engine_->getIOTensorName(i);
        std::string tensor_name(tensor_name_c);
        nvinfer1::Dims dims = engine_->getTensorShape(tensor_name_c);
        if (dims.d[0] == -1) {
            dims.d[0] = batch_size_;
            dynamic_shape = true;
        }
        for (int32_t j = 1; j < dims.nbDims; ++j) {
            if (dims.d[j] == -1) {
                dims.d[j] = default_dim_size_;
                dynamic_shape = true;
            }
        }

        if (dynamic_shape && engine_->getNbOptimizationProfiles() > 0) {
            dims = engine_->getProfileShape(tensor_name_c, 0, nvinfer1::OptProfileSelector::kOPT);
            ASSERT(ctx_->setInputShape(tensor_name_c, dims), "Failed to set input shape for tensor %s", tensor_name_c);
        }

        nvinfer1::DataType data_type = engine_->getTensorDataType(tensor_name_c);
        int32_t volume = std::accumulate(dims.d, dims.d + dims.nbDims, 1, std::multiplies<int32_t>());
        size_t total_size = volume * GetSize(data_type);

        bool is_input = engine_->getTensorIOMode(tensor_name_c) == nvinfer1::TensorIOMode::kINPUT;
        std::shared_ptr<Tensor> tensor = std::make_shared<Tensor>(total_size, dims, tensor_name);

        std::string log_str;
        if (is_input) {
            log_str = "[MODEL] created input tensor, name: " + tensor_name + ", " + GetTypeName(data_type) + "[" + std::to_string(dims.d[0]);
            input_tensors_[tensor_name] = tensor;
        } else {
            log_str = "[MODEL] created output tensor, name: " + tensor_name + ", " + GetTypeName(data_type) + "[" + std::to_string(dims.d[0]);
            output_tensors_[tensor_name] = tensor;
        }

        for (int32_t j = 1; j < dims.nbDims; ++j) {
            log_str += ", " + std::to_string(dims.d[j]);
        }
        log_str += "] (" + std::to_string(total_size) + " Bytes)";
        INFO("%s", log_str.c_str());
    }

    bindings_.clear();
    bindings_.reserve(engine_->getNbIOTensors());
    for (int32_t i = 0; i < engine_->getNbIOTensors(); ++i) {
        char const* tensor_name_c = engine_->getIOTensorName(i);
        std::string tensor_name(tensor_name_c);
        void* device_buffer = nullptr;
        if (engine_->getTensorIOMode(tensor_name_c) == nvinfer1::TensorIOMode::kINPUT) {
            device_buffer = input_tensors_[tensor_name]->DeviceBuffer();
        } else {
            device_buffer = output_tensors_[tensor_name]->DeviceBuffer();
        }
        ASSERT(ctx_->setTensorAddress(tensor_name_c, device_buffer), "Failed to bind tensor address for %s", tensor_name_c);
        bindings_.push_back(device_buffer);
    }
#else
    for (int32_t i = 0; i < engine_->getNbBindings(); ++i) {
        bool dynamic_shape = false;
        char const *tensor_name_c = engine_->getBindingName(i);
        std::string tensor_name(tensor_name_c);
        nvinfer1::Dims dims = engine_->getBindingDimensions(i);
        if (dims.d[0] == -1) {
            dims.d[0] = batch_size_;
            dynamic_shape = true;
        }
        for (int32_t j = 1; j < dims.nbDims; ++j) {
            if (dims.d[j] == -1) {
                dims.d[j] = default_dim_size_;
                dynamic_shape = true;
            }
        }

        if (dynamic_shape && engine_->getNbOptimizationProfiles() > 0) {
            dims = engine_->getProfileDimensions(i, 0, nvinfer1::OptProfileSelector::kOPT);
            ASSERT(ctx_->setBindingDimensions(i, dims), "Failed to set binding dimensions for %s", tensor_name_c);
        }

        nvinfer1::DataType data_type = engine_->getBindingDataType(i);
        int32_t volume = std::accumulate(dims.d, dims.d + dims.nbDims, 1, std::multiplies<int32_t>());
        size_t total_size = volume * GetSize(data_type);

        bool is_input = engine_->bindingIsInput(i);
        std::shared_ptr<Tensor> tensor = std::make_shared<Tensor>(total_size, dims, tensor_name);

        std::string log_str;
        if (is_input) {
            log_str = "[MODEL] created input tensor, name: " + tensor_name + ", " + GetTypeName(data_type) + "[" + std::to_string(dims.d[0]);
            input_tensors_[tensor_name] = tensor;
        } else {
            log_str = "[MODEL] created output tensor, name: " + tensor_name + ", " + GetTypeName(data_type) + "[" + std::to_string(dims.d[0]);
            output_tensors_[tensor_name] = tensor;
        }

        for (int32_t j = 1; j < dims.nbDims; ++j) {
            log_str += ", " + std::to_string(dims.d[j]);
        }
        log_str += "] (" + std::to_string(total_size) + " Bytes)";
        INFO("%s", log_str.c_str());
    }

    bindings_.clear();
    bindings_.assign(engine_->getNbBindings(), nullptr);
    for (int32_t i = 0; i < engine_->getNbBindings(); ++i) {
        char const *tensor_name_c = engine_->getBindingName(i);
        std::string tensor_name(tensor_name_c);
        void *device_buffer = nullptr;
        if (engine_->bindingIsInput(i)) {
            device_buffer = input_tensors_[tensor_name]->DeviceBuffer();
        } else {
            device_buffer = output_tensors_[tensor_name]->DeviceBuffer();
        }
        bindings_[i] = device_buffer;
    }
#endif
}

void TRTModel::Enqueue(cudaStream_t stream) {
#if TRT_HAS_IO_TENSOR_API
    ASSERT(ctx_->enqueueV3(stream), "Failed to enqueue inference");
#else
    ASSERT(ctx_->enqueueV2(bindings_.data(), stream, nullptr), "Failed to enqueue inference");
#endif
}
