#pragma once
#include "Object.h"
#include "ObjectHandle.h"
#include <cstdint>
#include <span>
#include <vector>

namespace core::modules::Object {

	class ObjectPool {
	public:
		explicit ObjectPool(std::uint32_t capacity);

		// 创建一个 Object，池满时返回无效 Handle。
		[[nodiscard]] ObjectHandle create();

		// 销毁指定 Object。
		// Handle 无效或已过期时返回 false。
		[[nodiscard]] bool destroy(ObjectHandle handle) noexcept;

		// 根据 Handle 获取 Object。
		// Handle 无效或已过期时返回 nullptr。
		[[nodiscard]] Object* get(ObjectHandle handle) noexcept;
		[[nodiscard]] Object const* get(ObjectHandle handle) const noexcept;
		[[nodiscard]] bool contains(ObjectHandle handle) const noexcept;

		// 销毁所有 Object，并使已有 Handle 全部过期。
		void clear() noexcept;

		// 活跃 Object 紧密存放，可直接用于逐帧顺序更新。
		// destroy()/clear() 会使受移动对象的指针、引用和迭代器失效。
		[[nodiscard]] std::span<Object> units() noexcept;
		[[nodiscard]] std::span<Object const> units() const noexcept;

		// 获取稠密数组中指定 Object 的安全句柄。
		[[nodiscard]] ObjectHandle handle_at(std::uint32_t dense_index) const noexcept;

		// 对象池容量。
		[[nodiscard]] std::uint32_t size() const noexcept;
		[[nodiscard]] std::uint32_t capacity() const noexcept;
		[[nodiscard]] bool empty() const noexcept;
		[[nodiscard]] bool full() const noexcept;

	private:
		struct Slot {
			// 当前槽位的代数。
			std::uint64_t generation = 1;

			// 活跃时是稠密数组下标；空闲时是空闲链表的下一项。
			std::uint32_t index = ObjectHandle::invalid_index;

			bool active = false;
		};

		// 稀疏槽位只保存 Handle 元数据，热数据 Object 独立紧密存放。
		std::vector<Slot> slots_;
		std::vector<Object> units_;
		std::vector<std::uint32_t> dense_to_slot_;

		// 空闲链表的第一个槽位。
		std::uint32_t free_head_ = ObjectHandle::invalid_index;

		[[nodiscard]] bool matches(ObjectHandle handle) const noexcept;
	};

}
