#ifndef PROXY_SCHEDULER_HPP
#define PROXY_SCHEDULER_HPP

#ifdef __APPLE__
// Since Python.h defines _XOPEN_SOURCE on Mac OS X, we undefine it
// here so that we don't get warning messages during the build.
#undef _XOPEN_SOURCE
#endif // __APPLE__
#include <Python.h>

#include <string>
#include <vector>

#include <mesos/scheduler.hpp>


namespace mesos { namespace python {

struct MesosSchedulerDriverImpl;

/**
 * Proxy Scheduler implementation that will call into Python
 */
class ProxyScheduler : public Scheduler
{
  MesosSchedulerDriverImpl *impl;

public:
  ProxyScheduler(MesosSchedulerDriverImpl *_impl) : impl(_impl) {}

  virtual ~ProxyScheduler() {}

  // Callbacks for various Mesos events.
  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId);

  virtual void resourceOffers(SchedulerDriver* driver,
                              const std::vector<Offer>& offers);

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId);

  virtual void statusUpdate(SchedulerDriver* driver,
                            const TaskStatus& status);

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const SlaveID& slaveId,
                                const ExecutorID& executorId,
                                const std::string& data);

  virtual void slaveLost(SchedulerDriver* driver,
                         const SlaveID& slaveId);

  virtual void error(SchedulerDriver* driver,
                     int code,
                     const std::string& message);

};

}} /* namespace mesos { namespace python { */

#endif /* PROXY_SCHEDULER_HPP */
