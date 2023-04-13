#include <iostream>
#include <chrono>
#include <vector>
#include <future>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <numeric>
#include <condition_variable>
#include <type_traits>

template <typename T>
struct MoC {

  MoC(T&& rhs) : object(std::move(rhs)) {}
  MoC(const MoC& other) : object(std::move(other.object)) {}

  T& get() { return object; }

  mutable T object;
};

// ----------------------------------------------------------------------------
// Class definition for Threadpool
// ----------------------------------------------------------------------------

class Threadpool {

  public:
    
    // constructor tasks a unsigned integer representing the number of
    // workers you need
    Threadpool(size_t N) {

      for(size_t i=0; i<N; i++) {
        threads.emplace_back([this](){
          // keep doing my job until the main thread sends a stop signal
          while(!stop) {
            std::function<void()> task;
            // my job is to iteratively grab a task from the queue
            {
              // Best practice: anything that happens inside the while continuation check
              // should always be protected by lock
              std::unique_lock lock(mtx);
              while(queue.empty() && !stop) {
                cv.wait(lock);
              }
              if(!queue.empty()) {
                task = queue.front();
                queue.pop();
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
      for(auto& t : threads) {
        t.join();
      }
    }

    // shutdown the threadpool
    void shutdown() {
      std::scoped_lock lock(mtx);
      stop = true;
      cv.notify_all();
    }

    // insert a task "callable object" into the threadpool
    template <typename C>
    auto insert(C&& task) {
      std::promise<void> promise;
      auto fu = promise.get_future();
      {
        std::scoped_lock lock(mtx);
        queue.push(
          [moc=MoC{std::move(promise)}, task=std::forward<C>(task)] () mutable {
            task();
            moc.object.set_value();
          }
        );
      }
      cv.notify_one();
      return fu;
    }
    
    template <typename Input, typename F>
    void for_each(Input beg, Input end, F func, size_t chunk_size = 1) {

      // the total number of elements in the range [beg, end)
      size_t N = std::distance(beg, end);

      std::vector<std::future<void>> futures;
      std::atomic<size_t> takens {0};

      for(size_t i=0; i<threads.size(); i++) {
        futures.emplace_back(insert([N, beg, end, func, chunk_size, &takens](){
          size_t curr_b = takens.fetch_add(chunk_size, std::memory_order_relaxed);              
          while(curr_b < N) {
            size_t curr_e = std::min(N, curr_b + chunk_size);
            // apply func to the range specified by beg + [curr_b, curr_e)
            std::for_each(beg + curr_b, beg + curr_e, func);
            // get the next chunk
            curr_b = takens.fetch_add(chunk_size, std::memory_order_relaxed);              
          }
        }));
      }
      
      // caller thread to wait for all W tasks finish (futures)
      for(auto & fu : futures) {
        fu.get();
      }
    }

    template <typename Input, typename T, typename F>
    T reduce(Input beg, Input end, T init, F bop, size_t chunk_size = 2) {
      size_t N = std::distance(beg, end);

      std::vector<std::future<void>> futures;
      std::atomic<size_t> takens {0};

      std::mutex mutex;
      
      for(size_t i=0; i<threads.size(); i++) {
        futures.emplace_back(insert([N, beg, end, bop, &init, &mutex, chunk_size, &takens](){
          // pre-reduce
          size_t curr_b = takens.fetch_add(2, std::memory_order_relaxed);
          
          // corner case #1: no more elements to reduce
          if(curr_b >= N) {
            return;
          }
          // corner case #2: only one element left
          if(N - curr_b == 1) {
            std::scoped_lock lock(mutex);
            init = bop(init, *(beg + curr_b));
            return;
          }
          // perform a reduction on these two elements
          T temp = bop( *(beg+curr_b), *(beg+curr_b+1) );
          curr_b = takens.fetch_add(chunk_size, std::memory_order_relaxed);              
          while(curr_b < N) {
            size_t curr_e = std::min(N, curr_b + chunk_size);
            // run a sequential reduction to the range specified by beg + [curr_b, curr_e)
            temp = std::accumulate(beg + curr_b, beg + curr_e, temp, bop);
            // get the next chunk
            curr_b = takens.fetch_add(chunk_size, std::memory_order_relaxed);              
          }
          // perform a final reduction on temp with init
          {
            std::scoped_lock lock(mutex);
            init = bop(init, temp);
          }
        }));
      }
      
      // caller thread to wait for all W tasks finish (futures)
      for(auto & fu : futures) {
        fu.get();
      }
      
      return init;
    }

    template <typename Input, typename T, typename F>
    T reduce_guided(Input beg, Input end, T init, F bop, size_t chunk_size = 2) {
      size_t N = std::distance(beg, end);

      std::vector<std::future<void>> futures;
      std::atomic<size_t> takens {0};

      std::mutex mutex;
      
      for(size_t i=0; i<threads.size(); i++) {
        futures.emplace_back(insert([this, N, beg, end, bop, &init, &mutex, chunk_size, &takens](){

          // pre-reduce
          size_t curr_b = takens.fetch_add(2, std::memory_order_relaxed);
          
          // corner case #1: no more elements to reduce
          if(curr_b >= N) {
            return;
          }
          // corner case #2: only one element left
          if(N - curr_b == 1) {
            std::scoped_lock lock(mutex);
            init = bop(init, *(beg + curr_b));
            return;
          }
          // perform a reduction on these two elements
          T temp = bop( *(beg + curr_b), *(beg + curr_b + 1) );

          // guided scheduling parameters
          size_t thres = 2 * threads.size() * (chunk_size + 1);
          float p = 1.0f / (2 * threads.size());

          curr_b = takens.fetch_add(chunk_size, std::memory_order_relaxed);              

          while(curr_b < N) {
            size_t remain = N - curr_b;

            if(remain <= thres) {
              size_t curr_e = std::min(N, curr_b + chunk_size);
              // run a sequential reduction to the range specified by beg + [curr_b, curr_e)
              temp = std::accumulate(beg + curr_b, beg + curr_e, temp, bop);
              // get the next chunk
              curr_b = takens.fetch_add(chunk_size, std::memory_order_relaxed);              
            }
            else {
              size_t q = remain * p;
              if(q < chunk_size) {
                q = chunk_size;
              }
              size_t curr_e = std::min(N, curr_b + q);
              if(takens.compare_exchange_strong(curr_b, curr_e, std::memory_order_relaxed, std::memory_order_relaxed)) {
                temp = std::accumulate(beg + curr_b, beg + curr_e, temp, bop);
                curr_b = takens.load(std::memory_order_relaxed);
              }
            
            }
          }

          // perform a final reduction on temp with init
          {
            std::scoped_lock lock(mutex);
            init = bop(init, temp);
          }
        }));
      }
      
      // caller thread to wait for all W tasks finish (futures)
      for(auto & fu : futures) {
        fu.get();
      }
      
      return init;
    }

  private:

    std::mutex mtx;
    std::vector<std::thread> threads;
    std::condition_variable cv;
    
    bool stop {false};
    std::queue< std::function<void()> > queue;

};

// seq version of for_each based on STL implementation
auto seq_reduce(std::vector<int>& vec) {
  return std::accumulate(vec.begin(), vec.end(), 0, [](int a, int b){ return a + b; });
}

auto par_reduce(std::vector<int>& vec, Threadpool& threadpool) {
  return threadpool.reduce_guided(
    vec.begin(), vec.end(), 0, [](int a, int b){ return a+b; }, 1024
  );
}

int main(int argc, char* argv[]) {

  // usage: ./a.out T N
  if(argc != 3) {
    std::cerr << "usage: ./a.out T N\n";
    std::exit(EXIT_FAILURE);
  } 

  size_t T = std::atoi(argv[1]);
  size_t N = std::atoi(argv[2]);

  // create a thread pool of the maximum hardware concurrency
  Threadpool threadpool(T);

  std::vector<int> vec(N);
  for(auto& i : vec) {
    i = 1;
  }
  
  // run reduce sequentially
  std::cout << "running seq_reduce ... ";
  auto beg = std::chrono::steady_clock::now();
  auto res1 = seq_reduce(vec);
  auto end = std::chrono::steady_clock::now();
  std::cout << "result: " << res1 << "\n";
  std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(end-beg).count()
            << "ns\n"; 

  // run reduce parallely
  std::cout << "running par_for_each ... ";
  beg = std::chrono::steady_clock::now();
  //auto res2 = par_reduce(vec, threadpool);
  auto res2 = par_reduce(vec, threadpool);
  std::cout << "result: " << res2 << "\n";
  end = std::chrono::steady_clock::now();
  std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(end-beg).count()
            << "ns\n"; 

  // shut down the threadpool
  threadpool.shutdown();


  return 0;
}


