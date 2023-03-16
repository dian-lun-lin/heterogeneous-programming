In the original threadpool (og_threadpool) derived from [lecture 10](https://github.com/tsung-wei-huang/ece6960-heterogeneous-programming/tree/main/Code/lecture10),
the task queue is centralized. The main issue of og_threadpool is as follows:

- ***Bottleneck***: A centralized task queue can become a bottleneck in the system. If there are multiple threads trying to access the queue simultaneously, they may need to wait their turn to access the queue. This can slow down the overall performance.
-  ***Difficulty scaling***: A centralized task queue can also make it difficult to scale the system. As the number of tasks and threads increases, the queue can become overwhelmed, thus slowing down the overall performance.

In my improved implementation (improved_threadpool), I define a class called Worker. A worker is essentially a worker thread that has its own mutex, condition variable, and task queue. 
When inserting a task, the main thread will enqueue the task into one of workers in a round-robin fashion.
Since each worker has its own task queue, we can avoid accessing the queue simultaneously.


