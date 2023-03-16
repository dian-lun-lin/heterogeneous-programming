#include <iostream>
#include <chrono>
#include <vector>
#include <future>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <type_traits>

template <typename T>
struct MoC {

  MoC(T&& rhs) : object(std::move(rhs)) {}
  MoC(const MoC& other) : object(std::move(other.object)) {}

  T& get() { return object; }

  mutable T object;
};

class Worker {

  friend class Threadpool;

  size_t _id;
  std::mutex _mtx;
  std::condition_variable _cv;
  std::queue< std::function<void()> > _que;
  bool _stop{false};
};

// ----------------------------------------------------------------------------
// Class definition for Threadpool
// ----------------------------------------------------------------------------

class Threadpool {

  public:
    
    // constructor tasks a unsigned integer representing the number of
    // workers you need
    Threadpool(size_t N): _workers{N} {


      for(size_t i=0; i<N; i++) {
        Worker& worker = _workers[i];
        worker._id = i;

        _threads.emplace_back([this, &worker](){
          // keep doing my job until the main thread sends a stop signal
          while(!worker._stop) {
            std::function<void()> task;
            // my job is to iteratively grab a task from the queue
            {
              std::unique_lock lock(worker._mtx);
              while(worker._que.empty() && !worker._stop) {
                worker._cv.wait(lock);
              }
              if(!worker._que.empty()) {
                task = worker._que.front();
                worker._que.pop();
              }
            }
            // and run the task...
            if(task) {
              task();
            }
          }
        });
      }
    }

    // destructor will release all threading resources by joining all of them
    ~Threadpool() {
      // I need to join the threads to release their resources
      for(auto& t : _threads) {
        t.join();
      }
    }

    // shutdown the threadpool
    void shutdown() {
      for(auto& worker: _workers) {
        std::scoped_lock lock(worker._mtx);
        worker._stop = true;
        worker._cv.notify_one();
      }
    }

    // insert a task "callable object" into the threadpool
    template <typename C>
    auto insert(C&& task) {

      std::promise<void> promise;
      auto fu = promise.get_future();

      size_t id = _cnt++ % _workers.size();
      Worker& worker = _workers[id];

      {
        std::scoped_lock lock(worker._mtx);
        worker._que.push(
          [moc=MoC{std::move(promise)}, task=std::forward<C>(task)] () mutable {
            task();
            moc.object.set_value();
          }
        );
      }

      worker._cv.notify_one();
      return fu;
    }
    
    // insert a task "callable object" into the threadpool
    //template <typename C>
    //auto insert_with_return(C&& task) {
      //using R = std::result_of_t<C()>;
      //std::promise<R> promise;
      //auto fu = promise.get_future();
      //{
        //std::scoped_lock lock(mtx);
        //queue.push(
          //[moc=MoC{std::move(promise)}, task=std::forward<C>(task)] () mutable {
            //moc.object.set_value(
              //task()
            //);
          //}
        //);
      //}
      //cv.notify_one();
      //return fu;
    //}
    
    //// insert a task "callable object" into the threadpool using a generic
    //// function wrapper (instead of a template argument)
    //auto insert_2(std::function<void()> task) {

      //std::promise<void> promise;
      //auto fu = promise.get_future();
    
      //{
        //std::scoped_lock lock(mtx);
        //queue.push(
          //[moc=MoC{std::move(promise)}, task=std::move(task)] () mutable {
            //task();
            //moc.object.set_value();
          //}
        //);
      //}
      //cv.notify_one();
      
      //return fu;
    //}

  private:

    std::vector<Worker> _workers;
    std::vector<std::thread> _threads;
    size_t _cnt{0};
};
// ----------------------------------------------------------------------------
// application code
//
// perform parallel matrix multiplication 
// A * B = C
// A is NxK
// B is KxM
// C is NxM
// ----------------------------------------------------------------------------

void matmul_seq(
  size_t N, size_t K, size_t M,
  const std::vector<int>& A,
  const std::vector<int>& B,
  std::vector<int>& C,
  Threadpool& threadpool
) {
  // seq version of matmul
  for(size_t i=0; i<N; i++) {
    for(size_t j=0; j<M; j++) {
      C[i*M + j] = 0;
      for(size_t k=0; k<K; k++) {
        C[i*M + j] += A[i*K + k] * B[k*M + j];
      }
    }
  }
}

void matmul(
  size_t N, size_t K, size_t M,
  const std::vector<int>& A,
  const std::vector<int>& B,
  std::vector<int>& C,
  Threadpool& threadpool
) {

  std::vector<std::future<void>> futures;
  
  // this version has a serious issue of false sharing
  //for(size_t i=0; i<N; i++) {
  //  for(size_t j=0; j<M; j++) {
  //    // each element C[i][j] is the result of inner product
  //    // of row i at A and column j at B
  //    futures.emplace_back(
  //      threadpool.insert([i, j, &A, &B, &C, M, K](){
  //        for(size_t k=0; k<K; k++) {
  //          C[i*M + j] += A[i*K + k] * B[k*M + j];
  //        }
  //      })
  //    );
  //  }
  //}
  
  for(size_t i=0; i<N; i++) {
    futures.emplace_back(
      threadpool.insert([=, &A, &B, &C](){
        for(size_t j=0; j<M; j++) {
          // each element C[i][j] is the result of inner product
          // of row i at A and column j at B
          for(size_t k=0; k<K; k++) {
            C[i*M + j] += A[i*K + k] * B[k*M + j];
          }
        }
      })
    );
  }
  
  // synchronize the execution on the N*M inner products
  for(auto& fu : futures) {
    fu.get();
  }
}

int main(int argc, char* argv[]) {

  if(argc != 5) {
    std::cerr << "usage: ./a.out N K M T\n";
    std::exit(EXIT_FAILURE);
  }

  size_t N = std::atoi(argv[1]);
  size_t K = std::atoi(argv[2]);
  size_t M = std::atoi(argv[3]);
  size_t T = std::atoi(argv[4]);  // number of threads to parallelize
                                  // the matrix multiplication

  // initialize three matrices A, B, and C
  std::vector<int> A(N*K, 1), B(K*M, 2), C(N*M, 0);

  // create a thread pool of the maximum hardware concurrency
  Threadpool threadpool(T);

  // run matrix multiplication in parallel
  auto beg = std::chrono::steady_clock::now();
  matmul(N, K, M, A, B, C, threadpool);
  auto end = std::chrono::steady_clock::now();

  std::cout << "Parallel AxB=C takes "
            << std::chrono::duration_cast<std::chrono::microseconds>(end-beg).count()
            << " us\n";

  // run matrix multiplication in sequential
  beg = std::chrono::steady_clock::now();
  matmul_seq(N, K, M, A, B, C, threadpool);
  end = std::chrono::steady_clock::now();

  std::cout << "Sequential AxB=C takes "
            << std::chrono::duration_cast<std::chrono::microseconds>(end-beg).count()
            << " us\n";
  
  // shut down the threadpool
  threadpool.shutdown();


  return 0;
}

