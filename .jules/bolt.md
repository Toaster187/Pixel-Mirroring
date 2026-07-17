## 2023-10-27 - [Network Scanner Thread Pooling]
**Learning:** Network scanning via repeated sequential thread batching is highly inefficient and suffers from the convoy effect (one slow thread holds up the whole batch) and OS overhead from repeatedly allocating/deallocating hundreds of threads.
**Action:** Use a fixed-size worker pool (e.g., 50 threads) combined with a lock-free `std::atomic<int>` counter to continuously feed work. This eliminates thread churn and keeps all workers fully saturated until the queue is empty.
