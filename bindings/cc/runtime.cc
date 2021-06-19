#include "runtime.h"

#include "thread.h"

namespace rt {

// initializes the runtime
int RuntimeInit(std::string cfg_path, std::function<void()> main_func) {
  auto *func_copy = new std::function<void()>(main_func);
  return runtime_init(cfg_path.c_str(), thread_internal::ThreadTrampoline,
                      reinterpret_cast<void *>(func_copy));
}

}  // namespace rt
