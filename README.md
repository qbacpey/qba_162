# Pintos

这是伯克利操作系统课程（CS162）的课程设计项目。学生需要以小组为单位，以课程初期给定的Pintos操作系统为框架，拓展下列三个模块的功能：
1. 进程系统（Tag：`proj-userprog-completed`）；
2. 线程系统（Branch：`proj-thread`）；
3. 文件系统（Branch：`proj-filesys`）；

## 进程系统

对Pintos的用户进程模块做出如下功能拓展：
+ 支持向用户进程传递命令行参数；
+ 实现以下进程控制系统调用（功能与Linux中的同名系统调用类似）：
  + `exec`：创建一个新进程，令其运行指定的应用程序（可以被当作是`fork`和`exec`的混合体）；
  + `wait`：等待当前进程的子进程`pid`执行完毕；
  + `halt`：关闭Pintos，用于熟悉系统调用执行流程；
+ 实现以下文件操作系统调用：`create, remove, open, filesize, read, write, seek, tell, close`；
  + `create, remove`：创建文件，删除文件；
  + `open, close`：开启指定文件，返回文件描述符。关闭与给定文件描述符对应的文件；
  + `read, write, seek, tell, filesize`：文件操作；

## 线程系统

对Pintos的线程系统模块做出如下功能拓展：
+ 实现一个严格优先级线程调度器（Strict Priority Scheduler），优先级范围为`0 ~ 63`：
  + 无论在何种情况下，高优先级线程都会先于低优先级线程被调度运行；
  + 三种同步原语（lock, semaphore, condition variable）需优先将资源给予高优先级线程；
  + 实现优先级捐献（priority donation），解决由于严格优先级调度导致的优先级反转问题（priority inversion）；
+ 实现简化版的`pthread`线程库，支持如下系统调用：
  + `sys_pthread_create`：创建用户线程；
  + `sys_pthread_exit`：用户线程退出；
  + `sys_pthread_join`：暂停当前线程，直到目标线程执行完毕再恢复；
  + `lock_acquire, lock_release, sema_down, sema_up`：用户空间同步原语；
+ 如果进程执行时出现了导致进程需要退出的事件，事件之间应按照如下优先级（从低到高）相互覆盖：
  1. 主线程执行`pthread_exit`：主线程等待其余所有线程自然退出之后退出进程；
  2. 任何线程执行系统调用`exit`：进程中所有位于用户态的线程不可继续执行，位于内核态的线程退出内核之后需立刻退出；
  3. 任何线程执行时触发异常：与`2`相同，只不过进程退出码必须为`-1`；
  
## 文件系统

对Pintos的文件系统做出如下功能拓展：
+ 为文件系统添加Buffer Cache：
  + 至多可缓存64个磁盘扇区；
  + 需使用任意一种类LRU算法作为调度政策；
  + 必须是一个Write Back Cache；
  + 确保某个扇区同一时间只能被一个线程所读写，但是属于同一个文件的不同扇区可以被多个线程同时读写；
+ 可拓展文件：
  + 支持对文件的随机访问；
  + 树型索引结构：
    + 文件数据块可分散在磁盘各处，无需连续分布；
    + 至少需要支持类似于Unix FFS文件中的二级间接指针（doubly-indirect pointers）；
  + 妥善处理扇区分配失败的情况，系统必须回滚到未分配之前的状态；
+ 子目录：
  + 实现`chdir, mkdir, readdir, isdir`等目录相关的系统调用；
  + `open, close, exec, remove, inumber`需妥善处理目录相关的逻辑；
  + 系统需同时支持相对目录和绝对目录；
