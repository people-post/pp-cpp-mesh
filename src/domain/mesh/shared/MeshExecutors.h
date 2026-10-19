#pragma once

#include "common/Executor.h"

#include <functional>
#include <memory>
#include <utility>

namespace pbr {

/**
 * Where one mesh hands work off its I/O lane (THREADING.md § Mesh threads), given by its host per
 * instance. Nothing posted here may wait on the mesh.
 *
 * A null executor runs the task inline on the caller: manual-drive test harnesses, which have no
 * threads to offer.
 */
struct MeshExecutors {
  /** Short and bounded: signature checks, disk, a hop to escape the I/O lane's lock order. */
  std::shared_ptr<pp::Executor> work;
  /** Waits on the outside world, for seconds: HTTP, UPnP discovery. */
  std::shared_ptr<pp::Executor> blocking;
};

/** Run `task` on `executor`, or inline when there is none. */
inline void PostOrRun(const std::shared_ptr<pp::Executor>& executor, std::function<void()> task) {
  if (!task) {
    return;
  }
  if (executor) {
    executor->Post(std::move(task));
  } else {
    task();
  }
}

} // namespace pbr
