#include <signal.h>

#include <queue>

#include <glog/logging.h>

#include <gtest/gtest.h>

#include <tr1/functional>

#include "common/lock.hpp"
#include "common/zookeeper.hpp"

#include "tests/base_zookeeper_test.hpp"
#include "tests/jvm.hpp"
#include "tests/utils.hpp"
#include "tests/zookeeper_server.hpp"

using mesos::internal::test::mesosRoot;
using std::tr1::bind;
using std::tr1::function;

namespace params = std::tr1::placeholders;

namespace mesos {
namespace internal {
namespace test {

void BaseZooKeeperTest::SetUpTestCase()
{
  static Jvm* singleton;
  if (singleton == NULL) {
    std::vector<std::string> opts;

    std::string zkHome = mesosRoot + "/third_party/zookeeper-3.3.1";
    std::string classpath = "-Djava.class.path=" +
        zkHome + "/conf:" +
        zkHome + "/zookeeper-3.3.1.jar:" +
        zkHome + "/lib/log4j-1.2.15.jar";
    LOG(INFO) << "Using classpath setup: " << classpath << std::endl;
    opts.push_back(classpath);
    singleton = new Jvm(opts);
  }

  // TODO(John Sirois): Introduce a mechanism to contribute classpath
  // requirements to a singleton Jvm, then access the singleton here.
  jvm = singleton;
}


void BaseZooKeeperTest::SetUp()
{
  zks = new ZooKeeperServer(jvm);
  zks->startNetwork();
};


void BaseZooKeeperTest::TearDown()
{
  zks->shutdownNetwork();
  delete zks;
};


Jvm* BaseZooKeeperTest::jvm = NULL;


BaseZooKeeperTest::TestWatcher::TestWatcher()
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);
}


BaseZooKeeperTest::TestWatcher::~TestWatcher()
{
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}


void BaseZooKeeperTest::TestWatcher::process(ZooKeeper* zk,
                                             int type,
                                             int state,
                                             const std::string& path)
{
  Lock lock(&mutex);
  events.push(Event(type, state, path));
  pthread_cond_signal(&cond);
}


static bool isSessionState(const BaseZooKeeperTest::TestWatcher::Event& event,
                           int state)
{
  return event.type == ZOO_SESSION_EVENT && event.state == state;
}


void BaseZooKeeperTest::TestWatcher::awaitSessionEvent(int state)
{
  awaitEvent(bind(&isSessionState, params::_1, state));
}


static bool isCreated(const BaseZooKeeperTest::TestWatcher::Event& event,
                      const std::string& path)
{
  return event.type == ZOO_CHILD_EVENT && event.path == path;
}


void BaseZooKeeperTest::TestWatcher::awaitCreated(const std::string& path)
{
  awaitEvent(bind(&isCreated, params::_1, path));
}


BaseZooKeeperTest::TestWatcher::Event
BaseZooKeeperTest::TestWatcher::awaitEvent()
{
  Lock lock(&mutex);
  while (true) {
    while (events.empty()) {
      pthread_cond_wait(&cond, &mutex);
    }
    Event event = events.front();
    events.pop();
    return event;
  }
}


BaseZooKeeperTest::TestWatcher::Event
BaseZooKeeperTest::TestWatcher::awaitEvent(function<bool(Event)> matches)
{
  while (true) {
    Event event = awaitEvent();
    if (matches(event)) {
      return event;
    }
  }
}

} // namespace test
} // namespace internal
} // namespace mesos

