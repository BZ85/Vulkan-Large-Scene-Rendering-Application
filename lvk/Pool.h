#pragma once

#include <assert.h>
#include <cstdint>
#include <vector>

#include "lvk/LVK.h"

/// Pool<> is used only by the implementation
namespace lvk {

// handle type and actual stored object type are separated
template<typename ObjectType, typename ImplObjectType>
class Pool {
  static constexpr uint32_t kListEndSentinel = 0xffffffff;
  struct PoolEntry {
    explicit PoolEntry(ImplObjectType& obj) : obj_(std::move(obj)) {}
    ImplObjectType obj_ = {}; // obj value
    uint32_t gen_ = 1; // generation
    uint32_t nextFree_ = kListEndSentinel; // maintain a linked list of free elements inside the array
  };
  uint32_t freeListHead_ = kListEndSentinel; // store the index of the first free element
  uint32_t numObjects_ = 0;

 public:
  std::vector<PoolEntry> objects_; // a collection of objects of type ImplObjectType 

  Handle<ObjectType> create(ImplObjectType&& obj) {
    uint32_t idx = 0;
    if (freeListHead_ != kListEndSentinel) { // if there is a free element in the array
      idx = freeListHead_;
      freeListHead_ = objects_[idx].nextFree_; // set the new head of free list
      objects_[idx].obj_ = std::move(obj); // move the R-value obj to the free spot
    } else { // if there is no free space in the array
      idx = (uint32_t)objects_.size();
      objects_.emplace_back(obj);  // append a new element to the objects_ list
    }
    numObjects_++;
    return Handle<ObjectType>(idx, objects_[idx].gen_); // fill the index and the generation into the handle
  }
  void destroy(Handle<ObjectType> handle) {
    if (handle.empty()) // empty handles should not be destroyed
      return;
    assert(numObjects_ > 0); // double deletion
    const uint32_t index = handle.index();
    assert(index < objects_.size());
	 // if the generation of the handle does not match the generation of the elements in the list
	 // then it indicates that we are attempting double deletion
    assert(handle.gen() == objects_[index].gen_); // double deletion

	 // if all checks are successful, then replace the stored object with an empty one
	 objects_[index].obj_ = ImplObjectType{};
    objects_[index].gen_++; // increment the generation
    objects_[index].nextFree_ = freeListHead_;
    freeListHead_ = index; // add it to the front of the free list
    numObjects_--;
  }

  // dereference of a handle (get the object)
  const ImplObjectType* get(Handle<ObjectType> handle) const {
    if (handle.empty())
      return nullptr;

    const uint32_t index = handle.index();
    assert(index < objects_.size());
	 // mismatch in the generations helps identify access to a deleted object
    assert(handle.gen() == objects_[index].gen_); // accessing deleted object
    return &objects_[index].obj_;
  }
  ImplObjectType* get(Handle<ObjectType> handle) {
    if (handle.empty())
      return nullptr;

    const uint32_t index = handle.index();
    assert(index < objects_.size());
    assert(handle.gen() == objects_[index].gen_); // accessing deleted object
    return &objects_[index].obj_;
  }

  // construct a Handle<> object only by its index in the pool, regardless of the generation value
  // unsafe and should only be used of debugging
  Handle<ObjectType> getHandle(uint32_t index) const {
    assert(index < objects_.size());
    if (index >= objects_.size())
      return {};

    return Handle<ObjectType>(index, objects_[index].gen_);
  }

  // check if a specific obj is in the pool
  // and get the handle of it
  Handle<ObjectType> findObject(const ImplObjectType* obj) {
    if (!obj)
      return {};

    for (size_t idx = 0; idx != objects_.size(); idx++) {
      if (objects_[idx].obj_ == *obj) {
        return Handle<ObjectType>((uint32_t)idx, objects_[idx].gen_);
      }
    }

    return {};
  }
  void clear() {
    objects_.clear(); // a destructor is called for every objects
    freeListHead_ = kListEndSentinel;
    numObjects_ = 0;
  }

  // the numObjects_ is used to track memory leaks and prevent deallocations inside an empty pool
  uint32_t numObjects() const {
    return numObjects_;
  }
};

} // namespace lvk
