#include "mesh/shared/AmpParkUntil.h"

#include <gtest/gtest.h>

#include <deque>

namespace pp::mesh {
namespace {

// The posting waits repost themselves until they end; once ended, nothing they captured may stay
// alive. A strong self-capture kept every wait's callbacks — and the sessions they hold — forever.
class PostedWaitTest : public ::testing::Test {
protected:
  std::function<void(std::function<void()>)> PostIo() {
    return [this](std::function<void()> task) { queue_.push_back(std::move(task)); };
  }

  void RunQueue() {
    for (int i = 0; i < 1000 && !queue_.empty(); ++i) {
      auto task = std::move(queue_.front());
      queue_.pop_front();
      task();
    }
  }

  std::deque<std::function<void()>> queue_;
};

TEST_F(PostedWaitTest, AChannelOpenWaitReleasesItsCallbackOnceDone) {
  auto held = std::make_shared<int>(0);
  const std::weak_ptr<int> watch = held;
  bool open = false;
  bool settled_open = false;
  AmpScheduleWhenChannelOpen(
      PostIo(), {}, [&open] { return open; }, std::chrono::steady_clock::now() + std::chrono::seconds(30),
      [held = std::move(held), &settled_open](const bool is_open) { settled_open = is_open; });
  RunQueue(); // not open yet: the wait reposts itself
  ASSERT_FALSE(queue_.empty());
  open = true;
  RunQueue();
  EXPECT_TRUE(settled_open);
  EXPECT_TRUE(queue_.empty());
  EXPECT_TRUE(watch.expired()) << "the ended wait still holds its callback";
}

TEST_F(PostedWaitTest, AnUntilSettledWaitReleasesItsCallbackOnceSettled) {
  auto held = std::make_shared<int>(0);
  const std::weak_ptr<int> watch = held;
  auto settled = std::make_shared<std::atomic<bool>>(false);
  bool timed_out = false;
  AmpScheduleUntilSettled(PostIo(), {}, settled, std::chrono::steady_clock::now() + std::chrono::seconds(30),
                          [held = std::move(held), &timed_out] { timed_out = true; });
  RunQueue();
  ASSERT_FALSE(queue_.empty());
  settled->store(true);
  RunQueue();
  EXPECT_FALSE(timed_out);
  EXPECT_TRUE(queue_.empty());
  EXPECT_TRUE(watch.expired()) << "the settled wait still holds its timeout";
}

} // namespace
} // namespace pp::mesh
