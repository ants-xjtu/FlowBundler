//
// Created by admin on 2021/12/6.
//

#ifndef HY_BATCHER_CIRCULAR_QUEUE_H
#define HY_BATCHER_CIRCULAR_QUEUE_H

#include <iostream>

template<class T>
class CircularQueue
{
public:
    explicit CircularQueue(uint32_t size);

    bool IsFull() { return (tail_pos + 1) % size_ == head_pos; }

    bool IsEmpty() { return head_pos == tail_pos; }

    uint32_t QueueSize() { return size_; }

    void Reset()
    {
        tail_pos = 0;
        head_pos = 0;
    }

    uint32_t ElementsNum() { return (size_ - head_pos + tail_pos) & (size_ - 1);}

    T Pop();

    int Push(T data);

    ~CircularQueue();

private:
    uint32_t size_;
    uint32_t head_pos;
    uint32_t tail_pos;
    uint32_t temp_pos;
    T* memory;

};

/// If size is not an integer power of 2, set size
/// to a minimum power of 2 that is not less than size
template<class T>
CircularQueue<T>::CircularQueue(uint32_t size)
{
    uint32_t MAX = 1 << 31;
    if (size >= MAX)
        size_ = MAX;
    else
    {
        uint32_t m = 1;
        while (m < size)
            m *= 2;
        size_ = m;
    }
    head_pos = 0;
    tail_pos = 0;
    temp_pos = 0;
    memory = new T[size_];
}

template<class T>
T CircularQueue<T>::Pop()
{
    if (IsEmpty())
        return {};
    temp_pos = head_pos;
    head_pos = (head_pos + 1) & (size_ - 1);
    return memory[temp_pos];

}


template<class T>
int CircularQueue<T>::Push(T data)
{
    if (IsFull())
        return -1;
    memory[tail_pos] = data;
    tail_pos = (tail_pos + 1) & (size_ - 1);
    return 0;
}

template<class T>
CircularQueue<T>::~CircularQueue()
{
    delete[] memory;
}

#endif //HY_BATCHER_CIRCULAR_QUEUE_H
