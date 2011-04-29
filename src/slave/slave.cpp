#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>

#include <google/protobuf/descriptor.h>

#include "slave.hpp"
#include "webui.hpp"

#include "common/utils.hpp"

// There's no gethostbyname2 on Solaris, so fake it by calling gethostbyname
#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using process::HttpOKResponse;
using process::HttpResponse;
using process::HttpRequest;
using process::PID;
using process::Process;
using process::Promise;
using process::UPID;

using std::list;
using std::make_pair;
using std::ostringstream;
using std::pair;
using std::queue;
using std::string;
using std::vector;


namespace mesos { namespace internal { namespace slave {

class ExecutorReaper : public Process<ExecutorReaper>
{
public:
  ExecutorReaper(const PID<Slave>& _slave)
    : slave(_slave) {}

  void reap(const FrameworkID& frameworkId, 
            const ExecutorID& executorId,
            pid_t pid)
  {
    LOG(INFO) << "Monitoring process " << pid << " for reaping";
    pids[pid] = make_pair(frameworkId, executorId);
  }

protected:
  virtual void operator () ()
  {
    link(slave);
    while (true) {
      serve(1);
      if (name() == process::TIMEOUT) {
        // Check whether any child process has exited.
        pid_t pid;
        int status;
        if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
          if (pids.count(pid) > 0) {
            const FrameworkID& frameworkId = pids[pid].first;
            const ExecutorID& executorId = pids[pid].second;

            LOG(INFO) << "Telling slave of exited executor " << executorId
                      << " of framework " << frameworkId;
            process::dispatch(slave, &Slave::executorExited,
                              frameworkId, executorId, status);

            pids.erase(pid);
          }
        } else {
          std::cout << "waitpid returned nothing" << std::endl;
        }
      } else if (name() == process::TERMINATE || name() == process::EXITED) {
        return;
      }
    }
  }

private:
  const PID<Slave> slave;
  unordered_map<pid_t, pair<FrameworkID, ExecutorID> > pids;
};


// class ExecutorHandler : public Process<ExecutorHandler>
// {
// public:
//   ExecutorHandler(const PID<Slave>& slave,
//                   const FrameworkID& frameworkId,
//                   const FrameworkInfo& frameworkInfo,
//                   const ExecutorInfo& executorInfo,
//                   const string& directory, 
//                   pid_t pid)
//   {

//   }

// public:
// //   void acknowledged(const TaskID& taskId)
// //   {

// //   }

// protected:
//   void operator () ()
//   {
//     // write pid into 'directory/pid'

//     // loop forever reading from 'directory/statuses'

//     // pass the status information back to the slave, get a future
//     // that tells us if/when that status has been acknowledged

//     // wait on that future (regardless of writes into statuses)

//     pause(1);
//     // read from statuses
//   }

// private:
//   const PID<Slave> slave;
  
// };

}}} // namespace mesos { namespace internal { namespace slave {


Slave::Slave(const Resources& _resources, bool _local,
             IsolationModule *_isolationModule)
  : MesosProcess<Slave>("slave"),
    resources(_resources), local(_local), isolationModule(_isolationModule)
{
  initialize();
}


Slave::Slave(const Configuration& _conf, bool _local,
             IsolationModule* _isolationModule)
  : MesosProcess<Slave>("slave"),
    conf(_conf), local(_local), isolationModule(_isolationModule)
{
  resources =
    Resources::parse(conf.get<string>("resources", "cpus:1;mem:1024"));

  initialize();
}


void Slave::registerOptions(Configurator* configurator)
{
  // TODO(benh): Is there a way to specify units for the resources?
  configurator->addOption<string>("resources",
                                  "Total consumable resources per slave\n");
//   configurator->addOption<string>("attributes",
//                                   "Attributes of machine\n");
  configurator->addOption<string>("work_dir",
                                  "Where to place framework work directories\n"
                                  "(default: MESOS_HOME/work)");
  configurator->addOption<string>("hadoop_home",
                                  "Where to find Hadoop installed (for\n"
                                  "fetching framework executors from HDFS)\n"
                                  "(default: look for HADOOP_HOME in\n"
                                  "environment or find hadoop on PATH)");
  configurator->addOption<bool>("switch_user", 
                                "Whether to run tasks as the user who\n"
                                "submitted them rather than the user running\n"
                                "the slave (requires setuid permission)",
                                true);
  configurator->addOption<string>("frameworks_home",
                                  "Directory prepended to relative executor\n"
                                  "paths (default: MESOS_HOME/frameworks)");
}


Slave::~Slave()
{
  // TODO(benh): Shut down and free frameworks?

  // TODO(benh): Shut down and free executors? The executor should get
  // an "exited" event and initiate shutdown itself.

  CHECK(reaper != NULL);
  process::post(reaper->self(), process::TERMINATE);
  process::wait(reaper->self());
  delete reaper;
}


Promise<state::SlaveState*> Slave::getState()
{
  Resources resources(this->resources);
  Resource::Scalar cpus;
  Resource::Scalar mem;
  cpus.set_value(0);
  mem.set_value(0);
  cpus = resources.getScalar("cpus", cpus);
  mem = resources.getScalar("mem", mem);

  state::SlaveState* state =
    new state::SlaveState(build::DATE, build::USER, slaveId.value(),
                          cpus.value(), mem.value(), self(), master);

  foreachpair (_, Framework* f, frameworks) {
    foreachpair (_, Executor* e, f->executors) {
      Resources resources(e->resources);
      Resource::Scalar cpus;
      Resource::Scalar mem;
      cpus.set_value(0);
      mem.set_value(0);
      cpus = resources.getScalar("cpus", cpus);
      mem = resources.getScalar("mem", mem);

      // TOOD(benh): For now, we will add a state::Framework object
      // for each executor that the framework has. Therefore, we tweak
      // the framework ID to also include the associated executor ID
      // to differentiate them. This is so we don't have to make very
      // many changes to the webui right now. Note that this ID
      // construction must be identical to what we do for directory
      // suffix returned from Slave::getUniqueWorkDirectory.

      string id = f->frameworkId.value() + "-" + e->info.executor_id().value();

      state::Framework* framework =
        new state::Framework(id, f->info.name(),
                             e->info.uri(), "",
                             cpus.value(), mem.value());

      state->frameworks.push_back(framework);

      foreachpair (_, Task* t, e->tasks) {
        Resources resources(t->resources());
        Resource::Scalar cpus;
        Resource::Scalar mem;
        cpus.set_value(0);
        mem.set_value(0);
        cpus = resources.getScalar("cpus", cpus);
        mem = resources.getScalar("mem", mem);

        state::Task* task =
          new state::Task(t->task_id().value(), t->name(),
                          TaskState_descriptor()->FindValueByNumber(t->state())->name(),
                          cpus.value(), mem.value());

        framework->tasks.push_back(task);
      }
    }
  }

  return state;
}


void Slave::initialize()
{
  // Setup the executor reaper.
  reaper = new ExecutorReaper(self());
  process::spawn(reaper);

  // Start all the statistics at 0.
  statistics.launched_tasks = 0;
  statistics.finished_tasks = 0;
  statistics.killed_tasks = 0;
  statistics.failed_tasks = 0;
  statistics.lost_tasks = 0;
  statistics.valid_status_updates = 0;
  statistics.invalid_status_updates = 0;
  statistics.valid_framework_messages = 0;
  statistics.invalid_framework_messages = 0;

  startTime = elapsedTime();

  install(NEW_MASTER_DETECTED, &Slave::newMasterDetected,
          &NewMasterDetectedMessage::pid);

  install(NO_MASTER_DETECTED, &Slave::noMasterDetected);

  install(M2S_REGISTER_REPLY, &Slave::registerReply,
          &SlaveRegisteredMessage::slave_id);

  install(M2S_REREGISTER_REPLY, &Slave::reregisterReply,
          &SlaveRegisteredMessage::slave_id);

  install(M2S_RUN_TASK, &Slave::runTask,
          &RunTaskMessage::framework,
          &RunTaskMessage::framework_id,
          &RunTaskMessage::pid,
          &RunTaskMessage::task);

  install(M2S_KILL_TASK, &Slave::killTask,
          &KillTaskMessage::framework_id,
          &KillTaskMessage::task_id);

  install(M2S_KILL_FRAMEWORK, &Slave::killFramework,
          &KillFrameworkMessage::framework_id);

  install(M2S_FRAMEWORK_MESSAGE, &Slave::schedulerMessage,
          &FrameworkMessageMessage::slave_id,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::executor_id,
          &FrameworkMessageMessage::data);

  install(M2S_UPDATE_FRAMEWORK, &Slave::updateFramework,
          &UpdateFrameworkMessage::framework_id,
          &UpdateFrameworkMessage::pid);

  install(M2S_STATUS_UPDATE_ACK, &Slave::statusUpdateAck,
          &StatusUpdateAckMessage::framework_id,
          &StatusUpdateAckMessage::slave_id,
          &StatusUpdateAckMessage::task_id);

  install(E2S_REGISTER_EXECUTOR, &Slave::registerExecutor,
          &RegisterExecutorMessage::framework_id,
          &RegisterExecutorMessage::executor_id);

  install(E2S_STATUS_UPDATE, &Slave::statusUpdate,
          &StatusUpdateMessage::framework_id,
          &StatusUpdateMessage::status);

  install(E2S_FRAMEWORK_MESSAGE, &Slave::executorMessage,
          &FrameworkMessageMessage::slave_id,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::executor_id,
          &FrameworkMessageMessage::data);

  install(PING, &Slave::ping);

  install(process::TIMEOUT, &Slave::timeout);

  install(process::EXITED, &Slave::exited);

  installHttpHandler("info.json", &Slave::http_info_json);
  installHttpHandler("frameworks.json", &Slave::http_frameworks_json);
  installHttpHandler("tasks.json", &Slave::http_tasks_json);
  installHttpHandler("stats.json", &Slave::http_stats_json);
  installHttpHandler("vars", &Slave::http_vars);
}


void Slave::operator () ()
{
  LOG(INFO) << "Slave started at " << self();
  LOG(INFO) << "Slave resources: " << resources;

  // Get our hostname
  char buf[512];
  gethostname(buf, sizeof(buf));
  hostent* he = gethostbyname2(buf, AF_INET);
  string hostname = he->h_name;

  // Check and see if we have a different public DNS name. Normally
  // this is our hostname, but on EC2 we look for the MESOS_PUBLIC_DNS
  // environment variable. This allows the master to display our
  // public name in its web UI.
  string public_hostname = hostname;
  if (getenv("MESOS_PUBLIC_DNS") != NULL) {
    public_hostname = getenv("MESOS_PUBLIC_DNS");
  }

  // Initialize slave info.
  slave.set_hostname(hostname);
  slave.set_public_hostname(public_hostname);
  slave.mutable_resources()->MergeFrom(resources);

  // Initialize isolation module.
  isolationModule->initialize(self(), conf, local);

  while (true) {
    serve(1);
    if (name() == process::TERMINATE) {
      LOG(INFO) << "Asked to shut down by " << from();
      foreachpaircopy (_, Framework* framework, frameworks) {
        killFramework(framework);
      }
      return;
    }
  }
}


void Slave::newMasterDetected(const string& pid)
{
  LOG(INFO) << "New master detected at " << pid;

  master = pid;
  link(master);

  if (slaveId == "") {
    // Slave started before master.
    MSG<S2M_REGISTER_SLAVE> out;
    out.mutable_slave()->MergeFrom(slave);
    send(master, out);
  } else {
    // Re-registering, so send tasks running.
    MSG<S2M_REREGISTER_SLAVE> out;
    out.mutable_slave_id()->MergeFrom(slaveId);
    out.mutable_slave()->MergeFrom(slave);

    foreachpair (_, Framework* framework, frameworks) {
      foreachpair (_, Executor* executor, framework->executors) {
	foreachpair (_, Task* task, executor->tasks) {
	  out.add_tasks()->MergeFrom(*task);
	}
      }
    }

    send(master, out);
  }
}


void Slave::noMasterDetected()
{
  LOG(INFO) << "Lost master(s) ... waiting";
}


void Slave::registerReply(const SlaveID& slaveId)
{
  LOG(INFO) << "Registered with master; given slave ID " << slaveId;
  this->slaveId = slaveId;
}


void Slave::reregisterReply(const SlaveID& slaveId)
{
  LOG(INFO) << "Re-registered with master";

  if (!(this->slaveId == slaveId)) {
    LOG(FATAL) << "Slave re-registered but got wrong ID";
  }
}


void Slave::runTask(const FrameworkInfo& frameworkInfo,
                    const FrameworkID& frameworkId,
                    const string& pid,
                    const TaskDescription& task)
{
  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    framework = new Framework(frameworkId, frameworkInfo, pid);
    frameworks[frameworkId] = framework;
  }

  // Either send the task to an executor or start a new executor
  // and queue the task until the executor has started.
  Executor* executor = task.has_executor()
    ? framework->getExecutor(task.executor().executor_id())
    : framework->getExecutor(framework->info.executor().executor_id());
        
  if (executor != NULL) {
    if (!executor->pid) {
      // Queue task until the executor starts up.
      executor->queuedTasks.push_back(task);
    } else {
      // Add the task to the executor.
      executor->addTask(task);

      MSG<S2E_RUN_TASK> out;
      out.mutable_framework()->MergeFrom(framework->info);
      out.mutable_framework_id()->MergeFrom(framework->frameworkId);
      out.set_pid(framework->pid);
      out.mutable_task()->MergeFrom(task);
      send(executor->pid, out);
      isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                        executor->info, executor->resources);
    }
  } else {
    // Launch an executor for this task.
    if (task.has_executor()) {
      executor = framework->createExecutor(task.executor());
    } else {
      executor = framework->createExecutor(framework->info.executor());
    }

    // Queue task until the executor starts up.
    executor->queuedTasks.push_back(task);

    // Determine the working directory for this executor.
    const string& directory = getUniqueWorkDirectory(framework->frameworkId,
                                                     executor->info.executor_id());

    // Tell the isolation module to launch the executor. (TODO(benh):
    // Let the isolation module possibly block by calling this from the executor handler.)
    pid_t pid = isolationModule->launchExecutor(framework->frameworkId, framework->info,
                                                executor->info, directory);

    // For now, an isolation module returning 0 effectively indicates
    // that the slave shouldn't try and reap it to determine if it has
    // exited, but instead that will be done another way.

    // Tell the executor reaper to monitor/reap this process.
    if (pid != 0) {
      process::dispatch(reaper->self(), &ExecutorReaper::reap,
                        framework->frameworkId, executor->info.executor_id(), pid);
    }

//     // Create an executor handler to monitor the process(es) and read
//     // it's task statuses, etc.
//     executor->handler =
//       new ExecutorHandler(framework->frameworkId, framework->info,
//                           executor->info, directory, pid);

//     process::spawn(executor->handler);
  }
}


void Slave::killTask(const FrameworkID& frameworkId,
                     const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    // Tell the executor to kill the task if it is up and
    // running, otherwise, consider the task lost.
    Executor* executor = framework->getExecutor(taskId);
    if (executor == NULL || !executor->pid) {
      // Update the resources locally, if an executor comes up
      // after this then it just won't receive this task.
      executor->removeTask(taskId);
      isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                        executor->info, executor->resources);

      MSG<S2M_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      TaskStatus *status = out.mutable_status();
      status->mutable_task_id()->MergeFrom(taskId);
      status->mutable_slave_id()->MergeFrom(slaveId);
      status->set_state(TASK_LOST);
      send(master, out);

      double deadline = elapsedTime() + STATUS_UPDATE_RETRY_TIMEOUT;
      framework->statuses[deadline][status->task_id()] = *status;
    } else {
      // Otherwise, send a message to the executor and wait for
      // it to send us a status update.
      MSG<S2E_KILL_TASK> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_task_id()->MergeFrom(taskId);
      send(executor->pid, out);
    }
  } else {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";

    MSG<S2M_STATUS_UPDATE> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    TaskStatus *status = out.mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->mutable_slave_id()->MergeFrom(slaveId);
    status->set_state(TASK_LOST);
    send(master, out);

    double deadline = elapsedTime() + STATUS_UPDATE_RETRY_TIMEOUT;
    framework->statuses[deadline][status->task_id()] = *status;
  }
}


void Slave::killFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to kill framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    killFramework(framework);
  }
}


void Slave::schedulerMessage(const SlaveID& slaveId,
			     const FrameworkID& frameworkId,
			     const ExecutorID& executorId,
                             const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);
    if (executor == NULL) {
      LOG(WARNING) << "Dropping message for executor '"
                   << executorId << "' of framework " << frameworkId
                   << " because executor does not exist";
      statistics.invalid_framework_messages++;
    } else if (!executor->pid) {
      // TODO(*): If executor is not started, queue framework message?
      // (It's probably okay to just drop it since frameworks can have
      // the executor send a message to the master to say when it's ready.)
      LOG(WARNING) << "Dropping message for executor '"
                   << executorId << "' of framework " << frameworkId
                   << " because executor is not running";
      statistics.invalid_framework_messages++;
    } else {
      MSG<S2E_FRAMEWORK_MESSAGE> out;
      out.mutable_slave_id()->MergeFrom(slaveId);
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_executor_id()->MergeFrom(executorId);
      out.set_data(data);
      send(executor->pid, out);

      statistics.valid_framework_messages++;
    }
  } else {
    LOG(WARNING) << "Dropping message for framework "<< frameworkId
                 << " because framework does not exist";
    statistics.invalid_framework_messages++;
  }
}


void Slave::updateFramework(const FrameworkID& frameworkId,
                            const string& pid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Updating framework " << frameworkId
              << " pid to " <<pid;
    framework->pid = pid;
  }
}


void Slave::statusUpdateAck(const FrameworkID& frameworkId,
                            const SlaveID& slaveId,
                            const TaskID& taskId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    foreachpair (double deadline, _, framework->statuses) {
      if (framework->statuses[deadline].count(taskId) > 0) {
        LOG(INFO) << "Got acknowledgement of status update"
                  << " for task " << taskId
                  << " of framework " << framework->frameworkId;
        framework->statuses[deadline].erase(taskId);
        break;
      }
    }
  }
}


void Slave::registerExecutor(const FrameworkID& frameworkId,
                             const ExecutorID& executorId)
{
  LOG(INFO) << "Got registration for executor '" << executorId
            << "' of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);

    // Check the status of the executor.
    if (executor == NULL) {
      LOG(WARNING) << "Not expecting executor '" << executorId
                   << "' of framework " << frameworkId;
      send(from(), S2E_KILL_EXECUTOR);
    } else if (executor->pid != UPID()) {
      LOG(WARNING) << "Not good, executor '" << executorId
                   << "' of framework " << frameworkId
                   << " is already running";
      send(from(), S2E_KILL_EXECUTOR);
    } else {
      // Save the pid for the executor.
      executor->pid = from();

      // Now that the executor is up, set its resource limits.
      isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                        executor->info, executor->resources);

      // Tell executor it's registered and give it any queued tasks.
      MSG<S2E_REGISTER_REPLY> out;
      ExecutorArgs* args = out.mutable_args();
      args->mutable_framework_id()->MergeFrom(framework->frameworkId);
      args->mutable_executor_id()->MergeFrom(executor->info.executor_id());
      args->mutable_slave_id()->MergeFrom(slaveId);
      args->set_hostname(slave.hostname());
      args->set_data(framework->info.executor().data());
      send(executor->pid, out);
      sendQueuedTasks(framework, executor);
    }
  } else {
    // Framework is gone; tell the executor to exit.
    LOG(WARNING) << "Framework " << frameworkId
                 << " does not exist (it may have been killed),"
                 << " telling executor to exit";

    // TODO(benh): Don't we also want to tell the isolation
    // module to shut this guy down!
    send(from(), S2E_KILL_EXECUTOR);
  }
}


void Slave::statusUpdate(const FrameworkID& frameworkId,
                         const TaskStatus& status)
{
  LOG(INFO) << "Status update: task " << status.task_id()
            << " of framework " << frameworkId
            << " is now in state "
            << TaskState_descriptor()->FindValueByNumber(status.state())->name();

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(status.task_id());
    if (executor != NULL) {
      executor->updateTaskState(status.task_id(), status.state());

      // Remove the task if necessary, and update statistics.
      switch (status.state()) {
        case TASK_FINISHED:
          statistics.finished_tasks++;
          executor->removeTask(status.task_id());
          isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                            executor->info, executor->resources);
          break;
        case TASK_FAILED:
          statistics.failed_tasks++;
          executor->removeTask(status.task_id());
          isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                            executor->info, executor->resources);
          break;
       case TASK_KILLED:
         statistics.killed_tasks++;
         executor->removeTask(status.task_id());
         isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                           executor->info, executor->resources);
         break;
        case TASK_LOST:
          statistics.lost_tasks++;
          executor->removeTask(status.task_id());
          isolationModule->resourcesChanged(framework->frameworkId, framework->info,
                                            executor->info, executor->resources);
          break;
      }

      // Send message and record the status for possible resending.
      MSG<S2M_STATUS_UPDATE> out;
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_status()->MergeFrom(status);
      send(master, out);

      double deadline = elapsedTime() + STATUS_UPDATE_RETRY_TIMEOUT;
      framework->statuses[deadline][status.task_id()] = status;

      statistics.valid_status_updates++;
    } else {
      LOG(WARNING) << "Status update error: couldn't lookup "
                   << "executor for framework " << frameworkId;
      statistics.invalid_status_updates++;
    }
  } else {
    LOG(WARNING) << "Status update error: couldn't lookup "
                 << "framework " << frameworkId;
    statistics.invalid_status_updates++;
  }
}


void Slave::executorMessage(const SlaveID& slaveId,
			    const FrameworkID& frameworkId,
			    const ExecutorID& executorId,
                            const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Sending message for framework " << frameworkId
              << " to " << framework->pid;

    // TODO(benh): This is weird, sending an M2F message.
    MSG<M2F_FRAMEWORK_MESSAGE> out;
    out.mutable_slave_id()->MergeFrom(slaveId);
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_executor_id()->MergeFrom(executorId);
    out.set_data(data);
    send(framework->pid, out);

    statistics.valid_framework_messages++;
  } else {
    LOG(WARNING) << "Cannot send framework message from slave "
                 << slaveId << " to framework " << frameworkId
                 << " because framework does not exist";
    statistics.invalid_framework_messages++;
  }
}


void Slave::ping()
{
  send(from(), PONG);
}


void Slave::timeout()
{
  // Check and see if we should re-send any status updates.
  foreachpair (_, Framework* framework, frameworks) {
    foreachpair (double deadline, _, framework->statuses) {
      if (deadline <= elapsedTime()) {
        foreachpair (_, const TaskStatus& status, framework->statuses[deadline]) {
          LOG(WARNING) << "Resending status update"
                       << " for task " << status.task_id()
                       << " of framework " << framework->frameworkId;
          MSG<S2M_STATUS_UPDATE> out;
          out.mutable_framework_id()->MergeFrom(framework->frameworkId);
          out.mutable_status()->MergeFrom(status);
          send(master, out);
        }
      }
    }
  }
}

void Slave::exited()
{
  LOG(INFO) << "Process exited: " << from();

  if (from() == master) {
    LOG(WARNING) << "Master disconnected! "
                 << "Waiting for a new master to be elected.";
    // TODO(benh): After so long waiting for a master, commit suicide.
  }
}


Promise<HttpResponse> Slave::http_info_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/info.json'";

  ostringstream out;

  out <<
    "{" <<
    "\"built_date\":\"" << build::DATE << "\"," <<
    "\"build_user\":\"" << build::USER << "\"," <<
    "\"start_time\":\"" << startTime << "\"," <<
    "\"pid\":\"" << self() << "\"" <<
    "}";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_frameworks_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/frameworks.json'";

  ostringstream out;

  out << "[";

  foreachpair (_, Framework* framework, frameworks) {
    out <<
      "{" <<
      "\"id\":\"" << framework->frameworkId << "\"," <<
      "\"name\":\"" << framework->info.name() << "\"," <<
      "\"user\":\"" << framework->info.user() << "\""
      "},";
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (frameworks.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_tasks_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/tasks.json'";

  ostringstream out;

  out << "[";

  foreachpair (_, Framework* framework, frameworks) {
    foreachpair (_, Executor* executor, framework->executors) {
      foreachpair (_, Task* task, executor->tasks) {
        // TODO(benh): Send all of the resources (as JSON).
        Resources resources(task->resources());
        Resource::Scalar cpus = resources.getScalar("cpus", Resource::Scalar());
        Resource::Scalar mem = resources.getScalar("mem", Resource::Scalar());
        const string& state =
          TaskState_descriptor()->FindValueByNumber(task->state())->name();
        out <<
          "{" <<
          "\"task_id\":\"" << task->task_id() << "\"," <<
          "\"framework_id\":\"" << task->framework_id() << "\"," <<
          "\"slave_id\":\"" << task->slave_id() << "\"," <<
          "\"name\":\"" << task->name() << "\"," <<
          "\"state\":\"" << state << "\"," <<
          "\"cpus\":" << cpus.value() << "," <<
          "\"mem\":" << mem.value() <<
          "},";
      }
    }
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (frameworks.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_stats_json(const HttpRequest& request)
{
  LOG(INFO) << "Http request for '/slave/stats.json'";

  ostringstream out;

  out <<
    "{" <<
    "\"uptime\":" << elapsedTime() - startTime << "," <<
    "\"total_frameworks\":" << frameworks.size() << "," <<
    "\"launched_tasks\":" << statistics.launched_tasks << "," <<
    "\"finished_tasks\":" << statistics.finished_tasks << "," <<
    "\"killed_tasks\":" << statistics.killed_tasks << "," <<
    "\"failed_tasks\":" << statistics.failed_tasks << "," <<
    "\"lost_tasks\":" << statistics.lost_tasks << "," <<
    "\"valid_status_updates\":" << statistics.valid_status_updates << "," <<
    "\"invalid_status_updates\":" << statistics.invalid_status_updates << "," <<
    "\"valid_framework_messages\":" << statistics.valid_framework_messages << "," <<
    "\"invalid_framework_messages\":" << statistics.invalid_framework_messages <<
    "}";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_vars(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/vars'";

  ostringstream out;

  out <<
    "build_date " << build::DATE << "\n" <<
    "build_user " << build::USER << "\n" <<
    "build_flags " << build::FLAGS << "\n";

  // Also add the configuration values.
  foreachpair (const string& key, const string& value, conf.getMap()) {
    out << key << " " << value << "\n";
  }

  out <<
    "uptime " << elapsedTime() - startTime << "\n" <<
    "total_frameworks " << frameworks.size() << "\n" <<
    "launched_tasks " << statistics.launched_tasks << "\n" <<
    "finished_tasks " << statistics.finished_tasks << "\n" <<
    "killed_tasks " << statistics.killed_tasks << "\n" <<
    "failed_tasks " << statistics.failed_tasks << "\n" <<
    "lost_tasks " << statistics.lost_tasks << "\n" <<
    "valid_status_updates " << statistics.valid_status_updates << "\n" <<
    "invalid_status_updates " << statistics.invalid_status_updates << "\n" <<
    "valid_framework_messages " << statistics.valid_framework_messages << "\n" <<
    "invalid_framework_messages " << statistics.invalid_framework_messages << "\n";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = lexical_cast<string>(out.str().size());
  response.body = out.str().data();
  return response;
}


Framework* Slave::getFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  }

  return NULL;
}


// Send any tasks queued up for the given framework to its executor
// (needed if we received tasks while the executor was starting up)
void Slave::sendQueuedTasks(Framework* framework, Executor* executor)
{
  LOG(INFO) << "Flushing queued tasks for framework "
            << framework->frameworkId;

  CHECK(executor->pid != UPID());

  foreach (const TaskDescription& task, executor->queuedTasks) {
    // Add the task to the executor.
    executor->addTask(task);

    MSG<S2E_RUN_TASK> out;
    out.mutable_framework()->MergeFrom(framework->info);
    out.mutable_framework_id()->MergeFrom(framework->frameworkId);
    out.set_pid(framework->pid);
    out.mutable_task()->MergeFrom(task);
    send(executor->pid, out);
  }

  executor->queuedTasks.clear();
}


// Kill a framework (including its executor if killExecutor is true).
void Slave::killFramework(Framework *framework, bool killExecutors)
{
  LOG(INFO) << "Cleaning up framework " << framework->frameworkId;

  // Shutdown all executors of this framework.
  foreachpaircopy (const ExecutorID& executorId, Executor* executor, framework->executors) {
    if (killExecutors) {
      LOG(INFO) << "Killing executor '" << executorId
                << "' of framework " << framework->frameworkId;

      send(executor->pid, S2E_KILL_EXECUTOR);

      // TODO(benh): There really isn't ANY time between when an
      // executor gets a S2E_KILL_EXECUTOR message and the isolation
      // module goes and kills it. We should really think about making
      // the semantics of this better.

      isolationModule->killExecutor(framework->frameworkId,
                                    framework->info,
                                    executor->info);
    }

    framework->destroyExecutor(executorId);
  }

  frameworks.erase(framework->frameworkId);
  delete framework;
}


// Called by the ExecutorReaper when an executor process exits.
void Slave::executorExited(const FrameworkID& frameworkId, const ExecutorID& executorId, int result)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(executorId);
    if (executor != NULL) {
      LOG(INFO) << "Exited executor '" << executorId
                << "' of framework " << frameworkId
                << " with result " << result;

      MSG<S2M_EXITED_EXECUTOR> out;
      out.mutable_slave_id()->MergeFrom(slaveId);
      out.mutable_framework_id()->MergeFrom(frameworkId);
      out.mutable_executor_id()->MergeFrom(executorId);
      out.set_result(result);
      send(master, out);

      isolationModule->killExecutor(framework->frameworkId,
                                    framework->info,
                                    executor->info);

      framework->destroyExecutor(executorId);

      // TODO(benh): When should we kill the presence of an entire
      // framework on a slave?
      if (framework->executors.size() == 0) {
        killFramework(framework);
      }
    } else {
      LOG(WARNING) << "UNKNOWN executor '" << executorId
                   << "' of framework " << frameworkId
                   << " has exited with result " << result;
    }
  } else {
    LOG(WARNING) << "UNKNOWN executor '" << executorId
                 << "' of UNKNOWN framework " << frameworkId
                 << " has exited with result " << result;
  }
};


string Slave::getUniqueWorkDirectory(const FrameworkID& frameworkId,
                                     const ExecutorID& executorId)
{
  string workDir = ".";
  if (conf.contains("work_dir")) {
    workDir = conf.get("work_dir", workDir);
  } else if (conf.contains("home")) {
    workDir = conf.get("home", workDir);
  }

  workDir = workDir + "/work";

  ostringstream os(std::ios_base::app | std::ios_base::out);
  os << workDir << "/slave-" << slaveId
     << "/fw-" << frameworkId << "-" << executorId;

  // Find a unique directory based on the path given by the slave
  // (this is because we might launch multiple executors from the same
  // framework on this slave).
  os << "/";

  string dir;
  dir = os.str();

  for (int i = 0; i < INT_MAX; i++) {
    os << i;
    if (opendir(os.str().c_str()) == NULL && errno == ENOENT)
      break;
    os.str(dir);
  }

  return os.str();
}


const Configuration& Slave::getConfiguration()
{
  return conf;
}
