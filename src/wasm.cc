// Copyright 2016-2019 Envoy Project Authors
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "include/proxy-wasm/wasm.h"

#include <cassert>
#include <cstdio>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>

#include "include/proxy-wasm/bytecode_util.h"
#include "include/proxy-wasm/signature_util.h"
#include "include/proxy-wasm/vm_id_handle.h"
#include "src/hash.h"

namespace proxy_wasm {

namespace {

// Map from Wasm key to the thread-local Wasm instance.
thread_local std::unordered_map<std::string, std::weak_ptr<WasmHandleBase>> local_wasms;
// Wasm key queue to track stale entries in `local_wasms`.
thread_local std::queue<std::string> local_wasms_keys;

// Map from plugin key to the thread-local plugin instance.
thread_local std::unordered_map<std::string, std::weak_ptr<PluginHandleBase>> local_plugins;
// Plugin key queue to track stale entries in `local_plugins`.
thread_local std::queue<std::string> local_plugins_keys;

// Check no more than `MAX_LOCAL_CACHE_GC_CHUNK_SIZE` cache entries at a time during stale entries
// cleanup.
const size_t MAX_LOCAL_CACHE_GC_CHUNK_SIZE = 64;

// Map from Wasm Key to the base Wasm instance, using a pointer to avoid the initialization fiasco.
std::mutex base_wasms_mutex;
std::unordered_map<std::string, std::weak_ptr<WasmHandleBase>> *base_wasms = nullptr;

void cacheLocalWasm(const std::string &key, const std::shared_ptr<WasmHandleBase> &wasm_handle) {
  local_wasms[key] = wasm_handle;
  local_wasms_keys.emplace(key);
}

void cacheLocalPlugin(const std::string &key,
                      const std::shared_ptr<PluginHandleBase> &plugin_handle) {
  local_plugins[key] = plugin_handle;
  local_plugins_keys.emplace(key);
}

template <class T>
void removeStaleLocalCacheEntries(std::unordered_map<std::string, std::weak_ptr<T>> &cache,
                                  std::queue<std::string> &keys) {
  auto num_keys_to_check = std::min(MAX_LOCAL_CACHE_GC_CHUNK_SIZE, keys.size());
  for (size_t i = 0; i < num_keys_to_check; i++) {
    std::string key(keys.front());
    keys.pop();

    const auto it = cache.find(key);
    if (it == cache.end()) {
      continue;
    }

    if (it->second.expired()) {
      cache.erase(it);
    } else {
      keys.push(std::move(key));
    }
  }
}

} // namespace

std::string makeVmKey(std::string_view vm_id, std::string_view vm_configuration,
                      std::string_view code) {
  return Sha256String({vm_id, "||", vm_configuration, "||", code});
}

class WasmBase::ShutdownHandle {
public:
  ~ShutdownHandle() { wasm_->finishShutdown(); }
  ShutdownHandle(std::shared_ptr<WasmBase> wasm) : wasm_(std::move(wasm)) {}

private:
  std::shared_ptr<WasmBase> wasm_;
};

void WasmBase::registerCallbacks() {
#define _REGISTER(_fn)                                                                             \
  wasm_vm_->registerCallback(                                                                      \
      "env", #_fn, &exports::_fn,                                                                  \
      &ConvertFunctionWordToUint32<decltype(exports::_fn),                                         \
                                   exports::_fn>::convertFunctionWordToUint32)
  _REGISTER(pthread_equal);
  _REGISTER(emscripten_notify_memory_growth);
#undef _REGISTER

  // Register the capability with the VM if it has been allowed, otherwise register a stub.
#define _REGISTER(module_name, name_prefix, export_prefix, _fn)                                    \
  if (capabilityAllowed(name_prefix #_fn)) {                                                       \
    wasm_vm_->registerCallback(                                                                    \
        module_name, name_prefix #_fn, &exports::export_prefix##_fn,                               \
        &ConvertFunctionWordToUint32<decltype(exports::export_prefix##_fn),                        \
                                     exports::export_prefix##_fn>::convertFunctionWordToUint32);   \
  } else {                                                                                         \
    typedef decltype(exports::export_prefix##_fn) export_type;                                     \
    constexpr export_type *stub = &exports::_fn##Stub<export_type>::stub;                          \
    wasm_vm_->registerCallback(                                                                    \
        module_name, name_prefix #_fn, stub,                                                       \
        &ConvertFunctionWordToUint32<export_type, stub>::convertFunctionWordToUint32);             \
  }

#define _REGISTER_WASI_UNSTABLE(_fn) _REGISTER("wasi_unstable", , wasi_unstable_, _fn)
#define _REGISTER_WASI_SNAPSHOT(_fn) _REGISTER("wasi_snapshot_preview1", , wasi_unstable_, _fn)
  FOR_ALL_WASI_FUNCTIONS(_REGISTER_WASI_UNSTABLE);
  FOR_ALL_WASI_FUNCTIONS(_REGISTER_WASI_SNAPSHOT);
#undef _REGISTER_WASI_UNSTABLE
#undef _REGISTER_WASI_SNAPSHOT

#define _REGISTER_PROXY(_fn) _REGISTER("env", "proxy_", , _fn)
  FOR_ALL_HOST_FUNCTIONS(_REGISTER_PROXY);

  if (abiVersion() == AbiVersion::ProxyWasm_0_1_0) {
    _REGISTER_PROXY(get_configuration);
    _REGISTER_PROXY(continue_request);
    _REGISTER_PROXY(continue_response);
    _REGISTER_PROXY(clear_route_cache);
  } else if (abiVersion() == AbiVersion::ProxyWasm_0_2_0) {
    _REGISTER_PROXY(continue_stream);
    _REGISTER_PROXY(close_stream);
  } else if (abiVersion() == AbiVersion::ProxyWasm_0_2_1 ||
             abiVersion() == AbiVersion::ProxyWasm_0_2_100) {
    _REGISTER_PROXY(continue_stream);
    _REGISTER_PROXY(close_stream);
    _REGISTER_PROXY(get_log_level);
    _REGISTER_PROXY(redis_init);
    _REGISTER_PROXY(redis_call);
    _REGISTER_PROXY(inject_encoded_data_to_filter_chain);
    _REGISTER_PROXY(set_upstream_override_host);
    _REGISTER_PROXY(get_upstream_hosts);
  }
#undef _REGISTER_PROXY

#undef _REGISTER
}

void WasmBase::getFunctions() {
#define _GET(_fn) wasm_vm_->getFunction(#_fn, &_fn##_);
#define _GET_ALIAS(_fn, _alias) wasm_vm_->getFunction(#_alias, &_fn##_);
  _GET(_initialize);
  if (_initialize_) {
    _GET(main);
  } else {
    _GET(_start);
  }

  _GET(malloc);
  if (!malloc_) {
    _GET_ALIAS(malloc, proxy_on_memory_allocate);
  }
  if (!malloc_) {
    fail(FailState::MissingFunction, "Wasm module is missing malloc function.");
  }
#undef _GET_ALIAS
#undef _GET

  // Try to point the capability to one of the module exports, if the capability has been allowed.
#define _GET_PROXY(_fn)                                                                            \
  if (capabilityAllowed("proxy_" #_fn)) {                                                          \
    wasm_vm_->getFunction("proxy_" #_fn, &_fn##_);                                                 \
  } else {                                                                                         \
    _fn##_ = nullptr;                                                                              \
  }
#define _GET_PROXY_ABI(_fn, _abi)                                                                  \
  if (capabilityAllowed("proxy_" #_fn)) {                                                          \
    wasm_vm_->getFunction("proxy_" #_fn, &_fn##_abi##_);                                           \
  } else {                                                                                         \
    _fn##_abi##_ = nullptr;                                                                        \
  }

  FOR_ALL_MODULE_FUNCTIONS(_GET_PROXY);

  if (abiVersion() == AbiVersion::ProxyWasm_0_1_0) {
    _GET_PROXY_ABI(on_request_headers, _abi_01);
    _GET_PROXY_ABI(on_response_headers, _abi_01);
  } else if (abiVersion() == AbiVersion::ProxyWasm_0_2_0 ||
             abiVersion() == AbiVersion::ProxyWasm_0_2_1) {
    _GET_PROXY_ABI(on_request_headers, _abi_02);
    _GET_PROXY_ABI(on_response_headers, _abi_02);
    _GET_PROXY(on_foreign_function);
    _GET_PROXY(on_redis_call_response);
  } else if (abiVersion() == AbiVersion::ProxyWasm_0_2_100) {
    _GET_PROXY_ABI(on_request_headers, _abi_03);
    _GET_PROXY_ABI(on_response_headers, _abi_02);
    _GET_PROXY(on_foreign_function);
    _GET_PROXY(on_redis_call_response);
  }
#undef _GET_PROXY_ABI
#undef _GET_PROXY
}

WasmBase::WasmBase(const std::shared_ptr<WasmHandleBase> &base_wasm_handle,
                   const WasmVmFactory &factory)
    : std::enable_shared_from_this<WasmBase>(*base_wasm_handle->wasm()),
      vm_id_(base_wasm_handle->wasm()->vm_id_), vm_key_(base_wasm_handle->wasm()->vm_key_),
      started_from_(base_wasm_handle->wasm()->wasm_vm()->cloneable()),
      envs_(base_wasm_handle->wasm()->envs()),
      allowed_capabilities_(base_wasm_handle->wasm()->allowed_capabilities_),
      base_wasm_handle_weak_(base_wasm_handle) {
  if (started_from_ != Cloneable::NotCloneable) {
    wasm_vm_ = base_wasm_handle->wasm()->wasm_vm()->clone();
  } else {
    wasm_vm_ = factory();
  }
  if (!wasm_vm_) {
    failed_ = FailState::UnableToCreateVm;
  } else {
    wasm_vm_->addFailCallback([this](FailState fail_state) { failed_ = fail_state; });
  }
}

WasmBase::WasmBase(std::unique_ptr<WasmVm> wasm_vm, std::string_view vm_id,
                   std::string_view vm_configuration, std::string_view vm_key,
                   std::unordered_map<std::string, std::string> envs,
                   AllowedCapabilitiesMap allowed_capabilities)
    : vm_id_(std::string(vm_id)), vm_key_(std::string(vm_key)), wasm_vm_(std::move(wasm_vm)),
      envs_(std::move(envs)), allowed_capabilities_(std::move(allowed_capabilities)),
      vm_configuration_(std::string(vm_configuration)), vm_id_handle_(getVmIdHandle(vm_id)) {
  if (!wasm_vm_) {
    failed_ = FailState::UnableToCreateVm;
  } else {
    wasm_vm_->addFailCallback([this](FailState fail_state) { failed_ = fail_state; });
  }
}

WasmBase::~WasmBase() {
  root_contexts_.clear();
  pending_done_.clear();
  pending_delete_.clear();
}

bool WasmBase::load(const std::string &code, bool allow_precompiled) {
  assert(!started_from_.has_value());

  if (!wasm_vm_) {
    return false;
  }

  if (wasm_vm_->getEngineName() == "null") {
    auto ok = wasm_vm_->load(code, {}, {});
    if (!ok) {
      fail(FailState::UnableToInitializeCode, "Failed to load NullVM plugin");
      return false;
    }
    abi_version_ = AbiVersion::ProxyWasm_0_2_1;
    return true;
  }

  // Verify signature.
  std::string message;
  if (!SignatureUtil::verifySignature(code, message)) {
    fail(FailState::UnableToInitializeCode, message);
    return false;
  }
  if (!message.empty()) {
    wasm_vm_->integration()->trace(message);
  }

  // Get ABI version from the module.
  if (!BytecodeUtil::getAbiVersion(code, abi_version_)) {
    fail(FailState::UnableToInitializeCode, "Failed to parse corrupted Wasm module");
    return false;
  }
  if (abi_version_ == AbiVersion::Unknown) {
    fail(FailState::UnableToInitializeCode, "Missing or unknown Proxy-Wasm ABI version");
    return false;
  }

  // Get function names from the module.
  if (!BytecodeUtil::getFunctionNameIndex(code, function_names_)) {
    fail(FailState::UnableToInitializeCode, "Failed to parse corrupted Wasm module");
    return false;
  }

  std::string_view precompiled = {};

  if (allow_precompiled) {
    // Check if precompiled module exists.
    const auto section_name = wasm_vm_->getPrecompiledSectionName();
    if (!section_name.empty()) {
      if (!BytecodeUtil::getCustomSection(code, section_name, precompiled)) {
        fail(FailState::UnableToInitializeCode, "Failed to parse corrupted Wasm module");
        return false;
      }
    }
  }

  // Get original bytecode (possibly stripped).
  std::string stripped;
  if (!BytecodeUtil::getStrippedSource(code, stripped)) {
    fail(FailState::UnableToInitializeCode, "Failed to parse corrupted Wasm module");
    return false;
  }

  auto ok = wasm_vm_->load(stripped, precompiled, function_names_);
  if (!ok) {
    fail(FailState::UnableToInitializeCode, "Failed to load Wasm bytecode");
    return false;
  }

  // Store for future use in non-cloneable Wasm engines.
  if (wasm_vm_->cloneable() == Cloneable::NotCloneable) {
    module_bytecode_ = stripped;
    module_precompiled_ = precompiled;
  }

  return true;
}

bool WasmBase::initialize() {
  if (!wasm_vm_) {
    return false;
  }

  if (started_from_ == Cloneable::NotCloneable) {
    auto base_wasm_handle = base_wasm_handle_weak_.lock();
    if (!base_wasm_handle) {
      fail(FailState::UnableToInitializeCode, "Base wasm handle is null");
      return false;
    }
    auto ok = wasm_vm_->load(base_wasm_handle->wasm()->moduleBytecode(),
                             base_wasm_handle->wasm()->modulePrecompiled(),
                             base_wasm_handle->wasm()->functionNames());
    if (!ok) {
      fail(FailState::UnableToInitializeCode, "Failed to load Wasm module from base Wasm");
      return false;
    }
  }

  if (started_from_.has_value()) {
    auto base_wasm_handle = base_wasm_handle_weak_.lock();
    if (!base_wasm_handle) {
      fail(FailState::UnableToInitializeCode, "Base wasm handle is null");
      return false;
    }
    abi_version_ = base_wasm_handle->wasm()->abiVersion();
  }

  if (started_from_ != Cloneable::InstantiatedModule) {
    registerCallbacks();
    if (!wasm_vm_->link(vm_id_)) {
      return false;
    }
  }

  vm_context_.reset(createVmContext());
  getFunctions();

  if (started_from_ != Cloneable::InstantiatedModule) {
    // Base VM was already started, so don't try to start cloned VMs again.
    startVm(vm_context_.get());
  }

  return !isFailed();
}

ContextBase *WasmBase::getRootContext(const std::shared_ptr<PluginBase> &plugin,
                                      bool allow_closed) {
  auto it = root_contexts_.find(plugin->key());
  if (it != root_contexts_.end()) {
    return it->second.get();
  }
  if (allow_closed) {
    it = pending_done_.find(plugin->key());
    if (it != pending_done_.end()) {
      return it->second.get();
    }
  }
  return nullptr;
}

void WasmBase::startVm(ContextBase *root_context) {
  // wasi_snapshot_preview1.clock_time_get
  wasm_vm_->setRestrictedCallback(
      true, {// logging (Proxy-Wasm)
             "env.proxy_log",
             // logging (stdout/stderr)
             "wasi_unstable.fd_write", "wasi_snapshot_preview1.fd_write",
             // args
             "wasi_unstable.args_sizes_get", "wasi_snapshot_preview1.args_sizes_get",
             "wasi_unstable.args_get", "wasi_snapshot_preview1.args_get",
             // environment variables
             "wasi_unstable.environ_sizes_get", "wasi_snapshot_preview1.environ_sizes_get",
             "wasi_unstable.environ_get", "wasi_snapshot_preview1.environ_get",
             // preopened files/directories
             "wasi_unstable.fd_prestat_get", "wasi_snapshot_preview1.fd_prestat_get",
             "wasi_unstable.fd_prestat_dir_name", "wasi_snapshot_preview1.fd_prestat_dir_name",
             // time
             "wasi_unstable.clock_time_get", "wasi_snapshot_preview1.clock_time_get",
             "wasi_unstable.clock_res_get", "wasi_snapshot_preview1.clock_res_get",
             // random
             "wasi_unstable.random_get", "wasi_snapshot_preview1.random_get",
             // Go runtime initialization
             "wasi_unstable.fd_fdstat_get", "wasi_snapshot_preview1.fd_fdstat_get",
             "wasi_unstable.fd_fdstat_set_flags", "wasi_snapshot_preview1.fd_fdstat_set_flags",
             "wasi_unstable.fd_fdstat_set_rights", "wasi_snapshot_preview1.fd_fdstat_set_rights",
             "wasi_unstable.path_filestat_get", "wasi_snapshot_preview1.path_filestat_get",
             "wasi_unstable.fd_filestat_get", "wasi_snapshot_preview1.fd_filestat_get",
             "wasi_unstable.fd_filestat_set_size", "wasi_snapshot_preview1.fd_filestat_set_size",
             "wasi_unstable.fd_filestat_set_times", "wasi_snapshot_preview1.fd_filestat_set_times",
             "wasi_unstable.fd_advise", "wasi_snapshot_preview1.fd_advise",
             "wasi_unstable.fd_allocate", "wasi_snapshot_preview1.fd_allocate",
             "wasi_unstable.fd_datasync", "wasi_snapshot_preview1.fd_datasync",
             "wasi_unstable.fd_pread", "wasi_snapshot_preview1.fd_pread",
             "wasi_unstable.fd_pwrite", "wasi_snapshot_preview1.fd_pwrite",
             "wasi_unstable.fd_readdir", "wasi_snapshot_preview1.fd_readdir",
             "wasi_unstable.fd_renumber", "wasi_snapshot_preview1.fd_renumber",
             "wasi_unstable.fd_sync", "wasi_snapshot_preview1.fd_sync",
             "wasi_unstable.fd_tell", "wasi_snapshot_preview1.fd_tell",
             "wasi_unstable.path_create_directory", "wasi_snapshot_preview1.path_create_directory",
             "wasi_unstable.path_filestat_set_times", "wasi_snapshot_preview1.path_filestat_set_times",
             "wasi_unstable.path_link", "wasi_snapshot_preview1.path_link",
             "wasi_unstable.path_readlink", "wasi_snapshot_preview1.path_readlink",
             "wasi_unstable.path_remove_directory", "wasi_snapshot_preview1.path_remove_directory",
             "wasi_unstable.path_rename", "wasi_snapshot_preview1.path_rename",
             "wasi_unstable.path_symlink", "wasi_snapshot_preview1.path_symlink",
             "wasi_unstable.path_unlink_file", "wasi_snapshot_preview1.path_unlink_file",
             "wasi_unstable.poll_oneoff", "wasi_snapshot_preview1.poll_oneoff",
             "wasi_unstable.sock_accept", "wasi_snapshot_preview1.sock_accept",
             "wasi_unstable.sock_recv", "wasi_snapshot_preview1.sock_recv",
             "wasi_unstable.sock_send", "wasi_snapshot_preview1.sock_send",
             "wasi_unstable.sock_shutdown", "wasi_snapshot_preview1.sock_shutdown"});
  if (_initialize_) {
    // WASI reactor.
    _initialize_(root_context);
    if (main_) {
      // Call main() if it exists in WASI reactor, to allow module to
      // do early initialization (e.g. configure SDK).
      //
      // Re-using main() keeps this consistent when switching between
      // WASI command (that calls main()) and reactor (that doesn't).
      main_(root_context, Word(0), Word(0));
    }
  } else if (_start_) {
    // WASI command.
    _start_(root_context);
  }
  wasm_vm_->setRestrictedCallback(false);
}

bool WasmBase::configure(ContextBase *root_context, std::shared_ptr<PluginBase> plugin) {
  return root_context->onConfigure(std::move(plugin));
}

ContextBase *WasmBase::start(const std::shared_ptr<PluginBase> &plugin) {
  auto it = root_contexts_.find(plugin->key());
  if (it != root_contexts_.end()) {
    it->second->onStart(plugin);
    return it->second.get();
  }
  auto context = std::unique_ptr<ContextBase>(createRootContext(plugin));
  auto *context_ptr = context.get();
  root_contexts_[plugin->key()] = std::move(context);
  if (!context_ptr->onStart(plugin)) {
    return nullptr;
  }
  return context_ptr;
};

uint32_t WasmBase::allocContextId() {
  while (true) {
    auto id = next_context_id_++;
    // Prevent reuse.
    if (contexts_.find(id) == contexts_.end()) {
      return id;
    }
  }
}

void WasmBase::startShutdown(std::string_view plugin_key) {
  auto it = root_contexts_.find(std::string(plugin_key));
  if (it != root_contexts_.end()) {
    if (it->second->onDone()) {
      it->second->onDelete();
    } else {
      pending_done_[it->first] = std::move(it->second);
    }
    root_contexts_.erase(it);
  }
}

void WasmBase::startShutdown() {
  auto it = root_contexts_.begin();
  while (it != root_contexts_.end()) {
    if (it->second->onDone()) {
      it->second->onDelete();
    } else {
      pending_done_[it->first] = std::move(it->second);
    }
    it = root_contexts_.erase(it);
  }
}

WasmResult WasmBase::done(ContextBase *root_context) {
  auto it = pending_done_.find(root_context->plugin_->key());
  if (it == pending_done_.end()) {
    return WasmResult::NotFound;
  }
  pending_delete_.insert(std::move(it->second));
  pending_done_.erase(it);
  // Defer the delete so that onDelete is not called from within the done() handler.
  shutdown_handle_ = std::make_unique<ShutdownHandle>(shared_from_this());
  addAfterVmCallAction(
      [shutdown_handle = shutdown_handle_.release()]() { delete shutdown_handle; });
  return WasmResult::Ok;
}

void WasmBase::finishShutdown() {
  auto it = pending_delete_.begin();
  while (it != pending_delete_.end()) {
    (*it)->onDelete();
    it = pending_delete_.erase(it);
  }
}

bool WasmHandleBase::canary(const std::shared_ptr<PluginBase> &plugin,
                            const WasmHandleCloneFactory &clone_factory) {
  if (this->wasm() == nullptr) {
    return false;
  }
  auto it = plugin_canary_cache_.find(plugin->key());
  if (it != plugin_canary_cache_.end()) {
    return it->second;
  }
  auto configuration_canary_handle = clone_factory(shared_from_this());
  if (!configuration_canary_handle) {
    this->wasm()->fail(FailState::UnableToCloneVm, "Failed to clone Base Wasm");
    return false;
  }
  if (!configuration_canary_handle->wasm()->initialize()) {
    configuration_canary_handle->wasm()->fail(FailState::UnableToInitializeCode,
                                              "Failed to initialize Wasm code");
    return false;
  }
  auto *root_context = configuration_canary_handle->wasm()->start(plugin);
  if (root_context == nullptr) {
    configuration_canary_handle->wasm()->fail(FailState::StartFailed, "Failed to start base Wasm");
    return false;
  }
  if (!configuration_canary_handle->wasm()->configure(root_context, plugin)) {
    configuration_canary_handle->wasm()->fail(FailState::ConfigureFailed,
                                              "Failed to configure base Wasm plugin");
    plugin_canary_cache_[plugin->key()] = false;
    return false;
  }
  configuration_canary_handle->kill();
  plugin_canary_cache_[plugin->key()] = true;
  return true;
}

std::shared_ptr<WasmHandleBase> createWasm(const std::string &vm_key, const std::string &code,
                                           const std::shared_ptr<PluginBase> &plugin,
                                           const WasmHandleFactory &factory,
                                           const WasmHandleCloneFactory &clone_factory,
                                           bool allow_precompiled) {
  std::shared_ptr<WasmHandleBase> wasm_handle;
  {
    std::lock_guard<std::mutex> guard(base_wasms_mutex);
    if (base_wasms == nullptr) {
      base_wasms = new std::remove_reference<decltype(*base_wasms)>::type;
    }
    auto it = base_wasms->find(vm_key);
    if (it != base_wasms->end()) {
      wasm_handle = it->second.lock();
      if (!wasm_handle) {
        base_wasms->erase(it);
      }
    }
    if (!wasm_handle) {
      // If no cached base_wasm, creates a new base_wasm, loads the code and initializes it.
      wasm_handle = factory(vm_key);
      if (!wasm_handle) {
        return nullptr;
      }
      if (!wasm_handle->wasm()->load(code, allow_precompiled)) {
        wasm_handle->wasm()->fail(FailState::UnableToInitializeCode, "Failed to load Wasm code");
        return nullptr;
      }
      if (!wasm_handle->wasm()->initialize()) {
        wasm_handle->wasm()->fail(FailState::UnableToInitializeCode,
                                  "Failed to initialize Wasm code");
        return nullptr;
      }
      (*base_wasms)[vm_key] = wasm_handle;
    }
  }

  // Either creating new one or reusing the existing one, apply canary for each plugin.
  if (!wasm_handle->canary(plugin, clone_factory)) {
    return nullptr;
  }
  return wasm_handle;
};

std::shared_ptr<WasmHandleBase> getThreadLocalWasm(std::string_view vm_key) {
  auto it = local_wasms.find(std::string(vm_key));
  if (it != local_wasms.end()) {
    auto wasm = it->second.lock();
    if (wasm) {
      return wasm;
    }
    local_wasms.erase(it);
  }
  removeStaleLocalCacheEntries(local_wasms, local_wasms_keys);
  return nullptr;
}

void setWasmFailCallback(const std::string &vm_key,
                         const std::shared_ptr<WasmHandleBase> &wasm_handle) {
  wasm_handle->wasm()->wasm_vm()->addFailCallback([vm_key](proxy_wasm::FailState fail_state) {
    if (fail_state == proxy_wasm::FailState::RuntimeError) {
      // If VM failed, erase the entry so that:
      // 1) we can recreate the new thread local VM from the same base_wasm.
      // 2) we wouldn't reuse the failed VM for new plugins accidentally.
      local_wasms.erase(vm_key);
    }
  });
}

void setWasmRecoverCallback(const std::string &vm_key,
                            const std::shared_ptr<WasmHandleBase> &wasm_handle,
                            const std::shared_ptr<WasmHandleBase> &base_handle,
                            const WasmHandleCloneFactory &clone_factory) {
  std::weak_ptr<WasmHandleBase> wasm_handle_for_copy = wasm_handle;
  std::weak_ptr<WasmHandleBase> base_handle_for_copy = base_handle;
  wasm_handle->setRecoverVmCallback([vm_key, wasm_handle_for_copy, base_handle_for_copy,
                                     clone_factory]() -> std::shared_ptr<WasmHandleBase> {
    const auto base_handle = base_handle_for_copy.lock();
    if (!base_handle) {
      std::cerr << "Failed to get base_handle shared_ptr in setRecoverVmCallback" << std::endl;
      return nullptr;
    }
    const auto &integration = base_handle->wasm()->wasm_vm()->integration();
    integration->trace("Start recover wasm_handle");
    auto it = local_wasms.find(vm_key);
    if (it != local_wasms.end()) {
      auto wasm_handle = it->second.lock();
      if (wasm_handle) {
        integration->trace("Wasm handle already exists");
        return wasm_handle;
      }
      local_wasms.erase(vm_key);
    }
    removeStaleLocalCacheEntries(local_wasms, local_wasms_keys);
    auto old_wasm_handle = wasm_handle_for_copy.lock();
    if (old_wasm_handle) {
      // avoid the context use the stale wasm ptr
      old_wasm_handle->wasm()->clearWasmInContext();
    }
    // Try to recover wasm vm
    auto new_handle = clone_factory(base_handle);
    if (!new_handle) {
      base_handle->wasm()->fail(FailState::RecoverError,
                                "Failed to clone Base Wasm during recover");
      return nullptr;
    }

    if (!new_handle->wasm()->initialize()) {
      base_handle->wasm()->fail(FailState::RecoverError,
                                "Failed to initialize Wasm code during recover");
      return nullptr;
    }
    cacheLocalWasm(vm_key, new_handle);
    setWasmFailCallback(vm_key, new_handle);
    setWasmRecoverCallback(vm_key, new_handle, base_handle, clone_factory);
    integration->trace("Wasm handle has been recovered");
    return new_handle;
  });
}

static std::shared_ptr<WasmHandleBase>
getOrCreateThreadLocalWasm(const std::shared_ptr<WasmHandleBase> &base_handle,
                           const WasmHandleCloneFactory &clone_factory) {
  std::string vm_key(base_handle->wasm()->vm_key());
  // Get existing thread-local WasmVM.
  auto it = local_wasms.find(vm_key);
  if (it != local_wasms.end()) {
    auto wasm_handle = it->second.lock();
    if (wasm_handle) {
      return wasm_handle;
    }
    local_wasms.erase(it);
  }
  removeStaleLocalCacheEntries(local_wasms, local_wasms_keys);
  // Create and initialize new thread-local WasmVM.
  auto wasm_handle = clone_factory(base_handle);
  if (!wasm_handle) {
    base_handle->wasm()->fail(FailState::UnableToCloneVm, "Failed to clone Base Wasm");
    return nullptr;
  }

  if (!wasm_handle->wasm()->initialize()) {
    base_handle->wasm()->fail(FailState::UnableToInitializeCode, "Failed to initialize Wasm code");
    return nullptr;
  }
  cacheLocalWasm(vm_key, wasm_handle);
  setWasmFailCallback(vm_key, wasm_handle);
  setWasmRecoverCallback(vm_key, wasm_handle, base_handle, clone_factory);
  return wasm_handle;
}

void setPluginFailCallback(const std::string &key,
                           const std::shared_ptr<WasmHandleBase> &wasm_handle) {
  wasm_handle->wasm()->wasm_vm()->addFailCallback(key, [key](proxy_wasm::FailState fail_state) {
    if (fail_state == proxy_wasm::FailState::RuntimeError) {
      // If VM failed, erase the entry so that:
      // 1) we can recreate the new thread local plugin from the same base_wasm.
      // 2) we wouldn't reuse the failed VM for new plugin configs accidentally.
      local_plugins.erase(key);
    }
  });
}

void setPluginRecoverCallback(const std::string &key,
                              const std::shared_ptr<PluginHandleBase> &plugin_handle,
                              const std::shared_ptr<WasmHandleBase> &base_handle,
                              const std::shared_ptr<PluginBase> &plugin,
                              const PluginHandleFactory &plugin_factory) {
  std::weak_ptr<WasmHandleBase> base_handle_for_copy = base_handle;

  plugin_handle->setRecoverPluginCallback(
      [key, base_handle_for_copy, plugin, plugin_factory](
          std::shared_ptr<WasmHandleBase> &wasm_handle) -> std::shared_ptr<PluginHandleBase> {
        const auto base_handle = base_handle_for_copy.lock();
        if (!base_handle) {
          std::cerr << "Failed to get base_handle shared_ptr in setRecoverPluginCallback"
                    << std::endl;
          return nullptr;
        }
        const auto &integration = base_handle->wasm()->wasm_vm()->integration();
        integration->trace("Start recover plugin_handle");
        auto it = local_plugins.find(key);
        if (it != local_plugins.end()) {
          auto plugin_handle = it->second.lock();
          if (plugin_handle) {
            return plugin_handle;
          }
          local_plugins.erase(key);
        }
        removeStaleLocalCacheEntries(local_plugins, local_plugins_keys);
        // Try to recover wasm plugin
        auto *plugin_context = wasm_handle->wasm()->start(plugin);
        if (plugin_context == nullptr) {
          base_handle->wasm()->fail(FailState::RecoverError,
                                    "Failed to start thread-local Wasm during recover");
          return nullptr;
        }
        if (!wasm_handle->wasm()->configure(plugin_context, plugin)) {
          base_handle->wasm()->fail(FailState::RecoverError,
                                    "Failed to configure thread-local Wasm plugin during recover");
          return nullptr;
        }
        auto new_handle = plugin_factory(wasm_handle, plugin);
        cacheLocalPlugin(key, new_handle);
        setPluginFailCallback(key, wasm_handle);
        setPluginRecoverCallback(key, new_handle, base_handle, plugin, plugin_factory);
        integration->trace("Plugin handle has been recovered");
        return new_handle;
      });
}

std::shared_ptr<PluginHandleBase> getOrCreateThreadLocalPlugin(
    const std::shared_ptr<WasmHandleBase> &base_handle, const std::shared_ptr<PluginBase> &plugin,
    const WasmHandleCloneFactory &clone_factory, const PluginHandleFactory &plugin_factory) {
  std::string key(std::string(base_handle->wasm()->vm_key()) + "||" + plugin->key());
  // Get existing thread-local Plugin handle.
  auto it = local_plugins.find(key);
  if (it != local_plugins.end()) {
    auto plugin_handle = it->second.lock();
    if (plugin_handle) {
      return plugin_handle;
    }
    local_plugins.erase(it);
  }
  removeStaleLocalCacheEntries(local_plugins, local_plugins_keys);
  // Get thread-local WasmVM.
  auto wasm_handle = getOrCreateThreadLocalWasm(base_handle, clone_factory);
  if (!wasm_handle) {
    return nullptr;
  }
  // Create and initialize new thread-local Plugin.
  auto *plugin_context = wasm_handle->wasm()->start(plugin);
  if (plugin_context == nullptr) {
    base_handle->wasm()->fail(FailState::StartFailed, "Failed to start thread-local Wasm");
    return nullptr;
  }
  if (!wasm_handle->wasm()->configure(plugin_context, plugin)) {
    base_handle->wasm()->fail(FailState::ConfigureFailed,
                              "Failed to configure thread-local Wasm plugin");
    return nullptr;
  }
  auto plugin_handle = plugin_factory(wasm_handle, plugin);
  cacheLocalPlugin(key, plugin_handle);
  plugin_handle->setPluginHandleKey(key);
  setPluginFailCallback(key, wasm_handle);
  setPluginRecoverCallback(key, plugin_handle, base_handle, plugin, plugin_factory);
  return plugin_handle;
}

void clearWasmCachesForTesting() {
  local_plugins.clear();
  local_wasms.clear();
  std::lock_guard<std::mutex> guard(base_wasms_mutex);
  if (base_wasms != nullptr) {
    delete base_wasms;
    base_wasms = nullptr;
  }
}

std::vector<std::string> staleLocalPluginsKeysForTesting() {
  std::vector<std::string> keys;
  for (const auto &kv : local_plugins) {
    if (kv.second.expired()) {
      keys.push_back(kv.first);
    }
  }
  return keys;
}

std::vector<std::string> staleLocalWasmsKeysForTesting() {
  std::vector<std::string> keys;
  for (const auto &kv : local_wasms) {
    if (kv.second.expired()) {
      keys.push_back(kv.first);
    }
  }
  return keys;
}

} // namespace proxy_wasm
