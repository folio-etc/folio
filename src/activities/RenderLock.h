#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  // Sentinel for the dry-run render pass. Constructs a lock object that
  // satisfies render(RenderLock&&) without taking the rendering mutex —
  // the outer real-render call still holds the mutex across both passes.
  struct DryRun {};

  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  explicit RenderLock(DryRun);     // no-op: does not acquire the mutex
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
};
