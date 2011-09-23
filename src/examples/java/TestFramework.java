import java.io.File;
import java.io.IOException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.apache.mesos.*;
import org.apache.mesos.Protos.*;


public class TestFramework {
  static class MyScheduler implements Scheduler {
    int launchedTasks = 0;
    int finishedTasks = 0;
    final int totalTasks;

    public MyScheduler() {
      this(5);
    }

    public MyScheduler(int numTasks) {
      totalTasks = numTasks;
    }

    @Override
    public void registered(SchedulerDriver driver, FrameworkID frameworkId) {
      System.out.println("Registered! ID = " + frameworkId.getValue());
    }

    @Override
    public void resourceOffers(SchedulerDriver driver,
                               List<Offer> offers) {
      for (Offer offer : offers) {
        List<TaskDescription> tasks = new ArrayList<TaskDescription>();
        if (launchedTasks < totalTasks) {
          TaskID taskId = TaskID.newBuilder()
            .setValue(Integer.toString(launchedTasks++)).build();

          System.out.println("Launching task " + taskId.getValue());

          TaskDescription task = TaskDescription.newBuilder()
            .setName("task " + taskId.getValue())
            .setTaskId(taskId)
            .setSlaveId(offer.getSlaveId())
            .addResources(Resource.newBuilder()
                          .setName("cpus")
                          .setType(Resource.Type.SCALAR)
                          .setScalar(Resource.Scalar.newBuilder()
                                     .setValue(1)
                                     .build())
                          .build())
            .addResources(Resource.newBuilder()
                          .setName("mem")
                          .setType(Resource.Type.SCALAR)
                          .setScalar(Resource.Scalar.newBuilder()
                                     .setValue(128)
                                     .build())
                          .build())
            .build();
          tasks.add(task);
        }
        Filters filters = Filters.newBuilder().setRefuseSeconds(1).build();
        driver.launchTasks(offer.getId(), tasks, filters);
      }
    }

    @Override
    public void offerRescinded(SchedulerDriver driver, OfferID offerId) {}

    @Override
    public void statusUpdate(SchedulerDriver driver, TaskStatus status) {
      System.out.println("Status update: task " + status.getTaskId() +
                         " is in state " + status.getState());
      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;
        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks)
          driver.stop();
      }
    }

    @Override
    public void frameworkMessage(SchedulerDriver driver, SlaveID slaveId, ExecutorID executorId, byte[] data) {}

    @Override
    public void slaveLost(SchedulerDriver driver, SlaveID slaveId) {}

    @Override
    public void error(SchedulerDriver driver, int code, String message) {
      System.out.println("Error: " + message);
    }
  }

  public static void main(String[] args) throws Exception {
    if (args.length < 1 || args.length > 2) {
      System.out.println("Invalid use: please specify a master");
    } else {
      ExecutorInfo executorInfo;

      File file = new File("./test_executor");
      executorInfo = ExecutorInfo.newBuilder()
                       .setExecutorId(ExecutorID.newBuilder().setValue("default").build())
                       .setUri(file.getCanonicalPath())
                       .build();

      if (args.length == 1) {
        new MesosSchedulerDriver(new MyScheduler(),
                                 "Java test framework",
                                 executorInfo,
                                 args[0]).run();
      } else {
        new MesosSchedulerDriver(new MyScheduler(Integer.parseInt(args[1])),
                                 "Java test framework",
                                 executorInfo,
                                 args[0]).run();
      }
    }
  }
}
