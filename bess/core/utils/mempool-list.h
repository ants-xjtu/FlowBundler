//
// Created by admin on 2021/10/28.
//

#ifndef HY_BATCHER_MEMPOOL_LIST_H
#define HY_BATCHER_MEMPOOL_LIST_H

#include <iostream>
#include <memory>
#include <unordered_map>
#include <hash_map>
#include <glog/logging.h>
#include <string>

template<class Node>
struct MemoryBlock
{
    MemoryBlock *prev = nullptr;
    MemoryBlock *next = nullptr;
    Node data_;
    size_t parent_id;
};

template<class T>
class MemoryPool
{
public:
    explicit MemoryPool(uint32_t n, std::string name);

    T *GetElement();

    int PutElement(T *elt);

    int GetNum();


    ~MemoryPool();


private:
    std::allocator<MemoryBlock<T>> alloc;
    MemoryBlock<T> *pool_;
    MemoryBlock<T> *head;
    MemoryBlock<T> *tail;
    size_t block_num;
    size_t size;
    size_t offset;
    size_t uuid;

};

template<class T>
MemoryPool<T>::MemoryPool(uint32_t n, std::string name):block_num(n), size(n)
{
    pool_ = alloc.allocate(n);
    head = new MemoryBlock<T>;
    tail = head;
    std::hash<std::string> h;
    uuid = h(name);
    offset = (uint64_t) (&(head->data_)) - (uint64_t) head;
    for (int i = 0; i < n; ++i)
    {
        tail->next = &(pool_[i]);
        alloc.template construct(pool_+i);
        tail->next->prev = tail;
        tail->next->parent_id = uuid;
        tail = tail->next;
    }
}

template<class T>
MemoryPool<T>::~MemoryPool()
{
    alloc.deallocate(pool_, size);
    tail = nullptr;
    delete head;
}

template<class T>
T *MemoryPool<T>::GetElement()
{
    if (block_num == 0)
    {
        LOG(INFO)<<"short of memory";
        return nullptr;
    }

    MemoryBlock<T> *block = tail;
    tail = tail->prev;
    tail->next = nullptr;
    block_num--;
    return &(block->data_);
}

template<class T>
int MemoryPool<T>::PutElement(T *elt)
{
    if (elt == nullptr)
        return -1;

    auto block = reinterpret_cast<MemoryBlock<T>*>(reinterpret_cast<uint8_t *>(elt)-offset);
    if (block->parent_id!=uuid)
    {
        LOG(INFO)<<"not belong to this pool";
        return -1;
    }
    tail->next = block;
    block->next = nullptr;
    block->prev = tail;
    tail = block;
    block_num++;
    return 0;
}

template<class T>
int MemoryPool<T>::GetNum()
{
    int count = 0;
    MemoryBlock<T> *block = head;
    while (block != tail)
    {
        count++;
        block = block->next;
    }
    return count;
}

#endif //HY_BATCHER_MEMPOOL_LIST_H
