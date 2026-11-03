#include "ObjectPool.h"

#include <cstddef>
#include <limits>
#include <utility>

namespace core::modules::Object {

	ObjectPool::ObjectPool(std::uint32_t capacity)
		: slots_(capacity)
	{
		// 后续 create() 不再分配内存，避免游戏过程中出现分配抖动。
		units_.reserve(capacity);
		dense_to_slot_.reserve(capacity);

		if (capacity == 0) {
			return;
		}

		free_head_ = 0;

		for (std::uint32_t index = 0; index < capacity; ++index) {
			Slot& slot = slots_[index];
			slot.generation = 1;
			slot.index = index + 1 < capacity
				? index + 1
				: ObjectHandle::invalid_index;
		}
	}

	ObjectHandle ObjectPool::create() {
		if (free_head_ == ObjectHandle::invalid_index) {
			return ObjectHandle{};
		}

		const std::uint32_t slot_index = free_head_;
		Slot& slot = slots_[slot_index];
		free_head_ = slot.index;

		const auto dense_index = static_cast<std::uint32_t>(units_.size());
		units_.emplace_back();
		dense_to_slot_.push_back(slot_index);
		slot.index = dense_index;
		slot.active = true;

		return ObjectHandle{ slot_index, slot.generation };
	}

	bool ObjectPool::destroy(ObjectHandle handle) noexcept {
		if (!matches(handle)) {
			return false;
		}

		Slot& removed_slot = slots_[handle.index];
		const std::uint32_t removed_dense_index = removed_slot.index;
		const std::uint32_t last_dense_index =
			static_cast<std::uint32_t>(units_.size() - 1);

		// 用尾元素填洞，让所有活跃 Object 始终保持连续。
		if (removed_dense_index != last_dense_index) {
			units_[removed_dense_index] = std::move(units_.back());

			const std::uint32_t moved_slot_index = dense_to_slot_.back();
			dense_to_slot_[removed_dense_index] = moved_slot_index;
			slots_[moved_slot_index].index = removed_dense_index;
		}

		units_.pop_back();
		dense_to_slot_.pop_back();
		removed_slot.active = false;

		// 不允许 generation 回绕；耗尽的槽位永久退役。
		if (removed_slot.generation == std::numeric_limits<std::uint64_t>::max()) {
			removed_slot.index = ObjectHandle::invalid_index;
			return true;
		}

		++removed_slot.generation;
		removed_slot.index = free_head_;
		free_head_ = handle.index;

		return true;
	}

	Object* ObjectPool::get(ObjectHandle handle) noexcept {
		return matches(handle) ? &units_[slots_[handle.index].index] : nullptr;
	}

	Object const* ObjectPool::get(ObjectHandle handle) const noexcept {
		return matches(handle) ? &units_[slots_[handle.index].index] : nullptr;
	}

	bool ObjectPool::contains(ObjectHandle handle) const noexcept {
		return matches(handle);
	}

	void ObjectPool::clear() noexcept {
		// 只访问活跃槽位，复杂度为 O(size)，而不是 O(capacity)。
		for (const std::uint32_t slot_index : dense_to_slot_) {
			Slot& slot = slots_[slot_index];
			slot.active = false;
			if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
				slot.index = ObjectHandle::invalid_index;
				continue;
			}

			++slot.generation;
			slot.index = free_head_;
			free_head_ = slot_index;
		}

		units_.clear();
		dense_to_slot_.clear();
	}

	std::span<Object> ObjectPool::units() noexcept {
		return units_;
	}

	std::span<Object const> ObjectPool::units() const noexcept {
		return units_;
	}

	ObjectHandle ObjectPool::handle_at(std::uint32_t dense_index) const noexcept {
		if (dense_index >= dense_to_slot_.size()) {
			return ObjectHandle{};
		}

		const std::uint32_t slot_index = dense_to_slot_[dense_index];
		return ObjectHandle{ slot_index, slots_[slot_index].generation };
	}

	std::uint32_t ObjectPool::size() const noexcept {
		return static_cast<std::uint32_t>(units_.size());
	}

	std::uint32_t ObjectPool::capacity() const noexcept {
		return static_cast<std::uint32_t>(slots_.size());
	}

	bool ObjectPool::empty() const noexcept {
		return units_.empty();
	}

	bool ObjectPool::full() const noexcept {
		return free_head_ == ObjectHandle::invalid_index;
	}

	bool ObjectPool::matches(ObjectHandle handle) const noexcept {
		if (!handle.is_valid() || handle.index >= slots_.size()) {
			return false;
		}

		const Slot& slot = slots_[handle.index];
		return slot.generation == handle.generation
			&& slot.active;
	}
}
