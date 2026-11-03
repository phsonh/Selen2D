#pragma once
#include <cstdint>
#include <limits>

namespace core::modules::Object {

	// ObjectPool 槽位的安全句柄
	struct ObjectHandle {
		// 非法值定为uint32最大值
		static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

		std::uint32_t index = invalid_index;   // 槽位索引
		std::uint64_t generation = 0;          // 槽位代数

		// generation == 0 永远不会由 ObjectPool 生成，可用于识别默认句柄。
		[[nodiscard]] constexpr bool is_valid() const noexcept {
			return index != invalid_index && generation != 0;
		}

		// 定义两个ObjectHandle结构体的比较运算
		friend bool operator==(ObjectHandle const&, ObjectHandle const&) = default;
	};
}
