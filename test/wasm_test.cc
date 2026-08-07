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

#include <unordered_set>

#include "gtest/gtest.h"

#include "test/utility.h"

#include "src/wasm.h"

namespace proxy_wasm {

INSTANTIATE_TEST_SUITE_P(WasmEngines, TestVm, testing::ValuesIn(getWasmEngines()),
                         [](const testing::TestParamInfo<std::string> &info) {
                           return info.param;
                         });

namespace {

enum class CloneResult { Normal, ReturnNull, Uninitializable, InitializeFailure };

struct ControlledFailureState {
  bool fail_start = false;
  bool fail_configure = false;
  bool trap_start = false;
  bool trap_configure = false;
  CloneResult clone_result = CloneResult::Normal;
};

class ControlledContext : public TestContext {
public:
  ControlledContext(WasmBase *wasm, const std::shared_ptr<PluginBase> &plugin,
                    std::shared_ptr<ControlledFailureState> state)
      : TestContext(wasm, plugin), state_(std::move(state)) {}

  bool onStart(std::shared_ptr<PluginBase> plugin) override {
    if (state_->trap_start) {
      wasm()->wasm_vm()->fail(FailState::RuntimeError, "injected start trap");
      return false;
    }
    return !state_->fail_start && TestContext::onStart(std::move(plugin));
  }

  bool onConfigure(std::shared_ptr<PluginBase> plugin) override {
    if (state_->trap_configure) {
      wasm()->wasm_vm()->fail(FailState::RuntimeError, "injected configure trap");
      return false;
    }
    return !state_->fail_configure && TestContext::onConfigure(std::move(plugin));
  }

private:
  std::shared_ptr<ControlledFailureState> state_;
};

class ControlledWasm : public TestWasm {
public:
  ControlledWasm(std::unique_ptr<WasmVm> wasm_vm, std::string_view vm_id,
                 std::string_view vm_configuration, std::string_view vm_key,
                 std::shared_ptr<ControlledFailureState> state)
      : TestWasm(std::move(wasm_vm), {}, vm_id, vm_configuration, vm_key),
        state_(std::move(state)) {}

  ControlledWasm(const std::shared_ptr<WasmHandleBase> &base_wasm_handle,
                 const WasmVmFactory &factory, std::shared_ptr<ControlledFailureState> state)
      : TestWasm(base_wasm_handle, factory), state_(std::move(state)) {}

  ContextBase *createRootContext(const std::shared_ptr<PluginBase> &plugin) override {
    return new ControlledContext(this, plugin, state_);
  }

private:
  std::shared_ptr<ControlledFailureState> state_;
};

class ErrorRecordingWasm : public WasmBase {
public:
  using WasmBase::WasmBase;

  void error(std::string_view message) override { last_error = message; }

  std::string last_error;
};

WasmHandleFactory makeControlledWasmFactory(const std::function<std::unique_ptr<WasmVm>()> &new_vm,
                                            const std::shared_ptr<ControlledFailureState> &state,
                                            std::string_view vm_id,
                                            std::string_view vm_configuration) {
  return [new_vm, state, vm_id = std::string(vm_id),
          vm_configuration = std::string(vm_configuration)](
             std::string_view vm_key) -> std::shared_ptr<WasmHandleBase> {
    auto wasm = std::make_shared<ControlledWasm>(new_vm(), vm_id, vm_configuration, vm_key, state);
    return std::make_shared<WasmHandleBase>(std::move(wasm));
  };
}

WasmHandleCloneFactory
makeControlledCloneFactory(const std::function<std::unique_ptr<WasmVm>()> &new_vm,
                           const std::shared_ptr<ControlledFailureState> &state,
                           std::shared_ptr<WasmHandleBase> *last_clone = nullptr) {
  return [new_vm, state, last_clone](const std::shared_ptr<WasmHandleBase> &base_wasm_handle)
             -> std::shared_ptr<WasmHandleBase> {
    if (state->clone_result == CloneResult::ReturnNull) {
      return nullptr;
    }

    std::shared_ptr<WasmBase> wasm;
    if (state->clone_result == CloneResult::Uninitializable) {
      wasm = std::make_shared<WasmBase>(
          std::unique_ptr<WasmVm>{}, base_wasm_handle->wasm()->vm_id(),
          base_wasm_handle->wasm()->vm_configuration(), base_wasm_handle->wasm()->vm_key(),
          std::unordered_map<std::string, std::string>{}, AllowedCapabilitiesMap{});
    } else {
      wasm = std::make_shared<ControlledWasm>(base_wasm_handle, new_vm, state);
      if (state->clone_result == CloneResult::InitializeFailure) {
        wasm->fail(FailState::UnableToInitializeCode, "injected initialize failure");
      }
    }
    auto handle = std::make_shared<WasmHandleBase>(std::move(wasm));
    if (last_clone) {
      *last_clone = handle;
    }
    return handle;
  };
}

PluginHandleFactory makePluginHandleFactory() {
  return [](const std::shared_ptr<WasmHandleBase> &wasm_handle,
            const std::shared_ptr<PluginBase> &plugin) {
    return std::make_shared<PluginHandleBase>(wasm_handle, plugin);
  };
}

} // namespace

// Fail callbacks only used for WasmVMs - not available for NullVM.
TEST_P(TestVm, GetOrCreateThreadLocalWasmFailCallbacks) {
  const auto *const plugin_name = "plugin_name";
  const auto *const root_id = "root_id";
  const auto *const vm_id = "vm_id";
  const auto *const vm_config = "vm_config";
  const auto *const plugin_config = "plugin_config";
  const auto fail_open = false;

  // Create a plugin.
  const auto plugin = std::make_shared<PluginBase>(plugin_name, root_id, vm_id, engine_,
                                                   plugin_config, fail_open, "plugin_key");

  // Define callbacks.
  WasmHandleFactory wasm_handle_factory =
      [this, vm_id, vm_config](std::string_view vm_key) -> std::shared_ptr<WasmHandleBase> {
    auto base_wasm = std::make_shared<WasmBase>(makeVm(engine_), vm_id, vm_config, vm_key,
                                                std::unordered_map<std::string, std::string>{},
                                                AllowedCapabilitiesMap{});
    return std::make_shared<WasmHandleBase>(base_wasm);
  };

  WasmHandleCloneFactory wasm_handle_clone_factory =
      [this](const std::shared_ptr<WasmHandleBase> &base_wasm_handle)
      -> std::shared_ptr<WasmHandleBase> {
    auto wasm = std::make_shared<WasmBase>(
        base_wasm_handle, [this]() -> std::unique_ptr<WasmVm> { return makeVm(engine_); });
    return std::make_shared<WasmHandleBase>(wasm);
  };

  PluginHandleFactory plugin_handle_factory =
      [](const std::shared_ptr<WasmHandleBase> &base_wasm,
         const std::shared_ptr<PluginBase> &plugin) -> std::shared_ptr<PluginHandleBase> {
    return std::make_shared<PluginHandleBase>(base_wasm, plugin);
  };

  // Read the minimal loadable binary.
  auto source = readTestWasmFile("abi_export.wasm");

  // Create base Wasm via createWasm.
  auto base_wasm_handle =
      createWasm("vm_key", source, plugin, wasm_handle_factory, wasm_handle_clone_factory, false);
  ASSERT_TRUE(base_wasm_handle && base_wasm_handle->wasm());

  // Create a thread local plugin.
  auto thread_local_plugin = getOrCreateThreadLocalPlugin(
      base_wasm_handle, plugin, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(thread_local_plugin && thread_local_plugin->plugin());
  // If the VM is not failed, same WasmBase should be used for the same configuration.
  ASSERT_EQ(getOrCreateThreadLocalPlugin(base_wasm_handle, plugin, wasm_handle_clone_factory,
                                         plugin_handle_factory)
                ->wasm(),
            thread_local_plugin->wasm());

  // Cause runtime crash.
  thread_local_plugin->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(thread_local_plugin->wasm()->isFailed());
  // the Base Wasm should not be affected by cloned ones.
  ASSERT_FALSE(base_wasm_handle->wasm()->isFailed());

  // Create another thread local plugin with the same configuration.
  // This one should not end up using the failed VM.
  auto thread_local_plugin2 = getOrCreateThreadLocalPlugin(
      base_wasm_handle, plugin, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(thread_local_plugin2 && thread_local_plugin2->plugin());
  ASSERT_FALSE(thread_local_plugin2->wasm()->isFailed());
  // Verify the pointer to WasmBase is different from the failed one.
  ASSERT_NE(thread_local_plugin2->wasm(), thread_local_plugin->wasm());

  // Cause runtime crash again.
  thread_local_plugin2->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(thread_local_plugin2->wasm()->isFailed());
  // the Base Wasm should not be affected by cloned ones.
  ASSERT_FALSE(base_wasm_handle->wasm()->isFailed());

  // This time, create another thread local plugin with *different* plugin key for the same vm_key.
  // This one also should not end up using the failed VM.
  const auto plugin2 = std::make_shared<PluginBase>(plugin_name, root_id, vm_id, engine_,
                                                    plugin_config, fail_open, "another_plugin_key");
  auto thread_local_plugin3 = getOrCreateThreadLocalPlugin(
      base_wasm_handle, plugin2, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(thread_local_plugin3 && thread_local_plugin3->plugin());
  ASSERT_FALSE(thread_local_plugin3->wasm()->isFailed());
  // Verify the pointer to WasmBase is different from the failed one.
  ASSERT_NE(thread_local_plugin3->wasm(), thread_local_plugin->wasm());
  ASSERT_NE(thread_local_plugin3->wasm(), thread_local_plugin2->wasm());
}

TEST_P(TestVm, ThreadLocalPluginFailuresBelongToClone) {
  clearWasmCachesForTesting();
  const auto state = std::make_shared<ControlledFailureState>();
  const auto new_vm = [this]() { return makeVm(engine_); };
  std::shared_ptr<WasmHandleBase> last_clone;
  const auto clone_factory = makeControlledCloneFactory(new_vm, state, &last_clone);
  const auto plugin_factory = makePluginHandleFactory();
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  const auto configure_plugin = std::make_shared<PluginBase>(
      "plugin_name", "root_id", "vm_id", engine_, "other_config", false, "plugin_key");
  const std::string vm_key = "thread-local-failure-vm-key";
  auto base_handle = createWasm(vm_key, readTestWasmFile("abi_export.wasm"), plugin,
                                makeControlledWasmFactory(new_vm, state, "vm_id", "vm_config"),
                                clone_factory, false);
  ASSERT_TRUE(base_handle && base_handle->wasm());
  last_clone.reset();

  state->fail_start = true;
  EXPECT_EQ(getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory),
            nullptr);
  ASSERT_TRUE(last_clone && last_clone->wasm());
  const auto start_failed_clone = last_clone;
  EXPECT_EQ(start_failed_clone->wasm()->fail_state(), FailState::StartFailed);
  EXPECT_FALSE(base_handle->wasm()->isFailed());
  EXPECT_EQ(getThreadLocalWasm(vm_key), nullptr);

  state->fail_start = false;
  auto healthy_plugin =
      getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory);
  ASSERT_TRUE(healthy_plugin && healthy_plugin->wasm());
  EXPECT_NE(healthy_plugin->wasmHandle(), start_failed_clone);

  const auto configure_failed_clone = healthy_plugin->wasmHandle();
  state->fail_configure = true;
  EXPECT_EQ(
      getOrCreateThreadLocalPlugin(base_handle, configure_plugin, clone_factory, plugin_factory),
      nullptr);
  EXPECT_EQ(configure_failed_clone->wasm()->fail_state(), FailState::ConfigureFailed);
  EXPECT_FALSE(base_handle->wasm()->isFailed());
  EXPECT_EQ(getThreadLocalWasm(vm_key), nullptr);

  state->fail_configure = false;
  auto replacement =
      getOrCreateThreadLocalPlugin(base_handle, configure_plugin, clone_factory, plugin_factory);
  ASSERT_TRUE(replacement && replacement->wasm());
  EXPECT_NE(replacement->wasmHandle(), configure_failed_clone);
  clearWasmCachesForTesting();
}

TEST_P(TestVm, EveryNonOkFailureEvictsOnlyItsGeneration) {
  clearWasmCachesForTesting();
  const auto state = std::make_shared<ControlledFailureState>();
  const auto new_vm = [this]() { return makeVm(engine_); };
  const auto clone_factory = makeControlledCloneFactory(new_vm, state);
  const auto plugin_factory = makePluginHandleFactory();
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  const std::string vm_key = "all-fail-states-vm-key";
  auto base_handle = createWasm(vm_key, readTestWasmFile("abi_export.wasm"), plugin,
                                makeControlledWasmFactory(new_vm, state, "vm_id", "vm_config"),
                                clone_factory, false);
  ASSERT_TRUE(base_handle && base_handle->wasm());

  const FailState failure_states[] = {
      FailState::UnableToCreateVm,       FailState::UnableToCloneVm, FailState::MissingFunction,
      FailState::UnableToInitializeCode, FailState::StartFailed,     FailState::ConfigureFailed,
      FailState::RuntimeError,           FailState::RecoverError,
  };
  auto current = getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory);
  ASSERT_TRUE(current && current->wasm());
  for (const auto fail_state : failure_states) {
    const auto failed_handle = current->wasmHandle();
    current->wasm()->wasm_vm()->fail(fail_state, "injected terminal failure");
    EXPECT_EQ(getThreadLocalWasm(vm_key), nullptr);
    auto replacement =
        getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory);
    ASSERT_TRUE(replacement && replacement->wasm());
    EXPECT_NE(replacement->wasmHandle(), failed_handle);
    current = std::move(replacement);
  }
  EXPECT_FALSE(base_handle->wasm()->isFailed());

  // Build two newer generations, then deliver failures from both retired generations. Both the
  // Wasm and plugin caches must continue to point at the newest generation.
  const auto generation_a = current;
  generation_a->wasm()->setShouldRebuild(true);
  std::shared_ptr<PluginHandleBase> generation_b;
  ASSERT_TRUE(generation_a->rebuild(generation_b));
  ASSERT_TRUE(generation_b && generation_b->wasm());
  generation_b->wasm()->setShouldRebuild(true);
  std::shared_ptr<PluginHandleBase> generation_c;
  ASSERT_TRUE(generation_b->rebuild(generation_c));
  ASSERT_TRUE(generation_c && generation_c->wasm());

  generation_a->wasm()->wasm_vm()->fail(FailState::RuntimeError, "delayed generation A failure");
  generation_b->wasm()->wasm_vm()->fail(FailState::RecoverError, "delayed generation B failure");
  EXPECT_EQ(getThreadLocalWasm(vm_key), generation_c->wasmHandle());
  EXPECT_EQ(getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory),
            generation_c);
  clearWasmCachesForTesting();
}

TEST_P(TestVm, RecoverFailuresBelongToNewCloneAndCallbacksStayBalanced) {
  clearWasmCachesForTesting();
  const auto state = std::make_shared<ControlledFailureState>();
  const auto new_vm = [this]() { return makeVm(engine_); };
  std::shared_ptr<WasmHandleBase> last_clone;
  const auto clone_factory = makeControlledCloneFactory(new_vm, state, &last_clone);
  const auto plugin_factory = makePluginHandleFactory();
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  const std::string vm_key = "recover-failure-vm-key";
  auto base_handle = createWasm(vm_key, readTestWasmFile("abi_export.wasm"), plugin,
                                makeControlledWasmFactory(new_vm, state, "vm_id", "vm_config"),
                                clone_factory, false);
  ASSERT_TRUE(base_handle && base_handle->wasm());
  auto original = getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory);
  ASSERT_TRUE(original && original->wasm());
  const auto original_wasm = original->wasmHandle();
  original_wasm->wasm()->setShouldRebuild(true);

  state->clone_result = CloneResult::ReturnNull;
  last_clone.reset();
  std::shared_ptr<PluginHandleBase> rebuilt;
  EXPECT_FALSE(original->rebuild(rebuilt));
  EXPECT_EQ(rebuilt, nullptr);
  EXPECT_EQ(last_clone, nullptr);
  EXPECT_FALSE(base_handle->wasm()->isFailed());
  EXPECT_FALSE(original_wasm->wasm()->isFailed());

  state->clone_result = CloneResult::Uninitializable;
  EXPECT_FALSE(original->rebuild(rebuilt));
  ASSERT_TRUE(last_clone && last_clone->wasm());
  EXPECT_EQ(last_clone->wasm()->fail_state(), FailState::RecoverError);
  EXPECT_FALSE(base_handle->wasm()->isFailed());
  EXPECT_FALSE(original_wasm->wasm()->isFailed());

  state->clone_result = CloneResult::Normal;
  state->fail_start = true;
  EXPECT_FALSE(original->rebuild(rebuilt));
  ASSERT_TRUE(last_clone && last_clone->wasm());
  EXPECT_EQ(last_clone->wasm()->fail_state(), FailState::RecoverError);
  EXPECT_FALSE(base_handle->wasm()->isFailed());
  EXPECT_FALSE(original_wasm->wasm()->isFailed());

  state->fail_start = false;
  ASSERT_TRUE(original->rebuild(rebuilt));
  ASSERT_TRUE(rebuilt && rebuilt->wasm());
  const auto recovered_after_start = rebuilt;

  recovered_after_start->wasm()->setShouldRebuild(true);
  state->fail_configure = true;
  rebuilt.reset();
  EXPECT_FALSE(recovered_after_start->rebuild(rebuilt));
  ASSERT_TRUE(last_clone && last_clone->wasm());
  EXPECT_EQ(last_clone->wasm()->fail_state(), FailState::RecoverError);
  EXPECT_FALSE(base_handle->wasm()->isFailed());
  EXPECT_FALSE(recovered_after_start->wasm()->isFailed());

  state->fail_configure = false;
  ASSERT_TRUE(recovered_after_start->rebuild(rebuilt));
  ASSERT_TRUE(rebuilt && rebuilt->wasm());

  const auto final_wasm = rebuilt->wasmHandle();
  const std::string callback_key = vm_key + "||" + plugin->key();
  bool stale_callback_ran = false;
  final_wasm->wasm()->wasm_vm()->addFailCallback(
      callback_key, [&stale_callback_ran](FailState) { stale_callback_ran = true; });
  rebuilt.reset();
  final_wasm->wasm()->wasm_vm()->fail(FailState::RuntimeError, "post-destruction failure");
  EXPECT_FALSE(stale_callback_ran);
  clearWasmCachesForTesting();
}

TEST_P(TestVm, NormalCloneFailuresKeepBaseFailureSemantics) {
  clearWasmCachesForTesting();
  const auto state = std::make_shared<ControlledFailureState>();
  const auto new_vm = [this]() { return makeVm(engine_); };
  const auto clone_factory = makeControlledCloneFactory(new_vm, state);
  const auto plugin_factory = makePluginHandleFactory();
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  const auto wasm_factory = makeControlledWasmFactory(new_vm, state, "vm_id", "vm_config");

  auto clone_failure_base =
      createWasm("normal-clone-failure-vm-key", readTestWasmFile("abi_export.wasm"), plugin,
                 wasm_factory, clone_factory, false);
  ASSERT_TRUE(clone_failure_base && clone_failure_base->wasm());
  state->clone_result = CloneResult::ReturnNull;
  EXPECT_EQ(getOrCreateThreadLocalPlugin(clone_failure_base, plugin, clone_factory, plugin_factory),
            nullptr);
  EXPECT_EQ(clone_failure_base->wasm()->fail_state(), FailState::UnableToCloneVm);

  state->clone_result = CloneResult::Normal;
  auto initialize_failure_base =
      createWasm("normal-initialize-failure-vm-key", readTestWasmFile("abi_export.wasm"), plugin,
                 wasm_factory, clone_factory, false);
  ASSERT_TRUE(initialize_failure_base && initialize_failure_base->wasm());
  state->clone_result = CloneResult::Uninitializable;
  EXPECT_EQ(
      getOrCreateThreadLocalPlugin(initialize_failure_base, plugin, clone_factory, plugin_factory),
      nullptr);
  EXPECT_EQ(initialize_failure_base->wasm()->fail_state(), FailState::UnableToInitializeCode);
  clearWasmCachesForTesting();
}

TEST_P(TestVm, PluginHandleDestructionAfterKillIsSilent) {
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  auto wasm = std::make_shared<WasmBase>(makeVm(engine_), "vm_id", "vm_config", "kill-safe-vm-key",
                                         std::unordered_map<std::string, std::string>{},
                                         AllowedCapabilitiesMap{});
  auto wasm_handle = std::make_shared<WasmHandleBase>(std::move(wasm));
  auto plugin_handle = std::make_shared<PluginHandleBase>(wasm_handle, plugin);
  wasm_handle->kill();

  testing::internal::CaptureStderr();
  plugin_handle.reset();
  EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
}

TEST_P(TestVm, WasmBaseFailKeepsHostErrorHook) {
  auto wasm = std::make_shared<ErrorRecordingWasm>(
      makeVm(engine_), "vm_id", "vm_config", "error-hook-vm-key",
      std::unordered_map<std::string, std::string>{}, AllowedCapabilitiesMap{});

  wasm->fail(FailState::RuntimeError, "host error hook");
  EXPECT_EQ(wasm->fail_state(), FailState::RuntimeError);
  EXPECT_EQ(wasm->last_error, "host error hook");
}

TEST_P(TestVm, ThreadLocalPluginResultReturnsHandleForSuccessAndCacheHit) {
  clearWasmCachesForTesting();
  const auto state = std::make_shared<ControlledFailureState>();
  const auto new_vm = [this]() { return makeVm(engine_); };
  const auto clone_factory = makeControlledCloneFactory(new_vm, state);
  const auto plugin_factory = makePluginHandleFactory();
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  auto base_handle = createWasm(
      "result-success-vm-key", readTestWasmFile("abi_export.wasm"), plugin,
      makeControlledWasmFactory(new_vm, state, "vm_id", "vm_config"), clone_factory, false);
  ASSERT_TRUE(base_handle && base_handle->wasm());

  const auto created =
      getOrCreateThreadLocalPluginWithResult(base_handle, plugin, clone_factory, plugin_factory);
  ASSERT_TRUE(created.handle);
  EXPECT_EQ(created.fail_state, FailState::Ok);

  const auto cached =
      getOrCreateThreadLocalPluginWithResult(base_handle, plugin, clone_factory, plugin_factory);
  EXPECT_EQ(cached.handle, created.handle);
  EXPECT_EQ(cached.fail_state, FailState::Ok);
  EXPECT_EQ(getOrCreateThreadLocalPlugin(base_handle, plugin, clone_factory, plugin_factory),
            created.handle);
  clearWasmCachesForTesting();
}

TEST_P(TestVm, ThreadLocalPluginResultPreservesFailureReason) {
  clearWasmCachesForTesting();
  const auto state = std::make_shared<ControlledFailureState>();
  const auto new_vm = [this]() { return makeVm(engine_); };
  std::shared_ptr<WasmHandleBase> last_clone;
  const auto clone_factory = makeControlledCloneFactory(new_vm, state, &last_clone);
  const auto plugin_factory = makePluginHandleFactory();
  const auto plugin = std::make_shared<PluginBase>("plugin_name", "root_id", "vm_id", engine_,
                                                   "plugin_config", false, "plugin_key");
  const auto wasm_factory = makeControlledWasmFactory(new_vm, state, "vm_id", "vm_config");
  const auto source = readTestWasmFile("abi_export.wasm");

  struct FailureCase {
    const char *vm_key;
    CloneResult clone_result;
    bool fail_start;
    bool fail_configure;
    bool trap_start;
    bool trap_configure;
    FailState expected;
  };
  const FailureCase cases[] = {
      {"result-clone-null", CloneResult::ReturnNull, false, false, false, false,
       FailState::UnableToCloneVm},
      {"result-construction-failure", CloneResult::Uninitializable, false, false, false, false,
       FailState::UnableToCreateVm},
      {"result-initialize-failure", CloneResult::InitializeFailure, false, false, false, false,
       FailState::UnableToInitializeCode},
      {"result-start-rejected", CloneResult::Normal, true, false, false, false,
       FailState::StartFailed},
      {"result-configure-rejected", CloneResult::Normal, false, true, false, false,
       FailState::ConfigureFailed},
      {"result-start-trap", CloneResult::Normal, false, false, true, false,
       FailState::RuntimeError},
      {"result-configure-trap", CloneResult::Normal, false, false, false, true,
       FailState::RuntimeError},
  };

  for (const auto &test_case : cases) {
    *state = ControlledFailureState{};
    auto base_handle =
        createWasm(test_case.vm_key, source, plugin, wasm_factory, clone_factory, false);
    ASSERT_TRUE(base_handle && base_handle->wasm()) << test_case.vm_key;
    last_clone.reset();
    state->clone_result = test_case.clone_result;
    state->fail_start = test_case.fail_start;
    state->fail_configure = test_case.fail_configure;
    state->trap_start = test_case.trap_start;
    state->trap_configure = test_case.trap_configure;

    const auto result =
        getOrCreateThreadLocalPluginWithResult(base_handle, plugin, clone_factory, plugin_factory);
    EXPECT_EQ(result.handle, nullptr) << test_case.vm_key;
    EXPECT_EQ(result.fail_state, test_case.expected) << test_case.vm_key;
    EXPECT_NE(result.fail_state, FailState::Ok) << test_case.vm_key;
    EXPECT_EQ(getThreadLocalWasm(test_case.vm_key), nullptr) << test_case.vm_key;
    if (test_case.clone_result != CloneResult::ReturnNull) {
      ASSERT_TRUE(last_clone && last_clone->wasm()) << test_case.vm_key;
      EXPECT_EQ(last_clone->wasm()->fail_state(), test_case.expected) << test_case.vm_key;
    }
  }
  clearWasmCachesForTesting();
}

// Recover  only used for WasmVMs - not available for NullVM.
TEST_P(TestVm, RecoverCrashedThreadLocalWasm) {
  const auto *const plugin_name = "plugin_name";
  const auto *const root_id = "root_id";
  const auto *const vm_id = "vm_id";
  const auto *const vm_config = "vm_config";
  const auto *const plugin_config = "plugin_config";
  const auto fail_open = false;

  // Create a plugin.
  const auto plugin = std::make_shared<PluginBase>(plugin_name, root_id, vm_id, engine_,
                                                   plugin_config, fail_open, "plugin_key");

  // Define callbacks.
  WasmHandleFactory wasm_handle_factory =
      [this, vm_id, vm_config](std::string_view vm_key) -> std::shared_ptr<WasmHandleBase> {
    auto base_wasm = std::make_shared<WasmBase>(makeVm(engine_), vm_id, vm_config, vm_key,
                                                std::unordered_map<std::string, std::string>{},
                                                AllowedCapabilitiesMap{});
    return std::make_shared<WasmHandleBase>(base_wasm);
  };

  WasmHandleCloneFactory wasm_handle_clone_factory =
      [this](const std::shared_ptr<WasmHandleBase> &base_wasm_handle)
      -> std::shared_ptr<WasmHandleBase> {
    auto wasm = std::make_shared<WasmBase>(
        base_wasm_handle, [this]() -> std::unique_ptr<WasmVm> { return makeVm(engine_); });
    return std::make_shared<WasmHandleBase>(wasm);
  };

  PluginHandleFactory plugin_handle_factory =
      [](const std::shared_ptr<WasmHandleBase> &base_wasm,
         const std::shared_ptr<PluginBase> &plugin) -> std::shared_ptr<PluginHandleBase> {
    return std::make_shared<PluginHandleBase>(base_wasm, plugin);
  };

  // Read the minimal loadable binary.
  auto source = readTestWasmFile("abi_export.wasm");

  // Create base Wasm via createWasm.
  auto base_wasm_handle =
      createWasm("vm_key", source, plugin, wasm_handle_factory, wasm_handle_clone_factory, false);
  ASSERT_TRUE(base_wasm_handle && base_wasm_handle->wasm());

  // Create a thread local plugin.
  auto plugin_handle = getOrCreateThreadLocalPlugin(
      base_wasm_handle, plugin, wasm_handle_clone_factory, plugin_handle_factory);
  // Cause runtime crash.
  plugin_handle->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(plugin_handle->wasm()->isFailed());

  // do recover.
  std::shared_ptr<PluginHandleBase> new_plugin_handle;
  ASSERT_TRUE(plugin_handle->rebuild(new_plugin_handle));
  // Verify recover success.
  ASSERT_FALSE(new_plugin_handle->wasm()->isFailed());
  // Verify the pointer to WasmBase is different from the crashed one.
  ASSERT_NE(new_plugin_handle->wasm(), plugin_handle->wasm());

  // Cause runtime crash again.
  new_plugin_handle->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(new_plugin_handle->wasm()->isFailed());
  // Do recover again.
  std::shared_ptr<PluginHandleBase> new_plugin_handle2;
  ASSERT_TRUE(new_plugin_handle->rebuild(new_plugin_handle2));
  // Verify recover again success.
  ASSERT_FALSE(new_plugin_handle2->wasm()->isFailed());
  // Verify the pointer to WasmBase is different from the crashed one.
  ASSERT_NE(new_plugin_handle2->wasm(), new_plugin_handle->wasm());

  // This time, create another thread local plugin with *different* plugin key for the same vm_key.
  // This one should reuse the recovered VM.
  const auto another_plugin = std::make_shared<PluginBase>(
      plugin_name, root_id, vm_id, engine_, plugin_config, fail_open, "another_plugin_key");
  auto another_handle = getOrCreateThreadLocalPlugin(
      base_wasm_handle, another_plugin, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(another_handle && another_handle->plugin());
  ASSERT_FALSE(another_handle->wasm()->isFailed());
  // Verify the pointer to WasmBase is same with recovered one
  ASSERT_EQ(another_handle->wasm(), new_plugin_handle2->wasm());
  // Verify plugin handle is different
  ASSERT_NE(another_handle, new_plugin_handle2);

  // Cause runtime crash again.
  new_plugin_handle2->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(new_plugin_handle2->wasm()->isFailed());
  // Create another thread local plugin with *different* plugin key before recover.
  // This one also should not end up using the failed VM.
  auto another_handle2 = getOrCreateThreadLocalPlugin(
      base_wasm_handle, another_plugin, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(another_handle2 && another_handle2->plugin());
  ASSERT_FALSE(another_handle2->wasm()->isFailed());
  // Verify the pointer to WasmBase is different from the failed one.
  ASSERT_NE(another_handle2->wasm(), new_plugin_handle2->wasm());
  // Do recover again.
  std::shared_ptr<PluginHandleBase> new_plugin_handle3;
  ASSERT_TRUE(new_plugin_handle2->rebuild(new_plugin_handle3));
  // Verify the pointer to WasmBase is different from the crashed one.
  ASSERT_NE(new_plugin_handle3->wasm(), new_plugin_handle2->wasm());

  // Cause the another plugin with same vm_key crash.
  another_handle2->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(another_handle2->wasm()->isFailed());
  // Do recover again
  std::shared_ptr<PluginHandleBase> new_another_handle2;
  ASSERT_TRUE(another_handle2->rebuild(new_another_handle2));
  // Verify the pointer to WasmBase is different from the crashed one.
  ASSERT_NE(new_another_handle2->wasm(), another_handle2->wasm());

  // Cause the another plugin crash again
  new_another_handle2->wasm()->wasm_vm()->fail(FailState::RuntimeError, "runtime error msg");
  ASSERT_TRUE(new_another_handle2->wasm()->isFailed());
  // Create thread local plugin with same plugin key
  auto another_handle3 = getOrCreateThreadLocalPlugin(
      base_wasm_handle, another_plugin, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(another_handle3 && another_handle3->plugin());
  ASSERT_FALSE(another_handle3->wasm()->isFailed());
  // Verify the pointer to WasmBase is different from the failed one.
  ASSERT_NE(another_handle3->wasm(), new_another_handle2->wasm());
  // Do recover again.
  std::shared_ptr<PluginHandleBase> new_another_handle3;
  ASSERT_TRUE(new_another_handle2->rebuild(new_another_handle3));
  // Recover should reuse the plugin handle
  ASSERT_EQ(new_another_handle3, another_handle3);
}

// Tests the canary is always applied when making a call `createWasm`
TEST_P(TestVm, AlwaysApplyCanary) {
  // Use different root_id, but the others are the same
  const auto *const plugin_name = "plugin_name";

  const std::string root_ids[2] = {"root_id_1", "root_id_2"};
  const std::string vm_ids[2] = {"vm_id_1", "vm_id_2"};
  const std::string vm_configs[2] = {"vm_config_1", "vm_config_2"};
  const std::string plugin_configs[3] = {"plugin_config_1", "plugin_config_2",
                                         /* raising the error */ ""};
  const std::string plugin_keys[2] = {"plugin_key_1", "plugin_key_2"};
  const auto fail_open = false;

  // Define common callbacks
  auto canary_count = 0;
  WasmHandleCloneFactory wasm_handle_clone_factory_for_canary =
      [&canary_count, this](const std::shared_ptr<WasmHandleBase> &base_wasm_handle)
      -> std::shared_ptr<WasmHandleBase> {
    auto wasm = std::make_shared<TestWasm>(
        base_wasm_handle, [this]() -> std::unique_ptr<WasmVm> { return makeVm(engine_); });
    canary_count++;
    return std::make_shared<WasmHandleBase>(wasm);
  };

  PluginHandleFactory plugin_handle_factory =
      [](const std::shared_ptr<WasmHandleBase> &base_wasm,
         const std::shared_ptr<PluginBase> &plugin) -> std::shared_ptr<PluginHandleBase> {
    return std::make_shared<PluginHandleBase>(base_wasm, plugin);
  };

  // Read the minimal loadable binary.
  auto source = readTestWasmFile("canary_check.wasm");

  WasmHandleFactory wasm_handle_factory_baseline =
      [this, vm_ids, vm_configs](std::string_view vm_key) -> std::shared_ptr<WasmHandleBase> {
    auto base_wasm =
        std::make_shared<TestWasm>(makeVm(engine_), std::unordered_map<std::string, std::string>(),
                                   vm_ids[0], vm_configs[0], vm_key);
    return std::make_shared<WasmHandleBase>(base_wasm);
  };

  // Create a baseline plugin.
  const auto plugin_baseline = std::make_shared<PluginBase>(
      plugin_name, root_ids[0], vm_ids[0], engine_, plugin_configs[0], fail_open, plugin_keys[0]);

  const auto vm_key_baseline = makeVmKey(vm_ids[0], vm_configs[0], "common_code");
  // Create a base Wasm by createWasm.
  auto wasm_handle_baseline =
      createWasm(vm_key_baseline, source, plugin_baseline, wasm_handle_factory_baseline,
                 wasm_handle_clone_factory_for_canary, false);
  ASSERT_TRUE(wasm_handle_baseline && wasm_handle_baseline->wasm());

  // Check if it ran for baseline root context
  EXPECT_TRUE(TestContext::isGlobalLogged("onConfigure: " + root_ids[0]));
  // For each create Wasm, canary should be done.
  EXPECT_EQ(canary_count, 1);

  bool first = true;
  std::unordered_set<std::shared_ptr<WasmHandleBase>> reference_holder;

  for (const auto &root_id : root_ids) {
    for (const auto &vm_id : vm_ids) {
      for (const auto &vm_config : vm_configs) {
        for (const auto &plugin_key : plugin_keys) {
          for (const auto &plugin_config : plugin_configs) {
            canary_count = 0;
            TestContext::resetGlobalLog();
            WasmHandleFactory wasm_handle_factory_comp =
                [this, vm_id,
                 vm_config](std::string_view vm_key) -> std::shared_ptr<WasmHandleBase> {
              auto base_wasm = std::make_shared<TestWasm>(
                  makeVm(engine_), std::unordered_map<std::string, std::string>(), vm_id, vm_config,
                  vm_key);
              return std::make_shared<WasmHandleBase>(base_wasm);
            };
            const auto plugin_comp = std::make_shared<PluginBase>(
                plugin_name, root_id, vm_id, engine_, plugin_config, fail_open, plugin_key);
            const auto vm_key = makeVmKey(vm_id, vm_config, "common_code");
            // Create a base Wasm by createWasm.
            auto wasm_handle_comp =
                createWasm(vm_key, source, plugin_comp, wasm_handle_factory_comp,
                           wasm_handle_clone_factory_for_canary, false);
            // Validate that canarying is cached for the first baseline plugin variant.
            if (first) {
              first = false;
              EXPECT_EQ(canary_count, 0);
            } else {
              // For each create Wasm, canary should be done.
              EXPECT_EQ(canary_count, 1);
              EXPECT_TRUE(TestContext::isGlobalLogged("onConfigure: " + root_id));
            }

            if (plugin_config.empty()) {
              // canary_check.wasm should raise the error at `onConfigure` in canary when the
              // `plugin_config` is empty string.
              EXPECT_EQ(wasm_handle_comp, nullptr);
              continue;
            }

            ASSERT_TRUE(wasm_handle_comp && wasm_handle_comp->wasm());
            // Keep the reference of wasm_handle_comp in order to utilize the WasmHandleBase
            // cache of createWasm. If we don't keep the reference, WasmHandleBase and VM will be
            // destroyed for each iteration.
            reference_holder.insert(wasm_handle_comp);

            // Wasm VM is unique for vm_key.
            if (vm_key == vm_key_baseline) {
              EXPECT_EQ(wasm_handle_baseline->wasm(), wasm_handle_comp->wasm());
            } else {
              EXPECT_NE(wasm_handle_baseline->wasm(), wasm_handle_comp->wasm());
            }

            // plugin->key() is unique for root_id + plugin_config + plugin_key.
            // plugin->key() is used as an identifier of local-specific plugins as well.
            if (root_id == root_ids[0] && plugin_config == plugin_configs[0] &&
                plugin_key == plugin_keys[0]) {
              EXPECT_EQ(plugin_baseline->key(), plugin_comp->key());
            } else {
              EXPECT_NE(plugin_baseline->key(), plugin_comp->key());
            }
          }
        }
      }
    }
  }
}

// Check that there are no stale thread-local cache keys (eventually)
TEST_P(TestVm, CleanupThreadLocalCacheKeys) {
  const auto *const plugin_name = "plugin_name";
  const auto *const root_id = "root_id";
  const auto *const vm_id = "vm_id";
  const auto *const vm_config = "vm_config";
  const auto *const plugin_config = "plugin_config";
  const auto fail_open = false;

  WasmHandleFactory wasm_handle_factory =
      [this, vm_id, vm_config](std::string_view vm_key) -> std::shared_ptr<WasmHandleBase> {
    auto base_wasm = std::make_shared<WasmBase>(makeVm(engine_), vm_id, vm_config, vm_key,
                                                std::unordered_map<std::string, std::string>{},
                                                AllowedCapabilitiesMap{});
    return std::make_shared<WasmHandleBase>(base_wasm);
  };

  WasmHandleCloneFactory wasm_handle_clone_factory =
      [this](const std::shared_ptr<WasmHandleBase> &base_wasm_handle)
      -> std::shared_ptr<WasmHandleBase> {
    auto wasm = std::make_shared<WasmBase>(
        base_wasm_handle, [this]() -> std::unique_ptr<WasmVm> { return makeVm(engine_); });
    return std::make_shared<WasmHandleBase>(wasm);
  };

  PluginHandleFactory plugin_handle_factory =
      [](const std::shared_ptr<WasmHandleBase> &base_wasm,
         const std::shared_ptr<PluginBase> &plugin) -> std::shared_ptr<PluginHandleBase> {
    return std::make_shared<PluginHandleBase>(base_wasm, plugin);
  };

  // Read the minimal loadable binary.
  auto source = readTestWasmFile("abi_export.wasm");

  // Simulate a plugin lifetime.
  const auto plugin1 = std::make_shared<PluginBase>(plugin_name, root_id, vm_id, engine_,
                                                    plugin_config, fail_open, "plugin_1");
  auto base_wasm_handle1 =
      createWasm("vm_1", source, plugin1, wasm_handle_factory, wasm_handle_clone_factory, false);
  ASSERT_TRUE(base_wasm_handle1 && base_wasm_handle1->wasm());

  auto local_plugin1 = getOrCreateThreadLocalPlugin(
      base_wasm_handle1, plugin1, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(local_plugin1 && local_plugin1->plugin());
  local_plugin1.reset();

  auto stale_plugins_keys = staleLocalPluginsKeysForTesting();
  EXPECT_EQ(1, stale_plugins_keys.size());

  // Now we create another plugin with a slightly different key and expect that there are no stale
  // thread-local cache entries.
  const auto plugin2 = std::make_shared<PluginBase>(plugin_name, root_id, vm_id, engine_,
                                                    plugin_config, fail_open, "plugin_2");
  auto local_plugin2 = getOrCreateThreadLocalPlugin(
      base_wasm_handle1, plugin2, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(local_plugin2 && local_plugin2->plugin());

  stale_plugins_keys = staleLocalPluginsKeysForTesting();
  EXPECT_TRUE(stale_plugins_keys.empty());

  // Trigger deletion of the thread-local WasmVM cloned from base_wasm_handle1 by freeing objects
  // referencing it.
  local_plugin2.reset();

  auto stale_wasms_keys = staleLocalWasmsKeysForTesting();
  EXPECT_EQ(1, stale_wasms_keys.size());

  // Create another base WASM handle and invoke WASM thread-local cache key cleanup.
  auto base_wasm_handle2 =
      createWasm("vm_2", source, plugin2, wasm_handle_factory, wasm_handle_clone_factory, false);
  ASSERT_TRUE(base_wasm_handle2 && base_wasm_handle2->wasm());

  auto local_plugin3 = getOrCreateThreadLocalPlugin(
      base_wasm_handle2, plugin2, wasm_handle_clone_factory, plugin_handle_factory);
  ASSERT_TRUE(local_plugin3 && local_plugin3->plugin());

  stale_wasms_keys = staleLocalWasmsKeysForTesting();
  EXPECT_TRUE(stale_wasms_keys.empty());
}

// Regression test for higress-group/higress#4034: doAfterVmCallActions() must not
// spin forever when a queued action re-enqueues itself during the drain. With the old
// unguarded `while (!empty())` loop this loops forever; with the drain-to-local fix each
// call runs the snapshot exactly once and the re-added copy is deferred to the next drain.
TEST_P(TestVm, DoAfterVmCallActionsReentrySafe) {
  // WasmBase::doAfterVmCallActions() calls shared_from_this(), so the instance must be
  // owned by a std::shared_ptr (a stack WasmBase would crash).
  auto wasm = std::make_shared<TestWasm>(makeVm(engine_));

  int count = 0;
  std::function<void()> f;
  f = [&]() {
    ++count;
    // Re-enqueue self during the drain. Under the old code this would be picked up by the
    // same loop and never terminate.
    wasm->addAfterVmCallAction(f);
  };

  wasm->addAfterVmCallAction(f);
  // Snapshot {f} runs exactly once; the re-added copy lands in the now-empty member queue.
  wasm->doAfterVmCallActions();
  EXPECT_EQ(count, 1); // Returns after one pass (the old while-loop would never return).

  // The re-added copy runs on the next frame (and re-enqueues once more).
  wasm->doAfterVmCallActions();
  EXPECT_EQ(count, 2);
}

} // namespace proxy_wasm
