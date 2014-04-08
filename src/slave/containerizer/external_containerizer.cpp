/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <iomanip>
#include <list>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>

#include <boost/shared_array.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "common/type_utils.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/external_containerizer.hpp"

// Process user environment.
extern char** environ;

using lambda::bind;
using std::list;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;
using tuples::tuple;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

using state::ExecutorState;
using state::FrameworkState;
using state::RunState;
using state::SlaveState;


ExternalContainerizer::ExternalContainerizer(const Flags& flags)
{
  process = new ExternalContainerizerProcess(flags);
  spawn(process);
}


ExternalContainerizer::~ExternalContainerizer()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Nothing> ExternalContainerizer::recover(
    const Option<state::SlaveState>& state)
{
  return dispatch(process, &ExternalContainerizerProcess::recover, state);
}


Future<ExecutorInfo> ExternalContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const FrameworkID& frameworkId,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process,
                  &ExternalContainerizerProcess::launch,
                  containerId,
                  taskInfo,
                  frameworkId,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
}


Future<Nothing> ExternalContainerizer::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(process,
                  &ExternalContainerizerProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> ExternalContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process, &ExternalContainerizerProcess::usage, containerId);
}


Future<containerizer::Termination> ExternalContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &ExternalContainerizerProcess::wait, containerId);
}


void ExternalContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process, &ExternalContainerizerProcess::destroy, containerId);
}


ExternalContainerizerProcess::ExternalContainerizerProcess(
    const Flags& _flags) : flags(_flags) {}


Future<Nothing> ExternalContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  // Filter the executor run states that we attempt to recover and do
  // so.
  /*
  if (state.isSome()) {
    foreachvalue (const FrameworkState& framework, state.get().frameworks) {
      foreachvalue (const ExecutorState& executor, framework.executors) {
        LOG(INFO) << "Recovering executor '" << executor.id
                  << "' of framework " << framework.id;

        if (executor.info.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its info could not be recovered";
          continue;
        }

        if (executor.latest.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its latest run could not be recovered";
          continue;
        }

        // We are only interested in the latest run of the executor!
        const ContainerID& containerId = executor.latest.get();
        Option<RunState> run = executor.runs.get(containerId);
        CHECK_SOME(run);

        // We need the pid so the reaper can monitor the executor so skip this
        // executor if it's not present. This is not an error because the slave
        // will try to wait on the container which will return a failed
        // Termination and everything will get cleaned up.
        if (!run.get().forkedPid.isSome()) {
          continue;
        }

        if (run.get().completed) {
          LOG(INFO) << "Skipping recovery of executor '" << executor.id
                    << "' of framework " << framework.id
                    << " because its latest run '" << containerId << "'"
                    << " is completed";
          continue;
        }

        const pid_t pid(run.get().forkedPid.get());
        containers.put(containerId, Owned<Container>(new Container(pid)));

        process::reap(pid)
          .onAny(defer(
            PID<ExternalContainerizerProcess>(this),
            &ExternalContainerizerProcess::reaped,
            containerId,
            lambda::_1));

        // Recreate the sandbox information.
        // TODO (tillt): This recreates logic that is supposed to be
        // further up, within the slave implementation.
        const string& directory = paths::createExecutorDirectory(
          flags.work_dir,
          state.get().id,
          framework.id,
          executor.id,
          containerId);

        CHECK(framework.info.isSome());

        const Option<string>& user = flags.switch_user
          ? Option<string>(framework.info.get().user()) : None();

        sandboxes.put(
          containerId,
          Owned<Sandbox>(new Sandbox(directory, user)));
      }
    }
  }
*/
  return Nothing();
}


Future<ExecutorInfo> ExternalContainerizerProcess::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const FrameworkID& frameworkId,
    const std::string& directory,
    const Option<std::string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  LOG(INFO) << "Launching container '" << containerId << "'";

  // Get the executor from our task. If no executor is associated with
  // the given task, this function renders an ExecutorInfo using the
  // mesos-executor as its command.
  ExecutorInfo executor = containerExecutorInfo(flags, taskInfo, frameworkId);
  executor.mutable_resources()->MergeFrom(taskInfo.resources());

  if (containers.contains(containerId)) {
    return Failure("Cannot start already running container '"
      + containerId.value() + "'");
  }

  sandboxes.put(containerId, Owned<Sandbox>(new Sandbox(directory, user)));

  map<string, string> environment = executorEnvironment(
      executor,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  if (!flags.hadoop_home.empty()) {
    environment["HADOOP_HOME"] = flags.hadoop_home;
  }

  TaskInfo task;
  task.CopyFrom(taskInfo);
  CommandInfo* command = task.has_executor()
    ? task.mutable_executor()->mutable_command()
    : task.mutable_command();
  // When the selected command has no container attached, use the
  // default from the slave startup flags, if available.
  if (!command->has_container()) {
    if (flags.default_container_image.isSome()) {
      command->mutable_container()->set_image(
          flags.default_container_image.get());
    } else {
      LOG(INFO) << "No container specified in task and no default given. "
                << "The external containerizer will have to fill in "
                << "defaults.";
    }
  }

  containerizer::Launch launch;
  launch.mutable_container_id()->CopyFrom(containerId);
  launch.mutable_task()->CopyFrom(task);
  launch.mutable_framework_id()->CopyFrom(frameworkId);
  launch.set_directory(directory);
  if (user.isSome()) {
    launch.set_user(user.get());
  }
  launch.mutable_slave_id()->CopyFrom(slaveId);
  launch.set_slave_pid(slavePid);
  launch.set_checkpoint(checkpoint);
  launch.set_mesos_executor_path(
      path::join(flags.launcher_dir, "mesos-executor"));

  // Checkpoint the lauch intend for this containerId if requested.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        slaveId,
        frameworkId,
        executorInfo.executor_id(),
        containerId);

    LOG(INFO) << "Checkpointing container '" << containerId << "' "
              << "launch intend to '" << path <<  "'";

    Try<Nothing> checkpointed = slave::state::checkpoint(path, launch);

    if (checkpointed.isError()) {
      terminate(containerId);
      return Failure("Failed to checkpoint launch intend '"
        + containerId.value() + "' to '" + path "'");
    }
  }

  Try<Subprocess> invoked = invoke(
      "launch",
      containerId,
      launch,
      environment);

  if (invoked.isError()) {
    return Failure("Launch of container '" + containerId.value()
      + "' failed (error: " + invoked.error() + ")");
  }

  // Record the successful container launch.
  containers.put(containerId, Owned<Container>(new Container));

  // Read from the result-pipe and invoke callbacks when reaching EOF.
  return invoked.get().status()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_launch,
        containerId,
        frameworkId,
        executor,
        slaveId,
        checkpoint,
        lambda::_1));
}


Future<ExecutorInfo> ExternalContainerizerProcess::_launch(
    const ContainerID& containerId,
    const FrameworkID& frameworkId,
    const ExecutorInfo executorInfo,
    const SlaveID& slaveId,
    bool checkpoint,
    const Future<Option<int> >& future)
{
  VLOG(1) << "Launch callback triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  Try<Nothing> done = isDone(future);
  if (done.isError()) {
    terminate(containerId);
    return Failure(done.error());
  }

  VLOG(1) << "Launch finishing up for container '" << containerId << "'";
  return executorInfo;
}


Future<containerizer::Termination> ExternalContainerizerProcess::wait(
    const ContainerID& containerId)
{
  VLOG(1) << "Wait triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    LOG(ERROR) << "not running";
    return Failure("Container '" + containerId.value() + "' not running");
  }

  Try<Subprocess> invoked = invoke("wait", containerId, containerId);

  if (invoked.isError()) {
    LOG(ERROR) << "not running";
    terminate(containerId);
    return Failure("Wait on container '" + containerId.value()
      + "' failed (error: " + invoked.error() + ")");
  }

  invoked.get().status()
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_wait,
        containerId,
        lambda::_1));

  containers[containerId]->pid = invoked.get().pid();

  return containers[containerId]->termination.future();
}


void ExternalContainerizerProcess::_wait(
    const ContainerID& containerId,
    const Future<Option<int> >& status)
{
  VLOG(1) << "Wait callback triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId << "' not running";
    return;
  }

  Owned<Container> container = containers[containerId];

  VLOG(2) << "future on containerId '" << containerId
          << "' has reached: "
          << (status.isReady() ? "READY" :
             status.isFailed() ? "FAILED: " + status.failure() :
             "DISCARDED");

  Future<containerizer::Termination> future;

  if (!status.isReady()) {
    // Something has gone wrong, probably an unsuccessful terminate().
    future = Failure(
        "Failed to get status: " +
        (status.isFailed() ? status.failure() : "discarded"));
  } else {
    LOG(INFO) << "Container '" << containerId << "' "
              << "has terminated with "
              << (status.get().isSome() ?
                  "status-code: " + stringify(status.get().get()) :
                  "no result");

    bool killed = false;
    if (status.get().isSome()) {
      killed = !WIFEXITED(status.get().get());
    }
    containerizer::Termination termination;
    termination.set_killed(killed);
    termination.set_message(killed ? "killed" : "terminated");
    if (status.get().isSome()) {
      termination.set_status(status.get().get());
    }
    future = termination;
  }

  // Set the promise to alert others waiting on this container.
  container->termination.set(future);

  // Ensure someone notices this termination by deferring final clean
  // up until the container has been waited on.
  container->waited.future()
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::cleanup,
        containerId));
}


Future<Nothing> ExternalContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  VLOG(1) << "Update triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "'' not running");
  }

  containers[containerId]->resources = resources;

  containerizer::Update update;
  update.mutable_container_id()->CopyFrom(containerId);
  update.mutable_resources()->CopyFrom(resources);

  Try<Subprocess> invoked = invoke("update", containerId, update);

  if (invoked.isError()) {
    terminate(containerId);
    return Failure("Update of container '" + containerId.value()
      + "' failed (error: " + invoked.error() + ")");
  }

  // Await both, input from the pipe as well as an exit of the
  // process.
  return invoked.get().status()
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_update,
        containerId,
        lambda::_1));
}


Future<Nothing> ExternalContainerizerProcess::_update(
    const ContainerID& containerId,
    const Future<Option<int> >& future)
{
  VLOG(1) << "Update callback triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  Try<Nothing> done = isDone(future);
  if (done.isError()) {
    terminate(containerId);
    return Failure(done.error());
  }

  return Nothing();
}


Future<ResourceStatistics> ExternalContainerizerProcess::usage(
    const ContainerID& containerId)
{
  VLOG(1) << "Usage triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "'' not running");
  }

  Try<Subprocess> invoked = invoke("usage", containerId, containerId);

  if (invoked.isError()) {
    terminate(containerId);
    return Failure("Usage on container '" + containerId.value()
      + "' failed (error: " + invoked.error() + ")");
  }

  // Await both, input from the pipe as well as an exit of the
  // process.
  return await(read(invoked.get().out()), invoked.get().status())
    .then(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_usage,
        containerId,
        lambda::_1));
}


Future<ResourceStatistics> ExternalContainerizerProcess::_usage(
    const ContainerID& containerId,
    const Future<ResultFutures>& future)
{
  VLOG(1) << "Usage callback triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not running");
  }

  Try<string> result = isDone(future);
  if (result.isError()) {
    terminate(containerId);
    return Failure(result.error());
  }

  ResourceStatistics statistics;

  if (!statistics.ParseFromString(result.get())) {
    // TODO(tillt): Consider not terminating the containerizer due
    // to protocol breach but only fail the operation.
    terminate(containerId);
    return Failure("Could not parse usage result protobuf (error: "
      + statistics.InitializationErrorString() + ")");
  }

  VLOG(2) << "Usage result: '" << statistics.DebugString() << "'";

  LOG(INFO) << "containerId '" << containerId << "' "
            << "total mem usage "
            << statistics.mem_rss_bytes() << " "
            << "total CPU user usage "
            << statistics.cpus_user_time_secs() << " "
            << "total CPU system usage "
            << statistics.cpus_system_time_secs();

  return statistics;
}


void ExternalContainerizerProcess::destroy(const ContainerID& containerId)
{
  VLOG(1) << "Destroy triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId << "' not running";
    return;
  }

  Try<Subprocess> invoked = invoke("destroy", containerId, containerId);

  if (invoked.isError()) {
    LOG(ERROR) << "Destroy of container '" << containerId
               << "' failed (error: " << invoked.error() << ")";
    terminate(containerId);
    return;
  }

  invoked.get().status()
    .onAny(defer(
        PID<ExternalContainerizerProcess>(this),
        &ExternalContainerizerProcess::_destroy,
        containerId,
        lambda::_1));
}


void ExternalContainerizerProcess::_destroy(
    const ContainerID& containerId,
    const Future<Option<int> >& future)
{
  VLOG(1) << "Destroy callback triggered on container '" << containerId << "'";

  if (!containers.contains(containerId)) {
    LOG(ERROR) << "Container '" << containerId.value() << "' not running";
    return;
  }

  Try<Nothing> done = isDone(future);
  if (done.isError()) {
    LOG(ERROR) << "Destroy of container '" << containerId
               << "' failed (error: " << done.error() << ")";
  }

  // Additionally to the optional external destroy-command, we need to
  // terminate the external containerizer's process.
  terminate(containerId);
}


void ExternalContainerizerProcess::cleanup(
    const ContainerID& containerId)
{
  VLOG(1) << "Callback performing final cleanup of running state";

  if (sandboxes.contains(containerId)) {
    sandboxes.erase(containerId);
  } else {
    LOG(WARNING) << "Container '" << containerId << "' not sandboxed";
  }

  if (containers.contains(containerId)) {
    containers.erase(containerId);
  } else {
    LOG(WARNING) << "Container '" << containerId << "' not running anymore";
  }
}


void ExternalContainerizerProcess::terminate(const ContainerID& containerId)
{
  if (!containers.contains(containerId)) {
    LOG(WARNING) << "Container '" << containerId << "' not running";
    return;
  }

  // Terminate the containerizer.
  pid_t pid = containers[containerId]->pid;
  VLOG(2) << "About to send a SIGKILL to containerizer pid: " << pid;

  // TODO(tillt): Add graceful termination as soon as we have an
  // accepted way to do that in place.
  Try<list<os::ProcessTree> > trees =
    os::killtree(pid, SIGKILL, true, true);

  if (trees.isError()) {
    LOG(WARNING) << "Failed to kill the process tree rooted at pid "
                 << pid << ": " << trees.error();
    return;
  }

  LOG(INFO) << "Killed the following process tree/s:\n"
            << stringify(trees.get());
}


// Payload read continuation.
void __read(
    int pipe,
    const Future<size_t>& future,
    const boost::shared_array<char>& data,
    Owned<Promise<string> > promise)
{
  if (future.isFailed()) {
    promise->fail(future.failure());
  }
  if (!future.isReady()) {
    promise->fail("Future is not ready");
  }

  // When EOF is reached, return an empty string.
  if (future.get() == 0) {
    promise->set(string());
    return;
  }

  stringstream in;
  in.write(data.get(), future.get());
  promise->set(in.str());
}


// Size read continuation.
void _read(
    int pipe,
    const Future<size_t>& future,
    const boost::shared_ptr<size_t>& data,
    Owned<Promise<string> > promise)
{
  if (future.isFailed()) {
    promise->fail(future.failure());
  }
  if (!future.isReady()) {
    promise->fail("Future is not ready");
  }

  // When EOF is reached, return an empty string.
  if (future.get() == 0) {
    promise->set(string());
    return;
  }

  size_t size(*data.get());
  VLOG(2) << "Receiving protobuf sized to " << size << " bytes";

  boost::shared_array<char> buffer(new char[size]);

  io::read(pipe, buffer.get(), size)
    .onAny(lambda::bind(&__read, pipe, lambda::_1, buffer, promise));
}


Future<string> ExternalContainerizerProcess::read(int pipe)
{
  Try<Nothing> nonblock = os::nonblock(pipe);
  if (nonblock.isError()) {
    return Failure("Failed to accept nonblock (error: " + nonblock.error()
      + ")");
  }

  Owned<Promise<string> > promise(new Promise<string>());

  boost::shared_ptr<size_t> buffer(new size_t);

  io::read(pipe, buffer.get(), sizeof(size_t))
    .onAny(lambda::bind(&_read, pipe, lambda::_1, buffer, promise));

  return promise->future();
}


Try<Nothing> ExternalContainerizerProcess::isDone(
    const Future<Option<int> >& future)
{
  if (!future.isReady()) {
    return Error("Status not ready");
  }

  Option<int> status = future.get();
  if (status.isNone()) {
    return Error("External containerizer has no status available");
  }

  // The status is a waitpid-result which has to be checked for SIGNAL
  // based termination before masking out the exit-code.
  if (!WIFEXITED(status.get())) {
    return Error(string("External containerizer terminated by signal ")
      + strsignal(WTERMSIG(status.get())));
  }

  int exitCode = WEXITSTATUS(status.get());
  if (exitCode != 0) {
    return Error("External containerizer failed (exit: "
      + stringify(exitCode) + ")");
  }

  return Nothing();
}


Try<string> ExternalContainerizerProcess::isDone(
    const Future<ResultFutures>& future)
{
  if (!future.isReady()) {
    return Error("Could not receive any result");
  }

  Try<Nothing> status = isDone(tuples::get<1>(future.get()));
  if (status.isError()) {
    return status.error();
  }

  Future<string> result = tuples::get<0>(future.get());
  if (result.isFailed()) {
    return Error("Could not receive any result (error: "
      + result.failure() + ")");
  }

  return result.get();
}


int setup(const string& directory)
{
  // Put child into its own process session to prevent slave suicide
  // on child process SIGKILL/SIGTERM.
  if (::setsid() == -1) {
    return errno;
  }

  // Re/establish the sandbox conditions for the containerizer.
  if (::chdir(directory.c_str()) == -1) {
    return errno;
  }

  // Sync parent and child process.
  int sync = 42;
  while (::write(STDOUT_FILENO, &sync, sizeof(sync)) == -1
    && errno == EINTR);

  return 0;
}


Try<process::Subprocess> ExternalContainerizerProcess::invoke(
      const string& command,
      const ContainerID& containerId,
      const google::protobuf::Message& message,
      const map<string, string>& environment)
{
  CHECK(flags.containerizer_path.isSome())
    << "containerizer_path not set";

  CHECK(sandboxes.contains(containerId));

  // Use the provided message or the containerId if no message was
  // provided for delivery towards the external containerizer.
  stringstream output;
  message.SerializeToOstream(&output);
  VLOG(1) << "Invoking external containerizer for method '" << command << "'";

  // Construct the command to execute.
  string execute = flags.containerizer_path.get()
                 + " " + command
                 + " " + containerId.value();

  VLOG(2) << "calling: [" << execute << "]";
  VLOG(2) << "directory: " << sandboxes[containerId]->directory;
  if(sandboxes[containerId]->user.isSome()) {
    VLOG(2) << "user: " << sandboxes[containerId]->user.get();
  }

  // Re/establish the sandbox conditions for the containerizer.
  if (sandboxes[containerId]->user.isSome()) {
    Try<Nothing> chown = os::chown(
        sandboxes[containerId]->user.get(),
        sandboxes[containerId]->directory);
    if (chown.isError()) {
      return Error(string("Failed to chown work directory: ") +
        strerror(errno));
    }
  }

  // Fork exec of external process. Run a chdir and a setsid within
  // the child-context.
  Try<Subprocess> external = process::subprocess(
      execute,
      environment,
      lambda::bind(&setup, sandboxes[containerId]->directory));

  if (external.isError()) {
    return Error(string("Failed to execute external containerizer: ")
      + external.error());
  }

  // Sync parent and child process to make sure we have done the
  // setsid within the child context before continuing.
  int sync;
  while (::read(external.get().out(), &sync, sizeof(sync)) == -1
    && errno == EINTR);

  // Redirect output (stderr) from the external containerizer to log
  // file in the executor work directory, chown'ing it if a user is
  // specified.
  Try<int> err = os::open(
      path::join(sandboxes[containerId]->directory, "stderr"),
      O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (err.isError()) {
    return Error("Failed to redirect stderr: " + err.error());
  }

  Try<Nothing> nonblock = os::nonblock(external.get().err());
  if (nonblock.isError()) {
    os::close(err.get());
    return Error("Failed to redirect stderr: " + nonblock.error());
  }

  io::splice(external.get().err(), err.get())
    .onAny(bind(&os::close, err.get()));

  // Transmit protobuf data via stdout towards the external
  // containerizer. Each message is prefixed by its total size.
  if (!output.str().empty()) {
    ssize_t len = output.str().length();

    VLOG(2) << "Writing to child's standard input "
            << "(" << output.str().length() + sizeof(len) << " bytes)";

    nonblock = os::nonblock(external.get().in());
    if (nonblock.isError()) {
      return Error("Failed to write to stdin: " + nonblock.error());
    }

    if (write(external.get().in(), &len, sizeof(len)) < sizeof(len)) {
      return Error("Failed to write protobuf size to pipe");
    }

    string s = output.str();
    if (write(external.get().in(), s.c_str(), len) < len) {
      return Error("Failed to write protobuf payload to pipe");
    }
  }

  VLOG(2) << "Returning pid: " << external.get().pid();
  VLOG(2) << "Child output pipe: " << external.get().out();

  return external;
}


ExecutorInfo containerExecutorInfo(
    const Flags& flags,
    const TaskInfo& task,
    const FrameworkID& frameworkId)
{
  CHECK_NE(task.has_executor(), task.has_command())
    << "Task " << task.task_id()
    << " should have either CommandInfo or ExecutorInfo set but not both";

  if (!task.has_command()) {
      return task.executor();
  }

  ExecutorInfo executor;
  // Command executors share the same id as the task.
  executor.mutable_executor_id()->set_value(task.task_id().value());
  executor.mutable_framework_id()->CopyFrom(frameworkId);

  // Prepare an executor name which includes information on the
  // task and a possibly attached container.
  string name =
    "(External Containerizer Task: " + task.task_id().value();
  if (task.command().has_container()) {
    name += " Container image: " + task.command().container().image();
  }
  name += ")";

  executor.set_name("Command Executor " + name);
  executor.set_source(task.task_id().value());
  executor.mutable_command()->MergeFrom(task.command());
  return executor;
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
