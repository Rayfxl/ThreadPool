# 基于C++11实现的通用线程池 

- 基于可变参数模板编程，实现线程池提交任务接口，支持任意任务函数和任意参数的传递

- 使用future类型获取和接收提交任务接口的返回值，使用map和queue容器管理线程对象和任务
- 基于condition_variable和mutex实现任务提交与执行线程间的通信，支持fixed和cached两种线程池模式
- 使用右值引用和移动语义来提升代码性能，使用智能指针自动管理内存的释放，防止内存泄露
